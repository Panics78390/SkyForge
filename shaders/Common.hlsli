// Constantes et fonctions partagées par tous les shaders.
// FrameCB doit rester identique à me::FrameConstants (src/Weather.h).
#ifndef COMMON_HLSLI
#define COMMON_HLSLI
#pragma pack_matrix(row_major)

cbuffer FrameCB : register(b0)
{
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gPrevViewProj;
    float3 gCamPos;        float gTime;
    float3 gSunDir;        float gSunIntensity;
    float3 gWindDir;       float gWindSpeed;
    float  gCloudCoverage; float gCloudDensity; float gCloudType; float gPrecip;
    float  gWetness;       float gTemperature;  float gDeltaTime; uint  gFrameIndex;
    float2 gResolution;    float2 gInvResolution;
    float3 gWindOrigin;    float gWindCellSize;
    float2 gCloudOffset;   float gGust;         float gExposure;
    float  gCloudBottom;   float gCloudTop;     float gFogDensity; float gOceanTime;
    float3 gCamForward;    float gPuddleAmount;
    float  gLodNear;       float gLodFar;       float gGerstnerAmp; float gGerstnerWavelength;
    float3 gTerrainMin;     float gWaterLevel;
    float3 gTerrainMax;     float gDebugView;
    uint3  gWindRes;       float gRainBoxSize;
};

SamplerState gLinearWrap  : register(s0);
SamplerState gLinearClamp : register(s1);
SamplerState gPointClamp  : register(s2);
SamplerState gPointWrap   : register(s3);

static const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// Hachage et bruit
uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}
uint Hash3(uint3 v) { return Hash(v.x ^ Hash(v.y ^ Hash(v.z))); }
float HashF(uint x) { return float(Hash(x) & 0x00FFFFFF) / 16777216.0; }
float3 Hash33(int3 p)
{
    uint h = Hash3(uint3(p));
    return float3(HashF(h), HashF(h ^ 0x9E3779B9u), HashF(h ^ 0x85EBCA6Bu));
}

// Bruit de gradient 3D (Perlin) avec période optionnelle (period = 0 : non périodique).
float3 Fade(float3 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float GradDot(int3 cell, int period, float3 f)
{
    int pp = max(period, 1); // évite un modulo par zéro lors du repliement de constantes
    int3 c = period > 0 ? ((cell % pp) + pp) % pp : cell;
    float3 g = normalize(Hash33(c) * 2.0 - 1.0 + 1e-4);
    return dot(g, f);
}
float Perlin3(float3 p, int period)
{
    int3 i = int3(floor(p));
    float3 f = frac(p);
    float3 u = Fade(f);
    float n000 = GradDot(i + int3(0,0,0), period, f - float3(0,0,0));
    float n100 = GradDot(i + int3(1,0,0), period, f - float3(1,0,0));
    float n010 = GradDot(i + int3(0,1,0), period, f - float3(0,1,0));
    float n110 = GradDot(i + int3(1,1,0), period, f - float3(1,1,0));
    float n001 = GradDot(i + int3(0,0,1), period, f - float3(0,0,1));
    float n101 = GradDot(i + int3(1,0,1), period, f - float3(1,0,1));
    float n011 = GradDot(i + int3(0,1,1), period, f - float3(0,1,1));
    float n111 = GradDot(i + int3(1,1,1), period, f - float3(1,1,1));
    float x00 = lerp(n000, n100, u.x), x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x), x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z); // ~[-1,1]
}

// Worley (distance au point le plus proche) périodique, renvoie 1 - d (cellules « gonflées »).
float Worley3(float3 p, int period)
{
    int3 i = int3(floor(p));
    float3 f = frac(p);
    float d = 1e9;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int3 c = i + int3(x, y, z);
        int pp = max(period, 1);
        int3 w = period > 0 ? ((c % pp) + pp) % pp : c;
        float3 o = float3(x, y, z) + Hash33(w) - f;
        d = min(d, dot(o, o));
    }
    return 1.0 - saturate(sqrt(d));
}

float Remap(float v, float l0, float h0, float l1, float h1)
{
    return l1 + (v - l0) * (h1 - l1) / max(h0 - l0, 1e-5);
}

// ---------------------------------------------------------------------------
// Ciel analytique (gradient + halo solaire), assombri par la couverture nuageuse.
float SunVisibility() { return saturate(gSunDir.y * 4.0 + 0.15); }

