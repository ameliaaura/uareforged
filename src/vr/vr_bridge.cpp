// Urban Assault VR — host-station bridge (Command Deck). See vr_bridge.h and
//
//
// Space: like the cockpit shells (vr_cockpit.cpp), everything is built in
// comfort-anchor-local UAV axes (x right, y DOWN, z forward) in METRES with the
// origin at the seated eye point. In the world the model matrix scales metres to
// world units and centreFromAnchor/eyeProj take it to clip space, so the room
// turns with the station view. On briefing frames the anchor is the tracking
// space itself (the theatre screen's space).
//
// The GL pipeline (one program, static VBO for the room, dynamic VBO for the
// screens' text, 5x7 font in an R8 atlas, procedural wear in the fragment
// shader) is a self-contained copy of the cockpit's, so this file does not
// touch vr_cockpit.cpp.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_bridge.h"
#include "vr_station.h"
#include "vr_gl.h"
#include "../system/gfx.h"
#include "../yw.h"
#include "../yw_internal.h"
#include "../ypabact.h"
#include "../yparobo.h"

extern yw_infolog info_log; // yw_game_ui.cpp (the radio / info ticker log)

namespace VR
{

namespace
{
uint32_t g_briefingNoted = 0; // Present() frame stamp of the last BridgeNoteBriefing()
uint32_t g_presentNo = 1;
}

void BridgeNoteBriefing() { g_briefingNoted = g_presentNo; }

namespace Bridge
{

namespace
{

// ---- config / state -------------------------------------------------------

double g_upm = 11.0;
bool g_log = false;
int g_env = -2; // UA_VR_BRIDGE: -1 unset, 0 off, 1 on
double g_monDist = 3.0, g_monElevDeg = 20.0, g_monW = 2.0; // main monitor (UA_VR_BRIDGE_MON)

bool Enabled()
{
    if (g_env == -2)
    {
        const char *v = getenv("UA_VR_BRIDGE");
        g_env = (v && v[0]) ? (strcmp(v, "0") != 0) : -1;
    }
    return g_env >= 0 ? g_env != 0 : CommandDeck();
}

double NowMs() { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

// ---- 5x7 font (ASCII 32..95; lowercase maps to uppercase), same as the cockpit

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
const int kS = 4, kCellW = 8, kCellH = 9, kCols = 16, kRows = 5;
const int kAtlasW = kCols * kCellW * kS, kAtlasH = kRows * kCellH * kS; // 512 x 180
const int kSolid = 64;

// ---- geometry ---------------------------------------------------------------

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

const float kDegF = (float)(M_PI / 180.0);
// Point on a vertical cylinder around the seat: yaw in degrees (+ = right), radius, height (y down).
V3 Cyl(float yawDeg, float r, float y) { return V3(r * sinf(yawDeg * kDegF), y, r * cosf(yawDeg * kDegF)); }

struct Vtx
{
    float p[3];
    float c[4];
    float uv[2];
    float mode; // 0 = lit room (procedural wear), 1 = atlas (text / bars), premultiplied, 2 = flat glass (MR panels)
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

// Lit, flat-shaded quad; the normal is turned towards the eye point.
void Quad(Geo &g, const V3 &a, const V3 &b, const V3 &c, const V3 &d, const V3 &albedo)
{
    V3 n = Norm(Cross(b - a, d - a));
    const V3 ctr = (a + b + c + d) * 0.25f;
    if (Dot(n, -ctr) < 0)
        n = -n;
    const float shade = 1.35f * (0.42f + 0.70f * std::max(0.0f, Dot(n, kLight)));
    const float col[4] = {albedo.x * shade, albedo.y * shade, albedo.z * shade, 1.0f};
    PushV(g, a, col, 0, 0, 0); PushV(g, b, col, 0, 0, 0); PushV(g, c, col, 0, 0, 0);
    PushV(g, a, col, 0, 0, 0); PushV(g, c, col, 0, 0, 0); PushV(g, d, col, 0, 0, 0);
}

void Box(Geo &g, const V3 &c, const V3 &ax, const V3 &ay, const V3 &az, const V3 &albedo)
{
    V3 p[8];
    for (int i = 0; i < 8; i++)
        p[i] = c + ax * ((i & 1) ? 1.f : -1.f) + ay * ((i & 2) ? 1.f : -1.f) + az * ((i & 4) ? 1.f : -1.f);
    Quad(g, p[0], p[1], p[3], p[2], albedo);
    Quad(g, p[4], p[5], p[7], p[6], albedo);
    Quad(g, p[0], p[1], p[5], p[4], albedo);
    Quad(g, p[2], p[3], p[7], p[6], albedo);
    Quad(g, p[0], p[2], p[6], p[4], albedo);
    Quad(g, p[1], p[3], p[7], p[5], albedo);
}

// Beam from a to b: w across (perpendicular to `up` and the beam), h along up.
void Beam(Geo &g, const V3 &a, const V3 &b, float w, float h, const V3 &up, const V3 &albedo)
{
    const V3 d = b - a;
    const V3 dir = Norm(d);
    V3 side = Cross(up, dir);
    if (Len(side) < 1e-4f)
        side = Cross(V3(1, 0, 0), dir);
    const V3 ax = Norm(side) * (w * 0.5f);
    const V3 ay = Norm(Cross(dir, ax)) * (h * 0.5f);
    Box(g, (a + b) * 0.5f, ax, ay, d * 0.5f, albedo);
}

void Bolts(Geo &g, const V3 &a, const V3 &b, int n, const V3 &out, float size, const V3 &albedo)
{
    const V3 o = Norm(out);
    V3 t = Cross(o, V3(0, 1, 0));
    if (Len(t) < 0.1f)
        t = Cross(o, V3(1, 0, 0));
    t = Norm(t);
    const V3 s = Norm(Cross(o, t));
    for (int i = 0; i < n; i++)
    {
        const float f = n > 1 ? (float)i / (n - 1) : 0.5f;
        const V3 c = a + (b - a) * f + o * (size * 0.4f);
        Box(g, c, t * (size * 0.5f), s * (size * 0.5f), o * (size * 0.4f), albedo);
    }
}

// A screen facing the eye point: bezel box + dark glass. Text is laid out in
// screen-local metres (x right, y down from the top-left corner of the glass).
struct Screen
{
    V3 tl, right, down, fwd, centre;
    float w = 0, h = 0;
};

Screen MakeScreen(Geo &g, const V3 &centre, float w, float h, const V3 &bezel, float margin)
{
    Screen s;
    s.fwd = Norm(centre);
    s.right = Norm(Cross(V3(0, 1, 0), s.fwd));
    s.down = Norm(Cross(s.fwd, s.right));
    s.w = w;
    s.h = h;
    s.centre = centre;
    const float depth = 0.05f;
    Box(g, centre + s.fwd * (depth * 0.5f), s.right * (w * 0.5f + margin), s.down * (h * 0.5f + margin), s.fwd * (depth * 0.5f), bezel);
    const V3 glass(0.026f, 0.036f, 0.033f);
    const V3 gc = centre - s.fwd * 0.003f;
    const V3 r = s.right * (w * 0.5f), d = s.down * (h * 0.5f);
    Quad(g, gc - r - d, gc + r - d, gc + r + d, gc - r + d, glass);
    s.tl = centre - r - d - s.fwd * 0.006f;
    return s;
}

// ---- the room -----------------------------------------------------------------

enum { SCR_ENERGY = 0, SCR_SQUADS, SCR_HOSTS, SCR_POWER, SCR_MAIN, SCR_COUNT };
Screen g_scr[SCR_COUNT];
int g_roomVerts = 0;

const float kFloorY = 1.15f;  // floor below the seated eye point
const float kDeckIn = 0.85f, kDeckOut = 2.55f;
const float kPostR0 = 2.60f, kPostR1 = 3.25f, kTopY = -2.05f; // window posts lean out
const float kSillY = 0.62f;
const float kPostYaw[8] = {-165, -120, -75, -30, 30, 75, 120, 165};

float PostR(float y) { return kPostR0 + (kPostR1 - kPostR0) * (kFloorY - y) / (kFloorY - kTopY); }

void MonitorCentre(V3 *c, float *w, float *h)
{
    const float e = (float)(g_monElevDeg * M_PI / 180.0);
    *c = V3(0, -(float)g_monDist * tanf(e), (float)g_monDist);
    *w = (float)g_monW;
    *h = (float)g_monW * 0.75f;
}

void BuildRoom(Geo &g)
{
    // Battle-worn gunmetal with an olive cast; scorched plates; worn edges (no camo).
    const V3 hull(0.25f, 0.27f, 0.24f), trim(0.15f, 0.16f, 0.14f), edge(0.42f, 0.41f, 0.37f), bezel(0.11f, 0.11f, 0.10f),
        bolt(0.21f, 0.21f, 0.19f), scorch(0.085f, 0.080f, 0.075f), deck(0.20f, 0.20f, 0.19f), rust(0.30f, 0.20f, 0.13f);
    const V3 up(0, -1, 0);

    // Floor plate under the seat (16-gon), with a raised rim.
    const int N = 16;
    for (int i = 0; i < N; i++)
    {
        const float a0 = 360.0f * i / N, a1 = 360.0f * (i + 1) / N;
        const V3 c(0, kFloorY, 0);
        Quad(g, Cyl(a0, kDeckIn, kFloorY), Cyl(a1, kDeckIn, kFloorY), c, c, deck * (0.90f + 0.05f * (float)((i * 7) % 4))); // plate-to-plate wear
        Beam(g, Cyl(a0, kDeckIn, kFloorY - 0.02f), Cyl(a1, kDeckIn, kFloorY - 0.02f), 0.06f, 0.05f, up, trim);
    }
    // Grated deck ring: radial slats + two hoops. The gaps show the battlefield below.
    for (int i = 0; i < 24; i++)
    {
        const float a = 360.0f * i / 24 + 7.5f;
        Beam(g, Cyl(a, kDeckIn, kFloorY), Cyl(a, kDeckOut, kFloorY), 0.07f, 0.05f, up, (i % 7 == 3) ? rust : hull);
    }
    for (int i = 0; i < 24; i++)
    {
        const float a0 = 360.0f * i / 24, a1 = 360.0f * (i + 1) / 24;
        Beam(g, Cyl(a0, 1.70f, kFloorY - 0.01f), Cyl(a1, 1.70f, kFloorY - 0.01f), 0.05f, 0.04f, up, trim);
        Beam(g, Cyl(a0, kDeckOut, kFloorY - 0.02f), Cyl(a1, kDeckOut, kFloorY - 0.02f), 0.09f, 0.08f, up, hull);
        Beam(g, Cyl(a0, kDeckOut, kFloorY - 0.065f), Cyl(a1, kDeckOut, kFloorY - 0.065f), 0.03f, 0.02f, up, edge); // worn top edge
    }

    // Window posts (lean out towards the top), sill and top rings, sparse roof.
    for (int i = 0; i < 8; i++)
    {
        const float a = kPostYaw[i];
        const V3 b = Cyl(a, kPostR0, kFloorY), t = Cyl(a, kPostR1, kTopY);
        Beam(g, b, t, 0.11f, 0.13f, Norm(V3(-b.x, 0, -b.z)), hull);
        Beam(g, Cyl(a, kPostR0 - 0.02f, kFloorY - 0.15f), Cyl(a, kPostR0 - 0.10f, kFloorY - 0.02f), 0.08f, 0.16f,
             Norm(V3(-b.x, 0, -b.z)), trim); // foot gusset
        const V3 inward = Norm(V3(-b.x, 0, -b.z));
        Bolts(g, Cyl(a, PostR(0.9f), 0.9f) + inward * 0.065f, Cyl(a, PostR(-1.7f), -1.7f) + inward * 0.065f, 7, inward, 0.016f, bolt);
    }
    for (int i = 0; i < 8; i++)
    {
        const float a0 = kPostYaw[i], a1 = kPostYaw[(i + 1) % 8];
        Beam(g, Cyl(a0, PostR(kSillY), kSillY), Cyl(a1, PostR(kSillY), kSillY), 0.09f, 0.07f, up, hull);
        Beam(g, Cyl(a0, PostR(kSillY) - 0.04f, kSillY - 0.05f), Cyl(a1, PostR(kSillY) - 0.04f, kSillY - 0.05f), 0.025f, 0.02f, up, edge);
        Beam(g, Cyl(a0, kPostR1, kTopY), Cyl(a1, kPostR1, kTopY), 0.13f, 0.15f, up, hull);
        // roof beam to the hub
        Beam(g, Cyl(a0, kPostR1, kTopY), V3(0, kTopY - 0.30f, 0), 0.08f, 0.10f, up, trim);
    }
    Box(g, V3(0, kTopY - 0.32f, 0), V3(0.25f, 0, 0), V3(0, 0.07f, 0), V3(0, 0, 0.25f), trim); // hub

    // Console arcs left and right (yaw 40..112 deg): sloped desk, front and back skirts.
    for (int s = -1; s <= 1; s += 2)
    {
        const float rIn = 1.22f, rOut = 1.62f, yIn = 0.52f, yOut = 0.40f;
        const int segs = 6;
        for (int i = 0; i < segs; i++)
        {
            const float a0 = s * (40.0f + 72.0f * i / segs), a1 = s * (40.0f + 72.0f * (i + 1) / segs);
            const V3 i0 = Cyl(a0, rIn, yIn), i1 = Cyl(a1, rIn, yIn), o0 = Cyl(a0, rOut, yOut), o1 = Cyl(a1, rOut, yOut);
            Quad(g, i0, i1, o1, o0, (i == 2) ? scorch : trim);                                            // desk top
            Quad(g, i0, i1, Cyl(a1, rIn + 0.08f, kFloorY), Cyl(a0, rIn + 0.08f, kFloorY), hull);          // front skirt
            Quad(g, o0, o1, Cyl(a1, rOut, kFloorY), Cyl(a0, rOut, kFloorY), hull);                         // back
            Beam(g, i0 + V3(0, -0.015f, 0), i1 + V3(0, -0.015f, 0), 0.03f, 0.03f, up, edge);               // worn lip
            // small lit switch row (dim amber / green blocks)
            if (i % 2 == 0)
                for (int k = 0; k < 3; k++)
                {
                    const float a = a0 + (a1 - a0) * (0.25f + 0.25f * k);
                    const V3 p = Cyl(a, rIn + 0.12f, yIn - 0.012f - 0.03f * 0.3f);
                    Box(g, p, V3(0.012f, 0, 0), V3(0, 0.008f, 0), V3(0, 0, 0.012f), k == 1 ? V3(0.30f, 0.45f, 0.18f) : V3(0.45f, 0.30f, 0.10f));
                }
        }
        // end caps
        for (int e = 0; e < 2; e++)
        {
            const float a = s * (e ? 112.0f : 40.0f);
            Quad(g, Cyl(a, rIn, yIn), Cyl(a, rOut, yOut), Cyl(a, rOut, kFloorY), Cyl(a, rIn + 0.08f, kFloorY), hull);
        }
    }

    // Side screens on the consoles: energy (left front), squads (left), hosts (right front), power (right).
    struct SS { int id; float yaw; };
    const SS ss[4] = {{SCR_ENERGY, -52.0f}, {SCR_SQUADS, -97.0f}, {SCR_HOSTS, 52.0f}, {SCR_POWER, 97.0f}};
    for (const SS &q : ss)
    {
        const V3 c = Cyl(q.yaw, 1.50f, 0.02f);
        g_scr[q.id] = MakeScreen(g, c, 0.64f, 0.48f, bezel, 0.025f);
        // mount: two struts from the desk to the bezel bottom
        const Screen &S = g_scr[q.id];
        for (int k = -1; k <= 1; k += 2)
        {
            const V3 top = c + S.right * (0.20f * k) + S.down * 0.27f;
            const V3 bot = Cyl(q.yaw + k * 6.0f, 1.45f, 0.47f);
            Beam(g, top, bot, 0.035f, 0.035f, up, trim);
        }
        Bolts(g, c + S.right * (-0.30f) + S.down * (-0.262f) - S.fwd * 0.0f, c + S.right * 0.30f + S.down * (-0.262f), 6, -S.fwd, 0.012f, bolt);
    }

    // Main monitor ahead-high, hung from the front top ring.
    {
        V3 c;
        float w, h;
        MonitorCentre(&c, &w, &h);
        g_scr[SCR_MAIN] = MakeScreen(g, c, w, h, bezel, 0.06f);
        const Screen &S = g_scr[SCR_MAIN];
        for (int k = -1; k <= 1; k += 2)
        {
            const V3 corner = c + S.right * (w * 0.38f * k) + S.down * (-h * 0.5f - 0.05f);
            const V3 ring = Cyl(k * 22.0f, PostR(kTopY) * cosf(30.0f * kDegF) / cosf(22.0f * kDegF), kTopY); // on the front chord
            Beam(g, corner, ring, 0.06f, 0.06f, up, hull);
        }
        Beam(g, c + S.right * (-w * 0.5f - 0.06f) + S.down * (h * 0.5f + 0.075f), c + S.right * (w * 0.5f + 0.06f) + S.down * (h * 0.5f + 0.075f),
             0.05f, 0.05f, S.fwd, edge); // worn lower lip
        Bolts(g, c + S.right * (-w * 0.48f) + S.down * (-h * 0.5f - 0.035f), c + S.right * (w * 0.48f) + S.down * (-h * 0.5f - 0.035f), 10, -S.fwd, 0.018f, bolt);
    }
}

// the screens alone, for the mixed-reality table (passthrough room + miniature): a thin
// frame of four bars round each glass and smoked, slightly see-through glass. Same places,
// sizes and text layout as the room's screens (g_scr, filled by BuildRoom), so the text
// lines up and nothing moves when the table opens or closes; no posts, rings, deck,
// consoles or mounts (no bridge metal over the real room).
void BuildPanels(Geo &g)
{
    const V3 frame(0.13f, 0.135f, 0.125f);
    for (int i = 0; i < SCR_COUNT; i++)
    {
        const Screen &s = g_scr[i];
        if (s.w <= 0)
            continue;
        const float m = i == SCR_MAIN ? 0.024f : 0.012f; // frame width
        const float hw = s.w * 0.5f, hh = s.h * 0.5f, dz = 0.008f;
        const V3 c = s.centre + s.fwd * dz;
        Box(g, c - s.down * (hh + m * 0.5f), s.right * (hw + m), s.down * (m * 0.5f), s.fwd * dz, frame); // top
        Box(g, c + s.down * (hh + m * 0.5f), s.right * (hw + m), s.down * (m * 0.5f), s.fwd * dz, frame); // bottom
        Box(g, c - s.right * (hw + m * 0.5f), s.right * (m * 0.5f), s.down * hh, s.fwd * dz, frame);      // left
        Box(g, c + s.right * (hw + m * 0.5f), s.right * (m * 0.5f), s.down * hh, s.fwd * dz, frame);      // right
        const V3 gc = s.centre - s.fwd * 0.003f, r = s.right * hw, d = s.down * hh;
        const float glass[4] = {0.020f, 0.030f, 0.028f, 0.86f};
        const V3 q[6] = {gc - r - d, gc + r - d, gc + r + d, gc - r - d, gc + r + d, gc - r + d};
        for (const V3 &v : q)
            PushV(g, v, glass, 0, 0, 2.0f);
    }
}

// ---- GL ---------------------------------------------------------------------

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

// Same look as the cockpit shells: flat-lit albedo x grime blotches, streaks, scratches.
const char *kFs =
    "uniform sampler2D atlas;\n"
    "uniform int wear;\n" // perf: 2 full (cockpit look), 1 cheap 2D noise, 0 flat
    "in vec4 vCol;\n"
    "in vec2 vUV;\n"
    "in vec3 vP;\n"
    "in float vMode;\n"
    "out vec4 frag;\n"
    "float h3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }\n"
    "float h2(vec2 p) { p = fract(p * vec2(0.3183099, 0.3678794) + 0.1); p *= 17.0; return fract(p.x * p.y * (p.x + p.y)); }\n"
    "float n2(vec2 x) { vec2 i = floor(x); vec2 f = fract(x); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(h2(i), h2(i + vec2(1,0)), f.x), mix(h2(i + vec2(0,1)), h2(i + vec2(1,1)), f.x), f.y); }\n"
    "float n3(vec3 x) { vec3 i = floor(x); vec3 f = fract(x); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(mix(h3(i), h3(i + vec3(1,0,0)), f.x), mix(h3(i + vec3(0,1,0)), h3(i + vec3(1,1,0)), f.x), f.y),\n"
    "             mix(mix(h3(i + vec3(0,0,1)), h3(i + vec3(1,0,1)), f.x), mix(h3(i + vec3(0,1,1)), h3(i + vec3(1,1,1)), f.x), f.y), f.z); }\n"
    "void main() {\n"
    "  if (vMode > 1.5) { frag = vec4(vCol.rgb * vCol.a, vCol.a); return; }\n" // MR panel glass (premultiplied)
    "  if (vMode > 0.5) {\n"
    "    float a = texture(atlas, vUV).r * vCol.a;\n"
    "    frag = vec4(vCol.rgb * a, a);\n"
    "    return;\n"
    "  }\n"
    "  vec3 p = vP;\n"
    "  if (wear == 0) { frag = vec4(vCol.rgb * 0.9, 1.0); return; }\n"
    "  if (wear == 1) {\n" // same four layers as the full look, 2D value noise (half the ALU)
    "    vec2 q = vec2(p.x * 0.8 + p.z * 0.6, p.y + 0.35 * (p.x - p.z));\n"
    "    float g1 = n2(q * 7.0) * 0.6 + n2(q.yx * 21.0 + 3.1) * 0.4;\n"
    "    vec3 c1 = vCol.rgb * mix(0.68, 1.12, g1);\n"
    "    float st1 = n2(vec2((p.x - p.z) * 48.0, p.y * 2.2));\n"
    "    c1 *= mix(1.0, 0.70, smoothstep(0.60, 0.86, st1));\n"
    "    float sc1 = n2(vec2(p.x * 150.0 + p.z * 70.0, p.y * 150.0 - p.z * 40.0));\n"
    "    c1 = mix(c1, min(c1 * 1.9 + 0.04, vec3(1.0)), smoothstep(0.84, 0.92, sc1) * 0.6);\n"
    "    frag = vec4(c1, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  float g = n3(p * 7.0) * 0.6 + n3(p * 21.0) * 0.4;\n"
    "  vec3 c = vCol.rgb * mix(0.68, 1.12, g);\n"
    "  float st = n3(vec3(p.x * 48.0, p.y * 2.2, p.z * 48.0));\n"
    "  c *= mix(1.0, 0.70, smoothstep(0.60, 0.86, st));\n"
    "  float sc = n3(vec3(p.x * 150.0 + p.z * 70.0, p.y * 150.0 - p.z * 40.0, p.z * 12.0));\n"
    "  c = mix(c, min(c * 1.9 + 0.04, vec3(1.0)), smoothstep(0.84, 0.92, sc) * 0.6);\n"
    "  frag = vec4(c, 1.0);\n"
    "}\n";

GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_vaoDyn = 0, g_vboDyn = 0, g_tex = 0;
GLuint g_vaoPan = 0, g_vboPan = 0; // MR panels (frames + glass)
int g_panVerts = 0;
Geo g_pan;                          // CPU copy until the first upload
bool g_mrPanels = true;             // UA_VR_BRIDGE_MR=0: no screens with the table open (A/B)
GLint g_locMVP = -1, g_locAtlas = -1, g_locWear = -1;
// perf (Quest A/B: the room cost ~1.15 ms GPU per eye with the full wear shader):
// UA_VR_BRIDGE_WEAR=0 flat / 1 cheap / 2 full (desktop default 2, GLES default below).
#ifdef UA_GLES
int g_wear = 1;
#else
int g_wear = 2;
#endif
bool g_glTried = false, g_glOk = false;
Geo g_room;            // CPU copy until the first upload
Geo g_dyn;             // screens' text and bars (anchor-local metres)
int g_dynCount = 0;
bool g_dynDirty = false;

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
        fprintf(stderr, "UAVR bridge: shader error: %s\n", log);
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
    gl.BindAttribLocation(p, 3, "aMode");
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "UAVR bridge: link failed\n");
        gl.DeleteProgram(p);
        return false;
    }
    g_prog = p;
    g_locMVP = gl.GetUniformLocation(p, "MVP");
    g_locAtlas = gl.GetUniformLocation(p, "atlas");
    g_locWear = gl.GetUniformLocation(p, "wear");

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
    gl.BufferData(GL_ARRAY_BUFFER, g_room.size() * sizeof(Vtx), g_room.data(), GL_STATIC_DRAW);
    SetupAttribs();
    g_roomVerts = (int)g_room.size();
    Geo().swap(g_room);
    gl.GenVertexArrays(1, &g_vaoPan); // MR panels
    gl.BindVertexArray(g_vaoPan);
    gl.GenBuffers(1, &g_vboPan);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vboPan);
    gl.BufferData(GL_ARRAY_BUFFER, g_pan.size() * sizeof(Vtx), g_pan.data(), GL_STATIC_DRAW);
    SetupAttribs();
    g_panVerts = (int)g_pan.size();
    Geo().swap(g_pan);
    gl.GenVertexArrays(1, &g_vaoDyn);
    gl.BindVertexArray(g_vaoDyn);
    gl.GenBuffers(1, &g_vboDyn);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vboDyn);
    gl.BufferData(GL_ARRAY_BUFFER, 4096 * sizeof(Vtx), NULL, GL_DYNAMIC_DRAW);
    SetupAttribs();

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

