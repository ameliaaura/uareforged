#define GLOBAL
#ifdef UA_VR_ENABLED
#include "vr/vr.h"
#endif
#include "system/fsmgr.h"
#include "reforge.h"
#include "system/system.h"
#include "env.h"
#include "system/perf.h"

#include "includes.h"
#include "system/gfx.h"
#include "system/inpt.h"
#include "winp.h"
#include "wintimer.h"

#include "ade.h"
#include "area.h"
#include "amesh.h"
#include "bitmap.h"
#include "bmpAnm.h"
#include "base.h"
#include "ilbm.h"
#include "particle.h"
#include "embed.h"
#include "network.h"
#include "windp.h"
#include "ypabact.h"
#include "ypatank.h"
#include "ypacar.h"
#include "ypaflyer.h"
#include "yparobo.h"
#include "ypaufo.h"
#include "3ds.h"
#include "image.h"


#include "font.h"
#include "yw.h"

#include "button.h"



#include "gui/widget.h"
#include "gui/uawidgets.h"
#include "gui/uamsgbox.h"
#include "gui/uaempty.h"
#include "system/movie.h"
#include "system/inivals.h"
#include "obj3d.h"
#include "aituning.h"
#include "gfxsettings.h"


int dword_513638 = 0;
int dword_51362C = 0;
int dword_513630 = 0;
std::string buildDate;

int tform_inited = 0;
int audio_inited = 0;
int input_inited = 0;

enum GAME_SCREEN_MODE {
    GAME_SCREEN_MODE_UNKNOWN = 0,
    GAME_SCREEN_MODE_MENU = 1,
    GAME_SCREEN_MODE_GAME = 2,
    GAME_SCREEN_MODE_REPLAY = 3
};

GAME_SCREEN_MODE GameScreenMode;
UserData userdata;

// Dev aid: UA_FF_SELFTEST=1 walks the legacy force-feedback effects in order
// once a level is running. The engine only fires these while the player is
// driving a vehicle (the host station robo has no engine effect at all), so
// without this the pad mapping could not be exercised — or felt — without a
// full hands-on session. Inert unless the variable is set.
static void FFSelfTest()
{
    static int enabled = -1;

    if ( enabled < 0 )
    {
        const char *v = getenv("UA_FF_SELFTEST");
        enabled = ( v && v[0] && strcmp(v, "0") != 0 ) ? 1 : 0;
    }

    if ( enabled <= 0 )
        return;

    struct Step { uint8_t state; uint8_t eff; float p1, p2; uint32_t holdMs; };

    static const Step script[] = {
        { Input::FF_STATE_START,  Input::FF_TYPE_TANKENGINE, 0.40, 0.00, 1200 }, // idle tank
        { Input::FF_STATE_UPDATE, Input::FF_TYPE_TANKENGINE, 0.40, 1.00, 1200 }, // tank at speed
        { Input::FF_STATE_STOP,   Input::FF_TYPE_TANKENGINE, 0.00, 0.00,  600 },
        { Input::FF_STATE_START,  Input::FF_TYPE_JETENGINE,  0.75, 1.00, 1200 },
        { Input::FF_STATE_STOP,   Input::FF_TYPE_JETENGINE,  0.00, 0.00,  600 },
        { Input::FF_STATE_START,  Input::FF_TYPE_HELIENGINE, 0.50, 0.50, 1200 },
        { Input::FF_STATE_STOP,   Input::FF_TYPE_HELIENGINE, 0.00, 0.00,  600 },
        { Input::FF_STATE_START,  Input::FF_TYPE_MINIGUN,    0.00, 0.00,  900 },
        { Input::FF_STATE_STOP,   Input::FF_TYPE_MINIGUN,    0.00, 0.00,  600 },
        { Input::FF_STATE_START,  Input::FF_TYPE_MISSILEFIRE,0.00, 0.00,  700 },
        { Input::FF_STATE_START,  Input::FF_TYPE_GRENADEFIRE,0.00, 0.00,  700 },
        { Input::FF_STATE_START,  Input::FF_TYPE_BOMBFIRE,   0.00, 0.00,  700 },
        { Input::FF_STATE_START,  Input::FF_TYPE_COLLISION,  0.80, 0.00,  700 },
        { Input::FF_STATE_START,  Input::FF_TYPE_SHAKE,      0.90, 400.0, 900 },
        { Input::FF_STATE_STOP,   Input::FF_TYPE_ALL,        0.00, 0.00,    0 },
    };

    static size_t step = 0;
    static uint32_t nextAt = 0;

    if ( step >= sizeof(script)/sizeof(script[0]) )
        return;

    uint32_t now = SDL_GetTicks();

    if ( nextAt && now < nextAt )
        return;

    const Step &s = script[step];

    // FF_TYPE_SHAKE takes (power, duration, dirX, dirY); everything else
    // takes at most (magnitude, period) — p2 carries whichever it needs.
    Input::Engine.ForceFeedback(s.state, s.eff, s.p1, s.p2);

    nextAt = now + s.holdMs;
    step++;
}

