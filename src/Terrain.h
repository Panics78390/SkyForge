// Copie C++ exacte de TerrainHeight() (shaders/Scene.hlsli) : sert à poser les rochers
// sur le sable et à empêcher la caméra de passer sous le terrain.
#pragma once
#include "MathUtil.h"
#include <cstdint>

namespace me::terrain {

inline uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) { return hash(a ^ hash(b ^ hash(c))); }
inline float hashF(uint32_t x) { return float(hash(x) & 0x00FFFFFFu) / 16777216.0f; }
inline float frac(float v) { return v - std::floor(v); }
inline float smoothstep(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float noise(float px, float py) {
    const int ix = int(std::floor(px)), iy = int(std::floor(py));
    float fx = frac(px), fy = frac(py);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    auto h = [](int x, int y) { return hashF(hash3(uint32_t(x), uint32_t(y), 11u)); };
    const float a = h(ix, iy), b = h(ix + 1, iy), c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

inline float fbm(float x, float y) {
    return noise(x, y) * 0.5f + noise(x * 2.07f + 17.3f, y * 2.07f + 17.3f) * 0.3f +
           noise(x * 4.13f + 41.7f, y * 4.13f + 41.7f) * 0.2f;
}

inline float shoreLine(float x) {
    return 12.0f * std::sin(x * 0.018f) + 7.0f * std::sin(x * 0.047f + 1.3f) + (fbm(x * 0.01f, 3.7f) - 0.5f) * 24.0f;
}

// x, z en mètres ; renvoie la hauteur du sol (le niveau de la mer est 0).
inline float height(float x, float z) {
    const float d = z - shoreLine(x);
    float h = d < 0.0f ? std::max(d * 0.05f, -12.0f) : (d < 70.0f ? d * 0.045f : 3.15f + (d - 70.0f) * 0.015f);
    h += smoothstep(40.0f, 100.0f, d) * (fbm(x * 0.025f, z * 0.025f) - 0.35f) * 4.0f;
    h += (fbm(x * 0.08f, z * 0.08f) - 0.5f) * 0.25f;
    const float mask = smoothstep(300.0f, 220.0f, std::fabs(x)) * smoothstep(340.0f, 260.0f, z);
    return lerp(-14.0f, h, mask);
}

} // namespace me::terrain