    fprintf(stderr, "UAVR bridge: room built (%d triangles), monitor %.1f m at %.0f deg up, %.2f m wide\n", g_roomVerts / 3, g_monDist,
            g_monElevDeg, g_monW);
    return true;
}

void UploadMVP(const M4 &mvp)
{
    float f[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            f[j * 4 + i] = (float)mvp.m[i][j]; // column-major for GL
    gl.UniformMatrix4fv(g_locMVP, 1, GL_FALSE, f);
}

// ---- text and bars ------------------------------------------------------------

struct Col { float r, g, b, a; };
const Col kAmber = {1.00f, 0.70f, 0.28f, 1.0f};
const Col kWhite = {0.92f, 0.94f, 0.88f, 1.0f};
const Col kGreen = {0.55f, 0.95f, 0.55f, 1.0f};
const Col kRed = {1.00f, 0.30f, 0.20f, 1.0f};
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
        const float x0 = x - 0.5f * p, x1 = x + 5.5f * p, y0 = y - 0.5f * p, y1 = y + 7.0f * p;
        AtlasQuad(g, SP(s, x0, y0), SP(s, x1, y0), SP(s, x1, y1), SP(s, x0, y1),
                  (cx + 0.5f) * kS / kAtlasW, (cy + 0.5f) * kS / kAtlasH, (cx + 6.5f) * kS / kAtlasW, (cy + 8.0f) * kS / kAtlasH, col);
    }
}

