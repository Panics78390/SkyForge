// Carte d'humidité du sol de la plage (R = humidité, G = flaques, B = épaisseur de neige).
//  * Mouillage : proportionnel aux précipitations, seulement si la pluie atteint le sol
//    (pas sous un rocher en surplomb) => le sable reste sec sous l'arche.
//  * Estran : sous la ligne d'eau + jet de rive, le sable est toujours détrempé.
//  * Flaques : se remplissent une fois la surface saturée.
//  * Séchage : base + vent + soleil (atténué par l'ombre des nuages), accéléré par la chaleur.
//  * Neige : tient au sol quand il gèle, fond au-dessus de 0 °C (plus vite au soleil et sous la pluie)
//    et l'eau de fonte mouille le sable.
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
RWTexture2D<float4>   outWet   : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gMapSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float(gMapSize);
    float2 xz = lerp(gTerrainMin.xz, gTerrainMax.xz, uv);
    float ground = TerrainHeight(xz);
    if (ground < gWaterLevel + 0.6) { outWet[id.xy] = float4(1.0, 0.0, 0.0, 0.0); return; } // estran

    float exposed = OcclusionHeight(gRocks, gRockCount, xz) <= ground + 0.05 ? 1.0 : 0.0;
    float snowFall = gPrecip * SnowFactor() * exposed;
    float rain = gPrecip * (1.0 - SnowFactor()) * exposed;

    float4 w = outWet[id.xy];
    w.x = saturate(w.x + rain * gWetRate * gDeltaTime);
    w.y = saturate(w.y + rain * saturate((w.x - 0.7) * 3.0) * gPuddleRate * gDeltaTime);

    float sun = saturate(gSunDir.y) * gSunIntensity * CloudShadow(gWeather, float3(xz.x, ground, xz.y));
    float dry = (gDryBase + gDryWind * gWindSpeed + gDrySun * sun) * max(0.2, 1.0 + 0.04 * (gTemperature - 15.0));
    dry *= 1.0 - saturate(rain * 4.0); // pas de séchage sous la pluie
    w.x = saturate(w.x - dry * gDeltaTime);
    w.y = saturate(w.y - dry * gPuddleDry * gDeltaTime);
    w.x = max(w.x, saturate(w.y * 3.0)); // une flaque garde ses bords mouillés

    // Neige : accumulation (~1 couche complète en 2 min de forte neige), fonte au-dessus de 0 °C.
    float melt = max(gTemperature, 0.0) * 0.004 + sun * 0.004 * step(0.0, gTemperature) + rain * 0.02;
    melt *= gTemperature > -0.5 ? 1.0 : 0.1;
    float snow = saturate(w.z + snowFall * 0.008 * gDeltaTime - melt * gDeltaTime);
    w.x = saturate(w.x + (w.z - snow) * 2.0 * step(snow, w.z)); // l'eau de fonte mouille
    w.z = snow;
    outWet[id.xy] = w;
}
