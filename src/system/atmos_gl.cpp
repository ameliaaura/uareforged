// Reforge ATMOSPHERE: GL side. See atmos.h for the levels and the shared sun.
//
// ATMOS 0: nothing in this file is called with any effect: no program compiled, no buffer, no uniform; the
// V2 and ground programs are built from their original texts (_v2Atmos / _gndAtmos stay 0).
// ATMOS 1+:
//   - legacy (1998) mesh draws use _atmStdProg instead of the std program: same inputs; baked shade x the
//     level's colour grade; the 1998 depth cue (mix to black between vizlimit-fade and vizlimit) mixes to the
//     sky's horizon colour at that azimuth instead (8-sector ring from atmos.txt), plus a light aerial haze from
//     near range. Night maps with the haze ring dark keep the 1998 fade-to-black look.
//     Sky, additive (lumtracy) and particle draws keep the std program (emitters: their fade to black = fade out).
//   - V2 + ground programs are rebuilt from variant texts (AtmosVariant: a few lines inserted) with the same
//     haze; the V2 sun takes the level's sky direction and sun/ambient tints (V2SunParams is the one source).
//   - ATMOS 2, night maps: facade atlases get lit windows (dark window cells vs their surroundings, a hashed
//     ~17 % lit, warm), V2 emissive (host-station lights) x hostLight. The HD (V2) building trims get the same
//     windows: a mask per trim from its window panes (atmos_winmask.h), added in the V2 variant.
//   - ATMOS 3: weather particles (one static VBO, one draw, wrapped around the camera in the vertex shader).
#include <algorithm>
#include <map>
#include <set>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>

#include "glapi.h" // GLES on the Quest / ANGLE, desktop GL otherwise
#include <SDL2/SDL_timer.h>

#include "gfx.h"
#include "../rsrc.h"
#include "atmos.h"
#include "atmos_winmask.h"
#include "perf.h"
#include "glfuncs.h"
#include "tform.h"
#ifdef _WIN32
#include "../winport.h"
#endif

// ATMOSPHERE night windows: the sets' building facade atlases (berud = ruins + the panel-block towers).
// n = normalised file name (lower case, no path).
bool AtmosFacadeAtlasName(const std::string &n)
{
    return n == "city1.ilbm" || n == "hype.ilbm" || n == "macht.ilbm" || n == "fcity1.ilbm" || n == "fhype.ilbm" ||
           n == "fmacht.ilbm" || n == "berud.ilbm" || n == "fberud.ilbm";
}

namespace GFX
{

namespace
{
const char *kAtmGlslLib = R"GLSL(
uniform vec3 AtmHaze[8];   // display colour per 45 deg sector, centre i at azimuth -180 + 45 i (atan2(x, z))
vec3 atmHazeAt(vec3 wdir)
{
    float t = (atan(wdir.x, wdir.z) * 57.2957795 + 180.0) / 45.0;
    float fl = floor(t);
    int i0 = int(fl) & 7;
    int i1 = (i0 + 1) & 7;
    return mix(AtmHaze[i0], AtmHaze[i1], t - fl);
}
)GLSL";

const char *kAtmStdVS = R"GLSL(#version 410 core
layout(std140) uniform Parameters
{
    mat4 MProj;
    mat4 MView;
    vec3 Fog;
    vec3 AlphaFog;
    bool Textured;
    bool Flat;
    bool ATest;
};
uniform mat3 AtmCamRot;    // view -> world rotation
uniform vec4 AtmFog;       // near haze: start, 1/range, max, -
uniform vec3 AtmGrade;     // linear-ish multiplier on the baked shade (display space, like the 1998 path)
in vec3 vPos;
in vec4 vColor;
in vec2 vUV;
out vec4 smoothColor;
flat out vec4 flatColor;
out vec2 texCoords;
out vec4 hazeV;            // rgb haze colour, a amount
@LIB@
void main()
{
    vec4 tformed = MView * vec4(vPos, 1.0);
    gl_Position = MProj * tformed;
    vec4 clr = vec4(vColor.rgb * AtmGrade, vColor.w);
    float fg = 0.0;
    if (Fog.x != 0.0)
        fg = clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);
    float nh = AtmFog.z * clamp((tformed.z - AtmFog.x) * AtmFog.y, 0.0, 1.0);
    hazeV = vec4(atmHazeAt(AtmCamRot * tformed.xyz), max(fg, nh));
    if (AlphaFog.x != 0.0)
        clr.w *= 1.0 - clamp((tformed.z - AlphaFog.y) / AlphaFog.z, 0.0, 1.0);
    flatColor = clr;
    smoothColor = clr;
    if (Textured) texCoords = vUV;
}
)GLSL";

const char *kAtmStdPS = R"GLSL(#version 410 core
layout(std140) uniform Parameters
{
    mat4 MProj;
    mat4 MView;
    vec3 Fog;
    vec3 AlphaFog;
    bool Textured;
    bool Flat;
    bool ATest;
};
uniform sampler2D texSample;
uniform float AtmWin;      // lit windows strength (facade atlases on night maps; 0 = off)
uniform sampler2D winSample; // unit 6: the facade's lit-window mask (BuildWindowMask)
in vec4 smoothColor;
flat in vec4 flatColor;
in vec2 texCoords;
in vec4 hazeV;
out vec4 fragColor;
void main()
{
    vec4 clr;
    if (Flat) clr = flatColor; else clr = smoothColor;
    vec4 tx = vec4(1.0);
    if (Textured)
    {
        tx = texture(texSample, texCoords);
        clr = tx * clr;
    }
    if (ATest && clr.w <= 0.0)
        discard;
    if (AtmWin > 0.0 && Textured)
        clr.rgb += texture(winSample, texCoords).rgb * AtmWin; // lit windows (mask built per facade atlas, mipmapped)
    clr.rgb = mix(clr.rgb, hazeV.rgb, hazeV.a);
    fragColor = clr;
}
)GLSL";

