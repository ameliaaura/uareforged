// Reforge sun shadows (SHADOWS). Level from gfxLevel("SHADOWS")
// = env UA_GFX_SHADOWS=0..3 until the settings helper is merged.
//
// Level 0 (default): nothing here runs. No program is compiled, no texture or
// framebuffer is allocated, the V2 and legacy shader texts are the original
// ones and the draw path takes no new branch that changes GL state, so the
// frame is byte-identical to the build without this file.
//
// Level 1..3: one directional shadow map from the V2 sun (UA_R2_SUN), drawn
// once per world frame (yw_game RenderGame only - briefing/vehicle viewers and
// the asset viewer never get shadows) from the queued solid render list:
//   - casters: every solid mesh except the ground atlas polygons (boden1-5,
//     sectors + fillers), sky, additive/lumtracy, fx-alpha, sky-falloff and
//     no-zwrite draws, and particles. Units, host stations, buildings, sector
//     statics and wrecks cast.
//   - receivers: every solid + colour-key draw of the world frame. V2 (PBR)
//     meshes lose the sun terms in shadow (diffuse + specular, ambient stays);
//     legacy meshes (ground, original buildings) are darkened by the ratio the
//     V2 sun model gives that face (its own n.l), so both kinds of surface agree
//     on how dark a shadow is.
//   - cascades are fixed-size squares in a world-anchored light frame whose
//     centre is snapped to whole texels: the map never rotates or rescales
//     with the camera, so shadow edges do not crawl or flicker when it moves.
//   - depth bias: slope-scaled polygon offset in the caster pass + a normal
//     offset of ~1 texel at lookup (geometric normal from screen derivatives)
//     + a small constant compare bias.
//   - distance fade by view depth, and an edge fade inside the last cascade:
//     at range the frame is the unshadowed original read. V2 meshes also
//     drop their own shadow by the far-v2 fade (units at far-read size look
//     exactly as before).
//
//                 cascades  map (each)   cover (half-size, world)   fade (view z)   filter
//   1 low         1         1024         700                        900..1150       4 taps (soft, 2x2 at +-0.5 texel)
//   2 medium      2         1024         300 / 1050                 1300..1700      4 taps (soft)
//   3 high        2         2048         360 / 1300                 1900..2300      16 taps (soft, 4x4 at +-0.5/1.5)
//   Every level has the soft edge; host stations (and their guns) cast a lighter shadow
//   (UA_GFX_SHADOWS_HOST, default 0.5 of a normal shadow) by an ordered 2x2 dither in the caster pass -
//   the half-texel filter grids above average a 2x2 pattern to exactly that fraction (no pattern visible).
//
// Env (look-dev; all optional):
//   UA_GFX_SHADOWS=0..3            level
//   UA_GFX_SHADOWS_STRENGTH=s      fraction of the sun a shadow removes (default 0.8)
//   UA_GFX_SHADOWS_HOST=h          host-station shadow as a fraction of a normal one: 0, .25, .5 (default), .75, 1
//   UA_GFX_SHADOWS_LOG=1           print the cascade set-up and caster counts
//   UA_GFX_SHADOWS_DEBUG=1         paint cascade 0 red / 1 green on receivers (tuning)
//
// GLES/Quest: everything here is GLES 3.0 core (depth texture + compare
// mode, sampler2DShadow, one FBO, no texture arrays, no geometry shaders).
// A Quest build would use a 16-bit depth map, level 1 sizes and one view
// (multiview would draw the map once for both eyes).

#include "../gfxsettings.h"
#include <SDL2/SDL_opengl.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/glext.h>
#else
#include <GL/glext.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx.h"
#include "glfuncs.h"
#include "tform.h"
#include "perf.h"

#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif


namespace GFX
{

static const int kShadowUnit = 7; // texture unit of the shadow map (V2 uses 1-3, the floor 1-5)

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------
static const char *kDepthVS = R"GLSL(#version 410 core
uniform mat4 ShadowMVP;
layout(location = 0) in vec3 vPos;
void main()
{
    gl_Position = ShadowMVP * vec4(vPos, 1.0);
}
)GLSL";

static const char *kDepthPS = R"GLSL(#version 410 core
void main()
{
}
)GLSL";

// Lighter casters (host stations): keep the caster in ShadowKeep of every 2x2 map texels (ordered dither,
// world-anchored like the map itself). The soft filters below average it to ShadowKeep/4 of a full shadow.
static const char *kDepthDitherPS = R"GLSL(#version 410 core
uniform int ShadowKeep;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) & 1;
    int idx = (p.x == p.y) ? p.x : 2 + p.y;
    if (idx >= ShadowKeep)
        discard;
}
)GLSL";

