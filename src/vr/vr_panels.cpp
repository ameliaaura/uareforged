// HUD panels FIXED / FREE. See vr_panels.h.
// FIXED never reaches this file's per-frame paths except the early returns, so the
// panel quad, pointer mapping and eye passes stay those of.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <algorithm>
#include <ctype.h>
#include <strings.h>

#include "vr_panels.h"
#include "vr_panels_logic.h"
#include "vr_grip.h"
#include "vr_math.h"
#include "vr_overlay.h"
#include "../yw.h"
#include "../yw_internal.h"

namespace VR
{
namespace Panels
{
namespace
{
using PanelLogic::Quad;

bool g_free = false;     // the Settings row (profile ;#!vr_panels)
bool g_forceFree = false; // dev aid UA_VR_PANELS_FREE=1 (Quest bench: the app-owned profile is not editable)
inline bool Free() { return g_free || g_forceFree; } // shown / used; the profile + Settings row keep g_free
bool g_log = false;      // UA_VR_CMD_LOG
double g_split = 0.86;   // 2D row fraction where the bottom panel (tool bar) starts
bool g_splitSeen = false;

struct Slot { bool set = false; Quad q; };
Slot g_layout[2][2];     // [ctx: 0 cockpit, 1 station + table][0 top, 1 bottom]
bool g_ever = false;     // write the vr_panel_ lines (a panel was moved / loaded / reset)
bool g_dirty = false;    // save to the profile at the next gameplay frame

// what the last Present showed
struct Last
{
    bool valid = false;
    int ctx = 0;
    Quad q[2];
    Quad dflt[2];
    double head[3] = {0, 0, 0};
};
Last g_last;

// grab
Grip::PanelLatch g_latch;
struct Drag
{
    bool on = false;
    int panel = 0, ctx = 0;
    PanelLogic::Zone zone = PanelLogic::Z_NONE;
    int side = 0;
    double dist = 0, gx = 0, gy = 0;
    Quad q0;
    uint32_t t = 0;
};
Drag g_drag;
PanelLogic::Hit g_hover;
int g_hoverPanel = -1;
bool g_near[2] = {false, false};
double g_rayO[3] = {0, 0, 0}, g_rayHit[3] = {0, 0, 0};
bool g_rayShow = false;

// dev aid UA_VR_PANEL_AIM="f0-f1:TOP|TOPL|TOPR|BOT|BOTL|BOTR|P:x,y,z;..." (Filter polls)
struct AimKey { int f0 = 0, f1 = 0; std::string what; double p[3] = {0, 0, 0}; };
std::vector<AimKey> g_aim;
int g_poll = 0;
int g_resetAt = -1; // dev aid UA_VR_PANEL_RESET_AT=<poll>: RESET PANELS as the pause row does

const char *kCtxName[2] = {"cockpit", "station"};
const char *kPanelName[2] = {"top", "bottom"};

double Band(double t) { return 0.045 * std::max(0.7, std::min(2.0, t / 1.5)); }

Quad FromLayer(const PanelLayer &p)
{
    Quad q;
    q.c[0] = p.pose.position.x;
    q.c[1] = p.pose.position.y;
    q.c[2] = p.pose.position.z;
    const M3 m = QuatToM3(p.pose.orientation);
    q.yaw = atan2(m.m[0][2], m.m[2][2]);
    q.pitch = -asin(std::max(-1.0, std::min(1.0, (double)m.m[1][2])));
    q.w = p.widthM;
    q.h = p.heightM;
    return q;
}

void ToLayer(const Quad &q, float v0, float v1, PanelLayer *p)
{
    p->mode = PANEL_COCKPIT;
    p->pose.position.x = (float)q.c[0];
    p->pose.position.y = (float)q.c[1];
    p->pose.position.z = (float)q.c[2];
    p->pose.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, q.yaw), QuatFromAxisAngle(1, 0, 0, q.pitch));
    p->widthM = (float)q.w;
    p->heightM = (float)q.h;
    p->v0 = v0;
    p->v1 = v1;
}

void Point(const Quad &q, double lx, double ly, double out[3])
{
    double ex[3], ey[3], n[3];
    PanelLogic::Axes(q.yaw, q.pitch, ex, ey, n);
    for (int i = 0; i < 3; i++)
        out[i] = q.c[i] + ex[i] * lx + ey[i] * ly;
}

// ---- eye helpers (tracking point -> eye UAV metres -> clip), as vr_command.cpp ----
void ToEye(const ViewState &vs, int eye, const double xr[3], double out[3])
{
    const M3 rh = QuatToM3(vs.head.orientation);
    const M3 re = rh * QuatToM3(vs.eye[eye].pose.orientation);
    const double ep[3] = {vs.eye[eye].pose.position.x, vs.eye[eye].pose.position.y, vs.eye[eye].pose.position.z};
    double off[3];
    rh.Apply(ep, off);
    const double p[3] = {xr[0] - (vs.head.position.x + off[0]), xr[1] - (vs.head.position.y + off[1]), xr[2] - (vs.head.position.z + off[2])};
    double l[3];
    re.T().Apply(p, l);
    out[0] = l[0];
    out[1] = -l[1];
    out[2] = -l[2];
}

bool Clip(const M4 &proj, const double e[3], float out[4])
{
    double c[4];
    for (int i = 0; i < 4; i++)
        c[i] = proj.m[i][0] * e[0] + proj.m[i][1] * e[1] + proj.m[i][2] * e[2] + proj.m[i][3];
    if (c[3] <= 1e-4)
        return false;
    out[0] = (float)c[0];
    out[1] = (float)c[1];
    out[2] = 0.0f;
    out[3] = (float)c[3];
    return true;
}

bool Beam(const ViewState &vs, int eye, const M4 &proj, const double axr[3], const double bxr[3], double w, float q[4][4])
{
    double a[3], b[3];
    ToEye(vs, eye, axr, a);
    ToEye(vs, eye, bxr, b);
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double m[3] = {(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5};
    double s[3] = {ab[1] * m[2] - ab[2] * m[1], ab[2] * m[0] - ab[0] * m[2], ab[0] * m[1] - ab[1] * m[0]};
    const double sl = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-9)
        return false;
    for (int i = 0; i < 3; i++) s[i] /= sl;
    const double p[4][3] = {{a[0] - s[0] * w, a[1] - s[1] * w, a[2] - s[2] * w}, {a[0] + s[0] * w, a[1] + s[1] * w, a[2] + s[2] * w},
                            {b[0] + s[0] * w, b[1] + s[1] * w, b[2] + s[2] * w}, {b[0] - s[0] * w, b[1] - s[1] * w, b[2] - s[2] * w}};
    for (int k = 0; k < 4; k++)
        if (!Clip(proj, p[k], q[k]))
            return false;
    return true;
}

bool Dot(const ViewState &vs, int eye, const M4 &proj, const double xr[3], double r, float q[4][4])
{
    double e[3];
    ToEye(vs, eye, xr, e);
    if (e[2] <= 1e-3)
        return false;
    static const double s[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int k = 0; k < 4; k++)
    {
        const double p[3] = {e[0] + s[k][0] * r, e[1] + s[k][1] * r, e[2]};
        if (!Clip(proj, p, q[k]))
            return false;
    }
    return true;
}

void AimAt(ControllerState &R, const double target[3])
{
    double d[3] = {target[0] - R.aimPose.position.x, target[1] - R.aimPose.position.y, target[2] - R.aimPose.position.z};
    const double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-6)
        return;
    for (int i = 0; i < 3; i++)
        d[i] /= l;
    const double p = asin(std::max(-1.0, std::min(1.0, d[1])));
    const double th = atan2(-d[0], -d[2]);
    R.aimPose.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, th), QuatFromAxisAngle(1, 0, 0, p));
}

