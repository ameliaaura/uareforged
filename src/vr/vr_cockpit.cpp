// Urban Assault VR — cockpit shells, dash screens, wrist panel, shake -> haptics.
// See vr_cockpit.h and
//
// Space: everything is built in comfort-anchor-local UAV axes (x right, y DOWN,
// z forward) in METRES, origin = the seated eye point; the model matrix scales
// metres to world units (UA_VR_UNITS_PER_M) and centreFromAnchor/eyeProj take
// it to clip space, exactly like the comfort rim.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_cockpit.h"
#include "vr_aim.h"
#include "vr_radio.h"
#include "vr_gl.h"
#include "vr_field_logic.h"
#include "vr_sites.h"
#include "vr_command.h"
#include "../system/gfx.h"
#include "../system/tform.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"

extern yw_infolog info_log; // yw_game_ui.cpp (the radio / info ticker log)

namespace VR
{

// ---------------------------------------------------------------------------
// Public switch (vr.h). Valid with VR off too (profile load/save).

namespace
{
bool g_shellsOn = true;
int g_shellsEnv = -2; // UA_VR_COCKPIT=0/1 overrides the profile/menu (tests); -1 = unset
bool ShellsOn()
{
    if (g_shellsEnv == -2)
    {
        const char *v = getenv("UA_VR_COCKPIT");
        g_shellsEnv = (v && v[0]) ? (strcmp(v, "0") != 0) : -1;
    }
    return g_shellsEnv >= 0 ? g_shellsEnv != 0 : g_shellsOn;
}
}

bool CockpitShellsOn() { return ShellsOn(); }
void SetCockpitShellsOn(bool on) { g_shellsOn = on; }

namespace Cockpit
{

namespace
{

// ---- config / state -------------------------------------------------------

enum Class { CLS_NONE = 0, CLS_GROUND, CLS_HELI, CLS_JET, CLS_GUN, CLS_COUNT };
const char *kClassName[CLS_COUNT] = {"none", "ground", "heli", "jet", "gun"};

Backend *g_backend = NULL;
double g_upm = 11.0;
bool g_log = false;
int g_forceClass = -1;     // UA_VR_COCKPIT_CLASS (captures/tests)
bool g_allowShake = false; // UA_VR_ALLOW_SHAKE=1: A/B, the engine shake moves the view again
bool g_hapticsOn = true;   // UA_VR_HAPTICS=0 disables the cockpit's own pulses

double EnvF(const char *name, double def)
{
    const char *v = getenv(name);
    return (v && v[0]) ? atof(v) : def;
}
bool EnvOn(const char *name, bool def)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    return strcmp(v, "0") != 0;
}

struct Snap
{
    int cls = CLS_NONE;
    bool hudOn = true, dead = false;
    int hp = 0, hpMax = 1, shield = 0;
    std::string vehName;
    bool hasWeapon = false;
    std::string weapName;
    int dmg = 0, nWeap = 1, reloadPct = 100;
    bool hasMg = false;
    // target (Aim::Target, copied)
    bool tgt = false, tgtLocked = false, tgtEnemy = false;
    std::string tgtName;
    int tgtPct = 0;
    double tgtDistM = 0, rayDistM = 0;
    bool rayHit = false;
    // radio
    int nMsg = 0;
    std::string msg[3];
    float msgAge[3] = {0, 0, 0};
    // wrist
    struct Sq { std::string name; int n; int pct; bool mine; uint32_t gid = 0; bool sel = false; }; // gid / sel: clickable wrist
    std::vector<Sq> squads;
    struct Blip { float x, z; int kind; }; // metres-on-radar (heading up), kind 0 own 1 enemy 2 host
    std::vector<Blip> blips;
    int hostPct = 0;
    uint32_t timeMs = 0;
};
Snap g_snap;

// haptics / shake bookkeeping
uint32_t g_viewerGid = 0;
int g_lastEnergy = -1;
double g_shakeDeg = 0, g_shakeApplied = 0;
double g_lastShakePulse = -1;
struct LogAcc
{
    double since = 0;
    double shakePeak = 0, appliedPeak = 0, viewDevPeak = 0;
    int hit = 0, shake = 0, ff = 0, frames = 0, shellFrames = 0;
    float hitAmpPeak = 0;
};
LogAcc g_la;

double NowMs() { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

std::string Upper(const std::string &s, size_t maxLen = 64)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        if ((c == 0xC3 || c == 0xC2) && i + 1 < s.size()) // UTF-8 umlauts in the game's German names
        {
            const unsigned char d = (unsigned char)s[++i];
            const char *m = (d == 0xA4 || d == 0x84) ? "AE" : (d == 0xB6 || d == 0x96) ? "OE" : (d == 0xBC || d == 0x9C) ? "UE" : (d == 0x9F) ? "SS" : "?";
            o += m;
            continue;
        }
        if (c == 0xE4 || c == 0xC4) { o += "AE"; continue; } // Latin-1
        if (c == 0xF6 || c == 0xD6) { o += "OE"; continue; }
        if (c == 0xFC || c == 0xDC) { o += "UE"; continue; }
        if (c == 0xDF) { o += "SS"; continue; }
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        if (c < 32 || c > 95)
            c = (c >= 128) ? '?' : ' ';
        o.push_back((char)c);
        if (o.size() >= maxLen)
            break;
    }
    return o;
}

// ---- 5x7 font (ASCII 32..95; lowercase maps to uppercase) -----------------

const uint8_t kFont[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, {0x0C,0x04,0x08,0x00,0x00,0x00,0x00},
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x00,0x01,0x02,0x04,0x08,0x10,0x00},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
};
// Atlas: 16 x 5 cells of 8 x 9 font pixels, 4 texels per font pixel. Glyph
// pixels at cell (1..5, 1..7). Cell 64 = solid (bars, boxes).
const int kS = 4, kCellW = 8, kCellH = 9, kCols = 16, kRows = 5;
const int kAtlasW = kCols * kCellW * kS, kAtlasH = kRows * kCellH * kS; // 512 x 180
const int kSolid = 64;

// ---- geometry ------------------------------------------------------------

struct V3
{
    float x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
    V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
    V3 operator*(float s) const { return V3(x * s, y * s, z * s); }
    V3 operator-() const { return V3(-x, -y, -z); }
};
float Dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(const V3 &a, const V3 &b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float Len(const V3 &a) { return sqrtf(Dot(a, a)); }
V3 Norm(const V3 &a) { const float l = Len(a); return l > 1e-9f ? a * (1.0f / l) : V3(0, 0, 1); }
V3 Mirror(const V3 &a) { return V3(-a.x, a.y, a.z); }

struct Vtx
{
    float p[3];
    float c[4];
    float uv[2];
    float mode; // 0 = lit shell (procedural wear), 1 = atlas (text / bars), premultiplied
};

typedef std::vector<Vtx> Geo;

const V3 kLight = Norm(V3(-0.30f, -0.85f, -0.45f)); // towards the light: up, back, left

void PushV(Geo &g, const V3 &p, const float c[4], float u, float v, float mode)
{
    Vtx t;
    t.p[0] = p.x; t.p[1] = p.y; t.p[2] = p.z;
    memcpy(t.c, c, sizeof(t.c));
    t.uv[0] = u; t.uv[1] = v;
    t.mode = mode;
    g.push_back(t);
}

// Lit, flat-shaded quad (a b c d in order). The normal is turned towards the
// eye point (origin): only the side facing the seat is ever seen.
void Quad(Geo &g, const V3 &a, const V3 &b, const V3 &c, const V3 &d, const V3 &albedo)
{
    V3 n = Norm(Cross(b - a, d - a));
    const V3 ctr = (a + b + c + d) * 0.25f;
    if (Dot(n, -ctr) < 0)
        n = -n;
    const float shade = 1.35f * (0.42f + 0.70f * std::max(0.0f, Dot(n, kLight))); // battle-worn, but readable in a dark headset
    const float col[4] = {albedo.x * shade, albedo.y * shade, albedo.z * shade, 1.0f};
    PushV(g, a, col, 0, 0, 0); PushV(g, b, col, 0, 0, 0); PushV(g, c, col, 0, 0, 0);
    PushV(g, a, col, 0, 0, 0); PushV(g, c, col, 0, 0, 0); PushV(g, d, col, 0, 0, 0);
}

// Box from centre and three half-extent vectors.
void Box(Geo &g, const V3 &c, const V3 &ax, const V3 &ay, const V3 &az, const V3 &albedo)
{
    V3 p[8];
    for (int i = 0; i < 8; i++)
        p[i] = c + ax * ((i & 1) ? 1.f : -1.f) + ay * ((i & 2) ? 1.f : -1.f) + az * ((i & 4) ? 1.f : -1.f);
    Quad(g, p[0], p[1], p[3], p[2], albedo); // -z
    Quad(g, p[4], p[5], p[7], p[6], albedo); // +z
    Quad(g, p[0], p[1], p[5], p[4], albedo); // -y
    Quad(g, p[2], p[3], p[7], p[6], albedo); // +y
    Quad(g, p[0], p[2], p[6], p[4], albedo); // -x
    Quad(g, p[1], p[3], p[7], p[5], albedo); // +x
}

// Beam from a to b: w across (perpendicular to `up` and the beam), h along up.
void Beam(Geo &g, const V3 &a, const V3 &b, float w, float h, const V3 &up, const V3 &albedo)
{
    const V3 d = b - a;
    const V3 dir = Norm(d);
    const V3 ax = Norm(Cross(up, dir)) * (w * 0.5f);
    const V3 ay = Norm(Cross(dir, ax)) * (h * 0.5f);
    Box(g, (a + b) * 0.5f, ax, ay, d * 0.5f, albedo);
}

// A row of bolt heads along a -> b (n bolts), standing off the surface by `out`.
void Bolts(Geo &g, const V3 &a, const V3 &b, int n, const V3 &out, float size, const V3 &albedo)
{
    const V3 o = Norm(out);
    V3 t = Norm(Cross(o, V3(0, 1, 0)));
    if (Len(Cross(o, V3(0, 1, 0))) < 0.1f)
        t = Norm(Cross(o, V3(1, 0, 0)));
    const V3 s = Norm(Cross(o, t));
    for (int i = 0; i < n; i++)
    {
        const float f = n > 1 ? (float)i / (n - 1) : 0.5f;
        const V3 c = a + (b - a) * f + o * (size * 0.4f);
        Box(g, c, t * (size * 0.5f), s * (size * 0.5f), o * (size * 0.4f), albedo);
    }
}

// Flat wall in a plane with a rectangular hole: corners P(u,v) = o + U*u + Vv*v,
// outer [0,1]x[0,1], hole [u0,u1]x[v0,v1].
void WallWithHole(Geo &g, const V3 &o, const V3 &U, const V3 &Vv, float u0, float u1, float v0, float v1, const V3 &albedo)
{
    auto P = [&](float u, float v) { return o + U * u + Vv * v; };
    Quad(g, P(0, 0), P(1, 0), P(1, v0), P(0, v0), albedo);
    Quad(g, P(0, v1), P(1, v1), P(1, 1), P(0, 1), albedo);
    Quad(g, P(0, v0), P(u0, v0), P(u0, v1), P(0, v1), albedo);
    Quad(g, P(u1, v0), P(1, v0), P(1, v1), P(u1, v1), albedo);
}

// A screen facing the eye point: bezel box + dark glass. Text is laid out in
// screen-local metres (x right, y down from the top-left corner).
struct Screen
{
    V3 tl, right, down, fwd;
    float w = 0, h = 0;
    bool ok = false;
};

Screen MakeScreen(Geo &g, const V3 &centre, float w, float h, const V3 &bezel, bool facing = true)
{
    Screen s;
    s.fwd = facing ? Norm(centre) : V3(0, 0, 1);
    s.right = Norm(Cross(V3(0, 1, 0), s.fwd));
    s.down = Norm(Cross(s.fwd, s.right));
    s.w = w;
    s.h = h;
    const float m = 0.014f, depth = 0.020f;
    Box(g, centre + s.fwd * (depth * 0.5f), s.right * (w * 0.5f + m), s.down * (h * 0.5f + m), s.fwd * (depth * 0.5f), bezel);
    const V3 glass(0.030f, 0.042f, 0.038f);
    const V3 gc = centre - s.fwd * 0.002f;
    const V3 r = s.right * (w * 0.5f), d = s.down * (h * 0.5f);
    Quad(g, gc - r - d, gc + r - d, gc + r + d, gc - r + d, glass);
    s.tl = centre - r - d - s.fwd * 0.004f;
    s.ok = true;
    return s;
}

struct Layout
{
    Screen health, weapons, target;
    int first = 0, count = 0; // static VBO range
};
// Indexed by VR::CockpitFrameStyle: [0] = open frames (default), [1] = open +
// overhead rim (ground only; others copy [0]), [2] = classic frames (ground
// and jet; others copy [0]). A copied layout shares the VBO range.
Layout g_layout[FRAME_COUNT][CLS_COUNT];
Layout g_deckHeli; // the look-down helicopter (COMMAND DECK only)
int g_frameEnv = -2;            // UA_VR_COCKPIT_FRAME=open|minimal / rim|open+rim / classic|full; -1 = unset
int g_frameSetting = FRAME_OPEN_RIM; // VR::SetCockpitFrame (the game saves it in the profile); default open + rim
int Frame()
{
    if (g_frameEnv == -2)
    {
        const char *v = getenv("UA_VR_COCKPIT_FRAME");
        g_frameEnv = -1;
        if (v && v[0])
            g_frameEnv = (!strcmp(v, "classic") || !strcmp(v, "full") || !strcmp(v, "0")) ? FRAME_CLASSIC
                       : (!strcmp(v, "rim") || !strcmp(v, "open+rim") || !strcmp(v, "openrim") || !strcmp(v, "2")) ? FRAME_OPEN_RIM
                       : FRAME_OPEN;
    }
    return g_frameEnv >= 0 ? g_frameEnv : g_frameSetting;
}
bool FrameOpen() { return Frame() != FRAME_CLASSIC; }
const char *FrameName(int f) { return f == FRAME_CLASSIC ? "classic" : (f == FRAME_OPEN_RIM ? "open+rim" : "open"); }

// Palette: battle-worn, desaturated; no camo patches (procedural grime/scratches
// in the shader do the wear).
struct Pal { V3 hull, trim, edge, bezel, bolt; };

void BuildGroundClassic(Geo &g, Layout &L)
{
    const Pal P = {V3(0.27f, 0.29f, 0.23f), V3(0.15f, 0.16f, 0.14f), V3(0.40f, 0.40f, 0.36f), V3(0.12f, 0.12f, 0.11f), V3(0.20f, 0.20f, 0.18f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    // dash face (towards the seat) and cowl (forward, under the vision opening)
    Quad(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), V3(0.80f, 0.66f, 0.28f), V3(-0.80f, 0.66f, 0.28f), P.hull);
    Quad(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), V3(0.80f, 0.27f, 0.92f), V3(-0.80f, 0.27f, 0.92f), P.trim);
    Beam(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), 0.035f, 0.030f, up, P.edge); // worn lip
    // lower panel + floor: the hull is closed below the dash
    Quad(g, V3(-0.80f, 0.66f, 0.28f), V3(0.80f, 0.66f, 0.28f), V3(0.80f, 1.10f, 0.12f), V3(-0.80f, 1.10f, 0.12f), P.trim);
    Quad(g, V3(-0.82f, 1.10f, 0.12f), V3(0.82f, 1.10f, 0.12f), V3(0.82f, 1.10f, -0.40f), V3(-0.82f, 1.10f, -0.40f), P.trim);
    // switch row on the dash face (right of centre)
    for (int i = 0; i < 6; i++)
        Box(g, V3(0.10f + i * 0.045f, 0.36f, 0.43f), V3(0.012f, 0, 0), V3(0, 0.012f, -0.006f), V3(0, 0.004f, 0.010f), P.bolt);
    // front frame: pillars + top bar
    for (int s = -1; s <= 1; s += 2)
    {
        Beam(g, V3(s * 0.66f, 0.27f, 0.90f), V3(s * 0.56f, -0.21f, 0.74f), 0.10f, 0.08f, toEye, P.hull);
        // cheek plate between the pillar and the side wall
        Quad(g, V3(s * 0.66f, 0.27f, 0.90f), V3(s * 0.82f, 0.27f, 0.90f), V3(s * 0.82f, -0.28f, 0.74f), V3(s * 0.56f, -0.28f, 0.74f), P.trim);
        // side wall with a vision slit
        WallWithHole(g, V3(s * 0.82f, -0.30f, -0.30f), V3(0, 0, 1.20f), V3(0, 1.00f, 0), 0.30f, 0.78f, 0.24f, 0.42f, P.hull);
        Beam(g, V3(s * 0.81f, -0.06f, 0.06f), V3(s * 0.81f, -0.06f, 0.64f), 0.03f, 0.03f, up, P.edge);
        Beam(g, V3(s * 0.81f, 0.12f, 0.06f), V3(s * 0.81f, 0.12f, 0.64f), 0.03f, 0.03f, up, P.edge);
        Bolts(g, V3(s * 0.80f, -0.10f, 0.08f), V3(s * 0.80f, -0.10f, 0.62f), 6, V3(-(float)s, 0, 0), 0.016f, P.bolt);
    }
    Beam(g, V3(-0.64f, -0.21f, 0.74f), V3(0.64f, -0.21f, 0.74f), 0.10f, 0.13f, up, P.hull);
    Beam(g, V3(-0.60f, -0.145f, 0.69f), V3(0.60f, -0.145f, 0.69f), 0.02f, 0.016f, up, P.edge); // lower edge of the top bar
    Bolts(g, V3(-0.58f, -0.265f, 0.688f), V3(0.58f, -0.265f, 0.688f), 11, toEye, 0.016f, P.bolt);
    // roof with a hatch ring
    Quad(g, V3(-0.82f, -0.28f, 0.74f), V3(0.82f, -0.28f, 0.74f), V3(0.82f, -0.30f, -0.30f), V3(-0.82f, -0.30f, -0.30f), P.trim);
    for (int i = 0; i < 12; i++)
    {
        const float a0 = i * 2 * (float)M_PI / 12, a1 = (i + 1) * 2 * (float)M_PI / 12;
        Beam(g, V3(0.28f * cosf(a0), -0.285f, 0.18f + 0.28f * sinf(a0)), V3(0.28f * cosf(a1), -0.285f, 0.18f + 0.28f * sinf(a1)), 0.04f, 0.03f, up, P.edge);
    }
    L.health = MakeScreen(g, V3(0.0f, -0.21f, 0.685f), 0.56f, 0.090f, P.bezel, false);
    L.weapons = MakeScreen(g, V3(-0.25f, 0.27f, 0.40f), 0.27f, 0.17f, P.bezel);
    Beam(g, V3(-0.25f, 0.33f, 0.42f), V3(-0.25f, 0.36f, 0.47f), 0.05f, 0.03f, up, P.trim); // MFD arm
    L.target = MakeScreen(g, V3(0.46f, -0.08f, 0.52f), 0.27f, 0.23f, P.bezel);
    Beam(g, V3(0.56f, -0.08f, 0.58f), V3(0.60f, -0.10f, 0.76f), 0.03f, 0.03f, up, P.trim); // bracket to the pillar
}

