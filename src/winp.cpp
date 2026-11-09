#include <inttypes.h>
#include <cstdlib>
#include <cstring>
#include "includes.h"
#include "nucleas.h"
#include "system/inpt.h"
#include "winp.h"
#include "utils.h"
#include "log.h"
#include "gui/widget.h"
#include "usage_stats.h"
#ifdef UA_VR_ENABLED
#include <cmath>
#include "vr/vr.h"
#include "vr/vr_grip.h"
#endif

std::map<int16_t, int16_t> NC_STACK_winp::KBDMapping;

int NC_STACK_winp::_kbdLastDown;
int NC_STACK_winp::_kbdLastHit;
std::queue<int32_t> NC_STACK_winp::_inputText;

bool NC_STACK_winp::_mLstate;
bool NC_STACK_winp::_mRstate;
bool NC_STACK_winp::_mMstate;
bool NC_STACK_winp::_mDBLstate;

int NC_STACK_winp::_mLUcnt, NC_STACK_winp::_mLDcnt;
int NC_STACK_winp::_mRUcnt, NC_STACK_winp::_mRDcnt;
int NC_STACK_winp::_mMUcnt, NC_STACK_winp::_mMDcnt;

Common::Point NC_STACK_winp::_mPos;
Common::Point NC_STACK_winp::_mMove;
Common::Point NC_STACK_winp::_mMoveQuery;

Common::Point NC_STACK_winp::_mLDpos;
Common::Point NC_STACK_winp::_mRDpos;
Common::Point NC_STACK_winp::_mMDpos;

Common::Point NC_STACK_winp::_mLUpos;
Common::Point NC_STACK_winp::_mRUpos;
Common::Point NC_STACK_winp::_mMUpos;


bool                 NC_STACK_winp::_joyEnable = false;
SDL_JoystickGUID     NC_STACK_winp::_joyWantGuid;

SDL_Joystick        *NC_STACK_winp::_joyHandle = NULL;
SDL_GameController  *NC_STACK_winp::_gameCtrl = NULL;
SDL_JoystickID       NC_STACK_winp::_joyInstanceId = -1;
SDL_Haptic          *NC_STACK_winp::_joyHaptic = NULL;
bool                 NC_STACK_winp::_joyDebug = false;
bool                 NC_STACK_winp::_joyStartHeld = false;
bool                 NC_STACK_winp::_joyPauseHeld = false;
bool                 NC_STACK_winp::_padThrottle = false;

std::array<int, 32>  NC_STACK_winp::_joyAxisMap;
std::array<bool, 32> NC_STACK_winp::_joyAxisMapInv;
std::array<int, 32>  NC_STACK_winp::_joyButtonMap;
std::array<int, 4>   NC_STACK_winp::_joyHatMap;

uint32_t             NC_STACK_winp::_joyButtonStates;
Common::Point        NC_STACK_winp::_joyPov;
Common::Point        NC_STACK_winp::_joyXYpos;
Common::Point        NC_STACK_winp::_joyZRZpos;




void sdlInputResetLog()
{
    FSMgr::FileHandle *v0 = uaOpenFileAlloc("env/sdlInputLog.txt", "w");
    if ( v0 )
    {
        v0->printf("YPA SDL Input log\n---------------------\n");
        delete v0;
    }
}

static bool sdlInputLogMirror = false; // UA_JOY_DEBUG=1: mirror joystick log to stderr

void sdlInputLog(const char *format, ...)
{
    va_list va;
    va_start(va, format);

    if ( sdlInputLogMirror )
    {
        va_list va2;
        va_copy(va2, va);
        fprintf(stderr, "[UA_JOY] ");
        vfprintf(stderr, format, va2);
        va_end(va2);
    }

    FSMgr::FileHandle *f = uaOpenFileAlloc("env/sdlInputLog.txt", "a");
    if ( f )
    {
        f->vprintf(format, va);
        delete f;
    }
    va_end(va);
}

SDL_JoystickGUID NC_STACK_winp::sdlReadJoyGuid()
{
    SDL_JoystickGUID guid;

    FSMgr::FileHandle *f = uaOpenFileAlloc("env/sdlJoy.txt", "r");
    if ( f )
    {
        std::string buf;
        f->ReadLine(&buf);
        delete f;

        size_t endp = buf.find_first_of("\r\n");

        if (endp != std::string::npos)
            buf.erase(endp);

        guid = SDL_JoystickGetGUIDFromString(buf.c_str());
    }
    else
        std::memset(&guid, 0, sizeof(SDL_JoystickGUID));

    return guid;
}

bool NC_STACK_winp::sdlGUIDcmp(SDL_JoystickGUID &gd1, SDL_JoystickGUID &gd2)
{
    return std::memcmp(&gd1, &gd2, sizeof(SDL_JoystickGUID)) == 0;
}

int NC_STACK_winp::sdlJoyAxis(SDL_Joystick* joystick, int axis)
{
    int tmp = SDL_JoystickGetAxis(joystick, _joyAxisMap[axis]);

    if ( _joyAxisMapInv[axis] )
        tmp = -tmp;

    if (abs(tmp) <= 6553) // 20% deadzone
        return 0;

    tmp /= 109; //-32768/32767  to -300/300

    if (tmp > 300)
        tmp = 300;
    else if (tmp < -300)
        tmp = -300;

    return tmp;
}

void NC_STACK_winp::sdlJoyReadMapping(SDL_Joystick* joystick)
{
    char bf[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), bf, 64);
    std::string guid(bf);

    for (size_t i = 0; i < MAXJOYHAT; i++)
        _joyHatMap[i] = i;

    for (size_t i = 0; i < MAXJOYMAP; i++)
    {
        _joyAxisMap[i] = i;
        _joyAxisMapInv[i] = false;
        _joyButtonMap[i] = i;
    }

    FSMgr::FileHandle *f = uaOpenFileAlloc("joyconf.txt", "r");

    if ( f )
    {
        std::string buf;
        while ( f->ReadLine(&buf) )
        {
            std::vector<std::string> splt = Stok::Split(buf, " :\t\n\r");
            if (splt.size() >= 3 && !StriCmp(splt[0], bf))
            {
                for(size_t i = 0; i < ((splt.size() - 1) / 2); i++)
                {
                    std::string &tp = splt.at(1 + i * 2);
                    std::string &val = splt.at(1 + i * 2 + 1);
                    
                    if(tolower(tp[0]) == 'a')
                    {
                        int axis = std::stoi( tp.substr(1) );
                        int realaxis = std::stoi( val );

                        if ( axis < MAXJOYMAP && axis >= 0 && abs(realaxis) < MAXJOYMAP)
                        {
                            if ( realaxis < 0 )
                                _joyAxisMapInv[ axis ] = true;
                            
                            realaxis = abs(realaxis);
                            
                            _joyAxisMap[ axis ] = realaxis;
                            sdlInputLog("\tAxis map %c%d -> %d\n", (_joyAxisMapInv[ axis ] ? '-' : ' '), realaxis, axis);
                        }

                    }
                    else if (tolower(tp[0]) == 'b')
                    {
                        int btn = std::stoi( tp.substr(1) );
                        int realbtn = abs(  std::stoi( val )  );

                        if ( btn < MAXJOYMAP && btn >= 0 && realbtn < MAXJOYMAP)
                        {
                            _joyButtonMap[ btn ] = realbtn;
                            sdlInputLog("\tButton map %d -> %d\n", realbtn, btn);
                        }
                    }
                    else if (tolower(tp[0]) == 'h')
                    {
                        int hat = std::stoi( tp.substr(1) );
                        int realhat = abs(  std::stoi( val )  );

                        if ( hat < MAXJOYHAT && hat >= 0 && realhat < MAXJOYHAT)
                        {
                            _joyHatMap[ hat ] = realhat;
                            sdlInputLog("\tHat map %d -> %d\n", realhat, hat);
                        }
                    }
                }
                break;
            }
        }

        delete f;
    }
}


