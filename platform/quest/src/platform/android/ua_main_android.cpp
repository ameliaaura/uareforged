// NativeActivity entry point of UA Reforged VR.
//
// Order at start-up:
//   1. stdout/stderr -> logcat (tag "UA"; lines starting "UAVR" go to tag "UAVR").
//   2. Data dir = <app external files>/ua (the engine's working folder); chdir there.
//   3. Configuration -> environment (the engine reads UA_* env vars):
//        system properties  debug.uavr.mute=1 (-> UA_MUTE=1, no audio output at all) and
//                           debug.uavr.env="K=V K=V ..." (any engine variable; wins;
//                           continued in debug.uavr.env2 / env3, 91 chars each)
//        <data dir>/uavr-user.cfg  the player's own KEY=VALUE lines (optional)
//        <data dir>/uavr.cfg       written by the start screens (look, pack folders)
//        defaults           UA_VR=openxr, UA_HDMODELS_DIR=<data dir>/hd-models
//   4. EGL (GLES 3.x) + OpenXR session (engine mode: seated LOCAL space).
//   5. The start screens (setup/setup_flow.cpp): on first launch "Get the game" and the Reforge pack,
//      on every launch the campaign choice when the add-on campaigns are installed.
//   6. UA_main() - the engine's main(); the VR backend drives xrWaitFrame/xrEndFrame and
//      calls PumpEvents() every frame.
//   7. On return: finish the activity, tear down XR/EGL and exit the process (engine statics
//      are not re-entrant, so a relaunch must start a fresh process).
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <vector>

#include "common/log.h"
#include "platform/android/egl_context.h"
#include "platform/android/ua_host.h"
#include "platform/xr/xr_platform.h"
#include "setup/setup_flow.h"
#include "setup/setup_core.h"
#include "usage_stats.h"
#include "version.h"

extern "C" int UA_main(int argc, char* argv[]);

// SDL's message box needs SDLActivity's JNI side; the Android build maps
// SDL_ShowSimpleMessageBox to this (src/CMakeLists.txt) and logs instead.
extern "C" int UA_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message,
                                       SDL_Window* window) {
    (void)flags;
    (void)window;
    LOGE("message box: %s: %s", title ? title : "", message ? message : "");
    return 0;
}

namespace uavr {

namespace {

Host g_host;
EglContext g_egl;
XrPlatform g_xr;
JavaVM* g_vm = nullptr;

// anonymous usage stats: POST through java.net.HttpURLConnection on the stats thread (docs/usage-stats.md)
int StatsSend(const std::string& url, const std::string& body, int timeoutMs) {
    if (!g_vm) return -1;
    setup::Http h(g_vm);
    return h.postJson(url, body, timeoutMs);
}

// "Report a bug" (pause menu): the site's feedback form in the headset's browser, by an Android VIEW intent.
// Runs on the game thread (android_main's, attached to the VM). The browser comes to the front; the game pauses
// as for any other app switch. The reference also stays on the pause menu row in case the browser didn't open.
bool StatsOpenUrl(const std::string& url) {
    android_app* app = g_host.app;
    if (!g_vm || !app || !app->activity) return false;
    JNIEnv* e = nullptr;
    if (g_vm->GetEnv((void**)&e, JNI_VERSION_1_6) != JNI_OK || !e) return false;
    bool ok = false;
    e->PushLocalFrame(16);
    jclass cUri = e->FindClass("android/net/Uri");
    jclass cIntent = e->FindClass("android/content/Intent");
    jclass cAct = e->GetObjectClass(app->activity->clazz);
    if (cUri && cIntent && cAct) {
        jobject uri = e->CallStaticObjectMethod(cUri, e->GetStaticMethodID(cUri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                                e->NewStringUTF(url.c_str()));
        jobject it = e->NewObject(cIntent, e->GetMethodID(cIntent, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                  e->NewStringUTF("android.intent.action.VIEW"), uri);
        e->CallObjectMethod(it, e->GetMethodID(cIntent, "addFlags", "(I)Landroid/content/Intent;"), 0x10000000);  // NEW_TASK
        e->CallVoidMethod(app->activity->clazz, e->GetMethodID(cAct, "startActivity", "(Landroid/content/Intent;)V"), it);
        ok = !e->ExceptionCheck();
    }
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->PopLocalFrame(nullptr);
    LOGI("report a bug: browser %s", ok ? "asked to open the feedback form" : "could not be opened");
    return ok;
}

// ---- stdout/stderr -> logcat --------------------------------------------------------------
int g_logPipe[2] = {-1, -1};

void* LogThread(void*) {
    FILE* in = fdopen(g_logPipe[0], "r");
    if (!in) return nullptr;
    char line[2048];
    while (fgets(line, sizeof(line), in)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[n - 1] = 0;
        const bool vr = !strncmp(line, "UAVR", 4);
        __android_log_write(ANDROID_LOG_INFO, vr ? "UAVR" : "UA", line);
    }
    return nullptr;
}

void RedirectStdio() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    if (pipe(g_logPipe) != 0) return;
    dup2(g_logPipe[1], STDOUT_FILENO);
    dup2(g_logPipe[1], STDERR_FILENO);
    pthread_t t;
    pthread_create(&t, nullptr, LogThread, nullptr);
    pthread_detach(t);
}

// ---- configuration -------------------------------------------------------------------------
std::string Prop(const char* name) {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get(name, v);
    return v;
}

void SetEnvLine(const std::string& kv, bool overwrite, const char* origin) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) return;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (!overwrite && getenv(k.c_str())) return;
    setenv(k.c_str(), v.c_str(), 1);
    LOGI("config (%s): %s=%s", origin, k.c_str(), v.c_str());
}

void ReadCfg(const std::string& path, const char* origin) {
    std::ifstream cfg(path);
    std::string line;
    while (std::getline(cfg, line)) {
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line[s] == '#') continue;
        if (!line.compare(s, 7, "export ")) s += 7;
        SetEnvLine(line.substr(s), false, origin);
    }
}

