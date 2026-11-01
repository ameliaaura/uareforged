// Urban Assault VR — Command Deck war table. See vr_table.h and
//
//
// Spaces:
//  * tracking space (XR metres, +y up, -z forward, origin = seated eye point):
//    hands, head, the table pose;
//  * board-local units: x right, y towards the far edge (map row 0), z up out of
//    the surface; the longest map side = 1. Mesh and tokens are built here, so a
//    move / turn / scale of the table is one model matrix, never a rebuild.
// Map fractions: s = x / wN + 0.5 (map +x), t = 0.5 - y / hN (map rows, = -z / lenY).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_table.h"
#include "vr_gl.h"
#include "vr_overlay.h"
#include "vr_radio.h"
#include "vr_station.h"
#include "vr_command.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"

namespace VR
{
bool Active();

namespace Table
{

namespace
{

// ---- options / constants -----------------------------------------------------------
bool g_log = false;
bool g_enabled = true;                 // UA_VR_TABLE=0: the flat board even in COMMAND DECK
// Living Diorama: the table shows the real game world in miniature (the engine draws
// the sectors, buildings, units and particles through a TableFromWorld view, see
// VR::DioramaView). UA_VR_DIORAMA=0 / profile key;#!vr_tokentable 1 = the token table.
bool g_dioEnv = true;
double g_upm = 11.0;
const double kReliefN = 0.032;         // relief range, board units (3.5 cm on a 1.1 m table)
const double kSlabN = 0.022;           // table slab thickness under the map
const double kRimN = 0.028;            // rim around the map
const double kGrabHandM = 0.10;        // a hand this close above the surface grabs it (no ray needed)
const double kCarryDiveM = 0.25;       // token this close to the head = dive
const uint32_t kSnapBackMs = 160, kBootMs = 520;

struct Pose
{
    double c[3];
    double yaw;   // radians about +y (tracking)
    double scale; // 1 = the base size
};
// Session poses (kept while the game runs; table and wall separately).
Pose g_tablePose = {{0.0, -0.55, -0.72}, 0.0, 1.0};
// The wall sits above the HUD panel (1.5 m ahead, 0.2 m low, a compositor layer that
// would cover it), its top leaning 10 degrees towards the player.
Pose g_wallPose = {{0.0, 1.05, -3.0}, 0.0, 1.0};
const double kTableM = 1.10, kTableTiltDeg = 15.0, kWallM = 2.40, kWallTiltDeg = 100.0;
// diorama: flat on the (real) table, the gameplay area ~1.1 m across at scale 1. The
// pose centre is the table surface; the terrain sits on a thin base above it.
Pose g_dioPose = {{0.0, -0.62, -0.60}, 0.0, 1.0};
const double kDioM = 1.10;
const double kDioBaseN = 0.010;   // base under the lowest terrain, board units (1.1 cm)

// ---- snapshot -------------------------------------------------------------------------
enum TokKind { TOK_SQUAD = 0, TOK_HOST, TOK_ENEMY };
struct Token
{
    uint32_t gid = 0;
    int kind = TOK_SQUAD;
    bool own = false;
    double s = 0, t = 0; // map fractions
    int count = 1;       // squad: units alive
    double z = 0;        // diorama: the unit's altitude, board units above the table
    uint8_t rgb[3] = {200, 200, 200};
};
struct TSnap
{
    bool valid = false;
    int mapW = 0, mapH = 0;
    double lenX = 1, lenY = 1;
    std::vector<float> h;      // normalised relief 0..1 per sector
    std::vector<float> hTrue;  // diorama: true terrain height above the table, board units (0 = border)
    double hTrueMax = 0;       // highest gameplay sector, board units
    uint64_t hTrueKey = 0;     // hash of hTrue (the diorama base's real input)
    double refY = 0;           // world y (down) of the table surface (board z = 0)
    int me = -1;
    int hostCx = -1, hostCy = -1; // own host station cell (diorama window centre on big maps)
    uint64_t hKey = 0;         // changes when the relief must be rebuilt
    std::vector<Token> tokens;
    std::vector<Token> rings; // diorama: every visible unit (team ring under the real model)
    uint32_t viewer = 0;
};
TSnap g_ts;

// radio events (VRRadio::RecentEvents) flash their sector on the table
// for kFlashS seconds of game time: red = attack / loss, green = taken, amber = other.
const float kFlashS = 2.5f;
struct Flash
{
    int cx = 0, cy = 0;
    int type = 0;
    float ageS = 0; // at the snapshot
};
std::vector<Flash> g_flash;
uint32_t g_flashT0 = 0; // SDL ticks of the snapshot (animation between gameplay frames)

// ---- frame (computed from the pose) ---------------------------------------------------
struct Frame
{
    double c[3], ex[3], ey[3], n[3];
    double L = 1;          // metres per board unit
    double wN = 1, hN = 1; // map extent, board units
};

bool Wall() { return MapWall(); }
// the Living Diorama replaces the token table (not the wall).
bool Dio() { return g_dioEnv && !Wall() && !TokenTable(); }

// diorama window: the sectors drawn (Quest budget, §perf:
// ~100 sectors hold 90 Hz, 64 hold 120 Hz without batching). Maps with more gameplay sectors per side show
// a kDioWin x kDioWin window around a focus sector; the right stick pans it (one sector per
// step) while the table is open. The window fills kDioM at scale 1.
// with static sector batching (yw_game.cpp DioBatching) the
// whole map fits the budget on every map (level 41 33x33 whole: ~1100 draws, 115 fps at 120 Hz
// with passthrough), so the default window is the whole map; UA_VR_DIO_BATCH=0 (the
// per-object path) keeps the budget of 10x10.
// the whole map only while the measured submissions allow it. The engine reports
// the diorama's draws per frame (DioramaReportDraws); the window shrinks to what fits
// kDioDrawBudget (bench: ~2500 draws/eye held 90 Hz with passthrough) and grows back
// with hysteresis. Fixed when UA_VR_DIO_WIN is set or batching is off.
int g_dioWinMax = 33;          // UA_VR_DIO_WIN=n (3..33)
bool g_dioWinAuto = true;
int g_dioDrawBudget = 2500;    // UA_VR_DIO_BUDGET=n
int g_dioReports = 0;
bool g_dioPoseSet = false;     // the player placed / moved it (profile lines written)
bool g_dioPoseDirty = false;   // save the placement into the profile (next gameplay frame)
uint32_t g_dioPlaceT0[2] = {0, 0};
bool g_dioPlaced[2] = {false, false};
const uint32_t kDioPlaceHoldMs = 800;
const double kCtrlRestM = 0.030; // grip pose centre above the table when a controller lies on it
int g_dioFx = -1, g_dioFy = -1; // focus sector (-1 = the own host station)
int g_dioWin[4] = {1, 1, 0, 0}; // x0, y0, x1, y1 (inclusive), updated by DioWindow()
uint32_t g_dioPanT = 0;
bool g_dioRecentre = false; // Station asked for the window on the host station (after a beam)
void DioWindow()
{
    const int W = g_ts.mapW, H = g_ts.mapH;
    if (W < 3 || H < 3)
        return;
    auto axis = [&](int n, int *focus, int host, int *a, int *b) {
        *a = 1;
        *b = n - 2;
        const int N = g_dioWinMax;
        if (*b - *a + 1 <= N)
        {
            *focus = -1;
            return;
        }
        if (*focus < 0)
            *focus = host >= 1 && host <= n - 2 ? host : n / 2;
        *a = std::max(1, std::min(n - 1 - N, *focus - N / 2));
        *b = *a + N - 1;
        *focus = std::max(*a + N / 2, std::min(*b - (N - 1) / 2, *focus)); // stays inside
    };
    axis(W, &g_dioFx, g_ts.hostCx, &g_dioWin[0], &g_dioWin[2]);
    axis(H, &g_dioFy, g_ts.hostCy, &g_dioWin[1], &g_dioWin[3]);
}
bool InDioWin(int cx, int cy) { return cx >= g_dioWin[0] && cx <= g_dioWin[2] && cy >= g_dioWin[1] && cy <= g_dioWin[3]; }
// The window's visible footprint: the engine clips the edge sectors where their
// content ends (450 of 1200 units from the sector centre) and the soil edge stands there;
// picks, occlusion, drops and tokens use the same footprint, in map fractions (s, t).
const double kDioContent = 0.375;
bool InDioFoot(double s, double t)
{
    const double x = s * g_ts.mapW, y = t * g_ts.mapH;
    return x >= g_dioWin[0] + 0.5 - kDioContent && x <= g_dioWin[2] + 0.5 + kDioContent &&
           y >= g_dioWin[1] + 0.5 - kDioContent && y <= g_dioWin[3] + 0.5 + kDioContent;
}

Frame CurFrame()
{
    Frame f;
    const Pose &p = Wall() ? g_wallPose : Dio() ? g_dioPose : g_tablePose;
    const double tilt = (Wall() ? kWallTiltDeg : Dio() ? 0.0 : kTableTiltDeg) * M_PI / 180.0;
    const double cy = cos(p.yaw), sy = sin(p.yaw);
    auto ry = [&](double x, double y, double z, double o[3]) { o[0] = x * cy + z * sy; o[1] = y; o[2] = -x * sy + z * cy; };
    ry(1, 0, 0, f.ex);
    ry(0, sin(tilt), -cos(tilt), f.ey);
    ry(0, cos(tilt), sin(tilt), f.n);
    memcpy(f.c, p.c, sizeof(f.c));
    f.L = (Wall() ? kWallM : kTableM) * p.scale;
    const double mw = std::max(1, g_ts.mapW), mh = std::max(1, g_ts.mapH);
    const double m = std::max(mw, mh);
    if (Dio())
    {
        // the window fills kDioM at scale 1, and its centre sits at the pose centre
        const int nw = g_dioWin[2] - g_dioWin[0] + 1, nh = g_dioWin[3] - g_dioWin[1] + 1;
        f.L = kDioM * p.scale * m / std::max(1, std::max(nw, nh));
        const double xc = ((g_dioWin[0] + g_dioWin[2] + 1) * 0.5 - mw * 0.5) / m;
        const double yc = (mh * 0.5 - (g_dioWin[1] + g_dioWin[3] + 1) * 0.5) / m;
        for (int i = 0; i < 3; i++)
            f.c[i] = p.c[i] - f.L * (xc * f.ex[i] + yc * f.ey[i]);
    }
    f.wN = mw / m;
    f.hN = mh / m;
    return f;
}

void ToTracking(const Frame &f, double x, double y, double z, double out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = f.c[i] + f.L * (x * f.ex[i] + y * f.ey[i] + z * f.n[i]);
}

void ToLocal(const Frame &f, const double p[3], double out[3])
{
    const double d[3] = {p[0] - f.c[0], p[1] - f.c[1], p[2] - f.c[2]};
    out[0] = (d[0] * f.ex[0] + d[1] * f.ex[1] + d[2] * f.ex[2]) / f.L;
    out[1] = (d[0] * f.ey[0] + d[1] * f.ey[1] + d[2] * f.ey[2]) / f.L;
    out[2] = (d[0] * f.n[0] + d[1] * f.n[1] + d[2] * f.n[2]) / f.L;
}

void DirToLocal(const Frame &f, const double d[3], double out[3])
{
    out[0] = d[0] * f.ex[0] + d[1] * f.ex[1] + d[2] * f.ex[2];
    out[1] = d[0] * f.ey[0] + d[1] * f.ey[1] + d[2] * f.ey[2];
    out[2] = d[0] * f.n[0] + d[1] * f.n[1] + d[2] * f.n[2];
}

double SFromX(const Frame &f, double x) { return x / f.wN + 0.5; }
double TFromY(const Frame &f, double y) { return 0.5 - y / f.hN; }
double XFromS(const Frame &f, double s) { return (s - 0.5) * f.wN; }
double YFromT(const Frame &f, double t) { return (0.5 - t) * f.hN; }

// relief top (board units) of the sector under (s, t); 0 outside
double TopAt(double s, double t)
{
    if (!g_ts.valid || !std::isfinite(s) || !std::isfinite(t) || s < 0 || t < 0 || s >= 1 || t >= 1)
        return 0;
    const int cx = std::min(g_ts.mapW - 1, (int)(s * g_ts.mapW)), cy = std::min(g_ts.mapH - 1, (int)(t * g_ts.mapH));
    if (Dio())
        return (g_ts.hTrue.empty() || !InDioWin(cx, cy)) ? 0.0 : g_ts.hTrue[(size_t)cy * g_ts.mapW + cx];
    return g_ts.h[(size_t)cy * g_ts.mapW + cx] * kReliefN;
}

Frame CurFrame();
// diorama: the token's reference point is the real unit (its altitude); tokens outside
// the window are not on the table.
double TokZ(const Token &k) { return Dio() ? k.z : TopAt(k.s, k.t); }
bool TokOnTable(const Token &k)
{
    if (!Dio())
        return true;
    return InDioFoot(k.s, k.t);
}

// the diorama pick volume is a short column around the real model, from
// one pick radius below it (not the ground under a flyer) up to the top TokenSize gives.
// *z0 = the ground on entry, *hgt = TokenSize's height; both rewritten in place.
void DioPickSpan(const Token &k, double r, double *z0, double *hgt)
{
    if (!Dio())
        return;
    const double top = *z0 + *hgt;
    *z0 = std::max(*z0, k.z - r);
    *hgt = std::max(0.0, top - *z0);
}

// token geometry (board units): radius, height of the pickable column
void TokenSize(const Token &k, double *r, double *hgt)
{
    if (Dio())
    {
        // pick volume around the real model: ~1.3 cm (host station 2.5 cm), from the
        // ground up to just above the unit (flyers are picked in the air or from below)
        const double L = std::max(1e-6, CurFrame().L);
        *r = (k.kind == TOK_HOST ? 0.025 : 0.013) / L;
        *hgt = std::max(0.0, k.z - TopAt(k.s, k.t)) + *r;
        return;
    }
    switch (k.kind)
    {
    case TOK_HOST: *r = 0.020; *hgt = 0.050; break;
    case TOK_ENEMY: *r = 0.010; *hgt = 0.018; break;
    default: *r = 0.014; *hgt = 0.013 + std::min(10, k.count) * 0.0055; break;
    }
    if (Wall()) // read from ~3 m: bigger pieces (pick columns follow)
        *r *= 1.6;
}

// ---- interaction state ---------------------------------------------------------------
enum Mode { M_IDLE = 0, M_GRAB1, M_GRAB2, M_CARRY };
Mode g_mode = M_IDLE;
int g_hand = -1;               // GRAB1 / CARRY hand
bool g_gripDown[2] = {false, false};
bool g_owned[2] = {false, false};
Pose g_pose0;                  // pose at the grab start
double g_hand0[2][3];          // hand positions at the grab start
double g_handYaw0 = 0, g_pairYaw0 = 0, g_pairDist0 = 1;
uint32_t g_carryGid = 0;
bool g_carryHost = false;             // station: the carried token is the own host station
double g_carryLocal[3] = {0, 0, 0};   // carried token, board-local
// station: the own host token was put down on a sector -> confirm ring there; a
// trigger with the ray on that sector confirms (the original beam), elsewhere cancels.
int g_beamCx = -1, g_beamCy = -1;
uint32_t g_beamT = 0;
bool g_beamConfirmed = false;
bool g_trigPrev[2] = {false, false};  // trigger above the claim level last poll
bool g_ringSqueeze[2] = {false, false}; // this squeeze belongs to the confirm ring
bool g_ringFired[2] = {false, false};   // ... and already confirmed / cancelled
const float kRingClaim = 0.2f;          // below the weapon threshold (winp.cpp 0.25)
const uint32_t kBeamPendingMs = 12000;
const double kDropAboveM = 0.20;      // token released within this height above the relief = put down
double g_carryPrevDist = 1e9;
uint32_t g_snapGid = 0, g_snapT0 = 0; // token snapping back
double g_snapFrom[3] = {0, 0, 0};
double g_head[3] = {0, 0, 0};
bool g_hasHead = false;
uint32_t g_hoverTokGid = 0;
int g_hoverTokKind = 0;
// boot flash
bool g_bootPending = false, g_bootSawBlack = false;
uint32_t g_bootReqT = 0, g_bootT0 = 0;

// ---- GL ---------------------------------------------------------------------------------
#ifdef UA_GLES
const char *kHdr = "#version 300 es\nprecision highp float;\n";
#else
const char *kHdr = "#version 410 core\n";
#endif
const char *kVs =
    "uniform mat4 MVP;\n"
    "in vec3 aPos;\n"
    "in vec4 aCol;\n"
    "in vec2 aUV;\n"
    "out vec4 vCol;\n"
    "out vec2 vUV;\n"
    "void main() { vCol = aCol; vUV = aUV; gl_Position = MVP * vec4(aPos, 1.0); }\n";
const char *kFs =
    "uniform sampler2D tex;\n"
    "in vec4 vCol;\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  if (vUV.x < -0.5) { frag = vec4(vCol.rgb, 1.0); return; }\n"
    "  frag = vec4(texture(tex, vUV).rgb * vCol.rgb, 1.0);\n"
    "}\n";

struct TV
{
    float p[3];
    uint8_t c[4];
    float uv[2];
};
typedef std::vector<TV> Mesh;

bool g_glTried = false, g_glOk = false;
GLuint g_prog = 0, g_vaoS = 0, g_vboS = 0, g_vaoD = 0, g_vboD = 0, g_white = 0;
GLint g_locMVP = -1, g_locTex = -1;
Mesh g_static, g_dyn;
uint64_t g_staticKey = ~0ull;
int g_staticCount = 0, g_dynCount = 0;

GLuint Compile(GLenum type, const char *body)
{
    std::string src = std::string(kHdr) + body;
    const char *p = src.c_str();
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &p, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        gl.GetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UAVR table: shader error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

void Attribs()
{
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TV), (void *)offsetof(TV, p));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TV), (void *)offsetof(TV, c));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(TV), (void *)offsetof(TV, uv));
}