// Open device by index: SDL_GameController path for recognized pads
// (DualSense, DualShock, Xbox, ...), raw SDL_Joystick for everything else.
void NC_STACK_winp::JoyOpenDevice(int deviceIndex)
{
    if ( _joyHandle )
        return;

    if ( SDL_IsGameController(deviceIndex) )
    {
        _gameCtrl = SDL_GameControllerOpen(deviceIndex);
        if ( _gameCtrl )
            _joyHandle = SDL_GameControllerGetJoystick(_gameCtrl); // owned by _gameCtrl
        else
            sdlInputLog("GameController #%d open failed: %s\n", deviceIndex, SDL_GetError());
    }

    if ( !_joyHandle )
    {
        _gameCtrl = NULL;
        _joyHandle = SDL_JoystickOpen(deviceIndex);
    }

    if ( !_joyHandle )
    {
        sdlInputLog("Joystick #%d open failed: %s\n", deviceIndex, SDL_GetError());
        return;
    }

    _joyInstanceId = SDL_JoystickInstanceID(_joyHandle);
    _joyHaptic = SDL_HapticOpenFromJoystick(_joyHandle);
    _joyEnable = true;

    if ( _gameCtrl )
    {
        const char *nm = SDL_GameControllerName(_gameCtrl);
        sdlInputLog("\nSelected game controller: %s (instance %d)\n", nm ? nm : "?", (int)_joyInstanceId);
        sdlInputLog("\tPad map: LStick=steer/move  RStick=aim  RT=fire  LT=machine gun\n"
                    "\t         R1/L1=throttle fwd/rev  Cross=brake  Triangle=missile cam\n"
                    "\t         DPad=gun aim (hat)  Circle/Square/L3/R3=JoyB4..7  Options=Esc\n");

        if ( _joyDebug )
        {
            // Tactile "the pad is alive and rumble reaches it" confirmation.
            if ( Rumble(0x6000, 0x6000, 250) )
                sdlInputLog("\tRumble test pulse: low=24576 high=24576 dur=250ms\n");
            else
                sdlInputLog("\tRumble test pulse FAILED (no rumble on this device)\n");
        }
    }
    else
    {
        sdlInputLog("\nSelected joystick: %s (instance %d)\n", SDL_JoystickName(_joyHandle), (int)_joyInstanceId);
        sdlInputLog("\tAxes:%d\tHats:%d\tButtons:%d\tBalls:%d\n", SDL_JoystickNumAxes(_joyHandle), SDL_JoystickNumHats(_joyHandle), SDL_JoystickNumButtons(_joyHandle), SDL_JoystickNumBalls(_joyHandle));
        sdlJoyReadMapping(_joyHandle);
    }
    sdlInputLog("\n");
}

void NC_STACK_winp::JoyCloseDevice()
{
    if ( !_joyHandle )
        return;

    // Release everything the pad held down so no key sticks after unplug
    for (int i = 0; i < 8; i++)
    {
        if ( _joyButtonStates & (1 << i) )
            KeyUp(i + Input::KC_JOYB0);
    }
    _joyButtonStates = 0;

    if ( _joyStartHeld )
    {
        KeyUp(Input::KC_ESCAPE);
        _joyStartHeld = false;
    }

    if ( _joyPauseHeld )
    {
        KeyUp(Input::KC_PAUSE);
        _joyPauseHeld = false;
    }
    _padThrottle = false;

    _joyXYpos = Common::Point();
    _joyZRZpos = Common::Point();
    _joyPov = Common::Point();

    if ( _joyHaptic )
    {
        SDL_HapticClose(_joyHaptic);
        _joyHaptic = NULL;
    }

    if ( _gameCtrl )
    {
        SDL_GameControllerRumble(_gameCtrl, 0, 0, 0); // no motor left running
        SDL_GameControllerClose(_gameCtrl);
        _gameCtrl = NULL;
    }
    else
    {
        SDL_JoystickClose(_joyHandle);
    }

    _joyHandle = NULL;
    _joyInstanceId = -1;
    _joyEnable = false;
}

// Pad rumble. The engine's force-feedback engine was written for DirectInput
// style SDL_Haptic effects (sine/ramp/damper), which a DualSense does not
// expose — SDL_HapticOpenFromJoystick gives nothing usable, so every FFDO*
// call was a no-op and the pad stayed silent. SDL_GameControllerRumble drives
// the two ERM motors directly instead; Input::INPEngine maps the legacy
// effects onto it.
bool NC_STACK_winp::HasRumble()
{
#ifdef UA_VR_ENABLED
    if ( VR::Active() )
        return true;
#endif
    return _gameCtrl != NULL;
}

bool NC_STACK_winp::Rumble(uint16_t lowFreq, uint16_t highFreq, uint32_t ms)
{
#ifdef UA_VR_ENABLED
    if ( VR::Active() )
    {
        // Camera shake never moves the VR view; it arrives here and goes to the hands.
        VR::Rumble(lowFreq / 65535.0f, highFreq / 65535.0f, ms);
        return true;
    }
#endif
    if ( !_gameCtrl )
        return false;

    if ( SDL_GameControllerRumble(_gameCtrl, lowFreq, highFreq, ms) != 0 )
    {
        static bool warned = false;
        if ( !warned )
        {
            warned = true;
            sdlInputLog("Rumble unavailable on this controller: %s\n", SDL_GetError());
        }
        return false;
    }

    return true;
}

// Enumerate devices and open the best one:
// env/sdlJoy.txt GUID match > first recognized game controller > device 0.
void NC_STACK_winp::JoyPickAndOpen()
{
    if ( _joyHandle )
        return;

    int numJoy = SDL_NumJoysticks();
    int pick = -1;
    int firstCtrl = -1;

    for (int i = 0; i < numJoy; i++)
    {
        SDL_JoystickGUID jGUID = SDL_JoystickGetDeviceGUID(i);

        char guidBuff[64];
        SDL_JoystickGetGUIDString(jGUID, guidBuff, sizeof(guidBuff));

        bool isCtrl = SDL_IsGameController(i) == SDL_TRUE;
        const char *cName = isCtrl ? SDL_GameControllerNameForIndex(i) : NULL;

        sdlInputLog("Found joystick #%d: %s\n\tGUID:%s\n\tGame controller: %s\n",
                    i, SDL_JoystickNameForIndex(i), guidBuff, cName ? cName : "no");

        if ( pick == -1 && sdlGUIDcmp(jGUID, _joyWantGuid) )
            pick = i;

        if ( firstCtrl == -1 && isCtrl )
            firstCtrl = i;
    }

    if ( pick == -1 )
        pick = (firstCtrl != -1) ? firstCtrl : (numJoy > 0 ? 0 : -1);

    if ( pick == -1 )
    {
        sdlInputLog("No joysticks found\n");
        return;
    }

    JoyOpenDevice(pick);
}


void NC_STACK_winp::KeyDown(int16_t vk)
{
    if ( vk != Input::KC_NONE )
        Input::Engine.KeyMatrix.at(vk).down = true;

    _kbdLastDown = vk;
    _kbdLastHit = vk;
}

void NC_STACK_winp::KeyUp(int16_t vk)
{
    if ( vk != Input::KC_NONE )
        Input::Engine.KeyMatrix.at(vk).down = false;

    if (_kbdLastDown == vk)
        _kbdLastDown = Input::KC_NONE;
}

