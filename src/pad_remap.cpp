#include "pad_remap.h"

#include <ctype.h>
#include <string.h>

namespace PadRemap
{

// Order = the order of the CONTROLLER BUTTONS page. The first block is everything a
// controller did before (defaults = the 0.8.2 mapping in winp.cpp); then the keyboard
// hotkeys of the 1998 key list (no default button). Not listed: Abort/Quit/Escape and
// Pause Game (= "In-mission menu" and "Pause" here) and Send Chat Message (needs typing).
static const Action kActions[] =
{
    { "fire",         "Fire weapon",                     KIND_JOYBIT,    0,    BTN_RIGHTTRIGGER },
    { "machinegun",   "Fire machine gun",                KIND_JOYBIT,    1,    BTN_LEFTTRIGGER },
    { "missilecam",   "Missile camera",                  KIND_JOYBIT,    2,    BTN_Y },
    { "brake",        "Brake",                           KIND_JOYBIT,    3,    BTN_A },
    { "forward",      "Drive forward / climb",           KIND_THROTTLE, -300,  BTN_RIGHTSHOULDER },
    { "reverse",      "Drive back / descend",            KIND_THROTTLE,  300,  BTN_LEFTSHOULDER },
    { "aimup",        "Aim up",                          KIND_HAT,       0,    BTN_DPUP },
    { "aimdown",      "Aim down",                        KIND_HAT,       1,    BTN_DPDOWN },
    { "aimleft",      "Aim left",                        KIND_HAT,       2,    BTN_DPLEFT },
    { "aimright",     "Aim right",                       KIND_HAT,       3,    BTN_DPRIGHT },
    { "menu",         "In-mission menu",                 KIND_ESC,       0,    BTN_START },
    { "pause",        "Pause",                           KIND_PAUSE,     0,    BTN_BACK },
    { "pause2",       "Pause (second button)",           KIND_PAUSE,     0,    BTN_TOUCHPAD },
    { "joyb4",        "Joystick button 5 (key list)",    KIND_JOYBIT,    4,    BTN_B },
    { "joyb5",        "Joystick button 6 (key list)",    KIND_JOYBIT,    5,    BTN_X },
    { "joyb6",        "Joystick button 7 (key list)",    KIND_JOYBIT,    6,    BTN_LEFTSTICK },
    { "joyb7",        "Joystick button 8 (key list)",    KIND_JOYBIT,    7,    BTN_RIGHTSTICK },
    { "waypoints",    "Map: set waypoints (hold)",       KIND_INPBUTTON, 4,    BTN_NONE },
    { "map",          "Map on/off",                      KIND_HOTKEY,    8,    BTN_NONE },
    { "squadmanager", "Squadron manager on/off",         KIND_HOTKEY,    9,    BTN_NONE },
    { "order",        "Give order to units",             KIND_HOTKEY,    0,    BTN_NONE },
    { "create",       "Create vehicle or building",      KIND_HOTKEY,    2,    BTN_NONE },
    { "add",          "Add vehicle to current squad",    KIND_HOTKEY,    3,    BTN_NONE },
    { "nextunit",     "Cycle through units",             KIND_HOTKEY,    22,   BTN_NONE },
    { "nextleader",   "Cycle through squad leaders",     KIND_HOTKEY,    20,   BTN_NONE },
    { "tohost",       "Jump to host station",            KIND_HOTKEY,    21,   BTN_NONE },
    { "toleader",     "Jump to current squad leader",    KIND_HOTKEY,    23,   BTN_NONE },
    { "lastunit",     "Jump to last occupied unit",      KIND_HOTKEY,    44,   BTN_NONE },
    { "lastmessage",  "Jump to last message sender",     KIND_HOTKEY,    31,   BTN_NONE },
    { "jump",         "Jump into vehicle",               KIND_HOTKEY,    4,    BTN_NONE },
    { "turret",       "Host station bridge to gun turret", KIND_HOTKEY,  1,    BTN_NONE },
    { "becomeleader", "Become current squad leader",     KIND_HOTKEY,    45,   BTN_NONE },
    { "beam",         "Beam host station",               KIND_HOTKEY,    7,    BTN_NONE },
    { "aggr1",        "Aggressiveness level 1",          KIND_HOTKEY,    38,   BTN_NONE },
    { "aggr2",        "Aggressiveness level 2",          KIND_HOTKEY,    39,   BTN_NONE },
    { "aggr3",        "Aggressiveness level 3",          KIND_HOTKEY,    40,   BTN_NONE },
    { "aggr4",        "Aggressiveness level 4",          KIND_HOTKEY,    41,   BTN_NONE },
    { "aggr5",        "Aggressiveness level 5",          KIND_HOTKEY,    42,   BTN_NONE },
    { "hud",          "HUD on/off",                      KIND_HOTKEY,    25,   BTN_NONE },
    { "infolog",      "Information log on/off",          KIND_HOTKEY,    27,   BTN_NONE },
    { "mapdetail",    "Map: sector detail",              KIND_HOTKEY,    10,   BTN_NONE },
    { "mapowner",     "Map: sector ownership",           KIND_HOTKEY,    11,   BTN_NONE },
    { "mapstatus",    "Map: vehicle status",             KIND_HOTKEY,    12,   BTN_NONE },
    { "mapsize",      "Map: toggle size",                KIND_HOTKEY,    18,   BTN_NONE },
    { "maplock",      "Map: lock on occupied unit",      KIND_HOTKEY,    14,   BTN_NONE },
    { "zoomin",       "Map: zoom in",                    KIND_HOTKEY,    16,   BTN_NONE },
    { "zoomout",      "Map: zoom out",                   KIND_HOTKEY,    17,   BTN_NONE },
    { "analyzer",     "Situation analyzer",              KIND_HOTKEY,    46,   BTN_NONE },
    { "help",         "Help",                            KIND_HOTKEY,    43,   BTN_NONE },
};

static const int kActionCount = (int)(sizeof(kActions) / sizeof(kActions[0]));
static_assert(sizeof(kActions) / sizeof(kActions[0]) <= 64, "Bindings holds 64 actions");

static const char *const kNames[BTN_COUNT] =
{
    "a", "b", "x", "y", "back", "guide", "start",
    "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright",
    "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
    "lefttrigger", "righttrigger",
};

static bool SameNoCase(const std::string &a, const char *b)
{
    size_t n = strlen(b);
    if ( a.size() != n )
        return false;
    for (size_t i = 0; i < n; i++)
        if ( tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]) )
            return false;
    return true;
}

