// Urban Assault VR — desktop simulator backend (Mac, no headset).
//
// Implements VR::Backend with:
//   - two per-eye render targets (default 2064x2208, Quest 3 per-eye panel),
//     optional MSAA (default 4x, resolved in ReleaseEye like an MSAA swapchain);
//   - a head pose from env (fixed yaw/pitch/roll or a scripted sweep) and the
//     mouse while Left/Right Alt is held;
//   - IPD 63 mm, symmetric FOV +-50 h / +-48 v (configurable, per eye mirrored);
//   - an offscreen 2D panel composited into both eyes (cockpit panel or theatre
//     screen), then both eyes blitted side by side into the desktop window;
//   - optional pacing to a target rate and side-by-side PNG captures.
//
// Env (all optional):
//   UA_VR_EYE=2064x2208      per-eye target size
//   UA_VR_MSAA=4             samples (0/1 = off)
//   UA_VR_PANEL=1280x720     2D canvas (HUD/menus) size
//   UA_VR_IPD_MM=63
//   UA_VR_FOV=50,50,48,48    LEFT eye angles in degrees: out(left),in(right),up,down
//                            (right eye mirrored). Quest 3 is roughly 52 out / 52 in.
//   UA_VR_HZ=0               pace frames to this rate (0 = uncapped)
//   UA_VR_SIM_HEAD=yaw,pitch,roll   degrees, yaw + = right, pitch + = up; or "sweep";
//                            or a schedule "frame:yaw,pitch,roll;frame:..." (world frames)
//   UA_VR_SIM_HEADPOS=x,y,z  metres, XR axes (+y up, -z forward)
//   UA_VR_SHOT_DIR=<dir>     PNG captures (side by side, full eye resolution)
//   UA_VR_SHOT_AT=n1,n2,...  capture after these presented WORLD frames
//   UA_VR_SHOT_EVERY=n       ... or every n-th presented world frame
//   UA_VR_SHOT_THEATRE=n     capture the n-th presented frame WITHOUT a 3D world
//   UA_VR_SIM_RAIM=yaw,pitch right-hand aim ray (degrees, from the hand at 0.2,-0.35,-0.35 m);
//                            or a schedule "frame:yaw,pitch;frame:yaw,pitch;..." (controller
//                            polls). Makes both hands active even without a gamepad.
//   UA_VR_SIM_PRESS=f0-f1:BTN,...  scripted presses over poll frames f0..f1; BTN = A B X Y
//                            MENU RT LT RG LG, sticks RSU RSD RSL RSR LSU LSD LSL LSR,
//                            OFF = both controllers inactive
//   UA_VR_SIM_STICK=lx,ly[,n] scripted left stick from world frame n (unattended drives)
// UA_VR_SIM_LWRIST=x,y,z,yaw,pitch,roll left grip pose (cockpit wrist panel captures)
//   UA_VR_SIM_RPOS / UA_VR_SIM_LPOS="frame:x,y,z;frame:x,y,z;..."  hand position schedule
// (XR metres, controller polls, linear between keys; table grab /
//                            token pick-up tests). Moves the aim and grip pose together.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include <SDL2/SDL_image.h>

#include "vr.h"
#include "vr_gl.h"
#include "vr_math.h"

