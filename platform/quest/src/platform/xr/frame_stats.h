// Per-second frame timing to logcat (tag UAVR), measurable over adb with nobody in the headset:
//   adb logcat -s UAVR:I | grep perf
// interval = xrWaitFrame-to-xrWaitFrame (what the compositor paced us at)
// cpu      = xrWaitFrame return -> xrEndFrame return (our CPU work incl. GL submission)
// gpu      = GL_EXT_disjoint_timer_query around the eye rendering (approximate on a tiler)
// late     = intervals > 1.5x the predicted display period (likely a dropped/repeated frame)
#pragma once

#include <GLES3/gl3.h>

#include <cstdint>

namespace uavr {

class FrameStats {
public:
    void initGpuTimer();                     // call once with a current GL context
    void gpuBegin();
    void gpuEnd();
    // Called once per frame after xrEndFrame.
    void frame(double intervalMs, double cpuMs, double predictedPeriodMs, bool rendered);
    void setInfo(float refreshHz, bool multiview, int msaa) {
        refreshHz_ = refreshHz;
        multiview_ = multiview;
        msaa_ = msaa;
    }

private:
    void collectGpu();

    static constexpr int kQueries = 4;
    bool gpuTimer_ = false;
    GLuint queries_[kQueries] = {};
    bool queryPending_[kQueries] = {};
    int queryIndex_ = 0;
    bool queryActive_ = false;

    double windowStartS_ = -1;
    int frames_ = 0, skipped_ = 0, late_ = 0;
    double intervalSum_ = 0, intervalMax_ = 0, cpuSum_ = 0, cpuMax_ = 0;
    double gpuSum_ = 0;
    int gpuCount_ = 0;
    double gpuMax_ = 0;
    float refreshHz_ = 0;
    bool multiview_ = false;
    int msaa_ = 1;
    uint64_t totalFrames_ = 0;
};

double nowSeconds();

}  // namespace uavr