int ActionCount()
{
    return kActionCount;
}

const Action &GetAction(int i)
{
    return kActions[(i >= 0 && i < kActionCount) ? i : 0];
}

int FindAction(const std::string &key)
{
    for (int i = 0; i < kActionCount; i++)
        if ( SameNoCase(key, kActions[i].key) )
            return i;
    return -1;
}

const char *ButtonName(int btn)
{
    if ( btn < 0 || btn >= BTN_COUNT )
        return "none";
    return kNames[btn];
}

int ButtonFromName(const std::string &name)
{
    if ( SameNoCase(name, "none") )
        return BTN_NONE;
    for (int i = 0; i < BTN_COUNT; i++)
        if ( SameNoCase(name, kNames[i]) )
            return i;
    return -2;
}

std::string ButtonLabel(int btn, Style style)
{
    struct L { const char *gen, *xbox, *ps4, *ps5; };
    static const L labels[BTN_COUNT] =
    {
        { "A / Cross",          "A",            "Cross",      "Cross" },
        { "B / Circle",         "B",            "Circle",     "Circle" },
        { "X / Square",         "X",            "Square",     "Square" },
        { "Y / Triangle",       "Y",            "Triangle",   "Triangle" },
        { "Back / Share",       "View",         "Share",      "Create" },
        { "Guide / PS",         "Xbox button",  "PS button",  "PS button" },
        { "Start / Options",    "Menu",         "Options",    "Options" },
        { "L3 (left stick)",    "Left stick click",  "L3",    "L3" },
        { "R3 (right stick)",   "Right stick click", "R3",    "R3" },
        { "L1 / LB",            "LB",           "L1",         "L1" },
        { "R1 / RB",            "RB",           "R1",         "R1" },
        { "D-pad up",           "D-pad up",     "D-pad up",   "D-pad up" },
        { "D-pad down",         "D-pad down",   "D-pad down", "D-pad down" },
        { "D-pad left",         "D-pad left",   "D-pad left", "D-pad left" },
        { "D-pad right",        "D-pad right",  "D-pad right","D-pad right" },
        { "Misc (Share / Mic)", "Share",        "Misc",       "Mic" },
        { "Paddle 1",           "Paddle P1",    "Paddle 1",   "Paddle 1" },
        { "Paddle 2",           "Paddle P2",    "Paddle 2",   "Paddle 2" },
        { "Paddle 3",           "Paddle P3",    "Paddle 3",   "Paddle 3" },
        { "Paddle 4",           "Paddle P4",    "Paddle 4",   "Paddle 4" },
        { "Touchpad click",     "Touchpad",     "Touchpad",   "Touchpad" },
        { "L2 / LT",            "LT",           "L2",         "L2" },
        { "R2 / RT",            "RT",           "R2",         "R2" },
    };
    if ( btn < 0 || btn >= BTN_COUNT )
        return "-";
    const L &l = labels[btn];
    switch ( style )
    {
    case STYLE_XBOX: return l.xbox;
    case STYLE_PS4:  return l.ps4;
    case STYLE_PS5:  return l.ps5;
    default:         return l.gen;
    }
}