// Before the start screens: properties and the player's own file. After them: the app's uavr.cfg
// (just written for the data set as it is now) and the defaults.
void ApplyConfig(const std::string& dataDir, bool appCfg) {
    if (!appCfg) {
        // 1. properties (highest priority)
        // A property value is at most 91 characters, so longer sets continue in
        // debug.uavr.env2 / env3 (scripted-input runs; run_on_quest.sh splits).
        std::string kv;
        for (const char* name : {"debug.uavr.env", "debug.uavr.env2", "debug.uavr.env3"}) {
            std::istringstream props(Prop(name));
            while (props >> kv) SetEnvLine(kv, true, name);
        }
        // The mute property wins over everything, including a UA_MUTE in debug.uavr.env
        // (applied last; run_on_quest.sh checks the engine's effective-mute log line).
        if (Prop("debug.uavr.mute") == "1") SetEnvLine("UA_MUTE=1", true, "debug.uavr.mute");
        // 2. the player's own settings
        ReadCfg(dataDir + "/uavr-user.cfg", "uavr-user.cfg");
        return;
    }
    // 3. the app's settings for this data set
    ReadCfg(dataDir + "/uavr.cfg", "uavr.cfg");
    // 4. defaults
    SetEnvLine("UA_VR=openxr", false, "default");
    struct stat st;
    std::string hd = dataDir + "/hd-models";
    if (stat(hd.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        SetEnvLine("UA_HDMODELS_DIR=" + hd, false, "default");
}

void OnAppCmd(android_app* app, int32_t cmd) {
    Host* h = static_cast<Host*>(app->userData);
    switch (cmd) {
        case APP_CMD_RESUME: h->resumed = true; LOGI("activity resume"); break;
        case APP_CMD_PAUSE: h->resumed = false; LOGI("activity pause"); break;
        case APP_CMD_DESTROY: LOGI("activity destroy"); break;
        default: break;
    }
}

void PostQuit(const char* why) {
    if (g_host.quitPosted) return;
    g_host.quitPosted = true;
    LOGI("posting quit to the engine: %s", why);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
}

}  // namespace

Host* GetHost() { return &g_host; }

void PumpEvents(bool blockWhileIdle) {
    android_app* app = g_host.app;
    for (;;) {
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const bool idle = blockWhileIdle && !app->destroyRequested && !g_host.quitPosted &&
                              !g_xr.sessionRunning() && !g_xr.exitRequested();
            // While idle, wake at least every 100 ms to poll OpenXR (session state events do
            // not arrive through the looper).
            if (ALooper_pollOnce(idle ? 100 : 0, nullptr, &events, (void**)&source) < 0) break;
            if (source) source->process(app, source);
        }
        g_xr.pollEvents();
        if (app->destroyRequested) PostQuit("activity destroy requested");
        if (g_xr.exitRequested()) PostQuit("OpenXR session exiting");
        if (!blockWhileIdle || g_host.quitPosted || g_xr.sessionRunning()) return;
    }
}

}  // namespace uavr