// ---------------------------------------------------------------------------
// Dev aid (inert unless the env vars are set): drive a real, in-process level
// SEQUENCE so a render bug that only shows on the SECOND map can be reproduced
// unattended.
//
//   UA_AUTOSTART_LEVEL=1,2      play level 1, then level 2, in ONE process
//   UA_AUTOSTART_SECONDS=25     end the running level after 25 s of gameplay
//
// The timeout takes the engine's OWN level-end path (STATE_COMPLETED when the
// level has a gate, else STATE_ABORTED), so the between-mission movie, the
// debrief and the shell all run exactly as they do for a human player. With
// only one id in the list this behaves as it always has.
// ---------------------------------------------------------------------------
namespace {

std::vector<int> AutostartLevels()
{
    static std::vector<int> ids;
    static bool parsed = false;

    if ( parsed )
        return ids;

    parsed = true;

    const char *lvl = getenv("UA_AUTOSTART_LEVEL");
    if ( !lvl )
    {
        // First-launch graphics benchmark (gfxsettings.cpp): one silent level run.
        if ( GfxSettings::BenchLevelId() > 0 )
            ids.push_back( GfxSettings::BenchLevelId() );
        return ids;
    }

    std::string v(lvl);
    size_t at = 0;
    while ( at <= v.size() )
    {
        size_t comma = v.find(',', at);
        std::string tok = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        int id = atoi(tok.c_str());
        if ( id > 0 && id < 256 )
            ids.push_back(id);
        if ( comma == std::string::npos )
            break;
        at = comma + 1;
    }

    return ids;
}

size_t autostartNext = 0;
int autostartWait = 90;

void AutostartLevelTimeout()
{
    static int limitMs = -1;
    if ( limitMs < 0 )
    {
        const char *v = getenv("UA_AUTOSTART_SECONDS");
        limitMs = (v && v[0]) ? atoi(v) * 1000 : 0;
    }

    // The graphics benchmark level ends itself; never any later level.
    int limit = limitMs;
    if ( limit <= 0 && GfxSettings::BenchRunning() )
        limit = GfxSettings::BenchSeconds() * 1000;

    if ( limit <= 0 )
        return;

    TLevelInfo &li = ypaworld->GetLevelInfo();

    if ( li.State != TLevelInfo::STATE_PLAYING )
        return;

    static uint32_t startedAt = 0;
    static int lastLevel = -1;

    if ( lastLevel != li.LevelID )
    {
        lastLevel = li.LevelID;
        startedAt = SDL_GetTicks();
        return;
    }

    if ( (int)(SDL_GetTicks() - startedAt) < limit )
        return;

    // The benchmark never completes a level (no campaign progress from it).
    if ( !li.Gates.empty() && !GfxSettings::BenchRunning() )
    {
        li.GateCompleteID = 0;
        li.State = TLevelInfo::STATE_COMPLETED;
        printf("[SEQ] level %d: forced COMPLETED after %d ms\n", li.LevelID, limit);
    }
    else
    {
        li.State = TLevelInfo::STATE_ABORTED;
        printf("[SEQ] level %d: forced ABORTED after %d ms (no gates)\n", li.LevelID, limit);
    }
    fflush(stdout);

    lastLevel = -1;
}

}

// Graphics benchmark bracket: sound and frame cap off while it runs.
void BenchBracket(bool on)
{
    static bool active = false;
    if ( on == active )
        return;
    active = on;
    GFX::Engine.BenchUncap(on);
    SFXEngine::SFXe.setMasterVolume(on ? 0 : userdata.soundVolume);
    SFXEngine::SFXe.SetMusicVolume(on ? 0 : userdata.musicVolume);
}

