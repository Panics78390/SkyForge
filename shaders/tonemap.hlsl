// Exposition + ACES (approximation de Narkowicz) + gamma sRGB.
#include "Common.hlsli"

Texture2D<float4> gHdr : register(t0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 ACES(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 c = gHdr.Load(int3(int2(i.pos.xy), 0)).rgb * gExposure;
    c = ACES(c);
    c = pow(c, 1.0 / 2.2);
    // Léger tramage pour éviter le banding du ciel.
    c += (HashF(Hash3(uint3(i.pos.xy, gFrameIndex))) - 0.5) / 255.0;
    return float4(c, 1);
}
