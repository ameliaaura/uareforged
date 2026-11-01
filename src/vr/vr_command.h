#ifndef UA_VR_COMMAND_H_INCLUDED
#define UA_VR_COMMAND_H_INCLUDED

// Urban Assault VR — command layer.
//
//  * Host station: left stick = snap turn 30 deg (or the game's smooth turn,
//    UA_VR_STATION_TURN=smooth, with the comfort vignette).
//  * Map board: X toggles a whole-level board at table height in front of the
//    player. R trigger on a squad selects it, R trigger on ground / an enemy
//    orders move / attack (the original map's context rule).
//  * Build wheel: Y (host station) opens a radial wheel of buildable vehicles;
//    flick the right stick to a slot, release = build.
//  * Dive: A while pointing at an own unit (board or world) = take control of
//    it behind a quick fade; A otherwise = the original "next unit" with a fade;
//    B held 0.5 s = back to the host station (fade).
//
// Every action is executed through the game's own user-action code
// (NC_STACK_ypaworld::ypaworld_func64__sub21__sub5 -> _updateMessage ->
// yparobo::doUserCommands, or the original hotkeys), never new game logic.
// Internal to the VR module; vr_engine.cpp calls these from its frame hooks.

#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Cmd
{

void Init(double unitsPerM);
// FrameStart, right after polling (and the comfort combo filter), before the
// laser pointer and the game input read the controllers: consumes the inputs
// the command layer owns (X, Y, A, B-hold, board trigger, wheel stick, snap stick).
void Filter(ControllerState c[2]);
// BeginWorldView, after the comfort anchor: tracking space -> world mapping.
void SetAnchor(const M3 &ra, const double pa[3]);
// XOR-ed into the comfort viewer key: changes on every snap turn, so the comfort
// filters snap with it instead of reading a 30-degree jump as rotation.
uint64_t SnapKey();
// Eye pass (eye FBO bound, before ReleaseEye): board, wheel, hover marks, fade.
void DrawEye(int eye, const ViewState &views, double zn, double zf, bool diorama = false); // diorama = hand / table overlays only, no full-scale world marks
// Panel FBO bound before EndFrame: the fade also covers the 2D panel.
void DrawPanel(int panelW, int panelH);
// After each gameplay frame: snapshot for the board / wheel, result logging.
void Gameplay(NC_STACK_ypaworld *yw);
// field orders (vr_field.cpp): queue a squad select (own unit gid) / an order for the
// squad (enemy unit gid, or 0 = the sector at world x,z) for the world hook. squadGid =
// the squad commander captured at the pull (0 = whatever is current at execution).
void QueueSelect(uint32_t gid);
void QueueOrder(uint32_t targetGid, double x, double z, uint32_t squadGid);
// the war TABLE (not the wall) is open at the host station in COMMAND DECK
// (the engine raises / narrows the HUD panel so it does not cover the table).
bool DeckTableAtStation();
// the table is open (not the wall, not behind the build wheel) and shows the diorama.
bool DioramaShown();
// QUICK STATION (Command Deck by default): the wrist shows "HOLD B: STATION" (in a
// vehicle or station gun), and while the player's own vehicle lies destroyed the 2D panel
// asks "BACK TO STATION? A" (*secLeft = seconds to the automatic return).
bool StationHint();
bool DeathPrompt(float *secLeft);
// a command-layer view change (dive / return fade) is under way.
bool ViewChanging();

} // namespace Cmd
} // namespace VR

#endif
