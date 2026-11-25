// Petite bibliothèque mathématique (convention vecteur-ligne : p' = p * M,
// identique à `mul(float4(p,1), M)` en HLSL avec #pragma pack_matrix(row_major)).
#pragma once
#include <cmath>
#include <algorithm>

namespace me {

constexpr float kPi = 3.14159265358979f;

struct float2 { float x = 0, y = 0; };
struct float3 {
    float x = 0, y = 0, z = 0;
    float3() = default;
    constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    float3 operator+(const float3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    float3 operator-(const float3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    float3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float3& operator+=(const float3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};
struct float4 { float x = 0, y = 0, z = 0, w = 0; };

inline float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 cross(const float3& a, const float3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const float3& v) { return std::sqrt(dot(v, v)); }
inline float3 normalize(const float3& v) { float l = length(v); return l > 0 ? v * (1.0f / l) : v; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

struct float4x4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    float4x4 operator*(const float4x4& b) const {
        float4x4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
        return r;
    }

    // Vue main gauche (Y haut, Z avant).
    static float4x4 lookTo(const float3& eye, const float3& fwd, const float3& up) {
        const float3 z = normalize(fwd);
        const float3 x = normalize(cross(up, z));
        const float3 y = cross(z, x);
        float4x4 r;
        r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x; r.m[0][3] = 0;
        r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y; r.m[1][3] = 0;
        r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z; r.m[2][3] = 0;
        r.m[3][0] = -dot(x, eye); r.m[3][1] = -dot(y, eye); r.m[3][2] = -dot(z, eye); r.m[3][3] = 1;
        return r;
    }

    // Projection perspective à Z inversé et plan lointain infini :
    // meilleure précision de profondeur sur un océan de plusieurs kilomètres.
    static float4x4 perspectiveReversedInf(float fovY, float aspect, float zNear) {
        const float f = 1.0f / std::tan(fovY * 0.5f);
        float4x4 r;
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = 0.0f;  r.m[2][3] = 1.0f;
        r.m[3][2] = zNear; r.m[3][3] = 0.0f;
        return r;
    }

    // Inverse générale (cofacteurs).
    float4x4 inverse() const {
        const float* a = &m[0][0];
        float inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        det = (std::fabs(det) > 1e-20f) ? 1.0f / det : 0.0f;
        float4x4 r;
        for (int i = 0; i < 16; ++i) (&r.m[0][0])[i] = inv[i] * det;
        return r;
    }
};

} // namespace me
