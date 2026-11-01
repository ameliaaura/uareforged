// First-launch setup building blocks; see setup_core.h.
#include "setup/setup_core.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

namespace uavr {
namespace setup {

// ================================================================ SHA-256 (FIPS 180-4)
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() {
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, init, sizeof(h_));
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += n;
    if (used_) {
        size_t take = std::min(n, 64 - used_);
        memcpy(buf_ + used_, p, take);
        used_ += take; p += take; n -= take;
        if (used_ == 64) { block(buf_); used_ = 0; }
    }
    while (n >= 64) { block(p); p += 64; n -= 64; }
    if (n) { memcpy(buf_, p, n); used_ = n; }
}

std::string Sha256::hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h_[i]);
    return std::string(out, 64);
}

std::string Sha256File(const std::string& path, const std::atomic<bool>* cancel,
                       const std::function<void(uint64_t)>& progress) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    Sha256 s;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) { close(fd); return ""; }
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0) { close(fd); return ""; }
        if (n == 0) break;
        s.update(buf.data(), (size_t)n);
        done += (uint64_t)n;
        if (progress) progress(done);
    }
    close(fd);
    return s.hex();
}

// ================================================================ small helpers
std::string HumanBytes(uint64_t b) {
    char t[32];
    if (b >= 1000000000ULL) snprintf(t, sizeof(t), "%.1f GB", b / 1e9);
    else if (b >= 1000000ULL) snprintf(t, sizeof(t), "%d MB", (int)(b / 1000000ULL));
    else snprintf(t, sizeof(t), "%d KB", (int)((b + 999) / 1000));
    return t;
}

bool IsDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool IsFile(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
uint64_t FileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; }

uint64_t FreeBytes(const std::string& p) {
    struct statvfs v;
    if (statvfs(p.c_str(), &v) != 0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

bool MakeDirs(const std::string& path) {
    if (path.empty() || IsDir(path)) return true;
    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && !MakeDirs(path.substr(0, slash))) return false;
    return mkdir(path.c_str(), 0770) == 0 || IsDir(path);
}

bool RemoveTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    DIR* d = opendir(path.c_str());
    bool ok = true;
    if (d) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            ok = RemoveTree(path + "/" + e->d_name) && ok;
        }
        closedir(d);
    }
    return rmdir(path.c_str()) == 0 && ok;
}

std::string FindChild(const std::string& dir, const char* name, bool wantDir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string found;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name) != 0) continue;
        std::string p = dir + "/" + e->d_name;
        if (wantDir ? IsDir(p) : IsFile(p)) { found = p; break; }
    }
    closedir(d);
    return found;
}

// ================================================================ zip
namespace {
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

bool preadAll(int fd, void* buf, size_t n, uint64_t off) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return true;
}

bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
}  // namespace

bool SafeZipPath(const std::string& n) {
    if (n.empty() || n.size() > 1024 || n[0] == '/' || n.find('\\') != std::string::npos ||
        n.find(':') != std::string::npos)
        return false;
    size_t s = 0;
    while (s <= n.size()) {
        size_t e = n.find('/', s);
        if (e == std::string::npos) e = n.size();
        std::string part = n.substr(s, e - s);
        if (part == "..") return false;
        for (char c : part)
            if ((unsigned char)c < 0x20) return false;
        s = e + 1;
    }
    return true;
}

ZipReader::~ZipReader() { if (fd_ >= 0) close(fd_); }