void Bar(Geo &g, const Screen &s, float x, float y, float w, float h, float frac, const Col &on, int segs)
{
    frac = std::max(0.0f, std::min(1.0f, frac));
    const float gap = std::min(0.005f, w / segs * 0.25f), sw = (w - gap * (segs - 1)) / segs;
    for (int i = 0; i < segs; i++)
    {
        const float lit = std::max(0.0f, std::min(1.0f, frac * segs - i));
        const float sx = x + i * (sw + gap);
        Rect(g, s, sx, y, sw, h, Fade(kDim, 0.30f));
        if (lit > 0)
            Rect(g, s, sx, y, sw * lit, h, on);
    }
}

Col LevelCol(float f)
{
    if (f > 0.5f) return kGreen;
    if (f > 0.25f) return kAmber;
    return kRed;
}

std::string Upper(const std::string &s, size_t maxLen)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        if ((c == 0xC3 || c == 0xC2) && i + 1 < s.size())
        {
            const unsigned char d = (unsigned char)s[++i];
            o += (d == 0xA4 || d == 0x84) ? "AE" : (d == 0xB6 || d == 0x96) ? "OE" : (d == 0xBC || d == 0x9C) ? "UE" : (d == 0x9F) ? "SS" : "?";
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c -= 32;
        o += (c >= 32 && c <= 95) ? (char)c : (c == '\n' || c == '\t' ? ' ' : '?');
    }
    if (o.size() > maxLen)
        o.resize(maxLen);
    return o;
}

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

