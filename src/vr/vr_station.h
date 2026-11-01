#ifndef UA_VR_STATION_H_INCLUDED
#define UA_VR_STATION_H_INCLUDED

// Urban Assault VR — "station". Command Deck
// only (VR::CommandDeck()); every entry point is inert in Classic.
//
//  1. Station 120 Hz: at the host station the war table + bridge replace the 2D Map and
//     Squadron Manager windows, so they are closed on entering the station unless the
//     player opens them himself (the saved window state is kept as it was).
//  2. Build by hand: a unit built at the station (VR build wheel or the original build
//     menu) gets a beam-in effect at its creation point (the game's real unit, created by
//     the original code) + a marker while it rolls out, and a bridge side-screen line
//     "<NAME> READY - LEFT". The VR build wheel's creation cursor sits left of the
//     table so the new unit appears in the front window.
//  3. Beam the station: the table hands over a sector (own host token placed on it +
//     trigger confirm); the original host-station beam (DOACTION_10, with the original
//     checks and energy cost) runs in the world hook. The world fades out / in around
//     the jump; the bridge and the table stay around the player.

#include "vr.h"
#include "vr_math.h"
#include <string>

class NC_STACK_ypaworld;

namespace VR
{
namespace Station
{

void Init(double unitsPerM);
bool On(); // VR live && COMMAND DECK
// BeginWorldView, after the comfort anchor (same mapping as Cmd::SetAnchor).
void SetAnchor(const M3 &ra, const double pa[3]);
// After each gameplay frame: 2D windows, built units, beam progress, beam preview.
void Gameplay(NC_STACK_ypaworld *yw);
// World update, before yparobo::HandleUserCommands (CommandWorldHook): a confirmed beam.
void WorldHook(NC_STACK_ypaworld *yw);
// Level boundary (VR::LevelBoundary, from NC_STACK_ypaworld::DeleteLevel): drop all state.
void ResetLevel();
// The VR build wheel's creation cursor (screen pixels) in COMMAND DECK; false = keep.
bool BuildCursor(int screenW, int screenH, int *x, int *y);
// Eye pass, before the cockpit / bridge: black over the world while the station beams.
void DrawWorldFade(int eye);
// The beam fade has the world mostly black (other world overlays, e.g. Sites, skip).
bool WorldHidden();
// Eye pass, after Field::DrawEye: beam-in effect + markers of newly built units.
void DrawEye(int eye, const ViewState &views, double zn, double zf);

// mixed-reality table (the diorama is up: passthrough room + miniature):
// the beam fade level for the miniature (Table::DioDim), 0 = none ...
float DioFade();
// ... and the built-unit roll-out on the miniature: beam-in column + a ring on the table
// beside the miniature host station, then a pulsing ring while it drives out (8 s).
void DrawDioEye(int eye, const ViewState &views, double zn, double zf);

// War table interface (vr_table.cpp).
enum BeamCheck
{
    BEAM_UNKNOWN = -1, // not evaluated yet
    BEAM_OK = 0,
    BEAM_NOENERGY = 1,
    BEAM_JAGGY = 2,     // uneven ground or a border sector
    BEAM_UNKNOWNLOC = 3, // sector not seen
    BEAM_TOOCLOSE = 4,
    BEAM_ENEMYHOST = 5,
    BEAM_INVALID = 6,   // no sector / under construction
    BEAM_BUSY = 7,      // already beaming, not at the station, game action pending
};
void PreviewBeam(int cx, int cy);           // the confirm ring is up on this sector (-1 = none)
int BeamStatus(int cx, int cy, int *cost);  // last evaluation for that sector
void ConfirmBeam(int cx, int cy);           // trigger on the ring: queue the original beam
bool Beaming();                             // a beam is queued / under way (table ring)
const char *BeamCheckText(int status);

// Bridge side-screen notice (built unit / beam result), empty when none. col 0 white,
// 1 green, 2 red, 3 cyan.
std::string BridgeNotice(int *col);

} // namespace Station
} // namespace VR

#endif
