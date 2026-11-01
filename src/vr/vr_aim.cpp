// Urban Assault VR —: world-space crosshair, laser pointer, controls card.
// Design and verification:
//
// Crosshair. The flat game draws its gun reticles into the 2D HUD at the
// screen position of the gun direction under the flat 90-degree camera. In VR
// the whole 2D canvas is a panel ~62 x 37 degrees wide, 1.5 m ahead and 0.2 m
// low, so those reticles land compressed towards the panel centre and ~7.6
// degrees low (ground units aim with _gun_angle_user, flyers straight ahead,
// which is why the helicopter looked right and tanks did not). Here the
// reticle is placed in the world instead: each unit's User_layer reports the
// exact direction and fire offset it hands to LaunchMissile/FireMinigun
// (NoteUserAim), the gun ray is ray-cast against sectors/buildings
// (ypaworld_func136, marched in short steps) and the unit UserTargeting
// locked, and a camera-facing reticle with constant angular size is drawn at
// the hit point in both eyes, without depth test, so the eyes converge on the
// target. Independent of UA_VR_UNITS_PER_M: everything is in world units and
// projected through the same per-eye matrices as the world.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#include "vr_aim.h"
#include "vr_gl.h"
#include "vr_overlay.h"
#include "../system/gfx.h"
#include "../yw.h"
#include "../ypabact.h"
#include "../yparobo.h"
#include "../ypagun.h"

