// Rendu de la surface d'océan.
//
// LOD de simulation :
//  * maillage centré caméra, densité radiale (cellules ~0,4 m au centre, ~100 m au bord) ;
//  * cascade fine (déplacement) à pleine force jusqu'à gLodNear (50 m), fondu jusqu'à 3x ;
//  * cascades FFT en fondu vers un modèle analytique de Gerstner entre 0,7 et 1 x gLodFar (500 m) ;
//  * au-delà de gLodFar : Gerstner seul (4 ondes), calé sur le pic du spectre JONSWAP.
//  * près de la plage : amplitude réduite avec la profondeur (le terrain est analytique),
//    eau turquoise qui laisse voir le sable, écume de rivage.
#include "Common.hlsli"
#include "Scene.hlsli"

cbuffer OceanDrawCB : register(b1)
{
    float4 gPatch;        // xyz tailles des cascades (m), w nombre de cascades
    uint   gGridQuads;
    float  gGridExtent;   // rayon du maillage (m)
    float  gGridLinear;   // part linéaire de la répartition des sommets
    float  gFoamIntensity;
    float  gChoppy;
    float3 _padDraw;
};

Texture2D<float4> gDisp0   : register(t0);
Texture2D<float4> gDisp1   : register(t1);
Texture2D<float4> gDisp2   : register(t2);
Texture2D<float4> gDeriv0  : register(t3);
Texture2D<float4> gDeriv1  : register(t4);
Texture2D<float4> gDeriv2  : register(t5);
Texture2D<float4> gWeather : register(t6);

// --- Gerstner ---------------------------------------------------------------
void Gerstner(float2 xz, out float3 disp, out float3 normal)
{
    static const float angles[4] = { 0.0, 0.42, -0.55, 0.9 };
    static const float lens[4]   = { 1.0, 0.61, 0.37, 0.23 };
    static const float amps[4]   = { 1.0, 0.45, 0.25, 0.12 };
    disp = 0;
    float3 n = float3(0, 1, 0);
    float2 wd = normalize(gWindDir.xz + 1e-5);
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float c = cos(angles[i]), s = sin(angles[i]);
        float2 d = float2(wd.x * c - wd.y * s, wd.x * s + wd.y * c);
        float L = max(gGerstnerWavelength * lens[i], 1.0);
        float k = 2.0 * PI / L;
        float w = sqrt(9.81 * k);
        float A = gGerstnerAmp * amps[i];
        float Q = min(0.6 / max(k * A * 4.0, 1e-4), 1.0) * saturate(gChoppy);
        float th = k * dot(d, xz) - w * gOceanTime + i * 1.7;
        disp += float3(Q * A * d.x * cos(th), A * sin(th), Q * A * d.y * cos(th));
        n -= float3(d.x * k * A * cos(th), Q * k * A * sin(th), d.y * k * A * cos(th));
    }
    normal = normalize(n);
}

struct Weights { float c2Disp; float fft; };
Weights LodWeights(float dist)
{
    Weights w;
    w.c2Disp = 1.0 - smoothstep(gLodNear, gLodNear * 3.0, dist);
    w.fft = 1.0 - smoothstep(gLodFar * 0.7, gLodFar, dist);
    return w;
}

float3 SampleDisplacement(float2 xz, float dist, out float foam)
{
    Weights w = LodWeights(dist);
    float4 d0 = gDisp0.SampleLevel(gLinearWrap, xz / gPatch.x, 0);
    float4 d1 = gPatch.w > 1.5 ? gDisp1.SampleLevel(gLinearWrap, xz / gPatch.y, 0) : 0;
    float4 d2 = gPatch.w > 2.5 ? gDisp2.SampleLevel(gLinearWrap, xz / gPatch.z, 0) : 0;
    float3 fft = d0.xyz + d1.xyz + d2.xyz * w.c2Disp;
    foam = saturate(d0.w + d1.w + d2.w) * w.fft;
    float3 gd, gn;
    Gerstner(xz, gd, gn);
    return lerp(gd, fft, w.fft);
}

struct VSOut
{
    float4 pos   : SV_Position;
    float3 world : WORLD;
    float2 xz    : UNDISPLACED;
    float  foam  : FOAM;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    static const uint2 corners[6] = { uint2(0,0), uint2(1,0), uint2(0,1), uint2(1,0), uint2(1,1), uint2(0,1) };
    uint q = vid / 6;
    uint2 c = corners[vid % 6];
    uint2 g = uint2(q % gGridQuads, q / gGridQuads) + c;
    float2 u = float2(g) / float(gGridQuads) * 2.0 - 1.0;
    // Répartition radiale : dense au centre, lâche au bord.
    float2 local = gGridExtent * (gGridLinear * u + (1.0 - gGridLinear) * u * u * u);
    // Alignement sur la grille de la cellule centrale pour limiter le « nagement ».
    float cell = gGridExtent * gGridLinear * 2.0 / float(gGridQuads);
    float2 snapped = floor(gCamPos.xz / cell) * cell;
    float2 xz = snapped + local;

