// Urban Assault VR — command layer: host-station snap turn, map board,
// build wheel, dive into units. See vr_command.h and
//
// Data flow (one engine frame):
//   FrameStart  -> Cmd::Filter      controllers: owned inputs consumed, picks on the
//                                   board / in the world (from the last snapshot),
//                                   actions queued, fades driven
//   world update-> CommandWorldHook queued actions executed through the game's own
//                                   user-action code, right before
//                                   yparobo::HandleUserCommands consumes _updateMessage
//   after Process -> Cmd::Gameplay  snapshot (cells, units, energy, build slots) and
//                                   the result log (what the game did with the order)
//   eye pass    -> Cmd::DrawEye     board / wheel / hover marks / fade
//   Present     -> Cmd::DrawPanel   the fade over the 2D panel
//
// Units are referenced by gid between frames (never by pointer), so a unit that
// dies between the pick and the execution is simply not found.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_command.h"
#include "vr_gl.h"
#include "vr_overlay.h"
#include "vr_panels.h"
#include "vr_table.h"
#include "vr_field.h"
#include "vr_field_logic.h"
#include "vr_station.h"
#include "../system/gfx.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"
#include "../utils.h"

extern bzd bzda; // yw_game_ui.cpp: the bottom bar (build list, action mode)

namespace VR
{
bool Active();

namespace Cmd
{

namespace
{

// ---- options ---------------------------------------------------------------
bool g_enabled = true;      // UA_VR_COMMAND=0: aim mapping only
bool g_snapTurn = true;     // UA_VR_STATION_TURN=snap|smooth;: Settings "STATION TURN"
bool g_boardOn = true;      // Settings "MAP BOARD"; off = X is the original 2D map
double g_snapDeg = 30.0;    // UA_VR_SNAP_DEG (env) else the setting below
int g_snapDegSetting = 30;  // Settings "STATION TURN" angle (profile;#!vr_snapdeg)
bool g_log = false;         // UA_VR_CMD_LOG=1
double g_upm = 11.0;
double g_boardM = 0.70;     // UA_VR_BOARD_M: longest board side
double g_boardY = -0.50, g_boardZ = -0.50, g_boardTiltDeg = 10.0;
const uint32_t kFadeOutMs = 120, kFadeInMs = 200, kHoldMs = 600, kBHoldMs = 500;

// ---- snapshot (built after every gameplay frame) ----------------------------
struct SUnit
{
    uint32_t gid = 0;
    double x = 0, y = 0, z = 0, radius = 0;
    uint8_t owner = 0;
    int type = 0;
    uint32_t cmdr = 0;  // squad commander gid (own units)
    bool own = false, isCmdr = false, robo = false, selectable = false, diveable = false;
};

struct SSlot
{
    int proto = 0;
    std::string name;
    int cost = 0;    // raw energy (_updateMessage.energy units)
    int shown = 0;   // as the build list shows it (cost / 100)
};

struct Snap
{
    bool valid = false;
    uint32_t ticks = 0;
    int mapW = 0, mapH = 0;
    double lenX = 1, lenY = 1;
    std::vector<uint8_t> owner, seen, border;
    std::vector<SUnit> units;
    SDL_Color ownerCol[8];
    uint8_t me = 0;
    bool inStation = false, inGun = false, canBuild = false;
    bool userDead = false;  // the player's own vehicle / gun is destroyed (death cam)
    uint32_t viewer = 0, robo = 0, selCmdr = 0, user = 0;
    int energy = 0;
    std::vector<SSlot> slots;
};
Snap g_snap;

// ---- input state ---------------------------------------------------------------
enum Act { ACT_NONE = 0, ACT_SELECT, ACT_ORDER, ACT_DIVE, ACT_HOST, ACT_BUILD, ACT_SNAP };
struct Pending
{
    Act kind = ACT_NONE;
    uint32_t gid = 0;     // select / dive / order-on-unit target
    double x = 0, z = 0;  // order on a sector
    int slot = -1;
    bool add = false;     // build: add to the selected squad (R grip held)
    double snapDeg = 0;
    uint32_t cmdr = 0;    // field order: the squad captured at the pull (0 = the current one)
};
std::vector<Pending> g_queue;

bool g_board = false, g_wheel = false;
bool g_boardHit = false;
double g_boardS = 0, g_boardT = 0;   // hit on the board, map fractions (s: +x, t: rows)
double g_hitXr[3] = {0, 0, 0};
uint32_t g_hoverGid = 0;             // unit under the board pointer
int g_hoverKind = 0;                 // 0 none, 1 select, 2 attack, 3 move, 4 attack sector
uint32_t g_worldGid = 0;             // own unit under the right-hand ray in the world
int g_wheelSlot = -1;
bool g_wheelNeedNeutral = false; // a panel grab had the right stick; wait for a real neutral
uint32_t g_builtFlashUntil = 0;
int g_builtSlot = -1;

// fade
enum Fade { FADE_NONE = 0, FADE_OUT, FADE_WAIT, FADE_IN };
Fade g_fade = FADE_NONE;
uint32_t g_fadeT0 = 0;
Pending g_fadeAction;   // queued when black
bool g_fadePulseA = false;
int g_pulseFrames = 0;
bool g_actionDone = false;
uint32_t g_waitViewer = 0;
int g_waitPolls = 0;

// buttons
struct Hold { bool down = false, owned = false, passed = false, watch = false; uint32_t t0 = 0; };
Hold g_x, g_y, g_b;
bool g_aDown = false, g_trigDown = false, g_trigOnBoard = false, g_snapLatch = false;
uint32_t g_snapEpoch = 0;
int g_poll = 0;

// anchor (tracking -> world)
bool g_anchorValid = false;
M3 g_ra = M3::Ident();
double g_pa[3] = {0, 0, 0};
ControllerState g_right;

// QUICK STATION: the "BACK TO STATION? A" prompt after the player's vehicle died
struct Death
{
    bool on = false;        // prompt shown
    bool autoReq = false;   // the timer ran out: Filter starts the return fade
    uint32_t gid = 0;       // the dead unit this prompt belongs to (one prompt per death)
    uint32_t viewer0 = 0;   // the camera unit when it started (a game switch cancels it)
    int32_t t0 = 0, left = 0; // game ms at the start, ms left
};
Death g_death;
const int32_t kDeathAutoMs = 3000;
int g_killUserAt = -2; // dev aid UA_VR_TEST_KILL_USER=<gameplay frame>

// result log
struct Verify { int kind = 0; uint32_t gid = 0; int energyBefore = 0; size_t squadsBefore = 0; uint32_t viewerBefore = 0; double angBefore = 0; int frames = 0; };
Verify g_verify;

// dev aid: scripted board / world aim (UA_VR_CMD_AIM)
struct AimKey { int f0 = 0, f1 = 0; std::string what; };
std::vector<AimKey> g_aimScript;

// textures
GLuint g_boardTex = 0, g_wheelTex = 0, g_blackTex = 0;
int g_boardTexW = 0, g_boardTexH = 0;
uint32_t g_boardTexTicks = 0, g_wheelTexTicks = 0;
std::vector<uint8_t> g_img;

double Deg(double r) { return r * 180.0 / M_PI; }
double Rad(double d) { return d * M_PI / 180.0; }

// ---- geometry (tracking space, metres, XR axes) ----------------------------------
struct Plane
{
    double c[3], ex[3], ey[3], n[3]; // centre, right, up (on the surface), normal (towards the player)
    double hw = 0, hh = 0;          // half sizes
};

void BoardSize(double *w, double *h)
{
    const double mw = std::max(1, g_snap.mapW), mh = std::max(1, g_snap.mapH);
    if (mw >= mh) { *w = g_boardM; *h = g_boardM * mh / mw; }
    else          { *h = g_boardM; *w = g_boardM * mw / mh; }
}

Plane BoardPlane()
{
    Plane p;
    const double t = Rad(g_boardTiltDeg);
    p.c[0] = 0; p.c[1] = g_boardY; p.c[2] = g_boardZ;
    p.ex[0] = 1; p.ex[1] = 0; p.ex[2] = 0;
    p.ey[0] = 0; p.ey[1] = sin(t); p.ey[2] = -cos(t);   // towards the far edge (map row 0)
    p.n[0] = 0; p.n[1] = cos(t); p.n[2] = sin(t);
    double w, h;
    BoardSize(&w, &h);
    p.hw = w * 0.5;
    p.hh = h * 0.5;
    return p;
}

Plane WheelPlane()
{
    Plane p;
    p.c[0] = 0; p.c[1] = -0.40; p.c[2] = -0.45;
    const double l = sqrt(p.c[1] * p.c[1] + p.c[2] * p.c[2]);
    p.n[0] = 0; p.n[1] = -p.c[1] / l; p.n[2] = -p.c[2] / l;   // faces the eye point
    p.ex[0] = 1; p.ex[1] = 0; p.ex[2] = 0;
    p.ey[0] = 0; p.ey[1] = p.n[2]; p.ey[2] = -p.n[1];          // n x ex, up on the wheel
    p.hw = p.hh = 0.17;
    return p;
}

void PlanePoint(const Plane &p, double u, double v, double out[3]) // u,v in -1..1
{
    for (int i = 0; i < 3; i++)
        out[i] = p.c[i] + p.ex[i] * u * p.hw + p.ey[i] * v * p.hh;
}

bool RayPlane(const Plane &p, const double o[3], const double d[3], double *u, double *v, double hit[3])
{
    const double dn = d[0] * p.n[0] + d[1] * p.n[1] + d[2] * p.n[2];
    if (fabs(dn) < 1e-6)
        return false;
    const double t = ((p.c[0] - o[0]) * p.n[0] + (p.c[1] - o[1]) * p.n[1] + (p.c[2] - o[2]) * p.n[2]) / dn;
    if (t <= 0)
        return false;
    double rel[3];
    for (int i = 0; i < 3; i++)
    {
        hit[i] = o[i] + d[i] * t;
        rel[i] = hit[i] - p.c[i];
    }
    *u = (rel[0] * p.ex[0] + rel[1] * p.ex[1] + rel[2] * p.ex[2]) / p.hw;
    *v = (rel[0] * p.ey[0] + rel[1] * p.ey[1] + rel[2] * p.ey[2]) / p.hh;
    return *u >= -1 && *u <= 1 && *v >= -1 && *v <= 1;
}

void RightRay(const ControllerState &r, double o[3], double d[3])
{
    const M3 m = QuatToM3(r.aimPose.orientation);
    o[0] = r.aimPose.position.x; o[1] = r.aimPose.position.y; o[2] = r.aimPose.position.z;
    d[0] = -m.m[0][2]; d[1] = -m.m[1][2]; d[2] = -m.m[2][2]; // aim forward = -Z
}

// board (s,t) map fractions <-> tracking space
void BoardPoint(double s, double t, double out[3])
{
    if (Table::On()) // Command Deck: the war table's relief top
    {
        Table::Point(s, t, out);
        return;
    }
    PlanePoint(BoardPlane(), s * 2 - 1, 1 - t * 2, out);
}

// world <-> tracking space (valid after the first world frame)
void WorldToXr(const double w[3], double xr[3])
{
    const double rel[3] = {w[0] - g_pa[0], w[1] - g_pa[1], w[2] - g_pa[2]};
    double l[3];
    g_ra.Apply(rel, l);
    xr[0] = l[0] / g_upm;
    xr[1] = -l[1] / g_upm;
    xr[2] = -l[2] / g_upm;
}

void XrRayToWorld(const double o[3], const double d[3], double wo[3], double wd[3])
{
    const double lo[3] = {o[0] * g_upm, -o[1] * g_upm, -o[2] * g_upm};
    const double ld[3] = {d[0], -d[1], -d[2]};
    const M3 rt = g_ra.T();
    rt.Apply(lo, wo);
    rt.Apply(ld, wd);
    for (int i = 0; i < 3; i++)
        wo[i] += g_pa[i];
}

const SUnit *FindSnap(uint32_t gid)
{
    for (const SUnit &u : g_snap.units)
        if (u.gid == gid)
            return &u;
    return NULL;
}

// ---- picking -----------------------------------------------------------------------
// Board: the unit nearest the hit within 14 mm (own units first when tied), else the
// sector. Context exactly as the original map (ypaworld_func64__sub21):
//   own unit -> select its squad; enemy unit -> attack it;
//   own and visible sector -> move; any other sector -> attack the sector.
void PickBoard()
{
    g_hoverGid = 0;
    g_hoverKind = 0;
    if (!g_boardHit || !g_snap.valid)
        return;
    if (Table::On()) // tokens on the war table (3D columns along the ray), else the sector rule below
    {
        uint32_t gid = 0;
        int kind = 0;
        if (Table::HoverToken(&gid, &kind))
        {
            g_hoverGid = gid;
            g_hoverKind = kind;
            return;
        }
    }
    double w, h;
    BoardSize(&w, &h);
    const double best0 = 0.014;
    double best = best0;
    const SUnit *pick = NULL;
    for (const SUnit &u : g_snap.units)
    {
        if (Table::On())
            break; // tokens were picked above
        if (u.robo && u.own)
            continue; // the host station itself is not an order target
        const double ds = (u.x / g_snap.lenX - g_boardS) * w;
        const double dt = (-u.z / g_snap.lenY - g_boardT) * h;
        double d = sqrt(ds * ds + dt * dt);
        if (u.own)
            d -= 0.002;
        if (d < best && (u.own ? u.selectable : true))
        {
            best = d;
            pick = &u;
        }
    }
    if (pick)
    {
        g_hoverGid = pick->gid;
        g_hoverKind = pick->own ? 1 : 2;
        return;
    }
    const int cx = (int)(g_boardS * g_snap.mapW), cy = (int)(g_boardT * g_snap.mapH);
    if (cx < 0 || cy < 0 || cx >= g_snap.mapW || cy >= g_snap.mapH)
        return;
    const int i = cy * g_snap.mapW + cx;
    if (g_snap.border[i])
        return;
    g_hoverKind = (g_snap.owner[i] == g_snap.me && g_snap.seen[i]) ? 3 : 4;
}

// World: the own unit closest to the right-hand ray (within 2.5 deg or its radius).
void PickWorld(const ControllerState &r)
{
    g_worldGid = 0;
    if (!g_anchorValid || !g_snap.valid || !r.active)
        return;
    double o[3], d[3], wo[3], wd[3];
    RightRay(r, o, d);
    XrRayToWorld(o, d, wo, wd);
    double best = 1e9;
    for (const SUnit &u : g_snap.units)
    {
        if (!u.own || !u.diveable || u.gid == g_snap.viewer)
            continue;
        const double v[3] = {u.x - wo[0], u.y - wo[1], u.z - wo[2]};
        const double along = v[0] * wd[0] + v[1] * wd[1] + v[2] * wd[2];
        if (along <= 0 || along > 6000)
            continue;
        const double dist = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const double ang = acos(std::max(-1.0, std::min(1.0, along / std::max(dist, 1e-6))));
        const double lim = std::max(Rad(2.5), atan2(u.radius * 1.2, dist));
        if (ang < lim && ang < best)
        {
            best = ang;
            g_worldGid = u.gid;
        }
    }
}

// Dev aid UA_VR_CMD_AIM="f0-f1:WHAT;..." (simulator verification): turns the right
// hand's aim towards a board / world target over poll frames f0..f1, so the real pick
// path runs unattended. WHAT = SQUAD (first own squad on the board), ENEMY (an enemy
// unit on the board, else an enemy sector), GROUND (own sector next to the station),
// CELL:x,y (a sector), WORLD (the nearest own unit in the 3D world).
void ScriptAim(ControllerState c[2])
{
    if (g_aimScript.empty() || !g_snap.valid)
        return;
    const AimKey *k = NULL;
    for (const AimKey &a : g_aimScript)
        if (g_poll >= a.f0 && g_poll <= a.f1)
            k = &a;
    if (!k)
        return;
    double target[3];
    bool ok = false;
    if (k->what == "TOKEN") // the first own squad token on the war table
        ok = Table::ScriptToken(target);
    else if (k->what == "HOST") // station: the own host station token
        ok = Table::ScriptHostToken(target);
    else if (k->what == "SQUAD" || k->what == "ENEMY")
    {
        const bool own = k->what == "SQUAD";
        for (const SUnit &u : g_snap.units)
            if (u.own == own && !u.robo && (!own || u.isCmdr))
            {
                BoardPoint(u.x / g_snap.lenX, -u.z / g_snap.lenY, target);
                ok = true;
                break;
            }
        if (!ok && !own)
        {
            for (int i = 0; i < g_snap.mapW * g_snap.mapH && !ok; i++)
                if (!g_snap.border[i] && g_snap.owner[i] != g_snap.me) // enemy or neutral: attack the sector
                {
                    BoardPoint((i % g_snap.mapW + 0.5) / g_snap.mapW, (i / g_snap.mapW + 0.5) / g_snap.mapH, target);
                    ok = true;
                }
        }
    }
    else if (k->what == "GROUND" || k->what.compare(0, 5, "CELL:") == 0)
    {
        int cx = -1, cy = -1;
        if (k->what == "GROUND")
        {
            const SUnit *r = FindSnap(g_snap.robo);
            if (r)
            {
                cx = (int)(r->x / World::CVSectorLength);
                cy = (int)(-r->z / World::CVSectorLength) - 1; // the sector north of the station
                if (cy < 1)
                    cy += 2;
            }
        }
        else
            sscanf(k->what.c_str() + 5, "%d,%d", &cx, &cy);
        if (cx >= 0 && cy >= 0)
        {
            BoardPoint((cx + 0.5) / g_snap.mapW, (cy + 0.5) / g_snap.mapH, target);
            ok = true;
        }
    }
    else if (k->what == "WORLD" && g_anchorValid)
    {
        const SUnit *r = FindSnap(g_snap.viewer);
        double best = 1e18;
        for (const SUnit &u : g_snap.units)
            if (u.own && u.diveable && r && u.gid != g_snap.viewer)
            {
                const double d2 = (u.x - r->x) * (u.x - r->x) + (u.z - r->z) * (u.z - r->z);
                if (d2 < best)
                {
                    best = d2;
                    const double w[3] = {u.x, u.y, u.z};
                    WorldToXr(w, target);
                    ok = true;
                }
            }
    }
    if (!ok)
        return;
    static const AimKey *logged = NULL;
    if (g_log && logged != k)
    {
        logged = k;
        fprintf(stderr, "UAVR cmd: script aim %s -> tracking %.3f,%.3f,%.3f\n", k->what.c_str(), target[0], target[1], target[2]);
    }
    ControllerState &R = c[1];
    const double o[3] = {R.aimPose.position.x, R.aimPose.position.y, R.aimPose.position.z};
    double d[3] = {target[0] - o[0], target[1] - o[1], target[2] - o[2]};
    const double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-6)
        return;
    for (int i = 0; i < 3; i++)
        d[i] /= l;
    // forward = RotY(th) * RotX(p) * (0,0,-1) = (-cos p sin th, sin p, -cos p cos th)
    const double p = asin(std::max(-1.0, std::min(1.0, d[1])));
    const double th = atan2(-d[0], -d[2]);
    R.aimPose.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, th), QuatFromAxisAngle(1, 0, 0, p));
}

