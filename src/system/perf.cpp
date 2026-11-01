#include "perf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo from kernel32, no psapi link
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace Perf
{

namespace
{
int gOn = -1;
FILE *gLog = NULL;

uint64_t gFrame[NBUCKET] = {};
uint64_t gLoad[NBUCKET] = {};
int gPolys = 0;
int gV2Draws = 0;

int64_t gTexBytes[2] = {0, 0};
int gTexCount[2] = {0, 0};

uint64_t gLoadT0 = 0;
int64_t gLoadTex0[2] = {0, 0};
}

bool On()
{
    if (gOn < 0)
    {
        const char *a = getenv("UA_FRAMETIME_LOG");
        const char *b = getenv("UA_PERF_LOG");
        gOn = ((a && a[0]) || (b && b[0])) ? 1 : 0;
        if (b && b[0])
            gLog = fopen(b, "a");
    }
    return gOn == 1;
}

uint64_t Now()
{
    return SDL_GetPerformanceCounter();
}

double TicksToMs(uint64_t t)
{
    return (double)t * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void Add(Bucket b, uint64_t ticks)
{
    gFrame[b] += ticks;
    gLoad[b] += ticks;
}

uint64_t RgbaBytes(int w, int h, bool mips)
{
    uint64_t total = 0;
    for (;;)
    {
        total += (uint64_t)w * (uint64_t)h * 4;
        if (!mips || (w == 1 && h == 1))
            break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

void TexAlloc(bool hd, int64_t bytes)
{
    gTexBytes[hd ? 1 : 0] += bytes;
    gTexCount[hd ? 1 : 0] += bytes > 0 ? 1 : (bytes < 0 ? -1 : 0);
}

int64_t TexBytes(bool hd) { return gTexBytes[hd ? 1 : 0]; }
int TexCount(bool hd) { return gTexCount[hd ? 1 : 0]; }

void AddPolys(int n) { gPolys += n; }
void AddV2Draw() { gV2Draws++; }

void TakeFrame(FrameCounters *out)
{
    for (int i = 0; i < NBUCKET; ++i)
    {
        out->ms[i] = TicksToMs(gFrame[i]);
        gFrame[i] = 0;
    }
    out->polys = gPolys;
    out->v2draws = gV2Draws;
    gPolys = 0;
    gV2Draws = 0;
}

void MemoryMB(double *rss, double *footprint)
{
    *rss = 0.0;
    *footprint = 0.0;
#if defined(__APPLE__)
    task_vm_info_data_t vm;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &cnt) == KERN_SUCCESS)
    {
        *rss = vm.resident_size / 1048576.0;
        *footprint = vm.phys_footprint / 1048576.0;
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        *rss = pmc.WorkingSetSize / 1048576.0;
        *footprint = pmc.PagefileUsage / 1048576.0;
    }
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f)
    {
        long pages = 0, res = 0;
        if (fscanf(f, "%ld %ld", &pages, &res) == 2)
            *rss = res * 4096.0 / 1048576.0;
        fclose(f);
    }
#endif
}

void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    fflush(stdout);
    if (gLog)
    {
        fputs(buf, gLog);
        fflush(gLog);
    }
}

void LevelLoadBegin(int id)
{
    if (!On())
        return;
    for (int i = 0; i < NBUCKET; ++i)
        gLoad[i] = 0;
    gLoadTex0[0] = gTexBytes[0];
    gLoadTex0[1] = gTexBytes[1];
    gLoadT0 = Now();
    Log("perf: level %d load begin\n", id);
}

void LevelLoadEnd(int id, bool ok)
{
    if (!On() || !gLoadT0)
        return;
    double rss, fp;
    MemoryMB(&rss, &fp);
    const double mb = 1.0 / 1048576.0;
    Log("perf: level %d load %s ms=%.1f | tex orig %.1f MB (%d) hd %.1f MB (%d) total %.1f MB"
        " | decode %.1f ms upload %.1f ms vbo %.1f ms shader %.1f ms | rss %.0f MB footprint %.0f MB\n",
        id, ok ? "ok" : "FAILED", TicksToMs(Now() - gLoadT0),
        gTexBytes[0] * mb, gTexCount[0], gTexBytes[1] * mb, gTexCount[1], (gTexBytes[0] + gTexBytes[1]) * mb,
        TicksToMs(gLoad[DEC]), TicksToMs(gLoad[TEX]), TicksToMs(gLoad[VBO]), TicksToMs(gLoad[SHD]), rss, fp);
    gLoadT0 = 0;
}

}
