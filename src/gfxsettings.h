#ifndef GFXSETTINGS_H_INCLUDED
#define GFXSETTINGS_H_INCLUDED

// Reforge graphics settings : ONE place that says how
// much of each new visual feature to draw.
//
// Every feature reads one integer level 0..3 (0 = off = the original draw path,
// 1 low, 2 medium, 3 high):
//
//     int lv = gfxLevel("SHADOWS", 0);     // dflt = what the engine did before
//
// Resolution order (first hit wins):
//   1. env UA_GFX_<NAME>=0..3                       (tests, launchers, one-off A/B)
//   2. the feature's legacy env switch, if it had one (env.sh keeps working):
//        V2       UA_RENDER_V2=0 -> 0, else 2 (far-v2 on unless UA_R2_FARV2=0 -> 1)
//        HDMODELS UA_HDMODELS_DIR=<missing dir> -> 0 (an existing dir is a location, not a switch)
//        FX       UA_FX=n   FLOOR UA_R2_GROUND=n   DRAWDIST UA_VIEW_SECTORS=n (sector count)
//   3. the player's saved per-feature override        (Graphics menu, SAVE/gfx.txt)
//   4. the active preset's table entry                (Original/Low/Medium/High/Ultra/VR)
//   5. dflt
// Steps 3-4 only apply while a preset is in force: a saved SAVE/gfx.txt, or
// UA_GFX_PRESET / UA_GFX_DEFAULT in the environment. With none of them (every
// test harness and old launcher) gfxLevel() returns the legacy/env value or
// dflt, i.e. the engine behaves exactly as before this module existed.
//
// When to read it: the levels only change in the options screen (shell), and
// the engine reloads the set when a model/renderer level changed, so reading
// at level load (or caching by GfxSettings::Generation()) is enough. gfxLevel()
// is a small map lookup: fine per level/per object, cache it for per-draw use.
//
// Files: SAVE/gfx.txt (UA_GFX_FILE=<path> overrides; tests must point it at a
// temporary file). Format: "preset = high", "SHADOWS = 1" (overrides), "bench = ...".

#include <stdint.h>
#include <string>

int gfxLevel(const char *name, int dflt);

class NC_STACK_ypaworld;

namespace GfxSettings
{
enum
{
    PRESET_NONE = -1,   // no preset in force: legacy behaviour
    PRESET_ORIGINAL = 0,
    PRESET_LOW,
    PRESET_MEDIUM,
    PRESET_HIGH,
    PRESET_ULTRA,
    PRESET_VR,          // VR standalone (Quest line); documented, selectable for tests
    PRESET_COUNT
};

void Init();                         // idempotent; first gfxLevel() call does it
bool Active();                       // a preset is in force
uint32_t Generation();               // bumps on every change of levels

// Feature table (menu rows).
int FeatureCount();
const char *FeatureName(int f);      // "SHADOWS"
const char *FeatureLabel(int f);     // "SHADOWS" as shown in the menu
const char *LevelLabel(int f, int lv);
int FeatureIndex(const char *name);  // -1 = not in the table
int PresetLevel(int preset, int f);
const char *PresetName(int p);       // "ORIGINAL", "LOW", ...
const char *PresetKey(int p);        // "original", "low", ...

// Player state (Graphics menu). Changes apply at the next level load.
int GetPreset();
void SetPreset(int p);               // also clears every per-feature override
int GetOverride(int f);              // -1 = follows the preset
void SetOverride(int f, int lv);     // lv -1 clears
int EffectiveLevel(int f);           // what gfxLevel() returns for that row now

// Skies: Original / New (players choose the original skies or the new ones).
// A player choice of its own, independent of the preset: it survives preset changes (only "Back to
// Original" clears it). SKIES_FOLLOW = the preset's SKY level; SKIES_ORIGINAL = SKY 0 (the 1998 skies,
// byte-identical path); SKIES_NEW = the SKY override/preset level, at least kSkiesNewMin when the
// preset itself has SKY 0 (Original preset + new skies). Saved as "skies = original|new".
// UA_GFX_SKY / env still wins (tests).
// Three choices (Original, New, New + 3D clouds): SKIES_NEW3D = new skies with the
// Blender 3D cloud towers baked in (row SKY3D 1; skies without a 3D bake fall back to the New panorama).
// SKIES_NEW = SKY3D 0; SKIES_ORIGINAL = SKY 0 and SKY3D 0; FOLLOW = both rows from the preset. Saved as
// "skies = original|new|new3d".
enum { SKIES_FOLLOW = -1, SKIES_ORIGINAL = 0, SKIES_NEW = 1, SKIES_NEW3D = 2 };
int GetSkies();
void SetSkies(int s);
int SkiesFor(int preset, int skyOverride, int skies); // effective SKY level for a menu conf copy
int Sky3dFor(int preset, int sky3dOverride, int skies); // effective SKY3D level for a menu conf copy
bool EnvLocked(int f);               // env/legacy forces it (menu shows it, can't change)
bool Save();

// Values implied by the levels for the legacy tuning env vars (V2 calibration,
// far-v2 knobs). Returns getenv(name) if set; else, when the V2 level comes from
// a preset/override, the New Look value for that level; else NULL.
const char *Param(const char *envName);

// Draw distance: sectors per side for the far view (iniSectors = gfx.view_sectors).
int ViewSectors(int iniSectors);

// Level load hook (LevelCommonLoader): applies V2 + draw distance; returns true
// once when model/renderer levels changed since the set was loaded (the caller
// then reloads the set so hd models / textures follow the new levels).
void OnLevelLoad(NC_STACK_ypaworld *yw);
bool TakeSetReload();

// First-launch benchmark (see gfxsettings.cpp).
int BenchLevelId();                  // >0: autostart this level for the bench
int BenchSeconds();
bool BenchRunning();                 // a bench level is up (camera path, uncapped)
void BenchFrame(double ms);          // per gameplay frame while running
void BenchLevelEnded();              // level done: decide + save
}

#endif
