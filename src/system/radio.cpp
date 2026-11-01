#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "env.h"
#include "includes.h"
#include "yw_internal.h"
#include "yw.h"
#include "system/radio.h"

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

// See system/radio.h for the contract. Everything here is cosmetic: the only
// effect a sidecar reply can have on the running game is a line of text.

namespace Radio
{

static const uint16_t SIDECAR_PORT       = 61237; // 61234 is the MP netcode
static const int      POLL_INTERVAL_MS   = 250;   // rides the world update tick
static const int      MIN_DISPLAY_GAP_MS = 8000;  // never bark over ourselves
static const int      MAX_REPLY_AGE_MS   = 3000;  // drop stale replies
static const size_t   MAX_LINE_CHARS     = 96;    // info_log's txt[] is 128

// MsgID -> event name. MsgIDs come from NC_STACK_yparobo::placeMessage and the
// handful of direct ypaworld_func159 callers; anything not in this table is not
// interesting enough to bark about and is dropped without a datagram.
struct EventName
{
    int         msgId;
    const char *name;
};

static const EventName EVENT_NAMES[] =
{
    { 2,  "sector_conquered"       }, // FEEDBACK_SECTCONQ
    { 6,  "enemy_hs_found"         }, // FEEDBACK_FOUNDEHS
    { 11, "enemy_hs_destroyed"     }, // FEEDBACK_HSDESTR
    { 12, "own_hs_destroyed"       }, // FEEDBACK_OWNHSDESTR
    { 13, "enemy_near_hs"          }, // FEEDBACK_ENMYNEARHS
    { 18, "support_requested"      }, // FEEDBACK_REQSUP  (squad in trouble)
    { 22, "enemy_sector_entered"   }, // FEEDBACK_ESECTENTER
    { 31, "flak_destroyed"         }, // FEEDBACK_FLAKDESTR
    { 32, "radar_destroyed"        }, // FEEDBACK_RADARDESTR
    { 33, "power_station_attacked" }, // FEEDBACK_ATKPS
    { 34, "flak_attacked"          }, // FEEDBACK_ATKFLAK
    { 35, "radar_attacked"         }, // FEEDBACK_ATKRADAR
    { 36, "building_built"         }, // power station finished
    { 37, "building_built"         }, // flak finished
    { 38, "building_built"         }, // radar finished
    { 40, "hs_energy_low"          }, // FEEDBACK_HSENERGY
    { 41, "mission_start"          }, // first world frame of a level
    { 45, "power_station_taken"    }, // we took a power station
    { 67, "power_station_lost"     }, // we lost a power station
    { 81, "key_sector_lost"        },
    { 82, "key_sector_taken"       },
};

static const char *EventNameFor(int msgId)
{
    for ( size_t i = 0; i < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]); i++ )
    {
        if ( EVENT_NAMES[i].msgId == msgId )
            return EVENT_NAMES[i].name;
    }
    return NULL;
}

// Owner id -> faction, per the engine's own net-player table (yw_game.cpp).
static const char *FactionFor(int owner)
{
    switch ( owner )
    {
    case 1: return "Resistance";
    case 3: return "Mykonian";
    case 4: return "Taerkasten";
    case 6: return "Ghorkov";
    default: return "hostile";  // reads correctly inside a line
    }
}

class RadioLink
{
public:
    ~RadioLink() { Close(); }

    bool Enabled()
    {
        if ( _state )
            return _state > 0;

        const char *v = getenv("UA_RADIO");
        if ( !v || atoi(v) != 1 )
        {
            _state = -1;                 // nothing allocated, nothing opened
            return false;
        }

        _state = Open() ? 1 : -1;
        return _state > 0;
    }

    void Send(const char *json, size_t len)
    {
        if ( _fd < 0 )
            return;
#if !defined(_WIN32)
        // Fire and forget. MSG_DONTWAIT on top of O_NONBLOCK because a full
        // socket buffer must never cost us a frame.
        ssize_t r = ::send(_fd, json, len, MSG_DONTWAIT);
        (void)r;                          // EAGAIN/ENOBUFS: the bark is lost, fine
#else
        (void)json; (void)len;
#endif
    }

