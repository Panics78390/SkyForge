#include "Atmos.h"
#include "Terrain.h"
#include <cmath>

namespace me {

// ---------------------------------------------------------------------------
// Scène : plage + rochers
struct SceneCB {
    uint32_t rockCount; float puddleScale, rippleAmount, showWet;
    uint32_t terrainQuads; float pad[3];
};
static_assert(sizeof(SceneCB) == 32, "SceneCB");

void DemoScene::create(Gpu& gpu) {
    rockBuffer = gpu.createStructured(sizeof(SceneRock), kMaxRocks, false, L"Scene.Rocks");
    generateRocks();
}

void DemoScene::generateRocks() {
    rocks.clear();
    uint32_t state = uint32_t(rockSeed) * 747796405u + 2891336453u;
    auto rnd = [&state]() { state = terrain::hash(state + 0x9E3779B9u); return float(state & 0xFFFFFF) / 16777216.0f; };
    auto place = [&](float x, float z, float3 radii, float bury, uint32_t seed) {
        if (rocks.size() >= kMaxRocks) return;
        const float y = terrain::height(x, z) + radii.y * bury;
        rocks.push_back({{x, y, z}, rnd() * 2.0f * kPi, radii, seed});
    };
    // 1. Éboulis le long du rivage (en partie dans l'eau : les vagues les contournent).
    for (int i = 0; i < 22; ++i) {
        const float x = -200.0f + rnd() * 400.0f;
        const float z = terrain::shoreLine(x) + (rnd() - 0.6f) * 30.0f;
        const float r = 0.6f + rnd() * rnd() * 3.0f;
        place(x, z, {r * (0.9f + rnd() * 0.6f), r * (0.6f + rnd() * 0.4f), r * (0.9f + rnd() * 0.6f)}, 0.1f, i);
    }
    // 2. Pointe rocheuse à l'est : gros blocs qui avancent dans la mer.
    for (int i = 0; i < 14; ++i) {
        const float x = 120.0f + rnd() * 55.0f;
        const float z = terrain::shoreLine(x) - 35.0f + i * 5.0f + (rnd() - 0.5f) * 10.0f;
        const float r = 3.0f + rnd() * 6.0f;
        place(x, z, {r * (0.8f + rnd() * 0.5f), r * (0.7f + rnd() * 0.5f), r * (0.8f + rnd() * 0.5f)}, 0.05f, 100 + i);
    }
    // 3. Arche : deux piliers et une dalle en surplomb. Le sable reste sec dessous quand il pleut.
    if (showArch) {
        const float ax = -25.0f, az = terrain::shoreLine(-25.0f) + 32.0f;
        const float g = terrain::height(ax, az);
        rocks.push_back({{ax - 8.5f, g + 2.2f, az}, 0.3f, {2.6f, 4.2f, 2.4f}, 200});
        rocks.push_back({{ax + 8.5f, g + 2.2f, az}, 1.1f, {2.4f, 4.0f, 2.7f}, 201});
        rocks.push_back({{ax, g + 6.4f, az}, 0.05f, {12.5f, 1.4f, 4.6f}, 202});
    }
    // 4. Blocs épars sur la plage et dans les dunes.
    for (int i = 0; i < scatteredRocks; ++i) {
        const float x = -230.0f + rnd() * 460.0f;
        const float z = terrain::shoreLine(x) + 5.0f + rnd() * 140.0f;
        const float r = 0.3f + rnd() * rnd() * 2.2f;
        place(x, z, {r * (0.9f + rnd() * 0.5f), r * (0.5f + rnd() * 0.4f), r * (0.9f + rnd() * 0.5f)}, 0.15f, 300 + i);
    }
    dirty = true;
}

void DemoScene::createPipelines(Pipelines& p) {
    GraphicsDesc g;
    g.file = "scene_draw.hlsl";
    g.depth = DepthMode::WriteGreater;
    g.vsEntry = "VSTerrain";
    m_terrain = p.graphics(g);
    g.vsEntry = "VSRock";
    m_rock = p.graphics(g);
}

void DemoScene::upload(Gpu& gpu) {
    if (!dirty) return;
    if (!rocks.empty()) gpu.uploadBuffer(rockBuffer, rocks.data(), rocks.size() * sizeof(SceneRock));
    gpu.transition(rockBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dirty = false;
}

void DemoScene::draw(Gpu& gpu, const Texture& wetMap, const Texture& weatherMap) {
    SceneCB cb{rockCountGpu(), puddleScale, rippleAmount, showWetnessDebug ? 1.0f : 0.0f, terrainQuads, {}};
    gpu.cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (m_terrain) {
        gpu.bindGraphics(m_terrain.Get(), &cb, sizeof(cb), {rockBuffer, wetMap, weatherMap});
        gpu.cmd()->DrawInstanced(terrainQuads * terrainQuads * 6, 1, 0, 0);
    }
    if (m_rock && !rocks.empty()) {
        gpu.bindGraphics(m_rock.Get(), &cb, sizeof(cb), {rockBuffer, wetMap, weatherMap});
        gpu.cmd()->DrawInstanced(14 * 20 * 6, UINT(rocks.size()), 0, 0); // ROCK_STACKS x ROCK_SLICES
    }
}

// ---------------------------------------------------------------------------
// Vent
void WindField::create(Gpu& gpu, const QualitySettings& q) {
    for (int i = 0; i < 3; ++i) m_res[i] = q.windRes[i];
    volume = gpu.createTexture3D(m_res[0], m_res[1], m_res[2], DXGI_FORMAT_R16G16B16A16_FLOAT, L"Wind.Volume");
}

void WindField::createPipelines(Pipelines& p) { m_pso = p.compute("wind.hlsl"); }

float3 WindField::origin(const float3& cam) const {
    const float c = params.cellSize;
    return {std::floor(cam.x / c) * c - m_res[0] * c * 0.5f,
            std::floor(cam.y / c) * c - m_res[1] * c * 0.25f,
            std::floor(cam.z / c) * c - m_res[2] * c * 0.5f};
}

void WindField::update(Gpu& gpu, GpuProfiler& prof) {
    ScopedPass sp(prof, "Vent 3D");
    gpu.dispatch(m_pso.Get(), nullptr, 0, {}, {volume}, divUp(m_res[0], 4), divUp(m_res[1], 4), divUp(m_res[2], 4));
}

// ---------------------------------------------------------------------------
// Pluie
void RainSystem::create(Gpu& gpu, const QualitySettings& q) {
    m_max = q.maxRainParticles;
    m_drops = gpu.createStructured(32, m_max, true, L"Rain.Drops");
    occlusion = gpu.createTexture2D(kOccSize, kOccSize, DXGI_FORMAT_R32_FLOAT, TexUav, L"Rain.Occlusion");
}

void RainSystem::createPipelines(Pipelines& p) {
    m_occ = p.compute("rain.hlsl", {}, "Occlusion");
    m_sim = p.compute("rain.hlsl", {}, "Simulate");
    GraphicsDesc g;
    g.file = "rain_draw.hlsl";
    g.depth = DepthMode::TestGreater;
    g.blend = BlendMode::Alpha;
    m_draw = p.graphics(g);
}

void RainSystem::simulate(Gpu& gpu, GpuProfiler& prof, const DemoScene& scene, const WindField& wind,
                          const float3& cam, float precipitation) {
    ScopedPass sp(prof, "Pluie sim");
    m_active = uint32_t(float(m_max) * saturate(precipitation));
    const float half = kOccSize * m_occTexel * 0.5f;
    m_occOrigin[0] = std::floor((cam.x - half) / m_occTexel) * m_occTexel;
    m_occOrigin[1] = std::floor((cam.z - half) / m_occTexel) * m_occTexel;
    RainCB cb{m_max, m_active, params.boxHalf, params.boxHeight, m_occOrigin[0], m_occOrigin[1], m_occTexel, kOccSize,
              scene.rockCountGpu(), params.terminalSpeed, params.streak, params.brightness};
    // La carte d'occlusion : en production, une passe de profondeur orthographique vue du ciel,
    // mise à jour 1 frame sur 4 ou quand la caméra a bougé d'une demi-carte.
    gpu.dispatch(m_occ.Get(), &cb, sizeof(cb), {scene.rockBuffer}, {occlusion}, divUp(kOccSize, 8), divUp(kOccSize, 8), 1);
    gpu.dispatch(m_sim.Get(), &cb, sizeof(cb), {scene.rockBuffer, wind.volume, occlusion}, {View{}, m_drops},
                 divUp(m_max, 256), 1, 1);
    m_lastCB = cb;
}

void RainSystem::draw(Gpu& gpu) {
    if (!m_draw || m_active == 0) return;
    gpu.bindGraphics(m_draw.Get(), &m_lastCB, sizeof(m_lastCB), {m_drops});
    gpu.cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.cmd()->DrawInstanced(m_active * 6, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Humidité
struct WetCB {
    uint32_t mapSize, rockCount; float wetRate, puddleRate;
    float dryBase, dryWind, drySun, puddleDry;
};
static_assert(sizeof(WetCB) == 32, "WetCB");

void WetnessSystem::create(Gpu& gpu, const QualitySettings& q) {
    m_size = q.wetnessMapSize;
    map = gpu.createTexture2D(m_size, m_size, DXGI_FORMAT_R16G16_FLOAT, TexUav, L"Wetness.Map");
}

void WetnessSystem::createPipelines(Pipelines& p) { m_pso = p.compute("wetness.hlsl"); }

void WetnessSystem::update(Gpu& gpu, GpuProfiler& prof, const DemoScene& scene, const Texture& weatherMap) {
    ScopedPass sp(prof, "Humidite");
    WetCB cb{m_size, scene.rockCountGpu(), params.wetRate, params.puddleRate,
             params.dryBase, params.dryWind, params.drySun, params.puddleDry};
    gpu.dispatch(m_pso.Get(), &cb, sizeof(cb), {scene.rockBuffer, weatherMap}, {map}, divUp(m_size, 8), divUp(m_size, 8), 1);
}

void WetnessSystem::updateGlobal(const WeatherState& w, float dt) {
    const float rain = saturate(w.precipitation);
    globalWetness = saturate(globalWetness + rain * params.wetRate * dt);
    const float sun = saturate(w.sunDir().y) * w.sunIntensity * (1.0f - 0.7f * saturate(w.cloudCoverage));
    float dry = (params.dryBase + params.dryWind * w.windSpeed + params.drySun * sun) *
                std::max(0.2f, 1.0f + 0.04f * (w.temperatureC - 15.0f));
    dry *= 1.0f - saturate(rain * 4.0f);
    globalWetness = saturate(globalWetness - dry * dt);
}

} // namespace me
