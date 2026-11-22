// Frame-rate independence of the 1998 per-frame code (src/frame_rate.h): steering sliders,
// braking / drift factors, AI scan steps, repeated contact damage. Each behaviour is run at
// 20 / 30 / 60 / 144 / 240 / 1000 fps on the 0.8.5 clock and compared with the 1998 code at the
// 60 fps cap (the 0.8.4 behaviour). No SDL, no game data.
//   c++ -std=c++14 -Wall -Isrc tests/frame_rate_test.cpp src/game_clock.cpp -o frame_rate_test
//   frame_rate_test -v   prints the measured table
#include "frame_rate.h"
#include "game_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace FrameRate;

static int g_checks = 0, g_failed = 0;
static bool g_verbose = false;

static void Check(bool ok, const char *what, double fps, double a, double b)
{
    g_checks++;
    if ( !ok )
    {
        g_failed++;
        printf("FAIL: %s at %.0f fps (%.4f vs 1998 %.4f)\n", what, fps, a, b);
    }
}

struct Frame
{
    uint32_t measured;  // 1/1024 s, what the timer reports
    uint32_t ticks;     // game ticks the frame advances (frameTime)
};

// Frames of a steady `fps` for `seconds`: measured as the 1/1024 s timer does, ticks from the
// game clock (mode 1998 = the 0.8.4 clock: measured + 1).
static std::vector<Frame> Frames(double fps, double seconds, GameClock::Mode mode)
{
    std::vector<Frame> out;
    GameClock::State st;
    const uint64_t n = (uint64_t)(fps * seconds + 0.5);
    for ( uint64_t i = 0; i < n; i++ )
    {
        const uint64_t a = (uint64_t)(i * 1024.0 / fps), b = (uint64_t)((i + 1) * 1024.0 / fps);
        Frame f;
        f.measured = (uint32_t)(b - a);
        f.ticks = GameClock::FrameTicks(st, f.measured, mode);
        out.push_back(f);
    }
    return out;
}

// ---- 1998 slider code (winp.cpp GetSlider, per frame) ------------------------------------
struct Slider1998 { int pos = 0; };
static float Key1998(Slider1998 &s, bool down)
{
    if ( down ) s.pos += 37; else s.pos = 8 * s.pos / 10;
    if ( s.pos > 300 ) s.pos = 300; else if ( s.pos < -300 ) s.pos = -300;
    return s.pos / 300.0f;
}
static float Slew1998(Slider1998 &s, int target)
{
    if ( s.pos > target ) { s.pos -= 37; if ( s.pos < target ) s.pos = target; }
    else if ( s.pos < target ) { s.pos += 37; if ( s.pos > target ) s.pos = target; }
    return s.pos / 300.0f;
}

// Real time (s) at which the value first reaches >= hi (rising) or <= lo (falling).
struct Curve
{
    double t50 = -1, tFull = -1, tRel50 = -1, tRel10 = -1, tZero = -1;
};

