// Urban Assault VR — "sites": buildings at scale. See vr_sites.h and
//
//
//   gameplay frame -> Sites::Gameplay   poll the owners of the power stations, the beam
//                                       gates' key sectors and the gates' open state;
//                                       a change to the player's side starts a moment;
//                                       host income / energy trend; UA_VR_SITES_TEST
//   eye pass       -> Sites::DrawEye    pulsing sector outline (+ glow faces) and the
//                                       ground beacon (vehicle within ~300 m)
//   wrist          -> Sites::Wrist      energy readout + the moment's two lines
//
// Sites are referenced by sector index between frames (never by pointer across levels).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_sites.h"
#include "vr_sites_logic.h"
#include "vr_overlay.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"

namespace VR
{
bool Active();

namespace Sites
{

namespace
{
using namespace SitesLogic;

double g_upm = 11.0;
bool g_log = false;
int g_force = -1; // UA_VR_SITES=0/1

bool g_anchorValid = false;
M3 g_ra = M3::Ident();
double g_pa[3] = {0, 0, 0};

struct Site
{
    int kind = SITE_POWER;
    int cx = 0, cy = 0;
    int gate = -1;      // index of its gate (key sectors, gates)
    int owner = 0;      // last seen owner
    bool open = false;  // gates: open
};

struct Moment
{
    int kind = SITE_POWER;
    int cx = 0, cy = 0;
    double p[3] = {0, 0, 0}; // sector centre (world), y = ground
    uint32_t t0 = 0;         // SDL ticks
    int power = 0;
    int keysOwn = 0, keysTotal = 0;
    double incomeBefore = 0; // raw / s
    bool gainKnown = false;
    double gain = 0;         // display units / s
    bool logged = false, inViewLogged = false, beaconLogged = false;
};

// tracker
bool g_armed = false;
const void *g_levelKey = NULL;
int g_mapW = 0, g_mapH = 0;
int32_t g_lastStamp = 0;
std::vector<Site> g_sites;              // key sectors + gates (fixed per level)
std::vector<TrackedStation> g_stations; // power stations (follow _powerStations)
std::vector<Moment> g_moments; // newest last, at most 4

// live state for draw / wrist
bool g_live = false;
uint32_t g_liveTicks = 0;
bool g_inVehicle = false;
double g_viewer[3] = {0, 0, 0};
int g_energy = 0, g_energyMax = 1;
Trend g_trend;
double g_income = 0; // raw / s, this frame (main battery share)

// dev aid
int g_testFrame = -1, g_testDrain = -1;
bool g_testKey = false;
int g_frame = 0;
int g_testCell = -1;
uint32_t g_testTakeAt = 0;
uint32_t g_lastLog = 0;

bool On()
{
    return CommandDeck() && g_force != 0; // UA_VR_SITES=0 turns it off for A/B; never on outside COMMAND DECK
}

double Rad(double d) { return d * M_PI / 180.0; }

void WorldToXr(const double w[3], double xr[3])
{
    const double rel[3] = {w[0] - g_pa[0], w[1] - g_pa[1], w[2] - g_pa[2]};
    double l[3];
    g_ra.Apply(rel, l);
    xr[0] = l[0] / g_upm;
    xr[1] = -l[1] / g_upm;
    xr[2] = -l[2] / g_upm;
}

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

// a soft line between two eye-space points, `w` metres half-width (eye space)
bool BeamEye(const M4 &proj, const double a[3], const double b[3], double w, float q[4][4])
{
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

double Len(const double e[3]) { return sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]); }

// world point -> eye space
void W2E(const ViewState &vs, int eye, const double w[3], double e[3])
{
    double xr[3];
    WorldToXr(w, xr);
    ToEye(vs, eye, xr, e);
}

void Colour(int kind, float *r, float *g, float *b)
{
    if (kind == SITE_GATE) { *r = 0.45f; *g = 0.85f; *b = 1.0f; }
    else if (kind == SITE_KEY) { *r = 1.0f; *g = 0.85f; *b = 0.35f; }
    else { *r = 0.35f; *g = 1.0f; *b = 0.45f; }
}

// the power stations now (id = their map key, the cell id)
std::vector<CurrentStation> Stations(NC_STACK_ypaworld *yw)
{
    std::vector<CurrentStation> cur;
    for (auto &kv : yw->_powerStations)
    {
        const TPowerStationInfo &p = kv.second;
        if (!p.pCell || p.CellId.x < 0 || p.CellId.y < 0 || p.CellId.x >= yw->_mapSize.x || p.CellId.y >= yw->_mapSize.y)
            continue;
        CurrentStation c;
        c.id = kv.first;
        c.owner = p.pCell->owner;
        cur.push_back(c);
    }
    return cur;
}

void Track(NC_STACK_ypaworld *yw)
{
    g_sites.clear();
    g_stations.clear();
    ReconcileStations(g_stations, Stations(yw));
    for (size_t gi = 0; gi < yw->_levelInfo.Gates.size(); gi++)
    {
        const TMapGate &gate = yw->_levelInfo.Gates[gi];
        for (const TMapKeySector &ks : gate.KeySectors)
        {
            if (!ks.PCell)
                continue;
            Site s;
            s.kind = SITE_KEY;
            s.cx = ks.CellId.x;
            s.cy = ks.CellId.y;
            s.gate = (int)gi;
            s.owner = ks.PCell->owner;
            g_sites.push_back(s);
        }
        if (gate.PCell)
        {
            Site s;
            s.kind = SITE_GATE;
            s.cx = gate.CellId.x;
            s.cy = gate.CellId.y;
            s.gate = (int)gi;
            s.owner = gate.PCell->owner;
            s.open = gate.PCell->PurposeType == cellArea::PT_GATEOPENED;
            g_sites.push_back(s);
        }
    }
}

double HostIncome(NC_STACK_ypaworld *yw)
{
    NC_STACK_ypabact *robo = yw->_userRobo;
    if (!robo->_pSector)
        return 0;
    yw_arg176 a;
    a.owner = robo->_pSector->owner;
    yw->ypaworld_func176(&a);
    const double total = IncomePerS(robo->_reload_const, robo->_pSector->energy_power, a.field_4, robo->_pSector->owner, robo->_owner);
    // the MAIN battery's share (what the HOST number counts up by): EnergyInteract splits
    // the income over the batteries the fill mode charges (main 1, units 4, beam 8)
    NC_STACK_yparobo *rb = dynamic_cast<NC_STACK_yparobo *>(robo);
    if (!rb)
        return total;
    const int fill = rb->getROBO_fillMode();
    const int n = ((fill & 1) || total < 0 ? 1 : 0) + ((fill & 4) ? 1 : 0) + ((fill & 8) ? 1 : 0);
    if (n == 0 || !((fill & 1) || total < 0))
        return 0.0;
    return total / n;
}

void Start(NC_STACK_ypaworld *yw, const Site &s, double incomeBefore)
{
    cellArea &cell = yw->_cells(s.cx, s.cy);
    Moment m;
    m.kind = s.kind;
    m.cx = s.cx;
    m.cy = s.cy;
    m.p[0] = cell.CenterPos.x;
    m.p[1] = cell.height;
    m.p[2] = cell.CenterPos.z;
    m.t0 = SDL_GetTicks();
    m.incomeBefore = incomeBefore;
    if (s.kind == SITE_POWER)
    {
        auto it = yw->_powerStations.find(cell.Id);
        if (it != yw->_powerStations.end())
            m.power = it->second.EffectivePower;
    }
    if (s.gate >= 0 && s.gate < (int)yw->_levelInfo.Gates.size())
        for (const TMapKeySector &ks : yw->_levelInfo.Gates[s.gate].KeySectors)
        {
            m.keysTotal++;
            if (ks.PCell && ks.PCell->owner == yw->_userRobo->_owner)
                m.keysOwn++;
        }
    g_moments.push_back(m);
    if (g_moments.size() > 4)
        g_moments.erase(g_moments.begin());
    const double dx = m.p[0] - g_viewer[0], dz = m.p[2] - g_viewer[2];
    fprintf(stderr, "UAVR sites: %s sector %d,%d at %.0f,%.0f,%.0f (power %d, keys %d/%d, income before %.1f/s, viewer %.0f away%s)\n",
            KindTitle(m.kind), m.cx, m.cy, m.p[0], m.p[1], m.p[2], m.power, m.keysOwn, m.keysTotal, incomeBefore / 100.0,
            sqrt(dx * dx + dz * dz), g_inVehicle ? ", in a vehicle" : ", at the station");
}

void CancelTest()
{
    g_testCell = -1;
    g_testTakeAt = 0;
}

bool TestCellOk(NC_STACK_ypaworld *yw)
{
    return g_testCell >= 0 && g_testCell < yw->_mapSize.x * yw->_mapSize.y;
}

void DevTest(NC_STACK_ypaworld *yw)
{
    if (g_testFrame < 0)
        return;
    const uint8_t me = yw->_userRobo->_owner;
    if (g_frame == g_testFrame)
    {
        // the player's power station (or key sector) nearest the viewer goes neutral,
        // silently (CellSetOwner = the game's bookkeeping, no voice) ...
        double best = 1e18;
        g_testCell = -1;
        auto consider = [&](cellArea *c)
        {
            if (!c || c->owner != me)
                return;
            const double dx = c->CenterPos.x - g_viewer[0], dz = c->CenterPos.z - g_viewer[2];
            if (dx * dx + dz * dz < best)
            {
                best = dx * dx + dz * dz;
                g_testCell = c->CellId.x + c->CellId.y * yw->_mapSize.x;
            }
        };
        if (g_testKey)
        {
            for (const TMapGate &gate : yw->_levelInfo.Gates)
                for (const TMapKeySector &ks : gate.KeySectors)
                    consider(ks.PCell);
        }
        else
            for (auto &kv : yw->_powerStations)
                consider(kv.second.pCell);
        if (g_testCell < 0)
        {
            fprintf(stderr, "UAVR sites: TEST: no own %s to retake\n", g_testKey ? "key sector" : "power station");
            g_testFrame = -1;
            return;
        }
        cellArea &c = yw->_cells(g_testCell % yw->_mapSize.x, g_testCell / yw->_mapSize.x);
        yw->CellSetOwner(&c, 0);
        if (g_testDrain >= 0)
            yw->_userRobo->_energy = (int)((int64_t)yw->_userRobo->_energy_max * g_testDrain / 100);
        g_testTakeAt = SDL_GetTicks() + 2500;
        fprintf(stderr, "UAVR sites: TEST: %s %d,%d set neutral%s; retaken in 2.5 s through CellSetNewOwner\n",
                g_testKey ? "key sector" : "power station", c.CellId.x, c.CellId.y,
                g_testDrain >= 0 ? " (host energy drained for the count-up)" : "");
    }
    if (g_testTakeAt && !TestCellOk(yw))
        CancelTest();
    if (g_testCell >= 0 && g_testTakeAt && SDL_GetTicks() >= g_testTakeAt)
    {
        // ... and is retaken through the game's own capture path (voice 45 / 82, radio
        // place, history, power), credited to the viewer as the capturing unit
        cellArea &c = yw->_cells(g_testCell % yw->_mapSize.x, g_testCell / yw->_mapSize.x);
        fprintf(stderr, "UAVR sites: TEST: CellSetNewOwner(%d,%d -> owner %d, unit %u)\n", c.CellId.x, c.CellId.y, me, yw->_viewerBact->_gid);
        yw->CellSetNewOwner(&c, yw->_viewerBact, me);
        g_testCell = -1;
        g_testTakeAt = 0;
        g_testFrame = -1;
    }
}

} // namespace

// ---------------------------------------------------------------------------------

void Init(double upm)
{
    g_upm = upm;
    auto on = [](const char *n) { const char *v = getenv(n); return v && v[0] && strcmp(v, "0") != 0; };
    g_log = on("UA_VR_SITES_LOG");
    if (const char *v = getenv("UA_VR_SITES"))
        if (v[0])
            g_force = atoi(v) ? 1 : 0;
    if (const char *v = getenv("UA_VR_SITES_TEST")) // "<frame>[,drainPct][,KEY]"
    {
        char kind[16] = {0};
        int f = -1, d = -1;
        const int n = sscanf(v, "%d,%d,%15s", &f, &d, kind);
        if (n >= 1 && SimBackend(getenv("UA_VR"))) // state-changing aid: simulator only
        {
            g_testFrame = f;
            g_testDrain = n >= 2 ? d : -1;
            g_testKey = n >= 3 && strcmp(kind, "KEY") == 0;
        }
    }
}

// explicit level boundary (NC_STACK_ypaworld::DeleteLevel via
// VR::LevelBoundary): the next gameplay frame re-tracks / reseeds ownership without a
// moment; moments, trend and a pending dev retake end here.
void ResetLevel()
{
    g_armed = false;
    g_live = false;
    g_levelKey = NULL;
    g_moments.clear();
    TrendReset(g_trend);
    CancelTest();
}

void SetAnchor(const M3 &ra, const double pa[3])
{
    g_ra = ra;
    memcpy(g_pa, pa, sizeof(g_pa));
    g_anchorValid = true;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!Active() || !On() || !yw || !yw->_userRobo || !yw->_viewerBact || yw->_mapSize.x <= 0 ||
        yw->_userRobo->_status == BACT_STATUS_DEAD)
    {
        g_live = false;
        g_armed = false;
        g_anchorValid = false;
        g_moments.clear();
        CancelTest(); // a pending dev retake never survives into another level
        return;
    }
    // a new level / restart / load: re-track, no moments from the load itself
    const void *key = &yw->_cells(0, 0);
    if (key != g_levelKey || g_mapW != yw->_mapSize.x || g_mapH != yw->_mapSize.y || yw->_timeStamp < g_lastStamp || !g_armed)
    {
        g_levelKey = key;
        g_mapW = yw->_mapSize.x;
        g_mapH = yw->_mapSize.y;
        g_moments.clear();
        TrendReset(g_trend);
        g_frame = 0;
        CancelTest();
        Track(yw);
        g_armed = true;
        g_income = HostIncome(yw);
        g_lastStamp = yw->_timeStamp;
        fprintf(stderr, "UAVR sites: tracking %d power stations, %d key sectors / gates\n", (int)g_stations.size(), (int)g_sites.size());
        return;
    }
    g_lastStamp = yw->_timeStamp;
    g_frame++;

