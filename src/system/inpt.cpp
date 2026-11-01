#include <cstdlib>

#include "common/common.h"
#include "../ini.h"
#include "../log.h"
#include "inpt.h"
#include "inivals.h"
#include "winp.h"

namespace Input{

INPEngine INPEngine::Instance;

int INPEngine::Init()
{
    NC_STACK_winp::InitFirst();
    
    RegisterInterface( NC_STACK_winp::GetQueryInterface() );

    
    System::IniConf::ReadFromNucleusIni();

    for (InputNodeList &lst : _buttons)
        lst.clear();
    
    for (InputNodeList &lst : _sliders)
        lst.clear();
    
    for (int16_t &k : _hotKeys)
        k = KC_NONE;
    
    SDL_Haptic *joyHaptic = NC_STACK_winp::GetJoyHaptic();
    if (joyHaptic)
    {
        _ffTankEngine.Bind(joyHaptic);
        _ffJetEngine.Bind(joyHaptic);
        _ffCopterEngine.Bind(joyHaptic);
        _ffRotDamper.Bind(joyHaptic);
        _ffMGun.Bind(joyHaptic);
        _ffMissFire.Bind(joyHaptic);
        _ffGrenadeFire.Bind(joyHaptic);
        _ffBombFire.Bind(joyHaptic);
        _ffCollide.Bind(joyHaptic);
        _ffShake.Bind(joyHaptic);
    }

    std::string val = System::IniConf::InputTimer.Get<std::string>(); // input.timer

    if ( !val.empty() )
    {
        if ( ! InitDriver(ITYPE_TIMER, val) )
            ypa_log_out("input.engine: WARNING: Timer object creation failed.\n");
    }
    else
    {
        ypa_log_out("input.engine: WARNING: no Timer driver defined in prefs file.\n");
    }

    val = System::IniConf::InputWimp.Get<std::string>(); // input.wimp

    if ( !val.empty() )
    {
        if ( ! InitDriver(ITYPE_WIMP, val) )
            ypa_log_out("input.engine: WARNING: Wimp object creation failed.\n");
    }
    else
    {
        ypa_log_out("input.engine: WARNING: no Wimp driver defined in prefs file.\n");
    }

    val = System::IniConf::InputKeyboard.Get<std::string>(); // input.keyboard

    if ( !val.empty() )
    {
        if ( ! InitDriver(ITYPE_KBD, val) )
            ypa_log_out("input.engine: WARNING: Keyboard object creation failed.\n");
    }
    else
    {
        ypa_log_out("input.engine: WARNING: no Keyboard driver defined in prefs file.\n");
    }
    
    Common::Ini::Key *Buttons[] = {
        &System::IniConf::InputButton0
    ,   &System::IniConf::InputButton1
    ,   &System::IniConf::InputButton2
    ,   &System::IniConf::InputButton3
    ,   &System::IniConf::InputButton4
    ,   &System::IniConf::InputButton5
    ,   &System::IniConf::InputButton6
    ,   &System::IniConf::InputButton7
    ,   &System::IniConf::InputButton8
    ,   &System::IniConf::InputButton9
    ,   &System::IniConf::InputButton10
    ,   &System::IniConf::InputButton11
    ,   &System::IniConf::InputButton12
    ,   &System::IniConf::InputButton13
    ,   &System::IniConf::InputButton14
    ,   &System::IniConf::InputButton15
    ,   &System::IniConf::InputButton16
    ,   &System::IniConf::InputButton17
    ,   &System::IniConf::InputButton18
    ,   &System::IniConf::InputButton19
    ,   &System::IniConf::InputButton20
    ,   &System::IniConf::InputButton21
    ,   &System::IniConf::InputButton22
    ,   &System::IniConf::InputButton23
    ,   &System::IniConf::InputButton24
    ,   &System::IniConf::InputButton25
    ,   &System::IniConf::InputButton26
    ,   &System::IniConf::InputButton27
    ,   &System::IniConf::InputButton28
    ,   &System::IniConf::InputButton29
    ,   &System::IniConf::InputButton30
    ,   &System::IniConf::InputButton31 };

    int i = 0;
    for (Common::Ini::Key *k : Buttons)
    {
        val = k->Get<std::string>(); // input_buttons

        if ( !val.empty() )
        {
            if ( ! SetInputExpression(false, i, val) )
                ypa_log_out("input.engine: WARNING: Button[%d] object creation failed.\n", i);
        }
        i++;
    }
    
    Common::Ini::Key *Sliders[] = {
        &System::IniConf::InputSlider0
    ,   &System::IniConf::InputSlider1
    ,   &System::IniConf::InputSlider2
    ,   &System::IniConf::InputSlider3
    ,   &System::IniConf::InputSlider4
    ,   &System::IniConf::InputSlider5
    ,   &System::IniConf::InputSlider6
    ,   &System::IniConf::InputSlider7
    ,   &System::IniConf::InputSlider8
    ,   &System::IniConf::InputSlider9
    ,   &System::IniConf::InputSlider10
    ,   &System::IniConf::InputSlider11
    ,   &System::IniConf::InputSlider12
    ,   &System::IniConf::InputSlider13
    ,   &System::IniConf::InputSlider14
    ,   &System::IniConf::InputSlider15
    ,   &System::IniConf::InputSlider16
    ,   &System::IniConf::InputSlider17
    ,   &System::IniConf::InputSlider18
    ,   &System::IniConf::InputSlider19
    ,   &System::IniConf::InputSlider20
    ,   &System::IniConf::InputSlider21
    ,   &System::IniConf::InputSlider22
    ,   &System::IniConf::InputSlider23
    ,   &System::IniConf::InputSlider24
    ,   &System::IniConf::InputSlider25
    ,   &System::IniConf::InputSlider26
    ,   &System::IniConf::InputSlider27
    ,   &System::IniConf::InputSlider28
    ,   &System::IniConf::InputSlider29
    ,   &System::IniConf::InputSlider30
    ,   &System::IniConf::InputSlider31
    };

    i = 0;
    for (Common::Ini::Key *k : Sliders)
    {
        val = k->Get<std::string>(); // input_sliders

        if ( !val.empty() )
        {
            if ( ! SetInputExpression(true, i, val) )
                ypa_log_out("input.engine: WARNING: Slider[%d] object creation failed.\n", i);
        }
        i++;
    }
    
    Common::Ini::Key *Hotkeys[] = {   
        &System::IniConf::InputHotkey0
    ,   &System::IniConf::InputHotkey1
    ,   &System::IniConf::InputHotkey2
    ,   &System::IniConf::InputHotkey3
    ,   &System::IniConf::InputHotkey4
    ,   &System::IniConf::InputHotkey5
    ,   &System::IniConf::InputHotkey6
    ,   &System::IniConf::InputHotkey7
    ,   &System::IniConf::InputHotkey8
    ,   &System::IniConf::InputHotkey9
    ,   &System::IniConf::InputHotkey10
    ,   &System::IniConf::InputHotkey11
    ,   &System::IniConf::InputHotkey12
    ,   &System::IniConf::InputHotkey13
    ,   &System::IniConf::InputHotkey14
    ,   &System::IniConf::InputHotkey15
    ,   &System::IniConf::InputHotkey16
    ,   &System::IniConf::InputHotkey17
    ,   &System::IniConf::InputHotkey18
    ,   &System::IniConf::InputHotkey19
    ,   &System::IniConf::InputHotkey20
    ,   &System::IniConf::InputHotkey21
    ,   &System::IniConf::InputHotkey22
    ,   &System::IniConf::InputHotkey23
    ,   &System::IniConf::InputHotkey24
    ,   &System::IniConf::InputHotkey25
    ,   &System::IniConf::InputHotkey26
    ,   &System::IniConf::InputHotkey27
    ,   &System::IniConf::InputHotkey28
    ,   &System::IniConf::InputHotkey29
    ,   &System::IniConf::InputHotkey30
    ,   &System::IniConf::InputHotkey31
    ,   &System::IniConf::InputHotkey32
    ,   &System::IniConf::InputHotkey33
    ,   &System::IniConf::InputHotkey34
    ,   &System::IniConf::InputHotkey35
    ,   &System::IniConf::InputHotkey36
    ,   &System::IniConf::InputHotkey37
    ,   &System::IniConf::InputHotkey38
    ,   &System::IniConf::InputHotkey39
    ,   &System::IniConf::InputHotkey40
    ,   &System::IniConf::InputHotkey41
    ,   &System::IniConf::InputHotkey42
    ,   &System::IniConf::InputHotkey43
    ,   &System::IniConf::InputHotkey44
    ,   &System::IniConf::InputHotkey45
    ,   &System::IniConf::InputHotkey46
    ,   &System::IniConf::InputHotkey47
    };

    i = 0;
    for (Common::Ini::Key *k : Hotkeys)
    {
        val = k->Get<std::string>(); // input_hotkeys

        if ( !val.empty() )
        {
            if ( ! SetHotKey(i, val) )
                ypa_log_out("input.engine: WARNING: Hotkey[%d] (%s) not accepted.\n", i, val.c_str());
        }
        i++;
    }

    return 1;
}

void INPEngine::Deinit()
{
    if ( _timer )
        _timer->Delete();

    for (InputNodeList &lst : _buttons)
        FreeKNodes(&lst);
    
    for (InputNodeList &lst : _sliders)
        FreeKNodes(&lst);
}

void INPEngine::QueryInput(TInputState *state)
{
    *state = TInputState();

    if ( _timer )
        state->Period = _timer->itimer_func64();
    else
        state->Period = 20;

    // Dev/capture aid: UA_FIXED_PERIOD=<ms> feeds the simulation a constant
    // frame period instead of wall time, so frame N of a run shows the same
    // world state on every run/binary (pixel A/B of renderer changes).
    // Inert unless set.
    static int fixedPeriod = -1;
    if ( fixedPeriod < 0 )
    {
        const char *v = getenv("UA_FIXED_PERIOD");
        fixedPeriod = (v && v[0]) ? atoi(v) : 0;
    }
    if ( fixedPeriod > 0 )
        state->Period = fixedPeriod;

    if ( !_wimp.HasFocus || _wimp.HasFocus() )
    {
        if ( _keyboard.KeyboardQuery )
            _keyboard.KeyboardQuery(state);
        
        state->HotKeyID = CheckHotKey(state->KbdLastHit);
        
        if ( _wimp.PointerQuery )
            _wimp.PointerQuery(&state->ClickInf);
        
        ClickCheck.CheckClick(&state->ClickInf);

        for (size_t i = 0; i < _buttons.size(); i++)
        {
            bool btn = true;
            UpdateList(&_buttons[i], &btn);

            if (btn)
                state->Buttons.Set(i);
        }

        for (size_t i = 0; i < _sliders.size(); i++)
        {
            bool btn = true;
            float pos = 0.0;
            UpdateList(&_sliders[i], &btn, &pos);

            if (btn)
                state->Sliders[i] = pos;
        }
    }
    else
    {
        for ( InputNodeList &lst : _sliders )
            UpdateList(&lst);
    }

    RumbleTick();
}

void INPEngine::AddClickBoxFront(ClickBox *box)
{
    ClickCheck.CBoxList.push_front(box);
}

void INPEngine::AddClickBoxBack(ClickBox *box)
{
    ClickCheck.CBoxList.push_back(box);
}

void INPEngine::RemClickBox(ClickBox *box)
{
    ClickCheck.CBoxList.remove(box);
}    

bool INPEngine::InitDriver(uint8_t type, const std::string &val)
{
    switch ( type )
    {
    case ITYPE_WIMP:                                     // input.wimp
        _wimp = FindInterface(val + ".class");
        if ( !_wimp.PointerQuery )
            return false;
        break;

    case ITYPE_TIMER:                                   // input.timer
        if ( _timer )
            _timer->Delete();

        _timer = Nucleus::CTFInit<NC_STACK_itimer>(val + ".class");
        if ( !_timer )
            return false;
        break;

    case ITYPE_KBD:
        _keyboard = FindInterface(val + ".class");
        if ( !_keyboard.KeyboardQuery )
            return false;
        break;

    default:
        return false;
        break;
    }
    return true;
}

void INPEngine::RegisterInterface(const TQueryState& iface)
{
    for ( TQueryState& i : Interfaces )
    {
        if (!StriCmp(i.ClassName, iface.ClassName))
        {
            i = iface;
            return;
        }
    }
    
    Interfaces.push_back(iface);
}

TQueryState INPEngine::FindInterface(const std::string &name)
{
    for ( const TQueryState& i : Interfaces )
    {
        if (!StriCmp(i.ClassName, name))
            return i;
    }
    
    return TQueryState("");
}


std::string INPEngine::ParseInputNodeNext(const std::string &inputStr, InputNodeList *lst)
{
    if (!lst)
        return std::string();
    
    lst->emplace_back();
    
    InputNode &node = lst->back();

    std::string *out = &node.DriverName;
    bool finishParse = false;

    for( size_t i = 0; i < inputStr.size(); i++ )
    {
        switch(inputStr[i])
        {
            case '\t':
            case ' ':
                if (finishParse)
                    return inputStr.substr(i);
                break;
                
            case '#':
                if (finishParse)
                    return inputStr.substr(i);
                
                node.Flags |= InputNode::FLAG_FSLIDE;
                break;
            
            case '&':
                if (finishParse)
                    return inputStr.substr(i);;

                node.Flags |= InputNode::FLAG_AND;
                break;
                
            case ':':
                finishParse = true;
                out = &node.KeyName;
                break;
            
            case '|':
                if (finishParse)
                    return inputStr.substr(i);;

                node.Flags |= InputNode::FLAG_OR;
                break;
            
            case '~':
                if (finishParse)
                    return inputStr.substr(i);;

                node.Flags |= InputNode::FLAG_INV;
                break;
            
            default:
                (*out) += inputStr[i];
        }
    }
    return std::string();
}
    
bool INPEngine::SetInputExpression(bool slider, uint32_t index, const std::string &keysStr)
{
    InputNodeList *pLst;
    if ( slider )
        pLst = &_sliders.at(index);
    else
        pLst = &_buttons.at(index);

    if ( !keysStr.empty() )
    {
        FreeKNodes(pLst);

        std::string keysString = keysStr;

        while (!keysString.empty())
            keysString = ParseInputNodeNext(keysString, pLst);

        for ( InputNode &node : *pLst )
        {
            if ( !node.DriverName.empty() && !node.KeyName.empty() )
            {
                node.DriverObj = Nucleus::CTFInit<NC_STACK_idev>(node.DriverName + ".class");
                if ( node.DriverObj )
                    node.IsSlider = node.DriverObj->BindKey(node.KeyName);
                else
                    ypa_log_out("input.class, WARN: Unknown driver '%s'.\n", node.DriverName.c_str());
            }
        }
    }
    return true;
}

void INPEngine::UpdateList(InputNodeList *lst, bool *btn, float *slider)
{
    bool btmp = true;
    float ftmp = 0.0;
    
    if (btn)
        btmp = *btn;
    if (slider)
        ftmp = *slider;
    
    for ( InputNode &node : *lst )
    {
        if ( node.DriverObj )
        {
            if (node.IsSlider || (node.Flags & InputNode::FLAG_FSLIDE) )
            {
                if ( node.Flags & InputNode::FLAG_INV )
                    ftmp -= node.DriverObj->GetSlider();
                else
                    ftmp += node.DriverObj->GetSlider();
            }
            else
            {
                bool state = node.DriverObj->GetState();

                if ( node.Flags & InputNode::FLAG_INV )
                    state = !state;

                if ( node.Flags & InputNode::FLAG_OR )
                    btmp |= state;
                else
                    btmp &= state;
            }
        }
    }
    
    if (btn)
        *btn = btmp;
    if (slider)
        *slider = ftmp;
}

bool INPEngine::SetHotKey(uint16_t id, const std::string &keyname)
{
    if ( id < _hotKeys.size() )
    {
        int16_t kId = GetKeyIDByName(keyname);
        if (kId == -1)
            return false;
        
        if ( KeyMatrix.at(kId).IsSlider == false )
        {
            _hotKeys[ id ] = kId;
            return true;
        }
    }
    return false;
}

void INPEngine::ResetSlider(int sldr)
{
    for( InputNode &node : _sliders.at(sldr) )
    {
        if (node.DriverObj)
            node.DriverObj->ResetSlider();
    }
}

void INPEngine::FreeKNodes(InputNodeList *lst)
{
    while (!lst->empty())
    {
        InputNode &nod = lst->front();

        if (nod.DriverObj)
            nod.DriverObj->Delete();

        lst->pop_front();
    }
}


int16_t INPEngine::GetKeyIDByName(const std::string &name)
{
    for( KeyMapName &m : KeyNamesTable )
    {
        if ( !StriCmp(m.Name, name) )
            return m.ID;
    }
    
    for( KeyMapName &m : KeyAltNamesTable )
    {
        if ( !StriCmp(m.Name, name) )
            return m.ID;
    }
    
    return -1;
}

int16_t INPEngine::CheckHotKey(int16_t key)
{
    if ( key == KC_NONE )
        return -1;

    for (size_t i = 0; i < _hotKeys.size(); i++)
    {
        if (_hotKeys[i] == key)
            return i;
    }
    return -1;
}

int16_t INPEngine::GetHotKeyID(int16_t keycode)
{
    for ( size_t i = 0; i < _hotKeys.size(); i++ )
    {
        if ( _hotKeys[i] == keycode )
            return i;
    }
    return -1;
}

int16_t INPEngine::GetHotKey(uint16_t id)
{
    if (id < _hotKeys.size())
        return _hotKeys[id];
    
    return KC_NONE;
}

// ---- Pad rumble ------------------------------------------------------------
//
// The force-feedback engine above was written for DirectInput-style haptic
// effects (sine/ramp/damper), which a DualSense does not expose: SDL_Haptic
// finds nothing usable, every FFEffect::OK() is false and the pad stays dead.
// The legacy effects are therefore also mapped onto the pad's two ERM motors.
//
// Three contributions are mixed because they overlap in play: the engine hum
// (continuous, refreshed by FFeedback_Update every 250ms), the machine gun
// (continuous, START on trigger down / STOP on release) and one-shot kicks
// (weapon fire, collisions, explosion shake). SDL rumble takes a duration, so
// a sustained effect is re-armed with RUMBLE_ARM_MS every RUMBLE_REARM_MS —
// long enough to never gap, short enough that everything goes quiet within
// ~1/3 s of the last event even if a STOP is lost.
//
// This is device-level and independent of the "alternative joystick model"
// setting; the in-game force-feedback option (PREF_FFDISABLE) still gates it
// upstream, as it should.

static const uint32_t RUMBLE_ARM_MS   = 300;
static const uint32_t RUMBLE_REARM_MS = 150;
static const float    RUMBLE_DEADBAND = 0.02;

static float RumbleClamp(float v)
{
    if ( v < 0.0 )
        return 0.0;
    if ( v > 1.0 )
        return 1.0;
    return v;
}

static uint16_t RumbleMag(float v)
{
    return (uint16_t)(RumbleClamp(v) * 65535.0);
}

bool INPEngine::_rumbleHum = false;

// UA_RUMBLE_HUM forces the engine hum on (1/yes/true/on) or off (0/no/false/
// off) regardless of the profile setting — a test hook and an escape hatch for
// anyone running without the shell. Anything else leaves the setting in charge.
// Read once; getenv is not worth doing per frame.
static int RumbleHumEnvOverride()
{
    static int cached = -2;

    if ( cached == -2 )
    {
        const char *e = getenv("UA_RUMBLE_HUM");

        if ( !e || !*e )
            cached = -1;
        else if ( *e == '0' || *e == 'n' || *e == 'N' || *e == 'f' || *e == 'F' )
            cached = 0;
        else
            cached = 1;
    }

    return cached;
}

bool INPEngine::EngineRumble()
{
    int forced = RumbleHumEnvOverride();

    if ( forced >= 0 )
        return forced != 0;

    return _rumbleHum;
}

void INPEngine::SetEngineRumble(bool on)
{
    _rumbleHum = on;

    // Drop whatever the hum was holding the moment it is switched off, so the
    // pad goes quiet on OK rather than at the next vehicle change.
    if ( !EngineRumble() )
        Instance._rumbleEngine.Reset();

    if ( NC_STACK_winp::JoyDebug() )
        sdlInputLog("Rumble engine hum %s (pref=%s, env=%s)\n",
                    EngineRumble() ? "ON" : "OFF",
                    on ? "on" : "off",
                    RumbleHumEnvOverride() < 0 ? "unset"
                        : (RumbleHumEnvOverride() ? "on" : "off"));
}

void INPEngine::RumbleEngine(int effID, int state, float p1, float p2)
{
    // Default is event-only rumble: the hum is the one source that is silent
    // unless asked for. The legacy haptic engine effects above are untouched —
    // this gate is the pad's alone.
    if ( state == FF_STATE_STOP || !EngineRumble() )
    {
        _rumbleEngine.Reset();

        if ( state != FF_STATE_STOP && NC_STACK_winp::JoyDebug() )
            sdlInputLog("Rumble engine eff=%d suppressed (hum off)\n", effID);

        return;
    }

    // p1 = the vehicle's magnitude (fixed per hull), p2 = the same scaled by
    // normalized speed. A standing vehicle keeps 35% so it still murmurs.
    float power = RumbleClamp(p1) * (0.35 + 0.65 * RumbleClamp(p2));

    switch ( effID )
    {
    case FF_TYPE_TANKENGINE:
        _rumbleEngine.Set(power, power * 0.25);         // heavy, big motor
        break;
    case FF_TYPE_JETENGINE:
        _rumbleEngine.Set(power * 0.40, power);         // whine, small motor
        break;
    default: // FF_TYPE_HELIENGINE
        _rumbleEngine.Set(power * 0.80, power * 0.55);  // rotor beat, both
        break;
    }

    if ( NC_STACK_winp::JoyDebug() )
        sdlInputLog("Rumble engine eff=%d state=%d p1=%.3f p2=%.3f -> low=%.3f high=%.3f\n",
                    effID, state, p1, p2, _rumbleEngine.low, _rumbleEngine.high);
}

void INPEngine::RumblePulse(const char *name, float low, float high, uint32_t ms)
{
    _rumblePulse.Set(RumbleClamp(low), RumbleClamp(high));
    _rumblePulseEnd = SDL_GetTicks() + ms;

    if ( NC_STACK_winp::JoyDebug() )
        sdlInputLog("Rumble pulse %s -> low=%.3f high=%.3f dur=%ums\n",
                    name, _rumblePulse.low, _rumblePulse.high, ms);
}

void INPEngine::RumbleStopAll()
{
    _rumbleEngine.Reset();
    _rumbleMGun.Reset();
    _rumblePulse.Reset();
    _rumblePulseEnd = 0;
}

void INPEngine::RumbleTick()
{
    if ( !NC_STACK_winp::HasRumble() )
    {
        // Nothing to drive. Forget what "the pad" was doing so a controller
        // plugged in mid-level is armed on its very next tick instead of
        // being compared against a stale level and skipped.
        _rumbleApplied.Reset();
        _rumbleRunning = false;
        return;
    }

    uint32_t now = SDL_GetTicks();

    if ( _rumblePulseEnd && now >= _rumblePulseEnd )
    {
        _rumblePulse.Reset();
        _rumblePulseEnd = 0;
    }

    RumbleLevel want;
    want.low  = std::max( _rumbleEngine.low,  std::max(_rumbleMGun.low,  _rumblePulse.low) );
    want.high = std::max( _rumbleEngine.high, std::max(_rumbleMGun.high, _rumblePulse.high) );

    // Distant explosions arrive as shakes of a few thousandths; below the
    // motors' own threshold they only cost battery, so treat them as silence.
    if ( want.low < RUMBLE_DEADBAND )
        want.low = 0.0;
    if ( want.high < RUMBLE_DEADBAND )
        want.high = 0.0;

    bool changed = fabs(want.low  - _rumbleApplied.low)  > 0.02 ||
                   fabs(want.high - _rumbleApplied.high) > 0.02;

    if ( want.Silent() )
    {
        if ( !_rumbleRunning )
            return;
    }
    else if ( !changed && (now - _rumbleArmedAt) < RUMBLE_REARM_MS )
    {
        return; // still inside the armed window, nothing to do
    }

    uint32_t dur = want.Silent() ? 0 : RUMBLE_ARM_MS;

    NC_STACK_winp::Rumble( RumbleMag(want.low), RumbleMag(want.high), dur );

    if ( NC_STACK_winp::JoyDebug() )
        sdlInputLog("Rumble %s low=%u high=%u dur=%ums\n",
                    changed ? "set " : "arm ",
                    RumbleMag(want.low), RumbleMag(want.high), dur);

    _rumbleApplied = want;
    _rumbleArmedAt = now;
    _rumbleRunning = !want.Silent();
}
// ---------------------------------------------------------------------------


void INPEngine::ForceFeedback(uint8_t state, uint8_t effID, float p1, float p2, float p3, float p4)
{
    if ( NC_STACK_winp::JoyDebug() )
        sdlInputLog("FF event state=%u eff=%u p=(%.2f %.2f %.2f %.2f)\n",
                    state, effID, p1, p2, p3, p4);

    switch ( effID )
    {
    case FF_TYPE_TANKENGINE:
        FFDOTankEngine(state, p1, p2);
        break;
    case FF_TYPE_JETENGINE:
        FFDOJetEngine(state, p1, p2);
        break;
    case FF_TYPE_HELIENGINE:
        FFDOHeliEngine(state, p1, p2);
        break;
    case FF_TYPE_ROTDAMPER:
        FFDORotDamper(state, p1);
        break;
    case FF_TYPE_MINIGUN:
        FFDOMiniGun(state);
        break;
    case FF_TYPE_MISSILEFIRE:
        FFDOMissileFire(state);
        break;
    case FF_TYPE_GRENADEFIRE:
        FFDOGrenadeFire(state);
        break;
    case FF_TYPE_BOMBFIRE:
        FFDOBombFire(state);
        break;
    case FF_TYPE_COLLISION:
        FFDOCollision(state, p1, p3, p4);
        break;
    case FF_TYPE_SHAKE:
        FFDOShake(state, p1, p2, p3, p4);
        break;
    case FF_TYPE_ALL:
        FFstopAll();
        break;
    default:
        return;
    }
}


void INPEngine::FFDOTankEngine(int state, float p1, float p2)
{
    RumbleEngine(FF_TYPE_TANKENGINE, state, p1, p2);

    if ( _ffTankEngine.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffTankEngine.Stop();
            _ffJetEngine.Stop();
            _ffCopterEngine.Stop();

            _ffTankEngine.Run();

            float a2 = p1 * 16383.0; // 5000
            float a3 = 1000.0 / (p2 * 13.0 + 5.0);
            _ffTankEngine.Update(a2, a3);
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffTankEngine.Stop();
        }
        else if ( state == FF_STATE_UPDATE )
        {
            float v5 = p1 * 19660.0; // 6000
            float v6 = 1000.0 / (p2 * 13.0 + 5.0);
            _ffTankEngine.Update(v5, v6);
        }
    }
}

