// The headset's start screens and first-launch downloads; see setup_flow.h.
//
// Layout of the app's own storage (<external files>/ua, the engine's working folder):
//   DATA, LEVELS, ...   the original game (from the downloaded package, or copied over USB by the player)
//   res/, fonts/        the engine's own files, unpacked from the APK (res always the app's; fonts: the game's
//                       copies of the same names win, as on the desktop)
//   hd-models/ ui/ ground/   the Quest pack: the Reforge pack's files the headset uses (no BC/DDS maps, sharp-texture
//                       pack or restored videos) + ASTC textures; tools/vr/make_quest_full_pack.py builds it
//   uavr.cfg            written here at every start; uavr-user.cfg (optional, the player's) wins over it
//   .setup/             state + partial downloads (kept for a resume after Cancel, removed after a failure)
//
// Failsafe (the same rules as packaging/macos/downloads.sh): every link is checked first (a 1 KB ranged GET:
// must answer 2xx, must not be a web page, must start like a zip, must have a sane size); a stall of 30 s or a
// dropped connection is retried twice, resuming where the server allows it; pack parts must match their SHA-256;
// zips are checked entry by entry (CRC); the game must have DATA and LEVELS with the key files. Any break cleans
// up the partial files and shows a plain notice with "Open help page" and another way on.
//
// Test switches (environment, e.g. through the debug.uavr.env property; the same names as the desktop app):
//   UA_LINKS_URL, UA_GAME_URL, UA_PACK_URL   other addresses; UA_LINKS_ALLOW_LOCAL=1 also accepts
//                                            http://127.0.0.1 / localhost (the manifest allows cleartext only there)
//   UA_GAME_AUTO=get|copy|quit  UA_PACK_AUTO=yes|no|never  UA_NOTICE_AUTO=help|later  UA_CAMPAIGN=base|ghor|taer
//   (a game notice answered by a test run stops the app, as on the desktop; a pack notice starts the game)
//   UA_SETUP_EXIT=1   stop after the setup screens instead of starting the game
//   UA_SETUP=skip     no screens at all when the game is present (benchmarks)
#include "setup/setup_flow.h"
#include "usage_stats.h"

#include <android/asset_manager.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GLES3/gl3.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>

#include "common/log.h"
#include "platform/android/ua_host.h"
#include "platform/xr/xr_platform.h"
#include "setup/setup_core.h"

#ifndef UAVR_APP_VERSION
#define UAVR_APP_VERSION "0.9.0"
#endif

namespace uavr {
namespace setup {

namespace {

const char* kHelpGame = "https://uareforged.pages.dev/downloads/#nogame-h";
const char* kHelpPack = "https://uareforged.pages.dev/downloads/#install-h";

std::string Env(const char* k) {
    const char* v = getenv(k);
    return v ? v : "";
}

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string ReadAsset(android_app* app, const char* path) {
    AAsset* a = AAssetManager_open(app->activity->assetManager, path, AASSET_MODE_BUFFER);
    if (!a) return "";
    std::string s((const char*)AAsset_getBuffer(a), (size_t)AAsset_getLength(a));
    AAsset_close(a);
    return s;
}

bool WriteFile(const std::string& path, const std::string& data) {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) MakeDirs(path.substr(0, slash));
    std::string tmp = path + ".part";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(data.data(), (std::streamsize)data.size());
        if (!o) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string ReadFile(const std::string& path) {
    std::ifstream i(path, std::ios::binary);
    std::stringstream s;
    s << i.rdbuf();
    return s.str();
}

bool EndsWithCI(const std::string& s, const char* t) {
    size_t n = strlen(t);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, t);
}

bool CopyFile(const std::string& from, const std::string& to) {
    std::ifstream i(from, std::ios::binary);
    if (!i) return false;
    std::ofstream o(to, std::ios::binary | std::ios::trunc);
    o << i.rdbuf();
    return (bool)o;
}

// Moves every entry of src into dst (folders merged, files replaced). `skipTop` names are left out at the top.
bool MergeMove(const std::string& src, const std::string& dst, const std::vector<std::string>& skipTop, bool top) {
    DIR* d = opendir(src.c_str());
    if (!d) return false;
    bool ok = true;
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    MakeDirs(dst);
    for (const std::string& n : names) {
        if (top) {
            bool skip = false;
            for (const std::string& s : skipTop) if (!strcasecmp(s.c_str(), n.c_str())) skip = true;
            if (skip) continue;
        }
        std::string s = src + "/" + n, t = dst + "/" + n;
        if (IsDir(s)) {
            if (IsDir(t)) ok = MergeMove(s, t, skipTop, false) && ok;
            else ok = rename(s.c_str(), t.c_str()) == 0 && ok;
        } else {
            ok = rename(s.c_str(), t.c_str()) == 0 && ok;
        }
    }
    return ok;
}

// ---------------------------------------------------------------- the engine's own files from the APK
// assets/ua-assets.txt lists every file under assets/ua/ (relative paths). res/ is always the app's copy;
// fonts/ files are only added when missing (the game's own copies of the same names win, as on the desktop).
void ExtractEngineAssets(android_app* app, const std::string& data, const std::string& setupDir) {
    std::string list = ReadAsset(app, "ua-assets.txt");
    if (list.empty()) { LOGE("setup: ua-assets.txt missing from the APK"); return; }
    Sha256 h;
    h.update(list.data(), list.size());
    std::string stamp = h.hex();
    if (ReadFile(setupDir + "/assets-stamp") == stamp && IsDir(data + "/res")) return;
    std::istringstream in(list);
    std::string rel;
    int n = 0;
    while (std::getline(in, rel)) {
        if (rel.empty() || rel[0] == '#') continue;
        std::string dst = data + "/" + rel;
        bool font = !rel.compare(0, 6, "fonts/");
        if (font && IsFile(dst)) continue;
        std::string blob = ReadAsset(app, ("ua/" + rel).c_str());
        if (!WriteFile(dst, blob)) LOGE("setup: could not write %s", dst.c_str());
        else n++;
    }
    WriteFile(setupDir + "/assets-stamp", stamp);
    LOGI("setup: engine files unpacked (%d)", n);
}

// ---------------------------------------------------------------- worker thread
struct Worker {
    std::thread t;
    std::atomic<bool> cancel{false};
    std::mutex m;
    double frac = 0;
    std::string status, why;
    bool running = false, done = false;
    int rc = 0;  // 0 ok, 1 failed (why), 2 cancelled

