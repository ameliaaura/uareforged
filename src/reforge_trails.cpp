// Reforge TRAILS - see reforge_trails.h for the contract.
//
// Env reference
//   UA_GFX_TRAILS=0..3        level (or a preset through gfxsettings); 0 = original
//   UA_TRAILS_DIR=<dir>       pack directory (default <hd-models dir>/trails)
//   UA_TRAILS_STAGE=1         scripted firing range on level 97 (captures, cost); inert unless set
//   UA_TRAILS_STAGE_SHOT=<ms> stage shots every <ms> of stage time (trs_<ms>.ppm)
//   UA_TRAILS_DEBUG=1         per-second pool counts on stdout

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "includes.h"
#include "reforge_trails.h"
#include "gfxsettings.h"
#include "particle.h"
#include "area.h"
#include "bitmap.h"
#include "base.h"
#include "yw.h"
#include "ypabact.h"
#include "ypamissile.h"
#include "yparobo.h"
#include "system/gfx.h"
#include "system/tform.h"
#include "world/consts.h"

namespace ReforgeTrails
{

namespace
{

enum Style { ST_NONE = 0, ST_MISSILE, ST_MISSILE_LITE, ST_SHELL, ST_ENERGY };
const int kPal = 5;          // faction palettes (rows in tr_hot.png)
const int kFade = 4;         // brightness steps per hot mesh
const int kSmokeFrames = 16;
const int kSmokeTints = 3;   // 0 grey, 1 soot (Ghorkov / Black Sect), 2 steel (Taerkasten)

// Fixed pools (ring buffers, the oldest entry is overwritten).
const int kCapFlash = 96;
const int kCapTracer = 192;
const int kCapHead = 96;
const int kCapPuff = 1600;   // level 3; level 2 uses kCapPuffL2 of it
const int kCapPuffL2 = 640;

struct Tune
{
    float mgLen, mgBurst, shellLen, wakeLen;   // tracer lengths (world units), MG tracers per shot
    float puffSpacing, puffLife, puffS0, puffS1, puffRise, muzzlePuffs;
    int capPuff;
};
// index = level (0 unused)
const Tune kTune[4] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {180, 2, 100, 130, 0, 0, 0, 0, 0, 0, 0},
    {220, 2, 130, 170, 13, 1800, 14, 58, 8, 3, kCapPuffL2},
    {300, 3, 170, 230, 9, 4000, 16, 110, 18, 6, kCapPuff},
};

struct Flash { vec3d pos, dir; float size, age, life; int pal; uint32_t seed; bool live; };
struct Tracer { vec3d a, b; float len, width, speed, age, delay; int pal; bool live; };
struct Head { const void *key; vec3d pos, dir; float carry; int seen, style, pal, tint; uint32_t seed; };
struct Puff { vec3d pos, vel; float age, life, s0, s1, gain; int tint; uint32_t seed; bool live; };

int gLevel = 0;
bool gInit = false;
uint32_t gGen = 0;
bool gPackTried = false, gPackOk = false;
std::string gDir;
std::vector<int> gWeaponStyle;      // by weapon id, -1 = by missile type
// 1998 trail sprites our smoke replaces from level 2 (trails.txt `hide <bitmap>`,
// today the FIRESTORM bead trail 070397AA-AC). Particles still emit and move (game RNG untouched); only their
// draw is skipped. Cache on the particle generator itself (NC_STACK_particle::_trailsHide*): stage count + epoch;
// the epoch bumps whenever the pools are cleared (level change / new level / set reload), so every generator is
// re-classified once after it.
std::set<std::string> gHide;
uint32_t gHideEpoch = 1;

std::string BitmapKey(std::string s)
{
    // rsrc names from IFF can carry NUL padding / spaces / a path: key on the visible file name, upper case
    size_t z = s.find('\0');
    if (z != std::string::npos)
        s.resize(z);
    while (!s.empty() && isspace((unsigned char)s.back()))
        s.pop_back();
    size_t sl = s.find_last_of("/\\:");
    if (sl != std::string::npos)
        s = s.substr(sl + 1);
    for (char &c : s)
        c = toupper((unsigned char)c);
    return s;
}

ResBitmap *gHotTex = NULL, *gSmokeTex = NULL;
GFX::TMesh *gStar[kPal][4][kFade] = {};
GFX::TMesh *gCone[kPal][2][kFade] = {};
GFX::TMesh *gStreak[kPal][kFade] = {};
GFX::TMesh *gGlow[kPal][kFade] = {};
GFX::TMesh *gSmoke[kSmokeTints][kSmokeFrames] = {};

std::vector<Flash> gFlash;
std::vector<Tracer> gTracer;
std::vector<Head> gHead;
std::vector<Puff> gPuff;
size_t gFlashN = 0, gTracerN = 0, gPuffN = 0;
int gFrame = 1;
int32_t gLastTs = -1;
uint32_t gRng = 0x9e3779b9;

inline uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
inline float Rnd()   // own generator: the game's RNG is never touched
{
    gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
    return (gRng & 0xffffff) / 16777216.0f;
}

int PalForOwner(int owner)
{
    switch (owner)
    {
    case 2: return 4;          // Sulgogar
    case 3: return 3;          // Mykonian
    case 4: return 2;          // Taerkasten
    case 5: case 6: return 1;  // Black Sect, Ghorkov
    default: return 0;         // Resistance (and neutral)
    }
}
int TintForOwner(int owner)
{
    return (owner == 5 || owner == 6) ? 1 : (owner == 4 ? 2 : 0);
}

// Same convention as reforge_fx: with gfx.blending 0 the lumtracy pass is SRC_ALPHA / ONE_MINUS_SRC_ALPHA, so
// additive sprites carry luminance as alpha (and fade through alpha); with ONE / ONE they are premultiplied.
bool LumAlpha() { return !GFX::GFXEngine::can_destblend && GFX::GFXEngine::can_srcblend; }

void AdditivePixels(SDL_Surface *s)
{
    if (!LumAlpha())
        return;
    SDL_LockSurface(s);
    for (int y = 0; y < s->h; ++y)
    {
        uint8_t *p = (uint8_t *)s->pixels + y * s->pitch;
        for (int x = 0; x < s->w; ++x, p += 4)
        {
            int e[3], mx = 0;
            for (int c = 0; c < 3; ++c)
            {
                e[c] = (p[c] * p[3] + 127) / 255;
                mx = std::max(mx, e[c]);
            }
            if (mx <= 2)
                p[0] = p[1] = p[2] = p[3] = 0;
            else
            {
                for (int c = 0; c < 3; ++c)
                    p[c] = (uint8_t)std::min(255, (e[c] * 255 + mx / 2) / mx);
                p[3] = (uint8_t)mx;
            }
        }
    }
    SDL_UnlockSurface(s);
}

ResBitmap *LoadTex(const std::string &png, bool additive)
{
    SDL_Surface *s = IMG_Load(png.c_str());
    if (!s)
    {
        printf("trails: '%s' not loaded\n", png.c_str());
        return NULL;
    }
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c)
        return NULL;
    if (additive)
        AdditivePixels(c);
    ResBitmap *b = GFX::Engine.LoadRawTexture(c, NULL, false);
    SDL_FreeSurface(c);
    return b;
}

