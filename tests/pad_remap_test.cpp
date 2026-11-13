// Game controller button remapping (src/pad_remap.cpp) and its profile lines
// (src/profile_patch.cpp). No game data, no SDL:
//   c++ -std=c++14 -Wall -Isrc tests/pad_remap_test.cpp src/pad_remap.cpp src/profile_patch.cpp -o /tmp/prt && /tmp/prt
#include "pad_remap.h"
#include "profile_patch.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace PadRemap;

static int checks = 0, failures = 0;

#define CHECK(cond) do { checks++; if ( !(cond) ) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The hard-wired mapping of UA Reforged 0.8.2 (src/winp.cpp CheckGameController before
// the remapping), button for button. The default bindings must give exactly this.
static Output Legacy(const bool *p)
{
    Output o;
    if ( p[BTN_RIGHTSHOULDER] ) o.throttle -= 300;
    if ( p[BTN_LEFTSHOULDER] )  o.throttle += 300;
    if ( p[BTN_DPUP] )          o.hatY = 300;
    else if ( p[BTN_DPDOWN] )   o.hatY = -300;
    if ( p[BTN_DPRIGHT] )       o.hatX = 300;
    else if ( p[BTN_DPLEFT] )   o.hatX = -300;
    if ( p[BTN_RIGHTTRIGGER] )  o.joyBits |= 1 << 0;
    if ( p[BTN_LEFTTRIGGER] )   o.joyBits |= 1 << 1;
    if ( p[BTN_Y] )             o.joyBits |= 1 << 2;
    if ( p[BTN_A] )             o.joyBits |= 1 << 3;
    if ( p[BTN_B] )             o.joyBits |= 1 << 4;
    if ( p[BTN_X] )             o.joyBits |= 1 << 5;
    if ( p[BTN_LEFTSTICK] )     o.joyBits |= 1 << 6;
    if ( p[BTN_RIGHTSTICK] )    o.joyBits |= 1 << 7;
    o.esc = p[BTN_START];
    o.pause = p[BTN_BACK] || p[BTN_TOUCHPAD];
    return o;
}

static bool Same(const Output &a, const Output &b)
{
    return a.joyBits == b.joyBits && a.throttle == b.throttle && a.hatX == b.hatX && a.hatY == b.hatY
        && a.esc == b.esc && a.pause == b.pause && a.inpButtons == b.inpButtons && a.hotkeys == b.hotkeys;
}

static void Press(bool *p, std::initializer_list<int> btns)
{
    for (int i = 0; i < BTN_COUNT; i++)
        p[i] = false;
    for (int b : btns)
        p[b] = true;
}

static void TestTable()
{
    std::set<std::string> keys;
    std::set<int> defs;
    int withDefault = 0;
    for (int i = 0; i < ActionCount(); i++)
    {
        const Action &a = GetAction(i);
        CHECK(keys.insert(a.key).second);                  // unique profile keys
        CHECK(FindAction(a.key) == i);
        if ( a.def != BTN_NONE )
        {
            CHECK(defs.insert(a.def).second);              // no default button used twice
            withDefault++;
        }
        if ( a.kind == KIND_HOTKEY )
        {
            CHECK(a.param >= 0 && a.param < 64);
            CHECK(a.def == BTN_NONE);                      // new actions: no default button
            CHECK(a.param != 24 && a.param != 32 && a.param != 37); // quit / pause / chat are not hotkey rows
        }
    }
    CHECK(ActionCount() <= 64);
    CHECK(withDefault == 17); // triggers, Y, A, shoulders, 4 D-pad, Start, Back, touchpad, B, X, stick clicks
    for (int b = 0; b < BTN_COUNT; b++)
    {
        CHECK(ButtonFromName(ButtonName(b)) == b);
        CHECK(!ButtonLabel(b, STYLE_GENERIC).empty());
    }
    CHECK(ButtonFromName("none") == BTN_NONE);
    CHECK(ButtonFromName("RightTrigger") == BTN_RIGHTTRIGGER);
    CHECK(ButtonFromName("banana") == -2);
    CHECK(ButtonFromName("") == -2);
    CHECK(std::string(ButtonName(BTN_NONE)) == "none");
    CHECK(ButtonLabel(BTN_A, STYLE_GENERIC) == "A / Cross");
    CHECK(ButtonLabel(BTN_A, STYLE_XBOX) == "A");
    CHECK(ButtonLabel(BTN_A, STYLE_PS5) == "Cross");
    CHECK(ButtonLabel(BTN_BACK, STYLE_PS5) == "Create");
    CHECK(ButtonLabel(BTN_BACK, STYLE_PS4) == "Share");
    CHECK(ButtonLabel(BTN_BACK, STYLE_XBOX) == "View");
    CHECK(ButtonLabel(BTN_RIGHTTRIGGER, STYLE_PS5) == "R2");
    CHECK(ButtonLabel(BTN_NONE, STYLE_XBOX) == "-");
}

