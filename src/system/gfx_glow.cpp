// Reforge GLOW: bloom on energy and light. See gfx_glow.h for the design.
//
// Levels (UA_GFX_GLOW):
//   0  off: nothing allocated, nothing compiled, draw path untouched.
//   1  low: mask -> 1/2 -> 1/4 -> 1/8 dual-filter pyramid (RGBA8), composed
//      from 1/4 res. 3 downs + 1 up + composite.
//   2  medium: pyramid to 1/16 (RGBA16F), composed from 1/2 res (wider, smoother).
//   3  high: pyramid to 1/32 (RGBA16F), composed from 1/2 res (widest halo).
// Look-dev only: UA_GFX_GLOW_GAIN=<k> scales the glow, UA_GFX_GLOW_VIEW=1 shows
// the glow alone (frame cleared to black first).
//
// GLES/Quest: every shader is a fullscreen triangle with 4-8 bilinear taps
// (no compute, no textureGather, no MRT); the one desktop-only call is the
// depth blit from the default framebuffer (GLES 3.0 has glBlitFramebuffer, but
// Quest's multiview MSAA eye buffers would need the mask drawn inside the eye
// pass instead).

#include <SDL2/SDL_opengl.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/glext.h>
#else
#include <GL/glext.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "gfx.h"
#include "../gfxsettings.h"
#include "gfx_glow.h"
#include "glfuncs.h"
#include "system.h"

namespace GFX
{

int GlowPass::_level = 0;
bool GlowPass::_replaying = false;
bool GlowPass::_broken = false;
bool GlowPass::_capable = false;
std::vector<TRenderNode *> GlowPass::_nodes;
// Look rule: only fire / energy / lasers glow. Textures whose resource name is on the exclusion list
// (UA_GFX_GLOW_EXCLUDE overrides; comma list, case-insensitive, no path) never enter the mask.
static std::unordered_set<const ResBitmap *> gGlowSkip;
static std::unordered_map<const ResBitmap *, std::string> gGlowName; // look-dev (UA_GFX_GLOW_DUMP) only
static uint32_t gGlowFrame = 0;

namespace
{

typedef void (APIENTRY *PfnBlit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY *PfnDelFb)(GLsizei, const GLuint *);
typedef void (APIENTRY *PfnProgU1f)(GLuint, GLint, GLfloat);
PfnProgU1f pProgU1f = NULL;

PfnBlit pBlit = NULL;
PfnDelFb pDelFbo = NULL;
PfnDelFb pDelRbo = NULL;

const int MAXLV = 5;
// textures kept out of the glow (UA_GFX_GLOW_EXCLUDE replaces this list; "UA_GFX_GLOW_EXCLUDE=" = none).
// Default (only fire/energy/lasers glow): the five genesis grid shells of the HD fx pack (the cream
// "H"-like build-up shells round newly created units/hosts, one per faction).
const char *kGlowExclude = "genesis_alien.png,genesis_ghorkov.png,genesis_resistance.png,genesis_sulgogar.png,genesis_taerkasten.png";

struct
{
    int w = 0, h = 0;
    uint32_t maskFbo = 0, maskTex = 0, maskDepth = 0;
    int n = 0;                  // pyramid levels
    int compose = 0;            // level composited onto the frame
    uint32_t fbo[MAXLV] = {0};
    uint32_t tex[MAXLV] = {0};
    int lw[MAXLV] = {0}, lh[MAXLV] = {0};
    uint32_t vao = 0;
    uint32_t progDown = 0, progUp = 0, progComp = 0;
    int32_t downTexel = -1, upTexel = -1, upBoost = -1, compGain = -1, compCap = -1;
    float cap = 0.4;               // UA_GFX_GLOW_CAP
    float gain = 1.0;
    float keep = 0.5, boost = 1.7;  // sharp-level weight, wide-level lift (UA_GFX_GLOW_SHAPE=keep,boost)
    float wide = 0.0;               // lift of the extra (deeper) levels added on top of level 1 (UA_GFX_GLOW_WIDE)
    bool view = false;
    bool depthOk = true;
    bool resync = false;
    TShaderProg replay;            // std shader variant used for the mask replay
    int32_t replayW = -1, replayFog = -1;
    float fogKeep = 0.5;           // share of the depth-cue fade the glow keeps (far effects still read)
    float areaRef = 0.2;
    std::string dump;              // UA_GFX_GLOW_DUMP=<dir>: look-dev, one line + BMP per new mask texture           // screen half-height fraction above which a source's glow is attenuated       // GL state touched outside the engine cache
} G;

const char *kVS = R"GLSL(#version 410 core
out vec2 uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Dual-filter downsample (Bjorge 2015): centre x4 + 4 diagonal bilinear taps.
const char *kDown = R"GLSL(#version 410 core
uniform sampler2D src;
uniform vec2 texel;      // source texel size
in vec2 uv;
out vec4 o;
void main()
{
    vec2 h = texel;
    vec4 s = texture(src, uv) * 4.0;
    s += texture(src, uv - h);
    s += texture(src, uv + h);
    s += texture(src, uv + vec2(h.x, -h.y));
    s += texture(src, uv - vec2(h.x, -h.y));
    o = s * 0.125;
}
)GLSL";

// Dual-filter upsample: 4 edge taps (w 1) + 4 diagonal taps (w 2), /12.
const char *kUp = R"GLSL(#version 410 core
uniform sampler2D src;
uniform vec2 texel;      // source (smaller level) texel size
uniform float boost;     // >1 lifts the wide levels (a far 2-px laser still gets a halo)
in vec2 uv;
out vec4 o;
void main()
{
    vec2 h = texel;
    vec4 s = texture(src, uv + vec2(-2.0 * h.x, 0.0));
    s += texture(src, uv + vec2( 2.0 * h.x, 0.0));
    s += texture(src, uv + vec2(0.0, -2.0 * h.y));
    s += texture(src, uv + vec2(0.0,  2.0 * h.y));
    s += texture(src, uv + vec2(-h.x,  h.y)) * 2.0;
    s += texture(src, uv + vec2( h.x,  h.y)) * 2.0;
    s += texture(src, uv + vec2(-h.x, -h.y)) * 2.0;
    s += texture(src, uv + vec2( h.x, -h.y)) * 2.0;
    o = s * (boost / 12.0);
}
)GLSL";

