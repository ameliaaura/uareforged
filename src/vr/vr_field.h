#ifndef UA_VR_FIELD_H_INCLUDED
#define UA_VR_FIELD_H_INCLUDED

// Urban Assault VR — "field". In a vehicle:
//
//  * Field orders (VR::FieldOrders()): hold X = order mode, a ground marker under the
//    right-hand ray (green move / red attack); pull the R trigger = the current squad
//    moves there, or attacks the pointed enemy unit / enemy sector (the original order
//    code, DOACTION_2). The trigger pull never fires the weapon. X tapped = the old X.
//  * Wrist squads (VR::WristSquads()): the left-wrist SQUADS rows are buttons for the
//    right-hand ray: trigger = select that squad; on the current squad = dive into its
//    leader (the command layer's fade).
//  * Squad wheel (VR::SquadWheel()): hold Y = a radial wheel of the squads near the
//    left hand; flick the R stick to one and let go = it is the current squad; L stick
//    up / down = aggression 1..5 (the original setter). Y tapped = the original squad
//    manager. At the host station / station gun Y stays the build wheel.
//
// Everything is inert when its Settings row is off (Classic defaults), so the frame
// is exactly the / mode one. Called from vr_command.cpp (Filter) and
// vr_engine.cpp (anchor, gameplay snapshot, eye pass).

#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Field
{

struct In
{
    int poll = 0;           // command layer poll counter (UA_VR_SIM_PRESS frames)
    bool boardHit = false;  // the right ray is on the open map board
    bool boardOn = true;    // Settings MAP BOARD
    bool inVehicle = false; // viewer is a unit (not the host station / station gun)
    bool busy = false;      // a dive fade runs
    uint32_t selCmdr = 0;   // current squad commander gid
};

struct Out
{
    bool ownsX = false;       // this X press is field orders' (command layer skips its X)
    bool toggleBoard = false; // X tapped: the old board toggle
    bool ownsY = false;       // this Y press is the squad wheel's
    bool blockBoard = false;  // the ray is on the wrist: no board hover / click
};

void Init(double unitsPerM);
// Inside Cmd::Filter (live command layer only), after the board / world picks.
Out Filter(ControllerState c[2], const In &in);
// BeginWorldView, after the comfort anchor (same mapping as Cmd::SetAnchor).
void SetAnchor(const M3 &ra, const double pa[3]);
// After each gameplay frame: squads, terrain / unit ray cast, aggression, logs.
void Gameplay(NC_STACK_ypaworld *yw);
// Eye pass, before Cmd::DrawEye (so the dive fade covers it).
void DrawEye(int eye, const ViewState &views, double zn, double zf, bool diorama = false); // diorama = hand / table overlays only, no full-scale world marks

} // namespace Field
} // namespace VR

#endif
