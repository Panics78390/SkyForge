// Nuages volumétriques : bruit Perlin-Worley 3D généré au démarrage, ray marching
// en basse résolution, reprojection temporelle, composition.
#pragma once
#include "Pipelines.h"
#include "Weather.h"

namespace me {

struct CloudParams {
    float bottom = 1500.0f, top = 4200.0f;   // altitude de la couche (m)
    float shapeSize = 9000.0f;               // taille du motif de forme (m)
    float detailSize = 900.0f;
    float detailStrength = 0.35f;
    float extinction = 0.045f;               // 1/m pour densité 1
    float g1 = 0.75f, g2 = -0.25f;           // Henyey-Greenstein
    float powder = 0.6f;
    float ambient = 0.7f;
    float historyBlend = 0.9f;
    float windInfluence = 1.0f;              // vitesse de dérive des nuages / vent au sol
};

class Clouds {
public:
    void create(Gpu& gpu, const QualitySettings& q, UINT screenW, UINT screenH);
    void createPipelines(Pipelines& p);
    void bakeNoise(Gpu& gpu);                          // une fois au démarrage
    void render(Gpu& gpu, GpuProfiler& prof, const Texture& depth);
    void composite(Gpu& gpu, const Texture& sceneHdr, const Texture& depth, bool cloudsEnabled);
    void invalidateHistory() { m_historyValid = false; }

    CloudParams params;
    Texture weatherMap;                                // utilisé aussi pour l'ombre des nuages
    Texture shapeNoise, detailNoise;
    const Texture& output() const { return m_history[m_historyIndex]; }

private:
    Texture m_current, m_dist, m_history[2];
    ComPtr<ID3D12PipelineState> m_bakeShape, m_bakeDetail, m_bakeWeather, m_march, m_reproject, m_composite;
    UINT m_w = 0, m_h = 0;
    uint32_t m_steps = 64, m_lightSteps = 6;
    UINT m_historyIndex = 0;
    bool m_historyValid = false, m_baked = false;
};

} // namespace me
