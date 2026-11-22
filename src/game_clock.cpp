#include "game_clock.h"

#include <cstdlib>
#include <cstring>

namespace GameClock
{

uint32_t FrameTicks(State &st, uint32_t measured, Mode mode)
{
    if ( mode == MODE_1998 )
        return measured + 1;

    st.carry += (int64_t)measured * kTicksPerSecond;
    int64_t whole = st.carry >= 0 ? st.carry / kMeasuredPerSecond : 0;
    st.carry -= whole * kMeasuredPerSecond;

    if ( whole < 1 )
    {
        // Lend one tick; the following frames pay it back. Above ~1000 fps the
        // debt stops growing at kMaxDebt (the game then runs slightly fast, as
        // every frame still needs one tick).
        whole = 1;
        st.carry -= kMeasuredPerSecond;
        if ( st.carry < -kMaxDebt )
            st.carry = -kMaxDebt;
    }

    return (uint32_t)whole;
}

Mode CurrentMode()
{
    static int mode = -1;
    if ( mode < 0 )
    {
        const char *v = getenv("UA_GAME_CLOCK");
        mode = (v && !strcmp(v, "1998")) ? MODE_1998 : MODE_FIXED_RATE;
    }
    return (Mode)mode;
}

static State g_state;

uint32_t FrameTicks(uint32_t measured)
{
    return FrameTicks(g_state, measured, CurrentMode());
}

void Reset()
{
    g_state = State();
}

}
