// Urban Assault VR — engine glue. See vr.h for the frame contract and
// for the design. Inert unless UA_VR=sim (runtime) and
// compiled with -DUA_VR=ON.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <algorithm>
#include <vector>

#include "vr.h"
#include "vr_gl.h"
#include "vr_math.h"
#include "vr_aim.h"
#include "vr_comfort.h"
#include "vr_command.h"
#include "vr_field.h"
#include "vr_sites.h"
#include "vr_cockpit.h"
#include "vr_bridge.h"
#include "vr_station.h"
#include "vr_radio.h"
#include "vr_table.h"
#include "vr_panels.h"
#include "../system/gfx.h"
#include "../gfxsettings.h"
#include "../system/tform.h"
#include "../system/system.h"
#include "../yw.h"
#include "../ypabact.h"

namespace VR
{

Backend *CreateSimBackend(); // vr_sim.cpp
#ifdef __ANDROID__
Backend *CreateOpenXRBackend(); // vr_openxr.cpp
#endif

namespace
{

Backend *g_backend = NULL;
bool g_active = false;
double g_upm = 11.0; // visual scale, "World scale"
float g_panelWidthM = 1.8f;    // cockpit HUD panel width
float g_panelDistM = 1.5f;     // ... distance ahead of the seated eye point
float g_panelDropM = 0.20f;    // ... below eye line
// HUD panel while the Command Deck war table is open at the host station: narrower
// and raised so its bottom (toolbar) clears the table's far edge (~18 deg below the eye).
float g_deckPanelWidthM = 1.5f, g_deckPanelDropM = -0.05f;
float g_theatreWidthM = 6.0f;  // theatre screen width (menus, briefing, FMV)
float g_theatreDistM = 5.5f;

FrameInfo g_frame;
int64_t g_lastDisplayNs = 0;
double g_tickAcc = 0.0;
bool g_displayTick = false;
ControllerState g_ctrl[2];
bool g_viewerFlyer = false;
bool g_viewerDrives = false;       // tank / car (grips drive)
double g_viewerFwd = 0;            // speed along the vehicle's nose (engine units, + = forward)
double g_viewerVel[3] = {0, 0, 0}; // m/s, world axes (comfort vignette)
uint64_t g_viewerKey = 0;          // level id << 32 | viewer gid (comfort filter snap)
const char *g_comboLabel = NULL;   // comfort combo fired, shown by GameplayFrameHook
ViewState g_views;
bool g_worldRendered = false;
bool g_inWorldView = false;
// Living Diorama: this frame's world pass is the miniature (DioramaView).
bool g_dioFrame = false;
bool g_ptFrame = false; // this frame's eyes were drawn for passthrough
double g_dioK = 1.0;

// Saved vehicle camera (restored for HUD code after the 3D pass).
vec3d g_anchorPos;
mat3x3 g_anchorRot;
// Centre-eye -> eye transforms (UAV, including the inverse cull scale).
M4 g_eyeFromCentre[2];
// Scaled centre-eye coords from comfort-anchor-local coords (static reference).
M4 g_centreFromAnchor;
double g_cullSX = 1.0, g_cullSY = 1.0;

// ---- timing -------------------------------------------------------------
double Now() { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

struct FrameTimes
{
    double start = 0, collect = -1, eyesStart = -1, eyeCpu[2] = {0, 0}, hud = -1, present = -1, presentEnd = -1;
    double endFrame = -1; // perf: first GFX EndFrame / draw2DandFlush of the frame
};
FrameTimes g_ft;
double g_eyeT0 = 0;

struct Acc
{
    int frames = 0, world = 0, over = 0;
    double total = 0, totalMax = 0, update = 0, collect = 0, eye[2] = {0, 0}, gpu[2] = {0, 0}, hud = 0, present = 0;
    int gpuN = 0;
    double since = 0;
    std::vector<float> totals; // per-frame ms, for the p95 in the log line
    double acq = 0, ovl = 0, rel = 0; // both eyes: swapchain acquire+wait, overlays, release
    double tick = 0;                  // perf: frame start -> first EndFrame (all frames)
    int tickN = 0;
    double part[HUD_PARTS] = {};      // perf: HudAdd sums (all frames)
};
Acc g_acc;
double g_budgetMs = 1000.0 / 90.0;
double g_budgetCfgMs = 1000.0 / 90.0; // UA_VR_TARGET_HZ / UA_VR_HZ; the display period wins when known

// UA_VR_HZ=auto (default on the headset) - start at the highest offered rate.
// rewrite (auto dropped 120 -> 90 on a 0.01 ms p95 miss at level start and
// never came back: at 90 Hz the Quest lowers the CPU clocks, so the old up-test
// "p95 at 90 fits 97 % of the 120 Hz period" could never pass).
//  * Down: one rate after kAutoDownSecs consecutive world seconds that really miss:
//    delivered fps under 95 % of the rate (frames dropped - CPU, GPU or compositor),
//    or a p95 CPU frame time over 105 % of the period (hysteresis: the old 97 %
//    tripped on noise while frames were still delivered).
//  * Up = a timed trial. After g_autoUpSecs consecutive clean seconds whose p95 is
//    within kAutoUpHeadroom x the higher rate's period (the clocks rise with the rate,
//    so the slower-clock p95 is allowed to overshoot), the higher rate is requested.
//    The trial is judged by the same down rule: a step down within kAutoProbeSecs of
//    the trial doubles g_autoUpSecs (kAutoUpMinSecs .. 8 min) so a rate that cannot
//    hold is not retried every 20 s; a trial that holds resets it.
//  * Loading seconds (fewer than 10 world frames, or a frame over 100 ms) reset the
//    counters; the first kAutoWarmupSecs world seconds after a load and the first
//    kAutoSettleSecs after a rate change (clock ramp) are not judged.
bool g_autoHz = false;
int g_autoBad = 0, g_autoGood = 0, g_autoWorldSecs = 0, g_autoUpSecs = 20, g_autoSinceUp = -1, g_autoWarm = 8;
const int kAutoDownSecs = 3, kAutoWarmupSecs = 8, kAutoSettleSecs = 2, kAutoProbeSecs = 20;
const int kAutoUpMinSecs = 20, kAutoUpMaxSecs = 480;
const double kAutoCpuMiss = 1.05, kAutoFpsMiss = 0.95, kAutoUpHeadroom = 1.15;

void AutoHz(double p95, int worldFrames, double maxMs, double fps)
{
    if (!g_autoHz || !g_backend)
        return;
    if (worldFrames < 10 || maxMs > 100.0)
    {
        g_autoBad = g_autoGood = 0;
        g_autoWorldSecs = 0;
        g_autoWarm = kAutoWarmupSecs;
        return;
    }
    if (++g_autoWorldSecs <= g_autoWarm)
        return;
    float rates[16];
    const int n = g_backend->RefreshRates(rates, 16);
    const float cur = g_backend->RefreshHz();
    if (n <= 0 || cur <= 0.0f)
        return;
    int i = 0;
    while (i < n - 1 && rates[i] < cur - 0.5f)
        i++;
    if (g_autoSinceUp >= 0 && ++g_autoSinceUp > kAutoProbeSecs)
    {
        fprintf(stderr, "UAVR: auto Hz: %.0f Hz held for %d s after the up-step (next up-probe after %d s)\n", cur,
                kAutoProbeSecs, kAutoUpMinSecs);
        g_autoSinceUp = -1;
        g_autoUpSecs = kAutoUpMinSecs;
    }
    const double period = 1000.0 / cur;
    if (fps < kAutoFpsMiss * cur || p95 > kAutoCpuMiss * period)
    {
        g_autoGood = 0;
        if (++g_autoBad >= kAutoDownSecs && i > 0)
        {
            if (g_autoSinceUp >= 0)
                g_autoUpSecs = std::min(kAutoUpMaxSecs, g_autoUpSecs * 2);
            g_autoSinceUp = -1;
            fprintf(stderr, "UAVR: auto Hz: p95 %.2f ms / %.1f fps misses %.0f Hz -> %.0f Hz (next up-probe after %d s)\n",
                    p95, fps, cur, rates[i - 1], g_autoUpSecs);
            g_backend->RequestRefreshRate(rates[i - 1]);
            g_autoBad = 0;
            g_autoWorldSecs = 0;
            g_autoWarm = kAutoSettleSecs;
        }
        return;
    }
    g_autoBad = 0;
    if (i < n - 1 && p95 < kAutoUpHeadroom * 1000.0 / rates[i + 1])
    {
        if (++g_autoGood >= g_autoUpSecs)
        {
            fprintf(stderr, "UAVR: auto Hz: p95 %.2f ms / %.1f fps clean for %d s, trial %.0f -> %.0f Hz\n", p95, fps,
                    g_autoGood, cur, rates[i + 1]);
            g_backend->RequestRefreshRate(rates[i + 1]);
            g_autoGood = 0;
            g_autoWorldSecs = 0;
            g_autoWarm = kAutoSettleSecs;
            g_autoSinceUp = 0;
        }
    }
    else
        g_autoGood = 0;
}

// display rate player setting (Settings "DISPLAY", profile ";#!vr_hz").
// 0 = auto, else a pinned rate. g_hzApplied = the setting the backend last
// accepted (-1 = none yet; the boot request is auto). Env pins win.
#ifdef UA_VR_NEWLOOK
int g_hzSetting = 90; // the release default: 90 Hz, the New Look's frame budget (a saved setting wins)
#else
int g_hzSetting = 0;
#endif
// dev aid: UA_VR_HZ=auto runs auto even when the profile pins a rate (the setting is
// kept and saved, just not applied) - for benchmarking with an existing profile.
int HzEff()
{
    static int envAuto = -1;
    if (envAuto < 0)
    {
        const char *v = getenv("UA_VR_HZ");
        envAuto = (v && !strcmp(v, "auto")) ? 1 : 0;
    }
    return envAuto ? 0 : g_hzSetting;
}
float g_lastFps = 0.0f;  // menu: last per-second fps (FPS counter)
int g_hzApplied = 0;
float g_hzWant = 0.0f;  // rate last requested for a pinned setting
int g_hzMismatch = 0;   // seconds the runtime rate has differed from it
bool g_hzEnvPinned = false;
bool g_isXr = false;

void ApplyDisplayRate()
{
    if (!g_backend || g_hzEnvPinned || HzEff() == g_hzApplied)
        return;
    float rates[16];
    const int n = g_backend->RefreshRates(rates, 16);
    if (n <= 0)
    {
        g_hzApplied = HzEff(); // backend has no rate control: nothing to do
        return;
    }
    float want = 0.0f;
    if (HzEff() == 0)
    {
        want = g_isXr ? rates[n - 1] : 0.0f; // auto: start at the top (sim: back to its default pacing)
        // bench aid: UA_VR_AUTO_START_HZ=90 starts auto lower, to watch the up-trial.
        const char *st = getenv("UA_VR_AUTO_START_HZ");
        if (g_isXr && st && atof(st) > 0)
            for (int i = 0; i < n; i++)
                if (rates[i] <= atof(st) + 0.5f)
                    want = rates[i];
    }
    else
    {
        for (int i = 0; i < n; i++)
            if (rates[i] <= HzEff() + 0.5f)
                want = rates[i]; // exact, else the highest offered below
        if (want <= 0.0f)
            want = rates[0];
    }
    if (!g_backend->RequestRefreshRate(want))
    {
        fprintf(stderr, "UAVR: display setting %s: request %.0f Hz failed (retry in 1 s)\n", DisplayRateLabel(HzEff()), want);
        return;
    }
    g_autoHz = g_isXr && HzEff() == 0;
    g_autoBad = g_autoGood = g_autoWorldSecs = 0;
    g_autoUpSecs = kAutoUpMinSecs;
    g_autoSinceUp = -1;
    g_autoWarm = kAutoWarmupSecs;
    g_budgetCfgMs = 1000.0 / (want > 0.0f ? want : 90.0f);
    g_hzApplied = HzEff();
    g_hzWant = HzEff() ? want : 0.0f;
    g_hzMismatch = 0;
    fprintf(stderr, "UAVR: display setting %s -> requested %.0f Hz%s\n", DisplayRateLabel(HzEff()), want,
            g_autoHz ? " (auto)" : "");
}

// GPU timer queries: ring of 4 frames x 2 eyes.
const int kQRing = 4;
GLuint g_q[kQRing][2] = {{0}};
bool g_qIssued[kQRing] = {false};
int g_qFrame = 0;

void ReadGpuTimers()
{
    if (!gl.timerQueries)
        return;
    for (int f = 0; f < kQRing; f++)
    {
        if (!g_qIssued[f] || f == g_qFrame)
            continue;
        GLint avail = 0;
        gl.GetQueryObjectiv(g_q[f][1], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail)
            continue;
        GLuint64 a = 0, b = 0;
        gl.GetQueryObjectui64v(g_q[f][0], GL_QUERY_RESULT, &a);
        gl.GetQueryObjectui64v(g_q[f][1], GL_QUERY_RESULT, &b);
        g_acc.gpu[0] += a / 1.0e6;
        g_acc.gpu[1] += b / 1.0e6;
        g_acc.gpuN++;
        g_qIssued[f] = false;
    }
}

void CloseFrameTiming(double now)
{
    if (g_ft.start <= 0)
        return;
    const double total = now - g_ft.start;
    g_acc.frames++;
    g_acc.total += total;
    g_acc.totalMax = std::max(g_acc.totalMax, total);
    g_acc.totals.push_back((float)total);
    g_budgetMs = g_frame.predictedDisplayPeriodNs > 0 ? g_frame.predictedDisplayPeriodNs / 1.0e6 : g_budgetCfgMs;
    if (total > g_budgetMs)
        g_acc.over++;
    if (g_ft.collect >= 0 && g_ft.eyesStart >= 0)
    {
        g_acc.world++;
        g_acc.update += g_ft.collect - g_ft.start;
        g_acc.collect += g_ft.eyesStart - g_ft.collect;
        g_acc.eye[0] += g_ft.eyeCpu[0];
        g_acc.eye[1] += g_ft.eyeCpu[1];
        if (g_ft.hud >= 0 && g_ft.present >= 0)
            g_acc.hud += g_ft.present - g_ft.hud;
    }
    if (g_ft.present >= 0 && g_ft.presentEnd >= 0)
        g_acc.present += g_ft.presentEnd - g_ft.present;
    if (g_ft.endFrame >= 0)
    {
        g_acc.tick += g_ft.endFrame - g_ft.start;
        g_acc.tickN++;
    }

    ReadGpuTimers();

    if (g_acc.since <= 0)
        g_acc.since = now;
    if (now - g_acc.since >= 1000.0 && g_acc.frames > 0)
    {
        const double n = g_acc.frames, w = std::max(1, g_acc.world), g = std::max(1, g_acc.gpuN);
        int ew = 0, eh = 0;
        g_backend->EyeSize(&ew, &eh);
        const double rows2d = GFX::Engine._screenUploads ? (double)GFX::Engine._screenRowsUploaded / GFX::Engine._screenUploads : 0.0;
        GFX::Engine._screenRowsUploaded = GFX::Engine._screenUploads = 0;
        const double fr = std::max(1, g_acc.frames);
        double p95 = 0.0;
        if (!g_acc.totals.empty())
        {
            const size_t k = std::min(g_acc.totals.size() - 1, (size_t)(g_acc.totals.size() * 0.95));
            std::nth_element(g_acc.totals.begin(), g_acc.totals.begin() + k, g_acc.totals.end());
            p95 = g_acc.totals[k];
        }
        const double draws = GFX::Engine._statDraws / fr, ubo = GFX::Engine._statUbo / fr,
                     progs = GFX::Engine._statProg / fr, texs = GFX::Engine._statTex / fr;
        GFX::Engine._statDraws = GFX::Engine._statUbo = GFX::Engine._statProg = GFX::Engine._statTex = 0;
        const double uboMiss = GFX::Engine._statUboOverflow / fr;
        GFX::Engine._statUboOverflow = 0;
        const double clr = GFX::Engine._statClearMs / fr, scan = GFX::Engine._statScanMs / fr, upl = GFX::Engine._statUploadMs / fr;
        GFX::Engine._statClearMs = GFX::Engine._statScanMs = GFX::Engine._statUploadMs = 0;
        fprintf(stderr, "UAVR fps %.1f | frame avg %.2f p95 %.2f max %.2f ms | over %.1fms: %d%% | world frames %d: "
                        "update %.2f collect %.2f eyeL cpu %.2f gpu %.2f eyeR cpu %.2f gpu %.2f hud %.2f | present %.2f | eye %dx%d"
                        " | 2d rows/frame %.0f | per frame: draws %.0f ubo %.0f (ring miss %.0f) prog %.0f tex %.0f | eyes acq %.2f ovl %.2f rel %.2f | 2d clear %.2f scan %.2f upload %.2f fills %.0f\n",
                n * 1000.0 / (now - g_acc.since), g_acc.total / n, p95, g_acc.totalMax, g_budgetMs, (int)(100.0 * g_acc.over / n),
                g_acc.world, g_acc.update / w, g_acc.collect / w, g_acc.eye[0] / w, g_acc.gpuN ? g_acc.gpu[0] / g : -1.0,
                g_acc.eye[1] / w, g_acc.gpuN ? g_acc.gpu[1] / g : -1.0, g_acc.hud / w, g_acc.present / n, ew, eh, rows2d,
                draws, ubo, uboMiss, progs, texs, g_acc.acq / w, g_acc.ovl / w, g_acc.rel / w, clr, scan, upl, GFX::Engine._statFills / fr);
        GFX::Engine._statFills = 0;
        // perf: where the hud phase (and menu frames) go; ms per frame over all frames.
        // tick = frame start -> first EndFrame (game logic + 3D pass + HUD draw sequences).
        {
            const double *pp = g_acc.part;
            fprintf(stderr, "UAVR sub | tick %.2f (n %d) | radar %.2f win %.2f mark %.2f misc %.2f card %.2f | gui %.2f surf %.2f (flush %.2f x%.1f) text %.2f comp %.2f | panels %.2f\n",
                    g_acc.tickN ? g_acc.tick / g_acc.tickN : -1.0, g_acc.tickN, pp[HUD_RADAR] / n, pp[HUD_WINDOWS] / n,
                    pp[HUD_MARKERS] / n, pp[HUD_MISC] / n, pp[HUD_CARD] / n, pp[HUD_GUIROOT] / n, pp[HUD_SURFACE] / n,
                    pp[HUD_FLUSH] / n, GFX::Engine._statFlushes / n, pp[HUD_TEXT] / n, pp[HUD_COMPOSE] / n, pp[HUD_PANELS] / n);
        }
        GFX::Engine._statFlushes = 0;
        g_lastFps = (float)(n * 1000.0 / (now - g_acc.since)); // FPS counter row
        AutoHz(p95, g_acc.world, g_acc.totalMax, n * 1000.0 / (now - g_acc.since));
        // A pinned setting the runtime no longer runs at (e.g. an XR session restart
        // re-requested the boot rate): request it again after 3 s, then at most every 30 s.
        if (g_isXr && !g_hzEnvPinned && HzEff() && g_hzApplied == HzEff() && g_hzWant > 0.0f)
        {
            const float cur = g_backend->RefreshHz();
            if (cur > 0.0f && fabsf(cur - g_hzWant) > 0.5f)
            {
                if (++g_hzMismatch >= 3)
                {
                    fprintf(stderr, "UAVR: display rate %.0f Hz but setting %s: requesting again\n", cur, DisplayRateLabel(HzEff()));
                    g_hzApplied = -1;
                    ApplyDisplayRate();
                    g_hzMismatch = -27;
                }
            }
            else
                g_hzMismatch = 0; // back at the pinned rate: next mismatch retries after 3 s
        }
        ApplyDisplayRate(); // settings change not yet accepted by the runtime
        g_acc = Acc();
        g_acc.since = now;
    }
}

bool EnvOn(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

double EnvF(const char *name, double def)
{
    const char *v = getenv(name);
    return (v && v[0]) ? atof(v) : def;
}

// Engine view (world -> view rows, aspect-corrected) -> plain orthonormal rotation.
mat3x3 StripAspect(const mat3x3 &r)
{
    mat3x3 o = r;
    GFX::Engine.matrixAspectCorrection(o, true);
    return o;
}

M3 ToM3(const mat3x3 &r)
{
    M3 o;
    o.m[0][0] = r.m00; o.m[0][1] = r.m01; o.m[0][2] = r.m02;
    o.m[1][0] = r.m10; o.m[1][1] = r.m11; o.m[1][2] = r.m12;
    o.m[2][0] = r.m20; o.m[2][1] = r.m21; o.m[2][2] = r.m22;
    return o;
}

mat3x3 FromM3(const M3 &r)
{
    return mat3x3(r.m[0][0], r.m[0][1], r.m[0][2],
                  r.m[1][0], r.m[1][1], r.m[1][2],
                  r.m[2][0], r.m[2][1], r.m[2][2]);
}

void BindPanel()
{
    RenderTarget p = g_backend->PanelTarget();
    gl.BindFramebuffer(GL_FRAMEBUFFER, p.fbo);
    glViewport(0, 0, p.width, p.height);
}

} // namespace

// ---------------------------------------------------------------------------

bool Active() { return g_active; }
double UnitsPerMetre() { return g_upm; }
bool SuppressViewShake(double shakeTrace)
{
    if (!g_active)
        return false;
    const double deg = acos(std::max(-1.0, std::min(1.0, (shakeTrace - 1.0) * 0.5))) * 180.0 / M_PI;
    const bool apply = Cockpit::AllowShake();
    Cockpit::NoteShake(deg, apply); // -> haptics (cockpit)
    return !apply;
}

void Init()
{
    const char *mode = getenv("UA_VR");
    if (!mode || !mode[0] || !strcmp(mode, "0") || !strcmp(mode, "off"))
        return;

    const bool wantSim = !strcmp(mode, "sim");
#ifdef __ANDROID__
    const bool wantXr = !strcmp(mode, "openxr");
#else
    const bool wantXr = false;
#endif
    if (!wantSim && !wantXr)
    {
        fprintf(stderr, "UAVR: backend '%s' not available in this build; VR off\n", mode);
        return;
    }

    if (!gl.Load())
    {
        fprintf(stderr, "UAVR: GL entry points missing; VR off\n");
        return;
    }

#ifdef __ANDROID__
    g_backend = wantXr ? CreateOpenXRBackend() : CreateSimBackend();
#else
    g_backend = CreateSimBackend();
#endif
    if (!g_backend || !g_backend->Init())
    {
        fprintf(stderr, "UAVR: backend init failed; VR off\n");
        delete g_backend;
        g_backend = NULL;
        return;
    }

    g_upm = EnvF("UA_VR_UNITS_PER_M", 11.0);
    ComfortInit(g_upm); // horizon modes, smoothing, eye raise, vignette, reference
    g_panelWidthM = EnvF("UA_VR_PANEL_M", 1.8);
    g_panelDistM = EnvF("UA_VR_PANEL_DIST_M", 1.5);
    g_panelDropM = EnvF("UA_VR_PANEL_DROP_M", 0.20);
    g_deckPanelWidthM = EnvF("UA_VR_DECK_PANEL_M", 1.5);
    g_deckPanelDropM = EnvF("UA_VR_DECK_PANEL_DROP_M", -0.05);
    g_budgetCfgMs = g_budgetMs = 1000.0 / EnvF("UA_VR_TARGET_HZ", 90.0);
    {
        // UA_VR_HZ unset or "auto" -> auto (the shell asked for the highest rate).
        const char *hz = getenv("UA_VR_HZ");
        const char *req = getenv("UA_VR_HZ_REQUEST");
        g_hzEnvPinned = (req && req[0]) || (hz && hz[0] && strcmp(hz, "auto"));
        g_isXr = wantXr;
        g_autoHz = wantXr && !g_hzEnvPinned;
        if (hz && atof(hz) > 0)
            g_budgetCfgMs = g_budgetMs = 1000.0 / atof(hz);
        if (g_autoHz)
            fprintf(stderr, "UAVR: display rate auto (highest that holds; UA_VR_HZ=72|80|90|120 pins it)\n");
    }
    if (g_hzEnvPinned)
        fprintf(stderr, "UAVR: display rate pinned by env (UA_VR_HZ / UA_VR_HZ_REQUEST); the Settings DISPLAY row is saved but not applied\n");
    // bench aid: force one application so UA_VR_AUTO_START_HZ takes effect
    // even when the setting already matches the boot state (auto).
    if (g_isXr && getenv("UA_VR_AUTO_START_HZ"))
        g_hzApplied = -1;
    ApplyDisplayRate(); // a profile loaded before VR init
    Aim::Init();
    Panels::Init(); // PANELS FIXED / FREE
    Cmd::Init(g_upm); // command layer
    Field::Init(g_upm); // field orders, wrist squads, squad wheel
    Sites::Init(g_upm); // captured sites moment, wrist energy
    Cockpit::Init(g_backend, g_upm); // cockpit shells, dash, wrist, haptics
    Bridge::Init(g_upm); // host-station bridge (Command Deck)
    Station::Init(g_upm); // station: 2D windows, build beam-in, beam the station
    g_displayTick = wantXr ? !(getenv("UA_VR_DISPLAY_TICK") && !EnvOn("UA_VR_DISPLAY_TICK"))
                           : EnvOn("UA_VR_DISPLAY_TICK");

    if (gl.timerQueries)
        for (int f = 0; f < kQRing; f++)
            gl.GenQueries(2, g_q[f]);

    RenderTarget panel = g_backend->PanelTarget();
    // The engine's 2D canvas ("the screen") becomes the offscreen panel.
    System::SetVirtualResolution(Common::Point(panel.width, panel.height));
    GfxSettings::SetStereo(true); // hold the not-per-eye passes off (GLOW 0, ATMOS <= 2) before any level load
    GFX::Engine.SetVRMode(true, &DefaultFramebuffer, gl.BlendFuncSeparate);
    GFX::Engine.fpsLimitter(0); // VR paces in Backend::BeginFrame()
    SDL_GL_SetSwapInterval(EnvOn("UA_VR_VSYNC") ? 1 : 0);
    System::SetPresentHook(&Present);
    BindPanel();

    g_active = true;
    int ew, eh;
    g_backend->EyeSize(&ew, &eh);
    if (SDL_Surface *scr = GFX::Engine.Screen())
        fprintf(stderr, "UAVR: 2D software surface %dx%d (uploaded every frame by DrawScreenSurface)\n", scr->w, scr->h);
    fprintf(stderr, "UAVR: active, backend %s, eye %dx%d, panel %dx%d, %.2f units/m, timer queries %d\n",
            g_backend->Name(), ew, eh, panel.width, panel.height, g_upm, (int)gl.timerQueries);
}

void Shutdown()
{
    if (!g_backend)
        return;
    g_active = false;
    System::SetPresentHook(NULL);
    g_backend->Shutdown();
    delete g_backend;
    g_backend = NULL;
}

// perf dev aid: scripted controller input on the OpenXR backend (the headset
// lies unworn on a desk during automated perf runs, so real play is replayed).
// Inert unless one of these is set; the simulator backend parses them itself
// (vr_sim.cpp), so this only runs with g_isXr. Same syntax as the simulator:
//   UA_VR_SIM_STICK=lx,ly[,n]      left stick from world frame n (default 0)
//   UA_VR_SIM_PRESS=f0-f1:BTN,...  held over controller polls (= frames) f0..f1;
//                                  BTN = A B X Y MENU RT LT RG LG RSU RSD RSL RSR LSU LSD LSL LSR
// Scripted values overwrite the real ones; hands are forced active (fixed poses
// when the runtime reports none). Logged once at start-up.
static int g_presentedWorld = 0;
struct XrScript
{
    int state = -1; // -1 unparsed, 0 off, 1 on
    float sx = 0, sy = 0;
    int stickFrom = -1;
    struct Press { int f0, f1; std::string btn; };
    std::vector<Press> press;
    int poll = 0;
};
static XrScript g_xs;

void XrScriptInput(ControllerState c[2])
{
    if (g_xs.state == 0)
        return;
    if (g_xs.state < 0)
    {
        g_xs.state = 0;
        if (const char *v = getenv("UA_VR_SIM_STICK"))
        {
            int from = 0;
            if (sscanf(v, "%f,%f,%d", &g_xs.sx, &g_xs.sy, &from) >= 2)
                g_xs.stickFrom = from, g_xs.state = 1;
        }
        if (const char *v = getenv("UA_VR_SIM_PRESS"))
        {
            std::string str(v);
            size_t at = 0;
            while (at < str.size())
            {
                size_t e = str.find(',', at);
                std::string seg = str.substr(at, e == std::string::npos ? std::string::npos : e - at);
                XrScript::Press p;
                char btn[16] = {0};
                if (sscanf(seg.c_str(), "%d-%d:%15s", &p.f0, &p.f1, btn) == 3)
                {
                    p.btn = btn;
                    g_xs.press.push_back(p);
                    g_xs.state = 1;
                }
                if (e == std::string::npos)
                    break;
                at = e + 1;
            }
        }
        if (g_xs.state)
            fprintf(stderr, "UAVR: scripted input on (stick %.2f,%.2f from world frame %d, %d presses)\n", g_xs.sx, g_xs.sy,
                    g_xs.stickFrom, (int)g_xs.press.size());
        if (!g_xs.state)
            return;
    }
    g_xs.poll++;
    for (int h = 0; h < 2; h++)
    {
        if (c[h].active)
            continue;
        c[h] = ControllerState();
        c[h].active = true;
        c[h].aimPose.position.x = c[h].gripPose.position.x = h ? 0.2f : -0.2f;
        c[h].aimPose.position.y = c[h].gripPose.position.y = -0.35f;
        c[h].aimPose.position.z = c[h].gripPose.position.z = -0.35f;
    }
    if (g_xs.stickFrom >= 0 && g_presentedWorld >= g_xs.stickFrom)
    {
        c[0].stickX = g_xs.sx;
        c[0].stickY = g_xs.sy;
    }
    for (const XrScript::Press &p : g_xs.press)
    {
        if (g_xs.poll < p.f0 || g_xs.poll > p.f1)
            continue;
        const std::string &b = p.btn;
        if (b == "A") c[1].buttons |= BTN_A;
        else if (b == "B") c[1].buttons |= BTN_B;
        else if (b == "X") c[0].buttons |= BTN_X;
        else if (b == "Y") c[0].buttons |= BTN_Y;
        else if (b == "MENU") c[0].buttons |= BTN_MENU;
        else if (b == "RT") c[1].trigger = 1.0f;
        else if (b == "LT") c[0].trigger = 1.0f;
        else if (b == "RG") c[1].grip = 1.0f;
        else if (b == "LG") c[0].grip = 1.0f;
        else if (b == "RSU") c[1].stickY = 1.0f;
        else if (b == "RSD") c[1].stickY = -1.0f;
        else if (b == "RSL") c[1].stickX = -1.0f;
        else if (b == "RSR") c[1].stickX = 1.0f;
        else if (b == "LSU") c[0].stickY = 1.0f;
        else if (b == "LSD") c[0].stickY = -1.0f;
        else if (b == "LSL") c[0].stickX = -1.0f;
        else if (b == "LSR") c[0].stickX = 1.0f;
        else if (b == "RSC") c[1].buttons |= BTN_STICK_CLICK; // stick press (diorama placement)
        else if (b == "LSC") c[0].buttons |= BTN_STICK_CLICK;
    }
}

void FrameStart()
{
    if (!g_active)
        return;
    const double now = Now();
    CloseFrameTiming(now);
    g_backend->BeginFrame(&g_frame);
    g_backend->PollControllers(g_ctrl);
    if (g_isXr)
        XrScriptInput(g_ctrl); // perf dev aid, inert unless UA_VR_SIM_STICK / _PRESS
    if (const char *label = ComfortFilterCombo(g_ctrl)) // swallows the combo's clicks
        g_comboLabel = label;
    Cmd::Filter(g_ctrl);     // command layer: owned inputs, board / wheel picks, fades
    Aim::FrameStart(g_ctrl); // laser pointer vs last frame's panel
    g_ft = FrameTimes();
    g_ft.start = Now();
}

void BeginWorldView(TF::TForm3D *view)
{
    if (!g_active || !view)
        return;

    g_ft.collect = Now();

    // Late pose: sampled here, right before the scene is collected; both eyes
    // are derived from this single sample.
    if (!g_backend->LocateViews(g_frame.predictedDisplayTimeNs, &g_views) || !g_views.valid)
    {
        g_views = ViewState();
        g_views.valid = true;
    }
    // bench dev aid (headset lying unworn on the desk): UA_VR_BENCH_HEAD=x,y,z,yaw,pitch
    // pins the head pose (XR metres / degrees, yaw + = right, pitch + = up; eyes stay
    // head-relative), so a Quest bench looks at the diorama like a player at the table.
    {
        static int benchSet = -1;
        static double bh[5] = {0, 0, 0, 0, 0};
        if (benchSet < 0)
        {
            const char *v = getenv("UA_VR_BENCH_HEAD");
            benchSet = (v && sscanf(v, "%lf,%lf,%lf,%lf,%lf", &bh[0], &bh[1], &bh[2], &bh[3], &bh[4]) == 5) ? 1 : 0;
            if (benchSet)
                fprintf(stderr, "UAVR: bench head pinned at %.2f,%.2f,%.2f yaw %.0f pitch %.0f\n", bh[0], bh[1], bh[2], bh[3], bh[4]);
        }
        if (benchSet)
        {
            const double d2r = M_PI / 180.0;
            g_views.head.position.x = (float)bh[0];
            g_views.head.position.y = (float)bh[1];
            g_views.head.position.z = (float)bh[2];
            g_views.head.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, -bh[3] * d2r), QuatFromAxisAngle(1, 0, 0, bh[4] * d2r));
        }
    }