std::string Fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char *f, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

// ---- game-state snapshot -------------------------------------------------------

struct Snap
{
    bool atStation = false;
    // energy (as the 2D HUD's top panel: ypaworld_func64__sub7__sub7__sub0)
    int energy = 0, energyMax = 1, battVeh = 0, battBeam = 0, reloadNow = 0, reloadMax = 0, fillMode = 0;
    int sectorState = 0; // 1 own + powered, -1 enemy power, 0 none
    // squads
    struct Sq { std::string name; int n; int pct; int aggr; bool sel; };
    std::vector<Sq> squads;
    int nSquads = 0, nUnits = 0;
    // enemy host stations
    struct Host { std::string name; bool seen; int clock; float distSec; int pct; };
    std::vector<Host> hosts;
    int nHosts = 0;
    // power stations
    struct Ps { int x, y; int who; int power; }; // who: 1 own, 2 enemy, 0 neutral, 3 unknown
    std::vector<Ps> ps;
    int psOwn = 0, psTotal = 0, psPowerOwn = 0;
    // main monitor
    std::string level, obj1, obj2;
    int nMsg = 0;
    std::string msg[4];
    int msgAgeS[4] = {0, 0, 0, 0};
    // station: built unit / beam line under STATION ENERGY
    std::string notice;
    int noticeCol = 0;
};
Snap g_snap;
double g_lastSnapMs = -1e9, g_lastLogMs = -1e9;
std::string g_key; // content key of the last built screens

