// Reforge ground (UA_RENDER_V2=1 + UA_R2_GROUND=1).
//
// The original ground is the sets' boden1-5 atlases (2x2 tiles of 128 px, one
// tile per 300-unit sub-sector), drawn on the lego sector meshes and the
// sector-edge fillers. Close up a texel covers ~2.3 world units, so the ground
// reads as pixelated / smeared. This path keeps the original atlas as the
// MACRO colour (so every ground type keeps its colour and value structure at
// RTS distance - the far read is the original's by construction) and adds
// authored, tileable world-space detail on top:
//   - macro: the atlas through a B-spline bicubic at a floor LOD (no texel
//     steps, no AI-upscale hallucinations close up), handing over to the
//     game's own trilinear+aniso sample as soon as the atlas is minified;
//     taps are clamped to the atlas tile so neighbouring tiles never bleed;
//   - class: per-atlas class masks (pack set<N>/boden<k>_mask.png: soil, scrub,
//     asphalt, concrete, scorched, metal, rubble) pick the authored material,
//     top-2 classes height-blended;
//   - detail: albedo MULTIPLIER (every channel mean 1.0, so it never moves the
//     mean colour), normal, roughness, metalness from a texture array, sampled
//     in world space at two scales (fine + coarse rotated octave), planar
//     projection on the face's dominant axis;
//   - macro variation: a very low-frequency mean-1 noise breaks the
//     300-unit sub-sector repetition (UA_R2_GROUND_VAR, default small);
//   - light: the V2 sun, applied as a RATIO lit(detail normal) / lit(face
//     normal) on top of the game's baked per-polygon shade, plus a GGX
//     specular that fades out with distance. A flat detail normal gives ratio
//     1, so the baked look is the floor, never shifted.
// Off (default): nothing here runs - no program, no textures, no draw change.
//
// Env:
//   UA_R2_GROUND=1            enable (needs UA_RENDER_V2=1)
//   UA_R2_GROUND_DIR=<dir>    pack (default: reforge-ground under the cwd)
//   UA_R2_GROUND_DETAIL=a,n,s detail strengths: albedo, normal, specular (default 1,1,1)
//   UA_R2_GROUND_VAR=v        macro variation amplitude (default 0.06; 0 = off)
//   UA_R2_GROUND_MACRO=b      macro floor LOD bias over the original 256 px level (default 0.5)
//   UA_R2_GROUND_FADE=d0,d1   view depth where normal + specular fade out (default 900,3200)
//   UA_R2_GROUND_DEBUG=class|macro|detail|ratio|nodetail   debug output
//   UA_R2_GROUND_LOG=1        print the first ground draws (atlas, uv range)

#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_image.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/glext.h>
#else
#include <GL/glext.h>
#endif
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "../fmtlib/printf.h"
#include "gfx.h"
#include "glfuncs.h"
#include "tform.h"
#include "perf.h"

#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

// ---------------------------------------------------------------------------
// atlas tagging
// ---------------------------------------------------------------------------
static int gGroundSet = 0;

void GroundSetCurrentSet(int setId)
{
    gGroundSet = setId;
}

void GroundTagBitmap(ResBitmap *bitm, const std::string &rsrcName)
{
    if (!bitm)
        return;
    std::string n;
    for (char c : rsrcName)
        n += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    // Resource names can carry an embedded NUL / padding (fixed-size name buffers):
    // cut at the first NUL and trailing blanks, else no atlas ever matched
    // (2026-10-15: every ground draw stayed classic since stage 1).
    n = std::string(n.c_str());
    while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\n' || n.back() == '\t'))
        n.pop_back();
    size_t slash = n.find_last_of("/\\:");
    if (slash != std::string::npos)
        n = n.substr(slash + 1);
    static int diag = -1;
    if (diag < 0)
        diag = getenv("UA_R2_GROUND_LOG") ? atoi(getenv("UA_R2_GROUND_LOG")) : 0;
    if (diag > 0 && n.find("oden") != std::string::npos)
        printf("UA_R2_GROUND: tag? '%s' len %zu (raw len %zu) set %d bitm %p\n", n.c_str(), n.size(), rsrcName.size(), gGroundSet, (void *)bitm);
    if (n.size() == 11 && n.compare(0, 5, "boden") == 0 && n.compare(6, 5, ".ilbm") == 0 && n[5] >= '1' && n[5] <= '5')
    {
        bitm->groundAtlas = n[5] - '0';
        bitm->groundSet = gGroundSet;
    }
    // ATMOSPHERE (night windows): the sets' building facade atlases (atmos_gl.cpp list). A bool tag; nothing reads
    // it at ATMOS 0/1 (AtmosRefresh builds their window masks at ATMOS 2+ night levels).
    if (AtmosFacadeAtlasName(n))
        bitm->facadeAtlas = true;
}

