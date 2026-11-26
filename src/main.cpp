// SkyForge : démonstrateur météo et eau DirectX 12 réglable en temps réel.
//
// Ordre des passes dans une frame (voir README) :
//   1. Vent 3D -> 2. Océan FFT -> 3. Pluie (occlusion + simulation) -> 4. Humidité
//   5. Scène + surface d'eau (HDR, Z inversé) -> 6. Nuages (march + reprojection)
//   7. Composition ciel/nuages -> 8. Gouttes de pluie -> 9. Tonemapping -> 10. Interface
#include "Gpu.h"
#include "Pipelines.h"
#include "Weather.h"
#include "Ocean.h"
#include "Clouds.h"
#include "Atmos.h"
#include "Terrain.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace me {

struct Camera {
    float3 pos{-45.0f, 4.0f, -45.0f};
    float yaw = 0.3f, pitch = -0.05f;
    float fovDeg = 60.0f;
    float speed = 15.0f;
    float3 forward() const {
        return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    }
};

class App {
public:
    bool init(HWND hwnd, UINT w, UINT h);
    void frame();
    void shutdown();
    void onResize(UINT w, UINT h) { m_pendingW = w; m_pendingH = h; }

private:
    void createTargets();
    void createSubsystems();
    void createPipelines();
    void writePreviews();
    void applyResize();
    void updateCamera(float dt);
    void buildUi(float dt);
    void uiProfiler();
    void saveSettings(const char* path);
    void loadSettings(const char* path);
    std::map<std::string, float*> settingsTable();

    HWND m_hwnd = nullptr;
    Gpu m_gpu;
    Pipelines m_pipes;
    GpuProfiler m_prof;
    QualityLevel m_qualityLevel = QualityLevel::Medium;
    QualitySettings m_quality;
    WeatherTimeline m_timeline;
    float m_transitionSeconds = 20.0f;

    Ocean m_ocean;
    Clouds m_clouds;
    WindField m_wind;
    RainSystem m_rain;
    WetnessSystem m_wet;
    DemoScene m_scene;

    Texture m_hdr, m_hdr2, m_depth;
    ComPtr<ID3D12PipelineState> m_tonemap;

    Camera m_cam;
    float4x4 m_prevViewProj;
    bool m_firstFrame = true;
    float m_time = 0, m_oceanTime = 0, m_timeScale = 1.0f;
    bool m_paused = false, m_vsync = true, m_dayCycle = false;
    float m_dayCycleSpeed = 2.0f; // degrés par seconde
    float m_exposure = 1.0f, m_puddleAmount = 1.0f;
    float2 m_cloudOffset;
    bool m_enableOcean = true, m_enableClouds = true, m_enableRain = true, m_enableWetness = true;
    bool m_showUi = true, m_showPreviews = false, m_needsBake = true;
    UINT m_pendingW = 0, m_pendingH = 0;
    LARGE_INTEGER m_lastTick{}, m_freq{};
    float m_frameMs = 16.0f;
    UINT m_fontSlot = 0;
    UINT m_previewSlots[5] = {};
    std::string m_status;
    float m_cameraSpeed = 0.0f;
    int m_cloudRes = -1; // -1 = selon la qualité
    float m_rockSeedF = 7, m_scatteredF = 45; // copies flottantes pour le fichier de réglages
};

// ---------------------------------------------------------------------------
bool App::init(HWND hwnd, UINT w, UINT h) {
    m_hwnd = hwnd;
    m_gpu.init(hwnd, w, h);
    m_prof.init(m_gpu);

    // Les shaders sont dans le dossier « shaders » à côté de l'exécutable.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    m_pipes.init(m_gpu, dir + L"/shaders");

    m_timeline.setImmediate(presetState(WeatherPreset::Clear));
    loadSettings("skyforge.ini");
    m_quality = QualitySettings::preset(m_qualityLevel);

    m_fontSlot = m_gpu.allocPersistent();
    for (auto& s : m_previewSlots) s = m_gpu.allocPersistent();

    createTargets();
    m_scene.create(m_gpu);
    createSubsystems();
    createPipelines();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "skyforge_imgui.ini";
    ImGui::StyleColorsDark();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui::GetStyle().ScaleAllSizes(dpi);
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;
    // Police Windows avec accents (la police par défaut d'ImGui n'a que l'ASCII).
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpi, nullptr, io.Fonts->GetGlyphRangesDefault());
    else
        io.FontGlobalScale = dpi;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(m_gpu.device(), kFramesInFlight, DXGI_FORMAT_R8G8B8A8_UNORM, m_gpu.srvHeap(),
                        m_gpu.persistentCpu(m_fontSlot), m_gpu.persistentGpu(m_fontSlot));

    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_lastTick);
    return true;
}

