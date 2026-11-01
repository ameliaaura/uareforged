// Reforge MARKS (battle marks). See reforge_marks.h.
#include <stdarg.h>
#include "includes.h"
#include "reforge_marks.h"
#include "reforge_marks_tex.h"
#include "gfxsettings.h"
#include "yw.h"
#include "ypabact.h"
#include "yparobo.h"
#include "base.h"
#include "system/gfx.h"
#include "world/consts.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <map>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ReforgeMarks
{
void LayTrackSeg(NC_STACK_ypaworld *yw, const vec3d &a, const vec3d &b, float w);
void StageTick(NC_STACK_ypaworld *yw, int32_t ts);

namespace
{
using ReforgeMarksTex::H;
using ReforgeMarksTex::H01;

enum { K_SCORCH = 0, K_CRATER = 1, K_TRACK = 2 };

struct Cfg
{
    int decals = 0;        // scorch + crater pool
    int tracks = 0;        // track segment pool
    int smoke = 0;         // smoulder sources
    int32_t life = 0;      // decal life ms
    int32_t trackLife = 0;
    int32_t smokeLife = 0;
    int32_t puffEvery = 0; // ms between puffs of one source
    bool craters = false, embers = false;
    int atlas = 0;
};

Cfg CfgFor(int lv)
{
    Cfg c;
    if (lv <= 0)
        return c;
    c.decals = 40; c.life = 75000; c.atlas = 512;
    if (lv >= 2)
    {
        c.decals = 120; c.life = 180000; c.atlas = 1024; c.craters = true;
        c.smoke = 10; c.smokeLife = 40000; c.puffEvery = 420;
    }
    if (lv >= 3)
    {
        c.decals = 240; c.life = 360000; c.tracks = 400; c.trackLife = 150000;
        c.smoke = 20; c.smokeLife = 75000; c.puffEvery = 340; c.embers = true;
    }
    return c;
}

struct DV { float x, y, z, u, v, m; };

struct Decal
{
    bool alive = false;
    int kind = 0;
    vec3d c;
    float r = 0;
    int32_t born = 0, life = 0;
    float a0 = 1.0;        // peak alpha
    std::vector<DV> v;     // grid (world space, lifted), m = edge/height mask
    int g = 0;             // grid side (verts); tracks: 2 x g
};

struct Smoulder
{
    bool alive = false;
    vec3d p;
    float size = 50;
    int32_t born = 0, life = 0;
    uint32_t seed = 0;
};

struct TrackSt
{
    vec3d last;
    int32_t seen = 0;
    bool has = false;
    int n = 0;
};

struct Pending { vec3d p; bool big; int32_t at; };

int gLevel = 0;
uint32_t gGen = 0xffffffff;

// UA_MARKS_PARTS (cost tooling): bit 1 decal draw, 2 smoke draw, 4 track laying, 8 embers. Default all.
int Parts()
{
    static int p = -1;
    if (p < 0)
        p = getenv("UA_MARKS_PARTS") ? atoi(getenv("UA_MARKS_PARTS")) : 15;
    return p;
}
// Smoke cost tuning: puff spacing multiplier (alpha compensated so the column keeps its density),
// quad crop (the blob fills the centre of its cell), near fade (puffs that would cover a big part of the
// screen fade out between nearK and nearK/2 puff radii from the eye), tail (puff ends at this part of its life).
// UA_MARKS_SMOKE="every/crop/nearK/tail" overrides (cost tooling only).
struct SmokeTune { float every = 2.2f, crop = 0.85f, nearK = 0.0f, tail = 0.85f; };   // defaults (cost: L1010 level 3 smoke ~0.9 -> ~0.1 ms)
const SmokeTune &STune()
{
    static SmokeTune t;
    static bool init = false;
    if (!init)
    {
        init = true;
        if (const char *e = getenv("UA_MARKS_SMOKE"))
            sscanf(e, "%f/%f/%f/%f", &t.every, &t.crop, &t.nearK, &t.tail);
    }
    return t;
}
Cfg gCfg;
int gDebug = -1;

std::vector<Decal> gDecals, gTracks;
std::vector<Smoulder> gSmoke;
std::map<uint32_t, TrackSt> gTrackSt;
std::vector<Pending> gPending;
int32_t gNow = 0;
bool gDirty = false;
int32_t gLastRebuild = -100000;
int32_t gLastSweep = 0;
NC_STACK_ypaworld *gYw = NULL;

ResBitmap *gTex = NULL;
int gTexSize = 0;
GFX::TMesh *gDecalMesh = NULL;
GFX::TMesh *gSmokeMesh = NULL;

const float kCell = 0.25f;

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *fmt, ...)
{
    if (gDebug < 0)
        gDebug = getenv("UA_MARKS_DEBUG") ? 1 : 0;
    if (!gDebug)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void FreeGl()
{
    if (gTex)
    {
        GFX::Engine.FreeTexture(gTex);
        delete gTex;
        gTex = NULL;
        gTexSize = 0;
    }
    for (GFX::TMesh **m : {&gDecalMesh, &gSmokeMesh})
        if (*m)
        {
            GFX::Engine.MeshFreeVBO(*m);
            delete *m;
            *m = NULL;
        }
}

void ClearPools()
{
    gDecals.assign(gCfg.decals, Decal());
    gTracks.assign(gCfg.tracks, Decal());
    gSmoke.assign(gCfg.smoke, Smoulder());
    gTrackSt.clear();
    gPending.clear();
    gDirty = true;
}

bool EnsureTex()
{
    if (gTex && gTexSize == gCfg.atlas)
        return true;
    if (gTex)
    {
        GFX::Engine.FreeTexture(gTex);
        delete gTex;
        gTex = NULL;
    }
    std::vector<uint8_t> px;
    const uint32_t t0 = SDL_GetTicks();
    ReforgeMarksTex::Build(px, gCfg.atlas);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), gCfg.atlas, gCfg.atlas, 32, gCfg.atlas * 4,
                                                        SDL_PIXELFORMAT_ABGR8888);
    if (!s)
        return false;
    gTex = GFX::Engine.LoadRawTexture(s, NULL, false);
    SDL_FreeSurface(s);
    gTexSize = gCfg.atlas;
    printf("marks: atlas %d px built in %u ms\n", gCfg.atlas, SDL_GetTicks() - t0);
    return gTex != NULL;
}

