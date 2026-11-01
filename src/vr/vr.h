#ifndef UA_VR_H_INCLUDED
#define UA_VR_H_INCLUDED

// Urban Assault VR (proof, branch).
//
// Two layers live here:
//
//  1. VR::Backend — the abstract device interface. The desktop simulator
//     (vr_sim.cpp) implements it today; the Quest 3 OpenXR shell implements it
//     later. Conventions follow OpenXR exactly so the shell is a thin mapping:
//     metres, right-handed, +X right, +Y up, -Z forward, quaternions x,y,z,w,
//     FOV as four signed angles in radians (left/down negative).
//
//  2. VR:: engine glue (vr_engine.cpp) — the only thing the game code calls.
//     Every entry point is a no-op unless the runtime switch UA_VR=sim (or a
//     future UA_VR=openxr) selected a backend, and the whole module is only
//     compiled in with -DUA_VR=ON (CMake option: ON for the Quest build, OFF on the desktop). With VR off the
//     engine takes exactly the code paths it took without the module.
//
// Frame contract (one game tick per displayed frame, both eyes same tick):
//
//   FrameStart()          -> Backend::BeginFrame()   (xrWaitFrame/xrBeginFrame: pacing,
//                                                     predicted display time)
//   game logic (once)
//   RenderGame:
//     BeginWorldView()    -> Backend::LocateViews()  (LATE head pose for the predicted
//                                                     display time; both eyes from it)
//     scene collection (once, from the centre-eye camera; culling/LOD/particles)
//     RasterizeEyes()     -> Backend::AcquireEye()/ReleaseEye() per eye; the queued
//                            render list is drawn twice with per-eye projection
//     EndWorldView()      -> restore the vehicle ("cockpit anchor") camera for HUD code
//   2D/HUD               -> drawn into Backend::PanelTarget() (offscreen, quad layer later)
//   System::Flip()        -> Present() -> Backend::EndFrame() (simulator: composite + swap;
//                                                     OpenXR: xrEndFrame with projection
//                                                     layer + quad layer)

#include <stdint.h>

class NC_STACK_ypaworld;
class NC_STACK_ypabact;
namespace TF { class TForm3D; }

namespace VR
{

struct Quatf { float x = 0, y = 0, z = 0, w = 1; };
struct Vec3f { float x = 0, y = 0, z = 0; };

struct Posef
{
    Quatf orientation;
    Vec3f position; // metres
};

struct Fovf
{
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0; // radians
};

struct FrameInfo
{
    bool    shouldRender = true;          // false: runtime says skip rendering (xrBeginFrame)
    int64_t predictedDisplayTimeNs = 0;   // pass back to LocateViews()
    int64_t predictedDisplayPeriodNs = 0; // e.g. 11.1 ms at 90 Hz
};

struct EyeView
{
    Posef pose; // relative to the HEAD pose (IPD offset, optional canting)
    Fovf  fov;  // asymmetric per-eye frustum
};

struct ViewState
{
    bool    valid = false;
    Posef   head;   // in the tracking ("local", seated) space; origin = calibrated seated eye point
    EyeView eye[2]; // 0 = left, 1 = right
};

struct RenderTarget
{
    uint32_t fbo = 0;    // GL framebuffer to draw into (colour + depth complete)
    uint32_t colorTex = 0;
    int width = 0, height = 0;
};

enum ControllerButton
{
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_MENU = 1 << 4, BTN_STICK_CLICK = 1 << 5,
    BTN_TRIGGER_TOUCH = 1 << 6, BTN_THUMB_TOUCH = 1 << 7,
};

struct ControllerState
{
    bool     active = false;
    float    stickX = 0, stickY = 0; // -1..1, +Y = push forward
    float    trigger = 0, grip = 0;  // 0..1
    uint32_t buttons = 0;            // ControllerButton bits
    Posef    aimPose, gripPose;      // tracking space
};

// How the offscreen 2D panel (HUD, menus, FMV) is shown this frame.
enum PanelMode
{
    PANEL_COCKPIT = 0, // 3D world active: world-locked panel ~1.5 m ahead, slightly low
    PANEL_THEATRE = 1, // no 3D world this frame (menus, briefing, FMV): big screen
};

struct PanelLayer
{
    PanelMode mode = PANEL_THEATRE;
    Posef     pose;            // centre of the quad, tracking space
    float     widthM = 1, heightM = 1;
    float     v0 = 0, v1 = 1;  // rows of the 2D panel on this quad (0 = top row, 1 = bottom)
};

struct FrameEnd
{
    bool       worldRendered = false; // eyes hold a 3D view from this frame
    bool       passthrough = false;   // this frame's eyes are the miniature drawn for passthrough
    PanelLayer panel;
    // PANELS FREE: the 2D panel is shown as two quads, `panel` = rows v0..v1 (top:
    // stats + windows) and `panel2` (bottom: tool bar). false = one quad (FIXED, menus).
    bool       split = false;
    PanelLayer panel2;
};

class Backend
{
public:
    virtual ~Backend() {}
    virtual const char *Name() const = 0;

