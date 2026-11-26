#ifndef CLOUD_COMMON_HLSLI
#define CLOUD_COMMON_HLSLI
#include "Common.hlsli"

// Miroir de CloudCB (src/Clouds.cpp).
cbuffer CloudCB : register(b1)
{
    uint2 gCloudRes;
    uint  gSteps;
    uint  gLightSteps;
    float gShapeScale;      // 1 / taille du motif de forme (m)
    float gDetailScale;     // 1 / taille du motif de détail (m)
    float gDetailStrength;  // érosion des bords
    float gExtinction;      // coefficient d'extinction pour densité = 1 (1/m)
    float gG1;              // Henyey-Greenstein avant
    float gG2;              // Henyey-Greenstein arrière
    float gPowder;          // intensité de l'effet « powder »
    float gAmbient;         // intensité de la lumière ambiante
    float gHistoryBlend;    // poids de l'historique (reprojection)
    uint  gHistoryValid;
    float gNearStep;        // pas minimal près de la caméra (m)
    float _padCloud;
};

#endif