void ScriptAim(ControllerState &R)
{
    for (const AimKey &k : g_aim)
    {
        if (g_poll < k.f0 || g_poll > k.f1)
            continue;
        double t[3];
        if (k.what == "P")
            memcpy(t, k.p, sizeof(t));
        else
        {
            const int pi = k.what.compare(0, 3, "TOP") == 0 ? 0 : 1;
            const int outer = pi == 0 ? 1 : -1;
            const Quad &q = g_last.q[pi];
            const double ly = outer * (q.h * 0.5 + 0.018);
            double lx = 0;
            if (k.what.size() == 4)
                lx = (k.what[3] == 'R' ? 1 : -1) * (q.w * 0.5 - 0.01);
            Point(q, lx, ly, t);
        }
        AimAt(R, t);
    }
}

void EndDrag(const char *why)
{
    if (!g_drag.on)
        return;
    const Slot &s = g_layout[g_drag.ctx][g_drag.panel];
    fprintf(stderr, "UAVR panels: %s %s panel (%s) %s: centre %.3f,%.3f,%.3f yaw %.1f pitch %.1f width %.2f m\n",
            g_drag.zone == PanelLogic::Z_RESIZE ? "resized" : "moved", kPanelName[g_drag.panel], kCtxName[g_drag.ctx], why,
            s.q.c[0], s.q.c[1], s.q.c[2], s.q.yaw * 180 / M_PI, s.q.pitch * 180 / M_PI, s.q.w);
    g_drag = Drag();
    g_ever = true;
    g_dirty = true;
}

} // namespace

