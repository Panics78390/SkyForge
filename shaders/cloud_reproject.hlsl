// Reprojection temporelle des nuages.
// On reconstruit la position monde du « centre de masse » du nuage vu par chaque pixel,
// on la projette avec la matrice de la frame précédente, puis on mélange avec l'historique
// borné (clamp) par le voisinage 3x3 courant pour éviter les traînées (ghosting).
#include "CloudCommon.hlsli"

Texture2D<float4>   gCurrent : register(t0);
Texture2D<float>    gCurDist : register(t1);
Texture2D<float4>   gHistory : register(t2);
RWTexture2D<float4> outHist  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gCloudRes)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gCloudRes);
    float4 cur = gCurrent[id.xy];

    float4 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 q = clamp(int2(id.xy) + int2(x, y), 0, int2(gCloudRes) - 1);
        float4 s = gCurrent[q];
        mn = min(mn, s);
        mx = max(mx, s);
    }

    float3 world = gCamPos + ViewRay(uv) * gCurDist[id.xy];
    float4 prev = mul(float4(world, 1.0), gPrevViewProj);
    float2 puv = prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
    bool valid = gHistoryValid != 0 && prev.w > 0.0 && all(puv > 0.0) && all(puv < 1.0);

    float4 hist = gHistory.SampleLevel(gLinearClamp, puv, 0);
    hist = clamp(hist, mn, mx);
    outHist[id.xy] = lerp(cur, hist, valid ? gHistoryBlend : 0.0);
}