// Composite: soft-saturated so stacked effects never flood to flat white.
const char *kComp = R"GLSL(#version 410 core
uniform sampler2D src;
uniform float gain;
uniform float cap;       // ceiling of the added light: small sources stay linear (x gain), big ones flatten at cap
in vec2 uv;
out vec4 o;
void main()
{
    vec3 g = texture(src, uv).rgb * gain;
    float l = max(g.r, max(g.g, g.b));
    if (l > 1e-5)                                   // hue-preserving soft ceiling (per-channel would bleach to beige)
        g *= cap * (1.0 - exp(-l / cap)) / l;
    float y = dot(g, vec3(0.2126, 0.7152, 0.0722));
    g = max(mix(vec3(y), g, 1.25), 0.0);            // keep the light's colour (blue arcs, yellow veins, orange fire)
    o = vec4(g, 0.0);    // blended as 'screen' (ONE_MINUS_DST_COLOR, ONE): bright areas cannot burn to white
}
)GLSL";

uint32_t Link(GFXEngine &E, const char *fs)
{
    uint32_t vs = E.CompileShader(GL_VERTEX_SHADER, kVS);
    uint32_t ps = E.CompileShader(GL_FRAGMENT_SHADER, fs);
    if (!vs || !ps)
        return 0;
    uint32_t p = Glext::GLCreateProgram();
    Glext::GLAttachShader(p, vs);
    Glext::GLAttachShader(p, ps);
    Glext::GLLinkProgram(p);
    Glext::GLDeleteShader(vs);
    Glext::GLDeleteShader(ps);
    GLint ok = 0;
    Glext::GLGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        Glext::GLGetProgramInfoLog(p, sizeof(log) - 1, NULL, log);
        printf("UA_GFX_GLOW: link failed: %s\n", log);
        Glext::GLDeleteProgram(p);
        return 0;
    }
    Glext::GLUseProgram(p);
    Glext::GLUniform1i(Glext::GLGetUniformLocation(p, "src"), 0);
    return p;
}