bool InitGL()
{
    g_glTried = true;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
    {
        if (vs) gl.DeleteShader(vs);
        if (fs) gl.DeleteShader(fs);
        return false;
    }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.BindAttribLocation(p, 0, "aPos");
    gl.BindAttribLocation(p, 1, "aCol");
    gl.BindAttribLocation(p, 2, "aUV");
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "UAVR table: link failed\n");
        gl.DeleteProgram(p);
        return false;
    }
    g_prog = p;
    g_locMVP = gl.GetUniformLocation(p, "MVP");
    g_locTex = gl.GetUniformLocation(p, "tex");
    GLint prevVao = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    for (int k = 0; k < 2; k++)
    {
        GLuint &vao = k ? g_vaoD : g_vaoS, &vbo = k ? g_vboD : g_vboS;
        gl.GenVertexArrays(1, &vao);
        gl.GenBuffers(1, &vbo);
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        Attribs();
    }
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    fprintf(stderr, "UAVR table: GL ready\n");
    return true;
}

void EnsureWhite()
{
    if (g_white)
        return;
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    const uint8_t px[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint prevAlign = 4; // restore the caller's unpack state
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, prevTex);
}

// ---- mesh builders (board units) ----------------------------------------------------
const double kLight[3] = {-0.35, 0.45, 0.82}; // towards the light, board-local (normalised below)

uint8_t Cl(double v) { return (uint8_t)std::max(0.0, std::min(255.0, v)); }

void PushV(Mesh &m, double x, double y, double z, const double rgb[3], double shade, float u = -1, float v = -1)
{
    TV t;
    t.p[0] = (float)x; t.p[1] = (float)y; t.p[2] = (float)z;
    t.c[0] = Cl(rgb[0] * shade); t.c[1] = Cl(rgb[1] * shade); t.c[2] = Cl(rgb[2] * shade); t.c[3] = 255;
    t.uv[0] = u; t.uv[1] = v;
    m.push_back(t);
}

double Shade(double nx, double ny, double nz)
{
    static double l[3] = {0, 0, 0};
    if (l[0] == 0)
    {
        const double n = sqrt(kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
        for (int i = 0; i < 3; i++) l[i] = kLight[i] / n;
    }
    const double len = std::max(1e-9, sqrt(nx * nx + ny * ny + nz * nz));
    const double d = (nx * l[0] + ny * l[1] + nz * l[2]) / len;
    return 0.50 + 0.50 * std::max(0.0, d);
}

// quad a b c d (counter-clockwise seen from outside); flat shade from its normal
void QuadV(Mesh &m, const double a[3], const double b[3], const double c[3], const double d[3], const double rgb[3], bool lit = true)
{
    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    const double sh = lit ? Shade(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]) : 1.0;
    const double *q[6] = {a, b, c, a, c, d};
    for (int i = 0; i < 6; i++)
        PushV(m, q[i][0], q[i][1], q[i][2], rgb, sh);
}

// box from (x0,y0,z0) to (x1,y1,z1), five faces (no bottom)
void BoxV(Mesh &m, double x0, double y0, double z0, double x1, double y1, double z1, const double rgb[3])
{
    const double p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    QuadV(m, p[4], p[5], p[6], p[7], rgb); // top
    QuadV(m, p[0], p[1], p[5], p[4], rgb); // near (-y)
    QuadV(m, p[1], p[2], p[6], p[5], rgb); // +x
    QuadV(m, p[2], p[3], p[7], p[6], rgb); // far (+y)
    QuadV(m, p[3], p[0], p[4], p[7], rgb); // -x
}

// n-sided prism (vertical, board units) with a top cap
void PrismV(Mesh &m, double cx, double cy, double z0, double z1, double r, int sides, const double rgb[3], double topShade = 1.15, double phase = 0)
{
    for (int i = 0; i < sides; i++)
    {
        const double a0 = phase + 2 * M_PI * i / sides, a1 = phase + 2 * M_PI * (i + 1) / sides;
        const double p0[3] = {cx + r * cos(a0), cy + r * sin(a0), z0}, p1[3] = {cx + r * cos(a1), cy + r * sin(a1), z0};
        const double p2[3] = {p1[0], p1[1], z1}, p3[3] = {p0[0], p0[1], z1};
        QuadV(m, p0, p1, p2, p3, rgb);
        const double sh = Shade(0, 0, 1) * topShade;
        PushV(m, cx, cy, z1, rgb, sh);
        PushV(m, p3[0], p3[1], z1, rgb, sh);
        PushV(m, p2[0], p2[1], z1, rgb, sh);
    }
}

// four-sided pyramid
void PyramidV(Mesh &m, double cx, double cy, double z0, double hw, double hgt, const double rgb[3])
{
    const double apex[3] = {cx, cy, z0 + hgt};
    const double b[4][3] = {{cx - hw, cy - hw, z0}, {cx + hw, cy - hw, z0}, {cx + hw, cy + hw, z0}, {cx - hw, cy + hw, z0}};
    for (int i = 0; i < 4; i++)
    {
        const double *p0 = b[i], *p1 = b[(i + 1) % 4];
        const double u[3] = {p1[0] - p0[0], p1[1] - p0[1], 0}, v[3] = {apex[0] - p0[0], apex[1] - p0[1], apex[2] - p0[2]};
        const double sh = Shade(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]);
        PushV(m, p0[0], p0[1], p0[2], rgb, sh);
        PushV(m, p1[0], p1[1], p1[2], rgb, sh);
        PushV(m, apex[0], apex[1], apex[2], rgb, sh);
    }
}

// flat ring (annulus) at height z
void RingV(Mesh &m, double cx, double cy, double z, double r0, double r1, const double rgb[3], int seg = 16)
{
    for (int i = 0; i < seg; i++)
    {
        const double a0 = 2 * M_PI * i / seg, a1 = 2 * M_PI * (i + 1) / seg;
        const double p0[3] = {cx + r0 * cos(a0), cy + r0 * sin(a0), z}, p1[3] = {cx + r1 * cos(a0), cy + r1 * sin(a0), z};
        const double p2[3] = {cx + r1 * cos(a1), cy + r1 * sin(a1), z}, p3[3] = {cx + r0 * cos(a1), cy + r0 * sin(a1), z};
        QuadV(m, p0, p1, p2, p3, rgb, false);
    }
}

// Relief: one flat top per sector (textured with the board image), walls where
// neighbours differ, the outer walls down to the slab, the slab and its rim.
// diorama: the miniature's base. The engine draws the real terrain on top; this adds
// the cut edge (a soil cross-section skirt from each edge sector's height down to the
// table) and a thin gunmetal plinth, so it reads as a model standing on the table.
// The window's slab footprint, board-local (gameplay content reaches 450 of 1200 units
// from a sector centre): shared by the base mesh and the near-hand grab.
void DioBounds(const Frame &f, double *xa, double *xb, double *ya, double *yb)
{
    const int W = std::max(1, g_ts.mapW), H = std::max(1, g_ts.mapH);
    const double cw = f.wN / W, ch = f.hN / H;
    *xa = -f.wN / 2 + (g_dioWin[0] + 0.5 - kDioContent) * cw;
    *xb = -f.wN / 2 + (g_dioWin[2] + 0.5 + kDioContent) * cw;
    *yb = f.hN / 2 - (g_dioWin[1] + 0.5 - kDioContent) * ch; // first row far
    *ya = f.hN / 2 - (g_dioWin[3] + 0.5 + kDioContent) * ch; // last row near
}

void BuildDioBase(const Frame &f)
{
    g_static.clear();
    const int W = g_ts.mapW, H = g_ts.mapH;
    if (W < 3 || H < 3)
    {
        g_staticCount = 0;
        return;
    }
    const double cw = f.wN / W, ch = f.hN / H;
    auto hz = [&](int cx, int cy) { return (double)g_ts.hTrue[(size_t)cy * W + cx]; };
    // gameplay content: sector meshes reach 450 of 1200 units from the sector centre
    const double cxw = kDioContent;
    const int wx0 = g_dioWin[0], wy0 = g_dioWin[1], wx1 = g_dioWin[2], wy1 = g_dioWin[3];
    double xa, xb, ya, yb;
    DioBounds(f, &xa, &xb, &ya, &yb);
    const double soil[3] = {92, 80, 64}, soilDark[3] = {70, 61, 50};
    // one side: polyline along the edge, quads down to the plinth top
    const double zPl = kDioBaseN * 0.45;
    auto side = [&](bool alongX, double fixed, int fixedCell, bool flip)
    {
        std::vector<double> pt, ht;
        const int i0 = alongX ? wx0 : wy0, i1 = alongX ? wx1 : wy1;
        for (int i = i0; i <= i1; i++)
        {
            const double c = alongX ? (-f.wN / 2 + (i + 0.5) * cw) : (f.hN / 2 - (i + 0.5) * ch);
            const double half = cxw * (alongX ? cw : ch);
            const double h = alongX ? hz(i, fixedCell) : hz(fixedCell, i);
            const double dirSign = alongX ? 1.0 : -1.0; // y decreases with the row
            pt.push_back(c - half * dirSign);
            ht.push_back(h);
            pt.push_back(c + half * dirSign);
            ht.push_back(h);
        }
        for (size_t k = 0; k + 1 < pt.size(); k++)
        {
            double a[3], b[3], c2[3], d[3];
            if (alongX)
            {
                a[0] = pt[k]; a[1] = fixed; a[2] = zPl;
                b[0] = pt[k + 1]; b[1] = fixed; b[2] = zPl;
                c2[0] = pt[k + 1]; c2[1] = fixed; c2[2] = ht[k + 1];
                d[0] = pt[k]; d[1] = fixed; d[2] = ht[k];
            }
            else
            {
                a[0] = fixed; a[1] = pt[k]; a[2] = zPl;
                b[0] = fixed; b[1] = pt[k + 1]; b[2] = zPl;
                c2[0] = fixed; c2[1] = pt[k + 1]; c2[2] = ht[k + 1];
                d[0] = fixed; d[1] = pt[k]; d[2] = ht[k];
            }
            if (flip)
                QuadV(g_static, b, a, d, c2, (k & 1) ? soilDark : soil);
            else
                QuadV(g_static, a, b, c2, d, (k & 1) ? soilDark : soil);
        }
    };
    side(true, ya, wy1, false);  // near edge, faces -y
    side(true, yb, wy0, true);   // far edge, faces +y
    side(false, xa, wx0, true);  // left edge, faces -x
    side(false, xb, wx1, false); // right edge, faces +x
    // plinth: a thin gunmetal slab with a lip, under the cut terrain
    const double frame[3] = {48, 52, 50}, lip[3] = {78, 82, 78};
    const double r = 0.010;
    BoxV(g_static, xa - r, ya - r, 0.0, xb + r, yb + r, zPl, frame);
    BoxV(g_static, xa - r - 0.003, ya - r - 0.003, 0.0, xb + r + 0.003, yb + r + 0.003, zPl * 0.35, lip);
    // floor under the terrain (closes any seam between sectors seen from above)
    const double p[4][3] = {{xa, ya, zPl + 1e-4}, {xb, ya, zPl + 1e-4}, {xb, yb, zPl + 1e-4}, {xa, yb, zPl + 1e-4}};
    QuadV(g_static, p[0], p[1], p[2], p[3], soilDark);
    g_staticCount = (int)g_static.size();
}

