// controls settings: ground grip mode and the
// world crosshair colour. Plain values: the game persists them in the profile
// (yw_save.cpp / world/parsers.cpp) and shows them on the Settings page.
// The map board and station turn settings live with the command layer
// (vr_command.cpp), which owns those features.

#include "vr.h"

namespace VR
{
namespace
{
int g_gripMode = GRIP_DRIVE;
int g_xhair = XHAIR_YELLOW; // default: easiest to see
} // namespace

int GroundGripMode() { return g_gripMode; }

void SetGroundGripMode(int mode)
{
    g_gripMode = (mode >= 0 && mode < GRIP_COUNT) ? mode : GRIP_DRIVE;
}

const char *GroundGripLabel(int mode)
{
    switch (mode)
    {
    case GRIP_BRAKE: return "GRIPS: R BRAKE (OLD)";
    case GRIP_OFF: return "GRIPS: OFF";
    default: return "GRIPS: DRIVE / BRAKE-REV";
    }
}

int CrosshairColour() { return g_xhair; }

void SetCrosshairColour(int c)
{
    g_xhair = (c >= 0 && c < XHAIR_COUNT) ? c : XHAIR_YELLOW;
}

const char *CrosshairLabel(int c)
{
    static const char *n[XHAIR_COUNT] = {"CROSSHAIR: GREEN", "CROSSHAIR: WHITE", "CROSSHAIR: RED", "CROSSHAIR: YELLOW", "CROSSHAIR: CYAN"};
    return n[(c >= 0 && c < XHAIR_COUNT) ? c : 0];
}

void CrosshairRGB(int c, float rgb[3])
{
    static const float k[XHAIR_COUNT][3] = {
        {0.45f, 1.0f, 0.55f}, // green (the colour)
        {1.0f, 1.0f, 1.0f},   // white
        {1.0f, 0.25f, 0.2f},  // red
        {1.0f, 0.92f, 0.25f}, // yellow
        {0.3f, 0.95f, 1.0f},  // cyan
    };
    const float *v = k[(c >= 0 && c < XHAIR_COUNT) ? c : 0];
    rgb[0] = v[0];
    rgb[1] = v[1];
    rgb[2] = v[2];
}

} // namespace VR