bool ZipReader::open(const std::string& path, std::string* why) {
    entries_.clear();
    if (fd_ >= 0) close(fd_);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) { *why = "cannot open"; return false; }
    size_t size = 0;
    struct stat st;
    if (fstat(fd_, &st) != 0 || st.st_size < 22) { *why = "too small for a zip"; return false; }
    size_ = (uint64_t)st.st_size;
    size = (size_t)std::min<uint64_t>(size_, 65557);
    std::vector<uint8_t> tail(size);
    if (!preadAll(fd_, tail.data(), size, size_ - size)) { *why = "read error"; return false; }
    long eocd = -1;
    for (long i = (long)size - 22; i >= 0; --i)
        if (rd32(&tail[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { *why = "no zip directory (the file is cut off or not a zip)"; return false; }
    const uint8_t* E = &tail[eocd];
    uint64_t count = rd16(E + 10), cdSize = rd32(E + 12), cdOff = rd32(E + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {  // ZIP64
        if (eocd < 20 || rd32(&tail[eocd - 20]) != 0x07064b50) { *why = "bad zip64 locator"; return false; }
        uint64_t z64 = rd64(&tail[eocd - 20 + 8]);
        uint8_t r[56];
        if (!preadAll(fd_, r, sizeof(r), z64) || rd32(r) != 0x06064b50) { *why = "bad zip64 directory"; return false; }
        count = rd64(r + 32); cdSize = rd64(r + 40); cdOff = rd64(r + 48);
    }
    if (cdOff + cdSize > size_ || count > 2000000) { *why = "zip directory out of range (cut off?)"; return false; }
    std::vector<uint8_t> cd((size_t)cdSize);
    if (!preadAll(fd_, cd.data(), cd.size(), cdOff)) { *why = "read error"; return false; }
    size_t p = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50) { *why = "damaged zip directory"; return false; }
        const uint8_t* C = &cd[p];
        ZipEntry e;
        e.flags = rd16(C + 8); e.method = rd16(C + 10); e.crc = rd32(C + 16);
        e.csize = rd32(C + 20); e.usize = rd32(C + 24);
        uint16_t nlen = rd16(C + 28), xlen = rd16(C + 30), clen = rd16(C + 32);
        uint64_t lho = rd32(C + 42);
        if (p + 46 + nlen + xlen + clen > cd.size()) { *why = "damaged zip directory"; return false; }
        e.name.assign((const char*)C + 46, nlen);
        // ZIP64 extra field: the 0xFFFFFFFF fields, in this order
        size_t x = p + 46 + nlen, xe = x + xlen;
        while (x + 4 <= xe) {
            uint16_t id = rd16(&cd[x]), len = rd16(&cd[x + 2]);
            if (id == 0x0001) {
                size_t q = x + 4;
                if (e.usize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.usize = rd64(&cd[q]); q += 8; }
                if (e.csize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.csize = rd64(&cd[q]); q += 8; }
                if (lho == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { lho = rd64(&cd[q]); q += 8; }
            }
            x += 4 + len;
        }
        e.offset = lho;  // local header; data offset resolved in extract()
        e.dir = !e.name.empty() && e.name.back() == '/';
        entries_.push_back(e);
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool ZipReader::extract(const ZipEntry& e, const std::string& dst, std::string* why) {
    if (e.flags & 1) { *why = "encrypted entry " + e.name; return false; }
    if (e.method != 0 && e.method != 8) { *why = "unsupported compression in " + e.name; return false; }
    uint8_t lh[30];
    if (!preadAll(fd_, lh, 30, e.offset) || rd32(lh) != 0x04034b50) { *why = "damaged entry " + e.name; return false; }
    uint64_t data = e.offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data + e.csize > size_) { *why = "entry past the end (cut off): " + e.name; return false; }
    size_t slash = dst.rfind('/');
    if (slash != std::string::npos && !MakeDirs(dst.substr(0, slash))) { *why = "cannot create folder for " + e.name; return false; }
    std::string tmp = dst + ".part";
    int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (out < 0) { *why = "cannot write " + e.name; return false; }
    std::vector<uint8_t> in(1 << 18), ob(1 << 18);
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t left = e.csize, wrote = 0;
    bool ok = true;
    if (e.method == 0) {
        while (ok && left) {
            size_t n = (size_t)std::min<uint64_t>(left, in.size());
            ok = preadAll(fd_, in.data(), n, data + (e.csize - left)) && writeAll(out, in.data(), n);
            crc = crc32(crc, in.data(), (uInt)n);
            left -= n; wrote += n;
        }
    } else {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) ok = false;
        int zr = Z_OK;
        while (ok && zr != Z_STREAM_END) {
            if (z.avail_in == 0) {
                if (!left) { ok = false; break; }
                size_t n = (size_t)std::min<uint64_t>(left, in.size());
                if (!preadAll(fd_, in.data(), n, data + (e.csize - left))) { ok = false; break; }
                left -= n;
                z.next_in = in.data(); z.avail_in = (uInt)n;
            }
            z.next_out = ob.data(); z.avail_out = (uInt)ob.size();
            zr = inflate(&z, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) { ok = false; break; }
            size_t got = ob.size() - z.avail_out;
            if (got) {
                crc = crc32(crc, ob.data(), (uInt)got);
                ok = writeAll(out, ob.data(), got);
                wrote += got;
            }
        }
        inflateEnd(&z);
    }
    if (close(out) != 0) ok = false;
    if (ok && (wrote != e.usize || (uint32_t)crc != e.crc)) { *why = "checksum error in " + e.name; unlink(tmp.c_str()); return false; }
    if (!ok) { *why = "damaged data or disk full: " + e.name; unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), dst.c_str()) != 0) { *why = "cannot write " + e.name; unlink(tmp.c_str()); return false; }
    return true;
}