    // GL context is current. Return false to fall back to flat mode.
    virtual bool Init() = 0;
    virtual void Shutdown() = 0;

    // Frame pacing (blocks until the runtime wants the next frame).
    virtual bool BeginFrame(FrameInfo *out) = 0;
    // Late head + eye poses for the predicted display time. Called once per
    // frame, as late as possible (right before the scene is collected).
    virtual bool LocateViews(int64_t predictedDisplayTimeNs, ViewState *out) = 0;

    virtual RenderTarget AcquireEye(int eye) = 0; // swapchain image for this frame
    virtual void ReleaseEye(int eye) = 0;         // MSAA resolve / xrReleaseSwapchainImage

    // Offscreen 2D panel (HUD/menus/FMV). Size = the engine's 2D canvas.
    virtual RenderTarget PanelTarget() = 0;

    // Submit/compose and present. Simulator: draws the panel into both eyes,
    // shows both eyes side by side in the desktop window and swaps.
    virtual void EndFrame(const FrameEnd &end) = 0;

    virtual void PollControllers(ControllerState out[2]) = 0;
    // Hook for camera shake -> haptics (shake never moves the VR view).
    virtual void Haptic(int hand, float amplitude, float durationMs) { (void)hand; (void)amplitude; (void)durationMs; }

    // Eye render target size the backend wants (per eye).
    virtual void EyeSize(int *w, int *h) const = 0;

    // Display refresh: offered rates ascending, current rate, runtime request.
    virtual int RefreshRates(float *out, int max) { (void)out; (void)max; return 0; }
    virtual float RefreshHz() const { return 0.0f; }
    virtual bool RequestRefreshRate(float hz) { (void)hz; return false; }