    g_anchorPos = view->Pos;
    g_anchorRot = view->SclRot;

    // Cockpit anchor = the vehicle camera without the flat-screen aspect hack
    // (and without shake: Process() skips the shake matrix in VR).
    // Comfort (vr_comfort.cpp): horizon mode per class, vertical smoothing, eye raise.
    M3 ra = ToM3(StripAspect(view->SclRot));
    double pa[3] = {g_anchorPos.x, g_anchorPos.y, g_anchorPos.z};
    ComfortAnchor(&ra, pa, g_viewerFlyer, g_viewerVel, g_viewerKey ^ Cmd::SnapKey()); // snap turn = filter snap
    Cmd::SetAnchor(ra, pa);
    Field::SetAnchor(ra, pa);
    Station::SetAnchor(ra, pa);
    Sites::SetAnchor(ra, pa);

    // Head in UAV axes. Columns of rh = head axes in anchor-camera coordinates.
    const M3 rh = XrRotToUa(QuatToM3(g_views.head.orientation));
    double ph[3];
    XrPosToUa(g_views.head.position, g_upm, ph);

    // Centre-eye camera: world->view = rh^T * ra ; position = P + ra^T * ph.
    M3 rc = rh.T() * ra;
    // head right / forward in world axes (rows of the centre-eye view) for the directional radio
    VRRadio::NoteHead(rc.m[0], rc.m[2]);