// ================================================================ links
namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
bool endsWith(const std::string& s, const std::string& t) { return s.size() >= t.size() && !s.compare(s.size() - t.size(), t.size(), t); }

bool parsePartLine(const std::string& v, PackPart* p) {
    size_t a = v.find('\t'), b = a == std::string::npos ? a : v.find('\t', a + 1);
    if (b == std::string::npos) return false;
    p->name = v.substr(0, a);
    p->bytes = strtoull(v.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
    p->sha256 = v.substr(b + 1);
    return !p->name.empty() && p->bytes > 0 && p->sha256.size() == 64;
}
}  // namespace

bool UrlAllowed(const std::string& u, const std::vector<std::string>& hosts, bool allowLocalHttp, std::string* why) {
    if (u.size() > 512) { *why = "not a plain URL"; return false; }
    for (unsigned char c : u)
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '`' || c == '\\') { *why = "not a plain URL"; return false; }
    bool https;
    size_t p;
    if (!u.compare(0, 8, "https://")) { https = true; p = 8; }
    else if (!u.compare(0, 7, "http://")) { https = false; p = 7; }
    else { *why = "not an http(s) URL"; return false; }
    size_t h = p;
    while (p < u.size() && (isalnum((unsigned char)u[p]) || u[p] == '.' || u[p] == '-')) p++;
    if (p == h) { *why = "not an http(s) URL"; return false; }
    std::string host = lower(u.substr(h, p - h));
    bool port = false;
    if (p < u.size() && u[p] == ':') {
        size_t q = ++p;
        while (p < u.size() && isdigit((unsigned char)u[p])) p++;
        if (p == q || p - q > 5) { *why = "not an http(s) URL"; return false; }
        port = true;
    }
    if (p < u.size() && u[p] != '/' && u[p] != '?' && u[p] != '#') { *why = "not an http(s) URL"; return false; }
    size_t hash = u.find('#', p);
    if (hash != std::string::npos)
        for (size_t i = hash + 1; i < u.size(); i++)
            if (!(isalnum((unsigned char)u[i]) || u[i] == '_' || u[i] == '-')) { *why = "not an http(s) URL"; return false; }
    if (!https) {
        if (!(allowLocalHttp && (host == "127.0.0.1" || host == "localhost"))) { *why = "https only"; return false; }
        return true;
    }
    if (port) { *why = "no port numbers"; return false; }
    for (const std::string& a : hosts) {
        std::string al = lower(a);
        if (host == al || endsWith(host, "." + al)) return true;
    }
    *why = "host " + host + " is not on the allowlist";
    return false;
}

