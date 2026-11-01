// Reforge SKY: remastered sky panoramas (see reforge_sky.h).
#include "includes.h"
#include "base.h"
#include "bitmap.h"
#include "reforge_sky.h"
#include "system/gfx.h"

#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ReforgeSky
{
namespace
{
ResBitmap *gTex = NULL;       // the live panorama (owned here)
float gDriftDegPerMin = 0.0f; // 0 = static
float gDriftApplied = 0.0f;   // u offset currently in the VBOs (0..1)
uint32_t gDriftLastMs = 0;
uint32_t gDriftT0 = 0;
bool gDriftStarted = false;

bool FileExists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string Lower(const std::string &s)
{
    std::string o = s;
    for (char &c : o)
        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
    return o;
}

// "objects/X4.base" -> "x4"
std::string SkyKey(const std::string &skyName)
{
    std::string n = skyName;
    size_t sl = n.find_last_of("/\\:");
    if (sl != std::string::npos)
        n = n.substr(sl + 1);
    size_t dot = n.find('.');
    if (dot != std::string::npos)
        n = n.substr(0, dot);
    return Lower(n);
}

std::string SkyDir()
{
    const char *v = getenv("UA_SKY_DIR");
    if (v && v[0])
        return std::string(v);
    return "hd-sky";
}

// Largest authored width not above the level's, PNG or DDS (DDS = BC1/BC7,
// preferred when present and the GL can sample it).
std::string FindPanorama(const std::string &base, int level, int *widthOut, const char *stem = "sky_")
{
    static const int widths[3] = {1024, 2048, 4096};
    int want = std::max(1, std::min(3, level));
    for (int i = want - 1; i >= 0; --i)
    {
        for (const char *ext : {".dds", ".png"})
        {
            std::string p = base + "/" + stem + std::to_string(widths[i]) + ext;
            if (FileExists(p))
            {
                *widthOut = widths[i];
                return p;
            }
        }
    }
    return std::string();
}

void ReadParams(const std::string &dir)
{
    gDriftDegPerMin = 0.0f;
    FILE *f = fopen((dir + "/sky.txt").c_str(), "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        float v;
        if (sscanf(line, " drift = %f", &v) == 1)
            gDriftDegPerMin = v;
    }
    fclose(f);
}

struct Dir
{
    float u, v;
};

// View direction of an object-space dome point -> panorama (u, v). The dome is
// drawn at viewer + (0, skyHeight, 0), so the viewer sits at (0, -skyHeight, 0)
// in object space. UA world y points DOWN.
Dir PanoUV(const vec3f &p, int skyHeight)
{
    const double dx = p.x, dy = p.y + skyHeight, dz = p.z;
    const double el = atan2(-dy, sqrt(dx * dx + dz * dz)) * 180.0 / M_PI;
    const double az = atan2(dx, dz) * 180.0 / M_PI; // -180..180
    Dir d;
    d.u = (float)((az + 180.0) / 360.0);
    d.v = (float)((90.0 - el) / (90.0 - kElevMin));
    if (d.v < 0.0f)
        d.v = 0.0f;
    if (d.v > 1.0f)
        d.v = 1.0f;
    return d;
}

GFX::TGLColor Mix3(const GFX::TGLColor &a, const GFX::TGLColor &b, const GFX::TGLColor &c,
                   float wa, float wb, float wc)
{
    return GFX::TGLColor(a.r * wa + b.r * wb + c.r * wc, a.g * wa + b.g * wb + c.g * wc,
                         a.b * wa + b.b * wb + c.b * wc, a.a * wa + b.a * wb + c.a * wc);
}

// Subdivide every triangle n x n (points stay on the original planes) and give
// each corner its panorama UV. Fresh vertices per sub-triangle so the azimuth
// seam can be unwrapped per triangle (u + 1 on the small side; GL_REPEAT).
bool Rebuild(GFX::TMesh &m, int n, int skyHeight)
{
    if (m.Indixes.size() < 3 || (m.Indixes.size() % 3) != 0)
        return false;
    const size_t triCount = m.Indixes.size() / 3;
    const size_t outVerts = triCount * (size_t)n * n * 3;
    if (outVerts > 65000)
        return false;

    std::vector<GFX::TVertex> nv;
    std::vector<GFX::IndexType> ni;
    nv.reserve(outVerts);
    ni.reserve(outVerts);

    for (size_t t = 0; t < triCount; ++t)
    {
        const GFX::TVertex &A = m.Vertexes[m.Indixes[t * 3 + 0]];
        const GFX::TVertex &B = m.Vertexes[m.Indixes[t * 3 + 1]];
        const GFX::TVertex &C = m.Vertexes[m.Indixes[t * 3 + 2]];

        auto corner = [&](int i, int j) -> GFX::TVertex
        {
            // barycentric (i, j) on an n-grid: P = A + (B-A) i/n + (C-A) j/n
            const float wb = (float)i / n, wc = (float)j / n, wa = 1.0f - wb - wc;
            GFX::TVertex v;
            v.Pos = A.Pos * wa + B.Pos * wb + C.Pos * wc;
            v.Color = Mix3(A.Color, B.Color, C.Color, wa, wb, wc);
            v.ComputedColor = Mix3(A.ComputedColor, B.ComputedColor, C.ComputedColor, wa, wb, wc);
            return v;
        };

        auto emit = [&](GFX::TVertex a, GFX::TVertex b, GFX::TVertex c)
        {
            Dir da = PanoUV(a.Pos, skyHeight), db = PanoUV(b.Pos, skyHeight), dc = PanoUV(c.Pos, skyHeight);
            const float lo = std::min(da.u, std::min(db.u, dc.u));
            const float hi = std::max(da.u, std::max(db.u, dc.u));
            if (hi - lo > 0.5f)
            {
                if (da.u < 0.5f) da.u += 1.0f;
                if (db.u < 0.5f) db.u += 1.0f;
                if (dc.u < 0.5f) dc.u += 1.0f;
            }
            // A corner at the zenith has no azimuth: give it the mean of the other two
            // so the fan of triangles round the pole keeps each wedge's own u range.
            const float pole = 0.5f / (90.0f - kElevMin); // within 0.5 deg of the zenith
            if (da.v < pole) da.u = 0.5f * (db.u + dc.u);
            else if (db.v < pole) db.u = 0.5f * (da.u + dc.u);
            else if (dc.v < pole) dc.u = 0.5f * (da.u + db.u);
            a.TexCoord.tu = da.u; a.TexCoord.tv = da.v;
            b.TexCoord.tu = db.u; b.TexCoord.tv = db.v;
            c.TexCoord.tu = dc.u; c.TexCoord.tv = dc.v;
            ni.push_back((GFX::IndexType)nv.size()); nv.push_back(a);
            ni.push_back((GFX::IndexType)nv.size()); nv.push_back(b);
            ni.push_back((GFX::IndexType)nv.size()); nv.push_back(c);
        };

        for (int j = 0; j < n; ++j)
            for (int i = 0; i + j < n; ++i)
            {
                emit(corner(i, j), corner(i + 1, j), corner(i, j + 1));
                if (i + j + 1 < n)
                    emit(corner(i + 1, j), corner(i + 1, j + 1), corner(i, j + 1));
            }
    }

    m.Vertexes.swap(nv);
    m.Indixes.swap(ni);
    m.RecalcBoundBox();
    return true;
}

void ShiftU(NC_STACK_base *sky, float du)
{
    for (GFX::TMesh &m : sky->Meshes)
        for (GFX::TVertex &v : m.Vertexes)
            v.TexCoord.tu += du;
}
}

void Release()
{
    if (!gTex)
        return;
    GFX::Engine.FreeTexture(gTex);
    delete gTex;
    gTex = NULL;
    gDriftDegPerMin = 0.0f;
    gDriftApplied = 0.0f;
    gDriftStarted = false;
}

bool Apply(NC_STACK_base *sky, const std::string &skyName, int skyHeight, int level, int clouds3d)
{
    if (level <= 0 || !sky) // level 0: the original sky, untouched
        return false;

    Release();

    const std::string key = SkyKey(skyName);
    const std::string root = SkyDir();
    const std::string envDir = root + "/" + Lower(HDTex::GetEnvKey()) + "/" + key, skyDir = root + "/" + key;
    std::string dir;
    int width = 0;
    std::string file;
    // 3D clouds first (when chosen), then the New panorama; per-set folder before the shared one.
    for (const char *stem : {"sky3d_", "sky_"})
    {
        if (strcmp(stem, "sky3d_") == 0 && clouds3d <= 0)
            continue;
        for (const std::string *d : {&envDir, &skyDir})
        {
            file = FindPanorama(*d, level, &width, stem);
            if (!file.empty())
            {
                dir = *d;
                break;
            }
        }
        if (!file.empty())
            break;
    }
    if (file.empty())
    {
        printf("sky: level %d, no panorama for '%s' in %s - original sky kept\n", level, key.c_str(), root.c_str());
        return false;
    }

    uint32_t t0 = SDL_GetTicks();
    ResBitmap *tex = NULL;
    if (file.size() > 4 && file.compare(file.size() - 4, 4, ".dds") == 0)
        tex = GFX::Engine.LoadDdsTexture(file, false);
    else if (SDL_Surface *s = IMG_Load(file.c_str()))
    {
        tex = GFX::Engine.LoadRawTexture(s, NULL, false);
        SDL_FreeSurface(s);
    }
    if (!tex)
    {
        printf("sky: '%s' failed to load - original sky kept\n", file.c_str());
        return false;
    }

    const int n = level >= 2 ? 8 : 6;
    size_t tris = 0;
    for (GFX::TMesh &m : sky->Meshes)
    {
        if (!Rebuild(m, n, skyHeight))
        {
            printf("sky: mesh not rebuilt (%d indices) - original sky kept\n", (int)m.Indixes.size());
            GFX::Engine.FreeTexture(tex);
            delete tex;
            return false;
        }
        tris += m.Indixes.size() / 3;
    }
    for (GFX::TMesh &m : sky->Meshes)
    {
        m.Mat.TexSource = NULL;
        m.Mat.Tex = tex;
        m.CoordsCache.clear();
    }
    gTex = tex;
    ReadParams(dir);
    if (level < 2)
        gDriftDegPerMin = 0.0f;
    printf("sky: level %d%s '%s' <- %s (%dx%d, %u KB GPU, %d tris, drift %.2f deg/min, %u ms)\n", level,
           file.find("/sky3d_") != std::string::npos ? " 3d" : (clouds3d > 0 ? " 3d->new" : ""), key.c_str(),
           file.c_str(), tex->width, tex->height, tex->hwBytes / 1024, (int)tris, gDriftDegPerMin,
           SDL_GetTicks() - t0);
    return true;
}

void Tick(NC_STACK_base *sky, uint32_t timeMs)
{
    if (!gTex || gDriftDegPerMin == 0.0f || !sky)
        return;
    if (!gDriftStarted)
    {
        gDriftStarted = true;
        gDriftT0 = timeMs;
        gDriftLastMs = timeMs;
        return;
    }
    if (timeMs - gDriftLastMs < 250) // ~4 VBO refreshes a second; the shift is sub-texel
        return;
    gDriftLastMs = timeMs;
    float want = fmodf((timeMs - gDriftT0) / 60000.0f * gDriftDegPerMin / 360.0f, 1.0f);
    float du = want - gDriftApplied;
    if (du == 0.0f)
        return;
    ShiftU(sky, du);
    gDriftApplied = want;
    sky->MakeVBO();
}

namespace
{
std::vector<std::string> gCycle;
int gCycleStart = 400, gCycleFrames = 8;
long gCycleN = 0;
std::string gCycleShot;
bool gDevRead = false;  // the dev-aid env was read (first level load with SKY > 0)
bool gCycleLive = false; // this level has SKY > 0: the cycle may swap skies
std::string gForce;      // UA_SKY_FORCE

void ReadDevEnv()
{
    if (gDevRead)
        return;
    gDevRead = true;
    if (const char *f = getenv("UA_SKY_FORCE"))
        gForce = f;
    {
        const char *v = getenv("UA_SKY_CYCLE");
        if (v && v[0])
        {
            std::string s(v);
            size_t a = 0;
            while (a <= s.size())
            {
                size_t c = s.find(',', a);
                std::string t = s.substr(a, c == std::string::npos ? std::string::npos : c - a);
                if (!t.empty())
                    gCycle.push_back(t);
                if (c == std::string::npos)
                    break;
                a = c + 1;
            }
            if (const char *st = getenv("UA_SKY_CYCLE_START"))
                gCycleStart = std::max(1, atoi(st));
            if (const char *fr = getenv("UA_SKY_CYCLE_FRAMES"))
                gCycleFrames = std::max(2, atoi(fr));
        }
    }
}
}

const char *ForcedSky(int level)
{
    if (level <= 0) // SKY 0: no env read, the level's own sky, no cycle
    {
        if (gCycleLive)
            gCycleLive = false;
        return NULL;
    }
    ReadDevEnv();
    gCycleLive = !gCycle.empty();
    return gForce.empty() ? NULL : gForce.c_str();
}

bool CycleStep(std::string *next)
{
    if (!gCycleLive)
        return false;
    long k = gCycleN++ - gCycleStart;
    if (k < 0)
        return false;
    long idx = k / gCycleFrames, ph = k % gCycleFrames;
    if (idx >= (long)gCycle.size())
    {
        if (idx == (long)gCycle.size() && ph == 0)
        {
            printf("SKYCYCLE DONE\n");
            fflush(stdout);
        }
        return false;
    }
    if (ph == gCycleFrames - 1)
    {
        char b[160];
        snprintf(b, sizeof(b), "skycyc_%02ld_%s", idx, gCycle[idx].c_str());
        gCycleShot = b;
    }
    if (ph != 0)
        return false;
    *next = "objects/" + gCycle[idx] + ".base";
    printf("[SKYCYCLE] %ld %s\n", idx, next->c_str());
    return true;
}

std::string PendingShot()
{
    std::string s;
    s.swap(gCycleShot);
    return s;
}
}
