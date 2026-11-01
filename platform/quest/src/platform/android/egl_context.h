// Headless EGL context (GLES 3.x) for an OpenXR app: the runtime owns the display surface,
// so we only need a context made current on a tiny pbuffer.
#pragma once

#include <EGL/egl.h>

namespace uavr {

struct EglContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    bool init();
    void shutdown();
};

}  // namespace uavr