void MakeTex(uint32_t &t, int w, int h, bool half)
{
    if (!t)
        glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (half)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void Quad()
{
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}

// Settings helper (preset row / override / UA_GFX_GLOW), clamped; 0 without the capable path.
int GlowPass::WantLevel()
{
    int lv = gfxLevel("GLOW", 0);
    if (lv < 0) lv = 0;
    if (lv > 3) lv = 3;
    if (lv > 0 && !_capable)
    {
        static bool said = false;
        if (!said)
            printf("UA_GFX_GLOW=%d ignored: needs the VBO / GL 4.1 path without gfx.color_effects\n", lv);
        said = true;
        lv = 0;
    }
    return lv;
}

void GlowPass::Init(bool capable)
{
    _capable = capable;
    Configure(WantLevel());
}

void GlowPass::OnLevelLoad()
{
    const int lv = WantLevel();
    if (lv == _level) // the level-0 steady state included: nothing touched
        return;
    const int was = _level;
    Free(); // GL objects; Ensure() rebuilds them at the first glowing frame
    std::vector<TRenderNode *>().swap(_nodes);
    _broken = false;
    if (lv == 0)
    {
        std::unordered_set<const ResBitmap *>().swap(gGlowSkip);
        std::unordered_map<const ResBitmap *, std::string>().swap(gGlowName);
    }
    Configure(lv);
    printf("UA_GFX_GLOW: level %d -> %d (graphics settings)\n", was, lv);
}

void GlowPass::Configure(int lv)
{
    _level = lv;
    if (lv > 0)
    {
        // Higher = MORE glow, never calmer: every level is level 1's chain
        // (compose at 1/4, keep 0.5 / boost 1.7, gain 3.5) and the higher levels ADD wider pyramid levels on
        // top (1/16, 1/32) and a higher ceiling, so the core is at least as hot and the halo reaches further.
        static const float kWide[4] = {0.0f, 0.0f, 0.8f, 0.8f}, kCap[4] = {0.4f, 0.4f, 0.5f, 0.6f};
        G.keep = 0.5f;
        G.boost = 1.7f;
        G.wide = kWide[lv];
        G.cap = kCap[lv];
        if (const char *v = getenv("UA_GFX_GLOW_WIDE"))
            G.wide = atof(v);
        if (const char *v = getenv("UA_GFX_GLOW_GAIN"))
            G.gain = atof(v);
        if (const char *v = getenv("UA_GFX_GLOW_DUMP"))
            G.dump = v;
        if (const char *v = getenv("UA_GFX_GLOW_FOG"))
            G.fogKeep = atof(v);
        if (const char *v = getenv("UA_GFX_GLOW_AREA"))
            G.areaRef = std::max(0.01, atof(v));
        if (const char *v = getenv("UA_GFX_GLOW_CAP"))
            G.cap = std::max(0.05, atof(v));
        if (const char *v = getenv("UA_GFX_GLOW_SHAPE"))
            sscanf(v, "%f,%f", &G.keep, &G.boost);
        if (const char *v = getenv("UA_GFX_GLOW_VIEW"))
            G.view = atoi(v) != 0;
        printf("UA_GFX_GLOW: level %d\n", lv);
    }
}

void GlowPass::TagBitmap(const ResBitmap *bitm, const std::string &rsrcName)
{
    if (_level <= 0 || !bitm)
        return;
    std::string n;
    for (char c : rsrcName)
        n += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    size_t slash = n.find_last_of("/\\:");
    if (slash != std::string::npos)
        n = n.substr(slash + 1);
    static std::string list;
    static bool listInit = false;
    if (!listInit)
    {
        listInit = true;
        const char *v = getenv("UA_GFX_GLOW_EXCLUDE");
        list = std::string(",") + (v ? v : kGlowExclude) + ",";
        for (char &c : list)
            c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (!n.empty() && list.find("," + n + ",") != std::string::npos)
        gGlowSkip.insert(bitm);
    else
        gGlowSkip.erase(bitm);
    if (!G.dump.empty())
        gGlowName[bitm] = n;
}

void GlowPass::CollectSlow(TRenderNode *node)
{
    if (_broken || !node)
        return;
    if (node->Tex && !gGlowSkip.empty() && gGlowSkip.count(node->Tex))
        return;
    // Horizon emitters: lumtracy base meshes beyond the sky distance (FALLOFF) that the level data does not
    // depth-fade (no FOG) = the map-border energy curtain (FX atlas streak strip, seen behind the skyline on
    // L7575 / L1111 / L0404). It would bloom a third of the sky; real effects carry FOG (depth fade) or are
    // particles / fx-pack sprites (never FALLOFF).
    if ((node->Flags & RFLAGS_FALLOFF) && !(node->Flags & RFLAGS_FOG))
        return;
    _nodes.push_back(node);
}

bool GlowPass::BuildPrograms()
{
    GFXEngine &E = GFXEngine::Instance;
    pBlit = (PfnBlit)SDL_GL_GetProcAddress("glBlitFramebuffer");
    pDelFbo = (PfnDelFb)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    pDelRbo = (PfnDelFb)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    if (!pBlit || !pDelFbo || !pDelRbo)
        return false;
    pProgU1f = (PfnProgU1f)SDL_GL_GetProcAddress("glProgramUniform1f");
    if (!pProgU1f)
        return false;
    {
        // Mask replay program = the engine's std program with (a) the emitted light written premultiplied
        // (rgb * a * w, blended ONE/ONE: the same e = rgb*a the lumtracy blend adds) and (b) only part of the
        // depth-cue fade, so a far laser still owns a halo. Same attribs / UBO as the std program.
        std::string vs = GFXEngine::_stdVShaderText, ps = GFXEngine::_stdPShaderText;
        const std::string fogOld = "mix(vColor.xyz, vec3(0.0), fg)";
        const std::string outOld = "fragColor = clr;";
        size_t a = vs.find(fogOld), b = ps.find(outOld), c = vs.find("in vec3 vPos;"), d = ps.find("uniform sampler2D texSample;");
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos || d == std::string::npos)
            return false;
        vs.replace(a, fogOld.size(), "mix(vColor.xyz, vec3(0.0), fg * GlowFog)");
        vs.insert(c, "uniform float GlowFog;");
        ps.replace(b, outOld.size(), "fragColor = vec4(clr.rgb * clr.a * GlowW, clr.a);");
        ps.insert(d, "uniform float GlowW;");
        uint32_t v = E.CompileShader(GL_VERTEX_SHADER, vs), f = E.CompileShader(GL_FRAGMENT_SHADER, ps);
        if (!v || !f)
            return false;
        uint32_t p = Glext::GLCreateProgram();
        Glext::GLAttachShader(p, v);
        Glext::GLAttachShader(p, f);
        Glext::GLLinkProgram(p);
        Glext::GLDeleteShader(v);
        Glext::GLDeleteShader(f);
        GLint ok = 0;
        Glext::GLGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok)
            return false;
        G.replay = TShaderProg(p);
        E.BindVBOParameters(G.replay);
        G.replayW = Glext::GLGetUniformLocation(p, "GlowW");
        G.replayFog = Glext::GLGetUniformLocation(p, "GlowFog");
        pProgU1f(p, G.replayFog, G.fogKeep);
        pProgU1f(p, G.replayW, 1.0);
    }
    G.progDown = Link(E, kDown);
    G.progUp = Link(E, kUp);
    G.progComp = Link(E, kComp);
    if (!G.progDown || !G.progUp || !G.progComp)
        return false;
    G.downTexel = Glext::GLGetUniformLocation(G.progDown, "texel");
    G.upTexel = Glext::GLGetUniformLocation(G.progUp, "texel");
    G.upBoost = Glext::GLGetUniformLocation(G.progUp, "boost");
    G.compGain = Glext::GLGetUniformLocation(G.progComp, "gain");
    G.compCap = Glext::GLGetUniformLocation(G.progComp, "cap");
    G.resync = true;
    Glext::GLGenVertexArrays(1, &G.vao);
    return G.vao != 0;
}