// Weather: one point per particle in a unit cube; the vertex shader wraps it into a box round the camera and
// moves it with time (no CPU work per frame). Rain draws each drop as a short line (2 vertices: head/tail).
const char *kWxVS = R"GLSL(#version 410 core
layout(std140) uniform Parameters
{
    mat4 MProj;
    mat4 MView;
    vec3 Fog;
    vec3 AlphaFog;
    bool Textured;
    bool Flat;
    bool ATest;
};
uniform mat3 ViewRot;      // world -> view
uniform vec3 CamPos;       // world
uniform vec3 Box;          // box size (world units)
uniform vec3 Vel;          // world velocity (units/s; y DOWN)
uniform float Time;
uniform float Streak;      // rain: tail length in seconds of motion
uniform float PtSize;      // dust/snow: point size in px at 100 units
in vec4 vSeed;             // xyz unit-cube position, w: 0 head / 1 tail (rain), else per-particle phase
out float vAlpha;
void main()
{
    float tail = (Streak > 0.0) ? vSeed.w : 0.0;
    vec3 wob = vec3(0.0);
    if (Streak <= 0.0) // dust/snow: slow per-particle wobble
        wob = vec3(sin(Time * 0.7 + vSeed.w * 6.28), 0.0, cos(Time * 0.53 + vSeed.w * 4.1)) * 0.012;
    vec3 p = vSeed.xyz + (Vel * Time) / Box + wob;
    vec3 rel = (fract(p - CamPos / Box) - 0.5) * Box;    // wrapped round the camera (head)
    rel -= Vel * (tail * Streak);                        // rain tail: same wrap as its head
    vec3 v = ViewRot * rel;
    gl_Position = MProj * vec4(v, 1.0);
    float d = length(rel);
    // fade in from the eye (no huge drops on the lens) and out at the box edge
    vAlpha = smoothstep(12.0, 40.0, v.z) * (1.0 - smoothstep(0.30, 0.5, max(abs(rel.x) / Box.x, abs(rel.z) / Box.z)));
    vAlpha *= (1.0 - tail * 0.85);
    gl_PointSize = clamp(PtSize * 100.0 / max(v.z, 1.0), 1.5, 14.0);
}
)GLSL";

const char *kWxPS = R"GLSL(#version 410 core
uniform vec4 Color;        // rgb display colour, a alpha
uniform float Streak;
in float vAlpha;
out vec4 fragColor;
void main()
{
    float a = Color.a * vAlpha;
    if (Streak <= 0.0)
    {
        vec2 q = gl_PointCoord * 2.0 - 1.0;
        a *= clamp(1.0 - dot(q, q), 0.0, 1.0);
    }
    if (a <= 0.003)
        discard;
    fragColor = vec4(Color.rgb, a);
}
)GLSL";

std::string WithLib(const char *src)
{
    std::string s(src);
    size_t p = s.find("@LIB@");
    if (p != std::string::npos)
        s.replace(p, 5, kAtmGlslLib);
    return s;
}

bool ReplaceOnce(std::string &s, const std::string &from, const std::string &to)
{
    size_t p = s.find(from);
    if (p == std::string::npos)
        return false;
    s.replace(p, from.size(), to);
    return true;
}

// Insert right after the Parameters block ("\n};\n" - the first one in every program text).
bool InsertDecl(std::string &s, const std::string &decl)
{
    size_t p = s.find("\n};\n");
    if (p == std::string::npos)
        return false;
    s.insert(p + 4, decl);
    return true;
}

bool Inv3(const mat3x3 &m, float out[9])
{
    double a = m.m00, b = m.m01, c = m.m02, d = m.m10, e = m.m11, f = m.m12, g = m.m20, h = m.m21, i = m.m22;
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (fabs(det) < 1e-12)
        return false;
    double id = 1.0 / det;
    out[0] = A * id; out[1] = -(b * i - c * h) * id; out[2] = (b * f - c * e) * id;
    out[3] = B * id; out[4] = (a * i - c * g) * id;  out[5] = -(a * f - c * d) * id;
    out[6] = C * id; out[7] = -(a * h - b * g) * id; out[8] = (a * e - b * d) * id;
    return true;
}

uint32_t LinkProgram(GFXEngine *e, uint32_t vs, uint32_t ps, const char *what)
{
    uint32_t id = Glext::GLCreateProgram();
    Glext::GLAttachShader(id, ps);
    Glext::GLAttachShader(id, vs);
    Glext::GLLinkProgram(id);
    GLint linked = 0;
    Glext::GLGetProgramiv(id, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char logbuf[4096] = {0};
        Glext::GLGetProgramInfoLog(id, sizeof(logbuf) - 1, NULL, logbuf);
        printf("atmos: %s program link failed:\n%s\n", what, logbuf);
        Glext::GLDeleteProgram(id);
        return 0;
    }
    return id;
}

void SetHazeRing(uint32_t prog)
{
    const Atmos::State &st = Atmos::Get();
    for (int i = 0; i < 8; ++i)
    {
        char nm[16];
        snprintf(nm, sizeof(nm), "AtmHaze[%d]", i);
        Glext::GLUniform3f(Glext::GLGetUniformLocation(prog, nm), st.haze[i][0], st.haze[i][1], st.haze[i][2]);
    }
}

// ATMOS 2, night maps: the lit-window mask of one facade atlas, built once per level from the atlas's own GL
// texture (level 0 read back, so HD replacements work too) and mipmapped, so windows stay a soft warm glow at
// range instead of vanishing (a per-pixel mask in the shader only worked up close). Window = a dark texel
// block (2x2 at 256 px) clearly darker than its surroundings (8x8) inside a lit facade (unused black atlas space
// and painted black backgrounds stay dark); ~17 % of the cells on an 80x56 grid lit, warm, a few cold. RGB8.
// Freed at every level load (AtmosRefresh). Memory: 256 px (HD atlases read at the 256 px mip): ~350 KB per
// atlas, 8 atlases ~2.8 MB, charged to Perf texture MB.
std::map<const ResBitmap *, GLuint> gWinMasks; // bitmap -> mask tex (built in AtmosRefresh; draws only look up)
int64_t gWinMaskBytes = 0; // charged to Perf (TexAlloc) while the masks live
std::map<GLuint, int64_t> gWinMaskSize; // mask tex -> bytes charged

