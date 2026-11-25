#include "Weather.h"

namespace me {

WeatherState WeatherState::lerp(const WeatherState& a, const WeatherState& b, float t) {
    WeatherState r;
    auto L = [t](float x, float y) { return x + (y - x) * t; };
    r.cloudCoverage = L(a.cloudCoverage, b.cloudCoverage);
    r.cloudDensity = L(a.cloudDensity, b.cloudDensity);
    r.cloudType = L(a.cloudType, b.cloudType);
    // Angle : chemin le plus court.
    float d = b.windAngleDeg - a.windAngleDeg;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    r.windAngleDeg = a.windAngleDeg + d * t;
    r.windSpeed = L(a.windSpeed, b.windSpeed);
    r.gustStrength = L(a.gustStrength, b.gustStrength);
    r.precipitation = L(a.precipitation, b.precipitation);
    r.temperatureC = L(a.temperatureC, b.temperatureC);
    r.sunElevationDeg = L(a.sunElevationDeg, b.sunElevationDeg);
    r.sunAzimuthDeg = L(a.sunAzimuthDeg, b.sunAzimuthDeg);
    r.sunIntensity = L(a.sunIntensity, b.sunIntensity);
    r.fetchKm = L(a.fetchKm, b.fetchKm);
    r.fogDensity = L(a.fogDensity, b.fogDensity);
    return r;
}

float3 WeatherState::windDir() const {
    const float a = windAngleDeg * kPi / 180.0f;
    return {std::cos(a), 0.0f, std::sin(a)};
}

float3 WeatherState::sunDir() const {
    const float el = sunElevationDeg * kPi / 180.0f, az = sunAzimuthDeg * kPi / 180.0f;
    return normalize(float3{std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)});
}

const char* presetName(WeatherPreset p) {
    switch (p) {
    case WeatherPreset::Clear: return "Clair";
    case WeatherPreset::Cloudy: return "Nuageux";
    case WeatherPreset::Rain: return "Pluie";
    case WeatherPreset::Storm: return "Tempete";
    default: return "?";
    }
}

WeatherState presetState(WeatherPreset p) {
    WeatherState s;
    switch (p) {
    case WeatherPreset::Clear:
        s.cloudCoverage = 0.25f; s.cloudDensity = 0.5f; s.cloudType = 0.35f;
        s.windSpeed = 5.0f; s.gustStrength = 0.15f; s.precipitation = 0.0f;
        s.sunIntensity = 1.0f; s.fogDensity = 0.0001f;
        break;
    case WeatherPreset::Cloudy:
        s.cloudCoverage = 0.6f; s.cloudDensity = 0.7f; s.cloudType = 0.5f;
        s.windSpeed = 9.0f; s.gustStrength = 0.35f; s.precipitation = 0.0f;
        s.sunIntensity = 0.85f; s.fogDensity = 0.0002f;
        break;
    case WeatherPreset::Rain:
        s.cloudCoverage = 0.85f; s.cloudDensity = 0.9f; s.cloudType = 0.3f;
        s.windSpeed = 11.0f; s.gustStrength = 0.5f; s.precipitation = 0.6f;
        s.sunIntensity = 0.6f; s.temperatureC = 11.0f; s.fogDensity = 0.0004f;
        break;
    case WeatherPreset::Storm:
        s.cloudCoverage = 0.95f; s.cloudDensity = 1.0f; s.cloudType = 0.95f;
        s.windSpeed = 22.0f; s.gustStrength = 0.9f; s.precipitation = 1.0f;
        s.sunIntensity = 0.4f; s.temperatureC = 9.0f; s.fetchKm = 800.0f; s.fogDensity = 0.0008f;
        break;
    default: break;
    }
    return s;
}

void WeatherTimeline::setTarget(const WeatherState& target, float seconds) {
    m_from = m_current;
    m_to = target;
    m_duration = seconds;
    m_t = seconds <= 0.0f ? 1.0f : 0.0f;
    if (m_t >= 1.0f) m_current = target;
}

void WeatherTimeline::update(float dt) {
    if (m_t >= 1.0f) return;
    m_t = std::min(1.0f, m_t + dt / std::max(m_duration, 1e-3f));
    const float s = m_t * m_t * (3.0f - 2.0f * m_t);
    m_current = WeatherState::lerp(m_from, m_to, s);
}

QualitySettings QualitySettings::preset(QualityLevel q) {
    QualitySettings s;
    switch (q) {
    case QualityLevel::Low:
        s.fftSize = 128; s.oceanCascades = 2; s.oceanGridQuads = 256;
        s.cloudResScale = 0.25f; s.cloudSteps = 40; s.cloudLightSteps = 4;
        s.windRes[0] = 32; s.windRes[1] = 16; s.windRes[2] = 32;
        s.maxRainParticles = 16384; s.wetnessMapSize = 512;
        s.budgetOcean = 0.15f; s.budgetClouds = 0.55f; s.budgetRain = 0.10f; s.budgetMisc = 0.05f; s.budgetSurface = 0.35f;
        break;
    case QualityLevel::Medium:
        break; // valeurs par défaut de la structure
    case QualityLevel::Ultra:
        s.fftSize = 256; s.oceanCascades = 3; s.oceanGridQuads = 512;
        s.cloudResScale = 0.5f; s.cloudSteps = 96; s.cloudLightSteps = 6;
        s.maxRainParticles = 131072; s.wetnessMapSize = 2048;
        s.budgetOcean = 0.45f; s.budgetClouds = 1.35f; s.budgetRain = 0.35f; s.budgetMisc = 0.13f; s.budgetSurface = 0.65f;
        break;
    }
    return s;
}

} // namespace me