static void TestDefaultsEqualLegacy()
{
    Bindings b;
    CHECK(b.IsDefault());
    CHECK(ProfileLines(b).empty());
    bool p[BTN_COUNT];
    int n = 0;
    // nothing, every single button, every pair, every triple
    Press(p, {});
    CHECK(Same(Evaluate(b, p), Legacy(p)));
    for (int i = 0; i < BTN_COUNT; i++)
        for (int j = i; j < BTN_COUNT; j++)
            for (int k = j; k < BTN_COUNT; k++)
            {
                Press(p, {i, j, k});
                if ( !Same(Evaluate(b, p), Legacy(p)) )
                    n++;
            }
    CHECK(n == 0);
    // random chords
    srand(1998);
    n = 0;
    for (int t = 0; t < 50000; t++)
    {
        for (int i = 0; i < BTN_COUNT; i++)
            p[i] = (rand() & 3) == 0;
        if ( !Same(Evaluate(b, p), Legacy(p)) )
            n++;
    }
    CHECK(n == 0);
}

static void TestSwapAndLockout()
{
    Bindings b;
    const int fire = FindAction("fire"), brake = FindAction("brake"), map = FindAction("map"), menu = FindAction("menu");
    bool p[BTN_COUNT];

    // fire onto A (brake's): swapped, brake gets R2
    BindResult r = Bind(b, fire, BTN_A);
    CHECK(r.changed && r.other == brake && r.otherNow == BTN_RIGHTTRIGGER);
    CHECK(b.btn[fire] == BTN_A && b.btn[brake] == BTN_RIGHTTRIGGER);
    Press(p, {BTN_A});
    CHECK(Evaluate(b, p).joyBits == 1u);           // A fires now
    Press(p, {BTN_RIGHTTRIGGER});
    CHECK(Evaluate(b, p).joyBits == (1u << 3));    // R2 brakes, does not fire
    // the same again: nothing
    r = Bind(b, fire, BTN_A);
    CHECK(!r.changed && r.other == -1);

    // an unbound action takes a used button: the other one is cleared
    b.Reset();
    r = Bind(b, map, BTN_Y);
    CHECK(r.changed && r.other == FindAction("missilecam") && r.otherNow == BTN_NONE);
    Press(p, {BTN_Y});
    Output o = Evaluate(b, p);
    CHECK(o.joyBits == 0 && o.hotkeys == ((uint64_t)1 << 8));
    // a free button (Guide)
    r = Bind(b, FindAction("nextunit"), BTN_GUIDE);
    CHECK(r.changed && r.other == -1);
    Press(p, {BTN_GUIDE});
    CHECK(Evaluate(b, p).hotkeys == ((uint64_t)1 << 22));
    // clear
    r = Bind(b, map, BTN_NONE);
    CHECK(r.changed && b.btn[map] == BTN_NONE && b.ActionOn(BTN_Y) == -1);
    // waypoints = input button 4 held
    Bind(b, FindAction("waypoints"), BTN_PADDLE1);
    Press(p, {BTN_PADDLE1});
    CHECK(Evaluate(b, p).inpButtons == (1u << 4));
    // D-pad rebound: aim up from another button, D-pad up then does the new action
    b.Reset();
    Bind(b, FindAction("hud"), BTN_DPUP);
    CHECK(b.btn[FindAction("aimup")] == BTN_NONE);
    Press(p, {BTN_DPUP});
    o = Evaluate(b, p);
    CHECK(o.hatY == 0 && o.hotkeys == ((uint64_t)1 << 25));

    // lock-out: Start always opens the menu
    b.Reset();
    Bind(b, menu, BTN_Y);                          // menu moved to Y (Start loses it)
    CHECK(b.btn[menu] == BTN_Y);
    Press(p, {BTN_Y});
    CHECK(Evaluate(b, p).esc);
    Press(p, {BTN_START});
    CHECK(Evaluate(b, p).esc);                     // still
    Bind(b, map, BTN_START);                       // Start on the map
    Press(p, {BTN_START});
    o = Evaluate(b, p);
    CHECK(o.esc && o.hotkeys == ((uint64_t)1 << 8));
    Bind(b, menu, BTN_NONE);                       // menu cleared altogether
    Press(p, {BTN_START});
    CHECK(Evaluate(b, p).esc);
    Press(p, {BTN_Y});
    CHECK(!Evaluate(b, p).esc);
    // invalid input to Bind
    CHECK(!Bind(b, -1, BTN_A).changed && !Bind(b, 999, BTN_A).changed && !Bind(b, fire, 99).changed);
}

