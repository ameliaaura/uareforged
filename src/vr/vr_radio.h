// Urban Assault VR — directional radio.
//
//  * Directional callouts: when the game plays a voice message tied to a place
//    (unit / sector / host station / building), the voice is panned towards that
//    place relative to the player's HEAD (VR head pose), not the vehicle nose.
//    It reuses the engine's own voice carrier (no new OpenAL source), keeps the
//    engine's voice distance (no extra attenuation) and caps the pan so a hard-side
//    voice still reaches both ears. Messages without a place stay as the engine
//    plays them (centred).
//  * Wrist ping: a short arrow on the left-wrist radar towards the event, plus a
//    haptic tick on the left controller.
//  * RecentEvents(): the last radio events with place, type and age, for the war
// table to flash the sector. No table dependency here.
//
// Switch: VR::DirectionalRadio() (Settings row "DIRECTIONAL RADIO", profile
// ";#!vr_dirradio", -1 follows VR MODE: ON in COMMAND DECK, OFF in CLASSIC).
// Off = the engine's voice positioning exactly as before (Classic).
// Everything is a no-op unless VR is active.
#ifndef UA_VR_RADIO_H_INCLUDED
#define UA_VR_RADIO_H_INCLUDED

#include <stdint.h>
#include "../vectors.h"

class NC_STACK_ypaworld;
class NC_STACK_ypabact;
struct yw_arg159;

namespace VRRadio
{

enum EventType
{
    EV_OTHER = 0,         // a voice message with a place but no special class
    EV_UNDER_ATTACK,      // enemy contact (3), squad asks for support (18)
    EV_UNIT_LOST,         // unit / squad leader destroyed (8)
    EV_HOST_ATTACKED,     // enemy near the host station (13), host energy low (40)
    EV_BUILDING_ATTACKED, // power station / flak / radar attacked (33, 34, 35)
    EV_BUILDING_LOST,     // flak / radar destroyed (31, 32), power station lost (67)
    EV_ENEMY_HOST,        // enemy host station found (6) / destroyed (11)
    EV_SECTOR_TAKEN,      // sector conquered (2), power station taken (45), key sector taken (82)
    EV_SECTOR_LOST,       // key sector lost (81)
    EV_TYPE_COUNT
};
const char *TypeName(int type);
int TypeForMsg(int msgId);

struct Event
{
    float x = 0, y = 0, z = 0; // world position (engine units)
    int sectorX = -1, sectorY = -1; // map sector of the position
    int type = EV_OTHER;
    int msgId = 0;
    uint32_t gid = 0;          // reporting unit (0 = none / sector event)
    float ageS = 0;            // seconds since the event (filled by RecentEvents)
    bool voiced = false;       // its voice message was the one played
};

// Up to `max` newest events (newest first) younger than `maxAgeS`. Returns the
// count. Records only positioned events, only while VR is active; independent of
// the DIRECTIONAL RADIO switch (the table decides what to show).
int RecentEvents(Event *out, int max, float maxAgeS = 10.0f);

// ---- hooks (game code) ------------------------------------------------------
// ypaworld_func159, before the voice is chosen: classify + record the event.
void NoteEvent(NC_STACK_ypaworld *yw, const yw_arg159 *arg);
// Events whose place is not the reporting unit (sector events without a unit, power
// station attacked, enemy host found / destroyed): the place of the next NoteEvent
// for `msgId` in this same game frame. Any NoteEvent clears it, so a message the
// game drops (placeMessage filters) never lends its place to another event.
void SetNextEventPos(const vec3d &pos, int msgId);
// VoiceMessagePlayFile, after the new voice is set up: this event's voice plays.
void VoiceStarted(NC_STACK_ypaworld *yw, NC_STACK_ypabact *unit);
// VoiceMessageCalcPositionToUnit, after the engine placed the carrier: with the
// switch on, moves it so the engine pans the voice towards the event relative to
// the head. Returns true when it changed the position.
bool VoicePosition(NC_STACK_ypaworld *yw, NC_STACK_ypabact *unit, vec3d *carrierPos);

// ---- hooks (VR module) ------------------------------------------------------
// BeginWorldView: head right / forward in world axes (rows of the centre-eye view).
void NoteHead(const double right[3], const double fwd[3]);
// Gameplay frame: wrist ping direction (heading-up), dev aid UA_VR_RADIO_TEST.
void Gameplay(NC_STACK_ypaworld *yw);
// Wrist ping for the cockpit's wrist radar: (side, ahead) unit direction on the
// heading-up radar, strength 1 -> 0 over the ping, event type. False = no ping.
bool WristPing(float *side, float *ahead, float *strength, int *type);

} // namespace VRRadio

#endif