    // Cull scale: the engine culls against |x|,|y| <= 1.1 z (skeleton.cpp) and
    // particles against the 90-degree UAFrustum. Scale the view rows so that
    // frustum covers the union of both eye frusta (+5 % for the IPD offset);
    // the per-eye projection undoes the scale exactly.
    double tx = 0.0, ty = 0.0;
    for (int e = 0; e < 2; e++)
    {
        const Fovf &f = g_views.eye[e].fov;
        tx = std::max(tx, std::max((double)fabs(tan(f.angleLeft)), (double)fabs(tan(f.angleRight))));
        ty = std::max(ty, std::max((double)fabs(tan(f.angleUp)), (double)fabs(tan(f.angleDown))));
    }
    if (tx <= 0.0) tx = 1.0;
    if (ty <= 0.0) ty = 1.0;
    g_cullSX = 1.1 / (tx * 1.05);
    g_cullSY = 1.1 / (ty * 1.05);
    for (int j = 0; j < 3; j++)
    {
        rc.m[0][j] *= g_cullSX;
        rc.m[1][j] *= g_cullSY;
    }

    double off[3];
    ra.T().Apply(ph, off);
    view->Pos = vec3d(pa[0] + off[0], pa[1] + off[1], pa[2] + off[2]);
    view->SclRot = FromM3(rc);
    {
        const double cp[3] = {view->Pos.x, view->Pos.y, view->Pos.z};
        Aim::SetCentreCamera(rc, cp); // world-space reticle
    }