// Ground (sector collision surface) height under x,z, searching down from refY - 250.
bool GroundY(NC_STACK_ypaworld *yw, double x, double z, double refY, double *y, double span = 650.0)
{
    ypaworld_arg136 a;
    a.stPos = vec3d(x, refY - 250.0, z);
    a.vect = vec3d(0.0, span, 0.0);
    a.flags = 0;
    yw->ypaworld_func136(&a);
    if (!a.isect)
        return false;
    *y = a.isectPos.y;
    return true;
}

int Oldest(std::vector<Decal> &pool)
{
    int best = 0;
    for (size_t i = 0; i < pool.size(); ++i)
    {
        if (!pool[i].alive)
            return (int)i;
        if (pool[i].born < pool[best].born)
            best = (int)i;
    }
    return best;
}

// Height-sampled grid decal. cell = atlas cell index, rot in radians.
bool LayDecal(NC_STACK_ypaworld *yw, std::vector<Decal> &pool, int kind, const vec3d &c, float r, float rot, int cell,
              int32_t life, float a0, float aspect = 1.0f)
{
    if (pool.empty())
        return false;
    const int g = r > 70.0f ? 8 : 6;
    Decal d;
    d.alive = true;
    d.kind = kind;
    d.c = c;
    d.r = r;
    d.born = gNow;
    d.life = life;
    d.a0 = a0;
    d.g = g;
    d.v.resize(g * g);
    const float cs = cosf(rot), sn = sinf(rot);
    const float u0 = (cell % 4) * kCell, v0 = (cell / 4) * kCell;
    int masked = 0;
    for (int j = 0; j < g; ++j)
        for (int i = 0; i < g; ++i)
        {
            const float s = -1.0f + 2.0f * i / (g - 1), t = -1.0f + 2.0f * j / (g - 1);
            const double ls = s * aspect, lt = t / aspect;
            const double dx = (ls * cs - lt * sn) * r, dz = (ls * sn + lt * cs) * r;
            double y = c.y;
            const bool hit = GroundY(yw, c.x + dx, c.z + dz, c.y, &y);
            DV &o = d.v[j * g + i];
            o.x = c.x + dx;
            o.z = c.z + dz;
            o.y = y - 0.8;  // y is down: a hair above the surface
            o.u = u0 + (s * 0.5f + 0.5f) * kCell;
            o.v = v0 + (t * 0.5f + 0.5f) * kCell;
            // a vertex that landed on a wall/roof far from the centre's level fades out
            o.m = (hit && fabs(y - c.y) < 0.45 * r + 25.0) ? 1.0f : 0.0f;
            if (o.m == 0.0f)
                ++masked;
        }
    if (masked > g * g / 2)
    {
        Log("marks: decal kind %d r %.0f rejected (%d of %d vertices off the surface)\n", kind, r, masked, g * g);
        return false; // mostly off the surface (cliff edge / building wall)
    }
    const int slot = Oldest(pool);
    Log("marks: decal kind %d r %.0f cell %d slot %d masked %d\n", kind, r, cell, slot, masked);
    pool[slot] = d;
    gDirty = true;
    return true;
}

// Merge a new scorch/crater into a fresh one already there (rapid fire on one spot).
bool Refresh(const vec3d &c, float r, int kind)
{
    for (Decal &d : gDecals)
        if (d.alive && d.kind == kind)
        {
            const double dx = d.c.x - c.x, dz = d.c.z - c.z;
            if (dx * dx + dz * dz < (0.4 * std::max(d.r, r)) * (0.4 * std::max(d.r, r)) && r <= d.r * 1.25f)
            {
                d.born = gNow;
                gDirty = true;
                return true;
            }
        }
    return false;
}

void Scorch(NC_STACK_ypaworld *yw, const vec3d &c, float r, uint32_t seed)
{
    if (Refresh(c, r, K_SCORCH))
        return;
    LayDecal(yw, gDecals, K_SCORCH, c, r, H01(seed) * 6.2831853f, ReforgeMarksTex::CELL_SCORCH + (H(seed) >> 8) % 4,
             gCfg.life, 0.58f + 0.22f * H01(seed + 1), 0.85f + 0.3f * H01(seed + 2));
}

void Crater(NC_STACK_ypaworld *yw, const vec3d &c, float r, uint32_t seed)
{
    if (!gCfg.craters || Refresh(c, r, K_CRATER))
        return;
    // craters stay near their texture orientation (the baked shading matches one light direction): +-20 deg,
    // stretched up to 15 % so no two read as the same stamped hole
    LayDecal(yw, gDecals, K_CRATER, c, r * (0.85f + 0.3f * H01(seed + 3)), (H01(seed + 4) - 0.5f) * 0.7f,
             ReforgeMarksTex::CELL_CRATER + (H(seed) >> 9) % 2, gCfg.life, 0.85f + 0.15f * H01(seed + 5), 0.85f + 0.3f * H01(seed + 6));
}

void Smoke(const vec3d &p, float size, uint32_t seed, float lifeMul = 1.0f)
{
    if (gSmoke.empty())
        return;
    int best = 0;
    for (size_t i = 0; i < gSmoke.size(); ++i)
    {
        if (!gSmoke[i].alive) { best = (int)i; break; }
        if (gSmoke[i].born < gSmoke[best].born)
            best = (int)i;
    }
    Smoulder &s = gSmoke[best];
    s.alive = true;
    s.p = p;
    s.size = size;
    s.born = gNow;
    s.life = (int32_t)(gCfg.smokeLife * lifeMul * (0.8 + 0.4 * H01(seed + 9)));
    s.seed = seed;
}