// deck = "look-down" variant (COMMAND DECK only):
// the wide console slab becomes a narrow pedestal so the chin glass either side of it
// and a framed floor window under the seat show the ground below (field orders from
// the air). Classic / the other modes keep deck = false (identical geometry).
void BuildHeli(Geo &g, Layout &L, bool deck = false)
{
    const Pal P = {V3(0.23f, 0.26f, 0.23f), V3(0.13f, 0.14f, 0.13f), V3(0.38f, 0.39f, 0.36f), V3(0.11f, 0.11f, 0.11f), V3(0.19f, 0.19f, 0.18f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    // narrow centre console + glare shield; chin glass either side (open)
    const float cw = deck ? 0.15f : 0.44f, cb = deck ? 0.13f : 0.40f, cn = deck ? 0.12f : 0.34f; // console half-widths: top, bottom, nose
    Quad(g, V3(-cw, 0.28f, 0.56f), V3(cw, 0.28f, 0.56f), V3(cb, 0.70f, 0.34f), V3(-cb, 0.70f, 0.34f), P.hull);
    Quad(g, V3(-cw, 0.28f, 0.56f), V3(cw, 0.28f, 0.56f), V3(cn, 0.27f, 0.80f), V3(-cn, 0.27f, 0.80f), P.trim);
    Beam(g, V3(-cw, 0.28f, 0.56f), V3(cw, 0.28f, 0.56f), 0.03f, 0.025f, up, P.edge);
    if (deck)
    {
        // floor window: a framed opening under the seat (rim at floor level, 0.72 m
        // below the eye), the chin glass frame continued down to it, a centre rib
        const float fy = 0.72f, fx = 0.62f, fz0 = -0.20f, fz1 = 0.66f;
        Beam(g, V3(-fx, fy, fz0), V3(fx, fy, fz0), 0.035f, 0.03f, up, P.hull);
        Beam(g, V3(-fx, fy, fz1), V3(fx, fy, fz1), 0.035f, 0.03f, up, P.hull);
        Beam(g, V3(0, fy, fz0), V3(0, fy, 0.34f), 0.025f, 0.02f, up, P.trim);
        for (int s = -1; s <= 1; s += 2)
        {
            Beam(g, V3(s * fx, fy, fz0), V3(s * fx, fy, fz1), 0.035f, 0.03f, up, P.hull);
            Bolts(g, V3(s * fx, fy - 0.02f, fz0 + 0.05f), V3(s * fx, fy - 0.02f, fz1 - 0.05f), 6, up, 0.010f, P.bolt);
            Beam(g, V3(s * 0.88f, 0.62f, 0.72f), V3(s * fx, fy, fz1), 0.035f, 0.035f, toEye, P.hull); // chin frame foot
        }
    }
    for (int s = -1; s <= 1; s += 2)
    {
        Quad(g, V3(s * cw, 0.28f, 0.56f), V3(s * cn, 0.27f, 0.80f), V3(s * cn, 0.70f, 0.60f), V3(s * cb, 0.70f, 0.34f), P.trim); // console side
        Beam(g, V3(s * 0.88f, 0.62f, 0.72f), V3(s * 0.62f, -0.40f, 0.62f), 0.045f, 0.045f, toEye, P.hull);   // A-pillar
        Beam(g, V3(s * 0.88f, 0.62f, 0.72f), V3(s * 0.34f, 0.60f, 0.95f), 0.04f, 0.04f, up, P.hull);        // chin frame
        if (!deck)
            Beam(g, V3(s * 0.34f, 0.60f, 0.95f), V3(s * 0.34f, 0.27f, 0.80f), 0.035f, 0.035f, toEye, P.trim);  // console nose post
        Beam(g, V3(s * 0.90f, 0.64f, -0.30f), V3(s * 0.90f, 0.64f, 0.72f), 0.05f, 0.05f, up, P.hull);       // sill
        Beam(g, V3(s * 0.88f, 0.63f, 0.15f), V3(s * 0.78f, -0.43f, 0.20f), 0.04f, 0.04f, toEye, P.hull);    // door frame
        Beam(g, V3(s * 0.62f, -0.40f, 0.62f), V3(s * 0.72f, -0.44f, -0.30f), 0.04f, 0.04f, up, P.hull);     // top rail
        Bolts(g, V3(s * 0.86f, 0.55f, 0.70f), V3(s * 0.64f, -0.33f, 0.62f), 7, toEye, 0.012f, P.bolt);
    }
    Beam(g, V3(-0.64f, -0.40f, 0.62f), V3(0.64f, -0.40f, 0.62f), 0.05f, 0.06f, up, P.hull); // top frame
    Beam(g, V3(0, -0.42f, 0.62f), V3(0, -0.46f, -0.30f), 0.05f, 0.04f, up, P.trim);         // roof spine
    Box(g, V3(0, -0.47f, 0.18f), V3(0.24f, 0, 0), V3(0, 0.035f, 0), V3(0, 0, 0.24f), P.trim); // overhead console
    for (int i = 0; i < 5; i++)
        Box(g, V3(-0.16f + i * 0.08f, -0.43f, 0.30f), V3(0.012f, 0, 0), V3(0, 0.008f, 0), V3(0, 0, 0.012f), P.bolt);
    // health strip hung under the top frame on two struts
    Beam(g, V3(-0.20f, -0.40f, 0.61f), V3(-0.20f, -0.31f, 0.60f), 0.015f, 0.015f, toEye, P.trim);
    Beam(g, V3(0.20f, -0.40f, 0.61f), V3(0.20f, -0.31f, 0.60f), 0.015f, 0.015f, toEye, P.trim);
    L.health = MakeScreen(g, V3(0.0f, -0.27f, 0.60f), 0.50f, 0.085f, P.bezel, false);
    L.weapons = MakeScreen(g, V3(-0.27f, 0.33f, 0.40f), 0.27f, 0.17f, P.bezel);
    if (deck)
        Beam(g, V3(-0.16f, 0.38f, 0.43f), V3(-0.12f, 0.40f, 0.47f), 0.04f, 0.03f, up, P.trim); // arm to the pedestal
    else
        Beam(g, V3(-0.27f, 0.39f, 0.42f), V3(-0.27f, 0.42f, 0.49f), 0.05f, 0.03f, up, P.trim);
    L.target = MakeScreen(g, V3(0.49f, -0.12f, 0.52f), 0.27f, 0.23f, P.bezel);
    Beam(g, V3(0.60f, -0.12f, 0.56f), V3(0.70f, -0.12f, 0.64f), 0.025f, 0.025f, up, P.trim);
}

void BuildJetClassic(Geo &g, Layout &L)
{
    const Pal P = {V3(0.28f, 0.30f, 0.32f), V3(0.15f, 0.16f, 0.17f), V3(0.42f, 0.43f, 0.43f), V3(0.11f, 0.11f, 0.12f), V3(0.20f, 0.20f, 0.21f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    // low, wide dash with a glare shield; HUD combiner frame on top
    Quad(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), V3(0.50f, 0.68f, 0.32f), V3(-0.50f, 0.68f, 0.32f), P.hull);
    Quad(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), V3(0.30f, 0.28f, 0.88f), V3(-0.30f, 0.28f, 0.88f), P.trim);
    Beam(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), 0.03f, 0.025f, up, P.edge);
    Quad(g, V3(-0.50f, 0.68f, 0.32f), V3(0.50f, 0.68f, 0.32f), V3(0.56f, 1.00f, 0.10f), V3(-0.56f, 1.00f, 0.10f), P.trim); // footwell
    for (int s = -1; s <= 1; s += 2)
    {
        Beam(g, V3(s * 0.11f, 0.285f, 0.78f), V3(s * 0.11f, 0.15f, 0.80f), 0.012f, 0.012f, toEye, P.trim);
        Quad(g, V3(s * 0.50f, 0.30f, 0.55f), V3(s * 0.30f, 0.28f, 0.88f), V3(s * 0.55f, 0.27f, 0.75f), V3(s * 0.55f, 0.27f, 0.55f), P.trim);
        Beam(g, V3(s * 0.56f, 0.27f, -0.45f), V3(s * 0.56f, 0.27f, 0.75f), 0.05f, 0.04f, up, P.edge);               // canopy rail
        Box(g, V3(s * 0.48f, 0.42f, 0.10f), V3(0.08f, 0, 0), V3(0, 0.06f, 0), V3(0, 0, 0.32f), P.hull);              // side console
        Bolts(g, V3(s * 0.48f, 0.355f, -0.15f), V3(s * 0.48f, 0.355f, 0.35f), 5, up, 0.012f, P.bolt);
    }
    Beam(g, V3(-0.11f, 0.15f, 0.80f), V3(0.11f, 0.15f, 0.80f), 0.012f, 0.012f, up, P.trim);
    // canopy bow (windscreen arch) and a rear hoop
    const int N = 14;
    for (int hoop = 0; hoop < 2; hoop++)
    {
        const float z0 = hoop ? -0.15f : 0.72f, w = hoop ? 0.05f : 0.06f;
        for (int i = 0; i < N; i++)
        {
            const float t0 = (float)M_PI * i / N, t1 = (float)M_PI * (i + 1) / N;
            const V3 a(0.56f * cosf(t0), 0.27f - 0.70f * sinf(t0), z0 - 0.06f * sinf(t0));
            const V3 b(0.56f * cosf(t1), 0.27f - 0.70f * sinf(t1), z0 - 0.06f * sinf(t1));
            Beam(g, a, b, w, w, toEye, P.hull);
        }
    }
    Beam(g, V3(0, -0.43f, 0.66f), V3(0, -0.43f, -0.21f), 0.03f, 0.03f, up, P.trim); // canopy spine
    Beam(g, V3(-0.18f, -0.42f, 0.655f), V3(-0.18f, -0.31f, 0.645f), 0.012f, 0.012f, toEye, P.trim);
    Beam(g, V3(0.18f, -0.42f, 0.655f), V3(0.18f, -0.31f, 0.645f), 0.012f, 0.012f, toEye, P.trim);
    L.health = MakeScreen(g, V3(0.0f, -0.27f, 0.64f), 0.46f, 0.080f, P.bezel, false);
    L.weapons = MakeScreen(g, V3(-0.26f, 0.33f, 0.40f), 0.27f, 0.17f, P.bezel);
    Beam(g, V3(-0.26f, 0.39f, 0.42f), V3(-0.26f, 0.42f, 0.49f), 0.05f, 0.03f, up, P.trim);
    L.target = MakeScreen(g, V3(0.45f, -0.10f, 0.55f), 0.27f, 0.23f, P.bezel);
    Beam(g, V3(0.55f, -0.10f, 0.60f), V3(0.50f, -0.20f, 0.66f), 0.02f, 0.02f, up, P.trim);
}

// open frames ------------------------------
// the ground/jet frames blocked the view at the
// top, where players look up at aircraft. The "open" frames keep only what sits
// BELOW eye level: dash, cowl (the hood line), a low coaming/sill and the three
// screens, all on the dash under the hood line. Nothing above the horizon.
// The classic frames stay available (UA_VR_COCKPIT_FRAME=classic or
// VR::SetCockpitFrame(FRAME_CLASSIC)).

// Screens shared by the open frames: health strip in the centre, weapons lower
// left, target/radio lower right (about 39 deg out, 28 deg down). Angular gap
// between the health strip and the side screens.
// on the ground frames the health strip sat in the same direction as the 2D
// panel's tool bar (about 24.5-26 deg down, 1 m further away): binocular rivalry,
// the bar's icons were lost on the HULL text. Ground
// frames now mount it low on the dash face (centre about 36 deg down, top edge
// about 30.5 deg), clear of the bar band; jets keep it under the hood line.
void OpenDashScreens(Geo &g, Layout &L, const Pal &P, float yHealth, bool lowOnDash = false)
{
    const V3 up(0, -1, 0);
    if (lowOnDash)
    {
        L.health = MakeScreen(g, V3(0.0f, yHealth, 0.43f), 0.34f, 0.080f, P.bezel);
        Beam(g, V3(0.0f, yHealth, 0.45f), V3(0.0f, yHealth, 0.48f), 0.10f, 0.04f, up, P.trim); // mount into the dash face
    }
    else
    {
        L.health = MakeScreen(g, V3(0.0f, yHealth, 0.47f), 0.34f, 0.080f, P.bezel);
        Beam(g, V3(0.0f, yHealth + 0.050f, 0.49f), V3(0.0f, yHealth + 0.065f, 0.55f), 0.08f, 0.02f, up, P.trim); // foot onto the lip
    }
    // ground: side screens 8 cm further out (inner edge about 31 deg out), clear of the bar's ends
    const float xs = lowOnDash ? 0.40f : 0.32f;
    L.weapons = MakeScreen(g, V3(-xs, 0.27f, 0.40f), 0.27f, 0.17f, P.bezel);
    Beam(g, V3(-xs, 0.33f, 0.42f), V3(-xs, 0.36f, 0.47f), 0.05f, 0.03f, up, P.trim); // MFD arm
    L.target = MakeScreen(g, V3(xs, 0.27f, 0.40f), 0.27f, 0.23f, P.bezel);
    Beam(g, V3(xs, 0.36f, 0.42f), V3(xs, 0.38f, 0.46f), 0.05f, 0.03f, up, P.trim);
}

void BuildGroundOpen(Geo &g, Layout &L)
{
    const Pal P = {V3(0.27f, 0.29f, 0.23f), V3(0.15f, 0.16f, 0.14f), V3(0.40f, 0.40f, 0.36f), V3(0.12f, 0.12f, 0.11f), V3(0.20f, 0.20f, 0.18f)};
    const V3 up(0, -1, 0);
    // dash face, cowl (hood line 16 deg down at the centre), worn lip; closed below (as classic)
    Quad(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), V3(0.80f, 0.66f, 0.28f), V3(-0.80f, 0.66f, 0.28f), P.hull);
    Quad(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), V3(0.80f, 0.27f, 0.92f), V3(-0.80f, 0.27f, 0.92f), P.trim);
    Beam(g, V3(-0.80f, 0.24f, 0.50f), V3(0.80f, 0.24f, 0.50f), 0.035f, 0.030f, up, P.edge);
    Quad(g, V3(-0.80f, 0.66f, 0.28f), V3(0.80f, 0.66f, 0.28f), V3(0.80f, 1.10f, 0.12f), V3(-0.80f, 1.10f, 0.12f), P.trim);
    Quad(g, V3(-0.82f, 1.10f, 0.12f), V3(0.82f, 1.10f, 0.12f), V3(0.82f, 1.10f, -0.40f), V3(-0.82f, 1.10f, -0.40f), P.trim);
    // splash guard along the cowl's front edge, bolted
    Beam(g, V3(-0.80f, 0.255f, 0.91f), V3(0.80f, 0.255f, 0.91f), 0.04f, 0.03f, up, P.edge);
    Bolts(g, V3(-0.70f, 0.262f, 0.84f), V3(0.70f, 0.262f, 0.84f), 13, up, 0.016f, P.bolt);
    // switch row, low centre of the dash face (the side screens use its old spot)
    for (int i = 0; i < 6; i++)
        Box(g, V3(-0.11f + i * 0.044f, 0.50f, 0.350f), V3(0.012f, 0, 0), V3(0, 0.012f, -0.006f), V3(0, 0.004f, 0.010f), P.bolt);
    // low coaming around the seat: top 0.12 m below the eye (8 deg down at the side)
    const float yc = 0.12f;
    for (int s = -1; s <= 1; s += 2)
    {
        Quad(g, V3(s * 0.82f, yc, -0.30f), V3(s * 0.82f, yc, 0.92f), V3(s * 0.82f, 1.10f, 0.92f), V3(s * 0.82f, 1.10f, -0.30f), P.hull);
        Beam(g, V3(s * 0.81f, yc, -0.30f), V3(s * 0.81f, yc, 0.92f), 0.05f, 0.03f, up, P.edge); // worn lip
        Bolts(g, V3(s * 0.80f, yc + 0.05f, -0.20f), V3(s * 0.80f, yc + 0.05f, 0.82f), 7, V3(-(float)s, 0, 0), 0.016f, P.bolt);
        Beam(g, V3(s * 0.81f, yc + 0.20f, -0.20f), V3(s * 0.81f, yc + 0.20f, 0.82f), 0.03f, 0.03f, up, P.trim); // stiffener
    }
    Quad(g, V3(-0.82f, yc, -0.30f), V3(0.82f, yc, -0.30f), V3(0.82f, 1.10f, -0.30f), V3(-0.82f, 1.10f, -0.30f), P.hull); // rear
    Beam(g, V3(-0.82f, yc, -0.29f), V3(0.82f, yc, -0.29f), 0.05f, 0.03f, up, P.edge);
    OpenDashScreens(g, L, P, 0.32f, true);
}

