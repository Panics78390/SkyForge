// Ray marching volumétrique des nuages, en basse résolution.
//
// Maths en bref :
//  * Transmittance (Beer-Lambert) : T(t) = exp(-integrale de sigma_t ds).
//  * Diffusion simple vers l'œil : L = integrale de T(t) sigma_s(t) L_sun phase(theta) T_light(t) dt,
//    intégrée par segment de façon conservative : (S - S e^{-sigma dt}) / sigma (Hillaire 2015).
//  * Phase Henyey-Greenstein double lobe : p = lerp(HG(g1), HG(g2), 0.3),
//    HG(c, g) = (1 - g^2) / (4 pi (1 + g^2 - 2 g c)^{3/2}).
//  * Effet « powder » (Schneider 2015) : 1 - e^{-2d} assombrit les bords fins tournés vers
//    le soleil, ce qui donne l'aspect « chou-fleur » des cumulus.
//  * Pas adaptatif : la longueur du pas grandit avec la distance (fin près de la caméra pour
//    pouvoir voler dans les nuages, grossier au loin). Dans le vide, on avance par grands pas
//    avec la densité grossière ; dès qu'on touche un nuage, on recule d'un pas et on affine.
//  * Diffusion multiple (Wrenninge 2013, repris par Hillaire) : somme de 3 « octaves »
//    a^n * L_sun * phase(c^n g) * exp(-b^n tau), qui rend les nuages épais blancs et lumineux
//    au lieu de gris.
//  * Vu de près : une octave de détail supplémentaire et une distorsion « tourbillonnante »
//    donnent les volutes qu'on traverse en volant.
#include "CloudCommon.hlsli"

Texture2D<float>   gDepth   : register(t0);
Texture3D<float4>  gShape   : register(t1);
Texture3D<float4>  gDetail  : register(t2);
Texture2D<float4>  gWeather : register(t3);
RWTexture2D<float4> outCloud : register(u0); // rgb = lumière diffusée, a = transmittance
RWTexture2D<float>  outDist  : register(u1); // distance moyenne pondérée (pour la reprojection)

