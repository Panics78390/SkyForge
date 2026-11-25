// Champ de vent 3D (volume centré caméra, déplacé par pas entiers de cellule).
// xyz = vitesse (m/s), w = intensité de turbulence. Échantillonné par la pluie et
// disponible pour la végétation, le tissu et les particules via SampleWind().
//
//  * Profil logarithmique de couche limite : u(z) = U10 ln(z/z0) / ln(10/z0).
//  * Rafales : bruit basse fréquence transporté dans le sens du vent.
//  * Turbulence : bruit à divergence nulle (rotationnel d'un potentiel = « curl noise »),
//    pour que les particules tourbillonnent sans s'agglutiner.
#include "Common.hlsli"

RWTexture3D<float4> outWind : register(u0);

float3 Potential(float3 p)
{
    return float3(Perlin3(p, 0), Perlin3(p + float3(31.4, 17.2, 5.9), 0), Perlin3(p + float3(-12.7, 44.1, 23.3), 0));
}

float3 CurlNoise(float3 p)
{
    const float e = 0.1;
    float3 dx = Potential(p + float3(e, 0, 0)) - Potential(p - float3(e, 0, 0));
    float3 dy = Potential(p + float3(0, e, 0)) - Potential(p - float3(0, e, 0));
    float3 dz = Potential(p + float3(0, 0, e)) - Potential(p - float3(0, 0, e));
    return float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x) / (2.0 * e);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gWindRes)) return;
    float3 p = gWindOrigin + (float3(id) + 0.5) * gWindCellSize;

    const float z0 = 0.02; // rugosité (mer agitée / terrain dégagé)
    float height = max(p.y - gWaterLevel, z0 * 1.5);
    float profile = clamp(log(height / z0) / log(10.0 / z0), 0.0, 1.6);

    float3 travel = gWindDir * gTime * max(gWindSpeed, 1.0);
    float g = Perlin3(p * 0.012 - travel * 0.012 + float3(0, gTime * 0.03, 0), 0);
    float gust = 1.0 + gGust * g * 1.4;

    float3 turb = CurlNoise(p * 0.04 - travel * 0.04) * gWindSpeed * 0.12 * (0.3 + gGust);
    float3 v = gWindDir * gWindSpeed * profile * gust + turb;
    outWind[id] = float4(v, length(turb));
}