void BuildStatic(const Frame &f)
{
    if (Dio())
    {
        BuildDioBase(f);
        return;
    }
    g_static.clear();
    const int W = g_ts.mapW, H = g_ts.mapH;
    const double cw = f.wN / W, ch = f.hN / H;
    const double white[3] = {255, 255, 255};
    auto hz = [&](int cx, int cy) { return (cx < 0 || cy < 0 || cx >= W || cy >= H) ? 0.0 : g_ts.h[(size_t)cy * W + cx] * kReliefN; };
    for (int cy = 0; cy < H; cy++)
        for (int cx = 0; cx < W; cx++)
        {
            const double x0 = -f.wN / 2 + cx * cw, x1 = x0 + cw;
            const double y1 = f.hN / 2 - cy * ch, y0 = y1 - ch; // row 0 at the far (+y) edge
            const double z = hz(cx, cy);
            const float u0 = (float)cx / W, u1 = (float)(cx + 1) / W, v0 = (float)cy / H, v1 = (float)(cy + 1) / H;
            // top, textured (v grows towards -y)
            const double sh = 1.0;
            PushV(g_static, x0, y0, z, white, sh, u0, v1);
            PushV(g_static, x1, y0, z, white, sh, u1, v1);
            PushV(g_static, x1, y1, z, white, sh, u1, v0);
            PushV(g_static, x0, y0, z, white, sh, u0, v1);
            PushV(g_static, x1, y1, z, white, sh, u1, v0);
            PushV(g_static, x0, y1, z, white, sh, u0, v0);
            // walls towards lower neighbours (the outside counts as 0), textured with
            // this sector's colour (a strip of its texel row) and darkened
            const float um = (u0 + u1) * 0.5f, vm = (v0 + v1) * 0.5f;
            auto wall = [&](double ax, double ay, double bx, double by, double zl, double nx, double ny)
            {
                if (z - zl < 1e-6)
                    return;
                const double s2 = 0.45 + 0.35 * std::max(0.0, nx * -0.35 + ny * 0.45 + 0.5);
                PushV(g_static, ax, ay, zl, white, s2 * 0.8, um, vm);
                PushV(g_static, bx, by, zl, white, s2 * 0.8, um, vm);
                PushV(g_static, bx, by, z, white, s2, um, vm);
                PushV(g_static, ax, ay, zl, white, s2 * 0.8, um, vm);
                PushV(g_static, bx, by, z, white, s2, um, vm);
                PushV(g_static, ax, ay, z, white, s2, um, vm);
            };
            wall(x0, y0, x1, y0, hz(cx, cy + 1), 0, -1); // near side
            wall(x1, y1, x0, y1, hz(cx, cy - 1), 0, 1);  // far side
            wall(x1, y0, x1, y1, hz(cx + 1, cy), 1, 0);
            wall(x0, y1, x0, y0, hz(cx - 1, cy), -1, 0);
        }
    // slab + rim: the map sits in a gunmetal frame
    const double frame[3] = {52, 56, 54}, lip[3] = {88, 94, 88};
    const double ex = f.wN / 2, ey = f.hN / 2, r = kRimN, zt = 0.004;
    BoxV(g_static, -ex - r, -ey - r, -kSlabN, ex + r, -ey, zt, lip);    // near rim
    BoxV(g_static, -ex - r, ey, -kSlabN, ex + r, ey + r, zt, lip);      // far rim
    BoxV(g_static, -ex - r, -ey, -kSlabN, -ex, ey, zt, lip);            // left rim
    BoxV(g_static, ex, -ey, -kSlabN, ex + r, ey, zt, lip);              // right rim
    const double p[4][3] = {{-ex, -ey, -kSlabN * 0.5}, {ex, -ey, -kSlabN * 0.5}, {ex, ey, -kSlabN * 0.5}, {-ex, ey, -kSlabN * 0.5}};
    QuadV(g_static, p[0], p[1], p[2], p[3], frame); // floor under the relief (seen through no gaps; cheap)
    // skirt under the slab (the table's body), slightly inset
    const double sk = 0.06;
    BoxV(g_static, -ex - r + 0.01, -ey - r + 0.01, -kSlabN - sk, ex + r - 0.01, ey + r - 0.01, -kSlabN, frame);
    g_staticCount = (int)g_static.size();
}

// Tokens + hover / selection marks, rebuilt every frame (a few hundred triangles).
void BuildDynamic(const Frame &f, int hoverKind, uint32_t hoverGid, bool hit, double hs, double ht, uint32_t selCmdr, uint32_t now)
{
    g_dyn.clear();
    if (!g_ts.valid)
        return;
    const double W = g_ts.mapW, H = g_ts.mapH;
    if (Dio())
    {
        // the real models stand on the table; only a thin team ring on the ground under
        // each (subtle ownership cue), the hovered one brighter, the selected squad white.
        const double lift = 0.0015 / std::max(1e-6, f.L);
        const double base = 0.0075 / std::max(1e-6, f.L);
        for (const Token &k : g_ts.rings)
        {
            if (!TokOnTable(k) || (g_mode == M_CARRY && k.gid == g_carryGid))
                continue;
            const double x = XFromS(f, k.s), y = YFromT(f, k.t), z = TopAt(k.s, k.t) + lift;
            const double rr = base * (k.kind == TOK_HOST ? 2.6 : 1.0);
            const double tint[3] = {k.rgb[0] * 0.55 + 20, k.rgb[1] * 0.55 + 20, k.rgb[2] * 0.55 + 20};
            RingV(g_dyn, x, y, z, rr * 0.82, rr, tint, 14);
        }
        for (const Token &k : g_ts.tokens)
        {
            if (!TokOnTable(k) || (g_mode == M_CARRY && k.gid == g_carryGid))
                continue;
            const double x = XFromS(f, k.s), y = YFromT(f, k.t), z = TopAt(k.s, k.t) + lift * 1.5;
            const double rr = base * (k.kind == TOK_HOST ? 2.6 : 1.0);
            const bool hov = hoverGid && k.gid == hoverGid && (hoverKind == 1 || hoverKind == 2);
            if (k.gid == selCmdr)
            {
                const double w[3] = {235, 235, 225};
                RingV(g_dyn, x, y, z, rr * 1.35, rr * 1.6, w, 20);
            }
            if (hov)
            {
                const double c1[3] = {255, 255, 255}, c2[3] = {255, 90, 60};
                const double pulse = 0.5 + 0.5 * cos(now * 0.012);
                RingV(g_dyn, x, y, z, rr * (1.75 + 0.15 * pulse), rr * (2.05 + 0.15 * pulse), hoverKind == 1 ? c1 : c2, 24);
            }
        }
    }
    for (const Token &k : g_ts.tokens)
    {
        if (Dio())
            break; // the real unit models are on the table (rings above)
        if (g_mode == M_CARRY && k.gid == g_carryGid)
            continue; // drawn at the hand below
        double x = XFromS(f, k.s), y = YFromT(f, k.t), z = TopAt(k.s, k.t);
        if (k.gid == g_snapGid && now - g_snapT0 < kSnapBackMs)
        {
            const double a = (now - g_snapT0) / (double)kSnapBackMs, e = 1 - (1 - a) * (1 - a);
            x = g_snapFrom[0] + (x - g_snapFrom[0]) * e;
            y = g_snapFrom[1] + (y - g_snapFrom[1]) * e;
            z = g_snapFrom[2] + (z - g_snapFrom[2]) * e;
        }
        const bool hov = hoverGid && k.gid == hoverGid && (hoverKind == 1 || hoverKind == 2);
        const double grow = hov ? 1.3 : 1.0;
        double rgb[3] = {k.rgb[0] * 0.65 + 90.0, k.rgb[1] * 0.65 + 90.0, k.rgb[2] * 0.65 + 90.0};
        {
            const double ink[3] = {14, 16, 16};
            double rr, hh;
            TokenSize(k, &rr, &hh);
            RingV(g_dyn, x, y, z + 0.0008, rr * grow * 0.9, rr * grow * 1.25, ink, 12);
        }
        if (hov)
            for (int i = 0; i < 3; i++) rgb[i] = std::min(255.0, rgb[i] * 1.25 + 40);
        const double dark[3] = {rgb[0] * 0.35, rgb[1] * 0.35, rgb[2] * 0.35};
        double r, hgt;
        TokenSize(k, &r, &hgt);
        r *= grow;
        switch (k.kind)
        {
        case TOK_HOST:
            PrismV(g_dyn, x, y, z, z + 0.006, r * 1.25, 8, dark, 1.0, M_PI / 8);
            PrismV(g_dyn, x, y, z + 0.006, z + hgt * grow, r * 0.7, 8, rgb, 1.15, M_PI / 8);
            PrismV(g_dyn, x, y, z + hgt * grow, z + hgt * grow + 0.006, r, 8, rgb, 1.2, M_PI / 8);
            break;
        case TOK_ENEMY:
            PyramidV(g_dyn, x, y, z, r, hgt * grow, rgb);
            break;
        default:
        {
            // base puck in squad colour, then one thin slab per unit (count, capped at 10)
            const double base = 0.010 * grow;
            PrismV(g_dyn, x, y, z, z + base, r, 6, rgb, 1.15);
            double zz = z + base + 0.0012;
            const int n = std::min(10, std::max(1, k.count));
            for (int i = 0; i < n; i++)
            {
                const double sl[3] = {i % 2 ? rgb[0] * 0.75 : 235.0, i % 2 ? rgb[1] * 0.75 : 235.0, i % 2 ? rgb[2] * 0.75 : 225.0};
                PrismV(g_dyn, x, y, zz, zz + 0.0042 * grow, r * 0.62, 6, sl, 1.1);
                zz += 0.0055 * grow;
            }
            if (k.gid == selCmdr) // the selected squad: white ring on the table
            {
                const double w[3] = {255, 255, 255};
                RingV(g_dyn, x, y, z + 0.0015, r * 1.45, r * 1.85, w);
            }
            break;
        }
        }
        if (hov) // hover: a bright ring in the action colour
        {
            const double c1[3] = {255, 255, 255}, c2[3] = {255, 90, 60};
            RingV(g_dyn, x, y, z + 0.002, r * 2.0, r * 2.35, hoverKind == 1 ? c1 : c2);
        }
    }
    // the carried token at the hand (slightly bigger, it is close to the eye); diorama:
    // the engine draws the real model there instead (VR::DioramaView carry)
    if (g_mode == M_CARRY && !Dio())
        for (const Token &k : g_ts.tokens)
            if (k.gid == g_carryGid)
            {
                const double rgb[3] = {(double)k.rgb[0], (double)k.rgb[1], (double)k.rgb[2]};
                double r, hgt;
                TokenSize(k, &r, &hgt);
                const double *p = g_carryLocal;
                if (g_carryHost) // station: the host station tower at the hand
                {
                    const double dark[3] = {rgb[0] * 0.35, rgb[1] * 0.35, rgb[2] * 0.35};
                    PrismV(g_dyn, p[0], p[1], p[2] - 0.010, p[2] - 0.004, r * 1.25, 8, dark, 1.0, M_PI / 8);
                    PrismV(g_dyn, p[0], p[1], p[2] - 0.004, p[2] + hgt - 0.010, r * 0.7, 8, rgb, 1.15, M_PI / 8);
                    PrismV(g_dyn, p[0], p[1], p[2] + hgt - 0.010, p[2] + hgt - 0.004, r, 8, rgb, 1.2, M_PI / 8);
                    break;
                }
                PrismV(g_dyn, p[0], p[1], p[2] - 0.006, p[2] + 0.006, r * 1.2, 6, rgb, 1.2);
                const double w[3] = {240, 240, 230};
                PrismV(g_dyn, p[0], p[1], p[2] + 0.006, p[2] + 0.006 + std::min(10, k.count) * 0.004, r * 0.75, 6, w, 1.1);
            }
    // station: confirm ring + ghost station on the beam target sector. Green = the
    // original checks pass, red = refused (reason on the bridge), grey = checking,
    // cyan = confirmed (beaming).
    if (g_beamCx >= 0 && g_beamCx < W && g_beamCy < H)
    {
        int cost = 0;
        const int st = Station::BeamStatus(g_beamCx, g_beamCy, &cost);
        double col[3] = {150, 150, 150};
        if (g_beamConfirmed) { col[0] = 110; col[1] = 230; col[2] = 255; }
        else if (st == Station::BEAM_OK) { col[0] = 90; col[1] = 255; col[2] = 120; }
        else if (st != Station::BEAM_UNKNOWN) { col[0] = 255; col[1] = 70; col[2] = 50; }
        const double ss = (g_beamCx + 0.5) / W, tt = (g_beamCy + 0.5) / H;
        const double x = XFromS(f, ss), y = YFromT(f, tt), z = TopAt(ss, tt);
        const double cell = std::min(f.wN / W, f.hN / H);
        const double pulse = 0.5 + 0.5 * cos((now - g_beamT) * 0.0063); // ~1 Hz
        const double r0 = cell * (0.36 + 0.06 * pulse);
        RingV(g_dyn, x, y, z + 0.0025, r0, r0 + cell * 0.07, col, 24);
        RingV(g_dyn, x, y, z + 0.0025, cell * 0.16, cell * 0.20, col, 16);
        // ghost tower (smaller, in the ring colour)
        const double gr = 0.020 * 0.8, gh = 0.050 * 0.8;
        const double dim[3] = {col[0] * 0.45, col[1] * 0.45, col[2] * 0.45};
        PrismV(g_dyn, x, y, z, z + 0.005, gr * 1.25, 8, dim, 1.0, M_PI / 8);
        PrismV(g_dyn, x, y, z + 0.005, z + gh, gr * 0.7, 8, col, 1.15, M_PI / 8);
    }
    // sector hover: outline the sector in the action colour (move green, attack orange)
    if (hit && (hoverKind == 3 || hoverKind == 4))
    {
        const int cx = (int)(hs * W), cy = (int)(ht * H);
        if (cx >= 0 && cy >= 0 && cx < W && cy < H)
        {
            const double cw = f.wN / W, chh = f.hN / H;
            const double x0 = -f.wN / 2 + cx * cw, x1 = x0 + cw, y1 = f.hN / 2 - cy * chh, y0 = y1 - chh;
            const double z = TopAt(hs, ht) + 0.0015, lw = std::min(cw, chh) * 0.07;
            const double col[3] = {hoverKind == 3 ? 90.0 : 255.0, hoverKind == 3 ? 255.0 : 150.0, hoverKind == 3 ? 120.0 : 50.0};
            const double a[3] = {x0, y0, z}, b[3] = {x1, y0, z}, c[3] = {x1, y0 + lw, z}, d[3] = {x0, y0 + lw, z};
            QuadV(g_dyn, a, b, c, d, col, false);
            const double a2[3] = {x0, y1 - lw, z}, b2[3] = {x1, y1 - lw, z}, c2[3] = {x1, y1, z}, d2[3] = {x0, y1, z};
            QuadV(g_dyn, a2, b2, c2, d2, col, false);
            const double a3[3] = {x0, y0, z}, b3[3] = {x0 + lw, y0, z}, c3[3] = {x0 + lw, y1, z}, d3[3] = {x0, y1, z};
            QuadV(g_dyn, a3, b3, c3, d3, col, false);
            const double a4[3] = {x1 - lw, y0, z}, b4[3] = {x1, y0, z}, c4[3] = {x1, y1, z}, d4[3] = {x1 - lw, y1, z};
            QuadV(g_dyn, a4, b4, c4, d4, col, false);
        }
    }
    // radio flashes: a pulsing sector frame + a fill that fades over kFlashS
    for (const Flash &fl : g_flash)
    {
        const float age = fl.ageS + (now - g_flashT0) / 1000.0f;
        if (age < 0 || age > kFlashS)
            continue;
        if (Dio() && !InDioWin(fl.cx, fl.cy))
            continue; // off the diorama window
        const double cw = f.wN / W, chh = f.hN / H;
        const double x0 = -f.wN / 2 + fl.cx * cw, x1 = x0 + cw, y1 = f.hN / 2 - fl.cy * chh, y0 = y1 - chh;
        const double ss = (fl.cx + 0.5) / W, tt = (fl.cy + 0.5) / H;
        const double z = TopAt(ss, tt) + 0.0022;
        const double fade = 1.0 - age / kFlashS;
        const double pulse = 0.5 + 0.5 * cos(age * 2 * M_PI * 2.0); // 2 Hz
        const bool green = fl.type == VRRadio::EV_SECTOR_TAKEN;
        const bool amber = fl.type == VRRadio::EV_OTHER || fl.type == VRRadio::EV_ENEMY_HOST;
        const double base[3] = {green ? 70.0 : 255.0, green ? 255.0 : amber ? 190.0 : 45.0, green ? 90.0 : amber ? 40.0 : 35.0};
        const double lw = std::min(cw, chh) * (0.08 + 0.10 * pulse * fade);
        const double a[3] = {x0, y0, z}, b[3] = {x1, y0, z}, c[3] = {x1, y0 + lw, z}, d[3] = {x0, y0 + lw, z};
        QuadV(g_dyn, a, b, c, d, base, false);
        const double a2[3] = {x0, y1 - lw, z}, b2[3] = {x1, y1 - lw, z}, c2[3] = {x1, y1, z}, d2[3] = {x0, y1, z};
        QuadV(g_dyn, a2, b2, c2, d2, base, false);
        const double a3[3] = {x0, y0, z}, b3[3] = {x0 + lw, y0, z}, c3[3] = {x0 + lw, y1, z}, d3[3] = {x0, y1, z};
        QuadV(g_dyn, a3, b3, c3, d3, base, false);
        const double a4[3] = {x1 - lw, y0, z}, b4[3] = {x1, y0, z}, c4[3] = {x1, y1, z}, d4[3] = {x1 - lw, y1, z};
        QuadV(g_dyn, a4, b4, c4, d4, base, false);
        // inner fill: shrinks with age (opaque mesh, so the fade is the fill's size)
        const double inset = 0.5 - 0.42 * fade * (0.6 + 0.4 * pulse);
        const double ix0 = x0 + cw * inset, ix1 = x1 - cw * inset, iy0 = y0 + chh * inset, iy1 = y1 - chh * inset;
        if (ix1 > ix0 && iy1 > iy0)
        {
            const double fillc[3] = {base[0] * 0.8, base[1] * 0.8, base[2] * 0.8};
            const double e1[3] = {ix0, iy0, z - 0.0004}, e2[3] = {ix1, iy0, z - 0.0004}, e3[3] = {ix1, iy1, z - 0.0004}, e4[3] = {ix0, iy1, z - 0.0004};
            QuadV(g_dyn, e1, e2, e3, e4, fillc, false);
        }
    }
    g_dynCount = (int)g_dyn.size();
}

