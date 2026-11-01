#ifndef UA_VR_COCKPIT_H_INCLUDED
#define UA_VR_COCKPIT_H_INCLUDED

// Urban Assault VR — cockpit layer.
//
//  * Cockpit shell per vehicle class (ground, helicopter, jet, station gun):
//    a still frame fixed to the comfort anchor, built from boxes and quads in
//    code (no textures; procedural wear in the fragment shader). One static
//    VBO, one draw per eye.
//  * Dash screens on three fixed zones (Titanfall 2 layout): health along the
//    top edge, weapons lower-left, target + radio upper-right. Text is a 5x7
//    bitmap font in a small R8 atlas. One dynamic VBO, one draw per eye.
//  * Left-wrist panel: squads, radar, host-station energy; fades in when you
//    look at it.
//  * Shake -> haptics: the engine's view shake is never applied in VR; its
//    magnitude and the hits on the viewer go to controller haptics.
//
// vr_engine.cpp calls these from its frame hooks; the public switch is in vr.h
// (CockpitShellsOn / SetCockpitShellsOn / CockpitOwnsHud).

#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Cockpit
{

void Init(Backend *backend, double unitsPerM);
// Gameplay frame (after Process): readout snapshot, hit/shake haptics,
// UA_VR_TAKE dev aid, per-second log.
void Gameplay(NC_STACK_ypaworld *yw);
// Inside the eye pass (eye FBO bound, after the world, before the comfort
// vignette and the reticle). Returns true when a shell was drawn.
bool DrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const ViewState &views,
             const ControllerState ctrl[2]);
// diorama: the left-wrist panel alone (vehicle, miniature up), over the miniature.
bool DrawWristEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const ViewState &views,
                  const ControllerState ctrl[2]);
// The engine's view-shake rotation this frame (degrees) and whether it was applied.
void NoteShake(double angleDeg, bool applied);
// All VR haptics go through here (counted for the log). why: "hit", "shake", "ff".
void Haptic(int hand, float amplitude, float durationMs, const char *why);
// True while a shell is shown (the comfort rim is redundant then).
bool ShellVisible();
// UA_VR_ALLOW_SHAKE=1 (A/B only): the engine shake moves the view again.
bool AllowShake();
// field orders (vr_field.cpp, WRIST SQUADS on): the squad row of the left-wrist panel
// under a ray (XR tracking space, metres; the panel as last drawn, only while it
// faces the eye). gid = the squad commander.
bool WristPick(const double o[3], const double d[3], uint32_t *gid, double hit[3], double *dist);
bool WristRowCentre(int row, double xr[3]); // simulator aim aid
void SetWristHover(uint32_t gid);           // highlighted row (0 = none)

} // namespace Cockpit
} // namespace VR

#endif