// Hold a key for 0.5 s, then let go for 1 s.
static Curve KeyCurve(double fps, bool old)
{
    Curve c;
    std::vector<Frame> fr = Frames(fps, 1.5, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    Slider1998 o;
    Slider n;
    uint64_t meas = 0;
    for ( const Frame &f : fr )
    {
        const bool down = meas < 512;         // decided at the frame's start
        meas += f.measured;
        const double t = meas / 1024.0;
        const float v = old ? Key1998(o, down) : SliderKey(n, down, (float)f.measured);
        if ( down )
        {
            if ( c.t50 < 0 && v >= 0.5f ) c.t50 = t;
            if ( c.tFull < 0 && v >= 1.0f ) c.tFull = t;
        }
        else
        {
            const double tr = t - 0.5;
            if ( c.tRel50 < 0 && v <= 0.5f ) c.tRel50 = tr;
            if ( c.tRel10 < 0 && v <= 0.1f ) c.tRel10 = tr;
            if ( c.tZero < 0 && v <= 0.0f ) c.tZero = tr;
        }
    }
    return c;
}

// A stick pushed from centre to full (300) at t=0, back to centre at 0.5 s.
static Curve StickCurve(double fps, bool old)
{
    Curve c;
    std::vector<Frame> fr = Frames(fps, 1.0, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    Slider1998 o;
    Slider n;
    uint64_t meas = 0;
    for ( const Frame &f : fr )
    {
        const int target = meas < 512 ? 300 : 0;
        meas += f.measured;
        const double t = meas / 1024.0;
        const float v = old ? Slew1998(o, target) : SliderSlew(n, (float)target, (float)f.measured);
        if ( target )
        {
            if ( c.t50 < 0 && v >= 0.5f ) c.t50 = t;
            if ( c.tFull < 0 && v >= 1.0f ) c.tFull = t;
        }
        else
        {
            if ( c.tRel50 < 0 && v <= 0.5f ) c.tRel50 = t - 0.5;
            if ( c.tZero < 0 && v <= 0.0f ) c.tZero = t - 0.5;
        }
    }
    return c;
}

// Mouse: a 125 Hz mouse moving 2 px per report for 0.3 s, then still. Returns the slider
// at 0.3 s and the time after the last motion until it reaches <= 10 %; `dip` = the largest
// drop from one frame to the next while the mouse moves (decay between reports).
static void MouseRun(double fps, bool old, float &atStop, double &tRel10, float &dip)
{
    std::vector<Frame> fr = Frames(fps, 1.5, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    Slider1998 o;
    Slider n;
    uint64_t meas = 0;
    double nextReport = 0.008;
    atStop = 0; tRel10 = -1; dip = 0;
    float last = 0;
    for ( const Frame &f : fr )
    {
        const double t0 = meas / 1024.0;
        meas += f.measured;
        const double t1 = meas / 1024.0;
        int move = 0;
        while ( nextReport <= t1 && nextReport <= 0.3 + 1e-9 ) { move += 2; nextReport += 0.008; }
        float v;
        if ( old )
        {   // 1998: += move * 8, decay only on frames without motion
            o.pos += move * 8;
            if ( !move ) o.pos = 8 * o.pos / 10;
            if ( o.pos > 300 ) o.pos = 300;
            v = o.pos / 300.0f;
        }
        else
            v = SliderMouse(n, move, (float)f.measured);
        (void)t0;
        if ( t1 <= 0.3 && last - v > dip ) dip = last - v;
        if ( t1 <= 0.3 ) atStop = v;
        if ( t1 > 0.3 && tRel10 < 0 && v <= 0.1f * atStop ) tRel10 = t1 - 0.3;
        last = v;
    }
    (void)last;
}

// A speed damped by `factor` once per (60 fps) frame from v0 until < 0.1; position advances
// speed * frame seconds (game ticks / 1000) every frame. Distance and real time to stop.
static void DampRun(double fps, bool old, float factor, double &dist, double &tStop)
{
    std::vector<Frame> fr = Frames(fps, 5.0, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    double v = 100.0;
    dist = 0; tStop = -1;
    uint64_t meas = 0;
    for ( const Frame &f : fr )
    {
        meas += f.measured;
        v *= old ? factor : Damp(factor, (int)f.ticks, false);
        dist += v * (f.ticks / 1000.0);
        if ( v < 0.1 ) { tStop = meas / 1024.0; break; }
    }
}

// AI scan: cells per real second for `perFrame` cells per frame.
static double ScanRate(double fps, bool old, int perFrame)
{
    std::vector<Frame> fr = Frames(fps, 10.0, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    float acc = 0;
    uint64_t cells = 0, meas = 0;
    for ( const Frame &f : fr )
    {
        cells += old ? perFrame : Steps(acc, perFrame, (int)f.ticks, false);
        meas += f.measured;
    }
    return cells / (meas / 1024.0);
}

// Ground contact on every frame for 1 s: energy lost per second (1 per touch in 1998).
static double TouchRate(double fps, bool old)
{
    std::vector<Frame> fr = Frames(fps, 1.0, old ? GameClock::MODE_1998 : GameClock::MODE_FIXED_RATE);
    int32_t clock = 0, last = -100000;
    double lost = 0;
    for ( const Frame &f : fr )
    {
        clock += f.ticks;
        lost += old ? 1.0 : RepeatShare(clock, last, false);
    }
    return lost;
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    const double rates[] = {20, 30, 60, 144, 240, 1000};
    const double frame60 = 1.0 / 60.0;

    // the 0.8.4 baseline: 1998 code, 60 fps
    const Curve k0 = KeyCurve(60, true), s0 = StickCurve(60, true);
    double ufo0, ufoT0, host0, hostT0, park0, parkT0;
    DampRun(60, true, 0.7f, ufo0, ufoT0);
    DampRun(60, true, 0.8f, host0, hostT0);
    DampRun(60, true, 0.35f, park0, parkT0);
    float mStop0, mDip0; double mRel0;
    MouseRun(60, true, mStop0, mRel0, mDip0);
    const double scan0 = ScanRate(60, true, 7), touch0 = TouchRate(60, true);

    if ( g_verbose )
    {
        printf("fps   | key 50%%  full  rel50%% rel10%% zero | stick full zero | mouse@stop dip rel10%% | ufo brake dist/t | host drift dist/t | host 0.35 dist | scan cells/s | touch/s\n");
        printf("0.8.4 60 | %.3f %.3f %.3f %.3f %.3f | %.3f %.3f | %.2f %.2f %.3f | %.1f %.3f | %.1f %.3f | %.2f | %.0f | %.1f\n",
               k0.t50, k0.tFull, k0.tRel50, k0.tRel10, k0.tZero, s0.tFull, s0.tZero, mStop0, mDip0, mRel0,
               ufo0, ufoT0, host0, hostT0, park0, scan0, touch0);
    }

    for ( double fps : rates )
    {
        const Curve k = KeyCurve(fps, false), s = StickCurve(fps, false);
        double ufo, ufoT, host, hostT, park, parkT;
        DampRun(fps, false, 0.7f, ufo, ufoT);
        DampRun(fps, false, 0.8f, host, hostT);
        DampRun(fps, false, 0.35f, park, parkT);
        float mStop, mDip; double mRel;
        MouseRun(fps, false, mStop, mRel, mDip);
        const double scan = ScanRate(fps, false, 7), touch = TouchRate(fps, false);
        if ( g_verbose )
            printf("%5.0f  | %.3f %.3f %.3f %.3f %.3f | %.3f %.3f | %.2f %.2f %.3f | %.1f %.3f | %.1f %.3f | %.2f | %.0f | %.1f\n",
                   fps, k.t50, k.tFull, k.tRel50, k.tRel10, k.tZero, s.tFull, s.tZero, mStop, mDip, mRel,
                   ufo, ufoT, host, hostT, park, scan, touch);

        // timing: within one frame at this rate or at 60 fps, whichever is longer (+ the
        // timer's 1/1024 s steps); low rates can only hit a threshold on a frame boundary
        const double tol = std::max(1.0 / fps, frame60) + 2.0 / 1024.0;
        Check(fabs(k.t50 - k0.t50) <= tol, "key ramp to 50 %", fps, k.t50, k0.t50);
        Check(fabs(k.tFull - k0.tFull) <= tol, "key ramp to full", fps, k.tFull, k0.tFull);
        Check(fabs(k.tRel50 - k0.tRel50) <= tol, "key release to 50 %", fps, k.tRel50, k0.tRel50);
        Check(fabs(k.tRel10 - k0.tRel10) <= tol, "key release to 10 %", fps, k.tRel10, k0.tRel10);
        Check(fabs(k.tZero - k0.tZero) <= tol + frame60, "key release to 0", fps, k.tZero, k0.tZero);
        Check(fabs(s.tFull - s0.tFull) <= tol, "stick slew to full", fps, s.tFull, s0.tFull);
        Check(fabs(s.tZero - s0.tZero) <= tol, "stick slew back to centre", fps, s.tZero, s0.tZero);
        Check(fabs(mStop - mStop0) <= 0.02, "mouse: slider at the end of a steady motion", fps, mStop, mStop0);
        Check(mDip <= mDip0 + 0.005, "mouse: no extra decay between 125 Hz reports", fps, mDip, mDip0);
        Check(fabs(mRel - mRel0) <= tol + frame60, "mouse release to 10 %", fps, mRel, mRel0);
        // distances: the 1998 run integrates in 18-tick steps, so allow 6 % (low rates: coarse)
        const double dtol = fps >= 60 ? 0.06 : 0.25;
        Check(fabs(ufo - ufo0) <= dtol * ufo0, "UFO brake distance", fps, ufo, ufo0);
        Check(fabs(ufoT - ufoT0) <= 0.2 * ufoT0 + tol, "UFO brake time", fps, ufoT, ufoT0);
        Check(fabs(host - host0) <= dtol * host0, "host station drift distance", fps, host, host0);
        Check(fabs(hostT - hostT0) <= 0.2 * hostT0 + tol, "host station drift time", fps, hostT, hostT0);
        Check(fabs(park - park0) <= (fps >= 60 ? 0.10 : 0.6) * park0, "host station stop (0.35) distance", fps, park, park0);
        Check(fabs(scan - scan0) <= 0.01 * scan0, "AI scan cells per second", fps, scan, scan0);
        Check(fps < 60 || fabs(touch - touch0) <= 0.03 * touch0, "repeated contact damage per second", fps, touch, touch0);
    }

    // single touch after a pause counts in full; same-frame repeat counts nothing
    {
        int32_t last = -100000;
        Check(RepeatShare(1000, last, false) == 1.0f, "single touch", 0, RepeatShare(5000, last, false), 1);
        Check(RepeatShare(5000, last, false) == 0.0f, "repeat in the same tick", 0, 0, 0);
    }
    // the Quest (per-frame) path is the 1998 code
    {
        Check(Damp(0.7f, 5, true) == 0.7f, "per-frame damp", 0, Damp(0.7f, 5, true), 0.7);
        float acc = 0;
        Check(Steps(acc, 7, 3, true) == 7, "per-frame steps", 0, 0, 7);
        Check(FrameScale(0.005f, true) == 1.0f, "per-frame scale", 0, 0, 1);
    }

    printf("frame_rate_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