// "OPEN + RIM": the open ground frame plus a thin overhead rim (no roof, no
// pillars in the forward view): four slim corner posts and a rectangular hoop at
// roof height. The front bar is about 24 deg above the horizon, about 2 deg thick;
// the sky stays open through the middle.
void BuildGroundOpenRim(Geo &g, Layout &L)
{
    BuildGroundOpen(g, L);
    const Pal P = {V3(0.27f, 0.29f, 0.23f), V3(0.15f, 0.16f, 0.14f), V3(0.40f, 0.40f, 0.36f), V3(0.12f, 0.12f, 0.11f), V3(0.20f, 0.20f, 0.18f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    const float yr = -0.36f, zf = 0.80f, zb = -0.28f, xs = 0.79f;
    for (int s = -1; s <= 1; s += 2)
    {
        Beam(g, V3(s * 0.80f, 0.255f, 0.90f), V3(s * xs, yr, zf), 0.035f, 0.035f, toEye, P.hull); // front post from the cowl corner
        Beam(g, V3(s * 0.81f, 0.12f, zb), V3(s * xs, yr, zb), 0.035f, 0.035f, toEye, P.hull);     // rear post from the coaming
        Beam(g, V3(s * xs, yr, zb), V3(s * xs, yr, zf), 0.035f, 0.030f, up, P.hull);              // side rail
        Bolts(g, V3(s * xs, yr + 0.02f, zb + 0.10f), V3(s * xs, yr + 0.02f, zf - 0.10f), 6, V3(-(float)s, 0, 0), 0.012f, P.bolt);
    }
    Beam(g, V3(-xs, yr, zf), V3(xs, yr, zf), 0.030f, 0.035f, up, P.hull); // front bar
    Beam(g, V3(-xs + 0.04f, yr + 0.022f, zf - 0.004f), V3(xs - 0.04f, yr + 0.022f, zf - 0.004f), 0.012f, 0.010f, up, P.edge); // worn lower edge
    Beam(g, V3(-xs, yr, zb), V3(xs, yr, zb), 0.030f, 0.030f, up, P.hull); // rear bar
}

void BuildJetOpen(Geo &g, Layout &L)
{
    const Pal P = {V3(0.28f, 0.30f, 0.32f), V3(0.15f, 0.16f, 0.17f), V3(0.42f, 0.43f, 0.43f), V3(0.11f, 0.11f, 0.12f), V3(0.20f, 0.20f, 0.21f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    // low, wide dash with a glare shield (hood line 18 deg down); HUD combiner frame on top (as classic)
    Quad(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), V3(0.50f, 0.68f, 0.32f), V3(-0.50f, 0.68f, 0.32f), P.hull);
    Quad(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), V3(0.30f, 0.28f, 0.88f), V3(-0.30f, 0.28f, 0.88f), P.trim);
    Beam(g, V3(-0.50f, 0.30f, 0.55f), V3(0.50f, 0.30f, 0.55f), 0.03f, 0.025f, up, P.edge);
    Quad(g, V3(-0.50f, 0.68f, 0.32f), V3(0.50f, 0.68f, 0.32f), V3(0.56f, 1.00f, 0.10f), V3(-0.56f, 1.00f, 0.10f), P.trim);
    for (int s = -1; s <= 1; s += 2)
    {
        Beam(g, V3(s * 0.11f, 0.285f, 0.78f), V3(s * 0.11f, 0.15f, 0.80f), 0.012f, 0.012f, toEye, P.trim);
        Quad(g, V3(s * 0.50f, 0.30f, 0.55f), V3(s * 0.30f, 0.28f, 0.88f), V3(s * 0.55f, 0.27f, 0.75f), V3(s * 0.55f, 0.27f, 0.55f), P.trim);
        Beam(g, V3(s * 0.56f, 0.27f, -0.45f), V3(s * 0.56f, 0.27f, 0.75f), 0.05f, 0.04f, up, P.edge);   // canopy rail
        Box(g, V3(s * 0.50f, 0.42f, 0.10f), V3(0.06f, 0, 0), V3(0, 0.06f, 0), V3(0, 0, 0.32f), P.hull); // side console (narrower: clears the screens)
        Bolts(g, V3(s * 0.50f, 0.355f, -0.15f), V3(s * 0.50f, 0.355f, 0.35f), 5, up, 0.012f, P.bolt);
    }
    Beam(g, V3(-0.11f, 0.15f, 0.80f), V3(0.11f, 0.15f, 0.80f), 0.012f, 0.012f, up, P.trim);
    // frameless bubble (F-16 style): no windscreen bow, no spine. Only a thin
    // rear hoop well behind the head (about 133 deg from forward,
    // outside the view even at 70 deg head pitch).
    const int N = 14;
    for (int i = 0; i < N; i++)
    {
        const float t0 = (float)M_PI * i / N, t1 = (float)M_PI * (i + 1) / N;
        const V3 a(0.56f * cosf(t0), 0.27f - 0.70f * sinf(t0), -0.40f - 0.06f * sinf(t0));
        const V3 b(0.56f * cosf(t1), 0.27f - 0.70f * sinf(t1), -0.40f - 0.06f * sinf(t1));
        Beam(g, a, b, 0.04f, 0.04f, toEye, P.hull);
    }
    OpenDashScreens(g, L, P, 0.20f);
}

void BuildGun(Geo &g, Layout &L)
{
    const Pal P = {V3(0.30f, 0.30f, 0.27f), V3(0.17f, 0.17f, 0.16f), V3(0.44f, 0.43f, 0.39f), V3(0.12f, 0.12f, 0.11f), V3(0.22f, 0.22f, 0.20f)};
    const V3 up(0, -1, 0), toEye(0, 0, -1);
    const float zf = 0.72f, zt = 0.90f, x0 = 0.52f, yT = -0.22f, yB = 0.20f; // slit: +17 / -15 deg
    // armour plate with a wide slit, and the slit's thickness
    WallWithHole(g, V3(-0.95f, -0.55f, zf), V3(1.90f, 0, 0), V3(0, 1.35f, 0), (0.95f - x0) / 1.90f, (0.95f + x0) / 1.90f,
                 (yT + 0.55f) / 1.35f, (yB + 0.55f) / 1.35f, P.hull);
    Quad(g, V3(-x0, yT, zf), V3(x0, yT, zf), V3(x0, yT, zt), V3(-x0, yT, zt), P.trim);
    Quad(g, V3(-x0, yB, zf), V3(x0, yB, zf), V3(x0, yB, zt), V3(-x0, yB, zt), P.trim);
    for (int s = -1; s <= 1; s += 2)
    {
        Quad(g, V3(s * x0, yT, zf), V3(s * x0, yB, zf), V3(s * x0, yB, zt), V3(s * x0, yT, zt), P.trim);
        Quad(g, V3(s * 0.95f, -0.55f, -0.30f), V3(s * 0.95f, -0.55f, zf), V3(s * 0.95f, 0.80f, zf), V3(s * 0.95f, 0.80f, -0.30f), P.hull);
        Beam(g, V3(s * x0, yT, zf - 0.015f), V3(s * x0, yB, zf - 0.015f), 0.03f, 0.03f, toEye, P.edge);
        Bolts(g, V3(s * 0.88f, -0.48f, zf), V3(s * 0.88f, 0.70f, zf), 9, toEye, 0.020f, P.bolt);
        Beam(g, V3(s * 0.94f, -0.20f, -0.25f), V3(s * 0.94f, -0.20f, 0.70f), 0.04f, 0.05f, up, P.trim); // side rib
    }
    Beam(g, V3(-x0, yT, zf - 0.015f), V3(x0, yT, zf - 0.015f), 0.03f, 0.03f, up, P.edge);
    Beam(g, V3(-x0, yB, zf - 0.015f), V3(x0, yB, zf - 0.015f), 0.03f, 0.03f, up, P.edge);
    // faded warning paint along the slit sill (worn, not camo)
    Quad(g, V3(-x0, yB + 0.018f, zf - 0.031f), V3(x0, yB + 0.018f, zf - 0.031f), V3(x0, yB + 0.05f, zf - 0.031f), V3(-x0, yB + 0.05f, zf - 0.031f),
         V3(0.46f, 0.38f, 0.15f));
    Bolts(g, V3(-0.80f, -0.48f, zf), V3(0.80f, -0.48f, zf), 13, toEye, 0.020f, P.bolt);
    Bolts(g, V3(-0.80f, 0.72f, zf), V3(0.80f, 0.72f, zf), 13, toEye, 0.020f, P.bolt);
    Quad(g, V3(-0.95f, -0.55f, zf), V3(0.95f, -0.55f, zf), V3(0.95f, -0.58f, -0.30f), V3(-0.95f, -0.58f, -0.30f), P.trim); // roof
    // breech housing below the slit
    Box(g, V3(0, 0.46f, 0.50f), V3(0.11f, 0, 0), V3(0, 0.10f, 0), V3(0, 0, 0.22f), P.trim);
    Beam(g, V3(-0.11f, 0.36f, 0.30f), V3(0.11f, 0.36f, 0.30f), 0.02f, 0.02f, up, P.edge);
    L.health = MakeScreen(g, V3(0.0f, -0.315f, 0.70f), 0.56f, 0.090f, P.bezel, false);
    L.weapons = MakeScreen(g, V3(-0.34f, 0.34f, 0.62f), 0.27f, 0.17f, P.bezel);
    L.target = MakeScreen(g, V3(0.66f, -0.04f, 0.62f), 0.27f, 0.23f, P.bezel);
}

// ---- GL ------------------------------------------------------------------

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
    "in float aMode;\n"
    "out vec4 vCol;\n"
    "out vec2 vUV;\n"
    "out vec3 vP;\n"
    "out float vMode;\n"
    "void main() { vCol = aCol; vUV = aUV; vP = aPos; vMode = aMode; gl_Position = MVP * vec4(aPos, 1.0); }\n";

// Shell: flat-lit albedo (vertex colour) x procedural wear: grime blotches,
// vertical streaks, fine scratches catching the light. Atlas: premultiplied.
const char *kFs =
    "uniform sampler2D atlas;\n"
    "in vec4 vCol;\n"
    "in vec2 vUV;\n"
    "in vec3 vP;\n"
    "in float vMode;\n"
    "out vec4 frag;\n"
    "float h3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }\n"
    "float n3(vec3 x) { vec3 i = floor(x); vec3 f = fract(x); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(mix(h3(i), h3(i + vec3(1,0,0)), f.x), mix(h3(i + vec3(0,1,0)), h3(i + vec3(1,1,0)), f.x), f.y),\n"
    "             mix(mix(h3(i + vec3(0,0,1)), h3(i + vec3(1,0,1)), f.x), mix(h3(i + vec3(0,1,1)), h3(i + vec3(1,1,1)), f.x), f.y), f.z); }\n"
    "void main() {\n"
    "  if (vMode > 0.5) {\n"
    "    float a = texture(atlas, vUV).r * vCol.a;\n"
    "    frag = vec4(vCol.rgb * a, a);\n"
    "    return;\n"
    "  }\n"
    "  vec3 p = vP;\n"
    "  float g = n3(p * 7.0) * 0.6 + n3(p * 21.0) * 0.4;\n"
    "  vec3 c = vCol.rgb * mix(0.68, 1.12, g);\n"
    "  float st = n3(vec3(p.x * 48.0, p.y * 2.2, p.z * 48.0));\n"
    "  c *= mix(1.0, 0.70, smoothstep(0.60, 0.86, st));\n"
    "  float sc = n3(vec3(p.x * 150.0 + p.z * 70.0, p.y * 150.0 - p.z * 40.0, p.z * 12.0));\n"
    "  c = mix(c, min(c * 1.9 + 0.04, vec3(1.0)), smoothstep(0.84, 0.92, sc) * 0.6);\n"
    "  frag = vec4(c, 1.0);\n"
    "}\n";

GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_vaoDyn = 0, g_vboDyn = 0, g_tex = 0;
GLint g_locMVP = -1, g_locAtlas = -1;
// Frame style f has its own geometry for class c (else it shares the open one).
bool FrameDiffers(int f, int c)
{
    if (f == FRAME_OPEN) return true;
    if (f == FRAME_OPEN_RIM) return c == CLS_GROUND;
    return c == CLS_GROUND || c == CLS_JET;
}

// frame = VR::CockpitFrameStyle: open, open + rim (ground), classic
void BuildClass(int frame, int c, Geo &g, Layout &L)
{
    if (c == CLS_GROUND) (frame == FRAME_CLASSIC ? BuildGroundClassic : frame == FRAME_OPEN_RIM ? BuildGroundOpenRim : BuildGroundOpen)(g, L);
    else if (c == CLS_HELI) BuildHeli(g, L);
    else if (c == CLS_JET) (frame == FRAME_CLASSIC ? BuildJetClassic : BuildJetOpen)(g, L);
    else BuildGun(g, L);
}

// UA_VR_COCKPIT_MEASURE=1: shell occlusion numbers ------------------
// Rays from the seated (centre) eye point against every shell triangle, screens
// included. Elevation + = up, yaw + = right. No GL needed.
struct Occ
{
    std::vector<V3> t; // triangle corners
    explicit Occ(const Geo &g)
    {
        for (const Vtx &v : g)
            t.push_back(V3(v.p[0], v.p[1], v.p[2]));
    }
    bool Hit(const V3 &d) const
    {
        for (size_t i = 0; i + 2 < t.size(); i += 3)
        {
            const V3 e1 = t[i + 1] - t[i], e2 = t[i + 2] - t[i];
            const V3 p = Cross(d, e2);
            const float det = Dot(e1, p);
            if (fabsf(det) < 1e-9f)
                continue;
            const float inv = 1.0f / det;
            const V3 s = -t[i];
            const float u = Dot(s, p) * inv;
            if (u < 0 || u > 1)
                continue;
            const V3 q = Cross(s, e1);
            const float v = Dot(d, q) * inv;
            if (v < 0 || u + v > 1)
                continue;
            if (Dot(e2, q) * inv > 0.01f)
                return true;
        }
        return false;
    }
};

V3 Dir(float yawDeg, float elevDeg)
{
    const float y = yawDeg * (float)M_PI / 180, e = elevDeg * (float)M_PI / 180;
    return V3(sinf(y) * cosf(e), -sinf(e), cosf(y) * cosf(e));
}

void MeasureFrames()
{
    const float tx = tanf(50.0f * (float)M_PI / 180), ty = tanf(48.0f * (float)M_PI / 180); // simulator FOV 50/50/48/48
    for (int c = CLS_GROUND; c < CLS_COUNT; c++)
        for (int f = 0; f <= FRAME_COUNT; f++) // f == FRAME_COUNT: the COMMAND DECK look-down heli (sites)
        {
            const bool deckHeli = f == FRAME_COUNT;
            if (deckHeli ? c != CLS_HELI : (f != FRAME_OPEN && !FrameDiffers(f, c)))
                continue;
            Geo g;
            Layout L;
            if (deckHeli)
                BuildHeli(g, L, true);
            else
                BuildClass(f, c, g, L);
            const Occ o(g);
            const char *nm = deckHeli ? "deck" : (FrameDiffers(FRAME_CLASSIC, c) || FrameDiffers(FRAME_OPEN_RIM, c)) ? FrameName(f) : "only";
            // upper hemisphere (solid-angle weighted), all round and forward half
            double w = 0, wc = 0, wf = 0, wfc = 0;
            for (float yw = -179.0f; yw < 180.0f; yw += 2.0f)
                for (float e = 1.0f; e < 90.0f; e += 2.0f)
                {
                    const double ww = cos(e * M_PI / 180);
                    const bool clear = !o.Hit(Dir(yw, e));
                    w += ww; wc += clear ? ww : 0;
                    if (fabsf(yw) <= 90) { wf += ww; wfc += clear ? ww : 0; }
                }
            // vertical profile at yaw 0 / 30 / 60: hood line and first block above it
            char prof[3][64];
            const float yaws[3] = {0, 30, 60};
            for (int k = 0; k < 3; k++)
            {
                // the clear band through the horizon: from the highest blocked
                // elevation below it to the lowest blocked one above it
                float lo = -90, hi = 90;
                for (float e = 0; e >= -90; e -= 0.25f)
                    if (o.Hit(Dir(yaws[k], e))) { lo = e; break; }
                for (float e = 0; e <= 90; e += 0.25f)
                    if (o.Hit(Dir(yaws[k], e))) { hi = e; break; }
                if (lo == 0)
                    snprintf(prof[k], sizeof(prof[k]), "yaw %2.0f: horizon blocked", yaws[k]);
                else
                    snprintf(prof[k], sizeof(prof[k]), "yaw %2.0f: clear %+.1f..%+.1f (up to %.1f above the horizon)", yaws[k], lo, hi, hi);
            }
            // horizontal: clear yaw degrees at elevation +5 and +20
            int h5 = 0, h20 = 0;
            for (float yw = -179.75f; yw < 180; yw += 0.5f)
            {
                h5 += !o.Hit(Dir(yw, 5));
                h20 += !o.Hit(Dir(yw, 20));
            }
            // head poses: share of the view (sim FOV) that is unobstructed / sky
            char pose[3][96];
            const float pitches[3] = {0, 45, 70};
            for (int k = 0; k < 3; k++)
            {
                const float p = pitches[k] * (float)M_PI / 180;
                int n = 0, nClear = 0, nSky = 0, nSkyClear = 0;
                for (int iy = 0; iy < 97; iy++)
                    for (int ix = 0; ix < 101; ix++)
                    {
                        const float vx = tx * (ix / 50.0f - 1), vy = ty * (iy / 48.0f - 1); // vy + = down in the image
                        // camera: forward pitched up by p; y down
                        V3 d(vx, vy * cosf(p) - sinf(p), vy * sinf(p) + cosf(p));
                        d = Norm(d);
                        const bool clear = !o.Hit(d);
                        n++; nClear += clear;
                        if (d.y < 0) { nSky++; nSkyClear += clear; }
                    }
                snprintf(pose[k], sizeof(pose[k]), "pitch %2.0f: view clear %5.1f%%, above-horizon part clear %5.1f%%", pitches[k],
                         100.0 * nClear / n, nSky ? 100.0 * nSkyClear / nSky : 0.0);
            }
            fprintf(stderr, "UAVR cockpit measure: %-6s %-7s tris %4d | upper hemisphere clear %5.1f%% (forward half %5.1f%%) | "
                            "horizontal clear at +5 deg %5.1f deg, at +20 deg %5.1f deg\n",
                    kClassName[c], nm, (int)g.size() / 3, 100 * wc / w, 100 * wfc / wf, h5 * 0.5, h20 * 0.5);
            for (int k = 0; k < 3; k++)
                fprintf(stderr, "UAVR cockpit measure: %-6s %-7s   %s | %s\n", kClassName[c], nm, prof[k], pose[k]);
            // looking DOWN (field orders from the air): share of the view (sim
            // FOV) that is unobstructed for head yaw / pitch below the horizon
            {
                const float looks[6][2] = {{0, -45}, {0, -70}, {0, -89}, {75, -45}, {-75, -45}, {75, -70}};
                std::string ln;
                for (const auto &lk : looks)
                {
                    const float yw = lk[0] * (float)M_PI / 180, p = -lk[1] * (float)M_PI / 180; // p > 0 = down
                    int n = 0, nClear = 0;
                    for (int iy = 0; iy < 97; iy += 2)
                        for (int ix = 0; ix < 101; ix += 2)
                        {
                            const float vx = tx * (ix / 50.0f - 1), vy = ty * (iy / 48.0f - 1);
                            // camera pitched down by p (y down), then yawed
                            const V3 d0(vx, vy * cosf(p) + sinf(p), -vy * sinf(p) + cosf(p));
                            const V3 d = Norm(V3(d0.x * cosf(yw) + d0.z * sinf(yw), d0.y, -d0.x * sinf(yw) + d0.z * cosf(yw)));
                            n++;
                            nClear += !o.Hit(d);
                        }
                    char b[48];
                    snprintf(b, sizeof(b), " yaw %+.0f pitch %+.0f: %5.1f%%;", lk[0], lk[1], 100.0 * nClear / n);
                    ln += b;
                }
                fprintf(stderr, "UAVR cockpit measure: %-6s %-7s   look-down view clear:%s\n", kClassName[c], nm, ln.c_str());
            }
        }
}

bool g_glTried = false, g_glOk = false;
Geo g_dyn;
int g_dynCount = 0;

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
        fprintf(stderr, "UAVR cockpit: shader error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

void SetupAttribs()
{
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, p));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, c));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, uv));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, mode));
}

