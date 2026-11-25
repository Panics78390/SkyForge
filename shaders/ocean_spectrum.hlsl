// Spectre d'océan JONSWAP (Hasselmann 1973) + évolution temporelle (Tessendorf 2001).
//
// Maths en bref :
//  * Relation de dispersion en eau profonde : w(k) = sqrt(g |k|).
//  * JONSWAP : S(w) = a g^2 / w^5 * exp(-5/4 (wp/w)^4) * gamma^r,
//      r = exp(-(w-wp)^2 / (2 s^2 wp^2)), a = 0.076 (U^2/(F g))^0.22, wp = 22 (g^2/(U F))^(1/3).
//  * Étalement directionnel D(theta) = N(s) |cos(theta/2)|^(2s).
//  * Passage en nombre d'onde : S(k) = S(w) D(theta) dw/dk / k, puis amplitude
//      h0(k) = xi / sqrt(2) * sqrt(2 S(k) dk^2), xi ~ gaussienne complexe.
//  * À l'instant t : h(k,t) = h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}
//      (symétrie hermitienne => la transformée inverse est réelle).
#include "OceanCommon.hlsli"

Texture2D<float4>   h0Tex : register(t0);
RWTexture2D<float4> outA  : register(u0);   // InitSpectrum : (h0(k), conj(h0(-k))) ; TimeSpectrum : (Dx + i Dy, Dz + i dDy/dx)
RWTexture2D<float4> outB  : register(u1);   // (dDy/dz + i dDx/dx, dDz/dz + i dDx/dz)

float JONSWAP(float w)
{
    float U = max(gU10, 0.5);
    float F = max(gFetch, 1000.0);
    float alpha = 0.076 * pow(U * U / (F * G), 0.22);
    float wp = 22.0 * pow(G * G / (U * F), 1.0 / 3.0);
    float gamma = 3.3;
    float sigma = w <= wp ? 0.07 : 0.09;
    float r = exp(-(w - wp) * (w - wp) / (2.0 * sigma * sigma * wp * wp));
    return alpha * G * G / pow(w, 5.0) * exp(-1.25 * pow(wp / w, 4.0)) * pow(gamma, r);
}

float Spreading(float theta)
{
    // Plus le vent est fort, plus les vagues sont alignées.
    float s = lerp(4.0, 14.0, saturate(gU10 / 25.0));
    // Gamma(s+1)/Gamma(s+1/2) ~ sqrt(s + 1/4)
    float norm = sqrt(s + 0.25) / (2.0 * sqrt(PI));
    return norm * pow(abs(cos(theta * 0.5)), 2.0 * s);
}

float Amplitude(float2 k)
{
    float kl = length(k);
    if (kl < max(gKMin, 1e-4) || kl >= gKMax) return 0.0;
    float w = sqrt(G * kl);
    float dwdk = G / (2.0 * w);
    float theta = atan2(k.y, k.x) - atan2(gWindXZ.y, gWindXZ.x);
    float S = JONSWAP(w) * Spreading(theta) * dwdk / kl;
    S *= exp(-kl * kl * 1e-4); // amortit les ondes capillaires (< ~6 cm)
    float dk = 2.0 * PI / gPatchSize;
    return sqrt(2.0 * S * dk * dk);
}

float2 Gaussian(uint2 id)
{
    uint seed = Hash3(uint3(id, gCascade * 7919u + 17u));
    float u1 = max(HashF(seed), 1e-6);
    float u2 = HashF(seed ^ 0xA511E9B3u);
    float r = sqrt(-2.0 * log(u1));
    return float2(r * cos(2.0 * PI * u2), r * sin(2.0 * PI * u2));
}

float2 WaveVector(uint2 id)
{
    return 2.0 * PI * (float2(id) - float(gN / 2)) / gPatchSize;
}

// Passe 1 (seulement si le vent change) : spectre initial.
[numthreads(8, 8, 1)]
void InitSpectrum(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gN)) return;
    uint2 idNeg = (gN - id.xy) % gN;
    float2 h0  = Gaussian(id.xy) * Amplitude(WaveVector(id.xy)) * 0.70710678;
    float2 h0n = Gaussian(idNeg) * Amplitude(-WaveVector(id.xy)) * 0.70710678;
    outA[id.xy] = float4(h0, CConj(h0n));
}

// Passe 2 (chaque frame) : spectre à l'instant t pour les 8 champs utiles.
[numthreads(8, 8, 1)]
void TimeSpectrum(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gN)) return;
    float4 h0 = h0Tex[id.xy];
    float2 k = WaveVector(id.xy);
    float kl = max(length(k), 1e-4);

    // Fréquence quantifiée : le motif boucle exactement toutes les 200 s,
    // ce qui évite la perte de précision du temps en float.
    float w0 = 2.0 * PI / 200.0;
    float w = floor(sqrt(G * kl) / w0) * w0;
    float ph = w * gOceanTime;
    float2 e = float2(cos(ph), sin(ph));

    float2 h  = CMul(h0.xy, e) + CMul(h0.zw, CConj(e));
    float2 ih = float2(-h.y, h.x);                  // i * h

    float2 dx  = ih * (k.x / kl);
    float2 dz  = ih * (k.y / kl);
    float2 sx  = ih * k.x;                          // pente dDy/dx
    float2 sz  = ih * k.y;                          // pente dDy/dz
    float2 jxx = -h * (k.x * k.x / kl);             // dDx/dx
    float2 jzz = -h * (k.y * k.y / kl);             // dDz/dz
    float2 jxz = -h * (k.x * k.y / kl);             // dDx/dz

    outA[id.xy] = float4(Pack(dx, h), Pack(dz, sx));
    outB[id.xy] = float4(Pack(sz, jxx), Pack(jzz, jxz));
}