void App::createTargets() {
    const UINT w = m_gpu.width(), h = m_gpu.height();
    m_gpu.releaseViews(m_hdr);
    m_gpu.releaseViews(m_hdr2);
    m_gpu.releaseViews(m_depth);
    m_hdr = m_gpu.createTexture2D(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, TexRT, L"HDR.Scene");
    m_hdr2 = m_gpu.createTexture2D(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, TexRT, L"HDR.Composite");
    m_depth = m_gpu.createTexture2D(w, h, DXGI_FORMAT_D32_FLOAT, TexDepth, L"Depth");
}

void App::createSubsystems() {
    m_ocean.create(m_gpu, m_quality);
    m_clouds.create(m_gpu, m_quality, m_gpu.width(), m_gpu.height());
    m_wind.create(m_gpu, m_quality);
    m_rain.create(m_gpu, m_quality);
    m_wet.create(m_gpu, m_quality);
    writePreviews();
}

void App::createPipelines() {
    m_pipes.clearErrors();
    m_ocean.createPipelines(m_pipes);
    m_clouds.createPipelines(m_pipes);
    m_wind.createPipelines(m_pipes);
    m_rain.createPipelines(m_pipes);
    m_wet.createPipelines(m_pipes);
    m_scene.createPipelines(m_pipes);
    GraphicsDesc g;
    g.file = "tonemap.hlsl";
    g.rtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_tonemap = m_pipes.graphics(g);
    m_status = m_pipes.errors().empty() ? "Shaders compilés." : "Erreurs de compilation (voir ci-dessous).";
}

void App::writePreviews() {
    const Texture* t[5] = {&m_wet.map, &m_clouds.weatherMap, &m_rain.occlusion, &m_ocean.disp[0], &m_ocean.deriv[0]};
    for (int i = 0; i < 5; ++i) m_gpu.writeSrv(View(*t[i]), m_gpu.persistentCpu(m_previewSlots[i]));
}

void App::applyResize() {
    if (m_pendingW == 0 || m_pendingH == 0) return;
    if (m_pendingW == m_gpu.width() && m_pendingH == m_gpu.height()) { m_pendingW = m_pendingH = 0; return; }
    m_gpu.resize(m_pendingW, m_pendingH);
    createTargets();
    m_clouds.create(m_gpu, m_quality, m_gpu.width(), m_gpu.height());
    writePreviews();
    m_pendingW = m_pendingH = 0;
}

void App::updateCamera(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (GetForegroundWindow() != m_hwnd) return;
    // Clic droit maintenu : regarder autour.
    if (!io.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        m_cam.yaw += io.MouseDelta.x * 0.0035f;
        m_cam.pitch = std::clamp(m_cam.pitch - io.MouseDelta.y * 0.0035f, -1.5f, 1.5f);
    }
    if (io.WantCaptureKeyboard) return;
    const float3 f = m_cam.forward();
    const float3 r = normalize(cross(float3{0, 1, 0}, f));
    float3 move{};
    auto down = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
    if (down('W') || down('Z')) move += f;
    if (down('S')) move = move - f;
    if (down('D')) move += r;
    if (down('A') || down('Q')) move = move - r;
    if (down('E') || down(VK_SPACE)) move += float3{0, 1, 0};
    if (down('C') || down(VK_CONTROL)) move = move - float3{0, 1, 0};
    const float boost = down(VK_SHIFT) ? 5.0f : 1.0f;
    m_cam.pos += move * (m_cam.speed * boost * dt);
    // On reste au-dessus du sable et de la mer.
    m_cam.pos.y = std::max(m_cam.pos.y, std::max(terrain::height(m_cam.pos.x, m_cam.pos.z), 0.0f) + 0.8f);
}