namespace GFX
{

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------
const std::string GFXEngine::_gndVShaderText = R"GLSL(#version 410 core
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
uniform mat3 CamRotInv;  // view -> world rotation (inverse of the view's SclRot)
uniform vec3 CamPos;     // world
in vec3 vPos;
in vec4 vColor;
in vec2 vUV;
out vec4 smoothColor;
flat out vec4 flatColor;
out vec2 texCoords;
out vec3 wPos;
out float vDepth;
out float fogMul;
void main()
{
    vec4 tformed = MView * vec4(vPos, 1.0);
    gl_Position = MProj * tformed;
    wPos = CamRotInv * tformed.xyz + CamPos;
    vDepth = tformed.z;
    fogMul = 1.0;
    if (Fog.x != 0.0)
        fogMul = 1.0 - clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);
    vec4 clr = vColor;
    if (AlphaFog.x != 0.0)
        clr.w *= 1.0 - clamp((tformed.z - AlphaFog.y) / AlphaFog.z, 0.0, 1.0);
    flatColor = clr;
    smoothColor = clr;
    texCoords = vUV;
}
)GLSL";

const std::string GFXEngine::_gndPShaderText = R"GLSL(#version 410 core
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
uniform sampler2D texSample;      // 0: the original ground atlas (macro colour)
uniform sampler2D maskA;          // 1: class weights 0-3
uniform sampler2D maskB;          // 2: class weights 4-7
uniform sampler2DArray detAlb;    // 3: albedo multiplier x0.5 (rgb), height (a)
uniform sampler2DArray detNrm;    // 4: normal xy, roughness, metalness
uniform sampler2D macroNoise;     // 5: low-frequency variation
uniform vec3 CamPos;
uniform vec3 SunW;                // world, towards the sun
uniform vec3 Light;               // sunLin, ambLin, hemiHalf
uniform vec4 Tile;                // 1/fine, 1/coarse, cos, sin of the coarse rotation
uniform vec3 Strength;            // albedo, normal, specular
uniform float MacroVar;
uniform float MacroLod;           // bicubic floor level of the atlas
uniform vec2 Fade;                // view depth: normal/specular fade start, end
uniform vec4 ClassSpec[2];        // per-class specular strength
uniform int Layers;
uniform int Debug;
in vec4 smoothColor;
flat in vec4 flatColor;
in vec2 texCoords;
in vec3 wPos;
in float vDepth;
in float fogMul;
out vec4 fragColor;
const float PI = 3.14159265;

vec3 toLin(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 toSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Cubic B-spline of the atlas at one mip level, 4 bilinear taps, each clamped
// to the 2x2 atlas tile the fragment belongs to.
vec4 macroCubic(vec2 uv, float lod)
{
    int L = int(floor(lod));
    vec2 sz = vec2(textureSize(texSample, L));
    vec2 cell = floor(uv * 2.0);
    vec2 lo = cell * 0.5 + 0.75 / sz;
    vec2 hi = (cell + 1.0) * 0.5 - 0.75 / sz;
    vec2 st = uv * sz - 0.5;
    vec2 i = floor(st);
    vec2 f = st - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 / 6.0) * (-f3 + 3.0 * f2 - 3.0 * f + 1.0);
    vec2 w1 = (1.0 / 6.0) * (3.0 * f3 - 6.0 * f2 + 4.0);
    vec2 w2 = (1.0 / 6.0) * (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0);
    vec2 w3 = (1.0 / 6.0) * f3;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 h0 = (i - 1.0 + w1 / g0 + 0.5) / sz;
    vec2 h1 = (i + 1.0 + w3 / g1 + 0.5) / sz;
    h0 = clamp(h0, lo, hi);
    h1 = clamp(h1, lo, hi);
    float fl = float(L);
    vec4 a = textureLod(texSample, vec2(h0.x, h0.y), fl);
    vec4 b = textureLod(texSample, vec2(h1.x, h0.y), fl);
    vec4 c = textureLod(texSample, vec2(h0.x, h1.y), fl);
    vec4 d = textureLod(texSample, vec2(h1.x, h1.y), fl);
    return g0.y * (g0.x * a + g1.x * b) + g1.y * (g0.x * c + g1.x * d);
}