namespace VR
{
bool Active();
double UnitsPerMetre();

namespace Aim
{

namespace
{

struct AimState
{
    bool valid = false;
    NC_STACK_ypabact *unit = NULL;
    vec3d origin, dir, point;
    double dist = 0;
    bool hit = false, locked = false;
    uint32_t frame = 0;
};

AimState g_aim;
uint32_t g_frame = 0;
TargetSnap g_target; // cockpit readout

// centre camera (UAV rows with cull scale; world position)
M3 g_camRows = M3::Ident();
double g_camPos[3] = {0, 0, 0};

// pointer
ControllerState g_right;
bool g_panelValid = false;
PanelLayer g_panel;
bool g_panelSplit = false; // PANELS FREE: g_panel = top quad, g_panel2 = bottom quad
PanelLayer g_panel2;
int g_panelW = 1280, g_panelH = 720;
bool g_ptrHit = false;
double g_ptrU = 0, g_ptrV = 0; // 0..1, origin top-left
double g_ptrHitXr[3] = {0, 0, 0};
int g_uiMode = 0;
uint32_t g_uiFrame = 0;
const int kMaxRects = 24;
int g_uiRects[kMaxRects][4];
int g_uiRectN = 0;

// controls card
uint32_t g_cardUntil = 0;
bool g_cardAuto = true; // first-entry card on (profile;#!vr_card)
int g_cardKind = 0;
// VR MODE (0 classic / 1 command deck) and the extras (1 = follows the mode)
#ifdef UA_VR_PREVIEW
const int kModeDefault = 1;
#else
const int kModeDefault = 0;
#endif
int g_mode = kModeDefault;
int g_extra[MODE_EXTRA_COUNT] = {-1, -1, -1, -1, -1, -1};
int g_lastKind = -1;
bool g_kindSeen[3] = {false, false, false};

// options
double g_range = 2400.0;    // world units (UserTargeting 2000, gun AI 2400)
double g_reticleDeg = 1.1;  // reticle radius in degrees
bool g_log = false;

// verification log (UA_VR_AIM_LOG=1)
struct ShotLog
{
    const void *key = NULL;
    vec3d reticle, camPos, spawn, dir;
    uint32_t t = 0;
};
ShotLog g_shots[16];
int g_shotNext = 0;

double Len(const vec3d &v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

M3 PoseRot(const Posef &p) { return QuatToM3(p.orientation); }

void ToEye(const ViewState &vs, int eye, const double xr[3], double outUa[3])
{
    // XR tracking point -> eye local (XR axes) -> UAV axes (metres; projection is scale free)
    const M3 rh = PoseRot(vs.head);
    const M3 re = rh * PoseRot(vs.eye[eye].pose);
    const double ep[3] = {vs.eye[eye].pose.position.x, vs.eye[eye].pose.position.y, vs.eye[eye].pose.position.z};
    double off[3];
    rh.Apply(ep, off);
    const double p[3] = {xr[0] - (vs.head.position.x + off[0]), xr[1] - (vs.head.position.y + off[1]), xr[2] - (vs.head.position.z + off[2])};
    double l[3];
    re.T().Apply(p, l);
    outUa[0] = l[0];
    outUa[1] = -l[1];
    outUa[2] = -l[2];
}

bool Clip(const M4 &proj, const double e[3], float out[4])
{
    double c[4];
    for (int i = 0; i < 4; i++)
        c[i] = proj.m[i][0] * e[0] + proj.m[i][1] * e[1] + proj.m[i][2] * e[2] + proj.m[i][3];
    if (c[3] <= 1e-6)
        return false;
    out[0] = (float)c[0];
    out[1] = (float)c[1];
    out[2] = 0.0f; // always inside the depth range; no depth test anyway
    out[3] = (float)c[3];
    return true;
}

// Camera-facing square of angular radius `deg` centred at eye-space point e.
bool Billboard(const M4 &proj, const double e[3], double deg, float q[4][4])
{
    const double d = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (e[2] <= 1e-3 || d <= 1e-6)
        return false;
    // axes perpendicular to the line of sight (so the size is angular everywhere in the view)
    double f[3] = {e[0] / d, e[1] / d, e[2] / d};
    double up[3] = {0, 1, 0}; // UAV +Y = down on screen
    double rx[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    double rl = sqrt(rx[0] * rx[0] + rx[1] * rx[1] + rx[2] * rx[2]);
    for (int i = 0; i < 3; i++) rx[i] /= rl;
    double ry[3] = {f[1] * rx[2] - f[2] * rx[1], f[2] * rx[0] - f[0] * rx[2], f[0] * rx[1] - f[1] * rx[0]};
    // rx should point screen-right, ry screen-down
    if (rx[0] < 0) for (int i = 0; i < 3; i++) rx[i] = -rx[i];
    if (ry[1] < 0) for (int i = 0; i < 3; i++) ry[i] = -ry[i];
    const double r = d * tan(deg * M_PI / 180.0);
    static const double s[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}}; // uv (0,0) bottom-left
    for (int k = 0; k < 4; k++)
    {
        double p[3];
        for (int i = 0; i < 3; i++)
            p[i] = e[i] + r * (s[k][0] * rx[i] + s[k][1] * ry[i]);
        if (!Clip(proj, p, q[k]))
            return false;
    }
    return true;
}

// Strip between eye-space points a and b with angular half-width `w` radians.
bool Strip(const M4 &proj, const double a[3], const double b[3], double w, float q[4][4])
{
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double m[3] = {(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5};
    double s[3] = {ab[1] * m[2] - ab[2] * m[1], ab[2] * m[0] - ab[0] * m[2], ab[0] * m[1] - ab[1] * m[0]};
    const double sl = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-9)
        return false;
    for (int i = 0; i < 3; i++) s[i] /= sl;
    const double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * w;
    const double lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]) * w;
    double p[4][3];
    for (int i = 0; i < 3; i++)
    {
        p[0][i] = a[i] - s[i] * la;
        p[1][i] = a[i] + s[i] * la;
        p[2][i] = b[i] + s[i] * lb;
        p[3][i] = b[i] - s[i] * lb;
    }
    for (int k = 0; k < 4; k++)
        if (!Clip(proj, p[k], q[k]))
            return false;
    return true;
}

GLuint g_cursorTex = 0;
const void *g_cursorSurf = NULL;

// dev aid (simulator verification): UA_VR_SIM_PTR="f0-f1:x,y;..." turns the
// right-hand ray towards panel pixel x,y (origin top-left) over controller polls
// f0..f1, so scripted runs can click exact widgets (Settings rows, tool bar).
struct PtrKey { int f0 = 0, f1 = 0; double x = 0, y = 0; };
std::vector<PtrKey> g_ptrScript;

} // namespace

void Init()
{
    if (const char *v = getenv("UA_VR_AIM_RANGE")) g_range = atof(v);
    if (const char *v = getenv("UA_VR_RETICLE_DEG")) g_reticleDeg = atof(v);
    const char *l = getenv("UA_VR_AIM_LOG");
    g_log = l && l[0] && strcmp(l, "0") != 0;
    if (const char *v = getenv("UA_VR_SIM_PTR"))
    {
        std::string sc(v);
        size_t at = 0;
        while (at < sc.size())
        {
            size_t e = sc.find(';', at);
            const std::string seg = sc.substr(at, e == std::string::npos ? std::string::npos : e - at);
            PtrKey k;
            if (sscanf(seg.c_str(), "%d-%d:%lf,%lf", &k.f0, &k.f1, &k.x, &k.y) == 4)
                g_ptrScript.push_back(k);
            if (e == std::string::npos)
                break;
            at = e + 1;
        }
    }
}

void FrameStart(const ControllerState ctrl[2])
{
    g_frame++;
    g_right = ctrl[1];
    g_ptrHit = false;
    if (!g_panelValid || !g_right.active)
        return;

    const M3 r = QuatToM3(g_right.aimPose.orientation);
    const double o[3] = {g_right.aimPose.position.x, g_right.aimPose.position.y, g_right.aimPose.position.z};
    double d[3] = {-r.m[0][2], -r.m[1][2], -r.m[2][2]}; // aim forward = -Z
    for (const PtrKey &k : g_ptrScript)
    {
        if (g_frame < (uint64_t)k.f0 || g_frame > (uint64_t)k.f1 || g_panelW <= 0 || g_panelH <= 0)
            continue;
        // panel pixel -> panel-local metres -> tracking space; aim the ray at it
        const double gv = k.y / g_panelH;
        const PanelLayer &P = (g_panelSplit && gv >= g_panel2.v0) ? g_panel2 : g_panel; // the quad showing that row
        const double lv = (gv - P.v0) / std::max(1e-6, (double)(P.v1 - P.v0));
        const M3 pr = QuatToM3(P.pose.orientation);
        const double c[3] = {P.pose.position.x, P.pose.position.y, P.pose.position.z};
        const double pl[3] = {(k.x / g_panelW - 0.5) * P.widthM, (0.5 - lv) * P.heightM, 0};
        double pw[3];
        pr.Apply(pl, pw);
        double len = 0;
        for (int i = 0; i < 3; i++)
        {
            d[i] = c[i] + pw[i] - o[i];
            len += d[i] * d[i];
        }
        len = sqrt(len);
        for (int i = 0; i < 3; i++)
            d[i] /= len;
    }
    // with PANELS FREE the 2D panel is two quads; the nearer hit wins. One quad
    // (v0 0, v1 1) is exactly the test.
    double bestT = 1e30;
    for (int q = 0; q < (g_panelSplit ? 2 : 1); q++)
    {
        const PanelLayer &P = q ? g_panel2 : g_panel;
        const M3 pr = QuatToM3(P.pose.orientation);
        const double c[3] = {P.pose.position.x, P.pose.position.y, P.pose.position.z};
        const double n[3] = {pr.m[0][2], pr.m[1][2], pr.m[2][2]};
        const double dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
        if (fabs(dn) < 1e-6)
            continue;
        const double t = ((c[0] - o[0]) * n[0] + (c[1] - o[1]) * n[1] + (c[2] - o[2]) * n[2]) / dn;
        if (t <= 0 || t >= bestT)
            continue;
        double h[3], hl[3];
        for (int i = 0; i < 3; i++)
            h[i] = o[i] + d[i] * t;
        const double rel[3] = {h[0] - c[0], h[1] - c[1], h[2] - c[2]};
        pr.T().Apply(rel, hl);
        const double u = hl[0] / P.widthM + 0.5;
        const double vl = 0.5 - hl[1] / P.heightM;
        if (u < 0 || u > 1 || vl < 0 || vl > 1)
            continue;
        bestT = t;
        g_ptrHit = true;
        g_ptrU = u;
        g_ptrV = g_panelSplit ? P.v0 + vl * (P.v1 - P.v0) : vl;
        memcpy(g_ptrHitXr, h, sizeof(h));
    }
}

static bool PointerOn()
{
    if (!g_ptrHit)
        return false;
    if (g_panel.mode == PANEL_THEATRE)
        return true;
    if (g_frame - g_uiFrame > 2 || g_uiMode == 0)
        return false;
    if (g_uiMode == 1)
        return true;
    const Common::Point p = GFX::Engine.ConvertPosTo2DStuff(Common::Point((int)(g_ptrU * g_panelW), (int)(g_ptrV * g_panelH)));
    for (int i = 0; i < g_uiRectN; i++)
        if (p.x >= g_uiRects[i][0] && p.y >= g_uiRects[i][1] && p.x < g_uiRects[i][0] + g_uiRects[i][2] && p.y < g_uiRects[i][1] + g_uiRects[i][3])
            return true;
    return false;
}

void SetCentreCamera(const M3 &rows, const double pos[3])
{
    g_camRows = rows;
    memcpy(g_camPos, pos, sizeof(g_camPos));
}

void DrawEye(int eye, const ViewState &views, const M4 &eyeFromCentre, double zn, double zf, bool noReticle)
{
    const bool reticle = !noReticle && g_aim.valid && g_frame - g_aim.frame <= 2;
    const bool beam = PointerOn() && g_panel.mode == PANEL_COCKPIT;
    if (!reticle && !beam)
        return;
    if (!Overlay::Begin())
        return;
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);

