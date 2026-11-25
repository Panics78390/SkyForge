#ifndef OCEAN_COMMON_HLSLI
#define OCEAN_COMMON_HLSLI
#include "Common.hlsli"

// Constantes d'une cascade FFT (miroir de OceanCB dans src/Ocean.cpp).
cbuffer OceanCB : register(b1)
{
    float  gPatchSize;     // taille du motif en mètres (L)
    float  gKMin;          // bande de nombres d'onde gérée par la cascade
    float  gKMax;
    uint   gN;             // résolution FFT
    float2 gWindXZ;        // direction du vent
    float  gU10;           // vitesse du vent à 10 m (m/s)
    float  gFetch;         // fetch en mètres
    float  gChoppiness;    // lambda : déplacement horizontal (vagues pointues)
    uint   gCascade;
    float  gFoamDecay;     // 1/s
    float  gFoamThreshold; // seuil de Jacobien sous lequel l'écume apparaît
    uint   gVertical;      // passe IFFT : 0 = lignes, 1 = colonnes
    float3 _padOcean;
};

static const float G = 9.81;

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 CConj(float2 a) { return float2(a.x, -a.y); }
// Empaquetage de deux champs complexes à transformée réelle : a + i*b.
float2 Pack(float2 a, float2 b) { return float2(a.x - b.y, a.y + b.x); }

#endif
