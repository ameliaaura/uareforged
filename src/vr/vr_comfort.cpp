// Urban Assault VR — comfort layer. See vr_comfort.h and

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

#include "vr_comfort.h"
#include "vr_gl.h"

namespace VR
{

namespace
{

enum Horizon { HZ_YAW = 0, HZ_NOROLL = 1, HZ_FULL = 2 };

struct Cfg
{
    double upm = 11.0;
    Horizon ground = HZ_YAW, flyer = HZ_NOROLL;
    double pitchTauS = 0.15;   // flyer pitch low-pass (NOROLL mode)
    double bobTauS = 0.12;     // vertical anchor smoothing (ground), 0 = off
    double bobMaxM = 0.40;     // the filtered eye never strays further than this from the vehicle
    double eyeRaiseM = 0.50;   // ground class: eye this much above the engine viewpoint
    double eyeRaiseFlyerM = 0.0;
    double vignette = 0.5;     // 0..1 strength (player setting, or UA_VR_VIGNETTE)
    bool vignetteEnv = false;  // UA_VR_VIGNETTE set: overrides the player setting
    double vignetteForce = -1; // >= 0: fixed intensity (captures, frame-time measurement)
    double rotLoDeg = 10, rotHiDeg = 90;   // deg/s: vignette starts / full
    double accLo = 2.0, accHi = 12.0;     // perceived m/s^2: vignette starts / full
    double inS = 0.08, outS = 0.35;       // fade in / out time constants
    bool reference = true;
    double refAlpha = 0.45;
    bool log = false;
};
Cfg C;

// Player setting (Settings page / controller combo / profile), percent.
// Survives ComfortInit (the profile may be parsed before or after it).
// more levels (adjustable in-mission). LOW 50 stays the default
// and HIGH 85 keeps its value, so profiles still show their preset names.
const int kPresetN = 6;
const int kPresetPct[kPresetN] = {0, 25, 50, 70, 85, 100}; // Off, Very low, Low, Medium, High, Max
int g_userPct = 50; // LOW (default)

bool EnvOn(const char *name, bool def)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    return strcmp(v, "0") != 0 && strcmp(v, "off") != 0;
}

double EnvF(const char *name, double def)
{
    const char *v = getenv(name);
    return (v && v[0]) ? atof(v) : def;
}

Horizon EnvHorizon(const char *name, Horizon def)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    if (!strcmp(v, "yaw")) return HZ_YAW;
    if (!strcmp(v, "noroll")) return HZ_NOROLL;
    if (!strcmp(v, "full")) return HZ_FULL;
    fprintf(stderr, "UAVR comfort: %s=%s unknown (yaw|noroll|full), using default\n", name, v);
    return def;
}

const char *HorizonName(Horizon h) { return h == HZ_YAW ? "yaw" : h == HZ_NOROLL ? "noroll" : "full"; }

#ifdef UA_VR_COMFORT_TEST
double g_testClock = -1; // tools/vr/comfort_test.cpp drives time explicitly
#endif

