// Urban Assault VR — "field": field orders, clickable wrist squads, squad
// wheel. See vr_field.h and
//
// Data flow (one engine frame):
//   FrameStart  -> Cmd::Filter -> Field::Filter   X / Y / trigger ownership, wrist hover,
//                                                 the right-hand ray in world space,
//                                                 orders / selects queued on the command
//                                                 layer (Cmd::QueueOrder / QueueSelect)
//   world update-> CommandWorldHook               the queued actions run through the
//                                                 original user-action code (DOACTION_2 / 8)
//   after Process -> Field::Gameplay              squads snapshot, aggression requests
//                                                 (original setter), the ray cast with the
//                                                 game's own 3D-click tests
//                                                 (ypaworld_func149 terrain / buildings,
//                                                 ypaworld_func150 units), result logs
//   eye pass    -> Field::DrawEye                 aim marker + beam, order flash, wrist
//                                                 hover beam, squad wheel
//
// Units are referenced by gid between frames (never by pointer).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_field.h"
#include "vr_field_logic.h"
#include "vr_sites_logic.h"
#include "vr_command.h"
#include "vr_cockpit.h"
#include "vr_gl.h"
#include "vr_overlay.h"
#include "../system/gfx.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"

namespace VR
{
bool Active();

namespace Field
{

namespace
{
using namespace FieldLogic;

double g_upm = 11.0;
bool g_log = false;
// ray length: SitesLogic::RayLen (6000 units as PickWorld; flyers 14000, sites)
bool g_flyer = false; // the viewer is a helicopter / jet (Command Deck: longer ray, far marker)

// ---- anchor (tracking -> world) ---------------------------------------------------
bool g_anchorValid = false;
M3 g_ra = M3::Ident();
double g_pa[3] = {0, 0, 0};

// ---- snapshot (after every gameplay frame) ---------------------------------------
struct Sq
{
    uint32_t gid = 0;
    std::string name;
    int n = 0;
    int aggr = 0; // 0..100
    bool sel = false, mine = false;
};
struct Snap
{
    bool valid = false;
    uint32_t ticks = 0;
    uint8_t me = 0;
    uint32_t viewer = 0, selCmdr = 0;
    bool inVehicle = false;
    std::vector<Sq> squads;
};
Snap g_snap;

// ---- the right-hand ray (world) and the last cast --------------------------------
bool g_rayWant = false;
double g_rayO[3] = {0, 0, 0}, g_rayD[3] = {0, 0, 1};
struct Hit
{
    bool valid = false;
    OrderKind kind = ORD_NONE;
    double p[3] = {0, 0, 0};
    uint32_t unit = 0;
    uint32_t ticks = 0;
};
Hit g_hit;

// ---- input state ---------------------------------------------------------------------
XState g_xs;
TrigState g_ts;
WheelState g_ws;
bool g_aiming = false;
int g_pulseX = 0, g_pulseY = 0;
uint32_t g_wristGid = 0;
double g_wristHit[3] = {0, 0, 0};
bool g_wheelOpen = false;
uint32_t g_wheelHlGid = 0;          // the highlighted squad by gid
std::vector<uint32_t> g_wheelGids;  // slot -> squad, frozen for one Y press
bool g_orderPending = false; // X+trigger: resolved after this frame's cast (Gameplay)
uint32_t g_orderSel = 0;
ControllerState g_L, g_R;

struct AggReq { uint32_t gid; int step; };
std::vector<AggReq> g_agg;

// ---- feedback / verification ---------------------------------------------------------
uint32_t g_flashT = 0;
double g_flashP[3] = {0, 0, 0};
bool g_flashAttack = false;
uint32_t g_trackGid = 0;
int g_trackFrames = 0;
int32_t g_trackT0 = 0; // game time of the first tracked frame (game time, not frames)
double g_trackTgt[3] = {0, 0, 0};
int g_weaponTime = -1, g_mgunTime = -1;
uint32_t g_weaponViewer = 0;

// ---- dev aid: scripted aim (UA_VR_FIELD_AIM) -----------------------------------------
struct AimKey { int f0 = 0, f1 = 0; std::string what; };
std::vector<AimKey> g_aimScript;
bool g_tgt[3] = {false, false, false}; // GROUND, ENEMY, ESECTOR
double g_tgtP[3][3];

// ---- textures ------------------------------------------------------------------------
GLuint g_wheelTex = 0;
uint32_t g_wheelTexTicks = 0;
std::vector<uint8_t> g_img;

double Rad(double d) { return d * M_PI / 180.0; }

bool SnapLive() { return g_snap.valid && SDL_GetTicks() - g_snap.ticks < 300; }
bool AnyOn() { return FieldOrders() || WristSquads() || SquadWheel(); }
int SquadIndex(uint32_t gid)
{
    for (size_t i = 0; gid && i < g_snap.squads.size(); i++)
        if (g_snap.squads[i].gid == gid)
            return (int)i;
    return -1;
}

// ---- geometry (same mappings as vr_command.cpp) ----------------------------------------
void RightRay(const ControllerState &r, double o[3], double d[3])
{
    const M3 m = QuatToM3(r.aimPose.orientation);
    o[0] = r.aimPose.position.x; o[1] = r.aimPose.position.y; o[2] = r.aimPose.position.z;
    d[0] = -m.m[0][2]; d[1] = -m.m[1][2]; d[2] = -m.m[2][2]; // aim forward = -Z
}

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

void AimAt(ControllerState &R, const double target[3])
{
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

// Dev aid UA_VR_FIELD_AIM="f0-f1:WHAT;..." (simulator): turns the right hand towards a
// target over poll frames f0..f1 so the real ray / cast / click path runs unattended.
// WHAT = GROUND (an own seen sector about 2 sectors away), ENEMY (the nearest visible
// enemy unit), ESECTOR (the nearest sector that is not the player's), WRIST:n
// (row n of the wrist SQUADS list), SKY (straight up).
void ScriptAim(ControllerState c[2], int poll)
{
    if (g_aimScript.empty())
        return;
    const AimKey *k = NULL;
    for (const AimKey &a : g_aimScript)
        if (poll >= a.f0 && poll <= a.f1)
            k = &a;
    if (!k)
        return;
    double target[3];
    bool ok = false;
    const int idx = k->what == "GROUND" ? 0 : k->what == "ENEMY" ? 1 : k->what == "ESECTOR" ? 2 : -1;
    if (idx >= 0 && g_tgt[idx] && g_anchorValid)
    {
        WorldToXr(g_tgtP[idx], target);
        ok = true;
    }
    else if (k->what.compare(0, 6, "WRIST:") == 0)
        ok = Cockpit::WristRowCentre(atoi(k->what.c_str() + 6), target);
    else if (k->what == "SKY")
    {
        target[0] = c[1].aimPose.position.x;
        target[1] = c[1].aimPose.position.y + 10.0;
        target[2] = c[1].aimPose.position.z - 0.5;
        ok = true;
    }
    if (!ok)
        return;
    static const AimKey *logged = NULL;
    if (g_log && logged != k)
    {
        logged = k;
        fprintf(stderr, "UAVR field: script aim %s -> tracking %.3f,%.3f,%.3f\n", k->what.c_str(), target[0], target[1], target[2]);
    }
    AimAt(c[1], target);
}

const char *KindName(OrderKind k)
{
    switch (k)
    {
    case ORD_MOVE: return "MOVE";
    case ORD_ATTACK_UNIT: return "ATTACK UNIT";
    case ORD_ATTACK_SECTOR: return "ATTACK SECTOR";
    default: return "NONE";
    }
}

void IssueOrder(uint32_t selCmdr)
{
    const uint32_t now = SDL_GetTicks();
    if (!selCmdr)
    {
        fprintf(stderr, "UAVR field: X+trigger: no squad selected at the pull, no order\n");
        return;
    }
    if (!g_hit.valid || now - g_hit.ticks > 250 || g_hit.kind == ORD_NONE)
    {
        fprintf(stderr, "UAVR field: X+trigger: no order target under the ray\n");
        return;
    }
    Cmd::QueueOrder(g_hit.kind == ORD_ATTACK_UNIT ? g_hit.unit : 0, g_hit.p[0], g_hit.p[2], selCmdr); // the squad current at the pull
    memcpy(g_flashP, g_hit.p, sizeof(g_flashP));
    g_flashT = now;
    g_flashAttack = g_hit.kind != ORD_MOVE;
    g_trackGid = selCmdr;
    g_trackFrames = 0;
    memcpy(g_trackTgt, g_hit.p, sizeof(g_trackTgt));
    fprintf(stderr, "UAVR field: X+trigger -> %s squad %u -> %s%u at %.0f,%.0f,%.0f (trigger kept from the game)\n", KindName(g_hit.kind), selCmdr,
            g_hit.kind == ORD_ATTACK_UNIT ? "unit " : "sector, unit ", g_hit.unit, g_hit.p[0], g_hit.p[1], g_hit.p[2]);
}

// ---- the game's own 3D-click tests along the controller ray ------------------------------
void Cast(NC_STACK_ypaworld *yw)
{
    g_hit = Hit();
    const vec3d o(g_rayO[0], g_rayO[1], g_rayO[2]);
    const double rayLen = SitesLogic::RayLen(g_flyer && CommandDeck());
    const vec3d v(g_rayD[0] * rayLen, g_rayD[1] * rayLen, g_rayD[2] * rayLen);
    ypaworld_arg136 a149;       // terrain + buildings (as yw_3D_MouseSelect)
    a149.stPos = o;
    a149.vect = v;
    a149.flags = 0;
    yw->ypaworld_func149(&a149);
    yw_arg150 a150;             // units
    a150.unit = yw->_viewerBact;
    a150.pos = o;
    a150.field_18 = v;
    a150.field_24 = NULL;
    a150.field_28 = 0;
    yw->ypaworld_func150(&a150);

    HitInfo h;
    const uint8_t me = yw->_userRobo->_owner;
    NC_STACK_ypabact *u = a150.field_24;
    if (u && u->_status != BACT_STATUS_CREATE && u->_status != BACT_STATUS_DEAD && u->_status != BACT_STATUS_BEAM &&
        u->_bact_type != BACT_TYPES_MISSLE)
    {
        h.unit = true;
        h.unitEnemy = u->_owner != me;
        // the player only orders against what he can see: seen sector, not stealthed
        h.unitVisible = u->_pSector && u->_pSector->IsCanSee(me) && !u->IsHiddenFor(me);
        h.unitDist = a150.field_28;
    }
    cellArea *cell = NULL;
    if (a149.isect)
    {
        yw_130arg sa;
        sa.pos_x = a149.isectPos.x;
        sa.pos_z = a149.isectPos.z;
        if (yw->GetSectorInfo(&sa) && sa.pcell)
        {
            cell = sa.pcell;
            h.ground = true;
            h.groundDist = (a149.isectPos - o).length();
            h.sectorBorder = cell->IsBorder() || sa.CellId.x <= 0 || sa.CellId.y <= 0 ||
                             sa.CellId.x >= yw->_mapSize.x - 1 || sa.CellId.y >= yw->_mapSize.y - 1;
            h.sectorOwn = cell->owner == me;
            h.sectorSeen = cell->IsCanSee(me);
        }
    }
    g_hit.kind = DecideOrder(h);
    g_hit.valid = true;
    g_hit.ticks = SDL_GetTicks();
    if (g_hit.kind == ORD_ATTACK_UNIT)
    {
        g_hit.unit = u->_gid;
        g_hit.p[0] = u->_position.x; g_hit.p[1] = u->_position.y; g_hit.p[2] = u->_position.z;
    }
    else if (h.ground)
    {
        g_hit.p[0] = a149.isectPos.x; g_hit.p[1] = a149.isectPos.y; g_hit.p[2] = a149.isectPos.z;
    }
    static int lastKind = -1;
    static uint32_t lastUnit = 0;
    if (g_log && ((int)g_hit.kind != lastKind || g_hit.unit != lastUnit))
    {
        fprintf(stderr, "UAVR field: ray -> %s (ground %s%s%s, unit %s%s) at %.0f,%.0f,%.0f\n", KindName(g_hit.kind),
                h.ground ? "yes" : "no", h.ground && h.sectorOwn ? " own" : "", h.ground && h.sectorBorder ? " border" : "",
                h.unit ? (h.unitEnemy ? "enemy" : "own") : "none", h.unit && !h.unitVisible ? " hidden" : "", g_hit.p[0], g_hit.p[1], g_hit.p[2]);
        lastKind = g_hit.kind;
        lastUnit = g_hit.unit;
    }
}

// Script targets for UA_VR_FIELD_AIM (world points, refreshed every gameplay frame).
void ScriptTargets(NC_STACK_ypaworld *yw)
{
    const uint8_t me = yw->_userRobo->_owner;
    const vec3d vp = yw->_viewerBact->_position;
    double bestG = 1e18, bestS = 1e18, bestE = 1e18;
    g_tgt[0] = g_tgt[1] = g_tgt[2] = false;
    const double L = World::CVSectorLength;
    for (cellArea &cell : yw->_cells)
    {
        const int cx = cell.CellId.x, cy = cell.CellId.y;
        if (cell.IsBorder() || cx <= 0 || cy <= 0 || cx >= yw->_mapSize.x - 1 || cy >= yw->_mapSize.y - 1)
            continue;
        const double x = (cx + 0.5) * L, z = -(cy + 0.5) * L;
        const double dist = sqrt((x - vp.x) * (x - vp.x) + (z - vp.z) * (z - vp.z));
        if (cell.owner == me && cell.IsCanSee(me))
        {
            const double score = fabs(dist - 2.0 * L);
            if (score < bestG)
            {
                bestG = score;
                g_tgt[0] = true;
                g_tgtP[0][0] = x; g_tgtP[0][1] = cell.height; g_tgtP[0][2] = z;
            }
        }
        else if (cell.owner != me && dist < bestS)
        {
            bestS = dist;
            g_tgt[2] = true;
            g_tgtP[2][0] = x; g_tgtP[2][1] = cell.height; g_tgtP[2][2] = z;
        }
    }
    // ESECTOR: of the nearest non-own sectors, the first whose centre the hand can see
    // (the game's terrain test from the ray origin hits a non-own sector)
    static bool esFound = false;
    static double esP[3];
    static uint32_t esViewer = 0;
    if (esViewer != yw->_viewerBact->_gid)
    {
        esViewer = yw->_viewerBact->_gid;
        esFound = false;
    }
    if (g_tgt[2] && esFound) // keep the last one in sight (the aim moves the ray origin a little)
        memcpy(g_tgtP[2], esP, sizeof(esP));
    if (g_tgt[2] && g_anchorValid && !esFound)
    {
        std::vector<std::pair<double, cellArea *>> cand;
        for (cellArea &cell : yw->_cells)
        {
            const int cx = cell.CellId.x, cy = cell.CellId.y;
            if (cell.owner == me || cell.IsBorder() || cx <= 0 || cy <= 0 || cx >= yw->_mapSize.x - 1 || cy >= yw->_mapSize.y - 1)
                continue;
            const double x = (cx + 0.5) * L, z = -(cy + 0.5) * L;
            cand.push_back({(x - vp.x) * (x - vp.x) + (z - vp.z) * (z - vp.z), &cell});
        }
        std::sort(cand.begin(), cand.end(), [](const std::pair<double, cellArea *> &a, const std::pair<double, cellArea *> &b) { return a.first < b.first; });
        bool found = false;
        for (size_t i = 0; i < cand.size() && i < 100 && !found; i++)
        {
            cellArea *cell = cand[i].second;
            const double x = (cell->CellId.x + 0.5) * L, z = -(cell->CellId.y + 0.5) * L;
            ypaworld_arg136 a;
            a.stPos = vec3d(g_rayO[0], g_rayO[1], g_rayO[2]);
            a.vect = vec3d(x - g_rayO[0], cell->height - g_rayO[1], z - g_rayO[2]) * 1.05;
            a.flags = 0;
            yw->ypaworld_func149(&a);
            yw_130arg sa;
            sa.pos_x = a.isectPos.x;
            sa.pos_z = a.isectPos.z;
            if (a.isect && yw->GetSectorInfo(&sa) && sa.pcell && sa.pcell->owner != me)
            {
                g_tgtP[2][0] = a.isectPos.x; g_tgtP[2][1] = a.isectPos.y; g_tgtP[2][2] = a.isectPos.z;
                memcpy(esP, g_tgtP[2], sizeof(esP));
                found = esFound = true;
            }
        }
        static int lastFound = -1;
        if (g_log && lastFound != (int)found)
        {
            lastFound = found;
            fprintf(stderr, "UAVR field: script: ESECTOR %s (%d candidates)\n", found ? "in sight" : "none in sight", (int)cand.size());
        }
    }
    static bool enemyLogged = false;
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        auto consider = [&](NC_STACK_ypabact *u)
        {
            if (u->_owner == me || u->_status == BACT_STATUS_DEAD || u->_bact_type == BACT_TYPES_MISSLE ||
                !u->_pSector || !u->_pSector->IsCanSee(me) || u->IsHiddenFor(me))
                return;
            const double d2 = (u->_position - vp).length();
            if (d2 < bestE)
            {
                bestE = d2;
                g_tgt[1] = true;
                g_tgtP[1][0] = u->_position.x; g_tgtP[1][1] = u->_position.y; g_tgtP[1][2] = u->_position.z;
                if (!enemyLogged && g_log)
                {
                    enemyLogged = true;
                    fprintf(stderr, "UAVR field: script: first visible enemy unit %u (owner %d) at %.0f,%.0f\n", u->_gid, u->_owner, u->_position.x, u->_position.z);
                }
            }
        };
        consider(r);
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            consider(c);
            for (NC_STACK_ypabact *k : c->_kidList)
                consider(k);
        }
    }
}

// ---- tiny 5x7 font + image helpers (as vr_command.cpp's board / wheel) ------------------
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
                         dot[7] = {0,0,0,0,0,0x0C,0x0C}, gt[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
    if (ch == '-') return dash;
    if (ch == ':') return colon;
    if (ch == '.') return dot;
    if (ch == '>') return gt;
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
    void Pips(int cx, int y, int level, int pw, int ph, int gap)
    {
        const int total = 5 * pw + 4 * gap, x0 = cx - total / 2;
        for (int i = 0; i < 5; i++)
        {
            const bool on = i < level;
            const uint8_t r = on ? (uint8_t)(120 + 27 * i) : 40, g = on ? (uint8_t)(220 - 35 * i) : 45, b = on ? 70 : 45;
            Rect(x0 + i * (pw + gap), y, x0 + i * (pw + gap) + pw, y + ph, r, g, b, 255);
        }
    }
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
    GLint prevAlign = 4; // restore the caller's unpack state
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
}

// The squad index the wheel's aggression / centre refers to: highlighted, else current.
int WheelFocus()
{
    if (g_wheelHlGid) // highlighted: that squad or nothing (never another one)
        return SquadIndex(g_wheelHlGid);
    for (size_t i = 0; i < g_snap.squads.size(); i++)
        if (g_snap.squads[i].sel)
            return (int)i;
    return -1;
}

// Squad wheel image: squads clockwise from the top (the squad manager's order); name,
// unit count, aggression pips; current squad ringed white, yours named green, the
// highlighted one bright; centre = the focused squad's aggression 1..5.
void RenderWheel()
{
    const int W = 512;
    g_img.assign((size_t)W * W * 4, 0);
    Img im{W, W, g_img.data()};
    im.Disc(256, 256, 250, 10, 12, 16, 200);
    im.Disc(256, 256, 250, 120, 160, 220, 255, 246);
    const int n = (int)g_wheelGids.size();
    if (n == 0)
        im.TextC(256, 236, "NO SQUADS", 3, 220, 220, 220);
    const double ring = n > 6 ? 178 : 162;
    const double sr = std::min(76.0, M_PI * ring / std::max(1, n) * 0.92);
    for (int i = 0; i < n; i++)
    {
        const int si = SquadIndex(g_wheelGids[i]);
        const double a = 2 * M_PI * i / n;
        if (si < 0) // gone since the wheel opened: an empty slot
        {
            im.Disc(256 + sin(a) * ring, 256 - cos(a) * ring, sr, 60, 60, 60, 255, sr - 2);
            continue;
        }
        const Sq &q = g_snap.squads[si];
        const double cx = 256 + sin(a) * ring, cy = 256 - cos(a) * ring;
        const bool hl = q.gid == g_wheelHlGid;
        if (q.sel)
            im.Disc(cx, cy, sr + 6, 255, 255, 255, 255, sr + 1);
        im.Disc(cx, cy, sr, hl ? 50 : 22, hl ? 110 : 40, hl ? 170 : 70, 235);
        im.Disc(cx, cy, sr, hl ? 255 : 110, hl ? 255 : 150, hl ? 255 : 210, 255, sr - (hl ? 4 : 2));
        const int sc = sr > 60 ? 2 : 1;
        im.TextC((int)cx, (int)cy - 9 * sc - 4, q.name.substr(0, 8), sc, q.mine ? 140 : 235, q.mine ? 255 : 235, q.mine ? 150 : 235);
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", q.n);
        im.TextC((int)cx, (int)cy - 2, cnt, sc + 1, 235, 235, 235);
        im.Pips((int)cx, (int)cy + 8 * (sc + 1), AggrLevel(q.aggr), 5 * sc, 4 * sc, 2 * sc);
    }
    im.Disc(256, 256, 66, 0, 0, 0, 225);
    const int f = WheelFocus();
    if (f >= 0)
    {
        const Sq &q = g_snap.squads[f];
        im.TextC(256, 210, q.name.substr(0, 8), 2, 220, 230, 255);
        im.TextC(256, 234, "AGGR", 2, 200, 210, 220);
        char lv[8];
        snprintf(lv, sizeof(lv), "%d", AggrLevel(q.aggr));
        im.TextC(256, 254, lv, 3, 255, 230, 120);
        im.Pips(256, 284, AggrLevel(q.aggr), 14, 8, 4);
    }
    else
        im.TextC(256, 244, "SQUADS", 2, 200, 210, 220);
    im.TextC(256, 478, "L STICK: AGGR", 2, 170, 190, 210);
    Upload(&g_wheelTex, W, W, g_img.data());
}

// ---- eye-space helpers (as vr_command.cpp) ---------------------------------------------
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
    if (a[2] <= 1e-3 || b[2] <= 1e-3)
        return false;
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

// The squad wheel: 0.26 m, just above / in front of the left hand, facing the head.
bool WheelQuad(const ViewState &vs, int eye, const M4 &proj, float q[4][4])
{
    double c[3];
    if (g_L.active)
    {
        c[0] = g_L.gripPose.position.x + 0.04;
        c[1] = g_L.gripPose.position.y + 0.15;
        c[2] = g_L.gripPose.position.z - 0.10;
    }
    else
    {
        c[0] = -0.12; c[1] = -0.22; c[2] = -0.45;
    }
    double n[3] = {vs.head.position.x - c[0], vs.head.position.y - c[1], vs.head.position.z - c[2]};
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-6)
        return false;
    for (int i = 0; i < 3; i++) n[i] /= l;
    double ex[3] = {n[2], 0, -n[0]}; // up x n
    l = sqrt(ex[0] * ex[0] + ex[2] * ex[2]);
    if (l < 1e-6)
        return false;
    ex[0] /= l; ex[2] /= l;
    const double ey[3] = {n[1] * ex[2] - n[2] * ex[1], n[2] * ex[0] - n[0] * ex[2], n[0] * ex[1] - n[1] * ex[0]};
    const double hw = 0.13;
    static const double uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; k++)
    {
        double xr[3], e[3];
        for (int i = 0; i < 3; i++)
            xr[i] = c[i] + ex[i] * uv[k][0] * hw + ey[i] * uv[k][1] * hw;
        ToEye(vs, eye, xr, e);
        if (!Clip(proj, e, q[k]))
            return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------

void Init(double upm)
{
    g_upm = upm;
    auto on = [](const char *n) { const char *v = getenv(n); return v && v[0] && strcmp(v, "0") != 0; };
    g_log = on("UA_VR_FIELD_LOG") || on("UA_VR_CMD_LOG");
    if (const char *v = getenv("UA_VR_FIELD_AIM"))
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
}

void SetAnchor(const M3 &ra, const double pa[3])
{
    g_ra = ra;
    memcpy(g_pa, pa, sizeof(g_pa));
    g_anchorValid = true;
}

Out Filter(ControllerState c[2], const In &in)
{
    Out out;
    if (AnyOn())
        ScriptAim(c, in.poll);
    ControllerState &L = c[0], &R = c[1];
    g_L = L;
    g_R = R;
    const uint32_t now = SDL_GetTicks();
    const bool veh = in.inVehicle && SnapLive() && g_snap.inVehicle;
    const bool fieldOrders = veh && FieldOrders();
    const bool wristOn = veh && WristSquads();
    const bool wheelOn = veh && SquadWheel();

    // ---- X: held = order mode; tap = the old X ----
    const bool xDown = (L.buttons & BTN_X) != 0;
    bool aiming = false;
    const XEvent xe = UpdateX(g_xs, xDown, fieldOrders, now, &aiming);
    if (g_xs.owned || xe != XE_NONE)
    {
        out.ownsX = true;
        L.buttons &= ~BTN_X;
    }
    if (xe == XE_TAP)
    {
        if (in.boardOn)
            out.toggleBoard = true;
        else
            g_pulseX = 3; // the original 2D map (hotkey 8) via winp
    }
    if (xe != XE_NONE && g_log)
        fprintf(stderr, "UAVR field: X %s\n", xe == XE_TAP ? "tap (old X)" : "released, aim cancelled");
    aiming = aiming && !in.boardHit && !in.busy;
    if (aiming != g_aiming && g_log)
        fprintf(stderr, "UAVR field: order mode %s\n", aiming ? "on" : "off");
    g_aiming = aiming;

    // ---- right ray: world (for the cast after this frame), wrist rows ----
    double o[3], d[3];
    RightRay(R, o, d);
    g_rayWant = (aiming || !g_aimScript.empty()) && g_anchorValid && R.active;
    if (g_rayWant)
        XrRayToWorld(o, d, g_rayO, g_rayD);
    uint32_t wg = 0;
    double wh[3], wd = 0;
    const bool wristHover = wristOn && R.active && !in.busy && Cockpit::WristPick(o, d, &wg, wh, &wd);
    if (wristHover != (g_wristGid != 0) && g_log)
        fprintf(stderr, "UAVR field: wrist hover %s %u\n", wristHover ? "on squad" : "off", wg);
    g_wristGid = wristHover ? wg : 0;
    if (wristHover)
        memcpy(g_wristHit, wh, sizeof(wh));
    Cockpit::SetWristHover(g_wristGid);

    // ---- R trigger: a squeeze begun in order mode / on the wrist never reaches the game ----
    bool click = false;
    const float trigIn = R.trigger;
    if (UpdateTrig(g_ts, aiming || wristHover, R.trigger, &click))
        R.trigger = 0;
    // the board neither hovers nor clicks while the ray is on the wrist or a field-owned
    // squeeze is held (it may have left the wrist / X may be up by now)
    if (wristHover || g_ts.owned)
        out.blockBoard = true;
    if (click)
    {
        if (wristHover)
        {
            const WristAct a = WristClick(wg, in.selCmdr, true);
            if (a == WR_SELECT)
                Cmd::QueueSelect(wg);
            const bool dove = a == WR_DIVE && CommandDive(wg, false);
            fprintf(stderr, "UAVR field: wrist click squad %u (current %u) -> %s\n", wg, in.selCmdr,
                    a == WR_SELECT ? "select" : (a == WR_DIVE ? (dove ? "dive into its leader" : "dive refused (already there / busy)") : "none"));
        }
        else if (aiming)
        {
            g_xs.ordered = true;
            g_orderPending = true; // this poll's ray is cast after the frame (Gameplay)
            g_orderSel = in.selCmdr;
        }
    }
    if (g_log && g_ts.owned && trigIn > 0.2f)
    {
        static uint32_t lastLog = 0;
        if (now - lastLog > 250)
        {
            lastLog = now;
            fprintf(stderr, "UAVR field: trigger %.2f held by the field layer (game sees %.2f)\n", trigIn, R.trigger);
        }
    }

    // ---- Y: squad wheel (vehicle) ----
    const bool yDown = (L.buttons & BTN_Y) != 0;
    if (yDown && !g_ws.down) // a new press: the wheel's slots are fixed until it ends
    {
        g_wheelGids.clear();
        for (const Sq &q : g_snap.squads)
            g_wheelGids.push_back(q.gid);
        g_wheelHlGid = 0;
    }
    const WheelOut wo = UpdateWheel(g_ws, yDown, wheelOn && !in.busy, R.stickX, R.stickY, L.stickY, (int)g_wheelGids.size(), now);
    if (g_ws.owned)
    {
        out.ownsY = true;
        L.buttons &= ~BTN_Y;
        if (g_ws.hl >= 0 && g_ws.hl < (int)g_wheelGids.size())
            g_wheelHlGid = g_wheelGids[g_ws.hl];
    }
    else if (!wo.open && wo.select < 0)
        g_wheelHlGid = 0;
    if (wo.open != g_wheelOpen && g_log)
        fprintf(stderr, "UAVR field: squad wheel %s (%d squads)\n", wo.open ? "open" : "closed", (int)g_snap.squads.size());
    g_wheelOpen = wo.open;
    if (wo.open)
    {
        R.stickX = R.stickY = 0; // the wheel owns the right stick ...
        L.stickY = 0;           // ... and the left stick's up / down (aggression)
    }
    if (wo.select >= 0)
    {
        const uint32_t gid = g_wheelHlGid;
        if (SquadIndex(gid) < 0)
            fprintf(stderr, "UAVR field: wheel: the highlighted squad is gone, nothing selected\n");
        else
        {
            if (gid != in.selCmdr)
                Cmd::QueueSelect(gid);
            fprintf(stderr, "UAVR field: wheel -> squad %u (slot %d, %s)\n", gid, SquadIndex(gid), gid != in.selCmdr ? "select" : "already current");
        }
        if (!wo.open)
            g_wheelHlGid = 0;
    }
    if (wo.aggStep)
    {
        const int f = WheelFocus();
        if (f >= 0)
            g_agg.push_back(AggReq{g_snap.squads[f].gid, wo.aggStep});
    }
    if (wo.tap)
    {
        g_pulseY = 3; // the original squad manager (hotkey 9) via winp
        if (g_log) fprintf(stderr, "UAVR field: Y tap -> original squad manager\n");
    }

    // ---- pulses: the original hotkeys after a tap ----
    if (g_pulseX > 0)
    {
        L.buttons |= BTN_X;
        g_pulseX--;
        out.ownsX = true;
    }
    if (g_pulseY > 0)
    {
        L.buttons |= BTN_Y;
        g_pulseY--;
        out.ownsY = true;
    }
    return out;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!Active() || !yw || !yw->_userRobo || !yw->_viewerBact || yw->_mapSize.x <= 0)
    {
        g_snap.valid = false;
        g_anchorValid = false;
        g_agg.clear();
        g_orderPending = false;
        return;
    }
    if (!AnyOn()) // Classic defaults: nothing to do (input releases are handled in Filter)
    {
        g_snap.valid = false;
        g_agg.clear();
        g_orderPending = false;
        g_trackGid = 0;
        g_rayWant = false;
        g_hit.valid = false;
        g_weaponViewer = 0;
        return;
    }
    Snap &s = g_snap;
    s.me = yw->_userRobo->_owner;
    s.viewer = yw->_viewerBact->_gid;
    s.inVehicle = yw->_viewerBact != yw->_userRobo && !yw->_playerInHSGun;
    {
        const int bt = yw->_viewerBact->_bact_type;
        const bool flyer = s.inVehicle && (bt == BACT_TYPES_BACT || bt == BACT_TYPES_FLYER || bt == BACT_TYPES_UFO);
        if (flyer != g_flyer && g_log)
            fprintf(stderr, "UAVR field: viewer %s (ray %.0f units%s)\n", flyer ? "is a flyer" : "is not a flyer",
                    SitesLogic::RayLen(flyer && CommandDeck()), flyer && CommandDeck() ? ", far marker" : "");
        g_flyer = flyer;
        // Dev aid UA_VR_AIR_ALT=<units> (simulator, Command Deck): hold a flyer viewer at
        // this altitude over the ground below it, so the look-down order path can be
        // proven unattended (the sim profile's throttle is not bound to the grips). Never
        // set in a real session.
        static double airAlt = (getenv("UA_VR_AIR_ALT") && SitesLogic::SimBackend(getenv("UA_VR"))) ? atof(getenv("UA_VR_AIR_ALT")) : 0.0; // simulator only
        if (airAlt > 0 && flyer && CommandDeck())
        {
            NC_STACK_ypabact *vb = yw->_viewerBact;
            yw_130arg sa;
            sa.pos_x = vb->_position.x;
            sa.pos_z = vb->_position.z;
            if (yw->GetSectorInfo(&sa) && sa.pcell)
            {
                static bool logged = false;
                if (!logged && g_log)
                {
                    logged = true;
                    fprintf(stderr, "UAVR field: dev UA_VR_AIR_ALT: viewer %u held %.0f units above the ground\n", vb->_gid, airAlt);
                }
                vb->_position.y = sa.pcell->height - airAlt;
                vb->_old_pos.y = vb->_position.y;
                vb->_fly_dir.y = 0;
            }
        }
    }
    const bool selOk = yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size();
    s.selCmdr = selOk ? yw->_cmdrsRemap[yw->_activeCmdrRemapIndex]->_gid : 0;
    s.squads.clear();
    for (NC_STACK_ypabact *cmd : yw->_cmdrsRemap)
    {
        if (cmd->_status == BACT_STATUS_DEAD || cmd->_status == BACT_STATUS_BEAM || cmd->_bact_type == BACT_TYPES_GUN || s.squads.size() >= 12)
            continue;
        Sq q;
        q.gid = cmd->_gid;
        q.name = cmd->_vehicleID < yw->_vhclProtos.size() ? yw->GetVehicleName(yw->_vhclProtos[cmd->_vehicleID]) : "?";
        for (char &ch : q.name)
            if (ch >= 'a' && ch <= 'z') ch -= 32;
        q.n = 1;
        for (NC_STACK_ypabact *u : cmd->_kidList)
            if (u->_status != BACT_STATUS_DEAD)
                q.n++;
        q.aggr = cmd->_aggr;
        q.sel = cmd->_gid == s.selCmdr;
        q.mine = cmd == yw->_viewerBact || std::find(cmd->_kidList.begin(), cmd->_kidList.end(), yw->_viewerBact) != cmd->_kidList.end();
        s.squads.push_back(q);
    }
    s.valid = true;
    s.ticks = SDL_GetTicks();

    // aggression from the wheel: the squad manager's own setter (25 * step) + its click
    for (const AggReq &a : g_agg)
        for (NC_STACK_ypabact *cmd : yw->_cmdrsRemap)
            if (cmd->_gid == a.gid && cmd->_status != BACT_STATUS_DEAD)
            {
                const int before = AggrLevel(cmd->_aggr), after = std::max(1, std::min(5, before + a.step));
                if (after != before)
                {
                    cmd->setBACT_aggression(AggrValue(after));
                    if (yw->_GameShell)
                        SFXEngine::SFXe.startSound(&yw->_GameShell->samples1_info, 3);
                }
                fprintf(stderr, "UAVR field: wheel aggression squad %u: %d -> %d (_aggr %d)\n", a.gid, before, after, (int)cmd->_aggr);
                break;
            }
    g_agg.clear();

    if (!g_aimScript.empty())
        ScriptTargets(yw);
    if (g_rayWant)
        Cast(yw);
    else
        g_hit.valid = false;
    if (g_orderPending)
    {
        g_orderPending = false;
        if (g_flyer && CommandDeck() && g_hit.valid)
        {
            // order from the air - altitude above the ground under the viewer, range
            const NC_STACK_ypabact *vb = yw->_viewerBact;
            yw_130arg sa;
            sa.pos_x = vb->_position.x;
            sa.pos_z = vb->_position.z;
            const double ground = (yw->GetSectorInfo(&sa) && sa.pcell) ? sa.pcell->height : vb->_position.y;
            const double dx = g_hit.p[0] - g_rayO[0], dy = g_hit.p[1] - g_rayO[1], dz = g_hit.p[2] - g_rayO[2];
            fprintf(stderr, "UAVR field: order from the air: viewer %u altitude %.0f units (%.0f m), ray %.0f units (%.0f m), look-down %.0f deg\n",
                    vb->_gid, ground - vb->_position.y, (ground - vb->_position.y) / g_upm, sqrt(dx * dx + dy * dy + dz * dz),
                    sqrt(dx * dx + dy * dy + dz * dz) / g_upm, atan2(dy, sqrt(dx * dx + dz * dz)) * 180.0 / M_PI);
        }
        IssueOrder(g_orderSel);
    }

    // order result: the squad leader's way to the target (positions, every ~0.5 s for 10 s)
    if (g_trackGid)
    {
        g_trackFrames++;
        if (g_trackFrames == 1)
            g_trackT0 = yw->_timeStamp;
        if (g_trackFrames % 30 == 1)
        {
            for (NC_STACK_ypabact *cmd : yw->_cmdrsRemap)
                if (cmd->_gid == g_trackGid)
                {
                    const double dx = cmd->_position.x - g_trackTgt[0], dz = cmd->_position.z - g_trackTgt[2];
                    fprintf(stderr, "UAVR field: TRACK squad %u at %.0f,%.0f (target %.0f,%.0f, %.0f away)%s\n", g_trackGid, cmd->_position.x,
                            cmd->_position.z, g_trackTgt[0], g_trackTgt[2], sqrt(dx * dx + dz * dz), cmd == yw->_viewerBact ? " (you)" : "");
                    break;
                }
        }
        if (yw->_timeStamp - g_trackT0 > (g_flyer && CommandDeck() ? 30000 : 10000) || yw->_timeStamp < g_trackT0) // longer from the air (the squad routes around blocks)
            g_trackGid = 0;
    }

    // fire check: every shot of the viewer (weapon / machine gun) is logged
    NC_STACK_ypabact *vb = yw->_viewerBact;
    if (vb->_gid != g_weaponViewer)
    {
        g_weaponViewer = vb->_gid;
        g_weaponTime = vb->_weapon_time;
        g_mgunTime = vb->_mgun_time;
    }
    if (g_log && (vb->_weapon_time != g_weaponTime || vb->_mgun_time != g_mgunTime))
        fprintf(stderr, "UAVR field: FIRE viewer %u shot (%s)%s\n", vb->_gid, vb->_weapon_time != g_weaponTime ? "weapon" : "machine gun",
                g_ts.owned ? " WHILE THE FIELD LAYER HELD THE TRIGGER" : "");
    g_weaponTime = vb->_weapon_time;
    g_mgunTime = vb->_mgun_time;
}

void DrawEye(int eye, const ViewState &views, double zn, double zf, bool diorama)
{
    if (!SnapLive())
        return;
    const uint32_t now = SDL_GetTicks();
    // the full-scale world is not drawn under the miniature: no aim marker / flash
    const bool marker = g_aiming && g_anchorValid && !diorama;
    const bool flash = g_flashT && now - g_flashT < 500 && g_anchorValid && !diorama;
    const bool wrist = g_wristGid != 0;
    const bool wheel = g_wheelOpen && SnapLive();
    if (!marker && !flash && !wrist && !wheel)
        return;
    if (!Overlay::Begin())
        return;
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    float q[4][4];
    const double hand[3] = {g_R.aimPose.position.x, g_R.aimPose.position.y, g_R.aimPose.position.z};
    if (marker)
    {
        const bool hit = g_hit.valid && now - g_hit.ticks < 250 && g_hit.kind != ORD_NONE;
        float r = 0.6f, g = 0.6f, b = 0.6f;
        if (hit && g_hit.kind == ORD_MOVE) { r = 0.35f; g = 1.0f; b = 0.45f; }
        else if (hit) { r = 1.0f; g = 0.25f; b = 0.2f; }
        double end[3];
        if (hit)
            WorldToXr(g_hit.p, end);
        else
        {
            double o[3], d[3];
            RightRay(g_R, o, d);
            for (int i = 0; i < 3; i++)
                end[i] = o[i] + d[i] * 30.0;
        }
        if (Beam(views, eye, proj, hand, end, 0.0015, q))
            Overlay::Quad(q, Overlay::STYLE_BEAM, r, g, b, hit ? 0.55f : 0.3f);
        if (hit)
        {
            double e[3];
            ToEye(views, eye, end, e);
            const double dist = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            // the marker grows with distance (readable far below a helicopter)
            // far-marker growth and pin: COMMAND DECK only (Classic + FIELD ORDERS keeps the field orders' 2.2 deg)
            const double angDeg = CommandDeck() ? SitesLogic::MarkerAngleDeg(dist) : 2.2;
            const double rad = std::max(0.02, dist * tan(Rad(angDeg)));
            const bool mq = MarkQuad(views, eye, proj, end, rad, q);
            if (mq)
                Overlay::Quad(q, Overlay::STYLE_RETICLE, r, g, b, 0.95f);
            static uint32_t lastMk = 0;
            if (g_log && eye == 0 && now - lastMk > 400)
            {
                lastMk = now;
                fprintf(stderr, "UAVR field: marker %.0f m from the eye, radius %.1f m (%.1f deg), eye yaw %+.0f pitch %+.0f deg\n", dist, rad,
                        angDeg, atan2(e[0], e[2]) * 180.0 / M_PI, atan2(-e[1], sqrt(e[0] * e[0] + e[2] * e[2])) * 180.0 / M_PI);
            }
            // ... and far spots get a vertical pin (stands out over roofs)
            const double pinM = CommandDeck() ? SitesLogic::PinHeightM(dist) : 0.0;
            if (pinM > 0)
            {
                const double topW[3] = {g_hit.p[0], g_hit.p[1] - pinM * g_upm, g_hit.p[2]}; // world: up = -y
                double top[3];
                WorldToXr(topW, top);
                if (Beam(views, eye, proj, end, top, std::max(0.01, dist * tan(Rad(0.35))), q))
                    Overlay::Quad(q, Overlay::STYLE_BEAM, r, g, b, 0.7f);
            }
        }
    }
    if (flash)
    {
        const float t = (now - g_flashT) / 500.0f;
        double end[3], e[3];
        WorldToXr(g_flashP, end);
        ToEye(views, eye, end, e);
        const double dist = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (MarkQuad(views, eye, proj, end, std::max(0.03, dist * tan(Rad(2.5 + 5.0 * t))), q))
            Overlay::Quad(q, Overlay::STYLE_RETICLE, 1.0f, g_flashAttack ? 0.55f : 1.0f, g_flashAttack ? 0.45f : 1.0f, 1.0f - t);
    }
    if (wrist)
    {
        if (MarkQuad(views, eye, proj, g_wristHit, 0.003, q))
            Overlay::Quad(q, Overlay::STYLE_DOT, 0.5f, 0.85f, 1.0f, 0.95f);
        if (Beam(views, eye, proj, hand, g_wristHit, 0.001, q))
            Overlay::Quad(q, Overlay::STYLE_BEAM, 0.5f, 0.85f, 1.0f, 0.6f);
    }
    if (wheel)
    {
        if (eye == 0 && (now - g_wheelTexTicks > 100 || !g_wheelTex))
        {
            RenderWheel();
            g_wheelTexTicks = now;
        }
        if (g_wheelTex && WheelQuad(views, eye, proj, q))
            Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, 0.96f, g_wheelTex);
    }
    Overlay::End();
}

} // namespace Field
} // namespace VR