namespace VR
{

namespace
{

const double kDeg = M_PI / 180.0;

bool ParseWxH(const char *v, int *w, int *h)
{
    int a = 0, b = 0;
    if (v && sscanf(v, "%dx%d", &a, &b) == 2 && a > 0 && b > 0)
    {
        *w = a;
        *h = b;
        return true;
    }
    return false;
}

struct Target
{
    GLuint fbo = 0, tex = 0, depth = 0;       // resolved / single-sample
    GLuint msFbo = 0, msColor = 0, msDepth = 0; // multisample (optional)
    int w = 0, h = 0;
};

bool MakeTarget(Target *t, int w, int h, int samples)
{
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    gl.GenRenderbuffers(1, &t->depth);
    gl.BindRenderbuffer(GL_RENDERBUFFER, t->depth);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    gl.GenFramebuffers(1, &t->fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    bool ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (ok && samples > 1)
    {
        gl.GenRenderbuffers(1, &t->msColor);
        gl.BindRenderbuffer(GL_RENDERBUFFER, t->msColor);
        gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        gl.GenRenderbuffers(1, &t->msDepth);
        gl.BindRenderbuffer(GL_RENDERBUFFER, t->msDepth);
        gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
        gl.GenFramebuffers(1, &t->msFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, t->msFbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->msColor);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->msDepth);
        ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ok)
    {
        // start transparent/black
        gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    return ok;
}

const char *kVs =
    "#version 410 core\n"
    "uniform mat4 MVP;\n"
    "uniform vec4 uvRect;\n" // texture sub-rect (offset, size); (0,0,1,1) = the whole panel
    "in vec3 vPos;\n"
    "in vec2 vUV;\n"
    "out vec2 uv;\n"
    "void main() { uv = uvRect.xy + vUV * uvRect.zw; gl_Position = MVP * vec4(vPos, 1.0); }\n";

const char *kFs =
    "#version 410 core\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 tint;\n" // rgb multiply, a: 1 = premultiplied panel, 0 = solid colour
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "void main() { if (tint.a > 0.5) frag = texture(tex, uv) * vec4(tint.rgb, 1.0); else frag = vec4(tint.rgb, 1.0); }\n";

GLuint Compile(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
#ifdef UA_GLES
    // Mac ANGLE test build: same shader as GLSL ES 3.00.
    std::string es = std::string("#version 300 es\nprecision highp float;\n") + (strchr(src, '\n') + 1);
    src = es.c_str();
#endif
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        gl.GetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UAVR sim: shader error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

// Standard OpenXR/GL projection (XR view space, -Z forward), row-major.
M4 XrProjection(const Fovf &f, double zn, double zf)
{
    const double l = tan(f.angleLeft), r = tan(f.angleRight), u = tan(f.angleUp), d = tan(f.angleDown);
    M4 p;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) p.m[i][j] = 0;
    p.m[0][0] = 2.0 / (r - l);
    p.m[0][2] = (r + l) / (r - l);
    p.m[1][1] = 2.0 / (u - d);
    p.m[1][2] = (u + d) / (u - d);
    p.m[2][2] = -(zf + zn) / (zf - zn);
    p.m[2][3] = -2.0 * zf * zn / (zf - zn);
    p.m[3][2] = -1.0;
    return p;
}

M4 PoseMatrix(const Posef &p) // local -> parent
{
    M3 r = QuatToM3(p.orientation);
    M4 m = M4::Ident();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m.m[i][j] = r.m[i][j];
    m.m[0][3] = p.position.x;
    m.m[1][3] = p.position.y;
    m.m[2][3] = p.position.z;
    return m;
}

M4 InversePose(const M4 &m) // rigid inverse
{
    M4 o = M4::Ident();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            o.m[i][j] = m.m[j][i];
    for (int i = 0; i < 3; i++)
        o.m[i][3] = -(o.m[i][0] * m.m[0][3] + o.m[i][1] * m.m[1][3] + o.m[i][2] * m.m[2][3]);
    return o;
}

class SimBackend : public Backend
{
public:
    const char *Name() const override { return "sim"; }

    bool Init() override
    {
        ParseWxH(getenv("UA_VR_EYE"), &_eyeW, &_eyeH);
        ParseWxH(getenv("UA_VR_PANEL"), &_panelW, &_panelH);
        if (const char *v = getenv("UA_VR_MSAA"))
            _samples = atoi(v);
        GLint maxS = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxS);
        if (_samples > maxS)
            _samples = maxS;
        if (const char *v = getenv("UA_VR_IPD_MM"))
            _ipdM = atof(v) / 1000.0;
        if (const char *v = getenv("UA_VR_FOV"))
            sscanf(v, "%f,%f,%f,%f", &_fovOut, &_fovIn, &_fovUp, &_fovDown);
        if (const char *v = getenv("UA_VR_HZ"))
            _hz = atof(v);
        if (const char *v = getenv("UA_VR_SIM_HEAD"))
        {
            if (!strcmp(v, "sweep"))
                _sweep = true;
            else if (strchr(v, ':'))
            {
                // schedule "frame:yaw,pitch,roll;frame:..." (world frames; view captures)
                for (const char *p = v; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL)
                {
                    HeadKey k;
                    if (sscanf(p, "%d:%f,%f,%f", &k.frame, &k.yaw, &k.pitch, &k.roll) >= 3)
                        _headKeys.push_back(k);
                }
            }
            else
                sscanf(v, "%f,%f,%f", &_yaw, &_pitch, &_roll);
        }
        if (const char *v = getenv("UA_VR_SIM_HEADPOS"))
            sscanf(v, "%f,%f,%f", &_headPos.x, &_headPos.y, &_headPos.z);
        if (const char *v = getenv("UA_VR_SHOT_DIR"))
            _shotDir = v;
        if (const char *v = getenv("UA_VR_SHOT_THEATRE"))
            _shotTheatre = atoi(v);
        if (const char *v = getenv("UA_VR_SHOT_EVERY"))
            _shotEvery = atoi(v);
        if (const char *v = getenv("UA_VR_SHOT_AT"))
        {
            std::string s(v);
            size_t at = 0;
            while (at < s.size())
            {
                size_t c = s.find(',', at);
                _shotAt.push_back(atoi(s.substr(at, c == std::string::npos ? std::string::npos : c - at).c_str()));
                if (c == std::string::npos)
                    break;
                at = c + 1;
            }
        }

        for (int e = 0; e < 2; e++)
            if (!MakeTarget(&_eye[e], _eyeW, _eyeH, _samples))
            {
                fprintf(stderr, "UAVR sim: eye target %dx%d (msaa %d) incomplete\n", _eyeW, _eyeH, _samples);
                return false;
            }
        if (!MakeTarget(&_panel, _panelW, _panelH, 0))
            return false;

        // compositor program + quad
        GLuint vs = Compile(GL_VERTEX_SHADER, kVs), fs = Compile(GL_FRAGMENT_SHADER, kFs);
        if (!vs || !fs)
            return false;
        _prog = gl.CreateProgram();
        gl.AttachShader(_prog, vs);
        gl.AttachShader(_prog, fs);
        gl.BindAttribLocation(_prog, 0, "vPos");
        gl.BindAttribLocation(_prog, 1, "vUV");
        gl.LinkProgram(_prog);
        GLint ok = 0;
        gl.GetProgramiv(_prog, GL_LINK_STATUS, &ok);
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        if (!ok)
            return false;
        _locMVP = gl.GetUniformLocation(_prog, "MVP");
        _locTint = gl.GetUniformLocation(_prog, "tint");
        _locUv = gl.GetUniformLocation(_prog, "uvRect");
        _locTex = gl.GetUniformLocation(_prog, "tex");

        GLint prevVao = 0, prevBuf = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        gl.GenVertexArrays(1, &_vao);
        gl.BindVertexArray(_vao);
        gl.GenBuffers(1, &_vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, _vbo);
        // unit quad in the panel's local frame: x,y in [-0.5,0.5], z = 0; uv
        static const float quad[] = {
            -0.5f, -0.5f, 0.f, 0.f, 0.f,
             0.5f, -0.5f, 0.f, 1.f, 0.f,
             0.5f,  0.5f, 0.f, 1.f, 1.f,
            -0.5f, -0.5f, 0.f, 0.f, 0.f,
             0.5f,  0.5f, 0.f, 1.f, 1.f,
            -0.5f,  0.5f, 0.f, 0.f, 1.f,
        };
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
        gl.EnableVertexAttribArray(1);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
        gl.BindVertexArray(prevVao);
        gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);

        ParseScript();
        SDL_AddEventWatch(&SimBackend::EventWatch, this);
        _t0 = SDL_GetPerformanceCounter();
        _next = 0;
        fprintf(stderr, "UAVR sim: eye %dx%d msaa %d, IPD %.1f mm, FOV out %.0f in %.0f up %.0f down %.0f deg, hz %.0f, head %s%.0f/%.0f/%.0f\n",
                _eyeW, _eyeH, _samples, _ipdM * 1000.0, _fovOut, _fovIn, _fovUp, _fovDown, _hz,
                _sweep ? "sweep " : "", _yaw, _pitch, _roll);
        return true;
    }

