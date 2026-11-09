// Unit tests for the anonymous usage stats (src/usage_stats*.cpp), no game data needed.
//   c++ -std=c++14 -Wall -DUA_TEST_HOOKS -Isrc tests/usage_stats_test.cpp src/usage_stats.cpp \
//       src/usage_stats_platform.cpp src/usage_stats_crash.cpp [src/usage_stats_net_apple.mm -framework Foundation] -o t
// Covers: the state file and the opt-in state machine, every event body against docs/usage-stats.md (v3), the
// queue, the name clean-up, the perf maths, the hardware / input / bug-reference helpers, the address check, the
// runtime end to end with a fake sender (nothing sent before opt-in / after opt-out, offline queue, crash file
// pick-up, missions with level_start / level_end, the session file and session_end, bug reports on and off), the
// per-frame calls (no allocation, cost per frame) and, on POSIX, a real crash and a killed session in child
// processes.
#include "usage_stats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace UsageStats;

// every allocation in this process is counted (the per-frame calls must make none)
static std::atomic<long> g_allocs{0};
void *operator new(size_t n)
{
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if ( void *p = malloc(n ? n : 1) ) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *kId = "3f9c2a7d0b5e4c18a6d2f0e9b7c41a55";

static bool RndA(uint8_t o[16]) { for (int i = 0; i < 16; i++) o[i] = (uint8_t)(0x10 + i); return true; }
static int g_rndN = 0;
static bool RndCount(uint8_t o[16]) { g_rndN++; for (int i = 0; i < 16; i++) o[i] = (uint8_t)(g_rndN * 7 + i); return true; }
static bool RndNone(uint8_t[16]) { return false; }

static Record Rec(std::initializer_list<std::pair<std::string, std::string>> kv)
{
    Record r;
    for (auto &p : kv) r.Set(p.first, p.second);
    return r;
}

static std::string ReadAll(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static void WriteAll(const std::string &p, const std::string &t)
{
    FILE *f = fopen(p.c_str(), "wb");
    if (f) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
}

static bool Exists(const std::string &p)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

static void TestState()
{
    State s = ParseState("");
    CHECK(s.answer == ANSWER_UNASKED && !Sending(s));
    s = ParseState("garbage\nanswer = maybe\n");
    CHECK(s.answer == ANSWER_UNASKED);
    s = ParseState(std::string("answer = yes\ninstall = ") + kId + "\n");
    CHECK(Sending(s) && s.install == kId);
    s = ParseState(std::string("answer = no\ninstall = ") + kId + "\n");
    CHECK(s.answer == ANSWER_NO && s.install.empty());            // a "no" never keeps a number
    CHECK(FormatState(s).find("install") == std::string::npos);
    s = ParseState("answer = yes\ninstall = 3F9C\n");               // bad number
    CHECK(s.answer == ANSWER_YES && s.install.empty() && !Sending(s));
    CHECK(EnsureInstallId(s, RndA) && Sending(s) && s.install == "101112131415161718191a1b1c1d1e1f");
    CHECK(!EnsureInstallId(s, RndA));                               // already has one
    State back = ParseState(FormatState(s));
    CHECK(back.answer == ANSWER_YES && back.install == s.install);
    State z = ParseState("answer = yes\n");
    CHECK(EnsureInstallId(z, RndNone) && z.answer == ANSWER_NO);   // no randomness: off

    // the state machine
    State m;
    g_rndN = 0;
    CHECK(ApplyAnswer(m, false, RndCount) && m.answer == ANSWER_NO && m.install.empty() && !Sending(m));
    CHECK(g_rndN == 0);                                             // "no" makes no number
    CHECK(SetEnabled(m, true, RndCount) && Sending(m));
    const std::string first = m.install;
    CHECK(SetEnabled(m, false, RndCount) && !Sending(m) && m.install.empty() && m.answer == ANSWER_NO);
    CHECK(!SetEnabled(m, false, RndCount));                         // off again: no change
    CHECK(SetEnabled(m, true, RndCount) && Sending(m) && m.install != first); // on again: a NEW number
    State n;
    CHECK(ApplyAnswer(n, true, RndCount) && Sending(n));
    State q;
    CHECK(!SetEnabled(q, true, RndNone) || q.answer == ANSWER_NO);  // no randomness: never on
    CHECK(!Sending(q));
    CHECK(ValidInstallId(kId) && !ValidInstallId("3F9C2A7D0B5E4C18A6D2F0E9B7C41A55") && !ValidInstallId(""));
}

static void TestBodies()
{
    Common c{"desktop", "mac", "0.8.1"};
    std::string b = BuildBody(kId, c, Rec({{"event", "launch"}, {"preset", "high"}, {"os", "macOS 15.2"}, {"gpu", "Apple M2"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"launch\",\"preset\":\"high\",\"os\":\"macOS 15.2\",\"gpu\":\"Apple M2\"}");
    b = BuildBody(kId, Common{"vr", "quest", "0.9.1"}, Rec({{"event", "level_start"}, {"preset", "high"}, {"level", "7"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"vr\",\"platform\":\"quest\",\"version\":\"0.9.1\",\"event\":\"level_start\",\"preset\":\"high\",\"level\":7}");
    b = BuildBody(kId, Common{"desktop", "windows", "0.8.1"},
                  Rec({{"event", "perf"}, {"preset", "medium"}, {"level", "7"}, {"res", "1920x1080"}, {"fps_avg", "118.4"},
                       {"fps_p1low", "61.2"}, {"frame_ms_p95", "11.3"}, {"duration_s", "842"}, {"hitches", "3"}}));
    CHECK(b.find("\"fps_avg\":118.4,\"fps_p1low\":61.2,\"frame_ms_p95\":11.3,\"duration_s\":842,\"hitches\":3}") != std::string::npos);
    CHECK(b.find("\"res\":\"1920x1080\"") != std::string::npos);
    b = BuildBody(kId, c, Rec({{"event", "crash"}, {"preset", "high"}, {"level", "12"}, {"build", "0b8d9f6345e4"}, {"signal", "SIGSEGV"},
                               {"module", "UA_source"}, {"offset", "0x1a2f40"}, {"frames", "UA_source+0x1a2f40,libdyld.dylib+0x3f10"}, {"minutes", "23"}}));
    CHECK(b.find("\"frames\":[\"UA_source+0x1a2f40\",\"libdyld.dylib+0x3f10\"],\"minutes\":23}") != std::string::npos);
    b = BuildBody(kId, c, Rec({{"event", "crash"}, {"preset", "high"}, {"build", "dev"}, {"signal", "EXC_C0000005"},
                               {"module", "unknown"}, {"offset", "0x0"}, {"frames", ""}, {"minutes", "0"}}));
    CHECK(b.find("\"frames\":[]") != std::string::npos);
    b = BuildBody(kId, c, Rec({{"event", "error"}, {"code", "pack_checksum_mismatch"}, {"detail", "2"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"error\",\"code\":\"pack_checksum_mismatch\",\"detail\":2}");
    // a queued event keeps the version it was made with
    b = BuildBody(kId, c, Rec({{"event", "level_start"}, {"version", "0.8.0"}, {"preset", "low"}, {"level", "1"}}));
    CHECK(b.find("\"version\":\"0.8.0\"") != std::string::npos);

    // every rule refuses
    CHECK(BuildBody("nope", c, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "1"}})).empty());
    CHECK(BuildBody(kId, Common{"phone", "mac", "1"}, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "1"}})).empty());
    CHECK(BuildBody(kId, Common{"desktop", "linux", "1"}, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "1"}})).empty());
    CHECK(BuildBody(kId, Common{"desktop", "mac", "0.8.1 beta"}, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "1"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "vr"}, {"level", "1"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "10000"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "low"}})).empty());              // missing level
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "low"}, {"level", "1"}, {"user", "x"}})).empty()); // unknown key
    CHECK(BuildBody(kId, c, Rec({{"event", "hello"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "launch"}, {"preset", "high"}, {"os", "macOS 15.2"}, {"gpu", "Apple \"M2\""}})).empty()); // no free text
    CHECK(BuildBody(kId, c, Rec({{"event", "launch"}, {"preset", "high"}, {"os", "dir/x"}, {"gpu", "Apple M2"}})).empty()); // no paths
    CHECK(BuildBody(kId, c, Rec({{"event", "error"}, {"code", "something_else"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "error"}, {"code", "game_folder_invalid"}, {"detail", "-1"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "error"}, {"code", "game_folder_invalid"}, {"detail", "hello"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "perf"}, {"preset", "medium"}, {"level", "7"}, {"res", "1920x1080"}, {"fps_avg", "1e3"},
                                 {"fps_p1low", "61.2"}, {"frame_ms_p95", "11.3"}, {"duration_s", "842"}, {"hitches", "3"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "perf"}, {"preset", "medium"}, {"level", "7"}, {"res", "1920*1080"}, {"fps_avg", "1"},
                                 {"fps_p1low", "1"}, {"frame_ms_p95", "1"}, {"duration_s", "1"}, {"hitches", "1"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "crash"}, {"preset", "high"}, {"build", "dev"}, {"signal", "SIGSEGV"}, {"module", "dir/x/UA_source"},
                                 {"offset", "0x1"}, {"frames", ""}, {"minutes", "0"}})).empty());     // a path as a module
    std::string many;
    for (int i = 0; i < 33; i++) many += std::string(i ? "," : "") + "UA_source+0x1";
    CHECK(BuildBody(kId, c, Rec({{"event", "crash"}, {"preset", "high"}, {"build", "dev"}, {"signal", "SIGSEGV"}, {"module", "UA_source"},
                                 {"offset", "0x1"}, {"frames", many}, {"minutes", "0"}})).empty());  // > 32 frames
    // the biggest possible crash body stays under 8 KB, the biggest other bodies under 4 KB
    std::string big, mod(64, 'm');
    for (int i = 0; i < 32; i++) big += std::string(i ? "," : "") + mod + "+0xffffffffffffffff";
    b = BuildBody(kId, c, Rec({{"event", "crash"}, {"preset", "original"}, {"level", "9999"}, {"build", std::string(40, 'b').c_str()},
                               {"signal", "EXC_C0000005"}, {"module", mod.c_str()}, {"offset", "0xffffffffffffffff"}, {"frames", big.c_str()},
                               {"minutes", "100000"}}));
    CHECK(!b.empty() && b.size() <= 8192);
    CHECK(MaxBody("crash") == 8192 && MaxBody("perf") == 4096);

    // ---- v3: launch hardware
    b = BuildBody(kId, c, Rec({{"event", "launch"}, {"preset", "high"}, {"os", "macOS 15.2"}, {"gpu", "Apple M2"}, {"ram_gb", "16"},
                               {"cpu_cores", "8"}, {"refresh_hz", "60"}, {"display", "1080p"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"launch\",\"preset\":\"high\",\"os\":\"macOS 15.2\",\"gpu\":\"Apple M2\",\"ram_gb\":16,\"cpu_cores\":8,\"refresh_hz\":60,\"display\":\"1080p\"}");
    Record L = Rec({{"event", "launch"}, {"preset", "high"}, {"os", "macOS 15.2"}, {"gpu", "Apple M2"}});
    for (const char *bad : {"5", "128", "16.0", "-16", "x"}) { Record t = L; t.Set("ram_gb", bad); CHECK(BuildBody(kId, c, t).empty()); }
    for (const char *bad : {"3", "64", "0"}) { Record t = L; t.Set("cpu_cores", bad); CHECK(BuildBody(kId, c, t).empty()); }
    for (const char *bad : {"0", "501", "59.9"}) { Record t = L; t.Set("refresh_hz", bad); CHECK(BuildBody(kId, c, t).empty()); }
    for (const char *bad : {"8k", "1920x1080", "", "4K"}) { Record t = L; t.Set("display", bad); CHECK(BuildBody(kId, c, t).empty()); }
    for (const char *ok : {"2", "4", "8", "32", "64"}) { Record t = L; t.Set("ram_gb", ok); CHECK(!BuildBody(kId, c, t).empty()); }
    for (const char *ok : {"sd", "720p", "1440p", "4k", "5k+"}) { Record t = L; t.Set("display", ok); CHECK(!BuildBody(kId, c, t).empty()); }
    // ---- v3: level_start loading time
    b = BuildBody(kId, Common{"vr", "quest", "0.9.1"}, Rec({{"event", "level_start"}, {"preset", "high"}, {"level", "7"}, {"load_ms", "4210"}}));
    CHECK(b.find("\"level\":7,\"load_ms\":4210}") != std::string::npos);
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "high"}, {"level", "7"}, {"load_ms", "600001"}})).empty());
    CHECK(BuildBody(kId, c, Rec({{"event", "level_start"}, {"preset", "high"}, {"level", "7"}, {"load_ms", "-1"}})).empty());
    // ---- v3: level_end
    b = BuildBody(kId, Common{"desktop", "windows", "0.8.1"}, Rec({{"event", "level_end"}, {"preset", "medium"}, {"level", "7"}, {"result", "won"},
                                                                    {"duration_s", "911"}, {"input", "controller"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"windows\",\"version\":\"0.8.1\",\"event\":\"level_end\",\"preset\":\"medium\",\"level\":7,\"result\":\"won\",\"duration_s\":911,\"input\":\"controller\"}");
    b = BuildBody(kId, Common{"vr", "quest", "0.9.1"}, Rec({{"event", "level_end"}, {"preset", "high"}, {"level", "1"}, {"result", "lost"}, {"duration_s", "640"},
                                                           {"input", "vr_controllers"}, {"vr_table_s", "402"}, {"vr_unit_s", "198"}, {"vr_jumps", "6"}}));
    CHECK(b.find("\"result\":\"lost\",\"duration_s\":640,\"input\":\"vr_controllers\",\"vr_table_s\":402,\"vr_unit_s\":198,\"vr_jumps\":6}") != std::string::npos);
    Record E = Rec({{"event", "level_end"}, {"preset", "high"}, {"level", "1"}, {"result", "quit"}, {"duration_s", "10"}});
    CHECK(!BuildBody(kId, c, E).empty());
    for (const char *ok : {"won", "lost", "quit", "restart"}) { Record t = E; t.Set("result", ok); CHECK(!BuildBody(kId, c, t).empty()); }
    for (const char *bad : {"draw", "WON", "", "won "}) { Record t = E; t.Set("result", bad); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = E; t.Set("duration_s", "86401"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = E; t.Set("input", "joystick"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = E; t.Set("vr_jumps", "100001"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = E; t.Set("vr_table_s", "86401"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = E; t.Set("note", "it broke"); CHECK(BuildBody(kId, c, t).empty()); }   // no free text, ever
    { Record t = Rec({{"event", "level_end"}, {"preset", "high"}, {"level", "1"}, {"duration_s", "10"}}); CHECK(BuildBody(kId, c, t).empty()); } // no result
    // ---- v3: session_end
    b = BuildBody(kId, c, Rec({{"event", "session_end"}, {"preset", "high"}, {"minutes", "47"}, {"levels", "3"}, {"ended", "quit"},
                               {"input", "keyboard_mouse"}, {"look_switches", "2"}, {"preset_changes", "3"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"session_end\",\"preset\":\"high\",\"minutes\":47,\"levels\":3,\"ended\":\"quit\",\"input\":\"keyboard_mouse\",\"look_switches\":2,\"preset_changes\":3}");
    Record S = Rec({{"event", "session_end"}, {"preset", "high"}, {"minutes", "0"}, {"levels", "0"}, {"ended", "killed"}, {"look_switches", "0"}, {"preset_changes", "0"}});
    CHECK(!BuildBody(kId, c, S).empty());
    for (const char *bad : {"closed", "", "Quit"}) { Record t = S; t.Set("ended", bad); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = S; t.Set("minutes", "100001"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = S; t.Set("look_switches", "1001"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = Rec({{"event", "session_end"}, {"preset", "high"}, {"minutes", "1"}, {"levels", "0"}, {"ended", "quit"}, {"preset_changes", "0"}}); CHECK(BuildBody(kId, c, t).empty()); }
    // ---- v3: bug_report
    b = BuildBody(kId, c, Rec({{"event", "bug_report"}, {"ref", "k7m2q9xa"}, {"preset", "high"}, {"level", "7"}, {"minutes", "32"}, {"input", "keyboard_mouse"},
                               {"errors", "shader_load_failed"}, {"crash_signal", "SIGSEGV"}, {"crash_frames", "UA_source+0x1a2f40,UA_source+0x1a3b18"},
                               {"fps_avg", "58.2"}, {"fps_p1low", "31.0"}}));
    CHECK(b == std::string("{\"v\":3,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"bug_report\",\"ref\":\"k7m2q9xa\",\"preset\":\"high\",\"level\":7,\"minutes\":32,\"input\":\"keyboard_mouse\",\"errors\":[\"shader_load_failed\"],\"crash_signal\":\"SIGSEGV\",\"crash_frames\":[\"UA_source+0x1a2f40\",\"UA_source+0x1a3b18\"],\"fps_avg\":58.2,\"fps_p1low\":31.0}");
    Record B = Rec({{"event", "bug_report"}, {"ref", "abcd2345"}, {"preset", "original"}, {"minutes", "0"}});
    CHECK(!BuildBody(kId, c, B).empty());
    for (const char *bad : {"abc", "ABCD2345", "abcd23456", "abcd-345", "abcd 345", ""}) { Record t = B; t.Set("ref", bad); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = B; t.Set("errors", "shader_load_failed,not_a_code"); CHECK(BuildBody(kId, c, t).empty()); }
    { std::string e11; for (int i = 0; i < 11; i++) e11 += std::string(i ? "," : "") + "vr_session_lost";
      Record t = B; t.Set("errors", e11); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = B; t.Set("errors", ""); CHECK(BuildBody(kId, c, t).find("\"errors\":[]") != std::string::npos); }
    { Record t = B; t.Set("crash_frames", "a+0x1,b+0x2,c+0x3,d+0x4,e+0x5,f+0x6"); CHECK(BuildBody(kId, c, t).empty()); } // > 5 frames
    { Record t = B; t.Set("crash_frames", ""); CHECK(BuildBody(kId, c, t).empty()); }                                    // present = at least 1
    { Record t = B; t.Set("crash_frames", "dir/x+0x1"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = B; t.Set("crash_signal", "segv"); CHECK(BuildBody(kId, c, t).empty()); }
    { Record t = B; t.Set("message", "my note"); CHECK(BuildBody(kId, c, t).empty()); }  // the player's note never goes here
    { Record t = B; t.Set("fps_avg", "1000.1"); CHECK(BuildBody(kId, c, t).empty()); }
    // the biggest bug_report stays under 4 KB
    { std::string e10; for (int i = 0; i < 10; i++) e10 += std::string(i ? "," : "") + "pack_checksum_mismatch";
      std::string f5; for (int i = 0; i < 5; i++) f5 += std::string(i ? "," : "") + mod + "+0xffffffffffffffff";
      Record t = B; t.Set("level", "9999"); t.Set("input", "keyboard_mouse"); t.Set("errors", e10); t.Set("crash_signal", "EXC_C0000005");
      t.Set("crash_frames", f5); t.Set("fps_avg", "1000.0"); t.Set("fps_p1low", "1000.0"); t.Set("minutes", "100000");
      b = BuildBody(kId, c, t); CHECK(!b.empty() && b.size() <= 4096); }
}

static void TestHelpers()
{
    CHECK(BucketRamGb(16384) == 16 && BucketRamGb(16180) == 16 && BucketRamGb(15800) == 16); // reported a bit under 16 GB
    CHECK(BucketRamGb(8192) == 8 && BucketRamGb(11264) == 8 && BucketRamGb(24576) == 16);
    CHECK(BucketRamGb(3900) == 4 && BucketRamGb(2048) == 2 && BucketRamGb(512) == 2 && BucketRamGb(0) == 0 && BucketRamGb(-1) == 0);
    CHECK(BucketRamGb(65536) == 64 && BucketRamGb(262144) == 64 && BucketRamGb(36864) == 32);
    CHECK(BucketCores(1) == 1 && BucketCores(3) == 2 && BucketCores(10) == 8 && BucketCores(12) == 12 && BucketCores(14) == 12);
    CHECK(BucketCores(64) == 32 && BucketCores(0) == 0 && BucketCores(-4) == 0 && BucketCores(7) == 6);
    CHECK(!strcmp(DisplayBucket(1920, 1080), "1080p") && !strcmp(DisplayBucket(1512, 982), "720p") && !strcmp(DisplayBucket(640, 480), "sd"));
    CHECK(!strcmp(DisplayBucket(2560, 1440), "1440p") && !strcmp(DisplayBucket(3840, 2160), "4k") && !strcmp(DisplayBucket(5120, 2880), "5k+"));
    CHECK(!strcmp(DisplayBucket(0, 0), ""));
    CHECK(!strcmp(InputKind(0, 0, false), ""));
    CHECK(!strcmp(InputKind(50, 0, false), "keyboard_mouse") && !strcmp(InputKind(0, 9, false), "controller"));
    CHECK(!strcmp(InputKind(100, 9, false), "keyboard_mouse"));  // a stray pad press doesn't make it mixed
    CHECK(!strcmp(InputKind(100, 10, false), "mixed") && !strcmp(InputKind(10, 100, false), "mixed"));
    CHECK(!strcmp(InputKind(3, 200, false), "controller"));
    CHECK(!strcmp(InputKind(0, 0, true), "vr_controllers") && !strcmp(InputKind(7, 0, true), "vr_controllers"));
    uint8_t z[16] = {0}, f[16];
    for (int i = 0; i < 16; i++) f[i] = 0xff;
    CHECK(MakeBugRef(z) == "aaaaaaaa" && ValidBugRef(MakeBugRef(f)));
    uint8_t r[16];
    for (int i = 0; i < 16; i++) r[i] = (uint8_t)(i * 37 + 11);
    const std::string ref = MakeBugRef(r);
    CHECK(ref.size() == 8 && ValidBugRef(ref) && ref.find_first_of("ilo01") == std::string::npos);
    CHECK(ref != MakeBugRef(z));
    CHECK(BugReportUrl("k7m2q9xa") == "https://uareforged.pages.dev/contribute/?ref=k7m2q9xa#feedback");
    CHECK(BugReportUrl("bad ref") == "https://uareforged.pages.dev/contribute/#feedback");
    CHECK(!ValidBugRef("K7M2Q9XA") && !ValidBugRef("k7m2q9x") && !ValidBugRef("k7m2q9x/"));
}

static void TestQueueAndNames()
{
    Record r = Rec({{"event", "launch"}, {"preset", "high"}, {"os", "Windows 10 22H2"}, {"gpu", "NVIDIA GeForce RTX 3070"}});
    Record back;
    CHECK(ParseRecordLine(FormatRecordLine(r), &back) && back.Get("os") == "Windows 10 22H2" && back.f.size() == 4);
    CHECK(ParseRecordLine("event=error\tcode=pack_download_failed\tdetail=22", &back) && back.Get("detail") == "22");
    CHECK(!ParseRecordLine("event=nonsense\tx=1", &back));
    CHECK(!ParseRecordLine("not a record", &back));
    std::string text;
    for (int i = 0; i < 25; i++) text += "event=level_start\tpreset=low\tlevel=" + std::to_string(i) + "\n";
    text += "junk line\n";
    std::vector<Record> q = ParseQueue(text, 20);
    CHECK(q.size() == 20 && q.front().Get("level") == "5" && q.back().Get("level") == "24"); // newest 20 kept
    CHECK(ParseQueue(FormatQueue(q, 20), 20).size() == 20);
    CHECK(ValidErrorCode("vr_session_lost") && !ValidErrorCode("VR_SESSION_LOST"));

    CHECK(CleanGpu("NVIDIA GeForce RTX 3070/PCIe/SSE2") == "NVIDIA GeForce RTX 3070");
    CHECK(CleanGpu("Adreno (TM) 740") == "Adreno 740");
    CHECK(CleanGpu("Apple M2") == "Apple M2");
    CHECK(CleanGpu("AMD Radeon RX 6800 XT (radeonsi, navi21, LLVM 15.0.7, DRM 3.49)") == "AMD Radeon RX 6800 XT");
    CHECK(CleanGpu("Intel(R) UHD Graphics 620") == "Intel UHD Graphics 620");
    CHECK(CleanGpu("") == "unknown");
    CHECK(CleanGpu("\"; DROP") == "DROP");
    CHECK(CleanOs("macOS 15.2") == "macOS 15.2");
    CHECK(CleanOs("Windows 11 23H2") == "Windows 11 23H2");
    CHECK(CleanOs(std::string(80, 'x')).size() == 40);

    CHECK(UrlAllowed("https://uareforged.pages.dev/api/ping"));
    CHECK(UrlAllowed("http://127.0.0.1:8765/api/ping"));
    CHECK(UrlAllowed("http://localhost:8765/api/ping"));
    CHECK(!UrlAllowed("http://uareforged.pages.dev/api/ping"));
    CHECK(!UrlAllowed("https://example.com/api/ping"));
    CHECK(!UrlAllowed("http://127.0.0.1/api/ping"));              // a port is required
    CHECK(!UrlAllowed("http://127.0.0.1:80@evil.example/api"));
    CHECK(!UrlAllowed("http://127.0.0.1.evil.example:80/api"));
}

static void TestPerf()
{
    PerfHist h;
    for (int i = 0; i < 990; i++) h.Add(10.0);
    for (int i = 0; i < 10; i++) h.Add(120.0);
    CHECK(h.Frames() == 1000 && h.Hitches() == 10);
    CHECK(h.Percentile(95) == 10.25);                // the 0.25 ms bin's upper edge
    CHECK(h.Percentile(99) == 10.25);                // frame 990 of 1000
    CHECK(h.Percentile(99.5) == 120.25);
    const double avg = h.FpsAvg();                   // 1000 frames / 11.1 s
    CHECK(avg > 90.08 && avg < 90.10);
    Record r;
    CHECK(PerfRecord(h, 7, "high", 1920, 1080, 0, &r));
    CHECK(r.Get("fps_avg") == "90.1" && r.Get("fps_p1low") == "97.6" && r.Get("frame_ms_p95") == "10.3" &&
          r.Get("duration_s") == "11" && r.Get("hitches") == "10" && !r.Has("vr_hz"));
    CHECK(!BuildBody(kId, Common{"desktop", "mac", "0.8.1"}, r).empty());
    CHECK(PerfRecord(h, 1, "high", 1680, 1760, 90, &r) && r.Get("vr_hz") == "90");
    PerfHist s;
    for (int i = 0; i < 100; i++) s.Add(16.7);       // 1.67 s: too short
    CHECK(!PerfRecord(s, 1, "high", 640, 480, 0, &r));
    PerfHist o;
    for (int i = 0; i < 40; i++) o.Add(400.0);       // overflow bin: the real worst time
    CHECK(o.Percentile(95) == 400.0 && o.Hitches() == 40);
}

// ---------------------------------------------------------------- the runtime, with a fake sender
static std::mutex g_m;
static std::vector<std::string> g_sent;
static int g_status = 204;
static int FakeSend(const std::string &url, const std::string &body, int timeoutMs)
{
    std::lock_guard<std::mutex> g(g_m);
    if (url != kSiteUrl || timeoutMs != 3000) return -1;
    g_sent.push_back(body);
    return g_status;
}
static size_t SentCount() { std::lock_guard<std::mutex> g(g_m); return g_sent.size(); }
static std::string SentAt(size_t i) { std::lock_guard<std::mutex> g(g_m); return i < g_sent.size() ? g_sent[i] : ""; }
static bool WaitSent(size_t n)
{
    for (int i = 0; i < 300 && SentCount() < n; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return SentCount() >= n;
}
static void Settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
static bool WaitFile(const std::string &p, bool exists)
{
    for (int i = 0; i < 300 && Exists(p) != exists; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return Exists(p) == exists;
}
static std::vector<std::string> g_opened;
static bool FakeOpen(const std::string &url) { std::lock_guard<std::mutex> g(g_m); g_opened.push_back(url); return true; }
static std::string Field(const std::string &body, const char *key) // the raw JSON value of "key" (tests only)
{
    const std::string k = std::string("\"") + key + "\":";
    size_t a = body.find(k);
    if ( a == std::string::npos ) return "";
    a += k.size();
    size_t b = a;
    if ( body[a] == '[' ) b = body.find(']', a) + 1;
    else if ( body[a] == '"' ) b = body.find('"', a + 1) + 1;
    else while ( b < body.size() && body[b] != ',' && body[b] != '}' ) b++;
    return body.substr(a, b - a);
}

static void TestRuntime(const std::string &dir)
{
    const std::string st = dir + "/usage-stats.txt", qf = dir + "/usage-stats-queue.txt", cf = dir + "/usage-stats-crash.txt";
    // a previous run: the launcher stored "yes" (no number yet), one launcher error is queued, a crash file waits
    WriteAll(st, "answer = yes\n");
    WriteAll(qf, "event=error\tcode=pack_download_failed\tdetail=22\nevent=error\tcode=pack_download_failed\tdetail=23\n");
    WriteAll(cf, "event=crash\tversion=0.8.0\tbuild=dev\tpreset=high\tlevel=3\tsignal=SIGSEGV\tmodule=UA_source\toffset=0x10\tframes=UA_source+0x10\tminutes=4\n");
    SetSender(FakeSend);
    SetRandom(RndCount);
    Init(dir, "desktop", "mac", "0.8.1", "test");
    CHECK(Enabled() && Asked());
    State s = ParseState(ReadAll(st));
    CHECK(Sending(s));                                               // the number was made at start
    CHECK(WaitSent(2));
    Settle();
    CHECK(SentCount() == 2);                                         // the crash + ONE of the two same-code errors
    CHECK(SentAt(0).find("\"event\":\"crash\"") != std::string::npos && SentAt(0).find("\"version\":\"0.8.0\"") != std::string::npos);
    CHECK(SentAt(1).find("\"code\":\"pack_download_failed\"") != std::string::npos);
    CHECK(!Exists(cf) && !Exists(qf));                               // crash file deleted after sending

    Launch("high", "Apple M2");
    LevelStart(4, "high");
    CHECK(WaitSent(4));
    CHECK(SentAt(2).find("\"event\":\"launch\"") != std::string::npos && SentAt(2).find("\"gpu\":\"Apple M2\"") != std::string::npos);
    CHECK(SentAt(3).find("\"level\":4") != std::string::npos);
    for (size_t i = 0; i < 4; i++) CHECK(SentAt(i).find(s.install) != std::string::npos);
    Error("audio_device_failed");
    Error("audio_device_failed");                                    // once per run
    Error("free text is not a code");
    CHECK(WaitSent(5));
    Settle();
    CHECK(SentCount() == 5);

    // perf after a short scripted "mission"
    PerfBegin(4);
    for (int i = 0; i < 400; i++) PerfFrame(16.0);
    PerfFrame(150.0);
    PerfEnd("high", 1280, 720, 0, false);
    CHECK(WaitSent(6));
    CHECK(SentAt(5).find("\"event\":\"perf\"") != std::string::npos && SentAt(5).find("\"hitches\":1") != std::string::npos &&
          SentAt(5).find("\"res\":\"1280x720\"") != std::string::npos);

    // offline: the server can't be reached -> one retry, then the queue
    { std::lock_guard<std::mutex> g(g_m); g_status = -1; }
    LevelStart(5, "high");
    CHECK(WaitSent(8));                                              // two tries
    Settle();
    CHECK(SentCount() == 8);
    CHECK(ReadAll(qf).find("level=5") != std::string::npos);
    // 400 = bad body: dropped, never queued, no retry
    { std::lock_guard<std::mutex> g(g_m); g_status = 400; }
    LevelStart(6, "high");
    CHECK(WaitSent(9));
    Settle();
    CHECK(SentCount() == 9 && ReadAll(qf).find("level=6") == std::string::npos);
    { std::lock_guard<std::mutex> g(g_m); g_status = 204; }

    // off: the number, the queue and a crash file go at once; nothing is sent any more
    WriteAll(cf, "event=crash\n");
    const std::string oldId = s.install;
    SetEnabledByUser(false);
    CHECK(!Enabled());
    CHECK(ReadAll(st).find("answer = no") != std::string::npos && ReadAll(st).find("install") == std::string::npos);
    CHECK(!Exists(qf) && !Exists(cf));
    const size_t before = SentCount();
    Launch("high", "x");
    LevelStart(1, "high");
    Error("game_folder_invalid");
    PerfBegin(1);
    for (int i = 0; i < 400; i++) PerfFrame(16.0);
    PerfEnd("high", 640, 480, 0, false);
    Settle();
    CHECK(SentCount() == before);
    // on again: a new number
    SetEnabledByUser(true);
    s = ParseState(ReadAll(st));
    CHECK(Sending(s) && s.install != oldId);
    LevelStart(2, "low");
    CHECK(WaitSent(before + 1));
    CHECK(SentAt(before).find(s.install) != std::string::npos && SentAt(before).find(oldId) == std::string::npos);
    const std::string sf = dir + "/usage-stats-session.txt";

    // ---- v3: a whole mission. level_start at the first playable frame with the loading time; perf + level_end
    size_t base = SentCount();
    NotePreset("high");                                              // as at launch: not a change
    MissionRequested();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    MissionBegin(8, "high");
    for (int i = 0; i < 20; i++) NoteInput(INPUT_KBM);
    for (int i = 0; i < 5; i++) NoteInput(INPUT_PAD);
    Settle();
    CHECK(SentCount() == base);                                      // nothing before the first frame
    MissionFrame(-1, VIEW_NONE, 0);
    CHECK(WaitSent(base + 1));
    std::string ls = SentAt(base);
    CHECK(Field(ls, "event") == "\"level_start\"" && Field(ls, "level") == "8");
    const long loadMs = atol(Field(ls, "load_ms").c_str());
    CHECK(loadMs >= 40 && loadMs < 5000);
    for (int i = 0; i < 400; i++) MissionFrame(16.0, VIEW_NONE, 0);
    MissionEnd("won", "high", 1280, 720, 0, false);
    CHECK(WaitSent(base + 3));
    CHECK(Field(SentAt(base + 1), "event") == "\"perf\"");
    std::string le = SentAt(base + 2);
    CHECK(Field(le, "event") == "\"level_end\"" && Field(le, "result") == "\"won\"" && Field(le, "level") == "8" &&
          Field(le, "input") == "\"mixed\"" && Field(le, "duration_s") == "0" && le.find("vr_") == std::string::npos);
    CHECK(WaitFile(sf, true));                                       // the session file, written at the mission end
    std::string sess = ReadAll(sf);
    CHECK(sess.find("event=session_end") != std::string::npos && sess.find("ended=killed") != std::string::npos &&
          sess.find("levels=1") != std::string::npos && sess.find(s.install) == std::string::npos);
    // keyboard only in the next mission; a mission that never reached a frame sends nothing
    base = SentCount();
    MissionBegin(9, "high");
    MissionEnd("quit", "high", 640, 480, 0, false);
    Settle();
    CHECK(SentCount() == base);
    MissionRequested();
    MissionBegin(9, "ultra");
    MissionFrame(-1, VIEW_NONE, 0);
    for (int i = 0; i < 3; i++) NoteInput(INPUT_KBM);
    for (int i = 0; i < 10; i++) MissionFrame(16.0, VIEW_NONE, 0); // too short for perf
    MissionEnd("lost", "ultra", 640, 480, 0, false);
    CHECK(WaitSent(base + 2));
    Settle();
    CHECK(SentCount() == base + 2);                                  // level_start + level_end, no perf
    CHECK(Field(SentAt(base + 1), "result") == "\"lost\"" && Field(SentAt(base + 1), "input") == "\"keyboard_mouse\"");
    // preset changes and look switches (Original <-> anything else)
    NotePreset("ultra");
    NotePreset("ultra");
    NotePreset("original");
    NotePreset("high");

    // ---- v3: the per-frame calls make no allocation and take no lock; cost per frame
    MissionBegin(10, "high");
    MissionFrame(-1, VIEW_NONE, 0);                                  // (sends level_start: allowed to allocate)
    CHECK(WaitSent(base + 3));
    SessionTick();                                                   // (the first tick writes the session file)
    Settle();
    const long a0 = g_allocs.load();
    const int kFrames = 1000000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; i++)
    {
        MissionFrame(11.1, (i / 500) % 2 ? VIEW_UNIT : VIEW_TABLE, 100 + i / 5000);
        SessionTick();
        if ( (i & 63) == 0 ) NoteInput(INPUT_KBM);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kFrames;
    const long allocs = g_allocs.load() - a0;
    printf("per-frame stats cost: %.1f ns per frame (MissionFrame + SessionTick + 1/64 NoteInput), %ld allocations in %d frames\n",
           ns, allocs, kFrames);
    CHECK(allocs == 0);
    CHECK(ns < 1000.0);                                              // a 90 Hz frame is 11,100,000 ns
    MissionEnd("restart", "high", 640, 480, 0, false);
    CHECK(WaitSent(base + 5));                                       // perf + level_end (desktop: no vr_ fields)
    CHECK(Field(SentAt(base + 4), "result") == "\"restart\"" && SentAt(base + 4).find("vr_jumps") == std::string::npos);

    // ---- v3: Report a bug, stats on: the form opens with the reference, bug_report carries the same reference
    SetUrlOpener(FakeOpen);
    base = SentCount();
    const std::string ref = ReportBug("high", -1);
    CHECK(ValidBugRef(ref));
    CHECK(WaitSent(base + 1));
    std::string br = SentAt(base);
    CHECK(Field(br, "event") == "\"bug_report\"" && Field(br, "ref") == "\"" + ref + "\"" && Field(br, "level") == "10");
    CHECK(Field(br, "errors") == "[\"pack_download_failed\",\"audio_device_failed\"]");
    CHECK(Field(br, "crash_signal") == "\"SIGSEGV\"" && Field(br, "crash_frames") == "[\"UA_source+0x10\"]");
    CHECK(!Field(br, "fps_avg").empty() && Field(br, "input") == "\"keyboard_mouse\"");
    { std::lock_guard<std::mutex> g(g_m); CHECK(!g_opened.empty() && g_opened.back() == BugReportUrl(ref)); }

    // ---- stats off: Report a bug only opens the browser, nothing is sent; the session file goes
    SetEnabledByUser(false);
    CHECK(!Exists(sf));
    base = SentCount();
    const std::string ref2 = ReportBug("high", 3);
    CHECK(ValidBugRef(ref2) && ref2 != ref);
    MissionRequested();
    MissionBegin(3, "high");
    MissionFrame(-1, VIEW_NONE, 0);
    for (int i = 0; i < 400; i++) MissionFrame(16.0, VIEW_NONE, 0);
    MissionEnd("won", "high", 640, 480, 0, false);
    for (int i = 0; i < 100; i++) SessionTick();
    Settle();
    CHECK(SentCount() == base && !Exists(sf));
    { std::lock_guard<std::mutex> g(g_m); CHECK(g_opened.size() == 2 && g_opened.back() == BugReportUrl(ref2)); }
    // at most 5 per start; then only the plain form
    CHECK(ValidBugRef(ReportBug("high", -1)) && ValidBugRef(ReportBug("high", -1)) && ValidBugRef(ReportBug("high", -1)));
    CHECK(ReportBug("high", -1).empty());
    { std::lock_guard<std::mutex> g(g_m); CHECK(g_opened.size() == 6 && g_opened.back() == BugReportUrl("")); }
    SetEnabledByUser(true);
    s = ParseState(ReadAll(st));

    // quitting during a mission: the perf record waits in the queue for the next start
    PerfBegin(2);
    for (int i = 0; i < 400; i++) PerfFrame(20.0);
    PerfEnd("low", 640, 480, 0, true);
    // ... and a mission's level_end too (result quit)
    MissionRequested();
    MissionBegin(2, "low");
    base = SentCount();
    MissionFrame(-1, VIEW_NONE, 0);
    CHECK(WaitSent(base + 1));
    base = SentCount();
    MissionEnd("quit", "low", 640, 480, 0, true);
    Settle();
    CHECK(SentCount() == base);                                      // nothing sent while closing
    CHECK(ReadAll(qf).find("event=level_end") != std::string::npos && ReadAll(qf).find("result=quit") != std::string::npos);
    // closing normally: session_end (ended quit, the session's preset changes) is sent; the session file goes
    Shutdown();
    CHECK(ReadAll(qf).find("event=perf") != std::string::npos);
    CHECK(!Exists(sf));
    bool sessSent = false;
    for (size_t i = base; i < SentCount(); i++)
    {
        const std::string b2 = SentAt(i);
        if ( Field(b2, "event") != "\"session_end\"" ) continue;
        sessSent = true;
        CHECK(Field(b2, "ended") == "\"quit\"" && Field(b2, "preset_changes") == "3" && Field(b2, "look_switches") == "2" &&
              Field(b2, "minutes") == "0" && Field(b2, "levels") == "5" && Field(b2, "preset") == "\"high\"");
        if ( Field(b2, "levels") != "5" || Field(b2, "preset_changes") != "3" ) printf("session_end: %s\n", b2.c_str());
        CHECK(Field(b2, "input") == "\"keyboard_mouse\"");
    }
    CHECK(sessSent);
}

// ---------------------------------------------------------------- a killed / crashed session, sent at the next start
#ifndef _WIN32
static std::string g_childOut;
static int ChildSend(const std::string &, const std::string &body, int)
{
    FILE *f = fopen(g_childOut.c_str(), "a");
    if (f) { fprintf(f, "%s\n", body.c_str()); fclose(f); }
    return 204;
}
static std::string NextStart(const std::string &dir, const char *answer, bool crashFile)
{
    std::string d = dir + "/next-" + std::to_string(rand() % 100000);
    mkdir(d.c_str(), 0700);
    WriteAll(d + "/usage-stats.txt", std::string("answer = ") + answer + "\ninstall = " + kId + "\n");
    WriteAll(d + "/usage-stats-session.txt",
             "event=session_end\tversion=0.8.0\tpreset=low\tminutes=12\tlevels=2\tended=killed\tlook_switches=1\tpreset_changes=1\n");
    if ( crashFile )
        WriteAll(d + "/usage-stats-crash.txt", "event=crash\tversion=0.8.0\tbuild=dev\tpreset=low\tsignal=SIGSEGV\tmodule=UA_source\toffset=0x10\tframes=UA_source+0x10\tminutes=12\n");
    g_childOut = d + "/sent.txt";
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        SetSender(ChildSend);
        Init(d, "desktop", "mac", "0.8.1", "test");
        for (int i = 0; i < 200; i++)
        {
            std::string t = ReadAll(g_childOut);
            if ( std::count(t.begin(), t.end(), '\n') >= (crashFile ? 2 : 1) ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(!Exists(d + "/usage-stats-session.txt"));                 // picked up (or deleted unsent when off)
    return ReadAll(g_childOut);
}
#endif

// a Quest mission: table / unit seconds and unit jumps in level_end, input = vr_controllers
static void TestVrMission(const std::string &dir)
{
#ifndef _WIN32
    std::string d = dir + "/vr";
    mkdir(d.c_str(), 0700);
    WriteAll(d + "/usage-stats.txt", std::string("answer = yes\ninstall = ") + kId + "\n");
    g_childOut = d + "/sent.txt";
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        SetSender(ChildSend);
        Init(d, "vr", "quest", "0.9.1", "test");
        MissionRequested();
        MissionBegin(1, "high");
        MissionFrame(-1, VIEW_STATION, 77);                     // the host station (gid 77)
        for (int i = 0; i < 900; i++) MissionFrame(10.0, VIEW_TABLE, 77);   // 9 s at the table
        for (int i = 0; i < 500; i++) MissionFrame(10.0, VIEW_UNIT, 501);   // jump 1: into 501 for 5 s
        for (int i = 0; i < 300; i++) MissionFrame(10.0, VIEW_UNIT, 502);   // jump 2: straight into 502, 3 s
        for (int i = 0; i < 100; i++) MissionFrame(10.0, VIEW_STATION, 77); // back to the station: not a jump
        for (int i = 0; i < 200; i++) MissionFrame(10.0, VIEW_UNIT, 501);   // jump 3, 2 s
        MissionEnd("won", "high", 1680, 1760, 90, false);
        for (int i = 0; i < 200; i++)
        {
            std::string t = ReadAll(g_childOut);
            if ( std::count(t.begin(), t.end(), '\n') >= 3 ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const std::string out = ReadAll(g_childOut);
    size_t a = out.find("\"event\":\"level_end\"");
    CHECK(a != std::string::npos);
    const std::string le = a == std::string::npos ? "" : out.substr(out.rfind('{', a), out.find('\n', a) - out.rfind('{', a));
    CHECK(Field(le, "input") == "\"vr_controllers\"" && Field(le, "vr_table_s") == "9" && Field(le, "vr_unit_s") == "10" &&
          Field(le, "vr_jumps") == "3" && Field(le, "result") == "\"won\"");
    CHECK(out.find("\"vr_hz\":90") != std::string::npos && out.find("\"load_ms\":") != std::string::npos);
    if ( Field(le, "vr_jumps") != "3" ) printf("vr level_end: %s\n", le.c_str());
#else
    (void)dir;
#endif
}

static void TestNextStart(const std::string &dir)
{
#ifndef _WIN32
    std::string out = NextStart(dir, "yes", false);
    CHECK(out.find("\"event\":\"session_end\"") != std::string::npos && out.find("\"ended\":\"killed\"") != std::string::npos &&
          out.find("\"version\":\"0.8.0\"") != std::string::npos && out.find("\"minutes\":12") != std::string::npos);
    out = NextStart(dir, "yes", true);
    CHECK(out.find("\"event\":\"crash\"") != std::string::npos && out.find("\"ended\":\"crash\"") != std::string::npos);
    out = NextStart(dir, "no", true);
    CHECK(out.empty());                                              // off: nothing sent, the files deleted
#else
    (void)dir;
#endif
}

static void TestCrashChild(const std::string &dir)
{
#ifndef _WIN32
    const std::string cf = dir + "/crash-child.txt";
    for (const char *how : {"segv", "abort"})
    {
        remove(cf.c_str());
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
        {
            InstallCrashHandler(cf, "0.8.1", "test");
            CrashHandlerContext("medium", 9);
            CrashHandlerEnabled(true);
            TestCrashNow(how);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status));                                  // the crash still reaches the system
        std::string line = ReadAll(cf);
        Record r;
        CHECK(ParseRecordLine(line, &r));
        CHECK(r.Get("signal") == (strcmp(how, "segv") ? "SIGABRT" : "SIGSEGV") || (r.Get("signal") == "SIGBUS"));
        CHECK(r.Get("preset") == "medium" && r.Get("level") == "9" && r.Get("build") == "test" && r.Get("version") == "0.8.1");
        CHECK(line.find('/') == std::string::npos && line.find('\\') == std::string::npos); // no paths
        CHECK(r.Get("frames").find("usage_stats_test") != std::string::npos);
        CHECK(!BuildBody(kId, Common{"desktop", "mac", "0.8.1"}, r).empty());
        printf("crash line (%s): %s", how, line.c_str());
    }
    // switched off: no file at all
    remove(cf.c_str());
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        InstallCrashHandler(cf, "0.8.1", "test");
        CrashHandlerEnabled(false);
        TestCrashNow("segv");
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && !Exists(cf));
#else
    (void)dir;
#endif
}

int main()
{
    char tmpl[] = "/tmp/uastats-XXXXXX";
#ifndef _WIN32
    std::string dir = mkdtemp(tmpl);
#else
    std::string dir = ".";
    (void)tmpl;
#endif
    TestState();
    TestBodies();
    TestHelpers();
    TestQueueAndNames();
    TestPerf();
    TestCrashChild(dir);
    TestNextStart(dir);
    TestVrMission(dir);
    TestRuntime(dir);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
