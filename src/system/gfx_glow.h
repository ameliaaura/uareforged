#ifndef SYSTEM_GFX_GLOW_H_INCLUDED
#define SYSTEM_GFX_GLOW_H_INCLUDED

// Reforge GLOW (bloom on energy and light), UA_GFX_GLOW=0..3.
//
// The glow source is a MASK, not a luminance threshold: after the scene, the
// frame's additive/lumtracy draws (lasers, arcs, flipbook arc sheets, genesis
// shells, host columns, fire, flashes, muzzle flashes) are replayed into an
// off-screen mask that shares the scene's depth (blitted from the frame), so a
// daylight ground, sky or pale building never blooms and occluded effects do
// not glow through walls. Alpha-blended fx sprites (smoke, debris) replay as
// pure occluders (dst *= 1 - a). The mask is blurred through a dual-filter
// (Kawase-style) half-res pyramid and added onto the frame before the HUD.
//
// Level 0 (the default): Collect() is one
// predictable branch, nothing is allocated, no shader is compiled, the draw
// path is untouched -> byte-identical frame.

#include <cstdint>
#include <string>
#include <vector>

struct ResBitmap;

namespace GFX
{

struct TRenderNode;
struct GfxStates;

class GlowPass
{
public:
    // GFXEngine::Init(): reads the level once (env only, nothing allocated).
    // `capable` = VBO/GL 4.1 path and no colour-effects FBO.
    static void Init(bool capable);
    // GfxSettings::OnLevelLoad(): follow a GLOW change made in the menu (same level: returns at once).
    // Off -> on also makes the set reload (gfxsettings TakeSetReload) so the exclusion tags get written.
    static void OnLevelLoad();
    static int Level() { return _level; }

    // Rasterize(): called for each LUMATR node before it is drawn.
    static inline void Collect(TRenderNode *node)
    {
        if (_level > 0)
            CollectSlow(node);
    }

    // EndScene(): replay + blur + composite (no-op at level 0 / empty frame).
    static void Apply();

    // RenderingMesh(): adjusts the replayed draw's blend/depth state.
    static inline bool Replaying() { return _replaying; }
    static void ReplayStates(GfxStates &st, const TRenderNode *node);

    static void Free();

    // NC_STACK_bitmap::PrepareTexture(): exclusion tag by resource name (no-op at level 0).
    static void TagBitmap(const ::ResBitmap *bitm, const std::string &rsrcName);

private:
    static void CollectSlow(TRenderNode *node);
    static bool Ensure(int w, int h);
    static bool BuildPrograms();
    static int WantLevel();
    static void Configure(int lv);
    static bool _capable;

    static int _level;
    static bool _replaying;
    static bool _broken;
    static std::vector<TRenderNode *> _nodes;
};

}

#endif