void INPEngine::FFDOJetEngine(int state, float p1, float p2)
{
    RumbleEngine(FF_TYPE_JETENGINE, state, p1, p2);

    if ( _ffJetEngine.OK() )
    {
        if ( state == FF_STATE_START)
        {
            _ffTankEngine.Stop();
            _ffJetEngine.Stop();
            _ffCopterEngine.Stop();

            _ffJetEngine.Run();

            float a2 = p1 * 19660.0; // 6000
            float a3 = 1000.0 / (p2 * 12.0 + 14.0);
            _ffJetEngine.Update(a2, a3);
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffJetEngine.Stop();
        }
        else if ( state == FF_STATE_UPDATE )
        {
            float v5 = p1 * 19660.0; // 6000
            float v6 = 1000.0 / (p2 * 12.0 + 14.0);
            _ffJetEngine.Update(v5, v6);
        }
    }
}

void INPEngine::FFDOHeliEngine(int state, float p1, float p2)
{
    RumbleEngine(FF_TYPE_HELIENGINE, state, p1, p2);

    if ( _ffCopterEngine.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffTankEngine.Stop();
            _ffJetEngine.Stop();
            _ffCopterEngine.Stop();

            _ffCopterEngine.Run();

            float a2 = p1 * 26213.6; //8000.0;
            float a3 = 1000.0 / (p2 * 12.0 + 6.0);
            _ffCopterEngine.Update(a2, a3);
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffCopterEngine.Stop();
        }
        else if ( state == FF_STATE_UPDATE )
        {
            float v5 = p1 * 26213.6; //8000.0;
            float v6 = 1000.0 / (p2 * 12.0 + 6.0);
            _ffCopterEngine.Update(v5, v6);
        }
    }
}