    void start(const std::function<int(Worker&)>& fn) {
        join();
        cancel = false;
        {
            std::lock_guard<std::mutex> l(m);
            running = true; done = false; frac = 0; status.clear(); why.clear(); rc = 0;
        }
        t = std::thread([this, fn] {
            int r = fn(*this);
            std::lock_guard<std::mutex> l(m);
            rc = r; done = true; running = false;
        });
    }
    void set(double f, const std::string& s) {
        std::lock_guard<std::mutex> l(m);
        frac = f; status = s;
    }
    int fail(const std::string& w) {
        std::lock_guard<std::mutex> l(m);
        why = w;
        return 1;
    }
    void join() { if (t.joinable()) t.join(); }
};

struct Ctx {
    android_app* app = nullptr;
    JavaVM* vm = nullptr;
    std::string data, setupDir, dlGame, dlPack;
    Links L;
    bool linksDone = false, allowLocal = false;
};

// ---------------------------------------------------------------- links, link check, download
void ApplyEnvOverrides(Ctx& c) {
    if (!Env("UA_GAME_URL").empty()) c.L.gameUrl = Env("UA_GAME_URL");
    if (!Env("UA_PACK_URL").empty()) c.L.packUrl = Env("UA_PACK_URL");
    if (!c.L.packUrl.empty() && c.L.packUrl.back() != '/') c.L.packUrl += '/';
}

void LoadLinks(Ctx& c, Http& http) {
    if (c.linksDone) return;
    std::string src = Env("UA_LINKS_URL");
    if (src.empty()) src = c.L.linksUrl;
    std::string w;
    if (!src.empty() && UrlAllowed(src, c.L.allowHosts, c.allowLocal, &w)) {
        std::string body;
        HttpResponse r;
        bool ok = http.get(src, -1, -1, 8000, nullptr, [&](const uint8_t* p, size_t n) {
            body.append((const char*)p, n);
            return body.size() <= 16384;
        }, &r);
        if (ok) {
            if (ApplyLinksJson(body, c.allowLocal, &c.L, &w)) LOGI("setup: links: %s accepted", src.c_str());
            else {
                LOGI("setup: links: %s rejected (%s); built-in links used", src.c_str(), w.c_str());
                UsageStats::Error("links_json_invalid");
            }
        } else {
            LOGI("setup: links: %s not reachable (%s); built-in links used", src.c_str(), r.errText.c_str());
        }
    } else if (!src.empty()) {
        LOGI("setup: links: %s refused (%s); built-in links used", src.c_str(), w.c_str());
    }
    ApplyEnvOverrides(c);
    c.linksDone = true;
}

struct Check {
    uint64_t size = 0;
    bool ranges = false;
    std::string why;
};

bool Precheck(Http& h, const std::string& url, uint64_t min, uint64_t max, uint64_t exact, Check* ck) {
    *ck = Check();
    std::string head;
    HttpResponse r;
    bool ok = h.get(url, 0, 1023, 20000, nullptr, [&](const uint8_t* p, size_t n) {
        head.append((const char*)p, std::min(n, 1024 - head.size()));
        return head.size() < 1024;
    }, &r);
    if (!ok && r.err != NetErr::Cancelled) {
        ck->why = r.err == NetErr::Http ? "the server answered HTTP " + std::to_string(r.code) + " instead of the file"
                                        : NetErrText(r.err, false);
        LOGI("setup: link check: %s -> %s (%s)", url.c_str(), ck->why.c_str(), r.errText.c_str());
        return false;
    }
    if (r.code != 200 && r.code != 206) {
        ck->why = "the server answered HTTP " + std::to_string(r.code) + " instead of the file";
    } else if (!r.contentType.compare(0, 9, "text/html") || !r.contentType.compare(0, 17, "application/xhtml")) {
        ck->why = "the link now leads to a web page, not to the file";
    } else if (head.size() < 4 || memcmp(head.data(), "PK\x03\x04", 4) != 0) {
        ck->why = "the link does not lead to a zip file";
    }
    if (ck->why.empty()) {
        if (r.code == 206) {
            ck->ranges = true;
            size_t slash = r.contentRange.rfind('/');
            if (slash != std::string::npos) ck->size = strtoull(r.contentRange.c_str() + slash + 1, nullptr, 10);
        } else if (r.contentLength > 0) {
            ck->size = (uint64_t)r.contentLength;
        }
        if (ck->size > 0) {
            if (min && ck->size < min) ck->why = "the file on the server is far too small (" + HumanBytes(ck->size) + ")";
            if (max && ck->size > max) ck->why = "the file on the server is far too big (" + HumanBytes(ck->size) + ")";
            if (exact && ck->size != exact)
                ck->why = "the file on the server is not the expected one (size " + std::to_string(ck->size) +
                          ", expected " + std::to_string(exact) + ")";
        }
    }
    LOGI("setup: link check: %s -> %s (HTTP %d, %s, size %llu, ranges %d)", url.c_str(),
         ck->why.empty() ? "ok" : ck->why.c_str(), r.code, r.contentType.c_str(), (unsigned long long)ck->size,
         (int)ck->ranges);
    return ck->why.empty();
}

// 0 ok, 1 failed (why), 2 cancelled. Two automatic retries after a stall or a dropped connection, resuming
// when the server allows ranges.
int Fetch(Worker& w, Http& h, const std::string& url, const std::string& file, bool ranges, uint64_t maxSize,
          uint64_t base, uint64_t total, const std::string& what, std::string* why) {
    for (int attempt = 0;;) {
        if (!ranges) unlink(file.c_str());
        uint64_t have = ranges ? FileSize(file) : 0;
        int fd = -1;
        uint64_t got = have;
        bool ioErr = false, tooBig = false;
        auto t0 = std::chrono::steady_clock::now();
        HttpResponse r;
        bool ok = h.get(url, have > 0 ? (int64_t)have : -1, -1, 30000,
            [&](const HttpResponse& hr) {
                const bool append = have > 0 && hr.code == 206;
                if (!append) got = 0;
                fd = open(file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0660);
                if (fd < 0) ioErr = true;
                return fd >= 0;
            },
            [&](const uint8_t* p, size_t n) {
                if (w.cancel) return false;
                while (n) {
                    ssize_t k = write(fd, p, n);
                    if (k <= 0) { ioErr = true; return false; }
                    p += k; n -= (size_t)k; got += (uint64_t)k;
                }
                if (maxSize && got > maxSize) { tooBig = true; return false; }
                auto now = std::chrono::steady_clock::now();
                if (now - t0 > std::chrono::milliseconds(200)) {
                    t0 = now;
                    if (total) w.set((double)(base + got) * 0.9 / (double)total, what + ": " + HumanBytes(base + got) + " of " + HumanBytes(total));
                    else w.set(0.05, what + ": " + HumanBytes(got));
                }
                return true;
            }, &r);
        if (fd >= 0) close(fd);
        if (ok) return 0;
        if (w.cancel) return 2;
        if (ioErr) { *why = NetErrText(NetErr::Io, true); return 1; }
        if (tooBig) { *why = "the file on the server is far too big"; return 1; }
        if (r.err == NetErr::Http) {
            *why = "the server stopped serving the file (HTTP " + std::to_string(r.code) + ")";
            if (r.code == 416) unlink(file.c_str());
        } else {
            *why = NetErrText(r.err, true);
        }
        attempt++;
        LOGI("setup: download: %s -> %s (attempt %d; %s)", url.c_str(), why->c_str(), attempt, r.errText.c_str());
        if (attempt > 2) return 1;
        if (r.err == NetErr::Http && FileSize(file) == 0) return 1;
        for (int i = 0; i < 20 && !w.cancel; i++) Sleep(100);
        if (w.cancel) return 2;
    }
}

// ---------------------------------------------------------------- Get the game
std::string FindGameRoot(const std::string& dir, int depth) {
    if (GameValid(dir)) return dir;
    if (depth <= 0) return "";
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::vector<std::string> subs;
    while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && IsDir(dir + "/" + e->d_name)) subs.push_back(e->d_name);
    closedir(d);
    for (const std::string& s : subs) {
        std::string r = FindGameRoot(dir + "/" + s, depth - 1);
        if (!r.empty()) return r;
    }
    return "";
}

int GameGet(Worker& w, Ctx& c) {
    Http h(c.vm);
    w.set(0, "Checking the download link...");
    LoadLinks(c, h);
    auto broken = [&](const std::string& why) {
        RemoveTree(c.dlGame);
        return w.fail(why);
    };
    MakeDirs(c.dlGame);
    if (FreeBytes(c.data) < 1500000000ULL) return broken("there is not enough free space on the headset (it needs about 1.5 GB)");
    Check ck;
    if (!Precheck(h, c.L.gameUrl, c.L.gameMin, c.L.gameMax, 0, &ck)) return broken(ck.why);
    std::string f = c.dlGame + "/openua-package.zip", why;
    int rc = Fetch(w, h, c.L.gameUrl, f, ck.ranges, c.L.gameMax, 0, ck.size, "Downloading the game", &why);
    if (rc == 2) { LOGI("setup: get the game: cancelled (the part stays for a resume)"); return 2; }
    if (rc != 0) return broken(why);
    uint64_t got = FileSize(f);
    if (got < c.L.gameMin) return broken("the downloaded file is far too small (" + HumanBytes(got) + ")");
    w.set(0.92, "Unpacking the game...");
    ZipReader z;
    std::string unpack = c.dlGame + "/unpack";
    RemoveTree(unpack);
    if (!z.open(f, &why)) return broken("the downloaded file is damaged (it does not unpack)");
    size_t i = 0, n = z.entries().size();
    for (const ZipEntry& e : z.entries()) {
        if (w.cancel) { RemoveTree(unpack); return 2; }
        if (++i % 100 == 0) w.set(0.92 + 0.05 * (double)i / (double)n, "Unpacking the game... " + std::to_string(i * 100 / n) + " %");
        if (!SafeZipPath(e.name)) return broken("the downloaded file is damaged (it does not unpack)");
        if (e.dir) { MakeDirs(unpack + "/" + e.name); continue; }
        // Windows programs and libraries are no use on the headset
        if (EndsWithCI(e.name, ".exe") || EndsWithCI(e.name, ".dll")) continue;
        if (!z.extract(e, unpack + "/" + e.name, &why)) {
            LOGI("setup: unpack: %s", why.c_str());
            return broken("the downloaded file is damaged (it does not unpack)");
        }
    }
    std::string root = FindGameRoot(unpack, 4);
    if (root.empty()) return broken("the download has no complete DATA and LEVELS folders");
    w.set(0.98, "Setting up...");
    // res/ stays the app's own (the engine needs its versions of those files)
    if (!MergeMove(root, c.data, {"res", ".setup"}, true)) return broken("the game folder could not be created");
    RemoveTree(c.dlGame);
    WriteFile(c.setupDir + "/game-source", "download\n");
    LOGI("setup: get the game: installed in %s", c.data.c_str());
    return 0;
}

// ---------------------------------------------------------------- Reforge pack
// The headset keeps hd-models (minus the BC/DDS maps), ui and ground, plus the ASTC maps. The Quest pack holds only
// these (tools/vr/make_quest_full_pack.py); the filter stays as a guard (and for a pack zip with other folders).
bool WantPackEntry(const std::string& rel) {
    if (!rel.compare(0, 10, "hd-models/")) return !EndsWithCI(rel, ".dds");
    if (!rel.compare(0, 3, "ui/") || !rel.compare(0, 7, "ground/")) return true;
    return rel == "LINKS.tsv" || rel == "LICENSE.txt" || rel == "README.md" || rel == "PROVENANCE.md" ||
           rel == "QUEST-PACK.txt";
}

int PackGet(Worker& w, Ctx& c) {
    Http h(c.vm);
    w.set(0, "Checking the download links...");
    LoadLinks(c, h);
    auto broken = [&](const std::string& why) {
        RemoveTree(c.dlPack);
        return w.fail(why);
    };
    if (!c.L.packUrlSet() || c.L.questParts.empty()) return w.fail("this build has no download address for the pack");
    // The Quest pack only: the desktop parts (pack_parts) are for Mac/PC.
    const std::vector<PackPart> parts = c.L.questParts;
    uint64_t total = 0;
    for (auto& p : parts) total += p.bytes;
    MakeDirs(c.dlPack);
    if (FreeBytes(c.data) < total + 1500000000ULL)
        return broken("there is not enough free space on the headset (it needs about " + HumanBytes(total + 1500000000ULL) + ")");
    Check ck;
    for (auto& p : parts) {  // every link first
        if (IsFile(c.dlPack + "/" + p.name + ".ok")) continue;
        if (!Precheck(h, c.L.packUrl + p.name, 0, 0, p.bytes, &ck)) return broken(ck.why);
    }
    uint64_t doneB = 0;
    for (auto& p : parts) {
        std::string f = c.dlPack + "/" + p.name, why;
        if (IsFile(f + ".ok") && IsFile(f)) { doneB += p.bytes; continue; }
        if (FileSize(f) > p.bytes) unlink(f.c_str());
        LOGI("setup: reforge pack: downloading %s%s", c.L.packUrl.c_str(), p.name.c_str());
        if (!Precheck(h, c.L.packUrl + p.name, 0, 0, p.bytes, &ck)) return broken(ck.why);
        if (FileSize(f) < p.bytes) {
            int rc = Fetch(w, h, c.L.packUrl + p.name, f, ck.ranges, p.bytes, doneB, total, "Downloading the Reforge pack", &why);
            if (rc == 2) return 2;
            if (rc != 0) return broken(why);
        }
        w.set((double)(doneB + p.bytes) * 0.9 / (double)total, "Checking " + p.name + "...");
        std::string sum = Sha256File(f, &w.cancel, nullptr);
        if (w.cancel) return 2;
        if (sum != p.sha256) {
            LOGI("setup: reforge pack: %s sha256 %s, expected %s", p.name.c_str(), sum.c_str(), p.sha256.c_str());
            return broken("a downloaded part is damaged or not the expected file (" + p.name + ")");
        }
        WriteFile(f + ".ok", sum + "\n");
        doneB += p.bytes;
    }
    // unpack the parts the headset uses next to the downloads
    std::string unpack = c.dlPack + "/unpack", pack = unpack + "/reforge-pack";
    RemoveTree(unpack);
    int k = 0;
    for (auto& p : parts) {
        w.set(0.91 + 0.04 * k++ / (double)parts.size(), "Unpacking " + p.name + "...");
        ZipReader z;
        std::string why;
        if (!z.open(c.dlPack + "/" + p.name, &why)) return broken("a downloaded part does not unpack (" + p.name + ")");
        for (const ZipEntry& e : z.entries()) {
            if (w.cancel) { RemoveTree(unpack); return 2; }
            if (!SafeZipPath(e.name)) return broken("a downloaded part does not unpack (" + p.name + ")");
            std::string rel;
            if (!e.name.compare(0, 13, "reforge-pack/")) rel = e.name.substr(13);
            else if (!e.name.compare(0, 19, "reforge-pack-quest/")) rel = e.name.substr(19);
            else continue;
            if (e.dir || rel.empty() || !WantPackEntry(rel)) continue;
            if (!z.extract(e, pack + "/" + rel, &why)) {
                LOGI("setup: unpack: %s", why.c_str());
                return broken("a downloaded part does not unpack (" + p.name + ")");
            }
        }
    }
    if (!IsDir(pack + "/hd-models") || !IsFile(pack + "/LINKS.tsv")) return broken("the download is not a Reforge pack");
    if (!IsFile(pack + "/QUEST-PACK.txt")) return broken("the download has no Quest textures");
    // shared files (LINKS.tsv): the copies the headset uses, each checked against its SHA-256
    w.set(0.96, "Setting up the pack...");
    std::istringstream links(ReadFile(pack + "/LINKS.tsv"));
    std::string line;
    int made = 0, bad = 0;
    while (std::getline(links, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        std::string link = line.substr(0, a), target = line.substr(a + 1, b - a - 1), sum = line.substr(b + 1);
        if (!WantPackEntry(link) || !SafeZipPath(link) || !SafeZipPath(target)) continue;
        std::string L = pack + "/" + link, T = pack + "/" + target;
        if (!IsFile(T)) { LOGI("setup: missing in the pack: %s", target.c_str()); bad++; continue; }
        if (!IsFile(L)) {
            size_t slash = L.rfind('/');
            MakeDirs(L.substr(0, slash));
            if (::link(T.c_str(), L.c_str()) != 0 && !CopyFile(T, L)) { bad++; continue; }
        }
        if (Sha256File(L, nullptr, nullptr) != sum) { LOGI("setup: checksum mismatch: %s", link.c_str()); bad++; continue; }
        made++;
    }
    if (bad) return broken("the pack's files did not check out");
    LOGI("setup: reforge pack: %d shared files in place", made);
    // into place (the previous pack, if any, is replaced as a whole)
    w.set(0.99, "Setting up the pack...");
    for (const char* d : {"hd-models", "ui", "ground"}) {
        std::string dst = c.data + "/" + d, old = c.setupDir + "/old-" + d;
        RemoveTree(old);
        if (IsDir(dst) && rename(dst.c_str(), old.c_str()) != 0) return broken("the old pack folder could not be replaced");
        if (IsDir(pack + "/" + d) && rename((pack + "/" + d).c_str(), dst.c_str()) != 0)
            return broken("the pack folder could not be created");
        RemoveTree(old);
    }
    std::string docs = c.data + "/reforge-pack-docs";
    RemoveTree(docs);
    MakeDirs(docs);
    for (const char* f : {"LICENSE.txt", "README.md", "PROVENANCE.md", "QUEST-PACK.txt"})
        rename((pack + "/" + f).c_str(), (docs + "/" + f).c_str());
    WriteFile(c.setupDir + "/pack-version", c.L.version + "\n");
    RemoveTree(c.dlPack);
    unlink((c.setupDir + "/pack-offer-off").c_str());
    LOGI("setup: reforge pack: installed %s", c.L.version.c_str());
    return 0;
}

bool PackInstalled(const Ctx& c) {
    return IsDir(c.data + "/hd-models") && IsFile(c.setupDir + "/pack-version");
}

uint64_t PackPartialBytes(const Ctx& c) {
    uint64_t have = 0;
    for (auto& p : c.L.questParts) have += FileSize(c.dlPack + "/" + p.name);
    return have;
}

uint64_t PackTotal(const Ctx& c) {
    uint64_t t = 0;
    for (auto& p : c.L.questParts) t += p.bytes;
    return t;
}

// The app's settings for the data set as it is now (uavr-user.cfg, the player's own, is read first and wins).
void WriteConfig(const Ctx& c) {
    const bool pack = PackInstalled(c);
    std::string s =
        "# UA Reforged VR: written by the app at every start (put your own KEY=VALUE lines in uavr-user.cfg).\n"
        "# Look: New Look (VR preset) with the Reforge pack, else the original look; pause > Settings > LOOK switches.\n";
    s += pack ? "UA_GFX_DEFAULT=vr\n" : "UA_GFX_DEFAULT=vrclassic\n";
    s += "UA_GFX_BENCH=0\n";
    if (pack) {
        s += "UA_HDMODELS_DIR=" + c.data + "/hd-models\n";
        if (IsDir(c.data + "/hd-models/sky")) s += "UA_SKY_DIR=hd-models/sky\n";
        if (IsFile(c.data + "/ground/pack.txt")) s += "UA_R2_GROUND_DIR=ground\n";
        if (IsDir(c.data + "/ui")) s += "UA_UI_DIR=" + c.data + "/ui/\n";
    }
    WriteFile(c.data + "/uavr.cfg", s);
}

// ---------------------------------------------------------------- the panel
const int kW = 1280, kH = 800;
const float kPanelW = 1.30f, kPanelH = kPanelW * kH / kW;
const Vec3 kPanelPos{0.f, -0.08f, -1.35f};

struct Btn {
    std::string label;
    SDL_Rect r{};
};

class Panel {
public:
    bool init(android_app* app, XrPlatform& xr);
    void shutdown(XrPlatform& xr);
    // noFocus: no button has the focus until the player points at one or moves the thumbstick (A does nothing)
    void show(const std::string& title, const std::string& body, const std::vector<std::string>& buttons,
              double progress = -1, const std::string& status = "", bool noFocus = false);
    void progress(double f, const std::string& status);
    // One XR frame. Returns the clicked button index, -2 for back (B / Y), -1 for nothing.
    int frame(XrPlatform& xr);
    int buttonCount() const { return (int)btn_.size(); }

private:
    void redraw();
    void text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int wrap = 0, bool center = false, int cw = 0);
    bool upload(XrPlatform& xr);
    std::string fontData_[3];
    TTF_Font *title_ = nullptr, *body_ = nullptr, *btnFont_ = nullptr, *small_ = nullptr;
    SDL_Surface* surf_ = nullptr;
    std::vector<uint8_t> flip_;
    XrSwapchain sc_ = XR_NULL_HANDLE, cur_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> img_;
    int version_ = 1, uploaded_ = 0;
    bool cursorUploaded_ = false;
    std::string title_s_, body_s_, status_;
    double progress_ = -1;
    std::vector<Btn> btn_;
    int focus_ = 0, hover_[2] = {-1, -1};
    bool trigWas_[2] = {false, false};
    int stickLatch_ = 0;
    bool hit_[2] = {false, false};
    Vec3 hitPos_[2];
};

bool Panel::init(android_app* app, XrPlatform& xr) {
    if (TTF_Init() != 0) { LOGE("setup: TTF_Init: %s", TTF_GetError()); return false; }
    const char* files[3] = {"ua/fonts/Xolonium-Bold.otf", "ua/fonts/LiberationSans-Regular.ttf", "ua/fonts/Xolonium-Regular.otf"};
    for (int i = 0; i < 3; i++) {
        fontData_[i] = ReadAsset(app, files[i]);
        if (fontData_[i].empty()) { LOGE("setup: font %s missing", files[i]); return false; }
    }
    auto open = [&](int i, int pt) { return TTF_OpenFontRW(SDL_RWFromConstMem(fontData_[i].data(), (int)fontData_[i].size()), 1, pt); };
    title_ = open(0, 50);
    body_ = open(1, 32);
    btnFont_ = open(2, 30);
    small_ = open(1, 24);
    if (!title_ || !body_ || !btnFont_ || !small_) { LOGE("setup: fonts: %s", TTF_GetError()); return false; }
    surf_ = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!surf_) return false;
    flip_.resize((size_t)kW * kH * 4);