void StartFade(const Pending &act, bool pulseA)
{
    g_death.on = false; // any switch the command layer starts answers the prompt
    g_death.autoReq = false;
    g_fade = FADE_OUT;
    g_fadeT0 = SDL_GetTicks();
    g_fadeAction = act;
    g_fadePulseA = pulseA;
    g_actionDone = false;
    g_waitViewer = g_snap.viewer;
    g_waitPolls = 0;
}

float FadeAlpha()
{
    const uint32_t now = SDL_GetTicks();
    switch (g_fade)
    {
    case FADE_OUT: return std::min(1.0f, (now - g_fadeT0) / (float)kFadeOutMs);
    case FADE_WAIT: return 1.0f;
    case FADE_IN: return std::max(0.0f, 1.0f - (now - g_fadeT0) / (float)kFadeInMs);
    default: return 0.0f;
    }
}

void Ticker(NC_STACK_ypaworld *yw, const char *txt)
{
    yw_arg159 msg;
    msg.unit = NULL;
    msg.Priority = 60;
    msg.txt = txt;
    msg.MsgID = 0;
    yw->ypaworld_func159(&msg);
}

NC_STACK_ypabact *FindUnit(NC_STACK_ypaworld *yw, uint32_t gid)
{
    if (!gid)
        return NULL;
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        if (r->_gid == gid)
            return r;
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            if (c->_gid == gid)
                return c;
            for (NC_STACK_ypabact *u : c->_kidList)
                if (u->_gid == gid)
                    return u;
        }
    }
    return NULL;
}

