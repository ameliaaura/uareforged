// Urban Assault VR - OpenXR backend for Quest 3 standalone (Android, GLES 3.2).
//
// Implements VR::Backend (vr.h) on top of the NativeActivity shell in platform/quest
// (XrPlatform: instance, session, seated LOCAL space, events, input actions, refresh rate,
// perf levels). Mapping,:
//
//   BeginFrame   -> PumpEvents (looper + xrPollEvent; blocks while the session is not running)
//                   + xrWaitFrame + xrBeginFrame + xrSyncActions
//   LocateViews  -> xrLocateViews (LOCAL) + head = VIEW space in LOCAL; eye poses are passed
//                   to the engine relative to the head, both from the same late sample
//   AcquireEye   -> one GL_TEXTURE_2D swapchain per eye; the engine's per-eye draw path is
// two passes, so no multiview (optimisation). MSAA through
//                   GL_EXT_multisampled_render_to_texture (resolve on tile store, no blit)
//   ReleaseEye   -> depth invalidated (never stored), xrReleaseSwapchainImage
//   PanelTarget  -> persistent RGBA8 FBO the engine draws all 2D into
//   EndFrame     -> panel blitted into the quad-layer swapchain; projection layer (if the world
//                   was drawn this frame) + XrCompositionLayerQuad (cockpit panel / theatre)
//   Poll/Haptic  -> Oculus Touch actions / xrApplyHapticFeedback
//
// Colour: the engine writes display-referred (sRGB-encoded) values. Swapchains are created
// GL_SRGB8_ALPHA8 and sRGB write conversion is switched off (GL_EXT_sRGB_write_control), so the
// stored bytes are the engine's and the compositor decodes them correctly. Without that
// extension (or UA_VR_SWAPCHAIN=unorm) GL_RGBA8 is used.
//
// Env: UA_VR_MSAA (default 4), UA_VR_EYE_SCALE (default 1.0 x recommended), UA_VR_PANEL
// (default 1280x720), UA_VR_FOVEATION (0 off .. 3 high, default 2), UA_VR_SWAPCHAIN.

#ifdef __ANDROID__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <algorithm>
#include <vector>

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include "vr.h"
#include "vr_gl.h"

#include "platform/android/ua_host.h"
#include "platform/xr/xr_platform.h"