    // Drains the socket, keeping the newest acceptable line. Returns true and
    // fills `out` when a line should be shown now.
    bool Recv(int nowMs, std::string &out)
    {
        if ( _fd < 0 )
            return false;
#if !defined(_WIN32)
        char    buf[1024];
        bool    got = false;
        ssize_t n;

        while ( (n = ::recv(_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0 )
        {
            buf[n] = 0;

            std::string line;
            int64_t     echo = 0;
            if ( !ParseReply(buf, (size_t)n, line, echo) )
                continue;

            // Stale: the sidecar answered an event the player has moved on from.
            if ( echo > 0 && (int64_t)nowMs - echo > MAX_REPLY_AGE_MS )
                continue;

            if ( line.empty() )
                continue;

            if ( line.size() > MAX_LINE_CHARS )
                line.resize(MAX_LINE_CHARS);

            out = line;
            got = true;                   // keep draining; newest wins
        }

        if ( !got )
            return false;

        // The clock is the WORLD clock, which restarts at 0 on every new level.
        // Without this the first line of level 2 would sit behind a rate limit
        // stamped in level 1's time base.
        if ( nowMs < _lastShownMs )
        {
            _lastShownMs = 0;
            _lastLine.clear();
        }

        // Rate limit AFTER draining so a burst does not queue up behind us.
        if ( _lastShownMs != 0 && nowMs - _lastShownMs < MIN_DISPLAY_GAP_MS )
            return false;

        if ( out == _lastLine )
            return false;

        _lastShownMs = nowMs;
        _lastLine    = out;
        return true;
#else
        (void)nowMs; (void)out;
        return false;
#endif
    }

    void Close()
    {
#if !defined(_WIN32)
        if ( _fd >= 0 )
        {
            ::close(_fd);
            _fd = -1;
        }
#endif
        _state = -1;
    }

    uint32_t NextSeq() { return ++_seq; }

    int  lastPollMs = 0;
    bool inInject   = false;              // re-entrancy guard for func159
    bool debug      = false;              // UA_RADIO_DEBUG=1: log every datagram

private:
    bool Open()
    {
#if !defined(_WIN32)
        _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if ( _fd < 0 )
            return false;

        int fl = ::fcntl(_fd, F_GETFL, 0);
        if ( fl < 0 || ::fcntl(_fd, F_SETFL, fl | O_NONBLOCK) < 0 )
        {
            ::close(_fd);
            _fd = -1;
            return false;
        }

        // connect() on a UDP socket: send()/recv() without an address, and the
        // kernel drops datagrams from anyone other than the sidecar.
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(SIDECAR_PORT);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if ( ::connect(_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 )
        {
            ::close(_fd);
            _fd = -1;
            return false;
        }

        const char *dbg = getenv("UA_RADIO_DEBUG");
        debug = dbg && atoi(dbg) == 1;

        printf("UA_RADIO: flavour chatter bridge up on udp 127.0.0.1:%u%s\n",
               (unsigned)SIDECAR_PORT, debug ? " (debug)" : "");
        return true;
#else
        return false;
#endif
    }

    // Minimal, allocation-light reader for {"line":"...","t":123}. Deliberately
    // not a general JSON parser: it only ever looks for those two keys and
    // ignores everything else, so a malformed or hostile datagram can at worst
    // produce a truncated string.
    static bool ParseReply(const char *buf, size_t len, std::string &line, int64_t &echo)
    {
        const char *p = strstr(buf, "\"line\"");
        if ( !p )
            return false;

        p += 6;
        while ( *p && *p != ':' ) p++;
        if ( !*p ) return false;
        p++;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p != '"' ) return false;
        p++;

        line.clear();
        line.reserve(MAX_LINE_CHARS);

        while ( *p && *p != '"' && line.size() < MAX_LINE_CHARS )
        {
            if ( *p == '\\' && p[1] )
            {
                p++;
                switch ( *p )
                {
                case 'n': case 'r': case 't': line += ' '; break;
                case 'u':
                    // \uXXXX: not worth decoding for a 1998 bark, skip it
                    for ( int i = 0; i < 4 && p[1]; i++ ) p++;
                    break;
                default:  line += *p; break;
                }
                p++;
                continue;
            }

            unsigned char c = (unsigned char)*p;
            line += (c < 0x20) ? ' ' : *p;   // no control codes into the font
            p++;
        }

        const char *tp = strstr(buf, "\"t\"");
        if ( tp )
        {
            tp += 3;
            while ( *tp && *tp != ':' ) tp++;
            if ( *tp )
                echo = (int64_t)strtoll(tp + 1, NULL, 10);
        }

        (void)len;
        return true;
    }

    int         _state = 0;               // 0 unknown, 1 on, -1 off
    int         _fd    = -1;
    uint32_t    _seq   = 0;
    int         _lastShownMs = 0;
    std::string _lastLine;
};

static RadioLink Link;

// Copies `in` into `out` with the handful of characters JSON cares about
// escaped, capped at `cap` bytes so a hostile level name cannot overflow the
// datagram buffer.
static void JsonEscape(const std::string &in, char *out, size_t cap)
{
    size_t o = 0;
    for ( size_t i = 0; i < in.size() && o + 2 < cap; i++ )
    {
        unsigned char c = (unsigned char)in[i];
        if ( c == '"' || c == '\\' )
        {
            out[o++] = '\\';
            out[o++] = (char)c;
        }
        else if ( c < 0x20 )
        {
            out[o++] = ' ';
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

bool Enabled()
{
    return Link.Enabled();
}

void NotifyEvent(NC_STACK_ypaworld *yw, const yw_arg159 *arg)
{
    if ( !yw || !arg )
        return;

    if ( !Link.Enabled() )
        return;

    // Our own injected line comes back through func159 - do not echo it.
    if ( Link.inInject )
        return;

    const char *ev = EventNameFor(arg->MsgID);
    if ( !ev )
        return;                            // MsgID 0 / uninteresting: no traffic

    int player = yw->GetPlayerOwner();
    if ( player <= 0 && yw->_userRobo )
        player = yw->_userRobo->_owner;

    int owner = player;
    int sx = 0, sy = 0;

    if ( arg->unit )
    {
        owner = arg->unit->_owner;
        sx    = arg->unit->_cellId.x;
        sy    = arg->unit->_cellId.y;
    }
    else if ( yw->_userRobo )
    {
        sx = yw->_userRobo->_cellId.x;
        sy = yw->_userRobo->_cellId.y;
    }

    // The engine never tells func159 WHO the opposition is - `arg->unit` is
    // usually the player's own station doing the reporting. Without this the
    // sidecar would write "Resistance at the doorstep" about the player's own
    // side. Best available honest answer: the strongest hostile on the map,
    // by sectors held, units as the tie-break. Both counters are live.
    int enemy = 0;
    for ( int i = 1; i < 8; i++ )
    {
        if ( i == player )
            continue;
        if ( yw->_countSectorsPerOwner[i] == 0 && yw->_countUnitsPerOwner[i] == 0 )
            continue;
        if ( enemy == 0
             || yw->_countSectorsPerOwner[i] > yw->_countSectorsPerOwner[enemy]
             || ( yw->_countSectorsPerOwner[i] == yw->_countSectorsPerOwner[enemy]
                  && yw->_countUnitsPerOwner[i] > yw->_countUnitsPerOwner[enemy] ) )
        {
            enemy = i;
        }
    }

    char level[64];
    JsonEscape(yw->_levelInfo.MapName, level, sizeof(level));

    char json[512];
    int  n = snprintf(json, sizeof(json),
                      "{\"v\":1,\"seq\":%u,\"t\":%d,\"ev\":\"%s\",\"msg\":%d,"
                      "\"pri\":%d,\"owner\":%d,\"faction\":\"%s\","
                      "\"enemy\":\"%s\",\"player\":%d,\"self\":%d,"
                      "\"sector\":[%d,%d],\"level\":\"%s\"}",
                      (unsigned)Link.NextSeq(), yw->_timeStamp, ev, arg->MsgID,
                      arg->Priority, owner, FactionFor(owner),
                      enemy ? FactionFor(enemy) : "hostile", player,
                      (owner == player) ? 1 : 0, sx, sy, level);

    if ( n > 0 && (size_t)n < sizeof(json) )
    {
        Link.Send(json, (size_t)n);

        if ( Link.debug )
            printf("UA_RADIO tx: %s\n", json);
    }
}

void Update(NC_STACK_ypaworld *yw, int nowMs)
{
    if ( !yw || !Link.Enabled() )
        return;

    // Ride the world tick but only actually touch the socket ~4x a second.
    if ( Link.lastPollMs != 0 && nowMs - Link.lastPollMs < POLL_INTERVAL_MS
         && nowMs >= Link.lastPollMs )
        return;

    Link.lastPollMs = nowMs;

    std::string line;
    if ( !Link.Recv(nowMs, line) )
        return;

    // Rare by construction (>= MIN_DISPLAY_GAP_MS apart) so this is a useful
    // trace rather than log spam, and it makes the feature verifiable headless.
    printf("UA_RADIO rx @%dms: %s\n", nowMs, line.c_str());
    fflush(stdout);

    // Push it down the game's own message path. This is the whole display
    // story: func159 files the line in info_log, and the engine's existing
    // ticker (yw_game_ui.cpp:7943) reveals it over the 3D view and fades it
    // out, identical to a native radio call. MsgID 0 = text only, no voice
    // sample, so we never steal a 1998 speech slot.
    yw_arg159 msg;
    msg.unit     = NULL;
    msg.Priority = 60;
    msg.txt      = line;
    msg.MsgID    = 0;

    Link.inInject = true;
    yw->ypaworld_func159(&msg);
    Link.inInject = false;
}

void Shutdown()
{
    Link.Close();
}

}