// ---- tiny 5x7 font for the board / wheel textures ------------------------------------
const uint8_t *Glyph(char ch)
{
    static const uint8_t digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};
    static const uint8_t letters[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};
    static const uint8_t space[7] = {0}, dash[7] = {0,0,0,0x1F,0,0,0}, colon[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0},
                         dot[7] = {0,0,0,0,0,0x0C,0x0C}, slash[7] = {0,0x01,0x02,0x04,0x08,0x10,0};
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
    if (ch == '-') return dash;
    if (ch == ':') return colon;
    if (ch == '.') return dot;
    if (ch == '/') return slash;
    return space;
}

struct Img
{
    int w, h;
    uint8_t *p;
    void Px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t *q = p + (y * w + x) * 4;
        q[0] = r; q[1] = g; q[2] = b; q[3] = a;
    }
    void Rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        for (int y = std::max(0, y0); y < std::min(h, y1); y++)
            for (int x = std::max(0, x0); x < std::min(w, x1); x++)
                Px(x, y, r, g, b, a);
    }
    void Disc(double cx, double cy, double rad, uint8_t r, uint8_t g, uint8_t b, uint8_t a, double inner = -1)
    {
        for (int y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); y++)
            for (int x = (int)(cx - rad - 1); x <= (int)(cx + rad + 1); x++)
            {
                const double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
                if (d <= rad && d >= inner)
                    Px(x, y, r, g, b, a);
            }
    }
    int TextW(const std::string &s, int sc) { return (int)s.size() * 6 * sc - sc; }
    void Text(int x, int y, const std::string &s, int sc, uint8_t r, uint8_t g, uint8_t b)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            const uint8_t *gl = Glyph(s[i]);
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (gl[row] & (0x10 >> col))
                        Rect(x + (int)i * 6 * sc + col * sc, y + row * sc, x + (int)i * 6 * sc + (col + 1) * sc, y + (row + 1) * sc, r, g, b, 255);
        }
    }
    void TextC(int cx, int y, const std::string &s, int sc, uint8_t r, uint8_t g, uint8_t b) { Text(cx - TextW(s, sc) / 2, y, s, sc, r, g, b); }
};

void Upload(GLuint *tex, int w, int h, const uint8_t *px)
{
    if (!*tex)
        glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

// Board image: one block of `cs` pixels per sector, owner colour (dim when not seen
// now), border sectors dark, unit marks (enemies only where the player sees), the
// selected squad ringed white, the host station as a ring, the viewer as a cross.
void RenderBoard()
{
    const Snap &s = g_snap;
    const bool table = Table::On(); // finer texels on the bigger table; units are 3D tokens there
    const int cs = table ? std::max(4, std::min(Table::TexCellCap(), 1024 / std::max(1, std::max(s.mapW, s.mapH))))
                         : std::max(4, std::min(16, 512 / std::max(1, std::max(s.mapW, s.mapH))));
    const int W = s.mapW * cs, H = s.mapH * cs;
    g_img.assign((size_t)W * H * 4, 0);
    Img im{W, H, g_img.data()};
    for (int cy = 0; cy < s.mapH; cy++)
        for (int cx = 0; cx < s.mapW; cx++)
        {
            const int i = cy * s.mapW + cx;
            uint8_t r = 18, g = 20, b = 22;
            if (!s.border[i])
            {
                if (s.owner[i])
                {
                    const SDL_Color c = s.ownerCol[s.owner[i] & 7];
                    const double k = s.seen[i] ? 0.55 : 0.28;
                    r = (uint8_t)(c.r * k); g = (uint8_t)(c.g * k); b = (uint8_t)(c.b * k);
                }
                else
                {
                    const uint8_t v = s.seen[i] ? 70 : 42;
                    r = g = b = v;
                }
            }
            im.Rect(cx * cs, cy * cs, (cx + 1) * cs, (cy + 1) * cs, r, g, b, 235);
            // sector grid
            im.Rect(cx * cs, cy * cs, (cx + 1) * cs, cy * cs + 1, 8, 10, 10, 240);
            im.Rect(cx * cs, cy * cs, cx * cs + 1, (cy + 1) * cs, 8, 10, 10, 240);
        }
    const double px = W / s.lenX, pz = H / s.lenY;
    for (const SUnit &u : s.units)
    {
        if (table)
            break;
        const double x = u.x * px, y = -u.z * pz;
        const SDL_Color c = s.ownerCol[u.owner & 7];
        if (u.robo)
        {
            im.Disc(x, y, cs * 0.55, 0, 0, 0, 255, cs * 0.30);
            im.Disc(x, y, cs * 0.45, c.r, c.g, c.b, 255, cs * 0.34);
            continue;
        }
        const double rad = u.isCmdr ? std::max(2.5, cs * 0.24) : std::max(1.8, cs * 0.16);
        if (u.own && u.cmdr && u.cmdr == s.selCmdr)
            im.Disc(x, y, rad + 2.2, 255, 255, 255, 255);
        im.Disc(x, y, rad + 1.0, 0, 0, 0, 255);
        im.Disc(x, y, rad, c.r, c.g, c.b, 255);
    }
    if (const SUnit *v = FindSnap(s.viewer))
    {
        const double x = v->x * px, y = -v->z * pz;
        im.Rect((int)x - 4, (int)y, (int)x + 5, (int)y + 1, 255, 230, 60, 255);
        im.Rect((int)x, (int)y - 4, (int)x + 1, (int)y + 5, 255, 230, 60, 255);
    }
    Upload(&g_boardTex, W, H, g_img.data());
    g_boardTexW = W;
    g_boardTexH = H;
}

// Wheel image: slots clockwise from the top; name + cost (as the build list shows
// it); unaffordable slots red; the highlighted slot bright; centre = station energy.
void RenderWheel()
{
    const int W = 512;
    g_img.assign((size_t)W * W * 4, 0);
    Img im{W, W, g_img.data()};
    im.Disc(256, 256, 250, 10, 14, 12, 200);
    im.Disc(256, 256, 250, 120, 200, 140, 255, 246);
    const int n = (int)g_snap.slots.size();
    if (n == 0)
    {
        im.TextC(256, 236, "NOTHING TO BUILD", 3, 220, 220, 220);
    }
    const double ring = n > 6 ? 175 : 160;
    const double sr = std::min(78.0, M_PI * ring / std::max(1, n) * 0.92);
    for (int i = 0; i < n; i++)
    {
        const SSlot &sl = g_snap.slots[i];
        const double a = 2 * M_PI * i / n;
        const double cx = 256 + sin(a) * ring, cy = 256 - cos(a) * ring;
        const bool afford = sl.cost <= g_snap.energy;
        const bool hl = i == g_wheelSlot;
        const bool built = i == g_builtSlot && SDL_GetTicks() < g_builtFlashUntil;
        uint8_t r = afford ? 30 : 70, g = afford ? 70 : 20, b = afford ? 45 : 20;
        if (hl) { r = afford ? 60 : 140; g = afford ? 170 : 40; b = afford ? 90 : 40; }
        if (built) { r = 230; g = 230; b = 230; }
        im.Disc(cx, cy, sr, r, g, b, 235);
        im.Disc(cx, cy, sr, hl ? 255 : 120, hl ? 255 : 200, hl ? 255 : 140, 255, sr - (hl ? 4 : 2));
        std::string name = sl.name.substr(0, 8);
        const int sc = sr > 60 ? 2 : 1;
        im.TextC((int)cx, (int)cy - 8 * sc - 2, name, sc, 235, 235, 235);
        char cost[16];
        snprintf(cost, sizeof(cost), "%d", sl.shown);
        im.TextC((int)cx, (int)cy + 4, cost, sc + 1, afford ? 140 : 255, afford ? 255 : 120, afford ? 160 : 120);
    }
    char e[32];
    snprintf(e, sizeof(e), "%d", g_snap.energy / 100);
    im.Disc(256, 256, 62, 0, 0, 0, 220);
    im.TextC(256, 222, "BUILD", 2, 200, 220, 200);
    im.TextC(256, 244, e, 3, 140, 255, 160);
    im.TextC(256, 274, "ENERGY", 1, 200, 220, 200);
    Upload(&g_wheelTex, W, W, g_img.data());
}

// ---- eye-space helpers (tracking point -> eye UAV metres -> clip) --------------------
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

bool PlaneQuad(const ViewState &vs, int eye, const M4 &proj, const Plane &p, float q[4][4])
{
    static const double uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}; // uv (0,0) = bottom-left = image last row
    for (int k = 0; k < 4; k++)
    {
        double xr[3], e[3];
        PlanePoint(p, uv[k][0], uv[k][1], xr);
        ToEye(vs, eye, xr, e);
        if (!Clip(proj, e, q[k]))
            return false;
    }
    return true;
}