int ProcessGameplayFrame()
{
    FFSelfTest();

    if ( GfxSettings::BenchRunning() )
    {
        static uint64_t last = 0;
        uint64_t now = SDL_GetPerformanceCounter();
        BenchBracket(true);
        if ( last )
            GfxSettings::BenchFrame( (now - last) * 1000.0 / SDL_GetPerformanceFrequency() );
        last = now;
    }

    {
        Perf::Scope perfProc(Perf::PROC);
        ypaworld->Process(&world_update_arg);
    }

#ifdef UA_VR_ENABLED
    VR::GameplayFrameHook(ypaworld); // dev aid, inert unless UA_VR_TAKE_TANK is set
#endif

    // After Process(): a gameplay frame resets the level state, so a forced
    // end set before it would be swallowed.
    AutostartLevelTimeout();

    const TLevelInfo &levelInfo = ypaworld->GetLevelInfo();

    switch( levelInfo.State )
    {
    case TLevelInfo::STATE_COMPLETED:
    case TLevelInfo::STATE_ABORTED:
    {
        if ( GfxSettings::BenchRunning() )
        {
            GfxSettings::BenchLevelEnded();
            BenchBracket(false);
        }

        ypaworld->DeleteLevel();

        if ( dword_513638 || levelInfo.State == TLevelInfo::STATE_ABORTED )
        {
            if ( !ypaworld->LoadSettings("settings.tmp", 
                                         userdata.UserName,
                                         (World::SDF_BUDDY | World::SDF_PROTO | World::SDF_USER),
                                         false))
                return 0;

            dword_513638 = 0;
        }

        int v0 = 1;

        if ( userdata.remoteMode )
            v0 = 0;

        if ( userdata.EnvMode == ENVMODE_NETPLAY )
        {
            userdata.returnToTitle = true;
            userdata.EnvMode = ENVMODE_SINGLEPLAY;
        }
        else
        {
            userdata.returnToTitle = false;
        }

        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;
        world_update_arg.TimeStamp = 0;

        // Dev aid: re-arm UA_AUTOSTART_LEVEL for the next id in the list.
        autostartWait = 90;

        userdata.lastInputEvent = 0;
        userdata.GameIsOver = true;

        if ( ypaworld->OpenGameShell() )
        {
            GameScreenMode = GAME_SCREEN_MODE_MENU;
            Input::Engine.QueryInput(&input_states);

            // menu fix: a level started
            // straight from the title screen (UA_AUTOSTART_LEVEL, which the Quest
            // build always uses) comes back here with EnvMode still TITLE and
            // the level state still COMPLETED / ABORTED. The title buttons are
            // only enabled in STATE_MENU (GameShellUiHandleInput), so the main
            // menu showed no options at all. The normal path (Single-Player ->
            // map) never returns to the title with a level state.
            // It is also the flat "captions gone after a level" bug (same cause), but
            // the flat game is kept exactly as released: VR builds only for now.
#ifdef UA_VR_ENABLED
            if ( userdata.EnvMode == ENVMODE_TITLE )
            {
                ypaworld->_levelInfo.State = TLevelInfo::STATE_MENU;
                printf("UA: back to the title after a level: state -> MENU (title buttons enabled)\n");
            }
#endif

            if (!v0)
                return 0;
        }
        else
        {
            ypa_log_out("GameShell-Error!!!\n");
            ypaworld->DeinitGameShell();
            return 0;
        }
    }
    break;

    case TLevelInfo::STATE_RESTART:
    {
        ypaworld->DeleteLevel();

        if ( !ypaworld->LoadGame( fmt::sprintf("save:%s/%d.rst", userdata.UserName, levelInfo.LevelID) ) )
            ypa_log_out("Warning, load error\n");

        Input::Engine.QueryInput(&input_states);
    }
    break;

    case TLevelInfo::STATE_SAVE:
    {
        if ( !ypaworld->SaveGame( fmt::sprintf("save:%s/%d.sgm", userdata.UserName, 0) ) )
            ypa_log_out("Warning, Save error\n");

        Input::Engine.QueryInput(&input_states);
    }
    break;

    case TLevelInfo::STATE_LOAD:
    {
        ypaworld->DeleteLevel();

        if ( !ypaworld->LoadGame( fmt::sprintf("save:%s/%d.sgm", userdata.UserName, 0) ) )
            ypa_log_out("Warning, load error\n");

        Input::Engine.QueryInput(&input_states);
    }
    break;

    default:
        break;
    }

    return 1;
}

std::string sub_4107A0(uint32_t a1)
{
    if ( userdata.snaps.empty() )
        return std::string();

    int v2 = a1 % userdata.snaps.size();

    if ( dword_51362C == v2 )
    {
        if ( v2 )
        {
            v2 = v2 - 1;
        }
        else if ( (userdata.snaps.size() - 1) > 0 )
        {
            v2 = 1;
        }
    }

    dword_51362C = v2;
    return userdata.snaps[v2];
}

int sb_0x411324__sub2__sub0(base_64arg *arg)
{
    if ( (userdata.p_YW->_replayPlayer->field_74 - 3) <= userdata.p_YW->_replayPlayer->frame_id  &&  dword_513630 )
    {
        ypaworld->ypaworld_func164();

        if ( !ypaworld->ypaworld_func162( sub_4107A0(arg->TimeStamp) ) )
        {
            GameScreenMode = GAME_SCREEN_MODE_MENU;

            Input::Engine.QueryInput(&input_states);

            return 0;
        }

        dword_513630 = 1;
        GameScreenMode = GAME_SCREEN_MODE_REPLAY;
    }

    ypaworld->ypaworld_func163(arg);

    yw_arg165 arg165;
    arg165.field_0 = 0;
    arg165.frame = 0;

    int cont_play = 1;

    if ( arg->field_8->KbdLastHit == Input::KC_N )
    {
        arg165.field_0 = 4;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_P )
    {
        arg165.field_0 = 2;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_R )
    {
        arg165.field_0 = 3;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_S )
    {
        arg165.field_0 = 1;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_V )
    {
        arg165.frame = -10;
        arg165.field_0 = 7;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_B )
    {
        arg165.field_0 = 5;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_M )
    {
        arg165.frame = 10;
        arg165.field_0 = 7;
    }
    else if ( arg->field_8->KbdLastHit == Input::KC_SPACE || arg->field_8->KbdLastHit == Input::KC_ESCAPE )
    {
        cont_play = 0;
    }

    if ( arg165.field_0 )
        ypaworld->ypaworld_func165(&arg165);

    return cont_play;
}

