#include "platform/xr/xr_platform.h"
#include "usage_stats.h"

#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace uavr {

namespace {

constexpr int kWantMsaa = 4;  // only used with GL_OVR_multiview_multisampled_render_to_texture

PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC glFbTexMultiview = nullptr;
PFNGLFRAMEBUFFERTEXTUREMULTISAMPLEMULTIVIEWOVRPROC glFbTexMsMultiview = nullptr;

bool hasGlExt(const char* name) {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return ext && std::strstr(ext, name);
}

}  // namespace

const char* xrResultName(XrInstance instance, XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf)))
        return buf;
    std::snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}

// ---------------------------------------------------------------- init / shutdown

bool XrPlatform::initLoader(JavaVM* vm, jobject activity) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    XR_CHECK(XR_NULL_HANDLE,
             xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&initLoader)));
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = vm;
    li.applicationContext = activity;
    XR_CHECK(XR_NULL_HANDLE, initLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li)));
    return true;
}

bool XrPlatform::createInstance(JavaVM* vm, jobject activity) {
    uint32_t count = 0;
    XR_CHECK(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(XR_NULL_HANDLE,
             xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()));
    auto has = [&](const char* n) {
        return std::any_of(props.begin(), props.end(),
                           [&](const XrExtensionProperties& p) { return !std::strcmp(p.extensionName, n); });
    };
    std::string all;
    for (auto& p : props) { all += p.extensionName; all += ' '; }
    LOGI("boot: runtime extensions (%u): %s", count, all.c_str());

    std::vector<const char*> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                     XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    for (const char* e : exts)
        if (!has(e)) { LOGE("required extension missing: %s", e); return false; }
    hasRefreshRateExt_ = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    hasPerfSettingsExt_ = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (hasRefreshRateExt_) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (hasPerfSettingsExt_) exts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    // Optional, used by the engine backend when present (fixed foveated rendering, thread hints).
    for (const char* e : {XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME, XR_FB_FOVEATION_EXTENSION_NAME,
                          XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME,
                          XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
                          XR_FB_PASSTHROUGH_EXTENSION_NAME}) // diorama (room behind the miniature)
        if (has(e)) exts.push_back(e);
    for (const char* e : exts) enabledExts_.push_back(e);

    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = vm;
    android.applicationActivity = activity;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android;
    std::strncpy(ci.applicationInfo.applicationName, opt_.appName, XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "UAVR");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    XR_CHECK(XR_NULL_HANDLE, xrCreateInstance(&ci, &instance_));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &ip);
    LOGI("boot: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    if (hasRefreshRateExt_) {
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pEnumRates_));
        xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pGetRate_));
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pRequestRate_));
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(instance_, xrGetSystem(instance_, &sgi, &system_));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, system_, &sp);
    LOGI("boot: system '%s' vendor=%u maxLayers=%u orientTrack=%d posTrack=%d", sp.systemName,
         sp.vendorId, sp.graphicsProperties.maxLayerCount,
         sp.trackingProperties.orientationTracking, sp.trackingProperties.positionTracking);
    return true;
}

