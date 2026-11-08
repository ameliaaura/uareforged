// Anonymous usage stats: platform parts (files with UTF-8 paths, secure random bytes, the OS name, the
// settings folder, and the Windows sender over WinHTTP). macOS sends with NSURLSession
// (usage_stats_net_apple.mm); the Quest app registers its own sender (java.net.HttpURLConnection).
// Only system libraries: WinHTTP + BCrypt on Windows, Foundation on macOS, libc on Android.
#include "usage_stats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/random.h>
#include <sys/sysctl.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/random.h>
#endif

namespace UsageStats
{

#ifdef _WIN32
static std::wstring Wide(const std::string &s)
{
    if ( s.empty() ) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Utf8(const std::wstring &w)
{
    if ( w.empty() ) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
#endif

std::string EnvVar(const char *name)
{
#ifdef _WIN32
    const wchar_t *v = _wgetenv(Wide(name).c_str());
    return v ? Utf8(v) : std::string();
#else
    const char *v = getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

std::string PathJoin(const std::string &dir, const char *leaf)
{
    if ( dir.empty() ) return leaf;
    char last = dir[dir.size() - 1];
#ifdef _WIN32
    return (last == '\\' || last == '/') ? dir + leaf : dir + "\\" + leaf;
#else
    return last == '/' ? dir + leaf : dir + "/" + leaf;
#endif
}

static FILE *Open(const std::string &path, const char *mode)
{
#ifdef _WIN32
    return _wfopen(Wide(path).c_str(), Wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool FileRead(const std::string &path, std::string *out, size_t max)
{
    FILE *f = Open(path, "rb");
    if ( !f ) return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ( (n = fread(buf, 1, sizeof(buf), f)) > 0 && out->size() <= max )
        out->append(buf, n);
    fclose(f);
    if ( out->size() > max ) out->resize(max);
    return true;
}

bool FileExists(const std::string &path)
{
    FILE *f = Open(path, "rb");
    if ( !f ) return false;
    fclose(f);
    return true;
}

void FileRemove(const std::string &path)
{
    if ( path.empty() ) return;
#ifdef _WIN32
    DeleteFileW(Wide(path).c_str());
#else
    unlink(path.c_str());
#endif
}

void MakeDir(const std::string &path)
{
#ifdef _WIN32
    CreateDirectoryW(Wide(path).c_str(), NULL);
#else
    mkdir(path.c_str(), 0700);
#endif
}

// through a sibling temp file, so a crash or a full disk never leaves half a file
bool FileWriteAtomic(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp";
    FILE *f = Open(tmp, "wb");
    if ( !f ) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if ( !ok ) { FileRemove(tmp); return false; }
#ifdef _WIN32
    ok = MoveFileExW(Wide(tmp).c_str(), Wide(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if ( !ok ) FileRemove(tmp);
    return ok;
}

bool OsRandom(uint8_t out[16])
{
#if defined(_WIN32)
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, out, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#elif defined(__APPLE__)
    return getentropy(out, 16) == 0;
#elif defined(__ANDROID__)
    arc4random_buf(out, 16);
    return true;
#elif defined(__linux__)
    return getrandom(out, 16, 0) == 16;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if ( !f ) return false;
    bool ok = fread(out, 1, 16, f) == 16;
    fclose(f);
    return ok;
#endif
}

#if defined(__ANDROID__)
static std::string Prop(const char *k)
{
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(k, v);
    return v;
}
#endif

std::string OsName()
{
#if defined(__APPLE__)
    char v[64] = {0};
    size_t n = sizeof(v) - 1;
    if ( sysctlbyname("kern.osproductversion", v, &n, NULL, 0) == 0 && v[0] )
        return std::string("macOS ") + v;
    return "macOS";
#elif defined(_WIN32)
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if ( !fn || fn(&vi) != 0 )
        return "Windows";
    std::string name;
    if ( vi.dwMajorVersion == 10 )
        name = vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    else
        name = "Windows " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion);
    // "22H2" / "23H2" (older Windows 10: "ReleaseId", e.g. 2004)
    wchar_t dv[32];
    DWORD sz = sizeof(dv);
    if ( RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion",
                      RRF_RT_REG_SZ, NULL, dv, &sz) == ERROR_SUCCESS ||
         (sz = sizeof(dv), RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ReleaseId",
                                        RRF_RT_REG_SZ, NULL, dv, &sz) == ERROR_SUCCESS) )
        return name + " " + Utf8(dv);
    return name + " (build " + std::to_string(vi.dwBuildNumber) + ")";
#elif defined(__ANDROID__)
    // Horizon OS version where the system says it; else the Android version underneath
    std::string v = Prop("ro.vros.build.version");
    if ( v.empty() ) v = Prop("ro.ovr.os.api.version");
    if ( !v.empty() )
    {
        size_t dot = v.find('.');
        return "Quest OS " + v.substr(0, dot);
    }
    return "Quest OS (Android " + Prop("ro.build.version.release") + ")";
#else
    return "unknown";
#endif
}

std::string DefaultSettingsDir()
{
#if defined(__APPLE__)
    std::string home = EnvVar("HOME");
    return home.empty() ? std::string() : home + "/Library/Application Support/UA Reforged";
#elif defined(_WIN32)
    std::string la = EnvVar("LOCALAPPDATA");
    return la.empty() ? std::string() : la + "\\UA Reforged";
#else
    return std::string(); // Quest: the app passes its private storage (UA_SETTINGS_DIR)
#endif
}

#if defined(_WIN32)
int DefaultSend(const std::string &url, const std::string &body, int timeoutMs)
{
    std::wstring wurl = Wide(url);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[1024];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if ( !WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) )
        return -1;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    int status = -1;
    HINTERNET s = WinHttpOpen(L"UA-Reforged", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = NULL, r = NULL;
    if ( s )
    {
        WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        c = WinHttpConnect(s, host, uc.nPort, 0);
    }
    if ( c )
        r = WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if ( r )
    {
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
        DWORD n = (DWORD)body.size();
        if ( WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1L, (LPVOID)body.data(), n, n, 0) &&
             WinHttpReceiveResponse(r, NULL) )
        {
            DWORD code = 0, sz = sizeof(code);
            if ( WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                     &code, &sz, WINHTTP_NO_HEADER_INDEX) )
                status = (int)code;
        }
        WinHttpCloseHandle(r);
    }
    if ( c ) WinHttpCloseHandle(c);
    if ( s ) WinHttpCloseHandle(s);
    return status;
}
#elif !defined(__APPLE__)
int DefaultSend(const std::string &, const std::string &, int)
{
    return -1; // Quest: registered by the app (SetSender); other platforms: never sends
}
#endif

} // namespace UsageStats