int ProcessReplayFrame()
{
    if ( !sb_0x411324__sub2__sub0(&world_update_arg) )
    {
        ypaworld->ypaworld_func164();

        if ( dword_513638 )
        {
            if ( !ypaworld->LoadSettings("settings.tmp",
                                         userdata.UserName,
                                         (World::SDF_BUDDY | World::SDF_PROTO | World::SDF_USER),
                                         false) )
                return 0;

            dword_513638 = 0;
        }

        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;
        world_update_arg.TimeStamp = 0;
        userdata.lastInputEvent = 0;

        if ( !ypaworld->OpenGameShell() )
        {
            ypa_log_out("GameShell-Error!!!\n");
            ypaworld->DeinitGameShell();

            return 0;
        }

        userdata.p_YW->_levelInfo.State = TLevelInfo::STATE_MENU;

        GameScreenMode = GAME_SCREEN_MODE_MENU;

        input_states = TInputState();

        Input::Engine.QueryInput(&input_states);
    }
    return 1;
}

int ProcessMenuFrame()
{
    // Reforge tool modes (UA_EXPORT_VISPROTO / UA_VIEWER): run once the shell
    // has settled, then quit. Inert unless one of those env vars is set.
    if ( Reforge::MenuFrameHook() )
        return 0;

    userdata.GlobalTime = world_update_arg.TimeStamp;
    userdata.DTime = world_update_arg.DTime;
    userdata.Input = &input_states;

    ypaworld->ProcessGameShell();

    // Dev aid: UA_AUTOSTART_LEVEL=<id> starts that level from the menu with no
    // input, after a short settle delay. Synthetic input never reaches the SDL
    // loop on macOS, so this is the only automated way into a level (used for
    // headless render verification).
    // Dev aid: UA_SHOW_INPUT=1 opens Options -> Input with no input, so a
    // hidden run plus UA_SHOT_EVERY can screenshot the page. Synthetic clicks
    // never reach the SDL loop on macOS, so this is the only way to check a
    // menu layout unattended. Inert unless the variable is set.
    static int showInputWait = 200;
    if ( showInputWait > 0 )
    {
        const char *si = getenv("UA_SHOW_INPUT");
        if ( si && si[0] && si[0] != '0' )
        {
            showInputWait--;
            if ( showInputWait == 0 )
                userdata.ShowInputSettings();
        }
        else
            showInputWait = 0;
    }

    // Dev aid (UI graphics): UA_SHOW_BRIEFING=<level id> opens the single-player
    // campaign map after 200 menu frames and the briefing of that level 150
    // frames later, so a hidden run plus UA_SHOT_EVERY can screenshot the
    // mission map and the briefing. Inert unless the variable is set.
    {
        static int briefWait = -1;
        static int briefLvl = 0;
        if ( briefWait < 0 )
        {
            briefWait = 0;
            const char *sb = getenv("UA_SHOW_BRIEFING");
            if ( sb && atoi(sb) > 0 )
            {
                briefLvl = atoi(sb);
                briefWait = 350;
            }
        }
        if ( briefWait > 0 )
        {
            briefWait--;
            if ( briefWait == 150 && userdata.EnvMode == ENVMODE_TITLE )
            {
                printf("[SEQ] show campaign map\n");
                fflush(stdout);
                userdata.sub_bar_button->ShowScreen();
                userdata.titel_button->HideScreen();
                userdata.EnvMode = ENVMODE_SINGLEPLAY;
                // Load the region pictures like the menu does. EnvModeChanged alone is not
                // enough: ProcessGameShell clears it at the start of the next frame, before
                // GameShellBkgProcess reads it, so the campaign map never loaded (black frames).
                userdata.DevEnvModePending = true;
            }
            else if ( briefWait == 0 && userdata.EnvMode == ENVMODE_SINGLEPLAY )
            {
                printf("[SEQ] show briefing %d\n", briefLvl);
                fflush(stdout);
                ypaworld->InitBriefing(briefLvl);
            }
        }
    }

    // Dev aid (graphics settings): UA_SHOW_OPTIONS=1 opens Options after 200
    // menu frames (menu screenshots); =<code>,<code>,... then plays Graphics-row
    // clicks (1142 preset, 1143 feature, 1144 level, 1145 Original, 1146 Skies) one per 30
    // frames, 1124 = OK, 1125 = Cancel. UA_SHOW_OPTIONS_AFTER=N waits until N
    // autostart levels were played (runtime-switch tests); autostart waits for
    // the clicks. Inert unless set.
    static bool optPending = false;
    static size_t optAfter = 0;
    {
        static int optWait = -1;
        static std::vector<int> optCodes;
        static size_t optNext = 0;
        static bool opened = false;
        if ( optWait < 0 )
        {
            optWait = 0;
            const char *so = getenv("UA_SHOW_OPTIONS");
            if ( so && so[0] && so[0] != '0' )
            {
                optWait = 200;
                optPending = true;
                if ( const char *oa = getenv("UA_SHOW_OPTIONS_AFTER") )
                    optAfter = atoi(oa);
                for (const char *c = so; *c; )
                {
                    int v = atoi(c);
                    if ( v >= 1100 )
                        optCodes.push_back(v);
                    const char *n = strchr(c, ',');
                    if ( !n )
                        break;
                    c = n + 1;
                }
            }
        }
        if ( optPending && autostartNext >= optAfter && optWait > 0 && --optWait == 0 )
        {
            if ( !opened && userdata.EnvMode != ENVMODE_SETTINGS )
                userdata.ShowOptionsMenu();
            opened = true;
            if ( optNext < optCodes.size() )
            {
                int code = optCodes[optNext++];
                printf("[SEQ] options code %d\n", code);
                fflush(stdout);
                if ( code >= 1142 && code <= 1146 )
                    userdata.GfxMenuCode(code);
                else if ( code == 1124 )
                    userdata.sb_0x46aa8c();
                else if ( code == 1125 )
                    userdata.sub_46A3C0();
                optWait = 30;
            }
            else
                optPending = false;
        }
    }

    if ( autostartWait > 0 && userdata.envAction.action == EnvAction::ACTION_NONE && !(optPending && autostartNext >= optAfter) )
    {
        autostartWait--;
        if ( autostartWait == 0 )
        {
            const std::vector<int> ids = AutostartLevels();

            if ( autostartNext < ids.size() )
            {
                int id = ids[autostartNext++];
                printf("[SEQ] autostart level %d (%d of %d)\n", id, (int)autostartNext, (int)ids.size());
                fflush(stdout);
                userdata.envAction.action = EnvAction::ACTION_PLAY;
                userdata.envAction.params[0] = id;
                userdata.envAction.params[1] = id;
            }
        }
    }

    // Dev aid: UA_MP_HOST / UA_MP_JOIN drive the multiplayer menu with no
    // input (auto-host a session / auto-connect to one). Inert unless the
    // env vars are set; state machine lives in UserData::MpAutoDrive().
    if ( userdata.envAction.action == EnvAction::ACTION_NONE )
        userdata.MpAutoDrive();

    if ( userdata.envAction.action == EnvAction::ACTION_QUIT )
        return 0;
    else if ( userdata.envAction.action == EnvAction::ACTION_PLAY )
    {
        userdata.SaveSettings();
        ypaworld->CloseGameShell();

        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;

        if ( !userdata.SaveBuildProtoState() )
            return 0;

        yw_arg161 v22;
        v22.lvlID = userdata.envAction.params[0];
        v22.field_4 = 0;

        if ( !ypaworld->ypaworld_func183(&v22) )
        {
            ypa_log_out("Sorry, unable to init this level!\n");

            ypaworld->DeinitGameShell();
            return 0;
        }
        GameScreenMode = GAME_SCREEN_MODE_GAME;
        Input::Engine.QueryInput(&input_states);
    }
    else if ( userdata.envAction.action == EnvAction::ACTION_LOAD )
    {
        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;

        const TLevelInfo &a4 = ypaworld->GetLevelInfo();

        userdata.SaveSettings();
        ypaworld->CloseGameShell();

        if ( !userdata.SaveBuildProtoState() )
            return 0;

        if ( !ypaworld->LoadGame( fmt::sprintf("save:%s/%d.sgm", userdata.UserName, 0) ) )
        {
            ypa_log_out("Error while loading level (level %d, User %s\n", a4.LevelID, userdata.UserName.c_str());

            ypaworld->DeinitGameShell();
            return 0;
        }
        GameScreenMode = GAME_SCREEN_MODE_GAME;
        Input::Engine.QueryInput(&input_states);
    }
    else if ( userdata.envAction.action == EnvAction::ACTION_NETPLAY )
    {
        userdata.SaveSettings();

        if ( !userdata.SaveBuildProtoState() )
            return 0;

        dword_513638 = 1;

        ypaworld->CloseGameShell();

        yw_arg161 v22;
        v22.lvlID = userdata.envAction.params[0];
        v22.field_4 = 0;

        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;

        if ( !ypaworld->ypaworld_func179(&v22) )
        {
            ypa_log_out("Sorry, unable to init this level for network!\n");
            ypaworld->DeinitGameShell();

            return 0;
        }

        GameScreenMode = GAME_SCREEN_MODE_GAME;
        Input::Engine.QueryInput(&input_states);
    }
    else if ( userdata.envAction.action == EnvAction::ACTION_REPLAY )
    {
        std::string repname = sub_4107A0(world_update_arg.TimeStamp);
        if ( repname.empty() )
        {
            userdata.lastInputEvent = world_update_arg.TimeStamp;
            return 1;
        }

        if ( !userdata.SaveBuildProtoState() )
            return 0;

        dword_513638 = 1;

        ypaworld->CloseGameShell();

        GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;

        if ( ypaworld->ypaworld_func162(repname) )
        {
            dword_513630 = 1;
            GameScreenMode = GAME_SCREEN_MODE_REPLAY;
        }
        else
        {
            ypa_log_out("Sorry, unable to init player!\n");
            world_update_arg.TimeStamp = 0;
            userdata.lastInputEvent = 0;

            if ( !ypaworld->OpenGameShell() )
            {
                ypa_log_out("GameShell-Error!!!\n");
                ypaworld->DeinitGameShell();
                return 0;
            }

            GameScreenMode = GAME_SCREEN_MODE_MENU;
        }

        Input::Engine.QueryInput(&input_states);
    }

    return 1;
}