bool ParseDownloadsTxt(const std::string& text, Links* L) {
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(s, e - s);
        s = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        std::string k = line.substr(0, t), v = line.substr(t + 1);
        if (k == "links_url") L->linksUrl = v;
        else if (k == "help_url") L->helpUrl = v;
        else if (k == "pack_help_url") L->packHelpUrl = v;
        else if (k == "game_url") L->gameUrl = v;
        else if (k == "game_min_bytes") L->gameMin = strtoull(v.c_str(), nullptr, 10);
        else if (k == "game_max_bytes") L->gameMax = strtoull(v.c_str(), nullptr, 10);
        else if (k == "allow_host") L->allowHosts.push_back(v);
        else if (k == "version") L->version = v;
        else if (k == "url") L->packUrl = v;
        else if (k == "part" || k == "quest_part") {
            PackPart p;
            if (parsePartLine(v, &p)) (k == "part" ? L->parts : L->questParts).push_back(p);
        }
    }
    if (!L->packUrl.empty() && L->packUrl.back() != '/') L->packUrl += '/';
    return !L->gameUrl.empty();
}

// ---------------------------------------------------------------- a strict little JSON reader (links.json only)
namespace {
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const std::string& s;
    size_t p = 0;
    int depth = 0;
    explicit JParser(const std::string& t) : s(t) {}
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }
    bool str(std::string* out) {
        if (p >= s.size() || s[p] != '"') return false;
        p++;
        while (p < s.size() && s[p] != '"') {
            unsigned char c = (unsigned char)s[p];
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++p >= s.size()) return false;
                char e = s[p];
                if (e == 'u') {
                    if (p + 4 >= s.size()) return false;
                    unsigned v = (unsigned)strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (v < 0x80) out->push_back((char)v);
                    else if (v < 0x800) { out->push_back((char)(0xC0 | v >> 6)); out->push_back((char)(0x80 | (v & 63))); }
                    else { out->push_back((char)(0xE0 | v >> 12)); out->push_back((char)(0x80 | ((v >> 6) & 63))); out->push_back((char)(0x80 | (v & 63))); }
                } else {
                    const char* m = strchr("\"\\/bfnrt", e);
                    if (!m) return false;
                    out->push_back("\"\\/\b\f\n\r\t"[m - "\"\\/bfnrt"]);
                }
                p++;
            } else {
                out->push_back((char)c);
                p++;
            }
        }
        if (p >= s.size()) return false;
        p++;
        return true;
    }
    bool val(JVal* v) {
        if (++depth > 8) return false;
        ws();
        if (p >= s.size()) return false;
        char c = s[p];
        bool ok = true;
        if (c == '{') {
            v->t = JVal::Obj; p++; ws();
            if (p < s.size() && s[p] == '}') p++;
            else for (;;) {
                ws();
                std::string k;
                JVal x;
                if (!str(&k)) { ok = false; break; }
                ws();
                if (p >= s.size() || s[p] != ':') { ok = false; break; }
                p++;
                if (!val(&x)) { ok = false; break; }
                v->o.emplace_back(k, x);
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v->t = JVal::Arr; p++; ws();
            if (p < s.size() && s[p] == ']') p++;
            else for (;;) {
                JVal x;
                if (!val(&x)) { ok = false; break; }
                v->a.push_back(x);
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v->t = JVal::Str; ok = str(&v->s);
        } else if (!s.compare(p, 4, "true")) { v->t = JVal::Bool; v->b = true; p += 4; }
        else if (!s.compare(p, 5, "false")) { v->t = JVal::Bool; p += 5; }
        else if (!s.compare(p, 4, "null")) { v->t = JVal::Null; p += 4; }
        else if (c == '-' || isdigit((unsigned char)c)) {
            size_t q = p;
            if (s[p] == '-') p++;
            while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) p++;
            v->t = JVal::Num; v->n = strtod(s.substr(q, p - q).c_str(), nullptr);
        } else ok = false;
        depth--;
        return ok;
    }
};

bool isWhole(double n) { return n == (double)(long long)n; }