// Shared lookup (fragment). w = camera-relative world position.
static const char *kShadowLib = R"GLSL(
uniform sampler2DShadow ShadowMap;
uniform mat3 ShadowCamRotInv; // view -> camera-relative world (inverse of the view SclRot)
uniform mat4 ShadowMat[2];    // camera-relative world -> (atlas u, v, depth)
uniform vec4 ShadowCasc[2];   // x normal offset (world), y compare bias (depth units), z,w atlas u range
uniform vec4 ShadowFade;      // x,y fade over view z, z cascade count, w filter taps (1, 4, 9)
uniform vec3 ShadowSun;       // camera-relative world, towards the sun
uniform vec4 ShadowLook;      // x V2 sun cut (strength), y legacy sun/ambient ratio, z debug, w texel (1/atlas h)
uniform float ShadowOn;       // 0 = this draw receives no shadow; V2: 1 - far-v2 fade
float uaShadowPcf(vec3 c)
{
    // half-texel grids of hw-PCF taps (soft; a 2x2 dithered caster averages out exactly)
    vec2 t = vec2(ShadowLook.w / ShadowFade.z, ShadowLook.w);
    if (ShadowFade.w < 5.0)
    {
        vec2 o = t * 0.5;
        return 0.25 * (texture(ShadowMap, c + vec3(-o.x, -o.y, 0.0)) + texture(ShadowMap, c + vec3(o.x, -o.y, 0.0)) +
                       texture(ShadowMap, c + vec3(-o.x, o.y, 0.0)) + texture(ShadowMap, c + vec3(o.x, o.y, 0.0)));
    }
    float s = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s += texture(ShadowMap, c + vec3((vec2(float(x), float(y)) - 1.5) * t, 0.0));
    return s / 16.0;
}
// Visibility of the sun, 1 = lit. ndl = geometric N.L (camera-facing normal).
float uaShadow(vec3 vpos, out float ndl, out vec3 dbg)
{
    vec3 w = ShadowCamRotInv * vpos;
    vec3 ng = cross(dFdx(w), dFdy(w));
    float nl = length(ng);
    ng = nl > 1e-12 ? ng / nl : vec3(0.0, -1.0, 0.0);
    if (dot(ng, w) > 0.0)
        ng = -ng;
    ndl = dot(ng, ShadowSun);
    dbg = vec3(1.0);
    if (ShadowOn <= 0.0)
        return 1.0;
    float fade = 1.0 - smoothstep(ShadowFade.x, ShadowFade.y, vpos.z);
    if (fade <= 0.0)
        return 1.0;
    int n = int(ShadowFade.z);
    for (int i = 0; i < 2; i++)
    {
        if (i >= n)
            break;
        vec3 p = w + ng * ShadowCasc[i].x;
        vec3 c = (ShadowMat[i] * vec4(p, 1.0)).xyz;
        float uw = ShadowCasc[i].w - ShadowCasc[i].z;
        float e = min(min(c.x - ShadowCasc[i].z, ShadowCasc[i].w - c.x) / uw, min(c.y, 1.0 - c.y));
        if (e < 0.02 || c.z >= 1.0)
            continue;
        float edge = (i == n - 1) ? smoothstep(0.02, 0.12, e) : 1.0;
        float s = uaShadowPcf(vec3(c.xy, c.z - ShadowCasc[i].y));
        if (ShadowLook.z > 0.0)
            dbg = i == 0 ? vec3(1.0, 0.6, 0.6) : vec3(0.6, 1.0, 0.6);
        return mix(1.0, s, fade * edge * ShadowOn);
    }
    return 1.0;
}
)GLSL";

// Legacy (original .bas) receiver: the std shader plus the lookup. Must stay
// in step with GFXEngine::_stdVShaderText/_stdPShaderText (stdshdr.cpp):
// unshadowed pixels are the std shader's exactly.
static const char *kStdShdVS = R"GLSL(#version 410 core
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
in vec3 vPos;
in vec4 vColor;
in vec2 vUV;
out vec4 smoothColor;
flat out vec4 flatColor;
out vec2 texCoords;
out vec3 vsPos;
void main()
{
    vec4 tformed = MView * vec4(vPos, 1.0);
    gl_Position = MProj * tformed;
    vsPos = tformed.xyz;

    vec4 clr;
    if (Fog.x != 0.0)
    {
        float fg = clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);
        clr = vec4(mix(vColor.xyz, vec3(0.0), fg), vColor.w);
    }
    else
    {
        clr = vColor;
    }

    if (AlphaFog.x != 0.0)
    {
        clr.w *= 1.0 - clamp((tformed.z - AlphaFog.y) / AlphaFog.z, 0.0, 1.0);
    }
    flatColor = clr;
    smoothColor = clr;
    if (Textured) texCoords = vUV;
}
)GLSL";

static const char *kStdShdPSHead = R"GLSL(#version 410 core
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
in vec4 smoothColor;
flat in vec4 flatColor;
in vec2 texCoords;
in vec3 vsPos;
out vec4 fragColor;
)GLSL";

static const char *kStdShdPSMain = R"GLSL(
void main()
{
    float ndl;
    vec3 dbg;
    float s = uaShadow(vsPos, ndl, dbg);
    vec4 clr;
    if (Flat) clr = flatColor; else clr = smoothColor;
    if (Textured)
        clr = texture(texSample, texCoords) * clr;
    if (ATest && clr.w <= 0.0)
        discard;
    // the V2 sun model's ratio shadowed/lit for this face: (amb + sun*n.l*vis) / (amb + sun*n.l),
    // vis = 1 - strength*(1 - s), applied in display space. Faces turned away from the sun (n.l <= 0)
    // keep their baked shade exactly (no double dark); grazing faces get little.
    float nl = max(ndl, 0.0) * ShadowLook.y;
    float r = (1.0 + nl * (1.0 - ShadowLook.x * (1.0 - s))) / (1.0 + nl);
    clr.rgb *= pow(r, 1.0 / 2.2) * dbg;
    fragColor = clr;
}
)GLSL";

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
namespace
{
struct ShdLoc
{
    int32_t Map = -1, CamRotInv = -1, Mat = -1, Casc = -1, Fade = -1, Sun = -1, Look = -1, On = -1;
};

struct ShadowState
{
    int level = 0;
    int applied = -1;          // level the programs were built for (ShadowLevel; ShadowsApply at level load)
    int cascades = 0;
    int size = 0;              // per cascade (square)
    double half[2] = {0, 0};   // world half-size of each cascade square
    double fade0 = 0, fade1 = 0;
    int taps = 1;
    float strength = 0.8f;
    int hostKeep = 2;          // host-station casters: kept texels per 2x2 (0 none .. 4 = full shadow)
    bool debug = false;
    int log = 0;

