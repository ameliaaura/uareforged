// Minimal vector / quaternion / matrix maths. Column-major 4x4 (OpenGL convention).
#pragma once

#include <cmath>

namespace uavr {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Quat quatMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat quatFromYaw(float yaw) {  // rotation about +Y
    return {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
}
inline Vec3 quatRotate(Quat q, Vec3 v) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    Vec3 c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

struct Pose {
    Quat orientation;
    Vec3 position;
};
// Compose: result = parent * child (child expressed in parent's frame).
inline Pose poseMul(const Pose& parent, const Pose& child) {
    return {quatMul(parent.orientation, child.orientation),
            parent.position + quatRotate(parent.orientation, child.position)};
}

struct Mat4 {
    float m[16];  // column-major
    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

// Translation * rotation * scale.
inline Mat4 mat4FromTRS(Vec3 t, Quat q, Vec3 s) {
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    const float yz = q.y * z2, wx = q.w * x2, xy = q.x * y2;
    const float wz = q.w * z2, xz = q.x * z2, wy = q.w * y2;
    Mat4 r{};
    r.m[0] = (1 - (yy + zz)) * s.x; r.m[1] = (xy + wz) * s.x;       r.m[2] = (xz - wy) * s.x;
    r.m[4] = (xy - wz) * s.y;       r.m[5] = (1 - (xx + zz)) * s.y; r.m[6] = (yz + wx) * s.y;
    r.m[8] = (xz + wy) * s.z;       r.m[9] = (yz - wx) * s.z;       r.m[10] = (1 - (xx + yy)) * s.z;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; r.m[15] = 1;
    return r;
}

// Inverse of a rigid transform (rotation + translation only).
inline Mat4 mat4InverseRigid(const Pose& p) {
    Quat inv{-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
    Vec3 t = quatRotate(inv, p.position) * -1.f;
    return mat4FromTRS(t, inv, {1, 1, 1});
}

// OpenGL clip space (z in [-1,1]) projection from asymmetric FOV angles (radians).
inline Mat4 mat4FromFov(float angleLeft, float angleRight, float angleUp, float angleDown,
                        float nearZ, float farZ) {
    const float l = std::tan(angleLeft), r = std::tan(angleRight);
    const float u = std::tan(angleUp), d = std::tan(angleDown);
    const float w = r - l, h = u - d;
    Mat4 p{};
    p.m[0] = 2 / w;
    p.m[5] = 2 / h;
    p.m[8] = (r + l) / w;
    p.m[9] = (u + d) / h;
    p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m[11] = -1;
    p.m[14] = -2 * farZ * nearZ / (farZ - nearZ);
    return p;
}

inline Vec3 hsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6.f), f = h * 6.f - i;
    const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

}  // namespace uavr
