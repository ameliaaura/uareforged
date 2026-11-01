#ifndef ENGINE_MOVIE_H_INCLUDED
#define ENGINE_MOVIE_H_INCLUDED

#include <stdint.h>
#include <string>
#include <SDL2/SDL.h>

namespace System
{

// ---------------------------------------------------------------------------
// Movie scaling.
//
// The FMVs are 4:3 (320x240 originals, 1280x960 restored masters) and the
// screens they play on are not, so something has to give. The rule is
// contain-maximized: scale uniformly as large as the window allows with the
// whole frame still visible. Never cropped, never stretched out of aspect —
// where the aspects disagree the leftover shows as bars.
// ---------------------------------------------------------------------------

enum MovieScaleMode
{
    // Contain: grow until the frame touches the FIRST window edge. Whole frame
    // visible, bars in the leftover. The original 4:3 framing, nothing lost.
    MOVIE_SCALE_MAX  = 0,
    // Cover: grow until the frame passes the LAST window edge. No bars at all,
    // at the cost of the overflow being cropped off. 4:3 film on 16:9 gives
    // full width with equal slices taken off the top and bottom.
    MOVIE_SCALE_FILL = 1,
};

// Destination rectangle in window pixels; x/y is the top-left corner. Under MAX
// both are >= 0 and the rect stays inside the window; under FILL they go
// negative and the overhang is what gets cropped.
struct MovieRect
{
    int w;
    int h;
    int x;
    int y;
};

// Pure geometry: where a vidW x vidH video lands inside a winW x winH window.
//
// Both modes scale by a single factor, so the video is never stretched out of
// aspect; they differ only in which edge that factor is chosen against.
// MAX takes min(winW/vidW, winH/vidH) and stops at the first edge; FILL takes
// max(...) and carries on past the last one. Exactly one axis therefore ends up
// matching the window and the other follows the video's aspect, and the rect is
// centred so a FILL crop takes equal slices off both sides.
//
// Integer maths throughout, so the answer is exact and checkable at compile
// time — see the static_asserts below.
constexpr MovieRect MovieDestRect(MovieScaleMode mode, int winW, int winH, int vidW, int vidH)
{
    // Nothing sane to compute from; hand back the window itself.
    if (winW <= 0 || winH <= 0 || vidW <= 0 || vidH <= 0)
        return MovieRect{ winW, winH, 0, 0 };

    // Is the window the proportionally wider shape of the two? Cross-multiplied
    // so the compare stays in integers.
    bool windowWider = ((int64_t)winW * vidH >= (int64_t)winH * vidW);

    // For a window wider than the video, height is the first edge reached and
    // width the last — so the axis matched exactly is opposite between modes.
    bool matchWidth = (mode == MOVIE_SCALE_FILL) ? windowWider : !windowWider;

    // Height at full width, and width at full height.
    int64_t hNum = (int64_t)winW * vidH;    // / vidW
    int64_t wNum = (int64_t)winH * vidW;    // / vidH

    // MAX truncates, so the rect can never spill past the window and clip the
    // picture. FILL rounds up, so it can never fall a pixel short of covering
    // the window and leave a hairline bar.
    bool fill = (mode == MOVIE_SCALE_FILL);

    int destW = matchWidth ? winW
                           : (int)(fill ? ((wNum + vidH - 1) / vidH) : (wNum / vidH));

    int destH = matchWidth ? (int)(fill ? ((hNum + vidW - 1) / vidW) : (hNum / vidW))
                           : winH;

    return MovieRect{ destW, destH, (winW - destW) / 2, (winH - destH) / 2 };
}

// Compile-time spot checks. The restored HD-FMV masters are 1280x960; the
// window sizes are two real 16:9 / 16:10 test screens plus a 4:3 control.
static_assert(MovieDestRect(MOVIE_SCALE_MAX, 2560, 1440, 1280, 960).w == 1920 &&
              MovieDestRect(MOVIE_SCALE_MAX, 2560, 1440, 1280, 960).h == 1440 &&
              MovieDestRect(MOVIE_SCALE_MAX, 2560, 1440, 1280, 960).x == 320 &&
              MovieDestRect(MOVIE_SCALE_MAX, 2560, 1440, 1280, 960).y == 0,
              "max 2560x1440: full height, 320px pillars left and right");

static_assert(MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).w == 2978 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).h == 2234 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).x == 239 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).y == 0,
              "max 3456x2234: full height, 239px pillars left and right");

static_assert(MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1280, 960).w == 1600 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1280, 960).h == 1200 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1280, 960).x == 0 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1280, 960).y == 0,
              "max 1600x1200: 4:3 window, edge to edge, no bars at all");

// The vice versa: content wider than the window takes the full width and puts
// the bars top and bottom instead.
static_assert(MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1920, 1080).w == 1600 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1920, 1080).h == 900 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1920, 1080).x == 0 &&
              MovieDestRect(MOVIE_SCALE_MAX, 1600, 1200, 1920, 1080).y == 150,
              "max 16:9 film in a 4:3 window: full width, 150px bars top and bottom");

// Nothing is ever cropped: the rect stays inside the window on both axes.
static_assert(MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).x >= 0 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).y >= 0 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).w <= 3456 &&
              MovieDestRect(MOVIE_SCALE_MAX, 3456, 2234, 1280, 960).h <= 2234,
              "max never crops: the whole frame stays inside the window");

