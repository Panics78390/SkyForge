// Surfaces mouillées (d'après Lagarde, « Water drop » 2013), à inclure dans tout matériau PBR.
//  1. L'eau remplit les pores : l'albédo s'assombrit, d'autant plus que le matériau est poreux.
//  2. Un film d'eau lisse la surface : la rugosité baisse (plus vite sur les matériaux non poreux).
//  3. Dans les flaques, la couche d'eau domine : rugosité ~0, normale plate (+ ondulations
//     des impacts de pluie), F0 de l'eau (0,02).
#ifndef WET_SURFACE_HLSLI
#define WET_SURFACE_HLSLI

struct Surface
{
    float3 albedo;
    float  roughness;
    float  porosity;   // 0 = métal/verre, 1 = très poreux (terre, béton brut)
    float3 N;
    float  f0;
};

void ApplyWetness(inout Surface s, float wetness, float puddle, float3 puddleN)
{
    float darken = lerp(1.0, lerp(0.85, 0.3, s.porosity), wetness);
    s.albedo *= darken;
    s.roughness = lerp(s.roughness, 0.07, wetness * lerp(1.0, 0.55, s.porosity));

    s.albedo *= lerp(1.0, 0.55, puddle);
    s.roughness = lerp(s.roughness, 0.015, puddle);
    s.N = normalize(lerp(s.N, puddleN, puddle));
    s.f0 = lerp(s.f0, 0.02, puddle);
}

// Ondulations des impacts de pluie dans les flaques : anneaux qui s'élargissent,
// une goutte par cellule et par période, phase aléatoire.
float3 RainRipples(float2 xz, float time, float intensity)
{
    float2 grad = 0;
    [unroll] for (int layer = 0; layer < 2; ++layer)
    {
        float2 p = xz * (2.5 + layer * 1.3) + layer * 17.0;
        int2 cell = int2(floor(p));
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 c = cell + int2(x, y);
            float3 h = Hash33(int3(c, layer));
            float2 center = float2(c) + h.xy;
            float t = frac(time * 1.2 + h.z);
            float2 dv = p - center;
            float r = length(dv);
            float ring = r - t * 0.9;
            float wave = sin(ring * 40.0) * exp(-ring * ring * 400.0) * (1.0 - t) * (1.0 - t);
            grad += dv / max(r, 1e-3) * wave;
        }
    }
    grad *= intensity * 0.25;
    return normalize(float3(-grad.x, 1.0, -grad.y));
}

#endif
