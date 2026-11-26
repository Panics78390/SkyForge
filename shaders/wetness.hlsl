// Carte d'humidité du sol de la plage (R = humidité de surface, G = niveau des flaques).
//  * Mouillage : proportionnel aux précipitations, seulement si la pluie atteint le sol
//    (pas sous un rocher en surplomb) => le sable reste sec sous l'arche.
//  * Estran : sous la ligne d'eau + jet de rive, le sable est toujours détrempé.
//  * Flaques : se remplissent une fois la surface saturée.
//  * Séchage : base + vent + soleil (atténué par l'ombre des nuages), accéléré par la chaleur.
#include "Common.hlsli"
#include "Scene.hlsli"

cbuffer WetCB : register(b1)
{
    uint  gMapSize;
    uint  gRockCount;
    float gWetRate;     // 1/s par unité de précipitation
    float gPuddleRate;
    float gDryBase;
    float gDryWind;     // par m/s de vent
    float gDrySun;
    float gPuddleDry;   // les flaques sèchent plus lentement que la surface
};

StructuredBuffer<Rock> gRocks  : register(t0);
Texture2D<float4>     gWeather : register(t1);
RWTexture2D<float2>   outWet   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gMapSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float(gMapSize);
    float2 xz = lerp(gTerrainMin.xz, gTerrainMax.xz, uv);
    float ground = TerrainHeight(xz);
    if (ground < gWaterLevel + 0.6) { outWet[id.xy] = float2(1.0, 0.0); return; } // estran

    float exposed = OcclusionHeight(gRocks, gRockCount, xz) <= ground + 0.05 ? 1.0 : 0.0;
    float rain = gPrecip * exposed;

    float2 w = outWet[id.xy];
    w.x = saturate(w.x + rain * gWetRate * gDeltaTime);
    w.y = saturate(w.y + rain * saturate((w.x - 0.7) * 3.0) * gPuddleRate * gDeltaTime);

    float sun = saturate(gSunDir.y) * gSunIntensity * CloudShadow(gWeather, float3(xz.x, ground, xz.y));
    float dry = (gDryBase + gDryWind * gWindSpeed + gDrySun * sun) * max(0.2, 1.0 + 0.04 * (gTemperature - 15.0));
    dry *= 1.0 - saturate(rain * 4.0); // pas de séchage sous la pluie
    w.x = saturate(w.x - dry * gDeltaTime);
    w.y = saturate(w.y - dry * gPuddleDry * gDeltaTime);
    w.x = max(w.x, saturate(w.y * 3.0)); // une flaque garde ses bords mouillés
    outWet[id.xy] = w;
}