void Init()
{
    g_log = getenv("UA_VR_CMD_LOG") && strcmp(getenv("UA_VR_CMD_LOG"), "0") != 0;
    g_forceFree = getenv("UA_VR_PANELS_FREE") && strcmp(getenv("UA_VR_PANELS_FREE"), "0") != 0;
    if (const char *v = getenv("UA_VR_PANEL_RESET_AT"))
        g_resetAt = atoi(v);
    if (const char *v = getenv("UA_VR_PANEL_AIM"))
    {
        std::string s(v);
        size_t at = 0;
        while (at < s.size())
        {
            size_t e = s.find(';', at);
            const std::string seg = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
            AimKey k;
            char what[16] = {0};
            if (sscanf(seg.c_str(), "%d-%d:%15[A-Z]", &k.f0, &k.f1, what) == 3)
            {
                k.what = what;
                if (k.what == "P")
                {
                    const size_t c = seg.find("P:");
                    if (c == std::string::npos || sscanf(seg.c_str() + c + 2, "%lf,%lf,%lf", &k.p[0], &k.p[1], &k.p[2]) != 3)
                        k.what.clear();
                }
                if (!k.what.empty())
                    g_aim.push_back(k);
            }
            if (e == std::string::npos)
                break;
            at = e + 1;
        }
    }
}

void Apply(FrameEnd *fe, int ctx, const ViewState &views)
{
    static int lastFree = -1;
    if (lastFree != (Free() ? 1 : 0))
    {
        if (lastFree >= 0 || Free())
            fprintf(stderr, "UAVR panels: %s\n", Free() ? "FREE (top + bottom panel, trigger on the outer edge = move, corner = resize)" : "FIXED");
        lastFree = Free() ? 1 : 0;
    }
    if (!Free() || !fe || !fe->worldRendered || fe->panel.mode != PANEL_COCKPIT)
    {
        g_last.valid = false;
        return;
    }
    ctx = ctx ? 1 : 0;
    const Quad whole = FromLayer(fe->panel);
    Quad d[2];
    PanelLogic::SplitDefault(whole, g_split, &d[0], &d[1]);
    for (int p = 0; p < 2; p++)
    {
        g_last.dflt[p] = d[p];
        const Slot &s = g_layout[ctx][p];
        Quad q = d[p];
        if (s.set)
        {
            q = s.q;
            q.h = q.w * d[p].h / std::max(1e-6, d[p].w); // the panel's own pixel aspect
        }
        g_last.q[p] = q;
    }
    if (g_last.ctx != ctx && g_drag.on)
        EndDrag("(place changed)");
    g_last.ctx = ctx;
    g_last.valid = true;
    if (views.valid)
    {
        g_last.head[0] = views.head.position.x;
        g_last.head[1] = views.head.position.y;
        g_last.head[2] = views.head.position.z;
    }
    ToLayer(g_last.q[0], 0.0f, (float)g_split, &fe->panel);
    ToLayer(g_last.q[1], (float)g_split, 1.0f, &fe->panel2);
    fe->split = true;
    static int logged = -1;
    if (g_log && logged != ctx)
    {
        logged = ctx;
        fprintf(stderr, "UAVR panels: %s place, split at row %.3f: top %.2f x %.2f m at %.2f,%.2f,%.2f, bottom %.2f x %.2f m at %.2f,%.2f,%.2f\n",
                kCtxName[ctx], g_split, g_last.q[0].w, g_last.q[0].h, g_last.q[0].c[0], g_last.q[0].c[1], g_last.q[0].c[2],
                g_last.q[1].w, g_last.q[1].h, g_last.q[1].c[0], g_last.q[1].c[1], g_last.q[1].c[2]);
    }
}

