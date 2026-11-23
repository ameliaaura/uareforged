#ifndef TEST_PROBES_H_INCLUDED
#define TEST_PROBES_H_INCLUDED

// TEST ONLY (UA_TEST_HOOKS builds, never shipped; every probe is also off unless its
// environment variable is set): frame-rate invariance probes for the code converted in
// frame_rate.h. Run with UA_SIM_FPS=<f> (steady simulated frame rate) and compare the lines.
#ifdef UA_TEST_HOOKS

#include <cstdint>
#include "vectors.h"

namespace TestProbe
{
// UA_TEST_TURN=1: every banking aircraft (flyer type 2) under AI control turns at its full
// rate, all the time, and once per (simulated) real second the mean bank angle, the mean turn
// rate and the mean bank target of those aircraft are logged ("turnprobe:").
bool TurnForced();
void Turn(float yawPerFrame, float frameSeconds, float bankTarget, float bank);

// UA_TEST_DAMP=1: a unit braking by a per-frame factor (a host station drifting to a stop, a
// UFO stopping) logs, when it stops, its speed at the start, the distance it drifted and the
// real time it took ("dampprobe:"). Call Damp() on every braking frame before the factor is
// applied and DampStop() when it has stopped.
void Damp(const char *tag, const void *unit, float speed, const vec3d &pos);
void DampStop(const char *tag, const void *unit, const vec3d &pos);

// main loop: one call per frame with its measured time (1/1024 s)
void Frame(uint32_t measured);
}

#endif
#endif