uint32_t Seed(const vec3d &p)
{
    return H((uint32_t)(int)p.x * 2654435761u ^ (uint32_t)(int)p.z * 40503u ^ (uint32_t)gNow);
}

// ---------------------------------------------------------------------------
// Batches

void Fog(NC_STACK_ypaworld *yw, GFX::TRenderNode &n)
{
    n.FogLength = yw->_normalFadeLength > 0 ? yw->_normalFadeLength : 600;
    n.FogStart = (yw->_normalVizLimit > 0 ? yw->_normalVizLimit : 4096) - n.FogLength;
}

float Fade(const Decal &d)
{
    const float t = (float)(gNow - d.born) / (float)std::max(1, d.life);
    if (t >= 1.0f)
        return 0.0f;
    if (t < 0.7f)
        return 1.0f;
    return 1.0f - ReforgeMarksTex::Smooth((t - 0.7f) / 0.3f);
}

bool AnyFading()
{
    for (const std::vector<Decal> *pool : {&gDecals, &gTracks})
        for (const Decal &d : *pool)
            if (d.alive && (gNow - d.born) > 0.7f * d.life)
                return true;
    return false;
}

void RebuildDecals()
{
    if (!gDecalMesh)
    {
        gDecalMesh = new GFX::TMesh();
        gDecalMesh->Mat.Flags = GFX::RFLAGS_TEXTURED | GFX::RFLAGS_LUMTRACY | GFX::RFLAGS_FX_ALPHA | GFX::RFLAGS_FOG;
    }
    GFX::TMesh &m = *gDecalMesh;
    m.Vertexes.clear();
    m.Indixes.clear();
    // draw order: tracks, scorch, craters (craters sit on their scorch)
    for (int pass = 0; pass < 3; ++pass)
    {
        std::vector<Decal> &pool = pass == 0 ? gTracks : gDecals;
        for (Decal &d : pool)
        {
            if (!d.alive)
                continue;
            if (pass > 0 && d.kind != (pass == 1 ? K_SCORCH : K_CRATER))
                continue;
            const float f = Fade(d);
            if (f <= 0.0f)
            {
                d.alive = false;
                d.v.clear();
                continue;
            }
            const size_t base = m.Vertexes.size();
            if (base + d.v.size() > 60000)
                break;
            for (const DV &s : d.v)
            {
                GFX::TVertex v;
                v.Pos = vec3f(s.x, s.y, s.z);
                v.TexCoord.tu = s.u;
                v.TexCoord.tv = s.v;
                v.Color = GFX::TGLColor(1.0, 1.0, 1.0, d.a0 * f * s.m);
                v.ComputedColor = v.Color;
                m.Vertexes.push_back(v);
            }
            const int gx = d.kind == K_TRACK ? 2 : d.g, gz = d.kind == K_TRACK ? (int)d.v.size() / 2 : d.g;
            for (int j = 0; j + 1 < gz; ++j)
                for (int i = 0; i + 1 < gx; ++i)
                {
                    const GFX::IndexType a = base + j * gx + i, b = a + 1, c = a + gx, e = c + 1;
                    // the track basis (side, forward) is mirrored against the decal grid's (x, z): opposite winding
                    if (d.kind == K_TRACK)
                        m.Indixes.insert(m.Indixes.end(), {a, c, b, b, c, e});
                    else
                        m.Indixes.insert(m.Indixes.end(), {a, b, c, b, e, c});
                }
        }
    }
    if (!m.Vertexes.empty())
        GFX::Engine.MeshMakeVBO(&m);
    gDirty = false;
    gLastRebuild = gNow;
}

struct Puff { vec3d pv; float s, ang, a; float r, g, b; int cell; };
std::vector<Puff> gPuffBuf; // QueueSmoke's per-frame list: capacity kept across frames (freed at level 0)