void BuildScreens(Geo &g, const Snap &S)
{
    // Side screens: 0.64 x 0.48 m at about 1.5 m. Title cap 36 mm, body 27 mm.
    const float tH = 0.036f, bH = 0.027f, lp = 0.047f, x0 = 0.025f;
    // ENERGY
    {
        const Screen &s = g_scr[SCR_ENERGY];
        Text(g, s, x0, 0.022f, tH, "STATION ENERGY", kAmber);
        const float fMain = (float)S.energy / std::max(1, S.energyMax);
        float y = 0.085f;
        Text(g, s, x0, y, bH, "MAIN", kWhite);
        Text(g, s, s.w - x0, y, bH, Fmt("%d / %d", S.energy / 100, S.energyMax / 100), LevelCol(fMain), 1);
        Bar(g, s, x0, y + 0.034f, s.w - 2 * x0, 0.022f, fMain, LevelCol(fMain), 16);
        y += 0.078f;
        Text(g, s, x0, y, bH, (S.fillMode & 4) ? "UNITS  +" : "UNITS", kWhite);
        Text(g, s, s.w - x0, y, bH, Fmt("%d", S.battVeh / 100), kCyan, 1);
        Bar(g, s, x0, y + 0.034f, s.w - 2 * x0, 0.016f, (float)S.battVeh / std::max(1, S.energyMax), kCyan, 16);
        y += 0.070f;
        Text(g, s, x0, y, bH, (S.fillMode & 8) ? "BEAM   +" : "BEAM", kWhite);
        Text(g, s, s.w - x0, y, bH, Fmt("%d", S.battBeam / 100), kCyan, 1);
        Bar(g, s, x0, y + 0.034f, s.w - 2 * x0, 0.016f, (float)S.battBeam / std::max(1, S.energyMax), kCyan, 16);
        y += 0.078f;
        Text(g, s, x0, y, bH, Fmt("RELOAD %d/%d", S.reloadNow, S.reloadMax), S.reloadNow > 0 ? kGreen : kRed);
        Text(g, s, x0, y + lp - 0.004f, bH * 0.9f, S.sectorState > 0 ? "SECTOR POWER: OWN" : (S.sectorState < 0 ? "SECTOR POWER: ENEMY" : "SECTOR POWER: NONE"),
             S.sectorState > 0 ? kGreen : (S.sectorState < 0 ? kRed : kDim));
        if (!S.notice.empty()) // station: "TANK READY - LEFT", beam preview / result
            Text(g, s, x0, y + 2 * lp + 0.004f, bH * 0.8f, S.notice, S.noticeCol == 1 ? kGreen : S.noticeCol == 2 ? kRed : S.noticeCol == 3 ? kCyan : kWhite);
    }
    // SQUADS
    {
        const Screen &s = g_scr[SCR_SQUADS];
        Text(g, s, x0, 0.022f, tH, "SQUADS", kAmber);
        Text(g, s, s.w - x0, 0.022f, tH, Fmt("%d / %d", S.nSquads, S.nUnits), kWhite, 1);
        float y = 0.085f;
        if (S.squads.empty())
            Text(g, s, x0, y, bH, "NO SQUADS", kDim);
        for (const Snap::Sq &q : S.squads)
        {
            Text(g, s, x0, y, bH, (q.sel ? ">" : " ") + q.name, q.sel ? kAmber : kWhite);
            Text(g, s, 0.415f, y, bH, Fmt("%2d", q.n), kWhite, 1);
            Bar(g, s, 0.435f, y + 0.004f, 0.13f, 0.019f, q.pct / 100.0f, LevelCol(q.pct / 100.0f), 5);
            Text(g, s, s.w - x0, y, bH, Fmt("%d", q.aggr), kDim, 1);
            y += 0.046f;
        }
        if (S.nSquads > (int)S.squads.size())
            Text(g, s, x0, y, bH * 0.85f, Fmt("+%d MORE", S.nSquads - (int)S.squads.size()), kDim);
    }
    // ENEMY HOSTS
    {
        const Screen &s = g_scr[SCR_HOSTS];
        Text(g, s, x0, 0.022f, tH, "ENEMY HOSTS", kAmber);
        Text(g, s, s.w - x0, 0.022f, tH, Fmt("%d", S.nHosts), S.nHosts ? kRed : kGreen, 1);
        float y = 0.085f;
        if (S.hosts.empty())
            Text(g, s, x0, y, bH, "NONE LEFT", kGreen);
        for (const Snap::Host &h : S.hosts)
        {
            Text(g, s, x0, y, bH, h.name, kWhite);
            y += 0.036f;
            if (h.seen)
            {
                Text(g, s, x0 + 0.02f, y, bH, Fmt("%d O'CLOCK %.1f SEC", h.clock, h.distSec), kAmber);
                Bar(g, s, s.w - x0 - 0.10f, y + 0.004f, 0.10f, 0.019f, h.pct / 100.0f, LevelCol(h.pct / 100.0f), 4);
            }
            else
                Text(g, s, x0 + 0.02f, y, bH, "NO CONTACT", kDim);
            y += 0.050f;
        }
    }
    // POWER STATIONS
    {
        const Screen &s = g_scr[SCR_POWER];
        Text(g, s, x0, 0.022f, tH, "POWER STATIONS", kAmber);
        float y = 0.085f;
        Text(g, s, x0, y, bH, Fmt("OWN %d OF %d", S.psOwn, S.psTotal), S.psOwn ? kGreen : kRed);
        Text(g, s, s.w - x0, y, bH, Fmt("PWR %d", S.psPowerOwn), kWhite, 1);
        y += 0.056f;
        for (const Snap::Ps &p : S.ps)
        {
            const char *who = p.who == 1 ? "OWN" : (p.who == 2 ? "ENEMY" : (p.who == 0 ? "NEUTRAL" : "UNKNOWN"));
            const Col c = p.who == 1 ? kGreen : (p.who == 2 ? kRed : kDim);
            Text(g, s, x0, y, bH, Fmt("SEC %d,%d", p.x, p.y), kWhite);
            Text(g, s, 0.30f, y, bH, who, c);
            Text(g, s, s.w - x0, y, bH, p.who == 3 ? "?" : Fmt("%d", p.power), c, 1);
            y += 0.044f;
        }
        if (S.psTotal > (int)S.ps.size())
            Text(g, s, x0, y, bH * 0.85f, Fmt("+%d MORE", S.psTotal - (int)S.ps.size()), kDim);
    }
    // MAIN MONITOR (2.0 x 1.5 m at about 3.2 m): mission + radio.
    {
        const Screen &s = g_scr[SCR_MAIN];
        const float k = s.w / 2.0f; // scale with UA_VR_BRIDGE_MON width
        const float mh = 0.080f * k, mb = 0.062f * k, mx = 0.06f * k;
        // fit every line to the glass (Text() does not clip)
        auto Fit = [&](const std::string &t, float h) {
            const size_t n = (size_t)((s.w - 2 * mx) / (h * 6.0f / 7.0f));
            return t.size() > n ? t.substr(0, n) : t;
        };
        Text(g, s, mx, 0.05f * k, mh, Fit(S.level, mh), kAmber);
        Text(g, s, mx, 0.20f * k, mb, Fit(S.obj1, mb), kWhite);
        Text(g, s, mx, 0.30f * k, mb, Fit(S.obj2, mb), kCyan);
        Rect(g, s, mx, 0.41f * k, s.w - 2 * mx, 0.006f * k, Fade(kDim, 0.6f));
        Text(g, s, mx, 0.45f * k, mb, "RADIO", kAmber);
        float y = 0.56f * k;
        const size_t width = (size_t)((s.w - 2 * mx) / (mb * 6.0f / 7.0f));
        int lines = 0;
        for (int i = 0; i < S.nMsg && lines < 9; i++)
        {
            std::vector<std::string> wl;
            WrapLines(S.msg[i], width, wl, 3);
            const float a = S.msgAgeS[i] < 10 ? 1.0f : 0.65f;
            for (const std::string &l : wl)
            {
                if (lines >= 9)
                    break;
                Text(g, s, mx, y, mb, l, Fade(i == 0 ? kWhite : kDim, a));
                y += 0.094f * k;
                lines++;
            }
            y += 0.02f * k;
        }
        if (!S.nMsg)
            Text(g, s, mx, y, mb, "NO TRAFFIC", kDim);
    }
}