void App::frame() {
    applyResize();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float realDt = float(double(now.QuadPart - m_lastTick.QuadPart) / double(m_freq.QuadPart));
    m_lastTick = now;
    realDt = std::min(realDt, 0.1f);
    m_frameMs = m_frameMs * 0.95f + realDt * 1000.0f * 0.05f;
    const float dt = m_paused ? 0.0f : realDt * m_timeScale;

    m_gpu.beginFrame();
    m_prof.beginFrame();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) m_showUi = !m_showUi;
    if (m_showUi) buildUi(realDt);
    const float3 camBefore = m_cam.pos;
    updateCamera(realDt);
    m_cameraSpeed = realDt > 0.0f ? length(m_cam.pos - camBefore) / realDt : 0.0f;

    // --- Mise à jour CPU -----------------------------------------------------
    m_timeline.update(dt);
    WeatherState& w = m_timeline.current();
    if (m_dayCycle) {
        w.sunElevationDeg += m_dayCycleSpeed * dt;
        if (w.sunElevationDeg > 180.0f) w.sunElevationDeg -= 360.0f;
    }
    m_wet.updateGlobal(w, dt);
    m_time += dt;
    m_oceanTime = std::fmod(m_oceanTime + dt, 200.0f); // le spectre boucle sur 200 s
    const float3 wd = w.windDir();
    const float cloudWind = w.windSpeed * 2.5f * m_clouds.params.windInfluence; // vent d'altitude plus fort
    m_cloudOffset.x = std::fmod(m_cloudOffset.x + wd.x * cloudWind * dt, 300000.0f);
    m_cloudOffset.y = std::fmod(m_cloudOffset.y + wd.z * cloudWind * dt, 300000.0f);

    const float aspect = float(m_gpu.width()) / float(std::max(1u, m_gpu.height()));
    const float4x4 view = float4x4::lookTo(m_cam.pos, m_cam.forward(), {0, 1, 0});
    const float4x4 proj = float4x4::perspectiveReversedInf(m_cam.fovDeg * kPi / 180.0f, aspect, 0.1f);
    const float4x4 viewProj = view * proj;
    if (m_firstFrame) m_prevViewProj = viewProj;

    FrameConstants fc{};
    fc.viewProj = viewProj;
    fc.invViewProj = viewProj.inverse();
    fc.prevViewProj = m_prevViewProj;
    fc.camPos = m_cam.pos;
    fc.time = m_time;
    fc.sunDir = w.sunDir();
    fc.sunIntensity = w.sunIntensity;
    fc.windDir = wd;
    fc.windSpeed = w.windSpeed;
    fc.cloudCoverage = w.cloudCoverage;
    fc.cloudDensity = w.cloudDensity;
    fc.cloudType = w.cloudType;
    fc.precip = m_enableRain ? w.precipitation : 0.0f;
    fc.wetness = m_enableWetness ? m_wet.globalWetness : 0.0f;
    fc.temperature = w.temperatureC;
    fc.deltaTime = dt;
    fc.frameIndex = uint32_t(m_gpu.frameCount());
    fc.resolution = {float(m_gpu.width()), float(m_gpu.height())};
    fc.invResolution = {1.0f / fc.resolution.x, 1.0f / fc.resolution.y};
    fc.windOrigin = m_wind.origin(m_cam.pos);
    fc.windCellSize = m_wind.params.cellSize;
    fc.cloudOffset = m_cloudOffset;
    fc.gust = w.gustStrength;
    fc.exposure = m_exposure;
    fc.cloudBottom = m_clouds.params.bottom;
    fc.cloudTop = std::max(m_clouds.params.top, m_clouds.params.bottom + 100.0f);
    fc.fogDensity = w.fogDensity;
    fc.oceanTime = m_oceanTime;
    fc.camForward = m_cam.forward();
    fc.puddleAmount = m_enableWetness ? m_puddleAmount : 0.0f;
    fc.lodNear = m_ocean.params.lodNear;
    fc.lodFar = m_ocean.params.lodFar;
    fc.gerstnerAmp = m_ocean.gerstnerAmplitude();
    fc.gerstnerWavelength = m_ocean.gerstnerWavelength();
    fc.terrainMin = m_scene.terrainMin;
    fc.waterLevel = 0.0f;
    fc.terrainMax = m_scene.terrainMax;
    for (int i = 0; i < 3; ++i) fc.windRes[i] = m_wind.res()[i];
    fc.rainBoxSize = m_rain.params.boxHalf;
    m_gpu.setFrameConstants(&fc, sizeof(fc));

    // --- Simulation (compute) ------------------------------------------------
    m_scene.upload(m_gpu);
    if (m_needsBake) {
        ScopedPass sp(m_prof, "Bruit nuages (1x)");
        m_clouds.bakeNoise(m_gpu);
        m_needsBake = false;
    }
    m_wind.update(m_gpu, m_prof);
    if (m_enableOcean) m_ocean.simulate(m_gpu, m_prof, w, dt);
    if (m_enableRain) m_rain.simulate(m_gpu, m_prof, m_scene, m_wind, m_cam.pos, w.precipitation);
    if (m_enableWetness) m_wet.update(m_gpu, m_prof, m_scene, m_clouds.weatherMap);

    // --- Scène opaque --------------------------------------------------------
    auto* cmd = m_gpu.cmd();
    m_gpu.transition(m_hdr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_gpu.transition(m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const float clear[4] = {0, 0, 0, 1};
    cmd->ClearRenderTargetView(m_hdr.rtv, clear, 0, nullptr);
    cmd->ClearDepthStencilView(m_depth.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    m_gpu.setViewport(m_gpu.width(), m_gpu.height());
    {
        ScopedPass sp(m_prof, "Scene");
        cmd->OMSetRenderTargets(1, &m_hdr.rtv, FALSE, &m_depth.dsv);
        m_scene.draw(m_gpu, m_wet.map, m_clouds.weatherMap);
    }
    if (m_enableOcean) {
        ScopedPass sp(m_prof, "Surface eau");
        cmd->OMSetRenderTargets(1, &m_hdr.rtv, FALSE, &m_depth.dsv);
        m_ocean.draw(m_gpu, m_clouds.weatherMap);
    }

    // --- Nuages + composition -----------------------------------------------
    if (m_enableClouds) m_clouds.render(m_gpu, m_prof, m_depth, m_cameraSpeed);
    else m_clouds.invalidateHistory();
    {
        ScopedPass sp(m_prof, "Composite");
        m_gpu.transition(m_hdr2, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_gpu.setViewport(m_gpu.width(), m_gpu.height());
        cmd->OMSetRenderTargets(1, &m_hdr2.rtv, FALSE, nullptr);
        m_clouds.composite(m_gpu, m_hdr, m_depth, m_enableClouds); // le ciel est composé même sans nuages
    }
    if (m_enableRain) {
        ScopedPass sp(m_prof, "Pluie rendu");
        // Profondeur en lecture seule : test sans écriture, toujours lisible en SRV.
        m_gpu.transition(m_depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->OMSetRenderTargets(1, &m_hdr2.rtv, FALSE, &m_depth.dsv);
        m_rain.draw(m_gpu);
    }

    // --- Sortie --------------------------------------------------------------
    {
        ScopedPass sp(m_prof, "Tonemap");
        m_gpu.transition(m_gpu.backBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->OMSetRenderTargets(1, &m_gpu.backBuffer().rtv, FALSE, nullptr);
        m_gpu.bindGraphics(m_tonemap.Get(), nullptr, 0, {m_hdr2});
        if (m_tonemap) {
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawInstanced(3, 1, 0, 0);
        }
    }
    // Les aperçus affichés par l'interface doivent être en état « lecture ».
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_gpu.transition(m_wet.map, srv);
    m_gpu.transition(m_clouds.weatherMap, srv);
    m_gpu.transition(m_rain.occlusion, srv);
    m_gpu.transition(m_ocean.disp[0], srv);
    m_gpu.transition(m_ocean.deriv[0], srv);
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

    m_prof.endFrame();
    m_gpu.endFrame(m_vsync);
    m_prevViewProj = viewProj;
    m_firstFrame = false;
}

// ---------------------------------------------------------------------------
// Interface
static bool SliderDeg(const char* label, float* v, float mn, float mx) { return ImGui::SliderFloat(label, v, mn, mx, "%.0f°"); }

void App::uiProfiler() {
    const auto& q = m_quality;
    struct Budget { const char* name; float ms; };
    const Budget budgets[] = {
        {"Vent 3D", q.budgetMisc * 0.4f},  {"Ocean FFT", q.budgetOcean},
        {"Pluie sim", q.budgetRain * 0.4f}, {"Humidite", q.budgetMisc * 0.6f},
        {"Surface eau", q.budgetSurface},  {"Nuages march", q.budgetClouds * 0.85f},
        {"Nuages reproj", q.budgetClouds * 0.15f}, {"Pluie rendu", q.budgetRain * 0.6f},
    };
    float moduleMs = 0;
    if (ImGui::BeginTable("prof", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Passe");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("Budget");
        ImGui::TableHeadersRow();
        for (const auto& e : m_prof.results()) {
            float budget = -1;
            for (const auto& b : budgets) if (std::strcmp(b.name, e.name) == 0) budget = b.ms;
            if (budget > 0) moduleMs += e.avgMs;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name);
            ImGui::TableNextColumn();
            const ImVec4 col = budget < 0 ? ImVec4(0.7f, 0.7f, 0.7f, 1)
                             : e.avgMs <= budget ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1.0f, 0.45f, 0.35f, 1);
            ImGui::TextColored(col, "%.3f", e.avgMs);
            ImGui::TableNextColumn();
            if (budget > 0) ImGui::Text("%.2f", budget);
            else ImGui::TextDisabled("hors module");
        }
        ImGui::EndTable();
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Module : %.2f / 4.00 ms", moduleMs);
    ImGui::ProgressBar(std::min(moduleMs / 4.0f, 1.0f), ImVec2(-1, 0), buf);
}

void App::buildUi(float dt) {
    (void)dt;
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 760), ImGuiCond_FirstUseEver);
    ImGui::Begin("SkyForge  (F1 : masquer)");
    ImGui::Text("%.1f FPS  (%.2f ms)  |  GPU mesuré : %.2f ms", 1000.0f / std::max(m_frameMs, 0.01f), m_frameMs, m_prof.totalMs());
    ImGui::TextDisabled("Clic droit + souris : regarder. ZQSD/WASD : bouger, Maj : vite, Espace/C : monter/descendre");

    WeatherState& w = m_timeline.current();
    bool edited = false;

    if (ImGui::CollapsingHeader("Préréglages météo", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int p = 0; p < int(WeatherPreset::Count); ++p) {
            if (p) ImGui::SameLine();
            if (ImGui::Button(presetName(WeatherPreset(p)), ImVec2(90, 0))) {
                WeatherState target = presetState(WeatherPreset(p));
                // On garde la position du soleil choisie par l'utilisateur.
                target.sunElevationDeg = w.sunElevationDeg;
                target.sunAzimuthDeg = w.sunAzimuthDeg;
                m_timeline.setTarget(target, m_transitionSeconds);
            }
        }
        ImGui::SliderFloat("Durée de transition (s)", &m_transitionSeconds, 0.0f, 120.0f, "%.0f");
        if (m_timeline.transitioning()) ImGui::ProgressBar(m_timeline.progress(), ImVec2(-1, 0), "Transition en cours");
    }
    if (ImGui::CollapsingHeader("Météo", ImGuiTreeNodeFlags_DefaultOpen)) {
        edited |= ImGui::SliderFloat("Couverture nuageuse", &w.cloudCoverage, 0.0f, 1.0f);
        edited |= ImGui::SliderFloat("Densité des nuages", &w.cloudDensity, 0.0f, 2.0f);
        edited |= ImGui::SliderFloat("Type (stratus > cumulonimbus)", &w.cloudType, 0.0f, 1.0f);
        edited |= SliderDeg("Direction du vent", &w.windAngleDeg, 0.0f, 360.0f);
        edited |= ImGui::SliderFloat("Vitesse du vent (m/s)", &w.windSpeed, 0.0f, 35.0f, "%.1f");
        edited |= ImGui::SliderFloat("Rafales", &w.gustStrength, 0.0f, 1.0f);
        edited |= ImGui::SliderFloat("Précipitations", &w.precipitation, 0.0f, 1.0f);
        edited |= ImGui::SliderFloat("Température (°C)", &w.temperatureC, -10.0f, 40.0f, "%.0f");
        edited |= ImGui::SliderFloat("Fetch (km)", &w.fetchKm, 5.0f, 1500.0f, "%.0f");
        edited |= ImGui::SliderFloat("Brouillard", &w.fogDensity, 0.0f, 0.003f, "%.5f");
    }
    if (ImGui::CollapsingHeader("Soleil")) {
        edited |= SliderDeg("Élévation", &w.sunElevationDeg, -10.0f, 90.0f);
        edited |= SliderDeg("Azimut", &w.sunAzimuthDeg, 0.0f, 360.0f);
        edited |= ImGui::SliderFloat("Intensité", &w.sunIntensity, 0.0f, 3.0f);
        ImGui::Checkbox("Course du soleil automatique", &m_dayCycle);
        ImGui::SliderFloat("Vitesse (°/s)", &m_dayCycleSpeed, 0.1f, 20.0f);
        ImGui::SliderFloat("Exposition", &m_exposure, 0.1f, 4.0f);
    }
    if (edited) m_timeline.setImmediate(w); // une retouche manuelle arrête la transition

    if (ImGui::CollapsingHeader("Océan")) {
        auto& o = m_ocean.params;
        ImGui::SliderFloat("Vagues pointues (choppiness)", &o.choppiness, 0.0f, 2.5f);
        ImGui::SliderFloat("Écume", &o.foamIntensity, 0.0f, 3.0f);
        ImGui::SliderFloat("Disparition écume (1/s)", &o.foamDecay, 0.05f, 4.0f);
        ImGui::SliderFloat("Cascade 0 (m)", &o.patchSizes[0], 100.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("Cascade 1 (m)", &o.patchSizes[1], 20.0f, 150.0f, "%.0f");
        ImGui::SliderFloat("Cascade 2 (m)", &o.patchSizes[2], 3.0f, 25.0f, "%.1f");
        ImGui::SliderFloat("LOD : précision max (m)", &o.lodNear, 10.0f, 200.0f, "%.0f");
        ImGui::SliderFloat("LOD : Gerstner au-delà (m)", &o.lodFar, 100.0f, 3000.0f, "%.0f");
        ImGui::Text("Gerstner : amplitude %.2f m, longueur %.1f m", m_ocean.gerstnerAmplitude(), m_ocean.gerstnerWavelength());
    }
    if (ImGui::CollapsingHeader("Nuages")) {
        auto& c = m_clouds.params;
        ImGui::SliderFloat("Base (m)", &c.bottom, 300.0f, 4000.0f, "%.0f");
        ImGui::SliderFloat("Sommet (m)", &c.top, 800.0f, 9000.0f, "%.0f");
        ImGui::SliderFloat("Taille des formes (m)", &c.shapeSize, 2000.0f, 30000.0f, "%.0f");
        ImGui::SliderFloat("Taille du détail (m)", &c.detailSize, 200.0f, 4000.0f, "%.0f");
        ImGui::SliderFloat("Érosion des bords", &c.detailStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Extinction (1/m)", &c.extinction, 0.005f, 0.15f, "%.3f");
        ImGui::SliderFloat("Henyey-Greenstein avant", &c.g1, 0.0f, 0.95f);
        ImGui::SliderFloat("Henyey-Greenstein arrière", &c.g2, -0.9f, 0.0f);
        ImGui::SliderFloat("Effet powder", &c.powder, 0.0f, 1.0f);
        ImGui::SliderFloat("Lumière ambiante", &c.ambient, 0.0f, 2.0f);
        ImGui::SliderFloat("Poids de l'historique", &c.historyBlend, 0.0f, 0.98f);
        ImGui::SliderFloat("Dérive avec le vent", &c.windInfluence, 0.0f, 5.0f);
        ImGui::SliderFloat("Pas fin près de la caméra (m)", &c.nearStep, 1.0f, 40.0f, "%.1f");
        int res = m_cloudRes < 0 ? (m_quality.cloudResScale >= 0.99f ? 2 : m_quality.cloudResScale >= 0.49f ? 1 : 0) : m_cloudRes;
        if (ImGui::Combo("Résolution des nuages", &res, "1/4 (rapide)\0" "1/2\0" "Pleine (lent)\0")) {
            m_cloudRes = res;
            m_gpu.waitIdle();
            m_quality.cloudResScale = res == 0 ? 0.25f : res == 1 ? 0.5f : 1.0f;
            m_clouds.create(m_gpu, m_quality, m_gpu.width(), m_gpu.height());
            writePreviews();
        }
        if (ImGui::Button("Monter dans les nuages")) {
            m_cam.pos.y = c.bottom + (c.top - c.bottom) * 0.25f;
            m_cam.pitch = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Redescendre")) {
            m_cam.pos.y = std::max(terrain::height(m_cam.pos.x, m_cam.pos.z), 0.0f) + 4.0f;
        }
        ImGui::TextDisabled("Astuce : Maj + vitesse caméra élevée pour traverser vite les nuages.");
    }
    if (ImGui::CollapsingHeader("Pluie")) {
        auto& r = m_rain.params;
        ImGui::Text("Gouttes actives : %u", m_rain.activeCount());
        ImGui::SliderFloat("Rayon de la zone (m)", &r.boxHalf, 10.0f, 60.0f, "%.0f");
        ImGui::SliderFloat("Vitesse de chute (m/s)", &r.terminalSpeed, 2.0f, 15.0f, "%.1f");
        ImGui::SliderFloat("Longueur des traits", &r.streak, 0.002f, 0.06f, "%.3f");
        ImGui::SliderFloat("Luminosité", &r.brightness, 0.0f, 3.0f);
    }
    if (ImGui::CollapsingHeader("Plage et rochers")) {
        bool regen = ImGui::SliderInt("Graine des rochers", &m_scene.rockSeed, 1, 999);
        regen |= ImGui::SliderInt("Blocs épars", &m_scene.scatteredRocks, 0, 100);
        regen |= ImGui::Checkbox("Arche rocheuse (abri contre la pluie)", &m_scene.showArch);
        if (regen) m_scene.generateRocks();
        ImGui::Text("%u rochers", m_scene.rockCountGpu());
        if (ImGui::Button("Aller à l'arche")) {
            const float z = terrain::shoreLine(-25.0f) + 32.0f;
            m_cam.pos = {-25.0f, terrain::height(-25.0f, z - 22.0f) + 2.5f, z - 22.0f};
            m_cam.yaw = 0.0f;
            m_cam.pitch = 0.05f;
        }
    }
    if (ImGui::CollapsingHeader("Surfaces mouillées")) {
        auto& p = m_wet.params;
        ImGui::SliderFloat("Humidité globale", &m_wet.globalWetness, 0.0f, 1.0f);
        ImGui::SliderFloat("Vitesse de mouillage", &p.wetRate, 0.0f, 0.5f);
        ImGui::SliderFloat("Remplissage des flaques", &p.puddleRate, 0.0f, 0.3f);
        ImGui::SliderFloat("Séchage de base", &p.dryBase, 0.0f, 0.05f, "%.4f");
        ImGui::SliderFloat("Séchage par le vent", &p.dryWind, 0.0f, 0.01f, "%.4f");
        ImGui::SliderFloat("Séchage par le soleil", &p.drySun, 0.0f, 0.1f, "%.4f");
        ImGui::SliderFloat("Séchage relatif des flaques", &p.puddleDry, 0.0f, 1.0f);
        ImGui::SliderFloat("Quantité de flaques", &m_puddleAmount, 0.0f, 1.5f);
        ImGui::SliderFloat("Échelle du relief (1/m)", &m_scene.puddleScale, 0.02f, 0.6f);
        ImGui::SliderFloat("Ondulations des impacts", &m_scene.rippleAmount, 0.0f, 3.0f);
        ImGui::Checkbox("Afficher humidité (rouge) / flaques (vert)", &m_scene.showWetnessDebug);
    }
    if (ImGui::CollapsingHeader("Qualité et simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
        int q = int(m_qualityLevel);
        if (ImGui::Combo("Qualité", &q, "Bas\0Moyen\0Ultra\0")) {
            m_gpu.waitIdle();
            m_qualityLevel = QualityLevel(q);
            m_quality = QualitySettings::preset(m_qualityLevel);
            m_cloudRes = -1;
            createSubsystems();
            createPipelines();
        }
        ImGui::Checkbox("Océan", &m_enableOcean);
        ImGui::SameLine();
        ImGui::Checkbox("Nuages", &m_enableClouds);
        ImGui::SameLine();
        ImGui::Checkbox("Pluie", &m_enableRain);
        ImGui::SameLine();
        ImGui::Checkbox("Humidité", &m_enableWetness);
        ImGui::Checkbox("Synchro verticale", &m_vsync);
        ImGui::SameLine();
        ImGui::Checkbox("Pause", &m_paused);
        ImGui::SliderFloat("Vitesse du temps", &m_timeScale, 0.0f, 10.0f);
        ImGui::SliderFloat("Champ de vision", &m_cam.fovDeg, 30.0f, 100.0f, "%.0f°");
        ImGui::SliderFloat("Vitesse caméra", &m_cam.speed, 2.0f, 600.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("Profileur GPU", ImGuiTreeNodeFlags_DefaultOpen)) uiProfiler();
    if (ImGui::CollapsingHeader("Aperçu des textures")) {
        const char* names[5] = {"Humidité / flaques", "Carte météo", "Occlusion pluie", "Déplacement océan", "Pentes océan"};
        for (int i = 0; i < 5; ++i) {
            ImGui::Text("%s", names[i]);
            ImGui::Image(ImTextureID(m_gpu.persistentGpu(m_previewSlots[i]).ptr), ImVec2(180, 180));
        }
    }
    if (ImGui::CollapsingHeader("Shaders et réglages")) {
        if (ImGui::Button("Recharger les shaders")) {
            m_gpu.waitIdle();
            createPipelines();
        }
        ImGui::SameLine();
        if (ImGui::Button("Enregistrer")) { saveSettings("skyforge.ini"); m_status = "Réglages enregistrés dans skyforge.ini"; }
        ImGui::SameLine();
        if (ImGui::Button("Charger")) {
            const QualityLevel before = m_qualityLevel;
            loadSettings("skyforge.ini");
            if (before != m_qualityLevel) {
                m_gpu.waitIdle();
                m_quality = QualitySettings::preset(m_qualityLevel);
                createSubsystems();
                createPipelines();
            }
            m_status = "Réglages chargés.";
        }
        ImGui::TextWrapped("%s", m_status.c_str());
        if (!m_pipes.errors().empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", m_pipes.errors().c_str());
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Enregistrement des réglages (fichier texte clé = valeur).
std::map<std::string, float*> App::settingsTable() {
    WeatherState& w = m_timeline.current();
    auto& o = m_ocean.params;
    auto& c = m_clouds.params;
    auto& r = m_rain.params;
    auto& p = m_wet.params;
    return {
        {"meteo.couverture", &w.cloudCoverage}, {"meteo.densite", &w.cloudDensity}, {"meteo.type", &w.cloudType},
        {"meteo.vent_angle", &w.windAngleDeg}, {"meteo.vent_vitesse", &w.windSpeed}, {"meteo.rafales", &w.gustStrength},
        {"meteo.precipitations", &w.precipitation}, {"meteo.temperature", &w.temperatureC},
        {"meteo.soleil_elevation", &w.sunElevationDeg}, {"meteo.soleil_azimut", &w.sunAzimuthDeg},
        {"meteo.soleil_intensite", &w.sunIntensity}, {"meteo.fetch_km", &w.fetchKm}, {"meteo.brouillard", &w.fogDensity},
        {"ocean.choppiness", &o.choppiness}, {"ocean.ecume", &o.foamIntensity}, {"ocean.ecume_decroissance", &o.foamDecay},
        {"ocean.cascade0", &o.patchSizes[0]}, {"ocean.cascade1", &o.patchSizes[1]}, {"ocean.cascade2", &o.patchSizes[2]},
        {"ocean.lod_proche", &o.lodNear}, {"ocean.lod_loin", &o.lodFar},
        {"nuages.base", &c.bottom}, {"nuages.sommet", &c.top}, {"nuages.taille_forme", &c.shapeSize},
        {"nuages.taille_detail", &c.detailSize}, {"nuages.erosion", &c.detailStrength}, {"nuages.extinction", &c.extinction},
        {"nuages.hg_avant", &c.g1}, {"nuages.hg_arriere", &c.g2}, {"nuages.powder", &c.powder},
        {"nuages.ambiant", &c.ambient}, {"nuages.pas_proche", &c.nearStep}, {"nuages.historique", &c.historyBlend}, {"nuages.derive_vent", &c.windInfluence},
        {"pluie.rayon", &r.boxHalf}, {"pluie.vitesse", &r.terminalSpeed}, {"pluie.traits", &r.streak},
        {"pluie.luminosite", &r.brightness},
        {"humidite.mouillage", &p.wetRate}, {"humidite.flaques", &p.puddleRate}, {"humidite.sechage_base", &p.dryBase},
        {"humidite.sechage_vent", &p.dryWind}, {"humidite.sechage_soleil", &p.drySun},
        {"humidite.sechage_flaques", &p.puddleDry}, {"humidite.quantite_flaques", &m_puddleAmount},
        {"humidite.relief", &m_scene.puddleScale}, {"humidite.ondulations", &m_scene.rippleAmount},
        {"rendu.exposition", &m_exposure}, {"rendu.fov", &m_cam.fovDeg}, {"camera.vitesse", &m_cam.speed},
        {"camera.x", &m_cam.pos.x}, {"camera.y", &m_cam.pos.y}, {"camera.z", &m_cam.pos.z},
        {"camera.yaw", &m_cam.yaw}, {"camera.pitch", &m_cam.pitch},
        {"plage.graine", &m_rockSeedF}, {"plage.blocs", &m_scatteredF},
    };
}

void App::saveSettings(const char* path) {
    m_rockSeedF = float(m_scene.rockSeed);
    m_scatteredF = float(m_scene.scatteredRocks);
    std::ofstream f(path);
    if (!f) return;
    f << "# SkyForge : réglages (clé = valeur)\n";
    f << "qualite = " << int(m_qualityLevel) << "\n";
    for (const auto& [k, v] : settingsTable()) f << k << " = " << *v << "\n";
}

void App::loadSettings(const char* path) {
    std::ifstream f(path);
    if (!f) return;
    auto table = settingsTable();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        const float value = std::strtof(line.c_str() + eq + 1, nullptr);
        if (key == "qualite") m_qualityLevel = QualityLevel(std::clamp(int(value), 0, 2));
        else if (auto it = table.find(key); it != table.end()) *it->second = value;
    }
    m_timeline.setImmediate(m_timeline.current());
    m_scene.rockSeed = int(m_rockSeedF);
    m_scene.scatteredRocks = int(m_scatteredF);
    m_scene.generateRocks();
}

void App::shutdown() {
    m_gpu.waitIdle();
    saveSettings("skyforge.ini");
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_gpu.shutdown();
}

} // namespace me

// ---------------------------------------------------------------------------
static me::App* g_app = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_app && wp != SIZE_MINIMIZED) g_app->onResize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0; // pas de menu sur Alt
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SkyForgeWindow";
    RegisterClassExW(&wc);
    RECT r{0, 0, 1600, 900};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SkyForge : météo et eau en temps réel (DirectX 12)", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT client;
    GetClientRect(hwnd, &client);

    static me::App app;
    g_app = &app;
    app.init(hwnd, UINT(client.right - client.left), UINT(client.bottom - client.top));
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(hwnd)) { Sleep(16); continue; }
        app.frame();
    }
    app.shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