GFX::TMesh *MakeQuad(ResBitmap *tex, float x0, float y0, float x1, float y1, float texW, float texH,
                     uint32_t flags, const GFX::TGLColor &clr)
{
    const float e = 0.5;
    const float u0 = (x0 + e) / texW, v0 = (y0 + e) / texH, u1 = (x1 - e) / texW, v1 = (y1 - e) / texH;
    GFX::TMesh *m = new GFX::TMesh();
    m->Vertexes.resize(4);
    const float px[4] = {-0.5, -0.5, 0.5, 0.5};
    const float py[4] = {0.5, -0.5, -0.5, 0.5};
    const float pu[4] = {u0, u0, u1, u1};
    const float pv[4] = {v0, v1, v1, v0};
    for (int i = 0; i < 4; ++i)
    {
        m->Vertexes[i].Pos = vec3f(px[i], py[i], 0.0);
        m->Vertexes[i].TexCoord.tu = pu[i];
        m->Vertexes[i].TexCoord.tv = pv[i];
        m->Vertexes[i].TexCoordId = i;
        m->Vertexes[i].Color = clr;
    }
    m->Indixes.assign({0, 2, 1, 0, 3, 2});
    m->Mat.Tex = tex;
    m->Mat.Flags = flags;
    m->Mat.Color = clr;
    GFX::Engine.MeshMakeVBO(m);
    return m;
}

GFX::TGLColor HotColor(float k)
{
    return LumAlpha() ? GFX::TGLColor(1.0, 1.0, 1.0, k) : GFX::TGLColor(k, k, k, 1.0);
}