float Ss(float e0, float e1, float x)
{
    float t = std::min(1.0f, std::max(0.0f, (x - e0) / (e1 - e0)));
    return t * t * (3.0f - 2.0f * t);
}

// Reads one level of a bound-or-not GL_TEXTURE_2D as RGBA8 (the caller sets GL_PACK_ALIGNMENT). Desktop GL:
// glGetTexImage. GLES (the Quest build) has none: the level is attached to a temporary FBO and read back; a
// texture that cannot be a colour attachment (e.g. compressed) returns false (= no lit windows on that atlas).
// Leaves GL_TEXTURE_2D bound to `tex`.
static bool ReadTexLevel(GLuint tex, int lv, int w, int h, uint8_t *out)
{
#ifdef UA_GLES
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, lv);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glDeleteFramebuffers(1, &fbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    return ok;
#else
    (void)w;
    (void)h;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, lv, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return true;
#endif
}

GLuint BuildWindowMask(const ResBitmap *bm)
{
    GLint w = 0, h = 0, lv = 0;
    glBindTexture(GL_TEXTURE_2D, bm->hwTex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    // HD replacements (hd-textures, 4x upscales of the 256 px atlases): read the mip nearest 256 px - the windows
    // are 256 px art, and a 1024 px mask cost +118 MB peak footprint on L7272 (measured)
    while (bm->hwTexMips && w > 256 && h > 256)
    {
        w >>= 1;
        h >>= 1;
        ++lv;
    }
    if (w < 16 || h < 16 || w > 4096 || h > 4096)
        return 0;
    std::vector<uint8_t> px((size_t)w * h * 4);
    GLint packA = 4, unpackA = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packA);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackA);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (!ReadTexLevel(bm->hwTex, lv, w, h, px.data()))
    {
        glPixelStorei(GL_PACK_ALIGNMENT, packA);
        return 0;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, packA);
    // luminance (stored encoding = display), summed-area table for box means
    std::vector<double> sat((size_t)(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y)
    {
        double row = 0.0;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t *p = &px[((size_t)y * w + x) * 4];
            row += (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
            sat[(size_t)(y + 1) * (w + 1) + x + 1] = sat[(size_t)y * (w + 1) + x + 1] + row;
        }
    }
    auto box = [&](int x, int y, int r) {
        int x0 = std::max(0, x - r), y0 = std::max(0, y - r), x1 = std::min(w, x + r + 1), y1 = std::min(h, y + r + 1);
        double s = sat[(size_t)y1 * (w + 1) + x1] - sat[(size_t)y0 * (w + 1) + x1] - sat[(size_t)y1 * (w + 1) + x0] +
                   sat[(size_t)y0 * (w + 1) + x0];
        return (float)(s / ((x1 - x0) * (y1 - y0)));
    };
    const int rs = std::max(1, w / 256);          // 3x3 at 256 px: 1-2 px dark seams between atlas tiles stay out
    const int rb = std::max(4, 4 * w / 256);      // ~8x8 at 256 px
    std::vector<uint8_t> out((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float l = box(x, y, rs), lb = box(x, y, rb);
            float win = Ss(0.70f, 0.45f, l / (lb + 0.02f)) * Ss(0.34f, 0.14f, l) * Ss(0.10f, 0.20f, lb);
            float cu = floorf((x + 0.5f) / w * 80.0f), cv = floorf((y + 0.5f) / h * 56.0f);
            float hs = sinf(cu * 12.9898f + cv * 78.233f) * 43758.5453f;
            hs -= floorf(hs);
            // ~17 % of the cells lit (fewer than an earlier 35 %; 45 % read too lived-in)
            float lit = Ss(0.79f, 0.85f, hs);
            float f7 = hs * 7.0f - floorf(hs * 7.0f);
            float wr = 1.0f, wg = 0.62f + 0.18f * f7, wb = 0.30f + 0.20f * f7;
            if (hs > 0.975f) { wr = 0.70f; wg = 0.82f; wb = 1.0f; } // ~1 in 8 lit cells cold (earlier: 1 in 7)
            float k = std::min(1.0f, win * lit * 1.1f);
            uint8_t *o = &out[((size_t)y * w + x) * 3];
            o[0] = (uint8_t)(255.0f * std::min(1.0f, wr * k));
            o[1] = (uint8_t)(255.0f * std::min(1.0f, wg * k));
            o[2] = (uint8_t)(255.0f * std::min(1.0f, wb * k));
        }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, out.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackA);
    Glext::GLGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gWinMaskBytes += (int64_t)w * h * 4 * 4 / 3; // RGB8 is stored as 4 bytes/texel on most drivers, + mips
    gWinMaskSize[id] = (int64_t)w * h * 4 * 4 / 3;
    return id;
}

// HD (V2) trims (night windows): mask per trim albedo, RGBA = 4 lit-window sets (atmos_winmask.h), read at the
// mip nearest 512 px (2048 px trims: windows ~40 px there), GL_REPEAT like the albedo (trims tile in u). Keyed by
// the bitmap + its GL name (a freed and reloaded map never reuses a stale mask). Built in AtmosRefresh (night,
// ATMOS 2+), freed at every level load with the facade masks. ~1.4 MB per trim (5 on the tower set, 18 with the block facades).
struct V2Mask
{
    GLuint mask = 0, src = 0;
};
std::map<const ResBitmap *, V2Mask> gV2WinMasks;
int gV2WinCount = 0; // windows found over all trims this level (log)