bool XrPlatform::createSession() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs = nullptr;
    XR_CHECK(instance_, xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                                              reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)));
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(instance_, getReqs(instance_, system_, &reqs));
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion have = XR_MAKE_VERSION(major, minor, 0);
    LOGI("boot: GLES %d.%d (runtime needs %u.%u .. %u.%u)", major, minor,
         XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
         XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported));
    if (have < reqs.minApiVersionSupported) {
        LOGE("GLES version too old for runtime");
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = egl_->display;
    gb.config = egl_->config;
    gb.context = egl_->context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system_;
    XR_CHECK(instance_, xrCreateSession(instance_, &sci, &session_));

    // STAGE (floor origin) if the runtime offers it, otherwise LOCAL + assumed eye height.
    uint32_t n = 0;
    xrEnumerateReferenceSpaces(session_, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces(session_, n, &n, spaces.data());
    const bool hasStage = !opt_.seatedLocalSpace &&
        std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    rci.poseInReferenceSpace.orientation.w = 1.f;
    XR_CHECK(instance_, xrCreateReferenceSpace(session_, &rci, &appSpace_));
    eyeHeightOffset_ = (hasStage || opt_.seatedLocalSpace) ? 0.f : 1.6f;
    LOGI("boot: reference space %s", hasStage ? "STAGE"
                                     : opt_.seatedLocalSpace ? "LOCAL (seated, origin = eye point)"
                                                             : "LOCAL (+1.6 m eye height)");
    return true;
}

bool XrPlatform::createSwapchain() {
    uint32_t n = 0;
    XR_CHECK(instance_, xrEnumerateViewConfigurationViews(instance_, system_,
                                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                          0, &n, nullptr));
    std::vector<XrViewConfigurationView> vcv(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(instance_, xrEnumerateViewConfigurationViews(instance_, system_,
                                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                          n, &n, vcv.data()));
    if (n != 2) { LOGE("expected 2 stereo views, got %u", n); return false; }
    width_ = static_cast<int>(vcv[0].recommendedImageRectWidth);
    height_ = static_cast<int>(vcv[0].recommendedImageRectHeight);

    xrEnumerateSwapchainFormats(session_, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(session_, n, &n, formats.data());
    int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t want : {(int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8}) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) { format = want; break; }
    }

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = format;
    sci.sampleCount = 1;
    sci.width = width_;
    sci.height = height_;
    sci.faceCount = 1;
    sci.arraySize = 2;
    sci.mipCount = 1;
    XR_CHECK(instance_, xrCreateSwapchain(session_, &sci, &swapchain_));

    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    XR_CHECK(instance_, xrEnumerateSwapchainImages(
                            swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())));

    // Multiview?
    if (hasGlExt("GL_OVR_multiview2")) {
        glFbTexMultiview = reinterpret_cast<PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC>(
            eglGetProcAddress("glFramebufferTextureMultiviewOVR"));
        if (hasGlExt("GL_OVR_multiview_multisampled_render_to_texture"))
            glFbTexMsMultiview = reinterpret_cast<PFNGLFRAMEBUFFERTEXTUREMULTISAMPLEMULTIVIEWOVRPROC>(
                eglGetProcAddress("glFramebufferTextureMultisampleMultiviewOVR"));
        multiview_ = glFbTexMultiview != nullptr;
    }

    glGenTextures(1, &depthTex_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, depthTex_);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, width_, height_, 2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    auto buildFbos = [&](int samples) -> bool {
        if (!fbos_.empty()) glDeleteFramebuffers((GLsizei)fbos_.size(), fbos_.data());
        fbos_.assign(images_.size() * (multiview_ ? 1 : 2), 0);
        glGenFramebuffers((GLsizei)fbos_.size(), fbos_.data());
        for (size_t i = 0; i < images_.size(); ++i) {
            const GLuint color = images_[i].image;
            if (multiview_) {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos_[i]);
                if (samples > 1) {
                    glFbTexMsMultiview(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthTex_, 0, samples, 0, 2);
                    glFbTexMsMultiview(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, samples, 0, 2);
                } else {
                    glFbTexMultiview(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthTex_, 0, 0, 2);
                    glFbTexMultiview(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, 0, 2);
                }
                if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            } else {
                for (int eye = 0; eye < 2; ++eye) {
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos_[i * 2 + eye]);
                    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthTex_, 0, eye);
                    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, eye);
                    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
                }
            }
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        return true;
    };

    bool ok = false;
    if (multiview_ && glFbTexMsMultiview) {
        msaa_ = kWantMsaa;
        ok = buildFbos(msaa_);
        if (!ok) LOGW("multiview MSAA x%d FBO incomplete, retrying without MSAA", msaa_);
    }
    if (!ok && multiview_) {
        msaa_ = 1;
        ok = buildFbos(1);
        if (!ok) { LOGW("multiview FBO incomplete, falling back to two passes"); multiview_ = false; }
    }
    if (!ok) {
        msaa_ = 1;
        ok = buildFbos(1);
    }
    if (!ok) { LOGE("could not build any complete framebuffer"); return false; }

    LOGI("boot: swapchain %dx%d x2 layers, format 0x%llx, %zu images, render path: %s, msaa=%d",
         width_, height_, (long long)format, images_.size(),
         multiview_ ? "MULTIVIEW (GL_OVR_multiview2, single pass)" : "TWO PASSES (no GL_OVR_multiview2)",
         msaa_);
    return true;
}