int ProcessNextFrame()
{
#ifdef UA_VR_ENABLED
    VR::FrameStart(); // backend pacing + timing; no-op when VR is off
#endif

    input_states = TInputState();
    Input::Engine.QueryInput(&input_states);

#ifdef UA_VR_ENABLED
    const bool resetPeriod = userdata.ResetInputPeriod;
#endif
    if ( userdata.ResetInputPeriod )
    {
        input_states.Period = 1;
        userdata.ResetInputPeriod = false;
    }

    input_states.Period++;

#ifdef UA_VR_ENABLED
    {
        // OpenXR: game tick from the runtime's display clock (VR::TickMs); returns the
        // measured value unchanged when VR is off.
        uint32_t vrTick = VR::TickMs(input_states.Period);
        if ( !resetPeriod )
            input_states.Period = vrTick;
    }
#endif
    
    world_update_arg.DTime = input_states.Period;
    world_update_arg.field_8 = &input_states;

    world_update_arg.TimeStamp += input_states.Period;
    

    // If mouse captured, enable releative mouse control
    if (ypaworld->_mouseGrabbed)
        System::SetReleativeMouse(true);
    else
        System::SetReleativeMouse(false);
    
    Gui::Root::Instance.TimersUpdate( input_states.Period );

    if ( GameScreenMode == GAME_SCREEN_MODE_MENU )
    {
        return ProcessMenuFrame();
    }
    else if ( GameScreenMode == GAME_SCREEN_MODE_GAME )
    {
        return ProcessGameplayFrame();
    }
    else if ( GameScreenMode == GAME_SCREEN_MODE_REPLAY )
    {
        return ProcessReplayFrame();
    }
    return 1;
}

