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
bool NC_STACK_winp::Rumble(uint16_t lowFreq, uint16_t highFreq, uint32_t ms)
{
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

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
        if ( _joyDebug )
            sdlInputLog("Pad button %s: %s\n",
                        SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event->cbutton.button),
                        event->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
    }
    break;

    case SDL_CONTROLLERAXISMOTION:
    {
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
//   R1/L1        -> JOYTHROTTLE      (momentary throttle fwd/rev, like keyboard accel keys)
//   RT/LT        -> JOYB0/JOYB1      (fire weapon / fire machine gun)
//   Triangle     -> JOYB2            (missile cam)   Cross -> JOYB3 (brake)
//   Circle/Square/L3/R3 -> JOYB4..7  (free, bindable in the in-game input menu)
//   Options      -> Esc              (synthesized keyboard key: menus/quit)
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
}

void NC_STACK_winp::CheckJoy()
{
    if ( _joyEnable )
    {
        uint32_t prevBtnState = _joyButtonStates;
        uint32_t newBtnState = 0;
        _joyXYpos = Common::Point();
        _joyZRZpos = Common::Point();

        if ( _gameCtrl )
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

