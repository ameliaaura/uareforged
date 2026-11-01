// Reforge graphics settings: presets, per-feature levels, saved file, first-launch
// benchmark. See gfxsettings.h for the contract.

#include "gfxsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <vector>

#include <SDL2/SDL.h>

#include "includes.h"
#include "system/gfx_glow.h"
#include "yw.h"
#include "system/gfx.h"

namespace GfxSettings
{
namespace
{

enum Src { SRC_GFXENV, SRC_LEGACY, SRC_OVERRIDE, SRC_PRESET, SRC_DFLT };

struct Feature
{
    const char *name;
    const char *label;
    // Original, Low, Medium, High, Ultra, VR standalone
    int lv[PRESET_COUNT];
    const char *lvNames[4];
    bool setReload;          // models/textures depend on it: reload the set on change
};

// The preset table. Built features: HDMODELS, V2, FX, BUILDINGS (hd lego dir),
// DRAWDIST. The rest are placeholders for features in progress; their levels are
// read through gfxLevel() from day one, so a feature only fills in the meaning.
// Original must stay all-zero (= the classic game, byte-identical frame).
// High with every newer row (SKY, ATMOS, SHADOWS, GLOW, MARKS, TRAILS, UI 2, BUILDINGS 3 far LOD) at its
// old value must stay = the published New Look.
const Feature kFeatures[] = {
    //  name        menu label            Orig Low Med High Ultra VR
    // Preset levels: SHADOWS, GLOW, TRAILS (Low =
    // flashes+tracers, Med + short smoke, High/Ultra full), MARKS (tracks = level 3 only = High+, 6 min),
    // UI (Med/High 2, Ultra 3 = 4x), SKY (+ own Original/New toggle), ATMOS. VR: every
    // newer row 0 except ATMOS 1 (sun/grade/haze only, no night windows, no weather); the Quest line is a
    // separate engine, nothing measured there yet.
    // SHADOWS, GLOW and SKY
    // rows = the levels proposed with each feature.
    // UI: the 3x HUD/menu pack draws the 2D layer in software at
    // 9x the pixels and uploads 11 MB every frame (+1.4-2 ms per frame at any resolution; Apple M-series, L7575 4K Low
    // +38 % with it, +7 % without) -> off in Low/Medium/VR until a dirty-rect upload lands.
    {"HDMODELS",  "REMASTERED UNITS",  {0, 1, 2, 3, 3, 0}, {"OFF", "ON", "ON", "ON"}, true},
    {"V2",        "MODERN LIGHTING",   {0, 1, 2, 2, 3, 0}, {"OFF", "BASIC", "FULL", "FULL"}, true},
    {"FX",        "NEW EFFECTS",       {0, 1, 2, 3, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, true},
    {"BUILDINGS", "NEW BUILDINGS",     {0, 1, 2, 3, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, true},
    {"HDTEX",     "SHARP TEXTURES",    {0, 1, 1, 1, 1, 0}, {"OFF", "ON", "ON", "ON"}, true},
    {"FLOOR",     "GROUND DETAIL",     {0, 0, 0, 3, 3, 0}, {"OFF", "ON", "ON", "ON"}, false},
    {"DRAWDIST",  "DRAW DISTANCE",     {0, 2, 2, 0, 3, 1}, {"GAME", "SHORT", "MEDIUM", "FAR"}, false},
    {"SKY",       "NEW SKY",           {0, 1, 2, 3, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    {"ATMOS",     "ATMOSPHERE",        {0, 1, 2, 3, 3, 1}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    {"SHADOWS",   "SHADOWS",           {0, 0, 1, 2, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    {"GLOW",      "GLOW",              {0, 0, 1, 2, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    {"MARKS",     "BATTLE MARKS",      {0, 1, 2, 3, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    {"TRAILS",    "TRAILS",            {0, 1, 2, 3, 3, 0}, {"OFF", "LOW", "MEDIUM", "HIGH"}, false},
    // UI + UISTYLE are read once per run (the UI scale must never change mid-run): the menu label says so.
    {"UI",        "SHARP (RESTART)", {0, 0, 2, 2, 3, 0}, {"OFF", "ICONS", "FULL", "4K"}, false},
    // UI graphics: menu look. Modern on
    // High/Ultra, classic elsewhere. Needs UI >= 2 (High 2, Ultra 3).
    {"UISTYLE",   "STYLE (RESTART)",      {0, 0, 0, 1, 1, 0}, {"CLASSIC", "MODERN", "MODERN", "MODERN"}, false},
    // SKY3D: Blender 3D cloud towers baked into the dramatic skies' panoramas (same texture size/format as NEW,
    // measured cost = noise); High/Ultra on, VR off (Quest unmeasured). Only read when SKY > 0.
    // Appended last so every older row keeps its index (menu click scripts, saved files are by name anyway).
    {"SKY3D",     "3D CLOUDS",         {0, 0, 0, 1, 1, 0}, {"OFF", "ON", "ON", "ON"}, false},
};
const int kNumFeatures = sizeof(kFeatures) / sizeof(kFeatures[0]);

// DRAWDIST level -> sectors per side (0 = the game's own gfx.view_sectors, 17 by default).
const int kDrawSectors[4] = {0, 9, 13, 25};

const char *kPresetNames[PRESET_COUNT] = {"ORIGINAL", "LOW", "MEDIUM", "HIGH", "ULTRA", "VR"};
const char *kPresetKeys[PRESET_COUNT] = {"original", "low", "medium", "high", "ultra", "vr"};

// New Look calibration (releases/newlook/env.sh, batch-1 approved look).
const char *kNlSun = "-0.551,-0.695,-0.462,0.70,0.20";
const char *kNlExposure = "0.85";
const char *kNlFarSpec = "1,1.9";
const char *kNlFarEdge = "0.7,0.75,0.35";

bool gInited = false;
int gPreset = PRESET_NONE;
bool gEnvPreset = false;                 // UA_GFX_PRESET: saved overrides ignored
int gOverride[sizeof(kFeatures) / sizeof(kFeatures[0])];
int gSkies = -1;                         // SKIES_FOLLOW / ORIGINAL / NEW (own choice, survives presets)
const int kSkiesNewMin = 2;              // "new skies" under a preset whose SKY level is 0 (Original)
const char *SkiesKey(int s) { return s == SKIES_ORIGINAL ? "original" : (s == SKIES_NEW3D ? "new3d" : "new"); }
uint32_t gGen = 1;
uint32_t gSetSig = 0xFFFFFFFF;
std::string gFile;
std::string gBenchLine;                  // "bench = ..." kept across saves

// Bench state
int gBenchLevel = 0;
int gBenchState = 0;                     // 0 none, 1 armed, 2 running, 3 done
std::vector<float> gBenchMs;
uint32_t gBenchT0 = 0;
int gBenchFrames = 0;

const char *Env(const char *k)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : NULL;
}

bool IsDir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int Clamp3(int v)
{
    return v < 0 ? 0 : (v > 3 ? 3 : v);
}

int ParsePreset(const char *v)
{
    if (!v)
        return PRESET_NONE;
    for (int p = 0; p < PRESET_COUNT; ++p)
        if (!strcasecmp(v, kPresetKeys[p]))
            return p;
    if (v[0] >= '0' && v[0] <= '5' && !v[1])
        return v[0] - '0';
    return PRESET_NONE;
}

std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string FilePath()
{
    if (const char *f = Env("UA_GFX_FILE"))
        return f;
    // The game's save folder (prefix "save:" -> "Save", case-insensitive on
    // the shipped data); the settings are per device, not per profile.
    const char *dirs[] = {"SAVE", "Save", "save"};
    for (const char *d : dirs)
        if (IsDir(d))
            return std::string(d) + "/gfx.txt";
    return "gfx.txt";
}

// Legacy env switches (env.sh keeps working). Returns -1 when not set.
int LegacyLevel(int f)
{
    const char *n = kFeatures[f].name;
    if (!strcmp(n, "V2"))
    {
        const char *v = Env("UA_RENDER_V2");
        if (!v)
            return -1;
        if (!strcmp(v, "0"))
            return 0;
        const char *fv = Env("UA_R2_FARV2");
        return (fv && fv[0] == '0') ? 1 : 2;
    }
    if (!strcmp(n, "HDMODELS") || !strcmp(n, "BUILDINGS"))
    {
        const char *d = Env("UA_HDMODELS_DIR");
        return (d && !IsDir(d)) ? 0 : -1;
    }
    if (!strcmp(n, "FX"))
    {
        const char *v = Env("UA_FX");
        return v ? Clamp3(atoi(v)) : -1;
    }
    if (!strcmp(n, "FLOOR"))
    {
        const char *v = Env("UA_R2_GROUND");
        return v ? Clamp3(atoi(v)) : -1;
    }
    if (!strcmp(n, "DRAWDIST"))
    {
        const char *v = Env("UA_VIEW_SECTORS");
        if (!v)
            return -1;
        int s = atoi(v);
        if (s <= 0)
            return -1;
        return s <= 9 ? 1 : (s <= 13 ? 2 : (s >= 25 ? 3 : 0));
    }
    return -1;
}

int GfxEnvLevel(const char *name)
{
    std::string k = std::string("UA_GFX_") + name;
    const char *v = Env(k.c_str());
    return v ? Clamp3(atoi(v)) : -1;
}

int Resolve(const char *name, int f, int dflt, Src *src)
{
    int v = GfxEnvLevel(name);
    if (v >= 0)
    {
        *src = SRC_GFXENV;
        return v;
    }
    if (f >= 0)
    {
        v = LegacyLevel(f);
        if (v >= 0)
        {
            *src = SRC_LEGACY;
            return v;
        }
        if (gPreset != PRESET_NONE)
        {
            if (!strcmp(kFeatures[f].name, "SKY") && !gEnvPreset && gSkies != SKIES_FOLLOW)
            {
                *src = SRC_OVERRIDE;
                return SkiesFor(gPreset, gOverride[f], gSkies);
            }
            if (!strcmp(kFeatures[f].name, "SKY3D") && !gEnvPreset && gSkies != SKIES_FOLLOW)
            {
                *src = SRC_OVERRIDE;
                return Sky3dFor(gPreset, gOverride[f], gSkies);
            }
            if (!gEnvPreset && gOverride[f] >= 0)
            {
                *src = SRC_OVERRIDE;
                return gOverride[f];
            }
            *src = SRC_PRESET;
            return kFeatures[f].lv[gPreset];
        }
    }
    *src = SRC_DFLT;
    return Clamp3(dflt);
}

bool Load()
{
    FILE *fp = fopen(gFile.c_str(), "r");
    if (!fp)
        return false;
    char line[512];
    int preset = PRESET_NONE;
    while (fgets(line, sizeof(line), fp))
    {
        std::string s = line;
        size_t c = s.find(';');
        if (c != std::string::npos)
            s = s.substr(0, c);
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
        if (k == "preset")
            preset = ParsePreset(v.c_str());
        else if (k == "bench")
            gBenchLine = v;
        else if (k == "skies")
            gSkies = v == "original" ? SKIES_ORIGINAL
                   : (v == "new" ? SKIES_NEW : (v == "new3d" ? SKIES_NEW3D : SKIES_FOLLOW));
        else
        {
            int f = FeatureIndex(k.c_str());
            if (f >= 0 && !v.empty() && v != "preset")
                gOverride[f] = Clamp3(atoi(v.c_str()));
        }
    }
    fclose(fp);
    if (preset == PRESET_NONE)
        return false;
    gPreset = preset;
    return true;
}

bool Automated()
{
    // Harnesses never get a benchmark (or a fresh SAVE/gfx.txt) behind their back.
    const char *keys[] = {"UA_AUTOSTART_LEVEL", "UA_VIEWER", "UA_WINDOW_HIDDEN", "UA_FX_STAGE",
                          "UA_PERF_CAMPATH", "UA_SHOT_EVERY", "UA_MP_HOST", "UA_MP_JOIN", "UA_FIXED_PERIOD"};
    for (const char *k : keys)
        if (Env(k))
            return true;
    return false;
}

} // namespace

void Init()
{
    if (gInited)
        return;
    gInited = true;
    for (int f = 0; f < kNumFeatures; ++f)
        gOverride[f] = -1;
    gFile = FilePath();

    int envP = ParsePreset(Env("UA_GFX_PRESET"));
    bool loaded = Load();
    if (envP != PRESET_NONE)
    {
        gPreset = envP;
        gEnvPreset = true;
    }
    else if (!loaded)
    {
        gPreset = ParsePreset(Env("UA_GFX_DEFAULT"));
        for (int f = 0; f < kNumFeatures; ++f)
            gOverride[f] = -1;
        // First launch of a settings-aware launcher: measure and suggest.
        const char *b = Env("UA_GFX_BENCH");
        bool want = gPreset != PRESET_NONE && !Automated();
        if (b)
            want = b[0] != '0';
        if (want)
        {
            gBenchLevel = Env("UA_GFX_BENCH_LEVEL") ? atoi(Env("UA_GFX_BENCH_LEVEL")) : 75;
            gBenchState = gBenchLevel > 0 ? 1 : 0;
        }
    }

    if (gPreset != PRESET_NONE)
    {
        printf("gfx: preset %s%s (file %s)", kPresetKeys[gPreset], gEnvPreset ? " [UA_GFX_PRESET]" : "",
               gFile.c_str());
        for (int f = 0; f < kNumFeatures; ++f)
            printf(" %s=%d", kFeatures[f].name, EffectiveLevel(f));
        if (gSkies != SKIES_FOLLOW)
            printf(" skies=%s", SkiesKey(gSkies));
        printf("\n");
    }
}

bool Active()
{
    Init();
    return gPreset != PRESET_NONE;
}

uint32_t Generation()
{
    Init();
    return gGen;
}

int FeatureCount() { return kNumFeatures; }
const char *FeatureName(int f) { return (f >= 0 && f < kNumFeatures) ? kFeatures[f].name : ""; }
const char *FeatureLabel(int f) { return (f >= 0 && f < kNumFeatures) ? kFeatures[f].label : ""; }

const char *LevelLabel(int f, int lv)
{
    if (f < 0 || f >= kNumFeatures)
        return "";
    return kFeatures[f].lvNames[Clamp3(lv)];
}

int FeatureIndex(const char *name)
{
    for (int f = 0; f < kNumFeatures; ++f)
        if (!strcasecmp(name, kFeatures[f].name))
            return f;
    return -1;
}

int PresetLevel(int p, int f)
{
    if (p < 0 || p >= PRESET_COUNT || f < 0 || f >= kNumFeatures)
        return 0;
    return kFeatures[f].lv[p];
}

const char *PresetName(int p) { return (p >= 0 && p < PRESET_COUNT) ? kPresetNames[p] : "CLASSIC"; }
const char *PresetKey(int p) { return (p >= 0 && p < PRESET_COUNT) ? kPresetKeys[p] : "none"; }

int GetPreset()
{
    Init();
    return gPreset;
}

void SetPreset(int p)
{
    Init();
    if (p < 0 || p >= PRESET_COUNT)
        return;
    gPreset = p;
    gEnvPreset = false;
    for (int f = 0; f < kNumFeatures; ++f)
        gOverride[f] = -1;
    gGen++;
}

int GetOverride(int f)
{
    Init();
    return (f >= 0 && f < kNumFeatures) ? gOverride[f] : -1;
}

void SetOverride(int f, int lv)
{
    Init();
    if (f < 0 || f >= kNumFeatures)
        return;
    if (gPreset == PRESET_NONE)
        gPreset = PRESET_ORIGINAL;
    gEnvPreset = false;
    gOverride[f] = lv < 0 ? -1 : Clamp3(lv);
    // An override equal to the preset value is no override.
    if (gOverride[f] == kFeatures[f].lv[gPreset])
        gOverride[f] = -1;
    gGen++;
}

int SkiesFor(int preset, int skyOverride, int skies)
{
    const int f = FeatureIndex("SKY");
    if (f < 0 || preset < 0 || preset >= PRESET_COUNT)
        return 0;
    const int lv = skyOverride >= 0 ? skyOverride : kFeatures[f].lv[preset];
    if (skies == SKIES_ORIGINAL)
        return 0;
    if (skies == SKIES_NEW || skies == SKIES_NEW3D)
        return lv > 0 ? lv : kSkiesNewMin;
    return lv;
}

int Sky3dFor(int preset, int sky3dOverride, int skies)
{
    const int f = FeatureIndex("SKY3D");
    if (f < 0 || preset < 0 || preset >= PRESET_COUNT)
        return 0;
    if (skies == SKIES_ORIGINAL || skies == SKIES_NEW)
        return 0;
    if (skies == SKIES_NEW3D)
        return 1;
    return sky3dOverride >= 0 ? sky3dOverride : kFeatures[f].lv[preset];
}

int GetSkies()
{
    Init();
    return gSkies;
}

void SetSkies(int s)
{
    Init();
    s = (s == SKIES_ORIGINAL || s == SKIES_NEW || s == SKIES_NEW3D) ? s : SKIES_FOLLOW;
    if (s != SKIES_FOLLOW && gPreset == PRESET_NONE)
        gPreset = PRESET_ORIGINAL;
    if (s == gSkies)
        return;
    gSkies = s;
    gEnvPreset = false;
    gGen++;
}

int EffectiveLevel(int f)
{
    if (f < 0 || f >= kNumFeatures)
        return 0;
    return gfxLevel(kFeatures[f].name, 0);
}

bool EnvLocked(int f)
{
    if (f < 0 || f >= kNumFeatures)
        return false;
    Src s;
    Resolve(kFeatures[f].name, f, 0, &s);
    return s == SRC_GFXENV || s == SRC_LEGACY;
}

bool Save()
{
    Init();
    if (gPreset == PRESET_NONE)
        return false;
    FILE *fp = fopen(gFile.c_str(), "w");
    if (!fp)
    {
        printf("gfx: cannot write %s\n", gFile.c_str());
        return false;
    }
    fprintf(fp, "; Urban Assault Reforged graphics settings (Options > Graphics). Levels 0 off .. 3 high.\n");
    fprintf(fp, "; preset = original | low | medium | high | ultra | vr ; a feature line overrides the preset.\n");
    fprintf(fp, "preset = %s\n", kPresetKeys[gPreset]);
    if (gSkies != SKIES_FOLLOW) // Skies: Original / New (own choice, kept across presets)
        fprintf(fp, "skies = %s\n", SkiesKey(gSkies));
    for (int f = 0; f < kNumFeatures; ++f)
        if (gOverride[f] >= 0)
            fprintf(fp, "%s = %d\n", kFeatures[f].name, gOverride[f]);
    if (!gBenchLine.empty())
        fprintf(fp, "bench = %s\n", gBenchLine.c_str());
    fclose(fp);
    printf("gfx: saved %s (preset %s)\n", gFile.c_str(), kPresetKeys[gPreset]);
    return true;
}

const char *Param(const char *envName)
{
    if (const char *v = Env(envName))
        return v;
    Init();
    int f = FeatureIndex("V2");
    Src s;
    int lv = Resolve("V2", f, 0, &s);
    if (lv <= 0 || s == SRC_LEGACY || s == SRC_DFLT)
        return NULL;
    if (!strcmp(envName, "UA_R2_SUN"))
        return kNlSun;
    if (!strcmp(envName, "UA_R2_EXPOSURE"))
        return kNlExposure;
    if (lv == 1)
    {
        // BASIC: sun + exposure only; far-v2 and normal maps off (cheapest V2).
        if (!strcmp(envName, "UA_R2_FARV2"))
            return "0";
        if (!strcmp(envName, "UA_R2_NORMALMAP"))
            return "0";
        return NULL;
    }
    if (!strcmp(envName, "UA_R2_FARV2"))
        return "1";
    if (!strcmp(envName, "UA_R2_FARSPEC"))
        return kNlFarSpec;
    if (!strcmp(envName, "UA_R2_FAREDGE"))
        return kNlFarEdge;
    return NULL;
}

int ViewSectors(int iniSectors)
{
    if (const char *v = Env("UA_VIEW_SECTORS"))
        if (atoi(v) > 0)
            return atoi(v);
    int lv = gfxLevel("DRAWDIST", 0);
    return kDrawSectors[lv] > 0 ? kDrawSectors[lv] : iniSectors;
}

void OnLevelLoad(NC_STACK_ypaworld *yw)
{
    Init();
    (void)yw;
    GFX::Engine.SetRenderV2(gfxLevel("V2", 0) > 0);
    GFX::Engine.SetGround(gfxLevel("FLOOR", 0) > 0);
    GFX::Engine.ShadowsApply(); // SHADOWS / GLOW: same level = no work; a change rebuilds or frees here
    GFX::GlowPass::OnLevelLoad();
    if (gBenchState == 2)
    {
        gBenchMs.clear();
        gBenchFrames = 0;
        gBenchT0 = 0;
    }
}

bool TakeSetReload()
{
    uint32_t s = 0;
    for (int f = 0; f < kNumFeatures; ++f)
        if (kFeatures[f].setReload)
            s = s * 4 + (uint32_t)gfxLevel(kFeatures[f].name, 3);
    // GLOW off <-> on: its exclusion tags are written at texture prepare (none at level 0), so the set reloads
    s = s * 2 + (gfxLevel("GLOW", 0) > 0 ? 1u : 0u);
    bool r = gSetSig != 0xFFFFFFFF && s != gSetSig;
    gSetSig = s;
    if (r)
        printf("gfx: model/renderer levels changed, reloading the set\n");
    return r;
}

// ---------------------------------------------------------------------------
// First-launch benchmark.
// Runs once, when a settings-aware launcher (UA_GFX_DEFAULT=<preset>) starts
// with no SAVE/gfx.txt and no harness env: autostarts level 75 (L7575, the busy
// Mykonian reference scene) at the launcher's default preset, flies the fixed
// "tour" camera path with sound muted and the frame cap off, measures frames
// 3..BenchSeconds() s after the first 3D frame, ends the level, then picks the
// highest preset PREDICTED to hold 60 fps (p90 frame <= 16.7 ms and mean <= 15 ms;
// nothing fits -> Original). Predictions scale the measured preset by the cost-table
// ratios below (measured); a preset above
// the measured one is only suggested when the measurement itself has room.
// The suggestion is saved as the player's preset ("bench = ..." records the
// numbers); the Graphics menu changes it any time. UA_GFX_BENCH=0 disables it,
// =1 forces it (tests), UA_GFX_BENCH_LEVEL picks the level.
// ---------------------------------------------------------------------------
namespace
{
// Mean frame-time ratio vs High per preset (Original, Low, Medium, High, Ultra, VR), at two drawable sizes;
// the benchmark interpolates by the player's drawable pixel count (clamped to the two ends). Fill-bound
// features (the floor in High/Ultra) cost far more at 4K than at 1080p, so one table cannot fit both.
// Measured on Apple M-series (idle machine), L7575 tour, 3 reps:
//   1920x1080: Original 2.17 / Low 1.85 / Medium 3.14 / High 5.13 / Ultra 5.98 ms;
//   3840x2160: 4.15 / 4.44 / 10.18 / 16.06 / 17.34 ms. (Medium = UI 2 + shadows/glow 1, no floor; High =
//   floor + 3x UI pack.) VR (= Low with DRAWDIST 1, UI 0, no HD models in the real VR column, so this errs high)
//   keeps the earlier VR/Low ratio (0.845 at 1080p, 0.969 at 4K).
const double kPresetCost1080[PRESET_COUNT] = {0.423, 0.361, 0.612, 1.0, 1.166, 0.305};
const double kPresetCost2160[PRESET_COUNT] = {0.258, 0.276, 0.634, 1.0, 1.080, 0.268};

double PresetCost(int p)
{
    Common::Point r = System::GetResolution();
    double px = (double)r.x * (double)r.y, lo = 1920.0 * 1080.0, hi = 3840.0 * 2160.0;
    double t = std::max(0.0, std::min(1.0, (px - lo) / (hi - lo)));
    return kPresetCost1080[p] + (kPresetCost2160[p] - kPresetCost1080[p]) * t;
}
}

int BenchLevelId()
{
    Init();
    return gBenchState == 1 ? gBenchLevel : 0;
}

int BenchSeconds()
{
    const char *v = Env("UA_GFX_BENCH_SECONDS");
    int s = v ? atoi(v) : 16;
    return std::max(6, std::min(20, s));
}

bool BenchRunning()
{
    return gBenchState == 1 || gBenchState == 2;
}

void BenchFrame(double ms)
{
    if (gBenchState == 1)
        gBenchState = 2;
    if (gBenchState != 2)
        return;
    uint32_t now = SDL_GetTicks();
    if (!gBenchT0)
        gBenchT0 = now;
    gBenchFrames++;
    if (now - gBenchT0 >= 3000 && gBenchFrames > 2)
        gBenchMs.push_back((float)ms);
}

void BenchLevelEnded()
{
    if (gBenchState != 2)
        return;
    gBenchState = 3;
    int measured = gPreset == PRESET_NONE ? PRESET_HIGH : gPreset;
    int pick = PRESET_ORIGINAL;
    double mean = 0, p50 = 0, p90 = 0;
    if (gBenchMs.size() >= 30)
    {
        std::vector<float> v = gBenchMs;
        std::sort(v.begin(), v.end());
        for (float x : v)
            mean += x;
        mean /= v.size();
        p50 = v[v.size() / 2];
        p90 = v[(size_t)(v.size() * 0.90)];
        // "Holds 60 fps": 90 % of frames within 16.7 ms AND the mean within 15 ms (10 % headroom), both
        // scaled from the measured preset by the cost ratios. p90, not p95/p99: the original engine's own
        // load/sim hitches (100-370 ms, ~40/min, measured) sit in the top few % on every preset.
        const int order[] = {PRESET_ULTRA, PRESET_HIGH, PRESET_MEDIUM, PRESET_LOW};
        for (int p : order)
        {
            double k = PresetCost(p) / PresetCost(measured);
            if (p90 * k <= 16.7 && mean * k <= 15.0)
            {
                pick = p;
                break;
            }
        }
    }
    char buf[200];
    Common::Point res = System::GetResolution();
    snprintf(buf, sizeof(buf), "level %d mean %.1f ms p50 %.1f p90 %.1f over %d frames at %s %dx%d -> %s", gBenchLevel,
             mean, p50, p90, (int)gBenchMs.size(), kPresetKeys[measured], res.x, res.y, kPresetKeys[pick]);
    gBenchLine = buf;
    printf("gfx: bench %s\n", gBenchLine.c_str());
    SetPreset(pick);
    Save();
}

} // namespace GfxSettings

int gfxLevel(const char *name, int dflt)
{
    GfxSettings::Init();
    int f = GfxSettings::FeatureIndex(name);
    GfxSettings::Src s;
    return GfxSettings::Resolve(name, f, dflt, &s);
}
