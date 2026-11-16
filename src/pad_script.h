#ifndef PAD_SCRIPT_H_INCLUDED
#define PAD_SCRIPT_H_INCLUDED

// Dev aid: UA_PAD_SCRIPT attaches an SDL virtual game controller and presses its
// buttons on a timetable, so a hidden, unattended run goes through the real SDL
// game controller path (synthetic input never reaches SDL on macOS). Inert unless set.
//
//   UA_PAD_SCRIPT="t=2.0 a down; t=2.1 a up; m=3 righttrigger down; m=3.5 righttrigger up"
//     t=<s>  seconds after the pad was attached
//     m=<s>  seconds after the first mission frame (MissionStarted)
//     p=<s>  seconds after the CONTROLLER BUTTONS page first opened (PageOpened)
//     <button> down|up    SDL button names (a, b, x, y, back, guide, start, leftstick,
//                         rightstick, leftshoulder, rightshoulder, dpup, dpdown, dpleft,
//                         dpright, misc1, paddle1..4, touchpad, lefttrigger, righttrigger)
//     key <esc|return|up|down> (one keyboard hit: the page's Esc / Enter / arrows)
//   UA_PAD_TYPE=ps5|xbox|generic  the controller type it reports (button names on the page)
// Every step is logged ("pad script: ...") and the game side logs what it saw.
namespace PadScript
{
void Init();             // after SDL's game controller init, before the device pick
void Tick();             // once per input poll
bool Active();
void MissionStarted();   // first frame of a mission (m= times)
void PageOpened();       // the CONTROLLER BUTTONS page opened (p= times)
}

#endif