void INPEngine::FFDORotDamper(int state, float p1)
{
    // No rumble mapping: this is a steering-resistance spring, not a
    // vibration, and a pad has nothing to resist with.

    if ( _ffRotDamper.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffRotDamper.Stop();

            _ffRotDamper.Update(p1 * 32767.0);

            _ffRotDamper.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffRotDamper.Stop();
        }
    }
}

void INPEngine::FFDOMiniGun(int state)
{
    // Held down while firing, so it is sustained rather than a pulse.
    if ( state == FF_STATE_START )
        _rumbleMGun.Set(0.20, 0.85);       // buzz, small motor
    else if ( state == FF_STATE_STOP )
        _rumbleMGun.Reset();

    if ( _ffMGun.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffMGun.Stop();

            _ffMGun.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffMGun.Stop();
        }
    }
}

void INPEngine::FFDOMissileFire(int state)
{
    if ( state == FF_STATE_START )
        RumblePulse("missile", 1.00, 0.55, 300);   // legacy effect: 600ms, full magnitude

    if ( _ffMissFire.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffMissFire.Stop();
            _ffGrenadeFire.Stop();
            _ffBombFire.Stop();

            _ffMissFire.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffMissFire.Stop();
        }
    }
}

void INPEngine::FFDOGrenadeFire(int state)
{
    if ( state == FF_STATE_START )
        RumblePulse("grenade", 0.85, 0.45, 220);   // legacy effect: 300ms ramp

    if ( _ffGrenadeFire.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffMissFire.Stop();
            _ffGrenadeFire.Stop();
            _ffBombFire.Stop();

            _ffGrenadeFire.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffGrenadeFire.Stop();
        }
    }
}

