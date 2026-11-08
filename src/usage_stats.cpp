// Anonymous usage stats: state, records, body builder, perf maths and the background sender.
// Wire format: docs/usage-stats.md (v2). Platform parts: usage_stats_platform.cpp,
// usage_stats_net_apple.mm, usage_stats_crash.cpp.
#include "usage_stats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace UsageStats
{

// file helpers (usage_stats_platform.cpp): UTF-8 paths on every platform
bool FileRead(const std::string &path, std::string *out, size_t max);
bool FileWriteAtomic(const std::string &path, const std::string &text);
void FileRemove(const std::string &path);
bool FileExists(const std::string &path);
void MakeDir(const std::string &path);
std::string EnvVar(const char *name);
std::string PathJoin(const std::string &dir, const char *leaf);

const char *const kSiteUrl = "https://uareforged.pages.dev/api/ping";

namespace
{
const size_t kQueueMax = 20;

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// every character from `extra` or alphanumeric, length lo..hi
bool Charset(const std::string &s, const char *extra, size_t lo, size_t hi)
{
    if ( s.size() < lo || s.size() > hi )
        return false;
    for (char c : s)
        if ( !IsAlnum(c) && !strchr(extra, c) )
            return false;
    return true;
}

std::string Trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while ( a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n') ) a++;
    while ( b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n') ) b--;
    return s.substr(a, b - a);
}

bool ParseLong(const std::string &s, long lo, long hi, long *out)
{
    if ( s.empty() || s.size() > 12 )
        return false;
    size_t i = 0;
    if ( s[0] == '-' ) i = 1;
    if ( i == s.size() )
        return false;
    for (size_t k = i; k < s.size(); k++)
        if ( s[k] < '0' || s[k] > '9' )
            return false;
    long v = strtol(s.c_str(), nullptr, 10);
    if ( v < lo || v > hi )
        return false;
    *out = v;
    return true;
}

// a decimal number "123", "123.4" (no exponent, no sign) within lo..hi -> one decimal place
bool ParseDec1(const std::string &s, double lo, double hi, std::string *out)
{
    if ( s.empty() || s.size() > 16 )
        return false;
    int dots = 0;
    for (char c : s)
    {
        if ( c == '.' ) dots++;
        else if ( c < '0' || c > '9' ) return false;
    }
    if ( dots > 1 || s[0] == '.' || s.back() == '.' )
        return false;
    // locale-free parse
    double v = 0, scale = 0;
    for (char c : s)
    {
        if ( c == '.' ) { scale = 1; continue; }
        if ( scale > 0 ) { scale /= 10; v += (c - '0') * scale; }
        else v = v * 10 + (c - '0');
    }
    if ( v < lo || v > hi )
        return false;
    long tenths = (long)std::floor(v * 10 + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld.%ld", tenths / 10, tenths % 10);
    *out = buf;
    return true;
}

std::string Dec1(double v)
{
    if ( !(v >= 0) ) v = 0;
    long tenths = (long)std::floor(v * 10 + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld.%ld", tenths / 10, tenths % 10);
    return buf;
}

std::string Int(long v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", v);
    return buf;
}

bool ValidPreset(const std::string &p)
{
    return p == "original" || p == "low" || p == "medium" || p == "high" || p == "ultra";
}

bool ValidModule(const std::string &m) { return Charset(m, "._+-", 1, 64); }

bool ValidOffset(const std::string &o)
{
    if ( o.size() < 3 || o.size() > 18 || o[0] != '0' || o[1] != 'x' )
        return false;
    for (size_t i = 2; i < o.size(); i++)
        if ( !IsHex(o[i]) )
            return false;
    return true;
}

bool ValidFrame(const std::string &f)
{
    size_t p = f.rfind('+');
    return p != std::string::npos && p > 0 && ValidModule(f.substr(0, p)) && ValidOffset(f.substr(p + 1));
}

enum Kind { K_PRESET, K_LEVEL, K_NAME40, K_NAME64, K_RES, K_DEC, K_INT, K_SIGNAL, K_MODULE, K_OFFSET, K_FRAMES, K_BUILD, K_CODE };

struct Spec
{
    const char *key;
    Kind kind;
    bool required;
    double lo, hi;
};

const Spec kLaunch[] = {
    {"preset", K_PRESET, true, 0, 0}, {"os", K_NAME40, true, 0, 0}, {"gpu", K_NAME64, true, 0, 0}};
const Spec kLevelStart[] = {{"preset", K_PRESET, true, 0, 0}, {"level", K_LEVEL, true, 0, 9999}};
const Spec kPerf[] = {
    {"preset", K_PRESET, true, 0, 0},     {"level", K_LEVEL, true, 0, 9999},
    {"res", K_RES, true, 0, 0},           {"fps_avg", K_DEC, true, 0, 1000},
    {"fps_p1low", K_DEC, true, 0, 1000},  {"frame_ms_p95", K_DEC, true, 0, 10000},
    {"duration_s", K_INT, true, 0, 86400}, {"hitches", K_INT, true, 0, 1000000},
    {"vr_hz", K_INT, false, 0, 240}};
const Spec kCrash[] = {
    {"preset", K_PRESET, true, 0, 0},  {"level", K_LEVEL, false, 0, 9999}, {"build", K_BUILD, true, 0, 0},
    {"signal", K_SIGNAL, true, 0, 0},  {"module", K_MODULE, true, 0, 0},  {"offset", K_OFFSET, true, 0, 0},
    {"frames", K_FRAMES, true, 0, 0},  {"minutes", K_INT, true, 0, 100000}};
const Spec kError[] = {
    {"code", K_CODE, true, 0, 0}, {"detail", K_INT, false, 0, 1000000000},
    {"preset", K_PRESET, false, 0, 0}, {"level", K_LEVEL, false, 0, 9999}};

const char *const kErrorCodes[] = {
    "pack_download_failed", "pack_checksum_mismatch", "game_download_failed", "game_folder_invalid",
    "links_json_invalid", "level_load_failed", "material_map_missing", "shader_load_failed",
    "audio_device_failed", "vr_session_lost"};

bool SpecFor(const std::string &ev, const Spec **s, size_t *n)
{
#define UAS_SPEC(name, arr) if ( ev == name ) { *s = arr; *n = sizeof(arr) / sizeof(arr[0]); return true; }
    UAS_SPEC("launch", kLaunch)
    UAS_SPEC("level_start", kLevelStart)
    UAS_SPEC("perf", kPerf)
    UAS_SPEC("crash", kCrash)
    UAS_SPEC("error", kError)
#undef UAS_SPEC
    return false;
}

// the JSON value for one field, or "" when it breaks its rule
std::string Value(const Spec &sp, const std::string &v)
{
    long n;
    std::string d;
    switch ( sp.kind )
    {
    case K_PRESET: return ValidPreset(v) ? "\"" + v + "\"" : "";
    case K_LEVEL:
    case K_INT: return ParseLong(v, (long)sp.lo, (long)sp.hi, &n) ? Int(n) : "";
    case K_NAME40: return Charset(v, " .()_+-", 1, 40) && v[0] != ' ' ? "\"" + v + "\"" : "";
    case K_NAME64: return Charset(v, " .()_+-", 1, 64) && v[0] != ' ' ? "\"" + v + "\"" : "";
    case K_RES:
    {
        size_t x = v.find('x');
        long w, h;
        if ( x == std::string::npos || v[0] == '0' || x + 1 >= v.size() || v[x + 1] == '0' )
            return "";
        if ( !ParseLong(v.substr(0, x), 10, 99999, &w) || !ParseLong(v.substr(x + 1), 10, 99999, &h) )
            return "";
        return "\"" + Int(w) + "x" + Int(h) + "\"";
    }
    case K_DEC: return ParseDec1(v, sp.lo, sp.hi, &d) ? d : "";
    case K_SIGNAL:
        for (char c : v)
            if ( !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') )
                return "";
        return (!v.empty() && v.size() <= 24) ? "\"" + v + "\"" : "";
    case K_MODULE: return ValidModule(v) ? "\"" + v + "\"" : "";
    case K_OFFSET: return ValidOffset(v) ? "\"" + v + "\"" : "";
    case K_BUILD: return Charset(v, "._-", 1, 40) ? "\"" + v + "\"" : "";
    case K_CODE: return ValidErrorCode(v) ? "\"" + v + "\"" : "";
    case K_FRAMES:
    {
        std::string out = "[";
        size_t count = 0, a = 0;
        if ( !v.empty() )
        {
            for (;;)
            {
                size_t b = v.find(',', a);
                std::string f = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if ( !ValidFrame(f) || ++count > 32 )
                    return "";
                if ( count > 1 ) out += ",";
                out += "\"" + f + "\"";
                if ( b == std::string::npos ) break;
                a = b + 1;
            }
        }
        return out + "]";
    }
    }
    return "";
}
} // namespace

// ------------------------------------------------------------------ state
bool ValidInstallId(const std::string &id)
{
    if ( id.size() != 32 )
        return false;
    for (char c : id)
        if ( !IsHex(c) )
            return false;
    return true;
}

std::string HexId(const uint8_t b[16])
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; i++)
    {
        s += h[b[i] >> 4];
        s += h[b[i] & 15];
    }
    return s;
}

State ParseState(const std::string &text)
{
    State s;
    std::string install;
    size_t a = 0;
    while ( a < text.size() )
    {
        size_t b = text.find('\n', a);
        std::string line = Trim(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b == std::string::npos ? text.size() : b + 1;
        if ( line.empty() || line[0] == '#' )
            continue;
        size_t eq = line.find('=');
        if ( eq == std::string::npos )
            continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if ( k == "answer" )
            s.answer = v == "yes" ? ANSWER_YES : (v == "no" ? ANSWER_NO : ANSWER_UNASKED);
        else if ( k == "install" && ValidInstallId(v) )
            install = v;
    }
    if ( s.answer == ANSWER_YES )
        s.install = install;
    return s;
}

std::string FormatState(const State &s)
{
    std::string t = "# UA Reforged: anonymous usage stats (Options > Send anonymous usage stats)\n";
    if ( s.answer == ANSWER_UNASKED )
        return t;
    t += std::string("answer = ") + (s.answer == ANSWER_YES ? "yes" : "no") + "\n";
    if ( s.answer == ANSWER_YES && ValidInstallId(s.install) )
        t += "install = " + s.install + "\n";
    return t;
}

static bool NewId(State &s, RandomFn rnd)
{
    uint8_t b[16];
    if ( !rnd || !rnd(b) )
        return false;
    s.install = HexId(b);
    return true;
}

bool ApplyAnswer(State &s, bool yes, RandomFn rnd)
{
    return SetEnabled(s, yes, rnd);
}

bool SetEnabled(State &s, bool on, RandomFn rnd)
{
    const State before = s;
    if ( on )
    {
        State n;
        if ( NewId(n, rnd) ) // no randomness: stays as it was (never on without a number)
        {
            n.answer = ANSWER_YES;
            s = n;
        }
        else if ( s.answer != ANSWER_YES )
        {
            s.answer = ANSWER_NO;
            s.install.clear();
        }
    }
    else
    {
        s.answer = ANSWER_NO;
        s.install.clear();
    }
    return s.answer != before.answer || s.install != before.install;
}

bool EnsureInstallId(State &s, RandomFn rnd)
{
    if ( s.answer != ANSWER_YES || ValidInstallId(s.install) )
        return false;
    if ( !NewId(s, rnd) )
    {
        s.answer = ANSWER_NO; // can't make a number: off
        s.install.clear();
    }
    return true;
}

bool Sending(const State &s)
{
    return s.answer == ANSWER_YES && ValidInstallId(s.install);
}

// ------------------------------------------------------------------ records
void Record::Set(const std::string &k, const std::string &v)
{
    for (auto &p : f)
        if ( p.first == k ) { p.second = v; return; }
    f.push_back(std::make_pair(k, v));
}

std::string Record::Get(const std::string &k) const
{
    for (auto &p : f)
        if ( p.first == k ) return p.second;
    return std::string();
}

bool Record::Has(const std::string &k) const
{
    for (auto &p : f)
        if ( p.first == k ) return true;
    return false;
}

bool ValidErrorCode(const std::string &code)
{
    for (const char *c : kErrorCodes)
        if ( code == c ) return true;
    return false;
}

size_t MaxBody(const std::string &event) { return event == "crash" ? 8192 : 4096; }

std::string BuildBody(const std::string &install, const Common &c, const Record &r)
{
    const std::string ev = r.Get("event");
    const Spec *spec;
    size_t n;
    if ( !ValidInstallId(install) || !SpecFor(ev, &spec, &n) )
        return "";
    if ( c.app != "desktop" && c.app != "vr" )
        return "";
    if ( c.platform != "mac" && c.platform != "windows" && c.platform != "quest" )
        return "";
    const std::string version = r.Has("version") ? r.Get("version") : c.version;
    if ( !Charset(version, ".+-", 1, 32) )
        return "";
    // no key outside the event's list
    for (auto &p : r.f)
    {
        if ( p.first == "event" || p.first == "version" )
            continue;
        bool known = false;
        for (size_t i = 0; i < n; i++)
            if ( p.first == spec[i].key ) known = true;
        if ( !known )
            return "";
    }
    std::string body = "{\"v\":2,\"install\":\"" + install + "\",\"app\":\"" + c.app + "\",\"platform\":\"" + c.platform +
                       "\",\"version\":\"" + version + "\",\"event\":\"" + ev + "\"";
    for (size_t i = 0; i < n; i++)
    {
        if ( !r.Has(spec[i].key) )
        {
            if ( spec[i].required )
                return "";
            continue;
        }
        std::string v = Value(spec[i], r.Get(spec[i].key));
        if ( v.empty() )
            return "";
        body += std::string(",\"") + spec[i].key + "\":" + v;
    }
    body += "}";
    return body.size() <= MaxBody(ev) ? body : "";
}

std::string FormatRecordLine(const Record &r)
{
    std::string s;
    for (auto &p : r.f)
    {
        // a tab or a line break in a value would split the line; such a value is invalid anyway
        if ( p.first.find_first_of("\t\r\n=") != std::string::npos || p.second.find_first_of("\t\r\n") != std::string::npos )
            continue;
        if ( !s.empty() ) s += "\t";
        s += p.first + "=" + p.second;
    }
    return s;
}

bool ParseRecordLine(const std::string &line, Record *out)
{
    Record r;
    size_t a = 0;
    const std::string l = Trim(line);
    if ( l.empty() || l.size() > 9000 )
        return false;
    while ( a <= l.size() )
    {
        size_t b = l.find('\t', a);
        std::string kv = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
        size_t eq = kv.find('=');
        if ( eq == std::string::npos || eq == 0 )
            return false;
        r.Set(kv.substr(0, eq), kv.substr(eq + 1));
        if ( b == std::string::npos ) break;
        a = b + 1;
    }
    const Spec *s;
    size_t n;
    if ( !SpecFor(r.Get("event"), &s, &n) )
        return false;
    *out = r;
    return true;
}

std::vector<Record> ParseQueue(const std::string &text, size_t max)
{
    std::vector<Record> q;
    size_t a = 0;
    while ( a < text.size() )
    {
        size_t b = text.find('\n', a);
        Record r;
        if ( ParseRecordLine(text.substr(a, b == std::string::npos ? std::string::npos : b - a), &r) )
            q.push_back(r);
        a = b == std::string::npos ? text.size() : b + 1;
    }
    if ( q.size() > max )
        q.erase(q.begin(), q.end() - max);
    return q;
}

std::string FormatQueue(const std::vector<Record> &q, size_t max)
{
    std::string t;
    size_t from = q.size() > max ? q.size() - max : 0;
    for (size_t i = from; i < q.size(); i++)
        t += FormatRecordLine(q[i]) + "\n";
    return t;
}

// ------------------------------------------------------------------ names
static std::string Collapse(const std::string &s, const char *extra, size_t max)
{
    std::string o;
    for (char c : s)
    {
        if ( !IsAlnum(c) && !strchr(extra, c) )
            c = ' ';
        if ( c == ' ' && (o.empty() || o.back() == ' ') )
            continue;
        o += c;
    }
    while ( !o.empty() && o.back() == ' ' ) o.pop_back();
    if ( o.size() > max )
    {
        o.resize(max);
        while ( !o.empty() && o.back() == ' ' ) o.pop_back();
    }
    return o;
}

static void EraseAll(std::string &s, const char *what)
{
    size_t p;
    while ( (p = s.find(what)) != std::string::npos )
        s.erase(p, strlen(what));
}

std::string CleanGpu(const std::string &raw)
{
    std::string s = raw.substr(0, raw.find('/'));
    EraseAll(s, "(TM)");
    EraseAll(s, "(tm)");
    EraseAll(s, "(R)");
    EraseAll(s, "(r)");
    // anything in brackets that is left (driver names, versions): dropped
    std::string o;
    int depth = 0;
    for (char c : s)
    {
        if ( c == '(' ) { depth++; continue; }
        if ( c == ')' ) { if ( depth ) depth--; continue; }
        if ( !depth ) o += c;
    }
    o = Collapse(o, "._+-", 64);
    return o.empty() ? "unknown" : o;
}

std::string CleanOs(const std::string &raw)
{
    std::string o = Collapse(raw, ".()_+-", 40);
    return o.empty() ? "unknown" : o;
}

// ------------------------------------------------------------------ perf
void PerfHist::Reset() { *this = PerfHist(); }

void PerfHist::Add(double ms)
{
    if ( !(ms >= 0) )
        return;
    int i = (int)(ms / 0.25);
    if ( i >= kBins - 1 || ms >= 250.0 ) i = kBins - 1;
    bins_[i]++;
    frames_++;
    sumMs_ += ms;
    if ( ms > maxMs_ ) maxMs_ = ms;
    if ( ms > 100.0 ) hitches_++;
}

double PerfHist::FpsAvg() const
{
    if ( !frames_ || sumMs_ <= 0 ) return 0;
    return std::min(1000.0, frames_ / (sumMs_ / 1000.0));
}

double PerfHist::Percentile(double p) const
{
    if ( !frames_ ) return 0;
    double want = std::ceil(frames_ * p / 100.0);
    if ( want < 1 ) want = 1;
    uint32_t acc = 0;
    for (int i = 0; i < kBins; i++)
    {
        acc += bins_[i];
        if ( acc >= want )
            return i == kBins - 1 ? std::min(maxMs_, 10000.0) : (i + 1) * 0.25;
    }
    return std::min(maxMs_, 10000.0);
}

double PerfHist::FpsP1Low() const
{
    double p99 = Percentile(99);
    return p99 > 0 ? std::min(1000.0, 1000.0 / p99) : 0;
}

bool PerfRecord(const PerfHist &h, int level, const std::string &preset, int resW, int resH, int vrHz, Record *out)
{
    if ( h.Frames() < 30 || h.Seconds() < 5.0 || level < 0 || level > 9999 )
        return false;
    Record r;
    r.Set("event", "perf");
    r.Set("preset", preset);
    r.Set("level", Int(level));
    r.Set("res", Int(resW) + "x" + Int(resH));
    r.Set("fps_avg", Dec1(h.FpsAvg()));
    r.Set("fps_p1low", Dec1(h.FpsP1Low()));
    r.Set("frame_ms_p95", Dec1(h.Percentile(95)));
    r.Set("duration_s", Int(std::min(86400L, (long)std::floor(h.Seconds() + 0.5))));
    r.Set("hitches", Int(std::min<long>(1000000L, h.Hitches())));
    if ( vrHz > 0 )
        r.Set("vr_hz", Int(std::min(240, vrHz)));
    *out = r;
    return true;
}

bool UrlAllowed(const std::string &url)
{
    if ( url == kSiteUrl )
        return true;
    // test runs: plain http to this machine only, with a port
    for (const char *pre : {"http://127.0.0.1:", "http://localhost:"})
    {
        const size_t n = strlen(pre);
        if ( url.compare(0, n, pre) != 0 )
            continue;
        size_t i = n, digits = 0;
        while ( i < url.size() && url[i] >= '0' && url[i] <= '9' ) { i++; digits++; }
        if ( digits == 0 || digits > 5 || (i < url.size() && url[i] != '/') )
            return false;
        for (; i < url.size(); i++)
            if ( !IsAlnum(url[i]) && !strchr("/._-", url[i]) )
                return false;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ runtime
namespace
{
struct Item
{
    Record r;
    bool crashFile = false; // delete usage-stats-crash.txt when this one is done
};

struct Runtime
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<Item> pending;
    bool worker = false, inFlight = false;
    Item flight;
    uint32_t gen = 0;
    bool inited = false, debug = false, shutdown = false;
    State st;
    std::string dir, statePath, queuePath, crashPath, url;
    Common c;
    std::set<std::string> errorsSent;
    SendFn send = nullptr;
    RandomFn rnd = nullptr;
    // perf (main thread only)
    PerfHist perf;
    bool perfOn = false;
    int perfLevel = -1;
};

// never destroyed: the detached worker may still run while the process exits
Runtime &R()
{
    static Runtime *r = new Runtime;
    return *r;
}

void Debug(const std::string &event, const char *what)
{
    if ( R().debug )
        fprintf(stderr, "usage stats: %s %s\n", event.c_str(), what);
}

void SaveStateLocked(Runtime &r)
{
    if ( r.dir.empty() )
        return;
    MakeDir(r.dir);
    FileWriteAtomic(r.statePath, FormatState(r.st));
}

void QueueAppendLocked(Runtime &r, const std::vector<Record> &add)
{
    if ( r.queuePath.empty() || !Sending(r.st) || add.empty() )
        return;
    std::string text;
    FileRead(r.queuePath, &text, 1 << 20);
    std::vector<Record> q = ParseQueue(text, kQueueMax);
    q.insert(q.end(), add.begin(), add.end());
    FileWriteAtomic(r.queuePath, FormatQueue(q, kQueueMax));
}

void Worker()
{
    Runtime &r = R();
    std::unique_lock<std::mutex> lk(r.m);
    for (;;)
    {
        r.cv.wait(lk, [&r] { return !r.pending.empty(); });
        Item it = r.pending.front();
        r.pending.pop_front();
        if ( !Sending(r.st) || r.shutdown )
            continue;
        r.inFlight = true;
        r.flight = it;
        const uint32_t gen = r.gen;
        const std::string body = BuildBody(r.st.install, r.c, it.r), url = r.url;
        const std::string ev = it.r.Get("event");
        SendFn send = r.send;
        lk.unlock();

        int status = -1;
        if ( !body.empty() && send )
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                status = send(url, body, 3000);
                if ( (status >= 200 && status < 300) || (status >= 400 && status < 500 && status != 408 && status != 429) )
                    break;
                std::lock_guard<std::mutex> g(r.m);
                if ( r.gen != gen ) break; // turned off meanwhile: no retry
            }
        }

        lk.lock();
        r.inFlight = false;
        if ( r.gen == gen && !r.shutdown )
        {
            const bool ok = status >= 200 && status < 300;
            const bool refused = body.empty() || (status >= 400 && status < 500 && status != 408 && status != 429);
            if ( !ok && !refused )
                QueueAppendLocked(r, {it.r}); // offline: a later start sends it
            if ( it.crashFile )
                FileRemove(r.crashPath);      // sent, refused, or now in the queue
            char what[48];
            snprintf(what, sizeof(what), body.empty() ? "invalid, dropped" : ok ? "sent (%d)" : refused ? "refused (%d)" : "not sent (%d), queued", status);
            Debug(ev, what);
        }
        r.cv.notify_all();
    }
}

void EnqueueLocked(Runtime &r, const Item &it)
{
    if ( !Sending(r.st) || r.shutdown )
        return;
    r.pending.push_back(it);
    while ( r.pending.size() > kQueueMax )
        r.pending.pop_front();
    if ( !r.worker )
    {
        r.worker = true;
        std::thread(Worker).detach();
    }
    r.cv.notify_all();
}

void Enqueue(const Record &rec)
{
    Runtime &r = R();
    std::lock_guard<std::mutex> g(r.m);
    Item it;
    it.r = rec;
    EnqueueLocked(r, it);
}
} // namespace

void SetSender(SendFn fn) { std::lock_guard<std::mutex> g(R().m); R().send = fn; }
void SetRandom(RandomFn fn) { std::lock_guard<std::mutex> g(R().m); R().rnd = fn; }

void Init(const std::string &dirIn, const char *app, const char *platform, const char *version, const char *build)
{
    Runtime &r = R();
    std::lock_guard<std::mutex> g(r.m);
    if ( r.inited )
        return;
    r.inited = true;
    if ( !r.send ) r.send = DefaultSend;
    if ( !r.rnd ) r.rnd = OsRandom;
    r.c.app = app ? app : "";
    r.c.platform = platform ? platform : "";
    r.c.version = version ? version : "";
    const std::string dbg = EnvVar("UA_STATS_DEBUG");
    r.debug = !dbg.empty() && dbg != "0";
    r.url = kSiteUrl;
    const std::string testUrl = EnvVar("UA_STATS_URL");
    if ( !testUrl.empty() )
    {
        if ( UrlAllowed(testUrl) ) r.url = testUrl;
        else Debug("setup", "UA_STATS_URL refused (only the site or http on this machine)");
    }
    r.dir = !dirIn.empty() ? dirIn : EnvVar("UA_SETTINGS_DIR");
    if ( r.dir.empty() )
        r.dir = DefaultSettingsDir();
    if ( r.dir.empty() )
        return; // nowhere to keep the answer: off
    r.statePath = PathJoin(r.dir, "usage-stats.txt");
    r.queuePath = PathJoin(r.dir, "usage-stats-queue.txt");
    r.crashPath = PathJoin(r.dir, "usage-stats-crash.txt");

    std::string text;
    if ( FileRead(r.statePath, &text, 4096) )
        r.st = ParseState(text);
    // "answer = yes" from the launcher (no number yet), or a damaged number: make one now
    if ( EnsureInstallId(r.st, r.rnd) )
        SaveStateLocked(r);
    // a "no" never keeps a number: rewrite a file that still has one
    if ( r.st.answer == ANSWER_NO && text.find("install") != std::string::npos )
        SaveStateLocked(r);
    Debug("setup", Sending(r.st) ? "on" : (r.st.answer == ANSWER_UNASKED ? "off (never asked)" : "off"));

    const bool on = Sending(r.st);
    // the last crash: sent now when on, deleted unsent when off
    std::string crash;
    if ( FileRead(r.crashPath, &crash, 16384) )
    {
        Record rec;
        if ( on && ParseRecordLine(crash, &rec) && rec.Get("event") == "crash" )
        {
            Item it;
            it.r = rec;
            it.crashFile = true;
            EnqueueLocked(r, it);
        }
        else
            FileRemove(r.crashPath);
    }
    // unsent events from earlier starts (and launcher errors)
    std::string q;
    if ( FileRead(r.queuePath, &q, 1 << 20) )
    {
        FileRemove(r.queuePath);
        if ( on )
            for (const Record &rec : ParseQueue(q, kQueueMax))
            {
                // launcher lines carry only event/code/detail; each error code once per start
                if ( rec.Get("event") == "error" )
                {
                    if ( r.errorsSent.count(rec.Get("code")) ) continue;
                    r.errorsSent.insert(rec.Get("code"));
                }
                Item it;
                it.r = rec;
                EnqueueLocked(r, it);
            }
    }
    InstallCrashHandler(r.crashPath, r.c.version.c_str(), build ? build : "dev");
    CrashHandlerEnabled(on);
}

std::string SettingsDir() { std::lock_guard<std::mutex> g(R().m); return R().dir; }
bool Enabled() { std::lock_guard<std::mutex> g(R().m); return Sending(R().st); }
bool Asked() { std::lock_guard<std::mutex> g(R().m); return R().st.answer != ANSWER_UNASKED; }

static void Apply(bool on, bool answer)
{
    Runtime &r = R();
    std::lock_guard<std::mutex> g(r.m);
    if ( r.dir.empty() )
        return;
    if ( answer ) ApplyAnswer(r.st, on, r.rnd);
    else SetEnabled(r.st, on, r.rnd);
    r.gen++;
    if ( !Sending(r.st) )
    {
        // off: nothing unsent stays behind, on the device or in memory
        r.pending.clear();
        FileRemove(r.queuePath);
        FileRemove(r.crashPath);
    }
    SaveStateLocked(r);
    CrashHandlerEnabled(Sending(r.st));
    Debug("setting", Sending(r.st) ? "on (new install number)" : "off (install number deleted)");
}

void SetEnabledByUser(bool on) { Apply(on, false); }
void RecordAnswer(bool yes) { Apply(yes, true); }

void Launch(const char *preset, const std::string &gpuRaw)
{
    if ( !Enabled() )
        return;
    Record r;
    r.Set("event", "launch");
    r.Set("preset", preset ? preset : "original");
    r.Set("os", CleanOs(OsName()));
    r.Set("gpu", CleanGpu(gpuRaw));
    Enqueue(r);
}

void LevelStart(int level, const char *preset)
{
    SetCrashContext(preset, level);
    if ( !Enabled() )
        return;
    Record r;
    r.Set("event", "level_start");
    r.Set("preset", preset ? preset : "original");
    r.Set("level", Int(level));
    Enqueue(r);
}

void Error(const char *code, long detail, const char *preset, int level)
{
    Runtime &rt = R();
    std::lock_guard<std::mutex> g(rt.m);
    if ( !Sending(rt.st) || !code || !ValidErrorCode(code) || rt.errorsSent.count(code) )
        return;
    rt.errorsSent.insert(code);
    Record r;
    r.Set("event", "error");
    r.Set("code", code);
    if ( detail >= 0 ) r.Set("detail", Int(std::min(detail, 1000000000L)));
    if ( preset ) r.Set("preset", preset);
    if ( level >= 0 && level <= 9999 ) r.Set("level", Int(level));
    Item it;
    it.r = r;
    EnqueueLocked(rt, it);
}

void PerfBegin(int level)
{
    Runtime &r = R();
    r.perf.Reset();
    r.perfOn = true;
    r.perfLevel = level;
}

void PerfFrame(double ms)
{
    Runtime &r = R();
    if ( r.perfOn )
        r.perf.Add(ms);
}

bool PerfRunning() { return R().perfOn; }

void PerfEnd(const char *preset, int resW, int resH, int vrHz, bool quitting)
{
    Runtime &r = R();
    if ( !r.perfOn )
        return;
    r.perfOn = false;
    Record rec;
    if ( !Enabled() || !PerfRecord(r.perf, r.perfLevel, preset ? preset : "original", resW, resH, vrHz, &rec) )
        return;
    if ( quitting )
    {
        std::lock_guard<std::mutex> g(r.m);
        QueueAppendLocked(r, {rec}); // the app is closing: sent on the next start
        Debug("perf", "kept for the next start (quit)");
    }
    else
        Enqueue(rec);
}

void SetCrashContext(const char *preset, int level)
{
    CrashHandlerContext(preset, level);
}

void Shutdown()
{
    Runtime &r = R();
    std::unique_lock<std::mutex> lk(r.m);
    r.cv.wait_for(lk, std::chrono::milliseconds(1000), [&r] { return r.pending.empty() && !r.inFlight; });
    std::vector<Record> left;
    bool crash = false;
    for (const Item &it : r.pending)
    {
        left.push_back(it.r);
        crash = crash || it.crashFile;
    }
    r.pending.clear();
    QueueAppendLocked(r, left);
    if ( crash )
        FileRemove(r.crashPath); // it is in the queue now
    if ( !left.empty() )
        Debug("shutdown", "unsent events kept for the next start");
    r.shutdown = true;
}

} // namespace UsageStats
