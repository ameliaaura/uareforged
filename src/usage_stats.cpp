// Anonymous usage stats: state, records, body builder, perf maths and the background sender.
// Wire format: docs/usage-stats.md (v3). Platform parts: usage_stats_platform.cpp,
// usage_stats_net_apple.mm, usage_stats_crash.cpp.
#include "usage_stats.h"

#include <algorithm>
#include <atomic>
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

enum Kind
{
    K_PRESET, K_LEVEL, K_NAME40, K_NAME64, K_RES, K_DEC, K_INT, K_SIGNAL, K_MODULE, K_OFFSET, K_FRAMES, K_BUILD, K_CODE,
    K_ENUM,   // one of opts (a string)
    K_INTSET, // one of opts (an integer)
    K_CODES,  // array of error codes, lo..hi entries
    K_REF     // bug report reference
};

struct Spec
{
    const char *key;
    Kind kind;
    bool required;
    double lo, hi;           // numbers: range; K_FRAMES / K_CODES: entry count (hi 0 = 32)
    const char *const *opts = nullptr; // K_ENUM / K_INTSET: nullptr-terminated list
};

const char *const kRamOpts[] = {"2", "4", "8", "16", "32", "64", nullptr};
const char *const kCoreOpts[] = {"1", "2", "4", "6", "8", "12", "16", "24", "32", nullptr};
const char *const kDisplayOpts[] = {"sd", "720p", "1080p", "1440p", "4k", "5k+", nullptr};
const char *const kInputOpts[] = {"keyboard_mouse", "controller", "mixed", "vr_controllers", nullptr};
const char *const kResultOpts[] = {"won", "lost", "quit", "restart", nullptr};
const char *const kEndedOpts[] = {"quit", "killed", "crash", nullptr};

const Spec kLaunch[] = {
    {"preset", K_PRESET, true, 0, 0},       {"os", K_NAME40, true, 0, 0},          {"gpu", K_NAME64, true, 0, 0},
    {"ram_gb", K_INTSET, false, 0, 0, kRamOpts}, {"cpu_cores", K_INTSET, false, 0, 0, kCoreOpts},
    {"refresh_hz", K_INT, false, 1, 500},   {"display", K_ENUM, false, 0, 0, kDisplayOpts}};
const Spec kLevelStart[] = {
    {"preset", K_PRESET, true, 0, 0}, {"level", K_LEVEL, true, 0, 9999}, {"load_ms", K_INT, false, 0, 600000}};
const Spec kLevelEnd[] = {
    {"preset", K_PRESET, true, 0, 0},       {"level", K_LEVEL, true, 0, 9999},
    {"result", K_ENUM, true, 0, 0, kResultOpts}, {"duration_s", K_INT, true, 0, 86400},
    {"input", K_ENUM, false, 0, 0, kInputOpts},  {"vr_table_s", K_INT, false, 0, 86400},
    {"vr_unit_s", K_INT, false, 0, 86400},  {"vr_jumps", K_INT, false, 0, 100000}};
const Spec kSessionEnd[] = {
    {"preset", K_PRESET, true, 0, 0},           {"minutes", K_INT, true, 0, 100000},
    {"levels", K_INT, true, 0, 10000},          {"ended", K_ENUM, true, 0, 0, kEndedOpts},
    {"input", K_ENUM, false, 0, 0, kInputOpts}, {"look_switches", K_INT, true, 0, 1000},
    {"preset_changes", K_INT, true, 0, 1000}};
