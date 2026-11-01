#ifndef REFORGE_SKY_H_INCLUDED
#define REFORGE_SKY_H_INCLUDED

// Reforge SKY: remastered sky panoramas.
//
// One call in yw_loadSky, after the original .bas loaded and before its static
// fog / VBO are computed:
//
//     ReforgeSky::Apply(sky, skyname, skyHeight, gfxLevel("SKY", 0));
//
// Level 0 returns at once: no file probe, no texture, no mesh change (the
// original draw path, byte for byte). Levels 1..3 look for an authored
// panorama keyed on the sky's basename (objects/x4.base -> "x4"):
//
//   <dir>/<envkey>/<sky>/sky_<W>.png|.dds   level-specific (palette) variant, wins
//   <dir>/<sky>/sky_<W>.png|.dds            shared
//
// <dir> = UA_SKY_DIR, else "hd-sky" in the game dir. W = 1024 (level 1),
// 2048 (2), 4096 (3); the largest file not above the level's width is used, so
// a pack may ship only some sizes. No file = the original sky stays (logged).
//
// The panorama is an equirectangular strip: u = azimuth (0..1 over 360 deg,
// same direction as the original tiles), v = elevation seen from the viewer
// (0 = zenith, 1 = kElevMin below the horizon). The original dome geometry is
// kept exactly (same planes, so the static fog and the "new sky" ground fade
// behave the same); each triangle is only subdivided so the panorama's UVs can
// follow the view direction, and its texture swapped for the panorama.
//
// Optional <sky>/sky.txt keys: "drift = <deg per minute>" (slow azimuth drift
// of the panorama, levels 2-3 only; 0 = static like the original).

#include <stdint.h>
#include <string>

class NC_STACK_base;

namespace ReforgeSky
{
const float kElevMin = -40.0f; // panorama bottom edge, degrees

// true = the sky now uses the remastered panorama.
// clouds3d > 0 (SKY3D row): prefer the sky's 3D-cloud panorama (sky3d_<width>.dds|png in the same
// folder); a sky without one keeps its New panorama (sky_<width>).
bool Apply(NC_STACK_base *sky, const std::string &skyName, int skyHeight, int level, int clouds3d = 0);

// Frees the panorama texture (call when the sky object is deleted). No-op when
// nothing was loaded.
void Release();

// Per-frame hook from yw_renderSky: advances the drift (re-uploads the dome's
// UVs at most a few times a second). No-op unless a drifting panorama is live.
void Tick(NC_STACK_base *sky, uint32_t timeMs);

// Dev aid (sky comparison sheets; inert unless UA_SKY_CYCLE is set): show several skies in ONE level run with the
// same camera. UA_SKY_CYCLE=x4,x7,... ; UA_SKY_CYCLE_START=<sky renders before the first swap> (default 400);
// UA_SKY_CYCLE_FRAMES=<renders per sky> (default 8). Called once per sky render: true = swap the sky object to
// *next ("objects/<name>.base"). The last render of each sky queues a shot "skycyc_<NN>_<name>"; after the list
// it prints "SKYCYCLE DONE". NOTE: every NEWSKY-type sky embeds bitmaps named NEWSKY1/NEWSKY2, and a swap
// inside a running level reuses the cached bitmaps of the first sky -> only valid for the remastered levels
// (1-3, panorama loaded by sky key); for the 1998 sky use UA_SKY_FORCE=<sky> (one level load per sky).
bool CycleStep(std::string *next);
// Level load (yw_game, before the sky loads): UA_SKY_FORCE=<sky> or NULL. The sky dev-aid env (UA_SKY_FORCE,
// UA_SKY_CYCLE*) is read once, at the first level load with SKY > 0; at SKY 0 nothing is read and no dev aid acts.
const char *ForcedSky(int level);
std::string PendingShot();
}

#endif
