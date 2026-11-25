// IFFT 2D sur GPU : radix-2, une ligne (ou colonne) entière par groupe, en mémoire partagée.
// Deux passes : gVertical = 0 (lignes) puis gVertical = 1 (colonnes).
// Chaque texel RGBA32F contient deux nombres complexes : on transforme 4 complexes par
// thread (textures A et B) => 8 champs réels en une seule paire de dispatches.
//
// Algorithme : entrée en ordre bit-inversé, puis log2(N) étages de papillons
// (Cooley-Tukey, décimation temporelle). Signe + dans l'exponentielle = transformée inverse.
#include "OceanCommon.hlsli"

#ifndef FFT_N
#define FFT_N 256
#endif
#ifndef FFT_LOG_N
#define FFT_LOG_N 8
#endif

RWTexture2D<float4> texA : register(u0);
RWTexture2D<float4> texB : register(u1);

groupshared float4 sA[2][FFT_N];
groupshared float4 sB[2][FFT_N];

uint BitReverse(uint x) { return reversebits(x) >> (32 - FFT_LOG_N); }

float4 Twiddle2(float4 v, float2 w) { return float4(CMul(v.xy, w), CMul(v.zw, w)); }

[numthreads(FFT_N, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint row = gid.x;
    const uint i = tid.x;
    const uint2 src = gVertical ? uint2(row, BitReverse(i)) : uint2(BitReverse(i), row);
    sA[0][i] = texA[src];
    sB[0][i] = texB[src];
    GroupMemoryBarrierWithGroupSync();

    uint ping = 0;
    [loop]
    for (uint m = 2; m <= FFT_N; m <<= 1)
    {
        const uint h = m >> 1;
        const uint k = i & (m - 1);       // position dans le bloc
        const uint j = i - k;             // début du bloc
        const uint kk = k & (h - 1);
        const float ang = 2.0 * PI * float(kk) / float(m);
        const float2 w = float2(cos(ang), sin(ang));
        const float s = k < h ? 1.0 : -1.0;

        float4 ea = sA[ping][j + kk], oa = sA[ping][j + kk + h];
        float4 eb = sB[ping][j + kk], ob = sB[ping][j + kk + h];
        sA[1 - ping][i] = ea + s * Twiddle2(oa, w);
        sB[1 - ping][i] = eb + s * Twiddle2(ob, w);
        ping = 1 - ping;
        GroupMemoryBarrierWithGroupSync();
    }

    const uint2 dst = gVertical ? uint2(row, i) : uint2(i, row);
    texA[dst] = sA[ping][i];
    texB[dst] = sB[ping][i];
}