int NC_STACK_winp::InputWatch(void *, SDL_Event *event)
{
    switch(event->type)
    {
    case SDL_KEYDOWN:
        {
            if ( !event->key.repeat )
                UsageStats::NoteInput(UsageStats::INPUT_KBM); // usage stats: what kind of controls (a counter)
            auto it = KBDMapping.find(event->key.keysym.scancode);
            if (it != KBDMapping.end())
                KeyDown(it->second);
        }
        break;

    case SDL_KEYUP:
        {
            auto it = KBDMapping.find(event->key.keysym.scancode);
            if (it != KBDMapping.end())
                KeyUp(it->second);
        }
        break;

    case SDL_TEXTINPUT:
    {
        for( char c : event->text.text)
        {
            if (!c || _inputText.size() >= MAXCHARQUEUE)
                break;
            
            _inputText.push(c);
        }
    }
    break;

    case SDL_MOUSEBUTTONDOWN:
    {
        UsageStats::NoteInput(UsageStats::INPUT_KBM);
        Common::Point pos = GFX::GFXEngine::Instance.ConvertPosTo2DStuff( Common::Point(event->button.x, event->button.y) );
        if ( !Gui::Root::Instance.MouseDown(pos, event->button.button, event->button.clicks) )
            OnMouseDown(pos, event->button.button, event->button.clicks);
    }
    break;

    case SDL_MOUSEBUTTONUP:
    {
        Common::Point pos = GFX::GFXEngine::Instance.ConvertPosTo2DStuff( Common::Point(event->button.x, event->button.y) );
        if ( !Gui::Root::Instance.MouseUp(pos, event->button.button, event->button.clicks) )
            OnMouseUp(pos, event->button.button, event->button.clicks);
    }
    break;

    case SDL_MOUSEMOTION:
    {
        Common::Point pos = GFX::GFXEngine::Instance.ConvertPosTo2DStuff( Common::Point(event->button.x, event->button.y) );
        Common::Point rel = Common::Point(event->motion.xrel, event->motion.yrel);
        if ( !Gui::Root::Instance.MouseMove(pos, rel) )
            OnMouseMove(pos, rel);
    }
    break;

    case SDL_CONTROLLERDEVICEADDED: // .which = device index; fires on hot-plug AND for devices present at init
    {
        const char *nm = SDL_GameControllerNameForIndex(event->cdevice.which);
        sdlInputLog("Controller connected (device %d): %s\n", event->cdevice.which, nm ? nm : "?");
        JoyOpenDevice(event->cdevice.which);
    }
    break;

    case SDL_JOYDEVICEADDED: // .which = device index
    {
        // Recognized game controllers also raise SDL_CONTROLLERDEVICEADDED - let that path handle them
        if ( !SDL_IsGameController(event->jdevice.which) )
        {
            sdlInputLog("Joystick connected (device %d): %s\n", event->jdevice.which, SDL_JoystickNameForIndex(event->jdevice.which));
            JoyOpenDevice(event->jdevice.which);
        }
    }
    break;

    case SDL_CONTROLLERDEVICEREMOVED: // .which = instance id
    case SDL_JOYDEVICEREMOVED:        // .which = instance id
    {
        SDL_JoystickID inst = (event->type == SDL_CONTROLLERDEVICEREMOVED) ? event->cdevice.which
                                                                           : event->jdevice.which;
        if ( _joyHandle && inst == _joyInstanceId )
        {
            sdlInputLog("Active controller disconnected (instance %d)\n", (int)inst);
            JoyCloseDevice();
            JoyPickAndOpen(); // fall back to any other connected device
        }
    }
    break;

    case SDL_MOUSEWHEEL:
        UsageStats::NoteInput(UsageStats::INPUT_KBM);
        break;

    case SDL_JOYBUTTONDOWN: // plain joysticks only (a game controller also sends SDL_CONTROLLERBUTTONDOWN)
        if ( !SDL_GameControllerFromInstanceID(event->jbutton.which) )
            UsageStats::NoteInput(UsageStats::INPUT_PAD);
        break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
        if ( event->type == SDL_CONTROLLERBUTTONDOWN )
            UsageStats::NoteInput(UsageStats::INPUT_PAD);
        if ( _joyDebug )
            sdlInputLog("Pad button %s: %s\n",
                        SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event->cbutton.button),
                        event->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
    }
    break;

    case SDL_CONTROLLERAXISMOTION:
    {
        // usage stats: a stick or trigger pushed past a third of its travel counts once per push
        if ( event->caxis.axis < SDL_CONTROLLER_AXIS_MAX )
        {
            static std::array<bool, SDL_CONTROLLER_AXIS_MAX> pushed = {};
            const bool on = abs(event->caxis.value) > 10923;
            if ( on && !pushed[event->caxis.axis] )
                UsageStats::NoteInput(UsageStats::INPUT_PAD);
            pushed[event->caxis.axis] = on;
        }
        if ( _joyDebug && event->caxis.axis < SDL_CONTROLLER_AXIS_MAX )
        {
            // Log only deadzone crossings, not the event flood
            static std::array<bool, SDL_CONTROLLER_AXIS_MAX> engaged = {};
            bool on = abs(event->caxis.value) > 6553;
            if ( on != engaged[event->caxis.axis] )
            {
                engaged[event->caxis.axis] = on;
                sdlInputLog("Pad axis %s: %s (%d)\n",
                            SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)event->caxis.axis),
                            on ? "engaged" : "centered", event->caxis.value);
            }
        }
    }
    break;

    default:
        break;
    }

    return 1;
}


void NC_STACK_winp::OnMouseDown(Common::Point pos, int btn, int clkNum)
{
    if (btn == SDL_BUTTON_LEFT)
    {
        _mLstate = true;
        _mLDpos = pos;

        _mLDcnt++;

        KeyDown(Input::KC_LMB);
    }
    else if (btn == SDL_BUTTON_RIGHT)
    {
        _mRstate = true;
        _mRDpos = pos;

        _mRDcnt++;

        KeyDown(Input::KC_RMB);
    }
    else if (btn == SDL_BUTTON_MIDDLE)
    {
        _mMstate = true;
        _mMDpos = pos;

        _mMDcnt++;

        KeyDown(Input::KC_MMB);
    }
}


void NC_STACK_winp::OnMouseUp(Common::Point pos, int btn, int clkNum)
{
    if (btn == SDL_BUTTON_LEFT)
    {
        _mLstate = false;
        _mLUpos = pos;

        _mLUcnt++;

        KeyUp(Input::KC_LMB);

        if ((clkNum & 1) == 0)
            _mDBLstate = true;
    }
    else if (btn == SDL_BUTTON_RIGHT)
    {
        _mRstate = false;
        _mRUpos = pos;

        _mRUcnt++;

        KeyUp(Input::KC_RMB);
    }
    else if (btn == SDL_BUTTON_MIDDLE)
    {
        _mMstate = false;
        _mMUpos = pos;

        _mMUcnt++;

        KeyUp(Input::KC_MMB);
    }
}


void NC_STACK_winp::OnMouseMove(Common::Point pos, Common::Point rel)
{
    _mPos = pos;
    _mMoveQuery = rel;
}


size_t NC_STACK_winp::Init(IDVList &stak)
{
    if ( !NC_STACK_idev::Init(stak) )
        return 0;

    _bindedKey = -1;

    return 1;
}

size_t NC_STACK_winp::Deinit()
{
    return NC_STACK_idev::Deinit();
}


bool NC_STACK_winp::GetState()
{
    if ( _bindedKey < 0 )
        return false;

    switch ( _bindedKey )
    {
    case Input::KC_LMB:
        return _mLstate;
        break;
    case Input::KC_RMB:
        return _mRstate;
        break;
    case Input::KC_MMB:
        return _mMstate;
        break;
    case Input::KC_JOYB0:
    case Input::KC_JOYB1:
    case Input::KC_JOYB2:
    case Input::KC_JOYB3:
    case Input::KC_JOYB4:
    case Input::KC_JOYB5:
    case Input::KC_JOYB6:
    case Input::KC_JOYB7:
        if ( _joyButtonStates & (1 << (_bindedKey - Input::KC_JOYB0)) )
            return true;
        else
            return false;
        break;
    default:
        return Input::Engine.KeyMatrix.at(_bindedKey).down;
    }
    
    return false;
}