float classWeight(vec4 a, vec4 b, int i)
{
    return i < 4 ? a[i] : b[i - 4];
}

void main()
{
    vec4 vc = Flat ? flatColor : smoothColor;
    vec2 uv = texCoords;

    // ---- macro: the original atlas
    float qlod = textureQueryLod(texSample, uv).y;
    vec4 hw = texture(texSample, uv);              // exactly the classic sample
    vec4 macro = hw;
    float tm = clamp(qlod - MacroLod, 0.0, 1.0);    // 0 = magnified past the floor, 1 = minified
    if (tm < 1.0)
        macro = mix(macroCubic(uv, max(MacroLod, 0.0)), hw, tm);
    if (ATest && hw.w * vc.w <= 0.0)
        discard;

    // ---- geometry
    vec3 dpx = dFdx(wPos), dpy = dFdy(wPos);
    vec3 Ng = normalize(cross(dpx, dpy));
    if (dot(Ng, CamPos - wPos) < 0.0)
        Ng = -Ng;
    vec3 an = abs(Ng);
    vec2 p;
    vec3 U, V;
    if (an.y >= max(an.x, an.z)) { p = wPos.xz; U = vec3(1, 0, 0); V = vec3(0, 0, 1); }
    else if (an.x >= an.z)       { p = wPos.zy; U = vec3(0, 0, 1); V = vec3(0, 1, 0); }
    else                         { p = wPos.xy; U = vec3(1, 0, 0); V = vec3(0, 1, 0); }
    vec2 uv1 = p * Tile.x;
    mat2 R = mat2(Tile.z, Tile.w, -Tile.w, Tile.z);
    vec2 uv2 = R * (p * Tile.y) + vec2(0.37, 0.71);

    // ---- classes: top two of the atlas mask
    vec4 ma = texture(maskA, uv);
    vec4 mb = texture(maskB, uv);
    int c1 = 0, c2 = 0;
    float w1 = -1.0, w2 = -1.0;
    for (int i = 0; i < Layers; ++i)
    {
        float w = classWeight(ma, mb, i);
        if (w > w1) { w2 = w1; c2 = c1; w1 = w; c1 = i; }
        else if (w > w2) { w2 = w; c2 = i; }
    }
    w2 = max(w2, 0.0);

    vec4 a1 = texture(detAlb, vec3(uv1, float(c1)));
    vec4 a1c = texture(detAlb, vec3(uv2, float(c1)));
    vec4 n1 = texture(detNrm, vec3(uv1, float(c1)));
    vec4 n1c = texture(detNrm, vec3(uv2, float(c1)));
    float b1 = 1.0, b2 = 0.0;
    vec4 a2 = a1, a2c = a1c, n2 = n1, n2c = n1c;
    if (w2 > 0.02)
    {
        a2 = texture(detAlb, vec3(uv1, float(c2)));
        a2c = texture(detAlb, vec3(uv2, float(c2)));
        n2 = texture(detNrm, vec3(uv1, float(c2)));
        n2c = texture(detNrm, vec3(uv2, float(c2)));
        // height-aware blend: the higher material wins near the boundary
        float h1 = 0.6 * a1.a + 0.4 * a1c.a + w1 * 1.6;
        float h2 = 0.6 * a2.a + 0.4 * a2c.a + w2 * 1.6;
        float m = max(h1, h2) - 0.25;
        b1 = max(h1 - m, 0.0);
        b2 = max(h2 - m, 0.0);
        float s = b1 + b2;
        b1 /= s;
        b2 /= s;
    }
    // albedo multiplier (mean 1): fine x partially the coarse octave
    vec3 m1 = (a1.rgb * 2.0) * mix(vec3(1.0), a1c.rgb * 2.0, 0.6);
    vec3 m2 = (a2.rgb * 2.0) * mix(vec3(1.0), a2c.rgb * 2.0, 0.6);
    vec3 mdet = m1 * b1 + m2 * b2;
    // tangent-space normal: fine + coarse (rotated back into p space)
    mat2 Rt = mat2(Tile.z, -Tile.w, Tile.w, Tile.z);
    vec2 t1 = (n1.xy * 2.0 - 1.0) + 0.6 * (Rt * (n1c.xy * 2.0 - 1.0));
    vec2 t2 = (n2.xy * 2.0 - 1.0) + 0.6 * (Rt * (n2c.xy * 2.0 - 1.0));
    vec2 txy = t1 * b1 + t2 * b2;
    float rough = mix(n1.z, n1c.z, 0.3) * b1 + mix(n2.z, n2c.z, 0.3) * b2;
    float metal = n1.w * b1 + n2.w * b2;
    float spec = ClassSpec[c1 / 4][c1 % 4] * b1 + ClassSpec[c2 / 4][c2 % 4] * b2;

    float fade = 1.0 - smoothstep(Fade.x, Fade.y, vDepth);
    txy *= Strength.y * fade;
    vec3 Uo = normalize(U - Ng * dot(U, Ng));
    vec3 Vo = normalize(V - Ng * dot(V, Ng));
    vec3 N = normalize(Ng + Uo * txy.x + Vo * txy.y);

    // ---- colour
    vec3 base = toLin(macro.rgb * vc.rgb);
    vec3 var = vec3(1.0);
    if (MacroVar > 0.0)
    {
        vec3 nz = texture(macroNoise, wPos.xz * (1.0 / 4800.0)).rgb;
        float v = (nz.r - 0.5) * 1.1 + (nz.g - 0.5) * 0.6 + (nz.b - 0.5) * 0.3;
        var = vec3(1.0 + MacroVar * 2.0 * v);
    }
    vec3 albedo = base * mix(vec3(1.0), mdet, Strength.x) * var;

    // ---- light: V2 sun as a ratio over the face (the baked shade stays the floor)
    vec3 up = vec3(0.0, -1.0, 0.0); // UA world y is DOWN
    float lg = Light.x * max(dot(Ng, SunW), 0.0) + Light.y + Light.z * dot(Ng, up);
    float ld = Light.x * max(dot(N, SunW), 0.0) + Light.y + Light.z * dot(N, up);
    float ratio = clamp(ld / max(lg, 1e-3), 0.25, 2.5);
    vec3 col = albedo * ratio;

    float specAmt = Strength.z * spec * fade;
    if (specAmt > 0.0)
    {
        vec3 Vd = normalize(CamPos - wPos);
        vec3 H = normalize(SunW + Vd);
        float NdL = max(dot(N, SunW), 0.0);
        float NdV = max(dot(N, Vd), 1e-4);
        float NdH = max(dot(N, H), 0.0);
        float VdH = max(dot(Vd, H), 0.0);
        float r = clamp(rough, 0.08, 1.0);
        float a2 = r * r * r * r;
        float dd = NdH * NdH * (a2 - 1.0) + 1.0;
        float D = a2 / (PI * dd * dd);
        float k = (r + 1.0) * (r + 1.0) / 8.0;
        float G = (NdL / (NdL * (1.0 - k) + k)) * (NdV / (NdV * (1.0 - k) + k));
        vec3 F0 = mix(vec3(0.04), clamp(base * 2.0, 0.0, 1.0), metal);
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);
        float shade = dot(vc.rgb, vec3(0.333));
        col += D * G * F / max(4.0 * NdL * NdV, 1e-4) * Light.x * NdL * specAmt * shade;
    }

    if (Debug != 0)
    {
        vec3 dbg = col;
        if (Debug == 1)
        {
            const vec3 cc[8] = vec3[8](vec3(0.47, 0.35, 0.24), vec3(0.43, 0.59, 0.2), vec3(0.16, 0.16, 0.2),
                                       vec3(0.8, 0.8, 0.82), vec3(0.04), vec3(0.3, 0.55, 0.86),
                                       vec3(0.8, 0.47, 0.24), vec3(1, 0, 1));
            dbg = toLin(cc[c1] * b1 + cc[c2] * b2);
        }
        else if (Debug == 2) dbg = toLin(macro.rgb);
        else if (Debug == 3) dbg = toLin(clamp(mdet * 0.5, 0.0, 1.0));
        else if (Debug == 4) dbg = vec3(ratio * 0.5);
        else if (Debug == 5) dbg = base * ratio * var;
        fragColor = vec4(toSrgb(dbg) * fogMul, hw.w * vc.w);
        return;
    }
    fragColor = vec4(toSrgb(col) * fogMul, hw.w * vc.w);
}
)GLSL";

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------
namespace
{
struct GroundPack
{
    GLuint detAlb = 0, detNrm = 0, noise = 0;
    int layers = 0;
    float tileFine = 48.0f, tileCoarse = 209.0f;
    float classSpec[8] = {0};
    std::map<int, std::pair<GLuint, GLuint>> masks; // set * 16 + atlas -> (A, B)
    uint32_t bytes = 0;
};
// Heap, never destroyed: GFXEngine is a static whose destructor runs Deinit() ->
// GroundDeinit() during exit(), after this file's statics may already be gone
// (2026-10-15 crash on quit with UA_R2_GROUND=1: __tree_min in GroundDeinit).
GroundPack &gPack = *new GroundPack();

struct GndLoc
{
    int32_t CamRotInv = -1, CamPos = -1, SunW = -1, Light = -1, Tile = -1, Strength = -1, MacroVar = -1;
    int32_t MacroLod = -1, Fade = -1, ClassSpec = -1, Layers = -1, Debug = -1;
} gLoc;

float gStrength[3] = {1.8f, 1.7f, 1.2f}; // albedo, normal, specular: the "strong" column of the look comparison
float gVar = 0.06f;
float gMacroBias = -1.0f; // floor = original 256 px level - 1 (the 512 level of a 1024 HD atlas): ON keeps the New Look HD sharpness at range (2026-10-15; +0.5 read softer than OFF at commander height)
float gFade[2] = {900.0f, 3200.0f};
int gDebug = 0;
int gLog = 0;

typedef void (APIENTRY *PfnTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);

GLuint UploadArray(const std::string &path, int *layersOut, float aniso, uint32_t *bytes)
{
    static PfnTexImage3D texImage3D = (PfnTexImage3D)SDL_GL_GetProcAddress("glTexImage3D");
    if (!texImage3D)
        return 0;
    SDL_Surface *s = IMG_Load(path.c_str());
    if (!s)
    {
        printf("UA_R2_GROUND: cannot load %s\n", path.c_str());
        return 0;
    }
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c)
        return 0;
    const int w = c->w;
    const int n = c->h / w;
    if (n < 1 || c->h != n * w)
    {
        printf("UA_R2_GROUND: %s is not a vertical strip of square layers (%dx%d)\n", path.c_str(), c->w, c->h);
        SDL_FreeSurface(c);
        return 0;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    SDL_LockSurface(c);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, c->pitch / 4);
    texImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, w, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    SDL_UnlockSurface(c);
    SDL_FreeSurface(c);
    if (Glext::GLGenerateMipmap)
        Glext::GLGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, Glext::GLGenerateMipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (aniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    *layersOut = n;
    *bytes += (uint32_t)Perf::RgbaBytes(w, w, true) * n;
    return t;
}

// 2D texture from a sub-rectangle (rows y0..y0+h) of a surface.
GLuint Upload2D(SDL_Surface *c, int y0, int h, bool mips, bool repeat, uint32_t *bytes)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    SDL_LockSurface(c);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, c->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c->w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t *)c->pixels + (size_t)y0 * c->pitch);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    SDL_UnlockSurface(c);
    if (mips && Glext::GLGenerateMipmap)
        Glext::GLGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (mips && Glext::GLGenerateMipmap) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    *bytes += (uint32_t)Perf::RgbaBytes(c->w, h, mips);
    return t;
}