static Bindings FromLines(const std::vector<std::string> &lines)
{
    Bindings b;
    for (const std::string &l : lines)
    {
        const size_t eq = l.find('=');
        CHECK(ParseLine(b, l.substr(0, eq), l.substr(eq + 1)));
    }
    return b;
}

static void TestRoundTrip()
{
    srand(7);
    int bad = 0;
    for (int t = 0; t < 3000; t++)
    {
        Bindings b;
        const int steps = rand() % 12;
        for (int s = 0; s < steps; s++)
            Bind(b, rand() % ActionCount(), (rand() % (BTN_COUNT + 1)) - 1);
        // never two actions on one button
        for (int x = 0; x < BTN_COUNT; x++)
        {
            int n = 0;
            for (int a = 0; a < ActionCount(); a++)
                n += b.btn[a] == x;
            if ( n > 1 )
                bad++;
        }
        const std::vector<std::string> lines = ProfileLines(b);
        if ( FromLines(lines) != b )
            bad++;
        if ( b.IsDefault() != lines.empty() )
            bad++;
    }
    CHECK(bad == 0);

    Bindings b;
    Bind(b, FindAction("fire"), BTN_A);
    const std::vector<std::string> l = ProfileLines(b);
    CHECK(l.size() == 2);
    CHECK(l.size() == 2 && l[0] == "padbtn_fire = a" && l[1] == "padbtn_brake = righttrigger");
    Bind(b, FindAction("missilecam"), BTN_NONE);
    CHECK(ProfileLines(b).size() == 3 && ProfileLines(b)[1] == "padbtn_missilecam = none"); // table order
}

static void TestParse()
{
    Bindings b;
    std::string w;
    CHECK(!ParseLine(b, "joystick", "yes"));                 // not ours
    CHECK(!ParseLine(b, "input.hotkey[8]", "m"));
    CHECK(ParseLine(b, "padbtn_bogus", "a", &w) && !w.empty() && b.IsDefault());
    w.clear();
    CHECK(ParseLine(b, "padbtn_fire", "banana", &w) && !w.empty() && b.IsDefault());
    CHECK(ParseLine(b, "padbtn_fire", "", &w) && b.IsDefault());
    CHECK(ParseLine(b, "padbtn_", "a") && b.IsDefault());
    CHECK(ParseLine(b, "padbtn_fire", "   ; a comment only") && b.IsDefault());
    CHECK(ParseLine(b, "PADBTN_Fire", "  X  ; old comment\r\n"));
    CHECK(b.btn[FindAction("fire")] == BTN_X);
    CHECK(b.btn[FindAction("joyb5")] == BTN_NONE);           // default gave way

    // a profile line takes the button from an action at its default
    b.Reset();
    ParseLine(b, "padbtn_map", "a");
    CHECK(b.btn[FindAction("map")] == BTN_A && b.btn[FindAction("brake")] == BTN_NONE);
    // two profile lines on one button: the later wins
    b.Reset();
    ParseLine(b, "padbtn_fire", "guide");
    ParseLine(b, "padbtn_map", "guide", &w);
    CHECK(b.btn[FindAction("map")] == BTN_GUIDE && b.btn[FindAction("fire")] == BTN_NONE);
    // "none"
    b.Reset();
    ParseLine(b, "padbtn_menu", "none");
    CHECK(b.btn[FindAction("menu")] == BTN_NONE);
    // Reset = defaults again
    b.Reset();
    CHECK(b.IsDefault());
}