float NC_STACK_winp::GetSlider()
{
    if ( _bindedKey < 0 )
        return 0.0;

    int v6 = 1;

    switch ( _bindedKey )
    {
        case Input::KC_MOUSEX:
        {
            _sliderPos += _mMove.x * 8;

            if ( _mMove.x )
                v6 = 0;
        }
        break;

        case Input::KC_MOUSEY:
        {
            _sliderPos += _mMove.y * 8;

            if ( _mMove.y )
                v6 = 0;
        }
        break;

        case Input::KC_JOYX:
        {
            int v12 = _sliderPos;

            if ( v12 > _joyXYpos.x )
            {
                v12 -= 37;

                if ( v12 < _joyXYpos.x )
                    v12 = _joyXYpos.x;
            }
            else if ( v12 < _joyXYpos.x )
            {
                v12 += 37;

                if ( v12 > _joyXYpos.x )
                    v12 = _joyXYpos.x;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;

        case Input::KC_JOYY:
        {
            int v12 = _sliderPos;

            if ( v12 > _joyXYpos.y )
            {
                v12 -= 37;

                if ( v12 < _joyXYpos.y )
                    v12 = _joyXYpos.y;
            }
            else if ( v12 < _joyXYpos.y )
            {
                v12 += 37;

                if ( v12 > _joyXYpos.y )
                    v12 = _joyXYpos.y;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;

        case Input::KC_JOYTHROTTLE:
        {
            int v12 = _sliderPos;

            if ( v12 > -_joyZRZpos.x )
            {
                v12 -= 37;

                if ( v12 < -_joyZRZpos.x )
                    v12 = -_joyZRZpos.x;
            }
            else if ( v12 < -_joyZRZpos.x )
            {
                v12 += 37;

                if ( v12 > -_joyZRZpos.x )
                    v12 = -_joyZRZpos.x;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;

        case Input::KC_JOYHATX:
        {
            int v12 = _sliderPos;

            if ( v12 > _joyPov.x )
            {
                v12 -= 37;

                if ( v12 < _joyPov.x )
                    v12 = _joyPov.x;
            }
            else if ( v12 < _joyPov.x )
            {
                v12 += 37;

                if ( v12 > _joyPov.x )
                    v12 = _joyPov.x;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;

        case Input::KC_JOYHATY:
        {
            int v12 = _sliderPos;

            if ( v12 > _joyPov.y )
            {
                v12 -= 37;

                if ( v12 < _joyPov.y )
                    v12 = _joyPov.y;
            }
            else if ( v12 < _joyPov.y )
            {
                v12 += 37;

                if ( v12 > _joyPov.y )
                    v12 = _joyPov.y;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;
        
        case Input::KC_JOYRUDDER:
        {
            int v12 = _sliderPos;

            if ( v12 > _joyZRZpos.y )
            {
                v12 -= 37;

                if ( v12 < _joyZRZpos.y )
                    v12 = _joyZRZpos.y;
            }
            else if ( v12 < _joyZRZpos.y )
            {
                v12 += 37;

                if ( v12 > _joyZRZpos.y )
                    v12 = _joyZRZpos.y;
            }
            _sliderPos = v12;
            v6 = 0;
        }
        break;

        default:
            if (Input::Engine.KeyMatrix.at( _bindedKey ).down )
            {
                v6 = 0;
                _sliderPos += 37;
            }
            break;
    }

    if ( v6 )
        _sliderPos = 8 * _sliderPos / 10;

    if ( _sliderPos > 300 )
        _sliderPos = 300;
    else if ( _sliderPos < -300 )
        _sliderPos = -300;

    return _sliderPos / 300.0;
}

void NC_STACK_winp::QueryKeyboard(TInputState *arg)
{
    arg->HotKeyID = -1;
    arg->KbdLastDown = _kbdLastDown;
    arg->KbdLastHit = _kbdLastHit;
    
    _kbdLastHit = Input::KC_NONE;

    arg->chr = 0;

    if ( !_inputText.empty() )
    {
       arg->chr = _inputText.front();
       _inputText.pop();
    }
}

bool NC_STACK_winp::BindKey(const std::string &keyName)
{
    int16_t id = Input::Engine.GetKeyIDByName(keyName);
    if (id == -1)
        return false;
    
    _bindedKey = id;
    return Input::Engine.KeyMatrix.at(id).IsSlider;
}

void NC_STACK_winp::ResetSlider()
{
    _sliderPos = 0;
}





// Same scaling as sdlJoyAxis: 20% deadzone, -32768/32767 -> -300/300
int NC_STACK_winp::GameCtrlAxisVal(SDL_GameControllerAxis axis)
{
    int tmp = SDL_GameControllerGetAxis(_gameCtrl, axis);

    if (abs(tmp) <= 6553)
        return 0;

    tmp /= 109;

    if (tmp > 300)
        tmp = 300;
    else if (tmp < -300)
        tmp = -300;

    return tmp;
}

// Map a modern pad (DualSense etc.) onto the engine's virtual joystick:
//   left stick   -> JOYX/JOYY        (steer + pitch / drive)
//   right stick X-> JOYRUDDER        (extra steer authority, also merged into steering)
//   right stick  -> JOYHATX/JOYHATY  (POV hat: tank gun elevation), D-pad as fallback
// R1/level 1 -> JOYTHROTTLE (momentary throttle fwd/rev, like keyboard accel keys)
//   RT/LT        -> JOYB0/JOYB1      (fire weapon / fire machine gun)
//   Triangle     -> JOYB2            (missile cam)   Cross -> JOYB3 (brake)
//   Circle/Square/L3/R3 -> JOYB4..7  (free, bindable in the in-game input menu)
//   Options      -> Esc              (synthesized keyboard key: menus/quit)
//   Share/Create (DualSense), View/Back (Xbox), touchpad click -> Pause key
//                                    (single player: pause, press again to continue)
void NC_STACK_winp::CheckGameController(uint32_t *btnState)
{
    _joyXYpos.x = GameCtrlAxisVal(SDL_CONTROLLER_AXIS_LEFTX);
    _joyXYpos.y = GameCtrlAxisVal(SDL_CONTROLLER_AXIS_LEFTY);

    // GetSlider() negates the throttle axis, so "ahead" is -300 here.
    int thr = 0;
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) )
        thr -= 300;
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) )
        thr += 300;
    _joyZRZpos.x = thr;

    _joyZRZpos.y = GameCtrlAxisVal(SDL_CONTROLLER_AXIS_RIGHTX);

    // Hat up is +300 (see legacy SDL_HAT_UP), SDL stick up is negative -> invert Y
    int hx = GameCtrlAxisVal(SDL_CONTROLLER_AXIS_RIGHTX);
    int hy = -GameCtrlAxisVal(SDL_CONTROLLER_AXIS_RIGHTY);

    if ( hx == 0 && hy == 0 )
    {
        if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_DPAD_UP) )
            hy = 300;
        else if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN) )
            hy = -300;

        if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) )
            hx = 300;
        else if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT) )
            hx = -300;
    }

    _joyPov.x = hx;
    _joyPov.y = hy;

    if ( SDL_GameControllerGetAxis(_gameCtrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192 )
        *btnState |= 1 << 0; // fire weapon
    if ( SDL_GameControllerGetAxis(_gameCtrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192 )
        *btnState |= 1 << 1; // fire machine gun
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_Y) )
        *btnState |= 1 << 2; // missile cam
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_A) )
        *btnState |= 1 << 3; // brake
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_B) )
        *btnState |= 1 << 4;
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_X) )
        *btnState |= 1 << 5;
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_LEFTSTICK) )
        *btnState |= 1 << 6;
    if ( SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_RIGHTSTICK) )
        *btnState |= 1 << 7;

    bool start = SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_START) != 0;
    if ( start != _joyStartHeld )
    {
        if ( start )
            KeyDown(Input::KC_ESCAPE);
        else
            KeyUp(Input::KC_ESCAPE);

        _joyStartHeld = start;
    }

    // Pause: the Back button (DualSense Share/Create, Xbox View) on every SDL pad, and
    // the DualSense / DualShock 4 touchpad click as a second choice (macOS may keep
    // the Share button for its own screenshot shortcut).
    bool pause = SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_BACK) != 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    pause = pause || SDL_GameControllerGetButton(_gameCtrl, SDL_CONTROLLER_BUTTON_TOUCHPAD) != 0;
#endif
    if ( pause != _joyPauseHeld )
    {
        if ( pause )
            KeyDown(Input::KC_PAUSE);
        else
            KeyUp(Input::KC_PAUSE);

        _joyPauseHeld = pause;
    }
}