std::string KeyOf(const Snap &S)
{
    std::string k = Fmt("%d|%d|%d|%d|%d|%d|%d|%d|", S.energy / 100, S.energyMax, S.battVeh / 100, S.battBeam / 100, S.reloadNow, S.reloadMax,
                        S.fillMode, S.sectorState);
    for (const Snap::Sq &q : S.squads)
        k += Fmt("%s,%d,%d,%d,%d;", q.name.c_str(), q.n, q.pct, q.aggr, (int)q.sel);
    k += Fmt("|%d|%d|", S.nSquads, S.nUnits);
    for (const Snap::Host &h : S.hosts) // unseen hosts show only the name + NO CONTACT
        k += h.seen ? Fmt("%s,1,%d,%.1f,%d;", h.name.c_str(), h.clock, h.distSec, h.pct) : h.name + ",0;";
    for (const Snap::Ps &p : S.ps)
        k += Fmt("%d,%d,%d,%d;", p.x, p.y, p.who, p.who == 3 ? 0 : p.power); // unknown shows "?"
    k += Fmt("|%d|%d|%d|", S.psOwn, S.psTotal, S.psPowerOwn) + S.level + "|" + S.obj1 + "|" + S.obj2;
    for (int i = 0; i < S.nMsg; i++)
        k += "|" + S.msg[i] + (S.msgAgeS[i] < 10 ? "+" : "-");
    k += Fmt("|%d|", S.noticeCol) + S.notice;
    return k;
}

void TakeSnapshot(NC_STACK_ypaworld *yw, Snap &S)
{
    S.notice = Station::BridgeNotice(&S.noticeCol); // station line
    NC_STACK_ypabact *robo = yw->_userRobo;
    NC_STACK_yparobo *rb = dynamic_cast<NC_STACK_yparobo *>(robo);
    const int me = robo->_owner;

    // energy: the same fields and formulas as the 2D HUD's top panel
    S.energy = std::max(0, robo->_energy);
    S.energyMax = std::max(1, robo->_energy_max);
    if (rb)
    {
        S.battVeh = rb->getROBO_battVehicle();
        S.battBeam = rb->getROBO_battBeam();
        S.fillMode = rb->getROBO_fillMode();
        yw_arg176 a176;
        a176.owner = robo->_pSector ? robo->_pSector->owner : 0;
        yw->ypaworld_func176(&a176);
        const int idx = rb->getROBO_absReload();
        S.reloadMax = 10 * idx / 100;
        S.reloadNow = (int)(10 * (idx * a176.field_4) / 100);
        if (robo->_pSector)
        {
            const int pw = (int)(robo->_pSector->energy_power * a176.field_4); // as the HUD's v33
            S.sectorState = robo->_owner == robo->_pSector->owner ? (pw != 0 ? 1 : 0) : (pw ? -1 : 0);
        }
    }

    // squads (the station's commanders, like the wrist panel / squad manager)
    NC_STACK_ypabact *selCmd = NULL;
    if (yw->_activeCmdrRemapIndex >= 0 && yw->_activeCmdrRemapIndex < (int)yw->_cmdrsRemap.size())
        selCmd = yw->_cmdrsRemap[yw->_activeCmdrRemapIndex];
    for (NC_STACK_ypabact *cmd : robo->_kidList)
    {
        if (cmd->_status == BACT_STATUS_DEAD || cmd->_bact_type == BACT_TYPES_GUN)
            continue;
        Snap::Sq q;
        q.name = cmd->_vehicleID < yw->_vhclProtos.size() ? Upper(yw->GetVehicleName(yw->_vhclProtos[cmd->_vehicleID]), 13) : "?";
        q.n = 1;
        long e = std::max(0, cmd->_energy), em = std::max(1, cmd->_energy_max);
        for (NC_STACK_ypabact *u : cmd->_kidList)
        {
            if (u->_status == BACT_STATUS_DEAD)
                continue;
            q.n++;
            e += std::max(0, u->_energy);
            em += std::max(1, u->_energy_max);
        }
        q.pct = (int)std::max(0L, std::min(100L, 100 * e / em));
        q.aggr = (cmd->_aggr + 12) / 25 + 1; // 0..100 -> 1..5 as the squad manager buttons
        q.aggr = std::max(1, std::min(5, q.aggr));
        q.sel = cmd == selCmd;
        S.nSquads++;
        S.nUnits += q.n;
        if (S.squads.size() < 7)
            S.squads.push_back(q);
    }

    // enemy host stations: clock bearing from the station view, distance in sectors
    double fx = yw->_viewerRotation.m20, fz = yw->_viewerRotation.m22;
    const double fl = sqrt(fx * fx + fz * fz);
    if (fl > 1e-6) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
    const int myMask = cellArea::ViewMask(yw->GetPlayerOwner());
    for (NC_STACK_ypabact *st : yw->_unitsList)
    {
        if (st->_bact_type != BACT_TYPES_ROBO || st->_owner == me || st->_status == BACT_STATUS_DEAD)
            continue;
        Snap::Host h;
        h.name = st->_vehicleID < yw->_vhclProtos.size() ? Upper(yw->GetVehicleName(yw->_vhclProtos[st->_vehicleID]), 24) : "HOST STATION";
        h.seen = st->_pSector && (st->_pSector->view_mask & myMask) && !st->IsHiddenFor(yw->GetPlayerOwner());
        const double dx = st->_position.x - robo->_position.x, dz = st->_position.z - robo->_position.z;
        const double ahead = dx * fx + dz * fz, side = dx * fz - dz * fx; // side > 0 = right (as the wrist radar)
        double brg = atan2(side, ahead) * 180.0 / M_PI;
        if (brg < 0)
            brg += 360.0;
        h.clock = (int)floor(brg / 30.0 + 0.5) % 12;
        if (h.clock == 0)
            h.clock = 12;
        h.distSec = (float)(sqrt(dx * dx + dz * dz) / 1200.0); // CVSectorLength
        h.distSec = floorf(h.distSec * 10.0f + 0.5f) / 10.0f;
        h.pct = std::max(0, std::min(100, 100 * st->_energy / std::max(1, st->_energy_max)));
        S.nHosts++;
        if (S.hosts.size() < 4)
            S.hosts.push_back(h);
    }

    // power stations (the 2D map shows owners only for seen sectors)
    for (auto &kv : yw->_powerStations)
    {
        const TPowerStationInfo &p = kv.second;
        if (!p.pCell)
            continue;
        S.psTotal++;
        Snap::Ps q;
        q.x = p.CellId.x;
        q.y = p.CellId.y;
        q.power = p.EffectivePower;
        const bool seen = (p.pCell->view_mask & myMask) != 0;
        q.who = p.pCell->owner == me ? 1 : (!seen ? 3 : (p.pCell->owner == 0 ? 0 : 2));
        if (q.who == 1)
        {
            S.psOwn++;
            S.psPowerOwn += p.EffectivePower;
        }
        S.ps.push_back(q);
    }
    // own first, then the rest (stable by sector), then cut to what the screen shows
    std::stable_sort(S.ps.begin(), S.ps.end(), [](const Snap::Ps &a, const Snap::Ps &b) { return (a.who == 1) > (b.who == 1); });
    if (S.ps.size() > 6)
        S.ps.resize(6);

    // main monitor: mission + objective + radio
    S.level = Upper(yw->GetLevelName(yw->_levelInfo), 30);
    if (S.level.empty())
        S.level = Fmt("LEVEL %d", yw->_levelInfo.LevelID);
    S.obj1 = S.nHosts ? Fmt("DESTROY %d ENEMY HOST STATION%s", S.nHosts, S.nHosts == 1 ? "" : "S") : "NO ENEMY HOST STATIONS LEFT";
    S.obj2.clear();
    for (const TMapGate &gt : yw->_levelInfo.Gates)
    {
        if (!gt.PCell)
            continue;
        if (gt.PCell->PurposeType == cellArea::PT_GATEOPENED)
        {
            S.obj2 = "BEAM GATE OPEN: MOVE THE STATION IN";
            break;
        }
        int own = 0;
        for (const TMapKeySector &ks : gt.KeySectors)
            if (ks.PCell && ks.PCell->owner == me)
                own++;
        S.obj2 = Fmt("BEAM GATE: %d OF %d KEY SECTORS", own, (int)gt.KeySectors.size());
    }
    {
        int idx = info_log.msg_count;
        const int n = std::min(64, (int)info_log.numEntries);
        for (int k = 0; k < n && S.nMsg < 4; k++, idx = (idx + 63) % 64)
        {
            const inflog_msg &m = info_log.msgs[idx];
            const int age = (int)((yw->_timeStamp - m.field_4) / 1000);
            if (!m.txt[0] || age < 0 || age > 90)
                continue;
            S.msg[S.nMsg] = Upper(m.txt, 120);
            S.msgAgeS[S.nMsg] = age;
            S.nMsg++;
        }
    }
}

