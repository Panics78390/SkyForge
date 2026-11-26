// Rendu de la plage (terrain procédural) et des rochers, matériaux PBR et surfaces mouillées.
#include "Common.hlsli"
#include "Scene.hlsli"
#include "WetSurface.hlsli"

cbuffer SceneCB : register(b1)
{
    uint  gRockCount;
    float gPuddleScale;      // échelle du relief qui place les flaques (1/m)
    float gRippleAmount;
    float gShowWetnessDebug;
    uint  gTerrainQuads;     // résolution du maillage du terrain
    float3 _padScene;
};

StructuredBuffer<Rock> gRocks   : register(t0);
Texture2D<float4>      gWetMap  : register(t1); // humidité, flaques, neige
Texture2D<float4>      gWeather : register(t2);

static const uint ROCK_STACKS = 14;
static const uint ROCK_SLICES = 20;
static const uint NO_ROCK = 0xFFFFFFFF;

struct VSOut
{
    float4 pos   : SV_Position;
    float3 world : WORLD;
    nointerpolation uint rock : ROCK; // NO_ROCK pour le terrain
};

static const uint2 kCorners[6] = { uint2(0,0), uint2(1,0), uint2(0,1), uint2(1,0), uint2(1,1), uint2(0,1) };

VSOut VSTerrain(uint vid : SV_VertexID)
{
    uint q = vid / 6;
    uint2 g = uint2(q % gTerrainQuads, q / gTerrainQuads) + kCorners[vid % 6];
    float2 uv = float2(g) / float(gTerrainQuads);
    float2 xz = lerp(gTerrainMin.xz, gTerrainMax.xz, uv);
    VSOut o;
    o.world = float3(xz.x, TerrainHeight(xz), xz.y);
    o.pos = mul(float4(o.world, 1), gViewProj);
    o.rock = NO_ROCK;
    return o;
}

VSOut VSRock(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    Rock r = gRocks[inst];
    uint q = vid / 6;
    uint2 g = uint2(q % ROCK_SLICES, q / ROCK_SLICES) + kCorners[vid % 6];
    float theta = float(g.y) / float(ROCK_STACKS) * PI;
    float phi = float(g.x % ROCK_SLICES) / float(ROCK_SLICES) * 2.0 * PI;
    float3 dir = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
    // Forme : bruit basse fréquence (silhouette) + moyenne fréquence (arêtes).
    float3 so = float3(r.seed % 97, (r.seed / 97) % 89, r.seed % 61) * 1.37;
    float disp = 1.0 + Perlin3(dir * 1.6 + so, 0) * 0.18 + Perlin3(dir * 4.5 + so * 2.0, 0) * 0.06;
    float3 local = dir * disp * r.radii;
    local.xz = RotY(local.xz, r.yaw);

    VSOut o;
    o.world = r.center + local;
    o.pos = mul(float4(o.world, 1), gViewProj);
    o.rock = inst;
    return o;
}

// --- Sable procédural ---------------------------------------------------------
// Tout est calculé dans le shader, avec un fondu selon la taille du pixel au sol (fw, en m)
// pour éviter le scintillement : les grains et les petites rides ne sont visibles que de près.

// Profil asymétrique d'une ride de vent : pente douce au vent, pente raide sous le vent.
float RippleSlope(float u)
{
    float r = frac(u);
    return r < 0.75 ? 1.0 / 0.75 : -1.0 / 0.25;
}