SDL_Surface *LoadRgba(const std::string &path)
{
    SDL_Surface *s = IMG_Load(path.c_str());
    if (!s)
        return NULL;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    return c;
}

bool Inverse3(const mat3x3 &m, float out[9])
{
    double a = m.m00, b = m.m01, c = m.m02, d = m.m10, e = m.m11, f = m.m12, g = m.m20, h = m.m21, i = m.m22;
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (fabs(det) < 1e-12)
        return false;
    double id = 1.0 / det;
    // row-major inverse
    out[0] = A * id; out[1] = -(b * i - c * h) * id; out[2] = (b * f - c * e) * id;
    out[3] = B * id; out[4] = (a * i - c * g) * id;  out[5] = -(a * f - c * d) * id;
    out[6] = C * id; out[7] = -(a * h - b * g) * id; out[8] = (a * e - b * d) * id;
    return true;
}
}

// Graphics settings (FLOOR level, gfxsettings.cpp; UA_R2_GROUND=n maps onto it):
// called at engine init and at every level load. Off -> on builds the program and
// uploads the pack once; on -> off frees both (the classic draw path, nothing
// resident). The floor needs the V2 renderer (same sun/exposure calibration).
void GFXEngine::SetGround(bool on)
{
    static bool want = false;
    on = on && _renderV2 && _vbo;
    if (on == want)
        return;
    want = on;
    if (on)
        GroundInit();
    else if (_groundOn)
    {
        GroundDeinit();
        printf("UA_R2_GROUND: off (graphics settings)\n");
    }
}