bool GlowPass::Ensure(int w, int h)
{
    if (!G.progComp && !BuildPrograms())
        return false;
    if (G.w == w && G.h == h && G.maskFbo)
        return true;

    G.w = w;
    G.h = h;
    G.resync = true;
    // Level layout: pyramid depth, composed level, pyramid format.
    const bool half = _level >= 2;
    G.n = (_level == 1) ? 3 : (_level == 2 ? 4 : 5);
    G.compose = 1; // every level composes level 1's 1/4 target (higher levels add deeper levels into it)

    if (!G.maskFbo)
    {
        Glext::GLGenFramebuffers(1, &G.maskFbo);
        Glext::GLGenRenderbuffers(1, &G.maskDepth);
    }
    MakeTex(G.maskTex, w, h, false);
    Glext::GLBindRenderbuffer(GL_RENDERBUFFER, G.maskDepth);
    Glext::GLRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    Glext::GLBindRenderbuffer(GL_RENDERBUFFER, 0);
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, G.maskFbo);
    Glext::GLFrameBufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.maskTex, 0);
    Glext::GLFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.maskDepth);
    bool ok = Glext::GLCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    size_t bytes = (size_t)w * h * 8; // RGBA8 mask + D24 (padded to 4)
    for (int i = 0; i < G.n && ok; i++)
    {
        G.lw[i] = std::max(1, w >> (i + 1));
        G.lh[i] = std::max(1, h >> (i + 1));
        MakeTex(G.tex[i], G.lw[i], G.lh[i], half);
        if (!G.fbo[i])
            Glext::GLGenFramebuffers(1, &G.fbo[i]);
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, G.fbo[i]);
        Glext::GLFrameBufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.tex[i], 0);
        ok = Glext::GLCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        bytes += (size_t)G.lw[i] * G.lh[i] * (half ? 8 : 4);
    }
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("UA_GFX_GLOW: level %d targets %dx%d, %d pyramid levels (%s), %.1f MB %s\n", _level, w, h, G.n,
           half ? "RGBA16F" : "RGBA8", bytes / 1048576.0, ok ? "ok" : "INCOMPLETE");
    return ok;
}