// An HD (V2) building trim albedo: hd-models lego/set<N>/trim_*_albedo.* (classified by path at level load).
bool IsV2TrimPath(const std::string &path)
{
    const char *p = path.c_str();
    const char *slash = strrchr(p, '/');
    const char *file = slash ? slash + 1 : p;
    return strncasecmp(file, "trim_", 5) == 0 && strcasestr(file, "_albedo.") && strcasestr(p, "/lego/set");
}

GLuint BuildV2TrimMask(const ResBitmap *bm, const std::string &path)
{
    GLint w = 0, h = 0, lv = 0;
    std::vector<uint8_t> px;
    GLint unpackA = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackA);
    // uploaded from a .dds (obj3d records the file LoadDdsTexture accepted, full chain validated there): decode
    // its 512 px BC1 mip on the CPU (~1 ms) - a GL read-back of a BC1 texture cost ~200 ms per trim in the driver
    if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".dds") == 0)
    {
        if (FILE *f = fopen(path.c_str(), "rb"))
        {
            std::vector<uint8_t> buf;
            if (fseek(f, 0, SEEK_END) == 0)
            {
                const long n = ftell(f);
                if (n > 0 && n < (64l << 20) && fseek(f, 0, SEEK_SET) == 0)
                {
                    buf.resize((size_t)n);
                    if (fread(buf.data(), 1, buf.size(), f) != buf.size())
                        buf.clear();
                }
            }
            fclose(f);
            int dw = 0, dh = 0;
            if (!buf.empty() && AtmosWin::DecodeDdsBc1Mip(buf, 512, px, dw, dh))
            {
                w = dw;
                h = dh;
            }
            else
                px.clear();
        }
    }
    if (px.empty())
    {
        // GL read-back of the mip nearest 512 px; a texture without that level (no chain / partial chain) is read
        // at level 0 and box-downsampled here
        glBindTexture(GL_TEXTURE_2D, bm->hwTex);
        GLint w0 = 0, h0 = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        if (w0 < 64 || h0 < 64 || w0 > 8192 || h0 > 8192)
            return 0;
        w = w0;
        h = h0;
        int f = 1;
        while (w > 512 && h > 512)
        {
            w >>= 1;
            h >>= 1;
            f <<= 1;
            ++lv;
        }
        GLint lw = 0;
        if (lv > 0)
            glGetTexLevelParameteriv(GL_TEXTURE_2D, lv, GL_TEXTURE_WIDTH, &lw);
        const bool haveLv = lv == 0 || (bm->hwTexMips && lw == w);
        std::vector<uint8_t> src((size_t)(haveLv ? w : w0) * (haveLv ? h : h0) * 4);
        GLint packA = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &packA);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        const bool got = ReadTexLevel(bm->hwTex, haveLv ? lv : 0, haveLv ? w : w0, haveLv ? h : h0, src.data()); // sRGB: stored encoding
        glPixelStorei(GL_PACK_ALIGNMENT, packA);
        if (!got)
            return 0;
        if (haveLv)
            px.swap(src);
        else
        {
            px.assign((size_t)w * h * 4, 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        uint32_t sum = 0;
                        for (int yy = 0; yy < f; ++yy)
                            for (int xx = 0; xx < f; ++xx)
                                sum += src[(((size_t)(y * f + yy)) * w0 + x * f + xx) * 4 + c];
                        px[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum / (uint32_t)(f * f));
                    }
        }
    }
    if (w < 64 || h < 64 || w > 1024 || h > 1024)
        return 0;
    std::vector<uint8_t> out;
    const AtmosWin::Stats ws = AtmosWin::BuildTrimMask(px.data(), w, h, out);
    gV2WinCount += ws.windows;
    if (ws.windows == 0)
        return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackA);
    Glext::GLGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gWinMaskBytes += (int64_t)w * h * 4 * 4 / 3;
    gWinMaskSize[id] = (int64_t)w * h * 4 * 4 / 3;
    return id;
}

void FreeWindowMasks()
{
    for (auto &kv : gV2WinMasks)
        if (kv.second.mask)
            glDeleteTextures(1, &kv.second.mask);
    gV2WinMasks.clear();
    gV2WinCount = 0;
    for (auto &kv : gWinMasks)
        if (kv.second)
            glDeleteTextures(1, &kv.second);
    gWinMasks.clear();
    for (auto &kv : gWinMaskSize)
        Perf::TexAlloc(false, -kv.second);
    gWinMaskSize.clear();
    gWinMaskBytes = 0;
}

