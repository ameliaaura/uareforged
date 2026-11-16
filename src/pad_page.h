#ifndef PAD_PAGE_H_INCLUDED
#define PAD_PAGE_H_INCLUDED

class NC_STACK_ypaworld;
struct TInputState;

// The CONTROLLER BUTTONS page (desktop): one row per action (src/pad_remap.h) with its
// button; choose a row and press a pad button to bind it (a button another action uses
// swaps), "Reset to defaults", "Back". Opened from Options > Input and from the
// in-mission menu's CONTROLLER SETTINGS page; a 1998-style list window (the look of the
// Input page's key list). Works with the mouse, the keyboard (arrows, Enter, Esc,
// Backspace = clear) and the controller itself (D-pad / left stick, A choose, B back).
// Changes apply at once; when the page closes, a changed binding set is patched into
// the profile (yw_PatchInputProfile: only the input lines, atomic, no write without
// a change).
namespace PadPage
{
void Open(NC_STACK_ypaworld *yw, bool inMission);
void Close(NC_STACK_ypaworld *yw);
bool IsOpen();
// Handles the page's input and then clears the input state, so nothing behind the page
// (the Input page, the in-mission menu, the mission) sees it.
void Handle(NC_STACK_ypaworld *yw, TInputState *inpt);
void Draw(NC_STACK_ypaworld *yw);
}

#endif
