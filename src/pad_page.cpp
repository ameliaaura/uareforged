#include "pad_page.h"

#include <SDL2/SDL.h>
#include <string>

#include "includes.h"
#include "vr/vr.h"
#include "yw.h"
#include "yw_internal.h"
#include "font.h"
#include "lstvw.h"
#include "log.h"
#include "winp.h"
#include "pad_remap.h"
#include "pad_script.h"
#include "system/sound.h"

bool yw_PatchInputProfile(const std::string &userName, NC_STACK_ypaworld *yw);

namespace PadPage
{

static const uint32_t kCaptureMs = 5000;

static GuiList s_list;
static bool s_inited = false;
static bool s_open = false;
static bool s_inMission = false;
static PadRemap::Bindings s_atOpen;     // to tell whether anything changed
static int s_capture = -1;              // action waiting for a button, -1 = none
static bool s_waitRelease = false;      // capture starts once every pad button is up
static uint32_t s_captureT0 = 0;
static std::string s_msg;               // result of the last change
static std::string s_msg2;              // its second line (what was swapped), may be empty
static bool s_msgErr = false;

static int RowCount()
{
    return PadRemap::ActionCount() + 2; // + Reset to defaults, Back
}

static int RowReset() { return PadRemap::ActionCount(); }
static int RowBack()  { return PadRemap::ActionCount() + 1; }

bool IsOpen()
{
    return s_open;
}

// the action's caption: the 1998 key list's (player's language) where it has one
static std::string ActionTitle(NC_STACK_ypaworld *yw, int a)
{
    const PadRemap::Action &act = PadRemap::GetAction(a);
    int idx = -1;
    if ( act.kind == PadRemap::KIND_HOTKEY )
        idx = UserData::InputIndexFromConfig(World::INPUT_BIND_TYPE_HOTKEY, act.param);
    else if ( act.kind == PadRemap::KIND_INPBUTTON )
        idx = UserData::InputIndexFromConfig(World::INPUT_BIND_TYPE_BUTTON, act.param);
    else if ( act.kind == PadRemap::KIND_JOYBIT && act.param < 4 )
    {
        // JOYB0..3 = input buttons 0 (fire), 2 (machine gun), 1 (missile cam), 3 (brake)
        static const int joyToButton[4] = { 0, 2, 1, 3 };
        idx = UserData::InputIndexFromConfig(World::INPUT_BIND_TYPE_BUTTON, joyToButton[act.param]);
    }
    if ( idx > 0 && yw && yw->_GameShell && idx < (int)yw->_GameShell->InputConfigTitle.size()
         && !yw->_GameShell->InputConfigTitle[idx].empty() )
        return yw->_GameShell->InputConfigTitle[idx];
    return act.name;
}

static std::string Label(int btn)
{
    return PadRemap::ButtonLabel(btn, (PadRemap::Style)NC_STACK_winp::PadStyle());
}

static void SetMsg(const std::string &m, bool err = false, const std::string &m2 = std::string())
{
    s_msg = m;
    s_msg2 = m2;
    s_msgErr = err;
    const std::string all = m2.empty() ? m : m + "  " + m2;
    ypa_log_out("pad buttons: %s\n", all.c_str());
    fprintf(stderr, "pad buttons: %s\n", all.c_str());
}

void Open(NC_STACK_ypaworld *yw, bool inMission)
{
#ifdef UA_VR_ENABLED
    if ( VR::Active() )
        return; // VR has its own controls
#endif
    if ( s_open )
        return;

    if ( s_inited )
        s_list.Free();

    GuiList::tInit args;
    args.title = Locale::Text::Advanced(Locale::ADV_PADBUTTONS);
    args.resizeable = false;
    args.numEntries = RowCount();
    int shown = (int)(yw->_screenSize.y * 0.55) / yw->_fontH;
    if ( shown > 14 )
        shown = 14;
    if ( shown < 4 )
        shown = 4;
    args.shownEntries = shown;
    args.maxShownEntries = shown;
    args.minShownEntries = shown;
    args.firstShownEntry = 0;
    args.selectedEntry = 0;
    args.withIcon = false;
    args.entryHeight = yw->_fontH;
    args.entryWidth = yw->_screenSize.x * 0.66;
    args.minEntryWidth = args.entryWidth;
    args.maxEntryWidth = args.entryWidth;
    args.enabled = true;
    args.vborder = yw->_fontBorderH;
    args.instantInput = false;
    args.keyboardInput = true;
    args.withHelp = false;
    if ( !s_list.Init(yw, args) )
    {
        ypa_log_out("pad buttons: unable to create the page\n");
        return;
    }
    s_inited = true;
    // a bit above the centre: the message box goes below the list
    s_list.SetRect(yw, -1, (yw->_screenSize.y - s_list.h) / 2 - yw->_fontH * 2);

    yw->GuiWinOpen(&s_list);
    s_open = true;
    s_inMission = inMission;
    s_atOpen = PadRemap::Active();
    s_capture = -1;
    s_msg.clear();
    s_msg2.clear();
    s_msgErr = false;
    NC_STACK_winp::SetPadPage(true);
    PadScript::PageOpened(); // dev aid UA_PAD_SCRIPT (inert unless set)
    ypa_log_out("pad buttons: page opened (%s)\n", inMission ? "in-mission menu" : "Options > Input");
    fprintf(stderr, "pad buttons: page opened (%s), controller %s\n", inMission ? "in-mission menu" : "Options > Input",
            NC_STACK_winp::PadConnected() ? "connected" : "not connected");
}

void Close(NC_STACK_ypaworld *yw)
{
    if ( !s_open )
        return;
    yw->GuiWinClose(&s_list);
    s_open = false;
    s_capture = -1;
    NC_STACK_winp::SetPadPage(false);
    if ( PadRemap::Active() != s_atOpen && yw->_GameShell && !yw->_GameShell->UserName.empty() )
    {
        const bool ok = yw_PatchInputProfile(yw->_GameShell->UserName, yw);
        ypa_log_out("pad buttons saved to profile %s: %s\n", yw->_GameShell->UserName.c_str(), ok ? "ok" : "FAILED (the next profile save writes them)");
        fprintf(stderr, "pad buttons saved to profile %s: %s\n", yw->_GameShell->UserName.c_str(), ok ? "ok" : "FAILED");
    }
    fprintf(stderr, "pad buttons: page closed\n");
}

static void Select(int row)
{
    const int n = RowCount();
    if ( row < 0 )
        row = n - 1;
    if ( row >= n )
        row = 0;
    s_list.selectedEntry = row;
    if ( row < s_list.firstShownEntries )
        s_list.firstShownEntries = row;
    if ( row >= s_list.firstShownEntries + s_list.shownEntries )
        s_list.firstShownEntries = row - s_list.shownEntries + 1;
}

static void Choose(NC_STACK_ypaworld *yw, int row)
{
    if ( yw->_GameShell )
        SFXEngine::SFXe.startSound(&yw->_GameShell->samples1_info, World::SOUND_ID_BUTTON);
    if ( row == RowBack() )
    {
        Close(yw);
        return;
    }
    if ( row == RowReset() )
    {
        PadRemap::Active().Reset();
        SetMsg(Locale::Text::Advanced(Locale::ADV_PADRESET) + ": done");
        return;
    }
    if ( row >= 0 && row < PadRemap::ActionCount() )
    {
        s_capture = row;
        s_waitRelease = true;
        s_captureT0 = SDL_GetTicks();
        NC_STACK_winp::TakePadPresses(); // the press that chose the row does not count
        fprintf(stderr, "pad buttons: waiting for a button for %s\n", PadRemap::GetAction(row).key);
    }
}

static void Captured(NC_STACK_ypaworld *yw, int btn)
{
    const int a = s_capture;
    s_capture = -1;
    const PadRemap::BindResult r = PadRemap::Bind(PadRemap::Active(), a, btn);
    std::string m = ActionTitle(yw, a) + ": " + Label(btn), m2;
    if ( r.other >= 0 )
        m2 = "Swapped: " + ActionTitle(yw, r.other) + ": " + (r.otherNow >= 0 ? Label(r.otherNow) : std::string("-"));
    else if ( !r.changed )
        m += " (unchanged)";
    SetMsg(m, false, m2);
    fprintf(stderr, "pad buttons: bound %s = %s%s%s%s\n", PadRemap::GetAction(a).key, PadRemap::ButtonName(btn),
            r.other >= 0 ? ", swapped " : "", r.other >= 0 ? PadRemap::GetAction(r.other).key : "",
            r.other >= 0 ? (std::string(" = ") + PadRemap::ButtonName(r.otherNow)).c_str() : "");
}

static void ClearInput(TInputState *inpt)
{
    inpt->KbdLastHit = Input::KC_NONE;
    inpt->KbdLastDown = Input::KC_NONE;
    inpt->HotKeyID = -1;
    inpt->chr = 0;
    inpt->Buttons = Common::BitMan<32>();
    inpt->Sliders.fill(0.0);
    const Common::Point pos = inpt->ClickInf.move.ScreenPos;
    inpt->ClickInf = TClickBoxInf();
    inpt->ClickInf.move.ScreenPos = pos;
}

void Handle(NC_STACK_ypaworld *yw, TInputState *inpt)
{
    if ( !s_open )
        return;

    const uint32_t presses = NC_STACK_winp::TakePadPresses();
    const int key = inpt->KbdLastHit;

    if ( s_capture >= 0 )
    {
        if ( key == Input::KC_ESCAPE )
        {
            s_capture = -1;
            SetMsg("cancelled (Esc), unchanged");
        }
        else if ( SDL_GetTicks() - s_captureT0 >= kCaptureMs )
        {
            s_capture = -1;
            SetMsg("no button within 5 s, unchanged");
        }
        else if ( s_waitRelease )
        {
            if ( !(NC_STACK_winp::PadHeld() & ((1u << PadRemap::BTN_COUNT) - 1)) )
                s_waitRelease = false;
        }
        else
        {
            for (int b = 0; b < PadRemap::BTN_COUNT; b++)
            {
                if ( presses & (1u << b) )
                {
                    Captured(yw, b);
                    break;
                }
            }
        }
    }
    else
    {
        // controller: D-pad / left stick move, A choose, B or Start back
        if ( presses & ((1u << PadRemap::BTN_DPUP) | (1u << NC_STACK_winp::PAD_STICK_UP)) )
            Select(s_list.selectedEntry - 1);
        else if ( presses & ((1u << PadRemap::BTN_DPDOWN) | (1u << NC_STACK_winp::PAD_STICK_DOWN)) )
            Select(s_list.selectedEntry + 1);

        if ( presses & (1u << PadRemap::BTN_A) )
            Choose(yw, s_list.selectedEntry);
        else if ( presses & ((1u << PadRemap::BTN_B) | (1u << PadRemap::BTN_START)) )
            Close(yw);
        // keyboard: arrows (the list does them), Enter choose, Esc back, Backspace/Delete clear
        else if ( key == Input::KC_RETURN || key == Input::KC_NUMENTER || key == Input::KC_SPACE )
            Choose(yw, s_list.selectedEntry);
        else if ( key == Input::KC_ESCAPE )
            Close(yw);
        else if ( (key == Input::KC_BACKSPACE || key == Input::KC_DELETE) && s_list.selectedEntry < PadRemap::ActionCount() )
        {
            const int a = s_list.selectedEntry;
            if ( PadRemap::Bind(PadRemap::Active(), a, PadRemap::BTN_NONE).changed )
                SetMsg(ActionTitle(yw, a) + ": -");
        }

        if ( s_open )
        {
            // mouse: click a row to choose it, the close box = back
            TClickBoxInf &ci = inpt->ClickInf;
            if ( ci.selected_btn == &s_list && ci.selected_btnID == 0 && (ci.flag & TClickBoxInf::FLAG_BTN_UP) )
                Close(yw);
            else
            {
                s_list.InputHandle(yw, inpt);
                if ( s_list.listFlags & GuiList::GLIST_FLAG_SEL_DONE )
                    Choose(yw, s_list.selectedEntry);
            }
        }
    }

    if ( s_open )
        s_list.Formate(yw);
    ClearInput(inpt);
}

// one 1998 list row: left column (+ right column if given), highlighted or not
static void Row(NC_STACK_ypaworld *yw, CmdStream *c, int width, const std::string &left, const std::string &right, bool sel, bool err = false)
{
    FontUA::ColumnItem col[2];
    const int w = width - 2 * yw->_fontBorderW + 1;
    const int lw = right.empty() ? w : (int)(w * 0.62);
    const uint8_t pre = sel ? 98 : 102, post = sel ? 100 : 102, font = sel ? 9 : 0, space = sel ? 99 : 102;

    col[0].txt = left;
    col[0].width = lw;
    col[0].fontID = font;
    col[0].spaceChar = space;
    col[0].prefixChar = pre;
    col[0].postfixChar = right.empty() ? post : 0;
    col[0].flags = 37;

    col[1].txt = right;
    col[1].width = w - lw;
    col[1].fontID = font;
    col[1].spaceChar = space;
    col[1].prefixChar = 0;
    col[1].postfixChar = post;
    col[1].flags = 38;

    FontUA::select_tileset(c, 0);
    FontUA::store_s8(c, '{');
    const SDL_Color &clr = err ? yw->_iniColors[63] : (sel ? yw->_iniColors[62] : yw->_iniColors[61]);
    FontUA::set_txtColor(c, clr.r, clr.g, clr.b);
    FontUA::FormateColumnItem(yw, c, right.empty() ? 1 : 2, col);
    FontUA::select_tileset(c, 0);
    FontUA::store_s8(c, '}');
    FontUA::next_line(c);
}

void Draw(NC_STACK_ypaworld *yw)
{
    if ( !s_open )
        return;

    // the list (same layout as the Input page's key list)
    s_list.itemBlock.clear();
    s_list.ItemsPreLayout(yw, &s_list.itemBlock, 0, "uvw");
    const bool blink = (SDL_GetTicks() / 400) & 1;
    for (int i = 0; i < s_list.shownEntries; i++)
    {
        const int row = s_list.firstShownEntries + i;
        if ( row >= RowCount() )
            break;
        const bool sel = row == s_list.selectedEntry;
        if ( row == RowReset() )
            Row(yw, &s_list.itemBlock, s_list.entryWidth, Locale::Text::Advanced(Locale::ADV_PADRESET), std::string(), sel);
        else if ( row == RowBack() )
            Row(yw, &s_list.itemBlock, s_list.entryWidth, Locale::Text::Advanced(Locale::ADV_BACK), std::string(), sel);
        else
        {
            std::string btn = (row == s_capture) ? (blink ? Locale::Text::Dialogs(Locale::DLG_I_UNK) : std::string(" "))
                                                 : Label(PadRemap::Active().btn[row]);
            if ( row != s_capture && PadRemap::GetAction(row).kind == PadRemap::KIND_ESC && PadRemap::Active().btn[row] != PadRemap::BTN_START )
                btn += " + " + Label(PadRemap::BTN_START); // Start always opens the menu
            Row(yw, &s_list.itemBlock, s_list.entryWidth, ActionTitle(yw, row), btn, sel);
        }
    }
    s_list.ItemsPostLayout(yw, &s_list.itemBlock, 0, "xyz");
    FontUA::set_end(&s_list.itemBlock);
    GFX::Engine.ProcessDrawSeq(s_list.cmdCommands, &s_list.cmdInclude);

    // the message box under it: prompt / last change / how to use the page
    std::string l1, l2;
    if ( s_capture >= 0 )
    {
        l1 = Locale::Text::Advanced(Locale::ADV_PADPRESS);
        const size_t p = l1.find("%s");
        if ( p != std::string::npos )
            l1.replace(p, 2, ActionTitle(yw, s_capture));
        const uint32_t el = SDL_GetTicks() - s_captureT0;
        l2 = fmt::sprintf("%d s", (int)((kCaptureMs - (el < kCaptureMs ? el : kCaptureMs) + 999) / 1000));
    }
    else
    {
        l1 = s_msg.empty() ? Locale::Text::Advanced(Locale::ADV_PADHINT) : s_msg;
        if ( !NC_STACK_winp::PadConnected() )
            l2 = Locale::Text::Advanced(Locale::ADV_PADNOPAD);
        else if ( !s_msg2.empty() )
            l2 = s_msg2;
        else
            l2 = s_msg.empty() ? std::string(" ") : Locale::Text::Advanced(Locale::ADV_PADHINT);
    }
    CmdStream box;
    box.reserve(1024);
    const int bw = s_list.w;
    FontUA::reset_tileset(&box, 0);
    FontUA::set_center_xpos(&box, s_list.x - yw->_screenSize.x / 2);
    FontUA::set_center_ypos(&box, s_list.y + s_list.h + yw->_fontH / 2 - yw->_screenSize.y / 2);
    FontUA::set_yheight(&box, s_list.upperVborder);
    FontUA::store_s8(&box, 'u');
    FontUA::op17(&box, bw - yw->_fontBorderW);
    FontUA::store_s8(&box, 'v');
    FontUA::store_s8(&box, 'w');
    FontUA::next_line(&box);
    Row(yw, &box, bw, l1, std::string(), false, s_msgErr);
    Row(yw, &box, bw, l2, std::string(), false);
    FontUA::reset_tileset(&box, 0);
    FontUA::set_yoff(&box, yw->_guiTiles[0]->h - s_list.lowerVborder);
    FontUA::store_s8(&box, 'x');
    FontUA::op17(&box, bw - yw->_fontBorderW);
    FontUA::store_s8(&box, 'y');
    FontUA::store_s8(&box, 'z');
    FontUA::set_end(&box);
    GFX::Engine.ProcessDrawSeq(box);
}

}
