// pure layout math for the movable HUD panels
// (PANELS: FREE). No engine / GL dependencies, so tools/vr/test_vr_panels.cpp can test it.
// Tracking space, metres, XR axes (+x right, +y up, -z ahead). A panel quad faces +n.
#ifndef UA_VR_PANELS_LOGIC_H_INCLUDED
#define UA_VR_PANELS_LOGIC_H_INCLUDED

#include <math.h>
#include <algorithm>

namespace VR
{
namespace PanelLogic
{

const double kMinW = 0.40, kMaxW = 3.20;     // panel width clamp (m)
const double kMinDist = 0.45, kMaxDist = 4.0; // centre distance from the head (m)

struct Quad
{
    double c[3] = {0, 0, 0};
    double yaw = 0, pitch = 0; // R = RotY(yaw) * RotX(pitch); yaw 0 / pitch 0 = facing +z
    double w = 1, h = 1;
};

inline void Axes(double yaw, double pitch, double ex[3], double ey[3], double n[3])
{
    const double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    ex[0] = cy;      ex[1] = 0;   ex[2] = -sy;
    ey[0] = sp * sy; ey[1] = cp;  ey[2] = sp * cy;
    n[0] = cp * sy;  n[1] = -sp;  n[2] = cp * cy;
}

// yaw / pitch that turn the quad at c towards the eye point (a side panel becomes a wall)
inline void FaceHead(const double c[3], const double head[3], double *yaw, double *pitch)
{
    double v[3] = {head[0] - c[0], head[1] - c[1], head[2] - c[2]};
    const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 1e-6)
        return;
    for (int i = 0; i < 3; i++)
        v[i] /= l;
    *yaw = atan2(v[0], v[2]);
    *pitch = -asin(std::max(-1.0, std::min(1.0, v[1])));
}

// The two default quads cut from the whole panel (centre c0, axes from yaw / pitch,
// width w, height h): rows 0..split on top, split..1 below, in the same plane, so FREE
// before any move looks like FIXED.
inline void SplitDefault(const Quad &whole, double split, Quad *top, Quad *bot)
{
    double ex[3], ey[3], n[3];
    Axes(whole.yaw, whole.pitch, ex, ey, n);
    *top = whole;
    *bot = whole;
    top->h = whole.h * split;
    bot->h = whole.h * (1.0 - split);
    const double ut = whole.h * 0.5 - top->h * 0.5, ub = whole.h * 0.5 - bot->h * 0.5;
    for (int i = 0; i < 3; i++)
    {
        top->c[i] = whole.c[i] + ey[i] * ut;
        bot->c[i] = whole.c[i] - ey[i] * ub;
    }
}

// Ray vs the quad's plane: local x (along ex), y (along ey) and ray distance t.
inline bool RayLocal(const Quad &q, const double o[3], const double d[3], double *x, double *y, double *t)
{
    double ex[3], ey[3], n[3];
    Axes(q.yaw, q.pitch, ex, ey, n);
    const double dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (fabs(dn) < 1e-6)
        return false;
    const double tt = ((q.c[0] - o[0]) * n[0] + (q.c[1] - o[1]) * n[1] + (q.c[2] - o[2]) * n[2]) / dn;
    if (tt <= 0)
        return false;
    double r[3];
    for (int i = 0; i < 3; i++)
        r[i] = o[i] + d[i] * tt - q.c[i];
    *x = r[0] * ex[0] + r[1] * ex[1] + r[2] * ex[2];
    *y = r[0] * ey[0] + r[1] * ey[1] + r[2] * ey[2];
    *t = tt;
    return true;
}

// Grab zones on the panel's OUTER edge (outer = +1: the top edge of the top panel,
// -1: the bottom edge of the bottom panel). The inner edges sit near the line of sight
// and the crosshair, so they are not handles. band = the grab bar strip outside the edge.
enum Zone { Z_NONE = 0, Z_MOVE, Z_RESIZE };
struct Hit
{
    Zone zone = Z_NONE;
    double x = 0, y = 0, t = 0;
    int side = 0; // resize: -1 left corner, +1 right corner
};
inline Hit Zones(const Quad &q, int outer, const double o[3], const double d[3], double band)
{
    Hit h;
    if (!RayLocal(q, o, d, &h.x, &h.y, &h.t))
        return h;
    const double ye = outer * h.y, hw = q.w * 0.5, hh = q.h * 0.5;
    const double inside = band * 0.35, corner = band * 1.4, pad = band * 0.4;
    if (ye < hh - inside || ye > hh + band || fabs(h.x) > hw + pad)
        return h;
    if (fabs(h.x) >= hw - corner)
    {
        h.zone = Z_RESIZE;
        h.side = h.x < 0 ? -1 : 1;
    }
    else
        h.zone = Z_MOVE;
    return h;
}

// Move: the grab point (local gx, gy) stays on the ray at distance dist; the quad turns
// to face the head. Two passes settle the turn against the offset.
inline void Move(Quad &q, const double o[3], const double d[3], double dist, double gx, double gy, const double head[3])
{
    const double p[3] = {o[0] + d[0] * dist, o[1] + d[1] * dist, o[2] + d[2] * dist};
    for (int pass = 0; pass < 3; pass++)
    {
        double ex[3], ey[3], n[3];
        Axes(q.yaw, q.pitch, ex, ey, n);
        for (int i = 0; i < 3; i++)
            q.c[i] = p[i] - ex[i] * gx - ey[i] * gy;
        // keep the centre within reach of the head
        double v[3] = {q.c[0] - head[0], q.c[1] - head[1], q.c[2] - head[2]};
        const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6 && (l < kMinDist || l > kMaxDist))
        {
            const double k = std::max(kMinDist, std::min(kMaxDist, l)) / l;
            for (int i = 0; i < 3; i++)
                q.c[i] = head[i] + v[i] * k;
        }
        FaceHead(q.c, head, &q.yaw, &q.pitch);
    }
}

// Resize from a corner, aspect kept: the opposite corner stays put. q0 = the quad when
// grabbed, (lx, ly) = local ray hit on q0's plane, side / outer = the grabbed corner.
inline void Resize(Quad &q, const Quad &q0, int side, int outer, double lx, double ly)
{
    double ex[3], ey[3], n[3];
    Axes(q0.yaw, q0.pitch, ex, ey, n);
    const double ax = -side * q0.w * 0.5, ay = -outer * q0.h * 0.5; // anchor (opposite corner), local
    const double dx = side * q0.w, dy = outer * q0.h;               // anchor -> grabbed corner
    const double s0 = ((lx - ax) * dx + (ly - ay) * dy) / std::max(1e-9, dx * dx + dy * dy);
    const double w = std::max(kMinW, std::min(kMaxW, q0.w * s0));
    const double s = w / q0.w;
    const double cx = ax + dx * s * 0.5, cy = ay + dy * s * 0.5;
    q = q0;
    q.w = w;
    q.h = q0.h * s;
    for (int i = 0; i < 3; i++)
        q.c[i] = q0.c[i] + ex[i] * cx + ey[i] * cy;
}

} // namespace PanelLogic
} // namespace VR

#endif // UA_VR_PANELS_LOGIC_H_INCLUDED
