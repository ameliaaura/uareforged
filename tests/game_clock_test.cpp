// Game clock: the same game speed at every frame rate (src/game_clock.h). No SDL, no game data.
//   c++ -std=c++14 -Wall -Isrc tests/game_clock_test.cpp src/game_clock.cpp -o game_clock_test
#include "game_clock.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace GameClock;

static int g_checks = 0, g_failed = 0;

static void Check(bool ok, const char *what, double a = 0.0, double b = 0.0)
{
    g_checks++;
    if ( !ok )
    {
        g_failed++;
        printf("FAIL: %s (%.4f vs %.4f)\n", what, a, b);
    }
}

// What the engine's timer (NC_STACK_wintimer) measures for frames at `fps`: the 1/1024 s
// counter read once per frame, so the parts add up exactly to the elapsed time.
static std::vector<uint32_t> Frames(double fps, double seconds)
{
    std::vector<uint32_t> out;
    const uint64_t n = (uint64_t)(fps * seconds + 0.5);
    for ( uint64_t i = 0; i < n; i++ )
    {
        const uint64_t a = (uint64_t)(i * 1024.0 / fps), b = (uint64_t)((i + 1) * 1024.0 / fps);
        out.push_back((uint32_t)(b - a));
    }
    return out;
}

static double TicksPerSecond(double fps, Mode mode, double seconds = 120.0)
{
    State st;
    uint64_t ticks = 0, measured = 0;
    for ( uint32_t m : Frames(fps, seconds) )
    {
        const uint32_t t = FrameTicks(st, m, mode);
        Check(t >= 1, "every frame advances at least one tick");
        ticks += t;
        measured += m;
    }
    return ticks / (measured / 1024.0);
}

int main()
{
    const double kWant = kTicksPerSecond; // 1084

    // 1. Fixed rate: the same game speed (ticks per real second) at every frame rate.
    const double rates[] = {10, 20, 24, 30, 50, 59.94, 60, 72, 75, 90, 100, 120, 144, 165, 240, 360, 500, 1000};
    for ( double fps : rates )
    {
        const double tps = TicksPerSecond(fps, MODE_FIXED_RATE);
        char what[96];
        snprintf(what, sizeof what, "fixed rate: %.2f fps runs at 1084 ticks/s (within 1 tick per 120 s)", fps);
        Check(std::fabs(tps - kWant) * 120.0 <= 1.0, what, tps, kWant);
    }

    // 2. At 60 fps the new clock equals the old one (the 1998 feel at the cap is unchanged).
    Check(std::fabs(TicksPerSecond(60, MODE_FIXED_RATE) - TicksPerSecond(60, MODE_1998)) < 0.01, "60 fps: fixed rate = 1998 clock",
          TicksPerSecond(60, MODE_FIXED_RATE), TicksPerSecond(60, MODE_1998));

    // 3. The old clock really did run faster with the frame rate (what this fixes).
    Check(std::fabs(TicksPerSecond(144, MODE_1998) - (1024 + 144)) < 0.5, "1998 clock: 144 fps = 1168 ticks/s", TicksPerSecond(144, MODE_1998), 1168);
    Check(std::fabs(TicksPerSecond(240, MODE_1998) - (1024 + 240)) < 0.5, "1998 clock: 240 fps = 1264 ticks/s", TicksPerSecond(240, MODE_1998), 1264);

    // 4. Per frame the fixed clock never strays more than one tick from the exact share.
    {
        State st;
        double exact = 0.0, given = 0.0, worst = 0.0;
        for ( uint32_t m : Frames(144, 60) )
        {
            exact += m * kWant / 1024.0;
            given += FrameTicks(st, m, MODE_FIXED_RATE);
            worst = std::fmax(worst, std::fabs(exact - given));
        }
        Check(worst < 1.0, "144 fps: running total within one tick of exact", worst, 1.0);
    }

    // 5. Above 1084 fps a frame (0 measured) still gets one tick; the lent ticks are paid back.
    {
        State st;
        uint64_t ticks = 0;
        for ( int i = 0; i < 4; i++ )
            ticks += FrameTicks(st, 0, MODE_FIXED_RATE);              // 4 frames inside one 1/1024 s
        Check(ticks == 4, "zero-length frames still advance one tick each", (double)ticks, 4);
        for ( int i = 0; i < 1024; i++ )
            ticks += FrameTicks(st, 1, MODE_FIXED_RATE);              // then 1 s at 1024 fps
        Check(ticks >= 1084 && ticks <= 1086, "lent ticks are paid back within the next second", (double)ticks, 1084);
    }

    // 6. Above ~1084 fps every frame still needs a tick, so the game would run fast there; the main
    //    loop keeps frames at >= 1 ms even uncapped (main.cpp). The lent-tick debt stays bounded.
    {
        State st;
        for ( int i = 0; i < 20480; i++ )
            FrameTicks(st, (uint32_t)(i & 1), MODE_FIXED_RATE);
        Check(st.carry >= -kMaxDebt, "debt bounded", (double)st.carry, (double)-kMaxDebt);
        uint64_t ticks = 0;
        for ( int i = 0; i < 2048; i++ )
            ticks += FrameTicks(st, 1, MODE_FIXED_RATE);              // back to 1024 fps for 2 s
        Check(ticks >= 2048 && ticks <= 2168, "after a debt, 1024 fps pays back at most 8 s of lent ticks", (double)ticks, 2048);
    }

    // 7. Low frame rate (stalls): ticks follow real time, no speed-up.
    Check(std::fabs(TicksPerSecond(15, MODE_FIXED_RATE) - kWant) * 120.0 <= 1.0, "15 fps: 1084 ticks/s", TicksPerSecond(15, MODE_FIXED_RATE), kWant);
    {
        State st;
        // (the value is taken first: argument order is unspecified, and gcc evaluates the last argument first)
        uint32_t t = FrameTicks(st, 2000, MODE_FIXED_RATE);
        Check(t == 2117, "a 2000/1024 s frame = 2117 ticks (1084 per s)", t, 2117);
    }

    printf("game_clock_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