void BuildSmoke(TF::TForm3D *view, std::vector<Puff> &out)
{
    const SmokeTune &T = STune();
    const float puffLife = 5200.0f;
    const float aExp = std::max(1.0f, T.every); // fewer puffs: each one denser, same column opacity
    for (Smoulder &s : gSmoke)
    {
        if (!s.alive)
            continue;
        const int32_t age = gNow - s.born;
        if (age >= s.life)
        {
            s.alive = false;
            continue;
        }
        const float I = powf(1.0f - (float)age / s.life, 0.8f);
        const int every = std::max(1, (int)(gCfg.puffEvery * T.every));
        int k0 = std::max(0, (int)((age - puffLife) / every));
        for (int k = k0; k * every <= age; ++k)
        {
            const float pa = (float)(age - k * every);
            if (pa < 0.0f || pa >= puffLife * T.tail)
                continue;
            const uint32_t h = H(s.seed ^ (uint32_t)k * 2246822519u);
            // the source thins out as it dies: whole puffs drop away
            const float Ik = powf(1.0f - (float)(k * every) / s.life, 0.8f);
            if (H01(h) > 0.25f + 0.75f * Ik)
                continue;
            const float tt = pa / puffLife, sec = pa * 0.001f;
            const float jx = (H01(h + 1) - 0.5f) * s.size * 0.5f, jz = (H01(h + 2) - 0.5f) * s.size * 0.5f;
            vec3d w = s.p + vec3d(jx + 10.0 * powf(sec, 1.3f), -(14.0 + 26.0 * sec) * (0.8 + 0.4 * H01(h + 3)) * sec * 0.7 - s.size * 0.25,
                                  jz + 6.0 * powf(sec, 1.3f));
            Puff p;
            p.pv = view->CalcSclRot.Transform(w - view->CalcPos);
            if (p.pv.z < 1.0)
                continue;
            p.s = s.size * (0.55f + 1.6f * tt) * (0.8f + 0.4f * H01(h + 4));
            p.s = std::min(p.s, (float)p.pv.z);
            p.ang = H01(h + 5) * 6.2831853f + (H01(h + 6) - 0.5f) * 0.5f * sec;
            p.a = 0.85f * Ik * ReforgeMarksTex::SStep(0.0f, 0.08f, tt) * powf(1.0f - tt, 1.1f);
            if (aExp > 1.0f)
                p.a = 1.0f - powf(1.0f - std::min(p.a, 0.99f), aExp);
            if (T.tail < 1.0f)
                p.a *= 1.0f - ReforgeMarksTex::SStep(T.tail * 0.8f, T.tail, tt);
            if (T.nearK > 0.0f)
                p.a *= ReforgeMarksTex::SStep(T.nearK * 0.5f * p.s, T.nearK * p.s, (float)p.pv.z);
            // young smoke oily-dark, older greyer; strength follows the source
            const float grey = 0.16f + 0.22f * tt + 0.06f * H01(h + 7);
            p.r = grey * 1.04f; p.g = grey; p.b = grey * 0.94f;
            p.cell = ReforgeMarksTex::CELL_SMOKE + (h >> 11) % 4;
            if (p.a > 0.01f)
                out.push_back(p);
        }
        if (gCfg.embers && I > 0.35f && (Parts() & 8))
            for (int e = 0; e < 5; ++e)
            {
                const uint32_t h = H(s.seed ^ (uint32_t)e * 374761393u);
                vec3d w = s.p + vec3d((H01(h) - 0.5) * s.size * 0.7, -4.0 - 14.0 * H01(h + 1), (H01(h + 2) - 0.5) * s.size * 0.7);
                Puff p;
                p.pv = view->CalcSclRot.Transform(w - view->CalcPos);
                if (p.pv.z < 1.0)
                    continue;
                const float fl = 0.5f + 0.5f * sinf(gNow * (0.004f + 0.006f * H01(h + 3)) + H01(h + 4) * 6.28f);
                p.s = 7.0f + 6.0f * H01(h + 5);
                p.ang = 0;
                p.a = (I - 0.35f) / 0.65f * (0.35f + 0.65f * fl);
                p.r = 1.0f; p.g = 0.75f; p.b = 0.55f;
                p.cell = ReforgeMarksTex::CELL_EMBER;
                out.push_back(p);
            }
    }
}

void QueueSmoke(NC_STACK_ypaworld *yw, area_arg_65 *rrg)
{
    std::vector<Puff> &puffs = gPuffBuf;
    puffs.clear();
    BuildSmoke(rrg->ViewTForm, puffs);
    if (puffs.empty())
        return;
    std::sort(puffs.begin(), puffs.end(), [](const Puff &a, const Puff &b) { return a.pv.z > b.pv.z; });
    if (!gSmokeMesh)
    {
        gSmokeMesh = new GFX::TMesh();
        gSmokeMesh->Mat.Flags = GFX::RFLAGS_TEXTURED | GFX::RFLAGS_LUMTRACY | GFX::RFLAGS_FX_ALPHA | GFX::RFLAGS_FOG;
    }
    GFX::TMesh &m = *gSmokeMesh;
    m.Vertexes.clear();
    m.Indixes.clear();
    const float px[4] = {-1, -1, 1, 1}, py[4] = {1, -1, -1, 1};
    const float e = 1.5f / std::max(1, gTexSize);
    for (const Puff &p0 : puffs)
    {
        // embers keep their full cell; smoke quads are cropped to the blob (less empty fill)
        const float cr = p0.cell == ReforgeMarksTex::CELL_EMBER ? 1.0f : STune().crop;
        Puff p = p0;
        p.s *= cr;
        const float mc = 0.5f * kCell * (1.0f - cr);
        const float u0 = (p.cell % 4) * kCell + e + mc, v0 = (p.cell / 4) * kCell + e + mc, u1 = u0 + kCell - 2 * e - 2 * mc, v1 = v0 + kCell - 2 * e - 2 * mc;
        const float pu[4] = {u0, u0, u1, u1}, pv[4] = {v0, v1, v1, v0};
        const float c = cosf(p.ang) * p.s, sn = sinf(p.ang) * p.s;
        const GFX::IndexType b = m.Vertexes.size();
        for (int i = 0; i < 4; ++i)
        {
            GFX::TVertex v;
            v.Pos = vec3f(p.pv.x + c * px[i] - sn * py[i], p.pv.y + sn * px[i] + c * py[i], p.pv.z);
            v.TexCoord.tu = pu[i];
            v.TexCoord.tv = pv[i];
            v.Color = GFX::TGLColor(p.r, p.g, p.b, p.a);
            v.ComputedColor = v.Color;
            m.Vertexes.push_back(v);
        }
        m.Indixes.insert(m.Indixes.end(), {(GFX::IndexType)b, (GFX::IndexType)(b + 2), (GFX::IndexType)(b + 1),
                                           (GFX::IndexType)b, (GFX::IndexType)(b + 3), (GFX::IndexType)(b + 2)});
    }
    GFX::Engine.MeshMakeVBO(&m);
    GFX::TRenderNode &n = GFX::Engine.AllocRenderNode();
    n = GFX::TRenderNode(GFX::TRenderNode::TYPE_MESH);
    n.Mesh = &m;
    n.Flags = m.Mat.Flags;
    n.Tex = gTex;
    n.Color = GFX::TGLColor(1, 1, 1, 1);
    n.TForm = mat4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    n.Distance = puffs.front().pv.z; // with the other transparents, by its farthest puff
    n.TimeStamp = rrg->timeStamp;
    n.FrameTime = rrg->frameTime;
    Fog(yw, n);
    GFX::GFXEngine::Instance.QueueRenderMesh(&n);
}