bool MarkQuad(const ViewState &vs, int eye, const M4 &proj, const double xr[3], double radiusM, float q[4][4])
{
    double e[3];
    ToEye(vs, eye, xr, e);
    if (e[2] <= 1e-3)
        return false;
    static const double s[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int k = 0; k < 4; k++)
    {
        const double p[3] = {e[0] + s[k][0] * radiusM, e[1] + s[k][1] * radiusM, e[2]};
        if (!Clip(proj, p, q[k]))
            return false;
    }
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

void EnsureBlack()
{
    if (g_blackTex)
        return;
    const uint8_t px[4] = {0, 0, 0, 255};
    glGenTextures(1, &g_blackTex);
    glBindTexture(GL_TEXTURE_2D, g_blackTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

void FullQuad(float a)
{
    float q[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, a, g_blackTex);
}

bool Live() { return g_enabled && Active() && g_snap.valid && SDL_GetTicks() - g_snap.ticks < 300; }

} // namespace

// ---------------------------------------------------------------------------

void Init(double upm)
{
    g_upm = upm;
    auto env = [](const char *n) { const char *v = getenv(n); return (v && v[0]) ? v : (const char *)NULL; };
    if (const char *v = env("UA_VR_COMMAND")) g_enabled = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_STATION_TURN")) g_snapTurn = strcmp(v, "smooth") != 0;
    if (const char *v = env("UA_VR_SNAP_DEG")) g_snapDeg = atof(v);
    else g_snapDeg = g_snapDegSetting;
    if (const char *v = env("UA_VR_BOARD_M")) g_boardM = atof(v);
    if (const char *v = env("UA_VR_CMD_LOG")) g_log = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_CMD_AIM"))
    {
        std::string s(v);
        size_t at = 0;
        while (at < s.size())
        {
            size_t e = s.find(';', at);
            const std::string seg = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
            AimKey k;
            char what[32] = {0};
            if (sscanf(seg.c_str(), "%d-%d:%31s", &k.f0, &k.f1, what) == 3)
            {
                k.what = what;
                g_aimScript.push_back(k);
            }
            if (e == std::string::npos)
                break;
            at = e + 1;
        }
    }
    Table::Init(upm);
    if (g_enabled)
        fprintf(stderr, "UAVR cmd: command layer on (station turn %s %.0f deg, board %.2f m)\n", g_snapTurn ? "snap" : "smooth", g_snapDeg, g_boardM);
}

uint64_t SnapKey() { return (uint64_t)g_snapEpoch * 0x9E3779B97F4A7C15ull; }

void SetAnchor(const M3 &ra, const double pa[3])
{
    g_ra = ra;
    memcpy(g_pa, pa, sizeof(g_pa));
    g_anchorValid = true;
}

bool DeckTableAtStation() { return Live() && g_board && g_snap.inStation && Table::On() && !MapWall(); }
bool StationHint() { return QuickStation() && Live() && !g_snap.inStation; }
bool DeathPrompt(float *secLeft)
{
    if (!QuickStation() || !Live() || !g_death.on)
        return false;
    if (secLeft)
        *secLeft = g_death.left / 1000.0f;
    return true;
}
// the build wheel opens over the miniature (the mixed-reality table stays up, so a unit
// built from the station rolls out on it); the token table still swaps for the wheel.
bool DioramaShown() { return Live() && g_board && Table::DioramaOn(); }
bool ViewChanging() { return g_fade != FADE_NONE; }

void Filter(ControllerState c[2])
{
    g_poll++;
    if (!Live())
    {
        Panels::FilterIdle(c); // an owned grab squeeze stays swallowed
        g_boardHit = false;
        g_worldGid = 0;
        if (g_fade != FADE_NONE && SDL_GetTicks() - g_fadeT0 > 1000)
            g_fade = FADE_NONE;
        return;
    }
    const uint32_t now = SDL_GetTicks();
    static bool liveLogged = false;
    if (!liveLogged)
    {
        liveLogged = true;
        fprintf(stderr, "UAVR cmd: live at poll %d (map %dx%d, %d build slots, energy %d, me %d)\n", g_poll, g_snap.mapW, g_snap.mapH, (int)g_snap.slots.size(), g_snap.energy, g_snap.me);
        fprintf(stderr, "UAVR cmd: settings: grips %d, crosshair %d, map board %s, station turn %s\n", GroundGripMode(), CrosshairColour(), g_boardOn ? "on" : "off", g_snapTurn ? "snap" : "smooth");
        for (const SUnit &u : g_snap.units)
            fprintf(stderr, "UAVR cmd:   unit %u owner %d type %d at %.0f,%.0f%s%s\n", u.gid, u.owner, u.type, u.x, u.z, u.isCmdr ? " cmdr" : "", u.robo ? " robo" : "");
    }
    ScriptAim(c);
    // PANELS FREE: a right-trigger squeeze that starts on a panel's grab bar / corner
    // moves / resizes it and is zeroed for every layer below (one latch, vr_grip.h). The
    // table (last poll), the build wheel (it owns the right stick) or the squad wheel (Y held
    // in a vehicle) keep the hand.
    static bool tableBusyPrev = false;
    const float rawStickX = c[1].stickX, rawStickY = c[1].stickY; // before a grab zeroes them
    const bool panelOwns = Panels::Filter(c, tableBusyPrev || g_wheel || (SquadWheel() && !g_snap.inStation && !g_snap.inGun && (c[0].buttons & BTN_Y)), now, g_poll);
    ControllerState &L = c[0], &R = c[1];
    g_right = R;
    const float physGrip[2] = {L.grip, R.grip}; // table: grip edges before any layer zeroes them
    const uint32_t lb = L.buttons, rb = R.buttons;

    // ---- fade sequencing (dive / next unit / host station) ----
    if (g_fade == FADE_OUT && now - g_fadeT0 >= kFadeOutMs)
    {
        g_fade = FADE_WAIT;
        g_fadeT0 = now;
        if (g_fadePulseA)
            g_pulseFrames = 3; // the original "next unit" hotkey (22) via winp
        else
            g_queue.push_back(g_fadeAction);
        // the board must not cover the cockpit dash / wrist after a dive. While
        // black, a dive / next unit closes it (remembering the host-station state) and
        // the return to the host station brings the station's board back.
        static bool stationBoard = false;
        if (!g_fadePulseA && g_fadeAction.kind == ACT_HOST)
            g_board = stationBoard && g_boardOn;
        else
        {
            if (g_snap.inStation)
                stationBoard = g_board;
            g_board = false;
        }
    }
    if (g_fade == FADE_WAIT)
    {
        g_waitPolls++;
        const bool switched = g_snap.viewer != g_waitViewer;
        if ((g_actionDone && g_waitPolls > 1) || (g_fadePulseA && (switched || g_waitPolls > 8)) || now - g_fadeT0 > 400)
        {
            g_fade = FADE_IN;
            g_fadeT0 = now;
        }
    }
    if (g_fade == FADE_IN && now - g_fadeT0 >= kFadeInMs)
        g_fade = FADE_NONE;
    const bool busy = g_fade != FADE_NONE;
    // no view change (dive / return / QUICK STATION) while the host station
    // beam is queued, fading or holding: the two fades never overlap. A pending automatic
    // death return stays requested and runs when the beam is done.
    // evaluated at each decision (a ring confirmed by the table earlier in this poll counts).
    auto viewBusy = [&]() { return g_fade != FADE_NONE || Station::Beaming(); };

    // ---- right ray: board hit, world pick ----
    double o[3], d[3];
    RightRay(R, o, d);
    g_boardHit = false;
    if (g_board && !g_wheel && R.active)
    {
        double u, v, hit[3];
        if (Table::On()) // the war table / wall (relief + tokens)
        {
            double s, t;
            if (Table::Ray(o, d, true, &s, &t, hit))
            {
                g_boardHit = true;
                g_boardS = s;
                g_boardT = t;
                memcpy(g_hitXr, hit, sizeof(hit));
            }
        }
        else if (RayPlane(BoardPlane(), o, d, &u, &v, hit))
        {
            g_boardHit = true;
            g_boardS = (u + 1) * 0.5;
            g_boardT = (1 - v) * 0.5;
            memcpy(g_hitXr, hit, sizeof(hit));
        }
    }
    PickBoard();
    PickWorld(R);

    // war table: grips grab / turn / scale it, pick up squad tokens (dive) ----
    // input ownership: the table resolves its hands FIRST, so a hand it owns (grab,
    // carry, or a trigger latched until physically released) reaches neither the field
    // layer nor the board as a trigger. The squad wheel (Y held in a vehicle) owns
    // the hands while it is up (previous poll's ownership), so the grips never grab /
    // carry on the table behind it.
    // Squad wheel: decided from THIS poll's physical Y, before any
    // other layer acts on the hands.
    static FieldLogic::WheelTrig wheelTrig[2]; // per hand (LT = machine gun)
    const bool inVehicle = !g_snap.inStation && !g_snap.inGun;
    const bool wheelHands = FieldLogic::WheelOwnsHands(SquadWheel(), inVehicle, (lb & BTN_Y) != 0);
    const bool wheelTrigL = FieldLogic::UpdateWheelTrig(wheelTrig[0], wheelHands, L.trigger);
    const bool wheelTrigOwned = FieldLogic::UpdateWheelTrig(wheelTrig[1], wheelHands, g_right.trigger);
    if (wheelHands)
    {
        L.grip = 0; // no table grab / carry, no drive / brake grip behind the wheel
        R.grip = 0;
    }
    if (wheelTrigL)
        L.trigger = 0; // no machine gun behind the wheel (until LT is released)
    if (wheelTrigOwned)
        R.trigger = 0; // no weapon, field order or wrist click; the board is blocked below
    const bool tableBusy = Table::Input(c, physGrip, g_board && !g_wheel && !wheelHands, now, (lb & BTN_X) != 0);
    tableBusyPrev = tableBusy;

    // field orders (vr_field.cpp): field orders (X held + trigger), clickable wrist,
    // squad wheel (Y held). Inert unless its Settings row is on and the viewer is a vehicle.
    Field::In fin;
    fin.poll = g_poll;
    fin.boardHit = g_boardHit;
    fin.boardOn = g_boardOn;
    fin.inVehicle = inVehicle;
    fin.busy = busy;
    fin.selCmdr = g_snap.selCmdr;
    const Field::Out fout = Field::Filter(c, fin);
    if (fout.blockBoard || wheelHands || wheelTrigOwned)
    {
        g_boardHit = false;
        g_hoverKind = 0;
    }

    // ---- X: board (tap) / original 2D map (hold) ----
    auto holdButton = [&](Hold &h, bool down, bool own, uint32_t bit, ControllerState &cs, auto onTap)
    {
        if (down && !h.down)
        {
            h.t0 = now;
            h.owned = own;
            h.passed = false;
        }
        if (!down && h.down && h.owned && !h.passed)
            onTap();
        h.down = down;
        if (h.owned)
        {
            if (down && !h.passed && now - h.t0 >= kHoldMs)
                h.passed = true; // long press: the original binding from now until release
            if (!h.passed)
                cs.buttons &= ~bit;
        }
    };
    if (!g_boardOn)
        g_board = false; // board switched off in Settings: X is the original map (hotkey 8)
    auto toggleBoard = [&]()
    {
        g_board = !g_board;
        if (g_board)
            g_wheel = false;
        if (g_log) fprintf(stderr, "UAVR cmd: board %s\n", g_board ? "open" : "closed");
    };
    if (fout.ownsX) // this X press belongs to field orders (its tap comes back as toggleBoard)
    {
        g_x = Hold();
        g_x.down = (lb & BTN_X) != 0;
        if (fout.toggleBoard && g_boardOn)
            toggleBoard();
    }
    else
        holdButton(g_x, (lb & BTN_X) != 0, g_boardOn, BTN_X, L, toggleBoard);

    // ---- Y: build wheel (host station / station gun: tap) / squad manager (hold) ----
    const bool canWheel = g_snap.inStation || g_snap.inGun;
    if (fout.ownsY) // the squad wheel owns this Y press (vehicle)
    {
        g_y = Hold();
        g_y.down = (lb & BTN_Y) != 0;
    }
    else
    holdButton(g_y, (lb & BTN_Y) != 0, canWheel || g_wheel, BTN_Y, L, [&]()
    {
        g_wheel = !g_wheel;
        g_wheelSlot = -1;
        g_wheelNeedNeutral = panelOwns; // opened during a panel grab
        if (g_wheel) // the table under the wheel lets go of its state this poll
        {
            Table::WheelOpened(now);
            g_boardHit = false;
        }
        if (g_log) fprintf(stderr, "UAVR cmd: build wheel %s (%d slots, energy %d)\n", g_wheel ? "open" : "closed", (int)g_snap.slots.size(), g_snap.energy);
    });
    if (g_wheel && !canWheel)
        g_wheel = false;

    // ---- wheel: right stick flick selects, release builds ----
    if (g_wheel)
    {
        // while a panel grab owns the right hand its zeroed stick is not a
        // wheel release: the pick is dropped, and the wheel only arms again after the physical
        // stick has come back to neutral with the grab over.
        if (panelOwns)
        {
            g_wheelNeedNeutral = true;
            g_wheelSlot = -1;
        }
        else if (g_wheelNeedNeutral && sqrt(rawStickX * rawStickX + rawStickY * rawStickY) < 0.3)
            g_wheelNeedNeutral = false;
        const bool wheelArmed = !panelOwns && !g_wheelNeedNeutral;
        const double mag = sqrt(R.stickX * R.stickX + R.stickY * R.stickY);
        const int n = (int)g_snap.slots.size();
        if (wheelArmed && mag > 0.6 && n > 0)
        {
            double a = atan2(R.stickX, R.stickY); // 0 = up, clockwise
            if (a < 0) a += 2 * M_PI;
            g_wheelSlot = ((int)floor(a / (2 * M_PI / n) + 0.5)) % n;
        }
        else if (wheelArmed && mag < 0.3 && g_wheelSlot >= 0)
        {
            Pending p;
            p.kind = ACT_BUILD;
            p.slot = g_wheelSlot;
            p.add = g_right.grip > 0.5f; // R grip held on release: add to the selected squad
            g_queue.push_back(p);
            g_builtSlot = g_wheelSlot;
            g_builtFlashUntil = now + 300;
            g_wheelSlot = -1;
        }
        R.stickX = R.stickY = 0; // the wheel owns the right stick
        R.grip = 0;              // ... and the right grip (add-to-squad modifier)
        if ((rb & BTN_B) && !g_b.down)
        {
            g_wheel = false; // B closes the wheel (and is swallowed until released)
            g_b.owned = true;
            g_b.passed = true;
            g_b.down = true;
            g_b.t0 = now;
        }
    }


    // ---- board: R trigger = click (select / order), grip swallowed ----
    // A squeeze that started on the board stays a board click until released (it never
    // fires the weapon when the ray leaves the board); one that started off the board
    // never turns into a board click.
    {
        const bool trigPhys = g_right.trigger > 0.6f;
        const bool trigEdge = trigPhys && !g_trigDown;
        if (trigEdge)
            g_trigOnBoard = g_boardHit;
        if (g_right.trigger < 0.2f)
            g_trigOnBoard = false;
        if (g_boardHit && trigEdge && g_hoverKind && !tableBusy)
        {
            Pending p;
            if (g_hoverKind == 1)
            {
                p.kind = ACT_SELECT;
                p.gid = g_hoverGid;
            }
            else
            {
                p.kind = ACT_ORDER;
                p.gid = g_hoverKind == 2 ? g_hoverGid : 0;
                p.x = g_boardS * g_snap.lenX;
                p.z = -g_boardT * g_snap.lenY;
            }
            g_queue.push_back(p);
            if (g_log) fprintf(stderr, "UAVR cmd: board click kind %d unit %u at map %.3f,%.3f\n", g_hoverKind, g_hoverGid, g_boardS, g_boardT);
        }
        if (g_boardHit || g_trigOnBoard)
            R.trigger = 0;
        if (g_boardHit)
            R.grip = 0;
        g_trigDown = trigPhys;
    }

    // ---- A: dive into the pointed unit, else the original next unit; both behind a fade ----
    const bool aDown = (rb & BTN_A) != 0;
    // "BACK TO STATION? A" (own vehicle destroyed): A returns at once; the timer
    // (Gameplay) asks for the same return when it runs out.
    if (QuickStation() && !viewBusy() && ((g_death.on && aDown && !g_aDown) || g_death.autoReq))
    {
        const bool byA = !g_death.autoReq;
        Pending p;
        p.kind = ACT_HOST;
        StartFade(p, false);
        fprintf(stderr, "UAVR cmd: vehicle destroyed -> host station (%s, fade out)\n", byA ? "A pressed" : "auto return");
    }
    else if (aDown && !g_aDown && !viewBusy())
    {
        uint32_t target = 0;
        if (g_boardHit && g_hoverKind == 1)
        {
            const SUnit *u = FindSnap(g_hoverGid);
            if (u && u->diveable)
                target = u->gid;
        }
        if (!target && !g_boardHit)
            target = g_worldGid;
        Pending p;
        p.kind = ACT_DIVE;
        p.gid = target;
        StartFade(p, target == 0);
        if (g_log) fprintf(stderr, "UAVR cmd: A -> %s %u (fade out)\n", target ? "dive into" : "next unit (hotkey 22)", target);
    }
    g_aDown = aDown;
    R.buttons &= ~BTN_A;
    if (g_pulseFrames > 0)
    {
        R.buttons |= BTN_A; // winp: edge -> the original "next unit" / "station guns" hotkey
        g_pulseFrames--;
    }

    // ---- B held 0.5 s: back to the host station (fade). B stays a right click while pointing. ----
    {
        const bool bDown = (rb & BTN_B) != 0;
        int px, py;
        if (bDown && !g_b.down)
        {
            g_b.t0 = now;
            const bool ptr = GetPointer(&px, &py);
            g_b.owned = !ptr && !g_snap.inStation;
            // QUICK STATION: in a vehicle / gun (pointer only over windows / the tool bar,
            // where a resting hand points) the press stays the game's right click, and a hold
            // still returns to the station. Modal boxes (pause menu: pointer mode 1) keep B.
            g_b.watch = ptr && !g_snap.inStation && QuickStation() && UiPointerMode() == 2;
            g_b.passed = false;
            if (g_log && (g_snap.userDead || g_b.watch))
                fprintf(stderr, "UAVR cmd: B down (%s%s)\n", g_snap.userDead ? "own vehicle destroyed" : "vehicle",
                        ptr ? (g_b.watch ? ", pointer on: right click + hold watch" : ", pointer on: right click only") : "");
        }
        if (g_b.owned || g_b.watch)
        {
            if (bDown && !g_b.passed && now - g_b.t0 >= kBHoldMs && !viewBusy())
            {
                g_b.passed = true;
                Pending p;
                p.kind = ACT_HOST;
                StartFade(p, false);
                if (g_log || g_snap.userDead) fprintf(stderr, "UAVR cmd: B held -> host station (fade out)%s%s\n", g_b.watch ? " [pointer on]" : "", g_snap.userDead ? " [own vehicle destroyed]" : "");
            }
            if (g_b.owned)
                R.buttons &= ~BTN_B;
        }
        g_b.down = bDown;
        if (!bDown)
            g_b.owned = g_b.watch = false;
    }

    // ---- host station: snap turn on the left stick ----
    if (g_snap.inStation && g_snapTurn)
    {
        const float x = L.stickX;
        if (!g_snapLatch && fabs(x) > 0.7f && !busy)
        {
            g_snapLatch = true;
            Pending p;
            p.kind = ACT_SNAP;
            p.snapDeg = x > 0 ? g_snapDeg : -g_snapDeg; // + = turn right
            g_queue.push_back(p);
        }
        else if (g_snapLatch && fabs(x) < 0.3f)
            g_snapLatch = false;
        L.stickX = 0; // the game's smooth turn is replaced by the snap
    }
    else
        g_snapLatch = false;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    Table::Gameplay(yw); // war table snapshot (no-op outside COMMAND DECK)
    if (!g_enabled || !Active() || !yw || !yw->_userRobo || !yw->_viewerBact || yw->_mapSize.x <= 0)
    {
        g_snap.valid = false;
        g_anchorValid = false;
        return;
    }
    Snap &s = g_snap;
    s.mapW = yw->_mapSize.x;
    s.mapH = yw->_mapSize.y;
    s.lenX = yw->_mapLength.x;
    s.lenY = yw->_mapLength.y;
    s.me = yw->_userRobo->_owner;
    const size_t n = (size_t)s.mapW * s.mapH;
    s.owner.assign(n, 0);
    s.seen.assign(n, 0);
    s.border.assign(n, 0);
    for (cellArea &cell : yw->_cells)
    {
        const int cx = cell.CellId.x, cy = cell.CellId.y;
        if (cx < 0 || cy < 0 || cx >= s.mapW || cy >= s.mapH)
            continue;
        const size_t i = (size_t)cy * s.mapW + cx;
        s.owner[i] = cell.owner;
        s.seen[i] = cell.IsCanSee(s.me) ? 1 : 0;
        s.border[i] = (cell.Flags & cellArea::CF_BORDER) || cx == 0 || cy == 0 || cx == s.mapW - 1 || cy == s.mapH - 1;
    }
    for (int i = 0; i < 8; i++)
        s.ownerCol[i] = yw->GetColor(i);

    s.units.clear();
    auto add = [&](NC_STACK_ypabact *u, uint32_t cmdr, bool isCmdr)
    {
        if (u->_status == BACT_STATUS_DEAD || u->_bact_type == BACT_TYPES_MISSLE)
            return;
        const bool own = u->_owner == s.me;
        if (!own && !(u->_pSector && u->_pSector->IsCanSee(s.me)))
            return; // the board shows enemies only where the player sees, like the original map
        SUnit su;
        su.gid = u->_gid;
        su.x = u->_position.x; su.y = u->_position.y; su.z = u->_position.z;
        su.radius = u->_radius;
        su.owner = u->_owner;
        su.type = u->_bact_type;
        su.own = own;
        su.robo = u->_bact_type == BACT_TYPES_ROBO;
        su.isCmdr = isCmdr;
        su.cmdr = cmdr;
        su.selectable = own && !su.robo && u->_bact_type != BACT_TYPES_GUN;
        su.diveable = su.selectable && u->_status != BACT_STATUS_CREATE && u->_status != BACT_STATUS_BEAM;
        s.units.push_back(su);
    };
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        add(r, 0, false);
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            add(c, c->_gid, true);
            for (NC_STACK_ypabact *u : c->_kidList)
                add(u, c->_gid, false);
        }
    }
    s.robo = yw->_userRobo->_gid;
    s.viewer = yw->_viewerBact->_gid;
    s.inStation = yw->_viewerBact == yw->_userRobo;
    s.inGun = yw->_playerInHSGun;
    // QUICK STATION: own vehicle / gun destroyed (the game keeps the camera on the wreck)
    s.user = yw->_userUnit ? yw->_userUnit->_gid : 0;
    s.userDead = yw->_userUnit && yw->_userUnit != yw->_userRobo && yw->_userUnit->_status == BACT_STATUS_DEAD &&
                 yw->_userRobo->_status != BACT_STATUS_DEAD;
    if (g_killUserAt == -2)
    {
        const char *v = getenv("UA_VR_TEST_KILL_USER"); // dev aid: destroy the player's vehicle at gameplay frame n
        g_killUserAt = (v && v[0]) ? atoi(v) : -1;
    }
    {
        static int frames = 0;
        frames++;
        if (g_killUserAt >= 0 && frames >= g_killUserAt && yw->_userUnit && yw->_userUnit != yw->_userRobo &&
            yw->_userUnit->_status != BACT_STATUS_DEAD)
        {
            g_killUserAt = -1;
            bact_arg84 a;
            a.unit = NULL;
            a.energy = -2 * std::max(1, yw->_userUnit->_energy_max);
            fprintf(stderr, "UAVR cmd: TEST destroy the player's vehicle %u (type %d) at gameplay frame %d\n", yw->_userUnit->_gid, yw->_userUnit->_bact_type, frames);
            yw->_userUnit->ModifyEnergy(&a);
        }
    }
    if (QuickStation() && s.userDead)
    {
        if (!g_death.on && g_death.gid != s.user && g_fade == FADE_NONE)
        {
            g_death = Death();
            g_death.on = true;
            g_death.gid = s.user;
            g_death.viewer0 = s.viewer;
            g_death.t0 = yw->_timeStamp;
            g_death.left = kDeathAutoMs;
            fprintf(stderr, "UAVR cmd: own vehicle %u destroyed (camera %u) -> BACK TO STATION? A (auto in %.1f s)\n", s.user, s.viewer, kDeathAutoMs / 1000.0);
        }
        if (g_death.on)
        {
            g_death.left = std::max(0, kDeathAutoMs - (yw->_timeStamp - g_death.t0));
            if (s.viewer != g_death.viewer0 || s.inStation)
            {
                g_death.on = false;
                fprintf(stderr, "UAVR cmd: the game switched the view itself (camera %u -> %u): no auto return\n", g_death.viewer0, s.viewer);
            }
            else if (g_death.left <= 0 && !g_death.autoReq)
                g_death.autoReq = true;
        }
    }
    else
    {
        if (g_death.on)
            fprintf(stderr, "UAVR cmd: death prompt closed (%s)\n", QuickStation() ? "own unit alive / changed" : "QUICK STATION off");
        g_death.on = false;
        g_death.autoReq = false;
        if (!s.userDead)
            g_death.gid = 0;
    }
    s.selCmdr = 0;
    if (yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size())
        s.selCmdr = yw->_cmdrsRemap[yw->_activeCmdrRemapIndex]->_gid;
    s.energy = yw->_userRobo->_bact_type == BACT_TYPES_ROBO ? ((NC_STACK_yparobo *)yw->_userRobo)->getROBO_battVehicle() : 0;
    // Build slots = the bottom bar's vehicle list (bzda.field_2DC[0 .. field_8E0)),
    // costs as gui_update_create_btn__sub0 shows them.
    s.canBuild = (bzda.field_1CC & 2) != 0;
    s.slots.clear();
    for (int i = 0; i < bzda.field_8E0 && i < 256; i++)
    {
        const World::TVhclProto &p = yw->_vhclProtos.at(bzda.field_2DC[i]);
        SSlot sl;
        sl.proto = bzda.field_2DC[i];
        sl.name = yw->GetVehicleName(p);
        sl.cost = dround(yw->sub_4498F4() * (2 * p.energy));
        sl.shown = dround(yw->sub_4498F4() * 2 * p.energy / 100.0);
        s.slots.push_back(sl);
    }
    s.valid = true;
    s.ticks = SDL_GetTicks();

    // result log: what the game made of the last command
    if (g_verify.kind && ++g_verify.frames >= 2)
    {
        if (g_verify.kind == ACT_ORDER || g_verify.kind == ACT_SELECT)
        {
            NC_STACK_ypabact *c = FindUnit(yw, g_verify.gid);
            if (c && g_verify.kind == ACT_ORDER)
            {
                if (c->_waypoints_count > 0)
                {
                    const vec3d &w = c->_waypoints[c->_waypoints_count - 1];
                    fprintf(stderr, "UAVR cmd: RESULT squad %u path: %d waypoints, final waypoint sector %d,%d\n", g_verify.gid, c->_waypoints_count,
                            (int)(w.x / World::CVSectorLength), (int)(-w.z / World::CVSectorLength));
                }
                if (c->_primTtype == BACT_TGT_TYPE_CELL && c->_primT.pcell)
                    fprintf(stderr, "UAVR cmd: RESULT squad %u primary target = SECTOR %d,%d (owner %d)\n", g_verify.gid, c->_primT.pcell->CellId.x, c->_primT.pcell->CellId.y, c->_primT.pcell->owner);
                else if (c->_primTtype == BACT_TGT_TYPE_UNIT && c->_primT.pbact)
                    fprintf(stderr, "UAVR cmd: RESULT squad %u primary target = UNIT %u (owner %d)\n", g_verify.gid, c->_primT.pbact->_gid, c->_primT.pbact->_owner);
                else
                    fprintf(stderr, "UAVR cmd: RESULT squad %u primary target type %d\n", g_verify.gid, c->_primTtype);
            }
            else if (g_verify.kind == ACT_SELECT)
                fprintf(stderr, "UAVR cmd: RESULT selected squad = %u (active index %d of %d)\n", s.selCmdr, yw->_activeCmdrRemapIndex, (int)yw->_cmdrsRemap.size());
        }
        else if (g_verify.kind == ACT_BUILD)
            fprintf(stderr, "UAVR cmd: RESULT energy %d -> %d, squads %d -> %d, selected squad %u\n", g_verify.energyBefore, s.energy,
                    (int)g_verify.squadsBefore, (int)yw->_cmdrsRemap.size(), s.selCmdr);
        else if (g_verify.kind == ACT_DIVE || g_verify.kind == ACT_HOST)
            fprintf(stderr, "UAVR cmd: RESULT viewer %u -> %u (type %d)%s\n", g_verify.viewerBefore, s.viewer, yw->_viewerBact->_bact_type, s.inStation ? " = host station" : "");
        else if (g_verify.kind == ACT_SNAP)
            fprintf(stderr, "UAVR cmd: RESULT station view %.1f -> %.1f deg (viewer forward %.3f,%.3f)\n", g_verify.angBefore,
                    Deg(yw->_userRobo->_viewer_horiz_angle), yw->_viewerBact->_viewer_rotation.m20, yw->_viewerBact->_viewer_rotation.m22);
        g_verify = Verify();
    }
    // the A pulse: report the viewer switch the original hotkey made
    static uint32_t lastViewer = 0;
    if (g_log && lastViewer && lastViewer != s.viewer)
        fprintf(stderr, "UAVR cmd: viewer now %u (type %d)%s\n", s.viewer, yw->_viewerBact->_bact_type, s.inStation ? " host station" : (s.inGun ? " station gun" : ""));
    lastViewer = s.viewer;
}

