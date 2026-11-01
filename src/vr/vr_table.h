#ifndef UA_VR_TABLE_H_INCLUDED
#define UA_VR_TABLE_H_INCLUDED

// Urban Assault VR — Command Deck war table.
//
// Active only in VR MODE = COMMAND DECK (VR::CommandDeck()). It replaces the
// flat map board (vr_command.cpp) with:
//  * a ~1.1 m tilted table (or, with VR::MapWall(), a ~2.6 m wall ~3 m away)
//    showing the whole level: low-poly sector relief from the cells' heights,
//    the board texture (ownership colours + sector grid) on the sector tops;
//  * 3D tokens: one per own squad (leader position, unit-count stack), host
//    stations (own + seen enemy), seen enemy units (stealth respected);
//  * grip on the empty table = move / turn it, both grips = scale (clamped);
//  * grip on an own squad token = pick it up; bring it to the face (< 25 cm)
//    = dive into the squad leader (the command layer's fade + a short
//    "cockpit boot" flash); release elsewhere = the token snaps back.
// Orders keep the board path (two clicks, original DOACTION code); the
// table only supplies the surface, the token pick and the drawing.
//
// vr_command.cpp calls these; every function is a no-op when On() is false,
// so CLASSIC mode keeps the board exactly.

#include "vr.h"
#include "vr_math.h"

class NC_STACK_ypaworld;

namespace VR
{
namespace Table
{

void Init(double unitsPerM);
// COMMAND DECK and the VR module live: the table replaces the flat board.
bool On();
// After each gameplay frame: heights, tokens.
void Gameplay(NC_STACK_ypaworld *yw);

// Board surface (tracking space). s: 0..1 along +x of the map, t: 0..1 rows.
// Ray: relief-aware intersection; when `hover`, the token under the ray is
// remembered for HoverToken(). False when the ray misses the board.
bool Ray(const double o[3], const double d[3], bool hover, double *s, double *t, double hit[3]);
// The token picked by the last hover Ray(): kind 1 own squad (gid = leader),
// 2 enemy unit / host station. False = none (the sector rule applies).
bool HoverToken(uint32_t *gid, int *kind);

// Living Diorama ----
// The table (not the wall) in COMMAND DECK shows the real world in miniature unless the
// hidden profile key ;#!vr_tokentable is 1 (or UA_VR_DIORAMA=0). Flat, on the table.
bool DioramaOn();
// World -> tracking (XR metres) for the miniature:
//   p = c + L * (xb * ex + yb * ey + zb * n), xb = (wx - lenX/2) / D,
//   yb = (wz + lenY/2) / D, zb = (refY - wy) / D   (UA world y points down).
// win = the sector window drawn (x0, y0, x1, y1 inclusive, gameplay sectors only).
struct DioXform
{
    double c[3], ex[3], ey[3], n[3];
    double L, D, lenX, lenY, refY;
    int win[4];
    int me;
};
bool DioramaXform(DioXform *x);
// The carried unit (squad leader / own host station) and the hand position (tracking
// metres): the engine draws the real model there. False = nothing carried.
bool DioCarry(uint32_t *gid, double hand[3]);
// The unit under the right ray (board hover), 0 = none: the engine adds a glow pass.
uint32_t DioHoverGid();
// Passthrough (eye FBO bound, after the miniature): alpha 1 where it drew depth.
void DioPassthroughAlpha();
// darken the drawn miniature by a (0..1), alpha untouched (station beam fade on the
// mixed-reality table; the passthrough room stays). Eye FBO bound, after DioPassthroughAlpha.
void DioDim(float a);
// re-centre the diorama window on the own host station at the next gameplay frame
// (after a station beam; only matters on maps wider than the window).
void DioRecentre();
// the build wheel opened over the table this poll: release carry / ring / hover now.
void WheelOpened(uint32_t now);
// Point on the relief top at (s, t), tracking space.
void Point(double s, double t, double out[3]);
// Dev aid target for UA_VR_CMD_AIM=..:TOKEN (the first own squad token).
bool ScriptToken(double out[3]);
// Dev aid target for UA_VR_CMD_AIM=..:HOST (the own host station token, station).
bool ScriptHostToken(double out[3]);

// Controller pass (Cmd::Filter, after the board ray): grips on the table.
// boardOpen = the board/table is shown. Swallows the grips it owns. Returns
// true while a grab or a carried token owns the hands (no board clicks then).
// physGrip = the grips as polled, before any layer zeroed them (edge detection).
// xDown: X physically down this poll = cancel an unconfirmed beam ring
// before any confirm (squeezes already claimed stay swallowed until released).
bool Input(ControllerState c[2], const float physGrip[2], bool boardOpen, uint32_t now, bool xDown = false);
bool Busy();
// drop carry / beam ring state at a level boundary (Station's level identity).
void ResetLevel();

// Eye pass, called by Cmd::DrawEye in place of the flat board quad.
// tex = the board texture (owners + grid). hoverKind/hoverGid/hit (s,t) from
// the command layer's pick; selCmdr = the selected squad's leader.
void DrawEye(int eye, const ViewState &views, double zn, double zf, uint32_t tex, int hoverKind, uint32_t hoverGid,
             bool hit, double hs, double ht, uint32_t selCmdr);
// Eye pass, after the command layer's black fade quad (over the lifting black):
// the "cockpit boot" flash after a token dive. fade = the current black level.
void DrawFx(int eye, const ViewState &views, double zn, double zf, float fade);
bool FxOn(); // the boot flash is pending or running
// Texture detail for the board image (pixels per sector cap, units drawn?).
int TexCellCap();

} // namespace Table
} // namespace VR

#endif
