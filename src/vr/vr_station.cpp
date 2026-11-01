// Urban Assault VR — "station": 2D windows at the station, build by hand,
// beam the station. See vr_station.h and
//
// Data flow (one engine frame):
//   FrameStart   -> Cmd::Filter -> Table::Input   host token carry / drop / confirm ->
//                                                 Station::PreviewBeam / ConfirmBeam
//   world update -> CommandWorldHook -> Station::WorldHook
//                                                 confirmed beam: the original checks
//                                                 (ypaworld_func64__sub21__sub3) and the
//                                                 original order (DOACTION_10)
//   after Process -> Station::Gameplay            2D windows, new units of the own host
//                                                 station, beam progress (fade timing),
//                                                 beam preview for the table ring
//   eye pass     -> DrawWorldFade (before the cockpit / bridge), DrawEye (effects)
//
// Units are referenced by gid between frames (never by pointer).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_station.h"
#include "vr_table.h"
#include "vr_gl.h"
#include "vr_overlay.h"
#include "../system/gfx.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"

extern bzd bzda;               // yw_game_ui.cpp: the bottom bar
extern tehMap robo_map;         // yw_game_ui.cpp: the 2D map window
extern squadMan squadron_manager; // yw_game_ui.cpp: the 2D squadron manager window

namespace VR
{
bool Active();

namespace Station
{
namespace
{

bool g_log = false;
bool g_enabled = true; // UA_VR_STATION=0 turns the whole station feature off (A/B)
double g_upm = 1.0;
M3 g_ra = M3::Ident();
double g_pa[3] = {0, 0, 0};
bool g_anchorValid = false;
float g_cursorFx = 0.33f, g_cursorFy = 0.36f; // build creation cursor (screen fractions)

// ---- 1. 2D windows at the station ---------------------------------------------------------
struct Win
{
    bool autoClosed = false; // we closed it on entering the station (saved state: open)
    bool wish = false;       // the player opened it at the station himself: leave it alone
};
Win g_map, g_sq;
bool g_wasStation = false;
uint32_t g_winRobo = 0;

// ---- 2. built units -------------------------------------------------------------------------
const uint32_t kBeamInMs = 1400, kMarkMs = 8000, kNoticeMs = 8000;
struct Built
{
    uint32_t gid = 0;
    double w[3] = {0, 0, 0}; // current world position (updated per gameplay frame)
    uint32_t t0 = 0;         // SDL ticks when it appeared
    bool alive = true;
    int logged = -1; // last logged second (UA_VR_STATION_LOG)
    bool dioLogged = false; // roll-out shown on the miniature (logged once)
};
std::vector<Built> g_built;
std::vector<uint32_t> g_known; // sorted gids of the own host station's units
uint32_t g_knownRobo = 0;

std::string g_notice;
int g_noticeCol = 0;
uint32_t g_noticeT = 0;

void Notice(const std::string &s, int col)
{
    g_notice = s;
    g_noticeCol = col;
    g_noticeT = SDL_GetTicks();
}

// ---- 3. beam ---------------------------------------------------------------------------------
enum BState { B_IDLE = 0, B_QUEUED, B_PRE };
BState g_bs = B_IDLE;
int g_qCx = -1, g_qCy = -1, g_qFrames = 0;
int g_pvCx = -1, g_pvCy = -1;           // the table's confirm ring
int g_pvStatus = BEAM_UNKNOWN, g_pvCost = 0, g_pvEvCx = -1, g_pvEvCy = -1;
uint32_t g_beamT0 = 0;                  // order issued (SDL ticks)
uint32_t g_fadeT0 = 0, g_arriveT = 0;   // world fade: out start, arrival
float g_preAlpha = 0.0f;                // fade-out level from the game's pre-beam timer
uint32_t g_beamRobo = 0;                // the host station that beams
double g_from[3] = {0, 0, 0};
int g_battBefore = 0, g_moveBefore = 0, g_cost = 0;
const uint32_t kHoldMs = 150, kFadeInMs = 400;
const int kFadeLeadMs = 450; // start the fade when the original pre-beam timer is this low

// ---- helpers ---------------------------------------------------------------------------------
double Rad(double d) { return d * M_PI / 180.0; }

std::string Fmt(const char *f, int a, int b, int c = 0)
{
    char buf[64];
    snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

bool AtStation(NC_STACK_ypaworld *yw)
{
    return yw && yw->_userRobo && yw->_viewerBact == yw->_userRobo && yw->_userRobo->_status != BACT_STATUS_DEAD;
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

// bearing (deg, + = right) and elevation (deg) of a world point from the seat, in the
// station view's frame (the bridge turns with it)
void BearingOf(const double w[3], double *bearing, double *elev, double *distM)
{
    double xr[3];
    WorldToXr(w, xr);
    const double hz = sqrt(xr[0] * xr[0] + xr[2] * xr[2]);
    *bearing = atan2(xr[0], -xr[2]) * 180.0 / M_PI;
    *elev = atan2(xr[1], hz) * 180.0 / M_PI;
    *distM = sqrt(hz * hz + xr[1] * xr[1]);
}

const char *SideOf(double bearing)
{
    const double a = fabs(bearing);
    if (a <= 12.0)
        return "AHEAD";
    if (a >= 140.0)
        return "BEHIND";
    return bearing < 0 ? "LEFT" : "RIGHT";
}

std::string Upper(std::string s)
{
    for (char &ch : s)
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
    return s;
}

NC_STACK_ypabact *FindOwn(NC_STACK_ypaworld *yw, uint32_t gid)
{
    for (NC_STACK_ypabact *c : yw->_userRobo->_kidList)
    {
        if (c->_gid == gid)
            return c;
        for (NC_STACK_ypabact *u : c->_kidList)
            if (u->_gid == gid)
                return u;
    }
    return NULL;
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

// The original beam checks for sector (cx, cy), as a 2D-map click with the bar in
// beam mode: sets the "thing under the mouse" fields, runs ypaworld_func64__sub21__sub3
// (energy = squared distance / 230.4, free on an opened gate / own power station; the
// sector must be seen, flat, not a border sector, no enemy host there). When `issue`,
// the original order (DOACTION_10) follows with the fields still set. Every UI field
// is restored afterwards. Returns a BeamCheck.
int RunBeam(NC_STACK_ypaworld *yw, int cx, int cy, bool issue, int *cost)
{
    *cost = 0;
    if (!AtStation(yw) || !(bzda.field_1CC & 0x20))
        return BEAM_BUSY;
    NC_STACK_yparobo *robo = dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo);
    if (!robo || (robo->_roboState & NC_STACK_yparobo::ROBOSTATE_MOVE))
        return BEAM_BUSY;
    if (cx < 0 || cy < 0 || cx >= yw->_mapSize.x || cy >= yw->_mapSize.y)
        return BEAM_INVALID;
    yw_130arg a;
    a.pos_x = (cx + 0.5f) * World::CVSectorLength;
    a.pos_z = -(cy + 0.5f) * World::CVSectorLength;
    if (!yw->GetSectorInfo(&a) || !a.pcell)
        return BEAM_INVALID;
    NC_STACK_ypabact *const sBact = yw->_bactOnMouse;
    cellArea *const sCell = yw->_cellOnMouse;
    const vec3d sIsect = yw->_cellMouseIsectPos;
    const uint32_t sFlags = yw->_guiActFlags;
    const int sEnergy = yw->_updateMessage.energy;
    const int s1D0 = bzda.field_1D0;
    yw->_guiActFlags = (sFlags | 0x10u) & ~0x20u; // a sector under the mouse (map click)
    yw->_cellOnMouse = a.pcell;
    yw->_cellMouseIsectPos = vec3d(a.pos_x, a.pcell->height, a.pos_z);
    int r = yw->ypaworld_func64__sub21__sub3();
    *cost = yw->_updateMessage.energy;
    if (r == 0 && issue)
    {
        yw->ypaworld_func64__sub21__sub5(World::DOACTION_10); // original "beam host station"
        // the energy stays in _updateMessage: yparobo DOACTION_10 subtracts arg->energy
    }
    else
        yw->_updateMessage.energy = sEnergy;
    yw->_bactOnMouse = sBact;
    yw->_cellOnMouse = sCell;
    yw->_cellMouseIsectPos = sIsect;
    yw->_guiActFlags = sFlags;
    bzda.field_1D0 = s1D0; // the VR beam does not change the bar's mode
    if (r < BEAM_OK || r > BEAM_INVALID)
        r = BEAM_INVALID;
    return r;
}

// ---- drawing helpers (as vr_field.cpp) -----------------------------------------------------------
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

GLuint g_blackTex = 0;
void EnsureBlack()
{
    if (g_blackTex)
        return;
    const uint8_t px[4] = {0, 0, 0, 255};
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGenTextures(1, &g_blackTex);
    glBindTexture(GL_TEXTURE_2D, g_blackTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint prevAlign = 4; // restore the caller's unpack state
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, prevTex);
}

float WorldFade(uint32_t now)
{
    if (!g_fadeT0)
        return 0.0f;
    // fade out on the game's own pre-beam timer (game time), so the world is black when
    // the station jumps at any frame rate / time scale; then hold and fade in (real time)
    if (!g_arriveT)
        return g_preAlpha;
    const uint32_t holdEnd = g_arriveT + kHoldMs;
    if ((int32_t)(now - holdEnd) < 0)
        return 1.0f;
    return std::max(0.0f, 1.0f - (now - holdEnd) / (float)kFadeInMs);
}

// level boundary (-) -------------------------------------------------------------
// A restart / load reloads the same gids (and may reuse addresses), so the level boundary is
// explicit: NC_STACK_ypaworld::DeleteLevel -> VR::LevelBoundary -> Station::ResetLevel,
// before the next level's first poll / world update. Nothing of ours survives it (windows
// bookkeeping, built-unit effects + notice, beam preview / queue / fade, the table's carry
// and confirm ring); the loaded windows are handled as "entering the station" and the
// loaded units are seeded without effects.
void ResetState()
{
    g_map = Win();
    g_sq = Win();
    g_wasStation = false;
    g_winRobo = 0;
    g_built.clear();
    g_known.clear();
    g_knownRobo = 0; // Units() seeds the loaded units without effects
    g_notice.clear();
    g_noticeT = 0;
    if (g_bs != B_IDLE || g_pvCx >= 0)
        fprintf(stderr, "UAVR station: new level / restart / load -> beam state dropped\n");
    g_bs = B_IDLE;
    g_qCx = g_qCy = -1;
    g_qFrames = 0;
    g_pvCx = g_pvCy = -1;
    g_pvStatus = BEAM_UNKNOWN;
    g_pvEvCx = g_pvEvCy = -1;
    g_fadeT0 = g_arriveT = 0;
    g_preAlpha = 0.0f;
    g_beamRobo = 0;
    Table::ResetLevel();
}

// Backup to the explicit boundary (ResetLevel from NC_STACK_ypaworld::DeleteLevel): a
// different host gid or the game clock running backwards also resets.
void CheckLevel(NC_STACK_ypaworld *yw)
{
    static uint32_t s_robo = 0;
    static int32_t s_lastStamp = 0;
    const uint32_t robo = yw->_userRobo ? yw->_userRobo->_gid : 0;
    const bool changed = robo != s_robo || (int32_t)yw->_timeStamp < s_lastStamp;
    s_lastStamp = (int32_t)yw->_timeStamp;
    s_robo = robo;
    if (changed)
        ResetState();
}

// ---- 1. windows ------------------------------------------------------------------------------------
void Windows(NC_STACK_ypaworld *yw)
{
    const bool station = On() && AtStation(yw);
    struct One { Win *w; GuiBase *g; const char *name; };
    One ones[2] = {{&g_map, &robo_map, "Map"}, {&g_sq, &squadron_manager, "Squadron Manager"}};
    for (One &o : ones)
    {
        if (station && !g_wasStation)
        {
            // entering the station (level start, back from a unit, Command Deck switched on)
            if (o.g->IsOpen() && !o.w->wish)
            {
                yw->GuiWinClose(o.g);
                o.w->autoClosed = true;
                fprintf(stderr, "UAVR station: 2D %s window closed at the host station (Command Deck: table + bridge)\n", o.name);
            }
        }
        else if (station)
        {
            if (o.w->autoClosed && o.g->IsOpen())
            {
                o.w->autoClosed = false;
                o.w->wish = true;
                fprintf(stderr, "UAVR station: player opened the 2D %s window: kept open\n", o.name);
            }
            else if (o.w->wish && !o.w->autoClosed && o.g->IsClosed())
                o.w->wish = false; // closed again by the player
        }
        else if (g_wasStation)
        {
            // left the station state: back in Classic at the station -> reopen what we
            // closed; into a unit -> the original vehicle windows apply (our flags end,
            // the saved station state was kept by StationWindowsSaved)
            if (o.w->autoClosed && AtStation(yw))
            {
                yw->GuiWinOpen(o.g);
                yw->GuiWinToFront(o.g);
                fprintf(stderr, "UAVR station: 2D %s window reopened (Command Deck off)\n", o.name);
            }
            o.w->autoClosed = false;
        }
    }
    g_wasStation = station;
}

// ---- 2. built units ---------------------------------------------------------------------------------
void Units(NC_STACK_ypaworld *yw)
{
    const uint32_t now = SDL_GetTicks();
    if (!On() || !yw->_userRobo || yw->_userRobo->_status == BACT_STATUS_DEAD)
    {
        g_known.clear();
        g_knownRobo = 0;
        g_built.clear();
        return;
    }
    NC_STACK_ypabact *robo = yw->_userRobo;
    std::vector<uint32_t> cur;
    std::vector<NC_STACK_ypabact *> fresh;
    const bool seed = g_knownRobo != robo->_gid;
    auto visit = [&](NC_STACK_ypabact *u) {
        cur.push_back(u->_gid);
        if (!seed && !std::binary_search(g_known.begin(), g_known.end(), u->_gid) && u->_status == BACT_STATUS_CREATE &&
            u->_host_station == robo && u->_bact_type != BACT_TYPES_GUN)
            fresh.push_back(u);
    };
    for (NC_STACK_ypabact *c : robo->_kidList)
    {
        visit(c);
        for (NC_STACK_ypabact *u : c->_kidList)
            visit(u);
    }
    std::sort(cur.begin(), cur.end());
    g_known.swap(cur);
    g_knownRobo = robo->_gid;
    for (NC_STACK_ypabact *u : fresh)
    {
        Built b;
        b.gid = u->_gid;
        b.w[0] = u->_position.x;
        b.w[1] = u->_position.y;
        b.w[2] = u->_position.z;
        b.t0 = now;
        g_built.push_back(b);
        std::string name = "UNIT";
        if (u->_vehicleID < yw->_vhclProtos.size())
            name = Upper(yw->GetVehicleName(yw->_vhclProtos[u->_vehicleID]));
        double bearing = 0, elev = 0, dist = 0;
        BearingOf(b.w, &bearing, &elev, &dist);
        const char *side = SideOf(bearing);
        Notice(name + " READY - " + side, 3);
        // the bridge's sill ring is 0.62 m below the eye at 2.6 m (about -13 deg): above it
        // the unit is seen through a window, below it through the deck grating
        fprintf(stderr, "UAVR station: built %u '%s' (squad %u) at %.0f,%.0f,%.0f | bearing %+.0f deg %s, elevation %+.0f deg, %.1f m -> %s | beam-in + marker, bridge line '%s READY - %s' | robo battVeh %d\n",
                u->_gid, name.c_str(), u->_parent ? u->_parent->_gid : 0, b.w[0], b.w[1], b.w[2], bearing, side, elev, dist,
                elev > -13.0 && fabs(bearing) < 140 ? "through the window" : "below the sill (deck grating)", name.c_str(), side,
                dynamic_cast<NC_STACK_yparobo *>(robo) ? dynamic_cast<NC_STACK_yparobo *>(robo)->getROBO_battVehicle() : -1);
    }
    for (Built &b : g_built)
    {
        NC_STACK_ypabact *u = FindOwn(yw, b.gid);
        b.alive = u && u->_status != BACT_STATUS_DEAD;
        if (u)
        {
            b.w[0] = u->_position.x;
            b.w[1] = u->_position.y;
            b.w[2] = u->_position.z;
        }
        if (g_log && u && (int)((now - b.t0) / 1000) != b.logged && ((b.logged = (int)((now - b.t0) / 1000)), true))
            fprintf(stderr, "UAVR station: built unit %u at %.0f,%.0f,%.0f (%.1f s, status %d)\n", b.gid, b.w[0], b.w[1], b.w[2], (now - b.t0) / 1000.0, u->_status);
    }
    g_built.erase(std::remove_if(g_built.begin(), g_built.end(), [&](const Built &b) { return !b.alive || now - b.t0 > kMarkMs; }), g_built.end());
}

// ---- 3. beam progress ---------------------------------------------------------------------------------
void BeamProgress(NC_STACK_ypaworld *yw)
{
    const uint32_t now = SDL_GetTicks();
    // the fade ends by itself (render side); clear it once it is done
    if (g_fadeT0 && g_arriveT && WorldFade(now) <= 0.0f && now - g_arriveT > kHoldMs)
        g_fadeT0 = g_arriveT = 0;
    if (!On())
    {
        // leaving Command Deck ends everything of ours at once (no Classic fade)
        g_bs = B_IDLE;
        g_fadeT0 = g_arriveT = 0;
        g_preAlpha = 0.0f;
        return;
    }
    if (g_bs != B_PRE)
        return;
    NC_STACK_yparobo *robo = yw->_userRobo ? dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo) : NULL;
    if (!robo || robo->_status == BACT_STATUS_DEAD || robo->_gid != g_beamRobo)
    {
        g_bs = B_IDLE;
        if (g_fadeT0 && !g_arriveT)
            g_arriveT = now;
        return;
    }
    const bool moving = (robo->_roboState & NC_STACK_yparobo::ROBOSTATE_MOVE) != 0;
    if (!moving && !g_fadeT0)
    {
        const double d0 = sqrt((robo->_position.x - g_from[0]) * (robo->_position.x - g_from[0]) + (robo->_position.z - g_from[2]) * (robo->_position.z - g_from[2]));
        if (d0 < 50.0)
        {
            // the game never started the beam (no pre-timer tick, station in place): no fade
            fprintf(stderr, "UAVR station: the host station did not start the beam\n");
            g_bs = B_IDLE;
            return;
        }
    }
    if (!moving)
    {
        const double d = sqrt((robo->_position.x - g_from[0]) * (robo->_position.x - g_from[0]) + (robo->_position.z - g_from[2]) * (robo->_position.z - g_from[2]));
        if (!g_fadeT0)
            g_fadeT0 = now; // (no pre-timer tick seen): black hold from here
        if (!g_arriveT)
            g_arriveT = now;
        g_bs = B_IDLE;
        fprintf(stderr, "UAVR station: world black %.2f when the jump was rendered, hold %u ms, fade in %u ms\n", g_preAlpha, kHoldMs, kFadeInMs);
        const int cx = (int)(robo->_position.x / World::CVSectorLength), cy = (int)(-robo->_position.z / World::CVSectorLength);
        const int fx = (int)(g_from[0] / World::CVSectorLength), fy = (int)(-g_from[2] / World::CVSectorLength);
        fprintf(stderr, "UAVR station: host station beamed %.0f,%.0f,%.0f (sector %d,%d) -> %.0f,%.0f,%.0f (sector %d,%d), %.0f units | beam battery %d -> %d (cost %d), move energy %d -> %d | %.2f s after the order\n",
                g_from[0], g_from[1], g_from[2], fx, fy, robo->_position.x, robo->_position.y, robo->_position.z, cx, cy, d,
                g_battBefore, robo->getROBO_battBeam(), g_cost, g_moveBefore, robo->_roboEnergyMove, (now - g_beamT0) / 1000.0);
        char buf[96];
        snprintf(buf, sizeof(buf), "STATION BEAMED TO %d,%d", cx, cy);
        Notice(buf, 1);
        Table::DioRecentre(); // the miniature's window follows the station (in the black hold)
        return;
    }
    // no wall-clock timeout (a pause would end it early); the beam ends with the
    // game state above (arrival, station gone / dead, Command Deck off)
}

} // namespace

// ---------------------------------------------------------------------------------------------------

void Init(double upm)
{
    g_upm = upm;
    auto env = [](const char *n) { const char *v = getenv(n); return (v && v[0]) ? v : (const char *)NULL; };
    if (const char *v = env("UA_VR_STATION_LOG")) g_log = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_STATION")) g_enabled = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_BUILD_CURSOR"))
    {
        float fx, fy;
        if (sscanf(v, "%f,%f", &fx, &fy) == 2 && fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
        {
            g_cursorFx = fx;
            g_cursorFy = fy;
        }
    }
}

bool On() { return g_enabled && Active() && CommandDeck(); }

void SetAnchor(const M3 &ra, const double pa[3])
{
    g_ra = ra;
    memcpy(g_pa, pa, sizeof(g_pa));
    g_anchorValid = true;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!yw || !Active())
        return;
    CheckLevel(yw);
    Windows(yw);
    Units(yw);
    BeamProgress(yw);
    // dev aid UA_VR_STATION_SCAN=1: the original beam check for every sector, once
    static int scan = -1;
    if (scan < 0)
    {
        const char *v = getenv("UA_VR_STATION_SCAN");
        scan = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    if (scan == 1 && On() && AtStation(yw))
    {
        scan = 2;
        for (int y = 0; y < yw->_mapSize.y; y++)
        {
            std::string row;
            for (int x = 0; x < yw->_mapSize.x; x++)
            {
                int cost = 0;
                const int st = RunBeam(yw, x, y, false, &cost);
                row += st == BEAM_OK ? Fmt(" %d,%d:OK(%d)", x, y, cost / 100) : Fmt(" %d,%d:%d", x, y, st);
            }
            fprintf(stderr, "UAVR station: scan row %d:%s\n", y, row.c_str());
        }
    }
    // the table's confirm ring: evaluate the original checks for its sector (preview only)
    if (On() && g_pvCx >= 0)
    {
        int cost = 0;
        const int st = RunBeam(yw, g_pvCx, g_pvCy, false, &cost);
        if (st != g_pvStatus || cost / 100 != g_pvCost / 100 || g_pvEvCx != g_pvCx || g_pvEvCy != g_pvCy)
        {
            char buf[96];
            if (st == BEAM_OK)
                snprintf(buf, sizeof(buf), "BEAM TO %d,%d: COST %d", g_pvCx, g_pvCy, cost / 100);
            else
                snprintf(buf, sizeof(buf), "BEAM TO %d,%d: %s", g_pvCx, g_pvCy, BeamCheckText(st));
            Notice(buf, st == BEAM_OK ? 1 : 2);
            fprintf(stderr, "UAVR station: beam preview sector %d,%d -> %s (cost %d, beam battery %d)\n", g_pvCx, g_pvCy, BeamCheckText(st), cost,
                    yw->_userRobo ? dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo)->getROBO_battBeam() : -1);
        }
        g_pvStatus = st;
        g_pvCost = cost;
        g_pvEvCx = g_pvCx;
        g_pvEvCy = g_pvCy;
    }
}

void ResetLevel()
{
    ResetState();
}

void WorldHook(NC_STACK_ypaworld *yw)
{
    if (yw)
        CheckLevel(yw); // a queued beam never runs in a loaded / restarted world
    if (!yw || g_bs != B_QUEUED)
        return;
    if (!On() || !AtStation(yw))
    {
        fprintf(stderr, "UAVR station: beam dropped (left the station / Command Deck)\n");
        g_bs = B_IDLE;
        return;
    }
    if (yw->_updateMessage.user_action != World::DOACTION_0)
    {
        if (++g_qFrames > 30)
        {
            fprintf(stderr, "UAVR station: beam dropped, the game kept another user action\n");
            g_bs = B_IDLE;
        }
        return; // a mouse action the game queued this frame wins; try the next frame
    }
    NC_STACK_yparobo *robo = dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo);
    if (!robo)
    {
        g_bs = B_IDLE;
        return;
    }
    g_from[0] = robo->_position.x;
    g_from[1] = robo->_position.y;
    g_from[2] = robo->_position.z;
    g_battBefore = robo->getROBO_battBeam();
    g_moveBefore = robo->_roboEnergyMove;
    int cost = 0;
    const int st = RunBeam(yw, g_qCx, g_qCy, true, &cost);
    if (st != BEAM_OK || yw->_updateMessage.user_action != World::DOACTION_10)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "BEAM REFUSED: %s", BeamCheckText(st));
        Ticker(yw, buf);
        Notice(buf, 2);
        fprintf(stderr, "UAVR station: beam to sector %d,%d refused by the original checks: %s (cost %d, beam battery %d)\n", g_qCx, g_qCy, BeamCheckText(st), cost, g_battBefore);
        g_bs = B_IDLE;
        return;
    }
    g_cost = cost;
    g_beamT0 = SDL_GetTicks();
    g_fadeT0 = g_arriveT = 0;
    g_preAlpha = 0.0f;
    g_beamRobo = robo->_gid;
    g_bs = B_PRE;
    fprintf(stderr, "UAVR station: DOACTION_10 (beam host station %u) -> sector %d,%d target %.0f,%.0f,%.0f | cost %d, beam battery %d, move energy %d | _updateMessage.user_action=%d\n",
            robo->_gid, g_qCx, g_qCy, yw->_updateMessage.target_point.x, yw->_updateMessage.target_point.y, yw->_updateMessage.target_point.z, cost,
            g_battBefore, g_moveBefore, yw->_updateMessage.user_action);
}

