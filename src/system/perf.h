#ifndef SYSTEM_PERF_H_INCLUDED
#define SYSTEM_PERF_H_INCLUDED

// Reforge performance: measurement aids. Everything here is inert unless one of
// the env vars below is set; the always-on part is integer byte accounting on
// texture alloc/free (no GL calls, no logging).
//
//   UA_FRAMETIME_LOG=<file>  per-frame log (gfx.cpp LogFrameTime). With it set,
//                            the hitch buckets below are timed and appended to
//                            each line:  t_ms frame_ms tex_ms vbo_ms shd_ms
//                            dec_ms polys v2draws
//   UA_PERF_LOG=<file>       level-load summary lines (also printed to stdout):
//                            load time, texture MB by source (orig / hd), PNG
//                            decode ms, upload ms, VBO ms, RSS / footprint.
//                            Setting either variable enables the timers.
//
// Buckets (CPU wall time spent inside the GL call / decode, accumulated per
// frame and per level load):
//   TEX  glTexImage2D / glCompressedTexImage2D / glGenerateMipmap
//   VBO  static mesh VBO uploads (MeshMakeVBO)
//   SHD  glCompileShader / glLinkProgram
//   DEC  image decode of disk maps (IMG_Load of hd-model PNG / DDS read)
//   PROC ypaworld->Process (simulation + RenderGame), RND RenderGame (CPU side
//        of the 3D pass incl. GL draw submission), SWAP System::Flip (buffer
//        swap; absorbs GPU back-pressure). EndFrame runs inside Process, so
//        sim = PROC - RND - SWAP and other = frame - PROC (events, sound, shell).
// Driver work deferred past the call (first-use upload / shader variant
// compile at draw time) is NOT visible here and shows up as "other".

#include <stdint.h>

namespace Perf
{

enum Bucket { TEX = 0, VBO, SHD, DEC, PROC, RND, SWAP, NBUCKET };

bool On();
uint64_t Now();
double TicksToMs(uint64_t t);
void Add(Bucket b, uint64_t ticks);

struct Scope
{
    Bucket b;
    uint64_t t0;
    explicit Scope(Bucket bb) : b(bb), t0(On() ? Now() : 0) {}
    void Stop() { if (t0) Add(b, Now() - t0); t0 = 0; }
    ~Scope() { Stop(); }
};

// Texture memory accounting (bytes incl. the mip chain); hd = reforge disk maps.
void TexAlloc(bool hd, int64_t bytes);
int64_t TexBytes(bool hd);
int TexCount(bool hd);
uint64_t RgbaBytes(int w, int h, bool mips);

// Per-frame counters (reset by TakeFrame).
void AddPolys(int n);
void AddV2Draw();
struct FrameCounters { double ms[NBUCKET]; int polys; int v2draws; };
void TakeFrame(FrameCounters *out);

// Level load summary.
void LevelLoadBegin(int id);
void LevelLoadEnd(int id, bool ok);

// Memory (MB): resident set and, on macOS, phys_footprint (what Activity
// Monitor calls Memory; includes GPU allocations charged to the process).
void MemoryMB(double *rss, double *footprint);

void Log(const char *fmt, ...);

}

#endif