    // anchor-local -> scaled centre-eye: x_c = S rh^T (p - ph)
    {
        const M3 sr = rh.T();
        const double sc[3] = {g_cullSX, g_cullSY, 1.0};
        M4 m = M4::Ident();
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                m.m[i][j] = sc[i] * sr.m[i][j];
            m.m[i][3] = -(m.m[i][0] * ph[0] + m.m[i][1] * ph[1] + m.m[i][2] * ph[2]);
        }
        g_centreFromAnchor = m;
    }

    // Centre-eye (scaled) -> eye: x_e = Re^T (S^-1 x_c - pe)
    for (int e = 0; e < 2; e++)
    {
        const M3 re = XrRotToUa(QuatToM3(g_views.eye[e].pose.orientation));
        double pe[3];
        XrPosToUa(g_views.eye[e].pose.position, g_upm, pe);
        const M3 ret = re.T();
        M4 m = M4::Ident();
        const double inv[3] = {1.0 / g_cullSX, 1.0 / g_cullSY, 1.0};
        double t[3];
        ret.Apply(pe, t);
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                m.m[i][j] = ret.m[i][j] * inv[j];
            m.m[i][3] = -t[i];
        }
        g_eyeFromCentre[e] = m;
    }

    g_inWorldView = true;
}

bool DioramaView(TF::TForm3D *view, DioramaWindow *win)
{
    g_dioFrame = false;
    if (!g_active || !view || !g_inWorldView || !win || !Cmd::DioramaShown())
        return false;
    Table::DioXform x;
    if (!Table::DioramaXform(&x) || !(x.D > 0) || !(x.L > 0))
        return false;
    // Room = tracking space in UAV axes and units (the space BeginWorldView's centre-eye
    // view lives in). Miniature: room = k * Rt * (w - Wc) + Tc.
    const double k = g_upm * x.L / x.D;
    static const double C[3] = {1, -1, -1};
    M3 rt; // columns: world +x, +y (down), +z in room UAV axes
    for (int i = 0; i < 3; i++)
    {
        rt.m[i][0] = C[i] * x.ex[i];
        rt.m[i][1] = -C[i] * x.n[i];
        rt.m[i][2] = C[i] * x.ey[i];
    }
    const double wc[3] = {x.lenX * 0.5, x.refY, -x.lenY * 0.5};
    const double tc[3] = {g_upm * x.c[0], -g_upm * x.c[1], -g_upm * x.c[2]};
    const M3 rh = XrRotToUa(QuatToM3(g_views.head.orientation));
    double ph[3];
    XrPosToUa(g_views.head.position, g_upm, ph);
    // view = S rh^T (room - ph) = S k rh^T Rt (w - (Wc + Rt^T (ph - Tc) / k))
    M3 rc = rh.T() * rt;
    for (int j = 0; j < 3; j++)
    {
        rc.m[0][j] *= g_cullSX * k;
        rc.m[1][j] *= g_cullSY * k;
        rc.m[2][j] *= k;
    }
    const double dp[3] = {ph[0] - tc[0], ph[1] - tc[1], ph[2] - tc[2]};
    double dw[3];
    rt.T().Apply(dp, dw);
    const vec3d pos(wc[0] + dw[0] / k, wc[1] + dw[1] / k, wc[2] + dw[2] / k);
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(k))
        return false;
    view->Pos = pos;
    view->SclRot = FromM3(rc);
    win->x0 = x.win[0];
    win->y0 = x.win[1];
    win->x1 = x.win[2];
    win->y1 = x.win[3];
    win->k = k;
    win->me = x.me;
    win->hoverGid = Table::DioHoverGid();
    {
        uint32_t cg = 0;
        double hand[3];
        win->carryGid = 0;
        if (Table::DioCarry(&cg, hand))
        {
            const double room[3] = {g_upm * hand[0], -g_upm * hand[1], -g_upm * hand[2]};
            const double d[3] = {room[0] - tc[0], room[1] - tc[1], room[2] - tc[2]};
            double w[3];
            rt.T().Apply(d, w);
            win->carryGid = cg;
            for (int i = 0; i < 3; i++)
                win->carryPos[i] = wc[i] + w[i] / k;
            static double cs = -1;
            if (cs < 0)
                cs = getenv("UA_VR_DIO_CARRY_SCALE") ? std::max(0.5, atof(getenv("UA_VR_DIO_CARRY_SCALE"))) : 4.0;
            win->carryScale = cs; // reads at the hand (a tank is ~2 cm then)
        }
    }
    g_dioFrame = true;
    g_dioK = k;
    // Clip the terrain at the slab edge (the gameplay content of the window: sector
    // meshes reach 450 of the 1200 units from the centre, the fillers to the border are
    // not drawn). World planes n.w + d >= 0 -> view space: n_v = M^-T n, d_v = n.Pos + d.
    {
        const M3 &m = rc;
        const double det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
                           m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                           m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
        if (fabs(det) > 1e-300)
        {
            M3 inv; // M^-1
            inv.m[0][0] = (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) / det;
            inv.m[0][1] = (m.m[0][2] * m.m[2][1] - m.m[0][1] * m.m[2][2]) / det;
            inv.m[0][2] = (m.m[0][1] * m.m[1][2] - m.m[0][2] * m.m[1][1]) / det;
            inv.m[1][0] = (m.m[1][2] * m.m[2][0] - m.m[1][0] * m.m[2][2]) / det;
            inv.m[1][1] = (m.m[0][0] * m.m[2][2] - m.m[0][2] * m.m[2][0]) / det;
            inv.m[1][2] = (m.m[0][2] * m.m[1][0] - m.m[0][0] * m.m[1][2]) / det;
            inv.m[2][0] = (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]) / det;
            inv.m[2][1] = (m.m[0][1] * m.m[2][0] - m.m[0][0] * m.m[2][1]) / det;
            inv.m[2][2] = (m.m[0][0] * m.m[1][1] - m.m[0][1] * m.m[1][0]) / det;
            const double xa = win->x0 * 1200.0 + 150.0, xb = (win->x1 + 1) * 1200.0 - 150.0;
            const double za = -(win->y1 + 1) * 1200.0 + 150.0, zb = -win->y0 * 1200.0 - 150.0;
            const double pl[4][4] = {{1, 0, 0, -xa}, {-1, 0, 0, xb}, {0, 0, 1, -za}, {0, 0, -1, zb}};
            float out[16];
            for (int i = 0; i < 4; i++)
            {
                double nv[3];
                for (int r = 0; r < 3; r++)
                    nv[r] = inv.m[0][r] * pl[i][0] + inv.m[1][r] * pl[i][1] + inv.m[2][r] * pl[i][2];
                const double dv = pl[i][0] * pos.x + pl[i][1] * pos.y + pl[i][2] * pos.z + pl[i][3];
                // normalise (view units) so the shader's distances are well scaled
                const double len = sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
                for (int r = 0; r < 3; r++)
                    out[i * 4 + r] = (float)(nv[r] / len);
                out[i * 4 + 3] = (float)(dv / len);
            }
            GFX::Engine.SetVRClipPlanes(out);
        }
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        fprintf(stderr, "UAVR diorama: on, sectors %d..%d x %d..%d, %.3f m across the map (%.5f m per world unit, k %.6f), table at %.2f,%.2f,%.2f\n",
                win->x0, win->x1, win->y0, win->y1, x.L * (x.lenX >= x.lenY ? 1.0 : x.lenX / x.lenY), x.L / x.D, k, x.c[0], x.c[1], x.c[2]);
    }
    return true;
}