void QueueDecals(NC_STACK_ypaworld *yw, area_arg_65 *rrg)
{
    // re-upload only when a mark came/went, or (every 1.5 s) while some mark is in its fade-out window
    if (gDirty || gNow < gLastRebuild || (gNow - gLastRebuild >= 1500 && AnyFading()))
        RebuildDecals();
    if (!gDecalMesh || gDecalMesh->Vertexes.empty())
        return;
    TF::TForm3D *view = rrg->ViewTForm;
    // world -> view, scaled by k towards the eye: every vertex keeps its pixel,
    // its depth shrinks by 0.25 % (1 unit at 400, 5 at 2000): no z-fighting with the ground it lies on.
    const double k = 0.9975;
    const vec3d a = view->CalcSclRot.Transform(vec3d(1, 0, 0)) * k;
    const vec3d b = view->CalcSclRot.Transform(vec3d(0, 1, 0)) * k;
    const vec3d c = view->CalcSclRot.Transform(vec3d(0, 0, 1)) * k;
    const vec3d t = view->CalcSclRot.Transform(vec3d(0, 0, 0) - view->CalcPos) * k;
    GFX::TRenderNode &n = GFX::Engine.AllocRenderNode();
    n = GFX::TRenderNode(GFX::TRenderNode::TYPE_MESH);
    n.Mesh = gDecalMesh;
    n.Flags = gDecalMesh->Mat.Flags;
    n.Tex = gTex;
    n.Color = GFX::TGLColor(1, 1, 1, 1);
    n.TForm = mat4x4(a.x, b.x, c.x, t.x,
                     a.y, b.y, c.y, t.y,
                     a.z, b.z, c.z, t.z,
                     0.0, 0.0, 0.0, 1.0);
    n.Distance = 1.0e9; // first of the transparent list: under every particle and smoke puff
    n.TimeStamp = rrg->timeStamp;
    n.FrameTime = rrg->frameTime;
    Fog(yw, n);
    GFX::GFXEngine::Instance.QueueRenderMesh(&n);
}

// ---------------------------------------------------------------------------
// Tooling: UA_MARKS_FILL (worst-case pools), UA_MARKS_STAGE (scripted battle).

void Fill(NC_STACK_ypaworld *yw)
{
    std::vector<vec3d> hubs;
    for (NC_STACK_ypabact *r : yw->_unitsList)
        if (r && r->_bact_type == BACT_TYPES_ROBO)
            hubs.push_back(r->_position);
    if (hubs.empty())
        return;
    uint32_t s = 12345;
    int nd = 0, nt = 0, ns = 0;
    for (int i = 0; i < (int)gDecals.size() * 3 && nd < (int)gDecals.size(); ++i)
    {
        s = H(s + 1);
        const vec3d &h = hubs[s % hubs.size()];
        const float ang = H01(s + 2) * 6.2831853f, dist = 200.0f + 1500.0f * sqrtf(H01(s + 3));
        vec3d c(h.x + cos(ang) * dist, 0, h.z + sin(ang) * dist);
        double y;
        if (!GroundY(yw, c.x, c.z, -29750.0, &y, 50000.0))
            continue;
        c.y = y;
        const bool crater = gCfg.craters && (s & 3) == 0;
        const size_t before = std::count_if(gDecals.begin(), gDecals.end(), [](const Decal &d) { return d.alive; });
        if (crater)
            Crater(yw, c, 55.0f + 30.0f * H01(s + 4), s);
        else
            Scorch(yw, c, 40.0f + 50.0f * H01(s + 4), s);
        nd += std::count_if(gDecals.begin(), gDecals.end(), [](const Decal &d) { return d.alive; }) > (long)before;
        if (ns < (int)gSmoke.size() && crater)
        {
            Smoke(c, 70.0f, s);
            gSmoke[ns].born = gNow - (int32_t)(H01(s + 5) * 0.3 * gCfg.smokeLife);
            gSmoke[ns].life = 10 * 60 * 1000; // stays for the whole run
            ++ns;
        }
    }
    // tracks: random walks
    for (int w = 0; nt < (int)gTracks.size() && w < 200; ++w)
    {
        s = H(s + 7);
        const vec3d &h = hubs[s % hubs.size()];
        vec3d p(h.x + (H01(s + 1) - 0.5) * 2400.0, 0, h.z + (H01(s + 2) - 0.5) * 2400.0);
        float yaw = H01(s + 3) * 6.2831853f;
        for (int k = 0; k < 20 && nt < (int)gTracks.size(); ++k)
        {
            vec3d q(p.x + cos(yaw) * 55.0, 0, p.z + sin(yaw) * 55.0);
            double y0, y1;
            if (!GroundY(yw, p.x, p.z, -29750.0, &y0, 50000.0) || !GroundY(yw, q.x, q.z, -29750.0, &y1, 50000.0))
                break;
            p.y = y0; q.y = y1;
            LayTrackSeg(yw, p, q, 44.0f);
            ++nt;
            p = q;
            yaw += (H01(s + 10 + k) - 0.5f) * 0.4f;
        }
    }
    for (Decal &d : gDecals)
        if (d.alive) d.life = 10 * 60 * 1000;
    for (Decal &d : gTracks)
        if (d.alive) d.life = 10 * 60 * 1000;
    printf("marks: fill: %d decals (cap %d), %d track segments (cap %d), %d smoke sources (cap %d)\n",
           (int)std::count_if(gDecals.begin(), gDecals.end(), [](const Decal &d) { return d.alive; }), (int)gDecals.size(),
           (int)std::count_if(gTracks.begin(), gTracks.end(), [](const Decal &d) { return d.alive; }), (int)gTracks.size(),
           (int)std::count_if(gSmoke.begin(), gSmoke.end(), [](const Smoulder &d) { return d.alive; }), (int)gSmoke.size());
}

} // namespace