void GFXEngine::GroundInit()
{
    if (!_vbo)
        return;

    if (const char *v = getenv("UA_R2_GROUND_DETAIL"))
        sscanf(v, "%f,%f,%f", &gStrength[0], &gStrength[1], &gStrength[2]);
    if (const char *v = getenv("UA_R2_GROUND_VAR"))
        gVar = atof(v);
    if (const char *v = getenv("UA_R2_GROUND_MACRO"))
        gMacroBias = atof(v);
    if (const char *v = getenv("UA_R2_GROUND_FADE"))
        sscanf(v, "%f,%f", &gFade[0], &gFade[1]);
    if (const char *v = getenv("UA_R2_GROUND_DEBUG"))
    {
        static const char *modes[] = {"", "class", "macro", "detail", "ratio", "nodetail"};
        for (int i = 1; i < 6; ++i)
            if (!strcmp(v, modes[i]))
                gDebug = i;
    }
    if (const char *v = getenv("UA_R2_GROUND_LOG"))
        gLog = atoi(v);

    std::string dir = "reforge-ground";
    if (const char *v = getenv("UA_R2_GROUND_DIR"))
        if (v[0])
            dir = v;

    // pack.txt
    FILE *f = fopen((dir + "/pack.txt").c_str(), "r");
    if (!f)
    {
        printf("UA_R2_GROUND: no pack at %s (pack.txt missing) - ground stays classic\n", dir.c_str());
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        float v;
        int ci;
        char name[64];
        float r, m, s, d, n;
        if (sscanf(line, "tile_fine %f", &v) == 1) gPack.tileFine = v;
        else if (sscanf(line, "tile_coarse %f", &v) == 1) gPack.tileCoarse = v;
        else if (sscanf(line, "class %d %63s rough %f metal %f spec %f detail %f normal %f", &ci, name, &r, &m, &s, &d, &n) == 7
                 && ci >= 0 && ci < 8)
            gPack.classSpec[ci] = s;
    }
    fclose(f);

    Perf::Scope perfTex(Perf::TEX);
    uint32_t bytes = 0;
    int la = 0, ln = 0;
    gPack.detAlb = UploadArray(dir + "/common/detail_albedo.png", &la, _maxAniso, &bytes);
    gPack.detNrm = UploadArray(dir + "/common/detail_normal.png", &ln, _maxAniso, &bytes);
    if (SDL_Surface *c = LoadRgba(dir + "/common/macro_noise.png"))
    {
        gPack.noise = Upload2D(c, 0, c->h, true, true, &bytes);
        SDL_FreeSurface(c);
    }
    if (!gPack.detAlb || !gPack.detNrm || la != ln || !gPack.noise)
    {
        printf("UA_R2_GROUND: pack %s incomplete - ground stays classic\n", dir.c_str());
        GroundDeinit();
        return;
    }
    gPack.layers = std::min(la, 8);

    int nmask = 0;
    for (int set = 1; set <= 6; ++set)
        for (int k = 1; k <= 5; ++k)
        {
            SDL_Surface *c = LoadRgba(fmt::sprintf("%s/set%d/boden%d_mask.png", dir, set, k));
            if (!c)
                continue;
            if (c->h == c->w * 2)
            {
                GLuint a = Upload2D(c, 0, c->w, false, true, &bytes);
                GLuint b = Upload2D(c, c->w, c->w, false, true, &bytes);
                gPack.masks[set * 16 + k] = std::make_pair(a, b);
                nmask++;
            }
            SDL_FreeSurface(c);
        }
    glBindTexture(GL_TEXTURE_2D, 0);
    _lastStates.Tex = 0; // we touched unit 0's binding
    gPack.bytes = bytes;
    Perf::TexAlloc(true, bytes);

    if (!GroundBuildProgram())
    {
        GroundDeinit();
        return;
    }

    _groundOn = true;
    printf("UA_R2_GROUND: active, pack %s: %d layers, %d masks, %.1f MB GPU; detail %.2f/%.2f/%.2f var %.3f "
           "macro bias %.2f fade %.0f..%.0f debug %d\n", dir.c_str(), gPack.layers, nmask, bytes / 1048576.0,
           gStrength[0], gStrength[1], gStrength[2], gVar, gMacroBias, gFade[0], gFade[1], gDebug);
}

