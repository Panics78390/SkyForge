#include "Ocean.h"
#include <cmath>
#include <string>

namespace me {

// Miroir de OceanCB (shaders/OceanCommon.hlsli).
struct OceanCB {
    float patchSize, kMin, kMax; uint32_t n;
    float windX, windZ, u10, fetch;
    float choppiness; uint32_t cascade; float foamDecay, foamThreshold;
    uint32_t vertical; float pad[3];
};
static_assert(sizeof(OceanCB) == 64, "OceanCB");

struct OceanDrawCB {
    float patch[4];
    uint32_t gridQuads; float gridExtent, gridLinear, foamIntensity;
    float choppy, shoreAmp, shoreLength, shoreRunup;
    float shoreOmega, pad2[3];
};
static_assert(sizeof(OceanDrawCB) == 64, "OceanDrawCB");

void Ocean::create(Gpu& gpu, const QualitySettings& q) {
    m_n = q.fftSize;
    m_cascades = q.oceanCascades;
    m_gridQuads = q.oceanGridQuads;
    for (uint32_t c = 0; c < 3; ++c) {
        m_h0[c] = gpu.createTexture2D(m_n, m_n, DXGI_FORMAT_R32G32B32A32_FLOAT, TexUav, L"Ocean.H0");
        m_specA[c] = gpu.createTexture2D(m_n, m_n, DXGI_FORMAT_R32G32B32A32_FLOAT, TexUav, L"Ocean.SpecA");
        m_specB[c] = gpu.createTexture2D(m_n, m_n, DXGI_FORMAT_R32G32B32A32_FLOAT, TexUav, L"Ocean.SpecB");
        disp[c] = gpu.createTexture2D(m_n, m_n, DXGI_FORMAT_R16G16B16A16_FLOAT, TexUav, L"Ocean.Displacement");
        deriv[c] = gpu.createTexture2D(m_n, m_n, DXGI_FORMAT_R16G16B16A16_FLOAT, TexUav, L"Ocean.Derivatives");
    }
    m_lastWind = -1; // force le recalcul du spectre
}

void Ocean::createPipelines(Pipelines& p) {
    const std::string n = std::to_string(m_n);
    const std::string logn = std::to_string(m_n == 128 ? 7 : 8);
    m_init = p.compute("ocean_spectrum.hlsl", {}, "InitSpectrum");
    m_time = p.compute("ocean_spectrum.hlsl", {}, "TimeSpectrum");
    m_fft = p.compute("ocean_fft.hlsl", {{"FFT_N", n}, {"FFT_LOG_N", logn}});
    m_assemble = p.compute("ocean_assemble.hlsl");
    GraphicsDesc g;
    g.file = "ocean_draw.hlsl";
    g.depth = DepthMode::WriteGreater;
    m_draw = p.graphics(g);
    m_lastWind = -1;
}

void Ocean::updateAnalytic(const WeatherState& w) {
    // Pic du spectre (JONSWAP limité par le fetch, borné par la mer pleinement développée).
    const float g = 9.81f, U = std::max(w.windSpeed, 0.5f), F = std::max(w.fetchKm * 1000.0f, 1000.0f);
    float wp = 22.0f * std::cbrt(g * g / (U * F));
    wp = std::max(wp, 0.855f * g / U);
    m_gerstnerLen = 2.0f * kPi * g / (wp * wp);
    // Hauteur significative : limitée par le fetch et par la mer pleinement développée.
    const float hsFetch = 0.0016f * U * std::sqrt(F / g); // g Hs / U^2 = 0.0016 (g F / U^2)^0.5
    const float hsFull = 0.21f * U * U / g;
    m_gerstnerAmp = 0.3f * std::min(hsFetch, hsFull);
}

void Ocean::simulate(Gpu& gpu, GpuProfiler& prof, const WeatherState& w, float dt) {
    ScopedPass sp(prof, "Ocean FFT");
    m_lastWindForShore = w.windSpeed;
    updateAnalytic(w);
    const float3 wd = w.windDir();
    OceanCB cb{};
    cb.n = m_n;
    cb.windX = wd.x;
    cb.windZ = wd.z;
    cb.u10 = w.windSpeed;
    cb.fetch = w.fetchKm * 1000.0f;
    cb.choppiness = params.choppiness;
    cb.foamDecay = params.foamDecay;
    cb.foamThreshold = lerp(0.0f, 0.75f, saturate((w.windSpeed - 3.0f) / 15.0f));
    (void)dt;

    bool dirty = std::fabs(w.windSpeed - m_lastWind) > 0.05f || std::fabs(w.windAngleDeg - m_lastAngle) > 0.25f ||
                 std::fabs(w.fetchKm - m_lastFetch) > 1.0f;
    for (int c = 0; c < 3; ++c) dirty |= params.patchSizes[c] != m_lastPatch[c];

    const UINT groups = divUp(m_n, 8);
    for (uint32_t c = 0; c < m_cascades; ++c) {
        cb.cascade = c;
        cb.patchSize = params.patchSizes[c];
        // Bandes de nombres d'onde : chaque cascade ne garde que sa plage pour ne pas
        // compter deux fois la même énergie (frontière = 6 x la fondamentale de la suivante).
        cb.kMin = c == 0 ? 0.0f : 2.0f * kPi / params.patchSizes[c] * 6.0f;
        cb.kMax = (c + 1 < m_cascades) ? 2.0f * kPi / params.patchSizes[c + 1] * 6.0f : 1e9f;

        if (dirty) gpu.dispatch(m_init.Get(), &cb, sizeof(cb), {}, {m_h0[c]}, groups, groups, 1);
        gpu.dispatch(m_time.Get(), &cb, sizeof(cb), {m_h0[c]}, {m_specA[c], m_specB[c]}, groups, groups, 1);
        cb.vertical = 0;
        gpu.dispatch(m_fft.Get(), &cb, sizeof(cb), {}, {m_specA[c], m_specB[c]}, m_n, 1, 1);
        cb.vertical = 1;
        gpu.dispatch(m_fft.Get(), &cb, sizeof(cb), {}, {m_specA[c], m_specB[c]}, m_n, 1, 1);
        cb.vertical = 0;
        gpu.dispatch(m_assemble.Get(), &cb, sizeof(cb), {m_specA[c], m_specB[c]}, {disp[c], deriv[c]}, groups, groups, 1);
    }
    if (dirty) {
        m_lastWind = w.windSpeed;
        m_lastAngle = w.windAngleDeg;
        m_lastFetch = w.fetchKm;
        for (int c = 0; c < 3; ++c) m_lastPatch[c] = params.patchSizes[c];
    }
}

float Ocean::shoreOmega() const {
    // Dispersion en eau profonde, quantifiée pour que le motif boucle sur la période de 200 s.
    const float w = std::sqrt(9.81f * 2.0f * kPi / std::max(params.shoreWavelength, 1.0f));
    const float w0 = 2.0f * kPi / 200.0f;
    return std::max(1.0f, std::round(w / w0)) * w0;
}

float Ocean::shoreAmplitude() const {
    return params.shoreWaves * std::clamp(0.25f + m_lastWindForShore * 0.035f, 0.2f, 1.4f);
}

void Ocean::draw(Gpu& gpu, const Texture& weatherMap) {
    if (!m_draw) return;
    OceanDrawCB cb{};
    for (int c = 0; c < 3; ++c) cb.patch[c] = params.patchSizes[c];
    cb.patch[3] = float(m_cascades);
    cb.gridQuads = m_gridQuads;
    cb.gridExtent = 8000.0f;
    cb.gridLinear = 0.012f;
    cb.foamIntensity = params.foamIntensity;
    cb.choppy = params.choppiness;
    // Houle de bord : grandit avec le vent (borné), réglable par un multiplicateur.
    cb.shoreAmp = shoreAmplitude();
    cb.shoreLength = params.shoreWavelength;
    cb.shoreOmega = shoreOmega();
    cb.shoreRunup = params.shoreRunup * std::min(1.0f, 0.4f + m_lastWindForShore * 0.05f);
    gpu.bindGraphics(m_draw.Get(), &cb, sizeof(cb),
                     {disp[0], disp[1], disp[2], deriv[0], deriv[1], deriv[2], weatherMap});
    gpu.cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.cmd()->DrawInstanced(m_gridQuads * m_gridQuads * 6, 1, 0, 0);
}

} // namespace me
