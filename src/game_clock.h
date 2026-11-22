#ifndef GAME_CLOCK_H_INCLUDED
#define GAME_CLOCK_H_INCLUDED

#include <cstdint>

// Game clock: how many game ticks a rendered frame advances the world.
//
// The engine measures each frame in 1/1024 s (NC_STACK_wintimer) and every
// game system (units, weapons, AI timers, energy, the mission clock, the GUI)
// advances by that number of "ms". Until 0.8.4 the main loop also added ONE
// tick per frame (Period++), so the game ran faster the higher the frame rate:
// 1024 + fps ticks per real second (60 fps: 1084, 144 fps: 1168, 240 fps:
// 1264, 1000 fps: ~2000).
//
// MODE_FIXED_RATE (desktop default since 0.8.5): exactly kTicksPerSecond game
// ticks per real second at any frame rate, = the old speed at 60 fps, so the
// game plays exactly as it always did at the 60 fps cap. The fraction of a
// tick is carried to the next frame, so nothing is lost or gained over time.
// A frame always advances at least one tick (several engine paths divide by
// the frame time); a tick lent that way is paid back by the next frames
// (bounded, so a run above ~1000 fps does not build an endless debt).
//
// MODE_1998: the old behaviour (measured + 1), for comparison and as a switch
// (UA_GAME_CLOCK=1998).
//
// Pure logic, no SDL: unit-tested in tests/game_clock_test.cpp.
namespace GameClock
{
enum Mode { MODE_FIXED_RATE = 0, MODE_1998 = 1 };

static const uint32_t kMeasuredPerSecond = 1024;      // NC_STACK_wintimer units
static const uint32_t kTicksPerSecond = 1024 + 60;    // = the old speed at 60 fps
static const int64_t kMaxDebt = 8 * (int64_t)kMeasuredPerSecond;
// The desktop main loop never runs faster than this, even uncapped (main.cpp): above
// ~1084 fps a frame would be shorter than one game tick.
static const uint32_t kMaxFps = 1000;

struct State
{
    int64_t carry = 0;   // in 1/kMeasuredPerSecond of a game tick; < 0 = ticks lent
};

// Game ticks for one frame that took `measured` 1/1024 s.
uint32_t FrameTicks(State &st, uint32_t measured, Mode mode);

// Process-wide clock used by the main loop.
Mode CurrentMode();                 // from UA_GAME_CLOCK (unset = fixed rate)
uint32_t FrameTicks(uint32_t measured);
void Reset();                       // a new mission / shell start: drop the carry
}

#endif
