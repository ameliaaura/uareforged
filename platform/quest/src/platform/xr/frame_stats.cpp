#include "platform/xr/frame_stats.h"

#include <GLES2/gl2ext.h>

#include <cstring>
#include <ctime>

#include "common/log.h"

namespace uavr {

double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void FrameStats::initGpuTimer() {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    gpuTimer_ = ext && std::strstr(ext, "GL_EXT_disjoint_timer_query");
    if (gpuTimer_) glGenQueries(kQueries, queries_);
    LOGI("boot: gpu timer queries %s", gpuTimer_ ? "available" : "unavailable");
}

void FrameStats::gpuBegin() {
    if (!gpuTimer_ || queryActive_) return;
    collectGpu();
    if (queryPending_[queryIndex_]) return;  // ring full, skip this frame's sample
    glBeginQuery(GL_TIME_ELAPSED_EXT, queries_[queryIndex_]);
    queryActive_ = true;
}

void FrameStats::gpuEnd() {
    if (!queryActive_) return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    queryPending_[queryIndex_] = true;
    queryIndex_ = (queryIndex_ + 1) % kQueries;
    queryActive_ = false;
}

void FrameStats::collectGpu() {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (int i = 0; i < kQueries; ++i) {
        if (!queryPending_[i]) continue;
        GLuint available = 0;
        glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        GLuint ns = 0;
        glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT, &ns);
        queryPending_[i] = false;
        if (disjoint) continue;
        const double ms = ns * 1e-6;
        gpuSum_ += ms;
        if (ms > gpuMax_) gpuMax_ = ms;
        ++gpuCount_;
    }
}

void FrameStats::frame(double intervalMs, double cpuMs, double predictedPeriodMs, bool rendered) {
    const double t = nowSeconds();
    if (windowStartS_ < 0) windowStartS_ = t;
    ++frames_;
    ++totalFrames_;
    if (!rendered) ++skipped_;
    if (intervalMs > 0) {
        intervalSum_ += intervalMs;
        if (intervalMs > intervalMax_) intervalMax_ = intervalMs;
        if (predictedPeriodMs > 0 && intervalMs > 1.5 * predictedPeriodMs) ++late_;
    }
    cpuSum_ += cpuMs;
    if (cpuMs > cpuMax_) cpuMax_ = cpuMs;

    const double span = t - windowStartS_;
    if (span < 1.0) return;
    const double fps = frames_ / span;
    LOGI("perf: fps=%.1f hz=%.0f interval_ms avg=%.2f max=%.2f cpu_ms avg=%.2f max=%.2f "
         "gpu_ms avg=%.2f max=%.2f late=%d skipped=%d period_ms=%.2f mv=%d msaa=%d frames=%llu",
         fps, refreshHz_, intervalSum_ / frames_, intervalMax_, cpuSum_ / frames_, cpuMax_,
         gpuCount_ ? gpuSum_ / gpuCount_ : -1.0, gpuCount_ ? gpuMax_ : -1.0, late_, skipped_,
         predictedPeriodMs, multiview_ ? 1 : 0, msaa_, (unsigned long long)totalFrames_);
    windowStartS_ = t;
    frames_ = skipped_ = late_ = gpuCount_ = 0;
    intervalSum_ = intervalMax_ = cpuSum_ = cpuMax_ = gpuSum_ = gpuMax_ = 0;
}

}  // namespace uavr
