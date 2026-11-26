// Vent 3D, pluie GPU, humidité des surfaces et scène de démonstration.
#pragma once
#include "Pipelines.h"
#include "Weather.h"
#include <vector>

namespace me {

// Rocher (miroir de Rock dans shaders/Scene.hlsli) : ellipsoïde déformé par du bruit.
struct SceneRock {
    float3 center; float yaw;
    float3 radii;  uint32_t seed;
};
static_assert(sizeof(SceneRock) == 32, "SceneRock");

// Plage procédurale + rochers.
class DemoScene {
public:
    static constexpr uint32_t kMaxRocks = 160;
    void create(Gpu& gpu);
    void createPipelines(Pipelines& p);
    void generateRocks();             // placement déterministe selon rockSeed / rockCount
    void upload(Gpu& gpu);            // envoie les rochers au GPU s'ils ont changé
    void draw(Gpu& gpu, const Texture& wetMap, const Texture& weatherMap);
    uint32_t rockCountGpu() const { return uint32_t(rocks.size()); }

    float3 terrainMin{-320.0f, -14.0f, -220.0f};
    float3 terrainMax{320.0f, 12.0f, 360.0f};
    std::vector<SceneRock> rocks;
    Buffer rockBuffer;
    int rockSeed = 7, scatteredRocks = 45;
    bool showArch = true;
    uint32_t terrainQuads = 640;
    float puddleScale = 0.12f, rippleAmount = 1.0f;
    bool showWetnessDebug = false;
    bool dirty = true;
private:
    ComPtr<ID3D12PipelineState> m_terrain, m_rock;
};

struct WindParams { float cellSize = 4.0f; };

class WindField {
public:
    void create(Gpu& gpu, const QualitySettings& q);
    void createPipelines(Pipelines& p);
    void update(Gpu& gpu, GpuProfiler& prof);
    // Origine du volume, alignée sur la grille (évite le glissement des données).
    float3 origin(const float3& cam) const;
    const uint32_t* res() const { return m_res; }
    WindParams params;
    Texture volume;
private:
    ComPtr<ID3D12PipelineState> m_pso;
    uint32_t m_res[3] = {64, 32, 64};
};

// Miroir de RainCB (shaders/rain.hlsl et rain_draw.hlsl).
struct RainCB {
    uint32_t maxParticles, active; float boxHalf, boxHeight;
    float occOriginX, occOriginZ, occTexel; uint32_t occSize;
    uint32_t rockCount; float terminalSpeed, streak, brightness;
};
static_assert(sizeof(RainCB) == 48, "RainCB");

struct RainParams {
    float boxHalf = 35.0f, boxHeight = 30.0f;
    float terminalSpeed = 9.0f;
    float streak = 0.018f;
    float brightness = 1.0f;
};

class RainSystem {
public:
    void create(Gpu& gpu, const QualitySettings& q);
    void createPipelines(Pipelines& p);
    void simulate(Gpu& gpu, GpuProfiler& prof, const DemoScene& scene, const WindField& wind,
                  const float3& cam, float precipitation);
    void draw(Gpu& gpu);
    RainParams params;
    Texture occlusion;        // hauteur de la première surface vue du ciel
    uint32_t activeCount() const { return m_active; }
private:
    Buffer m_drops;
    ComPtr<ID3D12PipelineState> m_occ, m_sim, m_draw;
    uint32_t m_max = 0, m_active = 0;
    float m_occOrigin[2] = {}, m_occTexel = 0.25f;
    static constexpr uint32_t kOccSize = 512;
    RainCB m_lastCB{};
};

struct WetnessParams {
    float wetRate = 0.08f, puddleRate = 0.03f;
    float dryBase = 0.002f, dryWind = 0.0012f, drySun = 0.01f, puddleDry = 0.35f;
};

class WetnessSystem {
public:
    void create(Gpu& gpu, const QualitySettings& q);
    void createPipelines(Pipelines& p);
    void update(Gpu& gpu, GpuProfiler& prof, const DemoScene& scene, const Texture& weatherMap);
    // Même modèle côté CPU pour la variable globale d'humidité (toits, murs).
    void updateGlobal(const WeatherState& w, float dt);
    WetnessParams params;
    Texture map;              // R = humidité, G = flaques
    float globalWetness = 0.0f;
private:
    ComPtr<ID3D12PipelineState> m_pso;
    uint32_t m_size = 1024;
};

} // namespace me