float HG(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float HeightGradient(float h, float type)
{
    // Stratus : couche basse et fine ; cumulonimbus : toute la hauteur.
    float top = lerp(0.3, 1.0, type);
    float bottom = saturate(h / 0.07);
    float upper = saturate((top - h) / max(top * 0.45, 1e-3));
    return bottom * upper;
}

float Density(float3 p, bool cheap, float nearDetail)
{
    float h = (p.y - gCloudBottom) / (gCloudTop - gCloudBottom);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    float4 wm = gWeather.SampleLevel(gLinearWrap, (p.xz + gCloudOffset) / 30000.0, 0);
    // La pluie épaissit et abaisse les nuages.
    float coverage = saturate(gCloudCoverage + (wm.r - 0.5) * 0.9 + gPrecip * 0.15);
    float type = saturate(gCloudType + (wm.g - 0.5) * 0.4);

    float3 sp = (p + float3(gCloudOffset.x, 0, gCloudOffset.y)) * gShapeScale;
    float4 s = gShape.SampleLevel(gLinearWrap, sp, 0);
    float fbm = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float base = Remap(s.r, -(1.0 - fbm), 1.0, 0.0, 1.0);
    base *= HeightGradient(h, type);
    base = saturate(Remap(base, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;
    if (cheap || base <= 0.0) return base;

    // Érosion par le bruit de détail (plus « vaporeux » en bas, plus « bouillonnant » en haut),
    // légèrement entraîné par le vent et la turbulence.
    // Distorsion par les canaux de la forme : les détails « tourbillonnent » au lieu d'être alignés.
    float3 swirl = (s.gba - 0.5) * 0.35;
    float3 dp = (p + float3(gCloudOffset.x, -gTime * 2.0, gCloudOffset.y) * 1.3) * gDetailScale + swirl;
    float4 d = gDetail.SampleLevel(gLinearWrap, dp, 0);
    float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    if (nearDetail > 0.0)
    {
        // Octave très fine, seulement près de la caméra (volutes visibles en vol).
        float4 d2 = gDetail.SampleLevel(gLinearWrap, dp * 4.7 + swirl * 2.0, 0);
        dfbm = lerp(dfbm, dfbm * 0.75 + d2.r * 0.25, nearDetail);
    }
    float mod = lerp(dfbm, 1.0 - dfbm, saturate(h * 6.0));
    return saturate(Remap(base, mod * gDetailStrength, 1.0, 0.0, 1.0));
}

float LightOpticalDepth(float3 p)
{
    float od = 0.0;
    float stepLen = (gCloudTop - gCloudBottom) * 0.35 / float(gLightSteps);
    float t = stepLen * 0.5;
    [loop] for (uint i = 0; i < gLightSteps; ++i)
    {
        float3 q = p + gSunDir * t;
        od += Density(q, i + 1 == gLightSteps, 0.0) * stepLen;
        t += stepLen;
        stepLen *= 1.6; // cône qui s'élargit
    }
    return od * gExtinction * gCloudDensity;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gCloudRes)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gCloudRes);
    float3 dir = ViewRay(uv);

    // Distance de la géométrie opaque (les nuages s'arrêtent devant elle).
    float depth = gDepth.SampleLevel(gPointClamp, uv, 0);
    float sceneDist = depth > 0.0 ? length(WorldFromDepth(uv, depth) - gCamPos) : 1e9;

    // Intersection avec la couche nuageuse (Terre plate, suffisant à cette échelle).
    float tEnter, tExit;
    if (abs(dir.y) < 1e-4)
    {
        bool inside = gCamPos.y > gCloudBottom && gCamPos.y < gCloudTop;
        tEnter = 0; tExit = inside ? 60000.0 : 0.0;
    }
    else
    {
        float tb = (gCloudBottom - gCamPos.y) / dir.y;
        float tt = (gCloudTop - gCamPos.y) / dir.y;
        tEnter = max(min(tb, tt), 0.0);
        tExit = max(tb, tt);
    }
    tExit = min(min(tExit, 60000.0), sceneDist);
    if (tExit <= tEnter)
    {
        outCloud[id.xy] = float4(0, 0, 0, 1);
        outDist[id.xy] = 60000.0;
        return;
    }

    // Pas qui grandit avec la distance : fin autour de la caméra, grossier au loin.
    float range = tExit - tEnter;
    float farStep = clamp(range / float(gSteps), 40.0, 600.0);
    float jitter = frac(52.9829189 * frac(dot(float2(id.xy), float2(0.06711056, 0.00583715))) + float(gFrameIndex % 64) * 0.618034);
    float t = tEnter + jitter * min(gNearStep + tEnter * 0.02, farStep);

    float cosTheta = dot(dir, gSunDir);
    float powderAmount = gPowder * saturate(0.6 - 0.6 * cosTheta);
    float3 sunL = SunRadiance();
    float3 skyUp = SkyColor(float3(0, 1, 0));
    float3 ambTop = lerp(skyUp, dot(skyUp, float3(0.3, 0.59, 0.11)).xxx, 0.5) * gAmbient;
    float3 ambBottom = ambTop * lerp(float3(0.45, 0.48, 0.55), float3(0.22, 0.23, 0.26), gPrecip); // bases sombres sous la pluie

    float3 scatter = 0;
    float T = 1.0;
    float distSum = 0, distW = 0;
    bool fine = false;
    uint empty = 0;

    [loop] for (uint i = 0; i < gSteps * 3 && t < tExit; ++i)
    {
        float step = min(gNearStep + t * 0.02, farStep);
        float3 p = gCamPos + dir * t;
        if (!fine)
        {
            if (Density(p, true, 0.0) > 0.0) { fine = true; empty = 0; t = max(t - step, tEnter); }
            else t += step * 2.0;
            continue;
        }
        float nearDetail = 1.0 - smoothstep(150.0, 600.0, t);
        float d = Density(p, false, nearDetail);
        if (d > 0.002)
        {
            empty = 0;
            float sigma = d * gExtinction * gCloudDensity;
            float tau = LightOpticalDepth(p);
            float powder = lerp(1.0, saturate(2.0 * (1.0 - exp(-2.0 * tau))), powderAmount);
            // Diffusion multiple : 3 octaves (a = b = c = 0,5).
            float3 sunS = 0;
            float an = 1.0, bn = 1.0, cn = 1.0;
            [unroll] for (int o = 0; o < 3; ++o)
            {
                float ph = lerp(HG(cosTheta, gG1 * cn), HG(cosTheta, gG2 * cn), 0.3) * 4.0 * PI;
                sunS += an * ph * exp(-tau * bn);
                an *= 0.5; bn *= 0.5; cn *= 0.5;
            }
            float h = saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
            float3 Ls = sunL * sunS * powder * 0.55 + lerp(ambBottom, ambTop, h);
            float Ti = exp(-sigma * step);
            scatter += T * (Ls - Ls * Ti); // = T * (S - S Ti) / sigma avec S = Ls * sigma
            distSum += t * T * (1.0 - Ti);
            distW += T * (1.0 - Ti);
            T *= Ti;
            if (T < 0.01) { T = 0.0; break; }
        }
        else if (++empty > 6)
        {
            fine = false;
        }
        t += step;
    }

    // Perspective aérienne : les nuages lointains se fondent dans l'horizon.
    float far = distW > 0 ? distSum / distW : tEnter;
    float haze = saturate(far / 50000.0);
    scatter = lerp(scatter, SkyColor(dir) * (1.0 - T), haze * haze);

    outCloud[id.xy] = float4(scatter, T);
    outDist[id.xy] = distW > 0 ? far : 60000.0;
}