#ifdef UA_VR_ENABLED
// Touch controllers -> the engine's virtual joystick + hotkeys (vr-proof-plan.md, vehicle table):
//   left stick   -> JOYX/JOYY        drive: forward/back + turn
//   right stick  -> JOYRUDDER + JOYHATX/JOYHATY  turn the vehicle / gun (nose) up-down
//   RT / LT      -> JOYB0 / JOYB1    fire weapon / fire machine gun
// grips -> flyers: JOYTHROTTLE climb (right) / descend (left); tank / car (
//                   Settings "GRIPS"): DRIVE = right forward, left brake-then-reverse;
// BRAKE = right JOYB3 brake; OFF. Other ground viewers: right = brake.
//   right click  -> JOYB2            missile camera
//   A            -> hotkey 22        cycle through units (next unit in the squad)
//   B (hold 0.5 s) -> hotkey 21      jump to host station
//   X            -> hotkey 8         map on/off (command view placeholder)
//   Y            -> hotkey 9         squadron manager (squad wheel placeholder)
//   left click   -> hotkey 25        HUD on/off
//   menu         -> Esc              pause / options
// Laser pointer (VR::GetPointer: theatre screens, and in the cockpit while the map /
// squad manager / exit menu / confirm box is open or in the host station): the right
// aim ray moves the mouse on the 2D panel; R trigger = left button, R grip or B = right
// button. While it is on, those three do not fire / brake / climb / call the host station.
// in a vehicle (pointer only over windows) the R grip always drives / climbs /
// brakes and only B is the right button (a resting hand points at the tool bar).
// Hotkeys go through the key currently bound to them in the profile (GetHotKey), so a
// rebinding in the input menu keeps working.
static bool s_vrPtrL = false, s_vrPtrR = false, s_vrTrigUsed = false, s_vrGripUsed = false;
static int s_vrPtrMode = 0;
static Common::Point s_vrPtrPos(-1, -1);
// double-click: a left press within kVRDblMs of the previous one and within
// kVRDblPx of it is the second click of a double-click (SDL clicks = 2, which
// the game turns into FLAG_DBL_CLICK: "take control" on a unit). For that window
// the pointer is held at the first press point while the ray stays within
// kVRDblPx, so hand jitter does not break the game's 4-pixel test.
static const uint32_t kVRDblMs = 400;
static const int kVRDblPx = 10;
static uint32_t s_vrLDownT = 0;
static Common::Point s_vrLDownPos(-1000, -1000);
static int s_vrLClicks = 1;

void NC_STACK_winp::VRPointerButton(bool &state, bool down, int sdlBtn)
{
    state = down;
    const Common::Point pos = s_vrPtrPos;
    int clicks = 1;
    if ( sdlBtn == SDL_BUTTON_LEFT )
    {
        if ( down )
        {
            const uint32_t now = SDL_GetTicks();
            const bool dbl = s_vrLClicks == 1 && now - s_vrLDownT <= kVRDblMs
                             && std::abs(pos.x - s_vrLDownPos.x) <= kVRDblPx && std::abs(pos.y - s_vrLDownPos.y) <= kVRDblPx;
            s_vrLClicks = dbl ? 2 : 1;
            s_vrLDownT = now;
            s_vrLDownPos = pos;
        }
        clicks = s_vrLClicks;
    }
    bool gui;
    if ( down )
    {
        gui = Gui::Root::Instance.MouseDown(pos, sdlBtn, clicks);
        if ( !gui )
            OnMouseDown(pos, sdlBtn, clicks);
    }
    else
    {
        gui = Gui::Root::Instance.MouseUp(pos, sdlBtn, clicks);
        if ( !gui )
            OnMouseUp(pos, sdlBtn, clicks);
    }
    static int ptrLog = -1;
    if ( ptrLog < 0 )
        ptrLog = getenv("UA_VR_AIM_LOG") ? 1 : 0;
    if ( ptrLog )
        printf("UAVR pointer: button %d %s at %d,%d%s%s\n", sdlBtn, down ? "down" : "up", pos.x, pos.y, gui ? " (gui)" : "", clicks == 2 ? " (double-click)" : "");
}

void NC_STACK_winp::ReleaseVRPointer()
{
    if ( s_vrPtrL ) VRPointerButton(s_vrPtrL, false, SDL_BUTTON_LEFT);
    if ( s_vrPtrR ) VRPointerButton(s_vrPtrR, false, SDL_BUTTON_RIGHT);
}

