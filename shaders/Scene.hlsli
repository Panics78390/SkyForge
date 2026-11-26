// Scène de démonstration : une plage procédurale (sable, dunes, fond marin) et des rochers.
// Le terrain est une fonction analytique TerrainHeight(xz), dupliquée à l'identique en C++
// (src/Terrain.h) pour placer les rochers et la caméra. Les rochers sont des ellipsoïdes
// déformés par du bruit ; les mêmes ellipsoïdes servent aux ombres (lancer de rayon
// analytique) et à l'occlusion de la pluie (« carte de hauteur vue du ciel »).
#ifndef SCENE_HLSLI
#define SCENE_HLSLI
#include "Common.hlsli"

struct Rock
{
    float3 center; float yaw;
    float3 radii;  uint  seed;
};

// --- Terrain -------------------------------------------------------------------
float TNoise(float2 p)
{
    int2 i = int2(floor(p));
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = HashF(Hash3(uint3(uint2(i), 11u)));
    float b = HashF(Hash3(uint3(uint2(i + int2(1, 0)), 11u)));
    float c = HashF(Hash3(uint3(uint2(i + int2(0, 1)), 11u)));
    float d = HashF(Hash3(uint3(uint2(i + int2(1, 1)), 11u)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float TFbm(float2 p)
{
    return TNoise(p) * 0.5 + TNoise(p * 2.07 + 17.3) * 0.3 + TNoise(p * 4.13 + 41.7) * 0.2;
}

// Ligne de rivage (z en fonction de x) : sinuosités + bruit.
float ShoreLine(float x)
{
    return 12.0 * sin(x * 0.018) + 7.0 * sin(x * 0.047 + 1.3) + (TFbm(float2(x * 0.01, 3.7)) - 0.5) * 24.0;
}

float TerrainHeight(float2 p)
{
    float d = p.y - ShoreLine(p.x);                 // > 0 : vers l'intérieur des terres
    float h = d < 0.0 ? max(d * 0.05, -12.0)        // fond marin en pente douce
            : (d < 70.0 ? d * 0.045                 // plage (~2,5°)
            : 3.15 + (d - 70.0) * 0.015);           // arrière-plage
    h += smoothstep(40.0, 100.0, d) * (TFbm(p * 0.025) - 0.35) * 4.0; // dunes
    h += (TFbm(p * 0.08) - 0.5) * 0.25;                                // micro-relief
    float mask = smoothstep(300.0, 220.0, abs(p.x)) * smoothstep(340.0, 260.0, p.y);
    return lerp(-14.0, h, mask);
}

float3 TerrainNormal(float2 p)
{
    const float e = 0.35;
    float hx = TerrainHeight(p + float2(e, 0)) - TerrainHeight(p - float2(e, 0));
    float hz = TerrainHeight(p + float2(0, e)) - TerrainHeight(p - float2(0, e));
    return normalize(float3(-hx, 2.0 * e, -hz));
}

// --- Rochers -------------------------------------------------------------------
float2 RotY(float2 v, float a) { float c = cos(a), s = sin(a); return float2(c * v.x - s * v.y, s * v.x + c * v.y); }

float3 ToRockSpace(Rock r, float3 p)
{
    float3 q = p - r.center;
    q.xz = RotY(q.xz, -r.yaw);
    return q / r.radii;
}

// Sommet de l'ellipsoïde au-dessus de xz (ou -1e9 s'il n'y est pas).
float RockTop(Rock r, float2 xz)
{
    float2 q = RotY(xz - r.center.xz, -r.yaw) / r.radii.xz;
    float q2 = dot(q, q);
    return q2 < 1.0 ? r.center.y + r.radii.y * sqrt(1.0 - q2) : -1e9;
}

// Hauteur de la première surface rencontrée par une goutte tombant à la verticale.
float OcclusionHeight(StructuredBuffer<Rock> rocks, uint count, float2 xz)
{
    float h = max(TerrainHeight(xz), gWaterLevel);
    for (uint i = 0; i < count; ++i) h = max(h, RockTop(rocks[i], xz));
    return h;
}

// 1 = éclairé, 0 = à l'ombre (rochers analytiques + marche courte sur le relief).
// skip : index du rocher en cours d'ombrage (évite l'auto-ombrage dû au bruit de forme).
float ShadowRay(StructuredBuffer<Rock> rocks, uint count, float3 p, float3 d, uint skip)
{
    for (uint i = 0; i < count; ++i)
    {
        if (i == skip) continue;
        Rock r = rocks[i];
        float3 o = ToRockSpace(r, p);
        float3 dd = d;
        dd.xz = RotY(dd.xz, -r.yaw);
        dd /= r.radii;
        float a = dot(dd, dd), b = dot(o, dd), c = dot(o, o) - 1.0;
        float disc = b * b - a * c;
        if (disc > 0.0 && c > 0.0 && b < 0.0) return 0.0; // devant nous et touché
    }
    float t = 0.8;
    [loop] for (int s = 0; s < 12; ++s)
    {
        float3 q = p + d * t;
        if (q.y < TerrainHeight(q.xz)) return 0.0;
        t *= 1.45;
    }
    return 1.0;
}

#endif
