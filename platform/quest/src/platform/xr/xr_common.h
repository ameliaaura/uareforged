// OpenXR includes + error helpers shared by platform/xr.
#pragma once

#include <EGL/egl.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "common/log.h"
#include "common/vmath.h"

namespace uavr {

const char* xrResultName(XrInstance instance, XrResult r);

// Logs and returns false on failure; use in bool-returning init code.
#define XR_CHECK(instance, call)                                                        \
    do {                                                                                \
        XrResult xr_check_res_ = (call);                                                \
        if (XR_FAILED(xr_check_res_)) {                                                 \
            LOGE("%s failed: %s (%d) at %s:%d", #call,                                  \
                 ::uavr::xrResultName(instance, xr_check_res_), (int)xr_check_res_,     \
                 __FILE__, __LINE__);                                                   \
            return false;                                                               \
        }                                                                               \
    } while (0)

// Logs only; for per-frame calls where we want to keep going.
#define XR_LOG_FAIL(instance, call)                                                     \
    do {                                                                                \
        XrResult xr_check_res_ = (call);                                                \
        if (XR_FAILED(xr_check_res_))                                                   \
            LOGE("%s failed: %s (%d)", #call,                                           \
                 ::uavr::xrResultName(instance, xr_check_res_), (int)xr_check_res_);    \
    } while (0)

inline Pose toPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}

}  // namespace uavr