    GLuint fbo = 0, tex = 0;
    GLuint depthProg = 0, depthVs = 0, depthPs = 0;
    int32_t depthMvpLoc = -1, depthPosLoc = -1;
    GLuint hostProg = 0, hostPs = 0;
    int32_t hostMvpLoc = -1;
    GLuint stdProg = 0, stdVs = 0, stdPs = 0;
    ShdLoc stdLoc, v2Loc, gndLoc;
    bool v2Injected = false;
    bool gndInjected = false;   // the floor program (ground.cpp) carries the lookup too
    bool atmInjected = false;   // the ATMOSPHERE legacy program (atmos_gl.cpp) too
    ShdLoc atmLoc;
    GLuint v2LocProg = 0, gndLocProg = 0, atmLocProg = 0; // program the locations were fetched from (built later)
    int gndOn = -1, atmOn = -1; // last ShadowOn pushed (these programs also draw outside the world frame)
    int64_t texBytes = 0;

    // per frame
    float camRotInv[9];
    float mat[2][16];
    float casc[2][4];
    float sunRel[3];
    float darkLegacy = 1.0f;
    float legacyK = 0.0f;
    uint32_t frames = 0;
};
ShadowState gS;

// row-major 4x4 helpers (double)
struct M4
{
    double m[16];
};
M4 Mul(const M4 &a, const M4 &b)
{
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
M4 FromTForm(const mat4x4 &t)
{
    M4 r = {{t.m00, t.m01, t.m02, t.m03, t.m10, t.m11, t.m12, t.m13,
             t.m20, t.m21, t.m22, t.m23, t.m30, t.m31, t.m32, t.m33}};
    return r;
}
void ToGL(const M4 &a, float out[16]) // column-major for glUniformMatrix4fv(transpose = false)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[j * 4 + i] = (float)a.m[i * 4 + j];
}
bool Inverse3(const mat3x3 &a, double r[9])
{
    const double m[9] = {a.m00, a.m01, a.m02, a.m10, a.m11, a.m12, a.m20, a.m21, a.m22};
    double c0 = m[4] * m[8] - m[5] * m[7];
    double c1 = m[5] * m[6] - m[3] * m[8];
    double c2 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (fabs(det) < 1e-12)
        return false;
    double id = 1.0 / det;
    r[0] = c0 * id;
    r[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    r[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    r[3] = c1 * id;
    r[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    r[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    r[6] = c2 * id;
    r[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    r[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}

GLuint LinkProgram(GLuint vs, GLuint ps, const char *what)
{
    if (!vs || !ps)
        return 0;
    GLuint p = Glext::GLCreateProgram();
    Glext::GLAttachShader(p, vs);
    Glext::GLAttachShader(p, ps);
    {
        Perf::Scope perfShd(Perf::SHD);
        Glext::GLLinkProgram(p);
    }
    GLint ok = 0;
    Glext::GLGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char logbuf[2048] = {0};
        Glext::GLGetProgramInfoLog(p, sizeof(logbuf) - 1, NULL, logbuf);
        printf("UA_GFX_SHADOWS: %s link failed:\n%s\n", what, logbuf);
        Glext::GLDeleteProgram(p);
        return 0;
    }
    return p;
}

void FetchLoc(GLuint prog, ShdLoc *l)
{
    l->Map = Glext::GLGetUniformLocation(prog, "ShadowMap");
    l->CamRotInv = Glext::GLGetUniformLocation(prog, "ShadowCamRotInv");
    l->Mat = Glext::GLGetUniformLocation(prog, "ShadowMat");
    l->Casc = Glext::GLGetUniformLocation(prog, "ShadowCasc");
    l->Fade = Glext::GLGetUniformLocation(prog, "ShadowFade");
    l->Sun = Glext::GLGetUniformLocation(prog, "ShadowSun");
    l->Look = Glext::GLGetUniformLocation(prog, "ShadowLook");
    l->On = Glext::GLGetUniformLocation(prog, "ShadowOn");
}

// Per-frame uniforms into one program (made current by the caller).
void PushFrame(const ShdLoc &l)
{
    Glext::GLUniform1i(l.Map, kShadowUnit);
    Glext::GLUniformMatrix3fv(l.CamRotInv, 1, GL_FALSE, gS.camRotInv);
    Glext::GLUniformMatrix4fv(l.Mat, 2, GL_FALSE, &gS.mat[0][0]);
    Glext::GLUniform4fv(l.Casc, 2, &gS.casc[0][0]);
    Glext::GLUniform4f(l.Fade, (float)gS.fade0, (float)gS.fade1, (float)gS.cascades, (float)gS.taps);
    Glext::GLUniform3f(l.Sun, gS.sunRel[0], gS.sunRel[1], gS.sunRel[2]);
    Glext::GLUniform4f(l.Look, gS.strength, gS.legacyK, gS.debug ? 1.0f : 0.0f, 1.0f / (float)gS.size);
}
} // namespace

// The level every shadow program text was (or is about to be) built for. First read at engine init (settings
// helper: preset row / override / UA_GFX_SHADOWS); ShadowsApply moves it at a level load.
int GFXEngine::ShadowLevel()
{
    if (gS.applied < 0)
        gS.applied = gfxLevel("SHADOWS", 0);
    return gS.applied;
}

// Graphics settings (GfxSettings::OnLevelLoad, after SetRenderV2/SetGround): follow a SHADOWS change made in the
// menu. Same level (the level-0 steady state included): returns, no GL call. Otherwise the old resources are
// freed and every program that carries the lookup (V2, floor, ATMOSPHERE legacy) is rebuilt from its plain text
// for the new level - at 0 that is the exact original text, so the off path is the engine-init level-0 path.
void GFXEngine::ShadowsApply()
{
    const int lv = gfxLevel("SHADOWS", 0);
    const int was = ShadowLevel();
    if (lv == was)
        return;
    ShadowsDeinit();
    gS.applied = lv;
    gS.level = 0;
    gS.v2Injected = gS.gndInjected = gS.atmInjected = false;
    gS.v2LocProg = gS.gndLocProg = gS.atmLocProg = 0;
    gS.gndOn = gS.atmOn = -1;
    _shadowWorld = _shadowFrame = false;
    if (_vbo)
    {
        if (_v2ShaderProg.ID)
            RebuildV2Program();
        if (_groundOn)
            RebuildGroundProgram();
        if (_atmStdProg.ID)
            AtmosFreeLegacyProgram(); // rebuilt by AtmosRefresh (this level load) when the atmosphere is on
    }
    ShadowsInit();
    printf("UA_GFX_SHADOWS: level %d -> %d (graphics settings)\n", was, lv);
}

// Called from Init with the V2 fragment text about to be compiled. Level 0:
// the text is returned untouched. Level > 0: the lookup is added and the two
// sun terms are multiplied by the visibility.
std::string GFXEngine::ShadowV2PS(const std::string &src)
{
    gS.v2Injected = false;
    if (ShadowLevel() <= 0)
        return src;
    std::string s = src;
    size_t mainPos = s.find("void main()");
    size_t ndlPos = s.find("float NdL = max(dot(N, L), 0.0);");
    int sunTerms = 0;
    for (size_t p = s.find("SunColor * NdL"); p != std::string::npos; p = s.find("SunColor * NdL", p + 1))
        sunTerms++;
    if (mainPos == std::string::npos || ndlPos == std::string::npos || sunTerms != 3)
    {
        printf("UA_GFX_SHADOWS: V2 shader anchors not found (main %d, NdL %d, sun terms %d): V2 meshes "
               "receive no shadow\n", mainPos != std::string::npos, ndlPos != std::string::npos, sunTerms);
        return src;
    }
    const std::string ndl = "float NdL = max(dot(N, L), 0.0);";
    s.replace(ndlPos, ndl.size(), ndl + "\n    float uaNdlG; vec3 uaDbg;\n"
              "    float uaSv = 1.0 - ShadowLook.x * (1.0 - uaShadow(vsPos, uaNdlG, uaDbg));");
    for (size_t p = s.find("SunColor * NdL"); p != std::string::npos; p = s.find("SunColor * NdL", p + 1))
    {
        s.replace(p, 14, "SunColor * (NdL * uaSv)");
        p += 10;
    }
    size_t dbg = s.find("lit *= Exposure;");
    if (dbg != std::string::npos)
        s.replace(dbg, 16, "lit *= Exposure * uaDbg;");
    s.insert(s.find("void main()"), std::string(kShadowLib) + "\n");
    gS.v2Injected = true;
    return s;
}

void GFXEngine::ShadowsInit()
{
    gS.level = ShadowLevel();
    if (gS.level <= 0)
        return;
    if (!_vbo || !_glext)
    {
        printf("UA_GFX_SHADOWS: needs the VBO/GLSL path, off\n");
        gS.level = 0;
        return;
    }
    switch (gS.level)
    {
    case 1:
        gS.cascades = 1; gS.size = 1024; gS.half[0] = 700.0; gS.fade0 = 900.0; gS.fade1 = 1150.0; gS.taps = 4;
        break;
    case 2:
        gS.cascades = 2; gS.size = 1024; gS.half[0] = 300.0; gS.half[1] = 1050.0; gS.fade0 = 1300.0; gS.fade1 = 1700.0;
        gS.taps = 4;
        break;
    default:
        gS.cascades = 2; gS.size = 2048; gS.half[0] = 360.0; gS.half[1] = 1300.0; gS.fade0 = 1900.0; gS.fade1 = 2300.0;
        gS.taps = 16;
        break;
    }
    if (const char *v = getenv("UA_GFX_SHADOWS_STRENGTH"))
        gS.strength = std::max(0.0f, std::min(1.0f, (float)atof(v)));
    if (const char *v = getenv("UA_GFX_SHADOWS_HOST"))
        gS.hostKeep = std::max(0, std::min(4, (int)lround(atof(v) * 4.0)));
    gS.debug = getenv("UA_GFX_SHADOWS_DEBUG") && atoi(getenv("UA_GFX_SHADOWS_DEBUG")) > 0;
    gS.log = getenv("UA_GFX_SHADOWS_LOG") ? atoi(getenv("UA_GFX_SHADOWS_LOG")) : 0;

    // depth-only caster program
    gS.depthVs = CompileShader(GL_VERTEX_SHADER, kDepthVS);
    gS.depthPs = CompileShader(GL_FRAGMENT_SHADER, kDepthPS);
    gS.depthProg = LinkProgram(gS.depthVs, gS.depthPs, "depth");
    if (gS.hostKeep > 0 && gS.hostKeep < 4)
    {
        gS.hostPs = CompileShader(GL_FRAGMENT_SHADER, kDepthDitherPS);
        gS.hostProg = LinkProgram(gS.depthVs, gS.hostPs, "depth host");
        if (gS.hostProg)
        {
            gS.hostMvpLoc = Glext::GLGetUniformLocation(gS.hostProg, "ShadowMVP");
            Glext::GLUseProgram(gS.hostProg);
            Glext::GLUniform1i(Glext::GLGetUniformLocation(gS.hostProg, "ShadowKeep"), gS.hostKeep);
            Glext::GLUseProgram(_lastStates.Prog.ID);
        }
        else
            gS.hostKeep = 4; // dither program failed: hosts cast a normal shadow
    }
    // legacy receiver program
    gS.stdVs = CompileShader(GL_VERTEX_SHADER, kStdShdVS);
    gS.stdPs = CompileShader(GL_FRAGMENT_SHADER, std::string(kStdShdPSHead) + kShadowLib + kStdShdPSMain);
    gS.stdProg = LinkProgram(gS.stdVs, gS.stdPs, "legacy receiver");
    if (!gS.depthProg || !gS.stdProg)
    {
        ShadowsDeinit();
        gS.level = 0;
        return;
    }
    gS.depthMvpLoc = Glext::GLGetUniformLocation(gS.depthProg, "ShadowMVP");
    gS.depthPosLoc = Glext::GLGetAttribLocation(gS.depthProg, "vPos");
    _shdStdProg = TShaderProg(gS.stdProg);
    BindVBOParameters(_shdStdProg);
    FetchLoc(gS.stdProg, &gS.stdLoc);
    if (gS.v2Injected && _v2ShaderProg.ID)
    {
        FetchLoc(_v2ShaderProg.ID, &gS.v2Loc);
        gS.v2LocProg = _v2ShaderProg.ID;
    }

    // depth atlas: cascades side by side
    const int W = gS.size * gS.cascades, H = gS.size;
    glGenTextures(1, &gS.tex);
    glBindTexture(GL_TEXTURE_2D, gS.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, W, H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, _lastStates.Tex);
    gS.texBytes = (int64_t)W * H * 4;
    Perf::TexAlloc(true, gS.texBytes);

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    Glext::GLGenFramebuffers(1, &gS.fbo);
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, gS.fbo);
    Glext::GLFrameBufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gS.tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    GLenum st = Glext::GLCheckFramebufferStatus(GL_FRAMEBUFFER);
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    if (st != GL_FRAMEBUFFER_COMPLETE)
    {
        printf("UA_GFX_SHADOWS: framebuffer incomplete (0x%x), off\n", st);
        ShadowsDeinit();
        gS.level = 0;
        return;
    }

    // static uniforms: sampler unit
    Glext::GLUseProgram(gS.stdProg);
    Glext::GLUniform1i(gS.stdLoc.Map, kShadowUnit);
    if (gS.v2Injected && _v2ShaderProg.ID)
    {
        Glext::GLUseProgram(_v2ShaderProg.ID);
        Glext::GLUniform1i(gS.v2Loc.Map, kShadowUnit);
        Glext::GLUniform1f(gS.v2Loc.On, 0.0f);
    }
    Glext::GLUseProgram(_lastStates.Prog.ID);

    printf("UA_GFX_SHADOWS: level %d, %d cascade(s) %dpx, half %.0f/%.0f, fade %.0f..%.0f, %d taps, strength %.2f, "
           "host %d/4, %.1f MB%s\n", gS.level, gS.cascades, gS.size, gS.half[0], gS.half[1], gS.fade0, gS.fade1, gS.taps,
           gS.strength, gS.hostKeep, gS.texBytes / 1048576.0, gS.v2Injected ? ", V2 receives" : ", V2 off/no receive");
}

void GFXEngine::ShadowsDeinit()
{
    if (gS.fbo)
        Glext::GLDeleteFramebuffers(1, &gS.fbo);
    if (gS.tex)
    {
        glDeleteTextures(1, &gS.tex);
        Perf::TexAlloc(true, -gS.texBytes);
    }
    gS.fbo = gS.tex = 0;
    gS.texBytes = 0;
    if (gS.depthProg) Glext::GLDeleteProgram(gS.depthProg);
    if (gS.depthVs) Glext::GLDeleteShader(gS.depthVs);
    if (gS.depthPs) Glext::GLDeleteShader(gS.depthPs);
    if (gS.hostProg) Glext::GLDeleteProgram(gS.hostProg);
    if (gS.hostPs) Glext::GLDeleteShader(gS.hostPs);
    gS.hostProg = gS.hostPs = 0;
    if (gS.stdProg) Glext::GLDeleteProgram(gS.stdProg);
    if (gS.stdVs) Glext::GLDeleteShader(gS.stdVs);
    if (gS.stdPs) Glext::GLDeleteShader(gS.stdPs);
    gS.depthProg = gS.depthVs = gS.depthPs = gS.stdProg = gS.stdVs = gS.stdPs = 0;
    _shdStdProg = TShaderProg();
    _shadowFrame = false;
}

// The next Rasterize() is the world frame (yw_game RenderGame).
void GFXEngine::ShadowWorldScene()
{
    if (gS.level > 0)
        _shadowWorld = true;
}

static bool ShadowCaster(const TRenderNode *nod)
{
    if (nod->Type != TRenderNode::TYPE_MESH || !nod->Mesh || !nod->Mesh->glDataBuf || !nod->Mesh->glIndexBuf ||
        nod->Mesh->Indixes.empty())
        return false;
    if (nod->Flags & (RFLAGS_SKY | RFLAGS_LUMTRACY | RFLAGS_ZEROTRACY | RFLAGS_FX_ALPHA | RFLAGS_FALLOFF |
                      RFLAGS_DISABLE_ZWRITE))
        return false;
    // ground atlases boden1-5 of every set (sector + filler ground): GroundTagBitmap's tag (written at every
    // texture prepare by the published engine), so nothing extra runs at SHADOWS 0 and a later switch-on is right
    if (nod->Tex && nod->Tex->groundAtlas)
        return false;
    return true;
}

// Draw the shadow map from the queued solid list; set up the receivers.
void GFXEngine::ShadowPass()
{
    _shadowFrame = false;
    TF::TForm3D *view = TF::Engine.GetViewPoint();
    if (!view || gS.level <= 0)
        return;

    double ri[9];
    if (!Inverse3(view->CalcSclRot, ri))
        return;

    float sunW[3], sunLin, ambLin, hemi;
    V2SunParams(sunW, &sunLin, &ambLin, &hemi);
    // light frame (world, fixed): f = direction the light travels, u/v span the map
    const double f[3] = {-sunW[0], -sunW[1], -sunW[2]};
    double ref[3] = {1.0, 0.0, 0.0};
    if (fabs(f[0]) > 0.9) { ref[0] = 0.0; ref[2] = 1.0; }
    double u[3] = {ref[1] * f[2] - ref[2] * f[1], ref[2] * f[0] - ref[0] * f[2], ref[0] * f[1] - ref[1] * f[0]};
    double ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (double &x : u) x /= ul;
    const double v[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    auto dot = [](const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    // legacy darkening: the V2 sun model's sun/ambient ratio (per-face n.l in the shader);
    // darkLegacy = the resulting multiplier for a flat, up-facing face (log only)
    gS.legacyK = (ambLin + hemi) > 1e-6 ? sunLin / (ambLin + hemi) : 0.0f;
    {
        const double ndlUp = std::max(0.0, -(double)sunW[1]);
        const double lit = sunLin * ndlUp + ambLin + hemi;
        const double shd = sunLin * ndlUp * (1.0 - gS.strength) + ambLin + hemi;
        gS.darkLegacy = lit > 1e-6 ? (float)pow(shd / lit, 1.0 / 2.2) : 1.0f;
    }

    const vec3d cam = view->CalcPos;
    // view forward in camera-relative world: inverse rotation of view +z
    double fwd[3] = {ri[2], ri[5], ri[8]};
    double fl = sqrt(dot(fwd, fwd));
    for (double &x : fwd) x /= fl;
    // half-width of the view at depth 1 (x/y = +-z in view space; the view's
    // x/y rows carry the aspect/fov correction)
    const double kx = sqrt(ri[0] * ri[0] + ri[3] * ri[3] + ri[6] * ri[6]);
    const double ky = sqrt(ri[1] * ri[1] + ri[4] * ri[4] + ri[7] * ri[7]);

    M4 ortho[2];
    const double extraSun = 2600.0; // casters up to this far towards the sun (host stations fly high)
    for (int i = 0; i < gS.cascades; i++)
    {
        const double R = gS.half[i];
        // centre: the camera-relative point 'R' ahead along the view (clamped to the
        // ground-ish slice the cascade must cover), then snapped to whole texels in
        // the world-anchored light frame: no crawl/flicker when the camera moves.
        const double reach = (i == 0 ? 0.8 : 0.9) * R;
        const double c[3] = {fwd[0] * reach, fwd[1] * reach, fwd[2] * reach};
        const double cw[3] = {c[0] + cam.x, c[1] + cam.y, c[2] + cam.z};
        const double t = 2.0 * R / gS.size;
        const double a = dot(cw, u), b = dot(cw, v);
        const double da = floor(a / t) * t - a, db = floor(b / t) * t - b;
        const double cs[3] = {c[0] + da * u[0] + db * v[0], c[1] + da * u[1] + db * v[1], c[2] + da * u[2] + db * v[2]};
        // depth range along f relative to the centre: [-(R + extraSun), R + 1500]
        const double dn = R + extraSun, df = R + 1500.0;
        const double zs = 2.0 / (dn + df);
        M4 o = {{u[0] / R, u[1] / R, u[2] / R, -dot(cs, u) / R,
                 v[0] / R, v[1] / R, v[2] / R, -dot(cs, v) / R,
                 f[0] * zs, f[1] * zs, f[2] * zs, (-dot(cs, f) + dn) * zs - 1.0,
                 0.0, 0.0, 0.0, 1.0}};
        ortho[i] = o;
        // receiver: clip -> atlas uv (cascade i of n side by side) + depth 0..1
        const double n = gS.cascades;
        M4 bias = {{0.5 / n, 0.0, 0.0, (0.5 + i) / n,
                    0.0, 0.5, 0.0, 0.5,
                    0.0, 0.0, 0.5, 0.5,
                    0.0, 0.0, 0.0, 1.0}};
        ToGL(Mul(bias, o), gS.mat[i]);
        gS.casc[i][0] = (float)(1.25 * t);                    // normal offset ~1.25 texel
        gS.casc[i][1] = (float)(0.6 * t * 0.5 * zs);          // compare bias ~0.6 texel of depth
        gS.casc[i][2] = (float)(i / n);
        gS.casc[i][3] = (float)((i + 1) / n);
    }
    for (int i = gS.cascades; i < 2; i++)
    {
        memcpy(gS.mat[i], gS.mat[0], sizeof(gS.mat[i]));
        memcpy(gS.casc[i], gS.casc[0], sizeof(gS.casc[i]));
    }
    for (int i = 0; i < 9; i++)
        gS.camRotInv[(i % 3) * 3 + i / 3] = (float)ri[i]; // column-major mat3
    for (int i = 0; i < 3; i++)
        gS.sunRel[i] = sunW[i];

    // casters: object -> camera-relative world = RotInv * TForm
    const M4 rotInv = {{ri[0], ri[1], ri[2], 0.0, ri[3], ri[4], ri[5], 0.0, ri[6], ri[7], ri[8], 0.0,
                        0.0, 0.0, 0.0, 1.0}};
    struct Cst { TRenderNode *nod; M4 m; double c[3]; double r; bool host; };
    static std::vector<Cst> casters;
    casters.clear();
    for (TRenderNode *nod : _renderSolidList)
    {
        if (!ShadowCaster(nod))
            continue;
        const bool host = (nod->Flags & RFLAGS_SHADOW_HOST) && gS.hostKeep < 4;
        if (host && gS.hostKeep <= 0)
            continue;
        Cst k;
        k.nod = nod;
        k.host = host;
        k.m = Mul(rotInv, FromTForm(nod->TForm));
        const TBoundBox &bb = nod->Mesh->BoundBox;
        double lc[3] = {0, 0, 0}, half = 1e4;
        if (bb.Inited)
        {
            lc[0] = (bb.Min.x + bb.Max.x) * 0.5; lc[1] = (bb.Min.y + bb.Max.y) * 0.5; lc[2] = (bb.Min.z + bb.Max.z) * 0.5;
            half = (bb.Max - bb.Min).length() * 0.5;
        }
        for (int r = 0; r < 3; r++)
            k.c[r] = k.m.m[r * 4 + 0] * lc[0] + k.m.m[r * 4 + 1] * lc[1] + k.m.m[r * 4 + 2] * lc[2] + k.m.m[r * 4 + 3];
        double sc = 0.0;
        for (int col = 0; col < 3; col++)
            sc = std::max(sc, sqrt(k.m.m[col] * k.m.m[col] + k.m.m[4 + col] * k.m.m[4 + col] + k.m.m[8 + col] * k.m.m[8 + col]));
        k.r = half * sc;
        casters.push_back(k);
    }
    // lighter (host-station) casters last: one program switch per cascade
    std::stable_partition(casters.begin(), casters.end(), [](const Cst &k) { return !k.host; });

    // --- caster pass (raw GL; the state cache is restored below) ---
    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, gS.fbo);
    glViewport(0, 0, gS.size * gS.cascades, gS.size);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);

    if (_lastStates.Prog.ID)
    {
        const TShaderProg &lp = _lastStates.Prog;
        for (int32_t loc : {lp.PosLoc, lp.ColorLoc, lp.UVLoc, lp.NormalLoc, lp.TangentLoc})
            if (loc != -1)
                Glext::GLDisableVertexAttribArray(loc);
    }
    Glext::GLUseProgram(gS.depthProg);
    Glext::GLEnableVertexAttribArray(gS.depthPosLoc);

    int drawn[2] = {0, 0};
    GLuint lastBuf = 0, lastIdx = 0;
    for (int i = 0; i < gS.cascades; i++)
    {
        glViewport(i * gS.size, 0, gS.size, gS.size);
        const double R = gS.half[i];
        const M4 &o = ortho[i];
        int32_t mvpLoc = gS.depthMvpLoc;
        bool hostOn = false;
        if (i > 0)
            Glext::GLUseProgram(gS.depthProg);
        for (const Cst &k : casters)
        {
            if (k.host && !hostOn)
            {
                Glext::GLUseProgram(gS.hostProg); // same vertex shader, vPos at location 0
                mvpLoc = gS.hostMvpLoc;
                hostOn = true;
            }
            // cull against the cascade square (light x/y); depth is clamped by the range
            const double lx = o.m[0] * k.c[0] + o.m[1] * k.c[1] + o.m[2] * k.c[2] + o.m[3];
            const double ly = o.m[4] * k.c[0] + o.m[5] * k.c[1] + o.m[6] * k.c[2] + o.m[7];
            const double rr = k.r / R;
            if (fabs(lx) > 1.0 + rr || fabs(ly) > 1.0 + rr)
                continue;
            float mvp[16];
            ToGL(Mul(o, k.m), mvp);
            Glext::GLUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
            if (k.nod->Mesh->glDataBuf != lastBuf)
            {
                lastBuf = k.nod->Mesh->glDataBuf;
                Glext::GLBindBuffer(GL_ARRAY_BUFFER, lastBuf);
                Glext::GLVertexAttribPointer(gS.depthPosLoc, 3, GL_FLOAT, GL_FALSE, sizeof(TVertex), (void *)offsetof(TVertex, Pos));
            }
            if (k.nod->Mesh->glIndexBuf != lastIdx)
            {
                lastIdx = k.nod->Mesh->glIndexBuf;
                Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lastIdx);
            }
            glDrawElements(GL_TRIANGLES, k.nod->Mesh->Indixes.size(), GLINDEXTYPE, NULL);
            drawn[i]++;
        }
    }

    Glext::GLDisableVertexAttribArray(gS.depthPosLoc);
    glDisable(GL_POLYGON_OFFSET_FILL);
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    // shadow map on its own unit for the rest of the world frame
    Glext::GLActiveTexture(GL_TEXTURE0 + kShadowUnit);
    glBindTexture(GL_TEXTURE_2D, gS.tex);
    Glext::GLActiveTexture(GL_TEXTURE0);

    // per-frame receiver uniforms
    Glext::GLUseProgram(gS.stdProg);
    PushFrame(gS.stdLoc);
    Glext::GLUniform1f(gS.stdLoc.On, 1.0f);
    if (gS.v2Injected && _v2ShaderProg.ID)
    {
        if (gS.v2LocProg != _v2ShaderProg.ID) // V2 built after ShadowsInit (settings: at level load)
        {
            FetchLoc(_v2ShaderProg.ID, &gS.v2Loc);
            gS.v2LocProg = _v2ShaderProg.ID;
        }
        Glext::GLUseProgram(_v2ShaderProg.ID);
        PushFrame(gS.v2Loc);
    }
    if (gS.gndInjected && _gndShaderProg.ID) // floor program receives too
    {
        if (gS.gndLocProg != _gndShaderProg.ID)
        {
            FetchLoc(_gndShaderProg.ID, &gS.gndLoc);
            gS.gndLocProg = _gndShaderProg.ID;
        }
        Glext::GLUseProgram(_gndShaderProg.ID);
        PushFrame(gS.gndLoc);
    }
    if (gS.atmInjected && _atmStdProg.ID) // ATMOSPHERE legacy program = the legacy receiver when atmos is on
    {
        if (gS.atmLocProg != _atmStdProg.ID)
        {
            FetchLoc(_atmStdProg.ID, &gS.atmLoc);
            gS.atmLocProg = _atmStdProg.ID;
        }
        Glext::GLUseProgram(_atmStdProg.ID);
        PushFrame(gS.atmLoc);
    }
    gS.gndOn = gS.atmOn = -1;
    // hand everything back to the state cache: program + attribs, buffers,
    // texture, blend, depth (setAll = 1 re-applies _states == _lastStates)
    Glext::GLUseProgram(0);
    _lastStates.Prog = TShaderProg(); // nothing left to disable: the caster pass did it
    SetRenderStates(1);
    glDepthFunc(GL_LEQUAL);

    _shadowFrame = true;
    gS.frames++;
    if (gS.log > 0 && (gS.frames == 1 || gS.frames % 300 == 0))
        printf("UA_GFX_SHADOWS: frame %u casters %zu drawn c0 %d c1 %d, cam (%.0f %.0f %.0f) fwd (%.2f %.2f %.2f) "
               "k %.2f/%.2f dark %.3f\n", gS.frames, casters.size(), drawn[0], drawn[1], cam.x, cam.y, cam.z,
               fwd[0], fwd[1], fwd[2], kx, ky, gS.darkLegacy);
}

// V2 draw: shadow on/off for this mesh (units at far-read size: off).
void GFXEngine::ShadowV2Draw(float farFade)
{
    if (gS.v2Injected && _v2ShaderProg.ID && gS.v2LocProg == _v2ShaderProg.ID)
        Glext::GLUniform1f(gS.v2Loc.On, _shadowFrame ? 1.0f - farFade : 0.0f);
}

// Floor draw (ground program current): shadow on only in the world frame.
void GFXEngine::ShadowGroundDraw()
{
    const int on = _shadowFrame ? 1 : 0;
    if (gS.gndInjected && _gndShaderProg.ID && gS.gndLocProg == _gndShaderProg.ID && gS.gndOn != on)
    {
        Glext::GLUniform1f(gS.gndLoc.On, (float)on);
        gS.gndOn = on;
    }
}

// ATMOSPHERE legacy draw (its program current): shadow on only in the world frame.
void GFXEngine::ShadowAtmDraw()
{
    const int on = _shadowFrame ? 1 : 0;
    if (gS.atmInjected && _atmStdProg.ID && gS.atmLocProg == _atmStdProg.ID && gS.atmOn != on)
    {
        Glext::GLUniform1f(gS.atmLoc.On, (float)on);
        gS.atmOn = on;
    }
}

// The ATMOSPHERE legacy pixel shader gets the legacy receiver's shadow ratio (same maths as kStdShdPSMain),
// applied to the surface before the lit windows and the haze. Level 0: text unchanged.
std::string GFXEngine::ShadowAtmStdPS(const std::string &src)
{
    gS.atmInjected = false;
    if (ShadowLevel() <= 0)
        return src;
    std::string s = src;
    const std::string anchor = "    if (AtmWin > 0.0 && Textured)";
    size_t m = s.find("void main()"), a = s.find(anchor);
    if (m == std::string::npos || a == std::string::npos)
    {
        printf("UA_GFX_SHADOWS: atmos legacy PS anchors not found: atmos legacy draws receive no shadow\n");
        return src;
    }
    s.insert(a, "    float uaNdl; vec3 uaDbg;\n"
                "    float uaS = uaShadow(vsPos, uaNdl, uaDbg);\n"
                "    float uaNl = max(uaNdl, 0.0) * ShadowLook.y;\n"
                "    clr.rgb *= pow((1.0 + uaNl * (1.0 - ShadowLook.x * (1.0 - uaS))) / (1.0 + uaNl), 1.0 / 2.2) * uaDbg;\n");
    s.insert(m, std::string("in vec3 vsPos;\n") + kShadowLib + "\n");
    gS.atmInjected = true;
    return s;
}

// A program that may carry the lookup was just linked (V2 / floor, possibly after
// ShadowsInit): park its shadow sampler on its own unit (a sampler2DShadow left on unit 0
// would clash with texSample) and switch it off until the next ShadowPass fetches it.
void GFXEngine::ShadowProgramLinked(uint32_t prog)
{
    if (ShadowLevel() <= 0 || !prog)
        return;
    if (gS.v2LocProg == prog)
        gS.v2LocProg = 0;
    if (gS.gndLocProg == prog)
        gS.gndLocProg = 0;
    if (gS.atmLocProg == prog)
        gS.atmLocProg = 0;
    gS.gndOn = gS.atmOn = -1;
    const int32_t map = Glext::GLGetUniformLocation(prog, "ShadowMap");
    if (map < 0)
        return;
    Glext::GLUseProgram(prog);
    Glext::GLUniform1i(map, kShadowUnit);
    Glext::GLUniform1f(Glext::GLGetUniformLocation(prog, "ShadowOn"), 0.0f);
    Glext::GLUseProgram(_lastStates.Prog.ID);
}

// Floor shaders (ground.cpp). Level 0: texts unchanged (byte-identical floor).
std::string GFXEngine::ShadowGroundVS(const std::string &src)
{
    if (ShadowLevel() <= 0)
        return src;
    std::string s = src;
    const std::string gp = "gl_Position = MProj * tformed;";
    size_t m = s.find("void main()"), g = s.find(gp);
    if (m == std::string::npos || g == std::string::npos)
    {
        printf("UA_GFX_SHADOWS: floor VS anchors not found: floor receives no shadow\n");
        return src;
    }
    s.replace(g, gp.size(), gp + "\n    vsPos = tformed.xyz;");
    s.insert(m, "out vec3 vsPos;\n");
    return s;
}

std::string GFXEngine::ShadowGroundPS(const std::string &src)
{
    gS.gndInjected = false;
    if (ShadowLevel() <= 0)
        return src;
    std::string s = src;
    const std::string col = "vec3 col = albedo * ratio;";
    const std::string spec = "float specAmt = Strength.z * spec * fade;";
    size_t m = s.find("void main()"), c = s.find(col), sp = s.find(spec);
    if (m == std::string::npos || c == std::string::npos || sp == std::string::npos || sp < c)
    {
        printf("UA_GFX_SHADOWS: floor PS anchors not found: floor receives no shadow\n");
        return src;
    }
    // the sun term of the detail-normal light is cut by the visibility, like the V2 meshes
    s.replace(sp, spec.size(), "float specAmt = Strength.z * spec * fade * uaSv;");
    s.replace(c, col.size(),
              "float uaNdlG; vec3 uaDbg;\n"
              "    float uaSv = 1.0 - ShadowLook.x * (1.0 - uaShadow(vsPos, uaNdlG, uaDbg));\n"
              "    float ldS = Light.x * max(dot(N, SunW), 0.0) * uaSv + Light.y + Light.z * dot(N, up);\n"
              "    vec3 col = albedo * ratio * (ldS / max(ld, 1e-3)) * uaDbg;");
    s.insert(m, std::string("in vec3 vsPos;\n") + kShadowLib + "\n");
    gS.gndInjected = true;
    return s;
}

bool GFXEngine::ShadowLegacyReceiver(const TRenderNode *nod) const
{
    return nod->Type == TRenderNode::TYPE_MESH && _shdStdProg.ID &&
           !(nod->Flags & (RFLAGS_SKY | RFLAGS_LUMTRACY | RFLAGS_FX_ALPHA));
}

} // namespace GFX
