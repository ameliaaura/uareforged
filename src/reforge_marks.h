#ifndef REFORGE_MARKS_H_INCLUDED
#define REFORGE_MARKS_H_INCLUDED

// Reforge MARKS: battle marks that stay - scorch where things
// explode, craters + smouldering smoke where wrecks land and buildings fall,
// tank tracks on the ground. Purely visual: no gameplay value, no game RNG.
//
// Level = gfxLevel("MARKS", 0) (UA_GFX_MARKS / preset row "BATTLE MARKS"):
//   0  off: every hook returns at once, nothing is allocated, generated or
//      drawn (the original draw path, byte for byte).
//   1  scorch only, small pool (40), short life (75 s), 512 px atlas.
//   2  + craters, smouldering wreck/building smoke; pool 120, life 180 s.
//   3  + tank tracks, embers in the smoke; pool 240 (+400 track segments),
//      life 360 s.
// Every pool is fixed-size (oldest replaced), marks fade out at the end of
// their life. Decals are small height-sampled grids laid on the sector
// collision surface (the 1998 terrain, slopes and roofs) and drawn as ONE
// alpha-blended batch after the solid pass, pulled a fraction of their depth
// towards the camera (no z-fighting, same pixels; GLES-friendly: no extra
// pass, no depth read, no shader). Smoke/embers = one camera-facing batch.
// The atlas is generated procedurally at first use (reforge_marks_tex.h).
//
// Tooling (inert unless set): UA_MARKS_STAGE=1 scripted battle on the level
// (look sheets), UA_MARKS_FILL=1 fills every pool to its cap at level start
// (worst-case cost runs), UA_MARKS_DEBUG=1 logs every mark.

#include "vectors.h"

class NC_STACK_ypaworld;
class NC_STACK_ypabact;
struct area_arg_65;

namespace ReforgeMarks
{

int Level();   // 0..3, cached per GfxSettings generation

void OnLevelLoad();                                                   // clears the pools
void OnImpact(NC_STACK_ypaworld *yw, const vec3d &pos, int energy);   // weapon hit (ypamissile DEAD)
void OnWreck(NC_STACK_ypaworld *yw, NC_STACK_ypabact *b);             // unit wreck lands (megadeth)
void OnBuilding(NC_STACK_ypaworld *yw, const vec3d &pos, bool big);   // building destroyed (in view)
void OnUnitDrawn(NC_STACK_ypaworld *yw, NC_STACK_ypabact *b);         // tracks (RenderSector)
void Render(NC_STACK_ypaworld *yw, area_arg_65 *rrg);                 // queue the batches (RenderGame)

// UA_MARKS_STAGE camera (PerfCamPose hook).
bool StageCamera(vec3d *eye, vec3d *target);

}

#endif
