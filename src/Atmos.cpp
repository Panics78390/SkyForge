#include "Atmos.h"
#include <cmath>

namespace me {

// ---------------------------------------------------------------------------
// Scène
struct SceneCB { uint32_t boxCount; float puddleScale, rippleAmount, showWet; };

void DemoScene::create(Gpu& gpu) {
    boxes.clear();
    auto add = [&](float3 a, float3 b, uint32_t m) { boxes.push_back({a, m, b, 0}); };
    const float g = islandMax.y;
    add(islandMin, islandMax, 0);                                     // l'île (pavés)
    add({-45, g, 15}, {-22, 9, 35}, 4);                               // maison en briques
    add({-46, 9, 14}, {-21, 9.6f, 36}, 1);                            // dalle de toit
    add({25, g, 20}, {37, 24, 32}, 1);                                // tour en béton
    // Abri : toit de tôle sur 4 poteaux (le sol dessous doit rester sec).
    add({-8, 4.0f, -18}, {14, 4.35f, -4}, 2);
    add({-7.5f, g, -17.5f}, {-7.1f, 4.0f, -17.1f}, 1);
    add({13.1f, g, -17.5f}, {13.5f, 4.0f, -17.1f}, 1);
    add({-7.5f, g, -4.9f}, {-7.1f, 4.0f, -4.5f}, 1);
    add({13.1f, g, -4.9f}, {13.5f, 4.0f, -4.5f}, 1);
    add({-70, g, -45}, {-25, 1.4f, -44.2f}, 4);                       // muret
    add({105, -3, -22}, {175, 0.95f, -15}, 3);                        // ponton qui avance dans la mer
    add({40, g, -30}, {41.5f, g + 1.5f, -28.5f}, 3);                  // caisses
    add({42, g, -30}, {43.2f, g + 1.2f, -28.8f}, 3);
    add({40.4f, g + 1.5f, -29.8f}, {41.4f, g + 2.5f, -28.8f}, 3);
    boxBuffer = gpu.createStructured(sizeof(SceneBox), uint32_t(boxes.size()), false, L"Scene.Boxes");
    m_uploaded = false;
}

void DemoScene::createPipelines(Pipelines& p) {
    GraphicsDesc g;
    g.file = "scene_draw.hlsl";
    g.depth = DepthMode::WriteGreater;
    m_draw = p.graphics(g);
}

void DemoScene::upload(Gpu& gpu) {
    if (m_uploaded) return;
    gpu.uploadBuffer(boxBuffer, boxes.data(), boxes.size() * sizeof(SceneBox));
    gpu.transition(boxBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_uploaded = true;
}

void DemoScene::draw(Gpu& gpu, const Texture& wetMap, const Texture& weatherMap) {
    if (!m_draw) return;
    SceneCB cb{uint32_t(boxes.size()), puddleScale, rippleAmount, showWetnessDebug ? 1.0f : 0.0f};
    gpu.bindGraphics(m_draw.Get(), &cb, sizeof(cb), {boxBuffer, wetMap, weatherMap});
    gpu.cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.cmd()->DrawInstanced(36, UINT(boxes.size()), 0, 0);
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
              uint32_t(scene.boxes.size()), params.terminalSpeed, params.streak, params.brightness};
    // La carte d'occlusion : en production, une passe de profondeur orthographique vue du ciel,
    // mise à jour 1 frame sur 4 ou quand la caméra a bougé d'une demi-carte.
    gpu.dispatch(m_occ.Get(), &cb, sizeof(cb), {scene.boxBuffer}, {occlusion}, divUp(kOccSize, 8), divUp(kOccSize, 8), 1);
    gpu.dispatch(m_sim.Get(), &cb, sizeof(cb), {scene.boxBuffer, wind.volume, occlusion}, {View{}, m_drops},
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
    uint32_t mapSize, boxCount; float wetRate, puddleRate;
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
    WetCB cb{m_size, uint32_t(scene.boxes.size()), params.wetRate, params.puddleRate,
             params.dryBase, params.dryWind, params.drySun, params.puddleDry};
    gpu.dispatch(m_pso.Get(), &cb, sizeof(cb), {scene.boxBuffer, weatherMap}, {map}, divUp(m_size, 8), divUp(m_size, 8), 1);
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