void NC_STACK_winp::CheckVRControllers(uint32_t *btnState)
{
    VR::ControllerState c[2];
    if ( !VR::GetControllers(c) )
    {
        ReleaseVRPointer(); // controllers inactive / session lost while a pointer button was held
        return;
    }
    const VR::ControllerState &L = c[0], &R = c[1];

    auto axis = [](float v) -> int
    {
        if ( std::fabs(v) < 0.15f )   // stick dead zone
            return 0;
        int t = (int)(v * 300.0f);
        return t > 300 ? 300 : (t < -300 ? -300 : t);
    };

    _joyXYpos.x = axis(L.stickX);
    _joyXYpos.y = -axis(L.stickY);    // engine: stick forward = negative (SDL convention)
    _joyZRZpos.y = axis(R.stickX);
    _joyPov.x = axis(R.stickX);
    _joyPov.y = axis(R.stickY);       // hat up = +300

    // Laser pointer -> mouse. A pointer button is latched to the physical button: one
    // down when it is pressed while the pointer is on, one up on physical release (or when
    // the controllers / panel mode go away). While held, the position keeps following the
    // ray anywhere on the panel (drags do not break at window edges).
    int ppx = 0, ppy = 0;
    const bool ptr = VR::GetPointer(&ppx, &ppy);
    // in a vehicle (UI pointer mode 2) the right grip always drives / climbs /
    // brakes; a right click while pointing comes from B (vr_grip.h VehicleOwnsGrip).
    const bool flyer = VR::ViewerIsFlyer();
    const bool drives = !flyer && VR::ViewerDrives();
    const int gripMode = VR::GroundGripMode();
    const int uiPtrMode = VR::UiPointerMode();
    const bool vehGrip = VR::Grip::VehicleOwnsGrip(uiPtrMode, flyer || (drives && gripMode != VR::GRIP_OFF));
    const bool bDown = (R.buttons & VR::BTN_B) != 0;
    static bool s_vrGripDrive = false; // squeeze owned by the vehicle until released
    s_vrGripDrive = VR::Grip::UpdateGripDrive(s_vrGripDrive, vehGrip, R.grip);
    {
        int rx = 0, ry = 0, mode = 0;
        const bool raw = VR::GetPointerRaw(&rx, &ry, &mode);
        const bool held = s_vrPtrL || s_vrPtrR;
        if ( held && mode != s_vrPtrMode )
            ReleaseVRPointer(); // theatre <-> cockpit switch while holding
        if ( ptr || (held && raw) )
        {
            Common::Point pos = GFX::GFXEngine::Instance.ConvertPosTo2DStuff( ptr ? Common::Point(ppx, ppy) : Common::Point(rx, ry) );
            // hold still around a fresh left press (double-click window), see kVRDblMs
            if ( SDL_GetTicks() - s_vrLDownT <= kVRDblMs + 150
                 && std::abs(pos.x - s_vrLDownPos.x) <= kVRDblPx && std::abs(pos.y - s_vrLDownPos.y) <= kVRDblPx )
                pos = s_vrLDownPos;
            if ( pos != s_vrPtrPos )
            {
                Common::Point rel = (s_vrPtrPos.x < 0) ? Common::Point() : pos - s_vrPtrPos;
                if ( !Gui::Root::Instance.MouseMove(pos, rel) )
                    OnMouseMove(pos, rel);
                s_vrPtrPos = pos;
            }
        }
        const bool physL = s_vrPtrL ? R.trigger > 0.35f : R.trigger > 0.6f;
        const bool physR = VR::Grip::PhysRight(vehGrip, s_vrPtrR, R.grip, bDown, s_vrGripDrive);
        // press only while the pointer is on; release only on physical release
        const bool wantL = s_vrPtrL ? physL : (ptr && physL && !s_vrTrigUsed);
        const bool wantR = s_vrPtrR ? physR : (ptr && physR && !s_vrGripUsed);
        if ( wantL != s_vrPtrL ) VRPointerButton(s_vrPtrL, wantL, SDL_BUTTON_LEFT);
        if ( wantR != s_vrPtrR ) VRPointerButton(s_vrPtrR, wantR, SDL_BUTTON_RIGHT);
        if ( s_vrPtrL || s_vrPtrR )
            s_vrPtrMode = mode;
        // a squeeze that started as a weapon / brake press never turns into a click
        s_vrTrigUsed = R.trigger > 0.2f && !s_vrPtrL && (s_vrTrigUsed || !ptr);
        s_vrGripUsed = VR::Grip::RightUsed(s_vrGripUsed, vehGrip, ptr, s_vrPtrR, R.grip, bDown, s_vrGripDrive);
    }
    // A button pressed while pointing stays a mouse button until it is released
    // (closing the map with a click must not fire the gun).
    static bool trigLatch = false;
    static VR::Grip::Latch gripLatch;
    if ( (ptr || s_vrPtrL) && R.trigger > 0.25f ) trigLatch = true;
    if ( R.trigger < 0.2f ) trigLatch = false;
    VR::Grip::UpdateLatch(gripLatch, vehGrip, ptr || s_vrPtrR, R.grip, bDown, s_vrGripDrive);
    const bool rTrigFree = !ptr && !trigLatch && !s_vrPtrL;
    const bool rGripFree = VR::Grip::RGripFree(gripLatch, vehGrip, ptr, s_vrPtrR);
    const bool bFree = VR::Grip::BFree(gripLatch, vehGrip, ptr, s_vrPtrR);

    int thr = 0;
    if ( flyer )
    {
        if ( R.grip > 0.5f && rGripFree ) thr -= 300; // GetSlider() negates: "ahead"/climb is -300 here
        if ( L.grip > 0.5f ) thr += 300;
    }
    _joyZRZpos.x = thr;

    // ground grips (tank / car). DRIVE: the grips act as the left stick's forward /
    // back (analog: a light squeeze is a slow drive) while the stick's Y is idle, so the
    // stick keeps working and wins when used. L grip = car-game brake: while the vehicle
    // still rolls forward it brakes (JOYB3, which beats any thrust in User_layer); once
    // stopped, holding it reverses. BRAKE: the mapping, R grip = brake.
    bool gripBrake = false;
    if ( drives && gripMode == VR::GRIP_DRIVE )
    {
        auto gripAxis = [](float g) -> int
        {
            if ( g < 0.35f )
                return 0;
            const float v = 0.35f + (g - 0.35f) / 0.5f * 0.65f; // past the game's 0.3 brake band
            return (int)(300.0f * (v > 1.0f ? 1.0f : v));
        };
        const int fwd = rGripFree ? gripAxis(R.grip) : 0;
        const int back = gripAxis(L.grip);
        const bool stickIdle = std::abs(_joyXYpos.y) < 90; // the game brakes below 0.3
        // brake while rolling forward, hold the stop 300 ms, then reverse
        static VR::Grip::LState lState;
        const VR::Grip::LAction la = VR::Grip::LeftGrip(lState, back != 0, VR::ViewerForwardSpeed(), SDL_GetTicks());
        if ( la == VR::Grip::L_BRAKE )
            gripBrake = stickIdle;         // still rolling forward / stop pause: brake (an active stick wins)
        else if ( la == VR::Grip::L_REVERSE && stickIdle )
            _joyXYpos.y = back;            // stopped / reversing: reverse
        else if ( !back && fwd && stickIdle )
            _joyXYpos.y = -fwd;            // engine: forward = negative
        static int gripLog = -1;
        if ( gripLog < 0 )
            gripLog = getenv("UA_VR_GRIP_LOG") ? 1 : 0;
        static int lastState = 0;
        static bool lastB = false;
        const int state = gripBrake ? (lState.stopped ? 4 : 3) : (la == VR::Grip::L_REVERSE && stickIdle ? 2 : (!back && fwd && stickIdle ? 1 : (!stickIdle ? 5 : 0)));
        static int gripPolls = 0;
        gripPolls++;
        if ( gripLog && (state != lastState || ((state || R.grip > 0.35f || L.grip > 0.35f) && gripPolls % 30 == 0) || bDown != lastB) )
            fprintf(stderr, "UAVR grips: %s (fwd speed %.2f, joy y %d) R %.2f L %.2f ptr %d ui %d veh %d latch %d/%d ptrR %d B %d t %u%s\n",
                    state == 5 ? "L stick DRIVE" : state == 4 ? "L grip BRAKE (stop pause)" : state == 3 ? "L grip BRAKE" : state == 2 ? "L grip REVERSE" : state == 1 ? "R grip FORWARD" : "released",
                    VR::ViewerForwardSpeed(), _joyXYpos.y, R.grip, L.grip, ptr ? 1 : 0, uiPtrMode, vehGrip ? 1 : 0,
                    gripLatch.grip ? 1 : 0, gripLatch.b ? 1 : 0, s_vrPtrR ? 1 : 0, bDown ? 1 : 0, SDL_GetTicks(), (R.grip > 0.35f && !fwd) ? " R GRIP BLOCKED" : "");
        lastState = state;
        lastB = bDown;
    }
    else if ( !flyer && !(drives && gripMode == VR::GRIP_OFF) )
        gripBrake = R.grip > 0.5f && rGripFree; // BRAKE mode, and stations / guns as before

    if ( R.trigger > 0.25f && rTrigFree )        *btnState |= 1 << 0;
    if ( L.trigger > 0.25f )                     *btnState |= 1 << 1;
    if ( R.buttons & VR::BTN_STICK_CLICK )       *btnState |= 1 << 2;
    if ( gripBrake )                             *btnState |= 1 << 3;

    struct Edge { bool held = false; };
    static Edge eA, eX, eY, eLc, eMenu;
    static uint32_t bDownAt = 0;
    static bool bFired = false;

    auto hotkey = [](Edge &e, bool down, int hotkeyId)
    {
        int16_t kc = (hotkeyId >= 0) ? Input::Engine.GetHotKey(hotkeyId) : Input::KC_ESCAPE;
        if ( down && !e.held && kc != Input::KC_NONE )
            KeyDown(kc);
        else if ( !down && e.held && kc != Input::KC_NONE )
            KeyUp(kc);
        e.held = down;
    };
    hotkey(eA, (R.buttons & VR::BTN_A) != 0, 22);
    hotkey(eX, (L.buttons & VR::BTN_X) != 0, 8);
    hotkey(eY, (L.buttons & VR::BTN_Y) != 0, 9);
    hotkey(eLc, (L.buttons & VR::BTN_STICK_CLICK) != 0, 25);
    hotkey(eMenu, (L.buttons & VR::BTN_MENU) != 0, -1);

    const bool b = bDown && bFree; // B is a right click while pointing
    const int16_t kcHost = Input::Engine.GetHotKey(21);
    if ( b && !bDownAt )
    {
        bDownAt = SDL_GetTicks();
        bFired = false;
    }
    if ( b && !bFired && SDL_GetTicks() - bDownAt >= 500 && kcHost != Input::KC_NONE )
    {
        KeyDown(kcHost);
        bFired = true;
    }
    if ( !b )
    {
        if ( bFired && kcHost != Input::KC_NONE )
            KeyUp(kcHost);
        bDownAt = 0;
        bFired = false;
    }
}
#endif