void UploadMVP(const M4 &mvp)
{
    float m[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[j * 4 + i] = (float)mvp.m[i][j];
    gl.UniformMatrix4fv(g_locMVP, 1, GL_FALSE, m);
}

// projection * eye-from-tracking * tracking-from-board (eye space in metres, UAV axes)
M4 EyeMVP(const Frame &f, const ViewState &vs, int eye, double zn = 0.05, double zf = 30.0)
{
    const M3 rh = QuatToM3(vs.head.orientation);
    const M3 re = rh * QuatToM3(vs.eye[eye].pose.orientation);
    const double ep[3] = {vs.eye[eye].pose.position.x, vs.eye[eye].pose.position.y, vs.eye[eye].pose.position.z};
    double off[3];
    rh.Apply(ep, off);
    const double eyePos[3] = {vs.head.position.x + off[0], vs.head.position.y + off[1], vs.head.position.z + off[2]};
    const M3 rt = re.T();
    static const double flip[3] = {1, -1, -1};
    M4 view = M4::Ident();
    for (int i = 0; i < 3; i++)
    {
        double t = 0;
        for (int j = 0; j < 3; j++)
        {
            view.m[i][j] = flip[i] * rt.m[i][j];
            t += view.m[i][j] * eyePos[j];
        }
        view.m[i][3] = -t;
    }
    M4 model = M4::Ident();
    for (int i = 0; i < 3; i++)
    {
        model.m[i][0] = f.ex[i] * f.L;
        model.m[i][1] = f.ey[i] * f.L;
        model.m[i][2] = f.n[i] * f.L;
        model.m[i][3] = f.c[i];
    }
    return UaAsymFrustum(vs.eye[eye].fov, zn, zf) * view * model;
}

// ---- picking ----------------------------------------------------------------------------
// Ray vs relief: the plane at the current guess's sector top, a few refinements.
bool RayLocal(const Frame &f, const double o[3], const double d[3], double lo[3], double ld[3], double *s, double *t, double *along)
{
    // Exact ray vs the stepped relief: walk the sectors the ray crosses inside the
    // relief's height band (top of the band -> the base plane), in order; the first
    // sector whose top the ray reaches is hit, on its top or on its wall.
    ToLocal(f, o, lo);
    DirToLocal(f, d, ld);
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(lo[i]) || !std::isfinite(ld[i]))
            return false;
    if (ld[2] > -1e-6 || !g_ts.valid)
        return false; // only rays coming down onto the surface
    const double a1 = (0.0 - lo[2]) / ld[2];
    if (a1 <= 0)
        return false;
    const double band = Dio() ? g_ts.hTrueMax + 0.002 : kReliefN;
    const double a0 = std::max(0.0, (band - lo[2]) / ld[2]);
    const int W = g_ts.mapW, H = g_ts.mapH;
    const double cw = f.wN / W, ch = f.hN / H, x0 = -f.wN / 2, yTop = f.hN / 2;
    std::vector<double> ts;
    ts.push_back(a0);
    ts.push_back(a1);
    if (fabs(ld[0]) > 1e-12)
        for (int k = 0; k <= W; k++)
        {
            const double a = (x0 + k * cw - lo[0]) / ld[0];
            if (a > a0 && a < a1) ts.push_back(a);
        }
    if (fabs(ld[1]) > 1e-12)
        for (int k = 0; k <= H; k++)
        {
            const double a = (yTop - k * ch - lo[1]) / ld[1];
            if (a > a0 && a < a1) ts.push_back(a);
        }
    if (Dio())
    {
        // the footprint's cut edges are crossings too
        double fb[4];
        DioBounds(f, &fb[0], &fb[1], &fb[2], &fb[3]);
        for (int k = 0; k < 4; k++)
        {
            const int ax = k < 2 ? 0 : 1;
            if (fabs(ld[ax]) > 1e-12)
            {
                const double a = (fb[k] - lo[ax]) / ld[ax];
                if (a > a0 && a < a1) ts.push_back(a);
            }
        }
    }
    std::sort(ts.begin(), ts.end());
    double hitA = a1;
    int hcx = -1, hcy = -1;
    for (size_t k = 0; k + 1 < ts.size(); k++)
    {
        const double ta = ts[k], tb = ts[k + 1];
        if (tb - ta < 1e-12)
            continue;
        const double tm = (ta + tb) * 0.5;
        const int cx = (int)floor((lo[0] + ld[0] * tm - x0) / cw), cy = (int)floor((yTop - (lo[1] + ld[1] * tm)) / ch);
        const bool in = cx >= 0 && cy >= 0 && cx < W && cy < H;
        const bool foot = !Dio() || InDioFoot(SFromX(f, lo[0] + ld[0] * tm), TFromY(f, lo[1] + ld[1] * tm));
        const double h = (!in || !foot) ? 0.0 : Dio() ? ((g_ts.hTrue.empty() || !InDioWin(cx, cy)) ? 0.0 : g_ts.hTrue[(size_t)cy * W + cx]) : g_ts.h[(size_t)cy * W + cx] * kReliefN;
        const double za = lo[2] + ld[2] * ta, zb = lo[2] + ld[2] * tb;
        if (za <= h + 1e-9) { hitA = ta; hcx = cx; hcy = cy; break; }           // its wall
        if (zb <= h + 1e-9) { hitA = (h - lo[2]) / ld[2]; hcx = cx; hcy = cy; break; } // its top
    }
    double x = lo[0] + ld[0] * hitA, y = lo[1] + ld[1] * hitA;
    if (hcx >= 0 && hcy >= 0 && hcx < W && hcy < H)
    {
        // keep the point inside the hit sector (a wall hit lies on its border)
        x = std::max(x0 + hcx * cw + 1e-7, std::min(x0 + (hcx + 1) * cw - 1e-7, x));
        y = std::max(yTop - (hcy + 1) * ch + 1e-7, std::min(yTop - hcy * ch - 1e-7, y));
    }
    *s = SFromX(f, x);
    *t = TFromY(f, y);
    *along = hitA;
    if (Dio() && (!InDioWin((int)floor(*s * W), (int)floor(*t * H)) || !InDioFoot(*s, *t)))
        return false; // only the window's footprint is on the table
    return *s >= 0 && *s <= 1 && *t >= 0 && *t <= 1;
}

// the first point along a ray of any direction (board-local) that enters
// the miniature's solid (each window sector = a column from the table, z 0, up to its true
// height). Lets diorama picks aim up / sideways at flyers while terrain and the slab still
// occlude. 1e18 = nothing in the way.
double DioSolidAlong(const Frame &f, const double lo[3], const double ld[3])
{
    const int W = g_ts.mapW, H = g_ts.mapH;
    if (!g_ts.valid || W < 1 || H < 1 || g_ts.hTrue.empty())
        return 1e18;
    double xa, xb, ya, yb;
    DioBounds(f, &xa, &xb, &ya, &yb); // only the footprint is solid (the cut edge)
    const double lob[3] = {xa, ya, 0.0}, hib[3] = {xb, yb, g_ts.hTrueMax + 1e-4};
    double aIn = 0, aOut = 1e18;
    for (int i = 0; i < 3; i++)
    {
        if (fabs(ld[i]) < 1e-12)
        {
            if (lo[i] < lob[i] || lo[i] > hib[i])
                return 1e18;
            continue;
        }
        double a0 = (lob[i] - lo[i]) / ld[i], a1 = (hib[i] - lo[i]) / ld[i];
        if (a0 > a1) std::swap(a0, a1);
        aIn = std::max(aIn, a0);
        aOut = std::min(aOut, a1);
    }
    if (aOut <= aIn)
        return 1e18;
    const double cw = f.wN / W, ch = f.hN / H, x0 = -f.wN / 2, yTop = f.hN / 2;
    std::vector<double> ts{aIn, aOut};
    if (fabs(ld[0]) > 1e-12)
        for (int k = 0; k <= W; k++)
        {
            const double a = (x0 + k * cw - lo[0]) / ld[0];
            if (a > aIn && a < aOut) ts.push_back(a);
        }
    if (fabs(ld[1]) > 1e-12)
        for (int k = 0; k <= H; k++)
        {
            const double a = (yTop - k * ch - lo[1]) / ld[1];
            if (a > aIn && a < aOut) ts.push_back(a);
        }
    std::sort(ts.begin(), ts.end());
    for (size_t k = 0; k + 1 < ts.size(); k++)
    {
        const double ta = ts[k], tb = ts[k + 1];
        if (tb - ta < 1e-12)
            continue;
        const double tm = (ta + tb) * 0.5;
        const int cx = (int)floor((lo[0] + ld[0] * tm - x0) / cw), cy = (int)floor((yTop - (lo[1] + ld[1] * tm)) / ch);
        if (cx < 0 || cy < 0 || cx >= W || cy >= H || !InDioWin(cx, cy))
            continue;
        const double h = g_ts.hTrue[(size_t)cy * W + cx];
        // the part of [ta, tb] where z is inside [0, h]
        double a0 = ta, a1 = tb;
        if (fabs(ld[2]) > 1e-12)
        {
            double za = (0.0 - lo[2]) / ld[2], zb = (h - lo[2]) / ld[2];
            if (za > zb) std::swap(za, zb);
            a0 = std::max(a0, za);
            a1 = std::min(a1, zb);
        }
        else if (lo[2] < 0.0 || lo[2] > h)
            continue;
        if (a1 >= a0)
            return a0;
    }
    return 1e18;
}

// Token pick for a ray of any direction: the token table and the diorama on rays
// coming down use the relief hit (maxAlong); the diorama also takes rays going up or
// sideways (flyers), occluded by the miniature's solid.
int PickTokenAny(const Frame &f, const double lo[3], const double ld[3], uint32_t *gid, bool ownOnly, double maxAlong);