void XrPlatform::setupRefreshRate() {
    float current = 0.f;
    if (!hasRefreshRateExt_ || !pEnumRates_) {
        LOGI("boot: XR_FB_display_refresh_rate not available");
        stats_.setInfo(0.f, multiview_, msaa_);
        return;
    }
    uint32_t n = 0;
    pEnumRates_(session_, 0, &n, nullptr);
    std::vector<float> rates(n);
    pEnumRates_(session_, n, &n, rates.data());
    std::string s;
    for (float r : rates) s += std::to_string(static_cast<int>(r + 0.5f)) + " ";
    pGetRate_(session_, &current);
    LOGI("boot: display refresh rates supported: %s(current %.1f)", s.c_str(), current);

    // Exact match, else the highest offered rate below the target (UA_VR_HZ=auto
    // asks for 120 and lets the engine step down).
    float want = 0.f;
    for (float r : rates) if (std::fabs(r - opt_.targetRefreshHz) < 0.5f) want = r;
    if (want <= 0.f)
        for (float r : rates) if (r <= opt_.targetRefreshHz + 0.5f && r > want) want = r;
    if (want > 0.f) {
        XrResult r = pRequestRate_(session_, want);
        LOGI("boot: requested %.1f Hz -> %s", want, xrResultName(instance_, r));
    } else {
        LOGW("boot: %.0f Hz not offered by runtime", opt_.targetRefreshHz);
    }
    pGetRate_(session_, &current);
    refreshHz_ = current;
    stats_.setInfo(current, multiview_, msaa_);
}

std::vector<float> XrPlatform::refreshRates() const {
    std::vector<float> rates;
    if (!hasRefreshRateExt_ || !pEnumRates_ || session_ == XR_NULL_HANDLE) return rates;
    uint32_t n = 0;
    pEnumRates_(session_, 0, &n, nullptr);
    rates.resize(n);
    pEnumRates_(session_, n, &n, rates.data());
    rates.resize(n);
    std::sort(rates.begin(), rates.end());
    return rates;
}

bool XrPlatform::requestRefreshRate(float hz) {
    if (!hasRefreshRateExt_ || !pRequestRate_ || session_ == XR_NULL_HANDLE) return false;
    XrResult r = pRequestRate_(session_, hz);
    LOGI("refresh rate request %.1f Hz -> %s", hz, xrResultName(instance_, r));
    return XR_SUCCEEDED(r);
}

void XrPlatform::setupPerformance() {
    if (!hasPerfSettingsExt_) return;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = nullptr;
    xrGetInstanceProcAddr(instance_, "xrPerfSettingsSetPerformanceLevelEXT",
                          reinterpret_cast<PFN_xrVoidFunction*>(&setLevel));
    if (!setLevel) return;
    // Default sustained-high: the runtime throttles less aggressively than BOOST.
    auto lvl = [](int l) { return static_cast<XrPerfSettingsLevelEXT>(std::max(0, std::min(3, l)) * 25); };
    XrResult c = setLevel(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, lvl(opt_.cpuLevel));
    XrResult g = setLevel(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, lvl(opt_.gpuLevel));
    LOGI("boot: perf levels CPU %d / GPU %d (0 save .. 3 boost) -> %s / %s", opt_.cpuLevel,
         opt_.gpuLevel, xrResultName(instance_, c), xrResultName(instance_, g));
}

bool XrPlatform::hasExtension(const char* name) const {
    for (const std::string& e : enabledExts_)
        if (e == name) return true;
    return false;
}