bool BuildCursor(int screenW, int screenH, int *x, int *y)
{
    if (!On())
        return false;
    *x = (int)(screenW * g_cursorFx);
    *y = (int)(screenH * g_cursorFy);
    return true;
}

void DrawWorldFade(int eye)
{
    (void)eye;
    if (!On())
        return;
    const float a = WorldFade(SDL_GetTicks());
    if (a <= 0.0f || !Overlay::Begin())
        return;
    EnsureBlack();
    float q[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    Overlay::Quad(q, Overlay::STYLE_TEXTURE, 1, 1, 1, a, g_blackTex);
    Overlay::End();
}

bool WorldHidden()
{
    return On() && WorldFade(SDL_GetTicks()) >= 0.5f;
}

void DrawEye(int eye, const ViewState &views, double zn, double zf)
{
    if (!On() || g_built.empty() || !g_anchorValid || WorldHidden()) // not through the beam blackout
        return;
    if (!Overlay::Begin())
        return;
    const uint32_t now = SDL_GetTicks();
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    float q[4][4];
    for (const Built &b : g_built)
    {
        const uint32_t age = now - b.t0;
        double xr[3], e[3];
        WorldToXr(b.w, xr);
        ToEye(views, eye, xr, e);
        const double dist = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (age < kBeamInMs)
        {
            // beam-in: a cyan column from above onto the creation point + an expanding ring
            const float a = (float)age / kBeamInMs;
            const double top[3] = {xr[0], xr[1] + std::max(1.5, dist * 0.45), xr[2]};
            const double w = std::max(0.01, dist * 0.018) * (1.0 - 0.6 * a);
            if (Beam(views, eye, proj, xr, top, w, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, 0.45f, 0.95f, 1.0f, 0.85f * (1.0f - a * a));
            if (Beam(views, eye, proj, xr, top, w * 0.35, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, 0.9f, 1.0f, 1.0f, 0.9f * (1.0f - a));
            if (MarkQuad(views, eye, proj, xr, std::max(0.03, dist * tan(Rad(1.5 + 5.0 * a))), q))
                Overlay::Quad(q, Overlay::STYLE_RETICLE, 0.45f, 0.95f, 1.0f, 1.0f - a);
        }
        if (age > 400)
        {
            // marker while it rolls out: pulsing cyan ring, fading in the last second
            const float pulse = 0.75f + 0.25f * (float)cos(age * 0.008);
            const float fade = age > kMarkMs - 1000 ? (kMarkMs - age) / 1000.0f : 1.0f;
            if (MarkQuad(views, eye, proj, xr, std::max(0.025, dist * tan(Rad(1.3))), q))
                Overlay::Quad(q, Overlay::STYLE_RETICLE, 0.45f, 0.95f, 1.0f, 0.9f * pulse * std::max(0.0f, fade));
        }
    }
    Overlay::End();
}

//: the mixed-reality table (passthrough room + miniature) ------------------------------------

float DioFade()
{
    return On() ? WorldFade(SDL_GetTicks()) : 0.0f;
}

namespace
{
// world -> tracking on the miniature (Table::DioXform, vr_table.h)
void DioPoint(const Table::DioXform &x, const double w[3], double out[3])
{
    const double xb = (w[0] - x.lenX * 0.5) / x.D, yb = (w[2] + x.lenY * 0.5) / x.D, zb = (x.refY - w[1]) / x.D;
    for (int i = 0; i < 3; i++)
        out[i] = x.c[i] + x.L * (xb * x.ex[i] + yb * x.ey[i] + zb * x.n[i]);
}

// a square lying on the table plane (ex, ey) around c, radius r (m): a flat ring on the miniature
bool FlatQuad(const ViewState &vs, int eye, const M4 &proj, const Table::DioXform &x, const double c[3], double r, float q[4][4])
{
    static const double s[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int k = 0; k < 4; k++)
    {
        double p[3], e[3];
        for (int i = 0; i < 3; i++)
            p[i] = c[i] + r * (s[k][0] * x.ex[i] + s[k][1] * x.ey[i]);
        ToEye(vs, eye, p, e);
        if (e[2] <= 1e-3 || !Clip(proj, e, q[k]))
            return false;
    }
    return true;
}
} // namespace

void DrawDioEye(int eye, const ViewState &views, double zn, double zf)
{
    if (!On() || g_built.empty() || WorldHidden())
        return;
    Table::DioXform x;
    if (!Table::DioramaXform(&x) || !(x.D > 0) || !(x.L > 0))
        return;
    const double mpu = x.L / x.D;                       // metres per world unit on the table
    const double sec = World::CVSectorLength * mpu;      // one sector on the table (m)
    if (!Overlay::Begin())
        return;
    const uint32_t now = SDL_GetTicks();
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    float q[4][4];
    for (Built &b : g_built)
    {
        const int cx = (int)(b.w[0] / World::CVSectorLength), cy = (int)(-b.w[2] / World::CVSectorLength);
        if (cx < x.win[0] || cx > x.win[2] || cy < x.win[1] || cy > x.win[3])
            continue; // outside the drawn window
        const uint32_t age = now - b.t0;
        double p[3];
        DioPoint(x, b.w, p);
        const double r0 = std::max(0.006, sec * 0.30);
        if (age < kBeamInMs)
        {
            // beam-in on the miniature: a thin cyan column onto the unit + a ring opening on the table
            const float a = (float)age / kBeamInMs;
            double top[3];
            for (int i = 0; i < 3; i++)
                top[i] = p[i] + x.n[i] * std::max(0.05, sec * 1.2);
            const double w = std::max(0.0015, sec * 0.05) * (1.0 - 0.6 * a);
            if (Beam(views, eye, proj, p, top, w, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, 0.45f, 0.95f, 1.0f, 0.85f * (1.0f - a * a));
            if (Beam(views, eye, proj, p, top, w * 0.35, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, 0.9f, 1.0f, 1.0f, 0.9f * (1.0f - a));
            if (FlatQuad(views, eye, proj, x, p, r0 * (0.6 + 1.2 * a), q))
                Overlay::Quad(q, Overlay::STYLE_RETICLE, 0.45f, 0.95f, 1.0f, 1.0f - a);
        }
        if (age > 400)
        {
            // marker while it rolls out beside the station: pulsing ring flat on the table
            const float pulse = 0.75f + 0.25f * (float)cos(age * 0.008);
            const float fade = age > kMarkMs - 1000 ? (kMarkMs - age) / 1000.0f : 1.0f;
            if (FlatQuad(views, eye, proj, x, p, r0 * 0.8, q))
                Overlay::Quad(q, Overlay::STYLE_RETICLE, 0.45f, 0.95f, 1.0f, 0.9f * pulse * std::max(0.0f, fade));
        }
        if (!b.dioLogged && eye == 0)
        {
            b.dioLogged = true;
            fprintf(stderr, "UAVR station: built unit %u rolls out on the miniature (sector %d,%d, %.3f,%.3f,%.3f m on the table, ring %.3f m, column %.3f m, window %d..%d x %d..%d)\n",
                    b.gid, cx, cy, p[0], p[1], p[2], r0, std::max(0.05, sec * 1.2), x.win[0], x.win[2], x.win[1], x.win[3]);
        }
    }
    Overlay::End();
}

void PreviewBeam(int cx, int cy)
{
    if (cx != g_pvCx || cy != g_pvCy)
    {
        g_pvStatus = BEAM_UNKNOWN;
        g_pvCost = 0;
        g_pvEvCx = g_pvEvCy = -1;
        if (cx < 0 && g_notice.compare(0, 8, "BEAM TO ") == 0)
            g_noticeT = 0; // the ring went away: clear its line
    }
    g_pvCx = cx;
    g_pvCy = cy;
}

int BeamStatus(int cx, int cy, int *cost)
{
    if (cx != g_pvEvCx || cy != g_pvEvCy)
    {
        if (cost) *cost = 0;
        return BEAM_UNKNOWN;
    }
    if (cost) *cost = g_pvCost;
    return g_pvStatus;
}

void ConfirmBeam(int cx, int cy)
{
    if (!On() || g_bs != B_IDLE)
    {
        fprintf(stderr, "UAVR station: beam confirm ignored (%s)\n", On() ? "a beam is under way" : "not Command Deck");
        return;
    }
    g_qCx = cx;
    g_qCy = cy;
    g_qFrames = 0;
    g_pvCx = g_pvCy = -1; // the preview ends; the ring shows "confirmed" (vr_table.cpp)
    g_bs = B_QUEUED;
    g_beamT0 = SDL_GetTicks();
    fprintf(stderr, "UAVR station: beam to sector %d,%d confirmed on the table\n", cx, cy);
}

bool Beaming() { return g_bs != B_IDLE || g_fadeT0 != 0; }

const char *BeamCheckText(int st)
{
    switch (st)
    {
    case BEAM_OK: return "OK";
    case BEAM_NOENERGY: return "NOT ENOUGH ENERGY";
    case BEAM_JAGGY: return "GROUND TOO ROUGH";
    case BEAM_UNKNOWNLOC: return "SECTOR NOT SEEN";
    case BEAM_TOOCLOSE: return "TOO CLOSE";
    case BEAM_ENEMYHOST: return "ENEMY HOST THERE";
    case BEAM_BUSY: return "STATION BUSY";
    case BEAM_UNKNOWN: return "CHECKING";
    default: return "NOT POSSIBLE";
    }
}

std::string BridgeNotice(int *col)
{
    if (!On() || g_notice.empty() || !g_noticeT || SDL_GetTicks() - g_noticeT > kNoticeMs)
        return std::string();
    if (col)
        *col = g_noticeCol;
    return g_notice;
}

} // namespace Station

// Public (vr.h): yparobo.cpp doBeamUpdate, during the unit update (before this frame's
// eyes are rendered), with the pre-beam timer after this frame's step. The world fade
// follows the game's timer and is fully black on the frame the station jumps (<= 0).
void StationBeamTick(uint32_t roboGid, int timePre)
{
    using namespace Station;
    if (!On() || g_bs != B_PRE || roboGid != g_beamRobo || timePre > kFadeLeadMs)
        return;
    const uint32_t now = SDL_GetTicks();
    if (!g_fadeT0)
    {
        g_fadeT0 = now;
        fprintf(stderr, "UAVR station: beam pre-timer %d ms -> world fade out\n", timePre);
    }
    g_preAlpha = timePre <= 0 ? 1.0f : std::max(0.0f, std::min(1.0f, 1.0f - timePre / (float)kFadeLeadMs));
    if (timePre <= 0 && !g_arriveT)
        g_arriveT = now; // the jump happens in this update: black hold from this frame on
    if (g_log)
        fprintf(stderr, "UAVR station: beam pre-timer %d ms, world black %.2f\n", timePre, g_preAlpha);
}

// Public (vr.h): yw_game_ui.cpp, after the game stored the station's window state on
// leaving the host station. A window closed by the Command Deck (not by the player) is
// stored as it was before (open), so Classic and the saved profile keep the original
// choice.
void StationWindowsSaved(NC_STACK_ypaworld *yw)
{
    using namespace Station;
    if (!yw || !Active())
        return;
    if (g_map.autoClosed && robo_map.IsClosed())
        yw->_roboMapStatus.IsOpen = true;
    if (g_sq.autoClosed && squadron_manager.IsClosed())
        yw->_roboFinderStatus.IsOpen = true;
}

} // namespace VR