// nearest token column the ray passes through (board units); kind 0 = none
// maxAlong: the relief hit along the same ray (a token behind raised terrain is hidden).
int PickTokenLocal(const Frame &f, const double lo[3], const double ld[3], uint32_t *gid, bool ownOnly, double maxAlong)
{
    double best = 1e18;
    int kind = 0;
    *gid = 0;
    for (const Token &k : g_ts.tokens)
    {
        if (k.own && k.kind == TOK_HOST)
            continue; // the own host station is not an order target
        if (ownOnly && !(k.own && k.kind == TOK_SQUAD))
            continue;
        if (!TokOnTable(k))
            continue;
        double r, hgt;
        TokenSize(k, &r, &hgt);
        r = std::max(r * 1.35, 0.009);
        const double cx = XFromS(f, k.s), cy = YFromT(f, k.t);
        double z0 = TopAt(k.s, k.t);
        DioPickSpan(k, r, &z0, &hgt);
        // 2D ray-circle in x,y, then the z range
        const double ox = lo[0] - cx, oy = lo[1] - cy;
        const double A = ld[0] * ld[0] + ld[1] * ld[1], B = 2 * (ox * ld[0] + oy * ld[1]), C = ox * ox + oy * oy - r * r;
        double tEnter = -1;
        if (A < 1e-12)
        {
            if (C <= 0)
                tEnter = (z0 + hgt - lo[2]) / ld[2];
        }
        else
        {
            const double disc = B * B - 4 * A * C;
            if (disc < 0)
                continue;
            const double sq = sqrt(disc), t0 = (-B - sq) / (2 * A), t1 = (-B + sq) / (2 * A);
            // the part of [t0, t1] where z is inside the column
            double a0 = t0, a1 = t1;
            if (fabs(ld[2]) > 1e-12)
            {
                double za = (z0 - lo[2]) / ld[2], zb = (z0 + hgt - lo[2]) / ld[2];
                if (za > zb) std::swap(za, zb);
                a0 = std::max(a0, za);
                a1 = std::min(a1, zb);
            }
            else if (lo[2] < z0 || lo[2] > z0 + hgt)
                continue; // parallel to the board, above / below the column
            if (a1 < a0 || a1 <= 0)
                continue;
            tEnter = std::max(a0, 0.0);
        }
        if (tEnter >= 0 && tEnter < best && tEnter <= maxAlong + 1e-9)
        {
            best = tEnter;
            *gid = k.gid;
            kind = k.own ? 1 : 2;
        }
    }
    return kind;
}

int PickTokenAny(const Frame &f, const double lo[3], const double ld[3], uint32_t *gid, bool ownOnly, double maxAlong)
{
    *gid = 0;
    if (ld[2] < -1e-6)
        return PickTokenLocal(f, lo, ld, gid, ownOnly, maxAlong);
    if (!Dio())
        return 0; // token table: from below the slab hides them
    return PickTokenLocal(f, lo, ld, gid, ownOnly, DioSolidAlong(f, lo, ld));
}

const Token *FindTok(uint32_t gid)
{
    for (const Token &k : g_ts.tokens)
        if (k.gid == gid)
            return &k;
    return NULL;
}

// station: the own host station token (the viewer must be that station), picked by
// the ray through its column or a hand within 5 cm of it. 0 = none.
uint32_t PickOwnHost(const Frame &f, const double lo[3], const double ld[3], double maxAlong, const double hl[3], bool near)
{
    if (!Station::On())
        return 0;
    for (const Token &k : g_ts.tokens)
    {
        if (!(k.own && k.kind == TOK_HOST) || k.gid != g_ts.viewer || !TokOnTable(k))
            continue;
        double r, hgt;
        TokenSize(k, &r, &hgt);
        const double cx = XFromS(f, k.s), cy = YFromT(f, k.t);
        double z0 = TopAt(k.s, k.t);
        DioPickSpan(k, std::max(r * 1.35, 0.009), &z0, &hgt);
        if (near)
        {
            const double dx = cx - hl[0], dy = cy - hl[1], dz = std::max(0.0, std::max(z0 - hl[2], hl[2] - (z0 + hgt)));
            if (sqrt(dx * dx + dy * dy) * f.L < std::max(0.05, r * 1.4 * f.L) && dz * f.L < 0.05)
                return k.gid;
        }
        if (ld[2] < -1e-6)
        {
            // ray vs the column (as PickTokenLocal)
            const double rr = std::max(r * 1.35, 0.009);
            const double ox = lo[0] - cx, oy = lo[1] - cy;
            const double A = ld[0] * ld[0] + ld[1] * ld[1], B = 2 * (ox * ld[0] + oy * ld[1]), C = ox * ox + oy * oy - rr * rr;
            if (A < 1e-12)
            {
                // straight down the column axis: inside the circle = the top
                if (C <= 0 && (z0 + hgt - lo[2]) / ld[2] >= 0 && (z0 + hgt - lo[2]) / ld[2] <= maxAlong + 1e-9)
                    return k.gid;
                continue;
            }
            const double disc = B * B - 4 * A * C;
            if (disc < 0)
                continue;
            const double sq = sqrt(disc);
            double a0 = (-B - sq) / (2 * A), a1 = (-B + sq) / (2 * A);
            double za = (z0 - lo[2]) / ld[2], zb = (z0 + hgt - lo[2]) / ld[2];
            if (za > zb) std::swap(za, zb);
            a0 = std::max(a0, za);
            a1 = std::min(a1, zb);
            if (a1 >= a0 && a1 > 0 && std::max(a0, 0.0) <= maxAlong + 1e-9)
                return k.gid;
        }
    }
    return 0;
}

double HandYaw(const ControllerState &c)
{
    const M3 m = QuatToM3(c.gripPose.orientation);
    return atan2(m.m[0][2], m.m[2][2]); // forward = -Z column: atan2(-fx, -fz)
}

void HandPos(const ControllerState &c, double p[3])
{
    p[0] = c.gripPose.position.x;
    p[1] = c.gripPose.position.y;
    p[2] = c.gripPose.position.z;
}

void AimRay(const ControllerState &c, double o[3], double d[3])
{
    const M3 m = QuatToM3(c.aimPose.orientation);
    o[0] = c.aimPose.position.x; o[1] = c.aimPose.position.y; o[2] = c.aimPose.position.z;
    d[0] = -m.m[0][2]; d[1] = -m.m[1][2]; d[2] = -m.m[2][2];
}

// A trigger squeezed while a hand grabs / carries stays swallowed until it is let
// go, even after the grab ends, the table closes or the dive happens: it never
// turns into a weapon shot or a board click.
bool g_trigLatch[2] = {false, false};
void LatchTriggers(ControllerState c[2])
{
    for (int h = 0; h < 2; h++)
    {
        if (c[h].trigger < 0.2f)
            g_trigLatch[h] = false;
        else if (g_owned[h] && (g_mode != M_IDLE || g_bootPending))
            g_trigLatch[h] = true;
        if (g_trigLatch[h] || (g_owned[h] && g_mode != M_IDLE))
            c[h].trigger = 0;
    }
}

// The table owns a hand this poll (grab, carry, a grip held since a dive, or a
// latched trigger): the command layer must not turn its trigger into a board click.
bool Consumed()
{
    return g_mode != M_IDLE || g_owned[0] || g_owned[1] || g_trigLatch[0] || g_trigLatch[1];
}

void StopCarry(bool snapBack, uint32_t now)
{
    if (g_mode == M_CARRY && snapBack)
    {
        g_snapGid = g_carryGid;
        g_snapT0 = now;
        memcpy(g_snapFrom, g_carryLocal, sizeof(g_snapFrom));
        if (g_log) fprintf(stderr, "UAVR table: token %u released away from the face -> snaps back\n", g_carryGid);
    }
    g_carryGid = 0;
    g_carryHost = false;
}

// station: with an unconfirmed ring up at poll entry, every new squeeze of
// either hand belongs to the ring (claimed below the weapon threshold, swallowed until
// released), before grips / closing / timeout can remove the ring this poll.
void ClaimRingSqueezes(const ControllerState c[2])
{
    if (g_beamCx < 0) // also while a confirmed ring is shown (beaming)
        return;
    for (int h = 0; h < 2; h++)
        if (c[h].active && c[h].trigger > kRingClaim && !g_trigPrev[h] && !g_trigLatch[h])
        {
            g_trigLatch[h] = true;
            g_ringSqueeze[h] = true;
            g_ringFired[h] = false;
        }
}

void ClearBeam(const char *why)
{
    if (g_beamCx < 0)
        return;
    if (why)
        fprintf(stderr, "UAVR table: beam ring on %d,%d removed (%s)\n", g_beamCx, g_beamCy, why);
    g_beamCx = g_beamCy = -1;
    g_beamConfirmed = false;
    Station::PreviewBeam(-1, -1);
}

void Release(uint32_t now)
{
    if (g_mode == M_CARRY)
        StopCarry(true, now);
    g_mode = M_IDLE;
    g_hand = -1;
}

} // namespace

// a new level / restart / load. No carry or confirm ring survives into
// the loaded world; grips / triggers already owned stay swallowed until released.
void ResetLevel()
{
    StopCarry(false, 0); // unconditionally: no carry identity survives
    g_mode = M_IDLE;
    g_hand = -1;
    g_snapGid = 0;
    ClearBeam("new level / restart / load");
    g_dioFx = g_dioFy = -1; // the diorama window starts at the new level's host station
    g_dioRecentre = false;
    if (g_dioWinAuto) // every level starts at the whole map and measures again
    {
        g_dioWinMax = 33;
        g_dioReports = 0;
    }
    // no placement hold or pan repeat crosses a level boundary; a stick
    // still held needs a fresh press (g_dioPlaced clears on the first poll it is up)
    for (int h = 0; h < 2; h++)
    {
        g_dioPlaceT0[h] = 0;
        g_dioPlaced[h] = true;
    }
    g_dioPanT = 0;
    g_staticKey = ~0ull; // the board / soil-cut base is rebuilt for the new level
}

// ---------------------------------------------------------------------------------------

void Init(double unitsPerM)
{
    g_upm = unitsPerM > 0 ? unitsPerM : 11.0;
    auto env = [](const char *n) { const char *v = getenv(n); return (v && v[0]) ? v : (const char *)NULL; };
    if (const char *v = env("UA_VR_CMD_LOG")) g_log = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_TABLE")) g_enabled = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_DIORAMA")) g_dioEnv = strcmp(v, "0") != 0;
    if (const char *v = env("UA_VR_DIO_BATCH")) if (atoi(v) <= 0) { g_dioWinMax = 10; g_dioWinAuto = false; }
    if (const char *v = env("UA_VR_DIO_WIN")) { g_dioWinMax = std::max(3, std::min(33, atoi(v))); g_dioWinAuto = false; }
    if (const char *v = env("UA_VR_DIO_BUDGET")) g_dioDrawBudget = std::max(100, atoi(v));
}

bool On() { return g_enabled && Active() && CommandDeck(); }

bool Busy() { return On() && g_mode != M_IDLE; }

