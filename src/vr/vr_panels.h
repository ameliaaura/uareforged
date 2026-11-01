// HUD panels FIXED / FREE.
// FIXED (default) = the one 2D panel quad exactly as before. FREE = the 2D panel shown as
// two quads cut at the tool bar's top: the top panel (stats bar, windows, messages) and
// the bottom panel (tool bar + its tooltip line). The right laser + trigger moves a panel
// by its outer edge (a grab bar: above the top panel, below the bottom one) and resizes it
// by the outer corners (aspect kept); the right stick pushes / pulls while moving. A panel
// put to the side turns to face the head. Layout per place (cockpit / host station with
// the war table), saved in the profile.
#ifndef UA_VR_PANELS_H_INCLUDED
#define UA_VR_PANELS_H_INCLUDED

#include <stdint.h>
#include "vr.h"

class NC_STACK_ypaworld;

namespace VR
{
namespace Panels
{

void Init();
// Present, after the fixed pose (and the bridge theatre): with FREE and a cockpit-mode
// panel, turn fe->panel into the top quad + fe->panel2 (fe->split). ctx 0 cockpit place,
// 1 host station with the war table up.
void Apply(FrameEnd *fe, int ctx, const ViewState &views);
// Cmd::Filter, before the table / field / board / laser read the right hand. Returns
// true while a panel grab owns the right trigger (zeroed for everyone else).
bool Filter(ControllerState c[2], bool otherOwnsRight, uint32_t nowMs, int poll); // poll = the command layer's controller poll count (dev aids)
// Cmd::Filter while the command layer is not live (menus, loading): no grabs, but a
// squeeze a grab owns stays swallowed until released.
void FilterIdle(ControllerState c[2]);
// Eye pass: grab bars / corner handles / frame while the laser is near or grabbing.
void DrawEye(int eye, const ViewState &views, double zn, double zf);
// After each gameplay frame: profile save after a move / resize / reset.
void Gameplay(NC_STACK_ypaworld *yw);

} // namespace Panels
} // namespace VR

#endif