    // swapchains: the panel and a small round cursor (both RGBA; sRGB when the runtime offers it)
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(xr.session(), 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf);
    xrEnumerateSwapchainFormats(xr.session(), nf, &nf, fmts.data());
    int64_t fmt = GL_RGBA8;
    for (int64_t f : fmts) if (f == GL_SRGB8_ALPHA8) fmt = GL_SRGB8_ALPHA8;
    auto make = [&](int w, int h, XrSwapchain* out) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = fmt;
        ci.sampleCount = 1;
        ci.width = (uint32_t)w;
        ci.height = (uint32_t)h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        return XR_SUCCEEDED(xrCreateSwapchain(xr.session(), &ci, out));
    };
    if (!make(kW, kH, &sc_) || !make(64, 64, &cur_)) { LOGE("setup: panel swapchain creation failed"); return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc_, 0, &n, nullptr);
    img_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(sc_, n, &n, (XrSwapchainImageBaseHeader*)img_.data());
    LOGI("setup: panel %dx%d, %u images, format 0x%llx", kW, kH, n, (unsigned long long)fmt);
    return true;
}

void Panel::shutdown(XrPlatform&) {
    if (sc_) xrDestroySwapchain(sc_);
    if (cur_) xrDestroySwapchain(cur_);
    sc_ = cur_ = XR_NULL_HANDLE;
    for (TTF_Font* f : {title_, body_, btnFont_, small_}) if (f) TTF_CloseFont(f);
    title_ = body_ = btnFont_ = small_ = nullptr;
    if (surf_) SDL_FreeSurface(surf_);
    surf_ = nullptr;
    TTF_Quit();
}