bool XrPlatform::init(JavaVM* vm, jobject activity, const EglContext& egl,
                      const XrPlatformOptions& opt) {
    egl_ = &egl;
    opt_ = opt;
    if (!initLoader(vm, activity)) return false;
    if (!createInstance(vm, activity)) return false;
    if (!input_.init(instance_)) return false;
    if (!createSession()) return false;
    if (!input_.attach(session_)) return false;
    if (opt_.createStereoSwapchain && !createSwapchain()) return false;
    stats_.initGpuTimer();
    stats_.setInfo(0.f, multiview_, msaa_);
    return true;
}

void XrPlatform::shutdown() {
    if (!fbos_.empty()) glDeleteFramebuffers((GLsizei)fbos_.size(), fbos_.data());
    fbos_.clear();
    if (depthTex_) glDeleteTextures(1, &depthTex_);
    depthTex_ = 0;
    if (swapchain_) xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    input_.shutdown();
    if (appSpace_) xrDestroySpace(appSpace_);
    appSpace_ = XR_NULL_HANDLE;
    if (session_) xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    if (instance_) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
}

// ---------------------------------------------------------------- events

void XrPlatform::requestExit() {
    if (session_ && running_) xrRequestExitSession(session_);
    else exit_ = true;
}

void XrPlatform::handleSessionState(XrSessionState s) {
    static const char* names[] = {"UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                  "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING"};
    LOGI("session state %s -> %s", names[std::min<int>(state_, 8)], names[std::min<int>(s, 8)]);
    state_ = s;
    switch (s) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrResult r = xrBeginSession(session_, &bi);
            LOGI("xrBeginSession -> %s", xrResultName(instance_, r));
            running_ = XR_SUCCEEDED(r);
            if (running_) {
                setupRefreshRate();
                setupPerformance();
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            xrEndSession(session_);
            running_ = false;
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
            UsageStats::Error("vr_session_lost");  // anonymous usage stats (no-op unless the player said yes)
            running_ = false;
            exit_ = true;
            break;
        case XR_SESSION_STATE_EXITING:
            running_ = false;
            exit_ = true;
            break;
        default:
            break;
    }
}

void XrPlatform::pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                handleSessionState(reinterpret_cast<XrEventDataSessionStateChanged&>(ev).state);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGW("instance loss pending");
                UsageStats::Error("vr_session_lost");
                exit_ = true;
                break;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                auto& e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB&>(ev);
                LOGI("refresh rate changed %.1f -> %.1f Hz", e.fromDisplayRefreshRate,
                     e.toDisplayRefreshRate);
                stats_.setInfo(e.toDisplayRefreshRate, multiview_, msaa_);
                refreshHz_ = e.toDisplayRefreshRate;
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
                for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
                    XrPath p = XR_NULL_PATH;
                    xrStringToPath(instance_, hand, &p);
                    XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
                    char buf[XR_MAX_PATH_LENGTH] = "(none)";
                    uint32_t len = 0;
                    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, p, &ps)) &&
                        ps.interactionProfile != XR_NULL_PATH)
                        xrPathToString(instance_, ps.interactionProfile, sizeof(buf), &len, buf);
                    LOGI("interaction profile %s: %s", hand, buf);
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                LOGI("reference space change pending (recenter)");
                break;
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------- frame loop

bool XrPlatform::beginFrame(FrameInfo& out) {
    frameState_ = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrResult r = xrWaitFrame(session_, &wi, &frameState_);
    if (XR_FAILED(r)) { XR_LOG_FAIL(instance_, r); return false; }
    const double t = nowSeconds();
    intervalMs_ = lastWaitReturnS_ > 0 ? (t - lastWaitReturnS_) * 1e3 : 0;
    lastWaitReturnS_ = frameStartS_ = t;

    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(session_, &bi);
    if (XR_FAILED(r)) { XR_LOG_FAIL(instance_, r); return false; }

    out.shouldRender = frameState_.shouldRender == XR_TRUE;
    out.predictedDisplayTime = frameState_.predictedDisplayTime;
    out.predictedPeriodS = frameState_.predictedDisplayPeriod * 1e-9;
    out.dtS = lastDisplayTime_ ? (frameState_.predictedDisplayTime - lastDisplayTime_) * 1e-9
                               : out.predictedPeriodS;
    if (out.dtS <= 0 || out.dtS > 0.1) out.dtS = out.predictedPeriodS;
    lastDisplayTime_ = frameState_.predictedDisplayTime;

    input_.sync(appSpace_, frameState_.predictedDisplayTime);

    viewsValid_ = false;
    if (out.shouldRender) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = frameState_.predictedDisplayTime;
        li.space = appSpace_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        r = xrLocateViews(session_, &li, &vs, 2, &n, views_);
        const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        viewsValid_ = XR_SUCCEEDED(r) && n == 2 && (vs.viewStateFlags & need) == need;
    }
    return true;
}