static const char *kProfile =
    "new_user\n"
    "    maxroboenergy = 100\n"
    "end\n"
    "\n"
    "new_input\n"
    "    joystick = yes\n"
    "    altjoystick = yes\n"
    "    forcefeedback = yes\n"
    "    enginerumble = no\n"
    "    input.hotkey[32] = p         ; Pause Game\n"
    "end\n"
    "\n"
    "new_video\n"
    "    padbtn_fire = a\n"           // not the input section: never touched
    "end\n";

static std::vector<std::string> Fresh(const Bindings &b)
{
    std::vector<std::string> l = { "joystick = yes", "altjoystick = yes", "forcefeedback = yes", "enginerumble = no" };
    for (const std::string &x : ProfileLines(b))
        l.push_back(x);
    return l;
}

static void TestProfilePatch()
{
    // defaults: no padbtn lines, profile byte-identical
    std::string t = kProfile;
    Bindings b;
    CHECK(ProfilePatch::PatchInputSection(t, Fresh(b), kKeyPrefix));
    CHECK(t == kProfile);

    // a change: the lines are added before the section's end, everything else as it was
    Bind(b, FindAction("fire"), BTN_A);
    CHECK(ProfilePatch::PatchInputSection(t, Fresh(b), kKeyPrefix));
    const std::string expect = std::string(kProfile).replace(std::string(kProfile).find("end\n\nnew_video"), 0,
        "    padbtn_fire = a\n    padbtn_brake = righttrigger\n");
    CHECK(t == expect);
    // the same again: byte-identical
    std::string t2 = t;
    CHECK(ProfilePatch::PatchInputSection(t2, Fresh(b), kKeyPrefix));
    CHECK(t2 == t);

    // one binding changed: rewritten in place, the other untouched
    Bind(b, FindAction("fire"), BTN_RIGHTTRIGGER); // swap back -> brake = A again, fire default
    Bind(b, FindAction("map"), BTN_GUIDE);
    CHECK(ProfilePatch::PatchInputSection(t2, Fresh(b), kKeyPrefix));
    CHECK(t2.find("padbtn_fire") != std::string::npos);  // the new_video one only
    CHECK(t2.find("padbtn_brake") == std::string::npos);
    CHECK(t2.find("    padbtn_map = guide\n") != std::string::npos);

    // back to the defaults: the padbtn lines go, the profile is the original again
    b.Reset();
    CHECK(ProfilePatch::PatchInputSection(t2, Fresh(b), kKeyPrefix));
    CHECK(t2 == kProfile);

    // CRLF profile: line ends kept, a stale padbtn line removed, unknown padbtn line removed too
    std::string crlf = "new_input\r\n    joystick = yes\r\n    padbtn_fire = a\r\n    padbtn_bogus = x\r\nend\r\n";
    CHECK(ProfilePatch::PatchInputSection(crlf, { "joystick = yes" }, kKeyPrefix));
    CHECK(crlf == "new_input\r\n    joystick = yes\r\nend\r\n");

    // without a drop prefix (the old call): padbtn lines are kept as they are
    std::string keep = "new_input\n    padbtn_fire = a\nend\n";
    CHECK(ProfilePatch::PatchInputSection(keep, { "joystick = yes" }));
    CHECK(keep == "new_input\n    padbtn_fire = a\n    joystick = yes\nend\n");

    // parse what was written: same bindings
    Bindings w;
    Bind(w, FindAction("menu"), BTN_Y);
    Bind(w, FindAction("map"), BTN_START);
    std::string t3 = kProfile;
    CHECK(ProfilePatch::PatchInputSection(t3, Fresh(w), kKeyPrefix));
    Bindings r;
    size_t at = t3.find("new_input"), end = t3.find("\nend", at);
    std::string sec = t3.substr(at, end - at);
    size_t pos = 0;
    while ( (pos = sec.find("padbtn_", pos)) != std::string::npos )
    {
        size_t eol = sec.find('\n', pos);
        std::string line = sec.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        size_t eq = line.find('=');
        ParseLine(r, line.substr(0, eq), line.substr(eq + 1));
        pos = eol == std::string::npos ? sec.size() : eol;
    }
    CHECK(r == w);
}

int main()
{
    TestTable();
    TestDefaultsEqualLegacy();
    TestSwapAndLockout();
    TestRoundTrip();
    TestParse();
    TestProfilePatch();
    std::printf("pad_remap_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
