// pure decision logic for the right grip / B and
// the L-grip brake-then-reverse pause. No engine dependencies, so
// tools/vr/test_vr_grip.cpp can unit-test it; winp.cpp CheckVRControllers uses it.
#ifndef UA_VR_GRIP_H_INCLUDED
#define UA_VR_GRIP_H_INCLUDED

#include <cstdint>

namespace VR
{
namespace Grip
{

// The right grip belongs to the vehicle (drive / climb / brake) rather than to the
// laser pointer: in a vehicle the UI pointer is in "window rects" mode (2), and a
// real resting hand points forward-down, right at the tool bar / open map, so the
// pointer is on most of the time (otherwise the R grip would seem dead). There, a
// right click while pointing comes from B only. Host station / modal boxes
// (mode 1), menus (theatre) and grips set to OFF keep the rule
// (grip = right click while pointing).
inline bool VehicleOwnsGrip(int uiPointerMode, bool vehicleUsesGrip)
{
    return uiPointerMode == 2 && vehicleUsesGrip;
}

// Latches: a grip / B press made while pointing stays a mouse button until it is
// released (closing the map with a click must not brake / drive / call the station).
struct Latch
{
    bool grip = false; // grip press that started as a pointer press
    bool b = false;    // B press that started as a pointer press
};

// a squeeze that began while the vehicle owned the grip stays a
// drive squeeze until it is released, even if the pointer mode changes under it
// (Menu opens the pause panel = modal mode 1): it never becomes a right click or a
// pointer latch, so Continue with the grip still held keeps driving.
inline bool UpdateGripDrive(bool prev, bool vehGrip, float grip)
{
    if (grip < 0.3f)
        return false;
    return prev || (vehGrip && grip > 0.5f);
}

inline void UpdateLatch(Latch &l, bool vehGrip, bool pointing, float grip, bool bDown, bool gripDrive = false)
{
    if (pointing && grip > 0.5f && !vehGrip && !gripDrive) l.grip = true;
    if (pointing && bDown) l.b = true;
    if (grip < 0.3f) l.grip = false;
    if (!bDown) l.b = false;
}

// pointing = the pointer is on, or a pointer right button is held.
// Right grip free for the vehicle / station control.
inline bool RGripFree(const Latch &l, bool vehGrip, bool ptr, bool ptrRHeld)
{
    if (vehGrip)
        return !l.grip; // only a squeeze that began as a click (before entering) stays blocked
    // rule: B held while pointing also blocks the grip (one shared latch)
    return !ptr && !l.grip && !l.b && !ptrRHeld;
}

// B free for its own action (hold = host station).
inline bool BFree(const Latch &l, bool vehGrip, bool ptr, bool ptrRHeld)
{
    if (vehGrip)
        return !ptr && !l.b && !ptrRHeld;
    return RGripFree(l, vehGrip, ptr, ptrRHeld);
}

// Physical "right mouse button" input for the pointer.
inline bool PhysRight(bool vehGrip, bool ptrRHeld, float grip, bool bDown, bool gripDrive = false)
{
    if (vehGrip)
        return bDown;
    return (!gripDrive && (ptrRHeld ? grip > 0.35f : grip > 0.6f)) || bDown;
}

// "Used": the press started as a vehicle / weapon press, so it never becomes a click.
// A drive-owned squeeze (gripDrive) never counts here, so it cannot block a fresh B click
// in the pause panel.
inline bool RightUsed(bool prevUsed, bool vehGrip, bool ptr, bool ptrRHeld, float grip, bool bDown, bool gripDrive = false)
{
    const bool down = vehGrip ? bDown : ((grip > 0.3f && !gripDrive) || bDown);
    return down && !ptrRHeld && (prevUsed || !ptr);
}

// PANELS FREE (vr_panels.cpp): one latch for the right trigger. A squeeze that starts
// with the laser on a panel handle (outer edge / corner) and no other layer holding the
// hand belongs to the panel grab until it is released; one that starts anywhere else
// (weapon, laser click, war table, board) never becomes a grab, even if the ray then
// crosses a handle.
struct PanelLatch
{
    int state = 0; // 0 idle, 1 panel grab, 2 someone else's squeeze
};
// Decided at the first touch above kPanelTouch, below every other layer's threshold (the
// laser click / weapon latch in winp.cpp start at 0.25, board clicks at 0.6), so no other
// layer sees the start of a grab squeeze, and a squeeze that is already someone else's
// never becomes a grab.
const float kPanelTouch = 0.15f, kPanelRelease = 0.10f;
inline bool UpdatePanelLatch(PanelLatch &l, float trigger, bool onHandle, bool otherOwns)
{
    if (trigger < kPanelRelease)
        l.state = 0;
    else if (l.state == 0 && trigger > kPanelTouch)
        l.state = (onHandle && !otherOwns) ? 1 : 2;
    return l.state == 1;
}

// L grip: brake while rolling forward, then (after a stop that the brake made)
// pause kReversePauseMs before reversing, so a hard stop does not roll straight
// into reverse. A squeeze from standstill reverses at once.
static const uint32_t kReversePauseMs = 300;
enum LAction { L_NONE = 0, L_BRAKE, L_REVERSE };
struct LState
{
    bool braked = false;   // this squeeze braked
    bool stopped = false;  // stop speed reached after braking
    uint32_t stopAt = 0;   // first poll at / below stop speed after braking
};

inline LAction LeftGrip(LState &s, bool held, double fwdSpeed, uint32_t now)
{
    if (!held)
    {
        s = LState();
        return L_NONE;
    }
    if (fwdSpeed > 1.0)
    {
        s.braked = true;
        s.stopped = false;
        return L_BRAKE;
    }
    if (!s.braked)
        return L_REVERSE;
    if (!s.stopped)
    {
        s.stopped = true;
        s.stopAt = now;
    }
    return (now - s.stopAt >= kReversePauseMs) ? L_REVERSE : L_BRAKE;
}

} // namespace Grip
} // namespace VR

#endif // UA_VR_GRIP_H_INCLUDED