bool LoadPack()
{
    if (gPackTried)
        return gPackOk;
    gPackTried = true;
    if (const char *d = getenv("UA_TRAILS_DIR"))
        gDir = d;
    else
    {
        const char *h = getenv("UA_HDMODELS_DIR");
        gDir = std::string((h && h[0]) ? h : "hd-models") + "/trails";
    }
    FILE *f = fopen((gDir + "/trails.txt").c_str(), "r");
    if (!f)
    {
        printf("trails: no pack at %s (TRAILS stays off)\n", gDir.c_str());
        return false;
    }
    std::string hot = "tr_hot.png", smoke = "tr_smoke.png";
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        if (char *h = strchr(line, '#'))
            *h = 0;
        std::vector<std::string> tok;
        for (char *t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n"))
            tok.push_back(t);
        if (tok.size() >= 2 && tok[0] == "hot")
            hot = tok[1];
        else if (tok.size() >= 2 && tok[0] == "smoke")
            smoke = tok[1];
        else if (tok.size() >= 2 && tok[0] == "hide")
            gHide.insert(BitmapKey(tok[1]));
        else if (tok.size() >= 3 && tok[0] == "weapon")
        {
            int id = atoi(tok[1].c_str());
            if (id < 0 || id > 1023)
                continue;
            if ((int)gWeaponStyle.size() <= id)
                gWeaponStyle.resize(id + 1, -1);
            const std::string &s = tok[2];
            gWeaponStyle[id] = s == "missile" ? ST_MISSILE : s == "missile_lite" ? ST_MISSILE_LITE
                             : s == "shell" ? ST_SHELL : s == "energy" ? ST_ENERGY : ST_NONE;
        }
    }
    fclose(f);

    gHotTex = LoadTex(gDir + "/" + hot, true);
    gSmokeTex = LoadTex(gDir + "/" + smoke, false);
    if (!gHotTex || !gSmokeTex)
        return false;

    const uint32_t hf = GFX::RFLAGS_TEXTURED | GFX::RFLAGS_LUMTRACY;
    const uint32_t sf = hf | GFX::RFLAGS_FX_ALPHA;
    for (int p = 0; p < kPal; ++p)
        for (int k = 0; k < kFade; ++k)
        {
            const GFX::TGLColor c = HotColor((float)(k + 1) / kFade);
            for (int v = 0; v < 4; ++v)
                gStar[p][v][k] = MakeQuad(gHotTex, v * 128, p * 128, (v + 1) * 128, (p + 1) * 128, 1024, 1024, hf, c);
            for (int v = 0; v < 2; ++v)
                gCone[p][v][k] = MakeQuad(gHotTex, 512 + v * 256, p * 128, 512 + (v + 1) * 256, (p + 1) * 128, 1024, 1024, hf, c);
            gStreak[p][k] = MakeQuad(gHotTex, 0, 640 + p * 32, 512, 640 + (p + 1) * 32, 1024, 1024, hf, c);
            gGlow[p][k] = MakeQuad(gHotTex, 512 + p * 64, 640, 512 + (p + 1) * 64, 704, 1024, 1024, hf, c);
        }
    const float tint[kSmokeTints][4] = {{0.50, 0.49, 0.47, 0.75}, {0.27, 0.26, 0.25, 0.82}, {0.46, 0.48, 0.51, 0.75}};
    for (int t = 0; t < kSmokeTints; ++t)
        for (int i = 0; i < kSmokeFrames; ++i)
            gSmoke[t][i] = MakeQuad(gSmokeTex, (i % 4) * 128, (i / 4) * 128, (i % 4 + 1) * 128, (i / 4 + 1) * 128, 512, 512, sf,
                                    GFX::TGLColor(tint[t][0], tint[t][1], tint[t][2], tint[t][3]));
    gFlash.assign(kCapFlash, Flash());
    gTracer.assign(kCapTracer, Tracer());
    gHead.assign(kCapHead, Head());
    gPuff.assign(kCapPuff, Puff());
    printf("trails: pack %s loaded (%d weapon styles, %d hidden 1998 trail sprites)\n", gDir.c_str(),
           (int)gWeaponStyle.size(), (int)gHide.size());
    gPackOk = true;
    return true;
}

void ClearPools()
{
    if (++gHideEpoch == 0)
        gHideEpoch = 1;
    for (Flash &f : gFlash) f.live = false;
    for (Tracer &t : gTracer) t.live = false;
    for (Head &h : gHead) h.key = NULL;
    for (Puff &p : gPuff) p.live = false;
}

int StyleFor(NC_STACK_ypamissile *m)
{
    int id = m->_vehicleID;
    if (id >= 0 && id < (int)gWeaponStyle.size() && gWeaponStyle[id] >= 0)
        return gWeaponStyle[id];
    switch (m->GetMissileType())
    {
    case NC_STACK_ypamissile::MISL_BOMB: return ST_NONE;
    case NC_STACK_ypamissile::MISL_GRENADE: return ST_SHELL;
    default: return ST_MISSILE;
    }
}

void AddFlash(const vec3d &pos, const vec3d &dir, float size, float life, int pal)
{
    Flash &f = gFlash[gFlashN++ % gFlash.size()];
    f.pos = pos; f.dir = dir; f.size = size; f.age = 0; f.life = life; f.pal = pal; f.seed = Hash(gRng += 0x6d2b79f5);
    f.live = true;
}

void AddTracer(const vec3d &a, const vec3d &b, float len, float width, float speed, float delay, int pal)
{
    Tracer &t = gTracer[gTracerN++ % gTracer.size()];
    t.a = a; t.b = b; t.len = len; t.width = width; t.speed = speed; t.age = 0; t.delay = delay; t.pal = pal;
    t.live = true;
}

void AddPuff(const vec3d &pos, const vec3d &vel, float life, float s0, float s1, float gain, int tint)
{
    const Tune &tu = kTune[gLevel];
    if (tu.capPuff <= 0)
        return;
    Puff &p = gPuff[gPuffN++ % tu.capPuff];
    p.pos = pos; p.vel = vel; p.age = 0; p.life = life; p.s0 = s0; p.s1 = s1; p.gain = gain; p.tint = tint;
    p.seed = Hash(gRng += 0x632be5ab); p.live = true;
}

