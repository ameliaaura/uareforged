#ifndef SYSTEM_ATMOS_H_INCLUDED
#define SYSTEM_ATMOS_H_INCLUDED

// Reforge ATMOSPHERE: per-level light and haze that follow the level's sky.
//
//   gfxLevel("ATMOS", 0)   0 = off: the 1998 / New Look draw path exactly (no file read, no program compiled,
//                              no uniform touched, V2SunParams returns the calibrated New Look sun)
//                          1 = sky-matched sun direction + sun/ambient tint, legacy colour grade, distance haze
//                              (the 1998 black depth cue fades to the sky's horizon colour instead)
//                          2 = 1 + night maps: lit windows on the 1998 buildings, brighter host-station lights
//                          3 = 2 + light weather where the sky's look supports it (dust / rain / snow)
//
// Data: one small text table (gen_atmos.py, one line per sky; read at level load only when ATMOS > 0):
//   $UA_ATMOS_FILE, else <UA_SKY_DIR | hd-sky>/atmos.txt
//
// THE SHARED SUN. This header is the one place that says where the sun is for a level. GFXEngine::V2SunParams()
// (gfx.cpp; used by the V2 renderer, the floor shader and SHADOWS) returns Atmos::SunDir() when
// Atmos::Active(), else the New Look calibration (UA_R2_SUN). Glow/shadows/ground must not keep their own sun:
// call V2SunParams() (direction + strengths) and, for colour, Atmos::Get().sunTint / ambTint.
// Directions are UA world space: x right, y DOWN, z forward; the vector points TOWARDS the sun; azimuth is
// atan2(x, z) in degrees = the sky panorama's u (reforge_sky.cpp PanoUV), so the light comes from the sun burst
// painted in the sky.

#include <string>

class NC_STACK_ypaworld;

namespace Atmos
{
enum Family { FAM_CLEAR = 0, FAM_OVERCAST, FAM_STORM, FAM_NIGHT };
enum Weather { W_NONE = 0, W_DUST, W_RAIN, W_SNOW };

struct State
{
    int level = 0;          // effective ATMOS level for this level load (0 = off)
    bool active = false;    // level > 0 and the sky has a row in the table
    std::string sky;        // sky base name ("x7")
    int family = FAM_CLEAR;
    bool night = false;
    bool newSky = false;    // the remastered panorama is drawn (haze from it), else the 1998 dome
    float sunAz = 0.0f, sunEl = 44.0f;   // degrees
    float sunDir[3] = {-0.551f, -0.695f, -0.462f}; // world, unit, towards the sun
    float sunTint[3] = {1, 1, 1};  // linear multiplier on the V2 sun colour (strength included)
    float ambTint[3] = {1, 1, 1};  // linear multiplier on the V2 ambient
    float grade[3] = {1, 1, 1};    // linear multiplier on baked shade (1998 meshes, ground)
    float haze[8][3] = {};         // display 0..1, sector i centred on azimuth -180 + 45 i
    float nearStart = 600.0f, nearRange = 2400.0f, nearMax = 0.0f; // aerial haze (view depth)
    float windows = 0.0f;          // level 2+, night: lit-window strength on 1998 buildings (0 = off)
    float hostLight = 1.0f;        // level 2+, night: multiplier on V2 emissive (host-station / unit lights)
    int weather = W_NONE;          // level 3 only
    float weatherK = 0.0f;         // density 0..1
};

const State &Get();
inline bool Active() { return Get().active; }

// Level load (yw_loadSky, after the sky is built). skyName = the level's "objects/x7.base"; newSky = SKY
// drew its panorama. Reads gfxLevel("ATMOS", 0); level 0 returns before any file access.
void OnLevelLoad(NC_STACK_ypaworld *yw, const std::string &skyName, bool newSky);

// Haze colour (display 0..1) towards a world direction (only x, z used).
void HazeAt(float wx, float wz, float out[3]);
}

#endif