void INPEngine::FFDOBombFire(int state)
{
    if ( state == FF_STATE_START )
        RumblePulse("bomb", 0.80, 0.30, 260);      // legacy effect: 400ms ramp, 80%

    if ( _ffBombFire.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffMissFire.Stop();
            _ffGrenadeFire.Stop();
            _ffBombFire.Stop();

            _ffBombFire.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffBombFire.Stop();
        }
    }
}

void INPEngine::FFDOCollision(int state, float a2, float a3, float a4)
{
    // a2 = impact force 0..1 (a3/a4 are the direction the pad cannot express)
    if ( state == FF_STATE_START )
        RumblePulse("collision", a2, a2 * 0.70, 120);  // legacy effect: 95ms ramp

    if ( _ffCollide.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffCollide.Stop();

            _ffCollide.Update(a2, a3, a4);

            _ffCollide.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffCollide.Stop();
        }
    }
}

void INPEngine::FFDOShake(int state, float a2, float a3, float a4, float a5)
{
    // a2 = power 0..1, a3 = the effect's own duration in ms (a4/a5 = direction)
    if ( state == FF_STATE_START )
    {
        float dur = (a3 < 80.0) ? 80.0 : ((a3 > 800.0) ? 800.0 : a3);
        RumblePulse("shake", a2, a2 * 0.60, (uint32_t)dur);
    }

    if ( _ffShake.OK() )
    {
        if ( state == FF_STATE_START )
        {
            _ffShake.Stop();

            _ffShake.Update(a2, a3, a4, a5);

            _ffShake.Run();
        }
        else if ( state == FF_STATE_STOP )
        {
            _ffShake.Stop();
        }
    }
}