void DrawEye(int eye, const ViewState &views, double zn, double zf, bool diorama)
{
    if (!Live() && FadeAlpha() <= 0.0f && !Table::FxOn())
        return;
    const float fade = FadeAlpha();
    const bool board = Live() && g_board && (!g_wheel || diorama); // the miniature's base stays under the wheel
    const bool wheel = Live() && g_wheel;
    const bool world = Live() && !g_boardHit && g_worldGid && g_anchorValid && !diorama; // no full-scale marks under the miniature
    if (!board && !wheel && !world && fade <= 0.0f && !Table::FxOn())
        return;
    if (!Overlay::Begin())
        return;
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    const uint32_t now = SDL_GetTicks();
    float q[4][4];
    if (board)
    {
        if (eye == 0 && (now - g_boardTexTicks > 150 || !g_boardTex))
        {
            RenderBoard();
            g_boardTexTicks = now;
        }
        if (Table::On()) // the 3D war table (relief, tokens), board image on top
        {
            Overlay::End();
            Table::DrawEye(eye, views, zn, zf, g_boardTex, g_boardHit ? g_hoverKind : 0, g_hoverGid, g_boardHit, g_boardS, g_boardT, g_snap.selCmdr);
            if (!Overlay::Begin())
                return;
        }
        else if (g_boardTex && PlaneQuad(views, eye, proj, BoardPlane(), q))
            Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, 0.97f, g_boardTex);
        if (g_boardHit)
        {
            static const float col[5][3] = {{0.8f, 0.8f, 0.8f}, {1, 1, 1}, {1, 0.3f, 0.2f}, {0.4f, 1, 0.5f}, {1, 0.6f, 0.2f}};
            const float *cc = col[g_hoverKind];
            if (MarkQuad(views, eye, proj, g_hitXr, 0.008, q))
                Overlay::Quad(q, Overlay::STYLE_DOT, cc[0], cc[1], cc[2], 0.95f);
            const double a[3] = {g_right.aimPose.position.x, g_right.aimPose.position.y, g_right.aimPose.position.z};
            if (Beam(views, eye, proj, a, g_hitXr, 0.0015, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, cc[0], cc[1], cc[2], 0.6f);
        }
    }
    if (wheel)
    {
        if (eye == 0 && (now - g_wheelTexTicks > 100 || !g_wheelTex))
        {
            RenderWheel();
            g_wheelTexTicks = now;
        }
        if (g_wheelTex && PlaneQuad(views, eye, proj, WheelPlane(), q))
            Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, 0.97f, g_wheelTex);
    }
    if (world)
    {
        const SUnit *u = FindSnap(g_worldGid);
        if (u)
        {
            double w[3] = {u->x, u->y, u->z}, xr[3], e[3];
            WorldToXr(w, xr);
            ToEye(views, eye, xr, e);
            const double dist = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            if (MarkQuad(views, eye, proj, xr, dist * tan(Rad(1.6)), q))
                Overlay::Quad(q, Overlay::STYLE_RETICLE, 0.45f, 1.0f, 0.55f, 0.9f); // dive marker green (crosshair is yellow)
        }
    }
    if (fade > 0.0f)
    {
        EnsureBlack();
        FullQuad(fade);
    }
    Table::DrawFx(eye, views, zn, zf, fade); // "cockpit boot" after a token dive (over the lifting black)
    Overlay::End();
}