int TexCellCap() { return 32; }

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!On() || !yw || !yw->_userRobo || yw->_mapSize.x <= 0 || yw->_mapSize.y <= 0)
    {
        g_ts.valid = false;
        g_flash.clear();
        return;
    }
    TSnap &s = g_ts;
    s.mapW = yw->_mapSize.x;
    s.mapH = yw->_mapSize.y;
    s.lenX = yw->_mapLength.x;
    s.lenY = yw->_mapLength.y;
    const uint8_t me = yw->_userRobo->_owner;
    const size_t n = (size_t)s.mapW * s.mapH;
    std::vector<float> alt(n, 0.0f);
    std::vector<uint8_t> inner(n, 0);
    float lo = 1e30f, hi = -1e30f;
    for (cellArea &cell : yw->_cells)
    {
        const int cx = cell.CellId.x, cy = cell.CellId.y;
        if (cx < 0 || cy < 0 || cx >= s.mapW || cy >= s.mapH)
            continue;
        const size_t i = (size_t)cy * s.mapW + cx;
        alt[i] = -cell.height; // UA y points down
        inner[i] = !((cell.Flags & cellArea::CF_BORDER) || cx == 0 || cy == 0 || cx == s.mapW - 1 || cy == s.mapH - 1);
        if (inner[i])
        {
            lo = std::min(lo, alt[i]);
            hi = std::max(hi, alt[i]);
        }
    }
    s.h.assign(n, 0.0f);
    uint64_t key = 1469598103934665603ull ^ ((uint64_t)s.mapW << 32) ^ (uint64_t)s.mapH;
    const float range = hi - lo;
    for (size_t i = 0; i < n; i++)
    {
        float v = 0;
        if (inner[i] && range > 1.0f)
            v = std::max(0.0f, std::min(1.0f, (alt[i] - lo) / std::max(range, 1500.0f)));
        s.h[i] = v;
        key = (key ^ (uint64_t)(v * 255.0f + 0.5f)) * 1099511628211ull;
    }
    s.hKey = key;
    // diorama: true terrain heights, board units above the table surface (board z = 0).
    // The lowest gameplay sector sits kDioBaseN above it (the base); the border ring is
    // not part of the miniature (height 0).
    {
        const double D = std::max(s.lenX, s.lenY);
        s.refY = (lo < 1e29f ? -lo : 0.0) + kDioBaseN * D;
        s.hTrue.assign(n, 0.0f);
        s.hTrueMax = 0;
        for (size_t i = 0; i < n; i++)
            if (inner[i])
            {
                s.hTrue[i] = (float)((s.refY + alt[i]) / D); // alt = -height (up)
                s.hTrueMax = std::max(s.hTrueMax, (double)s.hTrue[i]);
            }
        uint64_t hk = 1469598103934665603ull ^ ((uint64_t)s.mapW << 32) ^ (uint64_t)s.mapH;
        for (size_t i = 0; i < n; i++)
        {
            uint32_t bits;
            memcpy(&bits, &s.hTrue[i], sizeof(bits));
            hk = (hk ^ bits) * 1099511628211ull;
        }
        s.hTrueKey = hk;
        s.me = me;
    }

    // tokens: own squads (leader, alive count), host stations, seen enemy units
    s.tokens.clear();
    auto colour = [&](uint8_t owner, uint8_t out[3]) {
        const SDL_Color c = yw->GetColor(owner & 7);
        out[0] = c.r; out[1] = c.g; out[2] = c.b;
    };
    auto seen = [&](NC_STACK_ypabact *b) {
        return b->_pSector && b->_pSector->IsCanSee(me) && !b->IsHiddenFor(me); // map rule + stealth (as the wrist radar)
    };
    auto alive = [](NC_STACK_ypabact *b) { return b->_status != BACT_STATUS_DEAD && b->_bact_type != BACT_TYPES_MISSLE; };
    auto place = [&](Token &k, NC_STACK_ypabact *b) {
        k.s = b->_position.x / s.lenX;
        k.t = -b->_position.z / s.lenY;
        k.z = (s.refY - b->_position.y) / std::max(s.lenX, s.lenY);
    };
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        if (!alive(r))
            continue;
        const bool ownSt = r->_owner == me;
        if (r->_bact_type == BACT_TYPES_ROBO && r == yw->_userRobo)
        {
            s.hostCx = (int)(r->_position.x / 1200.0);
            s.hostCy = (int)(-r->_position.z / 1200.0);
        }
        if (r->_bact_type == BACT_TYPES_ROBO && (ownSt || seen(r)))
        {
            Token k;
            k.gid = r->_gid;
            k.kind = TOK_HOST;
            k.own = ownSt;
            place(k, r);
            colour(r->_owner, k.rgb);
            s.tokens.push_back(k);
        }
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            if (!alive(c))
                continue;
            if (c->_owner == me)
            {
                if (c->_bact_type == BACT_TYPES_GUN || c->_bact_type == BACT_TYPES_ROBO)
                    continue;
                Token k;
                k.gid = c->_gid;
                k.kind = TOK_SQUAD;
                k.own = true;
                place(k, c);
                colour(c->_owner, k.rgb);
                int cnt = 1;
                for (NC_STACK_ypabact *u : c->_kidList)
                    if (alive(u))
                        cnt++;
                k.count = cnt;
                s.tokens.push_back(k);
                continue;
            }
            auto enemy = [&](NC_STACK_ypabact *u) {
                if (!alive(u) || u->_owner == me || !seen(u) || u->_bact_type == BACT_TYPES_ROBO)
                    return;
                Token k;
                k.gid = u->_gid;
                k.kind = TOK_ENEMY;
                place(k, u);
                colour(u->_owner, k.rgb);
                s.tokens.push_back(k);
            };
            enemy(c);
            for (NC_STACK_ypabact *u : c->_kidList)
                enemy(u);
        }
    }
    // diorama: a faint team ring under every unit on the table (own, and others where seen)
    s.rings.clear();
    if (Dio())
    {
        auto ring = [&](NC_STACK_ypabact *u) {
            if (!alive(u) || u->_bact_type == BACT_TYPES_GUN)
                return;
            if (u->_owner != me && !seen(u))
                return;
            Token k;
            k.gid = u->_gid;
            k.kind = u->_bact_type == BACT_TYPES_ROBO ? TOK_HOST : u->_owner == me ? TOK_SQUAD : TOK_ENEMY;
            k.own = u->_owner == me;
            place(k, u);
            colour(u->_owner, k.rgb);
            s.rings.push_back(k);
        };
        for (NC_STACK_ypabact *r : yw->_unitsList)
        {
            ring(r);
            for (NC_STACK_ypabact *c : r->_kidList)
            {
                ring(c);
                for (NC_STACK_ypabact *u : c->_kidList)
                    ring(u);
            }
        }
    }
    s.viewer = yw->_viewerBact ? yw->_viewerBact->_gid : 0;
    s.valid = true;
    if (g_dioRecentre) // the station beamed -> the window follows it (big maps only)
    {
        g_dioRecentre = false;
        const int old[4] = {g_dioWin[0], g_dioWin[1], g_dioWin[2], g_dioWin[3]};
        g_dioFx = g_dioFy = -1;
        DioWindow();
        if (g_beamCx >= 0 && !InDioWin(g_beamCx, g_beamCy))
            ClearBeam("window re-centred on the beamed station");
        fprintf(stderr, "UAVR diorama: window re-centred on the host station (sector %d,%d): sectors %d..%d x %d..%d (was %d..%d x %d..%d)\n",
                s.hostCx, s.hostCy, g_dioWin[0], g_dioWin[2], g_dioWin[1], g_dioWin[3], old[0], old[2], old[1], old[3]);
    }
    DioWindow();
    if (g_dioPoseDirty && g_mode == M_IDLE && yw->_GameShell)
    {
        g_dioPoseDirty = false;
        const bool ok = yw_VRPatchProfile(yw->_GameShell->UserName);
        fprintf(stderr, "UAVR diorama: placement saved to profile %s (%s): %.3f,%.3f,%.3f yaw %.1f scale %.2f\n", yw->_GameShell->UserName.c_str(),
                ok ? "ok" : "FAILED", g_dioPose.c[0], g_dioPose.c[1], g_dioPose.c[2], g_dioPose.yaw * 180 / M_PI, g_dioPose.scale);
    }
    {
        VRRadio::Event ev[8];
        const int ne = VRRadio::RecentEvents(ev, 8, kFlashS);
        g_flash.clear();
        g_flashT0 = SDL_GetTicks();
        for (int i = 0; i < ne; i++)
        {
            if (ev[i].sectorX < 0 || ev[i].sectorY < 0 || ev[i].sectorX >= s.mapW || ev[i].sectorY >= s.mapH)
                continue;
            bool dup = false; // one flash per sector (the newest event wins)
            for (const Flash &q : g_flash)
                dup = dup || (q.cx == ev[i].sectorX && q.cy == ev[i].sectorY);
            if (dup)
                continue;
            Flash fl;
            fl.cx = ev[i].sectorX;
            fl.cy = ev[i].sectorY;
            fl.type = ev[i].type;
            fl.ageS = ev[i].ageS;
            g_flash.push_back(fl);
            static uint64_t lastKey = 0;
            const uint64_t key = ((uint64_t)ev[i].msgId << 40) ^ ((uint64_t)fl.cx << 20) ^ (uint64_t)fl.cy ^ ((uint64_t)ev[i].gid << 8);
            if (g_log && fl.ageS < 0.05f && key != lastKey)
            {
                lastKey = key;
                fprintf(stderr, "UAVR table: radio flash sector %d,%d (%s, msg %d)\n", fl.cx, fl.cy, VRRadio::TypeName(fl.type), ev[i].msgId);
            }
        }
    }
    // a carried token whose squad is gone: drop it
    if (g_mode == M_CARRY && !FindTok(g_carryGid))
    {
        if (g_log) fprintf(stderr, "UAVR table: carried squad %u is gone\n", g_carryGid);
        StopCarry(false, 0); // clears the host-carry flag too
        g_mode = M_IDLE;
        g_hand = -1;
    }
    static bool logged = false;
    if (g_log && !logged)
    {
        logged = true;
        int sq = 0, en = 0, ho = 0;
        for (const Token &k : s.tokens)
            (k.kind == TOK_SQUAD ? sq : k.kind == TOK_HOST ? ho : en)++;
        fprintf(stderr, "UAVR table: live, map %dx%d, relief %.0f..%.0f, tokens: %d squads, %d host stations, %d enemy units\n", s.mapW, s.mapH, lo, hi, sq, ho, en);
    }
}

bool Ray(const double o[3], const double d[3], bool hover, double *s, double *t, double hit[3])
{
    if (hover)
    {
        g_hoverTokGid = 0;
        g_hoverTokKind = 0;
    }
    if (!On() || !g_ts.valid)
        return false;
    const Frame f = CurFrame();
    double lo[3], ld[3], a;
    a = 1e18;
    const bool onBoard = RayLocal(f, o, d, lo, ld, s, t, &a);
    if (hover)
        g_hoverTokKind = PickTokenAny(f, lo, ld, &g_hoverTokGid, false, a); // diorama: any direction, the solid occludes
    if (!onBoard && !(hover && g_hoverTokKind))
        return false;
    if (hover && g_hoverTokKind)
    {
        // the token's base point is the hit (the order / click use the token anyway)
        const Token *k = FindTok(g_hoverTokGid);
        if (k)
        {
            *s = k->s;
            *t = k->t;
        }
    }
    ToTracking(f, XFromS(f, *s), YFromT(f, *t), TopAt(*s, *t), hit);
    return true;
}

bool DioramaOn() { return On() && Dio() && g_ts.valid && g_ts.mapW >= 3 && g_ts.mapH >= 3; }

bool DioramaXform(DioXform *x)
{
    if (!DioramaOn())
        return false;
    const Frame f = CurFrame();
    memcpy(x->c, f.c, sizeof(x->c));
    memcpy(x->ex, f.ex, sizeof(x->ex));
    memcpy(x->ey, f.ey, sizeof(x->ey));
    memcpy(x->n, f.n, sizeof(x->n));
    x->L = f.L;
    x->D = std::max(g_ts.lenX, g_ts.lenY);
    x->lenX = g_ts.lenX;
    x->lenY = g_ts.lenY;
    x->refY = g_ts.refY;
    x->me = g_ts.me;
    memcpy(x->win, g_dioWin, sizeof(x->win));
    return x->win[2] >= x->win[0] && x->win[3] >= x->win[1];
}

void DioRecentre() { g_dioRecentre = true; }

// the build wheel opened this poll (after Input ran): drop what the open board
// held at once (carry, confirm ring, hover glow) instead of on the next poll, so nothing of it
// is drawn under the wheel. Same steps as Input's closed-board branch.
void WheelOpened(uint32_t now)
{
    if (!On())
        return;
    if (g_mode != M_IDLE)
        Release(now);
    ClearBeam("build wheel opened");
    g_hoverTokGid = 0;
    g_hoverTokKind = 0;
}

bool DioCarry(uint32_t *gid, double hand[3])
{
    if (!DioramaOn() || g_mode != M_CARRY || !g_carryGid)
        return false;
    *gid = g_carryGid;
    const Frame f = CurFrame();
    ToTracking(f, g_carryLocal[0], g_carryLocal[1], g_carryLocal[2], hand);
    hand[1] += 0.04; // stands on top of the controller, not inside it
    return true;
}

uint32_t DioHoverGid() { return DioramaOn() ? g_hoverTokGid : 0; }

bool HoverToken(uint32_t *gid, int *kind)
{
    if (!On() || !g_hoverTokKind)
        return false;
    *gid = g_hoverTokGid;
    *kind = g_hoverTokKind;
    return true;
}

void Point(double s, double t, double out[3])
{
    const Frame f = CurFrame();
    ToTracking(f, XFromS(f, s), YFromT(f, t), TopAt(s, t), out);
}

bool ScriptToken(double out[3])
{
    if (!On() || !g_ts.valid)
        return false;
    const Frame f = CurFrame();
    for (const Token &k : g_ts.tokens)
        if (k.own && k.kind == TOK_SQUAD && TokOnTable(k))
        {
            double r, hgt;
            TokenSize(k, &r, &hgt);
            ToTracking(f, XFromS(f, k.s), YFromT(f, k.t), Dio() ? TokZ(k) : TopAt(k.s, k.t) + hgt * 0.5, out);
            return true;
        }
    return false;
}

bool ScriptHostToken(double out[3])
{
    if (!On() || !g_ts.valid)
        return false;
    const Frame f = CurFrame();
    for (const Token &k : g_ts.tokens)
        if (k.own && k.kind == TOK_HOST)
        {
            double r, hgt;
            TokenSize(k, &r, &hgt);
            ToTracking(f, XFromS(f, k.s), YFromT(f, k.t), Dio() ? TokZ(k) : TopAt(k.s, k.t) + hgt * 0.5, out);
            return true;
        }
    return false;
}