// Track segment from a to b (ground points), width w: 2 x 3 grid along the path.
void LayTrackSeg(NC_STACK_ypaworld *yw, const vec3d &a, const vec3d &b, float w)
{
    if (gTracks.empty())
        return;
    Decal d;
    d.alive = true;
    d.kind = K_TRACK;
    d.c = (a + b) * 0.5;
    d.r = w;
    d.born = gNow;
    d.life = gCfg.trackLife;
    d.a0 = 0.72f;
    d.g = 3;
    vec3d f = b - a;
    f.y = 0;
    const double len = f.length();
    if (len < 1.0)
        return;
    f /= len;
    const vec3d side(-f.z, 0, f.x);
    const int cell = ReforgeMarksTex::CELL_TRACK + (H((uint32_t)(int)a.x ^ (uint32_t)(int)a.z) >> 7) % 2;
    const float u0 = (cell % 4) * kCell, v0 = (cell / 4) * kCell;
    const float e = 2.0f / 1024.0f;
    d.v.resize(6);
    for (int j = 0; j < 3; ++j)
    {
        const double t = j * 0.5;
        const vec3d m = a + (b - a) * t;
        double y = a.y + (b.y - a.y) * t;
        if (j == 1)
            GroundY(yw, m.x, m.z, y, &y);
        for (int i = 0; i < 2; ++i)
        {
            const double s = i == 0 ? -0.5 : 0.5;
            DV &o = d.v[j * 2 + i];
            o.x = m.x + side.x * s * w;
            o.z = m.z + side.z * s * w;
            double yy = y;
            GroundY(yw, o.x, o.z, y, &yy);
            o.y = (fabs(yy - y) < 30.0 ? yy : y) - 0.8;
            o.u = u0 + (i == 0 ? e : kCell - e);
            o.v = v0 + e + (float)t * (kCell - 2 * e);
            o.m = 1.0f;
        }
    }
    gTracks[Oldest(gTracks)] = d;
    gDirty = true;
}

int Level()
{
    if (gGen != GfxSettings::Generation())
    {
        gGen = GfxSettings::Generation();
        int lv = std::max(0, std::min(3, gfxLevel("MARKS", 0)));
        if (lv != gLevel)
        {
            gLevel = lv;
            gCfg = CfgFor(lv);
            if (lv == 0)
            {
                FreeGl();
                gDecals.clear(); gTracks.clear(); gSmoke.clear(); gTrackSt.clear(); gPending.clear();
                std::vector<Puff>().swap(gPuffBuf);
            }
            else
                ClearPools();
            printf("UA_GFX_MARKS: level %d (decals %d, tracks %d, smoke %d)\n", lv, gCfg.decals, gCfg.tracks, gCfg.smoke);
        }
    }
    return gLevel;
}

void OnLevelLoad()
{
    if (!Level())
        return;
    ClearPools();
    gNow = 0;
    gLastRebuild = -100000;
}

void OnImpact(NC_STACK_ypaworld *yw, const vec3d &pos, int energy)
{
    if (!Level() || !yw)
        return;
    double y;
    if (!GroundY(yw, pos.x, pos.z, pos.y - 30.0, &y) || y - pos.y > 90.0)
        return; // hit in the air (a flyer): nothing on the ground
    const float r = std::min(85.0f, 18.0f + 14.0f * sqrtf(std::max(500, energy) / 3000.0f));
    const vec3d c(pos.x, y, pos.z);
    Log("marks: impact e=%d r=%.0f at %.0f %.0f %.0f\n", energy, r, c.x, c.y, c.z);
    Scorch(yw, c, r, Seed(c));
}

void OnWreck(NC_STACK_ypaworld *yw, NC_STACK_ypabact *b)
{
    if (!Level() || !b || !yw || b->_owner == 0)
        return;
    if (b->_bact_type == BACT_TYPES_MISSLE || b->_bact_type == BACT_TYPES_ROBO)
        return;
    double y;
    if (!GroundY(yw, b->_position.x, b->_position.z, b->_position.y - 30.0, &y) || y - b->_position.y > 250.0)
        return;
    const vec3d c(b->_position.x, y, b->_position.z);
    const float rad = std::max(20.0f, std::min(80.0f, b->_radius));
    const uint32_t s = Seed(c) ^ b->_gid;
    Log("marks: wreck vid %d type %d radius %.0f at %.0f %.0f %.0f\n", b->_vehicleID, b->_bact_type, b->_radius, c.x, c.y, c.z);
    Scorch(yw, c, std::min(110.0f, rad * 2.2f), s);
    Crater(yw, c, std::min(85.0f, rad * 1.3f), s + 1);
    Smoke(c, std::min(110.0f, std::max(45.0f, rad * 1.5f)), s + 2);
}

void OnBuilding(NC_STACK_ypaworld *yw, const vec3d &pos, bool big)
{
    if (!Level() || !yw)
        return;
    // the building's collision changes to rubble after this call: lay the marks a moment later,
    // and let the marks that sat on the old building fade out now
    for (std::vector<Decal> *pool : {&gDecals, &gTracks})
        for (Decal &d : *pool)
            if (d.alive)
            {
                const double dx = d.c.x - pos.x, dz = d.c.z - pos.z;
                if (dx * dx + dz * dz < 170.0 * 170.0 && gNow - d.born < d.life - 1200)
                    d.born = gNow - d.life + 1200;
            }
    gPending.push_back({pos, big, gNow + 600});
    gDirty = true;
}

