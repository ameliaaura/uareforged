// Unit tests for the anonymous usage stats (src/usage_stats*.cpp), no game data needed.
//   c++ -std=c++14 -Wall -DUA_TEST_HOOKS -Isrc tests/usage_stats_test.cpp src/usage_stats.cpp \
//       src/usage_stats_platform.cpp src/usage_stats_crash.cpp [src/usage_stats_net_apple.mm -framework Foundation] -o t
// Covers: the state file and the opt-in state machine, every event body against docs/usage-stats.md (v2), the
// queue, the name clean-up, the perf maths, the address check, the runtime end to end with a fake sender
// (nothing sent before opt-in / after opt-out, offline queue, crash file pick-up) and, on POSIX, a real crash
// in a child process.
#include "usage_stats.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace UsageStats;

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
    CHECK(b == std::string("{\"v\":2,\"install\":\"") + kId +
                   "\",\"app\":\"desktop\",\"platform\":\"mac\",\"version\":\"0.8.1\",\"event\":\"launch\",\"preset\":\"high\",\"os\":\"macOS 15.2\",\"gpu\":\"Apple M2\"}");
    b = BuildBody(kId, Common{"vr", "quest", "0.9.1"}, Rec({{"event", "level_start"}, {"preset", "high"}, {"level", "7"}}));
    CHECK(b == std::string("{\"v\":2,\"install\":\"") + kId +
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
    CHECK(b == std::string("{\"v\":2,\"install\":\"") + kId +
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
    // quitting during a mission: the perf record waits in the queue for the next start
    PerfBegin(2);
    for (int i = 0; i < 400; i++) PerfFrame(20.0);
    PerfEnd("low", 640, 480, 0, true);
    Shutdown();
    CHECK(ReadAll(qf).find("event=perf") != std::string::npos);
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
    TestQueueAndNames();
    TestPerf();
    TestCrashChild(dir);
    TestRuntime(dir);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