void LogSnapshot(NC_STACK_ypaworld *yw, const Snap &S)
{
    NC_STACK_ypabact *robo = yw->_userRobo;
    fprintf(stderr, "UAVR bridge: game robo energy %d/%d batt veh %d beam %d | screen MAIN %d/%d UNITS %d BEAM %d RELOAD %d/%d fill %d sector %d\n",
            robo->_energy, robo->_energy_max, S.battVeh, S.battBeam, S.energy / 100, S.energyMax / 100, S.battVeh / 100, S.battBeam / 100,
            S.reloadNow, S.reloadMax, S.fillMode, S.sectorState);
    std::string sq;
    for (const Snap::Sq &q : S.squads)
        sq += Fmt(" [%s%s n%d %d%% a%d]", q.sel ? ">" : "", q.name.c_str(), q.n, q.pct, q.aggr);
    fprintf(stderr, "UAVR bridge: squads %d units %d (kids of robo %d):%s\n", S.nSquads, S.nUnits, (int)robo->_kidList.size(), sq.c_str());
    std::string hs;
    for (const Snap::Host &h : S.hosts)
        hs += Fmt(" [%s %s %d o'clock %.1f sec %d%%]", h.name.c_str(), h.seen ? "seen" : "no contact", h.clock, h.distSec, h.pct);
    fprintf(stderr, "UAVR bridge: enemy hosts %d:%s\n", S.nHosts, hs.c_str());
    std::string ps;
    for (const Snap::Ps &p : S.ps)
        ps += Fmt(" [%d,%d who%d pwr%d]", p.x, p.y, p.who, p.power);
    fprintf(stderr, "UAVR bridge: power stations own %d of %d (map has %d) power %d:%s\n", S.psOwn, S.psTotal, (int)yw->_powerStations.size(),
            S.psPowerOwn, ps.c_str());
    fprintf(stderr, "UAVR bridge: monitor \"%s\" | \"%s\" | \"%s\" | radio %d%s%s\n", S.level.c_str(), S.obj1.c_str(), S.obj2.c_str(), S.nMsg,
            S.nMsg ? " newest: " : "", S.nMsg ? S.msg[0].c_str() : "");
}

// Saved GL state around our draws (the engine caches state; leave it as found).
struct GlSave
{
    GLint prog = 0, vao = 0, buf = 0, tex = 0, active = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0, depthFunc = 0;
    GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, depthMask = GL_TRUE;
    void Save()
    {
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        gl.ActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    }
    void Restore()
    {
        gl.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        glDepthFunc(depthFunc);
        glDepthMask(depthMask);
        glBindTexture(GL_TEXTURE_2D, tex);
        gl.ActiveTexture(active);
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, buf);
        gl.UseProgram(prog);
    }
};

// The screens' text and bars (dynamic VBO, re-uploaded only when the content changed).
void DrawText(int eye)
{
    gl.BindVertexArray(g_vaoDyn);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vboDyn);
    if (eye == 0 && g_dynDirty)
    {
        g_dynCount = (int)g_dyn.size(); // only when the content changed (at most 4x per second)
        gl.BufferData(GL_ARRAY_BUFFER, g_dynCount * sizeof(Vtx), g_dyn.data(), GL_DYNAMIC_DRAW);
        g_dynDirty = false;
    }
    if (g_dynCount > 0)
    {
        // glass text: depth-tested against the room (a post in front hides it), no depth write
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, g_dynCount);
    }
}

// Room (+ screens when `withScreens`) with the given model-view-projection.
// Depth is cleared first: the bridge is the nearest thing in the scene.
void DrawRoom(int eye, const M4 &mvp, bool withScreens)
{
    GlSave st;
    st.Save();
    gl.UseProgram(g_prog);
    UploadMVP(mvp);
    gl.Uniform1i(g_locAtlas, 0);
    gl.Uniform1i(g_locWear, g_wear);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    gl.BindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_roomVerts);

    if (withScreens)
        DrawText(eye);
    st.Restore();
}