void RasterizeEyes()
{
    if (!g_active)
        return;

    g_ft.eyesStart = Now();

    const double zn = GFX::Engine.GetProjectionNear();
    const double zf = GFX::Engine.GetProjectionFar();
    M4 proj[2];
    for (int e = 0; e < 2; e++)
        proj[e] = UaAsymFrustum(g_views.eye[e].fov, zn, zf) * g_eyeFromCentre[e];

    const int qf = g_qFrame;

    // the real room behind the miniature (Quest passthrough; UA_VR_PASSTHROUGH=0 = off).
    static int ptEnv = -1;
    if (ptEnv < 0)
    {
        const char *v = getenv("UA_VR_PASSTHROUGH");
        ptEnv = (v && !strcmp(v, "0")) ? 0 : 1;
    }
    const bool pt = g_backend->SetPassthrough(g_dioFrame && ptEnv);
    g_ptFrame = g_dioFrame && pt;
    static bool ptLogged = false;
    if (pt && !ptLogged)
    {
        ptLogged = true;
        fprintf(stderr, "UAVR diorama: passthrough on (the room behind the miniature)\n");
    }

    GFX::Engine.RasterizePasses(2,
        [&](int eye)
        {
            g_eyeT0 = Now();
            RenderTarget rt = g_backend->AcquireEye(eye);
            g_acc.acq += Now() - g_eyeT0;
            gl.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
            glViewport(0, 0, rt.width, rt.height);
            glDepthMask(GL_TRUE);
            if (g_dioFrame && pt) // alpha 0 = the passthrough room shows through
                glClearColor(0, 0, 0, 0);
            else if (g_dioFrame) // no passthrough (desktop sim): a neutral room grey
                glClearColor(0.16f, 0.165f, 0.17f, 1);
            else
                glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDepthMask(GFX::Engine.GetLastStates().Zwrite ? GL_TRUE : GL_FALSE);
            const M4 &p = proj[eye];
            GFX::Engine.SetProjectionMatrix(mat4x4f(p.m[0][0], p.m[0][1], p.m[0][2], p.m[0][3],
                                                    p.m[1][0], p.m[1][1], p.m[1][2], p.m[1][3],
                                                    p.m[2][0], p.m[2][1], p.m[2][2], p.m[2][3],
                                                    p.m[3][0], p.m[3][1], p.m[3][2], p.m[3][3]));
            if (gl.timerQueries)
                gl.BeginQuery(GL_TIME_ELAPSED, g_q[qf][eye]);
        },
        [&](int eye)
        {
            const double tO = Now();
            if (g_dioFrame)
            {
                // diorama: the player stands in the room at the table. No cockpit, bridge,
                // comfort reference or world-anchored marks (they belong to the full-scale
                // world); the table layer (base, picks, rings, fade) draws over the miniature.
                if (pt)
                    Table::DioPassthroughAlpha(); // drawn miniature = opaque, the rest = the room
                Table::DioDim(Station::DioFade()); // station beam fade = the miniature darkens, the room stays
                Bridge::DrawPanelsEye(eye, proj[eye], g_centreFromAnchor); // bridge screens as panels in the room (no metal)
                Station::DrawDioEye(eye, g_views, zn, zf); // built unit rolls out on the miniature
                Aim::DrawEye(eye, g_views, g_eyeFromCentre[eye], zn, zf, true); // panel laser only
                Cockpit::DrawWristEye(eye, proj[eye], g_centreFromAnchor, g_views, g_ctrl); // wrist panel + rows stay usable
                Field::DrawEye(eye, g_views, zn, zf, true); // wrist pointer / squad wheel (hand-anchored)
                Panels::DrawEye(eye, g_views, zn, zf); // panel grab bars (PANELS FREE)
                Cmd::DrawEye(eye, g_views, zn, zf, true);
            }
            else
            {
            Station::DrawWorldFade(eye); // world black around a station beam (bridge + table stay)
            const bool shell = Cockpit::DrawEye(eye, proj[eye], g_centreFromAnchor, g_views, g_ctrl); // cockpit
            const bool bridge = !shell && Bridge::DrawEye(eye, proj[eye], g_centreFromAnchor); // bridge (Command Deck, station)
            ComfortDrawEye(eye, proj[eye], g_centreFromAnchor, g_views.eye[eye].fov, !shell && !bridge); // reference + vignette
            Aim::DrawEye(eye, g_views, g_eyeFromCentre[eye], zn, zf); // reticle + laser beam
            Field::DrawEye(eye, g_views, zn, zf); // order marker, wrist hover, squad wheel
            Station::DrawEye(eye, g_views, zn, zf); // built unit: beam-in + marker
            if (!Station::WorldHidden()) // no site outline floating in the beam blackout
                Sites::DrawEye(eye, g_views, zn, zf); // captured-site outline + beacon (Command Deck)
            Panels::DrawEye(eye, g_views, zn, zf); // panel grab bars (PANELS FREE)
            Cmd::DrawEye(eye, g_views, zn, zf); // map board, build wheel, fade
            }
            if (gl.timerQueries)
                gl.EndQuery(GL_TIME_ELAPSED);
            const double tR = Now();
            g_acc.ovl += tR - tO;
            g_backend->ReleaseEye(eye);
            g_acc.rel += Now() - tR;
            g_ft.eyeCpu[eye] = Now() - g_eyeT0;
        });

    // UA_VR_SYNC_TIMING=1: wait for the GPU here so the phases after the eye
    // pass (HUD/2D, present) are measured without the implicit GPU sync that
    // otherwise lands in whichever call first touches a busy resource.
    static int syncTiming = -1;
    if (syncTiming < 0)
        syncTiming = EnvOn("UA_VR_SYNC_TIMING") ? 1 : 0;
    if (syncTiming)
    {
        glFinish();
        g_ft.eyeCpu[1] = Now() - g_eyeT0; // right eye includes the GPU drain
    }

    if (gl.timerQueries)
    {
        g_qIssued[qf] = true;
        g_qFrame = (g_qFrame + 1) % kQRing;
    }

    g_worldRendered = true;
}