bool InitGL()
{
    g_glTried = true;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
        return false;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.BindAttribLocation(p, 0, "aPos");
    gl.BindAttribLocation(p, 1, "aCol");
    gl.BindAttribLocation(p, 2, "aUV");
    gl.BindAttribLocation(p, 3, "aMode");
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "UAVR cockpit: link failed\n");
        return false;
    }
    g_prog = p;
    g_locMVP = gl.GetUniformLocation(p, "MVP");
    g_locAtlas = gl.GetUniformLocation(p, "atlas");

    // static shells, all classes in one VBO
    Geo all;
    for (int f = 0; f < FRAME_COUNT; f++)
        for (int c = CLS_GROUND; c < CLS_COUNT; c++)
        {
            Layout &L = g_layout[f][c];
            if (!FrameDiffers(f, c))
            {
                L = g_layout[0][c];
                continue;
            }
            Geo g;
            BuildClass(f, c, g, L);
            L.first = (int)all.size();
            L.count = (int)g.size();
            all.insert(all.end(), g.begin(), g.end());
        }
    {
        Geo g;
        BuildHeli(g, g_deckHeli, true);
        g_deckHeli.first = (int)all.size();
        g_deckHeli.count = (int)g.size();
        all.insert(all.end(), g.begin(), g.end());
    }

    GLint prevVao = 0, prevBuf = 0, prevTex = 0, prevActive = 0, prevAlign = 4;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);

    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.GenBuffers(1, &g_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, all.size() * sizeof(Vtx), all.data(), GL_STATIC_DRAW);
    SetupAttribs();
    gl.GenVertexArrays(1, &g_vaoDyn);
    gl.BindVertexArray(g_vaoDyn);
    gl.GenBuffers(1, &g_vboDyn);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vboDyn);
    gl.BufferData(GL_ARRAY_BUFFER, 4096 * sizeof(Vtx), NULL, GL_DYNAMIC_DRAW);
    SetupAttribs();

    // font atlas: 4x upsampled 5x7 glyphs, softened by a 3x3 box filter
    std::vector<uint8_t> hi(kAtlasW * kAtlasH, 0), lo(kAtlasW * kAtlasH, 0);
    for (int ch = 0; ch <= kSolid; ch++)
    {
        const int cx = (ch % kCols) * kCellW * kS, cy = (ch / kCols) * kCellH * kS;
        for (int py = 0; py < kCellH; py++)
            for (int px = 0; px < kCellW; px++)
            {
                bool on;
                if (ch == kSolid)
                    on = true;
                else
                {
                    const int gx = px - 1, gy = py - 1;
                    on = gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (kFont[ch][gy] & (0x10 >> gx));
                }
                if (!on)
                    continue;
                for (int ty = 0; ty < kS; ty++)
                    for (int tx = 0; tx < kS; tx++)
                        hi[(cy + py * kS + ty) * kAtlasW + cx + px * kS + tx] = 255;
            }
    }
    for (int y = 0; y < kAtlasH; y++)
        for (int x = 0; x < kAtlasW; x++)
        {
            int s = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= kAtlasW || yy >= kAtlasH)
                        continue;
                    s += hi[yy * kAtlasW + xx];
                    n++;
                }
            lo[y * kAtlasW + x] = (uint8_t)(s / std::max(1, n));
        }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, lo.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    gl.ActiveTexture(prevActive);
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);

    int tris[FRAME_COUNT][CLS_COUNT] = {{0}};
    for (int f = 0; f < FRAME_COUNT; f++)
        for (int c = CLS_GROUND; c < CLS_COUNT; c++)
            tris[f][c] = g_layout[f][c].count / 3;
    fprintf(stderr, "UAVR cockpit: shells built (triangles ground %d heli %d jet %d gun %d; open+rim ground %d; classic ground %d jet %d), frame %s, atlas %dx%d R8\n",
            tris[0][CLS_GROUND], tris[0][CLS_HELI], tris[0][CLS_JET], tris[0][CLS_GUN], tris[FRAME_OPEN_RIM][CLS_GROUND],
            tris[FRAME_CLASSIC][CLS_GROUND], tris[FRAME_CLASSIC][CLS_JET], FrameName(Frame()), kAtlasW, kAtlasH);
    return true;
}