int init_classesLists_and_variables()
{
    ypa_log__ypa_general_log();
    init_d3dlog();
    init_dinputlog();

    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_nucleus>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_rsrc>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_bitmap>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_skeleton>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ilbm>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_sklt>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ade>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_area>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_base>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_bmpanim>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_amesh>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_particle>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_embed>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_idev>() );
    //Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_input>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_itimer>() );
    //Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_iwimp>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_sample>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_wav>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_button>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_network>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_winp>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_wintimer>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_windp>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypaworld>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypabact>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypatank>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_yparobo>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypamissile>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypaflyer>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypacar>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypaufo>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ypagun>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_3ds>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_image>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_Obj3D>() );

    return 1;
}

void deinit_globl_engines()
{
    if ( tform_inited )
        TF::Engine.Deinit();
    if ( input_inited )
        Input::Engine.Deinit();
    if ( audio_inited )
        SFXEngine::SFXe.deinit();

    ypa_log_out("Nucleus shutdown:\n");
}

int WinMain__sub0__sub0()
{
    ypaworld = 0;
    GameScreenMode = GAME_SCREEN_MODE_UNKNOWN;
    userdata.clear();
    input_states = TInputState();
    world_update_arg = base_64arg();

    if ( !init_classesLists_and_variables() )
    {
        sub_412038("Couldn't open Nucleus!");
        deinit_globl_engines();
        return 0;
    }

    Common::Env.AddGlobalIniKey("gfx.display  = win3d.class");
    Common::Env.AddGlobalIniKey("gfx.display2 = windd.class");
    Common::Env.AddGlobalIniKey("gfx.engine     = gfx.engine");
    Common::Env.AddGlobalIniKey("tform.engine   = tform_ng.engine");
    Common::Env.AddGlobalIniKey("input.engine   = input.engine");
    Common::Env.AddGlobalIniKey("input.wimp     = winp");
    Common::Env.AddGlobalIniKey("input.keyboard = winp");
    Common::Env.AddGlobalIniKey("input.slider[10] = winp:mousex");
    Common::Env.AddGlobalIniKey("input.slider[11] = winp:mousey");
    Common::Env.AddGlobalIniKey("input.slider[12] = winp:joyx winp:joyrudder");
    Common::Env.AddGlobalIniKey("input.slider[13] = winp:joyy");
    Common::Env.AddGlobalIniKey("input.slider[14] = winp:joythrottle");
    Common::Env.AddGlobalIniKey("input.slider[15] = winp:joyhatx");
    Common::Env.AddGlobalIniKey("input.slider[16] = winp:joyhaty");
    Common::Env.AddGlobalIniKey("input.button[16] = winp:joyb0");
    Common::Env.AddGlobalIniKey("input.button[17] = winp:joyb1");
    Common::Env.AddGlobalIniKey("input.button[18] = winp:joyb2");
    Common::Env.AddGlobalIniKey("input.button[19] = winp:joyb3");
    Common::Env.AddGlobalIniKey("input.button[20] = winp:joyb4");
    Common::Env.AddGlobalIniKey("input.button[21] = winp:joyb5");
    Common::Env.AddGlobalIniKey("input.button[22] = winp:joyb6");
    Common::Env.AddGlobalIniKey("input.button[23] = winp:joyb7");

    audio_inited = SFXEngine::SFXe.init();
    input_inited = Input::Engine.Init();
    tform_inited = TF::Engine.Init();

    if ( !audio_inited )
    {
        sub_412038("Couldn't open audio engine!");
        deinit_globl_engines();
        return 0;
    }

    if ( !tform_inited )
    {
        sub_412038("Couldn't open tform engine!");
        deinit_globl_engines();
        return 0;
    }
    if ( !input_inited )
    {
        sub_412038("Couldn't open input engine!");
        deinit_globl_engines();
        return 0;
    }

    return 1;
}