void OnUnitDrawn(NC_STACK_ypaworld *yw, NC_STACK_ypabact *b)
{
    if (gLevel < 3 || !b || !yw || (b->_bact_type != BACT_TYPES_TANK && b->_bact_type != BACT_TYPES_CAR) || !(Parts() & 4))
        return;
    if (b->_status != BACT_STATUS_NORMAL)
        return;
    TrackSt &st = gTrackSt[b->_gid];
    st.seen = gNow;
    vec3d p = b->_position;
    if (!st.has)
    {
        st.has = true;
        st.last = p;
        return;
    }
    vec3d d = p - st.last;
    d.y = 0;
    const double len = d.length();
    const double step = 55.0;
    if (len < step)
        return;
    if (len > step * 4.0)
    {
        st.last = p; // jumped (teleport / long unseen): no segment
        return;
    }
    double y0, y1;
    // tanks ride _overeof above the ground; search from the hull down
    if (GroundY(yw, st.last.x, st.last.z, st.last.y, &y0) && GroundY(yw, p.x, p.z, p.y, &y1) &&
        y0 - st.last.y < b->_overeof + 60.0 && y1 - p.y < b->_overeof + 60.0)
    {
        const float w = std::max(30.0f, std::min(80.0f, b->_radius * 1.35f));
        LayTrackSeg(yw, vec3d(st.last.x, y0, st.last.z), vec3d(p.x, y1, p.z), w);
    }
    st.last = p;
}

void Render(NC_STACK_ypaworld *yw, area_arg_65 *rrg)
{
    StageTick(yw, rrg ? rrg->timeStamp : 0); // UA_MARKS_STAGE tooling: runs at every level (same battle), inert unless set
    if (!Level() || !yw || !rrg || !rrg->ViewTForm)
        return;
    if (rrg->timeStamp < gNow - 1000)
        ClearPools(); // a new level / restart without OnLevelLoad
    gNow = rrg->timeStamp;
    gYw = yw;
    if (!EnsureTex())
        return;

    static int fill = -1;
    static NC_STACK_ypaworld *filled = NULL;
    static int32_t fillAt = -1;
    if (fill < 0)
        fill = getenv("UA_MARKS_FILL") ? atoi(getenv("UA_MARKS_FILL")) : 0;
    if (fill && filled != yw)
    {
        if (fillAt < 0)
            fillAt = gNow + (fill > 1 ? fill : 0); // UA_MARKS_FILL=<delay ms> (1 = first frame)
        else if (gNow >= fillAt)
        {
            Fill(yw);
            filled = yw;
        }
    }

    for (size_t i = 0; i < gPending.size();)
    {
        if (gNow >= gPending[i].at)
        {
            const Pending p = gPending[i];
            gPending.erase(gPending.begin() + i);
            double y;
            if (GroundY(yw, p.p.x, p.p.z, p.p.y - 400.0, &y))
            {
                const vec3d c(p.p.x, y, p.p.z);
                const uint32_t s = Seed(c);
                Log("marks: building at %.0f %.0f %.0f big %d\n", c.x, c.y, c.z, (int)p.big);
                Scorch(yw, c, p.big ? 170.0f : 130.0f, s);
                Smoke(c + vec3d(30, 0, -20), p.big ? 120.0f : 95.0f, s + 3, 1.4f);
                if (p.big)
                    Smoke(c + vec3d(-60, 0, 50), 100.0f, s + 4, 1.2f);
            }
        }
        else
            ++i;
    }

    if (gNow - gLastSweep > 5000)
    {
        gLastSweep = gNow;
        for (auto it = gTrackSt.begin(); it != gTrackSt.end();)
            it = (gNow - it->second.seen > 3000) ? gTrackSt.erase(it) : std::next(it);
    }

    if (Parts() & 1)
        QueueDecals(yw, rrg);
    if (!gSmoke.empty() && (Parts() & 2))
        QueueSmoke(yw, rrg);
}

// ---------------------------------------------------------------------------
// UA_MARKS_STAGE=1: a deterministic battle for look sheets (tooling). On the
// level it runs on (designed for 97, the showroom), around a point A east of
// the user host: 4 tanks drive a loop (tracks), a Wasp 450 up fires a weapon at
// a hashed ground point every 1.1 s (scorch), a unit is spawned + killed every
// 6 s (wreck: crater + smoke), a building sector is hit at 50 s and 100 s.
// Game time from the first stage frame; with UA_FIXED_PERIOD every run has the
// same events at the same frames, whatever the MARKS level.
namespace
{
int gStage = -1;
int32_t gT0 = -1;
bool gCam = false;
vec3d gCamEye, gCamTgt;
NC_STACK_ypabact *gTanks[4] = {};
NC_STACK_ypabact *gLauncher = NULL;
int gShots = 0, gKills = 0, gWay = 0, gBld = 0;

NC_STACK_ypabact *Spawn(NC_STACK_ypaworld *yw, int vid, const vec3d &pos)
{
    ypaworld_arg146 a;
    a.vehicle_id = vid;
    a.pos = pos;
    NC_STACK_ypabact *u = yw->ypaworld_func146(&a);
    if (!u)
        return NULL;
    static uint32_t cmd = 0x7e0000;
    u->_commandID = cmd++;
    u->_owner = yw->_userRobo->_owner;
    u->_host_station = dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo);
    yw->_userRobo->AddSubject(u);
    setState_msg s;
    s.newStatus = BACT_STATUS_NORMAL;
    s.setFlags = 0;
    s.unsetFlags = 0;
    u->SetState(&s);
    return u;
}

bool Alive(NC_STACK_ypaworld *yw, NC_STACK_ypabact *p)
{
    if (!p)
        return false;
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        if (r == p) return true;
        if (!r) continue;
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            if (c == p) return true;
            if (!c) continue;
            for (NC_STACK_ypabact *u : c->_kidList)
                if (u == p) return true;
        }
    }
    return false;
}

vec3d Ground(NC_STACK_ypaworld *yw, double x, double z)
{
    double y = 0;
    if (!GroundY(yw, x, z, -29750.0, &y, 50000.0))
    {
        Common::Point c = World::PositionToSectorID(vec3d(x, 0, z));
        if (c.x >= 0 && c.y >= 0 && c.x < yw->_mapSize.x && c.y < yw->_mapSize.y)
            y = yw->_cells(c.x, c.y).height;
    }
    return vec3d(x, y, z);
}