// Normalised resource name (lower case, cut at the first NUL / trailing blanks, file part only), as GroundTagBitmap.
std::string BaseName(const std::string &raw)
{
    std::string n(raw.c_str());
    for (char &c : n)
        c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\n' || n.back() == '\t'))
        n.pop_back();
    size_t slash = n.find_last_of("/\\:");
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

// Level load (AtmosRefresh, night maps at ATMOS 2+ only): the window mask of every loaded facade atlas, so the
// draw path never reads back, allocates or uploads. The set is loaded (textures prepared) before the sky.
int PrebuildWindowMasks()
{
    int n = 0;
    RSRCList *lists[2] = {NC_STACK_rsrc::getRsrc_sharedList(), NC_STACK_rsrc::getRsrc_privateList()};
    for (RSRCList *l : lists)
        for (rsrc *r : *l)
        {
            if (!r || !r->data || !AtmosFacadeAtlasName(BaseName(r->name)))
                continue;
            const ResBitmap *bm = (const ResBitmap *)r->data; // *.ilbm resources are bitmaps (bitmap.cpp rsrc data)
            if (!bm->facadeAtlas || !bm->hwTex || !bm->hwTexMips || gWinMasks.count(bm))
                continue;
            const int64_t before = gWinMaskBytes;
            const GLuint m = BuildWindowMask(bm);
            if (gWinMaskBytes != before)
                Perf::TexAlloc(false, gWinMaskBytes - before);
            gWinMasks[bm] = m;
            n += m ? 1 : 0;
        }
    return n;
}

// Level load (AtmosRefresh, night maps at ATMOS 2+ only): the window mask of every loaded HD trim albedo.
int PrebuildV2WindowMasks()
{
    int n = 0;
    Obj3DForEachCachedMap(
        [](ResBitmap *bm, const std::string &path, void *ctx) {
            if (!bm->hwTex || gV2WinMasks.count(bm) || !IsV2TrimPath(path))
                return;
            const int64_t before = gWinMaskBytes;
            V2Mask m;
            m.src = bm->hwTex;
            const uint32_t t0 = SDL_GetTicks();
            m.mask = BuildV2TrimMask(bm, path);
            if (getenv("UA_ATMOS_LOG")) // dev aid (night refresh at ATMOS 2+ only)
                printf("atmos: trim mask %s: %u ms\n", path.c_str(), (unsigned)(SDL_GetTicks() - t0));
            if (gWinMaskBytes != before)
                Perf::TexAlloc(false, gWinMaskBytes - before);
            gV2WinMasks[bm] = m;
            *(int *)ctx += m.mask ? 1 : 0;
        },
        &n);
    return n;
}

// V2 pixel variant (kind 2) addition: lit windows in display space before the haze mix, as the legacy program
// (clr.rgb += mask * AtmWin). One warm and one cold set per trim tile (floor(uv) hash), colours as BuildWindowMask.
const char *kAtmV2WinLib = R"GLSL(
uniform float AtmWin;          // lit windows strength (HD trims on night maps; 0 = off)
uniform sampler2D atmWinSample; // unit 6: the trim's window mask (atmos_winmask.h: RGBA = 4 lit sets)
vec3 atmWindows(vec2 uv)
{
    if (AtmWin <= 0.0)
        return vec3(0.0);
    vec4 m = texture(atmWinSample, uv);
    float h = fract(sin(dot(floor(uv), vec2(12.9898, 78.233))) * 43758.5453);
    float wa = h < 0.5 ? m.r : m.g;
    float co = fract(h * 2.0 + 0.25) < 0.5 ? m.b : m.a;
    float f = fract(h * 7.0);
    return (vec3(1.0, 0.62 + 0.18 * f, 0.30 + 0.20 * f) * wa + vec3(0.70, 0.82, 1.0) * co) * AtmWin;
}
)GLSL";
} // namespace



std::string GFXEngine::AtmosVariant(const std::string &src, int kind)
{
    std::string s = src;
    bool ok = true;
    const std::string nearLine =
        "\n    fogMul = min(fogMul, 1.0 - AtmFog.z * clamp((tformed.z - AtmFog.x) * AtmFog.y, 0.0, 1.0));\n";
    if (kind == 1 || kind == 3) // V2 / ground vertex: aerial haze folded into fogMul (1 = no haze)
    {
        ok = InsertDecl(s, "uniform vec4 AtmFog;\n") &&
             ReplaceOnce(s, "fogMul = 1.0 - clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);\n",
                         "fogMul = 1.0 - clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);\n" + nearLine);
    }
    else if (kind == 2) // V2 pixel: mix to this draw's haze colour instead of black
    {
        ok = InsertDecl(s, std::string("uniform vec3 AtmHazeCol;\n") + kAtmV2WinLib) &&
             ReplaceOnce(s, "vec3 outc = toSrgb(ToneMap != 0 ? neutralTonemap(lit, ToneMap == 1) : lit) * fogMul;",
                         "vec3 outc = mix(AtmHazeCol, toSrgb(ToneMap != 0 ? neutralTonemap(lit, ToneMap == 1) : lit)"
                         " + atmWindows(texCoords), fogMul);");
    }
    else if (kind == 4) // ground pixel: grade + per-pixel haze ring
    {
        ok = InsertDecl(s, std::string("uniform vec3 AtmGrade;\n") + kAtmGlslLib) &&
             ReplaceOnce(s, "    fragColor = vec4(toSrgb(col) * fogMul, hw.w * vc.w);\n}",
                         "    fragColor = vec4(mix(atmHazeAt(wPos - CamPos), toSrgb(col) * AtmGrade, fogMul), hw.w * vc.w);\n}");
    }
    if (!ok)
    {
        printf("atmos: shader variant %d: anchor not found - keeping the original text\n", kind);
        return src;
    }
    return s;
}

bool GFXEngine::AtmosLegacyUse(const TRenderNode *nod, uint32_t flags) const
{
    (void)nod;
    return _atmStdProg.ID && Atmos::Active() && !(flags & (RFLAGS_SKY | RFLAGS_LUMTRACY | RFLAGS_FX_ALPHA));
}

// Program rebuilds shared by AtmosRefresh (atmosphere variant on/off) and ShadowsApply (shadow lookup on/off).
void GFXEngine::RebuildV2Program()
{
    Glext::GLDeleteProgram(_v2ShaderProg.ID);
    if (_v2VsShader) Glext::GLDeleteShader(_v2VsShader);
    if (_v2PsShader) Glext::GLDeleteShader(_v2PsShader);
    _v2ShaderProg = TShaderProg();
    _v2VsShader = _v2PsShader = 0;
    const bool wasOn = _renderV2;
    InitV2Program();
    _renderV2 = wasOn && _v2ShaderProg.ID;
    AtmosV2Locations();
}

void GFXEngine::AtmosV2Locations()
{
    const bool on = _v2Atmos && _v2ShaderProg.ID;
    _v2AtmHaze = on ? Glext::GLGetUniformLocation(_v2ShaderProg.ID, "AtmHazeCol") : -1;
    _v2AtmFog = on ? Glext::GLGetUniformLocation(_v2ShaderProg.ID, "AtmFog") : -1;
    _v2AtmWin = on ? Glext::GLGetUniformLocation(_v2ShaderProg.ID, "AtmWin") : -1;
    _v2AtmWinS = on ? Glext::GLGetUniformLocation(_v2ShaderProg.ID, "atmWinSample") : -1;
}

