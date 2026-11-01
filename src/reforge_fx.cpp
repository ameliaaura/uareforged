// Reforge effects - see reforge_fx.h for the contract.
//
// Env reference
//   UA_FX=0                 everything here off (original paths), even with a pack
//   UA_FX_DIR=<dir>         fx pack directory (default <hd-models dir>/fx)
//   UA_FX_STAGE=1           scripted effect stage in a running level (captures):
//                           genesis, unit deaths, a building, then a barrage.
//                           Times / targets: see StageFrame below. Inert unless set.
//   UA_FX_STAGE_LOG=1       print every stage event (default on with the stage)

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "includes.h"
#include "reforge_fx.h"
#include "gfxsettings.h"
#include "particle.h"
#include "area.h"
#include "bitmap.h"
#include "base.h"
#include "yw.h"
#include "ypabact.h"
#include "yparobo.h"
#include "system/gfx.h"
#include "system/tform.h"
#include "world/consts.h"

namespace ReforgeFx
{

namespace
{

bool IsFileP(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Upper(std::string s)
{
    // rsrc names read from IFF can carry NUL padding / spaces: key on the visible name only
    size_t z = s.find('\0');
    if (z != std::string::npos)
        s.resize(z);
    while (!s.empty() && isspace((unsigned char)s.back()))
        s.pop_back();
    for (char &c : s)
        c = toupper((unsigned char)c);
    size_t sl = s.find_last_of("/\\:");
    if (sl != std::string::npos)
        s = s.substr(sl + 1);
    return s;
}

struct KindDef
{
    Kind k;
    std::string png;
    int cols = 1, rows = 1;
    bool built = false;
    ResBitmap *tex = NULL;
};

std::string gDir;
std::vector<KindDef> gKinds;
std::map<std::string, int> gMap;
int gOn = -1;
bool gLoadTried = false;   // pack parsed once (kept while the level drops to 0)
bool gPackOk = false;
GFX::TMesh *gGround[kGroundLevels] = {};
ResBitmap *gGroundTex = NULL;
std::string gGroundPng;

void Load()
{
    // Graphics settings: FX 0 = off (UA_FX=0 maps onto it); nothing is read
    // or loaded until the level is above 0.
    if (gfxLevel("FX", 3) == 0)
    {
        gOn = 0;
        return;
    }
    gLoadTried = true;
    std::string dir;
    if (const char *d = getenv("UA_FX_DIR"))
        dir = d;
    else
    {
        const char *h = getenv("UA_HDMODELS_DIR");
        dir = std::string((h && h[0]) ? h : "hd-models") + "/fx";
    }
    if (char *abs = realpath(dir.c_str(), NULL))
    {
        dir = abs;
        free(abs);
    }
    FILE *f = fopen((dir + "/fx.txt").c_str(), "r");
    if (!f)
    {
        gOn = 0;
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        std::vector<std::string> tok;
        for (char *t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n"))
            tok.push_back(t);
        if (tok.empty())
            continue;
        if (tok[0] == "kind" && tok.size() >= 7)
        {
            KindDef d;
            d.k.name = tok[1];
            d.png = dir + "/" + tok[2];
            d.cols = std::max(1, atoi(tok[3].c_str()));
            d.rows = std::max(1, atoi(tok[4].c_str()));
            d.k.frames = std::max(1, std::min(atoi(tok[5].c_str()), d.cols * d.rows));
            d.k.additive = tok[6] != "alpha";
            for (size_t i = 7; i < tok.size(); ++i)
            {
                const std::string &o = tok[i];
                size_t eq = o.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string key = o.substr(0, eq);
                float v = atof(o.c_str() + eq + 1);
                if (key == "spin") d.k.spin = v;
                else if (key == "ground") d.k.ground = v;
                else if (key == "gain") d.k.gain = v;
                else if (key == "size") d.k.size = v;
                else if (key == "var") d.k.variants = std::max(1, (int)v);
                else if (key == "glow") d.k.glow = std::max(0.0f, std::min(1.0f, v));
                else if (key == "gfrac") d.k.gfrac = std::max(0.0f, std::min(1.0f, v));
            }
            if (IsFileP(d.png))
                gKinds.push_back(d);
            else
                printf("fx: kind '%s' map '%s' missing, skipped\n", d.k.name.c_str(), d.png.c_str());
        }
        else if (tok[0] == "ground" && tok.size() >= 2)
            gGroundPng = dir + "/" + tok[1];
        else if (tok[0] == "map" && tok.size() >= 3)
        {
            for (size_t i = 0; i < gKinds.size(); ++i)
                if (gKinds[i].k.name == tok[2])
                    gMap[Upper(tok[1])] = (int)i;
        }
    }
    fclose(f);
    gDir = dir;
    gOn = 1;
    gPackOk = true;
    printf("fx: pack '%s' ON: %d kinds, %d mapped bitmaps\n", dir.c_str(), (int)gKinds.size(), (int)gMap.size());
}

// Additive sprites are authored premultiplied on black (alpha 255). With
// gfx.blending = 0 the lumtracy pass blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA, so
// convert exactly like obj3d LumTracyPixels: luminance-as-alpha there, and
// premultiplied (alpha 255) for ONE / ONE.
void AdditivePixels(SDL_Surface *s)
{
    const bool lumAlpha = !GFX::GFXEngine::can_destblend && GFX::GFXEngine::can_srcblend;
    if (!lumAlpha)
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
        printf("fx: '%s' not loaded\n", png.c_str());
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

GFX::TMesh *MakeQuad(ResBitmap *tex, float u0, float v0, float u1, float v1, uint32_t flags, const GFX::TGLColor &clr)
{
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

void Build(KindDef &d)
{
    if (d.built)
        return;
    d.built = true;
    d.tex = LoadTex(d.png, d.k.additive);
    if (!d.tex)
        return;
    const uint32_t flags = GFX::RFLAGS_TEXTURED | GFX::RFLAGS_LUMTRACY | (d.k.additive ? 0 : GFX::RFLAGS_FX_ALPHA);
    const GFX::TGLColor clr = d.k.additive ? GFX::TGLColor(d.k.gain, d.k.gain, d.k.gain, 1.0)
                                           : GFX::TGLColor(1.0, 1.0, 1.0, d.k.gain);
    const float eps = 0.5 / 1024.0;
    for (int i = 0; i < d.k.frames; ++i)
    {
        const int cx = i % d.cols, cy = i / d.cols;
        d.k.meshes.push_back(MakeQuad(d.tex, (float)cx / d.cols + eps, (float)cy / d.rows + eps,
                                      (float)(cx + 1) / d.cols - eps, (float)(cy + 1) / d.rows - eps, flags, clr));
    }
}

// Per-particle-generator stage -> kind cache.
std::map<NC_STACK_particle *, std::vector<int> > gStageKinds;

const std::vector<int> &StageKinds(NC_STACK_particle *gen)
{
    std::vector<int> &v = gStageKinds[gen];
    if (v.size() != gen->_lifeStagesAdes.size())
    {
        v.assign(gen->_lifeStagesAdes.size(), -1);
        for (size_t i = 0; i < v.size(); ++i)
        {
            NC_STACK_area *a = dynamic_cast<NC_STACK_area *>(gen->_lifeStagesAdes[i]);
            if (!a)
                continue;
            NC_STACK_bitmap *b = a->_texImg ? a->_texImg : a->_tracyImg;
            if (b)
                v[i] = KindForBitmap(b->getRsrc_name());
            static int dbg = -1;
            if (dbg < 0)
                dbg = getenv("UA_FX_DEBUG") ? 1 : 0;
            if (dbg)
                printf("fx: gen %p stage %d bitmap '%s' len %d (tex %p tracy %p) -> kind %d\n", (void *)gen, (int)i,
                       b ? b->getRsrc_name().c_str() : "-", b ? (int)b->getRsrc_name().size() : 0, (void *)a->_texImg, (void *)a->_tracyImg, v[i]);
        }
    }
    return v;
}

inline uint32_t Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

} // namespace

bool On()
{
    // Graphics settings changed (shell options): re-decide at the next use.
    static uint32_t gen = 0;
    if (gen != GfxSettings::Generation())
    {
        gen = GfxSettings::Generation();
        if (gLoadTried)
            gOn = (gPackOk && gfxLevel("FX", 3) > 0) ? 1 : 0;
        else
            gOn = -1;
    }
    if (gOn < 0)
        Load();
    return gOn == 1;
}

const std::string &PackDir()
{
    On();
    return gDir;
}

int KindForBitmap(const std::string &name)
{
    if (!On())
        return -1;
    auto it = gMap.find(Upper(name));
    return it == gMap.end() ? -1 : it->second;
}

Kind *GetKind(int id)
{
    if (id < 0 || id >= (int)gKinds.size())
        return NULL;
    Build(gKinds[id]);
    return gKinds[id].tex ? &gKinds[id].k : NULL;
}

GFX::TMesh *GroundMesh(int level)
{
    if (!gGroundTex && !gGroundPng.empty())
    {
        gGroundTex = LoadTex(gGroundPng, true);
        gGroundPng.clear();
        // vertex colour carries the brightness (the renderer ignores node colour): one quad per level
        for (int i = 0; gGroundTex && i < kGroundLevels; ++i)
        {
            const float b = (float)(i + 1) / kGroundLevels;
            gGround[i] = MakeQuad(gGroundTex, 0.0, 0.0, 1.0, 1.0, GFX::RFLAGS_TEXTURED | GFX::RFLAGS_LUMTRACY,
                                  GFX::TGLColor(b, b, b, 1.0));
        }
    }
    return gGround[std::max(0, std::min(level, kGroundLevels - 1))];
}

bool RenderParticle(NC_STACK_particle *gen, size_t stage, int32_t age, uint32_t seed,
                    const vec3d &worldPos, float scale, area_arg_65 *rndr)
{
    const std::vector<int> &sk = StageKinds(gen);
    if (stage >= sk.size() || sk[stage] < 0)
        return false;
    Kind *k = GetKind(sk[stage]);
    if (!k || k->meshes.empty())
        return false;

    TF::TForm3D *view = rndr->ViewTForm;
    vec3d pos = view->CalcSclRot.Transform(worldPos - view->CalcPos);

    const uint32_t h = Hash(seed);
    const float t = gen->_lifeTime > 0 ? (float)age / (float)gen->_lifeTime : 0.0;
    const int nv = std::max(1, std::min(k->variants, (int)k->meshes.size()));
    const int per = (int)k->meshes.size() / nv;
    int fr = (int)(t * per);
    fr = std::max(0, std::min(fr, per - 1)) + (int)((h >> 17) % nv) * per;
    GFX::TMesh *mesh = k->meshes[fr];

    const float a0 = (h & 0xffff) / 65535.0 * 6.2831853;
    const float dir = (h & 0x10000) ? 1.0 : -1.0;
    const float ang = a0 + dir * k->spin * age * 0.001;
    // Phase 2 (less distracting): a sprite never grows past its distance to the camera, so a
    // close camera is no longer swallowed by one fire/flash quad (a full-screen orange wash at the stage's close shots).
    const float s = std::min((float)(scale * k->size), (float)pos.length());
    const float c = cos(ang) * s, sn = sin(ang) * s;

    GFX::TRenderNode &rend = GFX::Engine.AllocRenderNode();
    rend = GFX::TRenderNode(GFX::TRenderNode::TYPE_PARTICLE);
    rend.Distance = pos.length();
    rend.Flags = mesh->Mat.Flags | rndr->flags;
    rend.Color = mesh->Mat.Color;
    rend.Tex = mesh->Mat.Tex;
    rend.Mesh = mesh;
    rend.TForm = mat4x4(c, -sn, 0.0, pos.x,
                        sn, c, 0.0, pos.y,
                        0.0, 0.0, 1.0, pos.z,
                        0.0, 0.0, 0.0, 1.0);
    rend.TimeStamp = rndr->timeStamp;
    rend.FrameTime = rndr->frameTime;
    rend.FogStart = rndr->fadeStart;
    rend.FogLength = rndr->fadeLength;
    rend.ParticleSize = s;
    GFX::GFXEngine::Instance.QueueRenderMesh(&rend);

    // Ground light pool: one per particle of a "ground" kind, brightest at
    // birth (the sprite frame carries the fade), on the sector floor.
    // Phase 2 (weaker, calmer): only gfrac of the particles light the ground, brightness
    // glow * (1 - t)^1.5 (quantised to the level meshes), radius shrinking more gently.
    const bool lit = k->gfrac >= 1.0 || ((h >> 8) & 0xff) < (uint32_t)(k->gfrac * 256.0);
    if (k->ground > 0.0 && ypaworld && lit)
    {
        const float gb = k->glow * pow(std::max(0.0f, 1.0f - t), 1.5f);
        GFX::TMesh *g = GroundMesh((int)(gb * kGroundLevels + 0.5) - 1);
        if (g && t < 0.85 && gb > 0.5 / kGroundLevels)
        {
            Common::Point cid = World::PositionToSectorID(worldPos);
            NC_STACK_ypaworld *yw = ypaworld;
            if (cid.x >= 0 && cid.y >= 0 && cid.x < yw->_mapSize.x && cid.y < yw->_mapSize.y)
            {
                const float gy = yw->_cells(cid.x, cid.y).height;
                const float above = gy - worldPos.y; // y is down: >0 = particle above ground
                if (above > -50.0 && above < 900.0)
                {
                    const float R = scale * k->ground * (1.0 - 0.6 * t);
                    vec3d gp = view->CalcSclRot.Transform(vec3d(worldPos.x, gy - 3.0, worldPos.z) - view->CalcPos);
                    vec3d ex = view->CalcSclRot.Transform(vec3d(R, 0.0, 0.0));
                    vec3d ez = view->CalcSclRot.Transform(vec3d(0.0, 0.0, R));
                    vec3d ey = view->CalcSclRot.Transform(vec3d(0.0, 1.0, 0.0));
                    GFX::TRenderNode &gr = GFX::Engine.AllocRenderNode();
                    gr = GFX::TRenderNode(GFX::TRenderNode::TYPE_MESH);
                    gr.Distance = gp.length();
                    gr.Flags = g->Mat.Flags | rndr->flags;
                    gr.Color = g->Mat.Color;
                    gr.Tex = g->Mat.Tex;
                    gr.Mesh = g;
                    gr.TForm = mat4x4(ex.x, ez.x, ey.x, gp.x,
                                      ex.y, ez.y, ey.y, gp.y,
                                      ex.z, ez.z, ey.z, gp.z,
                                      0.0, 0.0, 0.0, 1.0);
                    gr.TimeStamp = rndr->timeStamp;
                    gr.FrameTime = rndr->frameTime;
                    gr.FogStart = rndr->fadeStart;
                    gr.FogLength = rndr->fadeLength;
                    GFX::GFXEngine::Instance.QueueRenderMesh(&gr);
                }
            }
        }
    }
    return true;
}


// ---------------------------------------------------------------------------
// UA_FX_STAGE=1: scripted effect stage for side-by-side captures (tooling).
// Deterministic: the stage spawns its OWN units at fixed open-ground points
// (east of the showroom host, level 97) and kills them on a fixed game-time
// schedule, so an UA_FX=0 run and a pack run show the same events at the
// same places and times. Times = game ms from the first stage frame + 3 s.
//    2 s  genesis Wiesel (vid 1, VPgSIMPL)          at P
//    7 s  genesis Tekh (vid 26, VPgKPAN1)           at P + (450, 0)
//   12 s  genesis X00A (vid 69, VPgMYKO1)           at P + (900, 0)
//   17 s  kill the Wiesel  (ground death: megadeth + plasma shell)
//   22 s  kill the Tekh
//   27 s  Wasp (vid 6) spawned 280 up + killed (air death, falls, crash)
//   33 s  building: sector damage on the power station at sector 11,12
//   39 s  host-death style cluster: fx vehicles 87 B_BIGEXPLO + 47 B_FATTESTEXPLO + 42/43/44
//   44 s  barrage: a spawned unit killed every 0.5 s on a 4 x 4 grid (busy window 44-60 s)
//  phase 2 (alien / energy, weapon impacts, a real host death):
//   63 s  X00A (vid 69) spawned 280 up + killed (Mykonian air death: VPgMYKO1 dead shell, VPdBRGR2 lightning megadeth)
//   68 s  AMM-O (vid 72, Sulgogar, VPxSULG1 + green dest_fx) spawned 280 up + killed
//   73 s  WHOA-N (vid 73, Sulgogar, VPxSULG2 / VPdSULG2 plasma) spawned 280 up + killed
//   78 s  weapon impacts: a Wasp launcher 420 up fires, 0.8 s apart, at a row of ground points: SNOOPY missile (4),
//         FIRESTORM rocket (6), HAMMER bomb (5), GRANATE2 (3), SHOCKER Mykonian laser (69), RAUSSCHMISS
//         Sulgogar plasma (71), BERGSACHSE Mykonian shot (66)
//   86 s  a real host station (vid 56, owner 6) placed at 84 s and destroyed (yparobo death, 16 dest_fx, plasma shell)
// The render camera frames each event from 1.5 s before it (PerfCamPose hook).
namespace
{
struct StageEv
{
    int ms;
    int type;  // 0 genesis 1 kill slot 2 building 3 barrage 4 spawn+kill 5 fx vehicles
    int vid;
    double dx, dy, dz; // offset from P
    int slot;
};
const StageEv kStage[] = {
    {2000, 0, 1, 0, 0, 0, 0},    {7000, 0, 26, 450, 0, 0, 1}, {12000, 0, 69, 900, 0, 0, 2},
    {17000, 1, 0, 0, 0, 0, 0},   {22000, 1, 0, 450, 0, 0, 1}, {27000, 4, 6, 0, -280, 450, 3},
    {33000, 2, 0, 0, 0, 0, 0},   {39000, 5, 87, 450, 0, 500, 0}, {44000, 3, 1, 0, 0, 0, 0},
    {63000, 4, 69, 0, -280, 450, 0}, {68000, 4, 72, 450, -280, 450, 0}, {73000, 4, 73, 900, -280, 450, 0},
    {78000, 6, 0, 450, 0, -300, 0}, {84000, 7, 56, 450, 0, 1100, 0}, {86000, 8, 0, 450, 0, 1100, 0},
};
const double kCamD[] = {95, 170, 260, 115, 180, 300, 850, 620, 1000,
                        420, 650, 700, 560, 1300, 1300}; // camera distance per event
const int kStageEnd = 96000;
// weapon volley (event type 6): weapon id per shot, 0.8 s apart
const int kVolley[] = {4, 6, 5, 3, 69, 71, 66};
int gStage = -1;
int32_t gStageT0 = -1;
size_t gNext = 0;
bool gCamOn = false;
vec3d gCamTarget, gCamEye;
int gBarrageN = 0;
int32_t gBarrageLast = 0;
int gShotMs = -1;
int32_t gShotNext = 0;
std::string gShotName;
uint64_t gPerfLast = 0;
std::vector<float> gPerfMs;
bool gPerfDone = false;
NC_STACK_ypabact *gSlot[4] = {NULL, NULL, NULL, NULL};
NC_STACK_ypabact *gLauncher = NULL;
NC_STACK_ypabact *gHost = NULL;
int gVolleyN = -1;
int32_t gVolleyLast = 0;
vec3d gVolleyAt;

void Units(NC_STACK_ypaworld *yw, std::vector<NC_STACK_ypabact *> &out)
{
    for (NC_STACK_ypabact *r : yw->_unitsList)
    {
        if (!r)
            continue;
        out.push_back(r);
        for (NC_STACK_ypabact *c : r->_kidList)
        {
            if (!c)
                continue;
            out.push_back(c);
            for (NC_STACK_ypabact *u : c->_kidList)
                if (u)
                    out.push_back(u);
        }
    }
}

bool Live(NC_STACK_ypaworld *yw, NC_STACK_ypabact *p)
{
    if (!p)
        return false;
    std::vector<NC_STACK_ypabact *> all;
    Units(yw, all);
    for (NC_STACK_ypabact *b : all)
        if (b == p)
            return true;
    return false;
}

vec3d SectorCentre(NC_STACK_ypaworld *yw, int sx, int sy)
{
    vec3d p((sx + 0.5) * World::CVSectorLength, 0.0, -(sy + 0.5) * World::CVSectorLength);
    if (sx >= 0 && sy >= 0 && sx < yw->_mapSize.x && sy < yw->_mapSize.y)
        p.y = yw->_cells(sx, sy).height;
    return p;
}

vec3d P(NC_STACK_ypaworld *yw, const StageEv &e)
{
    vec3d p = SectorCentre(yw, 12, 10);
    p.x += e.dx - 450.0;
    p.z += e.dz;
    p.y += e.dy - 30.0;
    return p;
}

void Frame(const vec3d &tgt, double d)
{
    vec3d dir(0.42, -0.72, -0.55); // ~46 deg down, RTS-like: no sky in frame
    dir.normalise();
    gCamOn = true;
    gCamTarget = tgt;
    gCamEye = tgt + dir * d;
}

// A unit of the user host, as the host's own build makes it (yparobo.cpp:2471-2522).
NC_STACK_ypabact *Spawn(NC_STACK_ypaworld *yw, int vid, const vec3d &pos, bool genesis)
{
    ypaworld_arg146 a;
    a.vehicle_id = vid;
    a.pos = pos;
    NC_STACK_ypabact *u = yw->ypaworld_func146(&a);
    if (!u)
        return NULL;
    static uint32_t cmd = 0x7f0000;
    u->_commandID = cmd++;
    u->_owner = yw->_userRobo->_owner;
    u->_host_station = dynamic_cast<NC_STACK_yparobo *>(yw->_userRobo);
    yw->_userRobo->AddSubject(u);
    setState_msg s;
    s.newStatus = genesis ? BACT_STATUS_CREATE : BACT_STATUS_NORMAL;
    s.setFlags = 0;
    s.unsetFlags = 0;
    u->SetState(&s);
    if (genesis)
        u->_scale_time = u->_energy_max * 0.2;
    return u;
}

void Kill(NC_STACK_ypabact *b)
{
    bact_arg84 d;
    d.energy = -2000000;
    d.unit = NULL;
    b->ModifyEnergy(&d);
}

// One shot of weapon <wid> from the launcher at a ground point (the units' own LaunchMissile path).
void Shoot(NC_STACK_ypaworld *yw, NC_STACK_ypabact *l, int wid, const vec3d &tgt, int32_t ts)
{
    bact_arg79 a;
    vec3d d = tgt - l->_position;
    d.normalise();
    a.direction = d;
    a.start_point = vec3d(0.0, 0.0, 0.0);
    a.tgType = BACT_TGT_TYPE_CELL;
    Common::Point c = World::PositionToSectorID(tgt);
    a.target.pcell = (c.x >= 0 && c.y >= 0 && c.x < yw->_mapSize.x && c.y < yw->_mapSize.y) ? &yw->_cells(c.x, c.y) : NULL;
    a.tgt_pos = tgt;
    a.weapon = wid;
    a.g_time = ts;
    a.flags = 0;
    l->_weapon_time = 0; // no reload wait between the stage's shots
    if (!a.target.pcell || !l->LaunchMissile(&a))
        printf("fx-stage: weapon %d not launched\n", wid);
}

// A real host station of another owner, placed like the level loader does (yw_game.cpp:490-550).
NC_STACK_ypabact *SpawnHost(NC_STACK_ypaworld *yw, int vid, const vec3d &pos)
{
    ypaworld_arg146 a;
    a.vehicle_id = vid;
    a.pos = pos;
    NC_STACK_yparobo *r = dynamic_cast<NC_STACK_yparobo *>(yw->ypaworld_func146(&a));
    if (!r)
        return NULL;
    yw->ypaworld_func134(r);
    r->_owner = 6;
    r->_energy = r->_energy_max = 200000;
    r->setBACT_bactCollisions(1);
    return r;
}

// Explosion "vehicle" as a destroyed building spawns it (yw.cpp:822-854).
void FxVehicle(NC_STACK_ypaworld *yw, int vid, const vec3d &pos, const vec3d &push)
{
    ypaworld_arg146 a;
    a.vehicle_id = vid;
    a.pos = pos;
    NC_STACK_ypabact *boom = yw->ypaworld_func146(&a);
    if (!boom)
        return;
    boom->_owner = 0;
    setState_msg s;
    s.newStatus = BACT_STATUS_DEAD;
    s.setFlags = BACT_STFLAG_DEATH1;
    s.unsetFlags = 0;
    boom->SetState(&s);
    yw->ypaworld_func134(boom);
    bact_arg83 i;
    i.pos = pos;
    i.energ = 40000;
    i.pos2 = push;
    double l = i.pos2.length();
    if (l > 0.1)
        i.pos2 /= l;
    i.force = 30.0;
    i.mass = 50.0;
    boom->ApplyImpulse(&i);
}
} // namespace

std::string PendingShot()
{
    std::string s;
    s.swap(gShotName);
    return s;
}

bool StageCamera(vec3d *eye, vec3d *target)
{
    if (!gCamOn)
        return false;
    *eye = gCamEye;
    *target = gCamTarget;
    return true;
}

void StageFrame(NC_STACK_ypaworld *yw, int32_t ts)
{
    if (gStage < 0)
    {
        const char *v = getenv("UA_FX_STAGE");
        gStage = (v && v[0] && v[0] != '0') ? atoi(v) : 0;
        if (gStage)
            printf("fx-stage: on (variant %d)\n", gStage);
    }
    if (!gStage || !yw || !yw->_userRobo)
        return;
    if (gStageT0 < 0)
        gStageT0 = ts + 3000;
    const int32_t t = ts - gStageT0;

    {   // perf: wall-clock frame time while the barrage runs (same camera / events in every config)
        uint64_t now = SDL_GetPerformanceCounter();
        if (gPerfLast && t >= 44000 && t < 60000)
            gPerfMs.push_back((now - gPerfLast) * 1000.0 / SDL_GetPerformanceFrequency());
        gPerfLast = now;
        if (t >= 60000 && !gPerfDone && !gPerfMs.empty())
        {
            gPerfDone = true;
            std::vector<float> v = gPerfMs;
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (float x : v) sum += x;
            printf("fx-stage: perf busy window frames %d mean %.2f ms median %.2f p95 %.2f max %.2f\n", (int)v.size(),
                   sum / v.size(), v[v.size() / 2], v[(size_t)(v.size() * 0.95)], v.back());
        }
    }

    if (gShotMs < 0)
    {
        const char *v = getenv("UA_FX_STAGE_SHOT");
        gShotMs = v ? std::max(0, atoi(v)) : 0;
    }
    gShotName.clear();
    if (gShotMs > 0 && t >= gShotNext && t < kStageEnd)
    {
        while (gShotNext + gShotMs <= t)
            gShotNext += gShotMs;          // skipped slots are filled by the composer
        char nm[32];
        snprintf(nm, sizeof(nm), "fxs_%06d", (int)gShotNext);
        gShotName = nm;
        gShotNext += gShotMs;
    }

    static bool doneSaid = false;
    if (!doneSaid && t >= kStageEnd)
    {
        doneSaid = true;
        printf("fx-stage: done t=%d\n", t); // run_stage.sh ends the run here
        fflush(stdout);
    }

    const size_t nEv = sizeof(kStage) / sizeof(kStage[0]);
    if (gNext < nEv)
    {
        const StageEv &e = kStage[gNext];
        if (t >= e.ms - 1500)
        {
            vec3d tgt = P(yw, e);
            if (e.type == 1 && gSlot[e.slot])
                tgt = gSlot[e.slot]->_position;
            else if (e.type == 2)
                tgt = SectorCentre(yw, 11, 12);
            else if (e.type == 4)
                tgt.y += 200.0; // frame below the spawn: the wreck falls into view
            else if (e.type == 6)
                tgt.x += 3.0 * 110.0; // middle of the impact row
            else if (e.type == 7 || e.type == 8)
                tgt.y -= 250.0;
            if (e.type == 8 && gHost)
                tgt = gHost->_position + vec3d(0.0, 150.0, 0.0);
            Frame(tgt, kCamD[gNext]);
        }
        if (t >= e.ms)
        {
            printf("fx-stage: t=%d ev %d type %d vid %d cam target %.0f %.0f %.0f\n", t, (int)gNext, e.type, e.vid,
                   gCamTarget.x, gCamTarget.y, gCamTarget.z);
            if (e.type == 0)
            {
                gSlot[e.slot] = Spawn(yw, e.vid, P(yw, e), true);
                if (gSlot[e.slot])
                    printf("fx-stage: genesis vid %d scale_time %d ms\n", e.vid, gSlot[e.slot]->_scale_time);
            }
            else if (e.type == 1)
            {
                if (Live(yw, gSlot[e.slot]))
                    Kill(gSlot[e.slot]);
                else
                    printf("fx-stage: slot %d gone\n", e.slot);
            }
            else if (e.type == 4)
            {
                if (NC_STACK_ypabact *u = Spawn(yw, e.vid, P(yw, e), false))
                    Kill(u);
            }
            else if (e.type == 2)
            {
                vec3d c = SectorCentre(yw, 11, 12);
                for (int i = 0; i < 6; ++i)
                {
                    yw_arg129 a;
                    a.field_0 = 0;
                    a.pos = c;
                    a.field_10 = 400000;
                    a.OwnerID = World::OWNER_RECALC;
                    a.unit = NULL;
                    yw->ypaworld_func129(&a);
                }
            }
            else if (e.type == 5)
            {
                vec3d c = P(yw, e);
                FxVehicle(yw, 87, c, vec3d(0, -150, 0));
                FxVehicle(yw, 47, c + vec3d(-40, 0, 30), vec3d(-20, -150, 30));
                FxVehicle(yw, 42, c + vec3d(60, 0, -20), vec3d(50, -150, -20));
                FxVehicle(yw, 43, c + vec3d(-60, 0, -40), vec3d(-50, -150, -40));
                FxVehicle(yw, 44, c + vec3d(20, 0, 70), vec3d(10, -150, 60));
                FxVehicle(yw, 41, c + vec3d(0, -20, 0), vec3d(30, -150, 10));
                FxVehicle(yw, 40, c + vec3d(0, -20, 0), vec3d(-30, -150, -10));
            }
            else if (e.type == 3)
            {
                gBarrageN = 30;
                gBarrageLast = t - 500;
            }
            else if (e.type == 6)
            {
                gVolleyAt = P(yw, e);
                gLauncher = Spawn(yw, 6, gVolleyAt + vec3d(-260.0, -420.0, -380.0), false);
                gVolleyN = 0;
                gVolleyLast = t - 800;
            }
            else if (e.type == 7)
            {
                vec3d hp = P(yw, e);
                hp.y += 30.0;
                gHost = SpawnHost(yw, e.vid, hp);
                printf("fx-stage: host vid %d %s\n", e.vid, gHost ? "placed" : "NOT placed");
            }
            else if (e.type == 8)
            {
                if (gHost && Live(yw, gHost))
                    Kill(gHost);
                else
                    printf("fx-stage: host gone\n");
            }
            gNext++;
        }
    }
    if (gVolleyN >= 0 && gVolleyN < (int)(sizeof(kVolley) / sizeof(kVolley[0])) && t - gVolleyLast >= 800)
    {
        gVolleyLast = t;
        if (gLauncher && Live(yw, gLauncher))
        {
            vec3d tgt = gVolleyAt + vec3d(gVolleyN * 110.0, 0.0, (gVolleyN % 2) * 60.0);
            Common::Point c = World::PositionToSectorID(tgt);
            if (c.x >= 0 && c.y >= 0 && c.x < yw->_mapSize.x && c.y < yw->_mapSize.y)
                tgt.y = yw->_cells(c.x, c.y).height;
            printf("fx-stage: t=%d volley shot %d weapon %d\n", t, gVolleyN, kVolley[gVolleyN]);
            Shoot(yw, gLauncher, kVolley[gVolleyN], tgt, ts);
        }
        gVolleyN++;
    }
    if (gBarrageN > 0 && t - gBarrageLast >= 500)
    {
        gBarrageLast = t;
        const int k = 30 - gBarrageN;
        gBarrageN--;
        static const int vids[] = {1, 2, 16, 22, 26, 32, 37, 68};
        StageEv e = kStage[nEv - 1];
        e.dx = 150.0 + (k % 4) * 220.0;
        e.dz = -300.0 + ((k / 4) % 4) * 220.0;
        if (NC_STACK_ypabact *u = Spawn(yw, vids[k % 8], P(yw, e), false))
            Kill(u);
    }
}

} // namespace ReforgeFx
