#ifndef FRAME_RATE_H_INCLUDED
#define FRAME_RATE_H_INCLUDED

#include <cmath>
#include <cstdint>

// Frame-rate independent versions of the 1998 code that counted frames instead of time.
//
// Since 0.8.5 the desktop game clock runs at a fixed 1084 game ticks per real second at any
// frame rate (game_clock.h) and the frame cap follows the display (120/144/240 Hz). Code that
// did something "once per frame" (multiply a speed by 0.8, add 37 to a steering slider, scan
// 7 map cells) did it 2.4x as often per second at 144 fps as at the 60 fps the game was tuned
// for. The helpers below scale that per-frame step by the frame's length, so a second of play
// gives the same result at 20, 30, 60, 144 or 240 fps as at 60 fps.
//
// The Quest keeps its own display-clock timing (main.cpp), so there every helper is the
// 1998 per-frame step, unchanged.
//
// Pure logic, no SDL: unit-tested in tests/frame_rate_test.cpp.
namespace FrameRate
{

// Game ticks of one frame at the 60 fps cap (1084 ticks per real second / 60).
static const float kRefTicks = 1084.0f / 60.0f;
// Measured time (1/1024 s, the engine timer's unit) of one frame at 60 fps.
static const float kRefMeasured = 1024.0f / 60.0f;

#ifdef __ANDROID__
static const bool kPerFrame = true;
#else
static const bool kPerFrame = false;
#endif

// How many 60 fps frames a frame of `seconds` (game time, ticks / 1000) is worth: for per-frame
// thresholds (1 on the Quest).
inline float FrameScale(float seconds, bool perFrame = kPerFrame)
{
    if ( perFrame )
        return 1.0f;
    return seconds * 1000.0f / kRefTicks;
}

// The factor `f` that the 1998 code applied to a speed once per frame (brakes, drift), for a
// frame of `ticks` game ticks. The unit then moves speed x frame time, so at the 60 fps cap a
// braking unit covers v0 * T * f / (1 - f) (T = one 60 fps frame) before it stops. f^(ticks/T)
// would keep the speed-over-time curve but not that distance (more, smaller steps cover up to
// 20 % (f 0.7) / 75 % (f 0.35) more ground at high frame rates); 1 / (1 + (ticks/T)(1-f)/f)
// gives exactly f at 60 fps and the SAME braking distance at any frame rate (the speed then
// falls a little faster at high rates: the stop comes up to ~15 % sooner over the same ground).
inline float Damp(float f, int ticks, bool perFrame = kPerFrame)
{
    if ( perFrame )
        return f;
    if ( ticks <= 0 || f <= 0.0f )
        return ticks <= 0 ? 1.0f : f;
    return 1.0f / (1.0f + ((float)ticks / kRefTicks) * (1.0f - f) / f);
}

// Work that the 1998 code did `perFrame` items per frame (e.g. map cells an AI host station
// scans): the items for a frame of `ticks` game ticks, the fraction carried in `acc`.
inline int Steps(float &acc, int perFrame, int ticks, bool perFrame1998 = kPerFrame)
{
    if ( perFrame1998 )
        return perFrame;
    if ( ticks > 0 )
        acc += (float)perFrame * ((float)ticks / kRefTicks);
    int n = (int)acc;
    if ( n < 0 )
        n = 0;
    acc -= (float)n;
    return n;
}

// An amount the 1998 code took off once per frame while a condition held (a unit scraping the
// ground loses energy every frame it touches): the share for this frame. A single touch after
// a gap of at least one 60 fps frame counts in full, as before; touching on every frame costs
// the same per second as touching every frame at 60 fps. `now` and `last` are game ticks.
inline float RepeatShare(int32_t now, int32_t &last, bool perFrame = kPerFrame)
{
    if ( perFrame )
        return 1.0f;
    const int32_t gap = now - last;
    last = now;
    if ( gap < 0 || (float)gap >= kRefTicks )
        return 1.0f;
    return (float)gap / kRefTicks;
}

// Steering sliders (winp GetSlider): a key, a stick axis, a hat or the mouse moves a value in
// -300..300. 1998, once per frame: a held key adds 37; a stick/hat moves 37 toward its
// position; the mouse adds 8 per pixel moved; with no key held / no mouse motion the value
// decays to 8/10 (integer). Here the same per 60 fps frame, scaled by the frame's measured time
// (1/1024 s), so the ramp-up and release take the same real time at any frame rate.
//
// The mouse: at high frame rates many frames see no motion event although the mouse is moving
// (a 125 Hz mouse reports every 8 ms, a 240 fps frame lasts 4 ms). The decay starts only once
// the mouse has been still for kMouseStill (at 60 fps: the first frame without motion, as
// before), and then covers all the still time.
struct Slider
{
    float pos = 0.0f;     // -300..300
    float still = 0.0f;   // mouse: measured time since the last motion
};

static const float kSliderMax = 300.0f;
static const float kSliderStep = 37.0f;
static const float kSliderDecay = 0.8f;
static const float kMouseStill = 0.7f * kRefMeasured;
static const float kMaxFrameMeasured = 1024.0f; // a frame counts as at most 1 s

inline float ClampSlider(float v)
{
    if ( v > kSliderMax )
        return kSliderMax;
    if ( v < -kSliderMax )
        return -kSliderMax;
    return v;
}

inline void SliderDecay(Slider &s, float measured)
{
    // 8/10 per 60 fps frame; the 1998 integer division also dropped the fraction (on average
    // ~0.45 a frame), which is what brought small values to 0: kept, scaled the same way
    const float r = measured / kRefMeasured;
    const float mag = fabsf(s.pos) * powf(kSliderDecay, r) - 0.45f * r;
    s.pos = mag < 1.0f ? 0.0f : (s.pos < 0.0f ? -mag : mag); // 1998: 8 * 1 / 10 = 0
}

inline float FrameMeasured(float measured)
{
    if ( measured < 0.0f )
        return 0.0f;
    return measured > kMaxFrameMeasured ? kMaxFrameMeasured : measured;
}

// A key bound to the slider, held or not.
inline float SliderKey(Slider &s, bool down, float measured)
{
    measured = FrameMeasured(measured);
    if ( down )
        s.pos += kSliderStep * (measured / kRefMeasured);
    else
        SliderDecay(s, measured);
    s.pos = ClampSlider(s.pos);
    return s.pos / kSliderMax;
}

// A stick axis / hat / throttle, at `target` (-300..300): slews toward it, never decays.
inline float SliderSlew(Slider &s, float target, float measured)
{
    measured = FrameMeasured(measured);
    const float step = kSliderStep * (measured / kRefMeasured);
    if ( s.pos > target )
    {
        s.pos -= step;
        if ( s.pos < target )
            s.pos = target;
    }
    else if ( s.pos < target )
    {
        s.pos += step;
        if ( s.pos > target )
            s.pos = target;
    }
    s.pos = ClampSlider(s.pos);
    return s.pos / kSliderMax;
}

// The mouse moved `move` pixels on this axis this frame (0 = no motion event).
inline float SliderMouse(Slider &s, int move, float measured)
{
    measured = FrameMeasured(measured);
    if ( move )
    {
        s.pos += (float)move * 8.0f;
        s.still = 0.0f;
    }
    else
    {
        const float before = s.still;
        s.still += measured;
        if ( s.still > 1.0e6f )
            s.still = 1.0e6f;
        if ( s.still >= kMouseStill )
            SliderDecay(s, before >= kMouseStill ? measured : s.still);
    }
    s.pos = ClampSlider(s.pos);
    return s.pos / kSliderMax;
}

}

#endif
