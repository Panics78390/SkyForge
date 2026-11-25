// Rendu de la scène (île + bâtiments) avec matériaux PBR et surfaces mouillées.
#include "Common.hlsli"
#include "Scene.hlsli"
#include "WetSurface.hlsli"

cbuffer SceneCB : register(b1)
{
    uint  gBoxCount;
    float gPuddleScale;   // échelle du relief qui place les flaques (1/m)
    float gRippleAmount;
    float gShowWetnessDebug;
};

StructuredBuffer<Box> gBoxes   : register(t0);
Texture2D<float2>     gWetMap  : register(t1);
Texture2D<float4>     gWeather : register(t2);

struct VSOut
{
    float4 pos   : SV_Position;
    float3 world : WORLD;
    float3 n     : NORMAL;
    nointerpolation uint mat : MATERIAL;
};

VSOut VSMain(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    static const float3 normals[6] = { float3(1,0,0), float3(-1,0,0), float3(0,1,0), float3(0,-1,0), float3(0,0,1), float3(0,0,-1) };
    static const float3 us[6]      = { float3(0,1,0), float3(0,0,1), float3(0,0,1), float3(1,0,0), float3(1,0,0), float3(0,1,0) };
    static const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,-1), float2(1,1), float2(-1,1) };
    Box b = gBoxes[inst];
    uint face = vid / 6;
    float2 k = corners[vid % 6];
    float3 n = normals[face];
    float3 u = us[face];
    float3 v = cross(n, u);
    float3 c = (b.mn + b.mx) * 0.5;
    float3 h = (b.mx - b.mn) * 0.5;
    float3 p = c + (n + u * k.x + v * k.y) * h;

    VSOut o;
    o.world = p;
    o.n = n;
    o.mat = b.material;
    o.pos = mul(float4(p, 1), gViewProj);
    return o;
}