void INPEngine::FFstopAll()
{
    RumbleStopAll();

    _ffTankEngine.Stop();
    _ffJetEngine.Stop();
    _ffCopterEngine.Stop();
    _ffRotDamper.Stop();
    _ffMGun.Stop();
    _ffMissFire.Stop();
    _ffGrenadeFire.Stop();
    _ffBombFire.Stop();
    _ffCollide.Stop();
    _ffShake.Stop();
}

std::array<KeyInfo, KC_MAX> INPEngine::KeyMatrix {{
    {KC_NONE, false},       {KC_ESCAPE, false},         
    {KC_SPACE, false},      {KC_UP, false},      
    {KC_DOWN, false},       {KC_LEFT, false},      
    {KC_RIGHT, false},      {KC_F1, false},              
    {KC_F2, false},         {KC_F3, false},
    {KC_F4, false},         {KC_F5, false},              
    {KC_F6, false},         {KC_F7, false},      
    {KC_F8, false},         {KC_F9, false},          
    {KC_F10, false},        {KC_F11, false},            
    {KC_F12, false},        {KC_BACKSPACE, false},
    {KC_TAB, false},        {KC_CLEAR, false},        
    {KC_RETURN, false},     {KC_CTRL, false},  
    {KC_SHIFT, false},      {KC_ALT, false},           
    {KC_PAUSE, false},      {KC_PGUP, false},
    {KC_PGDOWN, false},     {KC_END, false},            
    {KC_HOME, false},       {KC_SELECT, false},
    {KC_EXECUTE, false},    {KC_SNAPSHOT, false},
    {KC_INSERT, false},     {KC_DELETE, false},         
    {KC_HELP, false},       {KC_1, false},
    {KC_2, false},          {KC_3, false},                  
    {KC_4, false},          {KC_5, false},          
    {KC_6, false},          {KC_7, false},              
    {KC_8, false},          {KC_9, false},                  
    {KC_0, false},          {KC_A, false},
    {KC_B, false},          {KC_C, false},                  
    {KC_D, false},          {KC_E, false},          
    {KC_F, false},          {KC_G, false},              
    {KC_H, false},          {KC_I, false},                  
    {KC_J, false},          {KC_K, false},
    {KC_L, false},          {KC_M, false},                  
    {KC_N, false},          {KC_O, false},          
    {KC_P, false},          {KC_Q, false},              
    {KC_R, false},          {KC_S, false},                  
    {KC_T, false},          {KC_U, false},
    {KC_V, false},          {KC_W, false},                  
    {KC_X, false},          {KC_Y, false},          
    {KC_Z, false},          {KC_NUM0, false},   
    {KC_NUM1, false},       {KC_NUM2, false},       
    {KC_NUM3, false},       {KC_NUM4, false},
    {KC_NUM5, false},       {KC_NUM6, false},       
    {KC_NUM7, false},       {KC_NUM8, false}, 
    {KC_NUM9, false},       {KC_NUMMUL, false},
    {KC_NUMPLUS, false},    {KC_NUMDOT, false},     
    {KC_NUMMINUS, false},   {KC_NUMENTER, false},
    {KC_NUMDIV, false},     {KC_EXTRA1, false},   
    {KC_EXTRA2, false},     {KC_EXTRA3, false},
    {KC_EXTRA4, false},     {KC_EXTRA5, false},   
    {KC_EXTRA6, false},     {KC_EXTRA7, false},       
    {KC_EXTRA8, false},     {KC_EXTRA9, false},
    {KC_EXTRA10, false},    {KC_EXTRA11, false},      
    {KC_EXTRA12, false},    {KC_EXTRA13, false},
    {KC_EXTRA14, false},    {KC_EXTRA15, false},
    {KC_EXTRA16, false},    {KC_EXTRA17, false},        
    {KC_EXTRA18, false},    {KC_LMB, false},
    {KC_RMB, false},        {KC_MMB, false},                
    {KC_MOUSEX, true},      {KC_MOUSEY, true},     
    {KC_JOYB0, false},      {KC_JOYB1, false},          
    {KC_JOYB2, false},      {KC_JOYB3, false},              
    {KC_JOYB4, false},      {KC_JOYB5, false},
    {KC_JOYB6, false},      {KC_JOYB7, false},              
    {KC_JOYX, true},        {KC_JOYY, true},      
    {KC_JOYTHROTTLE, true}, {KC_JOYHATX, true},         
    {KC_JOYHATY, true},     {KC_JOYRUDDER, true}
}};

