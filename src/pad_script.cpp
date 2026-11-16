#include "pad_script.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "pad_remap.h"

namespace PadScript
{

struct Step
{
    char base = 't';        // t= start, m= mission, p= page
    uint32_t ms = 0;
    int button = -1;        // PadRemap button
    bool down = false;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    std::string text;
};

static bool s_active = false;
static std::vector<Step> s_steps;
static size_t s_next = 0;
static uint32_t s_t0 = 0;
static uint32_t s_m0 = 0;       // 0 = no mission yet
static uint32_t s_p0 = 0;       // 0 = page not opened yet
static SDL_Joystick *s_joy = NULL;
static int s_devIndex = -1;

bool Active()
{
    return s_active;
}

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if ( a == std::string::npos )
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(a, e - a + 1);
}

static void Parse(const char *src)
{
    std::string all(src);
    size_t at = 0;
    while ( at <= all.size() )
    {
        size_t e = all.find(';', at);
        if ( e == std::string::npos )
            e = all.size();
        std::string item = Trim(all.substr(at, e - at));
        at = e + 1;
        if ( item.empty() )
            continue;
        Step st;
        st.text = item;
        char tm = 0, w1[64] = {0}, w2[64] = {0};
        float sec = 0;
        if ( sscanf(item.c_str(), "%c=%f %63s %63s", &tm, &sec, w1, w2) < 3 || (tm != 't' && tm != 'm' && tm != 'p') )
        {
            fprintf(stderr, "pad script: cannot read \"%s\"\n", item.c_str());
            continue;
        }
        st.base = tm;
        st.ms = (uint32_t)(sec * 1000.0f);
        if ( !strcmp(w1, "key") )
        {
            if ( !strcmp(w2, "esc") ) st.key = SDL_SCANCODE_ESCAPE;
            else if ( !strcmp(w2, "return") ) st.key = SDL_SCANCODE_RETURN;
            else if ( !strcmp(w2, "up") ) st.key = SDL_SCANCODE_UP;
            else if ( !strcmp(w2, "down") ) st.key = SDL_SCANCODE_DOWN;
            else if ( !strcmp(w2, "backspace") ) st.key = SDL_SCANCODE_BACKSPACE;
            if ( st.key == SDL_SCANCODE_UNKNOWN )
            {
                fprintf(stderr, "pad script: unknown key in \"%s\"\n", item.c_str());
                continue;
            }
        }
        else
        {
            st.button = PadRemap::ButtonFromName(w1);
            st.down = !strcmp(w2, "down");
            if ( st.button < 0 || (!st.down && strcmp(w2, "up")) )
            {
                fprintf(stderr, "pad script: unknown button / state in \"%s\"\n", item.c_str());
                continue;
            }
        }
        s_steps.push_back(st);
    }
}

void Init()
{
    const char *src = getenv("UA_PAD_SCRIPT");
    if ( !src || !src[0] )
        return;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    // a hidden window never has the focus; let the pad through anyway
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_VirtualJoystickDesc d;
    memset(&d, 0, sizeof(d));
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    // every button and axis mapped (without the masks SDL leaves misc1, the paddles and
    // the touchpad out of the virtual pad's mapping)
    d.button_mask = (1u << SDL_CONTROLLER_BUTTON_MAX) - 1;
    d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    d.name = "UA virtual pad (UA_PAD_SCRIPT)";
    const char *type = getenv("UA_PAD_TYPE");
    if ( type && !strcmp(type, "ps5") )
    {
        d.vendor_id = 0x054c;
        d.product_id = 0x0ce6;
    }
    else if ( type && !strcmp(type, "xbox") )
    {
        d.vendor_id = 0x045e;
        d.product_id = 0x0b12;
    }
    s_devIndex = SDL_JoystickAttachVirtualEx(&d);
    if ( s_devIndex < 0 )
    {
        fprintf(stderr, "pad script: SDL_JoystickAttachVirtualEx failed: %s\n", SDL_GetError());
        return;
    }
    {
        // SDL's own mapping for a virtual pad leaves the touchpad out: give it the full one
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(s_devIndex), guid, sizeof(guid));
        const std::string m = std::string(guid) + "," + d.name +
            ",a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
            "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,misc1:b15,paddle1:b16,paddle2:b17,"
            "paddle3:b18,paddle4:b19,touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
        if ( SDL_GameControllerAddMapping(m.c_str()) < 0 )
            fprintf(stderr, "pad script: mapping not added: %s\n", SDL_GetError());
    }
    s_joy = SDL_JoystickOpen(s_devIndex);
    if ( !s_joy )
    {
        fprintf(stderr, "pad script: SDL_JoystickOpen(%d) failed: %s\n", s_devIndex, SDL_GetError());
        return;
    }
    // triggers rest at the bottom of the raw range (mapped to 0), sticks centred
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        SDL_JoystickSetVirtualAxis(s_joy, a, (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) ? -32768 : 0);
    Parse(src);
    s_active = true;
    s_t0 = SDL_GetTicks();
    fprintf(stderr, "pad script: virtual game controller attached (device %d, game controller %s, type %s), %d steps\n",
            s_devIndex, SDL_IsGameController(s_devIndex) ? "yes" : "no", type ? type : "generic", (int)s_steps.size());
#else
    fprintf(stderr, "pad script: needs SDL 2.0.14 or newer\n");
#endif
}

void MissionStarted()
{
    if ( s_active && !s_m0 )
    {
        s_m0 = SDL_GetTicks();
        fprintf(stderr, "pad script: mission started at t=%.2f\n", (s_m0 - s_t0) / 1000.0f);
    }
}

void PageOpened()
{
    if ( s_active && !s_p0 )
    {
        s_p0 = SDL_GetTicks();
        fprintf(stderr, "pad script: page opened at t=%.2f\n", (s_p0 - s_t0) / 1000.0f);
    }
}

void Tick()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if ( !s_active || !s_joy )
        return;
    const uint32_t now = SDL_GetTicks();
    while ( s_next < s_steps.size() )
    {
        const Step &st = s_steps[s_next];
        if ( (st.base == 'm' && !s_m0) || (st.base == 'p' && !s_p0) )
            return;
        const uint32_t base = st.base == 'm' ? s_m0 : (st.base == 'p' ? s_p0 : s_t0);
        if ( now - base < st.ms )
            return;
        if ( st.key != SDL_SCANCODE_UNKNOWN )
        {
            SDL_Event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = SDL_KEYDOWN;
            ev.key.state = SDL_PRESSED;
            ev.key.keysym.scancode = st.key;
            ev.key.keysym.sym = SDL_GetKeyFromScancode(st.key);
            SDL_PushEvent(&ev);
            ev.type = SDL_KEYUP;
            ev.key.state = SDL_RELEASED;
            SDL_PushEvent(&ev);
        }
        else if ( st.button == PadRemap::BTN_LEFTTRIGGER || st.button == PadRemap::BTN_RIGHTTRIGGER )
        {
            SDL_JoystickSetVirtualAxis(s_joy, st.button == PadRemap::BTN_LEFTTRIGGER ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
                                       st.down ? 32767 : -32768);
        }
        else
        {
            SDL_JoystickSetVirtualButton(s_joy, st.button, st.down ? SDL_PRESSED : SDL_RELEASED);
        }
        fprintf(stderr, "pad script: t=%.2f %s\n", (now - s_t0) / 1000.0f, st.text.c_str());
        s_next++;
    }
#endif
}

}