// ---- text and bars (dynamic) ---------------------------------------------

struct Col { float r, g, b, a; };
const Col kAmber = {1.00f, 0.70f, 0.28f, 1.0f};
const Col kWhite = {0.92f, 0.94f, 0.88f, 1.0f};
const Col kGreen = {0.55f, 0.95f, 0.55f, 1.0f};
const Col kRed = {1.00f, 0.25f, 0.18f, 1.0f};
const Col kCyan = {0.45f, 0.85f, 1.00f, 1.0f};
const Col kDim = {0.50f, 0.52f, 0.46f, 1.0f};

Col Fade(Col c, float a) { c.a *= a; return c; }

void AtlasQuad(Geo &g, const V3 &a, const V3 &b, const V3 &c, const V3 &d, float u0, float v0, float u1, float v1, const Col &col)
{
    const float cc[4] = {col.r, col.g, col.b, col.a};
    PushV(g, a, cc, u0, v0, 1); PushV(g, b, cc, u1, v0, 1); PushV(g, c, cc, u1, v1, 1);
    PushV(g, a, cc, u0, v0, 1); PushV(g, c, cc, u1, v1, 1); PushV(g, d, cc, u0, v1, 1);
}

V3 SP(const Screen &s, float x, float y) { return s.tl + s.right * x + s.down * y; }

void Rect(Geo &g, const Screen &s, float x, float y, float w, float h, const Col &col)
{
    const float cx = (kSolid % kCols) * kCellW + kCellW * 0.5f, cy = (kSolid / kCols) * kCellH + kCellH * 0.5f;
    const float u = cx * kS / kAtlasW, v = cy * kS / kAtlasH;
    AtlasQuad(g, SP(s, x, y), SP(s, x + w, y), SP(s, x + w, y + h), SP(s, x, y + h), u, v, u, v, col);
}

float TextW(const std::string &t, float h) { return t.size() * h * 6.0f / 7.0f; }

// h = cap height (7 font pixels). align: 0 left, 1 right (x = right edge), 2 centre.
void Text(Geo &g, const Screen &s, float x, float y, float h, const std::string &txt, const Col &col, int align = 0)
{
    const float p = h / 7.0f, adv = 6.0f * p;
    if (align == 1)
        x -= TextW(txt, h);
    else if (align == 2)
        x -= TextW(txt, h) * 0.5f;
    for (size_t i = 0; i < txt.size(); i++, x += adv)
    {
        int c = (unsigned char)txt[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c < 32 || c > 95)
            c = '?';
        c -= 32;
        if (c == 0)
            continue;
        const float cx = (float)((c % kCols) * kCellW), cy = (float)((c / kCols) * kCellH);
        // glyph pixels sit at cell (1..5, 1..7); the quad covers (0.5..6.5, 0.5..8.0)
        const float x0 = x - 0.5f * p, x1 = x + 5.5f * p, y0 = y - 0.5f * p, y1 = y + 7.0f * p;
        AtlasQuad(g, SP(s, x0, y0), SP(s, x1, y0), SP(s, x1, y1), SP(s, x0, y1),
                  (cx + 0.5f) * kS / kAtlasW, (cy + 0.5f) * kS / kAtlasH, (cx + 6.5f) * kS / kAtlasW, (cy + 8.0f) * kS / kAtlasH, col);
    }
}

// Segmented bar, frac 0..1.
void Bar(Geo &g, const Screen &s, float x, float y, float w, float h, float frac, const Col &on, int segs)
{
    frac = std::max(0.0f, std::min(1.0f, frac));
    const float gap = std::min(0.004f, w / segs * 0.25f), sw = (w - gap * (segs - 1)) / segs;
    for (int i = 0; i < segs; i++)
    {
        const float lit = std::max(0.0f, std::min(1.0f, frac * segs - i));
        const float sx = x + i * (sw + gap);
        Rect(g, s, sx, y, sw, h, Fade(kDim, 0.30f));
        if (lit > 0)
            Rect(g, s, sx, y, sw * lit, h, on);
    }
}

Col HealthCol(float f, uint32_t t)
{
    if (f > 0.5f) return kGreen;
    if (f > 0.25f) return kAmber;
    return ((t / 250) & 1) ? kRed : Fade(kRed, 0.55f);
}

std::string Fmt(const char *f, int a) { char b[64]; snprintf(b, sizeof(b), f, a); return b; }

void WrapLines(const std::string &t, size_t width, std::vector<std::string> &out, size_t maxLines)
{
    size_t at = 0;
    while (at < t.size() && out.size() < maxLines)
    {
        while (at < t.size() && t[at] == ' ')
            at++;
        if (at >= t.size())
            break;
        size_t end = std::min(t.size(), at + width);
        if (end < t.size())
        {
            size_t sp = t.rfind(' ', end);
            if (sp != std::string::npos && sp > at)
                end = sp;
        }
        out.push_back(t.substr(at, end - at));
        at = end;
    }
}