void Bindings::Reset()
{
    for (int i = 0; i < 64; i++)
    {
        btn[i] = (i < kActionCount) ? (int8_t)kActions[i].def : (int8_t)BTN_NONE;
        explicitSet[i] = false;
    }
}

bool Bindings::IsDefault() const
{
    for (int i = 0; i < kActionCount; i++)
        if ( btn[i] != kActions[i].def )
            return false;
    return true;
}

int Bindings::ActionOn(int button) const
{
    if ( button < 0 || button >= BTN_COUNT )
        return -1;
    for (int i = 0; i < kActionCount; i++)
        if ( btn[i] == button )
            return i;
    return -1;
}

bool Bindings::operator==(const Bindings &o) const
{
    for (int i = 0; i < kActionCount; i++)
        if ( btn[i] != o.btn[i] )
            return false;
    return true;
}

BindResult Bind(Bindings &b, int action, int button)
{
    BindResult r;
    if ( action < 0 || action >= kActionCount || button < BTN_NONE || button >= BTN_COUNT )
        return r;
    const int old = b.btn[action];
    if ( old == button )
        return r;
    r.changed = true;
    if ( button != BTN_NONE )
    {
        const int other = b.ActionOn(button);
        if ( other >= 0 && other != action )
        {
            b.btn[other] = (int8_t)old;
            r.other = other;
            r.otherNow = old;
        }
    }
    b.btn[action] = (int8_t)button;
    return r;
}

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if ( a == std::string::npos )
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(a, e - a + 1);
}

bool ParseLine(Bindings &b, const std::string &keyIn, const std::string &valueIn, std::string *warn)
{
    const std::string key = Trim(keyIn);
    const size_t pl = strlen(kKeyPrefix);
    if ( key.size() < pl || !SameNoCase(key.substr(0, pl), kKeyPrefix) )
        return false;

    const int action = FindAction(key.substr(pl));
    // value: up to an inline comment
    std::string value = valueIn;
    size_t sc = value.find(';');
    if ( sc != std::string::npos )
        value = value.substr(0, sc);
    value = Trim(value);
    const int button = ButtonFromName(value);

    if ( action < 0 || button == -2 )
    {
        if ( warn )
            *warn = (action < 0) ? "unknown controller action " + key : "unknown controller button '" + value + "' for " + key;
        return true; // ours, ignored
    }

    if ( button != BTN_NONE )
    {
        for (int i = 0; i < kActionCount; i++)
        {
            if ( i != action && b.btn[i] == button )
            {
                b.btn[i] = BTN_NONE; // the later profile line wins; a default gives way
                if ( warn && b.explicitSet[i] )
                    *warn = std::string("controller button ") + ButtonName(button) + " set twice, kept for " + key;
            }
        }
    }
    b.btn[action] = (int8_t)button;
    b.explicitSet[action] = true;
    return true;
}

std::vector<std::string> ProfileLines(const Bindings &b)
{
    std::vector<std::string> l;
    for (int i = 0; i < kActionCount; i++)
        if ( b.btn[i] != kActions[i].def )
            l.push_back(std::string(kKeyPrefix) + kActions[i].key + " = " + ButtonName(b.btn[i]));
    return l;
}

Output Evaluate(const Bindings &b, const bool *pressed)
{
    Output o;
    bool hat[4] = { false, false, false, false };
    for (int i = 0; i < kActionCount; i++)
    {
        const int bt = b.btn[i];
        if ( bt < 0 || bt >= BTN_COUNT || !pressed[bt] )
            continue;
        const Action &a = kActions[i];
        switch ( a.kind )
        {
        case KIND_JOYBIT:    o.joyBits |= 1u << a.param; break;
        case KIND_THROTTLE:  o.throttle += a.param; break;
        case KIND_HAT:       hat[a.param] = true; break;
        case KIND_ESC:       o.esc = true; break;
        case KIND_PAUSE:     o.pause = true; break;
        case KIND_INPBUTTON: o.inpButtons |= 1u << a.param; break;
        case KIND_HOTKEY:    o.hotkeys |= (uint64_t)1 << a.param; break;
        }
    }
    // same precedence as the old D-pad code: up before down, right before left
    o.hatY = hat[0] ? 300 : (hat[1] ? -300 : 0);
    o.hatX = hat[3] ? 300 : (hat[2] ? -300 : 0);
    // lock-out rule: Start always opens the menu
    if ( pressed[BTN_START] )
        o.esc = true;
    return o;
}

Bindings &Active()
{
    static Bindings b;
    return b;
}

}