bool checkParts(const JVal* p, const char* key, std::vector<PackPart>* out, std::string* why) {
    if (!p) return true;
    if (p->t != JVal::Arr || p->a.empty() || p->a.size() > 8) { *why = std::string(key) + ": a list of 1 to 8 parts"; return false; }
    for (size_t i = 0; i < p->a.size(); i++) {
        const JVal& x = p->a[i];
        std::string at = std::string(key) + "[" + std::to_string(i) + "]";
        if (x.t != JVal::Obj) { *why = at + ": not an object"; return false; }
        const JVal *n = x.get("name"), *b = x.get("bytes"), *h = x.get("sha256");
        bool nameOk = n && n->t == JVal::Str && n->s.size() >= 5 && n->s.size() <= 84 && endsWith(n->s, ".zip") &&
                      n->s.find("..") == std::string::npos;
        if (nameOk)
            for (char c : n->s) if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) nameOk = false;
        if (!nameOk) { *why = at + ".name"; return false; }
        if (!b || b->t != JVal::Num || !isWhole(b->n) || b->n < 1 || b->n > 1e10) { *why = at + ".bytes"; return false; }
        bool hexOk = h && h->t == JVal::Str && h->s.size() == 64;
        if (hexOk) for (char c : h->s) if (!(isdigit((unsigned char)c) || (c >= 'a' && c <= 'f'))) hexOk = false;
        if (!hexOk) { *why = at + ".sha256"; return false; }
        out->push_back({n->s, (uint64_t)b->n, h->s});
    }
    return true;
}
}  // namespace

bool ApplyLinksJson(const std::string& text, bool allowLocal, Links* out, std::string* why) {
    if (text.empty() || text.size() > 16384) { *why = "missing or too big"; return false; }
    JParser jp(text);
    JVal j;
    if (!jp.val(&j)) { *why = "not valid JSON"; return false; }
    jp.ws();
    if (jp.p != text.size()) { *why = "not valid JSON"; return false; }
    if (j.t != JVal::Obj) { *why = "not a JSON object"; return false; }
    Links L = *out;
    const char* urlKeys[] = {"game_url", "pack_url", "help_url", "pack_help_url"};
    std::string* dst[] = {&L.gameUrl, &L.packUrl, &L.helpUrl, &L.packHelpUrl};
    for (int i = 0; i < 4; i++) {
        const JVal* v = j.get(urlKeys[i]);
        if (!v) continue;
        std::string w;
        if (v->t != JVal::Str || !UrlAllowed(v->s, out->allowHosts, allowLocal, &w)) {
            *why = std::string(urlKeys[i]) + ": " + (w.empty() ? "not a plain URL" : w);
            return false;
        }
        *dst[i] = v->s;
    }
    if (const JVal* v = j.get("game_min_bytes")) {
        if (v->t != JVal::Num || !isWhole(v->n) || v->n < 1e6 || v->n > 1e10) { *why = "game_min_bytes: not a whole number of bytes (1 MB..10 GB)"; return false; }
        L.gameMin = (uint64_t)v->n;
    }
    if (const JVal* v = j.get("message")) {
        bool ok = v->t == JVal::Str && v->s.size() <= 300;
        if (ok) for (unsigned char c : v->s) if (c < 0x20 || c == 0x7f) ok = false;
        if (!ok) { *why = "message: plain text of at most 300 characters"; return false; }
        L.message = v->s;
    }
    std::vector<PackPart> parts, qparts;
    if (!checkParts(j.get("pack_parts"), "pack_parts", &parts, why)) return false;
    if (!checkParts(j.get("quest_parts"), "quest_parts", &qparts, why)) return false;
    if (!parts.empty()) L.parts = parts;
    if (!qparts.empty()) L.questParts = qparts;
    if (!L.packUrl.empty() && L.packUrl.back() != '/') L.packUrl += '/';
    *out = L;
    return true;
}

// ================================================================ HTTP over JNI
const char* NetErrText(NetErr e, bool during) {
    switch (e) {
        case NetErr::Dns: return "the server name could not be found";
        case NetErr::Unreachable: return "the server could not be reached";
        case NetErr::Timeout: return during ? "the download stalled (no data for 30 seconds)" : "the server did not answer in time";
        case NetErr::Tls: return "the secure connection failed";
        case NetErr::Dropped: return during ? "the connection dropped during the download" : "the connection failed";
        case NetErr::Cancelled: return "cancelled";
        case NetErr::Io: return "the file could not be written (is the headset storage full?)";
        default: return "the connection failed";
    }
}