bool Input(ControllerState c[2], const float physGrip[2], bool boardOpen, uint32_t now, bool xDown)
{
    ClaimRingSqueezes(c);
    if (xDown && g_beamCx >= 0 && !g_beamConfirmed)
        ClearBeam("X"); // X cancels before a same-poll trigger can confirm
    if (!On() || !g_ts.valid || !boardOpen)
    {
        if (g_mode != M_IDLE)
            Release(now);
        ClearBeam("table closed");
        g_hoverTokGid = 0; // no stale hover glow on the miniature under the build wheel
        g_hoverTokKind = 0;
        for (int h = 0; h < 2; h++)
        {
            g_trigPrev[h] = c[h].active && c[h].trigger > kRingClaim;
            if (!g_trigPrev[h])
                g_ringSqueeze[h] = g_ringFired[h] = false;
        }
        // no placement hold survives the closed table; a stick held while
        // closed must be released before it can place on reopening
        for (int h = 0; h < 2; h++)
        {
            g_dioPlaceT0[h] = 0;
            g_dioPlaced[h] = c[h].active && (c[h].buttons & BTN_STICK_CLICK);
        }
        g_dioPanT = 0;
        // keep swallowing a grip this layer owned until it is let go
        for (int h = 0; h < 2; h++)
        {
            g_gripDown[h] = c[h].active && physGrip[h] > 0.6f; // physical state: a held grip is no new edge on reopening
            if (g_owned[h] && physGrip[h] < 0.35f)
                g_owned[h] = false;
            if (g_owned[h])
                c[h].grip = 0;
        }
        LatchTriggers(c);
        return Consumed();
    }
    // place the miniature on the real table: lay a controller on the tabletop and hold
    // its thumbstick pressed (0.8 s). The table surface = the controller's height minus
    // its resting height, centred on it, the near edge facing the head.
    if (Dio())
    {
        for (int h = 0; h < 2; h++)
        {
            const bool down = c[h].active && (c[h].buttons & BTN_STICK_CLICK);
            if (!down)
            {
                g_dioPlaceT0[h] = 0;
                g_dioPlaced[h] = false;
            }
            else if (g_mode != M_IDLE)
            {
                // no hold accumulates during a grab / carry; a fresh press is
                // needed after it (g_dioPlaced latches until the stick is let go)
                g_dioPlaceT0[h] = 0;
                g_dioPlaced[h] = true;
            }
            else if (g_dioPlaced[h])
                ; // this press already placed (or was spent): wait for its release
            else if (!g_dioPlaceT0[h])
                g_dioPlaceT0[h] = now ? now : 1;
            else if (now - g_dioPlaceT0[h] >= kDioPlaceHoldMs)
            {
                g_dioPlaced[h] = true;
                double hp[3];
                HandPos(c[h], hp);
                const double v[2] = {g_head[0] - hp[0], g_head[2] - hp[2]};
                const double yaw = (g_hasHead && v[0] * v[0] + v[1] * v[1] > 1e-4) ? atan2(v[0], v[1]) : g_dioPose.yaw;
                const double nc[3] = {hp[0], hp[1] - kCtrlRestM, hp[2]};
                if (std::isfinite(nc[0]) && std::isfinite(nc[1]) && std::isfinite(nc[2]) && std::isfinite(yaw))
                {
                    memcpy(g_dioPose.c, nc, sizeof(nc));
                    g_dioPose.yaw = yaw;
                    g_dioPoseSet = true;
                    g_dioPoseDirty = true;
                    Rumble(0.5f, 0.5f, 80);
                    fprintf(stderr, "UAVR diorama: placed on the table (%s controller) at %.3f,%.3f,%.3f yaw %.1f\n", h ? "right" : "left",
                            nc[0], nc[1], nc[2], yaw * 180 / M_PI);
                }
            }
            if (c[h].active)
                c[h].buttons &= ~BTN_STICK_CLICK; // the table's while the miniature is up
        }
    }
    // right stick pans the diorama window over a big map (one sector per step, repeat
    // every 220 ms while held). The stick is the table's while it is open there.
    if (Dio() && (g_dioFx >= 0 || g_dioFy >= 0) && c[1].active)
    {
        const float sx = c[1].stickX, sy = c[1].stickY;
        const int dx = sx > 0.6f ? 1 : sx < -0.6f ? -1 : 0, dy = sy > 0.6f ? -1 : sy < -0.6f ? 1 : 0; // stick up = towards the far edge
        if ((dx || dy) && now - g_dioPanT >= 220)
        {
            g_dioPanT = now;
            if (g_dioFx >= 0) g_dioFx += dx;
            if (g_dioFy >= 0) g_dioFy += dy;
            const int old[4] = {g_dioWin[0], g_dioWin[1], g_dioWin[2], g_dioWin[3]};
            DioWindow();
            if (g_beamCx >= 0 && !InDioWin(g_beamCx, g_beamCy))
                ClearBeam("ring sector panned out of the window"); //
            if (g_log && memcmp(old, g_dioWin, sizeof(old)))
                fprintf(stderr, "UAVR diorama: window panned to sectors %d..%d x %d..%d\n", g_dioWin[0], g_dioWin[2], g_dioWin[1], g_dioWin[3]);
        }
        if (!dx && !dy)
            g_dioPanT = 0;
        c[1].stickX = c[1].stickY = 0;
    }
    const Frame f = CurFrame();
    Pose &pose = Wall() ? g_wallPose : Dio() ? g_dioPose : g_tablePose;
    const double sMin = Wall() ? 0.5 : 0.6, sMax = Wall() ? 1.6 : Dio() ? 3.0 : 1.8;
    // What a grip at hand h would take: a squad token (kind 1, *tok), else the table
    // (*onTable / *nearOut), else nothing.
    auto probe = [&](int h, uint32_t *tokOut, bool *onTableOut, bool *nearOut) -> int {
        double hp[3];
        HandPos(c[h], hp);
        double o[3], d[3], lo[3], ld[3], s, t, a = 1e18;
        AimRay(c[h], o, d);
        const bool onTable = RayLocal(f, o, d, lo, ld, &s, &t, &a);
        uint32_t tok = 0;
        int kind = PickTokenAny(f, lo, ld, &tok, true, a);
        double hl[3];
        ToLocal(f, hp, hl);
        bool near;
        if (Dio())
        {
            // the visible window (its slab + plinth rim), up to the tallest relief
            double xa, xb, ya, yb;
            DioBounds(f, &xa, &xb, &ya, &yb);
            const double rim = 0.013 + 0.02 / f.L; // the plinth + its lip (board units) + 2 cm
            near = hl[0] >= xa - rim && hl[0] <= xb + rim && hl[1] >= ya - rim && hl[1] <= yb + rim &&
                   hl[2] * f.L >= -0.05 && (hl[2] - g_ts.hTrueMax) * f.L <= kGrabHandM;
        }
        else
            near = fabs(hl[0]) <= f.wN / 2 + kRimN && fabs(hl[1]) <= f.hN / 2 + kRimN &&
                   hl[2] * f.L >= -0.05 && hl[2] * f.L <= kGrabHandM;
        if (!kind && near)
        {
            // a squad token right at the hand
            double best = 0.04 / f.L;
            for (const Token &k : g_ts.tokens)
                if (k.own && k.kind == TOK_SQUAD && TokOnTable(k))
                {
                    const double dx = XFromS(f, k.s) - hl[0], dy = YFromT(f, k.t) - hl[1], dz = TokZ(k) - hl[2];
                    const double dd = sqrt(dx * dx + dy * dy + dz * dz);
                    if (dd < best)
                    {
                        best = dd;
                        tok = k.gid;
                        kind = 1;
                    }
                }
        }
        if (!kind) // station: the own host station token (beam the station)
        {
            const uint32_t host = PickOwnHost(f, lo, ld, a, hl, near);
            if (host)
            {
                tok = host;
                kind = 3;
            }
        }
        *tokOut = tok;
        *onTableOut = onTable;
        *nearOut = near;
        return kind;
    };
    // grip edges of both hands are resolved token-first, so a hand
    // closing on a squad token picks it up even when the other hand grips the empty
    // table in the same poll (it would otherwise start a grab and turn this into a scale).
    int order[2] = {0, 1};
    if (g_mode == M_IDLE)
    {
        bool e[2];
        for (int h = 0; h < 2; h++)
            e[h] = c[h].active && physGrip[h] > 0.6f && !g_gripDown[h];
        if (e[0] && e[1])
        {
            uint32_t tk;
            bool ot, nr;
            // squad tokens and (station) the own host token both beat the empty table
            auto isTok = [&](int h) {
                const int k = probe(h, &tk, &ot, &nr);
                return tk && (k == 1 || (k == 3 && !Station::Beaming()));
            };
            if (isTok(1) && !isTok(0))
            {
                order[0] = 1;
                order[1] = 0;
            }
        }
    }
    for (int oi = 0; oi < 2; oi++)
    {
        const int h = order[oi];
        const bool down = c[h].active && physGrip[h] > 0.6f; // edges from the unfiltered grips (the wheel
        const bool up = !c[h].active || physGrip[h] < 0.35f;   // may have zeroed them this poll)
        const bool edge = down && !g_gripDown[h];
        if (edge && g_beamCx >= 0 && !g_beamConfirmed)
            ClearBeam("grip"); // a new grip replaces the ring (e.g. pick the station up again)
        if (edge)
        {
            double hp[3];
            HandPos(c[h], hp);
            if (g_mode == M_GRAB1 && g_hand != h)
            {
                // second hand: scale (and turn / move with the pair)
                g_mode = M_GRAB2;
                g_pose0 = pose;
                HandPos(c[0], g_hand0[0]);
                HandPos(c[1], g_hand0[1]);
                const double v[3] = {g_hand0[1][0] - g_hand0[0][0], 0, g_hand0[1][2] - g_hand0[0][2]};
                g_pairYaw0 = atan2(-v[2], v[0]);
                g_pairDist0 = std::max(0.05, sqrt(v[0] * v[0] + (g_hand0[1][1] - g_hand0[0][1]) * (g_hand0[1][1] - g_hand0[0][1]) + v[2] * v[2]));
                g_owned[h] = true;
                if (g_log) fprintf(stderr, "UAVR table: two-hand grab (scale %.2f)\n", pose.scale);
            }
            else if (g_mode == M_IDLE)
            {
                // on the table? the hand's ray hits it, or the hand is just above it
                uint32_t tok = 0;
                bool onTable = false, near = false;
                const int kind = probe(h, &tok, &onTable, &near);
                if (kind == 1 && tok)
                {
                    g_mode = M_CARRY;
                    g_hand = h;
                    g_carryGid = tok;
                    g_carryHost = false; // a squad token, never the host-carry path
                    ToLocal(f, hp, g_carryLocal);
                    g_carryPrevDist = 1e9; // already at the face (e.g. from the wall): the next update dives
                    g_owned[h] = true;
                    if (g_log) fprintf(stderr, "UAVR table: picked up squad token %u (%s hand)\n", tok, h ? "right" : "left");
                }
                else if (kind == 3 && tok && !Station::Beaming())
                {
                    g_mode = M_CARRY;
                    g_hand = h;
                    g_carryGid = tok;
                    g_carryHost = true;
                    ToLocal(f, hp, g_carryLocal);
                    g_owned[h] = true;
                    fprintf(stderr, "UAVR table: picked up the host station token %u (%s hand)\n", tok, h ? "right" : "left");
                }
                else if (onTable || near)
                {
                    g_mode = M_GRAB1;
                    g_hand = h;
                    g_pose0 = pose;
                    memcpy(g_hand0[h], hp, sizeof(hp));
                    g_handYaw0 = HandYaw(c[h]);
                    g_owned[h] = true;
                    if (g_log) fprintf(stderr, "UAVR table: grab (%s hand) at %.3f,%.3f,%.3f\n", h ? "right" : "left", hp[0], hp[1], hp[2]);
                }
            }
        }
        if (up && g_owned[h])
        {
            g_owned[h] = false;
            if (g_mode == M_GRAB2)
            {
                // back to one hand: rebase on the other
                const int o = 1 - h;
                g_mode = M_GRAB1;
                g_hand = o;
                g_pose0 = pose;
                HandPos(c[o], g_hand0[o]);
                g_handYaw0 = HandYaw(c[o]);
            }
            else if (g_hand == h)
            {
                if (g_mode == M_CARRY && g_carryHost)
                {
                    // station: put down on a sector (token near the relief), or let go
                    // with the hand's ray on the table -> confirm ring on that sector
                    const Frame ff = CurFrame();
                    double hpNow[3]; // the release poll's hand, not the previous poll's
                    HandPos(c[h], hpNow);
                    ToLocal(ff, hpNow, g_carryLocal);
                    const double *p = g_carryLocal;
                    double s = SFromX(ff, p[0]), t = TFromY(ff, p[1]);
                    bool ok = false;
                    const char *how = "put down";
                    // in the diorama only a sector of the visible window
                    if (s >= 0 && s <= 1 && t >= 0 && t <= 1 &&
                        (!Dio() || (InDioWin((int)floor(s * g_ts.mapW), (int)floor(t * g_ts.mapH)) && InDioFoot(s, t))))
                    {
                        const double above = (p[2] - TopAt(s, t)) * ff.L;
                        ok = above > -0.05 && above < kDropAboveM;
                    }
                    if (!ok)
                    {
                        double o[3], d[3], lo[3], ld[3], a;
                        AimRay(c[h], o, d);
                        ok = RayLocal(ff, o, d, lo, ld, &s, &t, &a);
                        how = "pointed";
                    }
                    if (ok)
                    {
                        g_beamCx = std::max(0, std::min(g_ts.mapW - 1, (int)(s * g_ts.mapW)));
                        g_beamCy = std::max(0, std::min(g_ts.mapH - 1, (int)(t * g_ts.mapH)));
                        g_beamT = now;
                        g_beamConfirmed = false;
                        Station::PreviewBeam(g_beamCx, g_beamCy);
                        // a trigger already squeezed on this release poll is the
                        // ring's (swallowed until released); confirming needs a fresh squeeze
                        for (int k = 0; k < 2; k++)
                            if (c[k].active && c[k].trigger > kRingClaim)
                            {
                                g_trigLatch[k] = true;
                                g_ringSqueeze[k] = true;
                                g_ringFired[k] = true;
                            }
                        fprintf(stderr, "UAVR table: host station token %s on sector %d,%d -> confirm ring (trigger on it = beam, elsewhere = cancel)\n", how, g_beamCx, g_beamCy);
                    }
                    else
                        fprintf(stderr, "UAVR table: host station token released off the table -> snaps back\n");
                }
                if (g_log && g_mode == M_GRAB1)
                    fprintf(stderr, "UAVR table: release, pose %.3f,%.3f,%.3f yaw %.1f scale %.2f\n", pose.c[0], pose.c[1], pose.c[2], pose.yaw * 180 / M_PI, pose.scale);
                if (Dio() && g_mode == M_GRAB1)
                {
                    g_dioPoseSet = true; // moved / turned / scaled -> kept in the profile
                    g_dioPoseDirty = true;
                }
                Release(now);
            }
        }
        g_gripDown[h] = down;
    }

    // apply the grab
    if (g_mode == M_GRAB1 && g_hand >= 0)
    {
        double hp[3];
        HandPos(c[g_hand], hp);
        const double dy = HandYaw(c[g_hand]) - g_handYaw0;
        const double cy = cos(dy), sy = sin(dy);
        const double *h0 = g_hand0[g_hand];
        const double rel[3] = {g_pose0.c[0] - h0[0], g_pose0.c[1] - h0[1], g_pose0.c[2] - h0[2]};
        pose.c[0] = hp[0] + rel[0] * cy + rel[2] * sy;
        pose.c[1] = hp[1] + rel[1];
        pose.c[2] = hp[2] - rel[0] * sy + rel[2] * cy;
        pose.yaw = g_pose0.yaw + dy;
    }
    else if (g_mode == M_GRAB2)
    {
        double a[3], b[3];
        HandPos(c[0], a);
        HandPos(c[1], b);
        const double v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double dist = std::max(0.05, sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        const double sc = std::max(sMin, std::min(sMax, g_pose0.scale * dist / g_pairDist0));
        const double k = sc / g_pose0.scale;
        const double dy = atan2(-v[2], v[0]) - g_pairYaw0;
        const double cy = cos(dy), sy = sin(dy);
        const double m0[3] = {(g_hand0[0][0] + g_hand0[1][0]) * 0.5, (g_hand0[0][1] + g_hand0[1][1]) * 0.5, (g_hand0[0][2] + g_hand0[1][2]) * 0.5};
        const double m1[3] = {(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5};
        const double rel[3] = {(g_pose0.c[0] - m0[0]) * k, (g_pose0.c[1] - m0[1]) * k, (g_pose0.c[2] - m0[2]) * k};
        pose.c[0] = m1[0] + rel[0] * cy + rel[2] * sy;
        pose.c[1] = m1[1] + rel[1];
        pose.c[2] = m1[2] - rel[0] * sy + rel[2] * cy;
        pose.yaw = g_pose0.yaw + dy;
        static double lastLogged = 0;
        if (g_log && fabs(sc - lastLogged) > 0.1)
        {
            lastLogged = sc;
            fprintf(stderr, "UAVR table: scale %.2f (clamp %.1f..%.1f)\n", sc, sMin, sMax);
        }
        pose.scale = sc;
    }
    if ((g_mode == M_GRAB1 || g_mode == M_GRAB2) &&
        !(std::isfinite(pose.c[0]) && std::isfinite(pose.c[1]) && std::isfinite(pose.c[2]) && std::isfinite(pose.yaw) && std::isfinite(pose.scale)))
        pose = g_pose0; // a bad tracking sample never corrupts the session pose
    if (g_mode == M_CARRY && g_hand >= 0 && g_carryHost)
    {
        double hp[3];
        HandPos(c[g_hand], hp);
        ToLocal(CurFrame(), hp, g_carryLocal); // the station token never dives
    }
    else if (g_mode == M_CARRY && g_hand >= 0)
    {
        double hp[3];
        HandPos(c[g_hand], hp);
        ToLocal(CurFrame(), hp, g_carryLocal);
        const double dist = sqrt((hp[0] - g_head[0]) * (hp[0] - g_head[0]) + (hp[1] - g_head[1]) * (hp[1] - g_head[1]) + (hp[2] - g_head[2]) * (hp[2] - g_head[2]));
        if (dist < kCarryDiveM && g_carryPrevDist >= kCarryDiveM)
        {
            const uint32_t gid = g_carryGid;
            if (CommandDive(gid, false))
            {
                fprintf(stderr, "UAVR table: token %u brought to the face (%.2f m) -> beam-in dive\n", gid, dist);
                g_bootPending = true;
                g_bootSawBlack = false;
                g_bootReqT = now;
                g_bootT0 = 0;
                StopCarry(false, now);
                g_mode = M_IDLE;
                g_hand = -1; // the grip stays owned until released
            }
            else
                fprintf(stderr, "UAVR table: token %u at the face, dive refused (busy / not diveable)\n", gid);
        }
        g_carryPrevDist = dist;
    }
    // station: the confirm ring. A trigger with that hand's ray on the ring's sector
    // confirms (only when the original checks pass), any other trigger cancels; the
    // trigger is swallowed until released (no board click, no shot).
    if (g_beamCx >= 0)
    {
        if (g_beamConfirmed)
        {
            if (!Station::Beaming())
                ClearBeam(NULL);
        }
        else if (now - g_beamT > kBeamPendingMs)
            ClearBeam("timeout");
        else if (g_mode == M_IDLE)
        {
            //: squeezes were claimed at poll entry (ClaimRingSqueezes); each
            // decides at 0.6. Both hands are evaluated first; a cancel wins over a confirm.
            bool confirm = false, cancel = false;
            const char *why = NULL;
            int byHand = -1, cost = 0;
            for (int h = 0; h < 2; h++)
            {
                if (!(g_ringSqueeze[h] && !g_ringFired[h] && c[h].active && c[h].trigger > 0.6f))
                    continue;
                g_ringFired[h] = true;
                double o[3], d[3], lo[3], ld[3], s, t, a;
                AimRay(c[h], o, d);
                const bool on = RayLocal(f, o, d, lo, ld, &s, &t, &a) && (int)(s * g_ts.mapW) == g_beamCx && (int)(t * g_ts.mapH) == g_beamCy;
                const int st = Station::BeamStatus(g_beamCx, g_beamCy, &cost);
                if (on && st == Station::BEAM_OK)
                {
                    confirm = true;
                    byHand = h;
                }
                else
                {
                    cancel = true;
                    why = on ? Station::BeamCheckText(st) : "trigger off the ring";
                }
            }
            if (confirm && Cmd::ViewChanging())
            {
                cancel = true; // no beam while a dive / return fade runs
                why = "view changing";
            }
            if (cancel)
                ClearBeam(why);
            else if (confirm)
            {
                g_beamConfirmed = true;
                Station::ConfirmBeam(g_beamCx, g_beamCy);
                fprintf(stderr, "UAVR table: beam ring %d,%d confirmed (%s trigger, cost %d)\n", g_beamCx, g_beamCy, byHand ? "right" : "left", cost);
            }
        }
    }
    for (int h = 0; h < 2; h++)
    {
        g_trigPrev[h] = c[h].active && c[h].trigger > kRingClaim;
        if (!g_trigPrev[h])
            g_ringSqueeze[h] = g_ringFired[h] = false;
    }
    for (int h = 0; h < 2; h++)
        if (g_owned[h])
            c[h].grip = 0;
    LatchTriggers(c);
    return Consumed();
}

void DrawEye(int eye, const ViewState &views, double zn, double zf, uint32_t tex, int hoverKind, uint32_t hoverGid,
             bool hit, double hs, double ht, uint32_t selCmdr)
{
    if (!On() || !g_ts.valid || !views.valid)
        return;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return;
    const Frame f = CurFrame();
    const bool dio = Dio();
    const uint32_t now = SDL_GetTicks();
    if (eye == 0)
    {
        g_head[0] = views.head.position.x;
        g_head[1] = views.head.position.y;
        g_head[2] = views.head.position.z;
        g_hasHead = true;
        BuildDynamic(f, hoverKind, hoverGid, hit, hs, ht, selCmdr, now);
    }
    const uint64_t key = (Dio() ? g_ts.hTrueKey : g_ts.hKey) ^ (Wall() ? 0x5555ull : 0) ^
                         (Dio() ? 0xD10Aull ^ ((uint64_t)g_dioWin[0] << 16) ^ ((uint64_t)g_dioWin[1] << 28) ^ ((uint64_t)g_dioWin[2] << 40) ^ ((uint64_t)g_dioWin[3] << 52) : 0);
    const bool rebuild = eye == 0 && key != g_staticKey;

    GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevActive = 0, depthFunc = 0;
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    if (rebuild)
    {
        BuildStatic(f);
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vboS);
        gl.BufferData(GL_ARRAY_BUFFER, g_static.size() * sizeof(TV), g_static.data(), GL_STATIC_DRAW);
        g_staticKey = key;
        if (g_log) fprintf(stderr, "UAVR table: relief built, %d triangles (%s)\n", g_staticCount / 3, Wall() ? "wall" : dio ? "diorama base" : "table");
    }
    EnsureWhite();
    gl.UseProgram(g_prog);
    // diorama: the engine already drew the miniature into this depth buffer with the
    // world projection (zn, zf in units = zn / upm, zf / upm in metres): share it.
    UploadMVP(dio ? EyeMVP(f, views, eye, zn / g_upm, zf / g_upm) : EyeMVP(f, views, eye));
    gl.Uniform1i(g_locTex, 0);
    glBindTexture(GL_TEXTURE_2D, tex ? tex : g_white);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    // the table is the nearest thing in front of the player: fresh depth (as the
    // cockpit shell), opaque, depth-tested against itself and its tokens
    glDepthMask(GL_TRUE);
    if (!dio)
        glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    gl.BindVertexArray(g_vaoS);
    glDrawArrays(GL_TRIANGLES, 0, g_staticCount);
    if (g_dynCount > 0)
    {
        gl.BindVertexArray(g_vaoD);
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vboD);
        if (eye == 0)
            gl.BufferData(GL_ARRAY_BUFFER, g_dynCount * sizeof(TV), g_dyn.data(), GL_STREAM_DRAW);
        glBindTexture(GL_TEXTURE_2D, g_white);
        glDrawArrays(GL_TRIANGLES, 0, g_dynCount);
    }

    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthFunc(depthFunc);
    glDepthMask(depthMask);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    gl.ActiveTexture(prevActive);
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    gl.UseProgram(prevProg);
}

