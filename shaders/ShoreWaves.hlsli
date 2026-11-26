// Vagues de bord : trains de houle qui arrivent perpendiculairement au rivage, grandissent
// en eau peu profonde (loi de Green, A ~ h^-1/4), se raidissent puis déferlent quand leur
// hauteur approche ~0,8 fois la profondeur (critère de McCowan : H/h = 0,78, soit A = 0,39 h).
// Le jet de rive (run-up) fait monter et descendre le niveau de l'eau sur le sable.
#ifndef SHORE_WAVES_HLSLI
#define SHORE_WAVES_HLSLI
#include "Scene.hlsli"

struct ShoreInfo
{
    float3 disp;    // déplacement (m)
    float2 slope;   // pente de la surface (dy/dx, dy/dz)
    float  foam;    // écume de déferlement
};

// Normale horizontale au rivage, orientée vers la terre.
float2 ShoreNormal(float x)
{
    float dS = (ShoreLine(x + 1.0) - ShoreLine(x - 1.0)) * 0.5;
    return normalize(float2(-dS, 1.0));
}

// Phase des vagues : les crêtes avancent vers la terre et ondulent un peu le long du rivage.
float ShorePhase(float x, float d, float k, float w)
{
    float along = TFbm(float2(x * 0.015, 1.3)) * 6.0;
    return k * d + along - w * gOceanTime;
}

ShoreInfo ShoreWaves(float2 xz, float depth0, float amp, float wavelength, float runup, float w)
{
    ShoreInfo s;
    s.disp = 0;
    s.slope = 0;
    s.foam = 0;
    float d = xz.y - ShoreLine(xz.x);               // distance signée au rivage (< 0 en mer)
    float env = smoothstep(-170.0, -70.0, d) * (1.0 - smoothstep(15.0, 35.0, d));
    if (env <= 0.0 || amp <= 0.0) return s;

    float2 n = ShoreNormal(xz.x);
    float k = 2.0 * PI / wavelength;
    // w : pulsation fournie par le C++ (dispersion sqrt(g k), quantifiée pour boucler sur 200 s).
    float th = ShorePhase(xz.x, d, k, w);

    float h = max(depth0, 0.05);
    float A = amp * clamp(pow(4.0 / h, 0.25), 0.6, 1.7);   // levée de la houle
    float Amax = 0.39 * h + 0.03;                          // limite de déferlement
    float breaking = saturate((A / Amax - 0.7) * 3.0);
    A = min(A, Amax) * env;
    float Q = lerp(0.35, 0.95, saturate(1.0 - h / 3.0));   // crête qui se penche vers la plage

    float c = cos(th), sn = sin(th);
    s.disp.y = A * c;
    s.disp.xz = -n * (Q * A * sn);
    s.slope = n * (-A * k * sn);

    // Jet de rive : tout le bord monte et descend au rythme des vagues qui arrivent.
    float thShore = ShorePhase(xz.x, 0.0, k, w);
    float swash = pow(saturate(0.5 + 0.5 * cos(thShore)), 1.5);
    s.disp.y += runup * swash * smoothstep(-70.0, -20.0, d);

    // Écume : sur la crête qui déferle, puis une traînée qui s'efface derrière elle.
    float crest = pow(saturate(c), 6.0);
    float behind = frac(-th / (2.0 * PI));                 // temps écoulé depuis le passage de la crête
    float trail = exp(-behind * 5.0);
    s.foam = breaking * max(crest, trail * 0.85) * env;
    return s;
}

// Motif « dentelle » de l'écume (réseau de bulles) : bords de cellules de Worley.
float FoamLace(float2 xz, float t)
{
    float a = 1.0 - Worley3(float3(xz * 0.55, t * 0.12), 0);
    float b = 1.0 - Worley3(float3(xz * 1.7 + 13.0, t * 0.2), 0);
    return saturate(smoothstep(0.25, 0.6, a) * 0.7 + smoothstep(0.3, 0.7, b) * 0.5);
}

#endif
