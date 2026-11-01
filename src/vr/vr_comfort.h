#ifndef UA_VR_COMFORT_H_INCLUDED
#define UA_VR_COMFORT_H_INCLUDED

// Urban Assault VR — comfort layer.
//
//  * Horizon mode per vehicle class: the cockpit anchor keeps only the vehicle
//    heading (yaw), heading + smoothed pitch (no roll), or the full attitude.
//  * Vertical anchor smoothing: terrain bumps do not jolt the head; constant
//    slopes are tracked without lag; horizontal motion is never filtered.
//  * Eye raise: the seated eye sits higher above the vehicle (ground class).
//  * Comfort vignette: periphery darkens with artificial angular velocity and
//    linear acceleration (drawn into each eye, identical for both).
//  * Static reference: a dim cockpit rim + nose chevron fixed to the anchor.
//
// Everything is driven by env vars read once in ComfortInit(); vr_engine.cpp
// calls these three functions and nothing else.

#include "vr_math.h"

namespace VR
{

void ComfortInit(double unitsPerM);

// In: raw vehicle anchor (rows = right/down/forward in world, plain rotation)
// and anchor position (UA world units). Out: the comfort anchor. `flyer` =
// the viewer is a helicopter/plane/UFO; `velMps` = the viewer's physics
// velocity in m/s (UA world axes), used for the acceleration vignette (noise
// free, unlike differencing positions over integer-ms game ticks); `viewerKey`
// identifies viewer object + level (any change snaps all filters). Call once
// per world frame.
void ComfortAnchor(M3 *ra, double pos[3], bool flyer, const double velMps[3], uint64_t viewerKey);

// Per eye, with the eye framebuffer bound, after the world pass.
//   eyeProj        : clip from scaled centre-eye coords (UaAsymFrustum * eyeFromCentre)
//   centreFromAnchor: scaled centre-eye coords from anchor-local UAV coords (units)
//   fov            : the eye's FOV (vignette is radial around the eye axis)
//   reference      : false while a cockpit shell is drawn (the rim is redundant)
void ComfortDrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const Fovf &fov, bool reference = true);

// Controller combo (both thumbsticks clicked together): cycles the vignette
// preset. Filters `c` in place so the game never sees the combo's clicks (a
// lone click passes ~120 ms late). Returns the label when it fired, else NULL.
// Call once per frame right after polling, before anything reads the clicks.
const char *ComfortFilterCombo(ControllerState c[2]);

// Current vignette drive 0..1 (before strength).
float ComfortVignette();

} // namespace VR

#endif
