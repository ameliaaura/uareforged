// Anonymous usage stats: the crash handler.
//
// While stats are on, a crash writes one line to usage-stats-crash.txt (the fields of the "crash" event in
// docs/usage-stats.md): the signal / exception code, the app version and build, the faulting module's BASE
// NAME and the offset in it, up to 32 return addresses as module base name + offset, the level and preset at
// the time and the minutes played. No paths, no memory contents, no names. Then the crash goes on to the
// system as before (macOS crash reporter, Windows Error Reporting, Android tombstones): the previous handler
// is put back and the signal re-raised / the next filter called.
//
// POSIX (macOS, Quest): sigaction with SA_SIGINFO | SA_ONSTACK | SA_NODEFER on an alternate stack; the walk
// follows frame pointers (arm64 / x86-64) and a fault during the walk ends it (siglongjmp). Only
// async-signal-safe calls, plus dladdr() for the module name (best effort). Windows: an unhandled-exception
// filter that unwinds with RtlLookupFunctionEntry / RtlVirtualUnwind (x64 unwind data), and SIGABRT.
#include "usage_stats.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#endif

namespace UsageStats
{

namespace
{
volatile sig_atomic_t g_enabled = 0;
volatile int g_level = -1;
char g_preset[16] = "original";
char g_version[40] = "";
char g_build[48] = "dev";
bool g_installed = false;
#ifdef _WIN32
wchar_t g_path[1024];
uint64_t g_startMs = 0;
uint64_t NowMs() { return GetTickCount64(); }
#else
char g_path[1024];
uint64_t g_startMs = 0;
uint64_t NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

// a fixed buffer, filled without malloc / printf (signal-safe)
struct Line
{
    char b[6144];
    size_t n = 0;
    void s(const char *t)
    {
        while ( *t && n < sizeof(b) - 2 ) b[n++] = *t++;
    }
    void hex(uint64_t v)
    {
        char t[19];
        int i = 18;
        t[i] = 0;
        do { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while ( v && i > 2 );
        t[--i] = 'x';
        t[--i] = '0';
        s(t + i);
    }
    void dec(long v)
    {
        char t[24];
        int i = 23;
        t[i] = 0;
        bool neg = v < 0;
        unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
        do { t[--i] = (char)('0' + u % 10); u /= 10; } while ( u && i > 1 );
        if ( neg ) t[--i] = '-';
        s(t + i);
    }
    // a module base name: the part after the last / or \, characters outside [0-9A-Za-z._+-] -> _
    void module(const char *path)
    {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if ( *p == '/' || *p == '\\' ) base = p + 1;
        size_t k = 0;
        for (const char *p = base; *p && k < 64 && n < sizeof(b) - 2; p++, k++)
        {
            char c = *p;
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '+' || c == '-';
            b[n++] = ok ? c : '_';
        }
        if ( k == 0 ) s("unknown");
    }
};

void CopyStr(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

struct Frame
{
    char module[72];
    uint64_t offset;
};

// the common part of the line; frames[0] = the faulting address
void Compose(Line &l, const char *signal, const Frame *frames, int count)
{
    l.s("event=crash\tversion=");
    l.s(g_version);
    l.s("\tbuild=");
    l.s(g_build);
    l.s("\tpreset=");
    l.s(g_preset);
    if ( g_level >= 0 )
    {
        l.s("\tlevel=");
        l.dec(g_level);
    }
    l.s("\tsignal=");
    l.s(signal);
    l.s("\tmodule=");
    l.s(count > 0 ? frames[0].module : "unknown");
    l.s("\toffset=");
    l.hex(count > 0 ? frames[0].offset : 0);
    l.s("\tframes=");
    for (int i = 0; i < count && i < 32; i++)
    {
        if ( i ) l.s(",");
        l.s(frames[i].module);
        l.s("+");
        l.hex(frames[i].offset);
    }
    l.s("\tminutes=");
    l.dec((long)((NowMs() - g_startMs) / 60000));
    l.s("\n");
}

#ifdef _WIN32
// ------------------------------------------------------------------ Windows
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = NULL;
volatile LONG g_busy = 0;

void Resolve(uint64_t addr, Frame *f)
{
    HMODULE m = NULL;
    f->offset = addr;
    CopyStr(f->module, sizeof(f->module), "unknown");
    if ( GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)addr, &m) && m )
    {
        wchar_t w[MAX_PATH];
        DWORD n = GetModuleFileNameW(m, w, MAX_PATH);
        char a[MAX_PATH];
        DWORD k = 0;
        for (; k < n && k < MAX_PATH - 1; k++) a[k] = (w[k] < 128) ? (char)w[k] : '_';
        a[k] = 0;
        Line l;
        l.module(a);
        l.b[l.n] = 0;
        CopyStr(f->module, sizeof(f->module), l.b);
        f->offset = addr - (uint64_t)(uintptr_t)m;
    }
}

int Walk(CONTEXT ctx, Frame *out, int max)
{
    int n = 0;
#if defined(_M_X64) || defined(__x86_64__)
    for (; n < max; n++)
    {
        const DWORD64 pc = ctx.Rip;
        if ( !pc ) break;
        Resolve(pc, &out[n]);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, NULL);
        if ( !fn )
        {
            // leaf function: the return address is on top of the stack
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        }
        else
        {
            void *handlerData = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fn, &ctx, &handlerData, &establisher, NULL);
        }
    }
#else
    (void)ctx; (void)max;
#endif
    return n;
}