int yw_initGameWithSettings()
{
    FSMgr::FileHandle *user_def = uaOpenFileAlloc("env:user.def", "r");

    std::string settingsFileName;

    if ( user_def )
    {
        std::string line;
        user_def->ReadLine(&line);

        settingsFileName = fmt::sprintf("save:%s/user.txt", line);

        FSMgr::FileHandle *user_txt = uaOpenFileAlloc(settingsFileName, "r");

        if ( user_txt )
        {
            delete user_txt;

            userdata.UserName = line;
            settingsFileName = fmt::sprintf("%s/user.txt", line);
        }
        else
        {
            ypa_log_out("Warning: default user file doesn't exist (%s)\n", settingsFileName.c_str());
            settingsFileName = fmt::sprintf("sdu7/user.txt");
            userdata.UserName = "SDU7";
        }

        delete user_def;
    }
    else
    {
        settingsFileName = fmt::sprintf("sdu7/user.txt");
        userdata.UserName = "SDU7";
        ypa_log_out("Warning: No default user set\n");
    }

    userdata.diskListActiveElement = -1;

    int v8 = 1;
    for ( ProfileList::iterator it = userdata.profiles.begin(); it != userdata.profiles.end(); it++ )
    {
        if ( !StriCmp(it->name, userdata.UserName) )
        {
            userdata.diskListActiveElement = v8;
            break;
        }

        v8++;
    }

    return ypaworld->LoadSettings(settingsFileName,
                                  userdata.UserName,
                                  World::SDF_ALL,
                                  true, true) != 0;
}

void ReadSnapsDir()
{
    FSMgr::DirIter dir = uaOpenDir("env:snaps/");

    if ( dir )
    {
        FSMgr::iNode *entr;
        while ( dir.getNext(&entr) )
        {
            if ( entr->getType() == FSMgr::iNode::NTYPE_FILE && !StriCmp(entr->getName().substr(0, 4), "demo") )
                userdata.snaps.push_back( fmt::sprintf("env:snaps/%s", entr->getName()) );
        }
    }
}

void sub_4113E8()
{
    if ( ypaworld )
    {
        if ( GameScreenMode == GAME_SCREEN_MODE_GAME )
        {
            ypaworld->DeleteLevel();
            ypaworld->DeinitGameShell();
        }
        else if ( GameScreenMode == GAME_SCREEN_MODE_MENU )
        {
            userdata.SaveSettings();
            ypaworld->CloseGameShell();
            ypaworld->DeinitGameShell();
        }

        ypaworld->Delete();
    }

    deinit_globl_engines();
}

int WinMain__sub0__sub1()
{
    buildDate = "Jul 09 1998  23:52:47";
//    strcpy(buildDate, __DATE__);
//    strcat(buildDate, " ");
//    strcat(buildDate, __TIME__);

    ypaworld = Nucleus::CInit<NC_STACK_ypaworld>( { {NC_STACK_ypaworld::YW_ATT_BUILD_DATE, std::string(buildDate)} } );
    
    Gui::UA::yw = ypaworld;
    
    if ( !ypaworld )
    {
        ypa_log_out("Unable to init ypaworld.class\n");
        return 0;
    }

    if ( !ypaworld->InitGameShell(&userdata) )
    {
        ypa_log_out("Unable to init shell structure\n");
        return 0;
    }

    if ( !yw_initGameWithSettings() )
    {
        ypa_log_out("Unable to init game with default settings\n");
        return 0;
    }

    if ( !userdata.HasInited && !ypaworld->OpenGameShell())
    {
        ypa_log_out("Error: Unable to open Gameshell\n");
        return 0;
    }

    GameScreenMode = GAME_SCREEN_MODE_MENU;
    ReadSnapsDir();

    return 1;
}

void HandleMods()
{
    std::string modname;
    
    std::vector<std::string> &cmdl = System::GetCmdLineArray();
    int32_t i = System::FindCmdLineArg("-mod");
    if (i >= 0 && i + 1 < (int32_t)cmdl.size())
        modname = cmdl[i + 1];
    
    if (modname.empty())
        return;
    
    FSMgr::iNode *node = FSMgr::iDir::findNode("mods");
    
    if (!node || node->getType() != FSMgr::iNode::NTYPE_DIR)
        return;
    
    node->Detach();
    
    FSMgr::iNode *imod = ((FSMgr::iDir *)node)->getNode(modname);
    
    if (!imod || imod->getType() != FSMgr::iNode::NTYPE_DIR)
    {
        delete node;
        return;
    }
    
    imod->Detach();
    delete node;
    
    FSMgr::iDir *mod = dynamic_cast<FSMgr::iDir *>(imod);
    if (!mod)
    {
        delete imod;
        return;
    }

    // Detach original basepath "save" and "env" dirs
    FSMgr::iDir::GetRoot()->Detach("save");
    FSMgr::iDir::GetRoot()->Detach("env");
    
    // Make sure save and env created in mod dir and complete replace it
    FSMgr::iDir::GetRoot()->addNode( mod->MakeDir("save") );
    FSMgr::iDir::GetRoot()->addNode( mod->MakeDir("env") );
    
    FSMgr::iDir::GetRoot()->Override( mod );
    
    delete mod;
    
    World::AllowMods(true);
}