    void Shutdown() override
    {
        SDL_DelEventWatch(&SimBackend::EventWatch, this);
    }

    bool BeginFrame(FrameInfo *out) override
    {
        const double freq = (double)SDL_GetPerformanceFrequency();
        uint64_t now = SDL_GetPerformanceCounter();
        if (_hz > 0.0)
        {
            const uint64_t period = (uint64_t)(freq / _hz);
            if (_next == 0 || now > _next + period)
                _next = now;
            while (now < _next)
            {
                const double leftMs = (_next - now) * 1000.0 / freq;
                if (leftMs > 2.0)
                    SDL_Delay((uint32_t)(leftMs - 1.0));
                now = SDL_GetPerformanceCounter();
            }
            _next += period;
            out->predictedDisplayPeriodNs = (int64_t)(1.0e9 / _hz);
        }
        else
            out->predictedDisplayPeriodNs = 0;
        out->shouldRender = true;
        out->predictedDisplayTimeNs = (int64_t)((now - _t0) * 1.0e9 / freq) + out->predictedDisplayPeriodNs;
        return true;
    }

    bool LocateViews(int64_t t, ViewState *out) override
    {
        for (const HeadKey &k : _headKeys)
            if (_worldFrames >= k.frame)
                _yaw = k.yaw, _pitch = k.pitch, _roll = k.roll;
        float yaw = _yaw + _mouseYaw, pitch = _pitch + _mousePitch, roll = _roll;
        if (_sweep)
        {
            const double s = t / 1.0e9;
            yaw += 40.0 * sin(2.0 * M_PI * s / 6.0);
            pitch += 10.0 * sin(2.0 * M_PI * s / 4.1);
        }
        // yaw + = turn right = negative rotation about +Y (XR up)
        Quatf q = QuatMul(QuatFromAxisAngle(0, 1, 0, -yaw * kDeg),
                          QuatMul(QuatFromAxisAngle(1, 0, 0, pitch * kDeg),
                                  QuatFromAxisAngle(0, 0, -1, roll * kDeg)));
        out->valid = true;
        out->head.orientation = q;
        out->head.position = _headPos;
        for (int e = 0; e < 2; e++)
        {
            EyeView &v = out->eye[e];
            v.pose = Posef();
            v.pose.position.x = (e == 0 ? -0.5f : 0.5f) * _ipdM;
            const float out_ = _fovOut * kDeg, in_ = _fovIn * kDeg;
            v.fov.angleLeft = e == 0 ? -out_ : -in_;
            v.fov.angleRight = e == 0 ? in_ : out_;
            v.fov.angleUp = _fovUp * kDeg;
            v.fov.angleDown = -_fovDown * kDeg;
        }
        _lastViews = *out;
        return true;
    }