void Panel::show(const std::string& title, const std::string& body, const std::vector<std::string>& buttons,
                 double progress, const std::string& status, bool noFocus) {
    title_s_ = title; body_s_ = body; progress_ = progress; status_ = status;
    btn_.clear();
    for (auto& b : buttons) btn_.push_back({b, {}});
    focus_ = noFocus ? -1 : 0;
    hover_[0] = hover_[1] = -1;
    version_++;
}

void Panel::progress(double f, const std::string& status) {
    if (std::fabs(f - progress_) < 0.002 && status == status_) return;
    progress_ = f; status_ = status;
    version_++;
}

void Panel::text(TTF_Font* f, const std::string& s, int x, int y, SDL_Color c, int wrap, bool center, int cw) {
    if (s.empty()) return;
    SDL_Surface* t = wrap ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), c, (Uint32)wrap) : TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!t) return;
    SDL_Rect d{center ? x + (cw - t->w) / 2 : x, y, t->w, t->h};
    SDL_BlitSurface(t, nullptr, surf_, &d);
    SDL_FreeSurface(t);
}

void Panel::redraw() {
    const SDL_Color white{232, 238, 242, 255}, dim{150, 165, 178, 255}, dark{14, 20, 26, 255};
    auto fill = [&](SDL_Rect r, Uint8 R, Uint8 G, Uint8 B) { SDL_FillRect(surf_, &r, SDL_MapRGBA(surf_->format, R, G, B, 255)); };
    fill({0, 0, kW, kH}, 16, 22, 28);
    fill({0, 0, kW, 6}, 232, 160, 48);
    text(title_, title_s_, 48, 34, white);
    {
        int vw = 0;
        TTF_SizeUTF8(small_, "UA Reforged VR " UAVR_APP_VERSION, &vw, nullptr);
        text(small_, "UA Reforged VR " UAVR_APP_VERSION, kW - 48 - vw, 54, dim);
    }
    fill({48, 112, kW - 96, 2}, 60, 74, 88);
    text(body_, body_s_, 48, 138, white, kW - 96);
    if (progress_ >= 0) {
        text(body_, status_, 48, 506, dim, kW - 96);
        fill({48, 560, kW - 96, 30}, 38, 50, 62);
        int w = (int)((kW - 96) * std::max(0.0, std::min(1.0, progress_)));
        fill({48, 560, w, 30}, 232, 160, 48);
    }
    const int n = (int)btn_.size();
    if (n) {
        const int gap = 24, bw = (kW - 96 - (n - 1) * gap) / n, bh = 96, by = 640;
        for (int i = 0; i < n; i++) {
            SDL_Rect r{48 + i * (bw + gap), by, bw, bh};
            btn_[i].r = r;
            bool on = i == focus_;
            if (on) fill(r, 232, 160, 48);
            else {
                fill(r, 44, 58, 72);
                fill({r.x, r.y + r.h - 4, r.w, 4}, 70, 88, 104);
            }
            int th = 0;
            TTF_SizeUTF8(btnFont_, btn_[i].label.c_str(), nullptr, &th);
            int tw = 0;
            TTF_SizeUTF8(btnFont_, btn_[i].label.c_str(), &tw, nullptr);
            if (tw > bw - 24) {  // two lines
                SDL_Surface* t = TTF_RenderUTF8_Blended_Wrapped(btnFont_, btn_[i].label.c_str(), on ? dark : white, (Uint32)(bw - 24));
                if (t) {
                    SDL_Rect d{r.x + (bw - t->w) / 2, r.y + (bh - t->h) / 2, t->w, t->h};
                    SDL_BlitSurface(t, nullptr, surf_, &d);
                    SDL_FreeSurface(t);
                }
            } else {
                text(btnFont_, btn_[i].label, r.x, r.y + (bh - th) / 2, on ? dark : white, 0, true, bw);
            }
        }
        text(small_, "Point at a button and pull the trigger, or use the thumbstick and A.", 48, 752, dim);
    }
}