    if (reticle)
    {
        const double w[3] = {g_aim.point.x - g_camPos[0], g_aim.point.y - g_camPos[1], g_aim.point.z - g_camPos[2]};
        double c[3], e[3];
        g_camRows.Apply(w, c);
        for (int i = 0; i < 3; i++)
            e[i] = eyeFromCentre.m[i][0] * c[0] + eyeFromCentre.m[i][1] * c[1] + eyeFromCentre.m[i][2] * c[2] + eyeFromCentre.m[i][3];
        float q[4][4];
        if (Billboard(proj, e, g_reticleDeg, q))
        {
            // player colour (Settings, default green). Locked stays orange-red,
            // or white when the player colour is red (still distinct).
            const int xc = CrosshairColour();
            float rgb[3];
            CrosshairRGB(xc, rgb);
            if (g_aim.locked)
            {
                if (xc == XHAIR_RED)
                    Overlay::Quad(q, Overlay::STYLE_RETICLE, 1.0f, 1.0f, 1.0f, 0.95f);
                else
                    Overlay::Quad(q, Overlay::STYLE_RETICLE, 1.0f, 0.35f, 0.2f, 0.95f);
            }
            else
                Overlay::Quad(q, Overlay::STYLE_RETICLE, rgb[0], rgb[1], rgb[2], g_aim.hit ? 0.9f : 0.6f);
        }
    }

