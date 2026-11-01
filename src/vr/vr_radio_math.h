// directional radio: pure math for the directional radio.
// No engine dependencies, so tools/vr/test_vr_radio.cpp can unit-test it;
// vr_radio.cpp uses it to place the voice-message carrier.
//
// Axes are the engine's world axes (UA: +X east, +Y DOWN, +Z north). Bearings are
// horizontal (the Y component is ignored): a voice callout says "over there", not
// "above you", and a pitched head must not swing the pan.
#ifndef UA_VR_RADIO_MATH_H_INCLUDED
#define UA_VR_RADIO_MATH_H_INCLUDED

#include <math.h>

namespace VR
{
namespace RadioMath
{

// Bearing of the event direction (dx, dz) relative to a frame given by its forward
// (fx, fz) and right (rx, rz) vectors, horizontal parts only. Radians in
// (-pi, pi]: 0 = dead ahead, + = to the right, +-pi = behind.
// The heading is where the face points: the forward vector's horizontal part, made
// unit length (pitch and roll do not change it). Only when the forward vector is
// near vertical (looking almost straight down / up: horizontal part < 0.35) does it
// blend continuously into the heading implied by the right vector, (-rz, rx), so the
// bearing never jumps. The right axis is derived from the heading in the engine's
// world handedness (right = (fz, -fx) in X/Z, as the cockpit radar blips and the
// listener AxisX).
inline double RelBearing(double fx, double fz, double rx, double rz, double dx, double dz)
{
    const double fl = sqrt(fx * fx + fz * fz), rl = sqrt(rx * rx + rz * rz);
    double hx = 0, hz = 0;
    double t = (fl - 0.10) / (0.35 - 0.10);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double w = t * t * (3 - 2 * t); // weight of the forward heading
    if (fl > 1e-9) { hx += w * fx / fl; hz += w * fz / fl; }
    if (rl > 1e-9) { hx += (1 - w) * -rz / rl; hz += (1 - w) * rx / rl; }
    const double hl = sqrt(hx * hx + hz * hz);
    if (hl < 1e-9)
        return 0.0;
    fx = hx / hl; fz = hz / hl;
    rx = fz; rz = -fx;
    const double ahead = dx * fx + dz * fz;
    const double side = dx * rx + dz * rz;
    if (fabs(ahead) < 1e-12 && fabs(side) < 1e-12)
        return 0.0;
    return atan2(side, ahead);
}

// Stereo pan for a bearing: sin(bearing) scaled by maxPan (0..1). -maxPan = full
// left, +maxPan = full right, 0 = centre (ahead or exactly behind). maxPan < 1 keeps
// a share of the voice in both ears so a hard-side callout stays intelligible.
inline double PanFromBearing(double rel, double maxPan)
{
    double p = sin(rel) * maxPan;
    if (p > 1.0) p = 1.0;
    if (p < -1.0) p = -1.0;
    return p;
}

// Carrier offset (world) from the listener that makes the engine compute exactly
// `pan`: x = AxisX, z = AxisZ of the listener rotation (unit, orthogonal). The
// offset length is `dist` (the engine's own voice distance, 100 units), so the
// voice volume is unchanged: no distance attenuation at all.
inline void CarrierOffset(const double ax[3], const double az[3], double pan, double dist, double out[3])
{
    const double s = pan, c = sqrt(fmax(0.0, 1.0 - pan * pan));
    for (int i = 0; i < 3; i++)
        out[i] = (ax[i] * s + az[i] * c) * dist;
}

// The engine's pan for a carrier offset (listener -> carrier), as
// SFXEngine::sb_0x424c74__sub2__sub1 computes it (0..127, 64 = centre; the
// reverse-stereo setting is applied there on top and is not modelled here).
inline int EnginePan(const double ax[3], const double off[3])
{
    const double len = sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    double v = 0.0;
    if (len > 0.0)
        v = (ax[0] * off[0] + ax[1] * off[1] + ax[2] * off[2]) / len;
    int p = (int)(v * 64.0 + 64.0);
    return p < 0 ? 0 : (p > 127 ? 127 : p);
}

// Wrist radar (heading-up, as the cockpit blips): event offset (dx, dz) relative to
// the vehicle forward (fx, fz) -> (side, ahead) unit vector on the radar face.
inline void RadarDir(double fx, double fz, double dx, double dz, float *side, float *ahead)
{
    const double fl = sqrt(fx * fx + fz * fz);
    if (fl > 1e-9) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
    const double a = dx * fx + dz * fz, s = dx * fz - dz * fx; // same formula as vr_cockpit.cpp blips
    const double l = sqrt(a * a + s * s);
    *side = l > 1e-9 ? (float)(s / l) : 0.0f;
    *ahead = l > 1e-9 ? (float)(a / l) : 1.0f;
}

} // namespace RadioMath
} // namespace VR

#endif