    // diorama: show the real room behind the projection layer (XR_FB_passthrough).
    // Called every world frame; returns true when passthrough is running and the eye
    // images are composited with their alpha (then clear to alpha 0).
    virtual bool SetPassthrough(bool on) { (void)on; return false; }
};

// ---------------------------------------------------------------------------
// Engine glue (vr_engine.cpp). All functions are cheap no-ops when !Active().

// Reads UA_VR (off|sim). Call once after GFX::Engine.Init() (GL context live).
void Init();
void Shutdown();
bool Active();

// World scale: UA world units per metre (UA_VR_UNITS_PER_M, default 6.0;
// derived from the engine physics: velocity m/s * 6 = units/s, g = 9.80665).
double UnitsPerMetre();

// Start of every engine frame (main loop): backend pacing + timing.
void FrameStart();

// Camera shake/sway: true = the engine must not apply its view shake matrix.
// shakeTrace = trace of that matrix (3 = none): its angle is measured and sent
// to the controller haptics instead (vr_cockpit.cpp).
bool SuppressViewShake(double shakeTrace = 3.0);

// RenderGame hooks. `view` is the engine's view TForm (the vehicle camera).
void BeginWorldView(TF::TForm3D *view);
void RasterizeEyes();
void EndWorldView(TF::TForm3D *view);
// Living Diorama. Right after BeginWorldView: when the
// Command Deck table is open, the view TForm becomes the miniature's camera (the centre
// eye looking at the world through TableFromWorld: a uniform scale k = room units per
// world unit, so view space stays the room) and RenderGame collects the sector window
// below instead of the viewer's diamond (no sky; RasterizeEyes then draws the room
// background instead of the cockpit / bridge). False = the normal world view.
struct DioramaWindow
{
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // sectors, inclusive (gameplay sectors only)
    double k = 1.0;                       // view scale (room units per world unit)
    int me = -1;                          // the player's owner id (enemy units only where seen)
    uint32_t hoverGid = 0;                // the unit under the pointer: drawn again as a glow
    uint32_t carryGid = 0;                // picked up: drawn at the hand instead of its place
    double carryPos[3] = {0, 0, 0};       // ... world position of the hand
    double carryScale = 1.0;              // ... model scale there
};
bool DioramaView(TF::TForm3D *view, DioramaWindow *win);
// what the miniature submitted this frame (auto window budget, vr_table.cpp).
void DioramaReportDraws(int draws, int sectors);

// Framebuffer the engine treats as "the screen" (0 when VR is off).
uint32_t DefaultFramebuffer();

// System::Flip() replacement while active. Returns true when it presented.
bool Present();

// Dev aids for unattended runs (inert unless their env vars are set).
void GameplayFrameHook(NC_STACK_ypaworld *yw);

// Controllers (polled once per frame in FrameStart). False when VR is off or the
// backend has no controllers; the game input (winp.cpp) then ignores them.
bool GetControllers(ControllerState out[2]);

// Pad rumble from the engine's force-feedback mixer -> controller haptics
// (0..1 per motor; both hands get the stronger one).
void Rumble(float low, float high, uint32_t ms);

// Current viewer is a flying unit (helicopter/plane/UFO): grips climb/descend
// instead of braking. Updated by GameplayFrameHook.
bool ViewerIsFlyer();

// Game tick for this frame in whole ms. OpenXR: from the runtime's predicted
// display times (fractional accumulator), so the simulation advances exactly
// one display period per frame on average instead of wall-clock ms (removes
// the 11/12 ms jitter at 90 Hz). Returns `measured` when VR is off or the
// backend has no display clock (the desktop simulator, unless
// UA_VR_DISPLAY_TICK=1).
uint32_t TickMs(uint32_t measured);

// aim / pointer / controls card ----------

// World-space crosshair. Called from each unit's User_layer right after
// UserTargeting, with the exact direction the weapon code passes to
// LaunchMissile/FireMinigun (world axes; 0,0,0 = the unit's forward axis, as
// LaunchMissile treats it) and the fire offset it uses (unit-local; the
// alternating barrel x is passed as 0 = the centre line). Ray-casts the gun
// ray against sectors/buildings and the locked unit; the reticle is drawn at
// the hit point in both eyes.
void NoteUserAim(NC_STACK_ypaworld *world, NC_STACK_ypabact *unit, double dx, double dy, double dz,
                 double fireX, double fireY, double fireZ);
// UA_VR_AIM_LOG=1 verification: a user weapon was launched / exploded.
// `shot` identifies the projectile (pairs launch and impact).
void NoteUserShot(NC_STACK_ypabact *unit, const void *shot, double px, double py, double pz, double dx, double dy, double dz);
void NoteUserImpact(const void *shot, double px, double py, double pz);
// The flat 2D gun/weapon reticles are replaced by the world-space one.
bool HideFlatCrosshair();

// Laser pointer (right controller aim ray onto the 2D panel). Active in
// theatre mode (menus, briefing); in the cockpit while the ray is on an open
// game window (map, squad manager, ...), or anywhere on the panel in the host
// station and while the exit menu / a confirm box is up. panelX/Y = panel
// pixels, origin top-left. While active the right trigger is the left mouse
// button and the right grip / B the right mouse button (winp.cpp).
bool GetPointer(int *panelX, int *panelY);
// the game UI pointer mode of this frame (SetUiPointer; 0 when stale / off):
// 1 = whole panel (host station, modal box), 2 = only over window rects (vehicles).
int UiPointerMode();
// Where the ray meets the panel regardless of whether the pointer is "on" (used
// while a pointer button is held, so a drag keeps reporting outside a window).
// False when it misses the panel. panelMode = PanelMode of the panel it hit.
bool GetPointerRaw(int *panelX, int *panelY, int *panelMode);
// Game UI, every gameplay HUD frame: mode 0 = off, 1 = whole panel, 2 = only
// over the given window rects (x,y,w,h in the game's 2D coordinates).
void SetUiPointer(int mode, const int *xywh, int count);

// In-VR controls card: true while it should be drawn on the panel (8 s on the
// first entry into each vehicle class).: with the pause panel (exit menu)
// open, the card is shown only when the panel's CONTROLS toggle is on.
// kind: 0 ground vehicle, 1 flyer, 2 host station / station gun.
bool ControlsCard(int *kind);

// Comfort vignette ("tunnel vision") player setting, percent 0..100.
// Presets Off 0 / Very low 25 / Low 50 / Medium 70 / High 85 / Max 100 (
// default Low). Persisted in the profile by
// the game (yw_save.cpp ";#!vr_vignette"); UA_VR_VIGNETTE overrides the
// effect for testing. Implemented in vr_comfort.cpp; valid with VR off too.
int ComfortVignettePercent();
void SetComfortVignettePercent(int pct);
const char *ComfortVignettePreset(int pct); // "OFF" | "LOW" | "HIGH" | "CUSTOM"
int CycleComfortVignette();                 // next preset, returns its percent

// command layer --------
// World update, between the UI pass (ypaworld_func64__sub21) and
// yparobo::HandleUserCommands: executes the board / wheel / dive / snap actions
// through the original user-action code.
void CommandWorldHook(NC_STACK_ypaworld *yw);
// Controls card text: the command layer's line for card `kind` / line index, or `def`.
const char *CommandCardLine(int kind, int line, const char *def);

// cockpit ------------------------------

// Cockpit shells + dash screens + wrist panel player setting (default on).
// Off = the flat HUD panel as before. Persisted in the profile by the game
// (yw_save.cpp ";#!vr_cockpit"); UA_VR_COCKPIT=0/1 overrides at start. Valid
// with VR off too.
bool CockpitShellsOn();
void SetCockpitShellsOn(bool on);
// True while a shell is drawn for the current vehicle: the vehicle readouts
// (radar, compass, health/weapon info, target life bar) are shown on the dash
// screens and the flat HUD must not draw them on the panel.
bool CockpitOwnsHud();
// cockpit frame style for the ground and jet
// shells. Open (default) = nothing above eye level: dash, hood line, low sill,
// screens under the hood line. Classic = the frames (roof/pillars/top bar,
// canopy bow). Live switch, not persisted (a Settings row would belong to the
// menus); UA_VR_COCKPIT_FRAME=open|minimal|classic|full overrides.
bool CockpitFrameOpen();
// three frame styles. OPEN (default) as above; OPEN_RIM = the open ground
// frame plus a thin overhead rim (no roof, jets as OPEN); CLASSIC = frames.
// Profile ";#!vr_frame": 1 open, 0 classic (values kept), 2 open + rim.
enum CockpitFrameStyle { FRAME_OPEN = 0, FRAME_OPEN_RIM = 1, FRAME_CLASSIC = 2, FRAME_COUNT };
int CockpitFrame();               // effective style (UA_VR_COCKPIT_FRAME wins)
int CockpitFrameSetting();        // stored player setting (Settings row "COCKPIT FRAME")
void SetCockpitFrame(int style);
int CockpitFrameToProfile(int style);   // -> ;#!vr_frame value
int CockpitFrameFromProfile(int value); // <- ;#!vr_frame value (unknown -> OPEN)
const char *CockpitFrameLabel(int style);

// display rate setting (Settings row "DISPLAY", profile ";#!vr_hz").
// 0 = AUTO (vr_engine.cpp AutoHz: highest rate that holds), else 72 / 90 / 120
// pinned. UA_VR_HZ (non-auto) or UA_VR_HZ_REQUEST in the environment win: the
// setting is then stored and saved but not applied. Applied live (and retried
// once per second until the runtime accepts it).
int DisplayRateSetting();
void SetDisplayRateSetting(int hz);
int NextDisplayRateSetting(int hz); // AUTO -> 72 -> 90 -> 120 -> AUTO
const char *DisplayRateLabel(int hz);

// menu: FPS counter player setting (Settings row
// "FPS COUNTER", pause panel Settings page, profile ";#!vr_fps"; default off).
// When on, the game draws the last per-second fps in the panel's top-right corner.
bool FpsCounterOn();
void SetFpsCounterOn(bool on);
float LastFps(); // fps over the last stats second (0 = not measured / VR off)

// controls + settings ------------------
// All are player settings on the Settings page (VR sessions only), saved in the
// profile (yw_save.cpp ";#!vr_grips" etc.). Valid with VR off too.

// Viewer is a tank / car (VR active): the ground grip mode applies.
bool ViewerDrives();
// Viewer speed along its nose in engine units (+ forward, - reversing).
double ViewerForwardSpeed();

// Ground-vehicle grips. DRIVE (default): R grip = forward, L grip = brake while
// rolling forward, then reverse (car-game brake). BRAKE: R grip = brake.
// OFF: grips do nothing on the ground. Flyers always climb / descend.
enum GripMode { GRIP_DRIVE = 0, GRIP_BRAKE = 1, GRIP_OFF = 2, GRIP_COUNT };
int GroundGripMode();
void SetGroundGripMode(int mode);
const char *GroundGripLabel(int mode); // Settings button text
// World crosshair colour (default yellow; the profile values are unchanged).
enum { XHAIR_GREEN = 0, XHAIR_WHITE, XHAIR_RED, XHAIR_YELLOW, XHAIR_CYAN, XHAIR_COUNT };
int CrosshairColour();
void SetCrosshairColour(int c);
const char *CrosshairLabel(int c);
void CrosshairRGB(int c, float rgb[3]);
// Map board (X tap at the host station / in units). Off = X opens the original
// 2D map window. Implemented in vr_command.cpp.
bool MapBoardOn();
void SetMapBoardOn(bool on);
// Host-station turn: snap 30 deg (default) or the game's smooth turn.
bool StationSnapTurn();
void SetStationSnapTurn(bool snap);
// snap angle in degrees (20 / 30 / 45; profile ";#!vr_snapdeg"). UA_VR_SNAP_DEG
// in the environment wins for the effect (the setting is still stored).
int StationSnapDeg();
void SetStationSnapDeg(int deg);
// the Settings "STATION TURN" row cycles SNAP 30 -> SNAP 45 -> SNAP 20 -> SMOOTH.
void CycleStationTurn();
const char *StationTurnLabel();
// controls card on the first entry into each vehicle class (8 s): on (default)
// or off (profile ";#!vr_card"). The pause panel's Show Controls works either way.
bool ControlsCardAuto();
void SetControlsCardAuto(bool on);
// VR MODE. Profile ";#!vr_mode" 0 CLASSIC / 1 COMMAND DECK.
// Default CLASSIC; the Preview build (UA_VR_PREVIEW) defaults to COMMAND DECK.
bool CommandDeck();
void SetCommandDeck(bool on);
// extras: each is "explicit" (profile 0 / 1) or "follows the mode" (profile -1 or no key:
// ON in COMMAND DECK, OFF in CLASSIC). The accessors return the effective value; the
// setters store an explicit value; ResetModeExtras() goes back to "follows the mode".
bool FieldOrders();      // ";#!vr_fieldorders"
bool WristSquads();      // ";#!vr_wristsq"   (clickable wrist squad buttons)
bool SquadWheel();       // ";#!vr_sqwheel"
bool MapWall();          // ";#!vr_mapsize": false TABLE, true WALL (unset = TABLE in every mode)
bool DirectionalRadio(); // ";#!vr_dirradio" (directional radio, vr_radio.cpp: voice callouts from the event's direction + wrist ping)
bool QuickStationPrompt(float *secLeft); // "BACK TO STATION? A" is up (seconds to the auto return)
bool QuickStation();     // ";#!vr_quickstation" (vr_command.cpp: wrist "HOLD B: STATION", B hold also while
                         // pointing in a vehicle, "BACK TO STATION? A" + auto return when the vehicle dies)
void SetFieldOrders(bool on);
void SetWristSquads(bool on);
void SetSquadWheel(bool on);
void SetMapWall(bool wall);
void SetDirectionalRadio(bool on);
void SetQuickStation(bool on);
enum { MODE_EXTRA_COUNT = 6 };
int  ModeExtraRaw(int which);          // 0 fieldorders, 1 wristsq, 2 sqwheel, 3 mapsize, 4 dirradio, 5 quickstation: -1 follows, else 0/1
void SetModeExtraRaw(int which, int v); // v < 0 = follows the mode
void ResetModeExtras();
void ResetModeToDefault();             // mode + extras back to the build default
// Living Diorama: in COMMAND DECK the war table shows the
// live game world in miniature. Hidden profile key ";#!vr_tokentable" 1 = the token
// table instead (no Settings row; the key is written only when set).
bool TokenTable();
void SetTokenTable(bool on);
// diorama placement on the real table (tracking space: metres, yaw radians about +y,
// scale 1 = the window ~1.1 m across). Profile ";#!vr_dio_x/_y/_z" (mm), "_yaw"
// (centidegrees), "_scale" (per mille): written only once the player placed / moved it.
// The setter is the parser's (one key at a time); Reset = the build default (no lines).
bool DioramaPoseSet();
void SetDioramaPoseKey(int which, int value); // 0 x, 1 y, 2 z, 3 yaw, 4 scale (profile units)
int  DioramaPoseKey(int which);
void ResetDioramaPose();
// HUD panels FIXED (default: the one
// panel exactly as before) or FREE (the 2D panel splits into a top panel - stats, windows -
// and a bottom panel - the tool bar - that the right trigger moves by their outer edge
// and resizes by their outer corners). Profile ";#!vr_panels" 0 / 1; the free layout in
// ";#!vr_panel_<c|s><t|b>_<x|y|z|yaw|pitch|w>" (mm / centidegrees; c = cockpit place,
// s = host station with the war table; w 0 = not moved), written once a panel was moved.
bool PanelsFree();
void SetPanelsFree(bool on);
void ResetPanels();                                // every panel back to its default place
void PanelsProfileReset();                         // profile load: FIXED, no layout, nothing to save
bool PanelProfileKey(const char *key, int value);  // parser: true if `key` is a vr_panel_ key
int  PanelProfileLines(char lines[][48], int max); // the vr_panel_ lines to write (0 = none)
// the 2D row (0 = top) where the bottom panel (tool bar) starts, from the game UI
void PanelNoteSplit(int rowPx, int screenH);
// bridge (vr_bridge.cpp): the game shows the briefing / debriefing this frame
// (Command Deck puts it on the bridge monitor instead of the theatre void).
void BridgeNoteBriefing();
// station (vr_station.cpp): after the game stored the host station's 2D window
// state; windows the Command Deck closed (not the player) are stored as before.
void StationWindowsSaved(NC_STACK_ypaworld *yw);
// NC_STACK_ypaworld::DeleteLevel (every level end, restart, load): Command Deck
// state that refers to the old world (station beam / windows / built units, table carry +
// ring, captured-site tracking) is dropped before the next level's first frame.
void LevelBoundary();
// station: yparobo doBeamUpdate tick (pre-beam timer after this step) for the beam fade.
void StationBeamTick(uint32_t roboGid, int timePre);
// Dive into `gid` behind the command layer's fade (the host station gid = return
// to the station). False when the command layer cannot do it (not live, busy,
// unit not diveable); the caller then keeps the original path.
bool CommandDive(uint32_t gid, bool hostStation);
// true while a dive request would be dropped (command fade or station beam under way).
bool CommandDiveBlocked();

// Timing marks for the per-second UAVR log.
enum Mark { MARK_HUD_START = 0, MARK_END_FRAME };
void TimingMark(Mark m);

// perf: sub-phase timers, summed per frame and printed as the "sub" part of the
// per-second "UAVR fps" line (ms per frame, averaged over ALL frames incl. menus).
//   radar  sb_0x4d7c08__sub0__sub4 (HUD visor / radar / unit markers)
//   win    ProcessDrawSeq of the open GUI windows (+ postDraw)
//   mark   sub0__sub0 + sub0__sub2 (target markers, energy/status bars)
//   misc   tooltip, up_panel, sub0__sub1
//   card   VR pointer + controls card
//   gui    Gui::Root::Draw     surf  DrawScreenSurface (flush + scan + upload + quad)
//   flush  FlushFills (part of whoever triggered it: usually surf)
//   text   DrawTextLayer       comp  Gui::Root::HwCompose
//   panels Aim/Cmd::DrawPanel in Present
// Both are no-ops (HudT0 returns 0) unless a VR backend is active.
enum HudPart { HUD_RADAR = 0, HUD_WINDOWS, HUD_MARKERS, HUD_MISC, HUD_CARD, HUD_GUIROOT, HUD_SURFACE,
               HUD_FLUSH, HUD_TEXT, HUD_COMPOSE, HUD_PANELS, HUD_PARTS };
double HudT0();
void HudAdd(HudPart p, double t0);

} // namespace VR

#endif // UA_VR_H_INCLUDED