    if (beam)
    {
        const double a[3] = {g_right.aimPose.position.x, g_right.aimPose.position.y, g_right.aimPose.position.z};
        double ea[3], eb[3];
        ToEye(views, eye, a, ea);
        ToEye(views, eye, g_ptrHitXr, eb);
        float q[4][4];
        if (Strip(proj, ea, eb, 0.0025, q))
            Overlay::Quad(q, Overlay::STYLE_BEAM, 0.55f, 0.9f, 1.0f, 0.7f);
    }
    Overlay::End();
}

void DrawPanel(int panelW, int panelH)
{
    if (!PointerOn())
        return;
    if (!Overlay::Begin())
        return;
    const double px = g_ptrU * panelW, py = g_ptrV * panelH;
    const double sc = std::max(1.0, panelH / 400.0);
    auto ndcQuad = [&](double x0, double y0, double x1, double y1, float q[4][4])
    {
        // pixel rect (top-left origin) -> clip; uv (0,0) at bottom-left
        const double X0 = x0 / panelW * 2 - 1, X1 = x1 / panelW * 2 - 1;
        const double Y0 = 1 - y1 / panelH * 2, Y1 = 1 - y0 / panelH * 2;
        const double v[4][2] = {{X0, Y0}, {X1, Y0}, {X1, Y1}, {X0, Y1}};
        for (int k = 0; k < 4; k++)
        {
            q[k][0] = (float)v[k][0];
            q[k][1] = (float)v[k][1];
            q[k][2] = 0;
            q[k][3] = 1;
        }
    };
    float q[4][4];
    const GFX::GFXEngine::CursorImage *ci = GFX::Engine.CurrentCursorImage();
    if (ci && ci->surf)
    {
        SDL_Surface *s = ci->surf;
        if (!g_cursorTex)
            glGenTextures(1, &g_cursorTex);
        glBindTexture(GL_TEXTURE_2D, g_cursorTex);
        if (g_cursorSurf != s)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, s->pitch / 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            g_cursorSurf = s;
        }
        const double x0 = px - ci->hotX * sc, y0 = py - ci->hotY * sc;
        ndcQuad(x0, y0, x0 + s->w * sc, y0 + s->h * sc, q);
        Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, 1, g_cursorTex);
    }
    else
    {
        const double r = 7 * sc;
        ndcQuad(px - r, py - r, px + r, py + r, q);
        Overlay::Quad(q, Overlay::STYLE_DOT, 0.6f, 0.95f, 1.0f, 0.95f);
    }
    Overlay::End();
}

