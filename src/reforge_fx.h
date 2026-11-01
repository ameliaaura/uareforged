#ifndef REFORGE_FX_H_INCLUDED
#define REFORGE_FX_H_INCLUDED

// Reforge effects: death / explosion / genesis effects.
//
// Inert unless BOTH hold: UA_FX is not "0" AND an fx pack exists
// (UA_FX_DIR=<dir>, else <hd-models dir>/fx/ with an fx.txt in it).
// UA_FX=0 (or no pack) = the original code paths, byte for byte: nothing in
// this file is called on them except On(), which then returns false.
//
// What the pack changes (visual only; particle counts, life times, sizes,
// speeds, emit timing, spawn positions and every gameplay value stay the
// original ones):
//   * particle sprites: every particle life stage whose original bitmap
//     (ANM / ILBM resource name) is mapped in fx.txt draws our own flipbook
//     instead (frame = particle age / life, per-particle spin), additive or
//     alpha-blended per kind. Unmapped stages keep the original sprite.
//   * ground light: kinds flagged "ground" also draw a horizontal additive
//     light pool on the terrain under the particle.
//   * fx models: <pack>/models/<visproto>.obj replace visprotos like the
//     hd-models override (genesis shells VPg*, effect models), only while
//     the pack is on.
//
// fx.txt (one directive per line, # comments):
//   kind <name> <png> <cols> <rows> <frames> <add|alpha> [spin=<rad/s>] [ground=<scale>]
//        [gain=<0..1>] [size=<mul>] [var=<n>] [glow=<0..1>] [gfrac=<0..1>]
//   ground <png>                 light-pool texture for "ground" kinds
//   map  <BITMAP NAME> <kind>
//
// UA_FX_STAGE=1 (tooling, needs a level): scripted effect stage for captures,
// see reforge_fx.cpp.

#include <string>
#include <vector>
#include "vectors.h"

namespace GFX { struct TMesh; struct TRenderNode; }
class NC_STACK_particle;
class NC_STACK_ypaworld;
struct area_arg_65;

namespace ReforgeFx
{

bool On();
const std::string &PackDir();

struct Kind
{
    std::string name;
    int frames = 1;
    bool additive = true;
    float spin = 0.0;      // rad/s, sign random per particle
    float ground = 0.0;    // 0 = no ground light; else pool radius = scale * ground
    float gain = 1.0;      // vertex colour (additive brightness / alpha)
    float size = 1.0;      // sprite size multiplier (visual only)
    int variants = 1;      // frames split into n equal runs; run = per-particle hash
    float glow = 1.0;      // ground pool brightness at particle birth (fades with age)
    float gfrac = 1.0;     // fraction of particles (by hash) that light the ground
    std::vector<GFX::TMesh *> meshes; // one quad per frame
};

// Kind for a particle life-stage bitmap name; -1 = keep the original.
int KindForBitmap(const std::string &name);
Kind *GetKind(int id);

// Ground light pool mesh (shared), brightness level 0..kGroundLevels-1 (top = full).
const int kGroundLevels = 8;
GFX::TMesh *GroundMesh(int level = kGroundLevels - 1);

// Draw one particle the FX way. Returns false when the stage is unmapped
// (caller draws the original).
bool RenderParticle(NC_STACK_particle *gen, size_t stage, int32_t age, uint32_t seed,
                    const vec3d &worldPos, float scale, area_arg_65 *rndr);

// Tooling: scripted stage (UA_FX_STAGE). Called once per rendered game frame.
void StageFrame(NC_STACK_ypaworld *yw, int32_t timeStamp);
// Render-camera pose while the stage frames an event (PerfCamPose hook).
bool StageCamera(vec3d *eye, vec3d *target);
// UA_FX_STAGE_SHOT=<ms>: shot name for this frame ("fxs_<stage ms>", empty = none); GFX end of frame.
std::string PendingShot();

}

#endif