const Spec kBugReport[] = {
    {"ref", K_REF, true, 0, 0},                 {"preset", K_PRESET, true, 0, 0},
    {"level", K_LEVEL, false, 0, 9999},         {"minutes", K_INT, true, 0, 100000},
    {"input", K_ENUM, false, 0, 0, kInputOpts}, {"errors", K_CODES, false, 0, 10},
    {"crash_signal", K_SIGNAL, false, 0, 0},    {"crash_frames", K_FRAMES, false, 1, 5},
    {"fps_avg", K_DEC, false, 0, 1000},         {"fps_p1low", K_DEC, false, 0, 1000}};
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
    UAS_SPEC("level_end", kLevelEnd)
    UAS_SPEC("perf", kPerf)
    UAS_SPEC("session_end", kSessionEnd)
    UAS_SPEC("crash", kCrash)
    UAS_SPEC("error", kError)
    UAS_SPEC("bug_report", kBugReport)
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
    case K_CODES:
    {
        const size_t maxN = sp.hi > 0 ? (size_t)sp.hi : 32;
        std::string out = "[";
        size_t count = 0, a = 0;
        if ( !v.empty() )
        {
            for (;;)
            {
                size_t b = v.find(',', a);
                std::string f = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
                const bool ok = sp.kind == K_FRAMES ? ValidFrame(f) : ValidErrorCode(f);
                if ( !ok || ++count > maxN )
                    return "";
                if ( count > 1 ) out += ",";
                out += "\"" + f + "\"";
                if ( b == std::string::npos ) break;
                a = b + 1;
            }
        }
        if ( count < (size_t)sp.lo )
            return "";
        return out + "]";
    }
    case K_ENUM:
    case K_INTSET:
        for (const char *const *o = sp.opts; o && *o; o++)
            if ( v == *o )
                return sp.kind == K_ENUM ? "\"" + v + "\"" : v;
        return "";
    case K_REF: return ValidBugRef(v) ? "\"" + v + "\"" : "";
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
    std::string body = "{\"v\":3,\"install\":\"" + install + "\",\"app\":\"" + c.app + "\",\"platform\":\"" + c.platform +
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

// ------------------------------------------------------------------ v3 helpers
const char *const kFeedbackUrl = "https://uareforged.pages.dev/contribute/";

int BucketRamGb(long long mb)
{
    if ( mb <= 0 )
        return 0;
    // what the system reports is a little under the nominal size (memory kept by the system / the graphics chip):
    // anything from 85 % of a size up counts as that size (15.4 GB reported = 16, 7.4 GB = 8)
    static const int b[] = {64, 32, 16, 8, 4};
    for (int v : b)
        if ( mb * 100 >= (long long)v * 1024 * 85 ) return v;
    return 2;
}

int BucketCores(int n)
{
    if ( n <= 0 )
        return 0;
    static const int b[] = {32, 24, 16, 12, 8, 6, 4, 2};
    for (int v : b)
        if ( n >= v ) return v;
    return 1;
}

const char *DisplayBucket(int w, int h)
{
    if ( w <= 0 || h <= 0 )
        return "";
    if ( h < 720 ) return "sd";
    if ( h < 1080 ) return "720p";
    if ( h < 1440 ) return "1080p";
    if ( h < 2160 ) return "1440p";
    if ( h < 2880 ) return "4k";
    return "5k+";
}

const char *InputKind(uint32_t kbm, uint32_t pad, bool vr)
{
    if ( vr )
        return "vr_controllers";
    if ( !kbm && !pad )
        return "";
    const uint64_t lo = std::min(kbm, pad), hi = std::max(kbm, pad);
    if ( lo > 0 && lo * 10 >= hi )
        return "mixed";
    return kbm > pad ? "keyboard_mouse" : "controller";
}

static const char kRefAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789"; // 31: no i, l, o, 0, 1

std::string MakeBugRef(const uint8_t b[16])
{
    std::string r;
    for (int i = 0; i < 8; i++)
        r += kRefAlphabet[((unsigned)b[2 * i] << 8 | b[2 * i + 1]) % 31]; // 16 bits per character: bias < 0.05 %
    return r;
}

bool ValidBugRef(const std::string &ref)
{
    if ( ref.size() != 8 )
        return false;
    for (char c : ref)
        if ( !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) )
            return false;
    return true;
}