void WriteLine(const Line &l)
{
    HANDLE h = CreateFileW(g_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if ( h == INVALID_HANDLE_VALUE ) return;
    DWORD w = 0;
    WriteFile(h, l.b, (DWORD)l.n, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

LONG WINAPI Filter(EXCEPTION_POINTERS *ep)
{
    if ( g_enabled && InterlockedExchange(&g_busy, 1) == 0 && ep && ep->ContextRecord && ep->ExceptionRecord )
    {
        static Frame frames[32];
        int n = Walk(*ep->ContextRecord, frames, 32);
        char sig[16] = "EXC_";
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        for (int i = 0; i < 8; i++) sig[4 + i] = "0123456789ABCDEF"[(code >> (28 - 4 * i)) & 15];
        sig[12] = 0;
        static Line l;
        l.n = 0;
        Compose(l, sig, frames, n);
        WriteLine(l);
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int)
{
    if ( g_enabled && InterlockedExchange(&g_busy, 1) == 0 )
    {
        static Frame frames[32];
        void *pcs[34];
        USHORT n = RtlCaptureStackBackTrace(1, 33, pcs, NULL);
        int k = 0;
        for (USHORT i = 0; i < n && k < 32; i++) Resolve((uint64_t)(uintptr_t)pcs[i], &frames[k++]);
        static Line l;
        l.n = 0;
        Compose(l, "SIGABRT", frames, k);
        WriteLine(l);
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void Install()
{
    g_prevFilter = SetUnhandledExceptionFilter(Filter);
    signal(SIGABRT, OnAbort);
}

#else
// ------------------------------------------------------------------ POSIX (macOS, Android)
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
const char *const kNames[] = {"SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE", "SIGABRT", "SIGTRAP"};
const int kNumSignals = 6;
struct sigaction g_old[kNumSignals];
volatile sig_atomic_t g_busy = 0, g_walking = 0;
sigjmp_buf g_walkJump;

// return addresses may carry pointer-authentication bits (arm64e system libraries) or a tag byte
uint64_t Strip(uint64_t a)
{
#if defined(__aarch64__) || defined(__arm64__)
    return a & 0x0000007FFFFFFFFFull;
#else
    return a;
#endif
}

void Resolve(uint64_t addr, Frame *f)
{
    Dl_info di;
    memset(&di, 0, sizeof(di));
    f->offset = addr;
    CopyStr(f->module, sizeof(f->module), "unknown");
    if ( dladdr((void *)(uintptr_t)addr, &di) && di.dli_fname && di.dli_fbase )
    {
        Line l;
        l.module(di.dli_fname);
        l.b[l.n] = 0;
        CopyStr(f->module, sizeof(f->module), l.b);
        f->offset = addr - (uint64_t)(uintptr_t)di.dli_fbase;
    }
}

void Registers(void *ucv, uint64_t *pc, uint64_t *fp, uint64_t *lr)
{
    *pc = *fp = *lr = 0;
    ucontext_t *uc = (ucontext_t *)ucv;
    if ( !uc ) return;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    *pc = uc->uc_mcontext->__ss.__pc;
    *fp = uc->uc_mcontext->__ss.__fp;
    *lr = uc->uc_mcontext->__ss.__lr;
#elif defined(__APPLE__) && defined(__x86_64__)
    *pc = uc->uc_mcontext->__ss.__rip;
    *fp = uc->uc_mcontext->__ss.__rbp;
#elif defined(__ANDROID__) && defined(__aarch64__)
    *pc = uc->uc_mcontext.pc;
    *fp = uc->uc_mcontext.regs[29];
    *lr = uc->uc_mcontext.regs[30];
#elif defined(__linux__) && defined(__x86_64__)
    *pc = uc->uc_mcontext.gregs[REG_RIP];
    *fp = uc->uc_mcontext.gregs[REG_RBP];
#endif
}

int Walk(void *uc, Frame *out, int max)
{
    uint64_t pc, fp, lr;
    Registers(uc, &pc, &fp, &lr);
    static volatile int n;
    n = 0;
    if ( !pc ) return 0;
    Resolve(Strip(pc), &out[n++]);
    // a fault while reading a frame ends the walk here
    if ( sigsetjmp(g_walkJump, 1) != 0 )
    {
        g_walking = 0;
        return n;
    }
    g_walking = 1;
    uint64_t prev = 0;
    while ( n < max && fp && (fp & 7) == 0 && fp > prev )
    {
        const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
        uint64_t ret = Strip(frame[1]);
        uint64_t next = frame[0];
        if ( !ret ) break;
        Resolve(ret, &out[n++]); // return addresses as they are (the instruction after the call)
        prev = fp;
        fp = next;
        if ( fp - prev > (64ull << 20) ) break; // left the stack
    }
    if ( n == 1 && lr ) Resolve(Strip(lr), &out[n++]);
    g_walking = 0;
    return n;
}

void Handler(int sig, siginfo_t *si, void *uc)
{
    int idx = 0;
    for (; idx < kNumSignals; idx++)
        if ( kSignals[idx] == sig ) break;
    if ( g_walking )
        siglongjmp(g_walkJump, 1); // a fault inside our own walk
    if ( g_enabled && !g_busy && idx < kNumSignals )
    {
        g_busy = 1;
        static Frame frames[32];
        int n = Walk(uc, frames, 32);
        static Line l;
        l.n = 0;
        Compose(l, kNames[idx], frames, n);
        int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if ( fd >= 0 )
        {
            ssize_t w = write(fd, l.b, l.n);
            (void)w;
            fsync(fd);
            close(fd);
        }
    }
    // on to the system's crash reporting, as if we had never been here
    if ( idx < kNumSignals )
        sigaction(sig, &g_old[idx], NULL);
    else
        signal(sig, SIG_DFL);
    if ( !si || si->si_code <= 0 || sig == SIGABRT )
        raise(sig); // sent by kill/raise/abort: send it again
    // a hardware fault re-runs the faulting instruction and goes to the old handler
}

void Install()
{
    static char altStack[64 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, NULL); // the thread that installs (the main thread): stack overflows still report
    for (int i = 0; i < kNumSignals; i++)
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = Handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigaction(kSignals[i], &sa, &g_old[i]);
    }
}
#endif
} // namespace

void InstallCrashHandler(const std::string &crashPath, const char *version, const char *build)
{
    if ( g_installed || crashPath.empty() )
        return;
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, crashPath.c_str(), -1, g_path, 1023);
    if ( n <= 0 ) return;
#else
    if ( crashPath.size() >= sizeof(g_path) ) return;
    CopyStr(g_path, sizeof(g_path), crashPath.c_str());
#endif
    CopyStr(g_version, sizeof(g_version), version);
    CopyStr(g_build, sizeof(g_build), build);
    g_startMs = NowMs();
    // the crash test needs the handler; ordinary runs only want it while stats are on (CrashHandlerEnabled)
    Install();
    g_installed = true;
}

void CrashHandlerEnabled(bool on) { g_enabled = on ? 1 : 0; }

void CrashHandlerContext(const char *preset, int level)
{
    if ( preset ) CopyStr(g_preset, sizeof(g_preset), preset);
    g_level = level;
}

#ifdef UA_TEST_HOOKS
void TestCrashNow(const char *how)
{
    if ( how && !strcmp(how, "abort") )
        abort();
    volatile int *p = (volatile int *)(uintptr_t)8;
    *p = 1;
}
#endif

} // namespace UsageStats