void EndWorldView(TF::TForm3D *view)
{
    if (!g_active || !g_inWorldView)
        return;
    g_inWorldView = false;
    if (g_dioFrame)
        GFX::Engine.SetVRClipPlanes(NULL); // the slab clip never outlives the miniature's frame
    g_dioFrame = false;

    // HUD code (target markers, radar, visor) keeps projecting with the
    // vehicle camera, i.e. onto the cockpit panel. See "HUD".
    view->Pos = g_anchorPos;
    view->SclRot = g_anchorRot;
    view->CalcGlobal();

    GFX::Engine.SetProjectionMatrix(GFX::Engine.GetProjectionMatrix());
    BindPanel();
    g_ft.hud = Now();
}

bool GetControllers(ControllerState out[2])
{
    if (!g_active || (!g_ctrl[0].active && !g_ctrl[1].active))
        return false;
    out[0] = g_ctrl[0];
    out[1] = g_ctrl[1];
    return true;
}

void Rumble(float low, float high, uint32_t ms)
{
    if (!g_active)
        return;
    const float a = std::max(low, high);
    Cockpit::Haptic(0, a, (float)ms, "ff");
    Cockpit::Haptic(1, a, (float)ms, "ff");
}

bool ViewerIsFlyer() { return g_viewerFlyer; }
bool ViewerDrives() { return g_active && g_viewerDrives; }
double ViewerForwardSpeed() { return g_viewerFwd; }