void NC_STACK_winp::CheckJoy()
{
    bool vrPad = false;
#ifdef UA_VR_ENABLED
    VR::ControllerState vc[2];
    vrPad = VR::GetControllers(vc);
    if ( !vrPad && VR::Active() )
        ReleaseVRPointer(); // no controllers: never leave a laser click held
#endif
    if ( _joyEnable || vrPad )
    {
        uint32_t prevBtnState = _joyButtonStates;
        uint32_t newBtnState = 0;
        _joyXYpos = Common::Point();
        _joyZRZpos = Common::Point();
        // R1/L1 are momentary throttle buttons only on a game controller (a joystick's
        // throttle axis stays where it was left), see PadThrottleButtons().
        _padThrottle = !vrPad && _gameCtrl != NULL;

        if ( vrPad )
        {
#ifdef UA_VR_ENABLED
            CheckVRControllers(&newBtnState);
#endif
        }
        else if ( _gameCtrl )
        {
            CheckGameController(&newBtnState);
        }
        else
        {

        _joyXYpos.x = sdlJoyAxis(_joyHandle, 0);
        _joyXYpos.y = sdlJoyAxis(_joyHandle, 1);

        int joyaxs = SDL_JoystickNumAxes(_joyHandle);

        if ( joyaxs > 2 )
            _joyZRZpos.x = sdlJoyAxis(_joyHandle, 2);
        else
            _joyZRZpos.x = -300;

        if ( joyaxs > 3 )
            _joyZRZpos.y = sdlJoyAxis(_joyHandle, 3);
        else
            _joyZRZpos.y = 0;

        for (int i = 0; i < 8; i++ )
        {
            if ( SDL_JoystickGetButton(_joyHandle, _joyButtonMap[i]) )
                newBtnState |= 1 << i;
        }

        if ( SDL_JoystickNumHats(_joyHandle) > 0)
        {
            switch( SDL_JoystickGetHat(_joyHandle, _joyHatMap[0]) )
            {
            case SDL_HAT_UP:
                _joyPov.x = 0;
                _joyPov.y = 300;
                break;

            case SDL_HAT_RIGHTUP:
                _joyPov.x = 212;
                _joyPov.y = 212;
                break;

            case SDL_HAT_RIGHT:
                _joyPov.x = 300;
                _joyPov.y = 0;
                break;

            case SDL_HAT_RIGHTDOWN:
                _joyPov.x = 212;
                _joyPov.y = -212;
                break;

            case SDL_HAT_DOWN:
                _joyPov.x = 0;
                _joyPov.y = -300;
                break;

            case SDL_HAT_LEFTDOWN:
                _joyPov.x = -212;
                _joyPov.y = -212;
                break;

            case SDL_HAT_LEFT:
                _joyPov.x = -300;
                _joyPov.y = 0;
                break;

            case SDL_HAT_LEFTUP:
                _joyPov.x = -212;
                _joyPov.y = 212;
                break;

            case SDL_HAT_CENTERED:
            default:
                _joyPov.x = 0;
                _joyPov.y = 0;
                break;
            }
        }
        else
        {
            _joyPov.x = 0;
            _joyPov.y = 0;
        }

        } // legacy joystick path

        _joyButtonStates = newBtnState;

        for (int i = 0; i < 8; i++ )
        {
            if ( newBtnState & (1 << i) )
            {
                if ( !((1 << i) & prevBtnState) )
                    KeyDown(i + Input::KC_JOYB0);
            }
            else if ( (1 << i) & prevBtnState )
            {
                KeyUp(i + Input::KC_JOYB0);
            }
        }
    }
}

void NC_STACK_winp::QueryPointer(TClickBoxInf *arg)
{
    arg->flag = 0;

    CheckJoy();

    arg->move.ScreenPos = _mPos;
    _mMove = _mMoveQuery;
    
    _mMoveQuery = Common::Point();

    if ( _mLstate )
        arg->flag |= TClickBoxInf::FLAG_LM_HOLD;
    if ( _mMstate )
        arg->flag |= TClickBoxInf::FLAG_MM_HOLD;
    if ( _mRstate )
        arg->flag |= TClickBoxInf::FLAG_RM_HOLD;
    if ( _mDBLstate )
        arg->flag |= TClickBoxInf::FLAG_DBL_CLICK;

    if ( _mLDcnt > 0 )
    {
        arg->ldw_pos.ScreenPos.x = _mLDpos.x;
        arg->ldw_pos.ScreenPos.y = _mLDpos.y;
        arg->flag |= TClickBoxInf::FLAG_LM_DOWN;
    }
    if ( _mLUcnt > 0 )
    {
        arg->lup_pos.ScreenPos.x = _mLUpos.x;
        arg->lup_pos.ScreenPos.y = _mLUpos.y;
        arg->flag |= TClickBoxInf::FLAG_LM_UP;
    }
    if ( _mRDcnt > 0 )
    {
        arg->flag |= TClickBoxInf::FLAG_RM_DOWN;
    }
    if ( _mRUcnt > 0 )
    {
        arg->flag |= TClickBoxInf::FLAG_RM_UP;
    }
    if ( _mMDcnt > 0 )
    {
        arg->flag |= TClickBoxInf::FLAG_MM_DOWN;
    }
    if ( _mMUcnt > 0 )
    {
        arg->flag |= TClickBoxInf::FLAG_MM_UP;
    }

    _mDBLstate = false;
    _mLDcnt = 0;
    _mLUcnt = 0;
    _mRDcnt = 0;
    _mRUcnt = 0;
    _mMDcnt = 0;
    _mMUcnt = 0;
}

