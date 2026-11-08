#ifndef INPUT_AIM_H_INCLUDED
#define INPUT_AIM_H_INCLUDED

// Joystick / pad axes -> the game's vertical aim sliders (NC_STACK_ypaworld::ypaworld_func64__sub1).
// Engine conventions: stick Y (slider 13) is negative when pushed forward / up (SDL),
// the hat / right stick Y (slider 16) is positive when pushed up. Slider 5 (tank gun)
// and slider 1 (aircraft nose, station gun, host station view) are positive for UP.
// invert = false: push up -> aim / nose goes up. invert = true: the 1998 joystick
// mapping (stick up = gun / nose down, flight-sim style).
// Header only, no game headers, so tests/controller_settings_test.cpp can check it.
namespace InputAim
{
// tank / car, stick layout AIM: stick Y -> gun pitch
inline float StickToGun(float stickY, bool invert)
{
    return (invert ? stickY : -stickY) * 2.0f;
}

// tank / car, either layout: hat or right stick Y -> gun pitch
inline float HatToGun(float hatY, bool invert)
{
    return invert ? -hatY : hatY;
}

// aircraft, station guns, host station view: stick Y -> slider 1
inline float StickToPitch(float stickY, bool invert)
{
    return invert ? stickY : -stickY;
}
}

#endif