uint32_t TickMs(uint32_t measured)
{
    if (!g_active || !g_displayTick || g_frame.predictedDisplayTimeNs <= 0)
        return measured;
    double ms;
    if (g_lastDisplayNs > 0 && g_frame.predictedDisplayTimeNs > g_lastDisplayNs)
        ms = (g_frame.predictedDisplayTimeNs - g_lastDisplayNs) / 1.0e6;
    else
        ms = g_frame.predictedDisplayPeriodNs > 0 ? g_frame.predictedDisplayPeriodNs / 1.0e6 : (double)measured;
    g_lastDisplayNs = g_frame.predictedDisplayTimeNs;
    g_tickAcc += std::min(ms, 40.0); // same 40 ms ceiling the engine uses for a stalled frame
    uint32_t whole = (uint32_t)g_tickAcc;
    g_tickAcc -= whole;
    return std::max<uint32_t>(whole, 1);
}

void TimingMark(Mark m)
{
    if (!g_active)
        return;
    if (m == MARK_HUD_START && g_ft.hud < 0)
        g_ft.hud = Now();
    if (m == MARK_END_FRAME && g_ft.endFrame < 0)
        g_ft.endFrame = Now();
}

double HudT0()
{
    return g_active ? Now() : 0.0;
}

void HudAdd(HudPart p, double t0)
{
    if (t0 > 0.0 && g_active && (unsigned)p < (unsigned)HUD_PARTS)
        g_acc.part[p] += Now() - t0;
}

uint32_t DefaultFramebuffer()
{
    return g_active ? g_backend->PanelTarget().fbo : 0;
}