Head *FindHead(const void *key, bool create)
{
    Head *freeSlot = NULL, *oldest = NULL;
    for (Head &h : gHead)
    {
        if (h.key == key)
            return &h;
        if (!h.key && !freeSlot)
            freeSlot = &h;
        if (!oldest || h.seen < oldest->seen)
            oldest = &h;
    }
    if (!create)
        return NULL;
    Head *h = freeSlot ? freeSlot : oldest;
    *h = Head();
    h->key = key;
    return h;
}

// ---- drawing (view space, like ReforgeFx::RenderParticle) -------------------------------------------------
struct View { TF::TForm3D *v; area_arg_65 *r; };

void Queue(const View &vw, GFX::TMesh *mesh, const mat4x4 &tf, float dist)
{
    GFX::TRenderNode &rend = GFX::Engine.AllocRenderNode();
    rend = GFX::TRenderNode(GFX::TRenderNode::TYPE_MESH);
    rend.Distance = dist;
    rend.Flags = mesh->Mat.Flags | vw.r->flags;
    rend.Color = mesh->Mat.Color;
    rend.Tex = mesh->Mat.Tex;
    rend.Mesh = mesh;
    rend.TForm = tf;
    rend.TimeStamp = vw.r->timeStamp;
    rend.FrameTime = vw.r->frameTime;
    rend.FogStart = vw.r->fadeStart;
    rend.FogLength = vw.r->fadeLength;
    GFX::GFXEngine::Instance.QueueRenderMesh(&rend);
}

bool InView(const vec3d &p)
{
    return p.z > GFX::Engine.GetProjectionNear() && p.z < GFX::Engine.GetProjectionFar();
}

void Billboard(const View &vw, GFX::TMesh *mesh, const vec3d &world, float size, float ang)
{
    vec3d p = vw.v->CalcSclRot.Transform(world - vw.v->CalcPos);
    if (!InView(p))
        return;
    const float d = p.length();
    const float s = std::min(size, d);   // never larger than its camera distance (fx rule)
    const float c = cos(ang) * s, sn = sin(ang) * s;
    Queue(vw, mesh, mat4x4(c, -sn, 0.0, p.x, sn, c, 0.0, p.y, 0.0, 0.0, 1.0, p.z, 0.0, 0.0, 0.0, 1.0), d);
}

// Camera-facing ribbon from a to b (world), u along a->b (head at b).
void Ribbon(const View &vw, GFX::TMesh *mesh, const vec3d &a, const vec3d &b, float width)
{
    vec3d pa = vw.v->CalcSclRot.Transform(a - vw.v->CalcPos);
    vec3d pb = vw.v->CalcSclRot.Transform(b - vw.v->CalcPos);
    if (!InView(pa) && !InView(pb))
        return;
    vec3d ax = pb - pa;
    vec3d mid = (pa + pb) * 0.5;
    vec3d w = mid * ax;   // quad normal = ax x w ~ +mid: faces like a billboard (front faces are culled otherwise)
    float wl = w.length();
    if (wl < 1e-4)
        return;
    const float d = mid.length();
    width = std::max(width, d * 0.0045f);   // keep ~2-3 px at range
    w *= width / wl;
    vec3d n = ax * w;
    float nl = n.length();
    if (nl > 1e-6)
        n /= nl;
    Queue(vw, mesh, mat4x4(ax.x, w.x, n.x, mid.x, ax.y, w.y, n.y, mid.y, ax.z, w.z, n.z, mid.z, 0.0, 0.0, 0.0, 1.0), d);
}

int FadeIdx(float k) { return std::max(0, std::min(kFade - 1, (int)(k * kFade + 0.5f) - 1)); }

int gDbgT = 0;
} // namespace

int Level()
{
    if (!gInit || gGen != GfxSettings::Generation())
    {
        gInit = true;
        gGen = GfxSettings::Generation();
        int lv = gfxLevel("TRAILS", 0);
        if (lv > 0 && !LoadPack())
            lv = 0;
        if (lv != gLevel && gPackOk)
            ClearPools();
        gLevel = lv;
    }
    return gLevel;
}

bool HideParticle(NC_STACK_particle *gen)
{
    if (gLevel < 2 || gHide.empty() || !gen)
        return false;
    if (gen->_trailsHideEpoch != gHideEpoch || gen->_trailsHideStages != gen->_lifeStagesAdes.size())
    {
        gen->_trailsHideEpoch = gHideEpoch;
        gen->_trailsHideStages = gen->_lifeStagesAdes.size();
        gen->_trailsHide = false;
        for (NC_STACK_ade *ade : gen->_lifeStagesAdes)
        {
            NC_STACK_area *a = dynamic_cast<NC_STACK_area *>(ade);
            NC_STACK_bitmap *b = a ? (a->_texImg ? a->_texImg : a->_tracyImg) : NULL;
            if (b && gHide.count(BitmapKey(b->getRsrc_name())))
                gen->_trailsHide = true;
        }
    }
    return gen->_trailsHide;
}

