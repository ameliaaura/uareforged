// Urban Assault VR — directional radio. See vr_radio.h and
//
//
// How the engine plays a voice message (yw_game_ui.cpp VoiceMessagePlayFile,
// yw_game.cpp VoiceMessageCalcPositionToUnit): one carrier, placed 100 units from
// the player towards the reporting unit (the host station's own messages sit on
// the player = centre). SFXEngine pans every carrier by dot(listener AxisX,
// offset) against the vehicle camera (stereo pan only; volume depends on the
// distance, which is always 100). This module only moves that carrier: it stays
// 100 units from the listener, rotated so the engine's pan equals
// sin(bearing relative to the HEAD) * kMaxPan. No new source, no listener change:
// UA_MUTE / ua-mute.dylib (listener gain 0) still silences it like every other sound.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

#include "vr.h"
#include "vr_radio.h"
#include "vr_radio_math.h"
#include "vr_cockpit.h"
#include "../yw.h"
#include "../ypabact.h"
#include "../world/consts.h"

namespace VRRadio
{

namespace
{

const double kMaxPan = 0.85;      // a hard-side voice keeps ~15 % in the far ear
const double kVoiceDist = 100.0;  // the engine's voice carrier distance (yw_game.cpp)
const double kHereDist = 150.0;   // closer than this (about 14 m) = "here": centred
const int kPingMs = 2500;         // wrist arrow
const int kHapticGapMs = 800;
const int kRing = 16;

struct Rec
{
    Event e;
    int32_t timeMs = 0;
    bool centre = false; // no place (or right here): the voice is centred
};

Rec g_ring[kRing];
int g_ringN = 0, g_ringHead = 0; // g_ringHead = next write slot
int32_t g_nowMs = 0;

// the event of the current func159 call (consumed by VoiceStarted)
bool g_pendingOk = false;
Rec g_pending;
bool g_nextPosOk = false;
vec3d g_nextPos;
int g_nextPosMsg = 0, g_nextPosFrame = -1;
int32_t g_lastGameplayMs = 0; // clock of the previous gameplay frame (rewind = new level / restart)

// the voice that is playing
bool g_voiceOk = false;
Rec g_voice;
bool g_voiceLogged = false;
double g_voiceLastPan = 0.0;

// head (world axes), from BeginWorldView
double g_headR[3] = {1, 0, 0}, g_headF[3] = {0, 0, 1};
int g_headFrame = -1000;
int g_frame = 0;

// wrist ping
bool g_pingOk = false;
int32_t g_pingStart = 0;
float g_pingSide = 0, g_pingAhead = 1, g_pingStrength = 0;
int g_pingType = EV_OTHER;
vec3d g_pingPos;
int32_t g_lastHaptic = -100000;

int g_envSwitch = -2; // UA_VR_DIRRADIO: -1 unset, 0 / 1 forced
bool g_logOff = false; // UA_VR_RADIO_LOG=1: also log events while the switch is off
bool g_haptics = true;
int g_testAt = -2;     // UA_VR_RADIO_TEST=<gameplay frame>

void ReadEnv()
{
    if (g_envSwitch != -2)
        return;
    const char *v = getenv("UA_VR_DIRRADIO");
    g_envSwitch = (v && v[0]) ? (atoi(v) != 0 ? 1 : 0) : -1;
    v = getenv("UA_VR_RADIO_LOG");
    g_logOff = v && atoi(v) != 0;
    v = getenv("UA_VR_HAPTICS");
    g_haptics = !(v && v[0] && atoi(v) == 0);
    v = getenv("UA_VR_RADIO_TEST");
    g_testAt = (v && v[0]) ? atoi(v) : -1;
    if (g_envSwitch >= 0)
        fprintf(stderr, "UAVR radio: UA_VR_DIRRADIO=%d overrides the DIRECTIONAL RADIO setting\n", g_envSwitch);
}

bool On()
{
    if (!VR::Active())
        return false;
    ReadEnv();
    return g_envSwitch >= 0 ? g_envSwitch == 1 : VR::DirectionalRadio();
}

void SectorOf(double x, double z, int *sx, int *sy)
{
    *sx = (int)floor(x / World::CVSectorLength);
    *sy = (int)floor(-z / World::CVSectorLength);
}

bool Alive(const NC_STACK_ypabact *u)
{
    return u && u->_status != BACT_STATUS_DEAD;
}

// Listener frame of this tick (yw.cpp: SFXEngine::sub_423EFC(_viewerPosition, _viewerRotation)).
void ListenerAxes(NC_STACK_ypaworld *yw, double ax[3], double az[3])
{
    const mat3x3 &r = yw->_viewerRotation;
    ax[0] = r.m00; ax[1] = r.m01; ax[2] = r.m02;
    az[0] = r.m20; az[1] = r.m21; az[2] = r.m22;
}

void Ping(NC_STACK_ypaworld *yw, const Rec &r)
{
    g_pingOk = true;
    g_pingStart = g_nowMs;
    g_pingType = r.e.type;
    g_pingPos = vec3d(r.e.x, r.e.y, r.e.z);
    g_pingStrength = 1.0f;
    const mat3x3 &m = yw->_viewerRotation;
    VR::RadioMath::RadarDir(m.m20, m.m22, r.e.x - yw->_viewerPosition.x, r.e.z - yw->_viewerPosition.z, &g_pingSide, &g_pingAhead);
    if (g_haptics && g_nowMs - g_lastHaptic >= kHapticGapMs)
    {
        g_lastHaptic = g_nowMs;
        VR::Cockpit::Haptic(0, 0.35f, 25.0f, "radio"); // left hand = the wrist
    }
}

} // namespace

const char *TypeName(int type)
{
    static const char *n[EV_TYPE_COUNT] = {"other", "under_attack", "unit_lost", "host_attacked", "building_attacked",
                                           "building_lost", "enemy_host", "sector_taken", "sector_lost"};
    return (type >= 0 && type < EV_TYPE_COUNT) ? n[type] : "?";
}

int TypeForMsg(int msgId)
{
    switch (msgId)
    {
    case 3: case 18: return EV_UNDER_ATTACK;
    case 8: return EV_UNIT_LOST;
    case 13: case 40: return EV_HOST_ATTACKED;
    case 33: case 34: case 35: return EV_BUILDING_ATTACKED;
    case 31: case 32: case 67: return EV_BUILDING_LOST;
    case 6: case 11: return EV_ENEMY_HOST;
    case 2: case 45: case 82: return EV_SECTOR_TAKEN;
    case 81: return EV_SECTOR_LOST;
    default: return EV_OTHER;
    }
}

void SetNextEventPos(const vec3d &pos, int msgId)
{
    if (!VR::Active())
        return;
    g_nextPos = pos;
    g_nextPosMsg = msgId;
    g_nextPosFrame = g_frame;
    g_nextPosOk = true;
}

void NoteEvent(NC_STACK_ypaworld *yw, const yw_arg159 *arg)
{
    const bool nextOk = g_nextPosOk && arg && arg->MsgID == g_nextPosMsg && g_nextPosFrame == g_frame;
    g_nextPosOk = false;
    g_pendingOk = false;
    if (!VR::Active() || !yw || !arg || !arg->MsgID || !yw->_userUnit)
        return;
    g_nowMs = yw->_timeStamp;

    Rec r;
    r.timeMs = yw->_timeStamp;
    r.e.msgId = arg->MsgID;
    r.e.type = TypeForMsg(arg->MsgID);
    NC_STACK_ypabact *u = arg->unit;
    vec3d p;
    bool ok = false;
    if (nextOk)
    {
        p = g_nextPos; // the event's own place (sector, power station, enemy host)
        ok = true; // gid stays 0: the place is fixed, not the reporter
    }
    else if (u && u == yw->_userRobo)
    {
        // The host station's own messages: only "host under attack / energy low" have a
        // place (the station). Voiced centred while the player is in the station; the
        // place is still recorded for the war table.
        if (r.e.type == EV_HOST_ATTACKED)
        {
            p = u->_position;
            r.e.gid = u->_gid;
            ok = true;
        }
    }
    else if (u)
    {
        p = u->_position;
        r.e.gid = u->_gid;
        ok = true;
    }
    // Audio: centred when there is no place, when the station reports while the player is
    // in it, or when the place is right here (the player's own unit). The place itself
    // still goes into the ring (RecentEvents -> table flash) whenever there is one.
    bool centre = !ok;
    if (ok)
    {
        const vec3d d = p - yw->_userUnit->_position;
        if (sqrt(d.x * d.x + d.z * d.z) < kHereDist || (u && u == yw->_userRobo && yw->_userUnit == yw->_userRobo && !nextOk))
            centre = true;
        r.e.x = (float)p.x; r.e.y = (float)p.y; r.e.z = (float)p.z;
        SectorOf(p.x, p.z, &r.e.sectorX, &r.e.sectorY);
        g_ring[g_ringHead] = r; // the ring keeps the place; the voice flag below is audio only
        g_ringHead = (g_ringHead + 1) % kRing;
        g_ringN = std::min(g_ringN + 1, kRing);
    }
    if (centre)
    {
        // No place (or here): centred. The engine already centres the host station's own and
        // unit-less messages; a unit message it would pan towards the reporter, so
        // VoicePosition centres that one explicitly.
        if (g_logOff || On())
            fprintf(stderr, "UAVR radio: msg %d (%s) %s -> centred\n", arg->MsgID, TypeName(r.e.type), ok ? "is here" : "has no place");
        r.centre = true;
        g_pending = r;
        g_pendingOk = true;
        return;
    }
    g_pending = r;
    g_pendingOk = true;
    if (!On() && g_logOff)
        fprintf(stderr, "UAVR radio: msg %d (%s) at sector %d,%d - directional radio OFF, engine positioning unchanged\n",
                r.e.msgId, TypeName(r.e.type), r.e.sectorX, r.e.sectorY);
}

void VoiceStarted(NC_STACK_ypaworld *yw, NC_STACK_ypabact *unit)
{
    (void)unit;
    g_voiceOk = false;
    g_voiceLogged = false;
    if (!g_pendingOk || !yw)
        return;
    g_pendingOk = false;
    if (g_pending.centre)
    {
        // centre explicitly only what the engine would pan (a unit other than the host station)
        if (On() && unit && unit != yw->_userRobo)
        {
            g_voice = g_pending;
            g_voiceOk = true;
        }
        return;
    }
    // mark the ring entry as voiced (it is the newest one)
    const int last = (g_ringHead + kRing - 1) % kRing;
    if (g_ringN && g_ring[last].timeMs == g_pending.timeMs && g_ring[last].e.msgId == g_pending.e.msgId)
        g_ring[last].e.voiced = true;
    g_pending.e.voiced = true;
    if (!On())
        return;
    g_voice = g_pending;
    g_voiceOk = true;
    Ping(yw, g_voice);
}

bool VoicePosition(NC_STACK_ypaworld *yw, NC_STACK_ypabact *unit, vec3d *carrierPos)
{
    if (!g_voiceOk || !yw || !carrierPos || !On())
        return false;

    if (g_voice.centre)
    {
        double ax[3], az[3];
        ListenerAxes(yw, ax, az);
        const vec3d lp = yw->_viewerPosition;
        *carrierPos = vec3d(lp.x + az[0] * kVoiceDist, lp.y + az[1] * kVoiceDist, lp.z + az[2] * kVoiceDist);
        if (!g_voiceLogged)
        {
            const double off[3] = {az[0] * kVoiceDist, az[1] * kVoiceDist, az[2] * kVoiceDist};
            fprintf(stderr, "UAVR radio: centre msg %d (%s): no place -> engine pan %d\n", g_voice.e.msgId,
                    TypeName(g_voice.e.type), VR::RadioMath::EnginePan(ax, off));
            g_voiceLogged = true;
        }
        return true;
    }

    // the event place: the reporting unit while it lives, else where it was
    vec3d ev(g_voice.e.x, g_voice.e.y, g_voice.e.z);
    if (g_voice.e.gid && unit && unit->_gid == g_voice.e.gid && Alive(unit))
        ev = unit->_position;

    const vec3d lp = yw->_viewerPosition;
    double ax[3], az[3];
    ListenerAxes(yw, ax, az);

    // head frame (late pose of the last rendered frame); the vehicle frame when
    // there is no recent head pose (no world view: should not happen in a mission)
    const bool head = g_frame - g_headFrame <= 30;
    const double *hr = head ? g_headR : ax, *hf = head ? g_headF : az;
    const double rel = VR::RadioMath::RelBearing(hf[0], hf[2], hr[0], hr[2], ev.x - lp.x, ev.z - lp.z);
    const double pan = VR::RadioMath::PanFromBearing(rel, kMaxPan);
    double off[3];
    VR::RadioMath::CarrierOffset(ax, az, pan, kVoiceDist, off);

    const vec3d orig = *carrierPos;
    *carrierPos = vec3d(lp.x + off[0], lp.y + off[1], lp.z + off[2]);

    if (!g_voiceLogged || fabs(pan - g_voiceLastPan) > 0.2)
    {
        const double oo[3] = {orig.x - lp.x, orig.y - lp.y, orig.z - lp.z};
        const double vrel = VR::RadioMath::RelBearing(az[0], az[2], ax[0], ax[2], ev.x - lp.x, ev.z - lp.z);
        fprintf(stderr,
                "UAVR radio: %s msg %d (%s) gid %u sector %d,%d: bearing head %+.1f deg (vehicle %+.1f, %s) -> pan %+.2f, "
                "engine pan %d (engine default %d), carrier %+.0f,%+.0f,%+.0f from listener\n",
                g_voiceLogged ? "repan" : "callout", g_voice.e.msgId, TypeName(g_voice.e.type), g_voice.e.gid,
                g_voice.e.sectorX, g_voice.e.sectorY, rel * 180.0 / M_PI, vrel * 180.0 / M_PI, head ? "head pose" : "no head pose",
                pan, VR::RadioMath::EnginePan(ax, off), VR::RadioMath::EnginePan(ax, oo), off[0], off[1], off[2]);
        g_voiceLogged = true;
        g_voiceLastPan = pan;
    }
    return true;
}

void NoteHead(const double right[3], const double fwd[3])
{
    for (int i = 0; i < 3; i++)
    {
        g_headR[i] = right[i];
        g_headF[i] = fwd[i];
    }
    g_headFrame = g_frame;
}

int RecentEvents(Event *out, int max, float maxAgeS)
{
    int n = 0;
    for (int i = 0; i < g_ringN && n < max; i++)
    {
        const Rec &r = g_ring[(g_ringHead + kRing - 1 - i) % kRing];
        const float age = (g_nowMs - r.timeMs) / 1000.0f;
        if (age > maxAgeS || age < 0)
            continue;
        out[n] = r.e;
        out[n].ageS = age;
        n++;
    }
    return n;
}

bool WristPing(float *side, float *ahead, float *strength, int *type)
{
    if (!g_pingOk || !On())
        return false;
    *side = g_pingSide;
    *ahead = g_pingAhead;
    *strength = g_pingStrength;
    *type = g_pingType;
    return true;
}

// UA_VR_RADIO_TEST=<frame> (dev aid, sim captures): from that gameplay frame, every
// 4 s of game time, one scripted radio event through the game's own
// ypaworld_func159 (real voice files, real carrier): own units picked at the
// right, left and behind of the vehicle, "power station attacked" 1.5 sectors
// ahead (no unit, as the game sends it), a key sector two sectors to the right,
// "host under attack", a centred control (mission start) and a report from the
// player's own unit (right here: centred).
static void TestInject(NC_STACK_ypaworld *yw)
{
    static int step = 0;
    static int32_t next = 0;
    if (g_testAt < 0 || g_frame < g_testAt || !yw->_userRobo || !yw->_userUnit)
        return;
    if (next && yw->_timeStamp < next)
        return;
    next = yw->_timeStamp + 4000;

    double ax[3], az[3];
    ListenerAxes(yw, ax, az);
    const vec3d lp = yw->_userUnit->_position;
    auto pick = [&](double wantDeg) -> NC_STACK_ypabact * {
        NC_STACK_ypabact *best = NULL;
        double bestErr = 1e9;
        auto consider = [&](NC_STACK_ypabact *u) {
            if (!Alive(u) || u == yw->_userUnit)
                return;
            const double dx = u->_position.x - lp.x, dz = u->_position.z - lp.z;
            if (sqrt(dx * dx + dz * dz) < kHereDist * 2)
                return;
            const double b = VR::RadioMath::RelBearing(az[0], az[2], ax[0], ax[2], dx, dz) * 180.0 / M_PI;
            double e = fabs(b - wantDeg);
            if (e > 180) e = 360 - e;
            if (e < bestErr) { bestErr = e; best = u; }
        };
        for (NC_STACK_ypabact *c : yw->_userRobo->_kidList)
        {
            consider(c);
            for (NC_STACK_ypabact *k : c->_kidList)
                consider(k);
        }
        return best;
    };

    yw_arg159 a;
    a.unit = NULL;
    a.Priority = 200 + step; // rising: above anything playing (the game drops a voice of lower / equal priority)
    a.MsgID = 0;
    const int s = step % 8;
    step++;
    switch (s)
    {
    case 0: a.unit = pick(90); a.MsgID = 18; break;   // support requested, right
    case 1: a.unit = pick(-90); a.MsgID = 8; break;   // unit lost, left
    case 2: // power station attacked (as the real producer: no unit), 1.5 sectors ahead
    {
        const double d = 1.5 * World::CVSectorLength;
        SetNextEventPos(vec3d(lp.x + az[0] * d, lp.y, lp.z + az[2] * d), 33);
        a.MsgID = 33;
        break;
    }
    case 3: a.unit = pick(180); a.MsgID = 1; break;   // unit report (other), behind
    case 4: // key sector taken, two sectors to the right of the vehicle
    {
        const double d = 2 * World::CVSectorLength;
        SetNextEventPos(vec3d(lp.x + ax[0] * d, lp.y, lp.z + ax[2] * d), 82);
        a.MsgID = 82;
        break;
    }
    case 5: a.unit = yw->_userRobo; a.MsgID = 13; break; // enemy near the host station
    case 6: a.unit = yw->_userRobo; a.MsgID = 41; break; // control: no place, centred
    case 7: a.unit = yw->_userUnit; a.MsgID = 1; break;  // the player's own unit: right here, centred
    }
    if ((s == 0 || s == 1 || s == 3) && !a.unit)
    {
        fprintf(stderr, "UAVR radio test: step %d: no own unit found\n", s);
        return;
    }
    fprintf(stderr, "UAVR radio test: inject msg %d (%s) unit gid %d\n", a.MsgID, TypeName(TypeForMsg(a.MsgID)),
            a.unit ? (int)a.unit->_gid : 0);
    yw->ypaworld_func159(&a);
}

void Gameplay(NC_STACK_ypaworld *yw)
{
    g_frame++;
    if (!yw || !VR::Active())
        return;
    ReadEnv();
    // new level / restart / load: the game clock starts again, old events are void
    static int level = -1;
    if (yw->_levelInfo.LevelID != level || yw->_timeStamp < g_lastGameplayMs)
    {
        level = yw->_levelInfo.LevelID;
        g_ringN = g_ringHead = 0;
        g_pingOk = g_voiceOk = g_pendingOk = g_nextPosOk = false;
        g_lastHaptic = -100000;
    }
    g_lastGameplayMs = yw->_timeStamp;
    g_nowMs = yw->_timeStamp;
    if (g_pingOk)
    {
        const int32_t t = g_nowMs - g_pingStart;
        if (t < 0 || t > kPingMs || !On())
            g_pingOk = false;
        else
        {
            g_pingStrength = 1.0f - (float)t / kPingMs;
            const mat3x3 &r = yw->_viewerRotation;
            const vec3d vp = yw->_viewerPosition;
            VR::RadioMath::RadarDir(r.m20, r.m22, g_pingPos.x - vp.x, g_pingPos.z - vp.z, &g_pingSide, &g_pingAhead);
        }
    }
    TestInject(yw);
}

} // namespace VRRadio