namespace VR
{

namespace
{

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC pFbTex2DMs = NULL;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC pRbStorageMs = NULL;

bool HasGlExt(const char *name)
{
    const char *e = (const char *)glGetString(GL_EXTENSIONS);
    return e && strstr(e, name);
}

int EnvI(const char *n, int def) { const char *v = getenv(n); return (v && v[0]) ? atoi(v) : def; }
float EnvF(const char *n, float def) { const char *v = getenv(n); return (v && v[0]) ? (float)atof(v) : def; }

Quatf ToQ(const XrQuaternionf &q) { Quatf o; o.x = q.x; o.y = q.y; o.z = q.z; o.w = q.w; return o; }
Vec3f ToV(const XrVector3f &v) { Vec3f o; o.x = v.x; o.y = v.y; o.z = v.z; return o; }

XrQuaternionf QMul(const XrQuaternionf &a, const XrQuaternionf &b)
{
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

XrQuaternionf QConj(const XrQuaternionf &q) { XrQuaternionf r = {-q.x, -q.y, -q.z, q.w}; return r; }

XrVector3f QRot(const XrQuaternionf &q, const XrVector3f &v)
{
    XrQuaternionf p = {v.x, v.y, v.z, 0.f};
    XrQuaternionf r = QMul(QMul(q, p), QConj(q));
    XrVector3f o = {r.x, r.y, r.z};
    return o;
}

struct EyeChain
{
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;  // one per image
    GLuint depth = 0;          // renderbuffer (multisampled when msaa > 1)
    uint32_t acquired = 0;
    bool holding = false;
};

class OpenXRBackend : public Backend
{
public:
    const char *Name() const override { return "openxr"; }

    bool Init() override
    {
        uavr::Host *h = uavr::GetHost();
        if (!h || !h->xr || h->xr->session() == XR_NULL_HANDLE)
        {
            fprintf(stderr, "UAVR: openxr backend: no OpenXR session from the shell\n");
            return false;
        }
        _xr = h->xr;
        _inst = _xr->instance();
        _sess = _xr->session();
        _space = _xr->appSpace();

        XrReferenceSpaceCreateInfo vci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        vci.poseInReferenceSpace.orientation.w = 1.f;
        if (XR_FAILED(xrCreateReferenceSpace(_sess, &vci, &_viewSpace)))
            return false;

        uint32_t n = 0;
        xrEnumerateViewConfigurationViews(_inst, _xr->systemId(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, NULL);
        std::vector<XrViewConfigurationView> vcv(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(_inst, _xr->systemId(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vcv.data());
        if (n != 2)
            return false;
        const float scale = std::max(0.3f, std::min(1.5f, EnvF("UA_VR_EYE_SCALE", 1.0f)));
        _w = (int)(vcv[0].recommendedImageRectWidth * scale) & ~1;
        _h = (int)(vcv[0].recommendedImageRectHeight * scale) & ~1;
        _w = std::min<int>(_w, vcv[0].maxImageRectWidth);
        _h = std::min<int>(_h, vcv[0].maxImageRectHeight);

        // Colour format (see the header comment).
        xrEnumerateSwapchainFormats(_sess, 0, &n, NULL);
        std::vector<int64_t> formats(n);
        xrEnumerateSwapchainFormats(_sess, n, &n, formats.data());
        auto offered = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
        const char *scEnv = getenv("UA_VR_SWAPCHAIN");
        const bool wantUnorm = scEnv && !strcmp(scEnv, "unorm");
        _srgbWriteCtl = HasGlExt("GL_EXT_sRGB_write_control");
        if (!wantUnorm && _srgbWriteCtl && offered(GL_SRGB8_ALPHA8))
        {
            _format = GL_SRGB8_ALPHA8;
            glDisable(GL_FRAMEBUFFER_SRGB_EXT); // store the engine's encoded bytes as-is
        }
        else
            _format = GL_RGBA8;

        _msaa = EnvI("UA_VR_MSAA", 4);
        if (_msaa > 1 && HasGlExt("GL_EXT_multisampled_render_to_texture"))
        {
            pFbTex2DMs = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
            pRbStorageMs = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
            GLint maxS = 0;
            glGetIntegerv(GL_MAX_SAMPLES_EXT, &maxS);
            if (!pFbTex2DMs || !pRbStorageMs)
                _msaa = 1;
            else if (maxS > 0)
                _msaa = std::min(_msaa, (int)maxS);
        }
        else
            _msaa = 1;

        for (int e = 0; e < 2; e++)
            if (!MakeEyeChain(&_eye[e]))
                return false;

        SetupFoveation();
        SetupThreadHint();

        // 2D panel: persistent FBO + its quad-layer swapchain.
        int pw = 1280, ph = 720;
        if (const char *p = getenv("UA_VR_PANEL"))
            sscanf(p, "%dx%d", &pw, &ph);
        _panelW = pw;
        _panelH = ph;
        if (!MakePanel())
            return false;

        fprintf(stderr, "UAVR: openxr backend: eye %dx%d (recommended %ux%u, scale %.2f), format %s, msaa %d, "
                        "panel %dx%d, foveation %d, refresh %.0f Hz\n",
                _w, _h, vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, scale,
                _format == GL_SRGB8_ALPHA8 ? "SRGB8_ALPHA8 (write control off)" : "RGBA8",
                _msaa, _panelW, _panelH, _foveationLevel, _xr->refreshHz());
        return true;
    }

    void Shutdown() override
    {
        for (int e = 0; e < 2; e++)
        {
            EyeChain &c = _eye[e];
            if (!c.fbos.empty())
                glDeleteFramebuffers((GLsizei)c.fbos.size(), c.fbos.data());
            if (c.depth)
                glDeleteRenderbuffers(1, &c.depth);
            if (c.sc)
                xrDestroySwapchain(c.sc);
            c = EyeChain();
        }
        if (_quadSc)
            xrDestroySwapchain(_quadSc);
        if (!_quadFbos.empty())
            glDeleteFramebuffers((GLsizei)_quadFbos.size(), _quadFbos.data());
        if (_panelFbo)
            glDeleteFramebuffers(1, &_panelFbo);
        if (_panelTex)
            glDeleteTextures(1, &_panelTex);
        if (_panelDepth)
            glDeleteRenderbuffers(1, &_panelDepth);
        if (_dummyFbo)
            glDeleteFramebuffers(1, &_dummyFbo);
        if (_foveationProfile && _pDestroyFoveation)
            _pDestroyFoveation(_foveationProfile);
        if (_viewSpace)
            xrDestroySpace(_viewSpace);
        _quadSc = XR_NULL_HANDLE;
        _viewSpace = XR_NULL_HANDLE;
    }

    bool BeginFrame(FrameInfo *out) override
    {
        uavr::PumpEvents(true);
        uavr::Host *h = uavr::GetHost();
        _inFrame = false;
        _viewsValid = false;
        _eyeDone[0] = _eyeDone[1] = false;
        out->shouldRender = false;
        if (h->quitPosted || !_xr->sessionRunning())
            return false;
        return BeginXrFrame(out);
    }

    bool LocateViews(int64_t predictedDisplayTimeNs, ViewState *out) override
    {
        if (!_inFrame)
            return false;
        const XrTime t = predictedDisplayTimeNs ? (XrTime)predictedDisplayTimeNs : _frame.predictedDisplayTime;

        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = t;
        li.space = _space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        _views[0] = {XR_TYPE_VIEW};
        _views[1] = {XR_TYPE_VIEW};
        if (XR_FAILED(xrLocateViews(_sess, &li, &vs, 2, &n, _views)) || n != 2)
            return false;
        const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if ((vs.viewStateFlags & need) != need)
            return false;

        XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
        XrPosef head;
        if (XR_SUCCEEDED(xrLocateSpace(_viewSpace, _space, t, &hl)) &&
            (hl.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)))
            head = hl.pose;
        else
        {
            // Mid-point of the eyes, orientation of the left eye.
            head.orientation = _views[0].pose.orientation;
            head.position.x = 0.5f * (_views[0].pose.position.x + _views[1].pose.position.x);
            head.position.y = 0.5f * (_views[0].pose.position.y + _views[1].pose.position.y);
            head.position.z = 0.5f * (_views[0].pose.position.z + _views[1].pose.position.z);
        }

        out->valid = true;
        out->head.orientation = ToQ(head.orientation);
        out->head.position = ToV(head.position);
        const XrQuaternionf hInv = QConj(head.orientation);
        for (int e = 0; e < 2; e++)
        {
            const XrPosef &p = _views[e].pose;
            XrVector3f d = {p.position.x - head.position.x, p.position.y - head.position.y, p.position.z - head.position.z};
            out->eye[e].pose.orientation = ToQ(QMul(hInv, p.orientation));
            out->eye[e].pose.position = ToV(QRot(hInv, d));
            out->eye[e].fov.angleLeft = _views[e].fov.angleLeft;
            out->eye[e].fov.angleRight = _views[e].fov.angleRight;
            out->eye[e].fov.angleUp = _views[e].fov.angleUp;
            out->eye[e].fov.angleDown = _views[e].fov.angleDown;
        }
        _viewsValid = true;
        return true;
    }

    RenderTarget AcquireEye(int eye) override
    {
        RenderTarget rt;
        rt.width = _w;
        rt.height = _h;
        EyeChain &c = _eye[eye];
        if (!_inFrame || !_viewsValid || c.holding)
        {
            // Not in an OpenXR frame (session idle): draw into a throw-away target.
            rt.fbo = DummyFbo();
            return rt;
        }
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(c.sc, &ai, &c.acquired)))
        {
            rt.fbo = DummyFbo();
            return rt;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(c.sc, &wi);
        c.holding = true;
        rt.fbo = c.fbos[c.acquired];
        rt.colorTex = c.images[c.acquired].image;
        return rt;
    }

    // Dev aid (perf, render-path A/B on the headset): UA_VR_EYE_SHOT_AT=n1,n2,...
    // reads back both eyes of the n-th rendered world frame (counted per process)
    // into <data dir>/eyeshot_<n>_<eye>.ppm. Costs one stall per shot; inert unless set.
    void MaybeEyeShot(int eye)
    {
        static std::vector<int> at;
        static int parsed = 0, frame = 0;
        if (!parsed)
        {
            parsed = 1;
            if (const char *v = getenv("UA_VR_EYE_SHOT_AT"))
                for (const char *q = v; *q;)
                {
                    at.push_back(atoi(q));
                    while (*q && *q != ',') q++;
                    if (*q) q++;
                }
        }
        if (at.empty())
            return;
        if (eye == 0)
            frame++;
        if (std::find(at.begin(), at.end(), frame) == at.end())
            return;
        std::vector<uint8_t> px((size_t)_w * _h * 4);
        glReadPixels(0, 0, _w, _h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        char name[64];
        snprintf(name, sizeof(name), "eyeshot_%d_%d.ppm", frame, eye);
        if (FILE *f = fopen(name, "wb"))
        {
            fprintf(f, "P6\n%d %d\n255\n", _w, _h);
            for (int y = _h - 1; y >= 0; y--)
                for (int x = 0; x < _w; x++)
                    fwrite(&px[((size_t)y * _w + x) * 4], 1, 3, f);
            fclose(f);
            fprintf(stderr, "UAVR: eye shot %s\n", name);
        }
    }

    void ReleaseEye(int eye) override
    {
        EyeChain &c = _eye[eye];
        if (!c.holding)
            return;
        glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[c.acquired]);
        MaybeEyeShot(eye);
        const GLenum discard[] = {GL_DEPTH_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, discard);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(c.sc, &ri);
        c.holding = false;

        XrCompositionLayerProjectionView &v = _projViews[eye];
        v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        v.pose = _views[eye].pose; // the pose these pixels were rendered with
        v.fov = _views[eye].fov;
        v.subImage.swapchain = c.sc;
        v.subImage.imageRect.offset = {0, 0};
        v.subImage.imageRect.extent = {_w, _h};
        v.subImage.imageArrayIndex = 0;
        _eyeDone[eye] = true;
    }

    RenderTarget PanelTarget() override
    {
        RenderTarget rt;
        rt.fbo = _panelFbo;
        rt.colorTex = _panelTex;
        rt.width = _panelW;
        rt.height = _panelH;
        return rt;
    }

    void EndFrame(const FrameEnd &end) override
    {
        if (!_inFrame)
        {
            // Present() without FrameStart() (loading screens, FMV-style loops): open a frame
            // now so the panel still reaches the compositor.
            uavr::PumpEvents(false);
            FrameInfo fi;
            if (uavr::GetHost()->quitPosted || !_xr->sessionRunning() || !BeginXrFrame(&fi))
                return;
        }

        const XrCompositionLayerBaseHeader *layers[4];
        uint32_t nl = 0;
        const bool world = end.worldRendered && _eyeDone[0] && _eyeDone[1] && _frame.shouldRender;

        // passthrough under the projection layer, which is then blended by its alpha
        XrCompositionLayerPassthroughFB ptl{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        const bool pt = world && end.passthrough && _ptOn && _ptLayer;
        if (pt)
        {
            ptl.layerHandle = _ptLayer;
            ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // must not be 0
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&ptl;
        }

        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        if (world)
        {
            if (pt)
                proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
            proj.space = _space;
            proj.viewCount = 2;
            proj.views = _projViews;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }

        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad quad2{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (_frame.shouldRender && end.split && CopyPanelToQuad())
        {
            // PANELS FREE: the panel image as two quads (top rows, bottom rows). GL
            // swapchain image rects count rows from the bottom, like the blit that filled it.
            const int32_t rowsTop = std::max(1, std::min(_panelH - 1, (int32_t)lround(end.panel.v1 * _panelH)));
            for (int k = 0; k < 2; k++)
            {
                XrCompositionLayerQuad &q = k ? quad2 : quad;
                const PanelLayer &P = k ? end.panel2 : end.panel;
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = _space;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = _quadSc;
                q.subImage.imageRect.offset = {0, k ? 0 : _panelH - rowsTop};
                q.subImage.imageRect.extent = {_panelW, k ? _panelH - rowsTop : rowsTop};
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation = {P.pose.orientation.x, P.pose.orientation.y, P.pose.orientation.z, P.pose.orientation.w};
                q.pose.position = {P.pose.position.x, P.pose.position.y, P.pose.position.z};
                q.size = {P.widthM, P.heightM};
                layers[nl++] = (const XrCompositionLayerBaseHeader *)&q;
            }
        }
        else if (_frame.shouldRender && CopyPanelToQuad())
        {
            // Premultiplied alpha (the engine's panel blend writes coverage that way).
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space = _space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = _quadSc;
            quad.subImage.imageRect.offset = {0, 0};
            quad.subImage.imageRect.extent = {_panelW, _panelH};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = {end.panel.pose.orientation.x, end.panel.pose.orientation.y,
                                     end.panel.pose.orientation.z, end.panel.pose.orientation.w};
            quad.pose.position = {end.panel.pose.position.x, end.panel.pose.position.y, end.panel.pose.position.z};
            quad.size = {end.panel.widthM, end.panel.heightM};
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
        }

        _xr->endFrameLayers(_frame.predictedDisplayTime, layers, nl, world);
        _inFrame = false;
        _viewsValid = false;
        _eyeDone[0] = _eyeDone[1] = false;
    }

    void PollControllers(ControllerState out[2]) override
    {
        const uavr::InputState &s = _xr->input().state();
        for (int hnd = 0; hnd < 2; hnd++)
        {
            const uavr::HandState &h = s.hand[hnd];
            ControllerState &c = out[hnd];
            c = ControllerState();
            c.active = h.active;
            c.stickX = h.stick.x;
            c.stickY = h.stick.y;
            c.trigger = h.trigger;
            c.grip = h.grip;
            if (h.stickClick)
                c.buttons |= BTN_STICK_CLICK;
            if (h.aimValid)
            {
                c.aimPose.orientation.x = h.aimPose.orientation.x;
                c.aimPose.orientation.y = h.aimPose.orientation.y;
                c.aimPose.orientation.z = h.aimPose.orientation.z;
                c.aimPose.orientation.w = h.aimPose.orientation.w;
                c.aimPose.position.x = h.aimPose.position.x;
                c.aimPose.position.y = h.aimPose.position.y;
                c.aimPose.position.z = h.aimPose.position.z;
            }
        }
        if (s.a) out[1].buttons |= BTN_A;
        if (s.b) out[1].buttons |= BTN_B;
        if (s.x) out[0].buttons |= BTN_X;
        if (s.y) out[0].buttons |= BTN_Y;
        if (s.menu) out[0].buttons |= BTN_MENU;
    }

    void Haptic(int hand, float amplitude, float durationMs) override
    {
        if (hand < 0 || hand > 1 || amplitude <= 0.f)
            return;
        _xr->input().haptic((uavr::Hand)hand, std::min(1.f, amplitude), durationMs / 1000.f);
    }

    void EyeSize(int *w, int *h) const override
    {
        *w = _w;
        *h = _h;
    }

    int RefreshRates(float *out, int max) override
    {
        const std::vector<float> r = _xr->refreshRates();
        int n = 0;
        for (float hz : r)
            if (n < max)
                out[n++] = hz;
        return n;
    }
    float RefreshHz() const override { return _xr->refreshHz(); }
    bool RequestRefreshRate(float hz) override { return _xr->requestRefreshRate(hz); }

    // diorama: XR_FB_passthrough, created on first use (paused), resumed while the
    // miniature is up, paused again otherwise (no camera cost in the cockpit).
    bool SetPassthrough(bool on) override
    {
        if (on && !_ptTried)
            InitPassthrough();
        if (!_ptLayer)
            return false;
        if (on != _ptOn)
        {
            if (on)
            {
                // both steps must succeed; a half-started feature is rolled back.
                XrResult r = _pPtStart(_pt); // the passthrough feature itself (camera) runs only while used
                if (XR_FAILED(r))
                {
                    fprintf(stderr, "UAVR: passthrough start failed (%d) - grey room\n", (int)r);
                    return false;
                }
                r = _pPtLayerResume(_ptLayer);
                if (XR_FAILED(r))
                {
                    fprintf(stderr, "UAVR: passthrough layer resume failed (%d) - grey room\n", (int)r);
                    _pPtPause(_pt);
                    return false;
                }
            }
            else
            {
                const XrResult r = _pPtLayerPause(_ptLayer);
                if (XR_FAILED(r))
                    fprintf(stderr, "UAVR: passthrough layer pause failed (%d)\n", (int)r);
                _pPtPause(_pt);
            }
            _ptOn = on;
            fprintf(stderr, "UAVR: passthrough %s\n", on ? "resumed" : "paused");
        }
        return _ptOn;
    }

private:
    void InitPassthrough()
    {
        _ptTried = true;
        if (!_xr->hasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME))
        {
            fprintf(stderr, "UAVR: XR_FB_passthrough not enabled - the diorama room stays grey\n");
            return;
        }
        PFN_xrCreatePassthroughFB create = NULL;
        PFN_xrCreatePassthroughLayerFB createLayer = NULL;
        xrGetInstanceProcAddr(_inst, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&create);
        xrGetInstanceProcAddr(_inst, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&createLayer);
        xrGetInstanceProcAddr(_inst, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&_pPtStart);
        xrGetInstanceProcAddr(_inst, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&_pPtPause);
        xrGetInstanceProcAddr(_inst, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction *)&_pPtLayerResume);
        xrGetInstanceProcAddr(_inst, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction *)&_pPtLayerPause);
        if (!create || !createLayer || !_pPtStart || !_pPtPause || !_pPtLayerResume || !_pPtLayerPause)
        {
            fprintf(stderr, "UAVR: XR_FB_passthrough entry points missing\n");
            return;
        }
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = 0; // started by SetPassthrough(true)
        XrResult r = create(_sess, &pci, &_pt);
        if (XR_FAILED(r))
        {
            fprintf(stderr, "UAVR: xrCreatePassthroughFB failed (%d)\n", (int)r);
            _pt = XR_NULL_HANDLE;
            return;
        }
        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = _pt;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        lci.flags = 0; // paused until the miniature is shown
        r = createLayer(_sess, &lci, &_ptLayer);
        if (XR_FAILED(r))
        {
            fprintf(stderr, "UAVR: xrCreatePassthroughLayerFB failed (%d)\n", (int)r);
            _ptLayer = XR_NULL_HANDLE;
            return;
        }
        _ptOn = false;
        fprintf(stderr, "UAVR: passthrough ready (XR_FB_passthrough)\n");
    }

    bool BeginXrFrame(FrameInfo *out)
    {
        if (!_xr->waitAndBeginFrame(_frame))
            return false;
        _inFrame = true;
        out->shouldRender = _frame.shouldRender == XR_TRUE;
        out->predictedDisplayTimeNs = _frame.predictedDisplayTime;
        out->predictedDisplayPeriodNs = _frame.predictedDisplayPeriod;
        return true;
    }

    bool MakeEyeChain(EyeChain *c)
    {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sci.format = _format;
        sci.sampleCount = 1;
        sci.width = _w;
        sci.height = _h;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(_sess, &sci, &c->sc)))
        {
            fprintf(stderr, "UAVR: eye swapchain creation failed\n");
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(c->sc, 0, &n, NULL);
        c->images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(c->sc, n, &n, (XrSwapchainImageBaseHeader *)c->images.data());

        glGenRenderbuffers(1, &c->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, c->depth);
        if (_msaa > 1)
            pRbStorageMs(GL_RENDERBUFFER, _msaa, GL_DEPTH_COMPONENT24, _w, _h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, _w, _h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        c->fbos.assign(n, 0);
        glGenFramebuffers((GLsizei)n, c->fbos.data());
        for (uint32_t i = 0; i < n; i++)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, c->fbos[i]);
            if (_msaa > 1)
                pFbTex2DMs(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->images[i].image, 0, _msaa);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->images[i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c->depth);
            const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE)
            {
                fprintf(stderr, "UAVR: eye FBO incomplete (0x%x, msaa %d)\n", st, _msaa);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (_msaa > 1)
                {
                    // Retry the whole chain without MSAA.
                    glDeleteFramebuffers((GLsizei)n, c->fbos.data());
                    glDeleteRenderbuffers(1, &c->depth);
                    xrDestroySwapchain(c->sc);
                    *c = EyeChain();
                    _msaa = 1;
                    return MakeEyeChain(c);
                }
                return false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    bool MakePanel()
    {
        glGenTextures(1, &_panelTex);
        glBindTexture(GL_TEXTURE_2D, _panelTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, _panelW, _panelH);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenRenderbuffers(1, &_panelDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, _panelDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, _panelW, _panelH);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &_panelFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, _panelFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _panelTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _panelDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "UAVR: panel FBO incomplete\n");
            return false;
        }
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = _format;
        sci.sampleCount = 1;
        sci.width = _panelW;
        sci.height = _panelH;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(_sess, &sci, &_quadSc)))
        {
            fprintf(stderr, "UAVR: quad swapchain creation failed\n");
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(_quadSc, 0, &n, NULL);
        _quadImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(_quadSc, n, &n, (XrSwapchainImageBaseHeader *)_quadImages.data());
        _quadFbos.assign(n, 0);
        glGenFramebuffers((GLsizei)n, _quadFbos.data());
        for (uint32_t i = 0; i < n; i++)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, _quadFbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _quadImages[i].image, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                fprintf(stderr, "UAVR: quad FBO incomplete\n");
                return false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    // Blit (no shader, no engine GL state touched apart from the framebuffer bindings,
    // which the engine re-binds itself after Present()).
    bool CopyPanelToQuad()
    {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(_quadSc, &ai, &idx)))
            return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(_quadSc, &wi);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _panelFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _quadFbos[idx]);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, _panelW, _panelH, 0, 0, _panelW, _panelH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(_quadSc, &ri);
        return true;
    }

    GLuint DummyFbo()
    {
        if (!_dummyFbo)
        {
            // Small colour+depth target; anything drawn while no XR frame is open lands here.
            GLuint tex = 0, depth = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
            glBindTexture(GL_TEXTURE_2D, 0);
            glGenRenderbuffers(1, &depth);
            glBindRenderbuffer(GL_RENDERBUFFER, depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 64, 64);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glGenFramebuffers(1, &_dummyFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, _dummyFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        return _dummyFbo;
    }

    // Fixed foveated rendering (XR_FB_foveation): the compositor-driven Adreno tile
    // foveation applied to both eye swapchains. UA_VR_FOVEATION=0..3 (off/low/medium/high).
    void SetupFoveation()
    {
        _foveationLevel = std::max(0, std::min(3, EnvI("UA_VR_FOVEATION", 2)));
        if (!_foveationLevel || !_xr->hasExtension(XR_FB_FOVEATION_EXTENSION_NAME) ||
            !_xr->hasExtension(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) ||
            !_xr->hasExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME))
        {
            if (_foveationLevel)
                fprintf(stderr, "UAVR: XR_FB_foveation not available\n");
            _foveationLevel = 0;
            return;
        }
        PFN_xrCreateFoveationProfileFB create = NULL;
        PFN_xrUpdateSwapchainFB update = NULL;
        xrGetInstanceProcAddr(_inst, "xrCreateFoveationProfileFB", (PFN_xrVoidFunction *)&create);
        xrGetInstanceProcAddr(_inst, "xrDestroyFoveationProfileFB", (PFN_xrVoidFunction *)&_pDestroyFoveation);
        xrGetInstanceProcAddr(_inst, "xrUpdateSwapchainFB", (PFN_xrVoidFunction *)&update);
        if (!create || !update)
        {
            _foveationLevel = 0;
            return;
        }
        XrFoveationLevelProfileCreateInfoFB lvl{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
        lvl.level = (XrFoveationLevelFB)_foveationLevel;
        lvl.verticalOffset = 0.f;
        lvl.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
        XrFoveationProfileCreateInfoFB pci{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
        pci.next = &lvl;
        if (XR_FAILED(create(_sess, &pci, &_foveationProfile)))
        {
            _foveationLevel = 0;
            return;
        }
        for (int e = 0; e < 2; e++)
        {
            XrSwapchainStateFoveationFB st{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
            st.profile = _foveationProfile;
            if (XR_FAILED(update(_eye[e].sc, (XrSwapchainStateBaseHeaderFB *)&st)))
                _foveationLevel = 0;
        }
    }

    // The engine is single-threaded: logic, collection and GL submission all run on the
    // android_main thread, so it is the renderer's main thread for the runtime's scheduler.
    void SetupThreadHint()
    {
        if (!_xr->hasExtension(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
            return;
        PFN_xrSetAndroidApplicationThreadKHR setThread = NULL;
        xrGetInstanceProcAddr(_inst, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&setThread);
        if (setThread)
            setThread(_sess, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)syscall(SYS_gettid));
    }

    uavr::XrPlatform *_xr = NULL;
    XrInstance _inst = XR_NULL_HANDLE;
    XrSession _sess = XR_NULL_HANDLE;
    XrSpace _space = XR_NULL_HANDLE, _viewSpace = XR_NULL_HANDLE;
    int _w = 0, _h = 0, _msaa = 1;
    int64_t _format = GL_RGBA8;
    bool _srgbWriteCtl = false;

    EyeChain _eye[2];
    XrFrameState _frame{XR_TYPE_FRAME_STATE};
    bool _inFrame = false, _viewsValid = false, _eyeDone[2] = {false, false};
    XrView _views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrCompositionLayerProjectionView _projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};

    int _panelW = 1280, _panelH = 720;
    GLuint _panelFbo = 0, _panelTex = 0, _panelDepth = 0, _dummyFbo = 0;
    XrSwapchain _quadSc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> _quadImages;
    std::vector<GLuint> _quadFbos;

    bool _ptTried = false, _ptOn = false;
    XrPassthroughFB _pt = XR_NULL_HANDLE;
    XrPassthroughLayerFB _ptLayer = XR_NULL_HANDLE;
    PFN_xrPassthroughStartFB _pPtStart = NULL;
    PFN_xrPassthroughPauseFB _pPtPause = NULL;
    PFN_xrPassthroughLayerResumeFB _pPtLayerResume = NULL;
    PFN_xrPassthroughLayerPauseFB _pPtLayerPause = NULL;

    int _foveationLevel = 0;
    XrFoveationProfileFB _foveationProfile = XR_NULL_HANDLE;
    PFN_xrDestroyFoveationProfileFB _pDestroyFoveation = NULL;
};

} // namespace

Backend *CreateOpenXRBackend() { return new OpenXRBackend(); }

} // namespace VR

#endif // __ANDROID__
