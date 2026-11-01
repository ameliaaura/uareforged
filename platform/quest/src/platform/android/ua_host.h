// The UA engine host on Quest: what the NativeActivity entry point (ua_main_android.cpp) owns
// and the engine's OpenXR VR backend (src/vr/vr_openxr.cpp) needs to reach.
#pragma once

#include <string>

struct android_app;

namespace uavr {

class XrPlatform;
struct EglContext;

struct Host {
    android_app* app = nullptr;
    EglContext* egl = nullptr;
    XrPlatform* xr = nullptr;
    bool resumed = false;
    bool quitPosted = false;
    std::string dataDir;  // .../Android/data/<pkg>/files/ua (the engine's CWD)
};

Host* GetHost();

// Drain the Android looper (lifecycle commands) and OpenXR events without blocking.
// When `blockWhileIdle`, waits on the looper while the session is not running (headset off,
// app paused) so the game does not spin. Posts SDL_QUIT once the activity or session ends.
void PumpEvents(bool blockWhileIdle);

}  // namespace uavr