std::array<KeyMapName, KC_MAX> INPEngine::KeyNamesTable {{
    {"nop", KC_NONE},        {"esc", KC_ESCAPE},         
    {"space", KC_SPACE},     {"up", KC_UP},      
    {"down", KC_DOWN},       {"left", KC_LEFT},      
    {"right", KC_RIGHT},     {"f1", KC_F1},              
    {"f2", KC_F2},           {"f3", KC_F3},
    {"f4", KC_F4},           {"f5", KC_F5},              
    {"f6", KC_F6},           {"f7", KC_F7},      
    {"f8", KC_F8},           {"f9", KC_F9},          
    {"f10", KC_F10},         {"f11", KC_F11},            
    {"f12", KC_F12},         {"bs", KC_BACKSPACE},
    {"tab", KC_TAB},         {"clear", KC_CLEAR},        
    {"return", KC_RETURN},   {"ctrl", KC_CTRL},    
    {"rshift", KC_SHIFT},     {"alt", KC_ALT},           
    {"pause", KC_PAUSE},     {"pageup", KC_PGUP},
    {"pagedown", KC_PGDOWN}, {"end", KC_END},            
    {"home", KC_HOME},       {"select", KC_SELECT},
    {"execute", KC_EXECUTE}, {"snapshot", KC_SNAPSHOT},
    {"ins", KC_INSERT},      {"del", KC_DELETE},         
    {"help", KC_HELP},       {"1", KC_1},
    {"2", KC_2},             {"3", KC_3},                  
    {"4", KC_4},             {"5", KC_5},          
    {"6", KC_6},             {"7", KC_7},              
    {"8", KC_8},             {"9", KC_9},                  
    {"0", KC_0},             {"a", KC_A},
    {"b", KC_B},             {"c", KC_C},                  
    {"d", KC_D},             {"e", KC_E},          
    {"f", KC_F},             {"g", KC_G},              
    {"h", KC_H},             {"i", KC_I},                  
    {"j", KC_J},             {"k", KC_K},
    {"l", KC_L},             {"m", KC_M},                  
    {"n", KC_N},             {"o", KC_O},          
    {"p", KC_P},             {"q", KC_Q},              
    {"r", KC_R},             {"s", KC_S},                  
    {"t", KC_T},             {"u", KC_U},
    {"v", KC_V},             {"w", KC_W},                  
    {"x", KC_X},             {"y", KC_Y},          
    {"z", KC_Z},             {"num0", KC_NUM0},   
    {"num1", KC_NUM1},       {"num2", KC_NUM2},       
    {"num3", KC_NUM3},       {"num4", KC_NUM4},
    {"num5", KC_NUM5},       {"num6", KC_NUM6},       
    {"num7", KC_NUM7},       {"num8", KC_NUM8}, 
    {"num9", KC_NUM9},       {"nummul", KC_NUMMUL},
    {"numplus", KC_NUMPLUS}, {"numdot", KC_NUMDOT},     
    {"numminus", KC_NUMMINUS},{"enter", KC_NUMENTER},
    {"numdiv", KC_NUMDIV},   {"extra1", KC_EXTRA1},   
    {"extra2", KC_EXTRA2},   {"extra3", KC_EXTRA3},
    {"extra4", KC_EXTRA4},   {"extra5", KC_EXTRA5},   
    {"extra6", KC_EXTRA6},   {"extra7", KC_EXTRA7},       
    {"extra8", KC_EXTRA8},   {"extra9", KC_EXTRA9},
    {"extra10", KC_EXTRA10}, {"extra11", KC_EXTRA11},      
    {"extra12", KC_EXTRA12}, {"extra13", KC_EXTRA13},
    {"extra14", KC_EXTRA14}, {"extra15", KC_EXTRA15},
    {"extra16", KC_EXTRA16}, {"extra17", KC_EXTRA17},        
    {"extra18", KC_EXTRA18}, {"lmb", KC_LMB},
    {"rmb", KC_RMB},         {"mmb", KC_MMB},                
    {"mousex", KC_MOUSEX},   {"mousey", KC_MOUSEY},     
    {"joyb0", KC_JOYB0},     {"joyb1", KC_JOYB1},          
    {"joyb2", KC_JOYB2},     {"joyb3", KC_JOYB3},              
    {"joyb4", KC_JOYB4},     {"joyb5", KC_JOYB5},
    {"joyb6", KC_JOYB6},     {"joyb7", KC_JOYB7},              
    {"joyx", KC_JOYX},       {"joyy", KC_JOYY},      
    {"joythrottle", KC_JOYTHROTTLE},{"joyhatx", KC_JOYHATX},         
    {"joyhaty", KC_JOYHATY}, {"joyrudder", KC_JOYRUDDER}
}};

