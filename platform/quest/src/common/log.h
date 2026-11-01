// Logging to logcat. Tag UAVR is what the perf scripts grep for.
#pragma once

#include <android/log.h>

#define UAVR_TAG "UAVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, UAVR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, UAVR_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, UAVR_TAG, __VA_ARGS__)
