// Composition plein écran : ciel + soleil là où il n'y a pas de géométrie, puis nuages
// (suréchantillonnés depuis la basse résolution) : couleur = scène * T + diffusion.
#include "Common.hlsli"

Texture2D<float4> gScene  : register(t0);
Texture2D<float>  gDepth  : register(t1);
Texture2D<float4> gClouds : register(t2);

cbuffer CompositeCB : register(b1) { float gCloudsEnabled; float3 _padComposite; };

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    int2 px = int2(i.pos.xy);
    float depth = gDepth.Load(int3(px, 0));
    float3 V = ViewRay(i.uv);
    float4 c = gCloudsEnabled > 0.5 ? gClouds.SampleLevel(gLinearClamp, i.uv, 0) : float4(0, 0, 0, 1);
    float3 col;
    if (depth <= 0.0)
        col = SkyColor(V) + SunDisk(V) * c.a;
    else
        col = gScene.Load(int3(px, 0)).rgb;
    col = col * c.a + c.rgb;

    // Voile de pluie au loin.
    float3 veil = SkyColor(normalize(float3(V.x, 0.05, V.z))) * lerp(0.8, 1.1, SnowFactor()); // neige : voile plus clair
    col = lerp(col, veil, saturate(gPrecip * lerp(0.35, 0.55, SnowFactor()) * (1.0 - saturate(V.y * 3.0))));
    return float4(col, 1);
}