    const uint8_t me = yw->_userRobo->_owner;
    NC_STACK_ypabact *vb = yw->_viewerBact;
    g_inVehicle = vb != yw->_userRobo && !yw->_playerInHSGun;
    g_viewer[0] = vb->_position.x; g_viewer[1] = vb->_position.y; g_viewer[2] = vb->_position.z;

    DevTest(yw);

    const double incomeBefore = g_income; // last frame's (before this frame's change)
    g_income = HostIncome(yw);

    // ownership poll: power stations (reconciled with the game's list first) ...
    ReconcileStations(g_stations, Stations(yw));
    for (TrackedStation &t : g_stations)
    {
        auto it = yw->_powerStations.find(t.id);
        if (it == yw->_powerStations.end() || !it->second.pCell)
            continue;
        const int owner = it->second.pCell->owner;
        if (Captured(t.owner, owner, me, g_armed))
        {
            Site s;
            s.kind = SITE_POWER;
            s.cx = it->second.CellId.x;
            s.cy = it->second.CellId.y;
            Start(yw, s, incomeBefore);
        }
        t.owner = owner;
    }
    // ... key sectors and gates
    for (Site &s : g_sites)
    {
        if (s.cx < 0 || s.cy < 0 || s.cx >= yw->_mapSize.x || s.cy >= yw->_mapSize.y)
            continue;
        cellArea &cell = yw->_cells(s.cx, s.cy);
        const int owner = cell.owner;
        if (s.kind == SITE_GATE)
        {
            const bool open = cell.PurposeType == cellArea::PT_GATEOPENED;
            if (GateOpened(s.open, open, g_armed))
                Start(yw, s, incomeBefore);
            s.open = open;
        }
        else if (Captured(s.owner, owner, me, g_armed))
            Start(yw, s, incomeBefore);
        s.owner = owner;
    }

