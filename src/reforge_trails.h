#ifndef REFORGE_TRAILS_H_INCLUDED
#define REFORGE_TRAILS_H_INCLUDED

// Reforge TRAILS: muzzle flashes, gun / cannon tracers,
// missile exhaust + smoke trails. Visual only: no gameplay value, no game RNG,
// no unit state is read back into the game.
//
// Level = gfxLevel("TRAILS", 0) (env UA_GFX_TRAILS, presets via gfxsettings):
//   0  off = original. Every hook is `if (ReforgeTrails::Level())` -> nothing
//      is loaded, allocated, queued or drawn; the frame is the 1998 one.
//   1  muzzle flashes (guns, cannon, launchers) + short tracers (MG, shells,
//      energy wakes) + missile exhaust flare.
//   2  + missile / rocket smoke trails, muzzle smoke on cannon; the 1998 trail
//      sprites listed as `hide` in the pack (FIRESTORM's orange beads) are not
//      drawn any more (our smoke replaces them). Level 0/1 keep them.
//   3  + longer / denser trails, smoke lingers and drifts, longer tracers.
// Pack: UA_TRAILS_DIR, else <UA_HDMODELS_DIR or hd-models>/trails (trails.txt,
// tr_hot.png, tr_smoke.png; made by make_trails_pack.py).
// No pack = level forced to 0.
//
// Hot parts (flash, tracer, exhaust, energy wake) are LUMTRACY additive draws,
// so the GLOW pass picks them up; smoke is LUMTRACY + RFLAGS_FX_ALPHA, which
// GLOW replays as an occluder only (no glow).
// Fixed pools (ring buffers, oldest overwritten): see kCap* in the .cpp.
//
// UA_TRAILS_STAGE=1 (tooling, level 97): scripted firing range for captures,
// UA_TRAILS_STAGE_SHOT=<ms> names shots by stage time (trs_<ms>.ppm).

#include <string>
#include <stdint.h>
#include "vectors.h"

class NC_STACK_ypabact;
class NC_STACK_ypamissile;
class NC_STACK_ypaworld;
class NC_STACK_particle;
struct area_arg_65;

namespace ReforgeTrails
{

int Level();   // 0..3, cached; re-read when the graphics settings change

// Hooks (call only when Level() > 0).
void Launch(NC_STACK_ypamissile *m, NC_STACK_ypabact *launcher, const vec3d &pos);   // LaunchMissile, per weapon object
void MissileMoved(NC_STACK_ypamissile *m);                                            // end of ypamissile::Move
void MgunShot(NC_STACK_ypabact *shooter, const vec3d &to);                           // FireMinigun, per visible shot
void Render(area_arg_65 *rrg, int32_t dtime);                                        // after the particle system
bool HideParticle(NC_STACK_particle *gen);  // level >= 2: a 1998 trail sprite our smoke replaces (pack `hide` list)

// Tooling (inert unless UA_TRAILS_STAGE is set).
void StageFrame(NC_STACK_ypaworld *yw, int32_t timeStamp);
bool StageCamera(vec3d *eye, vec3d *target);
std::string PendingShot();

}

#endif
