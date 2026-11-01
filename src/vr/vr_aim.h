#ifndef UA_VR_AIM_H_INCLUDED
#define UA_VR_AIM_H_INCLUDED

// Internal to the VR module: world-space reticle, laser pointer, controls
// card state. vr_engine.cpp calls these from its frame hooks; the public entry
// points the game calls are declared in vr.h. See

#include "vr.h"
#include "vr_math.h"

namespace VR
{
namespace Aim
{

void Init();
// After PollControllers: laser ray vs the last submitted panel quad.
void FrameStart(const ControllerState ctrl[2]);
// Engine centre-eye camera for this frame (UAV rows incl. the cull scale, eye
// position in world units), set at the end of BeginWorldView.
void SetCentreCamera(const M3 &rowsScaled, const double pos[3]);
// Inside the eye pass (eye FBO bound, before ReleaseEye): reticle + beam.
// noReticle (diorama): only the panel laser beam (the world reticle belongs to the
// full-scale world, which is not drawn while the miniature is up).
void DrawEye(int eye, const ViewState &views, const M4 &eyeFromCentre, double zn, double zf, bool noReticle = false);
// Panel FBO bound, right before Backend::EndFrame: pointer cursor.
void DrawPanel(int panelW, int panelH);
// The panel quad that was submitted this frame (pointer hit test next frame).
void NotePanel(const PanelLayer &panel, int panelW, int panelH, const PanelLayer *panel2 = NULL); // panel2 = the bottom quad (PANELS FREE)
// Gameplay frame (after Process): controls card trigger.
void Gameplay(NC_STACK_ypaworld *yw);

// cockpit (vr_cockpit.cpp): what the gun ray / lock sees, copied by value in
// NoteUserAim (no unit pointers survive the frame). `unit` = the user unit the
// ray was cast for (compare with the viewer).
struct TargetSnap
{
    bool valid = false;            // NoteUserAim ran for `unit`
    const void *unit = NULL;
    bool any = false;              // a unit is locked or on the ray
    bool locked = false;           // UserTargeting lock (yw->_guiVisor.field_18)
    int vehicleId = -1, energy = 0, energyMax = 1, owner = 0;
    double distUnits = 0;          // eye-independent: from the gun origin
    bool rayHit = false;           // the ray hits something (ground, building, unit)
    double rayDistUnits = 0;
};
const TargetSnap &Target();

} // namespace Aim
} // namespace VR

#endif