Surface TerrainMaterial(float3 p, float3 N, float fw)
{
    Surface s;
    s.N = N;
    s.f0 = 0.04;
    s.roughness = 0.93;
    s.porosity = 0.6;

    // Couleur de fond : grandes taches (tri par le vent et les marées) + variations moyennes.
    float macro = TFbm(p.xz * 0.045);
    float mid = TFbm(p.xz * 0.6);
    float3 light = float3(0.80, 0.71, 0.54), dark = float3(0.66, 0.56, 0.40), grey = float3(0.62, 0.60, 0.55);
    s.albedo = lerp(dark, light, saturate(macro * 1.3 - 0.1));
    s.albedo = lerp(s.albedo, grey, smoothstep(0.6, 0.8, TFbm(p.xz * 0.02 + 7.0)) * 0.35);
    s.albedo *= lerp(0.92, 1.06, mid);

    // Grains : minéraux sombres et quartz clairs (cellules de 4 mm), moyennés au loin.
    float grainVis = 1.0 - smoothstep(0.002, 0.012, fw);
    if (grainVis > 0.0)
    {
        int2 gc = int2(floor(p.xz / 0.004));
        float gh = HashF(Hash3(uint3(uint2(gc), 5u)));
        float3 g = gh < 0.07 ? float3(0.28, 0.25, 0.22) : (gh > 0.9 ? float3(0.95, 0.92, 0.86) : s.albedo);
        s.albedo = lerp(s.albedo, g, grainVis * 0.8);
    }

    // Coquillages et petits galets épars (cellules de 40 cm).
    float2 cell = floor(p.xz / 0.4);
    float3 ch = Hash33(int3(int2(cell), 3));
    if (ch.x < 0.05)
    {
        float2 c = (cell + 0.2 + ch.yz * 0.6) * 0.4;
        float r = length((p.xz - c) * float2(1.0, 1.6));
        float shell = 1.0 - smoothstep(0.010, 0.016, r);
        float3 shellCol = ch.y > 0.5 ? float3(0.93, 0.88, 0.82) : float3(0.55, 0.53, 0.50);
        s.albedo = lerp(s.albedo, shellCol, shell * (1.0 - smoothstep(0.01, 0.05, fw)));
    }

    // Laisse de mer : bande d'algues sèches à la limite des plus hautes vagues.
    float tide = smoothstep(0.75, 0.9, p.y - gWaterLevel) * (1.0 - smoothstep(1.05, 1.3, p.y - gWaterLevel));
    float wrack = tide * smoothstep(0.55, 0.75, TFbm(p.xz * float2(0.9, 2.5)));
    s.albedo = lerp(s.albedo, float3(0.13, 0.11, 0.07), wrack * 0.85);

    // Oyats (herbes des dunes) sur les zones hautes et peu pentues.
    float grass = smoothstep(2.8, 4.0, p.y) * smoothstep(0.45, 0.6, TFbm(p.xz * 0.12)) * smoothstep(0.75, 0.9, N.y);
    s.albedo = lerp(s.albedo, lerp(float3(0.30, 0.34, 0.16), float3(0.52, 0.50, 0.28), mid), grass);
    s.roughness = lerp(s.roughness, 0.85, grass);

    // Rides : rides de vent fines (9 cm) sur le sable sec, rides de courant (40 cm) sur l'estran.
    float2 wd = normalize(gWindDir.xz + 1e-4);
    float wob = TFbm(p.xz * 0.7) * 2.5;
    float uWind = dot(p.xz, wd) / 0.09 + wob;
    float windRip = RippleSlope(uWind) * 0.035 * (1.0 - smoothstep(0.006, 0.03, fw));
    float dS = (ShoreLine(p.x + 1.0) - ShoreLine(p.x - 1.0)) * 0.5;
    float2 sn = normalize(float2(-dS, 1.0));
    float uTide = dot(p.xz, sn) / 0.4 + wob * 0.6;
    float tideRip = cos(uTide * 2.0 * PI) * 0.12 * (1.0 - smoothstep(0.03, 0.15, fw));
    float lowBeach = 1.0 - smoothstep(gWaterLevel + 0.4, gWaterLevel + 1.0, p.y);
    float2 tilt = wd * windRip * (1.0 - lowBeach) + sn * tideRip * lowBeach;
    // Grandes ondulations douces visibles de loin.
    float big = sin(dot(p.xz, float2(0.8, 2.4)) + TFbm(p.xz * 0.6) * 6.0) * 0.04;
    s.N = normalize(N + float3(tilt.x + big * 0.3, 0, tilt.y + big) * (1.0 - grass));
    return s;
}

// Paillettes : quelques grains de quartz renvoient le soleil comme de petits miroirs.
float SandSparkle(float3 p, float3 V, float fw, float wetness)
{
    float vis = (1.0 - smoothstep(0.003, 0.015, fw)) * (1.0 - wetness);
    if (vis <= 0.0) return 0.0;
    int2 gc = int2(floor(p.xz / 0.003));
    float3 h = Hash33(int3(gc, 9));
    if (h.x < 0.985) return 0.0;
    float3 n = normalize(float3(h.y * 2.0 - 1.0, 1.5, h.z * 2.0 - 1.0));
    float3 Hh = normalize(gSunDir + V);
    return pow(saturate(dot(n, Hh)), 400.0) * 40.0 * vis;
}

