// First-launch setup on the headset, building blocks (no UI here):
//   * Sha256        - streaming SHA-256 (pack part checks)
//   * ZipReader     - stored/deflate zip extraction with CRC checks and safe paths (zlib)
//   * Links         - the built-in download addresses (assets/downloads.txt) and the strict links.json override
//   * Http          - HTTPS GET with ranges through the system's java.net.HttpURLConnection (JNI; no Java code
//                     in the app), used for the link check and the resumable downloads
// The same rules as the desktop app's downloader (packaging/macos/downloads.sh, links.js).
#pragma once

#include <jni.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace uavr {
namespace setup {

// ---------------------------------------------------------------- SHA-256
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::string hex();  // finishes; call once
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};
std::string Sha256File(const std::string& path, const std::atomic<bool>* cancel,
                       const std::function<void(uint64_t)>& progress);

// ---------------------------------------------------------------- zip
struct ZipEntry {
    std::string name;  // as stored ('/' separators)
    uint64_t offset = 0, csize = 0, usize = 0;
    uint32_t crc = 0;
    uint16_t method = 0, flags = 0;
    bool dir = false;
};

class ZipReader {
public:
    ~ZipReader();
    bool open(const std::string& path, std::string* why);
    const std::vector<ZipEntry>& entries() const { return entries_; }
    // Extracts one entry to `dst` (parent folders created). false + why on any error (CRC, method, I/O).
    bool extract(const ZipEntry& e, const std::string& dst, std::string* why);
private:
    int fd_ = -1;
    uint64_t size_ = 0;
    std::vector<ZipEntry> entries_;
};
// A stored name that is safe to write under a folder: relative, no "..", no drive letters, no backslashes.
bool SafeZipPath(const std::string& name);

// ---------------------------------------------------------------- links
struct PackPart {
    std::string name;
    uint64_t bytes = 0;
    std::string sha256;
};
struct Links {
    std::string linksUrl, helpUrl, packHelpUrl, gameUrl, packUrl, version, message;
    uint64_t gameMin = 100000000, gameMax = 5000000000ULL;
    std::vector<std::string> allowHosts;
    std::vector<PackPart> parts;       // links.json pack_parts: the desktop pack, checked like the desktop app, not used
    std::vector<PackPart> questParts;  // the Quest pack (downloads.txt quest_part, links.json quest_parts): the download
    bool packUrlSet() const { return !packUrl.empty() && packUrl.find("PACK-URL-NOT-SET") == std::string::npos; }
};
// Built-in values: "key<TAB>value" lines (downloads.txt from the APK assets).
bool ParseDownloadsTxt(const std::string& text, Links* out);
// links.json from the site: every value strictly checked (types, lengths, https on an allowed host); any problem
// rejects the whole file and `out` is untouched. allowLocalHttp: test runs only (http://127.0.0.1 / localhost).
bool ApplyLinksJson(const std::string& text, bool allowLocalHttp, Links* out, std::string* why);
// URL check shared by both: https on an allowed host (or local http in test runs), no port, no odd characters.
bool UrlAllowed(const std::string& url, const std::vector<std::string>& hosts, bool allowLocalHttp, std::string* why);

// ---------------------------------------------------------------- HTTP
enum class NetErr { None, Dns, Unreachable, Timeout, Tls, Dropped, Http, Cancelled, Io, Other };

struct HttpResponse {
    int code = 0;
    std::string contentType;   // lower case
    int64_t contentLength = -1;
    std::string contentRange;  // raw header, "" if none
    std::string finalUrl;
    NetErr err = NetErr::None;
    std::string errText;       // exception class + message, for the log
};

class Http {
public:
    explicit Http(JavaVM* vm) : vm_(vm) {}
    // GET url, optionally with "Range: bytes=from-" (to < 0) or "bytes=from-to". `sink` receives the body;
    // returning false from it stops the transfer (reported as Cancelled). connect timeout 15 s; a read that
    // gets no data for `stallMs` fails with Timeout (the stall rule). Call from a thread attached to the VM
    // (Http attaches the calling thread itself and detaches it in its destructor).
    // `onHead` (may be empty) sees the status and headers before any body byte; returning false stops there.
    bool get(const std::string& url, int64_t from, int64_t to, int stallMs,
             const std::function<bool(const HttpResponse&)>& onHead,
             const std::function<bool(const uint8_t*, size_t)>& sink, HttpResponse* res);
    // POST a small JSON body (anonymous usage stats): connect and read timeouts `timeoutMs`, no redirects,
    // no caches, fixed User-Agent. Returns the HTTP status, or -1 for a network error.
    int postJson(const std::string& url, const std::string& body, int timeoutMs);
    ~Http();
private:
    JNIEnv* env();
    JavaVM* vm_;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};
const char* NetErrText(NetErr e, bool duringDownload);

// ---------------------------------------------------------------- small helpers
std::string HumanBytes(uint64_t b);
bool MakeDirs(const std::string& path);
bool RemoveTree(const std::string& path);
bool IsDir(const std::string& path);
bool IsFile(const std::string& path);
uint64_t FileSize(const std::string& path);
uint64_t FreeBytes(const std::string& path);
// Case-insensitive lookup of a direct child folder or file ("" if absent).
std::string FindChild(const std::string& dir, const char* name, bool wantDir);

}  // namespace setup
}  // namespace uavr