int32_t gST = 0;

void StageFrame(NC_STACK_ypaworld *yw)
{
    if (gStage < 0)
    {
        const char *v = getenv("UA_MARKS_STAGE");
        gStage = (v && v[0] && v[0] != '0') ? atoi(v) : 0;
    }
    if (!gStage || !yw->_userRobo)
        return;
    if (gT0 < 0)
    {
        gT0 = gST + 2000;
        printf("marks-stage: on\n");
    }
    const int32_t t = gST - gT0;
    // battle centre: 1.5 sectors east of the user host, on its ground
    const vec3d hp = yw->_userRobo->_position;
    const vec3d A = Ground(yw, hp.x + 1800.0, hp.z - 300.0);
    {
        vec3d dir(0.40, -0.70, -0.59);
        dir.normalise();
        gCam = true;
        gCamTgt = A;
        gCamEye = A + dir * 1350.0;
    }
    if (t < 0)
        return;
    const int32_t end = 150000;
    static bool said = false;
    if (t >= end)
    {
        if (!said)
        {
            said = true;
            printf("marks-stage: done t=%d shots %d kills %d\n", t, gShots, gKills);
            fflush(stdout);
        }
        return;
    }
    // tanks
    if (!gTanks[0])
        for (int i = 0; i < 4; ++i)
            gTanks[i] = Spawn(yw, 1 + (i % 3), Ground(yw, A.x - 500.0 + i * 90.0, A.z + 420.0) - vec3d(0, 40, 0));
    const int way = t / 9000;
    if (way != gWay || t < 50)
    {
        gWay = way;
        // a loop of 4 corners, each tank offset
        static const double wp[4][2] = {{-480, 420}, {480, 380}, {460, -420}, {-460, -380}};
        for (int i = 0; i < 4; ++i)
            if (Alive(yw, gTanks[i]))
            {
                setTarget_msg m;
                m.tgt_type = BACT_TGT_TYPE_CELL;
                m.priority = 0;
                const double *w = wp[(way + 1) % 4];
                m.tgt_pos = vec3d(A.x + w[0] + (i - 1.5) * 70.0, 0, A.z + w[1] + (i - 1.5) * 40.0);
                gTanks[i]->SetTarget(&m);
            }
    }
    // launcher + shots
    if (!gLauncher || !Alive(yw, gLauncher))
    {
        gLauncher = Spawn(yw, 6, A + vec3d(0, -450, 0));
    }
    if (gLauncher)
    {
        gLauncher->_position = A + vec3d(0, -450, 0);
        const int n = t / 1100;
        while (gShots <= n)
        {
            const uint32_t h = H(0xabc0u + gShots);
            const float ang = H01(h) * 6.2831853f, d = 650.0f * sqrtf(H01(h + 1));
            vec3d tg = Ground(yw, A.x + cos(ang) * d, A.z + sin(ang) * d);
            static const int weap[] = {4, 6, 5, 3, 6, 4};
            bact_arg79 a;
            vec3d dd = tg - gLauncher->_position;
            dd.normalise();
            a.direction = dd;
            a.start_point = vec3d(0, 0, 0);
            a.tgType = BACT_TGT_TYPE_CELL;
            Common::Point c = World::PositionToSectorID(tg);
            a.target.pcell = (c.x >= 0 && c.y >= 0 && c.x < yw->_mapSize.x && c.y < yw->_mapSize.y) ? &yw->_cells(c.x, c.y) : NULL;
            a.tgt_pos = tg;
            a.weapon = weap[gShots % 6];
            a.g_time = gST;
            a.flags = 0;
            gLauncher->_weapon_time = 0;
            if (a.target.pcell)
                gLauncher->LaunchMissile(&a);
            ++gShots;
        }
    }
    // spawn + kill: wrecks
    {
        const int n = (t - 3000) / 6000;
        while (t >= 3000 && gKills <= n)
        {
            const uint32_t h = H(0xdef0u + gKills);
            const float ang = H01(h) * 6.2831853f, d = 600.0f * sqrtf(H01(h + 1));
            static const int vids[] = {2, 3, 26, 1};
            NC_STACK_ypabact *u = Spawn(yw, vids[gKills % 4], Ground(yw, A.x + cos(ang) * d, A.z + sin(ang) * d) - vec3d(0, 40, 0));
            if (u)
            {
                bact_arg84 k;
                k.energy = -2000000;
                k.unit = NULL;
                u->ModifyEnergy(&k);
            }
            ++gKills;
        }
    }
    // building hits (sector damage) at 50 s and 100 s on the sector nearest A + (600, -500) / (-650, 550)
    if ((t >= 50000 && gBld == 0) || (t >= 100000 && gBld == 1))
    {
        const vec3d p = gBld == 0 ? A + vec3d(600, 0, -500) : A + vec3d(-650, 0, 550);
        for (int i = 0; i < 6; ++i)
        {
            yw_arg129 a;
            a.field_0 = 0;
            a.pos = p;
            a.field_10 = 400000;
            a.OwnerID = World::OWNER_RECALC;
            a.unit = NULL;
            yw->ypaworld_func129(&a);
        }
        printf("marks-stage: building hit %d at %.0f %.0f\n", gBld, p.x, p.z);
        ++gBld;
    }
}
} // namespace

void StageTick(NC_STACK_ypaworld *yw, int32_t ts)
{
    if (gStage == 0 || !yw)
        return;
    gST = ts;
    StageFrame(yw);
}

bool StageCamera(vec3d *eye, vec3d *target)
{
    if (!gCam)
        return false;
    *eye = gCamEye;
    *target = gCamTgt;
    return true;
}

}