Http::~Http() {
    if (attached_ && vm_) vm_->DetachCurrentThread();
}

JNIEnv* Http::env() {
    if (env_) return env_;
    if (vm_->GetEnv((void**)&env_, JNI_VERSION_1_6) == JNI_OK) return env_;
    if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) { attached_ = true; return env_; }
    env_ = nullptr;
    return nullptr;
}

namespace {
struct LocalFrame {
    JNIEnv* e;
    explicit LocalFrame(JNIEnv* env) : e(env) { e->PushLocalFrame(64); }
    ~LocalFrame() { e->PopLocalFrame(nullptr); }
};

std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}

// Clears a pending Java exception; returns true (and fills the response) if there was one.
bool takeException(JNIEnv* e, HttpResponse* r, bool reading) {
    if (!e->ExceptionCheck()) return false;
    jthrowable t = e->ExceptionOccurred();
    e->ExceptionClear();
    jclass tc = e->GetObjectClass(t);
    jclass cc = e->FindClass("java/lang/Class");
    std::string name = jstr(e, (jstring)e->CallObjectMethod(tc, e->GetMethodID(cc, "getName", "()Ljava/lang/String;")));
    if (e->ExceptionCheck()) e->ExceptionClear();
    jclass thc = e->FindClass("java/lang/Throwable");
    std::string msg = jstr(e, (jstring)e->CallObjectMethod(t, e->GetMethodID(thc, "getMessage", "()Ljava/lang/String;")));
    if (e->ExceptionCheck()) e->ExceptionClear();
    r->errText = name + ": " + msg;
    if (name == "java.net.UnknownHostException") r->err = NetErr::Dns;
    else if (name == "java.net.ConnectException" || name == "java.net.NoRouteToHostException" ||
             name == "java.net.PortUnreachableException") r->err = NetErr::Unreachable;
    else if (name == "java.net.SocketTimeoutException") r->err = NetErr::Timeout;
    else if (!name.compare(0, 13, "javax.net.ssl")) r->err = NetErr::Tls;
    else if (reading || name.find("IOException") != std::string::npos || name.find("Socket") != std::string::npos ||
             name == "java.io.EOFException" || name == "java.net.ProtocolException") r->err = NetErr::Dropped;
    else r->err = NetErr::Other;
    return true;
}
}  // namespace