double NowS()
{
#ifdef UA_VR_COMFORT_TEST
    if (g_testClock >= 0)
        return g_testClock;
#endif
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

double Clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
double Ramp(double x, double lo, double hi) { return hi > lo ? Clamp01((x - lo) / (hi - lo)) : (x >= lo ? 1.0 : 0.0); }

// ---- per-frame state ------------------------------------------------------
struct State
{
    bool valid = false;
    double t = 0;
    bool flyer = false;
    uint64_t key = 0;         // viewer object + level identity
    double fh[3] = {0, 0, 1}; // tracked heading (horizontal unit vector)
    M3 raw = M3::Ident();     // last RAW vehicle attitude (heading tracker)
    double pitch = 0;         // smoothed pitch (rad, nose up +), relative to the heading
    M3 rot = M3::Ident();     // last applied anchor
    // vertical filter (UA y, down +)
    double y = 0, vy = 0, ytPrev = 0, vt = 0;
    double posPrev[3] = {0, 0, 0};
    double view[3] = {0, 0, 0};       // last view position shown
    double vel[3] = {0, 0, 0}, velPrev[3] = {0, 0, 0}; // smoothed view velocity, m/s
    double accS = 0, rotS = 0;
    double vig = 0; // current vignette intensity 0..1
    // log accumulators
    double logT = 0, logRawY = 0, logViewY = 0, logRawV = 0, logViewV = 0, logRaw2 = 0, logView2 = 0;
    double logRot = 0, logAcc = 0, logVig = 0;
    int logN = 0;
    double profMs = 0;
    int profN = 0;
};
State S;

// ---- GL ---------------------------------------------------------------------
const char *kVs =
    "#version 410 core\n"
    "uniform mat4 MVP;\n"
    "uniform vec4 uMode;\n" // x > 0.5: vignette ring vertex (pos = cos, sin, ring index)
    "uniform vec4 uVigP;\n" // tangent -> NDC: p00, p02, p11, p12
    "uniform vec4 uVigR;\n" // ring radii (tangent) 0, 1, 2, max alpha
    "in vec3 vPos;\n"
    "in vec4 vCol;\n"
    "out vec4 col;\n"
    "void main() {\n"
    "  if (uMode.x > 0.5) {\n"
    "    float r = vPos.z < 0.5 ? uVigR.x : (vPos.z < 1.5 ? uVigR.y : uVigR.z);\n"
    "    vec2 t = vPos.xy * r;\n"
    "    gl_Position = vec4(uVigP.x * t.x + uVigP.y, uVigP.z * t.y + uVigP.w, 0.0, 1.0);\n"
    "    col = vec4(0.0, 0.0, 0.0, vCol.a * uVigR.w);\n"
    "  } else {\n"
    "    col = vCol;\n"
    "    gl_Position = MVP * vec4(vPos, 1.0);\n"
    "  }\n"
    "}\n";
const char *kFs =
    "#version 410 core\n"
    "in vec4 col;\n"
    "out vec4 frag;\n"
    "void main() { frag = col; }\n";

GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
GLint g_locMVP = -1, g_locMode = -1, g_locVigP = -1, g_locVigR = -1;
GLsizei g_refCount = 0, g_vigFirst = 0, g_vigCount = 0; // static VBO layout
bool g_glOk = false, g_glTried = false;
std::vector<float> g_ref;   // static reference triangles, anchor-local units, x y z r g b a
const int kVigSeg = 48;

GLuint Compile(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
#ifdef UA_GLES
    std::string es = std::string("#version 300 es\nprecision mediump float;\n") + (strchr(src, '\n') + 1);
    src = es.c_str();
#endif
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        gl.GetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UAVR comfort: shader error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

void PushQuad(std::vector<float> &v, const double a[3], const double b[3], const double c[3], const double d[3],
              const float ca[4], const float cb[4], const float cc[4], const float cd[4]);

bool InitGL()
{
    g_glTried = true;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
        return false;
    g_prog = gl.CreateProgram();
    gl.AttachShader(g_prog, vs);
    gl.AttachShader(g_prog, fs);
    gl.BindAttribLocation(g_prog, 0, "vPos");
    gl.BindAttribLocation(g_prog, 1, "vCol");
    gl.LinkProgram(g_prog);
    GLint ok = 0;
    gl.GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!ok)
    {
        fprintf(stderr, "UAVR comfort: program link failed; vignette/reference off\n");
        return false;
    }
    g_locMVP = gl.GetUniformLocation(g_prog, "MVP");
    g_locMode = gl.GetUniformLocation(g_prog, "uMode");
    g_locVigP = gl.GetUniformLocation(g_prog, "uVigP");
    g_locVigR = gl.GetUniformLocation(g_prog, "uVigR");
    GLint prevVao = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.GenBuffers(1, &g_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    // One static buffer: reference triangles, then the unit vignette ring
    // (radii, alpha and the per-eye projection are uniforms: nothing is
    // uploaded per frame).
    std::vector<float> v = g_ref;
    g_refCount = (GLsizei)(g_ref.size() / 7);
    g_vigFirst = g_refCount;
    const float c0[4] = {0, 0, 0, 0}, c1[4] = {0, 0, 0, 1};
    for (int i = 0; i < kVigSeg; i++)
    {
        const double a0 = 2.0 * M_PI * i / kVigSeg, a1 = 2.0 * M_PI * (i + 1) / kVigSeg;
        double P[3][2][3];
        for (int ring = 0; ring < 3; ring++)
        {
            P[ring][0][0] = cos(a0); P[ring][0][1] = sin(a0); P[ring][0][2] = ring;
            P[ring][1][0] = cos(a1); P[ring][1][1] = sin(a1); P[ring][1][2] = ring;
        }
        PushQuad(v, P[0][0], P[0][1], P[1][1], P[1][0], c0, c0, c1, c1);
        PushQuad(v, P[1][0], P[1][1], P[2][1], P[2][0], c1, c1, c1, c1);
    }
    g_vigCount = (GLsizei)(v.size() / 7) - g_vigFirst;
    gl.BufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    return true;
}

void PushV(std::vector<float> &v, double x, double y, double z, const float c[4])
{
    v.push_back((float)x); v.push_back((float)y); v.push_back((float)z);
    v.push_back(c[0]); v.push_back(c[1]); v.push_back(c[2]); v.push_back(c[3]);
}

void PushQuad(std::vector<float> &v, const double a[3], const double b[3], const double c[3], const double d[3],
              const float ca[4], const float cb[4], const float cc[4], const float cd[4])
{
    PushV(v, a[0], a[1], a[2], ca); PushV(v, b[0], b[1], b[2], cb); PushV(v, c[0], c[1], c[2], cc);
    PushV(v, a[0], a[1], a[2], ca); PushV(v, c[0], c[1], c[2], cc); PushV(v, d[0], d[1], d[2], cd);
}

// Static reference in anchor-local UAV coords (x right, y down, z forward),
// metres scaled to units: a thin cockpit rim arc below the HUD panel and a
// small nose chevron at its centre. Dim, desaturated, partly transparent.
void BuildReference()
{
    g_ref.clear();
    const double u = C.upm;
    const double R = 0.70, drop = 0.50, h = 0.016; // rim radius, below eye, band height (m)
    const double span = 62.0 * M_PI / 180.0;
    const int N = 40;
    const float base[3] = {0.62f, 0.68f, 0.66f};
    for (int i = 0; i < N; i++)
    {
        const double p0 = -span + 2.0 * span * i / N, p1 = -span + 2.0 * span * (i + 1) / N;
        // taper the alpha towards the arc ends
        const double t0 = fabs(p0) / span, t1 = fabs(p1) / span;
        const float a0 = (float)(C.refAlpha * (1.0 - t0 * t0 * t0)), a1 = (float)(C.refAlpha * (1.0 - t1 * t1 * t1));
        const float c0[4] = {base[0], base[1], base[2], a0}, c1[4] = {base[0], base[1], base[2], a1};
        const double A[3] = {R * sin(p0) * u, (drop - h / 2) * u, R * cos(p0) * u};
        const double B[3] = {R * sin(p1) * u, (drop - h / 2) * u, R * cos(p1) * u};
        const double Cc[3] = {R * sin(p1) * u, (drop + h / 2) * u, R * cos(p1) * u};
        const double D[3] = {R * sin(p0) * u, (drop + h / 2) * u, R * cos(p0) * u};
        PushQuad(g_ref, A, B, Cc, D, c0, c1, c1, c0);
    }
    // nose chevron: flat on the rim plane, pointing forward, just inside the rim
    const float cn[4] = {0.70f, 0.78f, 0.74f, (float)std::min(1.0, C.refAlpha * 1.3)};
    const double tipZ = R - 0.015, armZ = R - 0.075, armX = 0.045, w = 0.010;
    for (int s = -1; s <= 1; s += 2)
    {
        const double A[3] = {0.0, drop * u, tipZ * u};
        const double B[3] = {s * armX * u, drop * u, armZ * u};
        const double Cc[3] = {s * armX * u, drop * u, (armZ - w * 1.6) * u};
        const double D[3] = {0.0, drop * u, (tipZ - w * 1.6) * u};
        PushQuad(g_ref, A, B, Cc, D, cn, cn, cn, cn);
    }
}

void UploadMVP(const M4 &mvp)
{
    float m[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[j * 4 + i] = (float)mvp.m[i][j]; // column-major
    gl.UniformMatrix4fv(g_locMVP, 1, GL_FALSE, m);
}

// Vignette uniforms for this eye: radial around the eye axis, radii are view
// angles from the axis; k = intensity * strength.
void SetVignette(const Fovf &fov, double k)
{
    const double l = tan(fov.angleLeft), r = tan(fov.angleRight), up = tan(fov.angleUp), dn = tan(fov.angleDown);
    // UaAsymFrustum: ndc.x = p00 * tx + p02 (tx = x/z, right +), ndc.y = p11 * ty + p12 (ty = y/z, DOWN +)
    const double p00 = 2.0 / (r - l), p02 = -(r + l) / (r - l);
    const double p11 = -2.0 / (up - dn), p12 = -(up + dn) / (up - dn);
    const double deg = M_PI / 180.0;
    // k 0.27 (stick turn at the tank's 46 deg/s, default strength): clear to 41 deg,
    // edge (~50 deg) ~35 % dark. k 0.6 (full drive): clear to 31 deg, black from 49 deg.
    const double inner = (50.0 + (18.0 - 50.0) * k) * deg;         // clear disc radius
    const double outer = inner + (25.0 + (14.0 - 25.0) * k) * deg; // darkest beyond
    const double far = 85.0 * deg;
    gl.Uniform4f(g_locMode, 1.0f, 0, 0, 0);
    gl.Uniform4f(g_locVigP, (float)p00, (float)p02, (float)p11, (float)p12);
    gl.Uniform4f(g_locVigR, (float)tan(inner), (float)tan(outer), (float)tan(far), (float)std::min(1.0, 2.0 * k));
}

double LowPassK(double dt, double tau) { return tau > 0 ? 1.0 - exp(-dt / tau) : 1.0; }

double Smooth01(double x, double lo, double hi)
{
    const double t = Clamp01((x - lo) / (hi - lo));
    return t * t * (3.0 - 2.0 * t);
}

// One-off heading for a fresh attitude (reset): projected forward (reversed
// when inverted), else the projected right axis, else +Z.
void InitialHeading(const M3 &r, double fh[3])
{
    double x = r.m[2][0], z = r.m[2][2];
    if (r.m[1][1] < 0.0) { x = -x; z = -z; } // inverted: belly-down travel direction
    double len = sqrt(x * x + z * z);
    if (len < 0.05)
    {
        x = -r.m[0][2]; z = r.m[0][0]; // right (rx, rz) -> forward (-rz, rx)
        len = sqrt(x * x + z * z);
    }
    if (len < 1e-6) { x = 0; z = 1; len = 1; }
    fh[0] = x / len; fh[1] = 0; fh[2] = z / len;
}

// Stateful, continuous heading. No continuous yaw function of attitude alone
// exists over all of SO(3), so the heading is TRACKED: each frame it turns by
// the vehicle's rotation about world vertical (from the raw attitude delta),
// then is pulled towards the projected forward axis only where that is well
// defined (forward far from vertical and the vehicle upright), rate-limited.
// Continuous in time for any continuous attitude path, including loops,
// knife-edge and straight up/down; on the ground it equals the vehicle yaw.
double ForwardWeight(const M3 &r)
{
    const double flen = sqrt(r.m[2][0] * r.m[2][0] + r.m[2][2] * r.m[2][2]);
    return Smooth01(flen, 0.35, 0.70) * Smooth01(r.m[1][1], 0.0, 0.30);
}

void TrackHeading(const M3 &prev, const M3 &cur, double dt, double fh[3])
{
    // yaw increment: the projected-forward heading change where that is well
    // defined, the world-vertical component of the attitude delta elsewhere,
    // blended by how well defined the forward projection is (both frames)
    const M3 q = cur.T() * prev; // world-frame delta rotation (body->world is M^T)
    const double s = std::max(-1.0, std::min(1.0, 0.5 * (q.m[0][2] - q.m[2][0])));
    const double c = std::max(-1.0, std::min(1.0, 0.5 * (q.m[0][0] + q.m[2][2])));
    double dpsi = atan2(s, c);
    const double wPrev = ForwardWeight(prev), w = ForwardWeight(cur);
    const double wd = std::min(wPrev, w);
    if (wd > 0.0)
    {
        const double a0 = atan2(prev.m[2][0], prev.m[2][2]), a1 = atan2(cur.m[2][0], cur.m[2][2]);
        double dproj = a1 - a0;
        while (dproj > M_PI) dproj -= 2.0 * M_PI;
        while (dproj < -M_PI) dproj += 2.0 * M_PI;
        // (heading angle atan2(x, z) grows with the rotation sense used below)
        dpsi = wd * dproj + (1.0 - wd) * dpsi;
    }
    double x = cos(dpsi) * fh[0] + sin(dpsi) * fh[2];
    double z = -sin(dpsi) * fh[0] + cos(dpsi) * fh[2];

    // drift correction towards the projected forward, only where defined;
    // gain rises smoothly to 1 at full definition, rate-capped (never a step)
    if (w > 0.0 && dt > 0.0)
    {
        const double rx = cur.m[2][0], rz = cur.m[2][2];
        const double flen = sqrt(rx * rx + rz * rz);
        const double hx = rx / flen, hz = rz / flen;
        const double ang = atan2(z * hx - x * hz, x * hx + z * hz);
        const double k = LowPassK(dt, 0.05);
        const double gain = w * (k + (1.0 - k) * Smooth01(w, 0.90, 1.0));
        const double cap = (M_PI / 3.0) * dt; // drift catch-up <= 60 deg/s
        const double corr = std::max(-cap, std::min(cap, ang * gain));
        const double nx = cos(corr) * x + sin(corr) * z, nz = -sin(corr) * x + cos(corr) * z;
        x = nx; z = nz;
    }
    const double len = sqrt(x * x + z * z);
    fh[0] = x / len; fh[1] = 0; fh[2] = z / len;
}

double WrapPi(double a)
{
    while (a > M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

// Rows right/down/forward from heading fh and pitch (nose up +).
M3 FromHeadingPitch(const double fh[3], double pitch)
{
    // right = down x forward with down = (0,1,0): (fz, 0, -fx)
    const double rx = fh[2], rz = -fh[0];
    const double cp = cos(pitch), sp = sin(pitch);
    const double f[3] = {cp * fh[0], -sp, cp * fh[2]}; // up = -y
    // down = forward x right
    const double d[3] = {f[1] * rz - f[2] * 0.0, f[2] * rx - f[0] * rz, f[0] * 0.0 - f[1] * rx};
    M3 o;
    o.m[0][0] = rx;   o.m[0][1] = 0;    o.m[0][2] = rz;
    o.m[1][0] = d[0]; o.m[1][1] = d[1]; o.m[1][2] = d[2];
    o.m[2][0] = f[0]; o.m[2][1] = f[1]; o.m[2][2] = f[2];
    return o;
}


} // namespace

// ---------------------------------------------------------------------------

void ComfortInit(double unitsPerM)
{
    C = Cfg();
    C.upm = unitsPerM;
    // UA_VR_HORIZON_LOCK=1 = roll removed for every class.
    if (EnvOn("UA_VR_HORIZON_LOCK", false))
        C.ground = C.flyer = HZ_NOROLL;
    if (const char *all = getenv("UA_VR_HORIZON"))
        if (all[0])
            C.ground = C.flyer = EnvHorizon("UA_VR_HORIZON", C.ground);
    C.ground = EnvHorizon("UA_VR_HORIZON_GROUND", C.ground);
    C.flyer = EnvHorizon("UA_VR_HORIZON_FLYER", C.flyer);
    C.pitchTauS = std::max(0.0, EnvF("UA_VR_PITCH_TAU", C.pitchTauS));
    C.bobTauS = EnvF("UA_VR_BOB_TAU", C.bobTauS);
    C.bobTauS = C.bobTauS <= 0.0 ? 0.0 : std::max(0.02, C.bobTauS); // 0 = off; else >= 20 ms
    C.bobMaxM = std::max(0.0, EnvF("UA_VR_BOB_MAX_M", C.bobMaxM));
    C.eyeRaiseM = EnvF("UA_VR_EYE_RAISE_M", C.eyeRaiseM);
    C.eyeRaiseFlyerM = EnvF("UA_VR_EYE_RAISE_FLYER_M", C.eyeRaiseFlyerM);
    C.vignetteEnv = getenv("UA_VR_VIGNETTE") && getenv("UA_VR_VIGNETTE")[0];
    C.vignette = C.vignetteEnv ? Clamp01(EnvF("UA_VR_VIGNETTE", 0.5)) : g_userPct / 100.0;
    C.vignetteForce = EnvF("UA_VR_VIGNETTE_FORCE", -1.0);
    C.rotLoDeg = EnvF("UA_VR_VIGNETTE_ROT", C.rotLoDeg);
    C.rotHiDeg = EnvF("UA_VR_VIGNETTE_ROT_FULL", C.rotHiDeg);
    C.accLo = EnvF("UA_VR_VIGNETTE_ACC", C.accLo);
    C.accHi = EnvF("UA_VR_VIGNETTE_ACC_FULL", C.accHi);
    C.reference = EnvOn("UA_VR_REFERENCE", C.reference);
    C.refAlpha = Clamp01(EnvF("UA_VR_REFERENCE_ALPHA", C.refAlpha));
    C.log = EnvOn("UA_VR_COMFORT_LOG", false);
    S = State();
    BuildReference();
    fprintf(stderr, "UAVR comfort: horizon ground %s flyer %s (pitch tau %.2f s), bob tau %.2f s max %.2f m, "
                    "eye raise %.2f m (flyer %.2f), vignette %.2f%s, reference %d\n",
            HorizonName(C.ground), HorizonName(C.flyer), C.pitchTauS, C.bobTauS, C.bobMaxM, C.eyeRaiseM,
            C.eyeRaiseFlyerM, C.vignette, C.vignetteForce >= 0 ? " (forced)" : "", (int)C.reference);
}

void ComfortAnchor(M3 *ra, double pos[3], bool flyer, const double velMps[3], uint64_t viewerKey)
{
    const double now = NowS();
    const double gap = S.valid ? now - S.t : 1e9;
    double dt = std::min(std::max(gap, 0.0), 0.1);
    const double u = C.upm;
    const double perceived = 6.0 / u; // physics metres are 6 units, the VR metre is C.upm units

    // Snap (reset every filter) on: first frame, another viewer object or level
    // (viewerKey), class change, a gap in world frames > 0.25 s (load, pause,
    // session resumed), or a > 30 m jump (beam/teleport).
    bool reset = !S.valid || flyer != S.flyer || viewerKey != S.key || gap > 0.25;
    if (S.valid)
    {
        const double dx = pos[0] - S.posPrev[0], dy = pos[1] - S.posPrev[1], dz = pos[2] - S.posPrev[2];
        if (sqrt(dx * dx + dy * dy + dz * dz) > 30.0 * u) // 30 m in one frame
            reset = true;
    }

    // ---- rotation -----------------------------------------------------------
    const Horizon mode = flyer ? C.flyer : C.ground;
    if (reset)
        InitialHeading(*ra, S.fh);
    else
        TrackHeading(S.raw, *ra, dt, S.fh);
    S.raw = *ra;
    // pitch relative to the tracked heading, continuous through vertical
    const double fwdH = ra->m[2][0] * S.fh[0] + ra->m[2][2] * S.fh[2];
    const double rawPitch = atan2(-ra->m[2][1], fwdH);
    S.pitch = reset ? rawPitch : WrapPi(S.pitch + WrapPi(rawPitch - S.pitch) * LowPassK(dt, C.pitchTauS));
    if (mode != HZ_FULL)
        *ra = FromHeadingPitch(S.fh, mode == HZ_NOROLL ? S.pitch : 0.0);

    // ---- position: eye raise + vertical smoothing (UA y is down) -----------
    const double raise = (flyer ? C.eyeRaiseFlyerM : C.eyeRaiseM) * u;
    const double yt = pos[1] - raise;
    const bool smooth = !flyer && C.bobTauS > 0.0;
    if (reset || !smooth || dt <= 0.0)
    {
        if (reset || !smooth)
        {
            S.y = yt; S.vy = 0; S.vt = 0;
        }
    }
    else
    {
        // Critically damped follower with target-velocity feed-forward: a steady
        // slope is tracked with no lag, short bumps are filtered (cut-off ~1/(2 pi tau)).
        const double vtRaw = (yt - S.ytPrev) / dt;
        S.vt += (vtRaw - S.vt) * LowPassK(dt, std::min(0.6, std::max(0.25, 3.0 * C.bobTauS)));
        const double w = 1.0 / C.bobTauS;
        // semi-implicit Euler: stable for w*h <= ~0.25 (tau >= 0.02 s enforced in init)
        const double hMax = std::min(0.004, 0.25 * C.bobTauS);
        const int n = std::max(1, (int)ceil(dt / hMax));
        const double h = dt / n;
        for (int i = 1; i <= n; i++)
        {
            const double target = S.ytPrev + (yt - S.ytPrev) * i / n;
            const double acc = w * w * (target - S.y) + 2.0 * w * (S.vt - S.vy);
            S.vy += acc * h;
            S.y += S.vy * h;
        }
        const double maxDev = C.bobMaxM * u;
        if (S.y > yt + maxDev) { S.y = yt + maxDev; S.vy = std::min(S.vy, S.vt); }
        if (S.y < yt - maxDev) { S.y = yt - maxDev; S.vy = std::max(S.vy, S.vt); }
    }
    S.ytPrev = yt;
    S.posPrev[0] = pos[0]; S.posPrev[1] = pos[1]; S.posPrev[2] = pos[2];
    const double viewPos[3] = {pos[0], S.y, pos[2]};

    // ---- vignette drive: artificial rotation + linear acceleration ---------
    if (!reset && dt > 0.0)
    {
        const M3 d = S.rot * ra->T();
        const double c = std::max(-1.0, std::min(1.0, (d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0) * 0.5));
        const double rotDeg = acos(c) / dt * 180.0 / M_PI;
        S.rotS += (rotDeg - S.rotS) * LowPassK(dt, 0.05);

        // acceleration from the physics velocity, smoothed, in PERCEIVED m/s^2
        double acc2 = 0;
        for (int i = 0; i < 3; i++)
        {
            S.velPrev[i] = S.vel[i];
            S.vel[i] += (velMps[i] * perceived - S.vel[i]) * LowPassK(dt, 0.05);
            const double a = (S.vel[i] - S.velPrev[i]) / dt;
            acc2 += a * a;
        }
        S.accS += (sqrt(acc2) - S.accS) * LowPassK(dt, 0.15);

        const double target = std::max(Ramp(S.rotS, C.rotLoDeg, C.rotHiDeg), Ramp(S.accS, C.accLo, C.accHi));
        S.vig += (target - S.vig) * LowPassK(dt, target > S.vig ? C.inS : C.outS);
    }
    else
    {
        // fresh view: no carried-over darkness; velocity history seeded from the
        // destination vehicle so a moving vehicle shows no false acceleration
        S.vig = 0;
        S.rotS = S.accS = 0;
        for (int i = 0; i < 3; i++)
            S.vel[i] = S.velPrev[i] = velMps[i] * perceived;
    }

    // UA_VR_COMFORT_LOG=1: once a second, vignette drive and the vertical
    // acceleration RMS of the raw anchor vs the view actually shown (m/s^2).
    if (C.log && !reset && dt > 0.0)
    {
        const double vr = (yt - S.logRawY) / dt, vs = (viewPos[1] - S.logViewY) / dt;
        if (S.logN > 0)
        {
            const double ar = (vr - S.logRawV) / dt / u, as = (vs - S.logViewV) / dt / u;
            S.logRaw2 += ar * ar;
            S.logView2 += as * as;
        }
        S.logRawV = vr; S.logViewV = vs;
        S.logN++;
        S.logRot = std::max(S.logRot, S.rotS);
        S.logAcc = std::max(S.logAcc, S.accS);
        S.logVig = std::max(S.logVig, S.vig);
        if (now - S.logT >= 1.0 && S.logN > 1)
        {
            fprintf(stderr, "UAVR comfort: vig max %.2f | rot max %.0f deg/s | acc max %.1f m/s2 | vert acc rms raw %.2f view %.2f m/s2 | speed %.1f m/s | overlay %.3f ms/eye\n",
                    S.logVig, S.logRot, S.logAcc, sqrt(S.logRaw2 / (S.logN - 1)), sqrt(S.logView2 / (S.logN - 1)),
                    sqrt(S.vel[0] * S.vel[0] + S.vel[2] * S.vel[2]), S.profN ? S.profMs / S.profN : 0.0);
            S.profMs = 0; S.profN = 0;
            S.logT = now; S.logN = 0; S.logRaw2 = S.logView2 = 0; S.logRot = S.logAcc = S.logVig = 0;
        }
    }
    S.logRawY = yt; S.logViewY = viewPos[1];
    S.view[0] = viewPos[0]; S.view[1] = viewPos[1]; S.view[2] = viewPos[2];
    S.rot = *ra;
    S.key = viewerKey;
    S.flyer = flyer;
    S.t = now;
    S.valid = true;
    pos[1] = viewPos[1];
}

int ComfortVignettePercent() { return g_userPct; }

void SetComfortVignettePercent(int pct)
{
    g_userPct = std::max(0, std::min(100, pct));
    if (!C.vignetteEnv)
        C.vignette = g_userPct / 100.0;
}

const char *ComfortVignettePreset(int pct)
{
    static const char *names[kPresetN] = {"OFF", "VERY LOW", "LOW", "MEDIUM", "HIGH", "MAX"};
    for (int i = 0; i < kPresetN; i++)
        if (pct == kPresetPct[i])
            return names[i];
    return "CUSTOM";
}

int CycleComfortVignette()
{
    // Off -> Very low -> Low -> Medium -> High -> Max -> Off; a custom value goes to
    // the next preset above it.
    int next = kPresetPct[0];
    for (int i = 0; i < kPresetN; i++)
        if (kPresetPct[i] > g_userPct)
        {
            next = kPresetPct[i];
            break;
        }
    SetComfortVignettePercent(next);
    return next;
}

const char *ComfortFilterCombo(ControllerState c[2])
{
    // Both thumbstick clicks = cycle the vignette preset. Each click is held
    // back for up to kWait waiting for the other one, so the game never sees a
    // click that belongs to the combo (the right click is the missile camera,
    // the left one the HUD toggle in winp.cpp). A lone click reaches the game
    // kWait late; a short tap is replayed for a few frames after release.
    const double kWait = 0.12;
    static bool swallow = false;
    static double downSince[2] = {-1, -1};
    static bool passed[2] = {false, false};
    static int replay[2] = {0, 0};
    const double now = NowS();
    bool raw[2];
    for (int h = 0; h < 2; h++)
        raw[h] = c[h].active && (c[h].buttons & BTN_STICK_CLICK);
    for (int h = 0; h < 2; h++)
        c[h].buttons &= ~BTN_STICK_CLICK;

    if (swallow)
    {
        if (!raw[0] && !raw[1])
            swallow = false;
        return NULL;
    }
    if (raw[0] && raw[1] && !passed[0] && !passed[1])
    {
        swallow = true;
        for (int h = 0; h < 2; h++) { downSince[h] = -1; passed[h] = false; replay[h] = 0; }
        const int pct = CycleComfortVignette();
        static char label[64];
        snprintf(label, sizeof(label), "TUNNEL VISION: %s (%d%%)", ComfortVignettePreset(pct), pct);
        return label;
    }
    for (int h = 0; h < 2; h++)
    {
        if (raw[h])
        {
            if (downSince[h] < 0)
                downSince[h] = now;
            if (passed[h] || now - downSince[h] >= kWait)
            {
                passed[h] = true;
                c[h].buttons |= BTN_STICK_CLICK;
            }
        }
        else
        {
            if (downSince[h] >= 0 && !passed[h])
                replay[h] = 3; // short tap: deliver it now
            downSince[h] = -1;
            passed[h] = false;
            if (replay[h] > 0)
            {
                c[h].buttons |= BTN_STICK_CLICK;
                replay[h]--;
            }
        }
    }
    return NULL;
}

float ComfortVignette()
{
    return (float)(C.vignetteForce >= 0 ? C.vignetteForce : S.vig);
}

void ComfortDrawEye(int eye, const M4 &eyeProj, const M4 &centreFromAnchor, const Fovf &fov, bool reference)
{
    (void)eye;
    const double k = ComfortVignette() * C.vignette;
    const bool vig = k > 0.01;
    const bool ref = reference && C.reference && !g_ref.empty();
    if (!vig && !ref)
        return;
    if (!g_glTried)
        g_glOk = InitGL();
    if (!g_glOk)
        return;

    // Save the GL state the engine's state cache relies on.
    GLint prevProg = 0, prevVao = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
                    cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean depthMask = GL_TRUE;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    // UA_VR_COMFORT_PROFILE=1: glFinish around the overlay to time it alone
    // (CPU + GPU, ms, reported in the UA_VR_COMFORT_LOG line). Dev only: stalls.
    static int profile = -1;
    if (profile < 0)
        profile = EnvOn("UA_VR_COMFORT_PROFILE", false) ? 1 : 0;
    double t0 = 0;
    if (profile)
    {
        glFinish();
        t0 = NowS();
    }

    gl.UseProgram(g_prog);
    gl.BindVertexArray(g_vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    if (ref)
    {
        gl.Uniform4f(g_locMode, 0, 0, 0, 0);
        UploadMVP(eyeProj * centreFromAnchor);
        glDrawArrays(GL_TRIANGLES, 0, g_refCount);
    }
    if (vig)
    {
        SetVignette(fov, std::min(1.0, k));
        glDrawArrays(GL_TRIANGLES, g_vigFirst, g_vigCount);
    }

    gl.BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glDepthMask(depthMask);
    gl.BindVertexArray(prevVao);
    gl.UseProgram(prevProg);
    if (profile)
    {
        glFinish();
        S.profMs += (NowS() - t0) * 1000.0;
        S.profN++;
    }
}

#ifdef UA_VR_COMFORT_TEST
void ComfortTestSetClock(double t) { g_testClock = t; }
#endif

} // namespace VR
