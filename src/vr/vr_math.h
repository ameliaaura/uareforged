#ifndef UA_VR_MATH_H_INCLUDED
#define UA_VR_MATH_H_INCLUDED

// Small math helpers for the VR module. Kept free of the engine's matrix.h so
// the conversions between OpenXR space and UA space are explicit in one place.
//
// Spaces
//   XR  : OpenXR tracking space. +X right, +Y up, -Z forward, metres.
//   UAV : UA view (camera) space. +X right, +Y DOWN, +Z forward, UA units.
//         The engine's TForm3D::SclRot rows are the camera's right/down/forward
//         axes in world space (world -> view), Pos is the eye position.
//   Basis change XR -> UAV is C = diag(1, -1, -1) (and the unit scale).

#include <math.h>
#include "vr.h"

namespace VR
{

struct M3 // row-major, m[r][c]; v' = M v
{
    double m[3][3];
    static M3 Ident() { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = (i == j); return r; }
    M3 operator*(const M3 &b) const
    {
        M3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
        return r;
    }
    M3 T() const { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i]; return r; }
    void Apply(const double v[3], double out[3]) const
    {
        for (int i = 0; i < 3; i++) out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
};

struct M4 // row-major, clip = M * v (the engine's mat4x4 convention before SetProjectionMatrix transposes)
{
    double m[4][4];
    static M4 Ident() { M4 r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = (i == j); return r; }
    M4 operator*(const M4 &b) const
    {
        M4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
        return r;
    }
};

// Rotation matrix of a unit quaternion (column vectors = rotated axes), XR space.
inline M3 QuatToM3(const Quatf &q)
{
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    M3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

inline Quatf QuatFromAxisAngle(double ax, double ay, double az, double rad)
{
    Quatf q;
    const double s = sin(rad * 0.5);
    q.x = ax * s; q.y = ay * s; q.z = az * s; q.w = cos(rad * 0.5);
    return q;
}

inline Quatf QuatMul(const Quatf &a, const Quatf &b)
{
    Quatf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

// XR rotation -> the same rotation expressed in UAV axes: C R C, C = diag(1,-1,-1).
inline M3 XrRotToUa(const M3 &r)
{
    static const double c[3] = {1, -1, -1};
    M3 o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            o.m[i][j] = c[i] * r.m[i][j] * c[j];
    return o;
}

inline void XrPosToUa(const Vec3f &p, double unitsPerM, double out[3])
{
    out[0] = p.x * unitsPerM;
    out[1] = -p.y * unitsPerM;
    out[2] = -p.z * unitsPerM;
}

// Asymmetric projection in the engine's convention (UAV in, GL clip out):
// same depth mapping as mat4x4::UAFrustum (z forward, w = z), y flipped
// because UAV +Y is down while the OpenXR angleUp is towards screen-up.
inline M4 UaAsymFrustum(const Fovf &f, double zn, double zf)
{
    const double l = tan(f.angleLeft), r = tan(f.angleRight);
    const double u = tan(f.angleUp), d = tan(f.angleDown);
    M4 p;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) p.m[i][j] = 0;
    p.m[0][0] = 2.0 / (r - l);
    p.m[0][2] = -(r + l) / (r - l);
    p.m[1][1] = -2.0 / (u - d);
    p.m[1][2] = -(u + d) / (u - d);
    p.m[2][2] = (zf + zn) / (zf - zn);
    p.m[2][3] = -2.0 * zf * zn / (zf - zn);
    p.m[3][2] = 1.0;
    return p;
}

} // namespace VR

#endif
