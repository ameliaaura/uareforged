// TEST ONLY: frame-rate invariance probes (test_probes.h). Empty unless UA_TEST_HOOKS.
#include "test_probes.h"

#ifdef UA_TEST_HOOKS

#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include "fmtlib/printf.h"

namespace TestProbe
{

static uint64_t g_frame = 0;      // frames since start
static uint64_t g_measured = 0;   // summed measured frame time (1/1024 s)

static bool Env(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] == '1';
}

void Frame(uint32_t measured)
{
    g_frame++;
    g_measured += measured;
}

// --- turn / bank ------------------------------------------------------------------------
static struct
{
    double bank = 0, rate = 0, target = 0;
    int n = 0;
    uint64_t sec = 0;
} g_turn;

bool TurnForced()
{
    static int on = -1;
    if ( on < 0 )
        on = Env("UA_TEST_TURN") ? 1 : 0;
    return on == 1;
}

void Turn(float yawPerFrame, float frameSeconds, float bankTarget, float bank)
{
    if ( !TurnForced() || frameSeconds <= 0.0f )
        return;
    const uint64_t sec = g_measured / 1024;
    if ( sec != g_turn.sec )
    {
        if ( g_turn.n > 0 )
            fmt::printf("turnprobe: s=%d n=%d rate=%.4f bank=%.4f target=%.4f\n", (int)g_turn.sec, g_turn.n,
                        g_turn.rate / g_turn.n, g_turn.bank / g_turn.n, g_turn.target / g_turn.n);
        g_turn = decltype(g_turn)();
        g_turn.sec = sec;
    }
    g_turn.rate += fabs(yawPerFrame / frameSeconds);
    g_turn.bank += fabs(bank);
    g_turn.target += fabs(bankTarget);
    g_turn.n++;
}

// --- damping ----------------------------------------------------------------------------
struct DampState
{
    bool active = false;
    uint64_t frame = 0, meas0 = 0, frames0 = 0;
    float v0 = 0;
    vec3d pos0;
};
static std::unordered_map<const void *, DampState> g_damp;

void Damp(const char *, const void *unit, float speed, const vec3d &pos)
{
    static int on = -1;
    if ( on < 0 )
        on = Env("UA_TEST_DAMP") ? 1 : 0;
    if ( !on )
        return;
    DampState &s = g_damp[unit];
    if ( !s.active || s.frame + 1 != g_frame )
    {   // a new braking run (or one interrupted by thrust: start over)
        s.active = true;
        s.v0 = fabs(speed);
        s.pos0 = pos;
        s.meas0 = g_measured;
        s.frames0 = g_frame;
    }
    s.frame = g_frame;
}

void DampStop(const char *tag, const void *unit, const vec3d &pos)
{
    auto it = g_damp.find(unit);
    if ( it == g_damp.end() || !it->second.active )
        return;
    DampState &s = it->second;
    s.active = false;
    if ( s.frame + 1 < g_frame || s.v0 < 1.0f )
        return; // not braking right up to now / too slow to tell
    const double dist = (pos - s.pos0).length();
    fmt::printf("dampprobe: %s v0=%.2f dist=%.2f ratio=%.4f real=%.3f frames=%d\n", tag, s.v0, dist, dist / s.v0,
                (g_measured - s.meas0) / 1024.0, (int)(g_frame - s.frames0));
}

}

#endif
