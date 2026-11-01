// sites: pure logic for "air" (field orders from flyers)
// and "sites" (buildings at scale: the captured power station / key sector / beam gate
// moment, the wrist energy readout). No engine dependencies, so
// tools/vr/test_vr_sites.cpp can unit-test it; vr_field.cpp and vr_sites.cpp use it.
#ifndef UA_VR_SITES_LOGIC_H_INCLUDED
#define UA_VR_SITES_LOGIC_H_INCLUDED

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace VR
{
namespace SitesLogic
{

// ---- air: field-order ray and marker ------------------------------------------------
// The field-order ray: 6000 units on the ground (as PickWorld); flyers look down from
// altitude at a shallow angle, so they get 14000 (about 12 sectors, 1.3 km at 11 u/m).
const double kRayGround = 6000.0, kRayFlyer = 14000.0;
inline double RayLen(bool flyer) { return flyer ? kRayFlyer : kRayGround; }

// Angular radius of the order marker (degrees) by distance from the eye (metres in
// the VR world). Near: 2.2 deg (field orders). It grows to 3.4 deg at 400 m and more so a
// spot far below a helicopter stays readable against the city texture.
inline double MarkerAngleDeg(double distM)
{
    const double a0 = 2.2, a1 = 3.4, d0 = 60.0, d1 = 400.0;
    if (!(distM > d0)) return a0;
    if (distM >= d1) return a1;
    return a0 + (a1 - a0) * (distM - d0) / (d1 - d0);
}
// A vertical pin over far markers (metres above the spot): none near, up to 25 m
// at 400 m and more, so the spot stands out over roofs. 0 = no pin.
inline double PinHeightM(double distM)
{
    if (!(distM > 80.0)) return 0.0;
    return std::fmin(25.0, (distM - 80.0) * 25.0 / 320.0);
}

// ---- sites: what counts as a captured site -------------------------------------------
enum SiteKind { SITE_POWER = 0, SITE_KEY, SITE_GATE, SITE_KIND_COUNT };

// A tracked sector changed hands. armed = the tracker has seen this level for at least
// one frame (no "capture" from loading a level / save game).
inline bool Captured(int prevOwner, int curOwner, int me, bool armed)
{
    return armed && prevOwner != me && curOwner == me;
}
// The beam gate opened (closed gate building -> open gate building) on the player's
// side: all its key sectors are now the player's.
inline bool GateOpened(bool prevOpen, bool curOpen, bool armed) { return armed && !prevOpen && curOpen; }

// Power stations come and go (built by the player, destroyed): the tracked list follows
// _powerStations every frame. Survivors keep their last owner (so a capture in the same
// frame still counts); new stations start at their current owner (no capture from being
// built); gone ones are dropped (an ordinary sector changing hands is no station capture).
struct TrackedStation { int id = -1; int owner = 0; };
struct CurrentStation { int id = -1; int owner = 0; };
template <class CurList>
inline void ReconcileStations(std::vector<TrackedStation> &tracked, const CurList &current)
{
    std::vector<TrackedStation> out;
    out.reserve(current.size());
    for (const CurrentStation &c : current)
    {
        TrackedStation t;
        t.id = c.id;
        t.owner = c.owner; // new: no history
        for (const TrackedStation &o : tracked)
            if (o.id == c.id)
            {
                t.owner = o.owner; // survivor: last seen owner
                break;
            }
        out.push_back(t);
    }
    tracked.swap(out);
}

// Dev aids that change game state run only in the desktop simulator (UA_VR=sim), never
// in a headset session that inherited the variable.
inline bool SimBackend(const char *uaVrEnv) { return uaVrEnv && std::string(uaVrEnv) == "sim"; }

// ---- the moment -----------------------------------------------------------------------
const uint32_t kMomentMs = 6000; // highlight + wrist line
const uint32_t kBeaconMs = 4500; // ground beacon (in a vehicle near the site)
const uint32_t kGainSampleMs = 2500; // income measured this long after the capture

// Highlight strength 0..1: pulses at 1.6 Hz, fades out over the last second.
inline float HighlightAlpha(uint32_t ageMs)
{
    if (ageMs >= kMomentMs) return 0.0f;
    const float t = ageMs / 1000.0f;
    const float pulse = 0.55f + 0.45f * std::cos(t * 2.0f * 3.14159265f * 1.6f);
    const float fade = ageMs > kMomentMs - 1000 ? (kMomentMs - ageMs) / 1000.0f : 1.0f;
    const float in = ageMs < 150 ? ageMs / 150.0f : 1.0f;
    return pulse * fade * in;
}
inline float BeaconAlpha(uint32_t ageMs)
{
    if (ageMs >= kBeaconMs) return 0.0f;
    const float fade = ageMs > kBeaconMs - 1200 ? (kBeaconMs - ageMs) / 1200.0f : 1.0f;
    return (0.7f + 0.3f * std::cos(ageMs / 1000.0f * 2.0f * 3.14159265f)) * fade;
}
// Wrist line: full for the moment, then gone.
inline float WristLineAlpha(uint32_t ageMs)
{
    if (ageMs >= kMomentMs) return 0.0f;
    return ageMs > kMomentMs - 800 ? (kMomentMs - ageMs) / 800.0f : 1.0f;
}

// "In a vehicle within about 300 m": the beacon. unitsPerM = the VR world scale.
inline bool BeaconWanted(bool inVehicle, double distUnits, double unitsPerM)
{
    return inVehicle && distUnits <= 300.0 * unitsPerM;
}

// ---- energy -------------------------------------------------------------------------------
// The host station's income per second in raw energy units, exactly as
// NC_STACK_yparobo::EnergyInteract: reload_const * sector power * reload ratio / 6000,
// negative in an enemy sector, zero in a neutral one. (Split across the batteries the
// fill mode charges; this is the total.) The HUD / bridge / wrist show energy / 100.
inline double IncomePerS(double reloadConst, double sectorPower, double ratio, int sectorOwner, int me)
{
    if (sectorOwner == 0) return 0.0;
    const double v = reloadConst * sectorPower * ratio / 6000.0;
    return sectorOwner == me ? v : -v;
}

// Measured trend of the displayed energy (raw units in, display units / s out) over a
// sliding window, with hysteresis on the arrow so it does not flicker.
struct Trend
{
    static const int N = 32;
    uint32_t t[N] = {0};
    double e[N] = {0};
    int n = 0, head = 0;
    int arrow = 0;    // +1 rising, -1 falling, 0 steady
    double rate = 0;  // display units per second
};
inline void TrendReset(Trend &s) { s = Trend(); }
// Feed one sample (game ms, raw energy). windowMs: rate over the newest samples
// spanning about this long.
inline void TrendAdd(Trend &s, uint32_t tMs, double rawEnergy, uint32_t windowMs = 2000)
{
    if (s.n > 0)
    {
        const int last = (s.head + Trend::N - 1) % Trend::N;
        if (tMs < s.t[last]) TrendReset(s); // time went back: a new level / restart
        else if (tMs - s.t[last] < 100) return; // at most 10 samples per second
    }
    s.t[s.head] = tMs;
    s.e[s.head] = rawEnergy;
    s.head = (s.head + 1) % Trend::N;
    if (s.n < Trend::N) s.n++;
    // oldest sample inside the window
    const int newest = (s.head + Trend::N - 1) % Trend::N;
    int oldest = newest;
    for (int k = 1; k < s.n; k++)
    {
        const int i = (newest + Trend::N - k) % Trend::N;
        if (tMs - s.t[i] > windowMs) break;
        oldest = i;
    }
    const double dt = (s.t[newest] - s.t[oldest]) / 1000.0;
    s.rate = dt > 0.2 ? (s.e[newest] - s.e[oldest]) / 100.0 / dt : 0.0;
    // arrow: on above 0.5 / s, off below 0.2 / s
    if (s.rate > 0.5) s.arrow = 1;
    else if (s.rate < -0.5) s.arrow = -1;
    else if (std::fabs(s.rate) < 0.2) s.arrow = 0;
}

// ---- text ---------------------------------------------------------------------------------
inline const char *KindTitle(int kind)
{
    switch (kind)
    {
    case SITE_POWER: return "POWER STATION TAKEN";
    case SITE_KEY: return "KEY SECTOR TAKEN";
    case SITE_GATE: return "BEAM GATE OPEN";
    default: return "SECTOR TAKEN";
    }
}
// Second wrist line. gainKnown: the income change was measured (kGainSampleMs after the
// capture). deltaPerS in display units / s. power: the station's effective power.
// keysOwn / keysTotal: key sectors of the gate this key belongs to.
inline std::string DetailLine(int kind, bool gainKnown, double deltaPerS, int power, int keysOwn, int keysTotal)
{
    char b[64];
    if (kind == SITE_POWER)
    {
        const int d = (int)std::lround(deltaPerS);
        if (gainKnown && d > 0) snprintf(b, sizeof(b), "+%d ENERGY/S", d);
        else snprintf(b, sizeof(b), "+%d POWER", power);
    }
    else if (kind == SITE_KEY) snprintf(b, sizeof(b), "KEY SECTORS %d/%d", keysOwn, keysTotal);
    else snprintf(b, sizeof(b), "GATE READY");
    return b;
}

} // namespace SitesLogic
} // namespace VR

#endif
