#include "platform/xr/xr_input.h"

#include <cstring>
#include <vector>

namespace uavr {

namespace {

XrPath path(XrInstance inst, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

bool makeAction(XrInstance inst, XrActionSet set, XrAction* out, const char* name,
                const char* localized, XrActionType type, const XrPath* subPaths,
                uint32_t subCount) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ci.actionType = type;
    ci.countSubactionPaths = subCount;
    ci.subactionPaths = subPaths;
    XR_CHECK(inst, xrCreateAction(set, &ci, out));
    return true;
}

}  // namespace

bool XrInput::init(XrInstance instance) {
    instance_ = instance;
    XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(sci.actionSetName, "gameplay");
    std::strcpy(sci.localizedActionSetName, "Gameplay");
    XR_CHECK(instance, xrCreateActionSet(instance, &sci, &set_));

    handPath_[kLeft] = path(instance, "/user/hand/left");
    handPath_[kRight] = path(instance, "/user/hand/right");
    const XrPath* hp = handPath_;

    bool ok = makeAction(instance, set_, &stick_, "thumbstick", "Thumbstick",
                         XR_ACTION_TYPE_VECTOR2F_INPUT, hp, 2) &&
              makeAction(instance, set_, &stickClick_, "thumbstick_click", "Thumbstick click",
                         XR_ACTION_TYPE_BOOLEAN_INPUT, hp, 2) &&
              makeAction(instance, set_, &trigger_, "trigger", "Trigger",
                         XR_ACTION_TYPE_FLOAT_INPUT, hp, 2) &&
              makeAction(instance, set_, &grip_, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT,
                         hp, 2) &&
              makeAction(instance, set_, &aimPose_, "aim_pose", "Aim pose",
                         XR_ACTION_TYPE_POSE_INPUT, hp, 2) &&
              makeAction(instance, set_, &gripPose_, "grip_pose", "Grip pose",
                         XR_ACTION_TYPE_POSE_INPUT, hp, 2) &&
              makeAction(instance, set_, &haptic_, "haptic", "Haptic",
                         XR_ACTION_TYPE_VIBRATION_OUTPUT, hp, 2) &&
              makeAction(instance, set_, &a_, "button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT,
                         nullptr, 0) &&
              makeAction(instance, set_, &b_, "button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT,
                         nullptr, 0) &&
              makeAction(instance, set_, &x_, "button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT,
                         nullptr, 0) &&
              makeAction(instance, set_, &y_, "button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT,
                         nullptr, 0) &&
              makeAction(instance, set_, &menu_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
                         nullptr, 0);
    if (!ok) return false;

    struct B { XrAction a; const char* p; };
    const B binds[] = {
        {stick_, "/user/hand/left/input/thumbstick"},
        {stick_, "/user/hand/right/input/thumbstick"},
        {stickClick_, "/user/hand/left/input/thumbstick/click"},
        {stickClick_, "/user/hand/right/input/thumbstick/click"},
        {trigger_, "/user/hand/left/input/trigger/value"},
        {trigger_, "/user/hand/right/input/trigger/value"},
        {grip_, "/user/hand/left/input/squeeze/value"},
        {grip_, "/user/hand/right/input/squeeze/value"},
        {aimPose_, "/user/hand/left/input/aim/pose"},
        {aimPose_, "/user/hand/right/input/aim/pose"},
        {gripPose_, "/user/hand/left/input/grip/pose"},
        {gripPose_, "/user/hand/right/input/grip/pose"},
        {haptic_, "/user/hand/left/output/haptic"},
        {haptic_, "/user/hand/right/output/haptic"},
        {a_, "/user/hand/right/input/a/click"},
        {b_, "/user/hand/right/input/b/click"},
        {x_, "/user/hand/left/input/x/click"},
        {y_, "/user/hand/left/input/y/click"},
        {menu_, "/user/hand/left/input/menu/click"},
    };
    std::vector<XrActionSuggestedBinding> sb;
    for (const B& b : binds) sb.push_back({b.a, path(instance, b.p)});

    XrInteractionProfileSuggestedBinding spb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    spb.interactionProfile = path(instance, "/interaction_profiles/oculus/touch_controller");
    spb.suggestedBindings = sb.data();
    spb.countSuggestedBindings = static_cast<uint32_t>(sb.size());
    XR_CHECK(instance, xrSuggestInteractionProfileBindings(instance, &spb));
    return true;
}

bool XrInput::attach(XrSession session) {
    session_ = session;
    for (int h = 0; h < kHandCount; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.poseInActionSpace.orientation.w = 1.f;
        ci.subactionPath = handPath_[h];
        ci.action = aimPose_;
        XR_CHECK(instance_, xrCreateActionSpace(session, &ci, &aimSpace_[h]));
        ci.action = gripPose_;
        XR_CHECK(instance_, xrCreateActionSpace(session, &ci, &gripSpace_[h]));
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    XR_CHECK(instance_, xrAttachSessionActionSets(session, &ai));
    return true;
}

void XrInput::sync(XrSpace baseSpace, XrTime time) {
    if (session_ == XR_NULL_HANDLE) return;
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult r = xrSyncActions(session_, &si);
    if (r == XR_SESSION_NOT_FOCUSED) {  // not an error: inputs read as inactive
        state_ = InputState{};
        return;
    }
    XR_LOG_FAIL(instance_, r);

    const InputState prev = state_;
    auto getBool = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session_, &gi, &s);
        return s.isActive && s.currentState;
    };
    auto getFloat = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session_, &gi, &s);
        return s.isActive ? s.currentState : 0.f;
    };

    for (int h = 0; h < kHandCount; ++h) {
        HandState& hs = state_.hand[h];
        const XrPath sub = handPath_[h];

        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = sub;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(session_, &gi, &v);
        hs.stick = v.isActive ? Vec2{v.currentState.x, v.currentState.y} : Vec2{};

        hs.stickClick = getBool(stickClick_, sub);
        hs.trigger = getFloat(trigger_, sub);
        hs.grip = getFloat(grip_, sub);

        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        gi.action = aimPose_;
        xrGetActionStatePose(session_, &gi, &ps);
        hs.active = ps.isActive;

        auto locate = [&](XrSpace space, bool& valid, Pose& pose) {
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            valid = false;
            if (!hs.active || XR_FAILED(xrLocateSpace(space, baseSpace, time, &loc))) return;
            const XrSpaceLocationFlags need =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((loc.locationFlags & need) == need) {
                valid = true;
                pose = toPose(loc.pose);
            }
        };
        locate(aimSpace_[h], hs.aimValid, hs.aimPose);
        locate(gripSpace_[h], hs.gripValid, hs.gripPose);
    }

    state_.a = getBool(a_, XR_NULL_PATH);
    state_.b = getBool(b_, XR_NULL_PATH);
    state_.x = getBool(x_, XR_NULL_PATH);
    state_.y = getBool(y_, XR_NULL_PATH);
    state_.menu = getBool(menu_, XR_NULL_PATH);
    state_.aPressed = state_.a && !prev.a;
    state_.bPressed = state_.b && !prev.b;
    state_.xPressed = state_.x && !prev.x;
    state_.yPressed = state_.y && !prev.y;
    state_.menuPressed = state_.menu && !prev.menu;
}

void XrInput::haptic(Hand hand, float amplitude, float seconds, float frequency) {
    if (session_ == XR_NULL_HANDLE) return;
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amplitude;
    vib.duration = static_cast<XrDuration>(seconds * 1e9);
    vib.frequency = frequency;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = handPath_[hand];
    XR_LOG_FAIL(instance_, xrApplyHapticFeedback(session_, &hi,
                                                 reinterpret_cast<XrHapticBaseHeader*>(&vib)));
}

void XrInput::shutdown() {
    for (int h = 0; h < kHandCount; ++h) {
        if (aimSpace_[h]) xrDestroySpace(aimSpace_[h]);
        if (gripSpace_[h]) xrDestroySpace(gripSpace_[h]);
        aimSpace_[h] = gripSpace_[h] = XR_NULL_HANDLE;
    }
    if (set_) xrDestroyActionSet(set_);  // destroys its actions too
    set_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
}

}  // namespace uavr
