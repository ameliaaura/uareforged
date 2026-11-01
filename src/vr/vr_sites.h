#ifndef UA_VR_SITES_H_INCLUDED
#define UA_VR_SITES_H_INCLUDED

// Urban Assault VR — "sites": buildings at scale.
// Only in COMMAND DECK (VR::CommandDeck(); env UA_VR_SITES=0/1 overrides for A/B).
//
//  * When the player's side takes a power station, a beam-gate key sector, or the
//    beam gate opens (the game's own ownership / gate state, polled every gameplay
//    frame), a short readable moment: the building's sector gets a pulsing outline in
//    the world (seen wherever it is in view), a two-line wrist message
//    ("POWER STATION TAKEN" / "+N ENERGY/S", the income change measured after the
//    capture), and, when the player is in a vehicle within about 300 m, a ground
// beacon at the building. The directional radio already places the
//    game's own voice line (msgs 45 / 82) at the sector, so the wrist arrow points to
//    it. No time stop, no camera motion.
//  * Wrist energy: the host station's energy (current / max, display units = energy /
//    100 as the HUD) with a rising / falling arrow and the measured rate.
//
// Classic: Gameplay returns at once, nothing is drawn, the wrist is unchanged.

#include <string>
#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Sites
{

void Init(double unitsPerM);
// BeginWorldView, after the comfort anchor (same mapping as Cmd / Field).
void SetAnchor(const M3 &ra, const double pa[3]);
// After each gameplay frame: ownership poll, moments, energy trend, dev aid.
void Gameplay(NC_STACK_ypaworld *yw);
// Level boundary (VR::LevelBoundary): everything re-tracks on the next gameplay frame.
void ResetLevel();
// Eye pass, after Field::DrawEye: outline + beacon.
void DrawEye(int eye, const ViewState &views, double zn, double zf);

// For the left-wrist panel (vr_cockpit.cpp). False in Classic / without a live host.
struct WristInfo
{
    int energy = 0, energyMax = 1; // display units (energy / 100)
    int arrow = 0;                 // +1 rising, -1 falling, 0 steady
    double rate = 0;               // display units per second (measured)
    bool line = false;             // a site moment is on
    std::string line1, line2;
    float lineAlpha = 0;
};
bool Wrist(WristInfo *out);

} // namespace Sites
} // namespace VR

#endif