    // the income change, measured after the power grid updated
    const uint32_t now = SDL_GetTicks();
    for (Moment &m : g_moments)
        if (m.kind == SITE_POWER && !m.gainKnown && now - m.t0 >= kGainSampleMs)
        {
            m.gainKnown = true;
            m.gain = (g_income - m.incomeBefore) / 100.0;
            fprintf(stderr, "UAVR sites: %s %d,%d income %.1f/s -> %.1f/s (%+.1f energy/s), wrist \"%s\"\n", KindTitle(m.kind), m.cx, m.cy,
                    m.incomeBefore / 100.0, g_income / 100.0, m.gain, DetailLine(m.kind, true, m.gain, m.power, m.keysOwn, m.keysTotal).c_str());
        }
    while (!g_moments.empty() && now - g_moments.front().t0 > std::max(kMomentMs, kBeaconMs))
        g_moments.erase(g_moments.begin());

    // energy (display units = energy / 100, as the HUD / bridge)
    NC_STACK_ypabact *robo = yw->_userRobo;
    g_energy = std::max(0, robo->_energy) / 100;
    g_energyMax = std::max(1, robo->_energy_max / 100);
    TrendAdd(g_trend, (uint32_t)yw->_timeStamp, std::max(0, robo->_energy));
    g_live = true;
    g_liveTicks = now;