void NotePanel(const PanelLayer &panel, int panelW, int panelH, const PanelLayer *panel2)
{
    g_panel = panel;
    g_panelSplit = panel2 != NULL;
    if (panel2)
        g_panel2 = *panel2;
    g_panelW = panelW;
    g_panelH = panelH;
    g_panelValid = true;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!yw || !yw->_userUnit)
        return;

    // Dev aid UA_VR_TAKE_GUN=<n>: after n gameplay frames sit in the first host
    // station gun (unattended reticle check on the station turrets).
    static int takeGun = -2, frames = 0;
    if (takeGun == -2)
        takeGun = getenv("UA_VR_TAKE_GUN") ? atoi(getenv("UA_VR_TAKE_GUN")) : -1;
    if (takeGun >= 0 && ++frames >= takeGun && yw->_userRobo)
    {
        NC_STACK_yparobo *robo = dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo);
        NC_STACK_ypabact *gun = NULL;
        if (robo)
            for (World::TRoboGun &g : robo->_roboGuns)
                if (g.gun_obj && !gun)
                    gun = g.gun_obj;
        if (gun && yw->_viewerBact)
        {
            yw->_viewerBact->setBACT_viewer(false);
            yw->_viewerBact->setBACT_inputting(false);
            gun->setBACT_viewer(true);
            gun->setBACT_inputting(true);
            fprintf(stderr, "UAVR: took control of host station gun gid %d\n", (int)gun->_gid);
            takeGun = -1;
        }
    }
    const int t = yw->_userUnit->_bact_type;
    int kind = -1;
    if (t == BACT_TYPES_TANK || t == BACT_TYPES_CAR || t == BACT_TYPES_HOVER)
        kind = 0;
    else if (t == BACT_TYPES_BACT || t == BACT_TYPES_FLYER || t == BACT_TYPES_UFO)
        kind = 1;
    else if (t == BACT_TYPES_ROBO || t == BACT_TYPES_GUN)
        kind = 2;
    if (kind < 0)
        return;
    g_cardKind = kind;
    if (kind != g_lastKind)
    {
        g_lastKind = kind;
        if (!g_kindSeen[kind] || getenv("UA_VR_CARD_ALWAYS"))
        {
            g_kindSeen[kind] = true;
            if (g_cardAuto || getenv("UA_VR_CARD_ALWAYS")) // Settings "CONTROLS CARD"
                g_cardUntil = SDL_GetTicks() + 8000;
        }
    }
}

} // namespace Aim

// ---------------------------------------------------------------------------
// Public entry points (vr.h)