std::string BugReportUrl(const std::string &ref)
{
    return ValidBugRef(ref) ? std::string(kFeedbackUrl) + "?ref=" + ref + "#feedback" : std::string(kFeedbackUrl) + "#feedback";
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
typedef std::chrono::steady_clock Clock;

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
    std::string dir, statePath, queuePath, crashPath, sessionPath, url;
    Common c;
    std::set<std::string> errorsSent;
    std::vector<std::string> errorList;  // the same codes in the order sent (bug_report)
    SendFn send = nullptr;
    RandomFn rnd = nullptr;
    UrlOpenFn openUrl = nullptr;
    // the crash reported at this start (bug_report)
    std::string lastCrashSignal, lastCrashFrames;
    // session file: written by the worker (once a minute), never after shutdown
    std::string sessionText;
    bool sessionDirty = false;
    std::mutex sessionFileM;
    std::atomic<bool> sessionClosed{false};

    // ---- main thread only (no lock)
    PerfHist perf;
    bool perfOn = false;
    int perfLevel = -1;
    // mission
    bool missionOn = false, missionStarted = false;
    int missionLevel = -1;
    char missionPreset[16] = "original";
    bool reqValid = false;
    Clock::time_point reqT, missionT0;
    uint32_t mKbm0 = 0, mPad0 = 0;
    double mTableMs = 0, mUnitMs = 0;
    uint32_t mJumps = 0;
    uintptr_t mUnit = 0;
    // last perf of this session (bug_report)
    std::string lastFpsAvg, lastFpsLow;
    int lastLevel = -1;
    // session
    Clock::time_point sessionT0, nextCheckpoint;
    uint32_t sessionLevels = 0, lookSwitches = 0, presetChanges = 0;
    char sessionPreset[16] = "";
    int bugReports = 0;

    // ---- any thread
    std::atomic<uint32_t> inKbm{0}, inPad{0};
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

void CopyPreset(char out[16], const char *preset)
{
    snprintf(out, 16, "%s", preset && preset[0] ? preset : "original");
}

bool IsVr(const Runtime &r) { return r.c.app == "vr"; }

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

// the session file: "" = remove it. Serialised with Shutdown / turning off by sessionFileM.
void WriteSessionFile(Runtime &r, const std::string &text)
{
    std::lock_guard<std::mutex> g(r.sessionFileM);
    if ( r.sessionClosed.load() || r.sessionPath.empty() )
        return;
    if ( text.empty() )
        FileRemove(r.sessionPath);
    else
    {
        MakeDir(r.dir);
        FileWriteAtomic(r.sessionPath, text);
    }
}

void Worker()
{
    Runtime &r = R();
    std::unique_lock<std::mutex> lk(r.m);
    for (;;)
    {
        r.cv.wait(lk, [&r] { return !r.pending.empty() || r.sessionDirty; });
        if ( r.sessionDirty )
        {
            r.sessionDirty = false;
            const std::string text = Sending(r.st) ? r.sessionText : std::string();
            lk.unlock();
            WriteSessionFile(r, text);
            lk.lock();
            continue;
        }
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

void StartWorkerLocked(Runtime &r)
{
    if ( !r.worker )
    {
        r.worker = true;
        std::thread(Worker).detach();
    }
    r.cv.notify_all();
}

void EnqueueLocked(Runtime &r, const Item &it)
{
    if ( !Sending(r.st) || r.shutdown )
        return;
    r.pending.push_back(it);
    while ( r.pending.size() > kQueueMax )
        r.pending.pop_front();
    StartWorkerLocked(r);
}

void Enqueue(const Record &rec)
{
    Runtime &r = R();
    std::lock_guard<std::mutex> g(r.m);
    Item it;
    it.r = rec;
    EnqueueLocked(r, it);
}

// send now, or (the app is closing) keep it in the queue for the next start
void SendOrKeep(const Record &rec, bool quitting)
{
    Runtime &r = R();
    if ( quitting )
    {
        std::lock_guard<std::mutex> g(r.m);
        QueueAppendLocked(r, {rec});
        Debug(rec.Get("event"), "kept for the next start (quit)");
    }
    else
        Enqueue(rec);
}

long MinutesSince(Clock::time_point t0)
{
    const long m = (long)std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - t0).count();
    return std::max(0L, std::min(100000L, m));
}

const char *SessionInput(const Runtime &r)
{
    return InputKind(r.inKbm.load(std::memory_order_relaxed), r.inPad.load(std::memory_order_relaxed), IsVr(r));
}

// this session as a session_end record (ended = quit; a leftover file is changed to killed / crash at the next start)
Record SessionRecord(const Runtime &r)
{
    Record rec;
    rec.Set("event", "session_end");
    rec.Set("version", r.c.version);
    rec.Set("preset", r.sessionPreset[0] ? r.sessionPreset : "original");
    rec.Set("minutes", Int(MinutesSince(r.sessionT0)));
    rec.Set("levels", Int(std::min<uint32_t>(10000, r.sessionLevels)));
    rec.Set("ended", "quit");
    const char *in = SessionInput(r);
    if ( in[0] ) rec.Set("input", in);
    rec.Set("look_switches", Int(std::min<uint32_t>(1000, r.lookSwitches)));
    rec.Set("preset_changes", Int(std::min<uint32_t>(1000, r.presetChanges)));
    return rec;
}

// hand the session file to the worker (main thread; once a minute and at mission ends)
void Checkpoint()
{
    Runtime &r = R();
    Record rec = SessionRecord(r);
    rec.Set("ended", "killed");
    const std::string line = FormatRecordLine(rec) + "\n";
    std::lock_guard<std::mutex> g(r.m);
    if ( !Sending(r.st) || r.shutdown || r.sessionPath.empty() )
        return;
    r.sessionText = line;
    r.sessionDirty = true;
    StartWorkerLocked(r);
}
} // namespace

void SetSender(SendFn fn) { std::lock_guard<std::mutex> g(R().m); R().send = fn; }
void SetRandom(RandomFn fn) { std::lock_guard<std::mutex> g(R().m); R().rnd = fn; }
void SetUrlOpener(UrlOpenFn fn) { std::lock_guard<std::mutex> g(R().m); R().openUrl = fn; }

void Init(const std::string &dirIn, const char *app, const char *platform, const char *version, const char *build)
{
    Runtime &r = R();
    std::lock_guard<std::mutex> g(r.m);
    if ( r.inited )
        return;
    r.inited = true;
    r.sessionT0 = Clock::now();
    r.nextCheckpoint = r.sessionT0; // the first frame writes the session file, then once a minute
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
    r.sessionPath = PathJoin(r.dir, "usage-stats-session.txt");

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
    const bool crashed = FileRead(r.crashPath, &crash, 16384);
    if ( crashed )
    {
        Record rec;
        if ( on && ParseRecordLine(crash, &rec) && rec.Get("event") == "crash" )
        {
            Item it;
            it.r = rec;
            it.crashFile = true;
            EnqueueLocked(r, it);
            r.lastCrashSignal = rec.Get("signal");
            const std::string fr = rec.Get("frames");
            size_t cut = 0;
            for (int n = 0; n < 5 && cut != std::string::npos; n++)
                cut = fr.find(',', cut ? cut + 1 : 0);
            r.lastCrashFrames = fr.substr(0, cut);
        }
        else
            FileRemove(r.crashPath);
    }
    // the previous session if it didn't close normally (killed by the system, force-quit, crashed)
    std::string sess;
    if ( FileRead(r.sessionPath, &sess, 4096) )
    {
        FileRemove(r.sessionPath);
        Record rec;
        if ( on && ParseRecordLine(sess, &rec) && rec.Get("event") == "session_end" )
        {
            rec.Set("ended", crashed ? "crash" : "killed");
            Item it;
            it.r = rec;
            EnqueueLocked(r, it);
        }
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
                    r.errorList.push_back(rec.Get("code"));
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
    {
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
            r.sessionDirty = false;
            FileRemove(r.queuePath);
            FileRemove(r.crashPath);
        }
        SaveStateLocked(r);
        CrashHandlerEnabled(Sending(r.st));
        Debug("setting", Sending(r.st) ? "on (new install number)" : "off (install number deleted)");
    }
    if ( !on )
        WriteSessionFile(r, std::string());
}

void SetEnabledByUser(bool on) { Apply(on, false); }
void RecordAnswer(bool yes) { Apply(yes, true); }

void Launch(const char *preset, const std::string &gpuRaw) { Launch(preset, gpuRaw, Hardware()); }

void Launch(const char *preset, const std::string &gpuRaw, const Hardware &hw)
{
    Runtime &rt = R();
    if ( !rt.sessionPreset[0] )
        CopyPreset(rt.sessionPreset, preset);
    if ( !Enabled() )
        return;
    Record r;
    r.Set("event", "launch");
    r.Set("preset", preset ? preset : "original");
    r.Set("os", CleanOs(OsName()));
    r.Set("gpu", CleanGpu(gpuRaw));
    if ( hw.ramGb > 0 ) r.Set("ram_gb", Int(hw.ramGb));
    if ( hw.cores > 0 ) r.Set("cpu_cores", Int(hw.cores));
    if ( hw.refreshHz > 0 ) r.Set("refresh_hz", Int(std::min(500, hw.refreshHz)));
    if ( hw.display && hw.display[0] ) r.Set("display", hw.display);
    Enqueue(r);
}

void LevelStart(int level, const char *preset, long loadMs)
{
    SetCrashContext(preset, level);
    if ( !Enabled() )
        return;
    Record r;
    r.Set("event", "level_start");
    r.Set("preset", preset ? preset : "original");
    r.Set("level", Int(level));
    if ( loadMs >= 0 ) r.Set("load_ms", Int(std::min(600000L, loadMs)));
    Enqueue(r);
}

void Error(const char *code, long detail, const char *preset, int level)
{
    Runtime &rt = R();
    std::lock_guard<std::mutex> g(rt.m);
    if ( !Sending(rt.st) || !code || !ValidErrorCode(code) || rt.errorsSent.count(code) )
        return;
    rt.errorsSent.insert(code);
    rt.errorList.push_back(code);
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
    if ( !PerfRecord(r.perf, r.perfLevel, preset ? preset : "original", resW, resH, vrHz, &rec) )
        return;
    r.lastFpsAvg = rec.Get("fps_avg");
    r.lastFpsLow = rec.Get("fps_p1low");
    if ( Enabled() )
        SendOrKeep(rec, quitting);
}

// ------------------------------------------------------------------ missions
void MissionRequested()
{
    Runtime &r = R();
    r.reqT = Clock::now();
    r.reqValid = true;
}

void MissionBegin(int level, const char *preset)
{
    Runtime &r = R();
    PerfBegin(level);
    r.missionOn = true;
    r.missionStarted = false;
    r.missionLevel = level;
    r.lastLevel = level;
    CopyPreset(r.missionPreset, preset);
    r.mKbm0 = r.inKbm.load(std::memory_order_relaxed);
    r.mPad0 = r.inPad.load(std::memory_order_relaxed);
    r.mTableMs = r.mUnitMs = 0;
    r.mJumps = 0;
    r.mUnit = 0;
    SetCrashContext(r.missionPreset, level);
}

void MissionFrame(double ms, int vrView, uintptr_t unit)
{
    Runtime &r = R();
    if ( !r.missionOn )
        return;
    if ( !r.missionStarted )
    {
        // the first playable frame: level_start with the loading time (once per mission)
        r.missionStarted = true;
        r.missionT0 = Clock::now();
        if ( r.sessionLevels < 100000 ) r.sessionLevels++;
        long loadMs = -1;
        if ( r.reqValid )
            loadMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(r.missionT0 - r.reqT).count();
        r.reqValid = false;
        LevelStart(r.missionLevel, r.missionPreset, loadMs);
        Checkpoint(); // the session file counts this mission even if the app is killed during it
    }
    if ( ms >= 0 )
    {
        if ( r.perfOn )
            r.perf.Add(ms);
        if ( vrView == VIEW_TABLE ) r.mTableMs += ms;
        else if ( vrView == VIEW_UNIT ) r.mUnitMs += ms;
    }
    if ( vrView != VIEW_NONE && unit && unit != r.mUnit )
    {
        if ( r.mUnit && vrView == VIEW_UNIT && r.mJumps < 100000 )
            r.mJumps++; // into a (different) unit; back to the host station is not a jump
        r.mUnit = unit;
    }
}

bool MissionRunning() { return R().missionOn; }

void MissionEnd(const char *result, const char *preset, int resW, int resH, int vrHz, bool quitting)
{
    Runtime &r = R();
    if ( !r.missionOn )
        return;
    r.missionOn = false;
    const char *p = preset ? preset : "original";
    PerfEnd(p, resW, resH, vrHz, quitting);
    SetCrashContext(p, -1);
    if ( !r.missionStarted || !result )
        return; // never reached a playable frame: no level_end (level_load_failed covers that)
    Record rec;
    rec.Set("event", "level_end");
    rec.Set("preset", p);
    rec.Set("level", Int(r.missionLevel));
    rec.Set("result", result);
    const long dur = (long)std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - r.missionT0).count();
    rec.Set("duration_s", Int(std::max(0L, std::min(86400L, dur))));
    const char *in = InputKind(r.inKbm.load(std::memory_order_relaxed) - r.mKbm0, r.inPad.load(std::memory_order_relaxed) - r.mPad0, IsVr(r));
    if ( in[0] ) rec.Set("input", in);
    if ( IsVr(r) )
    {
        rec.Set("vr_table_s", Int(std::min(86400L, (long)(r.mTableMs / 1000.0 + 0.5))));
        rec.Set("vr_unit_s", Int(std::min(86400L, (long)(r.mUnitMs / 1000.0 + 0.5))));
        rec.Set("vr_jumps", Int(r.mJumps));
    }
    if ( Enabled() )
        SendOrKeep(rec, quitting);
    if ( !quitting )
        Checkpoint();
}