// The ground program (also rebuilt by AtmosRefresh when the ATMOSPHERE variant text is needed or no longer is;
// _gndAtmos picks the text, 0 = the original).
static unsigned gGroundProgGen = 0; // bumped per new ground program: the uniform value caches in RenderingMeshGroundSetup restart

bool GFXEngine::GroundBuildProgram()
{
    gGroundProgGen++;
    _gndPs = CompileShader(GL_FRAGMENT_SHADER, ShadowGroundPS(_gndAtmos ? AtmosVariant(_gndPShaderText, 4) : _gndPShaderText));
    _gndVs = CompileShader(GL_VERTEX_SHADER, ShadowGroundVS(_gndAtmos ? AtmosVariant(_gndVShaderText, 3) : _gndVShaderText));
    uint32_t id = Glext::GLCreateProgram();
    Glext::GLAttachShader(id, _gndPs);
    Glext::GLAttachShader(id, _gndVs);
    { Perf::Scope perfShd(Perf::SHD); Glext::GLLinkProgram(id); }
    GLint linked = 0;
    Glext::GLGetProgramiv(id, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char logbuf[4096] = {0};
        Glext::GLGetProgramInfoLog(id, sizeof(logbuf) - 1, NULL, logbuf);
        printf("UA_R2_GROUND: program link failed - ground stays classic:\n%s\n", logbuf);
        Glext::GLDeleteProgram(id);
        return false;
    }
    _gndShaderProg = TShaderProg(id);
    BindVBOParameters(_gndShaderProg);
    ShadowProgramLinked(id); // shadow sampler unit (level 0: no call)
    gLoc.CamRotInv = Glext::GLGetUniformLocation(id, "CamRotInv");
    gLoc.CamPos = Glext::GLGetUniformLocation(id, "CamPos");
    gLoc.SunW = Glext::GLGetUniformLocation(id, "SunW");
    gLoc.Light = Glext::GLGetUniformLocation(id, "Light");
    gLoc.Tile = Glext::GLGetUniformLocation(id, "Tile");
    gLoc.Strength = Glext::GLGetUniformLocation(id, "Strength");
    gLoc.MacroVar = Glext::GLGetUniformLocation(id, "MacroVar");
    gLoc.MacroLod = Glext::GLGetUniformLocation(id, "MacroLod");
    gLoc.Fade = Glext::GLGetUniformLocation(id, "Fade");
    gLoc.ClassSpec = Glext::GLGetUniformLocation(id, "ClassSpec");
    gLoc.Layers = Glext::GLGetUniformLocation(id, "Layers");
    gLoc.Debug = Glext::GLGetUniformLocation(id, "Debug");

    Glext::GLUseProgram(id);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "texSample"), 0);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "maskA"), 1);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "maskB"), 2);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "detAlb"), 3);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "detNrm"), 4);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(id, "macroNoise"), 5);
    float sun[3], sl, al, hh;
    V2SunParams(sun, &sl, &al, &hh);
    Glext::GLUniform3f(gLoc.SunW, sun[0], sun[1], sun[2]);
    Glext::GLUniform3f(gLoc.Light, sl, al, hh);
    const float rot = 0.6283f;
    Glext::GLUniform4f(gLoc.Tile, 1.0f / gPack.tileFine, 1.0f / gPack.tileCoarse, cosf(rot), sinf(rot));
    Glext::GLUniform3f(gLoc.Strength, gStrength[0], gStrength[1], gStrength[2]);
    Glext::GLUniform1f(gLoc.MacroVar, gVar);
    Glext::GLUniform2f(gLoc.Fade, gFade[0], gFade[1]);
    Glext::GLUniform4f(Glext::GLGetUniformLocation(id, "ClassSpec[0]"), gPack.classSpec[0], gPack.classSpec[1],
                       gPack.classSpec[2], gPack.classSpec[3]);
    Glext::GLUniform4f(Glext::GLGetUniformLocation(id, "ClassSpec[1]"), gPack.classSpec[4], gPack.classSpec[5],
                       gPack.classSpec[6], gPack.classSpec[7]);
    Glext::GLUniform1i(gLoc.Layers, gPack.layers);
    Glext::GLUniform1i(gLoc.Debug, gDebug);
    if (_gndAtmos)
        AtmosGroundUniforms(id);
    Glext::GLUseProgram(_lastStates.Prog.ID);
    return true;
}

