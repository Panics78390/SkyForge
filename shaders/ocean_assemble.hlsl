// Assemblage après IFFT : correction de signe, déplacement final, dérivées et écume.
//
// Le spectre est centré (k = 0 au texel N/2) : la transformée inverse est donc multipliée
// par (-1)^(x+y). Le Jacobien de la transformation horizontale
//     J = (1 + l dDx/dx)(1 + l dDz/dz) - (l dDx/dz)^2
// devient < 1 quand la crête se replie sur elle-même : c'est là qu'on dépose l'écume,
// qui décroît ensuite exponentiellement.
#include "OceanCommon.hlsli"

Texture2D<float4>   texA     : register(t0);
Texture2D<float4>   texB     : register(t1);
RWTexture2D<float4> outDisp  : register(u0); // xyz déplacement (m), w écume [0,1]
RWTexture2D<float4> outDeriv : register(u1); // dDy/dx, dDy/dz, l dDx/dx, l dDz/dz

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gN)) return;
    const float sgn = ((id.x + id.y) & 1) ? -1.0 : 1.0;
    const float4 a = texA[id.xy] * sgn; // Dx, Dy, Dz, Sx
    const float4 b = texB[id.xy] * sgn; // Sz, Jxx, Jzz, Jxz
    const float l = gChoppiness;

    const float3 disp = float3(l * a.x, a.y, l * a.z);
    const float jxx = 1.0 + l * b.y;
    const float jzz = 1.0 + l * b.z;
    const float jxz = l * b.w;
    const float J = jxx * jzz - jxz * jxz;

    float foam = outDisp[id.xy].w * exp(-gFoamDecay * gDeltaTime);
    foam = max(foam, saturate((gFoamThreshold - J) * 1.5));

    outDisp[id.xy] = float4(disp, saturate(foam));
    outDeriv[id.xy] = float4(a.w, b.x, l * b.y, l * b.z);
}