bool Panel::upload(XrPlatform& xr) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc_, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc_, &wi);
    // every image gets the current content (a swapchain cycles through its images)
    if (uploaded_ != version_ * 16 + (int)idx) {
        redraw();
        const uint8_t* src = (const uint8_t*)surf_->pixels;
        for (int y = 0; y < kH; y++)  // GL rows count from the bottom
            memcpy(&flip_[(size_t)(kH - 1 - y) * kW * 4], src + (size_t)y * surf_->pitch, (size_t)kW * 4);
        glBindTexture(GL_TEXTURE_2D, img_[idx].image);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, flip_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        uploaded_ = version_ * 16 + (int)idx;
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc_, &ri);
    if (!cursorUploaded_) {
        uint32_t ci = 0;
        uint32_t cn = 0;
        xrEnumerateSwapchainImages(cur_, 0, &cn, nullptr);
        std::vector<XrSwapchainImageOpenGLESKHR> cimg(cn, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(cur_, cn, &cn, (XrSwapchainImageBaseHeader*)cimg.data());
        std::vector<uint8_t> dot(64 * 64 * 4);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                float d = std::hypot(x - 31.5f, y - 31.5f);
                float a = std::max(0.f, std::min(1.f, 30.f - d));
                bool ring = d > 22.f;
                uint8_t* p = &dot[(size_t)(y * 64 + x) * 4];
                // premultiplied: amber ring, white centre
                p[0] = (uint8_t)((ring ? 232 : 250) * a); p[1] = (uint8_t)((ring ? 160 : 250) * a);
                p[2] = (uint8_t)((ring ? 48 : 250) * a); p[3] = (uint8_t)(255 * a);
            }
        for (uint32_t k = 0; k < cn; k++) {  // a cursor that never changes: fill every image once
            XrSwapchainImageAcquireInfo a2{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(xrAcquireSwapchainImage(cur_, &a2, &ci))) break;
            XrSwapchainImageWaitInfo w2{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            w2.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(cur_, &w2);
            glBindTexture(GL_TEXTURE_2D, cimg[ci].image);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, dot.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            XrSwapchainImageReleaseInfo r2{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(cur_, &r2);
        }
        cursorUploaded_ = true;
    }
    (void)xr;
    return true;
}

int Panel::frame(XrPlatform& xr) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!xr.waitAndBeginFrame(fs)) return -1;
    const InputState& in = xr.input().state();
    int clicked = -1;
    const int n = (int)btn_.size();
    for (int h = 0; h < kHandCount; h++) {
        const HandState& hs = in.hand[h];
        hit_[h] = false;
        int over = -1;
        if (hs.active && hs.aimValid) {
            Vec3 o = hs.aimPose.position, d = quatRotate(hs.aimPose.orientation, Vec3{0, 0, -1});
            // the panel faces +Z at kPanelPos (no rotation)
            if (d.z < -1e-3f) {
                float t = (kPanelPos.z - o.z) / d.z;
                if (t > 0) {
                    Vec3 p = o + d * t;
                    float u = (p.x - kPanelPos.x) / kPanelW + 0.5f, v = 0.5f - (p.y - kPanelPos.y) / kPanelH;
                    if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                        hit_[h] = true;
                        hitPos_[h] = Vec3{p.x, p.y, kPanelPos.z + 0.004f};
                        int px = (int)(u * kW), py = (int)(v * kH);
                        for (int i = 0; i < n; i++) {
                            const SDL_Rect& r = btn_[i].r;
                            if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) over = i;
                        }
                    }
                }
            }
        }
        if (over != hover_[h]) {
            hover_[h] = over;
            if (over >= 0 && over != focus_) {
                focus_ = over;
                version_++;
                xr.input().haptic((Hand)h, 0.25f, 0.015f);
            }
        }
        const bool trig = hs.trigger > (trigWas_[h] ? 0.4f : 0.75f);
        if (trig && !trigWas_[h] && over >= 0) clicked = over;
        trigWas_[h] = trig;
    }
    // thumbstick focus, A / X activate, B / Y back
    float sx = 0;
    for (int h = 0; h < kHandCount; h++) if (std::fabs(in.hand[h].stick.x) > std::fabs(sx)) sx = in.hand[h].stick.x;
    int dir = sx > 0.7f ? 1 : (sx < -0.7f ? -1 : 0);
    if (std::fabs(sx) < 0.3f) stickLatch_ = 0;
    if (dir && !stickLatch_ && n) {
        stickLatch_ = dir;
        focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
        version_++;
    }
    if ((in.aPressed || in.xPressed) && n && clicked < 0 && focus_ >= 0) clicked = focus_;
    if ((in.bPressed || in.yPressed) && clicked < 0) clicked = -2;

    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD}, c[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t nl = 0;
    if (fs.shouldRender && upload(xr)) {
        q.space = xr.appSpace();
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = sc_;
        q.subImage.imageRect = {{0, 0}, {kW, kH}};
        q.pose.orientation = {0, 0, 0, 1};
        q.pose.position = {kPanelPos.x, kPanelPos.y, kPanelPos.z};
        q.size = {kPanelW, kPanelH};
        layers[nl++] = (const XrCompositionLayerBaseHeader*)&q;
        for (int h = 0; h < kHandCount; h++) {
            if (!hit_[h]) continue;
            XrCompositionLayerQuad& k = c[h];
            k.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            k.space = xr.appSpace();
            k.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            k.subImage.swapchain = cur_;
            k.subImage.imageRect = {{0, 0}, {64, 64}};
            k.pose.orientation = {0, 0, 0, 1};
            k.pose.position = {hitPos_[h].x, hitPos_[h].y, hitPos_[h].z};
            k.size = {0.022f, 0.022f};
            layers[nl++] = (const XrCompositionLayerBaseHeader*)&k;
        }
    }
    xr.endFrameLayers(fs.predictedDisplayTime, layers, nl, false);
    if (clicked >= 0) xr.input().haptic(kRight, 0.5f, 0.03f);
    return clicked;
}