void GFXEngine::GroundDeinit()
{
    for (auto &kv : gPack.masks)
    {
        glDeleteTextures(1, &kv.second.first);
        glDeleteTextures(1, &kv.second.second);
    }
    gPack.masks.clear();
    GLuint t[3] = {gPack.detAlb, gPack.detNrm, gPack.noise};
    for (GLuint x : t)
        if (x)
            glDeleteTextures(1, &x);
    gPack.detAlb = gPack.detNrm = gPack.noise = 0;
    if (_gndShaderProg.ID)
        Glext::GLDeleteProgram(_gndShaderProg.ID);
    if (_gndVs)
        Glext::GLDeleteShader(_gndVs);
    if (_gndPs)
        Glext::GLDeleteShader(_gndPs);
    _gndShaderProg = TShaderProg();
    _gndVs = _gndPs = 0;
    _groundOn = false;
}

bool GFXEngine::GroundUse(const TRenderNode *nod) const
{
    const uint32_t flags = nod->Flags;
    if (gLog > 0 && nod->Tex && nod->Tex->groundAtlas)
    {
        static int n = 0;
        if (n++ < gLog)
            printf("UA_R2_GROUND: use? set %d boden%d flags %x hwTex %u key %d\n", nod->Tex->groundSet,
                   nod->Tex->groundAtlas, flags, (unsigned)nod->Tex->hwTex,
                   (int)gPack.masks.count(nod->Tex->groundSet * 16 + nod->Tex->groundAtlas));
    }
    if (!(flags & RFLAGS_TEXTURED) || (flags & (RFLAGS_SKY | RFLAGS_LUMTRACY | RFLAGS_ZEROTRACY)))
        return false;
    const ResBitmap *t = nod->Tex;
    if (!t || !t->groundAtlas || !t->hwTex)
        return false;
    return gPack.masks.count(t->groundSet * 16 + t->groundAtlas) != 0;
}

