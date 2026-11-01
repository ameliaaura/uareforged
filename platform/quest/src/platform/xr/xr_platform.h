// OpenXR platform layer: loader init, instance, system, GLES session, reference space,
// stereo swapchain (texture array, multiview when available), session lifecycle, frame loop,
// display refresh rate (XR_FB_display_refresh_rate) and input (XrInput).
//
// The hosted app/engine never calls OpenXR directly; it gets EyeRender callbacks with an FBO
// already bound and per-eye pose/projection in tracking space.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "platform/android/egl_context.h"
#include "platform/xr/frame_stats.h"
#include "platform/xr/xr_common.h"
#include "platform/xr/xr_input.h"

namespace uavr {

struct EyeRender {
    // viewIndex == -1: multiview pass, draw once and the shader picks the eye via gl_ViewID_OVR.
    // viewIndex == 0/1: fallback pass for that eye only.
    int viewIndex = -1;
    int width = 0, height = 0;
    Pose eyePose[2];  // in tracking space (app reference space)
    Mat4 proj[2];
};

// How the platform is set up. Defaults = the hello app (STAGE space, one texture-array
// stereo swapchain driven by renderEyes/endFrame). The UA engine (src/vr/vr_openxr.cpp) runs
// in "engine mode": seated LOCAL space, no platform swapchain (the backend owns its eye and
// quad-layer swapchains) and the low-level waitAndBeginFrame/endFrameLayers calls.
struct XrPlatformOptions {
    const char* appName = "UAVR Hello";
    bool createStereoSwapchain = true;
    bool seatedLocalSpace = false;
    float targetRefreshHz = 90.f;
    // XR_EXT_performance_settings levels: 0 power savings, 1 sustained low,
    // 2 sustained high, 3 boost (values of XrPerfSettingsLevelEXT / 25).
    int cpuLevel = 2, gpuLevel = 2;
};

struct FrameInfo {
    bool shouldRender = false;
    XrTime predictedDisplayTime = 0;
    double predictedPeriodS = 0;
    double dtS = 0;  // between predicted display times
};

class XrPlatform {
public:
    // egl must outlive the platform; its context is current on the calling thread.
    bool init(JavaVM* vm, jobject activity, const EglContext& egl,
              const XrPlatformOptions& opt = XrPlatformOptions());
    void shutdown();

    void pollEvents();
    bool sessionRunning() const { return running_; }
    bool exitRequested() const { return exit_; }
    void requestExit();

    // Frame loop: waitFrame/beginFrame -> sync input -> renderEyes -> endFrame.
    bool beginFrame(FrameInfo& out);
    void renderEyes(const std::function<void(const EyeRender&)>& draw);
    void endFrame();

    // Engine mode: xrWaitFrame + xrBeginFrame + input sync. Returns false if the runtime
    // refused (no xrEndFrame is owed then).
    bool waitAndBeginFrame(XrFrameState& out);
    // Engine mode: xrEndFrame with the caller's layers (may be 0 layers).
    void endFrameLayers(XrTime displayTime, const XrCompositionLayerBaseHeader* const* layers,
                        uint32_t count, bool rendered);

    XrInstance instance() const { return instance_; }
    XrSystemId systemId() const { return system_; }
    XrSession session() const { return session_; }
    XrSpace appSpace() const { return appSpace_; }
    XrSessionState sessionState() const { return state_; }
    bool focused() const { return state_ == XR_SESSION_STATE_FOCUSED; }
    bool hasExtension(const char* name) const;
    float refreshHz() const { return refreshHz_; }
    // XR_FB_display_refresh_rate: offered rates (ascending) and a runtime request.
    std::vector<float> refreshRates() const;
    bool requestRefreshRate(float hz);
    FrameStats& stats() { return stats_; }

    bool multiview() const { return multiview_; }
    float eyeHeightOffset() const { return eyeHeightOffset_; }  // add to rig Y (LOCAL space)
    XrInput& input() { return input_; }

private:
    bool initLoader(JavaVM* vm, jobject activity);
    bool createInstance(JavaVM* vm, jobject activity);
    bool createSession();
    bool createSwapchain();
    void handleSessionState(XrSessionState s);
    void setupRefreshRate();
    void setupPerformance();

    const EglContext* egl_ = nullptr;
    XrPlatformOptions opt_;
    std::vector<std::string> enabledExts_;
    float refreshHz_ = 0.f;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false, exit_ = false;
    float eyeHeightOffset_ = 0.f;

    bool hasRefreshRateExt_ = false, hasPerfSettingsExt_ = false;
    PFN_xrEnumerateDisplayRefreshRatesFB pEnumRates_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB pGetRate_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB pRequestRate_ = nullptr;

    // Swapchain: one GL_TEXTURE_2D_ARRAY swapchain with 2 layers (left/right).
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    int width_ = 0, height_ = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images_;
    std::vector<GLuint> fbos_;  // multiview: 1 per image; fallback: 2 per image (per eye)
    GLuint depthTex_ = 0;       // GL_TEXTURE_2D_ARRAY, 2 layers
    bool multiview_ = false;
    int msaa_ = 1;

    // Per-frame
    XrFrameState frameState_{XR_TYPE_FRAME_STATE};
    XrView views_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool viewsValid_ = false;
    XrCompositionLayerProjectionView layerViews_[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                       {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrTime lastDisplayTime_ = 0;
    double lastWaitReturnS_ = 0, frameStartS_ = 0, intervalMs_ = 0;

    FrameStats stats_;
    XrInput input_;
};

}  // namespace uavr