bool Filter(ControllerState c[2], bool otherOwnsRight, uint32_t nowMs, int poll)
{
    g_poll = poll;
    if (g_poll == g_resetAt)
        ResetPanels();
    ControllerState &R = c[1];
    g_hover = PanelLogic::Hit();
    g_hoverPanel = -1;
    g_near[0] = g_near[1] = false;
    g_rayShow = false;
    if (!Free() || !g_last.valid || !R.active)
    {
        // the drag ends, but a squeeze the grab owns stays swallowed until it is
        // physically released (no menu click / shot from the same squeeze). an
        // inactive controller is no release; only an active sample below 0.10 is.
        EndDrag("(panels off / lost)");
        const bool own = R.active ? Grip::UpdatePanelLatch(g_latch, R.trigger, false, true) : g_latch.state == 1;
        if (own)
            R.trigger = 0;
        return own;
    }
    ScriptAim(R);
    const M3 m = QuatToM3(R.aimPose.orientation);
    const double o[3] = {R.aimPose.position.x, R.aimPose.position.y, R.aimPose.position.z};
    const double d[3] = {-m.m[0][2], -m.m[1][2], -m.m[2][2]};
    memcpy(g_rayO, o, sizeof(g_rayO));
    for (int p = 0; p < 2; p++)
    {
        const int outer = p == 0 ? 1 : -1;
        double x, y, t;
        if (PanelLogic::RayLocal(g_last.q[p], o, d, &x, &y, &t))
        {
            const double nb = 0.20;
            g_near[p] = fabs(x) < g_last.q[p].w * 0.5 + nb && fabs(y) < g_last.q[p].h * 0.5 + nb;
            const PanelLogic::Hit h = PanelLogic::Zones(g_last.q[p], outer, o, d, Band(t));
            if (h.zone != PanelLogic::Z_NONE && (g_hoverPanel < 0 || h.t < g_hover.t))
            {
                g_hover = h;
                g_hoverPanel = p;
            }
        }
    }
    const bool own = Grip::UpdatePanelLatch(g_latch, R.trigger, g_hoverPanel >= 0, otherOwnsRight);
    if (own && !g_drag.on && g_hoverPanel >= 0)
    {
        g_drag.on = true;
        g_drag.panel = g_hoverPanel;
        g_drag.ctx = g_last.ctx;
        g_drag.zone = g_hover.zone;
        g_drag.side = g_hover.side;
        g_drag.dist = g_hover.t;
        g_drag.gx = g_hover.x;
        g_drag.gy = g_hover.y;
        g_drag.q0 = g_last.q[g_hoverPanel];
        g_drag.t = nowMs;
        fprintf(stderr, "UAVR panels: grab %s panel (%s) by its %s at poll %d\n", kPanelName[g_drag.panel], kCtxName[g_drag.ctx],
                g_drag.zone == PanelLogic::Z_RESIZE ? (g_drag.side < 0 ? "left corner (resize)" : "right corner (resize)") : "edge (move)", g_poll);
    }
    if (own && g_drag.on)
    {
        const int p = g_drag.panel, outer = p == 0 ? 1 : -1;
        Slot &s = g_layout[g_drag.ctx][p];
        Quad q = g_last.q[p];
        if (g_drag.zone == PanelLogic::Z_MOVE)
        {
            const double dt = std::min(0.1, (nowMs - g_drag.t) / 1000.0);
            g_drag.dist = std::max(0.3, std::min(4.5, g_drag.dist + R.stickY * 1.2 * dt)); // stick: push / pull
            PanelLogic::Move(q, o, d, g_drag.dist, g_drag.gx, g_drag.gy, g_last.head);
            R.stickX = R.stickY = 0;
        }
        else
        {
            double lx, ly, t;
            if (PanelLogic::RayLocal(g_drag.q0, o, d, &lx, &ly, &t))
                PanelLogic::Resize(q, g_drag.q0, g_drag.side, outer, lx, ly);
        }
        g_drag.t = nowMs;
        s.set = true;
        s.q = q;
        g_last.q[p] = q;
        double x, y, t;
        if (PanelLogic::RayLocal(q, o, d, &x, &y, &t))
            for (int i = 0; i < 3; i++)
                g_rayHit[i] = o[i] + d[i] * t;
        g_rayShow = true;
    }
    else if (g_drag.on)
        EndDrag("(released)");
    else if (g_hoverPanel >= 0)
    {
        for (int i = 0; i < 3; i++)
            g_rayHit[i] = o[i] + d[i] * g_hover.t;
        g_rayShow = true;
    }
    if (own)
        R.trigger = 0; // the grab owns this squeeze: no weapon, laser click, table or board
    return own;
}