    float dist = length(xz - gCamPos.xz);
    float foam;
    float3 d = SampleDisplacement(xz, dist, foam);
    // Eau peu profonde : les vagues s'aplatissent à l'approche du rivage.
    float depth = gWaterLevel - TerrainHeight(xz);
    d *= lerp(0.12, 1.0, saturate(depth / 6.0));
    float3 world = float3(xz.x, gWaterLevel, xz.y) + d;

    VSOut o;
    o.pos = mul(float4(world, 1), gViewProj);
    o.world = world;
    o.xz = xz;
    o.foam = foam;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float dist = length(i.world - gCamPos);
    Weights w = LodWeights(dist);

    // Normale : somme des pentes des cascades (fondu des petites vagues avec la distance
    // pour éviter l'aliasing), puis mélange avec Gerstner au loin.
    float4 n0 = gDeriv0.Sample(gLinearWrap, i.xz / gPatch.x);
    float4 n1 = gPatch.w > 1.5 ? gDeriv1.Sample(gLinearWrap, i.xz / gPatch.y) : 0;
    float4 n2 = gPatch.w > 2.5 ? gDeriv2.Sample(gLinearWrap, i.xz / gPatch.z) : 0;
    float f1 = 1.0 - smoothstep(600.0, 2500.0, dist);
    float f2 = 1.0 - smoothstep(80.0, 400.0, dist);
    float2 slope = n0.xy + n1.xy * f1 + n2.xy * f2;
    float jx = n0.z + n1.z * f1 + n2.z * f2;
    float jz = n0.w + n1.w * f1 + n2.w * f2;
    float3 nFFT = normalize(float3(-slope.x / max(1.0 + jx, 0.2), 1.0, -slope.y / max(1.0 + jz, 0.2)));
    float3 gd, gn;
    Gerstner(i.xz, gd, gn);
    float3 N = normalize(lerp(gn, nFFT, max(w.fft, 0.35)));
    // Lissage lointain (la normale moyenne d'un pixel lointain tend vers le haut).
    N = normalize(lerp(N, float3(0, 1, 0), saturate(dist / 12000.0)));

    float3 V = normalize(gCamPos - i.world);
    float3 L = gSunDir;
    float3 H = normalize(V + L);
    float NoV = saturate(dot(N, V)) + 1e-4;
    float NoL = saturate(dot(N, L));
    float NoH = saturate(dot(N, H));

    float shadow = CloudShadow(gWeather, i.world);
    float3 sun = SunRadiance() * shadow;

    // Réflexion : ciel et nuages (le rayon réfléchi est replié au-dessus de l'horizon).
    float3 R = reflect(-V, N);
    R.y = abs(R.y);
    float F = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
    float3 refl = CloudySky(gWeather, R); // ciel + nuages reflétés

    // Spéculaire solaire GGX, rugosité qui augmente avec la distance et le vent.
    float rough = lerp(0.06, 0.2, saturate(dist / 3000.0)) + gWindSpeed * 0.003;
    float spec = D_GGX(NoH, rough * rough) * V_SmithJoint(NoV, NoL, rough * rough) * NoL;
    float3 specCol = sun * spec * F_Schlick(0.02, saturate(dot(V, H))).x;

    // Diffusion sous-surface : eau profonde + lumière transmise dans les crêtes.
    float3 deep = float3(0.004, 0.018, 0.035);
    float3 sssCol = float3(0.02, 0.20, 0.18);
    float crest = saturate((i.world.y - gWaterLevel) * 0.35 + 0.3);
    float back = pow(saturate(dot(V, -L) * 0.5 + 0.5), 3.0);
    float3 water = deep * (AmbientLight(N) + sun * 0.15) + sssCol * sun * back * crest * 0.06;

    // Eau peu profonde : on voit le sable à travers une couche absorbante (rouge absorbé d'abord).
    float waterDepth = max(i.world.y - TerrainHeight(i.world.xz), 0.0);
    float3 sandLit = float3(0.70, 0.62, 0.45) * (sun * saturate(gSunDir.y) * 0.8 + AmbientLight(float3(0, 1, 0)));
    float3 bottom = sandLit * exp(-waterDepth * float3(0.6, 0.14, 0.1));
    float3 under = lerp(water, bottom, exp(-waterDepth * 0.28));

    float3 col = lerp(under, refl, F) + specCol;

    // Écume de rivage : bande qui respire avec le ressac.
    float surf = 1.0 - smoothstep(0.0, 1.3, waterDepth);
    float swash = 0.55 + 0.45 * sin(gOceanTime * 0.9 + TFbm(i.world.xz * 0.15) * 9.0);
    i.foam = max(i.foam, surf * swash * smoothstep(0.35, 0.6, TFbm(i.world.xz * 0.9 + gOceanTime * 0.2)) );

    // Écume (crêtes qui se replient + vent fort).
    float foam = saturate(i.foam * gFoamIntensity);
    float3 foamCol = (sun * saturate(dot(N, L) * 0.6 + 0.4) + AmbientLight(N)) * 0.75;
    col = lerp(col, foamCol, foam);

    // Brouillard atmosphérique.
    float3 fogCol = SkyColor(normalize(float3(-V.x, 0.02, -V.z)));
    col = lerp(col, fogCol, FogFactor(dist));
    return float4(col, 1);
}