void GFXEngine::RebuildGroundProgram()
{
    Glext::GLDeleteProgram(_gndShaderProg.ID);
    if (_gndVs) Glext::GLDeleteShader(_gndVs);
    if (_gndPs) Glext::GLDeleteShader(_gndPs);
    _gndShaderProg = TShaderProg();
    _gndVs = _gndPs = 0;
    if (!GroundBuildProgram())
        GroundDeinit();
}

void GFXEngine::AtmosFreeLegacyProgram()
{
    if (_atmStdProg.ID) Glext::GLDeleteProgram(_atmStdProg.ID);
    if (_atmStdVs) Glext::GLDeleteShader(_atmStdVs);
    if (_atmStdPs) Glext::GLDeleteShader(_atmStdPs);
    _atmStdProg = TShaderProg();
    _atmStdVs = _atmStdPs = 0;
}

void GFXEngine::AtmosRefresh()
{
    if (!_vbo)
        return;
    const Atmos::State &st = Atmos::Get();
    const int want = st.active ? 1 : 0;
    ++_atmLevelGen;
    FreeWindowMasks(); // facade window masks are per level (night maps at ATMOS 2+ only)
    if (st.active && st.windows > 0.0f)
    {
        const int nm = PrebuildWindowMasks();
        const uint32_t t0 = SDL_GetTicks();
        const int nv = PrebuildV2WindowMasks();
        glBindTexture(GL_TEXTURE_2D, _lastStates.Tex); // both mask builds rebind unit 0: back to the cached state
        printf("atmos: %d facade window mask(s) + %d HD trim mask(s) (%d windows, %u ms) built (%.1f MB)\n", nm, nv,
               gV2WinCount, (unsigned)(SDL_GetTicks() - t0), gWinMaskBytes / 1048576.0);
    }

    if (!want && _atmStdProg.ID)
        AtmosFreeLegacyProgram(); // atmosphere switched off in the menu: nothing of it stays resident
    if (want && !_atmStdProg.ID)
    {
        // + the shadow lookup when SHADOWS > 0 (this program replaces the shadow legacy receiver)
        _atmStdVs = CompileShader(GL_VERTEX_SHADER, ShadowGroundVS(WithLib(kAtmStdVS)));
        _atmStdPs = CompileShader(GL_FRAGMENT_SHADER, ShadowAtmStdPS(kAtmStdPS));
        uint32_t id = LinkProgram(this, _atmStdVs, _atmStdPs, "legacy");
        if (id)
        {
            _atmStdProg = TShaderProg(id);
            BindVBOParameters(_atmStdProg);
            ShadowProgramLinked(id); // no call at SHADOWS 0
            _atmLoc.CamRot = Glext::GLGetUniformLocation(id, "AtmCamRot");
            _atmLoc.Haze0 = Glext::GLGetUniformLocation(id, "AtmHaze[0]");
            _atmLoc.Fog = Glext::GLGetUniformLocation(id, "AtmFog");
            _atmLoc.Grade = Glext::GLGetUniformLocation(id, "AtmGrade");
            _atmLoc.Win = Glext::GLGetUniformLocation(id, "AtmWin");
            Glext::GLUseProgram(id);
            Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "texSample"), 0);
            Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "winSample"), 6);
            Glext::GLUseProgram(_lastStates.Prog.ID);
        }
    }

    // V2 / ground programs: rebuild from the matching text when the atmosphere switched on or off.
    if (_v2ShaderProg.ID && _v2Atmos != want)
    {
        _v2Atmos = want;
        RebuildV2Program();
    }
    else if (!_v2ShaderProg.ID)
        _v2Atmos = want; // compiled later (SetRenderV2) from the right text
    if (_v2ShaderProg.ID)
        AtmosV2Locations();
    if (_groundOn && _gndAtmos != want)
    {
        _gndAtmos = want;
        RebuildGroundProgram();
    }
    else if (_groundOn && want)
    {
        // same text, new level: sun + haze uniforms
        Glext::GLUseProgram(_gndShaderProg.ID);
        float sun[3], sl, al, hh;
        V2SunParams(sun, &sl, &al, &hh);
        Glext::GLUniform3f(Glext::GLGetUniformLocation(_gndShaderProg.ID, "SunW"), sun[0], sun[1], sun[2]);
        Glext::GLUniform3f(Glext::GLGetUniformLocation(_gndShaderProg.ID, "Light"), sl, al, hh);
        AtmosGroundUniforms(_gndShaderProg.ID);
        Glext::GLUseProgram(_lastStates.Prog.ID);
    }
    else if (!_groundOn)
        _gndAtmos = want;

    // weather buffer (level 3, weather row): built once per kind, freed when off
    const int kind = (st.active && st.weather != Atmos::W_NONE) ? st.weather : 0;
    if (kind != _wxKind)
    {
        if (_wxVbo) { Glext::GLDeleteBuffers(1, &_wxVbo); _wxVbo = 0; }
        _wxCount = 0;
        _wxKind = kind;
        if (kind)
        {
            if (!_wxProg.ID)
            {
                _wxVs = CompileShader(GL_VERTEX_SHADER, kWxVS);
                _wxPs = CompileShader(GL_FRAGMENT_SHADER, kWxPS);
                uint32_t id = LinkProgram(this, _wxVs, _wxPs, "weather");
                if (id)
                {
                    _wxProg = TShaderProg();
                    _wxProg.ID = id;
                    BindVBOParameters(_wxProg);
                    _wxLoc.ViewRot = Glext::GLGetUniformLocation(id, "ViewRot");
                    _wxLoc.CamPos = Glext::GLGetUniformLocation(id, "CamPos");
                    _wxLoc.Box = Glext::GLGetUniformLocation(id, "Box");
                    _wxLoc.Vel = Glext::GLGetUniformLocation(id, "Vel");
                    _wxLoc.Time = Glext::GLGetUniformLocation(id, "Time");
                    _wxLoc.Color = Glext::GLGetUniformLocation(id, "Color");
                    _wxLoc.Streak = Glext::GLGetUniformLocation(id, "Streak");
                    _wxLoc.PtSize = Glext::GLGetUniformLocation(id, "PtSize");
                }
            }
            if (_wxProg.ID)
            {
                const int n = kind == Atmos::W_RAIN ? (int)(12000 * st.weatherK) : (int)(8000 * st.weatherK);
                std::vector<float> v;
                v.reserve(n * 8);
                uint32_t r = 0x2545F491u;
                auto rnd = [&r]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (r & 0xFFFFFF) / 16777216.0f; };
                for (int i = 0; i < n; ++i)
                {
                    float x = rnd(), y = rnd(), z = rnd(), ph = rnd();
                    if (kind == Atmos::W_RAIN)
                    {
                        float a[8] = {x, y, z, 0.0f, x, y, z, 1.0f};
                        v.insert(v.end(), a, a + 8);
                    }
                    else
                    {
                        float a[4] = {x, y, z, ph};
                        v.insert(v.end(), a, a + 4);
                    }
                }
                _wxCount = (int)(v.size() / 4);
                Glext::GLGenBuffers(1, &_wxVbo);
                Glext::GLBindBuffer(GL_ARRAY_BUFFER, _wxVbo);
                Glext::GLBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
                Glext::GLBindBuffer(GL_ARRAY_BUFFER, 0);
                _wxTime = 0.0;
            }
        }
    }
    if (st.active)
        printf("atmos: programs legacy %u v2 %u (atmos text %d) ground %u (atmos text %d) weather %d x%d\n",
               _atmStdProg.ID, _v2ShaderProg.ID, _v2Atmos, _gndShaderProg.ID, _gndAtmos, _wxKind, _wxCount);
}

