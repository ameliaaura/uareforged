#ifndef UA_VR_OVERLAY_H_INCLUDED
#define UA_VR_OVERLAY_H_INCLUDED

// Tiny GL overlay drawer for the VR module: world-space reticle and laser
// beam into the eye targets, pointer cursor into the 2D panel. Draws quads
// given directly in GL clip space (the caller projects), premultiplied alpha,
// no depth test. Saves and restores every GL state it touches, so the
// engine's state cache stays valid. Desktop GL 4.1 and GLES 3.0+.

#include <stdint.h>

namespace VR
{
namespace Overlay
{

enum Style
{
    STYLE_RETICLE = 0, // ring + centre dot + four ticks, dark outline
    STYLE_BEAM = 1,    // soft-edged strip (uv.x across)
    STYLE_DOT = 2,     // filled disc with outline
    STYLE_TEXTURE = 3, // RGBA texture (straight alpha), e.g. the game cursor
};

bool Begin();  // false if the program could not be built (then draw nothing)
void End();
// corners: 4 clip-space vertices (x,y,z,w) in order (0,0) (1,0) (1,1) (0,1) of uv.
void Quad(const float corners[4][4], Style style, float r, float g, float b, float a, uint32_t tex = 0);

} // namespace Overlay
} // namespace VR

#endif