Surface RockMaterial(float3 p, float3 N, uint seed)
{
    Surface s;
    s.N = N;
    s.f0 = 0.04;
    float var = TFbm(p.xz * 0.9 + p.y * 0.7 + seed * 3.1);
    s.albedo = lerp(float3(0.26, 0.25, 0.24), float3(0.46, 0.43, 0.39), var);
    // Lichen sur le dessus, algues sombres près de l'eau.
    float lichen = smoothstep(0.55, 0.85, N.y) * smoothstep(0.55, 0.7, TFbm(p.xz * 1.7 + 9.0));
    s.albedo = lerp(s.albedo, float3(0.52, 0.50, 0.32), lichen * 0.6);
    float algae = 1.0 - smoothstep(gWaterLevel + 0.3, gWaterLevel + 1.4, p.y);
    s.albedo = lerp(s.albedo, float3(0.10, 0.12, 0.07), algae * 0.85);
    s.roughness = lerp(0.72, 0.55, algae);
    s.porosity = 0.3;
    return s;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 p = i.world;
    float3 V = normalize(gCamPos - p);
    bool isRock = i.rock != NO_ROCK;
    float fw = length(fwidth(p.xz)) + 1e-5; // taille d'un pixel au sol (m), calculée hors des branches
    float3 N;
    if (isRock)
    {
        // Normale par facettes (dérivées écran) : aspect taillé des roches.
        N = normalize(cross(ddy(p), ddx(p)));
        if (dot(N, V) < 0.0) N = -N;
    }
    else
    {
        N = TerrainNormal(p.xz);
    }
    Surface s;
    if (isRock) s = RockMaterial(p, N, gRocks[i.rock].seed);
    else s = TerrainMaterial(p, N, fw);

    // --- Humidité -----------------------------------------------------------
    float wetness, puddle = 0.0, snowAmt = 0.0;
    float3 puddleN = float3(0, 1, 0);
    if (!isRock)
    {
        float2 uv = (p.xz - gTerrainMin.xz) / (gTerrainMax.xz - gTerrainMin.xz);
        float4 w = gWetMap.SampleLevel(gLinearClamp, uv, 0);
        wetness = w.x;
        snowAmt = smoothstep(0.02, 0.25, w.z) * smoothstep(0.45, 0.75, N.y);
        // Relief qui place les flaques : creux des zones plates au-dessus de la ligne d'eau.
        float relief = TFbm(p.xz * gPuddleScale);
        float flat = smoothstep(0.985, 0.998, N.y) * smoothstep(gWaterLevel + 1.0, gWaterLevel + 2.0, p.y);
        puddle = saturate((w.y * gPuddleAmount - relief) * 12.0) * flat;
        puddleN = RainRipples(p.xz, gTime, gPrecip * gRippleAmount);
    }
    else
    {
        // Variable globale d'humidité : dessus exposé à 100 %, flancs selon le vent,
        // surfaces abritées (sous l'arche) presque sèches ; zone d'embruns toujours mouillée.
        bool covered = N.y > 0.3 && OcclusionHeight(gRocks, gRockCount, p.xz) > p.y + 0.4;
        float facing = N.y > 0.5 ? 1.0 : saturate(0.25 + dot(N, -gWindDir) * 0.75);
        wetness = gWetness * (covered ? 0.05 : facing);
        wetness = max(wetness, saturate((gWaterLevel + 1.5 - p.y) * 0.8));
        snowAmt = covered ? 0.0 : smoothstep(0.02, 0.25, gSnowCover) * smoothstep(0.5, 0.8, N.y)
                                  * smoothstep(gWaterLevel + 1.0, gWaterLevel + 1.8, p.y);
    }
    ApplyWetness(s, wetness, puddle * (1.0 - snowAmt), puddleN);
    // Neige : couche blanche, mate et lisse qui recouvre le matériau (et ses rides).
    s.albedo = lerp(s.albedo, float3(0.88, 0.90, 0.94), snowAmt);
    s.roughness = lerp(s.roughness, 0.6, snowAmt);
    s.porosity = lerp(s.porosity, 0.0, snowAmt);
    s.N = normalize(lerp(s.N, N, snowAmt));

    // --- Éclairage ----------------------------------------------------------
    float3 L = gSunDir;
    float3 H = normalize(V + L);
    float NoL = saturate(dot(s.N, L));
    float NoV = saturate(dot(s.N, V)) + 1e-4;
    float NoH = saturate(dot(s.N, H));
    float a = max(s.roughness * s.roughness, 2e-4);
    uint skip = isRock ? i.rock : NO_ROCK;

    float shadow = NoL > 0.0 ? ShadowRay(gRocks, gRockCount, p + N * 0.05, L, skip) : 0.0;
    shadow *= CloudShadow(gWeather, p);
    float3 sun = SunRadiance() * shadow;
    float3 F = F_Schlick(s.f0.xxx, saturate(dot(V, H)));
    float3 spec = D_GGX(NoH, a) * V_SmithJoint(NoV, NoL, a) * F;
    float3 diffuse = s.albedo / PI * (1.0 - F);
    float3 col = (diffuse + spec) * sun * NoL;
    // Paillettes : grains de quartz du sable sec, cristaux de neige.
    float sparkleBlock = isRock ? 1.0 - snowAmt : saturate(wetness + puddle) * (1.0 - snowAmt);
    col += sun * NoL * SandSparkle(p, V, fw, sparkleBlock);

    // Ambiant + réflexion du ciel (sert de sonde de réflexion).
    col += s.albedo * AmbientLight(s.N) * (isRock ? 0.8 : 1.0);
    float3 R = reflect(-V, s.N);
    float3 Rup = normalize(float3(R.x, max(R.y, 0.02), R.z));
    float3 Fenv = s.f0 + (max(1.0 - s.roughness, s.f0) - s.f0) * pow(1.0 - NoV, 5.0);
    float envVis = (1.0 - s.roughness) > 0.3 ? ShadowRay(gRocks, gRockCount, p + N * 0.05, Rup, skip) : 1.0;
    col += CloudySky(gWeather, Rup) * Fenv * (1.0 - s.roughness) * lerp(0.3, 1.0, envVis);

    if (gShowWetnessDebug > 0.5)
        col = lerp(col, float3(wetness, puddle, 0.0) * 2.0, 0.7);

    float dist = length(gCamPos - p);
    col = lerp(col, SkyColor(normalize(float3(-V.x, 0.02, -V.z))), FogFactor(dist));
    return float4(col, 1);
}