void Launch(NC_STACK_ypamissile *m, NC_STACK_ypabact *launcher, const vec3d &pos)
{
    if (!gLevel || !m)
        return;
    const int st = StyleFor(m);
    const int pal = PalForOwner(launcher ? launcher->_owner : m->_owner);
    vec3d dir = m->_fly_dir;
    const float dl = dir.length();
    dir = dl > 1e-4 ? dir / dl : vec3d(0.0, 0.0, 1.0);
    Head *h = FindHead(m, true);
    h->pos = pos; h->dir = dir; h->carry = 0; h->seen = gFrame; h->style = st;
    h->pal = pal; h->tint = TintForOwner(m->_owner); h->seed = Hash((uint32_t)(size_t)m ^ gRng);
    if (st == ST_NONE)
        return;
    const Tune &tu = kTune[gLevel];
    switch (st)
    {
    case ST_SHELL:
        AddFlash(pos, dir, 85.0, 90.0, pal);
        for (int i = 0; i < (int)tu.muzzlePuffs; ++i)   // muzzle smoke (level 2+)
            AddPuff(pos + dir * (10.0 + 18.0 * i), dir * (60.0 + 40.0 * Rnd()) + vec3d(0.0, -6.0 - tu.puffRise, 0.0),
                    tu.puffLife * 0.8f, 20.0, tu.puffS1 * 0.8f, 1.0, h->tint);
        break;
    case ST_ENERGY:
        AddFlash(pos, dir, 50.0, 70.0, pal);
        break;
    default:
        AddFlash(pos, dir, 60.0, 80.0, pal);
        if (gLevel >= 2)
            AddPuff(pos - dir * 6.0, dir * -30.0, tu.puffLife * 0.7f, 20.0, tu.puffS1 * 0.7f, 1.0, h->tint);
        break;
    }
}

void MissileMoved(NC_STACK_ypamissile *m)
{
    if (!gLevel || !m)
        return;
    Head *h = FindHead(m, false);
    if (m->_status != BACT_STATUS_NORMAL)
    {
        if (h)
            h->key = NULL;   // hit / dead: exhaust and wake stop at once
        return;
    }
    const vec3d cur = m->_position;
    if (!h)
    {
        h = FindHead(m, true);
        h->style = StyleFor(m); h->pal = PalForOwner(m->_owner); h->tint = TintForOwner(m->_owner);
        h->pos = m->_old_pos; h->dir = vec3d(0.0, 0.0, 1.0); h->carry = 0;
        h->seed = Hash((uint32_t)(size_t)m ^ gRng);
    }
    vec3d seg = cur - h->pos;
    float sl = seg.length();
    if (sl > 600.0)   // a recycled object or a teleport: restart the trail here
    {
        h->pos = cur; h->carry = 0; sl = 0;
    }
    if (sl > 1e-3)
        h->dir = seg / sl;
    h->seen = gFrame;
    const Tune &tu = kTune[gLevel];
    if (gLevel >= 2 && (h->style == ST_MISSILE || h->style == ST_MISSILE_LITE) && sl > 1e-3)
    {
        const float sp = tu.puffSpacing * (h->style == ST_MISSILE_LITE ? 2.0f : 1.0f);
        const float sc = h->style == ST_MISSILE_LITE ? 0.8f : 1.0f;
        float at = sp - h->carry;
        while (at <= sl)
        {
            vec3d p = h->pos + h->dir * at - h->dir * 5.0;
            vec3d vel(18.0 * (Rnd() - 0.5), -tu.puffRise * (0.6 + 0.8 * Rnd()), 18.0 * (Rnd() - 0.5));
            AddPuff(p, vel, tu.puffLife * (0.8f + 0.4f * Rnd()), tu.puffS0 * sc, tu.puffS1 * sc * (0.8f + 0.4f * Rnd()), 1.0,
                    h->tint);
            at += sp;
        }
        h->carry = sl - (at - sp);
    }
    h->pos = cur;
}

void MgunShot(NC_STACK_ypabact *s, const vec3d &to)
{
    if (!gLevel || !s)
        return;
    const vec3d muz = s->_position + s->_rotation.Transpose().Transform(s->_fire_pos);
    vec3d d = to - muz;
    const float dl = d.length();
    if (dl < 1.0)
        return;
    d /= dl;
    const Tune &tu = kTune[gLevel];
    const int pal = PalForOwner(s->_owner);
    AddFlash(muz, d, 40.0, 60.0, pal);
    const int n = (int)tu.mgBurst;
    for (int i = 0; i < n; ++i)
    {
        vec3d j(Rnd() - 0.5, Rnd() - 0.5, Rnd() - 0.5);
        AddTracer(muz, to + j * (dl * 0.025), tu.mgLen, 6.5, 2600.0, i * 75.0, pal);
    }
}