bool Http::get(const std::string& urlStr, int64_t from, int64_t to, int stallMs,
               const std::function<bool(const HttpResponse&)>& onHead,
               const std::function<bool(const uint8_t*, size_t)>& sink, HttpResponse* r) {
    *r = HttpResponse();
    JNIEnv* e = env();
    if (!e) { r->err = NetErr::Other; r->errText = "no JNI"; return false; }
    LocalFrame frame(e);
    jclass cUrl = e->FindClass("java/net/URL");
    jclass cConn = e->FindClass("java/net/HttpURLConnection");
    jclass cIn = e->FindClass("java/io/InputStream");
    if (!cUrl || !cConn || !cIn) { takeException(e, r, false); r->err = NetErr::Other; return false; }
    jobject url = e->NewObject(cUrl, e->GetMethodID(cUrl, "<init>", "(Ljava/lang/String;)V"), e->NewStringUTF(urlStr.c_str()));
    if (takeException(e, r, false)) return false;
    jobject conn = e->CallObjectMethod(url, e->GetMethodID(cUrl, "openConnection", "()Ljava/net/URLConnection;"));
    if (takeException(e, r, false)) return false;
    if (!e->IsInstanceOf(conn, cConn)) { r->err = NetErr::Other; r->errText = "not an HTTP connection"; return false; }
    jmethodID setProp = e->GetMethodID(cConn, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID disconnect = e->GetMethodID(cConn, "disconnect", "()V");
    e->CallVoidMethod(conn, e->GetMethodID(cConn, "setConnectTimeout", "(I)V"), 15000);
    e->CallVoidMethod(conn, e->GetMethodID(cConn, "setReadTimeout", "(I)V"), stallMs);
    e->CallVoidMethod(conn, e->GetMethodID(cConn, "setInstanceFollowRedirects", "(Z)V"), JNI_TRUE);
    e->CallVoidMethod(conn, e->GetMethodID(cConn, "setUseCaches", "(Z)V"), JNI_FALSE);
    e->CallVoidMethod(conn, setProp, e->NewStringUTF("User-Agent"), e->NewStringUTF("UA-Reforged (Quest)"));
    e->CallVoidMethod(conn, setProp, e->NewStringUTF("Accept-Encoding"), e->NewStringUTF("identity"));
    if (from > 0 || to >= 0) {
        std::string range = "bytes=" + std::to_string(from < 0 ? 0 : from) + "-" + (to >= 0 ? std::to_string(to) : "");
        e->CallVoidMethod(conn, setProp, e->NewStringUTF("Range"), e->NewStringUTF(range.c_str()));
    }
    if (takeException(e, r, false)) return false;
    r->code = e->CallIntMethod(conn, e->GetMethodID(cConn, "getResponseCode", "()I"));
    if (takeException(e, r, false)) { e->CallVoidMethod(conn, disconnect); e->ExceptionClear(); return false; }
    r->contentType = lower(jstr(e, (jstring)e->CallObjectMethod(conn, e->GetMethodID(cConn, "getContentType", "()Ljava/lang/String;"))));
    r->contentLength = e->CallLongMethod(conn, e->GetMethodID(cConn, "getContentLengthLong", "()J"));
    r->contentRange = jstr(e, (jstring)e->CallObjectMethod(conn, e->GetMethodID(cConn, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;"),
                                                         e->NewStringUTF("Content-Range")));
    jobject fu = e->CallObjectMethod(conn, e->GetMethodID(cConn, "getURL", "()Ljava/net/URL;"));
    if (fu) r->finalUrl = jstr(e, (jstring)e->CallObjectMethod(fu, e->GetMethodID(cUrl, "toString", "()Ljava/lang/String;")));
    if (takeException(e, r, false)) { e->CallVoidMethod(conn, disconnect); e->ExceptionClear(); return false; }
    if (r->code < 200 || r->code > 299) {
        r->err = NetErr::Http;
        e->CallVoidMethod(conn, disconnect);
        e->ExceptionClear();
        return false;
    }
    if (onHead && !onHead(*r)) {
        r->err = NetErr::Cancelled;
        e->CallVoidMethod(conn, disconnect);
        e->ExceptionClear();
        return false;
    }
    jobject in = e->CallObjectMethod(conn, e->GetMethodID(cConn, "getInputStream", "()Ljava/io/InputStream;"));
    if (takeException(e, r, true)) { e->CallVoidMethod(conn, disconnect); e->ExceptionClear(); return false; }
    jmethodID read = e->GetMethodID(cIn, "read", "([B)I");
    jmethodID closeIn = e->GetMethodID(cIn, "close", "()V");
    const jsize kBuf = 1 << 16;
    jbyteArray jb = e->NewByteArray(kBuf);
    std::vector<uint8_t> nb(kBuf);
    bool ok = true;
    for (;;) {
        jint n = e->CallIntMethod(in, read, jb);
        if (takeException(e, r, true)) { ok = false; break; }
        if (n < 0) break;
        if (n == 0) continue;
        e->GetByteArrayRegion(jb, 0, n, (jbyte*)nb.data());
        if (!sink(nb.data(), (size_t)n)) { r->err = NetErr::Cancelled; ok = false; break; }
    }
    e->CallVoidMethod(in, closeIn);
    e->ExceptionClear();
    e->CallVoidMethod(conn, disconnect);
    e->ExceptionClear();
    return ok;
}

}  // namespace setup
}  // namespace uavr