void FilterIdle(ControllerState c[2])
{
    EndDrag("(gameplay input paused)");
    const bool own = c[1].active ? Grip::UpdatePanelLatch(g_latch, c[1].trigger, false, true) : g_latch.state == 1;
    if (own)
        c[1].trigger = 0;
}

void DrawEye(int eye, const ViewState &views, double zn, double zf)
{
    if (!Free() || !g_last.valid || (!g_near[0] && !g_near[1] && !g_drag.on))
        return;
    if (!Overlay::Begin())
        return;
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    float q[4][4];
    for (int p = 0; p < 2; p++)
    {
        const bool dragging = g_drag.on && g_drag.panel == p;
        if (!g_near[p] && !dragging)
            continue;
        const Quad &Q = g_last.q[p];
        const int outer = p == 0 ? 1 : -1;
        const bool hov = g_hoverPanel == p || dragging;
        const PanelLogic::Zone z = dragging ? g_drag.zone : (g_hoverPanel == p ? g_hover.zone : PanelLogic::Z_NONE);
        const int side = dragging ? g_drag.side : g_hover.side;
        // grab bar outside the outer edge (the Quest window bar)
        const double bl = std::min(0.32, Q.w * 0.30), by = outer * (Q.h * 0.5 + 0.022);
        double a[3], b[3];
        Point(Q, -bl * 0.5, by, a);
        Point(Q, bl * 0.5, by, b);
        const bool barHot = z == PanelLogic::Z_MOVE;
        if (Beam(views, eye, proj, a, b, barHot ? 0.0075 : 0.0055, q))
            Overlay::Quad(q, Overlay::STYLE_BEAM, barHot ? 0.55f : 0.92f, barHot ? 0.85f : 0.92f, 1.0f, barHot ? 0.95f : (hov ? 0.75f : 0.45f));
        // corner handles (resize)
        for (int s = -1; s <= 1; s += 2)
        {
            double c[3];
            Point(Q, s * Q.w * 0.5, outer * Q.h * 0.5, c);
            const bool hot = z == PanelLogic::Z_RESIZE && side == s;
            if (Dot(views, eye, proj, c, hot ? 0.016 : 0.011, q))
                Overlay::Quad(q, Overlay::STYLE_DOT, hot ? 0.55f : 0.92f, hot ? 0.85f : 0.92f, 1.0f, hot ? 0.95f : (hov ? 0.7f : 0.4f));
        }
        // thin frame while grabbing / pointing at a handle: shows the panel's extent
        if (hov)
        {
            double k[4][3];
            Point(Q, -Q.w * 0.5, -Q.h * 0.5, k[0]);
            Point(Q, Q.w * 0.5, -Q.h * 0.5, k[1]);
            Point(Q, Q.w * 0.5, Q.h * 0.5, k[2]);
            Point(Q, -Q.w * 0.5, Q.h * 0.5, k[3]);
            for (int e = 0; e < 4; e++)
                if (Beam(views, eye, proj, k[e], k[(e + 1) % 4], 0.0012, q))
                    Overlay::Quad(q, Overlay::STYLE_BEAM, 0.55f, 0.85f, 1.0f, 0.35f);
        }
    }
    if (g_rayShow)
    {
        if (Beam(views, eye, proj, g_rayO, g_rayHit, 0.0015, q))
            Overlay::Quad(q, Overlay::STYLE_BEAM, 0.55f, 0.85f, 1.0f, 0.6f);
        if (Dot(views, eye, proj, g_rayHit, 0.006, q))
            Overlay::Quad(q, Overlay::STYLE_DOT, 0.55f, 0.85f, 1.0f, 0.9f);
    }
    Overlay::End();
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (g_dirty && !g_drag.on && yw && yw->_GameShell)
    {
        g_dirty = false;
        const bool ok = yw_VRPatchProfile(yw->_GameShell->UserName);
        fprintf(stderr, "UAVR panels: layout saved to profile %s (%s)\n", yw->_GameShell->UserName.c_str(), ok ? "ok" : "FAILED");
    }
}

} // namespace Panels