void NC_STACK_winp::InitFirst()
{
    KBDMapping.clear();
    
    KBDMapping[SDL_SCANCODE_ESCAPE]      = Input::KC_ESCAPE;
    KBDMapping[SDL_SCANCODE_SPACE]       = Input::KC_SPACE;
    KBDMapping[SDL_SCANCODE_UP]          = Input::KC_UP;
    KBDMapping[SDL_SCANCODE_DOWN]        = Input::KC_DOWN;
    KBDMapping[SDL_SCANCODE_LEFT]        = Input::KC_LEFT;
    KBDMapping[SDL_SCANCODE_RIGHT]       = Input::KC_RIGHT;
    KBDMapping[SDL_SCANCODE_F1]          = Input::KC_F1;
    KBDMapping[SDL_SCANCODE_F2]          = Input::KC_F2;
    KBDMapping[SDL_SCANCODE_F3]          = Input::KC_F3;
    KBDMapping[SDL_SCANCODE_F4]          = Input::KC_F4;
    KBDMapping[SDL_SCANCODE_F5]          = Input::KC_F5;
    KBDMapping[SDL_SCANCODE_F6]          = Input::KC_F6;
    KBDMapping[SDL_SCANCODE_F7]          = Input::KC_F7;
    KBDMapping[SDL_SCANCODE_F8]          = Input::KC_F8;
    KBDMapping[SDL_SCANCODE_F9]          = Input::KC_F9;
    KBDMapping[SDL_SCANCODE_F10]         = Input::KC_F10;
    KBDMapping[SDL_SCANCODE_F11]         = Input::KC_F11;
    KBDMapping[SDL_SCANCODE_F12]         = Input::KC_F12;
    KBDMapping[SDL_SCANCODE_BACKSPACE]   = Input::KC_BACKSPACE;
    KBDMapping[SDL_SCANCODE_TAB]         = Input::KC_TAB;
    KBDMapping[SDL_SCANCODE_CLEAR]       = Input::KC_CLEAR;
    KBDMapping[SDL_SCANCODE_RETURN]      = Input::KC_RETURN;
    KBDMapping[SDL_SCANCODE_LCTRL]       = Input::KC_CTRL;
    KBDMapping[SDL_SCANCODE_RCTRL]       = Input::KC_CTRL;
    KBDMapping[SDL_SCANCODE_RSHIFT]      = Input::KC_SHIFT;
    KBDMapping[SDL_SCANCODE_LSHIFT]      = Input::KC_SHIFT;
    KBDMapping[SDL_SCANCODE_LALT]        = Input::KC_ALT;
    KBDMapping[SDL_SCANCODE_RALT]        = Input::KC_ALT;
    KBDMapping[SDL_SCANCODE_PAUSE]       = Input::KC_PAUSE;
    KBDMapping[SDL_SCANCODE_PAGEUP]      = Input::KC_PGUP;
    KBDMapping[SDL_SCANCODE_PAGEDOWN]    = Input::KC_PGDOWN;
    KBDMapping[SDL_SCANCODE_END]         = Input::KC_END;
    KBDMapping[SDL_SCANCODE_HOME]        = Input::KC_HOME;
    KBDMapping[SDL_SCANCODE_SELECT]      = Input::KC_SELECT;
    KBDMapping[SDL_SCANCODE_EXECUTE]     = Input::KC_EXECUTE;
    KBDMapping[SDL_SCANCODE_PRINTSCREEN] = Input::KC_SNAPSHOT;
    KBDMapping[SDL_SCANCODE_INSERT]      = Input::KC_INSERT;
    KBDMapping[SDL_SCANCODE_DELETE]      = Input::KC_DELETE;
    KBDMapping[SDL_SCANCODE_HELP]        = Input::KC_HELP;
    KBDMapping[SDL_SCANCODE_1]           = Input::KC_1;
    KBDMapping[SDL_SCANCODE_2]           = Input::KC_2;
    KBDMapping[SDL_SCANCODE_3]           = Input::KC_3;
    KBDMapping[SDL_SCANCODE_4]           = Input::KC_4;
    KBDMapping[SDL_SCANCODE_5]           = Input::KC_5;
    KBDMapping[SDL_SCANCODE_6]           = Input::KC_6;
    KBDMapping[SDL_SCANCODE_7]           = Input::KC_7;
    KBDMapping[SDL_SCANCODE_8]           = Input::KC_8;
    KBDMapping[SDL_SCANCODE_9]           = Input::KC_9;
    KBDMapping[SDL_SCANCODE_0]           = Input::KC_0;
    KBDMapping[SDL_SCANCODE_A]           = Input::KC_A;
    KBDMapping[SDL_SCANCODE_B]           = Input::KC_B;
    KBDMapping[SDL_SCANCODE_C]           = Input::KC_C;
    KBDMapping[SDL_SCANCODE_D]           = Input::KC_D;
    KBDMapping[SDL_SCANCODE_E]           = Input::KC_E;
    KBDMapping[SDL_SCANCODE_F]           = Input::KC_F;
    KBDMapping[SDL_SCANCODE_G]           = Input::KC_G;
    KBDMapping[SDL_SCANCODE_H]           = Input::KC_H;
    KBDMapping[SDL_SCANCODE_I]           = Input::KC_I;
    KBDMapping[SDL_SCANCODE_J]           = Input::KC_J;
    KBDMapping[SDL_SCANCODE_K]           = Input::KC_K;
    KBDMapping[SDL_SCANCODE_L]           = Input::KC_L;
    KBDMapping[SDL_SCANCODE_M]           = Input::KC_M;
    KBDMapping[SDL_SCANCODE_N]           = Input::KC_N;
    KBDMapping[SDL_SCANCODE_O]           = Input::KC_O;
    KBDMapping[SDL_SCANCODE_P]           = Input::KC_P;
    KBDMapping[SDL_SCANCODE_Q]           = Input::KC_Q;
    KBDMapping[SDL_SCANCODE_R]           = Input::KC_R;
    KBDMapping[SDL_SCANCODE_S]           = Input::KC_S;
    KBDMapping[SDL_SCANCODE_T]           = Input::KC_T;
    KBDMapping[SDL_SCANCODE_U]           = Input::KC_U;
    KBDMapping[SDL_SCANCODE_V]           = Input::KC_V;
    KBDMapping[SDL_SCANCODE_W]           = Input::KC_W;
    KBDMapping[SDL_SCANCODE_X]           = Input::KC_X;
    KBDMapping[SDL_SCANCODE_Y]           = Input::KC_Y;
    KBDMapping[SDL_SCANCODE_Z]           = Input::KC_Z;
    KBDMapping[SDL_SCANCODE_KP_0]        = Input::KC_NUM0;
    KBDMapping[SDL_SCANCODE_KP_1]        = Input::KC_NUM1;
    KBDMapping[SDL_SCANCODE_KP_2]        = Input::KC_NUM2;
    KBDMapping[SDL_SCANCODE_KP_3]        = Input::KC_NUM3;
    KBDMapping[SDL_SCANCODE_KP_4]        = Input::KC_NUM4;
    KBDMapping[SDL_SCANCODE_KP_5]        = Input::KC_NUM5;
    KBDMapping[SDL_SCANCODE_KP_6]        = Input::KC_NUM6;
    KBDMapping[SDL_SCANCODE_KP_7]        = Input::KC_NUM7;
    KBDMapping[SDL_SCANCODE_KP_8]        = Input::KC_NUM8;
    KBDMapping[SDL_SCANCODE_KP_9]        = Input::KC_NUM9;
    KBDMapping[SDL_SCANCODE_KP_MULTIPLY] = Input::KC_NUMMUL;
    KBDMapping[SDL_SCANCODE_KP_PLUS]     = Input::KC_NUMPLUS;
    KBDMapping[SDL_SCANCODE_KP_PERIOD]   = Input::KC_NUMDOT;
    KBDMapping[SDL_SCANCODE_KP_MINUS]    = Input::KC_NUMMINUS;
    KBDMapping[SDL_SCANCODE_KP_ENTER]    = Input::KC_NUMENTER;
    KBDMapping[SDL_SCANCODE_KP_DIVIDE]   = Input::KC_NUMDIV;
    KBDMapping[SDL_SCANCODE_COMMA]       = Input::KC_EXTRA1;
    KBDMapping[SDL_SCANCODE_PERIOD]      = Input::KC_EXTRA2;
    KBDMapping[SDL_SCANCODE_MINUS]       = Input::KC_EXTRA3;
    KBDMapping[SDL_SCANCODE_BACKSLASH]   = Input::KC_EXTRA4;
    KBDMapping[SDL_SCANCODE_SEMICOLON]   = Input::KC_EXTRA5;
    KBDMapping[SDL_SCANCODE_EQUALS]      = Input::KC_EXTRA6;
    KBDMapping[SDL_SCANCODE_GRAVE]       = Input::KC_EXTRA7;
    KBDMapping[SDL_SCANCODE_APOSTROPHE]  = Input::KC_EXTRA8;
    KBDMapping[SDL_SCANCODE_SLASH]       = Input::KC_EXTRA9;
    KBDMapping[SDL_SCANCODE_RIGHTBRACKET]= Input::KC_EXTRA10;
    KBDMapping[SDL_SCANCODE_BACKSLASH]   = Input::KC_EXTRA11;
    KBDMapping[SDL_SCANCODE_LEFTBRACKET] = Input::KC_EXTRA12;
    //KBDMapping[???]       = Input::KC_EXTRA13; // 	OEM_8 (§ !)
    KBDMapping[SDL_SCANCODE_SCROLLLOCK]  = Input::KC_EXTRA14;
    KBDMapping[SDL_SCANCODE_NUMLOCKCLEAR]= Input::KC_EXTRA15;
    KBDMapping[SDL_SCANCODE_F13]         = Input::KC_EXTRA16;
    KBDMapping[SDL_SCANCODE_F14]         = Input::KC_EXTRA17;
    KBDMapping[SDL_SCANCODE_F15]         = Input::KC_EXTRA18;

    //Joy staff

    _joyEnable = false;

    const char *joyDbg = getenv("UA_JOY_DEBUG");
    _joyDebug = joyDbg && *joyDbg && strcmp(joyDbg, "0") != 0;
    sdlInputLogMirror = _joyDebug;

    if ( _joyDebug )
    {
        // Keep pad events flowing while the window is unfocused (diagnosis aid)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    sdlInputResetLog();
    _joyWantGuid = sdlReadJoyGuid();

    SDL_version sdlVer;
    SDL_GetVersion(&sdlVer);
    sdlInputLog("SDL %d.%d.%d, %d joystick(s) at startup%s\n",
                sdlVer.major, sdlVer.minor, sdlVer.patch, SDL_NumJoysticks(),
                _joyDebug ? " (UA_JOY_DEBUG on)" : "");

    JoyPickAndOpen();

    _mLstate = false;
    _mRstate = false;
    _mMstate = false;
    _mDBLstate = false;

    _mLUcnt = 0;
    _mLDcnt = 0;
    _mRUcnt = 0;
    _mRDcnt = 0;
    _mMUcnt = 0;
    _mMDcnt = 0;
    
    _kbdLastDown = 0;
    _kbdLastHit = 0;

    _joyButtonStates = 0;

    System::EventsAddHandler(InputWatch);
}