// passthrough: everything the miniature drew with depth becomes opaque (alpha 1);
// the cleared room (alpha 0) and additive glows without depth stay see-through, so the
// compositor shows the real room behind (premultiplied alpha). Eye FBO bound.
void DioPassthroughAlpha()
{
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return;
    static GLuint vao = 0, vbo = 0;
    GLint prevProg = 0, prevVao = 0, prevBuf = 0, depthFunc = 0;
    GLboolean depthMask = GL_TRUE, cmask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    if (!vao)
    {
        // one full-screen triangle just in front of the far plane (clip z = 0.99999 w)
        const float z = 0.99999f;
        TV v[3] = {{{-1, -1, z}, {0, 0, 0, 255}, {-1, -1}}, {{3, -1, z}, {0, 0, 0, 255}, {-1, -1}}, {{-1, 3, z}, {0, 0, 0, 255}, {-1, -1}}};
        gl.GenVertexArrays(1, &vao);
        gl.GenBuffers(1, &vbo);
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        Attribs();
    }
    gl.UseProgram(g_prog);
    M4 id = M4::Ident();
    UploadMVP(id);
    gl.BindVertexArray(vao);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER); // only where something was drawn (stored depth < the triangle)
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthFunc(depthFunc);
    glDepthMask(depthMask);
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    gl.UseProgram(prevProg);
}

// the station beam's fade on the mixed-reality table: darken only what the miniature
// drew (depth written; the same full-screen triangle test as DioPassthroughAlpha), colour
// times (1 - a), alpha untouched, so the passthrough room never goes black. Call after the
// miniature and DioPassthroughAlpha, before the table base / rings (they stay lit).
void DioDim(float a)
{
    if (!(a > 0.0f))
        return;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return;
    static GLuint vao = 0, vbo = 0;
    const float z = 0.99999f;
    const uint8_t gr = (uint8_t)std::max(0, std::min(255, (int)lrintf((1.0f - std::min(1.0f, a)) * 255.0f)));
    TV v[3] = {{{-1, -1, z}, {gr, gr, gr, 255}, {-1, -1}}, {{3, -1, z}, {gr, gr, gr, 255}, {-1, -1}}, {{-1, 3, z}, {gr, gr, gr, 255}, {-1, -1}}};
    GLint prevProg = 0, prevVao = 0, prevBuf = 0, depthFunc = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
    GLboolean depthMask = GL_TRUE, cmask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    if (!vao)
    {
        gl.GenVertexArrays(1, &vao);
        gl.GenBuffers(1, &vbo);
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
        Attribs();
    }
    else
    {
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    }
    gl.UseProgram(g_prog);
    M4 id = M4::Ident();
    UploadMVP(id);
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE); // dst.rgb *= grey
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    gl.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthFunc(depthFunc);
    glDepthMask(depthMask);
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    gl.UseProgram(prevProg);
}

bool FxOn()
{
    if (!On())
    {
        g_bootPending = false; // left COMMAND DECK: no table effect survives into CLASSIC
        g_bootT0 = 0;
    }
    return g_bootPending || g_bootT0 != 0;
}

void DrawFx(int eye, const ViewState &views, double zn, double zf, float fade)
{
    (void)eye;
    (void)views;
    (void)zn;
    (void)zf;
    if (!FxOn())
        return;
    const uint32_t now = SDL_GetTicks();
    if (g_bootPending)
    {
        if (fade >= 0.99f)
            g_bootSawBlack = true;
        if (g_bootSawBlack && fade < 0.99f)
        {
            g_bootPending = false;
            g_bootT0 = now; // the black starts lifting: boot
            if (g_log) fprintf(stderr, "UAVR table: cockpit boot flash\n");
        }
        else if (now - g_bootReqT > 1500)
            g_bootPending = false; // the dive did not happen
    }
    if (!g_bootT0)
        return;
    const double k = (now - g_bootT0) / (double)kBootMs;
    if (k >= 1.0)
    {
        g_bootT0 = 0;
        return;
    }
    EnsureWhite();
    // cyan systems-on tint, flickering for the first ~150 ms, then decaying
    const double flick = k < 0.3 ? (((now / 40) & 1) ? 1.0 : 0.45) : 1.0;
    const float a = (float)(0.28 * (1.0 - k) * flick);
    float q[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    Overlay::Quad(q, Overlay::STYLE_TEXTURE, 0.35f, 0.95f, 1.0f, a, g_white);
    // a bright scan line sweeping top -> bottom over the first 60 %
    if (k < 0.6)
    {
        const double y = 1.0 - 2.0 * (k / 0.6), hh = 0.035;
        float b[4][4] = {{-1, (float)(y - hh), 0, 1}, {-1, (float)(y + hh), 0, 1}, {1, (float)(y + hh), 0, 1}, {1, (float)(y - hh), 0, 1}};
        Overlay::Quad(b, Overlay::STYLE_BEAM, 0.6f, 1.0f, 1.0f, 0.75f);
    }
}

} // namespace Table
} // namespace VR

namespace VR
{
// diorama placement <-> profile (see vr.h)
bool DioramaPoseSet() { return Table::g_dioPoseSet; }

// draws the miniature submitted this frame over `sectors` window sectors.
// Measured on the first frame and then every 90th: the window shrinks at once to what
// fits the budget (never below the 10x10) and grows back only with 2 sectors of margin.
void DioramaReportDraws(int draws, int sectors)
{
    using namespace Table;
    if (!g_dioWinAuto || sectors <= 0 || draws <= 0)
        return;
    if (g_dioReports++ % 90 != 0)
        return;
    const double perSector = (double)draws / sectors;
    int nt = (int)floor(sqrt(g_dioDrawBudget / perSector));
    nt = std::max(10, std::min(33, nt));
    const int cur = g_dioWinMax;
    int next = cur;
    if (nt < cur)
        next = nt;
    else if (nt >= cur + 2)
        next = std::min(33, nt - 1);
    if (next != cur)
    {
        g_dioWinMax = next;
        if (g_log)
            fprintf(stderr, "UAVR diorama: window budget %d draws: %d draws over %d sectors -> window max %d (was %d)\n",
                    g_dioDrawBudget, draws, sectors, next, cur);
    }
}
int DioramaPoseKey(int which)
{
    const Table::Pose &p = Table::g_dioPose;
    switch (which)
    {
    case 0: return (int)lround(p.c[0] * 1000.0);
    case 1: return (int)lround(p.c[1] * 1000.0);
    case 2: return (int)lround(p.c[2] * 1000.0);
    case 3: return (int)lround(p.yaw * 18000.0 / M_PI);
    case 4: return (int)lround(p.scale * 1000.0);
    default: return 0;
    }
}
void SetDioramaPoseKey(int which, int v)
{
    Table::Pose &p = Table::g_dioPose;
    switch (which)
    {
    case 0: p.c[0] = std::max(-5.0, std::min(5.0, v / 1000.0)); break;
    case 1: p.c[1] = std::max(-3.0, std::min(3.0, v / 1000.0)); break;
    case 2: p.c[2] = std::max(-5.0, std::min(5.0, v / 1000.0)); break;
    case 3: p.yaw = (v % 36000) * M_PI / 18000.0; break;
    case 4: p.scale = std::max(0.5, std::min(3.0, v / 1000.0)); break;
    default: return;
    }
    Table::g_dioPoseSet = true;
}
void ResetDioramaPose()
{
    const Table::Pose def = {{0.0, -0.62, -0.60}, 0.0, 1.0};
    Table::g_dioPose = def;
    Table::g_dioPoseSet = false;
}
} // namespace VR
