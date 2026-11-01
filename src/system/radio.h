#ifndef UA_SYSTEM_RADIO_H
#define UA_SYSTEM_RADIO_H

// Optional "Resistance radio chatter" bridge - FLAVOUR ONLY.
//
// Opt-in with UA_RADIO=1. When the variable is unset (the default) every entry
// point below returns after a single integer compare: no socket is created, no
// thread is started and nothing is allocated. `lsof` on the engine pid shows no
// UDP descriptor at all - that is the acceptance test for "zero impact".
//
// When enabled the engine:
//   * emits one small JSON datagram per notable game event to UDP
//     127.0.0.1:61237 (61234 belongs to the multiplayer netcode - do not reuse),
//     fire-and-forget on a non-blocking socket, never waiting for the sidecar;
//   * polls the same socket every ~250 ms for a one-line reply and pushes that
//     line through the game's own message path (ypaworld_func159 -> info_log),
//     which is what the 1998 engine already uses for every radio call. The
//     line therefore inherits the native on-screen ticker (typewriter reveal,
//     _iniColors[64], auto-fade - yw_game_ui.cpp:7943) and the scrollable log
//     window for free. There is no bespoke drawing code in this feature.
//
// The bridge is deliberately incapable of influencing the simulation: the only
// thing a reply can do is put text on screen. Nothing read from the socket ever
// reaches unit orders, AI state or RNG.

#include <string>

struct yw_arg159;
class NC_STACK_ypaworld;

namespace Radio
{

// True once UA_RADIO=1 has been seen AND the socket is live. Cheap after the
// first call (cached tri-state), safe to call from a per-frame path.
bool Enabled();

// Emit hook. Called at the top of NC_STACK_ypaworld::ypaworld_func159, the
// engine's single chokepoint for "something noteworthy happened" events.
void NotifyEvent(NC_STACK_ypaworld *yw, const yw_arg159 *arg);

// Poll hook, self-throttled to POLL_INTERVAL_MS off the world clock. Feeds any
// accepted line into the game's message log.
void Update(NC_STACK_ypaworld *yw, int nowMs);

// Closes the socket. Idempotent; also runs from the module destructor.
void Shutdown();

}

#endif