int WinMain__sub0()
{
    if ( WinMain__sub0__sub0() )
    {
        std::vector<std::string> &cmdl = System::GetCmdLineArray();
        int32_t i = System::FindCmdLineArg("-env");
        if (i >= 0 && i + 1 < (int32_t)cmdl.size())
            Common::Env.SetPrefix("env", cmdl[i + 1]);

        if ( WinMain__sub0__sub1() )
            return 1;
        deinit_globl_engines();
    }

    return 0;
}

uint32_t maxTicks = 1000/60; // init on 60FPS

#ifdef __ANDROID__
// Quest: called by the NativeActivity shell (platform/quest/.../ua_main_android.cpp)
// on the android_main thread, after EGL + OpenXR are up and the CWD is the data dir.
extern "C" int UA_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    // Line-buffer stdout/stderr so logs are visible when redirected to a file
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    for(int i = 0; i < argc; ++i)
        System::AddCmdLine( std::string(argv[i]) );

    System::IniConf::Init();

    // Commander tuning from ./ai-tuning.txt (the CWD is the game-data
    // dir). Absent file = compiled-in 1998 values, classic commander.
    AITune::Load();
    FSMgr::iDir::setBaseDir("");
    
    HandleMods();
    
    System::IniConf::ReadFromNucleusIni();
    bool gfxVbo = System::IniConf::GfxVBO.Get<bool>();
    
    if (System::IniConf::DevMode.Get<bool>() || System::FindCmdLineArg("-devmode") >= 0)
        World::AllowMods(true);        
    
    System::Init(!gfxVbo);
    
    GFX::Engine.Init();
#ifdef UA_VR_ENABLED
    VR::Init(); // UA_VR=sim selects the desktop simulator; unset = flat game
#endif
    System::Movie.Init();
    
    Gui::UA::Init();

    if ( !WinMain__sub0() )
        return 0;

    System::IniConf::ReadFromNucleusIni();

    uint32_t ticks = 0;
    
    Gui::Root::Instance.SetHwCompose(true);
    ypaworld->LoadGuiFonts();
    ypaworld->CreateNewGuiElements();
    
    
    //Gui::Root::Instance.AddPortal( Common::Point(640, 480), Common::Rect(0, 0, 300, 300));
    
    // New gui test windows
    /*Gui::UAWindow *smpl = new Gui::UAWindow("Test1", Common::PointRect(100, 100, 200, 300), 
        Gui::UAWindow::FLAG_WND_RESIZEABLE | 
        Gui::UAWindow::FLAG_WND_VSCROLL | 
        Gui::UAWindow::FLAG_WND_CLOSE );
    smpl->SetEnable(true);
    smpl->SetAlpha(190);

    Gui::Root::Instance.AddWidget(smpl);   
    
    Gui::UAWindow *smpl2 = new Gui::UAWindow("Test2", Common::PointRect(0, 0, 50, 60), 
        Gui::UAWindow::FLAG_WND_RESIZEABLE | 
        Gui::UAWindow::FLAG_WND_VSCROLL | 
        Gui::UAWindow::FLAG_WND_CLOSE |
        Gui::UAWindow::FLAG_WND_HELP | 
        Gui::UAWindow::FLAG_WND_MAXM |
        Gui::UAWindow::FLAG_WND_HSCROLL);
    smpl2->SetEnable(true);*/
    //smpl2->SetAlpha(190);
    
    //scl->MoveTo(100, 100);
    //scl->ResizeWH(300, 360);
    
    //Gui::Root::Instance.AddWidgetPortal(0, smpl2);  
    //Gui::Root::Instance.AddWidget(smpl2);  
    //smpl->AddChild(smpl2);
    
   
    

//    int fps = 0;
//    uint32_t fpstick = SDL_GetTicks() + 1000;

    while ( true )
    {

        if (GFX::Engine.FpsMaxTicks == 0)
        {
            if ( !ProcessNextFrame() )
                break;

            if ( System::ProcessEvents() )
                break;
        }
        else
        {
            uint32_t curTick = SDL_GetTicks();

            if (curTick >= ticks)
            {
                if ( !ProcessNextFrame() )
                    break;

                if ( System::ProcessEvents() )
                    break;

                ticks = curTick;

                uint32_t diffTick = SDL_GetTicks() - curTick;

                if (diffTick < GFX::Engine.FpsMaxTicks)
                {
                    uint16_t delay = GFX::Engine.FpsMaxTicks - diffTick;
                    ticks += delay;
                    SDL_Delay(delay);
                }
            }
            else
                SDL_Delay(1);
        }

//        fps++;
//        if (SDL_GetTicks() > fpstick)
//        {
//            printf("fps %d\n", fps);
//            fpstick = SDL_GetTicks() + 1000;
//            fps = 0;
//        }

    }
    
    ypaworld->DeleteNewGuiElements();
    
    sub_4113E8();
    Gui::UA::Deinit();
    
#ifdef UA_VR_ENABLED
    VR::Shutdown();
#endif
    System::Deinit();

    return 0;
}