void NoteUserAim(NC_STACK_ypaworld *yw, NC_STACK_ypabact *u, double dx, double dy, double dz, double fx, double fy, double fz)
{
    if (!Active() || !u || !yw)
        return;
    using namespace Aim;
    vec3d dir(dx, dy, dz);
    if (dir.x == 0.0 && dir.y == 0.0 && dir.z == 0.0)
        dir = u->_rotation.AxisZ(); // LaunchMissile: zero direction = the unit's forward axis
    const double l = Len(dir);
    if (l < 1e-6)
        return;
    dir = dir / l;
    // Same spawn transform as LaunchMissile.
    const vec3d origin = u->_position + u->_rotation.Transpose().Transform(vec3d(fx, fy, fz));

    double hitDist = g_range;
    bool hit = false;
    const double step = 250.0; // ypaworld_func136 handles at most 2x2 sub-sectors per call
    for (double s = 0.0; s < g_range; s += step)
    {
        const double seg = std::min(step, g_range - s);
        ypaworld_arg136 a;
        a.stPos = origin + dir * s;
        a.vect = dir * seg;
        a.flags = 0;
        yw->ypaworld_func136(&a);
        if (a.isect)
        {
            const double d = (a.isectPos - origin).dot(dir);
            if (d > 0.0)
            {
                hitDist = d;
                hit = true;
                break;
            }
        }
    }

    // Units on the ray, filtered exactly like ypamissile::TubeCollisionTest for a
    // user-inputting emitter (own units are hit too, a5 = true there).
    int weapFlags = -1;
    if (u->_weapon != -1)
        weapFlags = yw->GetWeaponsProtos().at(u->_weapon)._weaponFlags;
    NC_STACK_ypagun *uGun = (u->_bact_type == BACT_TYPES_GUN) ? dynamic_cast<NC_STACK_ypagun *>(u) : NULL;
    const bool uRoboGun = uGun && uGun->IsRoboGun();
    NC_STACK_ypabact *hitUnit = NULL;
    {
        cellArea *seen[64];
        int ns = 0;
        for (double s = 0.0; s <= hitDist && ns < 64; s += 150.0)
        {
            const vec3d p = origin + dir * s;
            yw_130arg a;
            a.pos_x = p.x;
            a.pos_z = p.z;
            if (!yw->GetSectorInfo(&a) || !a.pcell)
                continue;
            bool dup = false;
            for (int i = 0; i < ns; i++)
                dup |= (seen[i] == a.pcell);
            if (dup)
                continue;
            seen[ns++] = a.pcell;
            for (NC_STACK_ypabact *b : a.pcell->unitsList)
            {
                if (b == u || b->_bact_type == BACT_TYPES_MISSLE || b->_status == BACT_STATUS_DEAD)
                    continue;
                const bool bRoboGun = b->_bact_type == BACT_TYPES_GUN && dynamic_cast<NC_STACK_ypagun *>(b) && dynamic_cast<NC_STACK_ypagun *>(b)->IsRoboGun();
                if (bRoboGun && b->_shield >= 100)
                    continue;
                // station-gun emitter: own host station and its guns are transparent
                if (uRoboGun && b->_owner == u->_owner && (b->_bact_type == BACT_TYPES_ROBO || bRoboGun))
                    continue;
                // bombs (weapon flags 1) ignore units above their start height (UA y is down)
                if (weapFlags == 1 && b->_position.y < origin.y)
                    continue;
                World::rbcolls *cn = b->getBACT_collNodes();
                const int nc = cn ? (int)cn->roboColls.size() : 1;
                for (int j = 0; j < nc; j++)
                {
                    vec3d c = b->_position;
                    double r = b->_radius;
                    if (cn)
                    {
                        c = b->_position + b->_rotation.Transpose().Transform(cn->roboColls[j].coll_pos);
                        r = cn->roboColls[j].robo_coll_radius;
                    }
                    const vec3d oc = c - origin;
                    const double along = oc.dot(dir);
                    if (along <= 0.0)
                        continue;
                    const double perp2 = oc.dot(oc) - along * along;
                    if (perp2 > r * r)
                        continue;
                    const double d = along - sqrt(std::max(0.0, r * r - perp2));
                    if (d > 0.0 && d < hitDist)
                    {
                        hitDist = d;
                        hit = true;
                        hitUnit = b;
                    }
                }
            }
        }
    }

    // Target lock. Only homing weapons (weapon flags 7 -> MISL_TARGETED; LaunchMissile
    // calls SetTarget only for those) bend towards the unit UserTargeting locked: then
    // the reticle sits on that unit. Every other weapon flies the straight launch ray,
    // so the lock only colours the reticle when the ray really hits the locked unit.
    bool locked = false;
    vec3d point = origin + dir * hitDist;
    NC_STACK_ypabact *tgt = yw->_guiVisor.field_18;
    if (tgt && weapFlags == 7)
    {
        point = tgt->_position;
        hitDist = (point - origin).length();
        hit = true;
        locked = true;
    }
    else if (tgt && tgt == hitUnit)
        locked = true;

    g_aim.valid = true;
    g_aim.unit = u;
    g_aim.origin = origin;
    g_aim.dir = dir;
    g_aim.dist = hitDist;
    g_aim.point = point;
    g_aim.hit = hit;
    g_aim.locked = locked;
    g_aim.frame = g_frame;

    // cockpit: target readout (locked unit first, else the unit on the ray).
    TargetSnap ts;
    ts.valid = true;
    ts.unit = u;
    ts.rayHit = hit;
    ts.rayDistUnits = hitDist;
    NC_STACK_ypabact *show = tgt ? tgt : hitUnit;
    if (show)
    {
        ts.any = true;
        ts.locked = (show == tgt);
        ts.vehicleId = show->_vehicleID;
        ts.energy = show->_energy;
        ts.energyMax = std::max(1, show->_energy_max);
        ts.owner = show->_owner;
        ts.distUnits = (show->_position - origin).length();
    }
    g_target = ts;
}

