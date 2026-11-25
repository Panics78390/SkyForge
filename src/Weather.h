// État météo, préréglages, transitions et réglages qualité.
#pragma once
#include "MathUtil.h"
#include <cstdint>

namespace me {

// Tout ce qui pilote le module. Chaque champ s'interpole linéairement.
struct WeatherState {
    float cloudCoverage = 0.35f;  // [0,1]
    float cloudDensity = 0.6f;    // multiplicateur d'extinction
    float cloudType = 0.4f;       // 0 stratus -> 1 cumulonimbus
    float windAngleDeg = 30.0f;   // direction d'où va le vent, plan XZ
    float windSpeed = 6.0f;       // m/s à 10 m (U10)
    float gustStrength = 0.3f;    // [0,1]
    float precipitation = 0.0f;   // [0,1]
    float temperatureC = 15.0f;
    float sunElevationDeg = 35.0f;
    float sunAzimuthDeg = 140.0f;
    float sunIntensity = 1.0f;
    float fetchKm = 300.0f;       // JONSWAP
    float fogDensity = 0.00015f;  // brouillard de base (s'ajoute à celui de la pluie)

    static WeatherState lerp(const WeatherState& a, const WeatherState& b, float t);
    float3 windDir() const;
    float3 sunDir() const;
};

enum class WeatherPreset { Clear, Cloudy, Rain, Storm, Count };
const char* presetName(WeatherPreset p);
WeatherState presetState(WeatherPreset p);

// Transition douce (smoothstep) vers un état cible.
class WeatherTimeline {
public:
    void setTarget(const WeatherState& target, float seconds);
    void setImmediate(const WeatherState& s) { m_from = m_to = m_current = s; m_t = 1.0f; }
    void update(float dt);
    WeatherState& current() { return m_current; }
    bool transitioning() const { return m_t < 1.0f; }
    float progress() const { return m_t; }
private:
    WeatherState m_from{}, m_to{}, m_current{};
    float m_t = 1.0f, m_duration = 0.0f;
};

enum class QualityLevel : int { Low = 0, Medium = 1, Ultra = 2 };

struct QualitySettings {
    // Eau
    uint32_t fftSize = 256;           // puissance de 2 : 128 ou 256
    uint32_t oceanCascades = 3;       // 1..3
    uint32_t oceanGridQuads = 384;    // maillage de surface (quads par côté)
    // Nuages
    float cloudResScale = 0.25f;      // fraction de la résolution écran
    uint32_t cloudSteps = 64;
    uint32_t cloudLightSteps = 6;
    // Atmosphère
    uint32_t windRes[3] = {64, 32, 64};
    uint32_t maxRainParticles = 49152;
    uint32_t wetnessMapSize = 1024;
    // Budgets par passe (ms) pour le profileur
    float budgetOcean = 0.30f, budgetClouds = 0.90f, budgetRain = 0.20f, budgetMisc = 0.10f, budgetSurface = 0.50f;

    static QualitySettings preset(QualityLevel q);
};

// Constantes par frame, partagées avec shaders/Common.hlsli (même ordre, mêmes paddings).
struct FrameConstants {
    float4x4 viewProj, invViewProj, prevViewProj;
    float3 camPos;       float time;
    float3 sunDir;       float sunIntensity;
    float3 windDir;      float windSpeed;
    float cloudCoverage, cloudDensity, cloudType, precip;
    float wetness, temperature, deltaTime; uint32_t frameIndex;
    float2 resolution, invResolution;
    float3 windOrigin;   float windCellSize;
    float2 cloudOffset;  float gust; float exposure;
    float cloudBottom, cloudTop, fogDensity, oceanTime;
    float3 camForward;   float puddleAmount;
    float lodNear, lodFar, gerstnerAmp, gerstnerWavelength;
    float3 islandMin;    float waterLevel;
    float3 islandMax;    float debugView;
    uint32_t windRes[3]; float rainBoxSize;
};
static_assert(sizeof(FrameConstants) % 16 == 0, "FrameConstants doit être aligné sur 16 octets");
static_assert(sizeof(FrameConstants) == 416, "désaccord avec Common.hlsli");

} // namespace me