void BuildDash(Geo &g, const Layout &L, const Snap &S)
{
    const uint32_t t = S.timeMs;
    // --- health, top edge
    {
        const Screen &s = L.health;
        const float hf = (float)S.hp / std::max(1, S.hpMax);
        const float th = std::min(0.026f, s.h * 0.30f);
        Text(g, s, 0.014f, 0.012f, th, "HULL", kAmber);
        const float bx = 0.014f + TextW("HULL ", th);
        const float bw = s.w - bx - TextW(" 100%", th) - 0.014f;
        Bar(g, s, bx, 0.012f, bw, th, hf, HealthCol(hf, t), 20);
        Text(g, s, s.w - 0.014f, 0.012f, th, Fmt("%d%%", (int)(hf * 100.0f + 0.5f)), HealthCol(hf, t), 1);
        const float sh = th * 0.62f, y2 = 0.012f + th + 0.012f;
        Text(g, s, 0.014f, y2, sh, "ARMOR", kDim);
        Bar(g, s, bx, y2 + sh * 0.25f, bw * 0.45f, sh * 0.5f, S.shield / 100.0f, kCyan, 10);
        Text(g, s, bx + bw * 0.45f + 0.010f, y2, sh, Fmt("%d%%", S.shield), kCyan);
        Text(g, s, s.w - 0.014f, y2, sh, S.vehName, kWhite, 1);
    }
    // --- weapons, lower left
    {
        const Screen &s = L.weapons;
        const float th = 0.020f, sm = 0.0155f, x = 0.012f;
        float y = 0.012f;
        Text(g, s, x, y, sm, "WEAPON", kAmber);
        y += sm + 0.008f;
        Text(g, s, x, y, th, S.hasWeapon ? S.weapName : "NONE", kWhite);
        y += th + 0.010f;
        if (S.hasWeapon)
        {
            Text(g, s, x, y, sm, S.nWeap > 1 ? Fmt("DMG %d", S.dmg) + Fmt(" X%d", S.nWeap) : Fmt("DMG %d", S.dmg), kWhite);
            y += sm + 0.010f;
            const bool ready = S.reloadPct >= 100;
            Text(g, s, x, y, sm, "RLD", kAmber);
            const float bx = x + TextW("RLD ", sm);
            Bar(g, s, bx, y, s.w - bx - TextW(" READY", sm) - 0.012f, sm, S.reloadPct / 100.0f, ready ? kGreen : kAmber, 10);
            Text(g, s, s.w - 0.012f, y, sm, ready ? "READY" : Fmt("%d%%", S.reloadPct), ready ? kGreen : kAmber, 1);
            y += sm + 0.010f;
        }
        Text(g, s, x, y, sm, S.hasMg ? "MG  ARMED" : "MG  --", S.hasMg ? kGreen : kDim);
    }
    // --- target + radio, upper right
    {
        const Screen &s = L.target;
        const float th = 0.019f, sm = 0.0145f, x = 0.012f;
        float y = 0.012f;
        Text(g, s, x, y, sm, "TARGET", kAmber);
        if (S.tgt && S.tgtLocked)
        {
            const float lw = TextW("LOCK", sm) + 0.008f;
            Rect(g, s, s.w - 0.012f - lw, y - 0.003f, lw, sm + 0.006f, ((t / 300) & 1) ? kRed : Fade(kRed, 0.7f));
            Text(g, s, s.w - 0.016f, y, sm, "LOCK", Col{0.05f, 0.03f, 0.02f, 1.0f}, 1);
        }
        y += sm + 0.008f;
        if (S.tgt)
        {
            Text(g, s, x, y, th, S.tgtName, S.tgtEnemy ? kRed : kCyan);
            y += th + 0.008f;
            Text(g, s, x, y, sm, Fmt("RNG %dM", (int)(S.tgtDistM + 0.5)), kWhite);
            const float bx = x + TextW("RNG 0000M ", sm);
            Bar(g, s, bx, y, s.w - bx - 0.012f, sm, S.tgtPct / 100.0f, S.tgtEnemy ? kRed : kCyan, 8);
        }
        else
        {
            Text(g, s, x, y, th, "NO TARGET", kDim);
            y += th + 0.008f;
            Text(g, s, x, y, sm, S.rayHit ? Fmt("RNG %dM", (int)(S.rayDistM + 0.5)) : "RNG ---", kDim);
        }
        y += sm + 0.012f;
        Rect(g, s, x, y, s.w - 2 * x, 0.0015f, Fade(kAmber, 0.6f));
        y += 0.008f;
        Text(g, s, x, y, sm, "RADIO", kAmber);
        y += sm + 0.008f;
        const float mh = 0.0125f;
        const size_t width = (size_t)((s.w - 2 * x) / (mh * 6.0f / 7.0f));
        for (int i = 0; i < S.nMsg && y + mh < s.h - 0.006f; i++)
        {
            std::vector<std::string> lines;
            WrapLines(S.msg[i], width, lines, 2);
            const float a = std::max(0.35f, 1.0f - S.msgAge[i] / 30.0f);
            for (const std::string &ln : lines)
            {
                if (y + mh > s.h - 0.006f)
                    break;
                Text(g, s, x, y, mh, ln, Fade(i == 0 ? kWhite : kDim, a));
                y += mh + 0.005f;
            }
            y += 0.003f;
        }
    }
}

// the wrist panel as last built (tracking space in UAV axes, metres), for
// the clickable squad rows (vr_field.cpp). Rows hold the squad commander gid.
struct WristLayout
{
    bool valid = false;
    uint32_t ticks = 0;
    V3 tl, right, down;
    float w = 0, h = 0;
    int nRows = 0;
    float rowY0[8], rowY1[8], rowX0 = 0;
    uint32_t rowGid[8];
};
WristLayout g_wristLayout;
uint32_t g_wristHover = 0;

