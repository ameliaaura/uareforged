#ifndef PAD_REMAP_H_INCLUDED
#define PAD_REMAP_H_INCLUDED

#include <stdint.h>
#include <string>
#include <vector>

// Game controller button remapping (desktop): which pad button does which action.
// Pure logic, no SDL and no game headers, so tests/pad_remap_test.cpp builds it alone.
//
// Buttons use SDL's game controller button numbers (SDL_CONTROLLER_BUTTON_A = 0 ..
// SDL_CONTROLLER_BUTTON_TOUCHPAD = 20) plus the two triggers read as buttons
// (pressed past 8192 of 32767, as before). Names are SDL's mapping names
// ("a", "dpup", "righttrigger", ...); they are what a profile stores.
//
// Each action has at most one button and each button does at most one action;
// binding a button that another action uses swaps the two (the other action gets
// the old button of the one being changed, or none).
//
// Lock-out rule: Start/Options always opens the in-mission menu (Esc), whatever it is
// bound to, so a player can never lock themselves out of the menu.
//
// Profile (user.txt, new_input section): one line per action that differs from the
// default, "padbtn_<action> = <button name>" or "= none"; absent = default; unknown
// actions / button names are ignored.
namespace PadRemap
{

enum Button
{
    BTN_NONE = -1,
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START,
    BTN_LEFTSTICK, BTN_RIGHTSTICK, BTN_LEFTSHOULDER, BTN_RIGHTSHOULDER,
    BTN_DPUP, BTN_DPDOWN, BTN_DPLEFT, BTN_DPRIGHT,
    BTN_MISC1, BTN_PADDLE1, BTN_PADDLE2, BTN_PADDLE3, BTN_PADDLE4, BTN_TOUCHPAD,
    BTN_LEFTTRIGGER, BTN_RIGHTTRIGGER,
    BTN_COUNT
};

enum Kind
{
    KIND_JOYBIT,    // param = engine joystick button 0..7 (JOYB0..7: fire, MG, missile cam, brake, 4 free)
    KIND_THROTTLE,  // param = -300 forward / +300 back (momentary throttle, like R1/L1)
    KIND_HAT,       // param = 0 up, 1 down, 2 left, 3 right (gun aim; the right stick wins when used)
    KIND_ESC,       // the Esc key: in-mission menu
    KIND_PAUSE,     // the Pause key: single player pause
    KIND_INPBUTTON, // param = input button index held (4 = map waypoints)
    KIND_HOTKEY,    // param = game hotkey id (the 1998 key list), pressed once per press
};

struct Action
{
    const char *key;     // profile key suffix: padbtn_<key>
    const char *name;    // built-in English caption
    Kind kind;
    int param;
    Button def;          // default button = the hard-wired mapping of 0.8.2
};

enum Style
{
    STYLE_GENERIC = 0,   // "A / Cross"
    STYLE_XBOX,
    STYLE_PS4,
    STYLE_PS5,
};

int ActionCount();
const Action &GetAction(int i);
int FindAction(const std::string &key);        // case-insensitive, -1 = unknown

const char *ButtonName(int btn);                // SDL mapping name, "none" for BTN_NONE
int ButtonFromName(const std::string &name);    // case-insensitive; "none" -> BTN_NONE; unknown -> -2
std::string ButtonLabel(int btn, Style style);  // what the player sees

static const char *const kKeyPrefix = "padbtn_";

struct Bindings
{
    int8_t btn[64];        // per action (ActionCount() <= 64)
    bool explicitSet[64];  // set by a profile line since the last Reset (parse-time conflicts)

    Bindings() { Reset(); }
    void Reset();
    bool IsDefault() const;
    int ActionOn(int button) const;       // action using that button, -1 = none
    bool operator==(const Bindings &o) const;
    bool operator!=(const Bindings &o) const { return !(*this == o); }
};

struct BindResult
{
    bool changed = false;
    int other = -1;            // action that lost the button (-1 = none)
    int otherNow = BTN_NONE;   // its button now (the changed action's old one, or none)
};

// Give `action` the button `button` (BTN_NONE clears it); swaps on conflict.
BindResult Bind(Bindings &b, int action, int button);

// Profile line "padbtn_x = y": true when the key is one of ours (applied or safely
// ignored: unknown action or button name). A profile-set button taken from an action
// not set by the profile clears that one; between two profile lines the later wins.
bool ParseLine(Bindings &b, const std::string &key, const std::string &value, std::string *warn = NULL);

// "padbtn_<key> = <name>" for every action that differs from the default, table order.
std::vector<std::string> ProfileLines(const Bindings &b);

struct Output
{
    uint32_t joyBits = 0;      // engine JOYB0..7
    int throttle = 0;          // -300 forward / +300 back / 0
    int hatX = 0, hatY = 0;    // +-300 from buttons (hat up = +300)
    bool esc = false;          // the Esc key held (in-mission menu)
    bool pause = false;        // the Pause key held
    uint32_t inpButtons = 0;   // input buttons held (bit = index)
    uint64_t hotkeys = 0;      // hotkey ids held (bit = id, ids < 64)
};

// pressed[BTN_COUNT] = physical state this poll
Output Evaluate(const Bindings &b, const bool *pressed);

Bindings &Active();       // the live bindings (profile + page)

}

#endif
