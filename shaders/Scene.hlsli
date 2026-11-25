// Scène de démonstration : une île et quelques bâtiments décrits par des boîtes.
// Les mêmes boîtes servent au rendu, aux ombres (lancer de rayon analytique) et à
// l'occlusion de la pluie (« carte de hauteur vue du ciel »).
#ifndef SCENE_HLSLI
#define SCENE_HLSLI

struct Box
{
    float3 mn;  uint  material;
    float3 mx;  float pad;
};

// Hauteur de la première surface rencontrée par une goutte tombant à la verticale.
float OcclusionHeight(StructuredBuffer<Box> boxes, uint count, float2 xz, float baseHeight)
{
    float h = baseHeight;
    for (uint i = 0; i < count; ++i)
    {
        Box b = boxes[i];
        if (all(xz >= b.mn.xz) && all(xz <= b.mx.xz)) h = max(h, b.mx.y);
    }
    return h;
}

// Intersection rayon / boîtes (méthode des slabs) : 1 = éclairé, 0 = à l'ombre.
float ShadowRay(StructuredBuffer<Box> boxes, uint count, float3 p, float3 d)
{
    float3 inv = 1.0 / (abs(d) > 1e-6 ? d : 1e-6);
    for (uint i = 0; i < count; ++i)
    {
        Box b = boxes[i];
        float3 t0 = (b.mn - p) * inv;
        float3 t1 = (b.mx - p) * inv;
        float3 tmin = min(t0, t1), tmax = max(t0, t1);
        float tn = max(max(tmin.x, tmin.y), tmin.z);
        float tf = min(min(tmax.x, tmax.y), tmax.z);
        if (tf > max(tn, 1e-3) && tn > 1e-3) return 0.0;
    }
    return 1.0;
}

#endif