void GFXEngine::RenderingMeshGroundSetup(TRenderNode *nod)
{
    const ResBitmap *t = nod->Tex;
    const std::pair<GLuint, GLuint> &mk = gPack.masks[t->groundSet * 16 + t->groundAtlas];

    // camera: world = inverse(view SclRot) * viewPos + view pos
    static float lastInv[9] = {0};
    static float lastPos[3] = {1e30f, 0, 0};
    static unsigned seenGen = 0;
    static int lastW = -1;
    if (seenGen != gGroundProgGen) // a fresh program has none of the cached values: upload them again
    {
        seenGen = gGroundProgGen;
        memset(lastInv, 0, sizeof(lastInv));
        lastPos[0] = 1e30f; lastPos[1] = lastPos[2] = 0;
        lastW = -1;
    }
    if (TF::TForm3D *view = TF::Engine.GetViewPoint())
    {
        float inv[9];
        if (Inverse3(view->CalcSclRot, inv) && memcmp(inv, lastInv, sizeof(inv)) != 0)
        {
            memcpy(lastInv, inv, sizeof(inv));
            Glext::GLUniformMatrix3fv(gLoc.CamRotInv, 1, GL_TRUE, inv);
        }
        float pos[3] = {(float)view->CalcPos.x, (float)view->CalcPos.y, (float)view->CalcPos.z};
        if (memcmp(pos, lastPos, sizeof(pos)) != 0)
        {
            memcpy(lastPos, pos, sizeof(pos));
            Glext::GLUniform3f(gLoc.CamPos, pos[0], pos[1], pos[2]);
        }
    }
    // macro floor: the original 256 px level (+ bias), whatever the bound size (HD pack or not)
    int texW = (t->hdTex ? t->hdTex->w : t->width);
    if (texW != lastW)
    {
        lastW = texW;
        float lod = log2f(std::max(1.0f, texW / 256.0f)) + gMacroBias;
        if (!t->hwTexMips)
            lod = 0.0f;
        Glext::GLUniform1f(gLoc.MacroLod, lod);
    }

    Glext::GLActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mk.first);
    Glext::GLActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mk.second);
    Glext::GLActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, gPack.detAlb);
    Glext::GLActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, gPack.detNrm);
    Glext::GLActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, gPack.noise);
    ShadowGroundDraw(); // shadows (no GL call at level 0)
    Glext::GLActiveTexture(GL_TEXTURE0);

    if (gLog > 0)
    {
        static int logged = 0;
        if (logged < gLog && nod->Mesh)
        {
            float u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
            for (const TVertex &v : nod->Mesh->Vertexes)
            {
                u0 = std::min(u0, v.TexCoord.tu); u1 = std::max(u1, v.TexCoord.tu);
                v0 = std::min(v0, v.TexCoord.tv); v1 = std::max(v1, v.TexCoord.tv);
            }
            printf("UA_R2_GROUND: draw set %d boden%d tex %dx%d mips %d verts %zu uv [%.4f..%.4f]x[%.4f..%.4f] flags %x\n",
                   t->groundSet, t->groundAtlas, texW, t->hdTex ? t->hdTex->h : t->height, t->hwTexMips ? 1 : 0,
                   nod->Mesh->Vertexes.size(), u0, u1, v0, v1, nod->Flags);
            logged++;
        }
    }
}

}