// Opens a web page in the headset's browser (an Android VIEW intent; the browser comes to the front).
void OpenUrl(android_app* app, const std::string& url) {
    JNIEnv* e = nullptr;
    if (app->activity->vm->GetEnv((void**)&e, JNI_VERSION_1_6) != JNI_OK || !e) return;
    e->PushLocalFrame(16);
    jclass cUri = e->FindClass("android/net/Uri");
    jclass cIntent = e->FindClass("android/content/Intent");
    jclass cAct = e->GetObjectClass(app->activity->clazz);
    if (cUri && cIntent && cAct) {
        jobject uri = e->CallStaticObjectMethod(cUri, e->GetStaticMethodID(cUri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                                e->NewStringUTF(url.c_str()));
        jobject it = e->NewObject(cIntent, e->GetMethodID(cIntent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                  e->NewStringUTF("android.intent.action.VIEW"), uri);
        e->CallObjectMethod(it, e->GetMethodID(cIntent, "addFlags", "(I)Landroid/content/Intent;"), 0x10000000);  // NEW_TASK
        e->CallVoidMethod(app->activity->clazz, e->GetMethodID(cAct, "startActivity", "(Landroid/content/Intent;)V"), it);
    }
    if (e->ExceptionCheck()) { e->ExceptionClear(); LOGE("setup: could not open %s", url.c_str()); }
    e->PopLocalFrame(nullptr);
}

}  // namespace

bool GameValid(const std::string& d) {
    std::string data = FindChild(d, "DATA", true), levels = FindChild(d, "LEVELS", true);
    if (data.empty() || levels.empty()) return false;
    if (FindChild(data, "world.ini", false).empty()) return false;
    for (const char* s : {"SET1", "SCRIPTS", "SOUNDS"})
        if (FindChild(data, s, true).empty()) return false;
    std::string single = FindChild(levels, "SINGLE", true);
    if (single.empty()) return false;
    int files = 0;
    std::function<void(const std::string&)> count = [&](const std::string& p) {
        DIR* dd = opendir(p.c_str());
        if (!dd) return;
        while (dirent* e = readdir(dd)) {
            if (files >= 20) break;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string q = p + "/" + e->d_name;
            if (IsDir(q)) count(q);
            else files++;
        }
        closedir(dd);
    };
    count(single);
    return files >= 10;
}

bool RunSetup(android_app* app, XrPlatform& xr, const std::string& dataDir, std::vector<std::string>* engineArgs) {
    Ctx c;
    c.app = app;
    c.vm = app->activity->vm;
    c.data = dataDir;
    c.setupDir = dataDir + "/.setup";
    c.dlGame = c.setupDir + "/game-download";
    c.dlPack = c.setupDir + "/pack-download";
    c.allowLocal = Env("UA_LINKS_ALLOW_LOCAL") == "1";
    MakeDirs(c.setupDir);
    ParseDownloadsTxt(ReadAsset(app, "downloads.txt"), &c.L);
    ApplyEnvOverrides(c);
    if (c.L.helpUrl.empty()) c.L.helpUrl = kHelpGame;
    if (c.L.packHelpUrl.empty()) c.L.packHelpUrl = kHelpPack;
    ExtractEngineAssets(app, c.data, c.setupDir);

    const std::string gameAuto = Env("UA_GAME_AUTO"), packAuto = Env("UA_PACK_AUTO"), noticeAuto = Env("UA_NOTICE_AUTO");
    std::string campaign = Env("UA_CAMPAIGN");
    const bool exitAfter = Env("UA_SETUP_EXIT") == "1";
    auto finish = [&](bool start) {
        WriteConfig(c);
        if (campaign == "ghor") *engineArgs = {"-env", "Ghor_env"};
        else if (campaign == "taer") *engineArgs = {"-env", "Taer_env"};
        LOGI("SETUP RESULT %s campaign=%s pack=%s", start ? (exitAfter ? "done (exit for the test)" : "start") : "quit",
             campaign.empty() ? "base" : campaign.c_str(), PackInstalled(c) ? "yes" : "no");
        return start && !exitAfter;
    };
    if (Env("UA_SETUP") == "skip" && GameValid(c.data)) return finish(true);

    Panel P;
    if (!P.init(app, xr)) {
        LOGE("setup: no panel; starting only if the game is present");
        bool ok = GameValid(c.data);
        return finish(ok);
    }

    enum class S { Links, Choose, CopyHelp, GameWork, GameBroken, PackOffer, PackWork, PackBroken, PackStopped, Stats, Campaign, Start, Quit };
    // anonymous usage stats (docs/usage-stats.md): asked once, after the downloads, before the campaign;
    // UA_STATS_AUTO=yes|no|dismiss answers it (tests)
    const std::string statsAuto = Env("UA_STATS_AUTO");
    bool statsAsk = !UsageStats::Asked() && !UsageStats::SettingsDir().empty();
    // The site's links.json first (as on the desktop): it can fix a link or set the pack address without a new app.
    // Only needed when something may be downloaded.
    const bool needGame = !GameValid(c.data);
    const bool mayOfferPack = !PackInstalled(c) && (!IsFile(c.setupDir + "/pack-offer-off") || !packAuto.empty());
    S s = (needGame || mayOfferPack) ? S::Links : S::Campaign;
    S afterLinks = needGame ? S::Choose : S::PackOffer;
    S shown = S::Quit;
    Worker w;
    std::string why;
    bool packAsked = false;
    std::string folder = dataDir;
    size_t ad = folder.find("/Android/");
    if (ad != std::string::npos) folder = folder.substr(ad + 1);

    for (;;) {
        PumpEvents(true);
        if (GetHost()->quitPosted) {
            if (shown == S::Stats) UsageStats::RecordAnswer(false);  // closed while asked = No
            w.cancel = true; w.join(); P.shutdown(xr); return finish(false);
        }
        // pick the next screen
        if (s == S::PackOffer) {
            if (packAsked || PackInstalled(c) || !c.L.packUrlSet() || c.L.questParts.empty() ||
                (IsFile(c.setupDir + "/pack-offer-off") && packAuto.empty())) {
                if ((!c.L.packUrlSet() || c.L.questParts.empty()) && !PackInstalled(c)) LOGI("setup: reforge pack: none installed; this build has no download address");
                s = S::Campaign;
            }
        }
        if (s == S::Campaign && statsAsk) s = S::Stats;
        if (s == S::Campaign) {
            const bool md = !FindChild(c.data, "Ghor_env", true).empty() && !FindChild(c.data, "Taer_env", true).empty();
            if (!md || !campaign.empty() || !Env("UA_AUTOSTART_LEVEL").empty()) s = S::Start;
        }
        if (s == S::Start) { w.join(); P.shutdown(xr); return finish(true); }
        if (s == S::Quit) { w.join(); P.shutdown(xr); return finish(false); }

        if (s != shown) {
            shown = s;
            switch (s) {
                case S::Links:
                    P.show("UA Reforged VR", "Checking the download links...", {}, 0, "");
                    w.start([&c](Worker& ww) {
                        Http h(c.vm);
                        LoadLinks(c, h);
                        (void)ww;
                        return 0;
                    });
                    break;
                case S::Choose:
                    P.show("Welcome to UA Reforged VR",
                           "UA Reforged needs the original Urban Assault game files.\n\n"
                           "It can fetch the community OpenUA Standalone Package (about 445 MB) for you, straight from the "
                           "Metropolis Dawn community's server onto this headset. It is hosted by the Metropolis Dawn "
                           "community, not by UA Reforged, and the game files belong to Microsoft." +
                               (c.L.message.empty() ? std::string() : "\n\n" + c.L.message),
                           {"Get the game", "I'll copy the files myself", "Quit"});
                    break;
                case S::CopyHelp:
                    P.show("Copy your game files",
                           "Connect the headset to a computer with a USB cable and copy the contents of your Urban Assault "
                           "folder (DATA, LEVELS and the rest) into this folder on the headset:\n\n" + folder +
                               "\n\nSideQuest or adb can do it. Then choose Check again.",
                           {"Check again", "Open help page", "Back"});
                    break;
                case S::GameWork:
                    P.show("Getting the game", "Urban Assault, from the Metropolis Dawn community's server.", {"Cancel"}, 0,
                           "Checking the download link...");
                    w.start([&c](Worker& ww) { return GameGet(ww, c); });
                    break;
                case S::GameBroken:
                    P.show("The game download isn't working",
                           "The game download link isn't working right now (it's hosted by the Metropolis Dawn community). "
                           "The UA Reforged help page lists other ways to get it.\n\nWhat went wrong: " + why + ".",
                           {"Open help page", "Try again", "Copy the files myself"});
                    break;
                case S::PackOffer: {
                    uint64_t have = PackPartialBytes(c), total = PackTotal(c);
                    std::string msg = have > 0
                        ? "The Reforge pack download stopped at " + HumanBytes(have) + " of " + HumanBytes(total) + ". Resume it now?"
                        : "Download the Reforge pack (" + HumanBytes(total) + ")? It adds the new look: remastered units, "
                          "buildings and skies and sharp menus, with textures made for the Quest. Without it the game keeps "
                          "its 1998 look.";
                    P.show("The Reforge pack", msg + "\n\nIt goes to this app's own storage on the headset.",
                           {have > 0 ? "Resume download" : "Download", "Not now", "Don't ask again"});
                    break;
                }
                case S::PackWork:
                    P.show("Getting the Reforge pack", "The new look for UA Reforged.", {"Cancel"}, 0, "Checking the download links...");
                    w.start([&c](Worker& ww) { return PackGet(ww, c); });
                    break;
                case S::PackBroken:
                    P.show("The pack download isn't working",
                           "The Reforge pack download isn't working right now. The game starts with its 1998 look; "
                           "UA Reforged offers the pack again on a later start.\n\nWhat went wrong: " + why + ".",
                           {"Open help page", "Start the game"});
                    break;
                case S::PackStopped:
                    P.show("Download stopped",
                           "The game starts with the 1998 look. Next time UA Reforged offers to resume the download.",
                           {"Start the game"});
                    break;
                case S::Stats:
                    P.show("Help make UA Reforged better?",
                           "With anonymous usage stats on, UA Reforged sends a crash report if the game crashes and a short "
                           "performance summary after each mission, so we can find and fix crashes, slowdowns and bugs "
                           "quickly. It also counts how many people play.\n\n"
                           "No name, no IP, no files, no personal data, and it's completely optional. You can change it "
                           "anytime in Options (pause menu > Settings > CONTROLS, PRIVACY).",
                           {"Yes, send anonymous stats", "No thanks"}, -1, "", true);
                    break;
                case S::Campaign:
                    P.show("Which campaign?", "Metropolis Dawn is the expansion (two campaigns).",
                           {"Urban Assault", "Metropolis Dawn: Ghorkov", "Metropolis Dawn: Taerkasten"});
                    break;
                default: break;
            }
        }

        // automatic answers (test runs)
        int click = -1;
        if (s == S::Choose && !gameAuto.empty()) click = gameAuto == "get" ? 0 : (gameAuto == "copy" ? 1 : 2);
        else if (s == S::PackOffer && !packAuto.empty()) click = packAuto == "yes" ? 0 : (packAuto == "never" ? 2 : 1);
        else if ((s == S::GameBroken || s == S::PackBroken) && !noticeAuto.empty())
            click = noticeAuto == "help" ? 0 : 1;
        else if (s == S::PackStopped && !packAuto.empty()) click = 0;
        else if (s == S::CopyHelp && !gameAuto.empty()) click = 2;
        else if (s == S::Stats && !statsAuto.empty()) click = statsAuto == "yes" ? 0 : 1;  // "no" / "dismiss" (B) = No

        int ui = P.frame(xr);
        if (click < 0) click = ui;

        if (s == S::Links) {
            bool done;
            {
                std::lock_guard<std::mutex> l(w.m);
                done = w.done;
            }
            if (done) { w.join(); s = afterLinks; }
            continue;
        }
        // worker progress / results
        if (s == S::GameWork || s == S::PackWork) {
            double f;
            std::string st;
            bool done;
            int rc;
            {
                std::lock_guard<std::mutex> l(w.m);
                f = w.frac; st = w.status; done = w.done; rc = w.rc; why = w.why;
            }
            P.progress(f, st);
            if (click == 0 || click == -2) { w.cancel = true; P.progress(f, "Stopping..."); }
            if (done) {
                w.join();
                if (s == S::GameWork) {
                    if (rc == 0) { LOGI("SETUP RESULT game ok"); s = S::PackOffer; }
                    else if (rc == 2 || w.cancel) { LOGI("SETUP RESULT game cancelled"); s = gameAuto.empty() ? S::Choose : S::Quit; }
                    else {
                        LOGI("SETUP RESULT game broken: %s", why.c_str());
                        UsageStats::Error("game_download_failed");
                        s = S::GameBroken;
                    }
                } else {
                    packAsked = true;
                    if (rc == 0) { LOGI("SETUP RESULT pack ok"); s = S::Campaign; }
                    else if (rc == 2 || w.cancel) { LOGI("SETUP RESULT pack cancelled"); s = S::PackStopped; }
                    else {
                        LOGI("SETUP RESULT pack broken: %s", why.c_str());
                        UsageStats::Error(why.find("not the expected file") != std::string::npos ? "pack_checksum_mismatch"
                                                                                                  : "pack_download_failed");
                        s = S::PackBroken;
                    }
                }
            }
            continue;
        }
        if (click == -1) continue;

        switch (s) {
            case S::Choose:
                if (click == 0) s = S::GameWork;
                else if (click == 1) s = S::CopyHelp;
                else if (click == 2 || click == -2) s = S::Quit;
                if (!gameAuto.empty() && click == 1) s = S::Quit;  // a test run cannot copy files
                break;
            case S::CopyHelp:
                if (click == 0) {
                    if (GameValid(c.data)) { ExtractEngineAssets(app, c.data, c.setupDir); s = S::PackOffer; }
                    else { UsageStats::Error("game_folder_invalid"); P.progress(-1, ""); P.show("Copy your game files",
                           "The game files are not there yet: the folder needs DATA and LEVELS from your Urban Assault "
                           "install.\n\n" + folder, {"Check again", "Open help page", "Back"}); }
                } else if (click == 1) OpenUrl(app, c.L.helpUrl);
                else s = S::Choose;
                break;
            case S::GameBroken:
                if (click == 0) {
                    if (!noticeAuto.empty()) { LOGI("setup: help page: %s (test run: not opened)", c.L.helpUrl.c_str()); s = S::Quit; }
                    else OpenUrl(app, c.L.helpUrl);
                } else if (click == 1) s = S::GameWork;
                else if (click == 2) s = S::CopyHelp;
                else if (click == -2) s = S::Choose;
                if (!noticeAuto.empty() && click != 0) s = S::Quit;
                break;
            case S::PackOffer:
                if (click == 0) s = S::PackWork;
                else if (click == 1 || click == -2) { packAsked = true; LOGI("setup: reforge pack: not now"); s = S::Campaign; }
                else if (click == 2) {
                    WriteFile(c.setupDir + "/pack-offer-off", "off\n");
                    packAsked = true;
                    LOGI("setup: reforge pack: offer turned off");
                    s = S::Campaign;
                }
                break;
            case S::PackBroken:
                if (click == 0) {
                    if (!noticeAuto.empty()) LOGI("setup: help page: %s (test run: not opened)", c.L.packHelpUrl.c_str());
                    else OpenUrl(app, c.L.packHelpUrl);
                    if (!noticeAuto.empty()) s = S::Campaign;
                } else s = S::Campaign;
                break;
            case S::PackStopped: s = S::Campaign; break;
            case S::Stats:
                if (click == 0 || click == 1 || click == -2) {
                    UsageStats::RecordAnswer(click == 0);  // B / Y (back) = No
                    LOGI("setup: usage stats: %s", click == 0 ? "yes" : "no");
                    statsAsk = false;
                    s = S::Campaign;
                }
                break;
            case S::Campaign:
                if (click == 0) campaign = "base";
                else if (click == 1) campaign = "ghor";
                else if (click == 2) campaign = "taer";
                else break;
                s = S::Start;
                break;
            default: break;
        }
    }
}

}  // namespace setup
}  // namespace uavr
