// Urban Assault VR — overlay quads (reticle, laser beam, pointer cursor). See vr_overlay.h.

#include <stdio.h>
#include <string.h>
#include <string>

#include "vr_overlay.h"
#include "vr_gl.h"

namespace VR
{
namespace Overlay
{

namespace
{

#ifdef UA_GLES
const char *kHdr = "#version 300 es\nprecision highp float;\n";
#else
const char *kHdr = "#version 410 core\n";
#endif

const char *kVs =
    "in vec4 vPos;\n"
    "in vec2 vUV;\n"
    "out vec2 uv;\n"
    "void main() { uv = vUV; gl_Position = vPos; }\n";

// Output is premultiplied (the panel and the eye overlay both blend ONE, ONE_MINUS_SRC_ALPHA).
const char *kFs =
    "uniform sampler2D tex;\n"
    "uniform vec4 col;\n"
    "uniform int style;\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "float band(float d, float a, float b, float aa) { return smoothstep(a - aa, a, d) * (1.0 - smoothstep(b, b + aa, d)); }\n"
    "void main() {\n"
    "  vec2 p = uv * 2.0 - 1.0;\n"
    "  float d = length(p);\n"
    "  float aa = 0.06;\n"
    "  float ink = 0.0, edge = 0.0;\n"
    "  if (style == 0) {\n"
    "    ink = max(band(d, 0.50, 0.66, aa), 1.0 - smoothstep(0.10, 0.10 + aa, d));\n"
    "    float tick = (abs(p.x) < 0.07 && abs(p.y) > 0.72 && abs(p.y) < 0.98) || (abs(p.y) < 0.07 && abs(p.x) > 0.72 && abs(p.x) < 0.98) ? 1.0 : 0.0;\n"
    "    ink = max(ink, tick);\n"
    "    edge = max(band(d, 0.42, 0.74, aa), 1.0 - smoothstep(0.18, 0.18 + aa, d));\n"
    "    edge = max(edge, (abs(p.x) < 0.14 && abs(p.y) > 0.66 && abs(p.y) < 1.0) || (abs(p.y) < 0.14 && abs(p.x) > 0.66 && abs(p.x) < 1.0) ? 1.0 : 0.0);\n"
    "  } else if (style == 1) {\n"
    "    ink = 1.0 - smoothstep(0.2, 1.0, abs(p.x));\n"
    "    edge = 0.0;\n"
    "  } else if (style == 2) {\n"
    "    ink = 1.0 - smoothstep(0.55, 0.55 + aa, d);\n"
    "    edge = 1.0 - smoothstep(0.85, 0.85 + aa, d);\n"
    "  } else {\n"
    "    vec4 t = texture(tex, vec2(uv.x, 1.0 - uv.y));\n"
    "    frag = vec4(t.rgb * t.a, t.a) * col.a;\n"
    "    return;\n"
    "  }\n"
    "  float a = max(ink, edge * 0.75) * col.a;\n"
    "  vec3 c = mix(vec3(0.0), col.rgb, ink / max(max(ink, edge * 0.75), 1e-4));\n"
    "  frag = vec4(c * a, a);\n"
    "}\n";

GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
GLint g_locCol = -1, g_locStyle = -1, g_locTex = -1;
bool g_failed = false;

// saved state
GLint s_prog, s_vao, s_buf, s_tex, s_active, s_srcRGB, s_dstRGB, s_srcA, s_dstA;
GLboolean s_blend, s_depth, s_cull, s_scissor, s_depthMask;

GLuint Compile(GLenum type, const char *body)
{
    std::string src = std::string(kHdr) + body;
    const char *p = src.c_str();
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &p, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        gl.GetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UAVR overlay: shader error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

bool Build()
{
    if (g_prog)
        return true;
    if (g_failed)
        return false;
    g_failed = true;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
        return false;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.BindAttribLocation(p, 0, "vPos");
    gl.BindAttribLocation(p, 1, "vUV");
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        fprintf(stderr, "UAVR overlay: link failed\n");
        return false;
    }
    g_prog = p;
    g_locCol = gl.GetUniformLocation(p, "col");
    g_locStyle = gl.GetUniformLocation(p, "style");
    g_locTex = gl.GetUniformLocation(p, "tex");

    GLint prevVao = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.GenBuffers(1, &g_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, 6 * 6 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
    gl.BindVertexArray(prevVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
    g_failed = false;
    return true;
}

} // namespace

bool Begin()
{
    if (!Build())
        return false;
    glGetIntegerv(GL_CURRENT_PROGRAM, &s_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s_active);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s_srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &s_dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s_dstA);
    s_blend = glIsEnabled(GL_BLEND);
    s_depth = glIsEnabled(GL_DEPTH_TEST);
    s_cull = glIsEnabled(GL_CULL_FACE);
    s_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s_depthMask);

    gl.UseProgram(g_prog);
    gl.BindVertexArray(g_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    gl.Uniform1i(g_locTex, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

void End()
{
    gl.UseProgram(s_prog);
    gl.BindVertexArray(s_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, s_buf);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    gl.ActiveTexture(s_active);
    gl.BlendFuncSeparate(s_srcRGB, s_dstRGB, s_srcA, s_dstA);
    if (s_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (s_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthMask(s_depthMask);
}

void Quad(const float c[4][4], Style style, float r, float g, float b, float a, uint32_t tex)
{
    static const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    float v[6 * 6];
    for (int i = 0; i < 6; i++)
    {
        const int k = idx[i];
        memcpy(v + i * 6, c[k], 4 * sizeof(float));
        v[i * 6 + 4] = uv[k][0];
        v[i * 6 + 5] = uv[k][1];
    }
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
    gl.Uniform4f(g_locCol, r, g, b, a);
    gl.Uniform1i(g_locStyle, (int)style);
    if (style == STYLE_TEXTURE)
        glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

} // namespace Overlay
} // namespace VR
