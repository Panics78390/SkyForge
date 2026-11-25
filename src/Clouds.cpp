#include "Clouds.h"
#include <algorithm>

namespace me {

// Miroir de CloudCB (shaders/CloudCommon.hlsli).
struct CloudCB {
    uint32_t resX, resY, steps, lightSteps;
    float shapeScale, detailScale, detailStrength, extinction;
    float g1, g2, powder, ambient;
    float historyBlend; uint32_t historyValid; float pad[2];
};
static_assert(sizeof(CloudCB) == 64, "CloudCB");

void Clouds::create(Gpu& gpu, const QualitySettings& q, UINT screenW, UINT screenH) {
    m_w = std::max(1u, UINT(float(screenW) * q.cloudResScale));
    m_h = std::max(1u, UINT(float(screenH) * q.cloudResScale));
    m_steps = q.cloudSteps;
    m_lightSteps = q.cloudLightSteps;
    m_current = gpu.createTexture2D(m_w, m_h, DXGI_FORMAT_R16G16B16A16_FLOAT, TexUav, L"Clouds.Current");
    m_dist = gpu.createTexture2D(m_w, m_h, DXGI_FORMAT_R32_FLOAT, TexUav, L"Clouds.Distance");
    for (auto& h : m_history) h = gpu.createTexture2D(m_w, m_h, DXGI_FORMAT_R16G16B16A16_FLOAT, TexUav, L"Clouds.History");
    if (!m_baked) {
        shapeNoise = gpu.createTexture3D(128, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, L"Clouds.ShapeNoise");
        detailNoise = gpu.createTexture3D(32, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, L"Clouds.DetailNoise");
        weatherMap = gpu.createTexture2D(512, 512, DXGI_FORMAT_R8G8B8A8_UNORM, TexUav, L"Clouds.WeatherMap");
    }
    m_historyValid = false;
}

void Clouds::createPipelines(Pipelines& p) {
    m_bakeShape = p.compute("cloud_noise.hlsl", {}, "BakeShape");
    m_bakeDetail = p.compute("cloud_noise.hlsl", {}, "BakeDetail");
    m_bakeWeather = p.compute("cloud_noise.hlsl", {}, "BakeWeather");
    m_march = p.compute("cloud_march.hlsl");
    m_reproject = p.compute("cloud_reproject.hlsl");
    GraphicsDesc g;
    g.file = "composite.hlsl";
    m_composite = p.graphics(g);
}

void Clouds::bakeNoise(Gpu& gpu) {
    if (m_baked || !m_bakeShape || !m_bakeDetail || !m_bakeWeather) return;
    gpu.dispatch(m_bakeShape.Get(), nullptr, 0, {}, {shapeNoise}, 32, 32, 32);
    gpu.dispatch(m_bakeDetail.Get(), nullptr, 0, {}, {detailNoise}, 8, 8, 8);
    gpu.dispatch(m_bakeWeather.Get(), nullptr, 0, {}, {View{}, weatherMap}, 64, 64, 1);
    m_baked = true;
}

void Clouds::render(Gpu& gpu, GpuProfiler& prof, const Texture& depth) {
    CloudCB cb{};
    cb.resX = m_w;
    cb.resY = m_h;
    cb.steps = m_steps;
    cb.lightSteps = m_lightSteps;
    cb.shapeScale = 1.0f / params.shapeSize;
    cb.detailScale = 1.0f / params.detailSize;
    cb.detailStrength = params.detailStrength;
    cb.extinction = params.extinction;
    cb.g1 = params.g1;
    cb.g2 = params.g2;
    cb.powder = params.powder;
    cb.ambient = params.ambient;
    cb.historyBlend = params.historyBlend;
    cb.historyValid = m_historyValid ? 1u : 0u;
    {
        ScopedPass sp(prof, "Nuages march");
        gpu.dispatch(m_march.Get(), &cb, sizeof(cb), {depth, shapeNoise, detailNoise, weatherMap}, {m_current, m_dist},
                     divUp(m_w, 8), divUp(m_h, 8), 1);
    }
    {
        ScopedPass sp(prof, "Nuages reproj");
        const UINT prev = m_historyIndex, next = 1 - m_historyIndex;
        gpu.dispatch(m_reproject.Get(), &cb, sizeof(cb), {m_current, m_dist, m_history[prev]}, {m_history[next]},
                     divUp(m_w, 8), divUp(m_h, 8), 1);
        m_historyIndex = next;
    }
    m_historyValid = true;
}

void Clouds::composite(Gpu& gpu, const Texture& sceneHdr, const Texture& depth, bool cloudsEnabled) {
    if (!m_composite) return;
    const float cb[4] = {cloudsEnabled ? 1.0f : 0.0f, 0, 0, 0};
    gpu.bindGraphics(m_composite.Get(), cb, sizeof(cb), {sceneHdr, depth, output()});
    gpu.cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.cmd()->DrawInstanced(3, 1, 0, 0);
}

} // namespace me