    const bool moment = !g_moments.empty();
    if ((g_log || moment) && now - g_lastLog > (moment ? 500u : 2000u))
    {
        g_lastLog = now;
        fprintf(stderr, "UAVR sites: wrist energy %d/%d arrow %+d rate %+.1f/s income %.1f/s%s\n", g_energy, g_energyMax, g_trend.arrow,
                g_trend.rate, g_income / 100.0, moment ? " (moment on)" : "");
    }
}

void DrawEye(int eye, const ViewState &views, double zn, double zf)
{
    if (!g_live || g_moments.empty() || !g_anchorValid || SDL_GetTicks() - g_liveTicks > 300)
        return;
    const uint32_t now = SDL_GetTicks();
    if (!Overlay::Begin())
        return;
    const M4 proj = UaAsymFrustum(views.eye[eye].fov, zn, zf);
    float q[4][4];
    const double L = World::CVSectorLength;
    for (Moment &m : g_moments)
    {
        const uint32_t age = now - m.t0;
        float r, g, b;
        Colour(m.kind, &r, &g, &b);
        // ---- outline + glow: the building's sector, 0.84 sector wide, ~60 m high ----
        const float ha = HighlightAlpha(age);
        if (ha > 0.01f)
        {
            const double hs = 0.42 * L, H = 650.0; // UA y is down: up = -y
            double c[8][3], e[8][3];
            for (int k = 0; k < 8; k++)
            {
                const double sx = (k & 1) ? 1 : -1, sz = (k & 2) ? 1 : -1;
                c[k][0] = m.p[0] + sx * hs;
                c[k][1] = m.p[1] - ((k & 4) ? H : 0.0);
                c[k][2] = m.p[2] + sz * hs;
                W2E(views, eye, c[k], e[k]);
            }
            static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            static const int faces[4][4] = {{0, 1, 5, 4}, {1, 3, 7, 5}, {3, 2, 6, 7}, {2, 0, 4, 6}};
            static const int floorQ[4] = {0, 1, 3, 2};
            for (int fi = 0; fi < 5; fi++) // soft glow on the sides + the footprint
            {
                const int *f = fi < 4 ? faces[fi] : floorQ;
                bool ok = true;
                for (int k = 0; k < 4 && ok; k++)
                    ok = e[f[k]][2] > 1e-3 && Clip(proj, e[f[k]], q[k]);
                if (ok)
                    Overlay::Quad(q, Overlay::STYLE_BEAM, r, g, b, (fi < 4 ? 0.22f : 0.30f) * ha);
            }
            for (const auto &ed : edges) // each edge: a wide glow + a bright core
            {
                const double *a = e[ed[0]], *bb = e[ed[1]];
                const double mid[3] = {(a[0] + bb[0]) * 0.5, (a[1] + bb[1]) * 0.5, (a[2] + bb[2]) * 0.5};
                const double d = Len(mid);
                if (BeamEye(proj, a, bb, std::max(0.01, d * tan(Rad(0.9))), q))
                    Overlay::Quad(q, Overlay::STYLE_BEAM, r, g, b, 0.45f * ha);
                if (BeamEye(proj, a, bb, std::max(0.004, d * tan(Rad(0.3))), q))
                    Overlay::Quad(q, Overlay::STYLE_BEAM, 0.5f + 0.5f * r, 0.5f + 0.5f * g, 0.5f + 0.5f * b, 1.0f * ha);
            }
            if (eye == 0 && !m.inViewLogged)
            {
                // in view: the sector centre (half height) inside the left eye's frustum
                const double ctr[3] = {m.p[0], m.p[1] - H * 0.5, m.p[2]};
                double ec[3];
                float cc[4];
                W2E(views, eye, ctr, ec);
                static uint32_t lastOut = 0;
                const bool in = ec[2] > 1e-3 && Clip(proj, ec, cc) && fabsf(cc[0]) < cc[3] && fabsf(cc[1]) < cc[3];
                if (!in && now - lastOut > 1000)
                {
                    lastOut = now;
                    fprintf(stderr, "UAVR sites: highlight out of view: %d,%d at eye yaw %+.0f pitch %+.0f deg, %.0f m\n", m.cx, m.cy,
                            atan2(ec[0], ec[2]) * 180.0 / M_PI, atan2(-ec[1], sqrt(ec[0] * ec[0] + ec[2] * ec[2])) * 180.0 / M_PI, Len(ec));
                }
                if (in)
                {
                    m.inViewLogged = true;
                    fprintf(stderr, "UAVR sites: highlight in view: %s %d,%d (%.0f m from the eye, screen %.2f,%.2f)\n", KindTitle(m.kind), m.cx, m.cy,
                            Len(ec), cc[0] / cc[3], cc[1] / cc[3]);
                }
            }
        }
        // ---- ground beacon: in a vehicle within ~300 m ----
        const double dx = m.p[0] - g_viewer[0], dy = m.p[1] - g_viewer[1], dz = m.p[2] - g_viewer[2];
        const double dist3 = sqrt(dx * dx + dy * dy + dz * dz); // altitude counts
        const float ba = BeaconAlpha(age);
        if (ba > 0.01f && BeaconWanted(g_inVehicle, dist3, g_upm))
        {
            const double base[3] = {m.p[0], m.p[1], m.p[2]}, top[3] = {m.p[0], m.p[1] - 2600.0, m.p[2]};
            double eb[3], et[3];
            W2E(views, eye, base, eb);
            W2E(views, eye, top, et);
            const double w = std::max(0.01, Len(eb) * tan(Rad(0.45)));
            if (BeamEye(proj, eb, et, w * 2.5, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, r, g, b, 0.25f * ba);
            if (BeamEye(proj, eb, et, w, q))
                Overlay::Quad(q, Overlay::STYLE_BEAM, 1.0f, 1.0f, 1.0f, 0.75f * ba);
            // a ring on the ground at the foot of the beacon (screen-facing, 5 deg)
            if (eb[2] > 1e-3)
            {
                const double rr = Len(eb) * tan(Rad(5.0));
                static const double sq[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
                bool ok = true;
                for (int k = 0; k < 4 && ok; k++)
                {
                    const double p[3] = {eb[0] + sq[k][0] * rr, eb[1] + sq[k][1] * rr, eb[2]};
                    ok = Clip(proj, p, q[k]);
                }
                if (ok)
                    Overlay::Quad(q, Overlay::STYLE_RETICLE, r, g, b, 0.9f * ba);
            }
            if (eye == 0 && !m.beaconLogged)
            {
                m.beaconLogged = true;
                fprintf(stderr, "UAVR sites: ground beacon at %d,%d (vehicle %.0f units = %.0f m away, %.0f m above)\n", m.cx, m.cy, dist3,
                        dist3 / g_upm, dy / g_upm);
            }
        }
    }
    Overlay::End();
}

bool Wrist(WristInfo *out)
{
    if (!g_live || SDL_GetTicks() - g_liveTicks > 300)
        return false;
    out->energy = g_energy;
    out->energyMax = g_energyMax;
    out->arrow = g_trend.arrow;
    out->rate = g_trend.rate;
    out->line = false;
    const uint32_t now = SDL_GetTicks();
    for (auto it = g_moments.rbegin(); it != g_moments.rend(); ++it) // newest
    {
        const float a = WristLineAlpha(now - it->t0);
        if (a <= 0.01f)
            continue;
        out->line = true;
        out->lineAlpha = a;
        out->line1 = KindTitle(it->kind);
        out->line2 = DetailLine(it->kind, it->gainKnown, it->gain, it->power, it->keysOwn, it->keysTotal);
        break;
    }
    return true;
}

} // namespace Sites
} // namespace VR