// --- fill ------------------------------------------------------------------
// Same 4:3 masters, same windows: full width, and the vertical overflow is
// cropped in equal slices (the negative y is half the overflow).
static_assert(MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).w == 2560 &&
              MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).h == 1920 &&
              MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).x == 0 &&
              MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).y == -240,
              "fill 2560x1440: full width, 240 rows cropped top and bottom");

// NB 3456x2234 scales by max(3456/1280, 2234/960) = 2.7, the WIDTH ratio — so
// the frame goes to 3456x2592, not the 2979x2234 that the height ratio (and
// MAX) would give.
static_assert(MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).w == 3456 &&
              MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).h == 2592 &&
              MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).x == 0 &&
              MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).y == -179,
              "fill 3456x2234: full width, 179 rows cropped top and bottom");

// The 320x240 originals crop exactly the same way — mode is independent of
// which master is playing.
static_assert(MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).w == 1920 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).h == 1440 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).x == 0 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).y == -180,
              "fill 1920x1080 of a 320x240 original: 180 rows cropped each side");

// A 4:3 window needs no crop at all, and content wider than the window crops
// left/right instead of top/bottom.
static_assert(MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1280, 960).w == 1600 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1280, 960).h == 1200 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1280, 960).y == 0,
              "fill 1600x1200: aspects agree, so fill and max are the same rect");

static_assert(MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).w == 2134 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).h == 1200 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).x == -267 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).y == 0,
              "fill 16:9 film in a 4:3 window: crops the sides, not the top");

// The defining invariant: fill always covers the window on BOTH axes.
static_assert(MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).w >= 2560 &&
              MovieDestRect(MOVIE_SCALE_FILL, 2560, 1440, 1280, 960).h >= 1440 &&
              MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).w >= 3456 &&
              MovieDestRect(MOVIE_SCALE_FILL, 3456, 2234, 1280, 960).h >= 2234 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).w >= 1920 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1920, 1080, 320, 240).h >= 1080 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).w >= 1600 &&
              MovieDestRect(MOVIE_SCALE_FILL, 1600, 1200, 1920, 1080).h >= 1200,
              "fill always covers the window, leaving no bar on either axis");

class TMovie
{
private:
struct Context;

public:
    ~TMovie();
    
    void Init();
    void PlayMovie(const std::string &fname, int volume = 127);

    // "Modernised Videos". The shell checkbox / the loaded profile calls this;
    // until it does, the nucleus.ini default (gfx.movie_hd) is used. UA_FMV=0
    // in the environment forces the redirect off whatever either of them says.
    static void SetHDEnabled(bool on);
    static bool HDEnabled();

    // Real, ready-to-open path for a movie reference. Returns the hd-fmv
    // replacement when one exists and the feature is on, else fname unchanged.
    // isHD (optional) reports which of the two it picked; is43 (optional)
    // reports that the uncropped 4:3 spare was the one chosen.
    static std::string ResolveMoviePath(const std::string &fname,
                                        bool *isHD = NULL, bool *is43 = NULL);

    // "Fullscreen Movies" — a FILE-VARIANT picker, not a scale mode.
    //
    // The installed hd-fmv/<stem>.mkv films are pre-cropped to a widescreen
    // frame, and the eight that lost picture to that crop ship an uncropped
    // 4:3 spare beside them as hd-fmv/<stem>-43.mkv. So:
    //   on  -> hd-fmv/<stem>.mkv     (cropped, fills the window)
    //   off -> hd-fmv/<stem>-43.mkv  when one exists, else <stem>.mkv
    // Either way the frame is shown whole (contain-max); nothing is cropped at
    // playback time any more. The four films that were never cropped (tut1,
    // tut2, tut3, ende) have no spare and simply fall through to <stem>.mkv.
    //
    // Same two tiers as the HD toggle above: the shell checkbox / loaded
    // profile calls SetFillEnabled, and until one does the nucleus.ini default
    // (gfx.movie_scale) decides.
    static void SetFillEnabled(bool on);
    static bool FillEnabled();

    // Just what nucleus.ini says, ignoring the checkbox state. Loading a
    // profile reseeds from this so the setting cannot be inherited from the
    // previously loaded profile.
    static bool FillDefaultFromIni();

    // Resolved once per movie start. Always MOVIE_SCALE_MAX now that the
    // checkbox picks a file instead of a scale mode — only UA_FMV_SCALE=fill
    // can still reach the crop-to-cover path, and that is a developer escape
    // hatch, not a player-facing setting.
    static MovieScaleMode ScaleMode();

private:
    TMovie();

    // directPath: fname is a real on-disk path, skip the FSMgr node lookup (and
    // with it correctSeparatorAndExt's 3-char extension truncation).
    bool OpenFile(const std::string &fname, bool directPath = false);
    void Close();
    
    void ReadFrames();
    
    void ProcessFrame(uint32_t tm);
    void ProcessAudio();
    
    static int EventsWatcher(void *, SDL_Event *event);
    
public:
    static TMovie Instance;
private:
    Context *_ctx = NULL;
};  

static constexpr TMovie &Movie = TMovie::Instance;
    
}

#endif