void GFXEngine::AtmosGroundUniforms(uint32_t prog)
{
    const Atmos::State &st = Atmos::Get();
    if (!st.active)
        return;
    SetHazeRing(prog);
    Glext::GLUniform3f(Glext::GLGetUniformLocation(prog, "AtmGrade"), st.grade[0], st.grade[1], st.grade[2]);
    Glext::GLUniform4f(Glext::GLGetUniformLocation(prog, "AtmFog"), st.nearStart, 1.0f / st.nearRange, st.nearMax, 0.0f);
}

void GFXEngine::AtmosStdSetup(const TRenderNode *nod)
{
    const Atmos::State &st = Atmos::Get();
    static uint32_t sentGen = 0;
    static float lastInv[9] = {0};
    static float lastWin = -1.0f;
    if (sentGen != _atmLevelGen)
    {
        lastWin = -1.0f; // the program may have been rebuilt (ShadowsApply / AtmosRefresh): uniform not sent yet
        sentGen = _atmLevelGen;
        SetHazeRing(_atmStdProg.ID);
        Glext::GLUniform3f(_atmLoc.Grade, st.grade[0], st.grade[1], st.grade[2]);
        Glext::GLUniform4f(_atmLoc.Fog, st.nearStart, 1.0f / st.nearRange, st.nearMax, 0.0f);
        memset(lastInv, 0, sizeof(lastInv));
    }
    if (TF::TForm3D *view = TF::Engine.GetViewPoint())
    {
        float inv[9];
        if (Inv3(view->CalcSclRot, inv) && memcmp(inv, lastInv, sizeof(inv)) != 0)
        {
            memcpy(lastInv, inv, sizeof(inv));
            Glext::GLUniformMatrix3fv(_atmLoc.CamRot, 1, GL_TRUE, inv);
        }
    }
    static const bool wlog = getenv("UA_ATMOS_LOG") != NULL;
    if (wlog && nod->Tex && nod->Tex->facadeAtlas)
    {
        static std::set<const void *> seen;
        if (seen.insert(nod->Tex).second)
            printf("atmos: facade draw tex %p mips %d windows %.2f\n", (const void *)nod->Tex, nod->Tex->hwTexMips ? 1 : 0, st.windows);
    }
    const float win = (st.windows > 0.0f && nod->Tex && nod->Tex->facadeAtlas && nod->Tex->hwTexMips) ? st.windows : 0.0f;
    float winUse = win;
    if (win > 0.0f)
    {
        // built at level load (AtmosRefresh): lookup only; an atlas without a mask draws without windows
        auto it = gWinMasks.find(nod->Tex);
        if (it != gWinMasks.end() && it->second)
        {
            Glext::GLActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, it->second);
            Glext::GLActiveTexture(GL_TEXTURE0);
        }
        else
            winUse = 0.0f;
    }
    if (winUse != lastWin)
    {
        lastWin = winUse;
        Glext::GLUniform1f(_atmLoc.Win, winUse);
    }
}

