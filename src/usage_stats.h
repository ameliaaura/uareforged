#ifndef USAGE_STATS_H_INCLUDED
#define USAGE_STATS_H_INCLUDED

// Anonymous usage stats (opt-in). Wire format and every rule: docs/usage-stats.md (v2).
//
// Off unless the player said yes. The question is asked once (desktop: the launcher's first-start
// dialog; Quest: a start screen); Options has "Send anonymous usage stats" on the desktop and in VR.
// When on, the game POSTs small JSON bodies to https://uareforged.pages.dev/api/ping:
//   launch (+ os, gpu) · level_start · perf (after each mission) · crash (next start after a crash)
//   · error (a code from a fixed list + a number)
// Background thread only, 3 s timeout, one retry, then a local queue of at most 20 events (oldest
// dropped) sent on a later start. Never blocks the game; silent on failure; nothing about a request is
// logged to disk (UA_STATS_DEBUG=1: one console line per request, never the install number).
//
// Files (the app's settings folder: UA_SETTINGS_DIR from the launchers, else the platform default):
//   usage-stats.txt        answer + install number (deleted when turned off)
//   usage-stats-queue.txt  unsent events (deleted when turned off)
//   usage-stats-crash.txt  the last crash (deleted after sending, or at start when off)
//
// The pure parts (state, records, body builder, perf maths, name clean-up) have no engine
// dependencies and are unit-tested (tests/usage_stats_test.cpp).

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace UsageStats
{

// ------------------------------------------------------------------ state (usage-stats.txt)
enum Answer
{
    ANSWER_UNASKED = 0, // never asked, or the file is missing / unreadable: off
    ANSWER_NO,          // "No thanks", dismissed, or turned off in Options
    ANSWER_YES          // "Yes, send anonymous stats", or turned on in Options
};

struct State
{
    Answer answer = ANSWER_UNASKED;
    std::string install; // 32 lowercase hex; only kept while the answer is yes
};

typedef bool (*RandomFn)(uint8_t out[16]); // secure random bytes; false = none (stats stay off)

bool ValidInstallId(const std::string &id);
std::string HexId(const uint8_t bytes[16]);
State ParseState(const std::string &text);   // "answer = yes|no", "install = <hex>"
std::string FormatState(const State &s);
// The state machine; each returns true when the state changed.
bool ApplyAnswer(State &s, bool yes, RandomFn rnd); // first-start question (dismissed = no)
bool SetEnabled(State &s, bool on, RandomFn rnd);   // Options: off deletes the number, on makes a NEW one
bool EnsureInstallId(State &s, RandomFn rnd);       // yes without a number (the launcher wrote the answer)
bool Sending(const State &s);                       // yes + a valid number

// ------------------------------------------------------------------ events
// One event as ordered key/value strings: "event" first, then its own fields (docs/usage-stats.md).
// "frames" holds the crash frames comma-separated; "version" may be present (queued from an older run).
struct Record
{
    std::vector<std::pair<std::string, std::string>> f;
    void Set(const std::string &k, const std::string &v);
    std::string Get(const std::string &k) const;
    bool Has(const std::string &k) const;
};

struct Common
{
    std::string app;      // desktop | vr
    std::string platform; // mac | windows | quest
    std::string version;
};

// The JSON body, or "" when anything breaks a rule (nothing is ever sent then).
std::string BuildBody(const std::string &install, const Common &c, const Record &r);
size_t MaxBody(const std::string &event); // 4096, crash 8192

// Queue / crash file lines: key=value pairs separated by tabs, one event per line.
std::string FormatRecordLine(const Record &r);
bool ParseRecordLine(const std::string &line, Record *out);
// The queue file's lines, keeping the newest `max` valid ones.
std::vector<Record> ParseQueue(const std::string &text, size_t max);
std::string FormatQueue(const std::vector<Record> &q, size_t max);

// Fixed error codes (docs/usage-stats.md)
bool ValidErrorCode(const std::string &code);

// Coarse names: "NVIDIA GeForce RTX 3070/PCIe/SSE2" -> "NVIDIA GeForce RTX 3070", "Adreno (TM) 740" -> "Adreno 740".
std::string CleanGpu(const std::string &raw);
std::string CleanOs(const std::string &raw);

// Frame-time summary of one mission (histogram: 0.25 ms bins up to 250 ms, O(1) memory).
class PerfHist
{
public:
    void Reset();
    void Add(double ms);
    uint32_t Frames() const { return frames_; }
    double Seconds() const { return sumMs_ / 1000.0; }
    uint32_t Hitches() const { return hitches_; }
    double FpsAvg() const;
    double Percentile(double p) const; // frame time in ms at percentile p (0..100)
    double FpsP1Low() const;           // 1000 / 99th-percentile frame time
private:
    static const int kBins = 1001;     // [0, 250) ms in 0.25 ms steps + one overflow bin
    uint32_t bins_[kBins] = {};
    uint32_t frames_ = 0, hitches_ = 0;
    double sumMs_ = 0, maxMs_ = 0;
};
// The perf record for a mission, or false when it was too short (< 5 s or < 30 frames).
bool PerfRecord(const PerfHist &h, int level, const std::string &preset, int resW, int resH, int vrHz, Record *out);

extern const char *const kSiteUrl;
// The fixed site address, or (test runs, UA_STATS_URL) plain http on 127.0.0.1 / localhost.
bool UrlAllowed(const std::string &url);

// ------------------------------------------------------------------ the game's side
// HTTP POST of a JSON body: the HTTP status, or < 0 for a network error. Background thread.
typedef int (*SendFn)(const std::string &url, const std::string &body, int timeoutMs);
void SetSender(SendFn fn);          // the Quest app registers its own; desktop: built in
void SetRandom(RandomFn fn);        // tests
bool OsRandom(uint8_t out[16]);     // getentropy / BCryptGenRandom / arc4random_buf
std::string OsName();               // "macOS 15.2", "Windows 11 23H2", "Quest OS 74"
std::string DefaultSettingsDir();
int DefaultSend(const std::string &url, const std::string &body, int timeoutMs); // platform sender (or -1)

// Reads usage-stats.txt from <dir> (empty: UA_SETTINGS_DIR, else the platform default), makes the
// install number if needed, picks up the queue and a crash file (sent if on, deleted if off) and
// arms the crash handler. Call once at start-up. build = the build identifier ("dev" locally).
void Init(const std::string &dir, const char *app, const char *platform, const char *version, const char *build);
std::string SettingsDir();
bool Enabled();
bool Asked();
// Options (and the Quest start screen). Off: every unsent event, the queue, a crash file and the
// install number are deleted at once.
void SetEnabledByUser(bool on);
void RecordAnswer(bool yes);

// Events (each a no-op when off; return at once)
void Launch(const char *preset, const std::string &gpuRaw);
void LevelStart(int level, const char *preset);
void Error(const char *code, long detail = -1, const char *preset = nullptr, int level = -1); // once per code per run
// Perf: Begin at a mission start, Frame per played frame, End at its end (or quitting = true at quit,
// then the record goes to the queue for the next start instead of the network).
void PerfBegin(int level);
void PerfFrame(double ms);
void PerfEnd(const char *preset, int resW, int resH, int vrHz, bool quitting);
bool PerfRunning();
// What the crash handler records about "now" (cheap; any thread).
void SetCrashContext(const char *preset, int level);
// At quit: waits up to 1 s for requests in flight, then keeps anything unsent in the queue.
void Shutdown();

// Crash handler (usage_stats_crash.cpp): installs once; writes usage-stats-crash.txt only while on.
void InstallCrashHandler(const std::string &crashPath, const char *version, const char *build);
void CrashHandlerEnabled(bool on);
void CrashHandlerContext(const char *preset, int level);
#ifdef UA_TEST_HOOKS
void TestCrashNow(const char *how); // "segv" | "abort" (test builds only)
#endif

} // namespace UsageStats

#endif // USAGE_STATS_H_INCLUDED
