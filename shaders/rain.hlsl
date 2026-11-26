// Pluie en particules GPU.
//  * Occlusion : carte de hauteur vue du ciel autour de la caméra (ici calculée depuis les
//    terrain et les rochers ; dans un moteur complet, c'est une passe de profondeur orthographique).
//  * Simulation : chaque goutte tend vers (vent local + vitesse terminale) avec une constante
//    de temps courte (traînée), boîte toroïdale autour de la caméra pour garder la densité
//    constante, mort et réapparition en haut quand elle touche une surface.
//  * Neige : une part des particules (SnowFactor, selon la température) devient des flocons :
//    chute lente (~1 m/s), forte traînée (ils suivent le vent) et flottement latéral.
#include "Common.hlsli"
#include "Scene.hlsli"

cbuffer RainCB : register(b1)
{
    uint   gMaxParticles;
    uint   gActive;
    float  gBoxHalf;       // demi-largeur de la boîte de pluie (m)
    float  gBoxHeight;
    float2 gOccOrigin;     // coin de la carte d'occlusion (m)
    float  gOccTexel;      // taille d'un texel (m)
    uint   gOccSize;
    uint   gRockCount;
    float  gTerminalSpeed; // vitesse de chute (m/s), ~9 m/s pour une goutte de 2 mm
    float  gStreak;
    float  gBrightness;
};

struct Drop { float3 pos; float seed; float3 vel; float alive; };

bool IsSnowflake(float seed) { return frac(seed * 7.31 + 0.13) < SnowFactor(); }

StructuredBuffer<Rock>      gRocks : register(t0);
Texture3D<float4>           gWind  : register(t1);
Texture2D<float>            gOcc   : register(t2);
RWTexture2D<float>          outOcc : register(u0);
RWStructuredBuffer<Drop>    gDrops : register(u1);

[numthreads(8, 8, 1)]
void Occlusion(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gOccSize)) return;
    float2 xz = gOccOrigin + (float2(id.xy) + 0.5) * gOccTexel;
    outOcc[id.xy] = OcclusionHeight(gRocks, gRockCount, xz);
}

float OccAt(float2 xz)
{
    float2 uv = (xz - gOccOrigin) / (gOccTexel * gOccSize);
    if (any(uv < 0.0) || any(uv > 1.0)) return gWaterLevel;
    return gOcc.SampleLevel(gPointClamp, uv, 0);
}

float3 Respawn(uint i, float seedOffset, bool anywhere)
{
    uint h = Hash(i * 9781u + uint(seedOffset * 7919.0) + gFrameIndex * 31u);
    float3 r = float3(HashF(h), HashF(h ^ 0x68E31DA4u), HashF(h ^ 0xB5297A4Du));
    float y = anywhere ? lerp(-gBoxHeight * 0.3, gBoxHeight, r.y) : gBoxHeight * (0.7 + 0.3 * r.y);
    return float3(gCamPos.x + (r.x * 2.0 - 1.0) * gBoxHalf, gCamPos.y + y, gCamPos.z + (r.z * 2.0 - 1.0) * gBoxHalf);
}

[numthreads(256, 1, 1)]
void Simulate(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gMaxParticles) return;
    Drop d = gDrops[i];
    if (i >= gActive) { d.alive = 0; gDrops[i] = d; return; }
    if (d.alive < 0.5)
    {
        d.pos = Respawn(i, d.seed, true);
        d.vel = float3(0, -gTerminalSpeed, 0);
        d.alive = 1;
    }

    float3 wind = SampleWind(gWind, d.pos).xyz;
    bool snow = IsSnowflake(d.seed);
    float fall = snow ? 1.1 : gTerminalSpeed;
    float3 target = wind + float3(0, -fall * (0.85 + 0.3 * frac(d.seed * 13.1)), 0);
    if (snow)
    {
        // Flottement : petite oscillation propre à chaque flocon.
        float ph = gTime * (1.5 + frac(d.seed * 3.7)) + d.seed * 40.0;
        target.xz += float2(sin(ph), cos(ph * 1.3)) * 0.6;
    }
    d.vel = lerp(d.vel, target, 1.0 - exp(-gDeltaTime * (snow ? 2.5 : 4.0)));
    d.pos += d.vel * gDeltaTime;

    // Boîte toroïdale horizontale autour de la caméra.
    float2 rel = d.pos.xz - gCamPos.xz;
    rel = (frac(rel / (2.0 * gBoxHalf) + 0.5) - 0.5) * 2.0 * gBoxHalf;
    d.pos.xz = gCamPos.xz + rel;

    // Collision : toit, sol, mer, ou sortie par le bas de la boîte.
    if (d.pos.y < OccAt(d.pos.xz) || d.pos.y < gCamPos.y - gBoxHeight * 0.5)
    {
        d.seed = frac(d.seed + 0.618034);
        d.pos = Respawn(i, d.seed, false);
        d.vel = float3(wind.x, -(IsSnowflake(d.seed) ? 1.1 : gTerminalSpeed), wind.z);
    }
    gDrops[i] = d;
}