// V2 draw: haze colour from the object's azimuth (objects are small: one colour per draw), near haze.
void GFXEngine::AtmosV2Setup(const TRenderNode *nod, float sunView[3])
{
    (void)sunView;
    const Atmos::State &st = Atmos::Get();
    if (_v2AtmFog >= 0)
        Glext::GLUniform4f(_v2AtmFog, st.nearStart, 1.0f / st.nearRange, st.nearMax, 0.0f);
    if (_v2AtmHaze >= 0)
    {
        float hz[3] = {0, 0, 0};
        vec3d c(0.0, 0.0, 1.0);
        if (nod->Mesh && nod->Mesh->BoundBox.Inited)
            c = nod->TForm.Transform((nod->Mesh->BoundBox.Min + nod->Mesh->BoundBox.Max) * 0.5);
        float inv[9];
        TF::TForm3D *view = TF::Engine.GetViewPoint();
        if (view && Inv3(view->CalcSclRot, inv))
        {
            float wx = inv[0] * c.x + inv[1] * c.y + inv[2] * c.z;
            float wz = inv[6] * c.x + inv[7] * c.y + inv[8] * c.z;
            Atmos::HazeAt(wx, wz, hz);
        }
        Glext::GLUniform3f(_v2AtmHaze, hz[0], hz[1], hz[2]);
    }
    if (_v2AtmWin >= 0)
    {
        // lit windows on HD trims (night, ATMOS 2+): lookup only (masks built at level load); others draw with 0
        float win = 0.0f;
        if (st.windows > 0.0f && nod->Tex && !gV2WinMasks.empty())
        {
            auto it = gV2WinMasks.find(nod->Tex);
            if (it != gV2WinMasks.end() && it->second.mask && it->second.src == nod->Tex->hwTex)
            {
                win = st.windows;
                Glext::GLActiveTexture(GL_TEXTURE6);
                glBindTexture(GL_TEXTURE_2D, it->second.mask);
                Glext::GLActiveTexture(GL_TEXTURE0);
                Glext::GLUniform1i(_v2AtmWinS, 6);
            }
        }
        Glext::GLUniform1f(_v2AtmWin, win);
    }
}

void GFXEngine::AtmosDrawWeather(float dtSec)
{
    const Atmos::State &st = Atmos::Get();
    if (!_wxKind || !_wxVbo || !_wxProg.ID || !st.active)
        return;
    TF::TForm3D *view = TF::Engine.GetViewPoint();
    if (!view)
        return;
    _wxTime += dtSec;
    if (!_wxVao)
        Glext::GLGenVertexArrays(1, &_wxVao);

    // colours: lit by the level's ambient, a touch of the haze
    float hz[3];
    Atmos::HazeAt(1.0f, 0.0f, hz);
    float amb = (st.ambTint[0] + st.ambTint[1] + st.ambTint[2]) / 3.0f;
    float col[4], box[3], vel[3], streak = 0.0f, pt = 0.0f;
    if (_wxKind == Atmos::W_RAIN)
    {
        for (int c = 0; c < 3; ++c) col[c] = std::min(1.0f, 0.55f * amb + 0.35f * hz[c] + 0.1f);
        col[3] = 0.36f;
        box[0] = 700; box[1] = 500; box[2] = 700;
        vel[0] = 40; vel[1] = 520; vel[2] = 25;   // y DOWN: falling
        streak = 0.07f;                           // ~36-unit streaks (0.045 read as nothing at 1080p)
    }
    else if (_wxKind == Atmos::W_SNOW)
    {
        for (int c = 0; c < 3; ++c) col[c] = std::min(1.0f, 0.8f * amb + 0.2f);
        col[3] = 0.55f;
        box[0] = 600; box[1] = 400; box[2] = 600;
        vel[0] = 12; vel[1] = 35; vel[2] = 6;
        pt = 2.2f;
    }
    else // dust: warm motes drifting with the wind, lit by the sun colour
    {
        for (int c = 0; c < 3; ++c) col[c] = std::min(1.0f, 0.35f * hz[c] + 0.45f * st.sunTint[c] * 0.6f + 0.22f);
        col[3] = 0.55f;
        box[0] = 650; box[1] = 300; box[2] = 650;
        vel[0] = 55; vel[1] = -4; vel[2] = 30;
        pt = 7.0f; // 1.6 (1 px) and 4.0 (2 px, haze-coloured) were both invisible from the commander still
    }

    // world -> view = SclRot (view = SclRot * (world - CamPos))
    const mat3x3 &m = view->CalcSclRot;
    float vr[9] = {(float)m.m00, (float)m.m01, (float)m.m02, (float)m.m10, (float)m.m11, (float)m.m12,
                   (float)m.m20, (float)m.m21, (float)m.m22};

    Glext::GLUseProgram(_wxProg.ID);
    CommitUBOParameters();
    Glext::GLUniformMatrix3fv(_wxLoc.ViewRot, 1, GL_TRUE, vr);
    Glext::GLUniform3f(_wxLoc.CamPos, (float)view->CalcPos.x, (float)view->CalcPos.y, (float)view->CalcPos.z);
    Glext::GLUniform3f(_wxLoc.Box, box[0], box[1], box[2]);
    Glext::GLUniform3f(_wxLoc.Vel, vel[0], vel[1], vel[2]);
    Glext::GLUniform1f(_wxLoc.Time, (float)fmod(_wxTime, 3600.0));
    Glext::GLUniform4f(_wxLoc.Color, col[0], col[1], col[2], col[3]);
    Glext::GLUniform1f(_wxLoc.Streak, streak);
    Glext::GLUniform1f(_wxLoc.PtSize, pt);

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    Glext::GLBindVertexArray(_wxVao);
    Glext::GLBindBuffer(GL_ARRAY_BUFFER, _wxVbo);
    GLint loc = Glext::GLGetAttribLocation(_wxProg.ID, "vSeed");
    Glext::GLEnableVertexAttribArray(loc);
    Glext::GLVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#ifndef UA_GLES // GLES: gl_PointSize always applies
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    glDrawArrays(_wxKind == Atmos::W_RAIN ? GL_LINES : GL_POINTS, 0, _wxCount);
#ifndef UA_GLES
    glDisable(GL_PROGRAM_POINT_SIZE);
#endif

    Glext::GLBindVertexArray(prevVao);
    Glext::GLBindBuffer(GL_ARRAY_BUFFER, 0);
    Glext::GLUseProgram(_lastStates.Prog.ID);
    // restore the cached state machine's view of blend / z write
    if (_lastStates.Zwrite) glDepthMask(GL_TRUE);
    if (!_lastStates.AlphaBlend) glDisable(GL_BLEND);
    else glBlendFunc(_lastStates.SrcBlend, _lastStates.DstBlend);
}

}