// ------------------------------------------------------------------ session
void NoteInput(InputSource s)
{
    Runtime &r = R();
    (s == INPUT_PAD ? r.inPad : r.inKbm).fetch_add(1, std::memory_order_relaxed);
}

void NotePreset(const char *preset)
{
    Runtime &r = R();
    if ( !preset || !preset[0] )
        return;
    if ( !r.sessionPreset[0] )
    {
        CopyPreset(r.sessionPreset, preset); // the first value: not a change
        return;
    }
    if ( !strcmp(r.sessionPreset, preset) )
        return;
    const bool wasOriginal = !strcmp(r.sessionPreset, "original"), isOriginal = !strcmp(preset, "original");
    if ( r.presetChanges < 100000 ) r.presetChanges++;
    if ( wasOriginal != isOriginal && r.lookSwitches < 100000 ) r.lookSwitches++;
    CopyPreset(r.sessionPreset, preset);
}

void SessionTick()
{
    Runtime &r = R();
    if ( !r.inited )
        return;
    const Clock::time_point now = Clock::now();
    if ( now < r.nextCheckpoint )
        return;
    r.nextCheckpoint = now + std::chrono::seconds(60);
    Checkpoint();
}

// ------------------------------------------------------------------ bug report
std::string ReportBug(const char *preset, int level)
{
    Runtime &rt = R();
    uint8_t b[16];
    RandomFn rnd;
    UrlOpenFn open;
    {
        std::lock_guard<std::mutex> g(rt.m);
        rnd = rt.rnd ? rt.rnd : OsRandom;
        open = rt.openUrl;
    }
    if ( rt.bugReports >= 5 || !rnd(b) )
    {
        if ( open ) open(BugReportUrl(""));
        return "";
    }
    rt.bugReports++;
    const std::string ref = MakeBugRef(b);
    if ( Enabled() )
    {
        Record r;
        r.Set("event", "bug_report");
        r.Set("ref", ref);
        r.Set("preset", preset ? preset : "original");
        const int lv = level >= 0 ? level : rt.lastLevel;
        if ( lv >= 0 && lv <= 9999 ) r.Set("level", Int(lv));
        r.Set("minutes", Int(MinutesSince(rt.sessionT0)));
        const char *in = SessionInput(rt);
        if ( in[0] ) r.Set("input", in);
        {
            std::lock_guard<std::mutex> g(rt.m);
            std::string codes;
            for (size_t i = 0; i < rt.errorList.size() && i < 10; i++)
                codes += (i ? "," : "") + rt.errorList[i];
            if ( !codes.empty() ) r.Set("errors", codes);
            if ( !rt.lastCrashSignal.empty() ) r.Set("crash_signal", rt.lastCrashSignal);
            if ( !rt.lastCrashFrames.empty() ) r.Set("crash_frames", rt.lastCrashFrames);
        }
        if ( !rt.lastFpsAvg.empty() ) r.Set("fps_avg", rt.lastFpsAvg);
        if ( !rt.lastFpsLow.empty() ) r.Set("fps_p1low", rt.lastFpsLow);
        Enqueue(r);
        Debug("bug_report", "queued with the report reference");
    }
    if ( open && !open(BugReportUrl(ref)) )
        Debug("bug_report", "the browser could not be opened");
    return ref;
}

void SetCrashContext(const char *preset, int level)
{
    CrashHandlerContext(preset, level);
}

void Shutdown()
{
    Runtime &r = R();
    // this session, closed normally: sent now or kept in the queue below; the session file goes
    const Record sess = SessionRecord(r);
    if ( r.inited && Enabled() )
        Enqueue(sess);
    r.sessionClosed = true;
    {
        std::lock_guard<std::mutex> g(r.sessionFileM);
        if ( !r.sessionPath.empty() )
            FileRemove(r.sessionPath);
    }
    std::unique_lock<std::mutex> lk(r.m);
    r.sessionDirty = false;
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