void DrawPanel(int panelW, int panelH)
{
    (void)panelW;
    (void)panelH;
    const float fade = FadeAlpha();
    if (fade <= 0.0f || !Overlay::Begin())
        return;
    EnsureBlack();
    FullQuad(fade);
    Overlay::End();
}

} // namespace Cmd

// ---------------------------------------------------------------------------
// Public (vr.h): the world-update hook. Runs every gameplay frame between the UI
// pass (ypaworld_func64__sub21) and yparobo::HandleUserCommands. Each queued action
// goes through the original user-action switch (ypaworld_func64__sub21__sub5) with
// the "thing under the mouse" fields pointed at the board / wheel / ray target, then
// those UI fields are restored. A mouse action the game already queued this frame wins.
void CommandWorldHook(NC_STACK_ypaworld *yw)
{
    using namespace Cmd;
    if (Active() && yw)
        Station::WorldHook(yw); // a beam confirmed on the war table (original DOACTION_10)
    if (!Active() || !g_enabled || !yw || g_queue.empty())
        return;
    std::vector<Pending> q;
    q.swap(g_queue);
    if (!yw->_userRobo || !yw->_viewerBact || yw->_userRobo->_status == BACT_STATUS_DEAD)
    {
        g_actionDone = true;
        return;
    }
    // UI fields the original action code reads; restored after each call.
    NC_STACK_ypabact *const sBact = yw->_bactOnMouse;
    cellArea *const sCell = yw->_cellOnMouse;
    const vec3d sIsect = yw->_cellMouseIsectPos;
    const uint32_t sFlags = yw->_guiActFlags;
    const vec3d sVec = yw->_mouseVector;
    const float sDist = yw->_mouseSelDistance;
    auto restore = [&]()
    {
        yw->_bactOnMouse = sBact;
        yw->_cellOnMouse = sCell;
        yw->_cellMouseIsectPos = sIsect;
        yw->_guiActFlags = sFlags;
        yw->_mouseVector = sVec;
        yw->_mouseSelDistance = sDist;
    };
    for (const Pending &p : q)
    {
        if (yw->_updateMessage.user_action != World::DOACTION_0 && (p.kind == ACT_ORDER || p.kind == ACT_BUILD))
        {
            fprintf(stderr, "UAVR cmd: the game already has user action %d this frame; VR order dropped\n", yw->_updateMessage.user_action);
            continue;
        }
        switch (p.kind)
        {
        case ACT_SELECT:
        {
            NC_STACK_ypabact *u = FindUnit(yw, p.gid);
            if (!u || u->_owner != yw->_userRobo->_owner)
                break;
            yw->_bactOnMouse = u;
            yw->ypaworld_func64__sub21__sub5(World::DOACTION_8); // original "select" (map / 3D click on own unit)
            fprintf(stderr, "UAVR cmd: DOACTION_8 select unit %u -> squad commander %u (cmd id %u)\n", p.gid,
                    (yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size()) ? yw->_cmdrsRemap[yw->_activeCmdrRemapIndex]->_gid : 0, yw->_activeCmdrID);
            restore();
            g_verify = Verify();
            g_verify.kind = ACT_SELECT;
            break;
        }
        case ACT_ORDER:
        {
            int fieldIdx = -1; // an order for a captured squad (vr_field.cpp)
            if (p.cmdr)
            {
                for (size_t i = 0; i < yw->_cmdrsRemap.size(); i++)
                    if (yw->_cmdrsRemap[i]->_gid == p.cmdr && yw->_cmdrsRemap[i]->_status != BACT_STATUS_DEAD &&
                        yw->_cmdrsRemap[i]->_owner == yw->_userRobo->_owner)
                        fieldIdx = (int)i;
                if (fieldIdx < 0)
                {
                    fprintf(stderr, "UAVR cmd: field order dropped, squad %u is gone\n", p.cmdr);
                    break;
                }
            }
            const int sIdx = yw->_activeCmdrRemapIndex;
            if (fieldIdx >= 0)
                yw->_activeCmdrRemapIndex = fieldIdx;
            if (yw->_activeCmdrRemapIndex < 0 || yw->_activeCmdrRemapIndex >= (int)yw->_cmdrsRemap.size())
            {
                Ticker(yw, "SELECT A SQUAD ON THE BOARD FIRST");
                fprintf(stderr, "UAVR cmd: order ignored, no squad selected\n");
                break;
            }
            const char *what;
            if (p.gid)
            {
                NC_STACK_ypabact *t = FindUnit(yw, p.gid);
                if (!t)
                {
                    yw->_activeCmdrRemapIndex = sIdx;
                    break;
                }
                yw->_guiActFlags = (sFlags & ~0x10u) | 0x20u; // a unit under the mouse
                yw->_bactOnMouse = t;
                what = "ATTACK UNIT";
            }
            else
            {
                yw_130arg a;
                a.pos_x = p.x;
                a.pos_z = p.z;
                if (!yw->GetSectorInfo(&a) || !a.pcell)
                {
                    yw->_activeCmdrRemapIndex = sIdx;
                    break;
                }
                yw->_guiActFlags = (sFlags | 0x10u) & ~0x20u; // a sector under the mouse
                yw->_cellOnMouse = a.pcell;
                yw->_cellMouseIsectPos = vec3d(p.x, a.pcell->height, p.z);
                what = (a.pcell->owner == yw->_userRobo->_owner && a.pcell->IsCanSee(yw->_userRobo->_owner)) ? "MOVE" : "ATTACK SECTOR";
            }
            NC_STACK_ypabact *cmdr = yw->_cmdrsRemap[yw->_activeCmdrRemapIndex];
            yw->ypaworld_func64__sub21__sub5(World::DOACTION_2); // original move / attack order
            fprintf(stderr, "UAVR cmd: DOACTION_2 (%s) squad %u -> %s at %.0f,%.0f | _updateMessage.user_action=%d target_Sect=%s target_Bact=%u\n",
                    what, cmdr->_gid, p.gid ? "unit" : "sector", p.x, p.z, yw->_updateMessage.user_action,
                    yw->_updateMessage.target_Sect ? "yes" : "no", yw->_updateMessage.target_Bact ? yw->_updateMessage.target_Bact->_gid : 0);
            restore();
            yw->_activeCmdrRemapIndex = sIdx; // the selection is not changed by a field order
            g_verify = Verify();
            g_verify.kind = ACT_ORDER;
            g_verify.gid = cmdr->_gid;
            break;
        }
        case ACT_BUILD:
        {
            if (!(bzda.field_1CC & 2) || p.slot < 0 || p.slot >= bzda.field_8E0)
            {
                Ticker(yw, "BUILD FROM THE HOST STATION");
                break;
            }
            NC_STACK_yparobo *robo = (NC_STACK_yparobo *)yw->_userRobo;
            const World::TVhclProto &proto = yw->_vhclProtos.at(bzda.field_2DC[p.slot]);
            const int energy = dround(yw->sub_4498F4() * (2 * proto.energy)); // = ypaworld_func64__sub21__sub7
            if (energy > robo->getROBO_battVehicle())                       // = sb_0x4d3d44 case 2
            {
                Ticker(yw, "NOT ENOUGH ENERGY");
                fprintf(stderr, "UAVR cmd: build slot %d refused: energy %d > %d\n", p.slot, energy, robo->getROBO_battVehicle());
                break;
            }
            // creation point: the original search (yw_MouseFindCreationPoint) for a cursor a
            // little below the centre of the station view
            TClickBoxInf ci;
            ci.move.ScreenPos = Common::Point(yw->_screenSize.x / 2, (int)(yw->_screenSize.y * 0.6));
            int bcx, bcy; // Command Deck: left of the war table, so the unit beams in in the front window
            if (Station::BuildCursor(yw->_screenSize.x, yw->_screenSize.y, &bcx, &bcy))
                ci.move.ScreenPos = Common::Point(bcx, bcy);
            const int s8EC = bzda.field_8EC, s8F8 = bzda.field_8F8, s1D0 = bzda.field_1D0;
            bzda.field_8EC = p.slot;
            bzda.field_8F8 = p.slot;
            if (!yw->yw_MouseFindCreationPoint(&ci))                         // = sb_0x4d3d44 case 1
            {
                bzda.field_8EC = s8EC;
                bzda.field_8F8 = s8F8;
                restore();
                Ticker(yw, "NO ROOM TO BUILD HERE");
                break;
            }
            if (yw->_isNetGame && !yw->_levelUnitLimitType &&                   // = sb_0x4d3d44 case 3 (hard unit limit)
                yw->_levelUnitLimit <= yw->_countUnitsPerOwner[yw->_userRobo->_owner])
            {
                bzda.field_8EC = s8EC;
                bzda.field_8F8 = s8F8;
                restore();
                Ticker(yw, "UNIT LIMIT REACHED");
                fprintf(stderr, "UAVR cmd: build slot %d refused: unit limit %d\n", p.slot, yw->_levelUnitLimit);
                break;
            }
            g_verify = Verify();
            g_verify.kind = ACT_BUILD;
            g_verify.energyBefore = robo->getROBO_battVehicle();
            g_verify.squadsBefore = yw->_cmdrsRemap.size();
            yw->_updateMessage.energy = energy;
            const bool add = p.add && yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size();
            // original "new squad" / "add to squad" (bar buttons 0 / right click)
            yw->ypaworld_func64__sub21__sub5(add ? World::DOACTION_ADD_UNIT2 : World::DOACTION_ADD_UNIT1);
            fprintf(stderr, "UAVR cmd: %s build slot %d '%s' proto %d cost %d at %.0f,%.0f,%.0f | energy %d\n", add ? "DOACTION_ADD_UNIT2 (add to squad)" : "DOACTION_ADD_UNIT1 (new squad)", p.slot,
                    yw->GetVehicleName(proto).c_str(), (int)bzda.field_2DC[p.slot], energy, yw->_updateMessage.target_point.x,
                    yw->_updateMessage.target_point.y, yw->_updateMessage.target_point.z, robo->getROBO_battVehicle());
            bzda.field_1D0 = s1D0; // the VR wheel does not leave the bar in "add to squad" mode
            restore();
            break;
        }
        case ACT_DIVE:
        case ACT_HOST:
        {
            NC_STACK_ypabact *u = p.kind == ACT_HOST ? yw->_userRobo : FindUnit(yw, p.gid);
            g_actionDone = true;
            if (!u || u == yw->_viewerBact)
                break;
            g_verify = Verify();
            g_verify.kind = p.kind;
            g_verify.viewerBefore = yw->_viewerBact->_gid;
            if (p.kind == ACT_HOST && yw->_playerInHSGun)
                bzda.field_1D0 = 1; // as the bar's "to host station" button
            yw->_bactOnMouse = u;
            yw->ypaworld_func64__sub21__sub5(World::DOACTION_5); // original "take control"
            fprintf(stderr, "UAVR cmd: DOACTION_5 take control of %u (%s), viewer was %u\n", u->_gid, p.kind == ACT_HOST ? "host station" : "dive", g_verify.viewerBefore);
            restore();
            break;
        }
        case ACT_SNAP:
        {
            NC_STACK_ypabact *r = yw->_userRobo;
            if (yw->_viewerBact != r)
                break;
            g_verify = Verify();
            g_verify.kind = ACT_SNAP;
            g_verify.angBefore = Deg(r->_viewer_horiz_angle);
            // the original station view turn (yparobo_func71__sub0): the stick moves
            // _viewer_horiz_angle (decreasing = turning right), clamped to the proto's side limit
            r->_viewer_horiz_angle -= Rad(p.snapDeg);
            if (r->_viewer_max_side < 3.15)
                r->_viewer_horiz_angle = std::max(-r->_viewer_max_side, std::min(r->_viewer_max_side, r->_viewer_horiz_angle));
            g_snapEpoch++;
            if (g_log) fprintf(stderr, "UAVR cmd: snap turn %+.0f deg\n", p.snapDeg);
            break;
        }
        default:
            break;
        }
    }
    (void)sVec;
    (void)sDist;
}