void android_main(android_app* app) {
    using namespace uavr;
    RedirectStdio();
    LOGI("android_main start (UA Reforged VR)");

    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    g_host.app = app;
    g_host.egl = &g_egl;
    g_host.xr = &g_xr;
    app->userData = &g_host;
    app->onAppCmd = OnAppCmd;

    std::string base = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    std::string over = Prop("debug.uavr.datadir");
    g_host.dataDir = !over.empty() ? over : base + "/ua";
    setup::MakeDirs(g_host.dataDir);
    if (chdir(g_host.dataDir.c_str()) != 0)
        LOGE("data dir %s not usable", g_host.dataDir.c_str());
    else
        LOGI("data dir %s", g_host.dataDir.c_str());
    ApplyConfig(g_host.dataDir, false);

    // anonymous usage stats (opt-in): the answer and install number live in the app's private storage
    // (removed with the app or "clear data"); the start screens ask the question, the engine sends.
    // A UA_SETTINGS_DIR from debug.uavr.env wins (testing: an empty folder shows the question again).
    g_vm = app->activity->vm;
    if (app->activity->internalDataPath && app->activity->internalDataPath[0])
        setenv("UA_SETTINGS_DIR", app->activity->internalDataPath, 0);
    UsageStats::SetSender(StatsSend);
    UsageStats::SetUrlOpener(StatsOpenUrl);
    UsageStats::Init(std::string(), "vr", "quest", Common::PortVersion, UA_BUILD_ID);

    XrPlatformOptions opt;
    opt.appName = "UA Reforged VR";
    opt.createStereoSwapchain = false;  // the engine backend owns its swapchains
    opt.seatedLocalSpace = true;
    // Display rate: UA_VR_HZ=72|80|90|120|auto. Unset: 90 Hz (the frame budget the New Look
    // is tuned for; Settings > DISPLAY changes it in the game, see vr_engine.cpp). "auto" =
    // start at the highest offered rate and step down while frames miss. UA_VR_HZ_REQUEST
    // still wins. UA_VR_TARGET_HZ is only the engine's budget fallback.
    {
        const char* hz = getenv("UA_VR_HZ");
        opt.targetRefreshHz = (!hz || !hz[0]) ? 90.f : (!strcmp(hz, "auto") ? 1000.f : (float)atof(hz));
    }
    if (const char* hz = getenv("UA_VR_HZ_REQUEST")) opt.targetRefreshHz = (float)atof(hz);
    if (const char* l = getenv("UA_VR_CPU_LEVEL")) opt.cpuLevel = atoi(l);
    if (const char* l = getenv("UA_VR_GPU_LEVEL")) opt.gpuLevel = atoi(l);

    int rc = -1;
    if (g_egl.init() && g_xr.init(app->activity->vm, app->activity->clazz, g_egl, opt)) {
        // Serve the start-up lifecycle burst (resume, window, focus) and let the OpenXR session
        // reach READY -> xrBeginSession before anything long: the native_app_glue makes the
        // Java UI thread wait for us to acknowledge each command, so leaving them queued for
        // the whole load would risk an ANR.
        for (int i = 0; i < 60 && !(g_host.resumed && g_xr.sessionRunning()) && !app->destroyRequested; i++) {
            int events = 0;
            android_poll_source* source = nullptr;
            while (ALooper_pollOnce(50, nullptr, &events, (void**)&source) >= 0)
                if (source) source->process(app, source);
            g_xr.pollEvents();
        }
        LOGI("pre-start: resumed=%d session running=%d", (int)g_host.resumed, (int)g_xr.sessionRunning());
        std::vector<std::string> extra;
        if (setup::RunSetup(app, g_xr, g_host.dataDir, &extra) && !g_host.quitPosted) {
            ApplyConfig(g_host.dataDir, true);
            std::vector<std::string> args = {"uavr"};
            args.insert(args.end(), extra.begin(), extra.end());
            std::vector<char*> argv;
            for (std::string& a : args) argv.push_back(&a[0]);
            argv.push_back(nullptr);
            LOGI("entering UA_main");
            rc = UA_main((int)args.size(), argv.data());
            LOGI("UA_main returned %d", rc);
        } else {
            LOGI("start screens closed without starting the game");
        }
    } else {
        LOGE("start-up failed (EGL or OpenXR); finishing");
    }

    ANativeActivity_finish(app->activity);
    for (int i = 0; i < 200 && !app->destroyRequested; i++) {  // up to ~2 s for onDestroy
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(10, nullptr, &events, (void**)&source) >= 0)
            if (source) source->process(app, source);
    }
    g_xr.shutdown();
    g_egl.shutdown();
    app->activity->vm->DetachCurrentThread();
    LOGI("android_main exit (process exits, engine statics are not re-entrant)");
    exit(0);
}