void GlowPass::ReplayStates(GfxStates &st, const TRenderNode *node)
{
    st.Zwrite = false;
    st.Prog = G.replay;
    st.AlphaBlend = true;
    st.Stipple = false;
    if (node->Flags & RFLAGS_FX_ALPHA)
    {
        // smoke / debris: occlude the glow behind them, add nothing
        st.SrcBlend = GL_ZERO;
        st.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
        return;
    }
    st.SrcBlend = GL_ONE;
    st.DstBlend = GL_ONE;
    // Anti-wash: a source that fills a large part of the screen (beam-gate curtain, a fireball in the
    // camera's face, a genesis shell up close) gets its glow scaled by (ref / size)^2, so the halo of a
    // big emitter stays the size of a small one's. Screen size = bounding radius / view depth (UA frustum
    // has focal 1: half-height fraction).
    float w = 1.0;
    const TMesh *m = node->Mesh;
    if (m && m->BoundBox.Inited)
    {
        vec3d c = (m->BoundBox.Min + m->BoundBox.Max) * 0.5;
        vec3d h = (m->BoundBox.Max - m->BoundBox.Min) * 0.5;
        const mat4x4 &t = node->TForm;
        double sc = sqrt(t.m00 * t.m00 + t.m10 * t.m10 + t.m20 * t.m20);
        double r = h.length() * sc;
        double z = t.m20 * c.x + t.m21 * c.y + t.m22 * c.z + t.m23;
        double sz = (z > r * 0.25) ? r / z : 4.0;
        if (sz > G.areaRef)
            w = (float)((G.areaRef / sz) * (G.areaRef / sz));
    }
    pProgU1f(G.replay.ID, G.replayW, w);
    if (!G.dump.empty() && node->Tex && (gGlowFrame % 250) == 0 && m && m->BoundBox.Inited)
    {
        // look-dev: which texture is where on screen (view space: x/z, y/z = -1..1)
        vec3d c = (m->BoundBox.Min + m->BoundBox.Max) * 0.5;
        const mat4x4 &t = node->TForm;
        double x = t.m00 * c.x + t.m01 * c.y + t.m02 * c.z + t.m03;
        double y = t.m10 * c.x + t.m11 * c.y + t.m12 * c.z + t.m13;
        double z = t.m20 * c.x + t.m21 * c.y + t.m22 * c.z + t.m23;
        auto it = gGlowName.find(node->Tex);
        if (z > 1.0)
            printf("glow-where f %u tex %s x %.3f y %.3f z %.0f verts %d\n", gGlowFrame,
                   it != gGlowName.end() ? it->second.c_str() : "?", x / z, y / z, z, (int)m->Vertexes.size());
    }
    if (!G.dump.empty() && node->Tex)
    {
        static std::vector<uint32_t> seen;
        if (std::find(seen.begin(), seen.end(), node->Tex->hwTex) == seen.end())
        {
            seen.push_back(node->Tex->hwTex);
            std::string f = G.dump + "/glowtex_" + std::to_string(node->Tex->hwTex) + ".bmp";
            if (node->Tex->swTex)
                SDL_SaveBMP(node->Tex->swTex, f.c_str());
            printf("glow-dump tex %u %dx%d flags 0x%x type %d bbox %s verts %d w %.3f dist %.0f\n", node->Tex->hwTex,
                   node->Tex->width, node->Tex->height, node->Flags, node->Type,
                   (m && m->BoundBox.Inited) ? "yes" : "no", m ? (int)m->Vertexes.size() : -1, w, node->Distance);
        }
    }
}

