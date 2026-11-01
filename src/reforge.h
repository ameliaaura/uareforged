#ifndef REFORGE_H_INCLUDED
#define REFORGE_H_INCLUDED

// Reforge Visuals — engine-side tooling. Everything here is inert unless its
// env var is set (or, for the override, unless the hd-models directory exists).
//
//   hd-models override   <cwd>/hd-models/<visproto>.obj (dir: UA_HDMODELS_DIR)
//                        replaces the visproto of that name after the set loads.
//   hd lego override     <hd-models>/lego/set<N>/<lego name>.obj replaces the
//                        set-N lego piece (sector building) of that set.sdf name.
//   UA_EXPORT_LEGO       export original lego pieces per set, exit.
//   UA_EXPORT_VISPROTO   export original visprotos as OBJ/MTL/PNG + export.json, exit.
//   UA_VIEWER            deterministic turntable PNGs of visprotos, exit.
//
// See reforge.cpp for the full env var reference.

class NC_STACK_base;
class NC_STACK_ypaworld;
struct TLevelDescription;
#include "vectors.h"

namespace Reforge
{

// Called by load_set_base() once the combined set object exists (either the
// set.base path or the per-file .lst path). visprotoRoot is the first kid.
void ApplyHdModels(NC_STACK_base *visprotoRoot);

// Lazy hd models (default; UA_HD_LAZY=0 = load every hd state at set load):
// HdPreloadLevel() at level load, after the set and the level description, before
// any unit exists, loads the hd states the level can show; HdEnsure() loads a
// state first needed later ("hd-models: LATE" in the log).
void HdPreloadLevel(NC_STACK_ypaworld *yw, const TLevelDescription *mapp);
void HdEnsure(NC_STACK_ypaworld *yw, int vpIndex);

// Called by load_set_base() right after ApplyHdModels with the second kid (the
// lego group, set.sdf section 1 order). Inert unless <hd-models>/lego/set<N>/
// exists for the current rsrc: set. UA_GFX_BUILDINGS=0 turns it off entirely
// (original legos); levels 1/2/3 pick the far-LOD distance 900/1700/3000 u
// (default 2).
void ApplyHdLegos(NC_STACK_base *legoRoot);

// Lego far LOD (sector buildings). <hd-models>/lego/set<N>/<name>.far.obj
// beside an hd lego is drawn instead of it beyond UA_LEGO_LODDIST units from
// the camera (default 1700, 0 = off). UA_PERF_LEGOLOD=<dist> (perf runs):
// same distance, and slots without a .far.obj keep the original lego as far.
// Returns `near` itself when no far mesh applies.
NC_STACK_base *LegoLod(NC_STACK_base *near, const vec3d &pos);

// Called at the top of every menu (shell) frame. Returns true when a tool mode
// has finished and the engine should quit.
bool MenuFrameHook();

}

#endif
