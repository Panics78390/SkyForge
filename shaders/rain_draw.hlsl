// Affichage des gouttes : traits étirés selon la vitesse (flou de mouvement simulé),
// orientés face caméra autour de leur axe, testés contre la profondeur de la scène.
#include "Common.hlsli"

cbuffer RainCB : register(b1)
{
    uint   gMaxParticles;
    uint   gActive;
    float  gBoxHalf;
    float  gBoxHeight;
    float2 gOccOrigin;
    float  gOccTexel;
    uint   gOccSize;
    uint   gRockCount;
    float  gTerminalSpeed;
    float  gStreak;        // durée d'exposition simulée (s)
    float  gBrightness;
};

struct Drop { float3 pos; float seed; float3 vel; float alive; };
StructuredBuffer<Drop> gDrops : register(t0);

struct VSOut { float4 pos : SV_Position; float alpha : ALPHA; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    uint i = vid / 6;
    uint c = vid % 6;
    Drop d = gDrops[i];
    static const float2 corners[6] = { float2(-1,0), float2(1,0), float2(-1,1), float2(1,0), float2(1,1), float2(-1,1) };
    float2 k = corners[c];

    float3 axis = d.vel * gStreak;                 // longueur = vitesse x temps d'exposition
    float3 toCam = normalize(gCamPos - d.pos);
    float3 side = normalize(cross(normalize(axis + 1e-5), toCam)) * 0.006;
    float3 p = d.pos + side * k.x - axis * k.y;
    o.pos = mul(float4(p, 1), gViewProj);
    float dist = length(gCamPos - d.pos);
    float fade = saturate(1.0 - dist / gBoxHalf) * saturate(dist / 0.5);
    o.alpha = (i < gActive && d.alive > 0.5) ? fade * gBrightness : 0.0;
    if (o.alpha <= 0.0) o.pos = float4(0, 0, -1, 1); // sommet dégénéré, éliminé
    o.uv = k;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float edge = 1.0 - abs(i.uv.x);
    float3 light = AmbientLight(float3(0, 1, 0)) * 1.2 + SunRadiance() * 0.15;
    return float4(light, i.alpha * edge * 0.35);
}