const Aim::TargetSnap &Aim::Target() { return Aim::g_target; }

void NoteUserShot(NC_STACK_ypabact *u, const void *key, double px, double py, double pz, double dx, double dy, double dz)
{
    using namespace Aim;
    if (!Active() || !g_log || !g_aim.valid || u != g_aim.unit)
        return;
    vec3d spawn(px, py, pz), d(dx, dy, dz);
    const double l = Len(d);
    if (l > 0)
        d = d / l;
    // reticle point vs the projectile's initial ray
    const vec3d toR = g_aim.point - spawn;
    const double along = toR.dot(d);
    const vec3d perp = toR - d * along;
    const double dirErr = acos(std::max(-1.0, std::min(1.0, d.dot(g_aim.dir)))) * 180.0 / M_PI;
    // as seen from the camera (what the player sees): angle between eye->reticle and eye->(point on shot ray at same range)
    const vec3d cam(g_camPos[0], g_camPos[1], g_camPos[2]);
    const vec3d onRay = spawn + d * along;
    vec3d a = g_aim.point - cam, b = onRay - cam;
    const double camErr = acos(std::max(-1.0, std::min(1.0, a.dot(b) / (Len(a) * Len(b) + 1e-9)))) * 180.0 / M_PI;
    fprintf(stderr, "UAVR aim: SHOT unit %d dir err %.4f deg | reticle %.1f u ahead (%s%s) | reticle off shot ray %.2f u = %.3f deg from camera\n",
            (int)u->_gid, dirErr, g_aim.dist, g_aim.hit ? "hit" : "no hit", g_aim.locked ? ", locked" : "", Len(perp), camErr);
    ShotLog &g_shot = g_shots[g_shotNext++ % 16];
    g_shot.key = key;
    g_shot.reticle = g_aim.point;
    g_shot.camPos = cam;
    g_shot.spawn = spawn;
    g_shot.dir = d;
    g_shot.t = SDL_GetTicks();
}

void NoteUserImpact(const void *key, double px, double py, double pz)
{
    using namespace Aim;
    if (!Active() || !g_log || !key)
        return;
    ShotLog *found = NULL;
    for (ShotLog &sl : g_shots)
        if (sl.key == key)
            found = &sl;
    if (!found)
        return;
    ShotLog &g_shot = *found;
    g_shot.key = NULL;
    const vec3d imp(px, py, pz);
    const vec3d a = g_shot.reticle - g_shot.camPos, b = imp - g_shot.camPos;
    const double ang = acos(std::max(-1.0, std::min(1.0, a.dot(b) / (Len(a) * Len(b) + 1e-9)))) * 180.0 / M_PI;
    // vertical part of the miss as seen from the camera (drop)
    const double ea = atan2(-a.y, sqrt(a.x * a.x + a.z * a.z)) * 180.0 / M_PI;
    const double eb = atan2(-b.y, sqrt(b.x * b.x + b.z * b.z)) * 180.0 / M_PI;
    fprintf(stderr, "UAVR aim: IMPACT %.0f ms after shot | impact vs reticle %.1f u, %.3f deg from camera (elev reticle %.3f impact %.3f deg) | impact range %.0f u\n",
            (double)(SDL_GetTicks() - g_shot.t), Len(imp - g_shot.reticle), ang, ea, eb, Len(imp - g_shot.spawn));
}

