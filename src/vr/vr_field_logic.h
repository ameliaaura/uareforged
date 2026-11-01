// field orders: pure decision logic for field orders, the
// clickable wrist squads and the squad wheel. No engine dependencies, so
// tools/vr/test_vr_field.cpp can unit-test it; vr_field.cpp uses it.
#ifndef UA_VR_FIELD_LOGIC_H_INCLUDED
#define UA_VR_FIELD_LOGIC_H_INCLUDED

#include <cstdint>
#include <cmath>

namespace VR
{
namespace FieldLogic
{

// ---- X in a vehicle with FIELD ORDERS on --------------------------------------
// X held = order mode (aim marker, the R trigger is an order, never a shot).
// X released without an order: a short press (< kTapMs) is the old X tap (map board,
// or the original 2D map when MAP BOARD is off); a longer press is a cancelled aim.
// The press is owned from its down edge to its up edge, decided at the down edge, so
// a mode / setting change during the hold never splits it.
const uint32_t kTapMs = 600;

enum XEvent { XE_NONE = 0, XE_TAP, XE_CANCEL };

struct XState
{
    bool down = false;
    bool owned = false;   // this press belongs to field orders
    bool ordered = false; // a trigger order was pulled during this press
    uint32_t t0 = 0;
};

// enabled: FIELD ORDERS on and the viewer is a vehicle (not the host station / gun),
// evaluated at the press. *aiming: order mode this frame.
inline XEvent UpdateX(XState &s, bool xDown, bool enabled, uint32_t now, bool *aiming)
{
    XEvent ev = XE_NONE;
    if (xDown && !s.down)
    {
        s.owned = enabled;
        s.ordered = false;
        s.t0 = now;
    }
    if (!xDown && s.down && s.owned)
    {
        ev = s.ordered ? XE_NONE : (now - s.t0 < kTapMs ? XE_TAP : XE_CANCEL);
        s.owned = false;
    }
    s.down = xDown;
    *aiming = s.owned && s.down;
    return ev;
}

// ---- R trigger ownership -------------------------------------------------------
// A squeeze that begins (crosses 0.2, below the game's 0.25 fire threshold) while the
// hand is in order mode or on the wrist is owned by the field layer until it is
// released: it never reaches the game, so no shot is fired, even if X is released or
// the ray leaves the wrist mid-squeeze. *click = the squeeze crossed 0.6 (the board's
// click threshold) while owned; at most ONE click per owned squeeze (
// 0 -> 0.7 -> 0.4 -> 0.7 must not click twice). A squeeze that began earlier (already
// firing) is left alone.
struct TrigState
{
    bool owned = false;
    bool prevAbove = false;
    bool prevHigh = false;
    bool clicked = false; // this owned squeeze already clicked (until released below 0.2)
};

inline bool UpdateTrig(TrigState &t, bool claim, float trig, bool *click)
{
    const bool above = trig > 0.2f, high = trig > 0.6f;
    *click = false;
    if (above && !t.prevAbove && claim)
    {
        t.owned = true;
        t.clicked = false;
    }
    if (!above)
    {
        t.owned = false;
        t.clicked = false;
    }
    if (t.owned && high && !t.prevHigh && !t.clicked)
    {
        *click = true;
        t.clicked = true;
    }
    t.prevAbove = above;
    t.prevHigh = high;
    return t.owned;
}

// squad wheel owns the hands ----------------------------------
// While Y is physically held in a vehicle with SQUAD WHEEL on (this poll, not the last
// one), the wheel owns both hands: no grip reaches the table or the game, no board /
// field / wrist click and no weapon fire. A trigger squeezed while the wheel owned the
// hands stays swallowed until it is physically released (also after Y is let go).
inline bool WheelOwnsHands(bool wheelOn, bool inVehicle, bool yDown) { return wheelOn && inVehicle && yDown; }
struct WheelTrig
{
    bool latch = false;
};
// true = swallow the trigger this poll
inline bool UpdateWheelTrig(WheelTrig &w, bool owns, float trig)
{
    if (trig < 0.2f)
        w.latch = false;
    else if (owns)
        w.latch = true;
    return owns || w.latch;
}

// ---- what an order at the pointed spot means -------------------------------------
// Same context rule as the original 3D view / map click (ypaworld_func64__sub21):
// a visible enemy unit nearer than the ground = attack it; else the ground's sector:
// own and seen = move, any other = attack the sector; border sectors are no target.
enum OrderKind { ORD_NONE = 0, ORD_MOVE, ORD_ATTACK_UNIT, ORD_ATTACK_SECTOR };

struct HitInfo
{
    bool unit = false;        // a unit is on the ray
    bool unitEnemy = false;
    bool unitVisible = false; // its sector is seen and it is not stealthed for the player
    double unitDist = 0;
    bool ground = false;      // terrain / building hit
    double groundDist = 0;
    bool sectorBorder = false;
    bool sectorOwn = false;
    bool sectorSeen = false;
};

inline OrderKind DecideOrder(const HitInfo &h)
{
    const bool unitOk = h.unit && h.unitEnemy && h.unitVisible;
    if (unitOk && (!h.ground || h.unitDist <= h.groundDist))
        return ORD_ATTACK_UNIT;
    if (!h.ground || h.sectorBorder)
        return ORD_NONE;
    return (h.sectorOwn && h.sectorSeen) ? ORD_MOVE : ORD_ATTACK_SECTOR;
}

// ---- wrist row click ---------------------------------------------------------------
// First click on a row selects that squad; a click on the squad that is already the
// current one dives into its leader (vehicle only).
enum WristAct { WR_NONE = 0, WR_SELECT, WR_DIVE };

inline WristAct WristClick(uint32_t rowGid, uint32_t currentGid, bool inVehicle)
{
    if (!rowGid)
        return WR_NONE;
    if (rowGid != currentGid)
        return WR_SELECT;
    return inVehicle ? WR_DIVE : WR_NONE;
}

// Ray vs a rectangle (tl + right * [0,w] + down * [0,h]); plain arrays so the test can
// feed numbers. Returns the hit parameters (x along right, y along down, t along d).
inline bool RayRect(const double o[3], const double d[3], const double tl[3], const double right[3],
                    const double down[3], double w, double h, double *x, double *y, double *t)
{
    const double n[3] = {right[1] * down[2] - right[2] * down[1], right[2] * down[0] - right[0] * down[2],
                         right[0] * down[1] - right[1] * down[0]};
    const double dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (std::fabs(dn) < 1e-9)
        return false;
    const double tt = ((tl[0] - o[0]) * n[0] + (tl[1] - o[1]) * n[1] + (tl[2] - o[2]) * n[2]) / dn;
    if (tt <= 0)
        return false;
    const double rel[3] = {o[0] + d[0] * tt - tl[0], o[1] + d[1] * tt - tl[1], o[2] + d[2] * tt - tl[2]};
    *x = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
    *y = rel[0] * down[0] + rel[1] * down[1] + rel[2] * down[2];
    *t = tt;
    return *x >= 0 && *x <= w && *y >= 0 && *y <= h;
}

// ---- Y squad wheel (vehicle, SQUAD WHEEL on) ---------------------------------------
// Y held = wheel open. Right stick flick highlights a squad; letting the stick go
// back (or releasing Y while still flicked) makes it the current squad. Left stick
// up / down while open = aggression +1 / -1 (one step per push) for the highlighted
// squad, else the current one. Y tapped (< kYTapMs, nothing chosen) = the original
// squad manager, as Y did in a vehicle before.
const uint32_t kYTapMs = 350;

struct WheelState
{
    bool down = false;
    bool owned = false;
    bool used = false;   // a flick or an aggression step happened in this press
    bool flicked = false;
    int hl = -1;         // highlighted slot
    int aggLatch = 0;    // left stick: waiting for the return to centre
    uint32_t t0 = 0;
};

struct WheelOut
{
    bool open = false;
    int select = -1;  // slot that becomes the current squad this frame
    int aggStep = 0;  // +1 / -1 this frame
    bool tap = false; // pass a Y tap to the game (original squad manager)
};

inline int WheelSlot(float sx, float sy, int n)
{
    if (n <= 0)
        return -1;
    double a = std::atan2((double)sx, (double)sy); // 0 = up, clockwise
    if (a < 0)
        a += 2 * M_PI;
    return ((int)std::floor(a / (2 * M_PI / n) + 0.5)) % n;
}

inline WheelOut UpdateWheel(WheelState &w, bool yDown, bool enabled, float rx, float ry, float ly, int n, uint32_t now)
{
    WheelOut o;
    if (yDown && !w.down)
    {
        w.owned = enabled;
        w.used = false;
        w.flicked = false;
        w.hl = -1;
        w.aggLatch = 0;
        w.t0 = now;
    }
    if (w.owned && yDown)
    {
        o.open = true;
        const double mag = std::sqrt((double)rx * rx + (double)ry * ry);
        if (mag > 0.6 && n > 0)
        {
            w.hl = WheelSlot(rx, ry, n);
            w.flicked = true;
            w.used = true;
        }
        else if (mag < 0.3 && w.flicked)
        {
            o.select = w.hl; // flick released: this squad is now the current one
            w.flicked = false;
        }
        if (!w.aggLatch && (ly > 0.7f || ly < -0.7f))
        {
            o.aggStep = ly > 0 ? 1 : -1;
            w.aggLatch = 1;
            w.used = true;
        }
        else if (w.aggLatch && ly > -0.3f && ly < 0.3f)
            w.aggLatch = 0;
    }
    if (!yDown && w.down && w.owned)
    {
        if (w.flicked && w.hl >= 0)
            o.select = w.hl;
        else if (!w.used && now - w.t0 < kYTapMs)
            o.tap = true;
        w.owned = false;
    }
    w.down = yDown;
    return o;
}

// Aggression: the game stores 0..100 (squad manager buttons = 25 * i); the wheel
// shows 1..5.
inline int AggrLevel(int aggr) { int l = (aggr + 12) / 25 + 1; return l < 1 ? 1 : (l > 5 ? 5 : l); }
inline int AggrValue(int level) { return 25 * ((level < 1 ? 1 : (level > 5 ? 5 : level)) - 1); }

} // namespace FieldLogic
} // namespace VR

#endif