// ---- vr.h -------------------------------------------------------------------------
bool PanelsFree() { return Panels::g_free; }
void SetPanelsFree(bool on)
{
    Panels::g_free = on;
    if (!on)
        Panels::EndDrag("(PANELS FIXED)");
}
void ResetPanels()
{
    using namespace Panels;
    EndDrag("(reset)");
    bool any = false;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 2; p++)
        {
            any = any || g_layout[c][p].set;
            g_layout[c][p] = Slot();
        }
    if (any || g_ever)
    {
        g_ever = true; // write the cleared lines (w 0) so the old layout does not come back
        g_dirty = true;
    }
    fprintf(stderr, "UAVR panels: reset (every panel back to its place)\n");
}

void PanelsProfileReset()
{
    using namespace Panels;
    EndDrag("(profile load)");
    g_free = false;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 2; p++)
            g_layout[c][p] = Slot();
    g_ever = false;
    g_dirty = false;
}

void PanelNoteSplit(int rowPx, int screenH)
{
    if (screenH <= 0 || rowPx <= 0 || rowPx >= screenH)
        return;
    const double f = std::max(0.5, std::min(0.97, rowPx / (double)screenH));
    if (!Panels::g_splitSeen || fabs(f - Panels::g_split) > 1e-9)
    {
        if (Panels::Free() && Panels::g_log) // FIXED / Classic: no new log lines
            fprintf(stderr, "UAVR panels: tool bar panel starts at 2D row %d of %d (%.3f)\n", rowPx, screenH, f);
        Panels::g_split = f;
        Panels::g_splitSeen = true;
    }
}

// profile: ;#!vr_panel_<c|s><t|b>_<x|y|z|yaw|pitch|w>
static const char *kField[6] = {"x", "y", "z", "yaw", "pitch", "w"};
bool PanelProfileKey(const char *key, int v)
{
    using namespace Panels;
    if (strncasecmp(key, "vr_panel_", 9) != 0 || strlen(key) < 13 || key[11] != '_')
        return false;
    const char cc = (char)tolower(key[9]), pc = (char)tolower(key[10]);
    const int c = cc == 'c' ? 0 : cc == 's' ? 1 : -1, p = pc == 't' ? 0 : pc == 'b' ? 1 : -1;
    int f = -1;
    for (int i = 0; i < 6; i++)
        if (!strcasecmp(key + 12, kField[i]))
            f = i;
    if (c < 0 || p < 0 || f < 0)
        return true; // a vr_panel_ key this build does not know: ignore it
    Slot &s = g_layout[c][p];
    switch (f)
    {
    case 0: case 1: case 2: s.q.c[f] = v / 1000.0; break;
    case 3: s.q.yaw = v / 100.0 * M_PI / 180.0; break;
    case 4: s.q.pitch = v / 100.0 * M_PI / 180.0; break;
    default:
        s.q.w = std::max(PanelLogic::kMinW, std::min(PanelLogic::kMaxW, v / 1000.0));
        s.set = v > 0;
        break;
    }
    g_ever = true;
    return true;
}

int PanelProfileLines(char lines[][48], int max)
{
    using namespace Panels;
    if (!g_ever)
        return 0;
    int n = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 2; p++)
        {
            const Slot &s = g_layout[c][p];
            const int val[6] = {(int)lround(s.q.c[0] * 1000), (int)lround(s.q.c[1] * 1000), (int)lround(s.q.c[2] * 1000),
                                (int)lround(s.q.yaw * 18000 / M_PI), (int)lround(s.q.pitch * 18000 / M_PI), s.set ? (int)lround(s.q.w * 1000) : 0};
            for (int f = 0; f < 6 && n < max; f++)
                snprintf(lines[n++], 48, ";#!vr_panel_%c%c_%s = %d", c ? 's' : 'c', p ? 'b' : 't', kField[f], s.set ? val[f] : 0);
        }
    return n;
}

} // namespace VR