std::vector<KeyMapName> INPEngine::KeyAltNamesTable ({
    {"shift", KC_SHIFT},    {"lshift", KC_SHIFT},
    {"lctrl", KC_CTRL},      {"rctrl", KC_CTRL},
    {"lalt", KC_ALT},        {"ralt", KC_CTRL},
});

std::array<std::string, KC_MAX> INPEngine::KeyTitle;




void INPEngine::TClickCheck::GetClick(TClickBoxInf *click, TMousePos &mouse)
{
    for(std::list<ClickBox *>::iterator it = CBoxList.begin();
            it != CBoxList.end();
            it++)
    {
        ClickBox *box = *it;

        mouse.BoxPos.x = mouse.ScreenPos.x - box->x;
        mouse.BoxPos.y = mouse.ScreenPos.y - box->y;

        if ( mouse.BoxPos.x >= 0 && mouse.BoxPos.x < box->w &&
                mouse.BoxPos.y >= 0 && mouse.BoxPos.y < box->h )
        {
            click->selected_btn = box;

            for (unsigned int i = 0; i < box->buttons.size(); i++)
            {
                ButtonBox &btn = box->buttons[i];

                mouse.BtnPos.x = mouse.BoxPos.x - btn.x;
                mouse.BtnPos.y = mouse.BoxPos.y - btn.y;

                if ( mouse.BtnPos.x >= 0 && mouse.BtnPos.x < btn.w &&
                        mouse.BtnPos.y >= 0 && mouse.BtnPos.y < btn.h )
                {
                    click->selected_btnID = i;
                    break;
                }
            }
            break;
        }
    }
}

