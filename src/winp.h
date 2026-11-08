#ifndef WINP_H_INCLUDED
#define WINP_H_INCLUDED

#include <map>
#include <queue>
#include "idev.h"
#include "utils.h"
#include "system/inpt.h"

// Joystick/pad diagnostic log: env/sdlInputLog.txt in the data dir, mirrored
// to stderr as "[UA_JOY] ..." when UA_JOY_DEBUG=1.
void sdlInputLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

class NC_STACK_winp: public NC_STACK_idev
{
public:
    enum 
    {
        MAXCHARQUEUE = 8,
        MAXJOYMAP    = 32,
        MAXJOYHAT    = 4,
    };

public:
    static constexpr const char * __ClassName = "winp.class";

public:
    virtual size_t Init(IDVList &stak);
    virtual size_t Deinit();
    
    virtual bool GetState();
    virtual float GetSlider();
    
    virtual bool BindKey(const std::string &keyName);
    virtual void ResetSlider();

    NC_STACK_winp() {};
    virtual ~NC_STACK_winp() {};
    
    virtual const std::string ClassName() const {
        return __ClassName;
    };
  
public:
    static void InitFirst();
    
    static void OnMouseDown(Common::Point pos, int btn, int clkNum);
    static void OnMouseUp(Common::Point pos, int btn, int clkNum);
    static void OnMouseMove(Common::Point pos, Common::Point rel);
    
    static SDL_Haptic * GetJoyHaptic() { return _joyHaptic; };

    // Pad rumble (DualSense & friends). Magnitudes 0..65535, ms duration.
    // Returns false if no game controller is open or it cannot rumble, which
    // is what keeps the legacy SDL_Haptic path in charge for real FF sticks.
    static bool Rumble(uint16_t lowFreq, uint16_t highFreq, uint32_t ms);
    static bool HasRumble();
    static bool JoyDebug() { return _joyDebug; };
    // the last poll read a game controller (throttle = momentary R1/L1 buttons)
    static bool PadThrottleButtons() { return _padThrottle; };

    static void QueryKeyboard(TInputState *arg);
    static void QueryPointer(TClickBoxInf *arg);
    static bool HasFocus() { return true; };
    
    static Input::TQueryState GetQueryInterface()
    { return Input::TQueryState(__ClassName, QueryKeyboard, QueryPointer, HasFocus); };
    
protected:
    static void CheckJoy();
    static void KeyDown(int16_t vk);
    static void KeyUp(int16_t vk);
    static int InputWatch(void *, SDL_Event *event);

    static SDL_JoystickGUID sdlReadJoyGuid();
    static bool sdlGUIDcmp(SDL_JoystickGUID &gd1, SDL_JoystickGUID &gd2);
    static int sdlJoyAxis(SDL_Joystick* joystick, int axis);
    static void sdlJoyReadMapping(SDL_Joystick* joystick);

    static void JoyPickAndOpen();
    static void JoyOpenDevice(int deviceIndex);
    static void JoyCloseDevice();
    static void CheckGameController(uint32_t *btnState);
    static void CheckVRControllers(uint32_t *btnState);
    static void VRPointerButton(bool &state, bool down, int sdlBtn);
    static void ReleaseVRPointer();
    static int GameCtrlAxisVal(SDL_GameControllerAxis axis);
   

public:
    int16_t _bindedKey = -1;
    int32_t _sliderPos = 0;
    
    static std::map<int16_t, int16_t> KBDMapping;
    
protected:
    static int _kbdLastDown;
    static int _kbdLastHit;
    static std::queue<int32_t> _inputText;
    
    
    static bool _mLstate;
    static bool _mRstate;
    static bool _mMstate;
    static bool _mDBLstate;

    static int _mLUcnt, _mLDcnt;
    static int _mRUcnt, _mRDcnt;
    static int _mMUcnt, _mMDcnt;

    static Common::Point _mPos;
    static Common::Point _mMove;
    static Common::Point _mMoveQuery; // Read on event and null it on query

    static Common::Point _mLDpos;
    static Common::Point _mRDpos;
    static Common::Point _mMDpos;

    static Common::Point _mLUpos;
    static Common::Point _mRUpos;
    static Common::Point _mMUpos;
    
    
    static bool                 _joyEnable;
    static SDL_JoystickGUID     _joyWantGuid;

    static SDL_Joystick        *_joyHandle;
    static SDL_GameController  *_gameCtrl;
    static SDL_JoystickID       _joyInstanceId;
    static SDL_Haptic          *_joyHaptic;
    static bool                 _joyDebug;
    static bool                 _joyStartHeld;
    static bool                 _joyPauseHeld;
    static bool                 _padThrottle;

    static std::array<int, MAXJOYMAP>  _joyAxisMap;
    static std::array<bool, MAXJOYMAP> _joyAxisMapInv;
    static std::array<int, MAXJOYMAP>  _joyButtonMap;
    static std::array<int, MAXJOYHAT>  _joyHatMap;

    static uint32_t             _joyButtonStates;
    static Common::Point        _joyPov;
    static Common::Point        _joyXYpos;
    static Common::Point        _joyZRZpos;
};

#endif // WINP_H_INCLUDED
