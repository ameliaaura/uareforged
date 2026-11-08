// Controller settings: vertical aim signs (src/input_aim.h) and the in-mission profile
// patch (src/profile_patch.cpp). No game data, no SDL:
//   c++ -std=c++14 -Isrc tests/controller_settings_test.cpp src/profile_patch.cpp -o /tmp/cst && /tmp/cst
#include "input_aim.h"
#include "profile_patch.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if ( !(cond) ) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void TestAimSigns()
{
    // engine conventions: stick pushed up = negative slider 13; hat / right stick up = +1;
    // sliders 5 (gun) and 1 (nose) positive = up
    const float stickUp = -1.0f, stickDown = 1.0f, hatUp = 1.0f;

    // default (invert off): push up = aim up
    CHECK(InputAim::StickToGun(stickUp, false) > 0.0f);
    CHECK(InputAim::StickToGun(stickDown, false) < 0.0f);
    CHECK(InputAim::HatToGun(hatUp, false) > 0.0f);
    CHECK(InputAim::StickToPitch(stickUp, false) > 0.0f);
    CHECK(InputAim::StickToPitch(stickDown, false) < 0.0f);

    // invert on = the 1998 joystick mapping: Sliders[5] += Sliders[13] * 2, Sliders[1] += Sliders[13]
    CHECK(InputAim::StickToGun(stickUp, true) == stickUp * 2.0f);
    CHECK(InputAim::StickToGun(0.5f, true) == 1.0f);
    CHECK(InputAim::StickToPitch(stickUp, true) == stickUp);
    CHECK(InputAim::StickToPitch(0.25f, true) == 0.25f);
    CHECK(InputAim::HatToGun(hatUp, true) < 0.0f);

    // the stick gain stays the 1998 one (x2 for the tank gun, x1 for the nose)
    CHECK(InputAim::StickToGun(-0.5f, false) == 1.0f);
    CHECK(InputAim::StickToPitch(-0.5f, false) == 0.5f);
    CHECK(InputAim::HatToGun(0.3f, false) == 0.3f);

    // idle stays idle
    CHECK(InputAim::StickToGun(0.0f, false) == 0.0f && InputAim::StickToGun(0.0f, true) == 0.0f);
    CHECK(InputAim::StickToPitch(0.0f, false) == 0.0f && InputAim::HatToGun(0.0f, true) == 0.0f);
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
    "    moviehd = yes\n"
    "    input.hotkey[32] = p         ; Pause Game\n"
    "    input.slider[5] = ~#$z_#$a         ; Ground Unit Gun Down/Up\n"
    "end\n"
    "\n"
    "new_video\n"
    "    videomode = 1234\n"
    "    ; joystick = yes (a comment, not a key)\n"
    "end\n";

static std::vector<std::string> Lines(bool alt, bool invTank, bool invAir)
{
    return {
        "joystick = yes",
        std::string("altjoystick = ") + (alt ? "yes" : "no"),
        std::string("invertaim_tank = ") + (invTank ? "yes" : "no"),
        std::string("invertaim_air = ") + (invAir ? "yes" : "no"),
        "forcefeedback = yes",
        "enginerumble = no",
    };
}

static size_t Count(const std::string &s, const std::string &needle)
{
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1))
        n++;
    return n;
}

static void TestPatch()
{
    // missing keys are added before the input section's "end"; nothing else changes
    std::string t = kProfile;
    CHECK(ProfilePatch::PatchInputSection(t, Lines(true, false, false)));
    const std::string added = "    invertaim_tank = no\n    invertaim_air = no\nend\n\nnew_video";
    CHECK(t.find(added) != std::string::npos);
    CHECK(t.size() == std::string(kProfile).size() + std::string("    invertaim_tank = no\n    invertaim_air = no\n").size());
    CHECK(t.find("    input.hotkey[32] = p         ; Pause Game\n") != std::string::npos);
    CHECK(t.find("    videomode = 1234\n") != std::string::npos);

    // patching again with the same values changes nothing (byte-identical)
    std::string t2 = t;
    CHECK(ProfilePatch::PatchInputSection(t2, Lines(true, false, false)));
    CHECK(t2 == t);

    // changed values are rewritten in place, once each, indent kept
    CHECK(ProfilePatch::PatchInputSection(t2, Lines(false, true, true)));
    CHECK(t2.find("    altjoystick = no\n") != std::string::npos);
    CHECK(t2.find("    invertaim_tank = yes\n") != std::string::npos);
    CHECK(t2.find("    invertaim_air = yes\n") != std::string::npos);
    CHECK(Count(t2, "altjoystick") == 1 && Count(t2, "invertaim_tank") == 1 && Count(t2, "invertaim_air") == 1);
    CHECK(t2.size() == t.size() + 1); // yes->no, no->yes, no->yes
    // the video section's comment line is not a key
    CHECK(t2.find("    ; joystick = yes (a comment, not a key)\n") != std::string::npos);
    CHECK(Count(t2, "joystick = yes") == 2); // input line + the comment

    // CRLF profiles keep their line ends
    std::string crlf = "new_input\r\n  altjoystick = no\r\nend\r\n";
    CHECK(ProfilePatch::PatchInputSection(crlf, {"altjoystick = yes"}));
    CHECK(crlf == "new_input\r\n  altjoystick = yes\r\nend\r\n");

    // case-insensitive keys and section words, as the script parser reads them
    std::string mixed = "NEW_INPUT\n    AltJoystick = no\nEND\n";
    CHECK(ProfilePatch::PatchInputSection(mixed, {"altjoystick = yes"}));
    CHECK(mixed == "NEW_INPUT\n    altjoystick = yes\nEND\n");

    // no input section (or no "end") -> false, text untouched
    std::string none = "new_user\nend\n";
    CHECK(!ProfilePatch::PatchInputSection(none, Lines(true, true, true)));
    CHECK(none == "new_user\nend\n");
    std::string open = "new_input\n    joystick = yes\n";
    CHECK(!ProfilePatch::PatchInputSection(open, Lines(true, true, true)));
    CHECK(open == "new_input\n    joystick = yes\n");

    // a last line without a newline before "end" is impossible ("end" closes it), but an
    // unterminated final "end" is kept unterminated
    std::string tail = "new_input\n    joystick = yes\nend";
    CHECK(ProfilePatch::PatchInputSection(tail, {"invertaim_air = yes"}));
    CHECK(tail == "new_input\n    joystick = yes\n    invertaim_air = yes\nend");
}

static void TestAtomicWrite()
{
    const std::string path = "controller_settings_test.tmp.txt";
    CHECK(ProfilePatch::WriteFileAtomic(path, "first\n"));
    CHECK(ProfilePatch::WriteFileAtomic(path, "second\n"));
    FILE *f = std::fopen(path.c_str(), "rb");
    CHECK(f != NULL);
    if ( f )
    {
        char buf[32] = {0};
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        CHECK(std::string(buf, n) == "second\n");
    }
    std::remove(path.c_str());
    // a folder that does not exist: fails, nothing written
    CHECK(!ProfilePatch::WriteFileAtomic("no-such-folder-xyz/user.txt", "x"));
}

int main()
{
    TestAimSigns();
    TestPatch();
    TestAtomicWrite();
    if ( failures )
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("controller settings tests: all passed\n");
    return 0;
}