namespace Cmd
{
// field orders (vr_field.cpp): selections and orders go through the same queue and the
// same original user-action code (and result log) as the board's.
void QueueSelect(uint32_t gid)
{
    Pending p;
    p.kind = ACT_SELECT;
    p.gid = gid;
    g_queue.push_back(p);
}
void QueueOrder(uint32_t targetGid, double x, double z, uint32_t squadGid)
{
    Pending p;
    p.cmdr = squadGid;
    p.kind = ACT_ORDER;
    p.gid = targetGid;
    p.x = x;
    p.z = z;
    g_queue.push_back(p);
}
} // namespace Cmd

bool MapBoardOn() { return Cmd::g_boardOn; }
void SetMapBoardOn(bool on) { Cmd::g_boardOn = on; }
bool StationSnapTurn() { return Cmd::g_snapTurn; }
void SetStationSnapTurn(bool snap) { Cmd::g_snapTurn = snap; }
int StationSnapDeg() { return Cmd::g_snapDegSetting; }
void SetStationSnapDeg(int deg)
{
    Cmd::g_snapDegSetting = (deg == 20 || deg == 45) ? deg : 30;
    const char *v = getenv("UA_VR_SNAP_DEG");
    if (!(v && v[0]))
        Cmd::g_snapDeg = Cmd::g_snapDegSetting;
}
void CycleStationTurn()
{
    // SNAP 30 -> SNAP 45 -> SNAP 20 -> SMOOTH -> SNAP 30
    if (!Cmd::g_snapTurn)
    {
        Cmd::g_snapTurn = true;
        SetStationSnapDeg(30);
    }
    else if (Cmd::g_snapDegSetting == 30)
        SetStationSnapDeg(45);
    else if (Cmd::g_snapDegSetting == 45)
        SetStationSnapDeg(20);
    else
    {
        Cmd::g_snapTurn = false;
        SetStationSnapDeg(30);
    }
}
const char *StationTurnLabel()
{
    if (!Cmd::g_snapTurn)
        return "STATION TURN: SMOOTH";
    return Cmd::g_snapDegSetting == 45 ? "STATION TURN: SNAP 45" : (Cmd::g_snapDegSetting == 20 ? "STATION TURN: SNAP 20" : "STATION TURN: SNAP 30");
}

