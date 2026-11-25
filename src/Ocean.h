// Océan : spectre JONSWAP, IFFT GPU en cascades, rendu avec LOD (FFT -> Gerstner).
#pragma once
#include "Pipelines.h"
#include "Weather.h"

namespace me {

struct OceanParams {
    float choppiness = 1.2f;
    float foamIntensity = 1.0f;
    float foamDecay = 0.8f;
    float patchSizes[3] = {250.0f, 47.0f, 9.0f};
    float lodNear = 50.0f;   // précision maximale dans ce rayon (m)
    float lodFar = 500.0f;   // au-delà : modèle analytique de Gerstner seul
};

class Ocean {
public:
    void create(Gpu& gpu, const QualitySettings& q);
    void createPipelines(Pipelines& p);
    void simulate(Gpu& gpu, GpuProfiler& prof, const WeatherState& w, float dt);
    void draw(Gpu& gpu, const Texture& weatherMap);

    // Valeurs dérivées du spectre, utilisées par le modèle analytique lointain.
    float gerstnerAmplitude() const { return m_gerstnerAmp; }
    float gerstnerWavelength() const { return m_gerstnerLen; }

    OceanParams params;
    Texture disp[3], deriv[3];
    uint32_t cascades() const { return m_cascades; }

private:
    void updateAnalytic(const WeatherState& w);

    Texture m_h0[3], m_specA[3], m_specB[3];
    ComPtr<ID3D12PipelineState> m_init, m_time, m_fft, m_assemble, m_draw;
    uint32_t m_n = 256, m_cascades = 3, m_gridQuads = 384;
    float m_lastWind = -1, m_lastAngle = -1, m_lastFetch = -1, m_lastPatch[3] = {};
    float m_gerstnerAmp = 0.5f, m_gerstnerLen = 40.0f;
};

} // namespace me