    RenderTarget AcquireEye(int eye) override
    {
        const Target &t = _eye[eye];
        RenderTarget r;
        r.fbo = t.msFbo ? t.msFbo : t.fbo;
        r.colorTex = t.tex;
        r.width = t.w;
        r.height = t.h;
        return r;
    }

    void ReleaseEye(int eye) override
    {
        const Target &t = _eye[eye];
        if (!t.msFbo)
            return;
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.msFbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
        gl.BlitFramebuffer(0, 0, t.w, t.h, 0, 0, t.w, t.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    RenderTarget PanelTarget() override
    {
        RenderTarget r;
        r.fbo = _panel.fbo;
        r.colorTex = _panel.tex;
        r.width = _panel.w;
        r.height = _panel.h;
        return r;
    }

    void EndFrame(const FrameEnd &end) override
    {
        // Save the GL state the engine's state cache relies on.
        GLint prevProg = 0, prevVao = 0, prevTex = 0, prevActive = 0, prevBuf = 0;
        GLint prevSrcRGB = 0, prevDstRGB = 0, prevSrcA = 0, prevDstA = 0;
        GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
        GLboolean depthMask = GL_TRUE;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        gl.ActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

        gl.UseProgram(_prog);
        gl.BindVertexArray(_vao);
        gl.Uniform1i(_locTex, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);

        const M4 head = PoseMatrix(_lastViews.head);
        const M4 model = PoseMatrix(end.panel.pose) * Scale(end.panel.widthM, end.panel.heightM);

        for (int e = 0; e < 2; e++)
        {
            Target &t = _eye[e];
            gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glViewport(0, 0, t.w, t.h);
            if (!end.worldRendered)
            {
                glClearColor(0.035f, 0.035f, 0.04f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                // theatre: a dark frame slightly larger than the screen
                glDisable(GL_BLEND);
                const M4 frame = PoseMatrix(end.panel.pose) * Translate(0, 0, -0.01) *
                                 Scale(end.panel.widthM * 1.04, end.panel.heightM * 1.06);
                DrawQuad(e, head, frame, 0.0f, 0.10f, 0.10f, 0.11f);
            }
            glEnable(GL_BLEND);
            gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBindTexture(GL_TEXTURE_2D, _panel.tex);
            if (end.split)
            {
                // PANELS FREE: two quads, each its rows of the panel (GL rows count from the bottom)
                for (int k = 0; k < 2; k++)
                {
                    const PanelLayer &P = k ? end.panel2 : end.panel;
                    const float uvr[4] = {0.f, 1.f - P.v1, 1.f, P.v1 - P.v0};
                    DrawQuad(e, head, PoseMatrix(P.pose) * Scale(P.widthM, P.heightM), 1.0f, 1.0f, 1.0f, 1.0f, uvr);
                }
            }
            else
                DrawQuad(e, head, model, 1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (end.worldRendered)
            _worldFrames++;
        else
            _flatFrames++;
        MaybeShot(end.worldRendered);

        // Side by side into the window, letterboxed.
        int dw = 0, dh = 0;
        SDL_Window *win = SDL_GL_GetCurrentWindow();
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, dw, dh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        const double sc = std::min((dw / 2.0) / _eyeW, (double)dh / _eyeH);
        const int ow = (int)(_eyeW * sc), oh = (int)(_eyeH * sc);
        for (int e = 0; e < 2; e++)
        {
            const int x0 = e * dw / 2 + (dw / 2 - ow) / 2, y0 = (dh - oh) / 2;
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, _eye[e].fbo);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            gl.BlitFramebuffer(0, 0, _eyeW, _eyeH, x0, y0, x0 + ow, y0 + oh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

        SDL_GL_SwapWindow(win);

        // Restore.
        gl.UseProgram(prevProg);
        gl.BindVertexArray(prevVao);
        glBindTexture(GL_TEXTURE_2D, prevTex);
        gl.ActiveTexture(prevActive);
        gl.BindBuffer(GL_ARRAY_BUFFER, prevBuf);
        gl.BlendFuncSeparate(prevSrcRGB, prevDstRGB, prevSrcA, prevDstA);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        glDepthMask(depthMask);
    }

    void PollControllers(ControllerState out[2]) override
    {
        // The simulator has no tracked controllers; a connected gamepad stands
        // in for both hands (left stick/trigger/shoulder -> left, right -> right).
        out[0] = ControllerState();
        out[1] = ControllerState();
        // UA_VR_SIM_STICK=lx,ly[,fromFrame]: scripted left stick (unattended drive
        // captures, comfort); active from the given world frame (default 0).
        static float stickX = 0, stickY = 0;
        static int stickFrom = -1;
        if (stickFrom == -1)
        {
            stickFrom = -2;
            if (const char *v = getenv("UA_VR_SIM_STICK"))
            {
                int from = 0;
                if (sscanf(v, "%f,%f,%d", &stickX, &stickY, &from) >= 2)
                    stickFrom = from;
            }
        }
        if (stickFrom >= 0)
        {
            for (int h = 0; h < 2; h++)
            {
                ControllerState &c = out[h];
                c.active = true;
                c.aimPose.position.x = c.gripPose.position.x = h ? 0.2f : -0.2f;
                c.aimPose.position.y = c.gripPose.position.y = -0.35f;
                c.aimPose.position.z = c.gripPose.position.z = -0.35f;
            }
            if (_worldFrames >= stickFrom)
            {
                out[0].stickX = stickX;
                out[0].stickY = stickY;
            }
            Script(out); // merge fix: scripted drive + RAIM/PRESS fire together
            return;
        }
        if (!_pad)
        {
            for (int i = 0; i < SDL_NumJoysticks() && !_pad; i++)
                if (SDL_IsGameController(i))
                    _pad = SDL_GameControllerOpen(i);
        }
        if (!_pad)
        {
            if (!_scripted)
                return;
            for (int h = 0; h < 2; h++)
            {
                ControllerState &c = out[h];
                c.active = true;
                c.aimPose.position.x = c.gripPose.position.x = h ? 0.2f : -0.2f;
                c.aimPose.position.y = c.gripPose.position.y = -0.35f;
                c.aimPose.position.z = c.gripPose.position.z = -0.35f;
            }
            Script(out);
            return;
        }
        auto ax = [this](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(_pad, a) / 32767.0f; };
        auto bt = [this](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(_pad, b) != 0; };
        for (int h = 0; h < 2; h++)
        {
            ControllerState &c = out[h];
            c.active = true;
            c.stickX = ax(h ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
            c.stickY = -ax(h ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
            c.trigger = ax(h ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            c.grip = bt(h ? SDL_CONTROLLER_BUTTON_RIGHTSHOULDER : SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 1.0f : 0.0f;
            if (h) { if (bt(SDL_CONTROLLER_BUTTON_A)) c.buttons |= BTN_A; if (bt(SDL_CONTROLLER_BUTTON_B)) c.buttons |= BTN_B; }
            else   { if (bt(SDL_CONTROLLER_BUTTON_X)) c.buttons |= BTN_X; if (bt(SDL_CONTROLLER_BUTTON_Y)) c.buttons |= BTN_Y; }
            if (bt(h ? SDL_CONTROLLER_BUTTON_RIGHTSTICK : SDL_CONTROLLER_BUTTON_LEFTSTICK)) c.buttons |= BTN_STICK_CLICK;
            if (!h && bt(SDL_CONTROLLER_BUTTON_START)) c.buttons |= BTN_MENU;
            c.aimPose.position.x = c.gripPose.position.x = h ? 0.2f : -0.2f;
            c.aimPose.position.y = c.gripPose.position.y = -0.35f;
            c.aimPose.position.z = c.gripPose.position.z = -0.35f;
        }
        Script(out);
    }

    // UA_VR_SIM_RAIM / UA_VR_SIM_PRESS (unattended pointer and fire tests).
    void Script(ControllerState out[2])
    {
        _poll++;
        if (_lwrist)
            out[0].gripPose = out[0].aimPose = _lwristPose;
        for (int h = 0; h < 2; h++)
        {
            const std::vector<PosKey> &k = _posSched[h];
            if (k.empty() || _poll < k[0].frame)
                continue;
            double p[3] = {k.back().p[0], k.back().p[1], k.back().p[2]};
            for (size_t i = 0; i + 1 < k.size(); i++)
                if (_poll >= k[i].frame && _poll < k[i + 1].frame)
                {
                    const double a = ((double)_poll - k[i].frame) / std::max(1.0, (double)k[i + 1].frame - (double)k[i].frame);
                    for (int j = 0; j < 3; j++)
                        p[j] = (double)k[i].p[j] * (1.0 - a) + (double)k[i + 1].p[j] * a;
                    break;
                }
            if (!(std::isfinite((float)p[0]) && std::isfinite((float)p[1]) && std::isfinite((float)p[2])))
                continue; // never hand a nonfinite pose to the game
            out[h].aimPose.position.x = out[h].gripPose.position.x = (float)p[0];
            out[h].aimPose.position.y = out[h].gripPose.position.y = (float)p[1];
            out[h].aimPose.position.z = out[h].gripPose.position.z = (float)p[2];
        }
        if (!_aimSched.empty())
        {
            float yaw = 0, pitch = 0;
            for (const AimKey &k : _aimSched)
                if (_poll >= k.frame) { yaw = k.yaw; pitch = k.pitch; }
            out[1].aimPose.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, -yaw * kDeg), QuatFromAxisAngle(1, 0, 0, pitch * kDeg));
        }
        for (const Press &p : _press)
        {
            if (_poll < p.f0 || _poll > p.f1)
                continue;
            const std::string &b = p.btn;
            if (b == "A") out[1].buttons |= BTN_A;
            else if (b == "B") out[1].buttons |= BTN_B;
            else if (b == "X") out[0].buttons |= BTN_X;
            else if (b == "Y") out[0].buttons |= BTN_Y;
            else if (b == "MENU") out[0].buttons |= BTN_MENU;
            else if (b == "RT") out[1].trigger = 1.0f;
            else if (b == "LT") out[0].trigger = 1.0f;
            else if (b == "RG") out[1].grip = 1.0f;
            else if (b == "LG") out[0].grip = 1.0f;
            else if (b == "RSU") out[1].stickY = 1.0f;
            else if (b == "RSD") out[1].stickY = -1.0f;
            else if (b == "LSU") out[0].stickY = 1.0f;
            else if (b == "LSD") out[0].stickY = -1.0f;
            else if (b == "OFF") { out[0].active = false; out[1].active = false; } // controllers lost
            else if (b == "LSL") out[0].stickX = -1.0f;
            else if (b == "LSR") out[0].stickX = 1.0f;
            else if (b == "RSC") out[1].buttons |= BTN_STICK_CLICK; // stick press (diorama placement)
            else if (b == "LSC") out[0].buttons |= BTN_STICK_CLICK;
            else if (b == "RSL") out[1].stickX = -1.0f;
            else if (b == "RSR") out[1].stickX = 1.0f;
        }
    }

    void ParseScript()
    {
        // UA_VR_SIM_LWRIST=x,y,z,yaw,pitch,roll: left grip pose (XR metres, degrees,
        // R = Ry(yaw) Rx(pitch) Rz(roll)) for wrist-panel captures (cockpit).
        if (const char *v = getenv("UA_VR_SIM_LWRIST"))
        {
            float x, y, z, yw = 0, pt = 0, rl = 0;
            if (sscanf(v, "%f,%f,%f,%f,%f,%f", &x, &y, &z, &yw, &pt, &rl) >= 3)
            {
                _lwrist = true;
                _lwristPose.position.x = x; _lwristPose.position.y = y; _lwristPose.position.z = z;
                _lwristPose.orientation = QuatMul(QuatFromAxisAngle(0, 1, 0, yw * kDeg),
                                                  QuatMul(QuatFromAxisAngle(1, 0, 0, pt * kDeg), QuatFromAxisAngle(0, 0, 1, rl * kDeg)));
                _scripted = true;
            }
        }
        for (int h = 0; h < 2; h++)
            if (const char *v = getenv(h ? "UA_VR_SIM_RPOS" : "UA_VR_SIM_LPOS"))
            {
                std::string s(v);
                size_t at = 0;
                while (at < s.size())
                {
                    size_t e = s.find(';', at);
                    const std::string seg = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
                    PosKey k;
                    if (sscanf(seg.c_str(), "%d:%f,%f,%f", &k.frame, &k.p[0], &k.p[1], &k.p[2]) == 4 &&
                        std::isfinite(k.p[0]) && std::isfinite(k.p[1]) && std::isfinite(k.p[2]) &&
                        (_posSched[h].empty() || k.frame > _posSched[h].back().frame)) // finite, frames ascending
                        _posSched[h].push_back(k);
                    if (e == std::string::npos)
                        break;
                    at = e + 1;
                }
                _scripted = true;
            }
        if (const char *v = getenv("UA_VR_SIM_RAIM"))
        {
            std::string s(v);
            size_t at = 0;
            while (at < s.size())
            {
                size_t e = s.find(';', at);
                std::string seg = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
                AimKey k;
                if (seg.find(':') != std::string::npos)
                    sscanf(seg.c_str(), "%d:%f,%f", &k.frame, &k.yaw, &k.pitch);
                else
                    sscanf(seg.c_str(), "%f,%f", &k.yaw, &k.pitch);
                _aimSched.push_back(k);
                if (e == std::string::npos)
                    break;
                at = e + 1;
            }
            _scripted = true;
        }
        if (const char *v = getenv("UA_VR_SIM_PRESS"))
        {
            std::string s(v);
            size_t at = 0;
            while (at < s.size())
            {
                size_t e = s.find(',', at);
                std::string seg = s.substr(at, e == std::string::npos ? std::string::npos : e - at);
                Press p;
                char btn[16] = {0};
                if (sscanf(seg.c_str(), "%d-%d:%15s", &p.f0, &p.f1, btn) == 3)
                {
                    p.btn = btn;
                    _press.push_back(p);
                }
                if (e == std::string::npos)
                    break;
                at = e + 1;
            }
            _scripted = true;
        }
    }

    void EyeSize(int *w, int *h) const override
    {
        *w = _eyeW;
        *h = _eyeH;
    }

    // the Settings DISPLAY row paces the simulator like a headset rate
    // (0 = back to the start-up pacing, UA_VR_HZ or unpaced).
    int RefreshRates(float *out, int max) override
    {
        static const float r[3] = {72.f, 90.f, 120.f};
        int n = 0;
        for (; n < 3 && n < max; n++)
            out[n] = r[n];
        return n;
    }
    float RefreshHz() const override { return (float)_hz; }
    bool RequestRefreshRate(float hz) override
    {
        if (_hzStart < 0.0)
            _hzStart = _hz;
        _hz = hz > 0.0f ? hz : _hzStart;
        _next = 0;
        fprintf(stderr, "UAVR sim: pacing %.0f Hz%s\n", _hz, _hz > 0.0 ? "" : " (unpaced)");
        return true;
    }

private:
    static M4 Scale(double sx, double sy)
    {
        M4 m = M4::Ident();
        m.m[0][0] = sx;
        m.m[1][1] = sy;
        return m;
    }

    static M4 Translate(double x, double y, double z)
    {
        M4 m = M4::Ident();
        m.m[0][3] = x;
        m.m[1][3] = y;
        m.m[2][3] = z;
        return m;
    }

    void DrawQuad(int eye, const M4 &head, const M4 &model, float textured, float r, float g, float b, const float *uvr = NULL)
    {
        static const float kWhole[4] = {0.f, 0.f, 1.f, 1.f};
        const float *uv = uvr ? uvr : kWhole;
        const EyeView &ev = _lastViews.eye[eye];
        const M4 eyeWorld = head * PoseMatrix(ev.pose);
        const M4 mvp = XrProjection(ev.fov, 0.05, 100.0) * InversePose(eyeWorld) * model;
        float m[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                m[j * 4 + i] = (float)mvp.m[i][j]; // column-major
        gl.UniformMatrix4fv(_locMVP, 1, GL_FALSE, m);
        gl.Uniform4f(_locTint, r, g, b, textured);
        gl.Uniform4f(_locUv, uv[0], uv[1], uv[2], uv[3]);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    void MaybeShot(bool world)
    {
        if (_shotDir.empty())
            return;
        bool take = false;
        if (!world)
        {
            // UA_VR_SHOT_THEATRE=n: one capture of the n-th presented frame
            // without a 3D world (menus / briefing / FMV on the theatre screen).
            if (_shotTheatre > 0 && _flatFrames == _shotTheatre)
                take = true;
            if (!take)
                return;
        }
        else
        {
            if (_shotEvery > 0 && (_worldFrames % _shotEvery) == 0)
                take = true;
            for (int n : _shotAt)
                if (n == _worldFrames)
                    take = true;
        }
        if (!take)
            return;

        const int W = _eyeW * 2, H = _eyeH;
        std::vector<uint8_t> px((size_t)W * H * 4);
        std::vector<uint8_t> row((size_t)_eyeW * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, W);
        for (int e = 0; e < 2; e++)
        {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, _eye[e].fbo);
            glReadPixels(0, 0, _eyeW, _eyeH, GL_RGBA, GL_UNSIGNED_BYTE, px.data() + e * _eyeW * 4);
        }
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ABGR8888);
        if (!s)
            return;
        for (int y = 0; y < H; y++)
        {
            uint8_t *dst = (uint8_t *)s->pixels + (size_t)y * s->pitch;
            const uint8_t *src = px.data() + (size_t)(H - 1 - y) * W * 4;
            memcpy(dst, src, (size_t)W * 4);
            for (int x = 0; x < W; x++)
                dst[x * 4 + 3] = 255;
        }
        char name[512];
        if (world)
            snprintf(name, sizeof(name), "%s/vr_sbs_%05d.png", _shotDir.c_str(), _worldFrames);
        else
            snprintf(name, sizeof(name), "%s/vr_theatre_%05d.png", _shotDir.c_str(), _flatFrames);
        if (IMG_SavePNG(s, name) == 0)
            fprintf(stderr, "UAVR sim: saved %s (%dx%d)\n", name, W, H);
        else
            fprintf(stderr, "UAVR sim: PNG save failed %s: %s\n", name, IMG_GetError());
        SDL_FreeSurface(s);
    }

    static int EventWatch(void *ud, SDL_Event *ev)
    {
        SimBackend *self = (SimBackend *)ud;
        if (ev->type == SDL_MOUSEMOTION && (SDL_GetModState() & KMOD_ALT))
        {
            self->_mouseYaw += ev->motion.xrel * 0.15f;
            self->_mousePitch -= ev->motion.yrel * 0.15f;
            if (self->_mousePitch > 80.f) self->_mousePitch = 80.f;
            if (self->_mousePitch < -80.f) self->_mousePitch = -80.f;
        }
        return 1;
    }

    int _eyeW = 2064, _eyeH = 2208, _samples = 4;
    int _panelW = 1280, _panelH = 720;
    double _ipdM = 0.063;
    float _fovOut = 50.f, _fovIn = 50.f, _fovUp = 48.f, _fovDown = 48.f;
    double _hz = 0.0, _hzStart = -1.0;
    bool _sweep = false;
    struct HeadKey { int frame = 0; float yaw = 0, pitch = 0, roll = 0; };
    std::vector<HeadKey> _headKeys; // UA_VR_SIM_HEAD schedule
    float _yaw = 0.f, _pitch = 0.f, _roll = 0.f;
    float _mouseYaw = 0.f, _mousePitch = 0.f;
    Vec3f _headPos;
    uint64_t _t0 = 0, _next = 0;

    Target _eye[2];
    Target _panel;
    GLuint _prog = 0, _vao = 0, _vbo = 0;
    GLint _locMVP = -1, _locTint = -1, _locTex = -1, _locUv = -1;
    ViewState _lastViews;
    SDL_GameController *_pad = NULL;
    struct AimKey { int frame = 0; float yaw = 0, pitch = 0; };
    struct Press { int f0 = 0, f1 = 0; std::string btn; };
    std::vector<AimKey> _aimSched;
    std::vector<Press> _press;
    struct PosKey { int frame = 0; float p[3] = {0, 0, 0}; };
    std::vector<PosKey> _posSched[2]; // UA_VR_SIM_LPOS / UA_VR_SIM_RPOS
    bool _scripted = false;
    bool _lwrist = false; // UA_VR_SIM_LWRIST
    Posef _lwristPose;
    int _poll = 0;

    std::string _shotDir;
    int _shotEvery = 0;
    std::vector<int> _shotAt;
    int _worldFrames = 0, _flatFrames = 0, _shotTheatre = 0;
};

} // namespace

Backend *CreateSimBackend() { return new SimBackend(); }

} // namespace VR