bool XrPlatform::waitAndBeginFrame(XrFrameState& out) {
    frameState_ = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrResult r = xrWaitFrame(session_, &wi, &frameState_);
    if (XR_FAILED(r)) { XR_LOG_FAIL(instance_, r); return false; }
    const double t = nowSeconds();
    intervalMs_ = lastWaitReturnS_ > 0 ? (t - lastWaitReturnS_) * 1e3 : 0;
    lastWaitReturnS_ = frameStartS_ = t;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(session_, &bi);
    if (XR_FAILED(r)) { XR_LOG_FAIL(instance_, r); return false; }
    lastDisplayTime_ = frameState_.predictedDisplayTime;
    input_.sync(appSpace_, frameState_.predictedDisplayTime);
    out = frameState_;
    return true;
}

void XrPlatform::endFrameLayers(XrTime displayTime, const XrCompositionLayerBaseHeader* const* layers,
                                uint32_t count, bool rendered) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = count;
    ei.layers = count ? layers : nullptr;
    XR_LOG_FAIL(instance_, xrEndFrame(session_, &ei));
    const double cpuMs = (nowSeconds() - frameStartS_) * 1e3;
    stats_.frame(intervalMs_, cpuMs, frameState_.predictedDisplayPeriod * 1e-6, rendered);
}

void XrPlatform::renderEyes(const std::function<void(const EyeRender&)>& draw) {
    if (!viewsValid_) return;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) { viewsValid_ = false; return; }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(swapchain_, &wi);

    EyeRender er;
    er.width = width_;
    er.height = height_;
    for (int eye = 0; eye < 2; ++eye) {
        er.eyePose[eye] = toPose(views_[eye].pose);
        const XrFovf& f = views_[eye].fov;
        er.proj[eye] = mat4FromFov(f.angleLeft, f.angleRight, f.angleUp, f.angleDown, 0.05f, 500.f);
    }

    auto pass = [&](GLuint fbo, int viewIndex) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glViewport(0, 0, width_, height_);
        er.viewIndex = viewIndex;
        draw(er);
        // Depth is never needed after the pass: let the tiler skip storing it.
        const GLenum discard[] = {GL_DEPTH_ATTACHMENT};
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, discard);
    };
    stats_.gpuBegin();
    if (multiview_) {
        pass(fbos_[idx], -1);
    } else {
        pass(fbos_[idx * 2 + 0], 0);
        pass(fbos_[idx * 2 + 1], 1);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    stats_.gpuEnd();

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);

    for (int eye = 0; eye < 2; ++eye) {
        layerViews_[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        layerViews_[eye].pose = views_[eye].pose;
        layerViews_[eye].fov = views_[eye].fov;
        layerViews_[eye].subImage.swapchain = swapchain_;
        layerViews_[eye].subImage.imageRect = {{0, 0}, {width_, height_}};
        layerViews_[eye].subImage.imageArrayIndex = eye;
    }
}

void XrPlatform::endFrame() {
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1];
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frameState_.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const bool rendered = viewsValid_ && frameState_.shouldRender;
    if (rendered) {
        layer.space = appSpace_;
        layer.viewCount = 2;
        layer.views = layerViews_;
        layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
        ei.layerCount = 1;
        ei.layers = layers;
    }
    XR_LOG_FAIL(instance_, xrEndFrame(session_, &ei));
    const double cpuMs = (nowSeconds() - frameStartS_) * 1e3;
    stats_.frame(intervalMs_, cpuMs, frameState_.predictedDisplayPeriod * 1e-6, rendered);
}

}  // namespace uavr