void Render(area_arg_65 *rrg, int32_t dtime)
{
    if (!gLevel || !rrg || !rrg->ViewTForm)
        return;
    if (rrg->timeStamp < gLastTs)   // a new level (game time restarted)
        ClearPools();
    gLastTs = rrg->timeStamp;
    const float dt = (float)std::max(0, dtime);
    const float fs = dt * 0.001f;
    View vw = {rrg->ViewTForm, rrg};
    const Tune &tu = kTune[gLevel];
    int nF = 0, nT = 0, nH = 0, nP = 0;

    for (Flash &f : gFlash)
    {
        if (!f.live)
            continue;
        f.age += dt;
        if (f.age >= f.life)
        {
            f.live = false;
            continue;
        }
        ++nF;
        const float t = f.age / f.life;
        const int fi = FadeIdx(1.0f - t * 0.75f);
        const float sz = f.size * (0.75f + 0.5f * t) * (0.85f + 0.3f * ((f.seed >> 4 & 255) / 255.0f));
        Billboard(vw, gStar[f.pal][f.seed & 3][fi], f.pos, sz, (f.seed >> 12 & 1023) / 1023.0f * 6.283f);
        Ribbon(vw, gCone[f.pal][(f.seed >> 3) & 1][fi], f.pos - f.dir * (sz * 0.18f), f.pos + f.dir * (sz * 2.0f),
               sz * 0.9f);
    }

    for (Tracer &t : gTracer)
    {
        if (!t.live)
            continue;
        t.age += dt;
        const float run = (t.age - t.delay) * 0.001f * t.speed;
        if (run <= 0)
            continue;
        vec3d ab = t.b - t.a;
        const float L = ab.length();
        if (run - t.len >= L || L < 1.0)
        {
            t.live = false;
            continue;
        }
        ++nT;
        ab /= L;
        const float head = std::min(run, L), tail = std::max(0.0f, run - t.len);
        if (head - tail < 1.0)
            continue;
        Ribbon(vw, gStreak[t.pal][kFade - 1], t.a + ab * tail, t.a + ab * head, t.width);
    }

    for (Head &h : gHead)
    {
        if (!h.key)
            continue;
        if (h.seen < gFrame - 30)
        {
            h.key = NULL;
            continue;
        }
        if (h.seen < gFrame - 1 || h.style == ST_NONE)
            continue;
        ++nH;
        const uint32_t fl = Hash(h.seed + gFrame);
        switch (h.style)
        {
        case ST_SHELL:
            Ribbon(vw, gStreak[h.pal][kFade - 2], h.pos - h.dir * tu.shellLen, h.pos, 6.0);
            break;
        case ST_ENERGY:
            Ribbon(vw, gStreak[h.pal][kFade - 1], h.pos - h.dir * tu.wakeLen, h.pos, 7.0);
            break;
        default:
            Billboard(vw, gGlow[h.pal][kFade - 1 - (int)(fl & 1)], h.pos - h.dir * 6.0, 44.0f + 14.0f * ((fl >> 8 & 255) / 255.0f),
                      (fl >> 16 & 1023) / 1023.0f * 6.283f);
            Ribbon(vw, gCone[h.pal][fl >> 3 & 1][kFade - 2], h.pos - h.dir * 4.0, h.pos - h.dir * 60.0, 18.0);
            break;
        }
    }

    for (int i = 0; i < tu.capPuff; ++i)
    {
        Puff &p = gPuff[i];
        if (!p.live)
            continue;
        p.age += dt;
        if (p.age >= p.life)
        {
            p.live = false;
            continue;
        }
        ++nP;
        const float t = p.age / p.life;
        p.pos += p.vel * fs;
        p.vel *= std::max(0.0f, 1.0f - 1.6f * fs);   // drag: the puff stops, then only drifts
        p.vel.y -= tu.puffRise * 0.15f * fs;
        const float grow = 1.0f - (1.0f - t) * (1.0f - t);
        const float sz = p.s0 + (p.s1 - p.s0) * grow;
        const int fr = std::min(kSmokeFrames - 1, (int)(t * kSmokeFrames));
        Billboard(vw, gSmoke[p.tint][fr], p.pos, sz, (p.seed & 1023) / 1023.0f * 6.283f + ((p.seed & 1024) ? 0.3f : -0.3f) * t);
    }
    ++gFrame;

    if (getenv("UA_TRAILS_DEBUG") && rrg->timeStamp / 1000 != gDbgT)
    {
        gDbgT = rrg->timeStamp / 1000;
        printf("trails: t %d flashes %d tracers %d heads %d puffs %d\n", gDbgT, nF, nT, nH, nP);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// UA_TRAILS_STAGE=1: scripted firing range on level 97 (showroom), for the look sheets, clips and cost runs.
// Five lanes (Resistance, Ghorkov, Taerkasten, Mykonian, Sulgogar) of one launcher each (a Jaguar body, vid 2) on
// open ground east of the host, all firing north (-z) at fixed ground points. Each shot temporarily lends the
// launcher the faction's owner id, so the weapon (and its trail palette) belongs to that faction; it is restored
// right after the call, so the launchers never fight each other. Deterministic per game time (UA_FIXED_PERIOD).
// Times = game ms from the first stage frame + 3 s:
//    1.5 - 4.5 s  MG bursts, every lane (Sulgogar: mgun 112)
//    5 s          cannon / shell per lane      9 s  guided missile per lane     13 s  rockets / lasers per lane
//   17 - 24 s     everything at once (busy window for the cost rows)
//   26 - 33 s     close-up camera: a Resistance salvo (6 FIRESTORM + 2 SNOOPY, 150 ms apart) for the slow-motion clip
namespace
{
const int kOwner[5] = {1, 6, 4, 3, 2};
const int kMgun[5] = {49, 49, 49, 49, 112};
const int kShell[5] = {2, 26, 32, 66, 71};
const int kMissile[5] = {4, 24, 36, 69, 72};
const int kRocket[5] = {6, 31, 34, 64, 71};
const int kMix[5] = {7, 22, 33, 65, 72};
const int kStageEnd = 34000;
int gStage = -1;
int32_t gT0 = -1, gLastT = -1;
NC_STACK_ypabact *gLane[5] = {};
bool gCam = false;
vec3d gEye, gTgt;
int gShotMs = -1;
int gShotFrom = 0, gShotTo = 1 << 30;
int32_t gShotNext = 0;
std::string gShot;
uint64_t gPerfLast = 0;
std::vector<float> gPerfMs;
bool gPerfDone = false, gDone = false;

vec3d Base(NC_STACK_ypaworld *yw)
{
    const int sx = 12, sy = 10;
    vec3d p((sx + 0.5) * World::CVSectorLength, 0.0, -(sy + 0.5) * World::CVSectorLength);
    if (sx < yw->_mapSize.x && sy < yw->_mapSize.y)
        p.y = yw->_cells(sx, sy).height;
    return p;
}
vec3d LanePos(NC_STACK_ypaworld *yw, int i) { vec3d p = Base(yw); p.x += (i - 2) * 220.0; p.y -= 30.0; p.z += 300.0; return p; }
vec3d LaneTgt(NC_STACK_ypaworld *yw, int i, int k) { vec3d p = Base(yw); p.x += (i - 2) * 260.0 + (k % 3 - 1) * 60.0; p.z -= 900.0; return p; }

NC_STACK_ypabact *SpawnLane(NC_STACK_ypaworld *yw, const vec3d &pos)
{
    ypaworld_arg146 a;
    a.vehicle_id = 2;
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

void Fire(NC_STACK_ypaworld *yw, int lane, int wid, const vec3d &tgt, int32_t ts, bool lob)
{
    NC_STACK_ypabact *l = gLane[lane];
    if (!l)
        return;
    bact_arg79 a;
    vec3d d = tgt - l->_position;
    d.normalise();
    if (lob)
    {
        d.y -= 0.18;   // y is down: lob shells up a little
        d.normalise();
    }
    a.direction = d;
    a.start_point = vec3d(0.0, -12.0, 20.0);
    a.tgType = BACT_TGT_TYPE_CELL;
    Common::Point c = World::PositionToSectorID(tgt);
    a.target.pcell = (c.x >= 0 && c.y >= 0 && c.x < yw->_mapSize.x && c.y < yw->_mapSize.y) ? &yw->_cells(c.x, c.y) : NULL;
    a.tgt_pos = tgt;
    a.weapon = wid;
    a.g_time = ts;
    a.flags = 0;
    l->_weapon_time = 0;
    const uint8_t own = l->_owner;
    l->_owner = kOwner[lane];
    if (!a.target.pcell || !l->LaunchMissile(&a))
        printf("trails-stage: weapon %d not launched\n", wid);
    l->_owner = own;
}

void Mg(NC_STACK_ypaworld *yw, int lane, const vec3d &tgt, int32_t ts, float dtSec)
{
    NC_STACK_ypabact *l = gLane[lane];
    if (!l)
        return;
    bact_arg105 a;
    vec3d d = tgt - l->_position;
    d.normalise();
    a.field_0 = d;
    a.field_C = dtSec;
    a.field_10 = ts;
    const uint8_t own = l->_owner;
    const int mg = l->_mgun;
    const int en = l->_energy;
    l->_owner = kOwner[lane];
    l->_mgun = kMgun[lane];
    l->FireMinigun(&a);
    l->_owner = own;
    l->_mgun = mg;
    l->_energy = en;
}

void Frame(const vec3d &tgt, const vec3d &eye)
{
    gCam = true;
    gTgt = tgt;
    gEye = eye;
}
} // namespace

std::string PendingShot()
{
    std::string s;
    s.swap(gShot);
    return s;
}

bool StageCamera(vec3d *eye, vec3d *target)
{
    if (!gCam)
        return false;
    *eye = gEye;
    *target = gTgt;
    return true;
}

void StageFrame(NC_STACK_ypaworld *yw, int32_t ts)
{
    if (gStage < 0)
    {
        const char *v = getenv("UA_TRAILS_STAGE");
        gStage = (v && v[0] && v[0] != '0') ? atoi(v) : 0;
        if (gStage)
            printf("trails-stage: on, TRAILS level %d\n", Level());
        const char *s = getenv("UA_TRAILS_STAGE_SHOT");
        gShotMs = s ? atoi(s) : -1;
        if (const char *w = getenv("UA_TRAILS_STAGE_SHOT_WIN"))   // "<from ms>,<to ms>": shots only inside
            sscanf(w, "%d,%d", &gShotFrom, &gShotTo);
    }
    if (!gStage || gDone || !yw || !yw->_userRobo)
        return;
    if (gT0 < 0)
    {
        gT0 = ts + 3000;
        for (int i = 0; i < 5; ++i)
            gLane[i] = SpawnLane(yw, LanePos(yw, i));
    }
    const int32_t t = ts - gT0;
    const int32_t prev = gLastT;
    gLastT = t;
    const float dts = prev < 0 ? 0.02f : std::max(1, t - prev) * 0.001f;
    auto at = [&](int32_t ms) { return prev < ms && t >= ms; };

    for (int i = 0; i < 5; ++i)   // keep the launchers on their marks (they are props, not players)
        if (gLane[i] && t < kStageEnd)
        {
            gLane[i]->_position = LanePos(yw, i);
            vec3d f = LaneTgt(yw, i, 1) - gLane[i]->_position;   // face down range (muzzle at the front)
            f.y = 0.0;
            f.normalise();
            vec3d r = vec3d(0.0, 1.0, 0.0) * f;
            r.normalise();
            vec3d d = f * r;
            gLane[i]->_rotation = mat3x3(r.x, r.y, r.z, d.x, d.y, d.z, f.x, f.y, f.z);
            gLane[i]->_energy = gLane[i]->_energy_max;
        }

    {   // perf: wall-clock frame time in the busy window
        uint64_t now = SDL_GetPerformanceCounter();
        if (gPerfLast && t >= 17000 && t < 24000)
            gPerfMs.push_back((now - gPerfLast) * 1000.0 / SDL_GetPerformanceFrequency());
        gPerfLast = now;
        if (t >= 24000 && !gPerfDone && !gPerfMs.empty())
        {
            gPerfDone = true;
            std::vector<float> v = gPerfMs;
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (float x : v) sum += x;
            printf("trails-stage: perf busy window frames %d mean %.2f ms median %.2f p95 %.2f max %.2f\n", (int)v.size(),
                   sum / v.size(), v[v.size() / 2], v[(size_t)(v.size() * 0.95)], v.back());
        }
    }

    vec3d B = Base(yw);
    if (t < 26000)   // wide range camera: behind and above the line, looking down range
        Frame(B + vec3d(-200.0, -30.0, 50.0), B + vec3d(330.0, -260.0, 480.0));
    else             // close-up beside the Resistance launcher, looking along the salvo
        Frame(LanePos(yw, 0) + vec3d(-20.0, -30.0, -300.0), LanePos(yw, 0) + vec3d(170.0, -70.0, 60.0));

    for (int i = 0; i < 5; ++i)
    {
        if ((t >= 1500 && t < 4500) || (t >= 17000 && t < 24000))
            Mg(yw, i, LaneTgt(yw, i, 1), ts, dts);
        if (at(5000 + i * 300))
            Fire(yw, i, kShell[i], LaneTgt(yw, i, 0), ts, true);
        if (at(9000 + i * 300))
            Fire(yw, i, kMissile[i], LaneTgt(yw, i, 1), ts, false);
        for (int k = 0; k < 3; ++k)
            if (at(13000 + i * 300 + k * 250))
                Fire(yw, i, kRocket[i], LaneTgt(yw, i, k), ts, false);
        for (int k = 0; k < 7; ++k)   // busy window: a shot per lane per second, kinds rotating
        {
            const int ms = 17000 + k * 1000 + i * 170;
            if (at(ms))
            {
                const int kind = (k + i) % 4;
                const int wid = kind == 0 ? kShell[i] : kind == 1 ? kMissile[i] : kind == 2 ? kRocket[i] : kMix[i];
                Fire(yw, i, wid, LaneTgt(yw, i, k), ts, kind == 0);
            }
        }
    }
    for (int k = 0; k < 8; ++k)   // close-up salvo
        if (at(27000 + k * 150))
            Fire(yw, 0, k < 6 ? 6 : 4, LaneTgt(yw, 0, k), ts, false);

    if (gShotMs > 0 && t >= gShotNext && t >= gShotFrom && t <= gShotTo)
    {
        char nm[64];
        snprintf(nm, sizeof(nm), "trs_%06d", (int)t);
        gShot = nm;
        if (gShotNext < gShotFrom)
            gShotNext = gShotFrom;
        while (gShotNext <= t)
            gShotNext += gShotMs;
    }
    if (t >= kStageEnd)
    {
        gDone = true;
        printf("trails-stage: done\n");
    }
}

}