float ValueNoise(float2 p)
{
    int2 i = int2(floor(p));
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = HashF(Hash3(uint3(i, 7)));
    float b = HashF(Hash3(uint3(i + int2(1, 0), 7)));
    float c = HashF(Hash3(uint3(i + int2(0, 1), 7)));
    float d = HashF(Hash3(uint3(i + int2(1, 1), 7)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float Fbm2(float2 p)
{
    return ValueNoise(p) * 0.55 + ValueNoise(p * 2.03) * 0.27 + ValueNoise(p * 4.1) * 0.18;
}

Surface BaseMaterial(uint mat, float3 p, float3 n)
{
    Surface s;
    s.N = n;
    s.f0 = 0.04;
    float2 uv = abs(n.y) > 0.5 ? p.xz : (abs(n.x) > 0.5 ? p.zy : p.xy);
    float var = Fbm2(uv * 0.8);
    if (mat == 0)      // pavés
    {
        float2 tile = frac(uv * 0.7);
        float joint = smoothstep(0.0, 0.04, min(min(tile.x, 1.0 - tile.x), min(tile.y, 1.0 - tile.y)));
        s.albedo = lerp(float3(0.22, 0.21, 0.2), float3(0.42, 0.39, 0.35), var) * lerp(0.55, 1.0, joint);
        s.roughness = 0.8; s.porosity = lerp(0.95, 0.6, joint);
    }
    else if (mat == 1) // béton
    {
        s.albedo = lerp(float3(0.42, 0.42, 0.40), float3(0.58, 0.57, 0.55), var);
        s.roughness = 0.9; s.porosity = 0.85;
    }
    else if (mat == 2) // tôle (toit de l'abri)
    {
        float rib = 0.85 + 0.15 * step(0.5, frac(uv.x * 2.0));
        s.albedo = float3(0.25, 0.32, 0.38) * rib;
        s.roughness = 0.45; s.porosity = 0.0;
    }
    else if (mat == 3) // bois (ponton, caisses)
    {
        float grain = ValueNoise(float2(uv.x * 0.6, uv.y * 12.0));
        s.albedo = lerp(float3(0.25, 0.15, 0.08), float3(0.42, 0.28, 0.16), grain);
        s.roughness = 0.7; s.porosity = 0.65;
    }
    else               // briques
    {
        float2 b = uv * float2(4.0, 8.0);
        b.x += step(1.0, fmod(floor(b.y), 2.0)) * 0.5;
        float2 f = frac(b);
        float mortar = smoothstep(0.0, 0.06, min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y)));
        s.albedo = lerp(float3(0.55, 0.53, 0.5), lerp(float3(0.38, 0.12, 0.08), float3(0.5, 0.2, 0.12), var), mortar);
        s.roughness = 0.85; s.porosity = lerp(0.95, 0.75, mortar);
    }
    return s;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 p = i.world;
    float3 N = normalize(i.n);
    Surface s = BaseMaterial(i.mat, p, N);

    // --- Humidité -----------------------------------------------------------
    bool islandTop = i.mat == 0 && N.y > 0.5 && p.y >= gIslandMax.y - 0.01;
    float wetness, puddle = 0.0;
    float3 puddleN = float3(0, 1, 0);
    if (islandTop)
    {
        float2 uv = (p.xz - gIslandMin.xz) / (gIslandMax.xz - gIslandMin.xz);
        float2 w = gWetMap.SampleLevel(gLinearClamp, uv, 0);
        wetness = w.x;
        // Relief qui place les flaques : les creux (valeurs basses) se remplissent en premier.
        float relief = Fbm2(p.xz * gPuddleScale);
        float level = w.y * gPuddleAmount;
        puddle = saturate((level - relief) * 12.0);
        puddleN = RainRipples(p.xz, gTime, gPrecip * gRippleAmount);
    }
    else
    {
        // Autres surfaces : variable globale d'humidité. Toits exposés à 100 %,
        // murs selon leur orientation au vent, surfaces abritées à peine humides.
        bool covered = OcclusionHeight(gBoxes, gBoxCount, p.xz, gWaterLevel) > p.y + 0.05;
        float facing = N.y > 0.5 ? 1.0 : saturate(0.25 + dot(N, -gWindDir) * 0.75);
        wetness = gWetness * (covered && N.y > 0.5 ? 0.05 : facing);
        // Ruissellement : les bas de murs restent mouillés plus longtemps.
        if (abs(N.y) < 0.5) wetness = saturate(wetness * (1.0 + 0.5 * saturate(1.0 - (p.y - gIslandMax.y) / 3.0)));
    }
    ApplyWetness(s, wetness, puddle, puddleN);

    // --- Éclairage ----------------------------------------------------------
    float3 V = normalize(gCamPos - p);
    float3 L = gSunDir;
    float3 H = normalize(V + L);
    float NoL = saturate(dot(s.N, L));
    float NoV = saturate(dot(s.N, V)) + 1e-4;
    float NoH = saturate(dot(s.N, H));
    float a = max(s.roughness * s.roughness, 2e-4);

    float shadow = ShadowRay(gBoxes, gBoxCount, p + N * 0.02, L) * CloudShadow(gWeather, p);
    float3 sun = SunRadiance() * shadow;
    float3 F = F_Schlick(s.f0.xxx, saturate(dot(V, H)));
    float3 spec = D_GGX(NoH, a) * V_SmithJoint(NoV, NoL, a) * F;
    float3 diffuse = s.albedo / PI * (1.0 - F);
    float3 col = (diffuse + spec) * sun * NoL;

    // Ambiant + réflexion de l'environnement (le ciel tient lieu de sonde de réflexion).
    float ao = islandTop ? 1.0 : saturate(0.5 + (p.y - gIslandMax.y) * 0.2);
    col += s.albedo * AmbientLight(s.N) * ao;
    float3 R = reflect(-V, s.N);
    float3 Fenv = s.f0 + (max(1.0 - s.roughness, s.f0) - s.f0) * pow(1.0 - NoV, 5.0);
    float envVis = ShadowRay(gBoxes, gBoxCount, p + N * 0.02, normalize(float3(R.x, max(R.y, 0.05), R.z)));
    col += SkyColor(normalize(float3(R.x, max(R.y, 0.02), R.z))) * Fenv * (1.0 - s.roughness) * lerp(0.3, 1.0, envVis);

    if (gShowWetnessDebug > 0.5)
        col = lerp(col, float3(wetness, puddle, 0.0) * 2.0, 0.7);

    float dist = length(gCamPos - p);
    col = lerp(col, SkyColor(normalize(float3(-V.x, 0.02, -V.z))), FogFactor(dist));
    return float4(col, 1);
}