bool HideFlatCrosshair() { return Active(); }

bool GetPointer(int *x, int *y)
{
    using namespace Aim;
    if (!Active() || !PointerOn())
        return false;
    *x = (int)(g_ptrU * g_panelW);
    *y = (int)(g_ptrV * g_panelH);
    return true;
}

bool GetPointerRaw(int *x, int *y, int *mode)
{
    using namespace Aim;
    *mode = (int)g_panel.mode;
    if (!Active() || !g_ptrHit)
        return false;
    *x = (int)(g_ptrU * g_panelW);
    *y = (int)(g_ptrV * g_panelH);
    return true;
}

int UiPointerMode()
{
    using namespace Aim;
    if (!Active() || g_frame - g_uiFrame > 2)
        return 0;
    return g_uiMode;
}

void SetUiPointer(int mode, const int *xywh, int count)
{
    using namespace Aim;
    g_uiMode = mode;
    g_uiFrame = g_frame;
    g_uiRectN = std::min(count, kMaxRects);
    for (int i = 0; i < g_uiRectN; i++)
        for (int k = 0; k < 4; k++)
            g_uiRects[i][k] = xywh[i * 4 + k];
}

bool ControlsCardAuto() { return Aim::g_cardAuto; }
void SetControlsCardAuto(bool on)
{
    Aim::g_cardAuto = on;
    if (!on)
        Aim::g_cardUntil = 0;
}

bool CommandDeck() { return Aim::g_mode != 0; }
void SetCommandDeck(bool on) { Aim::g_mode = on ? 1 : 0; }
static bool ModeExtra(int i) { return Aim::g_extra[i] >= 0 ? Aim::g_extra[i] != 0 : Aim::g_mode != 0; }
bool FieldOrders() { return ModeExtra(0); }
bool WristSquads() { return ModeExtra(1); }
bool SquadWheel() { return ModeExtra(2); }
bool MapWall() { return Aim::g_extra[3] > 0; } // TABLE is the default in every mode; WALL only when chosen
bool DirectionalRadio() { return ModeExtra(4); }
bool QuickStation() { return ModeExtra(5); } //
void SetFieldOrders(bool on) { Aim::g_extra[0] = on ? 1 : 0; }
void SetWristSquads(bool on) { Aim::g_extra[1] = on ? 1 : 0; }
void SetSquadWheel(bool on) { Aim::g_extra[2] = on ? 1 : 0; }
void SetMapWall(bool wall) { Aim::g_extra[3] = wall ? 1 : 0; }
void SetDirectionalRadio(bool on) { Aim::g_extra[4] = on ? 1 : 0; }
void SetQuickStation(bool on) { Aim::g_extra[5] = on ? 1 : 0; }
int ModeExtraRaw(int which) { return (which >= 0 && which < MODE_EXTRA_COUNT) ? Aim::g_extra[which] : -1; }
void SetModeExtraRaw(int which, int v)
{
    if (which >= 0 && which < MODE_EXTRA_COUNT)
        Aim::g_extra[which] = v < 0 ? -1 : (v ? 1 : 0);
}
void ResetModeExtras() { for (int i = 0; i < MODE_EXTRA_COUNT; i++) Aim::g_extra[i] = -1; }
static bool s_tokenTable = false; // hidden ";#!vr_tokentable"
void ResetModeToDefault() { Aim::g_mode = Aim::kModeDefault; ResetModeExtras(); s_tokenTable = false; ResetDioramaPose(); }
bool TokenTable() { return s_tokenTable; }
void SetTokenTable(bool on) { s_tokenTable = on; }

bool ControlsCard(int *kind)
{
    using namespace Aim;
    *kind = g_cardKind;
    return Active() && SDL_GetTicks() < g_cardUntil;
}

} // namespace VR