bool Present()
{
    if (!g_active)
        return false;

    g_ft.present = Now();

    RenderTarget panel = g_backend->PanelTarget();
    const float aspect = panel.height > 0 ? (float)panel.width / (float)panel.height : 16.0f / 9.0f;

    // No 3D world this frame (menus, briefing, FMV): still sample the head so
    // the theatre screen is drawn from the current pose.
    if (!g_worldRendered)
    {
        if (!g_backend->LocateViews(g_frame.predictedDisplayTimeNs, &g_views) || !g_views.valid)
            g_views.valid = false;
    }

    // passthrough only for a frame whose eyes are the miniature; any other
    // frame (cockpit, menus, briefing, bridge theatre) pauses it and submits no layer.
    if (!(g_worldRendered && g_ptFrame))
    {
        g_ptFrame = false;
        g_backend->SetPassthrough(false);
    }
    FrameEnd fe;
    fe.worldRendered = g_worldRendered;
    fe.passthrough = g_ptFrame;
    if (g_worldRendered)
    {
        fe.panel.mode = PANEL_COCKPIT;
        const bool deckTable = Cmd::DeckTableAtStation(); // keep the war table clear
        const float pw = deckTable ? g_deckPanelWidthM : g_panelWidthM;
        const float pd = deckTable ? g_deckPanelDropM : g_panelDropM;
        static int lastDeckTable = -1;
        if (deckTable != (lastDeckTable == 1))
        {
            if (lastDeckTable >= 0 || deckTable)
                fprintf(stderr, "UAVR: HUD panel %s (%.2f m wide, %.2f m %s the eye)\n", deckTable ? "raised for the war table" : "back to the cockpit place", pw, fabs(pd), pd >= 0 ? "below" : "above");
            lastDeckTable = deckTable ? 1 : 0;
        }
        fe.panel.widthM = pw;
        fe.panel.heightM = pw / aspect;
        fe.panel.pose.position.x = 0.0f;
        fe.panel.pose.position.y = -pd;
        fe.panel.pose.position.z = -g_panelDistM;
        // tilt the panel to face the eye point
        fe.panel.pose.orientation = QuatFromAxisAngle(1, 0, 0, -atan2(pd, g_panelDistM));
    }
    else
    {
        fe.panel.mode = PANEL_THEATRE;
        fe.panel.widthM = g_theatreWidthM;
        fe.panel.heightM = g_theatreWidthM / aspect;
        fe.panel.pose.position.z = -g_theatreDistM;
    }
    // Command Deck: briefing / debrief on the bridge monitor instead of the void.
    if (Bridge::Theatre(g_backend, g_worldRendered ? ViewState() : g_views, aspect, &fe.panel))
        fe.worldRendered = true; // both eyes hold the bridge room
    Panels::Apply(&fe, Cmd::DeckTableAtStation() ? 1 : 0, g_views); // FREE = top + bottom quad (FIXED: untouched)

    BindPanel();
    const double tPanels = Now();
    Aim::DrawPanel(panel.width, panel.height); // pointer cursor
    Cmd::DrawPanel(panel.width, panel.height); // command-layer fade
    g_acc.part[HUD_PANELS] += Now() - tPanels;
    Aim::NotePanel(fe.panel, panel.width, panel.height, fe.split ? &fe.panel2 : NULL);
    g_backend->EndFrame(fe);
    GFX::Engine.VRFrameBoundary(); // fence + rotate the UBO ring

    if (g_worldRendered)
        g_presentedWorld++;
    g_worldRendered = false;
    g_ptFrame = false;
    BindPanel();
    g_ft.presentEnd = Now();
    return true;
}

// explicit level boundary from NC_STACK_ypaworld::DeleteLevel.
void LevelBoundary()
{
    if (!Active())
        return;
    fprintf(stderr, "UAVR: level boundary (DeleteLevel) -> Command Deck state reset\n");
    Station::ResetLevel(); // + Table::ResetLevel
    Sites::ResetLevel();
}

// Dev aid (inert unless set, works with VR on or off):
//   UA_VR_TAKE_TANK=<n>  after n gameplay frames, take control of the first
//                        player-owned tank (BACT_TYPES_TANK) - the unattended way
//                        to sit in a tank (level 1 starts in the host station).
void GameplayFrameHook(NC_STACK_ypaworld *yw)
{
    static int takeAt = -2;
    if (takeAt == -2)
    {
        const char *v = getenv("UA_VR_TAKE_TANK");
        takeAt = (v && v[0]) ? atoi(v) : -1;
    }
    Aim::Gameplay(yw); // controls card
    Cmd::Gameplay(yw); // command layer snapshot
    Field::Gameplay(yw); // squads, ray cast, aggression
    Sites::Gameplay(yw); // captured sites, host energy trend (Command Deck)
    Cockpit::Gameplay(yw); // cockpit readouts + haptics
    Bridge::Gameplay(yw); // bridge screens snapshot
    VRRadio::Gameplay(yw); // directional radio (wrist ping, dev aid)
    Station::Gameplay(yw); // station: 2D windows, built units, beam
    Panels::Gameplay(yw); // panel layout -> profile
    if (yw && yw->_viewerBact)
    {
        const int t = yw->_viewerBact->_bact_type;
        g_viewerFlyer = (t == BACT_TYPES_BACT || t == BACT_TYPES_FLYER || t == BACT_TYPES_UFO);
        const NC_STACK_ypabact *vb = yw->_viewerBact;
        g_viewerVel[0] = vb->_fly_dir.x * vb->_fly_dir_length;
        g_viewerVel[1] = vb->_fly_dir.y * vb->_fly_dir_length;
        g_viewerVel[2] = vb->_fly_dir.z * vb->_fly_dir_length;
        g_viewerDrives = (t == BACT_TYPES_TANK || t == BACT_TYPES_CAR);
        g_viewerFwd = vb->_fly_dir.dot(vb->_rotation.AxisZ()) * vb->_fly_dir_length;
        g_viewerKey = ((uint64_t)(uint32_t)yw->_levelInfo.LevelID << 32) | (uint32_t)vb->_gid;
    }
    // Both thumbsticks clicked: cycle the comfort vignette preset (vr_comfort.cpp),
    // confirmed in the info ticker. Saved with the profile like the menu setting.
    if (g_active && yw && g_comboLabel)
    {
        yw_arg159 msg;
        msg.unit = NULL;
        msg.Priority = 60;
        msg.txt = g_comboLabel;
        msg.MsgID = 0;
        yw->ypaworld_func159(&msg);
        g_comboLabel = NULL;
    }

    if (takeAt < 0 || !yw || !yw->_userRobo || !yw->_viewerBact)
        return;

    static int frames = 0;
    if (++frames < takeAt)
        return;

    NC_STACK_ypabact *tank = NULL;
    for (NC_STACK_ypabact *cmd : yw->_userRobo->_kidList)
    {
        if (cmd->_bact_type == BACT_TYPES_TANK && cmd->_status != BACT_STATUS_DEAD)
            tank = cmd;
        for (NC_STACK_ypabact *u : cmd->_kidList)
            if (!tank && u->_bact_type == BACT_TYPES_TANK && u->_status != BACT_STATUS_DEAD)
                tank = u;
        if (tank)
            break;
    }

    if (!tank)
    {
        if (frames == takeAt || frames % 200 == 0)
            fprintf(stderr, "UAVR: UA_VR_TAKE_TANK: no player tank yet (frame %d)\n", frames);
        return;
    }

    NC_STACK_ypabact *old = yw->_viewerBact;
    old->setBACT_viewer(false);
    old->setBACT_inputting(false);
    tank->setBACT_viewer(true);
    tank->setBACT_inputting(true);
    fprintf(stderr, "UAVR: took control of tank gid %d at gameplay frame %d\n", (int)tank->_gid, frames);
    takeAt = -1;
}

// menu: FPS counter (see vr.h).
static bool g_fpsCounter = false;
bool FpsCounterOn() { return g_fpsCounter; }
void SetFpsCounterOn(bool on) { g_fpsCounter = on; }
float LastFps() { return g_active ? g_lastFps : 0.0f; }

// display rate setting (see vr.h).
int DisplayRateSetting() { return g_hzSetting; }

void SetDisplayRateSetting(int hz)
{
    g_hzSetting = (hz == 72 || hz == 90 || hz == 120) ? hz : 0;
    ApplyDisplayRate();
}

int NextDisplayRateSetting(int hz)
{
    switch (hz)
    {
    case 0: return 72;
    case 72: return 90;
    case 90: return 120;
    default: return 0;
    }
}

const char *DisplayRateLabel(int hz)
{
    switch (hz)
    {
    case 72: return "DISPLAY: 72 HZ";
    case 90: return "DISPLAY: 90 HZ";
    case 120: return "DISPLAY: 120 HZ";
    default: return "DISPLAY: AUTO";
    }
}

} // namespace VR