void INPEngine::TClickCheck::CheckClick(TClickBoxInf *arg)
{
    arg->flag |= TClickBoxInf::FLAG_OK;
    arg->selected_btn = NULL;
    arg->selected_btnID = -1;

    GetClick(arg, arg->move);

    if ( arg->flag & TClickBoxInf::FLAG_LM_HOLD )
    {
        if ( arg->selected_btnID >= 0 &&
                SelectedCbox == arg->selected_btn &&
                SelectedButton == arg->selected_btnID )
            arg->flag |= TClickBoxInf::FLAG_BTN_HOLD;
    }

    if ( arg->flag & TClickBoxInf::FLAG_LM_DOWN )
    {
        GetClick(arg, arg->ldw_pos);

        if ( arg->selected_btnID == -1 )
        {
            SelectedCbox = 0;
            SelectedButton = -1;
        }
        else
        {
            SelectedCbox = arg->selected_btn;
            SelectedButton = arg->selected_btnID;
            arg->flag |= TClickBoxInf::FLAG_BTN_DOWN;
        }
    }

    if ( arg->flag & TClickBoxInf::FLAG_LM_UP )
    {
        GetClick(arg, arg->lup_pos);

        if ( arg->selected_btn )
        {
            if ( arg->selected_btn == SelectedCbox &&
                    arg->selected_btnID == SelectedButton )
                arg->flag |= TClickBoxInf::FLAG_BTN_UP;
        }
        SelectedButton = -1;
        SelectedCbox = 0;
    }
}

}