#ifndef UA_VR_BRIDGE_H_INCLUDED
#define UA_VR_BRIDGE_H_INCLUDED

// Urban Assault VR — host-station bridge (Command Deck only).
//
//  * Bridge room around the player while the viewer is the user's host station:
//    floor plate, grated deck ring, console arcs, slanted window posts, sill and
//    roof rings. Open frame (the battlefield stays visible at full scale through
//    the gaps). Built in comfort-anchor-local metres like the cockpit shells, so
//    it turns with the station view (snap or smooth turn). One static draw.
//  * Side screens (energy, squads, enemy hosts, power stations) and a main
//    monitor ahead-high (mission objective + radio), filled from the same game
//    state the 2D HUD reads. One dynamic draw, rebuilt at most 4x per second.
//  * Briefing / debriefing (theatre frames) in Command Deck: the room is drawn
//    around the player in tracking space and the 2D panel goes onto the main
//    monitor instead of the black theatre void.
//  * Classic mode and every other viewer: nothing is drawn, nothing changes.
//
// Dev aids: UA_VR_BRIDGE=0/1 overrides the mode gate (A/B, captures);
// UA_VR_BRIDGE_LOG=1 logs the screen values next to the raw game state every 2 s;
// UA_VR_BRIDGE_MON=dist,elevDeg,width moves / sizes the main monitor;
// UA_VR_BRIDGE_THEATRE=1 treats every theatre frame (title, menus) as a briefing (captures).

#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Bridge
{

void Init(double unitsPerM);
// After each gameplay frame: station check + screen values snapshot.
void Gameplay(NC_STACK_ypaworld *yw);
// Eye pass, after the cockpit (which draws nothing at the station). Returns true
// when the bridge was drawn (the comfort reference rim is redundant then).
bool DrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor);
// the table is open as the mixed-reality diorama (passthrough room + miniature): the
// side screens and the main monitor alone, in the same places as on the bridge (no room
// metal), depth-tested against the miniature. Call after Table::DioPassthroughAlpha.
// UA_VR_BRIDGE_MR=0 turns them off. Returns true when drawn.
bool DrawPanelsEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor);
// Present() on a frame without a 3D world: when Command Deck is on and a
// briefing / debriefing was noted this frame, draws the room into both eyes
// (tracking space) and moves `panel` onto the main monitor. Returns true when
// both eyes were drawn (the caller then submits them as the projection layer).
bool Theatre(Backend *backend, const ViewState &views, float panelAspect, PanelLayer *panel);

} // namespace Bridge
} // namespace VR

#endif