// the screens alone over the mixed-reality table. No depth clear: frames and glass are
// depth-tested against the miniature (it is nearer and wins where they overlap) and drawn
// after Table::DioPassthroughAlpha, so they write their own alpha (frames 1, glass 0.86,
// premultiplied): the real room shows faintly through the glass, never through the text.
void DrawPanels(int eye, const M4 &mvp)
{
    GlSave st;
    st.Save();
    gl.UseProgram(g_prog);
    UploadMVP(mvp);
    gl.Uniform1i(g_locAtlas, 0);
    gl.Uniform1i(g_locWear, g_wear);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.BindVertexArray(g_vaoPan);
    glDrawArrays(GL_TRIANGLES, 0, g_panVerts);
    DrawText(eye);
    st.Restore();
}

bool g_atStation = false; // gameplay snapshot: the viewer is the user's live host station
uint32_t g_shownLogged = 0;

} // namespace

void Init(double unitsPerM)
{
    g_upm = unitsPerM;
    if (const char *w = getenv("UA_VR_BRIDGE_WEAR"))
        g_wear = std::max(0, std::min(2, atoi(w)));
    const char *l = getenv("UA_VR_BRIDGE_LOG");
    g_log = l && l[0] && strcmp(l, "0") != 0;
    if (const char *m = getenv("UA_VR_BRIDGE_MON"))
    {
        double d = 0, e = 0, w = 0;
        if (sscanf(m, "%lf,%lf,%lf", &d, &e, &w) == 3 && d > 0.8 && d < 20 && e > -30 && e < 70 && w > 0.3 && w < 10)
        {
            g_monDist = d;
            g_monElevDeg = e;
            g_monW = w;
        }
    }
    if (const char *mr = getenv("UA_VR_BRIDGE_MR"))
        g_mrPanels = strcmp(mr, "0") != 0;
    g_room.clear();
    BuildRoom(g_room); // CPU only; uploaded on the first draw
    g_pan.clear();
    BuildPanels(g_pan); // after BuildRoom (needs g_scr)
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    g_atStation = false;
    if (!Active() || !yw || !Enabled())
        return;
    NC_STACK_ypabact *robo = yw->_userRobo;
    if (!robo || yw->_viewerBact != robo || robo->_status == BACT_STATUS_DEAD || robo->_bact_type != BACT_TYPES_ROBO)
        return;
    g_atStation = true;
    const double now = NowMs();
    if (now - g_lastSnapMs < 250.0)
        return;
    g_lastSnapMs = now;
    Snap S;
    S.atStation = true;
    TakeSnapshot(yw, S);
    const std::string key = KeyOf(S);
    if (key != g_key)
    {
        g_key = key;
        g_dyn.clear();
        BuildScreens(g_dyn, S);
        g_dynDirty = true;
    }
    g_snap = S;
    if (g_log && now - g_lastLogMs >= 2000.0)
    {
        g_lastLogMs = now;
        LogSnapshot(yw, S);
    }
}

bool DrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor)
{
    if (!g_atStation || !Enabled())
        return false;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return false;
    M4 sc = M4::Ident();
    sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = g_upm; // metres -> world units
    DrawRoom(eye, eyeProj * centreFromAnchor * sc, true);
    if (g_log && eye == 0 && g_shownLogged++ % 600 == 0)
        fprintf(stderr, "UAVR bridge: drawn at the host station (room %d tris, screens %d tris)\n", g_roomVerts / 3, g_dynCount / 3);
    return true;
}

bool DrawPanelsEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor)
{
    if (!g_atStation || !Enabled() || !g_mrPanels)
        return false;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return false;
    M4 sc = M4::Ident();
    sc.m[0][0] = sc.m[1][1] = sc.m[2][2] = g_upm; // metres -> world units
    DrawPanels(eye, eyeProj * centreFromAnchor * sc);
    static uint32_t s_logged = 0;
    if (g_log && eye == 0 && s_logged++ % 600 == 0)
        fprintf(stderr, "UAVR bridge: MR panels over the table (passthrough room, no bridge metal): %d panel tris, screens %d tris, 2 draws per eye\n",
                g_panVerts / 3, g_dynCount / 3);
    return true;
}

bool Theatre(Backend *backend, const ViewState &views, float panelAspect, PanelLayer *panel)
{
    static int any = -1; // UA_VR_BRIDGE_THEATRE=1 (captures): every theatre frame counts as a briefing
    if (any < 0)
    {
        const char *v = getenv("UA_VR_BRIDGE_THEATRE");
        any = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    const bool noted = g_briefingNoted == g_presentNo || any == 1;
    g_presentNo++;
    if (!noted || !backend || !views.valid || !Active() || !Enabled())
        return false;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return false;

    // Tracking space = the anchor (metres, UAV axes). centre <- anchor, eye <- centre.
    const M3 rh = XrRotToUa(QuatToM3(views.head.orientation));
    double ph[3];
    XrPosToUa(views.head.position, 1.0, ph);
    M4 cfa = M4::Ident();
    {
        const M3 t = rh.T();
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                cfa.m[i][j] = t.m[i][j];
            cfa.m[i][3] = -(t.m[i][0] * ph[0] + t.m[i][1] * ph[1] + t.m[i][2] * ph[2]);
        }
    }

    GLint prevFbo = 0, vp[4] = {0, 0, 0, 0};
    GLfloat cc[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int e = 0; e < 2; e++)
    {
        const M3 re = XrRotToUa(QuatToM3(views.eye[e].pose.orientation));
        double pe[3];
        XrPosToUa(views.eye[e].pose.position, 1.0, pe);
        M4 efc = M4::Ident();
        const M3 t = re.T();
        double tp[3];
        t.Apply(pe, tp);
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
                efc.m[i][j] = t.m[i][j];
            efc.m[i][3] = -tp[i];
        }
        const M4 mvp = UaAsymFrustum(views.eye[e].fov, 0.05, 200.0) * efc * cfa;
        RenderTarget rt = backend->AcquireEye(e);
        gl.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        glViewport(0, 0, rt.width, rt.height);
        GLboolean dm = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        glDepthMask(GL_TRUE);
        glClearColor(0.020f, 0.022f, 0.026f, 1.0f); // night outside the windows
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDepthMask(dm);
        DrawRoom(e, mvp, false);
        backend->ReleaseEye(e);
    }
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    gl.BindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    // The 2D panel (briefing / debrief) onto the monitor glass, tracking space (XR axes).
    V3 c;
    float w, h;
    MonitorCentre(&c, &w, &h);
    const Screen &S = g_scr[SCR_MAIN];
    const V3 front = c - S.fwd * 0.012f; // just in front of the glass
    const float aspect = panelAspect > 0.1f ? panelAspect : 16.0f / 9.0f;
    float pw = w * 0.98f, phgt = pw / aspect;
    if (phgt > h * 0.98f)
    {
        phgt = h * 0.98f;
        pw = phgt * aspect;
    }
    panel->mode = PANEL_THEATRE;
    panel->widthM = pw;
    panel->heightM = phgt;
    panel->pose.position.x = front.x;
    panel->pose.position.y = -front.y;
    panel->pose.position.z = -front.z;
    panel->pose.orientation = QuatFromAxisAngle(1, 0, 0, atan2((double)-front.y, (double)front.z));
    static uint32_t theatreLogged = 0;
    if (g_log && theatreLogged++ % 300 == 0)
        fprintf(stderr, "UAVR bridge: briefing on the bridge monitor (%.2f x %.2f m at %.2f m, %.0f deg up)\n", pw, phgt,
                sqrt(front.y * front.y + front.z * front.z), atan2(-front.y, front.z) * 180.0 / M_PI);
    return true;
}

} // namespace Bridge
} // namespace VR