// the command layer is live but a view change / station beam is under way:
// a dive request is consumed and dropped (no fallback to the original instant DOACTION_5).
bool CommandDiveBlocked()
{
    using namespace Cmd;
    return g_enabled && Live() && (g_fade != FADE_NONE || Station::Beaming());
}

bool CommandDive(uint32_t gid, bool hostStation)
{
    using namespace Cmd;
    if (!g_enabled || !Live() || g_fade != FADE_NONE || Station::Beaming()) // not during a station beam
        return false;
    Pending p;
    if (hostStation)
    {
        if (g_snap.inStation)
            return false;
        p.kind = ACT_HOST;
    }
    else
    {
        const SUnit *u = FindSnap(gid);
        if (!u || !u->diveable || gid == g_snap.viewer)
            return false;
        p.kind = ACT_DIVE;
        p.gid = gid;
    }
    StartFade(p, false);
    if (g_log) fprintf(stderr, "UAVR cmd: double-click -> %s %u (fade out)\n", hostStation ? "host station" : "dive into", gid);
    return true;
}

const char *CommandCardLine(int kind, int line, const char *def)
{
    if (!Active())
        return def;
    if (kind == 0 && line == 4) // ground grips per Settings "GRIPS"
    {
        switch (GroundGripMode())
        {
        case GRIP_BRAKE: return "R grip: brake";
        case GRIP_OFF: return "Grips: off (Settings, VR controls)";
        default: return "R grip: forward   L grip: brake, reverse";
        }
    }
    if (!Cmd::g_enabled)
        return def;
    static const char *common[5] = {
        "A: dive into pointed unit / next unit",
        "B (hold): host station    L-click: HUD",
        "X: map board   Y: build wheel (hold: 2D)",
        "Board: trigger squad, then ground / enemy",
        "Windows/toolbar: R trig click, grip/B right",
    };
    if (kind != 2 && line == 7 && FieldOrders()) // field orders (vehicles)
        return SquadWheel() ? "X held + trigger: order  Y held: squads" : "X tap: board  X held + trigger: order";
    if (kind != 2 && line == 8 && SquadWheel()) // squad wheel (vehicles)
        return "Y wheel: R flick squad, L stick aggr";
    if (line == 7 && !Cmd::g_boardOn)
        return "X: 2D map     Y: build wheel (hold: 2D)";
    if (line >= 5 && line < 10)
        return common[line - 5];
    if (kind == 2 && line == 3)
        return Cmd::g_snapTurn ? (Cmd::g_snapDeg >= 44.5 ? "Left stick: snap turn 45 (or smooth)"
                                  : Cmd::g_snapDeg <= 20.5 ? "Left stick: snap turn 20 (or smooth)"
                                                           : "Left stick: snap turn 30 (or smooth)")
                               : "Left stick: turn the view";
    if (kind == 2 && line == 4)
        return "Y wheel: flick R stick, release = build";
    return def;
}

} // namespace VR

namespace VR
{
bool QuickStationPrompt(float *secLeft) { return Cmd::DeathPrompt(secLeft); } // (vr.h)
} // namespace VR