void GlowPass::Apply()
{
    if (_level <= 0)
        return;
    gGlowFrame++;
    if (_nodes.empty())
        return;

    GFXEngine &E = GFXEngine::Instance;
    Common::Point scr = System::GetResolution();
    if (_broken || !Ensure(scr.x, scr.y))
    {
        if (!_broken)
            printf("UA_GFX_GLOW: setup failed, glow disabled\n");
        _broken = true;
        _nodes.clear();
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    const int w = G.w, h = G.h;
    if (G.resync)
    {
        // Ensure() bound programs/textures behind the cache's back (first use / resize only).
        G.resync = false;
        E.SetRenderStates(2);
    }

    // 1. Mask target gets the frame's depth, colour cleared.
    while (glGetError() != GL_NO_ERROR) {}
    Glext::GLBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    Glext::GLBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.maskFbo);
    if (G.depthOk)
    {
        pBlit(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (glGetError() != GL_NO_ERROR)
        {
            printf("UA_GFX_GLOW: depth blit refused (format/samples); mask without occlusion\n");
            G.depthOk = false;
        }
    }
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, G.maskFbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    if (G.depthOk)
        glClear(GL_COLOR_BUFFER_BIT);
    else
    {
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDepthMask(E._lastStates.Zwrite ? GL_TRUE : GL_FALSE);
    }

    // 2. Replay the frame's additive / lumtracy draws into the mask.
    _replaying = true;
    for (TRenderNode *n : _nodes)
        E.RenderNode(n);
    _replaying = false;
    _nodes.clear();

    // 3. Pyramid: downs, then additive ups to the composed level.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    Glext::GLBindVertexArray(G.vao);
    Glext::GLActiveTexture(GL_TEXTURE0);

    Glext::GLUseProgram(G.progDown);
    uint32_t src = G.maskTex;
    int sw = w, sh = h;
    for (int i = 0; i < G.n; i++)
    {
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, G.fbo[i]);
        glViewport(0, 0, G.lw[i], G.lh[i]);
        Glext::GLUniform2f(G.downTexel, 1.0 / sw, 1.0 / sh);
        glBindTexture(GL_TEXTURE_2D, src);
        Quad();
        src = G.tex[i];
        sw = G.lw[i];
        sh = G.lh[i];
    }

    // ups: level(i-1) = own down * keep + upsampled(i) * boost  (weight moves from the core to the halo)
    glEnable(GL_BLEND);
    // The deeper ups (levels 2 only) keep their own level whole and add the wider one x wide; the
    // last up into the composed level is level 1's (keep 0.5, boost 1.7) -> higher levels = level 1 + more.
    glBlendFunc(GL_ONE, GL_CONSTANT_ALPHA);
    Glext::GLUseProgram(G.progUp);
    for (int i = G.n - 1; i > G.compose; i--)
    {
        const bool last = (i - 1 == G.compose);
        glBlendColor(0, 0, 0, last ? G.keep : 1.0f);
        Glext::GLUniform1f(G.upBoost, last ? G.boost : G.wide);
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, G.fbo[i - 1]);
        glViewport(0, 0, G.lw[i - 1], G.lh[i - 1]);
        Glext::GLUniform2f(G.upTexel, 1.0 / G.lw[i], 1.0 / G.lh[i]);
        glBindTexture(GL_TEXTURE_2D, G.tex[i]);
        Quad();
    }

    // 4. Composite onto the frame (before HUD / UI).
    static const float kGain[4] = {0.0, 3.5, 3.5, 3.5}; // level 1 tuned on the fx stage (sweeps 1-3); same punch at 2/3
    Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    if (G.view)
    {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE); // screen
    Glext::GLUseProgram(G.progComp);
    Glext::GLUniform1f(G.compGain, kGain[_level] * G.gain);
    Glext::GLUniform1f(G.compCap, G.cap);
    glBindTexture(GL_TEXTURE_2D, G.tex[G.compose]);
    Quad();

    // 5. Hand the GL state back to the engine's cache.
    Glext::GLBindVertexArray(E._globalVao);
    E.SetRenderStates(2);
}

void GlowPass::Free()
{
    if (!G.vao && !G.maskFbo)
        return;
    if (G.maskFbo && pDelFbo) pDelFbo(1, &G.maskFbo);
    if (G.maskDepth && pDelRbo) pDelRbo(1, &G.maskDepth);
    if (G.maskTex) glDeleteTextures(1, &G.maskTex);
    for (int i = 0; i < MAXLV; i++)
    {
        if (G.fbo[i] && pDelFbo) pDelFbo(1, &G.fbo[i]);
        if (G.tex[i]) glDeleteTextures(1, &G.tex[i]);
        G.fbo[i] = G.tex[i] = 0;
    }
    if (G.progDown) Glext::GLDeleteProgram(G.progDown);
    if (G.progUp) Glext::GLDeleteProgram(G.progUp);
    if (G.progComp) Glext::GLDeleteProgram(G.progComp);
    if (G.replay.ID) Glext::GLDeleteProgram(G.replay.ID);
    if (G.vao) Glext::GLDeleteVertexArrays(1, &G.vao);
    G = decltype(G)();
}

}
