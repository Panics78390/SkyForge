// Génération au démarrage des textures de bruit des nuages (approche Schneider / Horizon Zero Dawn).
//  * Forme 128^3 : R = Perlin-Worley, G/B/A = Worley fBm à fréquences croissantes.
//  * Détail 32^3 : R/G/B = Worley fBm haute fréquence (érosion des bords).
//  * Carte météo 512^2 : R = couverture, G = type de nuage, B = humidité locale.
// Toutes périodiques (bruit « tileable ») pour l'échantillonnage en mode WRAP.
#include "Common.hlsli"

RWTexture3D<float4> outVolume  : register(u0);
RWTexture2D<float4> outWeather : register(u1);

float WorleyFbm(float3 p, int freq)
{
    return Worley3(p * freq, freq) * 0.625
         + Worley3(p * freq * 2, freq * 2) * 0.25
         + Worley3(p * freq * 4, freq * 4) * 0.125;
}

float PerlinFbm(float3 p, int freq, int octaves)
{
    float sum = 0, amp = 0.5, norm = 0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += (Perlin3(p * freq, freq) * 0.5 + 0.5) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2;
    }
    return sum / norm;
}

[numthreads(4, 4, 4)]
void BakeShape(uint3 id : SV_DispatchThreadID)
{
    float3 p = (float3(id) + 0.5) / 128.0;
    float perlin = PerlinFbm(p, 4, 4);
    float w4 = WorleyFbm(p, 4);
    // Perlin-Worley : le Perlin « gonflé » par les cellules de Worley.
    float pw = saturate(Remap(perlin, 0.0, 1.0, w4, 1.0));
    outVolume[id] = float4(pw, w4, WorleyFbm(p, 8), WorleyFbm(p, 16));
}

[numthreads(4, 4, 4)]
void BakeDetail(uint3 id : SV_DispatchThreadID)
{
    float3 p = (float3(id) + 0.5) / 32.0;
    outVolume[id] = float4(WorleyFbm(p, 2), WorleyFbm(p, 4), WorleyFbm(p, 8), 1.0);
}

[numthreads(8, 8, 1)]
void BakeWeather(uint3 id : SV_DispatchThreadID)
{
    float3 p = float3((float2(id.xy) + 0.5) / 512.0, 0.37);
    float cov = PerlinFbm(p, 5, 5);
    cov = saturate(Remap(cov, 0.25, 0.75, 0.0, 1.0));
    cov = saturate(cov * 0.75 + Worley3(p * 9, 9) * 0.35);
    float type = PerlinFbm(p + 0.5, 3, 3);
    float wet = PerlinFbm(p + 0.25, 6, 3);
    outWeather[id.xy] = float4(cov, type, wet, 1.0);
}