// Left-wrist panel, in anchor-local metres from the left grip pose.
void BuildWrist(Geo &g, const Snap &S, const ControllerState &c, const ViewState &views)
{
    g_wristLayout.valid = false;
    if (!c.active)
        return;
    double pg[3];
    XrPosToUa(c.gripPose.position, 1.0, pg);
    const M3 R = XrRotToUa(QuatToM3(c.gripPose.orientation)); // columns: right, down, forward (UAV)
    const V3 cr((float)R.m[0][0], (float)R.m[1][0], (float)R.m[2][0]);
    const V3 cd((float)R.m[0][1], (float)R.m[1][1], (float)R.m[2][1]);
    const V3 cf((float)R.m[0][2], (float)R.m[1][2], (float)R.m[2][2]);
    // watch face on top of the wrist, 9 cm behind the grip, tilted 25 deg towards the body
    const V3 C = V3((float)pg[0], (float)pg[1], (float)pg[2]) + cf * -0.09f + cd * -0.035f;
    const float tilt = 25.0f * (float)M_PI / 180.0f;
    const V3 n = Norm(-cd * cosf(tilt) + cr * sinf(tilt)); // towards the viewer
    double ph[3];
    XrPosToUa(views.head.position, 1.0, ph);
    const V3 head((float)ph[0], (float)ph[1], (float)ph[2]);
    const M3 rh = XrRotToUa(QuatToM3(views.head.orientation));
    const V3 gaze((float)rh.m[0][2], (float)rh.m[1][2], (float)rh.m[2][2]);
    const V3 toEye = Norm(head - C);
    const float facing = Dot(n, toEye), look = Dot(gaze, -toEye);
    auto ss = [](float a, float b, float x) { float t = std::max(0.0f, std::min(1.0f, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
    float alpha = ss(0.45f, 0.75f, facing) * ss(0.70f, 0.88f, look);
    // a radio ping lights the face while the wrist is roughly turned up,
    // without having to look straight at it (peripheral flash).
    float pSide = 0, pAhead = 1, pStr = 0;
    int pType = 0;
    const bool ping = VRRadio::WristPing(&pSide, &pAhead, &pStr, &pType);
    if (ping)
        alpha = std::max(alpha, pStr * ss(0.30f, 0.60f, facing));
    // sites (Command Deck only): energy readout + the captured-site lines; the
    // moment lights the face like a radio ping
    Sites::WristInfo site;
    const bool deck = Sites::Wrist(&site);
    if (deck && site.line)
        alpha = std::max(alpha, site.lineAlpha * ss(0.30f, 0.60f, facing));
    if (alpha < 0.02f)
        return;

    Screen s;
    s.fwd = -n;
    s.right = cf;                              // text runs along the forearm
    s.down = Norm(Cross(s.fwd, s.right));
    s.w = 0.16f;
    s.h = 0.112f;
    s.tl = C - s.right * (s.w * 0.5f) - s.down * (s.h * 0.5f);
    // backing plate (drawn in the atlas pass, solid)
    Rect(g, s, -0.004f, -0.004f, s.w + 0.008f, s.h + 0.008f, Col{0.10f, 0.11f, 0.10f, 0.92f * alpha});
    Rect(g, s, 0, 0, s.w, s.h, Col{0.025f, 0.04f, 0.035f, 0.95f * alpha});
    if (Cmd::StationHint())
    {
        // QUICK STATION: a small tab above the face (Command Deck default; Classic off)
        const float th = 0.0078f, tw = TextW("HOLD B: STATION", th) + 0.012f, ty = -0.004f - th - 0.008f;
        Rect(g, s, (s.w - tw) * 0.5f - 0.002f, ty - 0.002f, tw + 0.004f, th + 0.0085f, Col{0.10f, 0.11f, 0.10f, 0.92f * alpha});
        Rect(g, s, (s.w - tw) * 0.5f, ty, tw, th + 0.0045f, Col{0.025f, 0.04f, 0.035f, 0.95f * alpha});
        Text(g, s, s.w * 0.5f, ty + 0.0022f, th, "HOLD B: STATION", Fade(kAmber, alpha), 2);
    }
    const float sm = 0.0092f, x = 0.007f;
    // radar, heading up
    const float rr = 0.029f, rcx = x + rr, rcy = 0.010f + sm + 0.004f + rr;
    Text(g, s, x, 0.006f, sm, "RADAR", Fade(kAmber, alpha));
    for (int i = 0; i < 24; i++)
    {
        const float a0 = i * 2 * (float)M_PI / 24;
        Rect(g, s, rcx + rr * cosf(a0) - 0.0006f, rcy + rr * sinf(a0) - 0.0006f, 0.0012f, 0.0012f, Fade(kDim, alpha));
    }
    Rect(g, s, rcx - 0.0004f, rcy - rr, 0.0008f, 2 * rr, Fade(kDim, 0.4f * alpha));
    Rect(g, s, rcx - rr, rcy - 0.0004f, 2 * rr, 0.0008f, Fade(kDim, 0.4f * alpha));
    for (const Snap::Blip &b : S.blips)
    {
        const float bx = rcx + b.x * rr, by = rcy - b.z * rr, d = b.kind == 2 ? 0.0034f : 0.0024f;
        const Col bc = b.kind == 1 ? kRed : (b.kind == 2 ? kWhite : kCyan);
        Rect(g, s, bx - d * 0.5f, by - d * 0.5f, d, d, Fade(bc, alpha));
    }
    Rect(g, s, rcx - 0.0015f, rcy - 0.0030f, 0.0030f, 0.0045f, Fade(kGreen, alpha)); // you
    if (ping)
    {
        // arrow from the centre to the radar rim towards the event + a pulsing frame
        const Col pc = (pType == VRRadio::EV_SECTOR_TAKEN) ? kGreen : (pType == VRRadio::EV_OTHER || pType == VRRadio::EV_ENEMY_HOST) ? kAmber : kRed;
        const float pulse = 0.55f + 0.45f * cosf(pStr * 6.0f * (float)M_PI);
        const float pa = alpha * pStr * pulse;
        for (int i = 2; i <= 9; i++)
        {
            const float f = i / 9.0f, d = 0.0012f + 0.0010f * f;
            Rect(g, s, rcx + pSide * rr * f - d * 0.5f, rcy - pAhead * rr * f - d * 0.5f, d, d, Fade(pc, pa));
        }
        const float hd = 0.0050f;
        Rect(g, s, rcx + pSide * rr - hd * 0.5f, rcy - pAhead * rr - hd * 0.5f, hd, hd, Fade(pc, pa));
        const float t = 0.0016f;
        Rect(g, s, 0, 0, s.w, t, Fade(pc, pa));
        Rect(g, s, 0, s.h - t, s.w, t, Fade(pc, pa));
        Rect(g, s, 0, 0, t, s.h, Fade(pc, pa));
        Rect(g, s, s.w - t, 0, t, s.h, Fade(pc, pa));
    }
    // squads
    const float sx = x + 2 * rr + 0.010f;
    float y = 0.006f;
    Text(g, s, sx, y, sm, "SQUADS", Fade(kAmber, alpha));
    y += sm + 0.004f;
    const size_t maxCh = (size_t)((s.w - sx - 0.004f) / (sm * 6.0f / 7.0f));
    int shown = 0;
    const bool clickable = WristSquads(); // rows are buttons (vr_field.cpp)
    for (const Snap::Sq &q : S.squads)
    {
        if (y + sm > s.h - 0.022f)
            break;
        if (clickable && shown < 8)
        {
            if (q.sel || q.gid == g_wristHover)
                Rect(g, s, sx - 0.002f, y - 0.0015f, s.w - sx - 0.002f, sm + 0.0045f,
                     q.gid == g_wristHover ? Col{0.25f, 0.55f, 0.75f, 0.55f * alpha} : Col{0.20f, 0.32f, 0.22f, 0.55f * alpha});
            g_wristLayout.rowY0[shown] = y - 0.002f;
            g_wristLayout.rowY1[shown] = y + sm + 0.002f;
            g_wristLayout.rowGid[shown] = q.gid;
        }
        std::string line = (q.mine ? ">" : " ") + q.name.substr(0, std::max<size_t>(1, maxCh - 5)) + Fmt(" %d", q.n);
        Text(g, s, sx, y, sm, line, Fade(q.mine ? kGreen : kWhite, alpha));
        Bar(g, s, sx + 0.002f, y + sm + 0.0008f, s.w - sx - 0.008f, 0.0012f, q.pct / 100.0f, Fade(HealthCol(q.pct / 100.0f, S.timeMs), alpha), 6);
        y += sm + 0.0040f;
        shown++;
    }
    if (!shown)
        Text(g, s, sx, y, sm, "NONE", Fade(kDim, alpha));
    if (clickable && alpha > 0.5f)
    {
        g_wristLayout.valid = true;
        g_wristLayout.ticks = SDL_GetTicks();
        g_wristLayout.tl = s.tl;
        g_wristLayout.right = s.right;
        g_wristLayout.down = s.down;
        g_wristLayout.w = s.w;
        g_wristLayout.h = s.h;
        g_wristLayout.rowX0 = sx - 0.004f;
        g_wristLayout.nRows = std::min(shown, 8);
    }
    // host station energy
    const float hy = s.h - 0.014f;
    Text(g, s, x, hy, sm, "HOST", Fade(kAmber, alpha));
    if (deck)
    {
        // current / max (display units, as the HUD) + rising / falling arrow
        // and the measured rate; a thin fill bar under the line
        char nb[32];
        snprintf(nb, sizeof(nb), "%d/%d", site.energy, site.energyMax);
        const std::string num = nb;
        const Col ac = site.arrow > 0 ? kGreen : (site.arrow < 0 ? kRed : kDim);
        const int r = std::max(-9999, std::min(9999, (int)lround(site.rate)));
        const std::string rt = site.arrow ? Fmt("%+d/S", r) : std::string("");
        // fit the line: "HOST " + number, gap, arrow (1.4 chars), rate; the
        // number fields shrink to fit the face instead of overlapping
        const float avail = s.w - 0.006f - x;
        const float chars = (float)(num.size() + rt.size()) + 1.0f + 1.4f + (rt.empty() ? 0.0f : 0.6f);
        const float fh = std::min(sm, (avail - TextW("HOST ", sm)) * 7.0f / 6.0f / chars);
        const float nx = x + TextW("HOST ", sm);
        Text(g, s, nx, hy + (sm - fh), fh, num, Fade(kCyan, alpha));
        const float rx = s.w - 0.006f;
        if (!rt.empty())
            Text(g, s, rx, hy + (sm - fh), fh, rt, Fade(ac, alpha), 1);
        const float tri = fh * 0.9f;
        const float ax = rx - TextW(rt, fh) - (rt.empty() ? 0.0f : 0.6f * fh * 6.0f / 7.0f) - tri * 0.5f; // arrow centre
        const float ty = hy + (sm - fh);
        for (int k = 0; k < 4; k++) // a 4-row triangle; up = rising
        {
            const float w = tri * (k + 1) / 4.0f, rowH = tri / 4.0f;
            const float yy = site.arrow >= 0 ? ty + k * rowH : ty + (3 - k) * rowH;
            if (site.arrow == 0)
                Rect(g, s, ax - tri * 0.5f, ty + tri * 0.45f, tri, rowH, Fade(ac, alpha)); // steady: a dash
            else
                Rect(g, s, ax - w * 0.5f, yy, w, rowH * 1.05f, Fade(ac, alpha));
            if (site.arrow == 0)
                break;
        }
        Bar(g, s, x, s.h - 0.0035f, s.w - 2 * x, 0.0012f, std::min(1.0f, site.energy / (float)std::max(1, site.energyMax)), Fade(kCyan, alpha), 24);
        if (site.line)
        {
            // the captured-site lines on a strip under the face
            const float la = alpha * site.lineAlpha, y0 = s.h + 0.006f, lh = 0.0085f, sh = 2 * lh + 0.010f;
            Rect(g, s, -0.004f, y0 - 0.004f, s.w + 0.008f, sh + 0.008f, Col{0.10f, 0.11f, 0.10f, 0.92f * la});
            Rect(g, s, 0, y0, s.w, sh, Col{0.04f, 0.10f, 0.05f, 0.95f * la});
            const float t = 0.0014f;
            Rect(g, s, 0, y0, s.w, t, Fade(kGreen, la));
            Rect(g, s, 0, y0 + sh - t, s.w, t, Fade(kGreen, la));
            Text(g, s, s.w * 0.5f, y0 + 0.004f, lh, site.line1.substr(0, 22), Fade(kGreen, la), 2);
            Text(g, s, s.w * 0.5f, y0 + 0.006f + lh, lh, site.line2.substr(0, 22), Fade(kWhite, la), 2);
        }
        return;
    }
    const float bx = x + TextW("HOST ", sm);
    Bar(g, s, bx, hy, s.w - bx - TextW(" 100%", sm) - 0.006f, sm, S.hostPct / 100.0f, Fade(kCyan, alpha), 12);
    Text(g, s, s.w - 0.006f, hy, sm, Fmt("%d%%", S.hostPct), Fade(kCyan, alpha), 1);
}

void UploadMVP(const M4 &mvp)
{
    float m[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[j * 4 + i] = (float)mvp.m[i][j];
    gl.UniformMatrix4fv(g_locMVP, 1, GL_FALSE, m);
}

int ClassOf(int bactType)
{
    switch (bactType)
    {
    case BACT_TYPES_TANK: case BACT_TYPES_CAR: case BACT_TYPES_HOVER: return CLS_GROUND;
    case BACT_TYPES_BACT: return CLS_HELI;
    case BACT_TYPES_FLYER: case BACT_TYPES_UFO: return CLS_JET;
    case BACT_TYPES_GUN: return CLS_GUN;
    default: return CLS_NONE;
    }
}

// Rotation angle (deg) between two row-orthonormal matrices.
double AngleDeg(const mat3x3 &a, const mat3x3 &b)
{
    const double tr = a.m00 * b.m00 + a.m01 * b.m01 + a.m02 * b.m02 + a.m10 * b.m10 + a.m11 * b.m11 + a.m12 * b.m12 +
                      a.m20 * b.m20 + a.m21 * b.m21 + a.m22 * b.m22;
    return acos(std::max(-1.0, std::min(1.0, (tr - 1.0) * 0.5))) * 180.0 / M_PI;
}

// UA_VR_TAKE=<heli|jet|gun|tank|car>[,frame]: dev aid, sit in the first player
// unit of that class after n gameplay frames (unattended cockpit captures).
void TakeUnit(NC_STACK_ypaworld *yw)
{
    static int want = -2, at = 0, frames = 0;
    if (want == -2)
    {
        want = -1;
        if (const char *v = getenv("UA_VR_TAKE"))
        {
            char name[16] = {0};
            int n = 100;
            if (sscanf(v, "%15[a-z],%d", name, &n) >= 1)
            {
                const std::string s(name);
                want = s == "heli" ? BACT_TYPES_BACT : s == "jet" ? BACT_TYPES_FLYER : s == "gun" ? BACT_TYPES_GUN
                     : s == "car" ? BACT_TYPES_CAR : s == "tank" ? BACT_TYPES_TANK : -1;
                at = n;
            }
        }
    }
    if (want < 0 || !yw->_userRobo || !yw->_viewerBact || ++frames < at)
        return;
    NC_STACK_ypabact *pick = NULL;
    std::string seen;
    for (NC_STACK_ypabact *cmd : yw->_userRobo->_kidList)
    {
        auto consider = [&](NC_STACK_ypabact *u) {
            if (u->_status == BACT_STATUS_DEAD)
                return;
            seen += Fmt(" %d", u->_bact_type);
            if (!pick && (u->_bact_type == want || (want == BACT_TYPES_FLYER && u->_bact_type == BACT_TYPES_UFO)))
                pick = u;
        };
        consider(cmd);
        for (NC_STACK_ypabact *u : cmd->_kidList)
            consider(u);
    }
    if (!pick)
    {
        if (frames == at || frames % 300 == 0)
            fprintf(stderr, "UAVR cockpit: UA_VR_TAKE: no player unit of type %d yet (frame %d; types:%s)\n", want, frames, seen.c_str());
        return;
    }
    NC_STACK_ypabact *old = yw->_viewerBact;
    old->setBACT_viewer(false);
    old->setBACT_inputting(false);
    pick->setBACT_viewer(true);
    pick->setBACT_inputting(true);
    fprintf(stderr, "UAVR cockpit: took unit gid %d type %d at gameplay frame %d\n", (int)pick->_gid, (int)pick->_bact_type, frames);
    want = -1;
}

} // namespace

// ---------------------------------------------------------------------------

void Init(Backend *backend, double unitsPerM)
{
    g_backend = backend;
    g_upm = unitsPerM;
    g_log = EnvOn("UA_VR_COCKPIT_LOG", false);
    g_allowShake = EnvOn("UA_VR_ALLOW_SHAKE", false);
    g_hapticsOn = EnvOn("UA_VR_HAPTICS", true);
    if (const char *v = getenv("UA_VR_COCKPIT_CLASS"))
    {
        for (int c = 0; c < CLS_COUNT; c++)
            if (!strcmp(v, kClassName[c]))
                g_forceClass = c;
    }
    fprintf(stderr, "UAVR cockpit: shells %s%s, frame %s, haptics %s%s\n", ShellsOn() ? "on" : "off",
            g_forceClass >= 0 ? " (class forced)" : "", FrameName(Frame()), g_hapticsOn ? "on" : "off",
            g_allowShake ? ", VIEW SHAKE ALLOWED (A/B)" : "");
    if (EnvOn("UA_VR_COCKPIT_MEASURE", false))
        MeasureFrames();
}

bool AllowShake() { return g_allowShake; }

// clickable wrist rows. Ray and hit in XR tracking space (metres).
bool WristPick(const double o[3], const double d[3], uint32_t *gid, double hit[3], double *dist)
{
    const WristLayout &W = g_wristLayout;
    if (!W.valid || W.nRows <= 0 || SDL_GetTicks() - W.ticks > 120)
        return false;
    // the layout is in UAV axes (x right, y down, z forward): flip y, z
    const double ou[3] = {o[0], -o[1], -o[2]}, du[3] = {d[0], -d[1], -d[2]};
    const double tl[3] = {W.tl.x, W.tl.y, W.tl.z}, r[3] = {W.right.x, W.right.y, W.right.z}, dn[3] = {W.down.x, W.down.y, W.down.z};
    double x, y, t;
    if (!FieldLogic::RayRect(ou, du, tl, r, dn, W.w, W.h, &x, &y, &t) || x < W.rowX0)
        return false;
    for (int i = 0; i < W.nRows; i++)
        if (y >= W.rowY0[i] && y < W.rowY1[i])
        {
            *gid = W.rowGid[i];
            hit[0] = ou[0] + du[0] * t;
            hit[1] = -(ou[1] + du[1] * t);
            hit[2] = -(ou[2] + du[2] * t);
            *dist = t;
            return true;
        }
    return false;
}

bool WristRowCentre(int row, double xr[3])
{
    const WristLayout &W = g_wristLayout;
    if (!W.valid || row < 0 || row >= W.nRows)
        return false;
    const float x = (W.rowX0 + W.w) * 0.5f, y = (W.rowY0[row] + W.rowY1[row]) * 0.5f;
    const V3 p = W.tl + W.right * x + W.down * y;
    xr[0] = p.x;
    xr[1] = -p.y;
    xr[2] = -p.z;
    return true;
}

void SetWristHover(uint32_t gid) { g_wristHover = gid; }

void Haptic(int hand, float amplitude, float durationMs, const char *why)
{
    if (!g_backend)
        return;
    g_backend->Haptic(hand, amplitude, durationMs);
    if (hand == 0) // count once per pulse (both hands are always driven together)
    {
        if (!strcmp(why, "hit")) g_la.hit++;
        else if (!strcmp(why, "shake")) g_la.shake++;
        else g_la.ff++;
        if (!strcmp(why, "hit"))
            g_la.hitAmpPeak = std::max(g_la.hitAmpPeak, amplitude);
    }
}

void NoteShake(double angleDeg, bool applied)
{
    g_shakeDeg = angleDeg;
    g_shakeApplied = applied ? angleDeg : 0.0;
    g_la.shakePeak = std::max(g_la.shakePeak, angleDeg);
    g_la.appliedPeak = std::max(g_la.appliedPeak, g_shakeApplied);
}

bool ShellVisible()
{
    return Active() && ShellsOn() && g_snap.cls != CLS_NONE;
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    if (!Active() || !yw)
        return;
    TakeUnit(yw);
    NC_STACK_ypabact *vb = yw->_viewerBact;
    NC_STACK_ypabact *uu = yw->_userUnit;
    Snap S;
    S.timeMs = (uint32_t)yw->_timeStamp;
    if (!vb || !uu)
    {
        g_snap = S;
        return;
    }
    S.cls = g_forceClass >= 0 ? g_forceClass : ClassOf(vb->_bact_type); // viewer: missile camera / station = no shell
    S.hudOn = yw->_hud.field_0 != 0;
    S.dead = uu->_status == BACT_STATUS_DEAD;
    S.hp = std::max(0, uu->_energy);
    S.hpMax = std::max(1, uu->_energy_max);
    S.shield = uu->_shield;
    if (uu->_vehicleID < yw->_vhclProtos.size())
    {
        const World::TVhclProto &vp = yw->_vhclProtos[uu->_vehicleID];
        S.vehName = Upper(yw->GetVehicleName(vp), 14);
        S.nWeap = std::max(1, (int)vp.num_weapons);
        if (vp.weapon != -1 && vp.weapon < (int)yw->_weaponProtos.size())
        {
            const World::TWeapProto &wp = yw->_weaponProtos[vp.weapon];
            S.hasWeapon = true;
            S.weapName = Upper(wp.name, 12);
            S.dmg = wp.energy / 100;
            int per = wp.shot_time_user;
            if (wp.salve_shots && uu->_salve_counter >= wp.salve_shots)
                per = wp.salve_delay;
            S.reloadPct = per > 0 ? std::min(100, std::max(0, 100 * (int)(uu->_clock - uu->_weapon_time) / per)) : 100;
        }
        S.hasMg = vp.mgun != -1;
    }
    // target (the aim module copied it in NoteUserAim)
    const Aim::TargetSnap &ts = Aim::Target();
    if (ts.valid && ts.unit == uu)
    {
        S.rayHit = ts.rayHit;
        S.rayDistM = ts.rayDistUnits / g_upm;
        if (ts.any && ts.vehicleId >= 0 && ts.vehicleId < (int)yw->_vhclProtos.size())
        {
            S.tgt = true;
            S.tgtLocked = ts.locked;
            S.tgtName = Upper(yw->GetVehicleName(yw->_vhclProtos[ts.vehicleId]), 16);
            S.tgtPct = std::max(0, std::min(100, 100 * ts.energy / std::max(1, ts.energyMax)));
            S.tgtDistM = ts.distUnits / g_upm;
            S.tgtEnemy = yw->_userRobo && ts.owner != yw->_userRobo->_owner;
        }
    }
    // radio: newest messages from the info log (30 s)
    {
        int idx = info_log.msg_count;
        const int n = std::min(64, (int)info_log.numEntries);
        for (int k = 0; k < n && S.nMsg < 3; k++, idx = (idx + 63) % 64)
        {
            const inflog_msg &m = info_log.msgs[idx];
            const float age = (yw->_timeStamp - m.field_4) / 1000.0f;
            if (!m.txt[0] || age < 0 || age > 30.0f)
                continue;
            S.msg[S.nMsg] = Upper(m.txt, 120);
            S.msgAge[S.nMsg] = age;
            S.nMsg++;
        }
    }
    // wrist: squads, radar, host energy
    if (NC_STACK_ypabact *robo = yw->_userRobo)
    {
        S.hostPct = std::max(0, std::min(100, 100 * robo->_energy / std::max(1, robo->_energy_max)));
        for (NC_STACK_ypabact *cmd : robo->_kidList)
        {
            if (cmd->_status == BACT_STATUS_DEAD || cmd->_bact_type == BACT_TYPES_GUN)
                continue;
            Snap::Sq q;
            q.name = cmd->_vehicleID < yw->_vhclProtos.size() ? Upper(yw->GetVehicleName(yw->_vhclProtos[cmd->_vehicleID]), 10) : "?";
            q.n = 1;
            long e = cmd->_energy, em = std::max(1, cmd->_energy_max);
            q.mine = (cmd == uu);
            q.gid = cmd->_gid;
            q.sel = yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size() &&
                    yw->_cmdrsRemap[yw->_activeCmdrRemapIndex] == cmd;
            for (NC_STACK_ypabact *u : cmd->_kidList)
            {
                if (u->_status == BACT_STATUS_DEAD)
                    continue;
                q.n++;
                e += u->_energy;
                em += std::max(1, u->_energy_max);
                q.mine |= (u == uu);
            }
            q.pct = (int)std::max(0L, std::min(100L, 100 * e / em));
            S.squads.push_back(q);
            if (S.squads.size() >= 8)
                break;
        }
        // radar blips: every unit of every station within 3 sectors, heading-up
        const double range = 3600.0;
        double fx = yw->_viewerRotation.m20, fz = yw->_viewerRotation.m22;
        const double fl = sqrt(fx * fx + fz * fz);
        if (fl > 1e-6) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
        auto blip = [&](NC_STACK_ypabact *b, int kind) {
            if (b == vb || b->_status == BACT_STATUS_DEAD || S.blips.size() >= 64 ||
                b->IsHiddenFor(yw->GetPlayerOwner())) // stealth, as sub_4F6DFC
                return;
            const double dx = b->_position.x - vb->_position.x, dz = b->_position.z - vb->_position.z;
            const double ahead = dx * fx + dz * fz, side = dx * fz - dz * fx;
            const double r = sqrt(ahead * ahead + side * side);
            if (r > range)
                return;
            S.blips.push_back(Snap::Blip{(float)(side / range), (float)(ahead / range), kind});
        };
        for (NC_STACK_ypabact *st : yw->_unitsList)
        {
            const bool own = st->_owner == robo->_owner;
            blip(st, own ? 2 : 1);
            for (NC_STACK_ypabact *cmd : st->_kidList)
            {
                blip(cmd, own ? 0 : 1);
                for (NC_STACK_ypabact *u : cmd->_kidList)
                    blip(u, own ? 0 : 1);
            }
        }
    }

    // ---- shake -> haptics; hits -> short pulse
    const double now = NowMs();
    if (g_hapticsOn)
    {
        if (uu->_gid != g_viewerGid || uu->_bact_type == BACT_TYPES_ROBO) // the station spends energy: not hits
        {
            g_viewerGid = uu->_gid;
            g_lastEnergy = uu->_energy;
        }
        else if (uu->_energy < g_lastEnergy)
        {
            const float frac = (float)(g_lastEnergy - uu->_energy) / (float)std::max(1, uu->_energy_max);
            const float amp = std::min(1.0f, 0.35f + 3.0f * frac);
            const float ms = frac > 0.08f ? 110.0f : 70.0f;
            Haptic(0, amp, ms, "hit");
            Haptic(1, amp, ms, "hit");
            if (g_log)
                fprintf(stderr, "UAVR haptic: hit -%d energy (%.1f%%) -> pulse amp %.2f %.0f ms\n", g_lastEnergy - uu->_energy, frac * 100.0f, amp, ms);
            g_lastEnergy = uu->_energy;
        }
        else
            g_lastEnergy = uu->_energy;
        if (g_shakeDeg > 0.05 && (g_lastShakePulse < 0 || now - g_lastShakePulse >= 40.0))
        {
            const float amp = (float)std::min(1.0, 0.20 + g_shakeDeg / 2.5);
            Haptic(0, amp, 50.0f, "shake");
            Haptic(1, amp, 50.0f, "shake");
            g_lastShakePulse = now;
        }
    }

    // ---- measurement: the rendered vehicle camera vs the unshaken viewer rotation
    if (TF::TForm3D *vp = TF::Engine.GetViewPoint())
    {
        mat3x3 r = vp->SclRot;
        GFX::Engine.matrixAspectCorrection(r, true);
        g_la.viewDevPeak = std::max(g_la.viewDevPeak, AngleDeg(r, yw->_viewerRotation));
    }
    g_la.frames++;
    if (ShellsOn() && S.cls != CLS_NONE)
        g_la.shellFrames++;
    if (g_la.since <= 0)
        g_la.since = now;
    if (g_log && now - g_la.since >= 1000.0)
    {
        fprintf(stderr, "UAVR cockpit: shell %s%s | shake engine peak %.2f deg, applied to view peak %.2f deg, camera-vs-vehicle peak %.3f deg"
                        " | haptic pulses hit %d (peak amp %.2f) shake %d ff %d | hp %d%% tgt %s rng %.0fm msgs %d squads %d blips %d\n",
                kClassName[S.cls], ShellsOn() ? "" : " (shells off)", g_la.shakePeak, g_la.appliedPeak, g_la.viewDevPeak, g_la.hit, g_la.hitAmpPeak,
                g_la.shake, g_la.ff, 100 * S.hp / S.hpMax, S.tgt ? S.tgtName.c_str() : "-", S.tgt ? S.tgtDistM : S.rayDistM, S.nMsg,
                (int)S.squads.size(), (int)S.blips.size());
        g_la = LogAcc();
        g_la.since = now;
    }
    g_snap = S;
}

bool DrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const ViewState &views, const ControllerState ctrl[2])
{
    if (!ShellVisible())
        return false;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return false;
    const Layout &L = (g_snap.cls == CLS_HELI && CommandDeck()) ? g_deckHeli : g_layout[Frame()][g_snap.cls]; // sites

    // Dynamic geometry once per frame (eye 0), drawn in both eyes.
    if (eye == 0)
    {
        g_dyn.clear();
        if (g_snap.hudOn && !g_snap.dead)
        {
            BuildDash(g_dyn, L, g_snap);
            BuildWrist(g_dyn, g_snap, ctrl[0], views); // same HUD-visible gate as the dash
        }
        if (g_dyn.size() > 60000)
            g_dyn.resize(60000);
        g_dynCount = (int)g_dyn.size();
    }

    GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevActive = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0, depthFunc = 0;
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    M4 sc = M4::Ident();
    sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = g_upm; // metres -> world units
    gl.UseProgram(g_prog);
    UploadMVP(eyeProj * centreFromAnchor * sc);
    gl.Uniform1i(g_locAtlas, 0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // Shell: nearest thing in the scene. Fresh depth, opaque, depth-tested
    // against itself only (the world was drawn already).
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    gl.BindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, L.first, L.count);

    // Screens' text/bars and the wrist: premultiplied, over everything.
    if (g_dynCount > 0)
    {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl.BindVertexArray(g_vaoDyn);
        gl.BindBuffer(GL_ARRAY_BUFFER, g_vboDyn);
        if (eye == 0)
            gl.BufferData(GL_ARRAY_BUFFER, g_dynCount * sizeof(Vtx), g_dyn.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, g_dynCount);
    }

    gl.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
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
    return true;
}

// diorama: in a vehicle with the miniature up, the cockpit shell is not drawn
// (the player stands at the table), but the left-wrist panel stays: drawn alone, over the
// miniature (no depth clear, no depth test, as in the cockpit pass), and its row layout is
// refreshed every frame so WristPick keeps working. Own program: the miniature's clip
// planes (DioClip uniforms of the world shaders) do not apply here.
bool DrawWristEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const ViewState &views, const ControllerState ctrl[2])
{
    if (!ShellVisible())
        return false;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return false;
    if (eye == 0)
    {
        g_dyn.clear();
        if (g_snap.hudOn && !g_snap.dead)
            BuildWrist(g_dyn, g_snap, ctrl[0], views);
        if (g_dyn.size() > 60000)
            g_dyn.resize(60000);
        g_dynCount = (int)g_dyn.size();
    }
    if (g_dynCount <= 0)
        return false;

    GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevActive = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    M4 sc = M4::Ident();
    sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = g_upm; // metres -> world units
    gl.UseProgram(g_prog);
    UploadMVP(eyeProj * centreFromAnchor * sc);
    gl.Uniform1i(g_locAtlas, 0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE); // the miniature's depth stays (passthrough alpha was already resolved from it)
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.BindVertexArray(g_vaoDyn);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vboDyn);
    if (eye == 0)
        gl.BufferData(GL_ARRAY_BUFFER, g_dynCount * sizeof(Vtx), g_dyn.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_dynCount);

    gl.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthMask(depthMask);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    gl.ActiveTexture(prevActive);
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    gl.UseProgram(prevProg);
    static bool logged = false;
    if (!logged && eye == 0 && g_log)
    {
        logged = true;
        fprintf(stderr, "UAVR cockpit: wrist panel drawn over the diorama (%d verts)\n", g_dynCount);
    }
    return true;
}

} // namespace Cockpit

bool CockpitOwnsHud()
{
    return Cockpit::ShellVisible();
}

bool CockpitFrameOpen() { return Cockpit::FrameOpen(); }
int CockpitFrame() { return Cockpit::Frame(); }
int CockpitFrameSetting() { return Cockpit::g_frameSetting; }
void SetCockpitFrame(int style) { Cockpit::g_frameSetting = (style >= 0 && style < FRAME_COUNT) ? style : FRAME_OPEN_RIM; }
// profiles stored 1 = open, 0 = classic; adds 2 = open + rim.
int CockpitFrameToProfile(int style) { return style == FRAME_CLASSIC ? 0 : (style == FRAME_OPEN_RIM ? 2 : 1); }
int CockpitFrameFromProfile(int v) { return v == 0 ? FRAME_CLASSIC : (v == 2 ? FRAME_OPEN_RIM : FRAME_OPEN); }
const char *CockpitFrameLabel(int style)
{
    return style == FRAME_CLASSIC ? "COCKPIT FRAME: CLASSIC" : (style == FRAME_OPEN_RIM ? "COCKPIT FRAME: OPEN+RIM" : "COCKPIT FRAME: OPEN");
}

} // namespace VR
