// OpenXR input actions bound to the Oculus Touch interaction profile.
// Everything the engine may need: both sticks (+click), triggers, grips, A/B/X/Y, menu,
// aim + grip poses and haptics. The rest of the code only sees InputState.
#pragma once

#include "platform/xr/xr_common.h"

namespace uavr {

enum Hand { kLeft = 0, kRight = 1, kHandCount = 2 };

struct HandState {
    bool active = false;        // controller tracked / bound
    Vec2 stick;                 // thumbstick [-1,1]
    bool stickClick = false;
    float trigger = 0.f;        // [0,1]
    float grip = 0.f;           // squeeze [0,1]
    bool aimValid = false, gripValid = false;
    Pose aimPose, gripPose;     // in the app reference space (tracking space)
};

struct InputState {
    HandState hand[kHandCount];
    bool a = false, b = false;  // right controller
    bool x = false, y = false;  // left controller
    bool menu = false;          // left controller menu button
    // Rising edges since the previous sync, handy for toggles.
    bool aPressed = false, bPressed = false, xPressed = false, yPressed = false,
         menuPressed = false;
};

class XrInput {
public:
    bool init(XrInstance instance);                 // create action set + suggest bindings
    bool attach(XrSession session);                 // attach set + create pose spaces
    void sync(XrSpace baseSpace, XrTime time);      // xrSyncActions + locate poses
    void haptic(Hand hand, float amplitude, float seconds, float frequency = XR_FREQUENCY_UNSPECIFIED);
    void shutdown();

    const InputState& state() const { return state_; }

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath handPath_[kHandCount] = {};

    XrAction stick_ = XR_NULL_HANDLE, stickClick_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE;
    XrAction aimPose_ = XR_NULL_HANDLE, gripPose_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE;
    XrAction a_ = XR_NULL_HANDLE, b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE,
             y_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE;

    XrSpace aimSpace_[kHandCount] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace gripSpace_[kHandCount] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    InputState state_;
};

}  // namespace uavr