float3 SunRadiance()
{
    float h = saturate(gSunDir.y);
    float3 warm = lerp(float3(1.0, 0.45, 0.2), float3(1.0, 0.96, 0.9), saturate(h * 3.0));
    return warm * 3.2 * gSunIntensity * SunVisibility();
}

float3 SkyColor(float3 dir)
{
    float up = saturate(dir.y);
    float sunH = saturate(gSunDir.y * 2.0 + 0.15);
    float3 zenith  = float3(0.10, 0.28, 0.75);
    float3 horizon = float3(0.55, 0.70, 0.90);
    float3 col = lerp(horizon, zenith, pow(up, 0.45)) * (0.15 + 0.85 * sunH);
    // Ciel couvert : désaturé et plus sombre.
    float overcast = saturate(gCloudCoverage * 1.25 - 0.25);
    float lum = dot(col, float3(0.3, 0.59, 0.11));
    col = lerp(col, lum * float3(0.85, 0.88, 0.92) * (1.0 - 0.45 * gPrecip), overcast * 0.75);
    float mu = saturate(dot(dir, gSunDir));
    col += float3(1.0, 0.75, 0.5) * pow(mu, 12.0) * 0.35 * sunH * (1.0 - overcast * 0.7);
    // Sous l'horizon : on fond vers une teinte sombre (sera recouvert par l'océan).
    col = lerp(col, float3(0.08, 0.10, 0.12) * sunH, saturate(-dir.y * 3.0));
    return col * gSunIntensity * 1.6;
}

float3 SunDisk(float3 dir)
{
    float mu = dot(dir, gSunDir);
    return SunRadiance() * 40.0 * smoothstep(0.99975, 0.99990, mu);
}

// Lumière ambiante hémisphérique très simple.
float3 AmbientLight(float3 n)
{
    float3 sky = SkyColor(normalize(float3(n.x * 0.3, max(n.y, 0.05) + 0.4, n.z * 0.3)));
    float3 ground = float3(0.06, 0.07, 0.08) * gSunIntensity;
    return lerp(ground, sky, saturate(n.y * 0.5 + 0.5)) * 0.9;
}

// Brouillard (densifié par la pluie).
float FogFactor(float dist)
{
    float density = gFogDensity + gPrecip * 0.0015;
    return 1.0 - exp(-dist * density);
}

// ---------------------------------------------------------------------------
// Vent : volume 3D centré caméra (voir wind.hlsl).
float4 SampleWind(Texture3D<float4> windTex, float3 worldPos)
{
    float3 uvw = (worldPos - gWindOrigin) / (gWindCellSize * float3(gWindRes));
    float4 w = windTex.SampleLevel(gLinearClamp, uvw, 0);
    // En dehors du volume : vent de base seulement.
    float inside = all(uvw > 0.0) && all(uvw < 1.0) ? 1.0 : 0.0;
    return lerp(float4(gWindDir * gWindSpeed, 0), w, inside);
}

// ---------------------------------------------------------------------------
// Reconstruction position monde depuis UV écran et profondeur (Z inversé).
float3 WorldFromDepth(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 w = mul(clip, gInvViewProj);
    return w.xyz / w.w;
}
float3 ViewRay(float2 uv)
{
    return normalize(WorldFromDepth(uv, 0.5) - gCamPos);
}

// Ombre des nuages au sol : on lit la carte météo à l'intersection du rayon solaire
// avec le milieu de la couche nuageuse (approximation peu coûteuse).
float CloudShadow(Texture2D<float4> weatherMap, float3 p)
{
    float midH = lerp(gCloudBottom, gCloudTop, 0.35);
    float t = (midH - p.y) / max(gSunDir.y, 0.05);
    float3 q = p + gSunDir * t;
    float cov = weatherMap.SampleLevel(gLinearWrap, (q.xz + gCloudOffset) / 30000.0, 0).r;
    float c = saturate((cov + gCloudCoverage - 0.75) * 2.5) * gCloudDensity;
    return lerp(1.0, 0.25, saturate(c));
}

// GGX (spéculaire) pour l'éclairage solaire.
float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float V_SmithJoint(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
float3 F_Schlick(float3 f0, float VoH)
{
    return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
}

#endif
