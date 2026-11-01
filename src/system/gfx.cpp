#include "../reforge_fx.h"
#include "../reforge_sky.h"
#include "../reforge_trails.h"
#include "../gfxsettings.h"
#include "../fmtlib/printf.h"
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_image.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/glext.h>
#include <stack>
#include <algorithm>
#else
#include <GL/glext.h>
#include <queue>
#endif

#include "gfx.h"
#include "gfx_glow.h"
#include "atmos.h"
#include "../utils.h"
#include "common/common.h"
#include "../ini.h"
#include "../gui/root.h"
#include "../bitmap.h"
#include "../ilbm.h"
#include "../log.h"
#include "../font.h"
#include "inivals.h"
#include "glfuncs.h"
#include "tform.h"
#include "perf.h"

namespace GFX
{   
GFXEngine GFXEngine::Instance;

int GFXEngine::can_srcblend;
int GFXEngine::can_destblend;
int GFXEngine::can_stippling;
uint32_t GFXEngine::FpsMaxTicks = 1000/60;

SDL_PixelFormat *GFXEngine::_pixfmt = NULL;
GLint GFXEngine::_glPixfmt, GFXEngine::_glPixtype;
bool GFXEngine::_staticInited = false;

const std::array<vec3d, 8> GFXEngine::_clrEff 
{   vec3d(1.0,  1.0,  1.0)
,   vec3d(1.21, 0.0,  0.29)
,   vec3d(0.13, 0.43, 2.17)
,   vec3d(0.0,  1.60, 1.60)
,   vec3d(1.0,  1.0,  1.0)
,   vec3d(0.57, 0.59, 0.59)
,   vec3d(1.4,  1.08,  1.12)
,   vec3d(0.3,  0.60, 0.7)};

std::vector<TGFXDeviceInfo> GFXEngine::_devices
{
    TGFXDeviceInfo("Opengl", "<primary>")         
};


bool TRenderNode::CompareSolid(TRenderNode *a, TRenderNode *b)
{
    if ( !a->Mesh )
        return true;
    
    if ( !b->Mesh )
        return false;
    
    return a->Tex < b->Tex;
}

bool TRenderNode::CompareTransparent(TRenderNode* a, TRenderNode* b)
{
    return a->Distance > b->Distance;
}

bool TRenderNode::CompareDistance(TRenderNode* a, TRenderNode* b)
{
    return a->Distance < b->Distance;
}

bool TRenderParams::operator==(const TRenderParams &b)
{
    if (Flags != b.Flags)
        return false;

    if (Pbr != b.Pbr)
        return false;

    if (Flags & RFLAGS_DYNAMIC_TEXTURE)
        return TexSource->IsSameRes( b.TexSource );

    return Tex == b.Tex;
}

void GFXEngine::StaticInit()
{
    if (_staticInited)
        return;

    _staticInited = true;
    
    SDL_DisplayMode curr;
    SDL_GetCurrentDisplayMode(0, &curr);
    
    switch(curr.format)
    {
        case SDL_PIXELFORMAT_RGB888:
        case SDL_PIXELFORMAT_ARGB8888:
            _pixfmt = SDL_AllocFormat( SDL_PIXELFORMAT_ARGB8888 );
            _glPixfmt = GL_BGRA;
            _glPixtype = GL_UNSIGNED_BYTE;
            break;
        
        case SDL_PIXELFORMAT_BGR888:
        case SDL_PIXELFORMAT_ABGR8888:
        default:
            _pixfmt = SDL_AllocFormat( SDL_PIXELFORMAT_ABGR8888 );
            _glPixfmt = GL_RGBA;
            _glPixtype = GL_UNSIGNED_BYTE;
            break;
    }
}

GFXEngine::GFXEngine()
{
    for(TileMap *&t : _tiles)
        t = NULL;
    
    for(SDL_Color &c : _palette)
        c = {0, 0, 0, 0};
    
    _forcesoftcursor = 0;
    _field_38 = 0;
    _txt16bit = 0;
    _use_simple_d3d = 0;
    _disable_lowres = 0;
    _export_window_mode = 0;
    _flags = 0;

    _dither = 0;
    _filter = 0;
    _antialias = 0;
    _alpha = 255;
    _zbuf_when_tracy = 0;
    _colorkey = 0;

    _sceneBeginned = 0;

    _corrIW = _corrW = 1.0;
    _corrIH = _corrH = 1.0;

    _solidFont = true;
    
    _setFrustumClip(1.0, 8192.0);
    
    _normClr = vec3d(1.0, 1.0, 1.0);
    _invClr = vec3d(0.0, 0.0, 0.0);
}

std::string read_guid(const std::string &filename)
{
    FSMgr::FileHandle *fil = uaOpenFileAlloc(filename, "r");
    if ( !fil )
        return "";
    
    std::string guid;
    fil->ReadLine(&guid);
    delete fil;
    return guid;
}

bool out_guid_to_file(const std::string &filename, const std::string &name)
{
    FSMgr::FileHandle *fil = uaOpenFileAlloc(filename, "w");
    if ( !fil )
        return false;
    
    fil->puts(name);
    delete fil;
    return true;
}

void out_yes_no_status(const char *filename, int val)
{
    FSMgr::FileHandle *fil = uaOpenFileAlloc(filename, "w");
    if ( fil )
    {
        if ( val )
            fil->puts("yes");
        else
            fil->puts("no");
        delete fil;
    }
}

void GFXEngine::DrawTextEntry(const ScreenText *txt)
{
    if ( _font.ttfFont )
    {
        if ( txt->flag & 0x20 )
        {
            _font.r = txt->p1;
            _font.g = txt->p2;
            _font.b = txt->p3;
        }
        else
        {
            if (!txt->string.empty())
            {

                int cx = 0, cy = 0;

                if ( txt->flag & 0xE )
                {
                    TTF_SizeUTF8(_font.ttfFont, txt->string.c_str(), &cx, &cy);
                }

                int p1 = txt->p1;
                int p2 = txt->p2;
                int p3 = txt->p3;
                int p4 = txt->p4;

                if ( txt->flag & 8 )
                    p3 = cx * p3 / 100;

                SDL_Rect clipRect;

                clipRect.x = p1;
                clipRect.w = p3 + 4;
                clipRect.h = p4 + 1;
                clipRect.y = p2;

                if ( txt->flag & 2 )
                {
                    if ( txt->flag & 8 )
                    {
                        p1 -= cx;
                        clipRect.x = p1;
                        clipRect.w = p3 + 4;
                    }
                    else
                    {
                        p1 += (p3 - cx);
                    }
                }
                else if ( txt->flag & 4 )
                {
                    if ( txt->flag & 8 )
                    {
                        p1 -= cx / 2;
                        clipRect.x = p1;
                        clipRect.w = p3 + 4;
                    }
                    else
                    {
                        p1 += (p3 - cx) / 2;
                    }
                }

                int v10 = ((p4 - _font.height) / 2) - 2 + p2;
                if ( txt->flag & 0x10 )
                {
                    v10++;
                    p1++;
                }

                SDL_Color clr;
                clr.a = 255;
                clr.r = 0;
                clr.g = 0;
                clr.b = 0;

                // ---- hi-res text layer -------------------------------------
                // Everything above ran in logical pixels with the 1x font, so
                // the clip box, alignment and baseline are exactly what the
                // classic path computes. Only the glyph RASTER changes: it is
                // produced by the hi-res twin and blitted into a surface the
                // size of the real drawable, scaled to the very same footprint
                // (w1 x h1 logical pixels). Layout boxes therefore cannot move
                // - which is what killed the first "just scale the font" hack.
                // Which surface takes the glyphs, and at what density:
                //   TextSurface        - the hi-res text layer, drawable resolution
                //   Screen() at n > 1  - the hi-density UI surface, when the
                //                        text layer is explicitly off but the
                //                        bitmap layer is on (1x glyphs would
                //                        otherwise land n times too small)
                SDL_Surface *hiDst = NULL;
                double hsx = 1.0, hsy = 1.0;

                if ( TextSurface && EnsureHiFont(TextSurface->h) )
                {
                    hiDst = TextSurface;
                    hsx = (double)TextSurface->w / (double)_resolution.x;
                    hsy = (double)TextSurface->h / (double)_resolution.y;
                }
                else if ( !TextSurface && UiScale() > 1 && EnsureHiFont(_resolution.y * _uiScale) )
                {
                    hiDst = Screen();
                    hsx = hsy = (double)_uiScale;
                }

                if ( hiDst )
                {
                    const double sx = hsx;
                    const double sy = hsy;

                    int w1 = 0, h1 = 0;
                    TTF_SizeUTF8(_font.ttfFont, txt->string.c_str(), &w1, &h1);

                    // Map EDGES, never (origin, size): rounding an origin and a
                    // size independently can shave a pixel off the far edge and
                    // clip the last glyph.
                    SDL_Rect hClip;
                    hClip.x = (int)(clipRect.x * sx + 0.5);
                    hClip.y = (int)(clipRect.y * sy + 0.5);
                    hClip.w = (int)((clipRect.x + clipRect.w) * sx + 0.5) - hClip.x;
                    hClip.h = (int)((clipRect.y + clipRect.h) * sy + 0.5) - hClip.y;

                    SDL_SetClipRect(hiDst, &hClip);

                    // shadow, then the coloured copy one logical pixel up/left
                    for (int pass = 0; pass < 2; pass++)
                    {
                        if (pass)
                        {
                            clr.r = _font.r;
                            clr.g = _font.g;
                            clr.b = _font.b;
                        }

                        SDL_Surface *hi = TTF_RenderUTF8_Blended(_font.ttfFontHi, txt->string.c_str(), clr);
                        if (!hi)
                            continue;

                        SDL_SetSurfaceBlendMode(hi, SDL_BLENDMODE_BLEND);

                        const int lx = p1 + (pass ? 1 : 2);
                        const int ly = v10 + (pass ? 0 : 1);

                        SDL_Rect dst;
                        dst.x = (int)(lx * sx + 0.5);
                        dst.y = (int)(ly * sy + 0.5);
                        dst.w = (int)((lx + w1) * sx + 0.5) - dst.x;
                        dst.h = (int)((ly + h1) * sy + 0.5) - dst.y;

                        if ( hiDst == ScreenSurface )
                            MarkScreen(dst);
                        SDL_BlitScaled(hi, NULL, hiDst, &dst);
                        SDL_FreeSurface(hi);
                    }

                    SDL_SetClipRect(hiDst, NULL);

                    if ( hiDst == TextSurface )
                        TextLayerDirty(hClip);
                    return;
                }
                // ---- classic path (logical-resolution glyphs) ---------------
                // Untouched at uiScale 1 (byte-identical to the original
                // build). It is only reachable at uiScale > 1 if the hi font
                // failed to open, in which case scaling the boxes at least
                // keeps the text where the layout put it.
                const int tn = UiScale();
                if ( tn > 1 )
                {
                    clipRect.w = (clipRect.x + clipRect.w) * tn - clipRect.x * tn;
                    clipRect.h = (clipRect.y + clipRect.h) * tn - clipRect.y * tn;
                    clipRect.x *= tn;
                    clipRect.y *= tn;
                    p1  *= tn;
                    v10 *= tn;
                }

                SDL_SetClipRect(Screen(), &clipRect);

                SDL_Surface *tmp;

                if (_solidFont)
                {
                    tmp = TTF_RenderUTF8_Solid(_font.ttfFont, txt->string.c_str(), clr);
                    SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_NONE);
                }
                else
                {
                    tmp = TTF_RenderUTF8_Blended(_font.ttfFont, txt->string.c_str(), clr);
                    SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);
                }
                
                if (tmp)
                {
                    SDL_Rect want;
                    want.w = tmp->w;
                    want.h = tmp->h;
                    want.x = p1 + 2;
                    want.y = v10 + 1;

                    SDL_BlitSurface(tmp, NULL, Screen(), &want);
                    MarkScreen(want);

                    clr.a = 255;
                    clr.r = _font.r;
                    clr.g = _font.g;
                    clr.b = _font.b;

                    if (_solidFont)
                    {
                        SDL_SetPaletteColors(tmp->format->palette, &clr, 1, 1);
                    }
                    else
                    {
                        SDL_FreeSurface(tmp);
                        tmp = TTF_RenderUTF8_Blended(_font.ttfFont, txt->string.c_str(), clr);
                        SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);
                    }

                    want.w = tmp->w;
                    want.h = tmp->h;
                    want.x = p1 + 1;
                    want.y = v10;

                    SDL_BlitSurface(tmp, NULL, Screen(), &want);
                    MarkScreen(want);
                    SDL_FreeSurface(tmp);


                    SDL_SetClipRect(Screen(), NULL);
                }
            }
        }
    }
}

void GFXEngine::AddScreenText(const std::string &string, int p1, int p2, int p3, int p4, int flag)
{
    ScreenText *v8 = new ScreenText;
    v8->string = string;
    v8->p1 = p1;
    v8->p2 = p2;
    v8->p3 = p3;
    v8->p4 = p4;
    v8->flag = flag;

    _font.entries.push_back(v8);
}

void GFXEngine::DrawScreenText()
{
    _font.r = 255;
    _font.g = 255;
    _font.b = 0;

    for ( std::list<ScreenText *>::iterator it = _font.entries.begin(); it != _font.entries.end(); it++ )
    {
        DrawTextEntry(*it);
        delete (*it);
    }

    _font.entries.clear();
}

void GFXEngine::initPolyEngine()
{
    _states = GfxStates();
    _states.LinearFilter = (_filter != 0);

    SetRenderStates(1);

    glEnable(GL_CULL_FACE);

    glEnable(GL_MULTISAMPLE); // harmless without an MSAA framebuffer

    if (_dither)
        glEnable(GL_DITHER);
    else
        glDisable(GL_DITHER);
}

int GFXEngine::LoadFontByDescr(const std::string &fontname)
{
    std::vector<std::string> splt = Stok::Split(fontname, ",");

    std::string facename;
    std::string s_height;
    
    if (splt.size() > 0)
        facename = splt[0];
    
    if (splt.size() > 1)
        s_height = splt[1];
    //const char *s_weight = strtok(0, ",");
    //const char *s_charset = strtok(0, ",");

    int height;//, weight, charset;
    if ( !facename.empty() && !s_height.empty() )//&& s_weight && s_charset )
    {
        height = std::stoi(s_height);
        //weight = atoi(s_weight);
        //charset = atoi(s_charset);
    }
    else
    {
        height = 12;
        //charset = 0;
        facename = "MS Sans Serif";
        //weight = 400;
    }

    if ( _font.ttfFont )
    {
        TTF_CloseFont(_font.ttfFont);
        _font.ttfFont = NULL;
    }

    if ( _font.ttfFontHi )
    {
        TTF_CloseFont(_font.ttfFontHi);
        _font.ttfFontHi = NULL;
    }
    _font.hiHeight = 0;

    _font.height = height;
    _font.face = facename;
    _font.ttfFont = System::LoadFont(facename, height);

    if ( _font.ttfFont )
    {
        if (!_solidFont)
            TTF_SetFontHinting(_font.ttfFont, TTF_HINTING_LIGHT);
        else
            TTF_SetFontHinting(_font.ttfFont, TTF_HINTING_MONO);

        return 1;
    }

    printf("Can't load font\n");

    return 0;
}


size_t GFXEngine::windd_func0(IDVList &stak)
{
    int txt16bit_def = read_yes_no_status("env/txt16bit.def", 1);
    int drawprim_def = read_yes_no_status("env/drawprim.def", 0);
    _export_window_mode = System::IniConf::GfxExportWindowMode.Get<bool>();     // gfx.export_window_mode

    switch(System::IniConf::GfxBlending.Get<int>())
    {
        case 0:
        {
            can_srcblend = 1;
            can_destblend = 0;
            can_stippling = 0;
        }
        break;
        
        default:
        case 1:
        {
            can_srcblend = 1;
            can_destblend = 1;
            can_stippling = 0;
        }
        break;
    
        case 2:
        {
            can_srcblend = 0;
            can_destblend = 0;
            can_stippling = 1;
        }
        break;
    }

    SetResVariables( Common::Point(stak.Get<int32_t>(ATT_WIDTH, DEFAULT_WIDTH), stak.Get<int32_t>(ATT_HEIGHT, DEFAULT_HEIGHT)) );

    _forcesoftcursor = 0;
    _disable_lowres = System::IniConf::GfxDisableLowres.Get<bool>();
    _txt16bit = txt16bit_def;
    _use_simple_d3d = drawprim_def;

    _solidFont = System::IniConf::GfxSolidFont.Get<bool>();

    // An off-screen window is never composited, so a vsynced SDL_GL_SwapWindow
    // waits for a vblank that never arrives and the frame loop crawls
    // (measured: <300 frames in 30s). Hidden mode therefore runs unsynced.
    int vsync = System::WindowHidden() ? 0 : System::IniConf::GfxVsync.Get<int>();

    switch( vsync )
    {
        case 0:
            SDL_GL_SetSwapInterval(0);
            break;

        default:
        case 1:
            SDL_GL_SetSwapInterval(1);
            break;

        case 2:
            {
                if ( SDL_GL_SetSwapInterval(-1) == -1)
                    SDL_GL_SetSwapInterval(1);
            }
            break;
    }

    fpsLimitter(System::IniConf::GfxMaxFps.Get<int>());

    // Dev/perf aid: UA_MAXFPS=<n> overrides gfx.maxfps for this run only
    // (0 = uncapped), without touching the shared config file.
    if ( const char *mf = getenv("UA_MAXFPS") )
        fpsLimitter(atoi(mf));

    LoadFontByDescr("MS Sans Serif,12,400,0");

    //win3d->field_54______rsrc_field4 = (bitmap_intern *)getRsrc_pData();
    return 1;
}


bool GFXEngine::SetResVariables(Common::Point res)
{
    _resolution = res;   //stak.Get<int32_t>(ATT_WIDTH, 0);
    
    _clip = _resolution - Common::Point(1, 1);

    _field_54c = _resolution.x / 2;
    _field_550 = _resolution.y / 2;

    _field_554 = _resolution.x / 2;
    _field_558 = _resolution.y / 2;
    
    return true;
}

size_t GFXEngine::func0(IDVList &stak)
{
    System::IniConf::ReadFromNucleusIni();

    if ( !windd_func0(stak) )
        return 0;

    _dither = System::IniConf::GfxDither.Get<bool>();
    _filter = System::IniConf::GfxFilter.Get<bool>();
    _antialias = System::IniConf::GfxAntialias.Get<bool>();
    _zbuf_when_tracy = System::IniConf::GfxZbufWhenTracy.Get<bool>();
    _colorkey = System::IniConf::GfxColorkey.Get<bool>();

    if ( can_srcblend )
        _alpha = 192;
    else
        _alpha = 128;

    ApplyResolution();

    return 1;
}

void GFXEngine::ApplyResolution()
{
    UpdateAspectCorrection();

    initPolyEngine();
}

void GFXEngine::UpdateAspectCorrection()
{
    // The 3D scene renders into the real drawable (glViewport uses
    // System::GetResolution()), while _resolution is only the logical 2D
    // resolution. Base the FOV/aspect correction on the drawable, otherwise
    // a 4:3 logical mode on a widescreen display renders stretched 3D.
    Common::Point real = System::GetResolution();
    if (!real)
        real = _resolution;

    if ( (float)real.x / (float)real.y >= 1.4 )
    {
        int half = (real.x + real.y) / 2;
        _corrW = (float)half * 1.1429 / (float)real.x;
        _corrH = (float)half * 0.85715 / (float)real.y;
        _corrIW = 1.0 / _corrW;
        _corrIH = 1.0 / _corrH;
    }
    else //No correction
    {
        _corrIW = _corrW = 1.0;
        _corrIH = _corrH = 1.0;
    }
}


// A logical 2D line is one logical pixel wide. On the hi-density surface that
// has to stay one LOGICAL pixel wide - a hairline would make the briefing map
// and the radar cross thinner than the classic look, not crisper - so the
// scaled line is stamped over an n x n footprint, which is exactly what a
// magnified 1px line is.
void GFXEngine::DrawLineUI(const Common::Line &line, uint8_t r, uint8_t g, uint8_t b)
{
    const int n = UiScale();

    if ( n <= 1 )
    {
        DrawLine(Screen(), line, r, g, b);
        return;
    }

    MarkScreen(std::min(line.x1, line.x2) * n, std::min(line.y1, line.y2) * n,
               (std::abs(line.x2 - line.x1) + 1) * n, (std::abs(line.y2 - line.y1) + 1) * n);
    for (int dy = 0; dy < n; dy++)
        for (int dx = 0; dx < n; dx++)
            DrawLine(Screen(), Common::Line(line.x1 * n + dx, line.y1 * n + dy,
                                            line.x2 * n + dx, line.y2 * n + dy), r, g, b);
}

// Background art (the full-screen shell paintings, briefing maps) is the one 2D
// source that WANTS smoothing: it is 640x480 palette-dithered art that used to
// be scaled to the logical surface and then smoothed by the GL_LINEAR screen
// quad. At _uiScale > 1 the GL step no longer magnifies, so a plain
// SDL_BlitScaled - which is nearest - would hand back a crunchy, speckled
// version of a picture that used to look smooth. Do the magnification with a
// linear stretch instead, and fall back to the classic call whenever the
// source needs blit semantics SoftStretch does not implement (colour key,
// per-surface alpha, format mismatch).
// True when every pixel of the rect is fully opaque, i.e. an alpha blend of it
// is indistinguishable from a copy. Shell background art is opaque in practice
// but is blitted with SDL_BLENDMODE_BLEND, and SDL_SoftStretchLinear ignores
// blending - so prove it rather than assume it. ~300k masked loads on a 640x480
// source, well under 0.1 ms, and only on the magnifying path.
static bool uiRectOpaque(SDL_Surface *s, const SDL_Rect *r)
{
    const uint32_t am = s->format->Amask;
    if ( am == 0 )
        return true;
    if ( s->format->BytesPerPixel != 4 )
        return false;

    if ( SDL_LockSurface(s) != 0 )
        return false;

    bool opaque = true;
    for (int y = r->y; y < r->y + r->h && opaque; y++)
    {
        const uint32_t *p = (const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * s->pitch) + r->x;
        for (int x = 0; x < r->w; x++)
        {
            if ( (p[x] & am) != am )
            {
                opaque = false;
                break;
            }
        }
    }

    SDL_UnlockSurface(s);
    return opaque;
}

static void uiBlitScaled(SDL_Surface *src, SDL_Rect *srcR, SDL_Surface *dst, SDL_Rect *dstR, int uiN)
{
#if SDL_VERSION_ATLEAST(2, 0, 16)
    // At uiScale 1 nothing about this call may change: the classic path has to
    // stay byte-identical, and art that the shell already magnifies inside the
    // logical surface would otherwise start arriving smoothed.
    if ( uiN <= 1 )
    {
        SDL_BlitScaled(src, srcR, dst, dstR);
        return;
    }

    uint32_t ck;
    SDL_BlendMode bm = SDL_BLENDMODE_NONE;
    uint8_t amod = 255;

    const bool magnifying = ( dstR->w > srcR->w && dstR->h > srcR->h );

    const bool sameFmt = ( src->format->format == dst->format->format );
    const bool noKey   = ( SDL_GetColorKey(src, &ck) != 0 );
    const bool noMod   = ( SDL_GetSurfaceAlphaMod(src, &amod) == 0 && amod == 255 );
    const bool blendOk = ( SDL_GetSurfaceBlendMode(src, &bm) == 0 &&
                           ( bm == SDL_BLENDMODE_NONE || bm == SDL_BLENDMODE_BLEND ) );

    if ( magnifying && sameFmt && noKey && noMod && blendOk &&
         srcR->x >= 0 && srcR->y >= 0 &&
         srcR->x + srcR->w <= src->w && srcR->y + srcR->h <= src->h &&
         uiRectOpaque(src, srcR) &&
         SDL_SoftStretchLinear(src, srcR, dst, dstR) == 0 )
        return;
#endif

    SDL_BlitScaled(src, srcR, dst, dstR);
}


// UI graphics, UI level >= 2: a background picture's hi-density twin. The 2D
// pictures (shell menus, campaign map, briefing/debrief backgrounds, loading
// art) are 640x480-class palette paintings that uiBlitScaled() resamples
// linearly to the hi-density surface; a twin in hd-textures/ui-art/ (same
// content-addressed name as the UI sheet pack: art_<w>x<h>_<fnv1a>.png, an
// exact integer k = 2..6 times the original) is drawn instead, from k-scaled
// source coordinates. Looked up once per bitmap on first draw, so level < 2
// never opens a file. UA_UI_DUMP=1 also dumps the originals to hd-dump/ui-art/.
static SDL_Surface *uiArtFor(ResBitmap *bm, SDL_Surface *like, int *k)
{
    *k = 0;
    if ( !bm || !bm->swTex || HDTex::UiLevel() < 2 )
        return NULL;

    if ( !bm->uiArtTried )
    {
        bm->uiArtTried = true;

        SDL_Surface *src = bm->swTex;
        const std::string base = HDTex::UiFileName("art", src);

        if ( HDTex::UiDumpEnabled() )
        {
            HDTex::MakeDir("hd-dump");
            HDTex::MakeDir("hd-dump/ui-art");
            const std::string dp = "hd-dump/ui-art/" + base;
            if ( !HDTex::FileExists(dp) )
                IMG_SavePNG(src, dp.c_str());
        }

        std::string path;
        for ( const std::string &dir : HDTex::UiArtDirs() )
            if ( HDTex::FileExists(dir + base) ) { path = dir + base; break; }
        if ( !path.empty() )
        {
            SDL_Surface *raw = IMG_Load(path.c_str());
            if ( raw )
            {
                const int kx = raw->w / src->w;
                if ( kx >= 2 && kx <= 6 && raw->w == src->w * kx && raw->h == src->h * kx )
                {
                    SDL_Surface *cv = SDL_ConvertSurfaceFormat(raw, like->format->format, 0);
                    if ( cv )
                    {
                        uint8_t am = 255;
                        SDL_BlendMode bmode = SDL_BLENDMODE_NONE;
                        SDL_GetSurfaceAlphaMod(src, &am);
                        SDL_GetSurfaceBlendMode(src, &bmode);
                        SDL_SetSurfaceAlphaMod(cv, am);
                        SDL_SetSurfaceBlendMode(cv, bmode);
                        bm->uiArt = cv;
                        bm->uiArtScale = kx;
                        fmt::printf("ui-art: %s (%dx%d -> %dx)\n", path, src->w, src->h, kx);
                    }
                }
                else
                    fmt::printf("ui-art: %s IGNORED - %dx%d is not an integer multiple of %dx%d\n",
                                path, raw->w, raw->h, src->w, src->h);
                SDL_FreeSurface(raw);
            }
        }
    }

    // The game may change the alpha mod of a picture between draws (fades).
    if ( bm->uiArt )
    {
        uint8_t am = 255;
        if ( SDL_GetSurfaceAlphaMod(bm->swTex, &am) == 0 )
            SDL_SetSurfaceAlphaMod(bm->uiArt, am);
        *k = bm->uiArtScale;
    }
    return bm->uiArt;
}

// Draw one background picture rect at uiScale n: from its hi-density twin when
// there is one (UI >= 2), else the classic resample.
static void uiBlitArt(ResBitmap *bm, SDL_Rect *srcR, SDL_Surface *dst, SDL_Rect *dstR, int n)
{
    int k = 0;
    SDL_Surface *hi = ( n > 1 ) ? uiArtFor(bm, dst, &k) : NULL;
    if ( hi && k > 1 )
    {
        SDL_Rect s2 = { srcR->x * k, srcR->y * k, srcR->w * k, srcR->h * k };
        if ( s2.w == dstR->w && s2.h == dstR->h )
            SDL_BlitSurface(hi, &s2, dst, dstR);
        else
            uiBlitScaled(hi, &s2, dst, dstR, n);
        return;
    }
    uiBlitScaled(bm->swTex, srcR, dst, dstR, n);
}

size_t GFXEngine::raster_func198(const Common::FLine &arg)
{
    float tX = _field_554 - 1.0;
    float tY = _field_558 - 1.0;

    int y1 = (arg.y1 + 1.0) * tY;
    int y2 = (arg.y2 + 1.0) * tY;
    int x1 = (arg.x1 + 1.0) * tX;
    int x2 = (arg.x2 + 1.0) * tX;

    DrawLineUI(Common::Line(x1, y1, x2, y2),
                      _field_4.r,
                      _field_4.g,
                      _field_4.b);
    return 1;
}


size_t GFXEngine::raster_func199(const Common::Line &arg)
{
    DrawLineUI(Common::Line(_field_54c + arg.x1, _field_550 + arg.y1,
                                   _field_54c + arg.x2, _field_550 + arg.y2),
                      _field_4.r,
                      _field_4.g,
                      _field_4.b );

    return 1;
}

void GFXEngine::sub_420EDC(Common::Line line, uint8_t r, uint8_t g, uint8_t b)
{
    if ( line.ClipBy(_clip) )
    {
        Common::Line tmp2 = line;

        // Clipping is done above, in logical coordinates; DrawLineUI only maps
        // the survivors onto the hi-density surface.
        if ( _inverseClip.IsEmpty() || !tmp2.ClipBy(_inverseClip) )
        {
            DrawLineUI(line, r, g, b);
        }
        else
        {
            if ( tmp2.P2() != line.P2() )
            {
                if ( tmp2.P1() != line.P1() )
                {
                    DrawLineUI(Common::Line(line.P1(), tmp2.P1()), r, g, b);
                    DrawLineUI(Common::Line(tmp2.P2(), line.P2()), r, g, b);
                }
                else
                {
                    DrawLineUI(Common::Line(tmp2.P2(), line.P2()), r, g, b);
                }
            }
            else
            {
                DrawLineUI(Common::Line(line.P1(), tmp2.P1()), r, g, b);
            }
        }
    }
}

size_t GFXEngine::raster_func200(const Common::FLine &arg)
{
    float tX = _field_554 - 1.0;
    float tY = _field_558 - 1.0;

    sub_420EDC (Common::Line( (arg.x1 + 1.0) * tX
                            , (arg.y1 + 1.0) * tY
                            , (arg.x2 + 1.0) * tX
                            , (arg.y2 + 1.0) * tY),
                        _field_4.r,
                        _field_4.g,
                        _field_4.b);

    return 1;
}

size_t GFXEngine::raster_func201(const Common::Line &l)
{
    sub_420EDC( Common::Line( _field_54c + l.x1
                            , _field_550 + l.y1
                            , _field_54c + l.x2
                            , _field_550 + l.y2),
                        _field_4.r,
                        _field_4.g,
                        _field_4.b );

    return 1;
}

size_t GFXEngine::raster_func202(rstr_arg204 *arg)
{
    Common::Rect r1;
    r1.left   = (arg->float4.left   + 1.0) * (arg->pbitm->width / 2);
    r1.top    = (arg->float4.top    + 1.0) * (arg->pbitm->height / 2);
    r1.right  = (arg->float4.right  + 1.0) * (arg->pbitm->width / 2);
    r1.bottom = (arg->float4.bottom + 1.0) * (arg->pbitm->height / 2);

    Common::Rect r2;
    r2.left   = _field_554 * (arg->float14.left   + 1.0);
    r2.top    = _field_558 * (arg->float14.top    + 1.0);
    r2.right  = _field_554 * (arg->float14.right  + 1.0);
    r2.bottom = _field_558 * (arg->float14.bottom + 1.0);
    
    const int n = UiScale();

    SDL_Rect src = r1;
    SDL_Rect dst = Common::Rect(r2.left * n, r2.top * n, r2.right * n, r2.bottom * n);

    // Resampled once, straight to its final density, instead of twice (to
    // 640x480 and then by the GL_LINEAR screen quad).
    MarkScreen(dst);
    uiBlitArt(arg->pbitm, &src, Screen(), &dst, n);

    return 1;
}

size_t GFXEngine::raster_func204(rstr_arg204 *arg)
{   
    Common::Rect r1;
    r1.left   = (arg->float4.left   + 1.0) * (arg->pbitm->width / 2);
    r1.top    = (arg->float4.top    + 1.0) * (arg->pbitm->height / 2);
    r1.right  = (arg->float4.right  + 1.0) * (arg->pbitm->width / 2);
    r1.bottom = (arg->float4.bottom + 1.0) * (arg->pbitm->height / 2);

    Common::Rect r2;
    r2.left   = _field_554 * (arg->float14.left   + 1.0);
    r2.top    = _field_558 * (arg->float14.top    + 1.0);
    r2.right  = _field_554 * (arg->float14.right  + 1.0);
    r2.bottom = _field_558 * (arg->float14.bottom + 1.0);
    
    if ( _clip.IsIntersects(r2) )
    {
        if ( r2.left < _clip.left )
        {
            r1.left += (_clip.left - r2.left) * r1.Width() / r2.Width();
            r2.left = _clip.left;
        }

        if ( r2.right > _clip.right )
        {
            r1.right += (_clip.right - r2.right) * r1.Width() / r2.Width();
            r2.right = _clip.right;
        }

        if ( r2.top < _clip.top )
        {
            r1.top += (_clip.top - r2.top) * r1.Height() / r2.Height();
            r2.top = _clip.top;
        }

        if ( r2.bottom > _clip.bottom )
        {
            r1.bottom += (_clip.bottom - r2.bottom) * r1.Height() / r2.Height();
            r2.bottom = _clip.bottom;
        }
    
        const int n = UiScale();

        SDL_Rect src = r1;
        SDL_Rect dst = Common::Rect(r2.left * n, r2.top * n, r2.right * n, r2.bottom * n);
        MarkScreen(dst);
        uiBlitArt(arg->pbitm, &src, Screen(), &dst, n);
    }

    return 1;
}

void GFXEngine::SetRenderStates(int setAll)
{
//    static const std::array<int, 4> blends = {GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
    GfxStates *newStates;
    
    if (setAll < 2)
        newStates = &_states;
    else
        newStates = &_lastStates;
    
    bool forceSetShader = false;
    if (setAll)
        forceSetShader = true;
    
    if (_glext)
    {
        if (setAll || (newStates->Prog.ID != _lastStates.Prog.ID))
        {
            if (_vbo && _lastStates.Prog.ID)
            {
                if (_lastStates.Prog.PosLoc != -1)
                    Glext::GLDisableVertexAttribArray(_lastStates.Prog.PosLoc);
                
                if (_lastStates.Prog.ColorLoc != -1)
                    Glext::GLDisableVertexAttribArray(_lastStates.Prog.ColorLoc);
                
                if (_lastStates.Prog.UVLoc != -1)
                    Glext::GLDisableVertexAttribArray(_lastStates.Prog.UVLoc);

                if (_lastStates.Prog.NormalLoc != -1)
                    Glext::GLDisableVertexAttribArray(_lastStates.Prog.NormalLoc);

                if (_lastStates.Prog.TangentLoc != -1)
                    Glext::GLDisableVertexAttribArray(_lastStates.Prog.TangentLoc);
            }
            
            Glext::GLUseProgram(newStates->Prog.ID);
            
            forceSetShader = true;
            
            if (_vbo && newStates->Prog.ID)
            {
                if (newStates->Prog.PosLoc != -1)
                    Glext::GLEnableVertexAttribArray(newStates->Prog.PosLoc);
                
                if (newStates->Prog.ColorLoc != -1)
                    Glext::GLEnableVertexAttribArray(newStates->Prog.ColorLoc);

                if (newStates->Prog.NormalLoc != -1)
                    Glext::GLEnableVertexAttribArray(newStates->Prog.NormalLoc);

                if (newStates->Prog.TangentLoc != -1)
                    Glext::GLEnableVertexAttribArray(newStates->Prog.TangentLoc);
            }
        }
    }
    
    if (_vbo)
    {        
        if (setAll || (newStates->DataBuf != _lastStates.DataBuf))
        {
            Glext::GLBindBuffer(GL_ARRAY_BUFFER, newStates->DataBuf);
        }
        
        if (setAll || (newStates->IndexBuf != _lastStates.IndexBuf))
        {
            Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newStates->IndexBuf);
        }
        
        if (forceSetShader || (newStates->Tex != _lastStates.Tex))
        {
            if (newStates->Tex)
            {
                if (newStates->Prog.UVLoc != -1)
                    Glext::GLEnableVertexAttribArray(newStates->Prog.UVLoc);
                
                glBindTexture(GL_TEXTURE_2D, newStates->Tex);

                _vboStatesBlock.Textured = 1;
            }
            else
            {
                if (newStates->Prog.UVLoc != -1)
                    Glext::GLDisableVertexAttribArray(newStates->Prog.UVLoc);
                
                glBindTexture(GL_TEXTURE_2D, 0);

                _vboStatesBlock.Textured = 0;
            }
            _vboStatesChanged = true;
        }
        
        if ((forceSetShader || (newStates->Fog != _lastStates.Fog) ||
            (newStates->FogLength != _lastStates.FogLength) ||
            (newStates->FogStart != _lastStates.FogStart)) )
        {
            if (newStates->Fog)
            {
                _vboStatesBlock.Fog = 1.0;
                _vboStatesBlock.FogStart = newStates->FogStart;
                _vboStatesBlock.FogLength = newStates->FogLength;
            }
            else
            {
                _vboStatesBlock.Fog = 0.0;
            }
            _vboStatesChanged = true;
        }
        
        if ((forceSetShader || (newStates->AFog != _lastStates.AFog) ||
            (newStates->AFogLength != _lastStates.AFogLength) ||
            (newStates->AFogStart != _lastStates.AFogStart)) )
        {
            if (newStates->AFog)
            {
                _vboStatesBlock.AFog = 1.0;
                _vboStatesBlock.AFogStart = newStates->AFogStart;
                _vboStatesBlock.AFogLength = newStates->AFogLength;
            }
            else
            {
                _vboStatesBlock.AFog = 0.0;
            }
            _vboStatesChanged = true;
        }
        
        if (forceSetShader || (newStates->Shaded != _lastStates.Shaded))
        {
            if (newStates->Shaded)
            {
                _vboStatesBlock.Flat = 0;
            }
            else
            {
                _vboStatesBlock.Flat = 1;
            }
            _vboStatesChanged = true;
        }
        
        if (setAll || (newStates->AlphaTest != _lastStates.AlphaTest))
        {
            if (newStates->AlphaTest == false)
            {
                _vboStatesBlock.ATest = 0;
            }
            else
            {
                _vboStatesBlock.ATest = 1;
            }
            _vboStatesChanged = true;
        }
    }
    else
    {
        if (setAll || (newStates->Stipple != _lastStates.Stipple))
        {
        }

        if (setAll || (newStates->Fog != _lastStates.Fog) ||
            (newStates->FogLength != _lastStates.FogLength) ||
            (newStates->FogStart != _lastStates.FogStart) )
        {
            if (newStates->Fog)
            {
                glEnable(GL_FOG);
                glFogf(GL_FOG_DENSITY, 1.0);
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glHint(GL_FOG_HINT, GL_DONT_CARE);

                float fcolors[4] = {0.0, 0.0, 0.0, 1.0};

                glFogfv(GL_FOG_COLOR, fcolors);
                
                glFogf(GL_FOG_START, newStates->FogStart);
                glFogf(GL_FOG_END, newStates->FogStart + newStates->FogLength);
            }
            else
            {
                glDisable(GL_FOG);
            }
        }
        
        if (setAll || (newStates->Shaded != _lastStates.Shaded))
        {
            if (newStates->Shaded)
                glShadeModel(GL_SMOOTH);
            else
                glShadeModel(GL_FLAT);
        }
        
        if (setAll || (newStates->Tex != _lastStates.Tex))
        {
            if (newStates->Tex)
            {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, newStates->Tex);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            }
            else
            {
                glDisable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, 0);
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            }
        }
        
        if (setAll || (newStates->AlphaTest != _lastStates.AlphaTest))
        {
            if (newStates->AlphaTest == false)
            {
                glDisable(GL_ALPHA_TEST);
            }
            else
            {
                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, 0.0);
            }
        }
    }
    
    if (setAll || (newStates->DepthTest != _lastStates.DepthTest))
    {
        if (newStates->DepthTest)
        {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
    }

    if (setAll || (newStates->Tex != _lastStates.Tex)
               || (newStates->LinearFilter != _lastStates.LinearFilter)
               || (newStates->TexMip != _lastStates.TexMip) )
    {
        if (newStates->Tex)
        {
            if (newStates->LinearFilter)
            {
                // This per-bind stamp used to force plain GL_LINEAR, silently
                // discarding the mipmap MIN filter AllocTexture set at upload
                // time — mipmaps/anisotropy were dead in the live 3D path
                // (distant terrain shimmered). Textures with a chain now keep
                // trilinear sampling; chainless ones stay on GL_LINEAR.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                newStates->TexMip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            }
            else
            {
                // Authentic unfiltered look (gfx.filter=off / ZEROTRACY
                // colorkey sprites) — deliberately no mip sampling here.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            }
        }
    }
    
    if (setAll || (newStates->SrcBlend != _lastStates.SrcBlend)
                   || (newStates->DstBlend != _lastStates.DstBlend))
    {
        glBlendFunc(newStates->SrcBlend, newStates->DstBlend);
    }

    if (setAll || (newStates->TexBlend != _lastStates.TexBlend))
    {
        if (newStates->TexBlend == 0)
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        else if (newStates->TexBlend == 1)
        {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        }
        else if (newStates->TexBlend == 2)
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    if (setAll || (newStates->AlphaBlend != _lastStates.AlphaBlend))
    {
        if (newStates->AlphaBlend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }

    if (setAll || (newStates->Zwrite != _lastStates.Zwrite))
    {
        if (newStates->Zwrite)
            glDepthMask(GL_TRUE);
        else
            glDepthMask(GL_FALSE);
    }

    if (setAll < 2)
        _lastStates = _states;
}

void GFXEngine::RenderingMeshOld(TRenderNode *nod)
{
    if ( !_sceneBeginned )
        return;
    
    if (!nod)
        return;
    
    TMesh *mesh = nod->Mesh;
    
    if (!mesh)
        return;
    
    uint32_t flags = nod->Flags;

    _states.Shaded = false;
    _states.Stipple = false;
    _states.SrcBlend = GL_ONE;
    _states.DstBlend = GL_ZERO;
    _states.TexBlend = 0; //REPLACE
    _states.AlphaBlend = false;
    _states.Zwrite = true;
    _states.Tex = 0;
    _states.TexMip = false;
    _states.LinearFilter = (_filter != 0);
    _states.Fog = false;
    _states.AlphaTest = false;
    _states.AFog = false;
    _states.Prog = TShaderProg();
    
    bool useComputedColor = false;

    if ( flags & RFLAGS_TEXTURED )
    {
        if (nod->Tex)
        {
            _states.Tex = nod->Tex->hwTex;
            _states.TexMip = nod->Tex->hwTexMips;
        }
    }

    if ( flags & RFLAGS_SHADED )
    {
        _states.TexBlend = 2; //MODULATE
        _states.Shaded = true;
    }
    
    if ( flags & RFLAGS_FOG )
    {
        _states.Fog = true;
        _states.FogStart = nod->FogStart;
        _states.FogLength = nod->FogLength;
    }
    
    if ( flags & RFLAGS_LUMTRACY )
    {
        if ( !_zbuf_when_tracy )
            _states.Zwrite = false;

        if ( can_destblend )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_ONE;
            _states.DstBlend = GL_ONE;
            _states.Shaded = false;
        }
        else if ( can_srcblend )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_SRC_ALPHA;
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
            _states.Shaded = false;
        }
        else if ( can_stippling )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_SRC_ALPHA;//D3DBLEND_SRCALPHA;, 
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;//D3DBLEND_INVSRCALPHA;
            _states.Stipple = true;
            _states.Shaded = false;
        }
    }
    else if ( flags & RFLAGS_ZEROTRACY )
    {
        _states.AlphaTest = true;        

        if ( _pixfmt->BytesPerPixel != 1 )
        {
            _states.SrcBlend = GL_SRC_ALPHA;//D3DBLEND_SRCALPHA;, 
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;//D3DBLEND_INVSRCALPHA;
        }

        _states.AlphaBlend = true;
        _states.LinearFilter = false;
        _states.TexBlend = 2; //MODULATE
    }

    if (flags & RFLAGS_SKY)
    {
        _states.Fog = false;
        _states.Shaded = true;
    }
    else if (flags & RFLAGS_FALLOFF)
    {
        float transDist = System::IniConf::GfxSkyDistance.Get<int>();
        float transLen = System::IniConf::GfxSkyLength.Get<int>();

        for(TVertex &v : mesh->Vertexes)
        {
            v.ComputedColor = v.Color;

            float distance = nod->TForm.Transform( v.Pos ).XZ().length();

            if (distance > transDist)
            {
                float prc = (distance - transDist) / transLen;
                if (prc > 1.0)
                    prc = 1.0;
                if (prc < 0.0)
                    prc = 0.0;

                prc = (1.0 - prc);

                v.ComputedColor.a *= prc;

                useComputedColor = true;
            }
        }

        if (useComputedColor)
        {
            _states.Zwrite = true;
            _states.AlphaBlend = true;
            _states.TexBlend = 2; // MODULATE
            _states.SrcBlend = GL_SRC_ALPHA;

            if (flags & RFLAGS_LUMTRACY)
                _states.DstBlend = GL_ONE;
            else
                _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;

            _states.Shaded = true;
            _states.Stipple = false;
        }
    }
    
    if (flags & RFLAGS_COMPUTED_COLOR)
        useComputedColor = true;
    
    if (flags & RFLAGS_DISABLE_ZWRITE)
        _states.Zwrite = false;

    if (flags & RFLAGS_FX_ALPHA) // reforge fx only; never set on original meshes
    {
        _states.AlphaBlend = true;
        _states.AlphaTest = false;
        _states.Stipple = false;
        _states.Shaded = false;
        _states.TexBlend = 1;
        _states.SrcBlend = GL_SRC_ALPHA;
        _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
        _states.Zwrite = false;
    }

    SetRenderStates(0);

    SetModelViewMatrix( nod->TForm );
  
    glVertexPointer(3, GL_FLOAT, sizeof(TVertex), &mesh->Vertexes[0].Pos);
    
    if (useComputedColor)
        glColorPointer(4, GL_FLOAT, sizeof(TVertex), &mesh->Vertexes[0].ComputedColor);
    else
        glColorPointer(4, GL_FLOAT, sizeof(TVertex), &mesh->Vertexes[0].Color);
    
    if (flags & RFLAGS_TEXTURED)
    {
        if ( (flags & RFLAGS_DYNAMIC_TEXTURE) && nod->coordsID >= 0 )
            glTexCoordPointer(2, GL_FLOAT, sizeof(tUtV), nod->Mesh->CoordsCache.at( nod->coordsID ).Coords.data());
        else
            glTexCoordPointer(2, GL_FLOAT, sizeof(TVertex), &mesh->Vertexes[0].TexCoord);
    }
    
    glDrawElements(GL_TRIANGLES, mesh->Indixes.size(), GLINDEXTYPE, mesh->Indixes.data());
}

void GFXEngine::RenderingMesh(TRenderNode *nod)
{
    if ( !_sceneBeginned )
        return;
    
    if (!nod)
        return;
    
    TMesh *mesh = nod->Mesh;
    
    if (!mesh)
        return;
    
    uint32_t flags = nod->Flags;

    _states.Shaded = false;
    _states.Stipple = false;
    _states.SrcBlend = GL_ONE;
    _states.DstBlend = GL_ZERO;
    _states.TexBlend = 0; //REPLACE
    _states.AlphaBlend = false;
    _states.Zwrite = true;
    _states.Tex = 0;
    _states.TexMip = false;
    _states.LinearFilter = (_filter != 0);
    _states.Fog = false;
    _states.AlphaTest = false;
    _states.AFog = false;
    _states.Prog = _stdShaderProg;
    
    if ( flags & RFLAGS_TEXTURED )
    {
        if (nod->Tex)
        {
            _states.Tex = nod->Tex->hwTex;
            _states.TexMip = nod->Tex->hwTexMips;
        }
    }

    if ( flags & RFLAGS_SHADED )
    {
        _states.TexBlend = 2; //MODULATE
        _states.Shaded = true;
    }
    
    if ( flags & RFLAGS_FOG )
    {
        _states.Fog = true;
        _states.FogStart = nod->FogStart;
        _states.FogLength = nod->FogLength;
    }
    
    if ( flags & RFLAGS_LUMTRACY )
    {
        if ( !_zbuf_when_tracy )
            _states.Zwrite = false;

        if ( can_destblend )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_ONE;
            _states.DstBlend = GL_ONE;
            _states.Shaded = false;
        }
        else if ( can_srcblend )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_SRC_ALPHA;
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
            _states.Shaded = false;
        }
        else if ( can_stippling )
        {
            _states.AlphaBlend = true;
            _states.TexBlend = 1; //MODULATEALPHA;
            _states.SrcBlend = GL_SRC_ALPHA;//D3DBLEND_SRCALPHA;, 
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;//D3DBLEND_INVSRCALPHA;
            _states.Stipple = true;
            _states.Shaded = false;
        }
    }
    else if ( flags & RFLAGS_ZEROTRACY )
    {
        _states.AlphaTest = true;        

        if ( _pixfmt->BytesPerPixel != 1 )
        {
            _states.SrcBlend = GL_SRC_ALPHA;//D3DBLEND_SRCALPHA;, 
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;//D3DBLEND_INVSRCALPHA;
        }

        _states.AlphaBlend = true;
        _states.LinearFilter = false;
        _states.TexBlend = 2; //MODULATE
    }

    if (flags & RFLAGS_SKY)
    {
        _states.Fog = false;
        _states.Shaded = true;
    }
    else if (flags & RFLAGS_FALLOFF)
    {
        _states.Zwrite = true;
        _states.AlphaBlend = true;
        _states.TexBlend = 2; // MODULATE
        _states.SrcBlend = GL_SRC_ALPHA;

        if (flags & RFLAGS_LUMTRACY)
            _states.DstBlend = GL_ONE;
        else
            _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;

        _states.Shaded = true;
        _states.Stipple = false;
        
        _states.AFog = true; 
        _states.AFogStart = System::IniConf::GfxSkyDistance.Get<int>();
        _states.AFogLength = System::IniConf::GfxSkyLength.Get<int>();
    }
    
    if (flags & RFLAGS_DISABLE_ZWRITE)
        _states.Zwrite = false;

    if (flags & RFLAGS_FX_ALPHA) // reforge fx only; never set on original meshes
    {
        _states.AlphaBlend = true;
        _states.AlphaTest = false;
        _states.Stipple = false;
        _states.Shaded = false;
        _states.TexBlend = 1;
        _states.SrcBlend = GL_SRC_ALPHA;
        _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
        _states.Zwrite = false;
    }

    // Reforge: only meshes with modern material maps switch program. Sky and
    // additive (lumtracy) passes stay legacy - they are emitters, not lit.
    const bool v2 = _renderV2 && nod->Pbr && _v2ShaderProg.ID &&
                    !(flags & (RFLAGS_SKY | RFLAGS_LUMTRACY));
    if (v2)
        _states.Prog = _v2ShaderProg;
    // Reforge shadows: legacy receivers of the world frame (never at level 0).
    else if (_shadowFrame && ShadowLegacyReceiver(nod))
        _states.Prog = _shdStdProg;
    // Reforge ground: only tagged ground-atlas draws, only with UA_R2_GROUND.
    const bool gnd = !v2 && _groundOn && GroundUse(nod);
    if (gnd)
        _states.Prog = _gndShaderProg;
    // Reforge ATMOSPHERE: legacy mesh draws take the haze/grade program (never at ATMOS 0: no program exists).
    const bool atm = !v2 && !gnd && _atmStdProg.ID && AtmosLegacyUse(nod, flags);
    if (atm)
        _states.Prog = _atmStdProg;

    _states.DataBuf = nod->Mesh->glDataBuf;
    _states.IndexBuf = nod->Mesh->glIndexBuf;

    if (GlowPass::Replaying())
        GlowPass::ReplayStates(_states, nod);
    
    SetRenderStates(0);
    
    SetModelViewMatrix(nod->TForm);

    if (v2)
        RenderingMeshV2Setup(nod);
    else if (gnd)
        RenderingMeshGroundSetup(nod);
    else if (atm)
    {
        AtmosStdSetup(nod);
        ShadowAtmDraw(); // shadows on atmos legacy draws (no GL call at SHADOWS 0)
    }

    Glext::GLVertexAttribPointer(_lastStates.Prog.PosLoc, 3, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, Pos));
    
    if (flags & RFLAGS_COMPUTED_COLOR)
        Glext::GLVertexAttribPointer(_lastStates.Prog.ColorLoc, 4, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, ComputedColor));
    else
        Glext::GLVertexAttribPointer(_lastStates.Prog.ColorLoc, 4, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, Color));
    
    if (flags & RFLAGS_TEXTURED)
    {
        if ( (flags & RFLAGS_DYNAMIC_TEXTURE) && nod->coordsID >= 0 )
            Glext::GLVertexAttribPointer(_lastStates.Prog.UVLoc, 2, GL_FLOAT, GL_FALSE,  sizeof(tUtV), (void *)(size_t)nod->Mesh->CoordsCache.at( nod->coordsID ).BufferPos);
        else
            Glext::GLVertexAttribPointer(_lastStates.Prog.UVLoc, 2, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, TexCoord));
    }
    
    CommitUBOParameters();    
    glDrawElements(GL_TRIANGLES, mesh->Indixes.size(), GLINDEXTYPE, NULL);
}

void GFXEngine::RenderNode(TRenderNode *node)
{
    if (!node)
        return;
    
    switch(node->Type)
    {
        case TRenderNode::TYPE_MESH:
            if (_vbo)
                RenderingMesh(node);
            else
                RenderingMeshOld(node);
            break;
            
        case TRenderNode::TYPE_PARTICLE:
        {
            if (_vbo)
                RenderingMesh(node);
            else
                RenderingMeshOld(node);
        }
            break;
            
        default:
            break;
    }
}

void GFXEngine::QueueRenderMesh(TRenderNode *nod)
{
    if (!nod)
        return;
    
    TMesh *mesh = nod->Mesh;    
    if (!mesh)
        return;
    
    uint32_t flags = nod->Flags;
    
    if (flags & RFLAGS_SKY)
        _renderSkyBoxList.push_back(nod);
    else if (flags & RFLAGS_ZEROTRACY)
        _renderZeroTracyList.push_back(nod);
    else if (flags & RFLAGS_LUMTRACY)
        _renderLumaTracyList.push_back(nod);
    else
        _renderSolidList.push_back(nod);
}

void GFXEngine::Rasterize(uint32_t RasterEtapes)
{
    if (RasterEtapes & RASTER_SKY)
    {
        _renderSkyBoxList.sort(TRenderNode::CompareSolid);
        
        while(!_renderSkyBoxList.empty())
        {
            RenderNode( _renderSkyBoxList.front() );
            _renderSkyBoxList.pop_front();
        }
    }
    
    if (RasterEtapes & RASTER_SOLID)
    {
        if (_shadowWorld) // reforge shadows: world frame, level > 0 only
            ShadowPass();

        _renderSolidList.sort(TRenderNode::CompareSolid);
        
        while(!_renderSolidList.empty())
        {
            RenderNode( _renderSolidList.front() );
            _renderSolidList.pop_front();
        }
    }
    
    if (RasterEtapes & RASTER_ZEROTR)
    {
        _renderZeroTracyList.sort(TRenderNode::CompareSolid);
        
        while(!_renderZeroTracyList.empty())
        {
            RenderNode( _renderZeroTracyList.front() );
            _renderZeroTracyList.pop_front();
        }
    }
    
    if (RasterEtapes & RASTER_LUMATR)
    {
        _renderLumaTracyList.sort(TRenderNode::CompareTransparent);
        
        while(!_renderLumaTracyList.empty())
        {
            GlowPass::Collect( _renderLumaTracyList.front() );
            RenderNode( _renderLumaTracyList.front() );
            _renderLumaTracyList.pop_front();
        }
    }
}


void GFXEngine::raster_func207(int id, TileMap *t)
{
    _tiles[id] = t;
}

TileMap *GFXEngine::raster_func208(int id)
{
    return _tiles[id];
}

int GFXEngine::raster_func208(TileMap *t)
{
    if ( t )
    {
        for (int i = 0; i < 256; i++)
        {
            if (_tiles[i] == t)
                return i;
        }
    }
    return -1;
}

void GFXEngine::ProcessDrawSeq(const CmdStream &drawSeq, const CmdIncludes *includes)
{
    struct CmdStkEntr
    {
        const CmdStream &seq;
        const int32_t pos;
    };
    
    int v11;

    int bytesPerColor = Screen()->format->BytesPerPixel;

    int32_t curPos = 0;
    const CmdStream *curStream = &drawSeq;

    // Everything in this interpreter stays in LOGICAL pixels - including the
    // negative-x wrap below, hence the divide.
    const int uiN = UiScale();
    int w_pixels = Screen()->pitch / bytesPerColor / uiN;
    TileMap *tile = NULL;

    int x_out = 0;
    int y_out = 0;

    int x_out_txt = 0;
    int y_out_txt = 0;
    int txt_flag = 0;

    int y_pos_line = 0;
    int x_pos_line = 0;

    int halfWidth = _resolution.x / 2;
    int halfHeight = _resolution.y / 2;

    int line_width = 0;
    int line_height = 0;

    //if v11 = 0 - clone first column of tile  (count = line_width)
    //if v11 = 1 - normal copy of tile
    v11 = 1;


    int x_off = 0;
    int y_off = 0;

    std::stack<CmdStkEntr> Stack;

    while ( 1 )
    {
        int v13 = FontUA::get_u8(*curStream, &curPos);

        if ( v13 )
        {
            Common::PointRect &chrr = tile->map[v13];

            int cpy_width;

            if ( line_width )
                cpy_width = line_width - x_off;
            else
                cpy_width = chrr.w - x_off;

            int cpy_height = line_height - y_off;

            //SDL_Rect srcR, dstR;
            Common::Rect dstR(x_out, y_out, x_out + cpy_width, y_out + cpy_height);            
            /*dstR.x = x_out;
            dstR.y = y_out;
            dstR.w = cpy_width;
            dstR.h = cpy_height;*/
            
            Common::Rect srcR;
            srcR.left = chrr.x + x_off;
            srcR.top = chrr.y + y_off;
            srcR.bottom = chrr.y + y_off + cpy_height;
            
            if (v11)
                srcR.right = chrr.x + x_off + cpy_width;
            else
                srcR.right = chrr.x + x_off + 1;

            // The one final SDL call of the tile interpreter. Source and
            // destination are multiplied by the SAME factor, edge to edge, so
            // DrawFill's repeat maths (dw/sw, dw%sw, the sh row wrap) comes out
            // identical - the pattern is the classic one at uiN x the density.
            SDL_Surface *sheet = UiSheetFor(tile);

            if ( sheet )
            {
                MarkScreen(dstR.left * uiN, dstR.top * uiN, (dstR.right - dstR.left) * uiN, (dstR.bottom - dstR.top) * uiN);
                DrawFill(sheet,
                         Common::Rect(srcR.left * uiN, srcR.top * uiN, srcR.right * uiN, srcR.bottom * uiN),
                         Screen(),
                         Common::Rect(dstR.left * uiN, dstR.top * uiN, dstR.right * uiN, dstR.bottom * uiN));
            }
            else
            {
                MarkScreen(dstR.left, dstR.top, dstR.right - dstR.left, dstR.bottom - dstR.top);
                DrawFill(tile->img->GetSwTex(), srcR, Screen(), dstR);
            }


            /*srcR.h = cpy_height;
            
            if (v11)
                srcR.w = cpy_width;
            else
                srcR.w = 1;
            
            for(int i = 0; i < cpy_width; i += srcR.w)
            {
                SDL_BlitSurface(tile->img->GetSwTex(), &srcR, Screen(), &dstR);
                dstR.x += srcR.w;
            }*/     

            line_width = 0;
            x_off = 0;
            x_out += cpy_width;
            v11 = 1;
        }
        else // 0
        {
            int opcode = FontUA::get_u8(*curStream, &curPos);

            switch ( opcode )
            {
            case 0: // End
                
                if (Stack.empty())
                {
                    DrawScreenText();
                    return;
                }
                
                curPos = Stack.top().pos;
                curStream = &Stack.top().seq;
                
                Stack.pop();
                break;

            case 1: // x pos from center
                x_out = halfWidth + FontUA::get_s16(*curStream, &curPos);
                x_pos_line = x_out;

                y_pos_line = y_out;
                y_off = 0;

                line_height = tile->h;
                break;

            case 2: // y pos from center
                y_out = halfHeight + FontUA::get_s16(*curStream, &curPos);
                x_pos_line = x_out;

                y_pos_line = y_out;
                y_off = 0;

                line_height = tile->h;
                break;

            case 3: //xpos
                x_out = FontUA::get_s16(*curStream, &curPos);
                if ( x_out < 0 )
                    x_out += w_pixels;

                x_pos_line = x_out;
                y_pos_line = y_out;

                line_height = tile->h;
                y_off = 0;
                break;

            case 4: //ypos
                y_out = FontUA::get_s16(*curStream, &curPos);
                if ( y_out < 0 )
                    y_out += _resolution.y;

                x_pos_line = x_out;
                y_pos_line = y_out;

                line_height = tile->h;
                y_off = 0;
                break;

            case 5: //add to x pos
                x_out += FontUA::get_s16(*curStream, &curPos);
                break;

            case 6: //add to y pos
                y_out += FontUA::get_s16(*curStream, &curPos);
                break;

            case 7: //next line
                y_out = (line_height - y_off) + y_pos_line;
                y_pos_line = y_out;
                x_out = x_pos_line;

                y_off = 0;
                line_height = tile->h;
                break;

            case 8: // Select tileset
                tile = _tiles[FontUA::get_u8(*curStream, &curPos)];
                break;

            case 9: // Include another cmdlist source
            {
                int azaza = FontUA::get_u8(*curStream, &curPos);
                Stack.push( {*curStream, curPos} );
                curPos = 0;
                curStream = includes->at(azaza);
            }
            break;

            case 10:
                line_width = FontUA::get_u8(*curStream, &curPos);

                v11 = 0;
                x_off = 0;

                break;

            case 11:

                line_width = FontUA::get_u8(*curStream, &curPos);

                v11 = 0;
                x_off = 0;

                line_width -= (x_out - x_pos_line);
                break;

            case 12: // Set x offset
                x_off = FontUA::get_u8(*curStream, &curPos);
                break;

            case 13: // Set x width
                line_width = FontUA::get_u8(*curStream, &curPos);
                break;

            case 14: // Set y offset
                y_off = FontUA::get_u8(*curStream, &curPos);
                break;

            case 15: // Set y height
                line_height = FontUA::get_u8(*curStream, &curPos);
                break;

            case 16: // Full reset tileset
                tile = _tiles[FontUA::get_u8(*curStream, &curPos)];
                line_height = tile->h;
                y_off = 0;
                break;

            case 17:
                line_width = FontUA::get_s16(*curStream, &curPos);
                v11 = 0;
                x_off = 0;
                line_width -= (x_out - x_pos_line);
                break;

            case 18: // Add text
            {
                int block_width = FontUA::get_s16(*curStream, &curPos);
                int flag = txt_flag | FontUA::get_u16(*curStream, &curPos);
                
                int32_t sz = FontUA::get_u16(*curStream, &curPos);

                std::string txt;
                txt.assign((const char *)(curStream->data()) + curPos, sz);
                

                curPos += sz + 1;
                AddScreenText(txt, x_out_txt, y_out_txt, block_width, tile->h, flag);
            }
            break;

            case 19: // Copy current x/y pos for text output
                x_out_txt = x_out;
                y_out_txt = y_out;
                break;

            case 20: // Add txtout flag
                txt_flag |= FontUA::get_u16(*curStream, &curPos);
                break;

            case 21: // Delete txtout flag
                txt_flag &= ~(FontUA::get_u16(*curStream, &curPos));
                break;

            case 22: // set color for font
            {
                int r = FontUA::get_u16(*curStream, &curPos);

                int g = FontUA::get_u16(*curStream, &curPos);

                int b = FontUA::get_u16(*curStream, &curPos);

                AddScreenText("", r, g, b, 0, 0x20);
            }
            break;
            }
        }
    }
}


void GFXEngine::raster_func210(const Common::FRect &arg)
{
    _clip = Common::Rect(   (arg.left + 1.0) * (_field_554 + -1.0),
                            (arg.top + 1.0) * (_field_558 + -1.0),
                            (arg.right + 1.0) * (_field_554 + -1.0),
                            (arg.bottom + 1.0) * (_field_558 + -1.0) );
    }

void GFXEngine::raster_func211(const Common::Rect &arg)
{
    _clip = Common::Rect(   _field_54c + arg.left,
                            _field_550 + arg.top,
                            _field_54c + arg.right,
                            _field_550 + arg.bottom );
}


void GFXEngine::BeginScene()
{
    SetRenderStates(2);
    
    SetProjectionMatrix( _frustum );
    SetModelViewMatrix( mat4x4f() );
    
    _sceneBeginned = 1;
}

// Draw transparent
void GFXEngine::EndScene()
{
    GlowPass::Apply(); // UA_GFX_GLOW: no-op at level 0
    _sceneBeginned = 0;
    _shadowWorld = false;
    _shadowFrame = false;
    
    _renderSkyBoxList.clear();
    _renderSolidList.clear();
    _renderZeroTracyList.clear();
    _renderLumaTracyList.clear();
    
    _renderNodesCache.Rewind();
}

size_t GFXEngine::raster_func217(SDL_Color color)
{
    //if ( !ColorCmp(color, Color(255, 255, 255, 255) ) )
    _field_4 = color;

    return 0;
}

// UI graphics: k x copy of an 8-bit region mask (index compare only, no palette needed).
// Classical EPX scalers (Scale2x, Scale3x; 4 = Scale2x twice) instead of nearest, so the
// region cut follows diagonals instead of 1998-pixel stairs. Exact: every output index is
// one of its source neighbours, so no region id is ever invented.
static std::vector<uint8_t> uiEpx2(const std::vector<uint8_t> &s, int w, int h)
{
    std::vector<uint8_t> d((size_t)w * h * 4);
    auto at = [&](int x, int y) { x = std::max(0, std::min(w - 1, x)); y = std::max(0, std::min(h - 1, y)); return s[(size_t)y * w + x]; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t E = at(x, y), B = at(x, y - 1), D = at(x - 1, y), F = at(x + 1, y), H = at(x, y + 1);
            uint8_t *o0 = &d[(size_t)(y * 2) * w * 2 + x * 2], *o1 = o0 + (size_t)w * 2;
            if ( B != H && D != F )
            {
                o0[0] = D == B ? D : E; o0[1] = B == F ? F : E;
                o1[0] = D == H ? D : E; o1[1] = H == F ? F : E;
            }
            else
                o0[0] = o0[1] = o1[0] = o1[1] = E;
        }
    return d;
}

static std::vector<uint8_t> uiEpx3(const std::vector<uint8_t> &s, int w, int h)
{
    std::vector<uint8_t> d((size_t)w * h * 9);
    auto at = [&](int x, int y) { x = std::max(0, std::min(w - 1, x)); y = std::max(0, std::min(h - 1, y)); return s[(size_t)y * w + x]; };
    const size_t W3 = (size_t)w * 3;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            uint8_t A = at(x - 1, y - 1), B = at(x, y - 1), C = at(x + 1, y - 1);
            uint8_t D = at(x - 1, y),     E = at(x, y),     F = at(x + 1, y);
            uint8_t G = at(x - 1, y + 1), H = at(x, y + 1), I = at(x + 1, y + 1);
            uint8_t o[9];
            std::fill(o, o + 9, E);
            if ( B != H && D != F )
            {
                o[0] = D == B ? D : E;
                o[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o[2] = B == F ? F : E;
                o[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o[6] = D == H ? D : E;
                o[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o[8] = H == F ? F : E;
            }
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    d[(size_t)(y * 3 + j) * W3 + x * 3 + i] = o[j * 3 + i];
        }
    return d;
}

static SDL_Surface *uiMaskFor(ResBitmap *bm, int k)
{
    if ( !bm || !bm->swTex || bm->swTex->format->BitsPerPixel != 8 )
        return NULL;
    if ( bm->uiMaskK && bm->uiMaskScale == k )
        return bm->uiMaskK;
    SDL_Surface *m = bm->swTex;
    SDL_Surface *o = SDL_CreateRGBSurfaceWithFormat(0, m->w * k, m->h * k, 8, SDL_PIXELFORMAT_INDEX8);
    if ( !o )
        return NULL;
    SDL_LockSurface(m); SDL_LockSurface(o);
    std::vector<uint8_t> src((size_t)m->w * m->h);
    for (int y = 0; y < m->h; y++)
        memcpy(&src[(size_t)y * m->w], (const uint8_t *)m->pixels + y * m->pitch, m->w);
    std::vector<uint8_t> up;
    if ( k == 2 )
        up = uiEpx2(src, m->w, m->h);
    else if ( k == 3 )
        up = uiEpx3(src, m->w, m->h);
    else if ( k == 4 )
        up = uiEpx2(uiEpx2(src, m->w, m->h), m->w * 2, m->h * 2);
    for (int y = 0; y < o->h; y++)
    {
        uint8_t *d = (uint8_t *)o->pixels + y * o->pitch;
        if ( !up.empty() )
            memcpy(d, &up[(size_t)y * o->w], o->w);
        else
        {
            const uint8_t *s = (const uint8_t *)m->pixels + (y / k) * m->pitch;
            for (int x = 0; x < o->w; x++)
                d[x] = s[x / k];
        }
    }
    SDL_UnlockSurface(o); SDL_UnlockSurface(m);
    if ( bm->uiMaskK )
        SDL_FreeSurface(bm->uiMaskK);
    bm->uiMaskK = o;
    bm->uiMaskScale = k;
    return o;
}

void GFXEngine::raster_func218(rstr_218_arg *arg)
{
    Common::Rect sRect( (arg->rect1.left + 1.0) * (arg->bitm_intern->width / 2),
                        (arg->rect1.top + 1.0) * (arg->bitm_intern->height / 2),
                        (arg->rect1.right + 1.0) * (arg->bitm_intern->width / 2),
                        (arg->rect1.bottom + 1.0) * (arg->bitm_intern->height / 2) );

    Common::Rect dRect( (arg->rect2.left + 1.0) * _field_554,
                        (arg->rect2.top + 1.0) * _field_558,
                        (arg->rect2.right + 1.0) * _field_554,
                        (arg->rect2.bottom + 1.0) * _field_558 );

    const int n = UiScale();

    // BlitScaleMasked is a 16.16 nearest scaler, so the destination may grow on
    // its own; the mask is indexed from the source, which stays 1x.
    {
        Common::Rect m(dRect.left * n, dRect.top * n, dRect.right * n, dRect.bottom * n);
        if ( m.IsEmpty() || !m.IsValid() )
            MarkScreen(0, 0, ScreenSurface->w, ScreenSurface->h);   // BlitScaleMasked then fills the whole dst
        else
            MarkScreen(m.left, m.top, m.Width(), m.Height());
    }
    // UI >= 2: the region picture's twin (campaign map rollover/finished/enabled), cut by a k x mask.
    if ( n > 1 )
    {
        int k = 0;
        SDL_Surface *hi = uiArtFor(arg->bitm_intern, Screen(), &k);
        SDL_Surface *mk = ( hi && k > 1 ) ? uiMaskFor(arg->bitm_intern2, k) : NULL;
        if ( mk )
        {
            BlitScaleMasked(hi, Common::Rect(sRect.left * k, sRect.top * k, sRect.right * k, sRect.bottom * k),
                            mk, arg->flg, Screen(),
                            Common::Rect(dRect.left * n, dRect.top * n, dRect.right * n, dRect.bottom * n));
            return;
        }
    }
    BlitScaleMasked(arg->bitm_intern->swTex, sRect, arg->bitm_intern2->swTex, arg->flg, Screen(),
                    Common::Rect(dRect.left * n, dRect.top * n, dRect.right * n, dRect.bottom * n));
}

void GFXEngine::raster_func221(const Common::Rect &arg)
{
    _inverseClip.left = _field_54c + arg.left;
    _inverseClip.top = _field_550 + arg.top;
    _inverseClip.right = _field_54c + arg.right;
    _inverseClip.bottom = _field_550 + arg.bottom;
}

void GFXEngine::BeginFrame()
{
    ClearScreenSurface();

    ClearTextLayer();
    
    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);
    
    bool saved = _states.Zwrite;
    
    _states.Zwrite = true;
    SetRenderStates(0);
    
    if (_colorEffects)
    {
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
    
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    }

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    _states.Zwrite = saved;
    SetRenderStates(0);
    
    if (!_vbo)
    {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
    }
    
    _states.Prog = _stdShaderProg;
}

void GFXEngine::EndFrame()
{       
    if (_colorEffects > 1)
    {
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);   
        DrawFBO();
    }    

    if ( _scrTiled && Gui::Root::Instance.SoftDrawsAnything() )
        MarkScreen(0, 0, ScreenSurface->w, ScreenSurface->h);   // software widgets: whole surface
    Gui::Root::Instance.Draw(Screen());
    DrawScreenSurface();
    DrawTextLayer();
    Gui::Root::Instance.HwCompose();
    
    if (_colorEffects == 1)
    {
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
        DrawFBO();
    }

    MaybeSaveShot();

    {
        Perf::Scope perfSwap(Perf::SWAP);
        System::Flip();
    }

    LogFrameTime();
}

// Dev aid: UA_SHOT_EVERY=<n> dumps every n-th finished frame to
// <cwd>/shot_XXXXX.ppm. Reads back our own framebuffer only - safe on a shared
// desktop, needs no permissions, works regardless of window focus.
//
// Called from BOTH frame-completion paths. EndFrame() alone is not enough: the
// SHELL (title screen, menus, briefing - i.e. every screen made of the tile
// art) finishes its frames through draw2DandFlush() and reaches EndFrame()
// about once per session, so a shell run used to capture exactly one frame,
// taken during startup before the window had even been sized. Measured: 20 s on
// the title screen produced 1 shot at UA_SHOT_EVERY=10.
void GFXEngine::MaybeSaveShot()
{
    {   // effects stage UA_FX_STAGE_SHOT: shots named by stage time (empty unless the stage runs)
        std::string fxShot = ReforgeFx::PendingShot();
        if (!fxShot.empty())
            SaveScreenshot(fxShot);
        std::string skyShot = ReforgeSky::PendingShot(); // SKY UA_SKY_CYCLE (empty unless set)
        if (!skyShot.empty())
            SaveScreenshot(skyShot);
        std::string trShot = ReforgeTrails::PendingShot(); // gfx TRAILS UA_TRAILS_STAGE_SHOT
        if (!trShot.empty())
            SaveScreenshot(trShot);
    }

    static int shotEvery = -1;
    static uint32_t shotStart = 0; // UA_SHOT_START=<frame>: no shots before it (clip captures)
    if ( shotEvery < 0 )
    {
        const char *v = getenv("UA_SHOT_EVERY");
        shotEvery = v ? atoi(v) : 0;
        const char *s0 = getenv("UA_SHOT_START");
        shotStart = s0 ? (uint32_t)std::max(0, atoi(s0)) : 0;
    }

    if ( shotEvery <= 0 )
        return;

    static uint32_t shotFrame = 0;
    if ( shotFrame >= shotStart && (shotFrame % shotEvery) == 0 )
        SaveScreenshot( fmt::sprintf("shot_%05u", shotFrame) );
    shotFrame++;
}

void GFXEngine::SetColorEffectsPowers(const std::vector<ColorFx> &arg)
{    
    if (arg.empty())
    {
        _normClr = vec3d(1.0, 1.0, 1.0);
        _invClr = vec3d(0.0, 0.0, 0.0);
    }
    else
    {
        _normClr = vec3d(0.0, 0.0, 0.0);
        _invClr = vec3d(0.0, 0.0, 0.0);

        for (ColorFx fx : arg)
        {
            switch(fx.Id)
            {
                case 4:
                case 5:
                case 7:
                    _invClr += _clrEff.at( fx.Id ) * fx.Pwr;
                    break;
                default:
                    _normClr += _clrEff.at( fx.Id ) * fx.Pwr;
                    break;
            }
        }
    }
}

bool GFXEngine::AllocTexture(ResBitmap *bitm)
{
    if (bitm->swTex && !bitm->hwTex)
    {
        glGenTextures(1, &bitm->hwTex);

        if (!bitm->hwTex)
        {
            return false;
        }

        _states.Tex = bitm->hwTex;
        _states.TexMip = false; // params are set explicitly right below
        SetRenderStates(0);

        bool mips = _mipMaps && Glext::GLGenerateMipmap;
        bitm->hwTexMips = mips;

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        if (mips && _maxAniso > 1.0f)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, _maxAniso);

        // hd-textures/ override replaces only the GL upload; the game keeps
        // working in original width/height (UVs are normalized).
        SDL_Surface *src = bitm->hdTex ? bitm->hdTex : bitm->swTex;
        Perf::Scope perfTex(Perf::TEX);
        bitm->hwBytes = (uint32_t)Perf::RgbaBytes(src->w, src->h, mips);
        bitm->hwHd = false;
        Perf::TexAlloc(false, bitm->hwBytes);

        if (src->format->format == _pixfmt->format)
        {
            SDL_LockSurface(src);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src->w, src->h, 0, _glPixfmt, _glPixtype, src->pixels);
            SDL_UnlockSurface(src);
        }
        else
        {
            SDL_Surface *conv = ConvertSDLSurface(src, _pixfmt);

            SDL_LockSurface(conv);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, conv->w, conv->h, 0, _glPixfmt, _glPixtype, conv->pixels);
            SDL_UnlockSurface(conv);

            SDL_FreeSurface(conv);
        }

        if (mips)
            Glext::GLGenerateMipmap(GL_TEXTURE_2D);
    }

    return true;
}

void GFXEngine::ForgetTexture(uint32_t tex)
{
    if ( !tex )
        return;

    if ( _lastStates.Tex == tex )
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        _lastStates.Tex = 0;
        _lastStates.TexMip = false;
    }

    if ( _states.Tex == tex )
    {
        _states.Tex = 0;
        _states.TexMip = false;
    }
}

void GFXEngine::ForgetBuffer(uint32_t buf)
{
    if ( !buf || !_vbo )
        return;

    if ( _lastStates.DataBuf == buf )
    {
        Glext::GLBindBuffer(GL_ARRAY_BUFFER, 0);
        _lastStates.DataBuf = 0;
    }

    if ( _lastStates.IndexBuf == buf )
    {
        Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        _lastStates.IndexBuf = 0;
    }

    if ( _states.DataBuf == buf )
        _states.DataBuf = 0;

    if ( _states.IndexBuf == buf )
        _states.IndexBuf = 0;
}

void GFXEngine::FreeTexture(ResBitmap *bitm)
{
    if ( bitm->hwTex )
    {
        // Level teardown frees every bitmap of the level that just ended; this
        // is the site that used to leave the cache naming a dead texture.
        ForgetTexture(bitm->hwTex);
        glDeleteTextures(1, &bitm->hwTex);
        Perf::TexAlloc(bitm->hwHd, -(int64_t)bitm->hwBytes);
    }

    bitm->hwTex = 0;
    bitm->hwTexMips = false;
    bitm->hwBytes = 0;
}

void GFXEngine::display_func271(IDVPair *stak)
{

}

void GFXEngine::win3d_func274__sub0(FSMgr::FileHandle *fil)
{
    int bf_w = 0, bf_h = 0;

    uint8_t *buf = MakeScreenCopy(&bf_w, &bf_h);

    if (buf && bf_w && bf_h)
    {
        fil->printf("P6\n#YPA screenshot\n%d %d\n255\n", bf_w, bf_h);

        int lwidth = 3 * bf_w;

        for (int j = 0; j < bf_h; j++)
        {
            uint8_t *ln = buf + lwidth * (bf_h - 1 - j);

            for (int i = 0; i < bf_w; i++)
            {
                uint8_t *px = ln + i * 3;

                uint8_t r = px[0];
                uint8_t g = px[1];
                uint8_t b = px[2];

                fil->writeU8(r);
                fil->writeU8(g);
                fil->writeU8(b);
            }
        }

        free(buf);
    }
}

void GFXEngine::SaveScreenshot(const std::string & screenName)
{
    // UA_SHOT_DIR=<disk dir>: write there instead of the (shared) game CWD.
    FSMgr::FileHandle *fil = NULL;
    const char *shotDir = getenv("UA_SHOT_DIR");
    if ( shotDir && shotDir[0] )
    {
        fil = new FSMgr::FileHandle(fmt::sprintf("%s/%s.ppm", shotDir, screenName), "wb");
        if ( !fil->OK() )
        {
            delete fil;
            fil = NULL;
        }
    }
    else
        fil = uaOpenFileAlloc(fmt::sprintf("%s.ppm", screenName), "wb");
    if ( fil )
    {
        win3d_func274__sub0(fil);
        delete fil;
    }
}


void GFXEngine::windd_func320(IDVPair *)
{
}

void GFXEngine::windd_func321(IDVPair *)
{
}





char * GFXEngine::windd_func322__sub0(const char *box_title, const char *box_ok, const char *box_cancel, const char *box_startText, uint32_t timer_time, void (*timer_func)(int, int, int), void *timer_context, int replace, int maxLen)
{
    dprintf("MAKE ME %s\n","windd_func322__sub0");
    return NULL;
}

//Show DLGBox with edit field and get entered value
void GFXEngine::windd_func322(windd_dlgBox *dlgBox)
{
    windd_func320(NULL);

    dlgBox->result = windd_func322__sub0(
                         dlgBox->title,
                         dlgBox->ok,
                         dlgBox->cancel,
                         dlgBox->startText,
                         dlgBox->time,
                         dlgBox->timer_func,
                         dlgBox->timer_context,
                         dlgBox->replace,
                         dlgBox->maxLen);

    windd_func321(NULL);
}



const std::vector<TGFXDeviceInfo>& GFXEngine::GetDevices()
{
    return _devices;
}

void GFXEngine::SetDeviceByGUID(const std::string &guid, bool writefile)
{
    std::string guidWrite = guid;
    
    bool found = false;
    for( TGFXDeviceInfo &dev: _devices )
    {
        if (dev.guid == guid)
        {
            dev.isCurrent = true;
            found = true;
        }
        else
            dev.isCurrent = false;
    }
    
    if ( !found )
    {
        for( TGFXDeviceInfo &dev: _devices )
        {
            if (dev.guid == "<primary>")
            {
                dev.isCurrent = true;
                found = true;
                guidWrite = "<primary>";
            }
        }
        
        for( TGFXDeviceInfo &dev: _devices )
        {
            if (dev.guid == "<software>")
            {
                dev.isCurrent = true;
                found = true;
                guidWrite = "<software>";
            }
        }
        
        if (!_devices.empty())
        {
            TGFXDeviceInfo &dev = _devices[0];
            found = true;
            dev.isCurrent = true;
            guidWrite = dev.guid;
        }
    }
    
    if (writefile)
    {
        if (guidWrite == "<primary>" || guidWrite == "<software>")
            out_guid_to_file("env/guid3d.def", guidWrite);
        else
            out_guid_to_file("env/guid3d.def", "<error>");
    }
}



void GFXEngine::setWDD_cursor(int mode)
{
}

void GFXEngine::setWDD_disLowRes(int arg)
{
}

void GFXEngine::setWDD_16bitTex(int arg)
{
    out_yes_no_status("env/txt16bit.def", arg);
}

void GFXEngine::setWDD_drawPrim(int arg)
{
    out_yes_no_status("env/drawprim.def", arg);
}



int GFXEngine::getWDD_16bitTex()
{
    return _txt16bit;
}

int GFXEngine::getWDD_drawPrim()
{
    return _use_simple_d3d;
}

void GFXEngine::setW3D_texFilt(int arg)
{
    _filter = arg;
}

void GFXEngine::SetPalette(UA_PALETTE &newPal)
{
    _palette = newPal;
    
    _normClr = vec3d(1.0, 1.0, 1.0);
    _invClr = vec3d(0.0, 0.0, 0.0);
}

void GFXEngine::SetPen(SDL_Color pen)
{
    _field_4 = pen;
}

UA_PALETTE *GFXEngine::GetPalette()
{
    return &_palette;
}




void GFXEngine::draw2DandFlush()
{
    if (_colorEffects)
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    if ( _scrTiled && Gui::Root::Instance.SoftDrawsAnything() )
        MarkScreen(0, 0, ScreenSurface->w, ScreenSurface->h);   // software widgets: whole surface
    Gui::Root::Instance.Draw(Screen());
    DrawScreenSurface();
    DrawTextLayer();
    Gui::Root::Instance.HwCompose();

    MaybeSaveShot();

    ClearScreenSurface();
    ClearTextLayer();

    if (_colorEffects)
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, _fbo);
}

void GFXEngine::matrixAspectCorrection(mat3x3 &inout, bool invert)
{
    if (invert)
    {
        inout.m00 *= _corrIW;
        inout.m01 *= _corrIW;
        inout.m02 *= _corrIW;
        inout.m10 *= _corrIH;
        inout.m11 *= _corrIH;
        inout.m12 *= _corrIH;
    }
    else
    {
        inout.m00 *= _corrW;
        inout.m01 *= _corrW;
        inout.m02 *= _corrW;
        inout.m10 *= _corrH;
        inout.m11 *= _corrH;
        inout.m12 *= _corrH;
    }
}

void GFXEngine::getAspectCorrection(float &cW, float &cH, bool invert)
{
    if (invert)
    {
        cW = _corrIW;
        cH = _corrIH;
    }
    else
    {
        cW = _corrW;
        cH = _corrH;
    }
}

void GFXEngine::setFrustumClip(float _near, float _far)
{
    if (_near != _frustumNear || _far != _frustumFar)
        _setFrustumClip(_near, _far);
}

void GFXEngine::_setFrustumClip(float _near, float _far)
{
    //-z * frustum
    _frustumNear = _near;
    _frustumFar = _far;
    
    _frustum = mat4x4f::UAFrustum(_near, _far);
}

void GFXEngine::ConvAlphaPalette(UA_PALETTE *dst, const UA_PALETTE &src, bool transp)
{
    for (uint16_t i = 0; i < dst->size(); i++)
    {
        SDL_Color &c = (*dst)[i];
        c = src[i];
        c.a = 255;

        if (c.r == 255 && c.g == 255 && c.b == 0)
        {
            c.a = 0;
            c.r = 0;
            c.g = 0;
            c.b = 0;
        }
        else
        {
            if (!can_destblend && can_srcblend && transp)
            {
                int mx = (c.r >= c.g) ? (c.r > c.b ? c.r: c.b) : (c.g > c.b ? c.g : c.b);

                if (mx <= 8)
                {
                    c.r = 0;
                    c.g = 0;
                    c.b = 0;
                    c.a = 0;
                }
                else
                {
                    float prm = mx;
                    c.r = 255.0 * (c.r / prm);
                    c.g = 255.0 * (c.g / prm);
                    c.b = 255.0 * (c.b / prm);
                    c.a = mx;
                }
            }
            else
            {
                c.a = 255;
            }
        }
    }
}

SDL_Surface *GFXEngine::CreateSurfaceScreenFormat(int width, int height)
{
#if SDL_VERSION_ATLEAST(2,0,5)
    return SDL_CreateRGBSurfaceWithFormat(0, width, height, _pixfmt->BitsPerPixel, _pixfmt->format);
#else
    return SDL_CreateRGBSurface(0, width, height, _pixfmt->BitsPerPixel, _pixfmt->Rmask, _pixfmt->Gmask, _pixfmt->Bmask, _pixfmt->Amask );
#endif
}

SDL_Surface *GFXEngine::ConvertToScreenFormat(SDL_Surface *src)
{
    return ConvertSDLSurface(src, _pixfmt);
}

SDL_Surface * GFXEngine::ConvertSDLSurface(SDL_Surface *src, const SDL_PixelFormat * fmt)
{
#if (SDL_COMPILEDVERSION == SDL_VERSIONNUM(2, 0, 12))
    /***
     * Workaround for bug with convertation of surface with palette introduced
     * in SDL2 2.0.12 and fixed soon but after release.
     ***/
    if (src->format->BytesPerPixel == 1)
    {
#if SDL_VERSION_ATLEAST(2,0,5)
        SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h, fmt->BitsPerPixel, fmt->format);
#else
        SDL_Surface *tmp = SDL_CreateRGBSurface(0, src->w, src->h, fmt->BitsPerPixel, fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask );
#endif
        SDL_BlendMode blend = SDL_BLENDMODE_NONE;
        SDL_GetSurfaceBlendMode(src, &blend);
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(src, NULL, tmp, NULL);
        SDL_SetSurfaceBlendMode(src, blend);
        return tmp;
    }
    else
        return SDL_ConvertSurface(src, fmt, 0);
#else
    return SDL_ConvertSurface(src, fmt, 0);
#endif
}

// Graphics benchmark: run uncapped (no vsync, no fps limiter) to measure the real
// frame cost; off restores the configured cap (gfx.vsync / gfx.maxfps / UA_MAXFPS).
void GFXEngine::BenchUncap(bool on)
{
    if (on)
    {
        SDL_GL_SetSwapInterval(0);
        fpsLimitter(0);
        return;
    }
    int vsync = System::WindowHidden() ? 0 : System::IniConf::GfxVsync.Get<int>();
    if (vsync == 0)
        SDL_GL_SetSwapInterval(0);
    else if (vsync == 2 && SDL_GL_SetSwapInterval(-1) != -1)
        ;
    else
        SDL_GL_SetSwapInterval(1);
    fpsLimitter(System::IniConf::GfxMaxFps.Get<int>());
    if ( const char *mf = getenv("UA_MAXFPS") )
        fpsLimitter(atoi(mf));
}

void GFXEngine::fpsLimitter(int value)
{
    if (value > 1000)
        FpsMaxTicks = 0;
    else if (value <= 0)
        FpsMaxTicks = 0;
    else
        FpsMaxTicks = 1000/value;
}


}


TileMap::TileMap()
{
    img = NULL;
    h = 0;
}
    
TileMap::~TileMap()
{
    if (img)
        img->Delete();

    if (hiTex)
        SDL_FreeSurface(hiTex);
}

void TileMap::Draw(SDL_Surface *surface, const Common::Point &pos, uint8_t c)
{
    SDL_Rect src = map[c];
    SDL_Rect dst = pos;
    SDL_BlitSurface(img->GetSwTex(), &src, surface, &dst);
}

void TileMap::Draw(SDL_Surface *surface, const Common::PointRect &pos, uint8_t c)
{
    SDL_Rect src = map[c];
    if (src.w > pos.w)
        src.w = pos.w;
    if (src.h > pos.h)
        src.h = pos.h;    
    SDL_Rect dst = pos;
    SDL_BlitSurface(img->GetSwTex(), &src, surface, &dst);
}

void TileMap::Draw(SDL_Surface *surface, const Common::Rect &pos, uint8_t c)
{
    SDL_Rect src = map[c];
    if (src.w > pos.Width())
        src.w = pos.Width();
    if (src.h > pos.Height())
        src.h = pos.Height();    
    SDL_Rect dst = pos;
    SDL_BlitSurface(img->GetSwTex(), &src, surface, &dst);
}

void TileMap::Fill(SDL_Surface *surface, const Common::Rect &rect, uint8_t c)
{
    GFX::Engine.DrawFill(img->GetSwTex(), map[c], surface, rect);
}

void TileMap::Fill(SDL_Surface *surface, const Common::PointRect &rect, uint8_t c)
{
    GFX::Engine.DrawFill(img->GetSwTex(), map[c], surface, rect);
}

void TileMap::FillColumn(SDL_Surface *surface, const Common::Rect &rect, uint8_t c)
{
    Common::PointRect sPRect = map[c];
    sPRect.w = 1;
    GFX::Engine.DrawFill(img->GetSwTex(), sPRect, surface, rect);
}

void TileMap::FillColumn(SDL_Surface *surface, const Common::PointRect &rect, uint8_t c)
{
    Common::PointRect sPRect = map[c];
    sPRect.w = 1;
    GFX::Engine.DrawFill(img->GetSwTex(), sPRect, surface, rect);
}

int TileMap::GetWidth(uint8_t c) const
{
    return map[c].w;
}

int TileMap::GetWidth(const std::string &str) const
{
    int wdth = 0;
    for (uint8_t c : str)
        wdth += map[c].w;
    return wdth;
}

Common::Point TileMap::GetSize(uint8_t c) const
{
    return Common::Point(map[c].w, h);
}












namespace GFX
{

GfxMode::GfxMode(GfxMode &&g)
{
    w = std::move(g.w);
    h = std::move(g.h);
    bpp = std::move(g.bpp);
    mode = std::move(g.mode);
    name = std::move(g.name);
    windowed = std::move(g.windowed);
}

GfxMode::GfxMode(const GfxMode &g)
{
    w = g.w;
    h = g.h;
    bpp = g.bpp;
    mode = g.mode;
    name = g.name;
    windowed = g.windowed;
}

GfxMode::GfxMode(const Common::Point &sz)
: w(sz.x), h(sz.y)
{
    name = GenName(w, h);
}

GfxMode& GfxMode::operator=(const GfxMode &g)
{
    w = g.w;
    h = g.h;
    bpp = g.bpp;
    mode = g.mode;
    name = g.name;
    windowed = g.windowed;
    return *this;
}

GfxMode::operator bool() const
{
    if (w == 0 || h == 0)
        return false;
    return true;
}

bool GfxMode::operator==(const GfxMode &g) const
{
    return (w == g.w && h == g.h);
}

bool GfxMode::operator==(const Common::Point &g) const
{
    return (w == g.x && h == g.y);
}

bool GfxMode::operator!=(const GfxMode &g) const
{
    return w != g.w || h != g.h;
}

bool GfxMode::operator!=(const Common::Point &g) const
{
    return w != g.x || h != g.y;
}

bool GfxMode::SortCompare(const GfxMode &a, const GfxMode &b)
{
    if (a.w > b.w)
        return true;
    else if (a.w == b.w && a.h > b.h)
        return true;
    return false;
}

std::string GfxMode::GenName(int w, int h)
{
    return fmt::sprintf("%d x %d", w, h);
}




void GFXEngine::AddGfxMode(const GfxMode &md)
{
    for (const GfxMode &m : graphicsModes)
    {
        if ( m.w == md.w && m.h == md.h )
            return;
    }

    graphicsModes.push_back(md);
}


uint32_t GFXEngine::CursPix(uint8_t *data, int ofs, int bpp)
{
    switch (bpp)
    {
    case 1:
        return (data[ofs / 8] >> (7 - ofs % 8)) & 1;
    case 2:
        return (data[ofs / 4] >> ((3 - ofs % 4) << 1)) & 3;
    case 4:
        return (data[ofs / 2] >> ((1 - ofs % 2) << 2)) & 15;
    case 8:
        return data[ofs];
    case 16:
        return data[2 * ofs] | data[2 * ofs + 1] << 8;
    case 24:
        return data[3 * ofs] | data[3 * ofs + 1] << 8 | data[ 3 * ofs + 2] << 16;
    case 32:
        return data[4 * ofs] | data[4 * ofs + 1] << 8 | data[4 * ofs + 2] << 16 | data[4 * ofs + 3] << 24;
    }

    return 0;
}

SDL_Cursor *GFXEngine::LoadCursor(const std::string &name)
{
    FSMgr::FileHandle *fil = uaOpenFileAlloc( fmt::sprintf("res/%s.cur", name) , "rb");

    UA_PALETTE pal;

    if (!fil)
        return NULL;

    fil->readU16L();
    if (fil->readU16L() != 2)
    {
        delete fil;
        return NULL;
    }

    fil->readU16L(); //count

    //Only first entry
    fil->readU8(); //w
    fil->readU8(); //h
    fil->readU8(); //color count
    fil->readU8(); //reserved
    int hotX = fil->readU16L();
    int hotY = fil->readU16L();
    fil->readU32L(); //size
    int off = fil->readU32L();

    //seek to cursor
    fil->seek(off, SEEK_SET);

    //read InfoHeader
    fil->readU32L(); //header size
    int bmpw = fil->readS32L();
    int bmph = fil->readS32L();
    fil->readU16L(); //planes
    int bitcount = fil->readU16L();
    fil->readU32L(); //compression
    fil->readU32L(); //imagesize
    fil->readU32L(); //XpixelsPerM
    fil->readU32L(); //YpixelsPerM
    int clrused = fil->readU32L(); //ColorsUsed
    fil->readU32L(); //ColorsImportant

    // read pallete
    int palcnt = 0;
    if (clrused == 0 || bitcount < 16)
    {
        palcnt = clrused != 0 ? clrused : 1 << bitcount;

        for (int i = 0; i < palcnt; i++)
        {
            pal[i].r = fil->readU8();
            pal[i].g = fil->readU8();
            pal[i].b = fil->readU8();
            fil->readU8(); //reserved
        }
    }

    int width = bmpw;
    int height = abs(bmph)/2;

    int imgsz = height * width * bitcount / 8;
    int mask_size = height * width / 8;

    uint8_t *data = (uint8_t *)malloc(imgsz);
    uint8_t *mask = (uint8_t *)malloc(mask_size);

    fil->read(data, imgsz);
    fil->read(mask, mask_size);

    delete fil;


    SDL_Surface *cursr = SDL_CreateRGBSurface(0, width, height, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);

    for (int y = 0; y < height; y++)
    {
        int invY = height - 1 - y;
        uint8_t *row = (uint8_t *)cursr->pixels + y * cursr->pitch;

        for (int x = 0; x < width; x++)
        {
            if (palcnt > 0)
            {
                int idx = CursPix(data, invY * width + x, bitcount);
                int alph = CursPix(mask, invY * width + x, 1);
                row[x * 4 + 0] = pal[ idx ].r;
                row[x * 4 + 1] = pal[ idx ].g;
                row[x * 4 + 2] = pal[ idx ].b;
                row[x * 4 + 3] = (1 - alph) * 255;
            }
            else
            {
                uint32_t clr = CursPix(data, invY * width + x, bitcount);
                int alph = CursPix(mask, invY * width + x, 1);
                row[x * 4 + 0] = clr & 0xFF;
                row[x * 4 + 1] = (clr >> 8) & 0xFF;
                row[x * 4 + 2] = (clr >> 16) & 0xFF;
                row[x * 4 + 3] = (1 - alph) * 255;
            }
        }
    }

    free(data);
    free(mask);

    SDL_Cursor* cursor = SDL_CreateColorCursor(cursr, hotX, hotY);

    SDL_FreeSurface(cursr);

    return cursor;
}

void GFXEngine::Init()
{
    StaticInit();
    
    _glext = Glext::init();
    
    SetDeviceByGUID( read_guid("env/guid3d.def") );
    
    System::EventsAddHandler(EventsWatcher);
    
    System::IniConf::ReadFromNucleusIni();
    
    _vbo = System::IniConf::GfxVBO.Get<bool>();
    _colorEffects = System::IniConf::GfxColorEffects.Get<int32_t>();

    if (!_glext)
    {
        _colorEffects = 0;
        _vbo = false;
    }

    _mipMaps = System::IniConf::GfxMipmaps.Get<bool>();

    _maxAniso = 0.0f;
    int wantAniso = System::IniConf::GfxAnisotropy.Get<int32_t>();
    if (wantAniso > 1)
    {
        float hwMax = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &hwMax);
        glGetError(); // extension may be absent; ignore

        _maxAniso = (hwMax > 0.0f) ? std::min<float>(wantAniso, hwMax) : 0.0f;
    }

    printf("gfx: mipmaps=%d anisotropy=%.0f msaa=%d\n",
           (int)_mipMaps, _maxAniso, System::IniConf::GfxMSAA.Get<int32_t>());
    
    if (_vbo)
    {
        Glext::GLGenVertexArrays(1, &_globalVao);
        Glext::GLBindVertexArray(_globalVao);
        
        Glext::GLGenBuffers(1, &_vboParams);
        Glext::GLBindBuffer(GL_UNIFORM_BUFFER, _vboParams);
        Glext::GLBufferData(GL_UNIFORM_BUFFER, _vboParamsSize, NULL, GL_STREAM_DRAW); 
        
        Glext::GLBindBufferBase(GL_UNIFORM_BUFFER, _vboParamsBlockBinding, _vboParams);
    }
    
    std::array<Common::Point, 17> checkModes
    {{
        {640, 480},     {800, 600},     {1024, 768},    {1280, 1024}, 
        {1440, 1050},   {1600, 1200},   {720, 480},     {852, 480}, 
        {1280, 720},    {1366, 768},    {1600, 900},    {1920, 1080}, 
        {1920, 1200},   {2560, 1080},   {2560, 1440},   {3440, 1440},
        {3840, 2160}
     }};
    
    graphicsModes.reserve(checkModes.size());

    // The mode list defines the LOGICAL 2D/game resolutions. Rendering uses
    // borderless fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP) or a window, and
    // the 2D surface is scaled to the drawable, so a mode does not need a
    // matching physical display mode. Snapping requests through
    // SDL_GetClosestDisplayMode() collapsed the whole list into one or two
    // huge modes on macOS (640x480 -> 3456x2234), which left the shell UI
    // microscopic and made the resolution setting appear to do nothing.
    // Keep the REQUESTED size as the mode identity; store the closest real
    // display mode only as metadata for exclusive-fullscreen platforms.
    SDL_DisplayMode desktop;
    bool hasDesktop = (SDL_GetDesktopDisplayMode(0, &desktop) == 0);

    for(Common::Point m : checkModes)
    {
        if (hasDesktop && (m.x > desktop.w || m.y > desktop.h))
            continue;

        SDL_DisplayMode target, closest;

        target.w = m.x;
        target.h = m.y;
        target.format = _pixfmt->format;
        target.refresh_rate = 0;
        target.driverdata   = 0;

        GfxMode mode;
        mode.w = target.w;
        mode.h = target.h;
        mode.mode = target;
        mode.bpp = _pixfmt->BytesPerPixel;
        mode.name = GfxMode::GenName(mode.w, mode.h);

        if (SDL_GetClosestDisplayMode(0, &target, &closest) )
            mode.mode = closest;

        AddGfxMode(mode);
    }

    // Native desktop resolution is always a valid logical mode.
    if (hasDesktop)
    {
        GfxMode mode;
        mode.w = desktop.w;
        mode.h = desktop.h;
        mode.mode = desktop;
        mode.bpp = _pixfmt->BytesPerPixel;
        mode.name = GfxMode::GenName(mode.w, mode.h);

        AddGfxMode(mode);
    }
    
    // Force to add custom resolutions
    std::vector<std::string> customModes = Stok::Split(System::IniConf::GfxAdditionalModes.Get<std::string>(), ",");
    for (std::string mod : customModes)
    {
        std::vector<std::string> vals = Stok::Split(mod, ":-x \t");
        if (vals.size() >= 2)
        {
            SDL_DisplayMode target, closest;
            
            target.w = std::stoi(vals[0]);
            target.h = std::stoi(vals[1]);
            target.format = _pixfmt->format;
            target.refresh_rate = 0;
            target.driverdata   = 0;

            GfxMode mode;
            mode.w = target.w;
            mode.h = target.h;
            mode.mode = target;
            mode.bpp = _pixfmt->BytesPerPixel;
            mode.name = GfxMode::GenName(mode.w, mode.h);
                
            if (SDL_GetClosestDisplayMode(0, &target, &closest) )
            {
                mode.mode = closest;
                mode.bpp = SDL_BYTESPERPIXEL(closest.format) * 8;
                mode.name = GfxMode::GenName(mode.w, mode.h);
            }
            
            AddGfxMode(mode);
        }
    }
    
    

    std::sort(graphicsModes.begin(), graphicsModes.end(), GfxMode::SortCompare);
    
    cursors[0] = LoadCursor("Pointer");
    cursors[1] = LoadCursor("Cancel");
    cursors[2] = LoadCursor("Select");
    cursors[3] = LoadCursor("Attack");
    cursors[4] = LoadCursor("Goto");
    cursors[5] = LoadCursor("Disk");
    cursors[6] = LoadCursor("New");
    cursors[7] = LoadCursor("Add");
    cursors[8] = LoadCursor("Control");
    cursors[9] = LoadCursor("Beam");
    cursors[10] = LoadCursor("Build");

    IDVList stak { {ATT_WIDTH,  (int32_t)640},
            {ATT_HEIGHT, (int32_t)480} };
    func0( stak );

    RecreateScreenSurface();
    Gui::Instance.SetScreenSize(GetScreenSize());
    
    LoadPalette(System::IniConf::GfxPalette.Get<std::string>());

    if (_vbo)
    {
        _stdPsShader = CompileShader(GL_FRAGMENT_SHADER, _stdPShaderText);
        _stdVsShader = CompileShader(GL_VERTEX_SHADER,   _stdVShaderText);
        uint32_t progID = Glext::GLCreateProgram();
        
        Glext::GLAttachShader(progID, _stdPsShader);
        Glext::GLAttachShader(progID, _stdVsShader);
        { Perf::Scope perfShd(Perf::SHD); Glext::GLLinkProgram(progID); }
        
        _stdShaderProg = TShaderProg( progID );
        
        BindVBOParameters(_stdShaderProg);

        // Reforge graphics settings: the V2 program is only compiled when the V2
        // level is on (gfxLevel("V2"); UA_RENDER_V2 maps onto it). Off = no
        // shader compile, the classic program only. SetRenderV2() compiles it
        // later if the player turns V2 on (applied at the next level load).
        _renderV2 = false;
        if (gfxLevel("V2", 0) > 0)
            SetRenderV2(true);

        // Reforge floor (gfxLevel("FLOOR"); UA_R2_GROUND maps onto it): needs V2.
        SetGround(gfxLevel("FLOOR", 0) > 0);

        ShadowsInit(); // reforge shadows: level 0 = returns at once
    }

    if (_colorEffects > 0)
    {
        Glext::GLGenFramebuffers(1, &_fbo);
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, _fbo);

        glGenTextures(1, &_fboTex);
        glBindTexture(GL_TEXTURE_2D, _fboTex);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexImage2D(GL_TEXTURE_2D, 0, FBOTEXTYPE, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);


        Glext::GLGenRenderbuffers(1, &_fbod);
        Glext::GLBindRenderbuffer(GL_RENDERBUFFER, _fbod);
        Glext::GLRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 640, 480);

        
        Glext::GLFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _fbod);

        Glext::GLFrameBufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _fboTex, 0);

        Glext::GLBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        Glext::GLBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (_vbo)
        {
            _psShader = LoadShader(GL_FRAGMENT_SHADER, "res/clreff_vbo.ps");
            _vsShader = LoadShader(GL_VERTEX_SHADER, "res/clreff_vbo.vs");
        }
        else
        {
            _psShader = LoadShader(GL_FRAGMENT_SHADER, "res/clreff.ps");
            _vsShader = LoadShader(GL_VERTEX_SHADER, "res/clreff.vs");
        }

        uint32_t progID = Glext::GLCreateProgram();
        
        Glext::GLAttachShader(progID, _psShader);
        Glext::GLAttachShader(progID, _vsShader);
        { Perf::Scope perfShd(Perf::SHD); Glext::GLLinkProgram(progID); }
        
        _colorEffectsShaderProg = TColorEffectsProg(progID);
        
        if (_vbo)
            BindVBOParameters(_colorEffectsShaderProg);
    }

    GlowPass::Init(_glext && _vbo && _colorEffects == 0); // UA_GFX_GLOW: env read only
}

// Reforge graphics settings: V2 on/off at level load. Compiles the V2 program
// on first use; off just stops selecting it (the program stays for a later on).
void GFXEngine::SetRenderV2(bool on)
{
    if (on && !_v2ShaderProg.ID && _vbo)
        InitV2Program();
    _renderV2 = on && _v2ShaderProg.ID;
}

void GFXEngine::InitV2Program()
{
    _renderV2 = true;
    // ATMOSPHERE: the variant texts (+ the shadow lookup at SHADOWS > 0; ShadowV2PS returns level 0 text unchanged)
    // only when the atmosphere is on for this level (_v2Atmos, AtmosRefresh)
    _v2PsShader = CompileShader(GL_FRAGMENT_SHADER, ShadowV2PS(_v2Atmos ? AtmosVariant(_v2PShaderText, 2) : _v2PShaderText));
    _v2VsShader = CompileShader(GL_VERTEX_SHADER,   _v2Atmos ? AtmosVariant(_v2VShaderText, 1) : _v2VShaderText);
    uint32_t v2ID = Glext::GLCreateProgram();

    Glext::GLAttachShader(v2ID, _v2PsShader);
    Glext::GLAttachShader(v2ID, _v2VsShader);
    { Perf::Scope perfShd(Perf::SHD); Glext::GLLinkProgram(v2ID); }

    GLint linked = 0;
    Glext::GLGetProgramiv(v2ID, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char logbuf[2048] = {0};
        Glext::GLGetProgramInfoLog(v2ID, sizeof(logbuf) - 1, NULL, logbuf);
        printf("UA_RENDER_V2: program link failed, staying legacy:\n%s\n", logbuf);
        Glext::GLDeleteProgram(v2ID);
        _renderV2 = false;
    }
    else
    {
        _v2ShaderProg = TShaderProg( v2ID );
        BindVBOParameters(_v2ShaderProg);
        ShadowProgramLinked(v2ID); // shadow sampler unit (level 0: no call)

        _v2Loc.SunDir      = Glext::GLGetUniformLocation(v2ID, "SunDir");
        _v2Loc.SunColor    = Glext::GLGetUniformLocation(v2ID, "SunColor");
        _v2Loc.Ambient     = Glext::GLGetUniformLocation(v2ID, "Ambient");
        _v2Loc.HasNormal   = Glext::GLGetUniformLocation(v2ID, "HasNormal");
        _v2Loc.HasOrm      = Glext::GLGetUniformLocation(v2ID, "HasOrm");
        _v2Loc.HasEmissive = Glext::GLGetUniformLocation(v2ID, "HasEmissive");
        _v2Loc.RoughMetal  = Glext::GLGetUniformLocation(v2ID, "RoughMetal");
        _v2Loc.Emission    = Glext::GLGetUniformLocation(v2ID, "Emission");
        _v2Loc.AlbedoTex   = Glext::GLGetUniformLocation(v2ID, "texSample");
        _v2Loc.NormalTex   = Glext::GLGetUniformLocation(v2ID, "normSample");
        _v2Loc.OrmTex      = Glext::GLGetUniformLocation(v2ID, "ormSample");
        _v2Loc.EmissiveTex = Glext::GLGetUniformLocation(v2ID, "emiSample");
        _v2Loc.Exposure    = Glext::GLGetUniformLocation(v2ID, "Exposure");
        _v2Loc.HemiUp      = Glext::GLGetUniformLocation(v2ID, "HemiUp");
        _v2Loc.HemiDelta   = Glext::GLGetUniformLocation(v2ID, "HemiDelta");
        _v2Loc.ToneMap     = Glext::GLGetUniformLocation(v2ID, "ToneMap");
        _v2Loc.UseAo       = Glext::GLGetUniformLocation(v2ID, "UseAo");
        _v2Loc.AlbedoLinear= Glext::GLGetUniformLocation(v2ID, "AlbedoLinear");
        _v2Loc.NormalScale = Glext::GLGetUniformLocation(v2ID, "NormalScale");
        _v2Loc.NormalRG = Glext::GLGetUniformLocation(v2ID, "NormalRG");
        _v2Loc.Debug       = Glext::GLGetUniformLocation(v2ID, "Debug");
        _v2Loc.Terms       = Glext::GLGetUniformLocation(v2ID, "Terms");
        _v2Loc.FarLod      = Glext::GLGetUniformLocation(v2ID, "FarLod");
        _v2Loc.NormalMip   = Glext::GLGetUniformLocation(v2ID, "NormalMip");
        _v2Loc.FarNormLod  = Glext::GLGetUniformLocation(v2ID, "FarNormLod");
        _v2Loc.FarGain     = Glext::GLGetUniformLocation(v2ID, "FarGain");

        // Sampler units are program state: set once, then hand the
        // program binding back to whatever the state cache believes.
        Glext::GLUseProgram(v2ID);
        Glext::GLUniform1i(_v2Loc.AlbedoTex, 0);
        Glext::GLUniform1i(_v2Loc.NormalTex, 1);
        Glext::GLUniform1i(_v2Loc.OrmTex, 2);
        Glext::GLUniform1i(_v2Loc.EmissiveTex, 3);
        Glext::GLUseProgram(_lastStates.Prog.ID);

        printf("UA_RENDER_V2: reforge material shader active\n");
    }
}

void GFXEngine::Deinit()
{
    GlowPass::Free();

    if ( _font.ttfFont )
    {
        TTF_CloseFont(_font.ttfFont);
        _font.ttfFont = NULL;
    }
    
    if ( _font.ttfFontHi )
    {
        TTF_CloseFont(_font.ttfFontHi);
        _font.ttfFontHi = NULL;
    }
    _font.hiHeight = 0;

    if (ScreenSurface)
        SDL_FreeSurface(ScreenSurface);

    ScreenSurface = NULL;

    if (TextSurface)
        SDL_FreeSurface(TextSurface);

    TextSurface = NULL;

    if (_stdQuadDataBuf)
    {
        ForgetBuffer(_stdQuadDataBuf);
        Glext::GLDeleteBuffers(1, &_stdQuadDataBuf);
    }
    
    _stdQuadDataBuf = 0;
    
    if (_stdQuadIndexBuf)
    {
        ForgetBuffer(_stdQuadIndexBuf);
        Glext::GLDeleteBuffers(1, &_stdQuadIndexBuf);
    }
    
    _stdQuadIndexBuf = 0;
    
    if (_stdShaderProg.ID)
        Glext::GLDeleteProgram(_stdShaderProg.ID);
    
    if (_stdVsShader)
        Glext::GLDeleteShader(_stdVsShader);
    
    if (_stdPsShader)
        Glext::GLDeleteShader(_stdPsShader);
    
    _stdShaderProg.ID = 0;
    _stdVsShader = 0;
    _stdPsShader = 0;   

    GroundDeinit();
    ShadowsDeinit(); // reforge shadows (nothing to free at level 0)
    if (_v2ShaderProg.ID)
        Glext::GLDeleteProgram(_v2ShaderProg.ID);
    if (_v2VsShader)
        Glext::GLDeleteShader(_v2VsShader);
    if (_v2PsShader)
        Glext::GLDeleteShader(_v2PsShader);
    _v2ShaderProg = TShaderProg();
    _v2VsShader = 0;
    _v2PsShader = 0;
    for (uint32_t &t : _v2DefaultTex)
    {
        if (t)
            glDeleteTextures(1, &t);
        t = 0;
    }
    
    if (_colorEffectsShaderProg.ID)
        Glext::GLDeleteProgram(_colorEffectsShaderProg.ID); 
    
    if (_psShader)
        Glext::GLDeleteShader(_psShader);
    
    if (_vsShader)
        Glext::GLDeleteShader(_vsShader);
    
    _colorEffectsShaderProg.ID = 0;
    _vsShader = 0;
    _psShader = 0;
}

GFXEngine::~GFXEngine()
{
    Deinit();
}

int GFXEngine::EventsWatcher(void *, SDL_Event *event)
{
    switch(event->type)
    {
    case SDL_WINDOWEVENT:
    {
        switch(event->window.event)
        {
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            Instance.UpdateFBOSizes();
            Instance.UpdateAspectCorrection();
            break;
        default:
            break;
        }

    }
    break;
    }
    
    return 1; // This event can be passed to another event watcher
}

void GFXEngine::SetCursor(int curID, int force)
{
    int sett = 0;

    if ( force )
        sett = 1;
    else if ( curID != CurrentCursorID )
        sett = 1;

    if ( sett )
    {
        if ( curID == 0 )
            SDL_ShowCursor(SDL_DISABLE);
        else if ( curID <= 11 )
        {
            if ( cursors[curID - 1] )
                SDL_SetCursor( cursors[curID - 1] );

            if (!CurrentCursorID)
                SDL_ShowCursor(SDL_ENABLE);
        }

    }

    CurrentCursorID = curID;
}

GfxMode GFXEngine::sub_41F68C()
{
    for (const GfxMode &m : graphicsModes)
    {
        if (m.w == 640 && m.h == 480)
            return m;
    }

    return graphicsModes.front();
}


GfxMode GFXEngine::windd_func0__sub0(const std::string &file)
{
    FSMgr::FileHandle *fil = uaOpenFileAlloc(file, "r");

    if ( fil )
    {
        std::string line;
        if ( fil->ReadLine(&line) )
        {
            size_t pos = line.find_first_of("\n\r");
            
            if (pos != std::string::npos)
                line.erase(pos);
            
            bool windowed = false;
            pos = line.find("Windowed");
            
            if (pos != std::string::npos && pos >= 1)
            {
                windowed = true;
                line.erase(pos - 1);
            }           

            for (const GfxMode &m : graphicsModes)
            {
                if ( StriCmp(m.name, line) == 0 )
                {
                    GfxMode tmp = m;
                    tmp.windowed = windowed;
                    return tmp;
                }
            }
        }
        delete fil;
    }

    return sub_41F68C();
}

int GFXEngine::GetGfxModeIndex(const Common::Point &res)
{
    int i = 0;
    for (const GfxMode &m : graphicsModes)
    {
        if (m == res)
            return i;
        ++i;
    }
    return -1;
}

void GFXEngine::SetResolution(const Common::Point &res, bool windowed)
{
    if (GfxSelectedMode == res && GfxSelectedMode.windowed == windowed)
        return;
    
    UA_PALETTE *screen_palette = GetPalette();

    UA_PALETTE palette_copy;

    if ( screen_palette )
        palette_copy = *screen_palette; // Copy palette
    else
    {
        for(auto &x : palette_copy)
        {
            x.r = 0;
            x.g = 0;
            x.b = 0;
            x.a = 0;
        }
    }

    EndFrame();

    //cls3D->Delete();
    
    GfxMode picked;
    if ( res )
    {
        for (const GfxMode &m : graphicsModes)
        {
            if ( m.w == res.x && m.h == res.y )
            {
                picked = m;
                picked.windowed = windowed;
                break;
            }
        }

        if ( !picked )
            picked = sub_41F68C();
    }
    else
    {
        picked = windd_func0__sub0("env/vid.def");
    }
    
    if (!picked.windowed)
        System::SetVideoMode(Common::Point(picked.w, picked.h), SDL_WINDOW_FULLSCREEN_DESKTOP, &picked.mode);
    else
    {
        // The logical resolution stays picked.w/h; the OS window may be an
        // integer multiple of it (the 2D surface scales to the drawable and
        // mouse input is mapped back). Without this, small classic modes make
        // for an unusably small window on high-res displays.
        int scale = System::IniConf::GfxWindowScale.Get<int32_t>();

        if (scale < 1) // auto
        {
            scale = 1;

            SDL_DisplayMode desktop;
            if (SDL_GetDesktopDisplayMode(0, &desktop) == 0)
            {
                while ( scale < 4 &&
                        picked.w * (scale + 1) <= desktop.w &&
                        picked.h * (scale + 1) <= desktop.h - 64 )
                    scale++;
            }
        }

        if (scale > 8)
            scale = 8;

        System::SetVideoMode(Common::Point(picked.w * scale, picked.h * scale), 0, NULL);
    }
    
    SetResVariables(picked);        
    ApplyResolution();
    
    RecreateScreenSurface();
    
    BeginFrame();
    SetPalette(palette_copy);
    
    GfxSelectedMode = picked;
    
    FSMgr::FileHandle *fil = uaOpenFileAlloc("env/vid.def", "w");
    if ( fil )
    {
        if (picked.windowed)
            fil->printf("%s Windowed\n", picked.name.c_str());
        else
            fil->printf("%s\n", picked.name.c_str());
            
        delete fil;
    }
    
    Gui::Instance.SetScreenSize(GetScreenSize());
    
    UpdateFBOSizes();
}

const std::vector<GfxMode> &GFXEngine::GetAvailableModes()
{
    return graphicsModes;
}




void GFXEngine::SetTracyRmp(ResBitmap *rmp)
{
    setRSTR_trcRmp(rmp);
}

void GFXEngine::SetShadeRmp(ResBitmap *rmp)
{
    setRSTR_shdRmp(rmp);
}

GfxMode GFXEngine::GetGfxMode()
{
    return GfxSelectedMode;
}

TileMap * GFXEngine::GetTileset(int id)
{
    return raster_func208(id);
}

void GFXEngine::SetTileset(TileMap *tileset, int id)
{
    raster_func207(id, tileset);
}


bool GFXEngine::LoadPalette(const std::string &palette_ilbm)
{
    NC_STACK_bitmap *ilbm = Nucleus::CInit<NC_STACK_ilbm>( {
        {NC_STACK_rsrc::RSRC_ATT_NAME, std::string(palette_ilbm)},
        {NC_STACK_bitmap::BMD_ATT_HAS_COLORMAP, (int32_t)1}} );

    if (!ilbm)
        return false;

    SetPalette( *ilbm->getBMD_palette() );

    ilbm->Delete();

    return true;
}
    
uint8_t *GFXEngine::MakeScreenCopy(int *ow, int *oh)
{
    Common::Point res = System::GetResolution();

    // power of 2
    res.x &= ~1;
    res.y &= ~1;

    uint8_t *buf = (uint8_t *)malloc(res.x * res.y * 3);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, res.x, res.y, GL_RGB, GL_UNSIGNED_BYTE, buf);

    *ow = res.x;
    *oh = res.y;
    return buf;
}

uint8_t *GFXEngine::MakeDepthScreenCopy(int *ow, int *oh)
{
    Common::Point res = System::GetResolution();

    // power of 2
    res.x &= ~1;
    res.y &= ~1;

    uint8_t *buf = (uint8_t *)malloc(res.x * res.y);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, res.x, res.y, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, buf);

    *ow = res.x;
    *oh = res.y;
    return buf;
}



SDL_Surface *GFXEngine::Screen()
{
    return ScreenSurface;
}



// Draw line Bresenham's algorithm
void GFXEngine::DrawLine(SDL_Surface *surface, const Common::Line &line, uint8_t cr, uint8_t cg, uint8_t cb )
{
    if ((line.Width() == 0 && line.Height() == 0) || 
         !Common::Rect(surface->w, surface->h).IsIn(line.P1()) ||
         !Common::Rect(surface->w, surface->h).IsIn(line.P2()) )
        return;
    
    int rilWidth = surface->pitch / surface->format->BytesPerPixel;

    int xCount = line.Width();
    int yCount = line.Height();

    uint32_t color = SDL_MapRGBA(surface->format, cr, cg, cb, 255);

    int stepAdd, stepOdd;
    int steps, subSteps;

    if ( xCount <= yCount )
    {
        if ( line.y2 <= line.y1 )
            stepAdd = -rilWidth;
        else
            stepAdd = rilWidth;

        if ( line.x2 <= line.x1 )
            stepOdd = -1;
        else
            stepOdd = 1;

        steps = yCount;
        subSteps = xCount;
    }
    else
    {
        if ( line.x2 <= line.x1 )
            stepAdd = -1;
        else
            stepAdd = 1;

        if ( line.y2 <= line.y1 )
            stepOdd = -rilWidth;
        else
            stepOdd = rilWidth;
        
        steps = xCount;
        subSteps = yCount;
    }

    int incr1 = 2 * subSteps;
    int t = 2 * subSteps - steps;
    int incr2 = 2 * (subSteps - steps);
    
    SDL_LockSurface(surface);
    
    void *surfPos = (void *) ((uint8_t *) surface->pixels 
                    + line.y1 * surface->pitch 
                    + line.x1 * surface->format->BytesPerPixel );

    switch(surface->format->BytesPerPixel)
    {
        case 1:
        {
            uint8_t *surf = (uint8_t *)surfPos;

            for (int i = 0; i <= steps; i++) // Verify i bound
            {
                *surf = color;
                if ( t > 0 )
                {
                    t += incr2;
                    surf += stepOdd;
                }
                else
                    t += incr1;

                surf += stepAdd;
            }
        }
        break;
        
        case 2:
        {
            uint16_t *surf = (uint16_t *)surfPos;

            for (int i = 0; i <= steps; i++) // Verify i bound
            {
                *surf = color;
                if ( t > 0 )
                {
                    t += incr2;
                    surf += stepOdd;
                }
                else
                    t += incr1;

                surf += stepAdd;
            }
        }
        break;
        
        case 4:
        {
            uint32_t *surf = (uint32_t *)surfPos;

            for (int i = 0; i <= steps; i++) // Verify i bound
            {
                *surf = color;
                if ( t > 0 )
                {
                    t += incr2;
                    surf += stepOdd;
                }
                else
                    t += incr1;

                surf += stepAdd;
            }
        }
        break;
        
        default:
        break;
    }
    
    SDL_UnlockSurface(surface);
}

void GFXEngine::BlitScaleMasked(SDL_Surface *src, Common::Rect sRect, SDL_Surface *mask, uint8_t index, SDL_Surface *dst, Common::Rect dRect)
{
    if (mask->format->BitsPerPixel != 8)
        return;
        
    if (src->w != mask->w || src->h != mask->h)
        return;
    
    if (sRect.IsEmpty() || !sRect.IsValid())
        sRect = Common::Rect(src->w, src->h);
    else if (!Common::Rect(src->w, src->h).IsIn(sRect))
        return;
    
    if (dRect.IsEmpty() || !dRect.IsValid())
        dRect = Common::Rect(dst->w, dst->h);
    else if (!Common::Rect(dst->w, dst->h).IsIn(dRect))
        return;
    
    // Try fast
    if (src->format->format == dst->format->format)
    {
        switch(src->format->BytesPerPixel)
        {
            case 2:
            {
                SDL_LockSurface(src);
                SDL_LockSurface(mask);
                SDL_LockSurface(dst);
                
                int32_t dY = (sRect.Height() << 16) / dRect.Height();
                int32_t dX = (sRect.Width()  << 16) / dRect.Width();
                
                int32_t srcY  = sRect.top << 16;
                for (int y = dRect.top; y < dRect.bottom; y++)
                {
                    uint16_t *dBuf = (uint16_t *)((uint8_t *)dst->pixels + y * dst->pitch) + dRect.left;
                    uint16_t *sBuf = (uint16_t *)((uint8_t *)src->pixels + (srcY >> 16) * src->pitch) + sRect.left;
                    uint8_t  *mBuf = (uint8_t *)mask->pixels + (srcY >> 16) * mask->pitch + sRect.left;
                    
                    int32_t xx = 0;
                    for (int x = dRect.left; x < dRect.right; x++)
                    {
                        if (mBuf[xx >> 16] == index)
                            *dBuf = sBuf[xx >> 16];
                        dBuf++;
                        xx += dX;
                    }
                    srcY += dY;
                }
                
                SDL_UnlockSurface(dst);
                SDL_UnlockSurface(mask);
                SDL_UnlockSurface(src);
            }
            break;
            
            case 4:
            {
                SDL_LockSurface(src);
                SDL_LockSurface(mask);
                SDL_LockSurface(dst);
                
                int32_t dY = (sRect.Height() << 16) / dRect.Height();
                int32_t dX = (sRect.Width()  << 16) / dRect.Width();
                
                int32_t srcY  = sRect.top << 16;
                for (int y = dRect.top; y < dRect.bottom; y++)
                {
                    uint32_t *dBuf = (uint32_t *)((uint8_t *)dst->pixels + y * dst->pitch) + dRect.left;
                    uint32_t *sBuf = (uint32_t *)((uint8_t *)src->pixels + (srcY >> 16) * src->pitch) + sRect.left;
                    uint8_t  *mBuf = (uint8_t *)mask->pixels + (srcY >> 16) * mask->pitch + sRect.left;
                    
                    int32_t xx = 0;
                    for (int x = dRect.left; x < dRect.right; x++)
                    {
                        if (mBuf[xx >> 16] == index)
                            *dBuf = sBuf[xx >> 16];
                        dBuf++;
                        xx += dX;
                    }
                    srcY += dY;
                }
                
                SDL_UnlockSurface(dst);
                SDL_UnlockSurface(mask);
                SDL_UnlockSurface(src);
            }
            break;
            
            default:
            break;
        }
    }
    else // Slow
    {
        SDL_LockSurface(src);
        SDL_LockSurface(mask);
        SDL_LockSurface(dst);

        uint8_t sbpp = src->format->BytesPerPixel;
        uint8_t dbpp = dst->format->BytesPerPixel;
        
        int32_t dY = (sRect.Height() << 16) / dRect.Height();
        int32_t dX = (sRect.Width()  << 16) / dRect.Width();

        int32_t srcY  = sRect.top << 16;
        for (int y = dRect.top; y < dRect.bottom; y++)
        {
            uint8_t *dBuf = (uint8_t *)dst->pixels + y * dst->pitch + dRect.left * dbpp;
            uint8_t *sBuf = (uint8_t *)src->pixels + (srcY >> 16) * src->pitch + sRect.left * sbpp;
            uint8_t  *mBuf = (uint8_t *)mask->pixels + (srcY >> 16) * mask->pitch + sRect.left;

            int32_t xx = 0;
            for (int x = dRect.left; x < dRect.right; x++)
            {
                if (mBuf[xx >> 16] == index)
                {
                    uint8_t r,g,b;
                    uint32_t clr = 0;
                    
                    uint8_t *spix = sBuf + (xx >> 16) * sbpp;
                    for(int i = 0; i < sbpp; i++)
                        clr |= spix[i] << (i * 8);
                    
                    SDL_GetRGB(clr, src->format, &r, &g, &b);
                    clr = SDL_MapRGB(dst->format, r, g, b);
                    
                    for(int i = 0; i < dbpp; i++)
                        dBuf[i] = (clr >> (i * 8)) & 0xFF;
                }
                dBuf += dbpp;
                xx += dX;
            }
            srcY += dY;
        }

        SDL_UnlockSurface(dst);
        SDL_UnlockSurface(mask);
        SDL_UnlockSurface(src);
    }
}

void GFXEngine::DrawFillIntCpy(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect)
{
    const int32_t sh = sRect.Height();
    const int32_t sw = sRect.Width();
    const int32_t dh = dRect.Height();
    const int32_t dw = dRect.Width();
        
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
                    
    if (dw > sw)
    {
        uint8_t *psrc = (uint8_t *)src->pixels + sRect.top * src->pitch + sRect.left * src->format->BytesPerPixel;
        uint8_t *pdst = (uint8_t *)dst->pixels + dRect.top * dst->pitch + dRect.left * dst->format->BytesPerPixel;
        
        const int32_t nit = dw / sw;
        const int32_t nit2 = (dw % sw) * dst->format->BytesPerPixel;
        const int32_t cpsz = sw * dst->format->BytesPerPixel;
        const int32_t skipdst = dst->pitch - dw * dst->format->BytesPerPixel + nit2;
                
        int sptch = src->pitch;
        
        int ln = 0;
        uint8_t *psrcl = psrc;
        for(int ih = dh; ih > 0; --ih)
        {
            for(int n = nit; n > 0; --n)
            {
                memcpy(pdst, psrcl, cpsz);
                pdst += cpsz;
            }
            
            memcpy(pdst, psrcl, nit2);
            pdst += skipdst;
            
            ++ln;
            if (ln >= sh)
            {
                ln = 0;
                psrcl = psrc;
            }
            else
                psrcl += sptch;
        }        
    }
    else
    {
        uint8_t *psrc = (uint8_t *)src->pixels + sRect.top * src->pitch + sRect.left * src->format->BytesPerPixel;
        uint8_t *pdst = (uint8_t *)dst->pixels + dRect.top * dst->pitch + dRect.left * dst->format->BytesPerPixel;
        
        const int32_t cpsz = dw * dst->format->BytesPerPixel;
        const int32_t skipdst = dst->pitch;
                
        int sptch = src->pitch;
        
        int ln = 0;
        uint8_t *psrcl = psrc;
        for(int ih = dh; ih > 0; --ih)
        {            
            memcpy(pdst, psrcl, cpsz);
            pdst += skipdst;
            
            ++ln;
            if (ln >= sh)
            {
                ln = 0;
                psrcl = psrc;
            }
            else
                psrcl += sptch;
        }        
    }
                
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}
    
// One colour-keyed row: d[i] = s[i] unless s[i] is the key. Written as a select over
// typed pointers so the compiler vectorises it (at UI 3x the HUD tile fills are 9x
// the pixels; the old byte-stepped loop was ~5 % of the frame). Same pixels as before:
// a keyed texel leaves the destination value unchanged.
template <typename T>
static inline void CKeyRow(const T *src, T *dst, int n, uint32_t key, bool keyFits)
{
    if ( !keyFits )                 // the key can never equal a T value: plain copy
    {
        for (int i = 0; i < n; i++)
            dst[i] = src[i];
        return;
    }
    const T k = (T)key;
    for (int i = 0; i < n; i++)
    {
        const T v = src[i];
        dst[i] = (v != k) ? v : dst[i];
    }
}

template <typename T>
void GFXEngine::DrawFillIntCKey(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect)
{
    const int32_t sh = sRect.Height();
    const int32_t sw = sRect.Width();
    const int32_t dh = dRect.Height();
    const int32_t dw = dRect.Width();
        
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    
    uint32_t clrKey = 0;
    SDL_GetColorKey(src, &clrKey);
    const bool keyFits = clrKey == (uint32_t)(T)clrKey;

    // Pixel stepping is in T units (the old loop stepped dst BytesPerPixel for both
    // surfaces; the dispatcher only calls this with sizeof(T) == BytesPerPixel).
    const int sz = dst->format->BytesPerPixel;
    uint8_t *psrc = (uint8_t *)src->pixels + sRect.top * src->pitch + sRect.left * sz;
    uint8_t *pdst = (uint8_t *)dst->pixels + dRect.top * dst->pitch + dRect.left * sz;
    const int sptch = src->pitch;

    // dw > sw: the source row repeats nit times, then a partial nit2 run (DrawFill
    // REPEATS, never scales). dw <= sw: one dw-wide run. Rows wrap after sh lines.
    const int32_t nit = (dw > sw) ? dw / sw : 0;
    const int32_t nit2 = (dw > sw) ? dw % sw : dw;

    int ln = 0;
    uint8_t *psrcl = psrc;
    for(int ih = dh; ih > 0; --ih)
    {
        T *d = (T *)pdst;
        const T *ls = (const T *)psrcl;
        for(int n = nit; n > 0; --n)
        {
            CKeyRow<T>(ls, d, sw, clrKey, keyFits);
            d += sw;
        }
        CKeyRow<T>(ls, d, nit2, clrKey, keyFits);

        ++ln;
        if (ln >= sh)
        {
            ln = 0;
            psrcl = psrc;
        }
        else
            psrcl += sptch;

        pdst += dst->pitch;
    }
                
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

template <typename T>
void GFXEngine::DrawFillIntACpy(SDL_Surface* src, const Common::Rect& sRect, SDL_Surface* dst, const Common::Rect& dRect)
{
    const int32_t sh = sRect.Height();
    const int32_t sw = sRect.Width();
    const int32_t dh = dRect.Height();
    const int32_t dw = dRect.Width();
        
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    
    if (dw > sw)
    {
        const int32_t skipdst = dst->pitch - dw * dst->format->BytesPerPixel;
        uint8_t *psrc = (uint8_t *)src->pixels + sRect.top * src->pitch + sRect.left * src->format->BytesPerPixel;
        uint8_t *pdst = (uint8_t *)dst->pixels + dRect.top * dst->pitch + dRect.left * dst->format->BytesPerPixel;
        
        const int32_t nit = dw / sw;
        const int32_t nit2 = dw % sw;
        uint32_t amask = src->format->Amask;
        
        const int sz = dst->format->BytesPerPixel;
        
        int sptch = src->pitch;
        
        int ln = 0;
        uint8_t *psrcl = psrc;
        for(int ih = dh; ih > 0; --ih)
        {
            
            for(int n = nit; n > 0; --n)
            {
                const uint8_t *lsrc = psrcl;
                for (int iw = sw; iw > 0; --iw)
                {
                    if (*(T *)lsrc & amask)
                        *(T *)pdst = *(T *)lsrc;
                    
                    lsrc += sz;
                    pdst += sz;
                }
            }
            
            uint8_t *rsrc = psrcl;            
            for(int iw = nit2; iw > 0; --iw)
            {
                if (*(T *)rsrc & amask)
                    *(T *)pdst = *(T *)rsrc;

                rsrc += sz;
                pdst += sz;
            }
            
            ++ln;
            if (ln >= sh)
            {
                ln = 0;
                psrcl = psrc;
            }
            else
                psrcl += sptch;
            
            pdst += skipdst;
        }        
    }
    else
    {
        const int32_t skipdst = dst->pitch - dw * dst->format->BytesPerPixel;
        uint8_t *psrc = (uint8_t *)src->pixels + sRect.top * src->pitch + sRect.left * src->format->BytesPerPixel;
        uint8_t *pdst = (uint8_t *)dst->pixels + dRect.top * dst->pitch + dRect.left * dst->format->BytesPerPixel;
        
        const int sz = dst->format->BytesPerPixel;
        
        int sptch = src->pitch;
        uint32_t amask = src->format->Amask;
        
        int ln = 0;
        uint8_t *psrcl = psrc;
        for(int ih = dh; ih > 0; --ih)
        {            
            uint8_t *rsrc = psrcl;            
            for(int iw = dw; iw > 0; --iw)
            {
                if (*(T *)rsrc & amask)
                    *(T *)pdst = *(T *)rsrc;

                rsrc += sz;
                pdst += sz;
            }
            
            ++ln;
            if (ln >= sh)
            {
                ln = 0;
                psrcl = psrc;
            }
            else
                psrcl += sptch;
            
            pdst += skipdst;
        }        
    }
                
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
}

void GFXEngine::DrawFill(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect)
{
    if (sRect.IsEmpty() || dRect.IsEmpty())
        return;
    
    if (src->format->format == dst->format->format)
    {     
#if SDL_VERSION_ATLEAST(2, 0, 9)
        if (SDL_HasColorKey(src))
#else
        uint32_t tmp;
        if (SDL_GetColorKey(src, &tmp) == 0)
#endif
        {
            switch(src->format->BytesPerPixel)
            {

                case 2:
                    DrawFillIntCKey<uint16_t>(src, sRect, dst, dRect);
                    break;

                case 4:
                    DrawFillIntCKey<uint32_t>(src, sRect, dst, dRect);
                    break;

                default:
                break;
            }
        }
        else
        {
            if (src->format->Amask)
            {
                switch(src->format->BytesPerPixel)
                {

                    case 2:
                        DrawFillIntACpy<uint16_t>(src, sRect, dst, dRect);
                        break;

                    case 4:
                        DrawFillIntACpy<uint32_t>(src, sRect, dst, dRect);
                        break;

                    default:
                    break;
                }
            }
            else
                DrawFillIntCpy(src, sRect, dst, dRect);
        }  
    }
    else
    {
        SDL_Rect lsrc = sRect;
        SDL_Rect ldst;
        for(ldst.y = dRect.top; ldst.y < dRect.bottom; ldst.y += lsrc.h)
        {
            if (dRect.bottom - ldst.y < lsrc.h)
                lsrc.h = dRect.bottom - ldst.y;

            lsrc.w = sRect.Width();

            for(ldst.x = dRect.left; ldst.x < dRect.right; ldst.x += lsrc.w)
            {
                if (dRect.right - ldst.x < lsrc.w)
                    lsrc.w = dRect.right - ldst.x;

                SDL_BlitSurface(src, &lsrc, dst, &ldst);
            }
        }
    }
}

void GFXEngine::Draw(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, Common::Point dPoint)
{
    SDL_Rect Ssrc = sRect;
    SDL_Rect Sdst = dPoint;
    SDL_BlitSurface(src, &Ssrc, dst, &Sdst);
}

void GFXEngine::Draw(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, Common::PointRect dRect)
{
    SDL_Rect Ssrc = sRect;
    if (Ssrc.w > dRect.w)
        Ssrc.w = dRect.w;
    if (Ssrc.h > dRect.h)
        Ssrc.h = dRect.h;
    SDL_Rect Sdst = dRect;
    SDL_BlitSurface(src, &Ssrc, dst, &Sdst);
}

void GFXEngine::GetGlPixTypeFmt(GLint *format, GLint *type)
{
    *format = _glPixfmt;
    *type = _glPixtype;
}

// ---------------------------------------------------------------------------
// Hi-density UI bitmap layer
//
// The hi-res text layer fixed the TTF half of "the UI looks like 1998". The
// other half - every icon, gauge, panel frame, map cursor and HUD digit - is
// bitmap TILE art (.ILB pages indexed by .FON tables) that ProcessDrawSeq
// software-blits 1:1 into ScreenSurface. ScreenSurface is the LOGICAL 2D mode
// (640x480 by default), stretched over the whole drawable as one GL_LINEAR
// quad, so those blits arrive on a 2K panel ~3x magnified and smeared.
//
// Fix: make ScreenSurface _resolution * _uiScale and multiply by _uiScale at
// the FINAL SDL call of each 2D primitive, never inside an interpreter. Every
// layout decision, the command-stream maths, _clip, ConvertPosTo2DStuff and all
// game logic keep running in logical pixels, so nothing can move; only the
// pixel density of what lands on the surface rises.
//
// The tile source has to grow with it, and for a reason worth spelling out:
// DrawFill() REPEATS the source rect over the destination rect (that is what
// draws a stretched panel edge from a 1px column) - it does not scale. Blowing
// up only the destination would therefore tile a tile N times instead of
// magnifying it. So each TileMap carries hiTex, exactly _uiScale times bigger,
// and the blit scales srcR and dstR by the same factor: sw/dw, the modulo and
// the row wrap are all preserved exactly, and the pattern is identical at N x
// the density. That is also why an installed pack's scale is authoritative
// (hd-textures/ui/pack.txt) rather than something derived from the drawable.
// ---------------------------------------------------------------------------
static const int UI_SCALE_MAX = 4;
static const int SCR_TILE_W = 64;
static const int SCR_TILE_H = 32;

void GFXEngine::ResolveUiScale()
{
    if ( _uiScaleResolved )
        return;

    _uiScaleResolved = true;

    int mode = System::IniConf::GfxUiScale.Get<int32_t>();
    if ( const char *ev = getenv("UA_UI_SCALE") )
        mode = atoi(ev);

    const int pack = HDTex::UiPackScale();

    if ( mode <= 0 )
        _uiScale = pack > 0 ? pack : 1;      // auto: follow the pack, else classic
    else
        _uiScale = mode;

    if ( _uiScale < 1 )
        _uiScale = 1;
    if ( _uiScale > UI_SCALE_MAX )
        _uiScale = UI_SCALE_MAX;

    if ( _uiScale > 1 )
        fmt::printf("ui-hd: UI bitmap scale %dx%s\n", _uiScale,
                    (pack == _uiScale) ? " (hd-textures/ui pack)"
                                       : " (no matching pack - sheets replicated)");
    else
        fmt::printf("ui-hd: UI bitmap scale 1x (classic)\n");
}

int GFXEngine::UiScale()
{
    // Sheets can be loaded before the first RecreateScreenSurface(), so resolve
    // on first use and then never change: hiTex surfaces built at one factor
    // must not outlive it.
    ResolveUiScale();
    return _uiScale;
}

// Exact pixel replication - no new information, but hard edges instead of the
// bilinear smear, and it keeps srcR*N / dstR*N in exact agreement for sheets
// with no HD twin.
static SDL_Surface *uiReplicate(SDL_Surface *src, int n)
{
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, src->w * n, src->h * n,
                                                      src->format->BitsPerPixel,
                                                      src->format->format);
    if ( !dst )
        return NULL;

    const int bpp = src->format->BytesPerPixel;

    SDL_LockSurface(src);
    SDL_LockSurface(dst);

    for (int y = 0; y < src->h; y++)
    {
        const uint8_t *srow = (const uint8_t *)src->pixels + y * src->pitch;

        for (int ry = 0; ry < n; ry++)
        {
            uint8_t *drow = (uint8_t *)dst->pixels + (y * n + ry) * dst->pitch;

            for (int x = 0; x < src->w; x++)
            {
                const uint8_t *sp = srow + x * bpp;
                for (int rx = 0; rx < n; rx++)
                {
                    memcpy(drow, sp, bpp);
                    drow += bpp;
                }
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);

    return dst;
}

// Turn a pack PNG (alpha-keyed, the pipeline's --key-out alpha contract) into a
// surface the software blitter can consume: same pixel format as the original
// sheet, transparency expressed as the SAME exact colour key, and no accidental
// key colour left inside the art (that would punch holes through an icon).
static SDL_Surface *uiFromPackPng(SDL_Surface *raw, SDL_Surface *like, uint32_t key)
{
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    if ( !rgba )
        return NULL;

    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, raw->w, raw->h,
                                                      like->format->BitsPerPixel,
                                                      like->format->format);
    if ( !dst )
    {
        SDL_FreeSurface(rgba);
        return NULL;
    }

    // What the key colour must NOT be confused with: art that happens to be
    // pure yellow. Nudge it by one green level - invisible, and it keeps the
    // exact-match key honest.
    const uint32_t nearKey = SDL_MapRGB(dst->format, 255, 254, 0);

    SDL_LockSurface(rgba);
    SDL_LockSurface(dst);

    for (int y = 0; y < raw->h; y++)
    {
        const uint8_t *srow = (const uint8_t *)rgba->pixels + y * rgba->pitch;
        uint8_t *drow = (uint8_t *)dst->pixels + y * dst->pitch;

        for (int x = 0; x < raw->w; x++)
        {
            const uint8_t *sp = srow + x * 4;
            uint32_t px;

            if ( sp[3] < 128 )
            {
                px = key;
            }
            else
            {
                px = SDL_MapRGBA(dst->format, sp[0], sp[1], sp[2], 255);
                if ( px == key )
                    px = nearKey;
            }

            switch (dst->format->BytesPerPixel)
            {
                case 2: *(uint16_t *)(drow + x * 2) = (uint16_t)px; break;
                case 4: *(uint32_t *)(drow + x * 4) = px; break;
                default: break;
            }
        }
    }

    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(rgba);
    SDL_FreeSurface(rgba);

    // Without this the whole surface is opaque and DrawFill takes its
    // alpha-copy path, painting the key colour as solid yellow. (Seen exactly
    // once, on the title screen's identity strip - the only shell element whose
    // tile actually relies on the key.)
    SDL_SetColorKey(dst, SDL_TRUE, key);

    return dst;
}

// Runtime guarantee for the tile blit: at _uiScale > 1 a hi-density source MUST
// exist, or srcR*N against dstR*N would repeat the tile instead of magnifying
// it. AttachUiSheet() fills hiTex from the pack at load; anything it never saw
// (or a scale changed underneath us) falls back to exact replication here.
SDL_Surface *GFXEngine::UiSheetFor(TileMap *tileset)
{
    if ( UiScale() <= 1 || !tileset )
        return NULL;

    if ( tileset->hiTex && tileset->hiScale == _uiScale )
        return tileset->hiTex;

    if ( tileset->hiTex )
    {
        SDL_FreeSurface(tileset->hiTex);
        tileset->hiTex = NULL;
    }

    SDL_Surface *src = tileset->img ? tileset->img->GetSwTex() : NULL;
    if ( !src )
        return NULL;

    uint32_t key = 0;
    const bool keyed = ( SDL_GetColorKey(src, &key) == 0 );

    SDL_Surface *hi = uiReplicate(src, _uiScale);
    if ( !hi )
        return NULL;

    if ( keyed )
        SDL_SetColorKey(hi, SDL_TRUE, key);
    SDL_SetSurfaceBlendMode(hi, SDL_BLENDMODE_NONE);

    tileset->hiTex = hi;
    tileset->hiScale = _uiScale;
    return hi;
}

void GFXEngine::AttachUiSheet(TileMap *tileset, const std::string &sheetName)
{
    if ( !tileset || !tileset->img )
        return;

    SDL_Surface *src = tileset->img->GetSwTex();
    if ( !src )
        return;

    const std::string base = HDTex::UiFileName(sheetName, src);

    // The 2D layer never reaches PrepareTexture, so UA_TEX_DUMP cannot see
    // these sheets at all; UA_UI_DUMP is their own hook and feeds exactly the
    // same hd-pipeline.
    if ( HDTex::UiDumpEnabled() )
    {
        HDTex::MakeDir("hd-dump");
        HDTex::MakeDir("hd-dump/ui");

        const std::string path = "hd-dump/ui/" + base;
        if ( !HDTex::FileExists(path) )
            IMG_SavePNG(src, path.c_str());
    }

    const int n = UiScale();
    if ( n <= 1 || tileset->hiTex )
        return;

    uint32_t key = 0;
    const bool keyed = ( SDL_GetColorKey(src, &key) == 0 );

    SDL_Surface *hi = NULL;

    const std::string path = HDTex::UiPackDir() + base;
    if ( HDTex::FileExists(path) )
    {
        SDL_Surface *raw = IMG_Load(path.c_str());
        if ( raw )
        {
            if ( raw->w == src->w * n && raw->h == src->h * n && keyed )
            {
                hi = uiFromPackPng(raw, src, key);
                if ( hi )
                    fmt::printf("ui-hd: %s (%dx%d -> %dx%d)\n", path,
                                src->w, src->h, hi->w, hi->h);
            }
            else
            {
                fmt::printf("ui-hd: %s IGNORED - %dx%d is not %dx sheet %dx%d\n",
                            path, raw->w, raw->h, n, src->w, src->h);
            }
            SDL_FreeSurface(raw);
        }
    }

    if ( !hi )
    {
        hi = uiReplicate(src, n);
        if ( hi && keyed )
            SDL_SetColorKey(hi, SDL_TRUE, key);
    }

    if ( !hi )
        return;

    SDL_SetSurfaceBlendMode(hi, SDL_BLENDMODE_NONE);

    tileset->hiTex = hi;
    tileset->hiScale = n;
}

void GFXEngine::RecreateScreenSurface()
{
    if ( ScreenSurface )
        SDL_FreeSurface(ScreenSurface);

    ResolveUiScale();

    ScreenSurface = SDL_CreateRGBSurface(0, _resolution.x * _uiScale, _resolution.y * _uiScale, _pixfmt->BitsPerPixel, _pixfmt->Rmask, _pixfmt->Gmask, _pixfmt->Bmask, _pixfmt->Amask);

    if ( !screenTex )
        glGenTextures(1, &screenTex);

    _states.Tex = screenTex;
    _states.TexMip = false;
    SetRenderStates(0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenSurface->w, ScreenSurface->h, 0, _glPixfmt, _glPixtype, NULL);

    // Tiled upload only for the hi-density UI (UI pack on); the classic 1x path
    // (preset Original) keeps the full clear + upload exactly as before.
    const char *tv = getenv("UA_UI_TILED");
    _scrTiled = _uiScale > 1 && ScreenSurface && ScreenSurface->format->BytesPerPixel == 4 &&
                !(tv && tv[0] == '0');
    _scrVerify = (_scrTiled && tv && tv[0] == '2') ? 1 : 0;
    if ( _scrTiled )
    {
        _scrTilesX = (ScreenSurface->w + SCR_TILE_W - 1) / SCR_TILE_W;
        _scrTilesY = (ScreenSurface->h + SCR_TILE_H - 1) / SCR_TILE_H;
        _scrTileNow.assign(_scrTilesX * _scrTilesY, 1);   // unknown content: clear + upload all once
        _scrTileGpu.assign(_scrTilesX * _scrTilesY, 1);   // texture content undefined (NULL upload)
        _scrGpuAny = true;
    }

    RecreateTextSurface();
}

// Tiled screen surface (see gfx.h). Marks every tile holding a non-zero pixel.
void GFXEngine::MarkScreen(int x, int y, int w, int h)
{
    if ( !_scrTiled || w <= 0 || h <= 0 )
        return;
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(ScreenSurface->w, x + w), y1 = std::min(ScreenSurface->h, y + h);
    if ( x1 <= x0 || y1 <= y0 )
        return;
    for (int ty = y0 / SCR_TILE_H; ty <= (y1 - 1) / SCR_TILE_H; ty++)
        for (int tx = x0 / SCR_TILE_W; tx <= (x1 - 1) / SCR_TILE_W; tx++)
            _scrTileNow[ty * _scrTilesX + tx] = 1;
}

void GFXEngine::ScanScreenTiles(std::vector<uint8_t> &out)
{
    out.assign(_scrTilesX * _scrTilesY, 0);
    // Tile-major with early exit: a non-zero tile stops at its first non-zero row.
    SDL_Surface *s = ScreenSurface;
    const uint8_t *base = (const uint8_t *)s->pixels;
    for (int ty = 0; ty < _scrTilesY; ty++)
    {
        int y0 = ty * SCR_TILE_H, y1 = std::min(s->h, y0 + SCR_TILE_H);
        for (int tx = 0; tx < _scrTilesX; tx++)
        {
            int x0 = tx * SCR_TILE_W, n = std::min(s->w, x0 + SCR_TILE_W) - x0;
            uint32_t acc = 0;
            if ( n == SCR_TILE_W )
            {
                for (int y = y0; y < y1 && !acc; y++)
                {
                    const uint64_t *p = (const uint64_t *)(base + (size_t)y * s->pitch + x0 * 4);
                    uint64_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
                    for (int i = 0; i < SCR_TILE_W / 2; i += 4)
                    {
                        b0 |= p[i]; b1 |= p[i + 1]; b2 |= p[i + 2]; b3 |= p[i + 3];
                    }
                    acc = (b0 | b1 | b2 | b3) != 0;
                }
            }
            else
            {
                for (int y = y0; y < y1 && !acc; y++)
                {
                    const uint32_t *p = (const uint32_t *)(base + (size_t)y * s->pitch) + x0;
                    for (int i = 0; i < n; i++)
                        acc |= p[i];
                }
            }
            out[ty * _scrTilesX + tx] = acc ? 1 : 0;
        }
    }
}

void GFXEngine::TileBoxes(const std::vector<uint8_t> &mask, std::vector<SDL_Rect> &out)
{
    out.clear();
    const int W = _scrTilesX, H = _scrTilesY;
    static std::vector<uint8_t> seen;
    seen.assign(mask.size(), 0);
    for (int i = 0; i < W * H; i++)
    {
        if ( !mask[i] || seen[i] )
            continue;
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        _scrStack.clear();
        _scrStack.push_back(i);
        seen[i] = 1;
        while ( !_scrStack.empty() )
        {
            int j = _scrStack.back(); _scrStack.pop_back();
            int x = j % W, y = j / W;
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            const int nb[4] = { x > 0 ? j - 1 : -1, x < W - 1 ? j + 1 : -1, y > 0 ? j - W : -1, y < H - 1 ? j + W : -1 };
            for (int k : nb)
                if ( k >= 0 && mask[k] && !seen[k] ) { seen[k] = 1; _scrStack.push_back(k); }
        }
        out.push_back(SDL_Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1});
    }
    // Merge overlapping boxes (a box may cover tiles of another group).
    for (bool merged = true; merged; )
    {
        merged = false;
        for (size_t a = 0; a < out.size() && !merged; a++)
            for (size_t b = a + 1; b < out.size(); b++)
            {
                SDL_Rect &A = out[a], &B = out[b];
                if ( A.x < B.x + B.w && B.x < A.x + A.w && A.y < B.y + B.h && B.y < A.y + A.h )
                {
                    int nx = std::min(A.x, B.x), ny = std::min(A.y, B.y);
                    A.w = std::max(A.x + A.w, B.x + B.w) - nx; A.h = std::max(A.y + A.h, B.y + B.h) - ny;
                    A.x = nx; A.y = ny;
                    out[b] = out.back(); out.pop_back();
                    merged = true;
                    break;
                }
            }
    }
}

void GFXEngine::ClearScreenSurface()
{
    if ( !_scrTiled )
    {
        SDL_FillRect(Screen(), NULL, SDL_MapRGBA(Screen()->format, 0, 0, 0, 0) );
        return;
    }
    // Only tiles that held something are non-zero; the rest are already clear.
    for (int ty = 0; ty < _scrTilesY; ty++)
        for (int tx = 0; tx < _scrTilesX; )
        {
            if ( !_scrTileNow[ty * _scrTilesX + tx] ) { tx++; continue; }
            int t0 = tx;
            while ( tx < _scrTilesX && _scrTileNow[ty * _scrTilesX + tx] )
                _scrTileNow[ty * _scrTilesX + tx++] = 0;
            SDL_Rect r = {t0 * SCR_TILE_W, ty * SCR_TILE_H, (tx - t0) * SCR_TILE_W, SCR_TILE_H};
            SDL_FillRect(ScreenSurface, &r, 0);
        }
}

// ---------------------------------------------------------------------------
// Hi-res UI text layer
//
// WHY: the whole 2D UI (background art, tile fonts, icons, panels AND the
// classic TTF ScreenText) is software-rendered into ScreenSurface, which is
// _resolution (the LOGICAL mode, 640x480 by default - ENV/vid.def) big. That
// one surface is uploaded as a single texture and stretched over the drawable
// with GL_LINEAR, so on a 2K panel every UI pixel is blown up ~3x. The 3D world
// renders at the real drawable resolution, which is why HD world textures look
// sharp while the UI still looks like 1998.
//
// This layer fixes the TEXT half of that: ScreenText glyphs are rasterised at
// the drawable resolution into their own RGBA surface and composited as a
// second fullscreen quad straight after the screen quad (still BEFORE
// Gui::Root::HwCompose(), so hardware-composited widgets stay on top). Layout
// is untouched: all positioning still happens in logical pixels with the 1x
// font, only the glyph raster gains pixels.
//
// Upload cost is kept to the union of the rectangles that actually changed.
// ---------------------------------------------------------------------------
static inline bool RectEmpty(const SDL_Rect &r) { return r.w <= 0 || r.h <= 0; }

static SDL_Rect RectUnion(const SDL_Rect &a, const SDL_Rect &b)
{
    if (RectEmpty(a)) return b;
    if (RectEmpty(b)) return a;

    SDL_Rect o;
    o.x = Common::MIN(a.x, b.x);
    o.y = Common::MIN(a.y, b.y);
    o.w = Common::MAX(a.x + a.w, b.x + b.w) - o.x;
    o.h = Common::MAX(a.y + a.h, b.y + b.h) - o.y;
    return o;
}

// Text entries are horizontal bands scattered down the screen; one bounding box
// over all of them would clear and upload most of the surface every frame. Keep
// a short list instead, merging only rectangles that actually touch, and fall
// back to a single union once the list would grow unreasonable.
static const size_t TEXT_DIRTY_MAX = 24;

static bool RectTouches(const SDL_Rect &a, const SDL_Rect &b)
{
    return !(b.x > a.x + a.w || b.x + b.w < a.x ||
             b.y > a.y + a.h || b.y + b.h < a.y);
}

static void RectListAdd(std::vector<SDL_Rect> &list, const SDL_Rect &r)
{
    if ( RectEmpty(r) )
        return;

    SDL_Rect acc = r;

    for ( size_t i = 0; i < list.size(); )
    {
        if ( RectTouches(list[i], acc) )
        {
            acc = RectUnion(acc, list[i]);
            list[i] = list.back();
            list.pop_back();
            i = 0;                 // the merge may now touch an earlier entry
        }
        else
            i++;
    }

    list.push_back(acc);

    if ( list.size() > TEXT_DIRTY_MAX )
    {
        SDL_Rect all = list[0];
        for ( const SDL_Rect &e : list )
            all = RectUnion(all, e);
        list.clear();
        list.push_back(all);
    }
}

void GFXEngine::TextLayerDirty(const SDL_Rect &r)
{
    RectListAdd(_textDirtyCur, r);
}

// Wipe last frame's glyphs off the CPU surface. _textDirtyPrev is deliberately
// kept: DrawTextLayer still has to push the cleared area to the GPU.
void GFXEngine::ClearTextLayer()
{
    if ( !TextSurface || _textDirtyPrev.empty() )
        return;

    SDL_FillRects(TextSurface, _textDirtyPrev.data(), (int)_textDirtyPrev.size(),
                  SDL_MapRGBA(TextSurface->format, 0, 0, 0, 0));
}

bool GFXEngine::EnsureHiFont(int targetH)
{
    if ( targetH <= 0 || !_font.ttfFont || _font.face.empty() || _resolution.y <= 0 )
        return false;

    int want = (int)((double)_font.height * (double)targetH / (double)_resolution.y + 0.5);
    if (want < _font.height)
        want = _font.height;

    if ( _font.ttfFontHi && _font.hiHeight == want )
        return true;

    if ( _font.ttfFontHi )
    {
        TTF_CloseFont(_font.ttfFontHi);
        _font.ttfFontHi = NULL;
    }

    _font.ttfFontHi = System::LoadFont(_font.face, want);
    if ( !_font.ttfFontHi )
    {
        _font.hiHeight = 0;
        return false;
    }

    TTF_SetFontHinting(_font.ttfFontHi, TTF_HINTING_LIGHT);
    _font.hiHeight = want;
    return true;
}

void GFXEngine::RecreateTextSurface()
{
    if ( TextSurface )
    {
        SDL_FreeSurface(TextSurface);
        TextSurface = NULL;
    }

    _textDirtyCur.clear();
    _textDirtyPrev.clear();

    if ( _font.ttfFontHi )
    {
        TTF_CloseFont(_font.ttfFontHi);
        _font.ttfFontHi = NULL;
    }
    _font.hiHeight = 0;

    int mode = System::IniConf::GfxUiTextScale.Get<int32_t>();
    if ( const char *ev = getenv("UA_UI_TEXT_SCALE") )
        mode = atoi(ev);

    if ( mode == 1 )                 // explicit "classic 1998 text"
        return;

    // Auto matches the real drawable exactly, so glyph texels land on screen
    // pixels 1:1. The 4x ceiling only ever bites on a drawable more than four
    // times the logical mode (a 4K+ panel on a 640x480 shell), where the extra
    // sharpness is invisible but the per-frame upload grows with the square of
    // the factor.
    static const int TEXT_SCALE_MAX = 4;

    Common::Point want;
    if ( mode <= 0 )
    {
        want = System::GetResolution();
        if ( want.x > _resolution.x * TEXT_SCALE_MAX ) want.x = _resolution.x * TEXT_SCALE_MAX;
        if ( want.y > _resolution.y * TEXT_SCALE_MAX ) want.y = _resolution.y * TEXT_SCALE_MAX;
    }
    else
    {
        if ( mode > TEXT_SCALE_MAX * 2 )
            mode = TEXT_SCALE_MAX * 2;
        want = Common::Point(_resolution.x * mode, _resolution.y * mode);
    }

    // Nothing to gain when the UI already owns every drawable pixel.
    if ( want.x <= _resolution.x || want.y <= _resolution.y )
        return;

    // Sanity clamp - a runaway multiplier must not allocate gigabytes.
    if ( want.x > 8192 ) want.x = 8192;
    if ( want.y > 8192 ) want.y = 8192;

    TextSurface = SDL_CreateRGBSurface(0, want.x, want.y, _pixfmt->BitsPerPixel,
                                       _pixfmt->Rmask, _pixfmt->Gmask, _pixfmt->Bmask, _pixfmt->Amask);
    if ( !TextSurface )
        return;

    _textForDrawable = System::GetResolution();

    SDL_SetSurfaceBlendMode(TextSurface, SDL_BLENDMODE_NONE);
    SDL_FillRect(TextSurface, NULL, SDL_MapRGBA(TextSurface->format, 0, 0, 0, 0));

    if ( !textTex )
        glGenTextures(1, &textTex);

    _states.Tex = textTex;
    _states.TexMip = false;
    SetRenderStates(0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TextSurface->w, TextSurface->h, 0, _glPixfmt, _glPixtype, NULL);

    ypa_log_out("gfx: hi-res UI text layer %dx%d (logical %dx%d)\n",
                TextSurface->w, TextSurface->h, _resolution.x, _resolution.y);
}

void GFXEngine::DrawTextLayer()
{
    if ( !TextSurface )
        return;

    // A bare window resize changes the drawable without going through
    // SetResolution; re-fit rather than stretch a stale surface.
    Common::Point drw = System::GetResolution();
    if ( drw != _textForDrawable )
    {
        RecreateTextSurface();
        return;
    }

    // Everything the CPU surface changed since the last upload: this frame's
    // glyphs plus the area ClearTextLayer() just wiped.
    std::vector<SDL_Rect> upload = _textDirtyCur;
    for ( const SDL_Rect &r : _textDirtyPrev )
        RectListAdd(upload, r);

    for ( SDL_Rect &r : upload )
    {
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.x + r.w > TextSurface->w) r.w = TextSurface->w - r.x;
        if (r.y + r.h > TextSurface->h) r.h = TextSurface->h - r.y;
    }

    if ( upload.empty() )
    {
        _textDirtyPrev.swap(_textDirtyCur);
        _textDirtyCur.clear();
        return;
    }

    GfxStates save = _states;

    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);

    SetProjectionMatrix( mat4x4f() );
    SetModelViewMatrix( mat4x4f() );

    _states.DepthTest = false;
    _states.Zwrite = false;
    _states.AlphaBlend = true;
    _states.SrcBlend = GL_SRC_ALPHA;
    _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
    _states.Tex = textTex;
    _states.TexMip = false;
    _states.TexBlend = 2;
    _states.Prog = _stdShaderProg;
    _states.AlphaTest = false;
    _states.Shaded = true;
    _states.LinearFilter = true;

    SetRenderStates(0);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, TextSurface->pitch / TextSurface->format->BytesPerPixel);

    for ( const SDL_Rect &r : upload )
    {
        if ( RectEmpty(r) )
            continue;

        glPixelStorei(GL_UNPACK_SKIP_PIXELS, r.x);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, r.y);

        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h,
                        _glPixfmt, _glPixtype, TextSurface->pixels);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    float ex, ey;
    UiQuadExtent(&ex, &ey);

    std::array<TVertex, 4> vtx = {
        GFX::TVertex( vec3f(-ex,  ey, 0.0), tUtV(0.0, 0.0) ),
        GFX::TVertex( vec3f(-ex, -ey, 0.0), tUtV(0.0, 1.0) ),
        GFX::TVertex( vec3f( ex, -ey, 0.0), tUtV(1.0, 1.0) ),
        GFX::TVertex( vec3f( ex,  ey, 0.0), tUtV(1.0, 0.0) )
    };

    DrawVtxQuad(vtx);

    _states = save;

    _textDirtyPrev.swap(_textDirtyCur);
    _textDirtyCur.clear();
}

void GFXEngine::DrawVtxQuad(const std::array<GFX::TVertex, 4> &vtx)
{
    static const uint16_t indexes[6] = {0, 1, 2, 0, 2, 3};
    
    if (_vbo)
    {
        if (!_stdQuadDataBuf)
        {
            Glext::GLGenBuffers(1, &_stdQuadDataBuf);
            Glext::GLBindBuffer(GL_ARRAY_BUFFER, _stdQuadDataBuf);
            Glext::GLBufferData(GL_ARRAY_BUFFER, sizeof(TVertex) * vtx.size(), NULL, GL_STREAM_DRAW);
        }
        
        if (!_stdQuadIndexBuf)
        {
            Glext::GLGenBuffers(1, &_stdQuadIndexBuf);
            Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _stdQuadIndexBuf);
            Glext::GLBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indexes), &indexes, GL_STATIC_DRAW);
        }
        
        _states.DataBuf = _stdQuadDataBuf;
        _states.IndexBuf = _stdQuadIndexBuf;
        
        SetRenderStates(0);
        
        Glext::GLBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(TVertex) * vtx.size(), vtx.data()); 
        
        if (_lastStates.Prog.PosLoc != -1)
            Glext::GLVertexAttribPointer(_lastStates.Prog.PosLoc, 3, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, Pos));  
        if (_lastStates.Prog.ColorLoc != -1)
            Glext::GLVertexAttribPointer(_lastStates.Prog.ColorLoc, 4, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, Color));
        if (_lastStates.Prog.UVLoc != -1)
            Glext::GLVertexAttribPointer(_lastStates.Prog.UVLoc, 2, GL_FLOAT, GL_FALSE,  sizeof(TVertex), (void *)offsetof(TVertex, TexCoord));
        
        CommitUBOParameters();    
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, NULL);
    }
    else
    {
        glVertexPointer(3, GL_FLOAT, sizeof(TVertex), &vtx[0].Pos);
        glColorPointer(4, GL_FLOAT, sizeof(TVertex), &vtx[0].Color);
        glTexCoordPointer(2, GL_FLOAT, sizeof(TVertex), &vtx[0].TexCoord);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, &indexes);
    }
}

void GFXEngine::DrawScreenSurface()
{
    GfxStates save = _states;
    
    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);
    
    SetProjectionMatrix( mat4x4f() );
    SetModelViewMatrix( mat4x4f() );
    
    _states.DepthTest = false;
    _states.Zwrite = false;
    _states.AlphaBlend = true;
    _states.SrcBlend = GL_SRC_ALPHA;
    _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
    _states.Tex = screenTex;
    _states.TexMip = false;
    _states.TexBlend = 2;
    _states.Prog = _stdShaderProg;
    _states.AlphaTest = false;
    _states.Shaded = true;
    _states.LinearFilter = true;

    bool anyNow = true;
    if ( _scrTiled )
    {
        if ( _scrVerify )
        {
            // Debug: every non-zero tile must have been marked by its writer.
            static std::vector<uint8_t> scan;
            static int frames = 0, missFrames = 0, missTiles = 0;
            ScanScreenTiles(scan);
            int miss = 0;
            for (size_t i = 0; i < scan.size(); i++)
                if ( scan[i] && !_scrTileNow[i] ) { miss++; _scrTileNow[i] = 1; }
            if ( miss ) { missFrames++; missTiles += miss; }
            if ( ++frames % 300 == 0 || (miss && missFrames <= 5) )
                printf("ui-tiled verify: frame %d, %d frames with unmarked tiles (%d tiles), this frame %d; marked %d of %d\n",
                       frames, missFrames, missTiles, miss,
                       (int)std::count(_scrTileNow.begin(), _scrTileNow.end(), 1), (int)_scrTileNow.size());
        }
        anyNow = std::find(_scrTileNow.begin(), _scrTileNow.end(), 1) != _scrTileNow.end();
        // Nothing drawn now and nothing on the GPU: the quad would blend alpha 0
        // everywhere (dst unchanged), so skip the upload and the full-screen pass.
        if ( !anyNow && !_scrGpuAny )
        {
            _states = save;
            return;
        }
    }

    SetRenderStates(0);
    
    // Will be binded with SetRenderStates
    if ( !_scrTiled )
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ScreenSurface->w, ScreenSurface->h, _glPixfmt, _glPixtype, ScreenSurface->pixels);
    else
    {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, ScreenSurface->pitch / 4);
        // Upload every tile that holds something now or held something on the
        // GPU (it must be cleared there), as one rect per group of such tiles.
        _scrMask.resize(_scrTileNow.size());
        for (size_t i = 0; i < _scrMask.size(); i++)
            _scrMask[i] = _scrTileNow[i] | _scrTileGpu[i];
        TileBoxes(_scrMask, _scrBoxes);
        for (const SDL_Rect &b : _scrBoxes)
        {
            int x0 = b.x * SCR_TILE_W, y0 = b.y * SCR_TILE_H;
            int w = std::min(ScreenSurface->w, (b.x + b.w) * SCR_TILE_W) - x0;
            int h = std::min(ScreenSurface->h, (b.y + b.h) * SCR_TILE_H) - y0;
            const uint8_t *px = (const uint8_t *)ScreenSurface->pixels + (size_t)y0 * ScreenSurface->pitch + x0 * 4;
            glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, _glPixfmt, _glPixtype, px);
        }
        _scrTileGpu = _scrTileNow;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        _scrGpuAny = anyNow;
    }

    float ex, ey;
    UiQuadExtent(&ex, &ey);

    std::array<TVertex, 4> vtx = {
        GFX::TVertex( vec3f(-ex,  ey, 0.0), tUtV(0.0, 0.0) ),
        GFX::TVertex( vec3f(-ex, -ey, 0.0), tUtV(0.0, 1.0) ),
        GFX::TVertex( vec3f( ex, -ey, 0.0), tUtV(1.0, 1.0) ),
        GFX::TVertex( vec3f( ex,  ey, 0.0), tUtV(1.0, 0.0) )
    };

    // Tiled path: blend the same full-screen quad, but scissored to the screen
    // area of the tiles holding something (each fragment is computed exactly as
    // in the unscissored draw; outside, every texel is alpha 0 = dst unchanged).
    // At 4K this skips ~90 % of a full-screen blended pass in-game.
    static int scis = -1;
    // UI graphics: OFF by default - measured Apple M-series 4K L7575 Low+UI 2 (2 reps): scissored per tile run 9.46 ms,
    // per tile group 6.83, one full-screen quad 5.88 (= the settings benchmark). The scissored draws cost
    // more than the blended texels they skip. UA_UI_SCISSOR=1 opts back in.
    if ( scis < 0 ) { const char *v = getenv("UA_UI_SCISSOR"); scis = (v && v[0] == '1') ? 1 : 0; }
    int used = 0;
    if ( _scrTiled && scis )
        used = (int)std::count(_scrTileNow.begin(), _scrTileNow.end(), 1);
    if ( !_scrTiled || !scis || used * 2 > (int)_scrTileNow.size() )
        DrawVtxQuad(vtx);
    else
    {
        // Surface px -> window px (quad spans [-ex,ex] x [-ey,ey] of the viewport;
        // window y is bottom-up). One surface texel + 2 px margin covers the
        // GL_LINEAR footprint of a tile edge.
        const double vw = scrSz.x * ex, vh = scrSz.y * ey;
        const double ox = (scrSz.x - vw) * 0.5, oy = (scrSz.y - vh) * 0.5;
        const double kx = vw / ScreenSurface->w, ky = vh / ScreenSurface->h;
        glEnable(GL_SCISSOR_TEST);
        TileBoxes(_scrTileNow, _scrBoxes);
        for (const SDL_Rect &b : _scrBoxes)
        {
            double sx0 = b.x * SCR_TILE_W - 1.0, sx1 = std::min(ScreenSurface->w, (b.x + b.w) * SCR_TILE_W) + 1.0;
            double sy0 = b.y * SCR_TILE_H - 1.0, sy1 = std::min(ScreenSurface->h, (b.y + b.h) * SCR_TILE_H) + 1.0;
            int wx0 = (int)std::floor(ox + sx0 * kx) - 2, wx1 = (int)std::ceil(ox + sx1 * kx) + 2;
            int wyT = (int)std::floor(oy + sy0 * ky) - 2, wyB = (int)std::ceil(oy + sy1 * ky) + 2;
            glScissor(wx0, scrSz.y - wyB, wx1 - wx0, wyB - wyT);
            DrawVtxQuad(vtx);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    _states = save;
}

uint32_t GFXEngine::CompileShader(int32_t type, const std::string &string)
{
    GLuint sh = Glext::GLCreateShader(type);
    if (!sh)
        return 0;
    
    const GLchar *source = (const GLchar *)string.c_str();
    
    Glext::GLShaderSource(sh, 1, &source, 0);
    {
        Perf::Scope perfShd(Perf::SHD);
        Glext::GLCompileShader(sh);
    }
    
    GLint tmpvar;
    Glext::GLGetShaderiv(sh, GL_COMPILE_STATUS, &tmpvar);
    
    if (tmpvar == GL_FALSE)
    {
        Glext::GLGetShaderiv(sh, GL_INFO_LOG_LENGTH, &tmpvar);
        if (tmpvar > 0)
        {
            char *logbuff = new char[tmpvar + 2];
            Glext::GLGetShaderInfoLog(sh, tmpvar, NULL, logbuff);
            printf("Shader error: %s\n", logbuff);
            log_d3dlog("Shader error: %s\n", logbuff);
            delete[] logbuff;
        }
        Glext::GLDeleteShader(sh);
        return 0;
    }
    
    return sh;
}


uint32_t GFXEngine::LoadShader(int32_t type, const std::string &fl)
{
    FSMgr::FileHandle *f = FSMgr::iDir::openFileAlloc(fl, "rb");
    if (!f)
        return 0;

    f->seek(0, SEEK_END);
    size_t sz = f->tell();
    f->seek(0, SEEK_SET);

    char *tmp = new char[sz];
    f->read(tmp, sz);

    std::string b;
    b.assign(tmp, sz);

    delete[] tmp;

    delete f;
    
    return CompileShader(type, b);
}

void GFXEngine::DrawFBO()
{
    GfxStates save = _states;
    
    Common::Point scrSz = System::GetResolution();
    glViewport(0, 0, scrSz.x, scrSz.y);
        
    SetProjectionMatrix( mat4x4f() );
    SetModelViewMatrix( mat4x4f() );
    
    _states.DepthTest = false;
    _states.Zwrite = false;
    _states.AlphaBlend = true;
    
    if (_fboBlend == 0)
    {
        _states.SrcBlend = GL_ONE;
        _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
    }
    else if (_fboBlend == 1)
    {
        _states.SrcBlend = GL_SRC_ALPHA;
        _states.DstBlend = GL_ONE_MINUS_SRC_ALPHA;
    }
    
    _states.Tex = _fboTex;
    _states.TexMip = false;
    _states.TexBlend = 2;
    _states.Prog = _colorEffectsShaderProg;
    
    _states.AlphaTest = false;
    _states.Shaded = true;
    _states.LinearFilter = true;

    // Apply texture and program
    SetRenderStates(0);
    
    if (_colorEffectsShaderProg.NormLoc >= 0)
        Glext::GLUniform3f(_colorEffectsShaderProg.NormLoc, _normClr.x, _normClr.y, _normClr.z);
    
    if (_colorEffectsShaderProg.InvLoc >= 0)
        Glext::GLUniform3f(_colorEffectsShaderProg.InvLoc, _invClr.x, _invClr.y, _invClr.z);
    
    if (_colorEffectsShaderProg.RandLoc >= 0)
        Glext::GLUniform1i(_colorEffectsShaderProg.RandLoc, rand());
    
    if (_colorEffectsShaderProg.ScrSizeLoc >= 0)
        Glext::GLUniform2i(_colorEffectsShaderProg.ScrSizeLoc, scrSz.x, scrSz.y);
    
    if (_colorEffectsShaderProg.MillisecsLoc >= 0)
        Glext::GLUniform1i(_colorEffectsShaderProg.MillisecsLoc, SDL_GetTicks());
    
    static std::array<TVertex, 4> vtx = {
        GFX::TVertex( vec3f(-1.0,  1.0, 0.0), tUtV(0.0, 1.0) ),
        GFX::TVertex( vec3f(-1.0, -1.0, 0.0), tUtV(0.0, 0.0) ),
        GFX::TVertex( vec3f( 1.0, -1.0, 0.0), tUtV(1.0, 0.0) ),
        GFX::TVertex( vec3f( 1.0,  1.0, 0.0), tUtV(1.0, 1.0) )
    };
    
    DrawVtxQuad(vtx);
    
    _states = save;
}
 
void GFXEngine::SetFBOBlending(int mode)
{
    _fboBlend = mode;
}

void GFXEngine::UpdateFBOSizes()
{
    if (_colorEffects)
    {
        Common::Point scrSz = System::GetResolution();

        _states.Tex = _fboTex;
        _states.TexMip = false;
        SetRenderStates(0);

        glTexImage2D(GL_TEXTURE_2D, 0, FBOTEXTYPE, scrSz.x, scrSz.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        Glext::GLBindRenderbuffer(GL_RENDERBUFFER, _fbod);
        Glext::GLRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, scrSz.x, scrSz.y);
        Glext::GLBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
}

SDL_Color GFXEngine::Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_Color tmp;
    tmp.a = a;
    tmp.r = r;
    tmp.g = g;
    tmp.b = b;
    return tmp;
}

bool GFXEngine::ColorCmp(const SDL_Color &a, const SDL_Color &b)
{
    if (a.a != b.a || a.r != b.r || a.g != b.g || a.b != b.b)
        return false;
    return true;
}

float GFXEngine::GetColorEffectPower(int id)
{
    int32_t pwr = 0;
    switch(id)
    {
        default:
        case 0:
            return 1.0;
        
        case 1:
            pwr = System::IniConf::GfxColorEffPower1.Get<int32_t>();
            break;
        
        case 2:
            pwr = System::IniConf::GfxColorEffPower2.Get<int32_t>();
            break;
        
        case 3:
            pwr = System::IniConf::GfxColorEffPower3.Get<int32_t>();
            break;
        
        case 4:
            pwr = System::IniConf::GfxColorEffPower4.Get<int32_t>();
            break;
            
        case 5:
            pwr = System::IniConf::GfxColorEffPower5.Get<int32_t>();
            break;
            
        case 6:
            pwr = System::IniConf::GfxColorEffPower6.Get<int32_t>();
            break;
            
        case 7:
            pwr = System::IniConf::GfxColorEffPower7.Get<int32_t>();
            break;
    }
    
    if (pwr < 0)
        pwr = 0;
    
    if (pwr > 100)
        pwr = 100;
    
    return (float)pwr / 100.0;
}

// ---------------------------------------------------------------------------
// 2D letterbox (gfx.ui_letterbox / UA_UI_LETTERBOX, off by default)
//
// The 2D layer is authored 4:3 and the port stretches it over whatever shape
// the drawable is, so on a widescreen panel every icon, gauge and map cursor is
// horizontally fat. This draws the 2D quads aspect-correct instead, with bars
// where the shapes disagree. Off by default because it changes the framing the
// port has always had - and because in a LEVEL only the 2D layer is boxed: the
// 3D world still fills the drawable, so the HUD frame stops sitting on the
// screen edge. It is unambiguously right for the shell, a taste call in-game.
//
// Everything else - the software surface, layout, hit-testing in logical
// pixels - is untouched; only the final quad's extent and the inverse mapping
// used for the mouse know about the bars.
// ---------------------------------------------------------------------------
bool GFXEngine::UiLetterbox()
{
    if ( !_uiLetterboxResolved )
    {
        _uiLetterboxResolved = true;
        _uiLetterbox = System::IniConf::GfxUiLetterbox.Get<bool>();

        if ( const char *ev = getenv("UA_UI_LETTERBOX") )
            _uiLetterbox = ( ev[0] && ev[0] != '0' );
    }

    return _uiLetterbox;
}

// Fraction of the drawable the 2D layer covers on each axis: (1,1) when
// stretching (the classic behaviour), else <= 1 on the axis with the bars.
void GFXEngine::UiQuadExtent(float *ex, float *ey)
{
    *ex = 1.0f;
    *ey = 1.0f;

    if ( !UiLetterbox() )
        return;

    const Common::Point drw = System::GetResolution();
    if ( drw.x <= 0 || drw.y <= 0 || _resolution.x <= 0 || _resolution.y <= 0 )
        return;

    const double want = (double)_resolution.x / (double)_resolution.y;
    const double have = (double)drw.x / (double)drw.y;

    if ( have > want )
        *ex = (float)(want / have);      // wider drawable  -> pillars L/R
    else if ( have < want )
        *ey = (float)(have / want);      // taller drawable -> bars T/B
}

Common::Point GFXEngine::ConvertPosTo2DStuff(const Common::Point &pos)
{
    Common::Point real = System::GetResolution();

    float ex, ey;
    UiQuadExtent(&ex, &ey);

    if (real == _resolution && ex == 1.0f && ey == 1.0f)
        return pos;

    // Undo the bars before the drawable -> logical divide, or every click in a
    // letterboxed shell lands to the right of / below the thing it hit.
    Common::Point t( pos.x, pos.y );

    if ( ex != 1.0f )
        t.x = (int)((pos.x - real.x * (1.0f - ex) * 0.5f) / ex);
    if ( ey != 1.0f )
        t.y = (int)((pos.y - real.y * (1.0f - ey) * 0.5f) / ey);

    t = Common::Point( t.x * _resolution.x / real.x,
                       t.y * _resolution.y / real.y );

    if (t.x < 0)
        t.x = 0;
    if (t.x >= _resolution.x)
        t.x = _resolution.x - 1;

    if (t.y < 0)
        t.y = 0;
    if (t.y >= _resolution.y)
        t.y = _resolution.y - 1;

    return t;
}

TMesh::TMesh(const TMesh &b)
{
    Mat = b.Mat;
    Vertexes = b.Vertexes;
    Indixes = b.Indixes;
    CoordsCache = b.CoordsCache;
    BoundBox = b.BoundBox;
    
    glDataBuf = 0;
    glIndexBuf = 0;
}

TMesh &TMesh::operator=(const TMesh& b)
{
    Mat = b.Mat;
    Vertexes = b.Vertexes;
    Indixes = b.Indixes;
    CoordsCache = b.CoordsCache;
    BoundBox = b.BoundBox;
    
    glDataBuf = 0;
    glIndexBuf = 0;
    
    return *this;
}

TMesh::~TMesh()
{
    GFX::Engine.MeshFreeVBO(this);
    
    // Just notify about still existing buffers
    if (glDataBuf || glIndexBuf)
        printf("TMesh still has buffers!\n");
}

void GFXEngine::MeshMakeVBO(TMesh *mesh)
{
    Perf::Scope perfVbo(Perf::VBO);
    if (_vbo)
    {        
        if (!mesh->glDataBuf)
            Glext::GLGenBuffers(1, &mesh->glDataBuf);
        
        if (!mesh->glIndexBuf)
            Glext::GLGenBuffers(1, &mesh->glIndexBuf);
                
        Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->glIndexBuf);
        Glext::GLBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh->Indixes.size() * sizeof(IndexType), mesh->Indixes.data(), GL_STATIC_DRAW);
        
        Glext::GLBindBuffer(GL_ARRAY_BUFFER, mesh->glDataBuf);
        int32_t vtxDataSz = mesh->Vertexes.size() * sizeof(TVertex);
        int32_t coordDataSz = mesh->Vertexes.size() * sizeof(tUtV);
        Glext::GLBufferData(GL_ARRAY_BUFFER, vtxDataSz + mesh->CoordsCache.size() * coordDataSz, NULL, GL_STATIC_DRAW);
        
        int32_t off = 0;
        Glext::GLBufferSubData(GL_ARRAY_BUFFER, off, vtxDataSz, mesh->Vertexes.data());
        
        off += vtxDataSz;
        for (TCoordsCache &cch : mesh->CoordsCache)
        {
            Glext::GLBufferSubData(GL_ARRAY_BUFFER, off, coordDataSz, cch.Coords.data());
            cch.BufferPos = off;
            
            off += coordDataSz;
        }
        
        Glext::GLBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _lastStates.IndexBuf);
        Glext::GLBindBuffer(GL_ARRAY_BUFFER, _lastStates.DataBuf);  
    }
}

void GFXEngine::MeshFreeVBO(TMesh *mesh)
{
    if (_vbo)
    {
        if (mesh->glDataBuf)
        {
            ForgetBuffer(mesh->glDataBuf);
            Glext::GLDeleteBuffers(1, &mesh->glDataBuf);
            mesh->glDataBuf = 0;
        }
        
        if (mesh->glIndexBuf)
        {
            ForgetBuffer(mesh->glIndexBuf);
            Glext::GLDeleteBuffers(1, &mesh->glIndexBuf);
            mesh->glIndexBuf = 0;
        }
        
//        if (mesh->glVao)
//        {
//            Glext::GLDeleteVertexArrays(1, &mesh->glVao);
//            mesh->glVao = 0;
//        }
    }
}

void GFXEngine::BindVBOParameters(TShaderProg &shader)
{
    if (_vbo)
    {
        uint32_t blockIndex = Glext::GLGetUniformBlockIndex(shader.ID, "Parameters");   
        if (blockIndex != GL_INVALID_INDEX)
            Glext::GLUniformBlockBinding(shader.ID, blockIndex, _vboParamsBlockBinding);
    }
}

void GFXEngine::CommitUBOParameters()
{
    if (_vbo && _vboStatesChanged)
    {
        _vboStatesChanged = false;
        
        Glext::GLBindBuffer(GL_UNIFORM_BUFFER, _vboParams);
        
        // Orphan ubo
        Glext::GLBufferData(GL_UNIFORM_BUFFER, _vboParamsSize, NULL, GL_STREAM_DRAW);
        
        Glext::GLBufferData(GL_UNIFORM_BUFFER, _vboParamsSize, &_vboStatesBlock, GL_STREAM_DRAW);
    }
}

void GFXEngine::SetProjectionMatrix(const mat4x4f &mat)
{
    mat4x4f tmp = mat.Transpose();
    if (_vbo)
    {
        _vboStatesBlock.Proj = tmp;
        _vboStatesChanged = true;
    }
    else
    {
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(&tmp.m00);
    }
}

void GFXEngine::SetModelViewMatrix(const mat4x4f &mat)
{
    mat4x4f tmp = mat.Transpose();
    if (_vbo)
    {
        _vboStatesBlock.View = tmp;
        _vboStatesChanged = true;
    }
    else
    {
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(&tmp.m00);
    }
}

TShaderProg::TShaderProg(uint32_t id)
: ID(id)
{
    PosLoc = Glext::GLGetAttribLocation(ID, "vPos");
    UVLoc  = Glext::GLGetAttribLocation(ID, "vUV");
    ColorLoc = Glext::GLGetAttribLocation(ID, "vColor");
    NormalLoc = Glext::GLGetAttribLocation(ID, "vNormal");
    TangentLoc = Glext::GLGetAttribLocation(ID, "vTangent");
}

TColorEffectsProg::TColorEffectsProg(uint32_t id)
: TShaderProg(id)
{
    NormLoc = Glext::GLGetUniformLocation(ID, "normclr");
    InvLoc = Glext::GLGetUniformLocation(ID, "invclr");
    RandLoc = Glext::GLGetUniformLocation(ID, "randval");
    ScrSizeLoc = Glext::GLGetUniformLocation(ID, "screenSize");
    MillisecsLoc = Glext::GLGetUniformLocation(ID, "millisecs");
}


// ---------------------------------------------------------------------------
// Reforge renderer (UA_RENDER_V2) - per-draw setup for a TPbrMaterial mesh.
// Called right after SetRenderStates() made _v2ShaderProg current and bound
// the albedo on unit 0 through the state cache. Units 1-3 are touched ONLY
// here and are not cached: they are rebound for every V2 draw, so a deleted
// texture name can never be re-bound from a stale cache entry.
// Unit 0 is re-selected before returning - the rest of the engine assumes it.
//
// Sun: direction and strength fitted to the baked per-polygon shade values of
// the original .bas meshes (least squares), in
// UA world space (x right, y DOWN, z forward).
// Fit over 6512 shaded, textured polygons of level 1: key from
// -x/-z at 44 deg elevation; display-space ambient 0.691 + sun 0.172 (full-lit
// 0.863). R^2 is only 0.06 - UA's baked shade is mostly flat per-face artistry
// with a weak directional bias, so the V2 sun is deliberately gentle.
// UA_R2_SUN="x,y,z,sunLin,ambLin" overrides for look-dev.
static vec3d kV2SunWorld = vec3d(-0.551, -0.695, -0.462);
static float kV2SunLin = 0.2809;
static float kV2AmbLin = 0.4349;
// Far-read brightness calibration, all inert unless set:
// UA_R2_EXPOSURE=k        linear scale of the lit colour before the tonemap
// UA_R2_HEMI=sky,ground   hemispheric ambient (linear): ambient = mid + half *
//                         dot(N, world up), mid = (sky+ground)/2 replaces the
//                         UA_R2_SUN ambient, half = (sky-ground)/2
// UA_R2_TONEMAP=shoulder  Khronos PBR Neutral WITHOUT its toe (default): the
//                         toe subtracts a per-pixel constant taken from the
//                         min channel (x - 6.25x^2 below 0.08), which crushes
//                         UA's dark albedos quadratically and blows up the
//                         chroma of near-greys (colour-parity notes)
//              =neutral   the full Khronos curve (pre-parity behaviour)
//              =0|clamp   no tonemap, clamp only
// UA_R2_AO=0|1            ORM.r occludes the ambient term (default 0: the
//                         reforge albedo already carries baked AO, and the
//                         Blender material never applies ORM.r)
// UA_R2_SRGBTEX=0|1       V2 albedo as GL sRGB texture (default 1)
// UA_R2_DEBUG=albedo|normal|orm|uv|base|lit|gnormal|ndv   unlit debug output (V2 meshes)
//
// Far fade v2. Everything below scales with a per-draw FAR FADE
// f = 1 - smoothstep(px0, px1, D), D = the mesh's projected bounding-sphere
// diameter in vertical pixels, so it is exactly 0 (and the shader path is the
// old one) once D >= px1 - close-ups and the default turntable are untouched.
// UA_R2_FARPX=px0,px1        fade range in D px (default 64,160; D is ~0.86x the viewer's
//                            silhouette span for VP_PANZ1: full <= ~75 px span, 0 >= ~185 px span;
//                            the default turntable is D 360-400, the 64/32 px far read D 55/27)
// UA_R2_FARSPEC=k[,g]        far matte: cut k*f of the dielectric specular + ambient-Fresnel terms and
//                            scale the diffuse terms by 1+(g-1)*f instead. DEFAULT 1,1.9: that grey
//                            additive floor was what desaturated the far read and lifted its blacks and
//                            silhouette; g = 1.9 keeps top planes at their albedo value.
//                            UA_R2_FARSPEC=0 = the pre-far-v2 far look.
// UA_R2_FAREDGE=s[,n0,n1]    silhouette darkening: lit *= 1 - s*f*(1-smoothstep(n1,n0,N.V)), geometric
//                            normal. Default OFF; proposed default 0.7,0.75,0.35 (UA_R2_FARV2=1).
// UA_R2_FARV2=0|1            0 = everything far-v2 off (pre-far-v2 V2); 1 = the proposed set (far matte +
//                            silhouette darkening 0.7,0.75,0.35). Single knobs below override it.
// UA_R2_NORMALMIP=renorm|toksvig|fade[,l0,l1]   filtered-normal handling (default renorm = as before)
// UA_R2_NORMALMAP=0          ignore the normal map (A/B only)
// UA_R2_TERMS=mask           lighting terms: 1 sun diffuse 2 sun spec 4 amb diffuse 8 amb Fresnel 16 emissive
// UA_R2_FARLOG=1             print D and f of the first V2 draws of each frame size (calibration)
static float kV2Exposure = 1.0f;
static float kV2HemiHalf = 0.0f;
static int kV2ToneMap = 2;
static int kV2UseAo = 0;
static int kV2Debug = 0;
static int kV2Terms = 31;
static float kV2FarPx0 = 64.0f, kV2FarPx1 = 160.0f;
static float kV2FarEdge = 0.0f, kV2FarEdgeN0 = 0.75f, kV2FarEdgeN1 = 0.35f;
static float kV2FarSpec = 1.0f, kV2FarGain = 1.9f;
static int kV2NormalMip = 0;
static float kV2FarNormLod0 = 4.0f, kV2FarNormLod1 = 5.5f;
static bool kV2NormalMap = true;
static int kV2FarLog = 0;
#define V2_KNOBS(X) X(kV2SunWorld) X(kV2SunLin) X(kV2AmbLin) X(kV2Exposure) X(kV2HemiHalf) X(kV2ToneMap) \
    X(kV2UseAo) X(kV2Debug) X(kV2Terms) X(kV2FarPx0) X(kV2FarPx1) X(kV2FarEdge) X(kV2FarEdgeN0) X(kV2FarEdgeN1) \
    X(kV2FarSpec) X(kV2FarGain) X(kV2NormalMip) X(kV2FarNormLod0) X(kV2FarNormLod1) X(kV2NormalMap) X(kV2FarLog)
static void V2SunOverride()
{
    // Re-read when the graphics settings changed (V2 level -> calibration and
    // far-v2 values via GfxSettings::Param; env vars still win), starting
    // from the compiled-in defaults each time.
    static uint32_t doneGen = 0;
    if (doneGen == GfxSettings::Generation())
        return;
    const bool first = doneGen == 0;
    doneGen = GfxSettings::Generation();
#define V2_SAVE(v) static decltype(v) def_##v = v;
#define V2_RESTORE(v) v = def_##v;
    V2_KNOBS(V2_SAVE)
    if (!first)
    {
        V2_KNOBS(V2_RESTORE)
    }
#undef V2_SAVE
#undef V2_RESTORE
    if (const char *v = GfxSettings::Param("UA_R2_SUN"))
    {
        double x, y, z;
        float sl, al;
        if (sscanf(v, "%lf,%lf,%lf,%f,%f", &x, &y, &z, &sl, &al) == 5)
        {
            kV2SunWorld = vec3d(x, y, z);
            kV2SunLin = sl;
            kV2AmbLin = al;
        }
    }
    if (const char *v = GfxSettings::Param("UA_R2_EXPOSURE"))
    {
        float e;
        if (sscanf(v, "%f", &e) == 1 && e > 0.0f)
            kV2Exposure = e;
    }
    if (const char *v = getenv("UA_R2_HEMI"))
    {
        float sky, ground;
        if (sscanf(v, "%f,%f", &sky, &ground) == 2)
        {
            kV2AmbLin = 0.5f * (sky + ground);
            kV2HemiHalf = 0.5f * (sky - ground);
        }
    }
    if (const char *v = getenv("UA_R2_TONEMAP"))
    {
        if (!strcmp(v, "0") || !strcmp(v, "clamp"))
            kV2ToneMap = 0;
        else if (!strcmp(v, "neutral") || !strcmp(v, "1"))
            kV2ToneMap = 1;
        else
            kV2ToneMap = 2;
    }
    if (const char *v = getenv("UA_R2_AO"))
        kV2UseAo = (v[0] == '1') ? 1 : 0;
    if (const char *v = getenv("UA_R2_DEBUG"))
    {
        static const char *modes[] = {"", "albedo", "normal", "orm", "uv", "base", "lit", "gnormal", "ndv"};
        for (int i = 1; i < 9; ++i)
            if (!strcmp(v, modes[i]))
                kV2Debug = i;
    }
    // far-v2 preset first, so the single knobs below can override parts of it.
    if (const char *v = GfxSettings::Param("UA_R2_FARV2"))
    {
        if (v[0] == '0')
        {
            kV2FarSpec = 0.0f;
            kV2FarGain = 1.0f;
            kV2FarEdge = 0.0f;
        }
        else if (v[0] == '1')
            kV2FarEdge = 0.7f;
    }
    if (const char *v = getenv("UA_R2_TERMS"))
        kV2Terms = atoi(v) & 31;
    if (const char *v = getenv("UA_R2_FARPX"))
    {
        float a, b;
        if (sscanf(v, "%f,%f", &a, &b) == 2 && b > a && a >= 0.0f)
        {
            kV2FarPx0 = a;
            kV2FarPx1 = b;
        }
    }
    if (const char *v = GfxSettings::Param("UA_R2_FAREDGE"))
    {
        float s, n0, n1;
        int n = sscanf(v, "%f,%f,%f", &s, &n0, &n1);
        if (n >= 1)
            kV2FarEdge = std::max(0.0f, std::min(1.0f, s));
        if (n == 3 && n0 > n1)
        {
            kV2FarEdgeN0 = n0;
            kV2FarEdgeN1 = n1;
        }
    }
    if (const char *v = GfxSettings::Param("UA_R2_FARSPEC"))
    {
        float k, g;
        int n = sscanf(v, "%f,%f", &k, &g);
        if (n >= 1)
        {
            kV2FarSpec = std::max(0.0f, std::min(1.0f, k));
            kV2FarGain = kV2FarSpec > 0.0f ? kV2FarGain : 1.0f;
        }
        if (n == 2 && g > 0.0f)
            kV2FarGain = g;
    }
    if (const char *v = getenv("UA_R2_NORMALMIP"))
    {
        if (!strncmp(v, "toksvig", 7))
            kV2NormalMip = 1;
        else if (!strncmp(v, "fade", 4))
        {
            kV2NormalMip = 2;
            float a, b;
            if (sscanf(v, "fade,%f,%f", &a, &b) == 2 && b > a)
            {
                kV2FarNormLod0 = a;
                kV2FarNormLod1 = b;
            }
        }
        else
            kV2NormalMip = 0;
    }
    if (const char *v = GfxSettings::Param("UA_R2_NORMALMAP"))
        kV2NormalMap = v[0] != '0';
    if (const char *v = getenv("UA_R2_FARLOG"))
        kV2FarLog = atoi(v);
    printf("UA_RENDER_V2: sun (%.3f,%.3f,%.3f) sunLin %.3f ambLin %.3f hemiHalf %.3f exposure %.3f tonemap %d ao %d debug %d\n",
           kV2SunWorld.x, kV2SunWorld.y, kV2SunWorld.z, kV2SunLin, kV2AmbLin, kV2HemiHalf, kV2Exposure, kV2ToneMap,
           kV2UseAo, kV2Debug);
    printf("UA_RENDER_V2: far-v2 px %.0f..%.0f edge %.2f (N.V %.2f..%.2f) spec cut %.2f gain %.2f normalmip %d "
           "(lod %.1f..%.1f) normalmap %d terms %d\n", kV2FarPx0, kV2FarPx1, kV2FarEdge, kV2FarEdgeN0, kV2FarEdgeN1,
           kV2FarSpec, kV2FarGain, kV2NormalMip, kV2FarNormLod0, kV2FarNormLod1, kV2NormalMap ? 1 : 0, kV2Terms);
}

void GFXEngine::V2SunParams(float sun[3], float *sunLin, float *ambLin, float *hemiHalf)
{
    V2SunOverride();
    vec3d s = kV2SunWorld;
    const Atmos::State &atmS = Atmos::Get();
    if (atmS.active) // ATMOSPHERE: the level's sky sun is THE sun (atmos.h)
        s = vec3d(atmS.sunDir[0], atmS.sunDir[1], atmS.sunDir[2]);
    s.normalise();
    sun[0] = s.x; sun[1] = s.y; sun[2] = s.z;
    *sunLin = kV2SunLin;
    *ambLin = kV2AmbLin;
    *hemiHalf = kV2HemiHalf;
}

// far-v2 fade for one draw: 1 when the mesh is small on screen, 0 at close range.
static float V2FarFade(const TRenderNode *nod)
{
    if (!nod->Mesh || !nod->Mesh->BoundBox.Inited)
        return 0.0f;
    const TBoundBox &bb = nod->Mesh->BoundBox;
    const mat4x4 &m = nod->TForm;
    vec3d c = m.Transform((bb.Min + bb.Max) * 0.5);
    if (c.z <= 1.0)
        return 0.0f;
    // View-space radius: object radius x the y row's scale (the y row carries
    // the aspect correction, UAFrustum maps y/z = +-1 to the viewport edges).
    double r = (bb.Max - bb.Min).length() * 0.5 * sqrt(m.m10 * m.m10 + m.m11 * m.m11 + m.m12 * m.m12);
    Common::Point res = System::GetResolution();
    double D = 2.0 * r / c.z * 0.5 * res.y;
    float f = 1.0f;
    if (D >= kV2FarPx1)
        f = 0.0f;
    else if (D > kV2FarPx0)
    {
        float t = (float)((D - kV2FarPx0) / (kV2FarPx1 - kV2FarPx0));
        f = 1.0f - t * t * (3.0f - 2.0f * t);
    }
    if (kV2FarLog)
    {
        static double lastD = -1.0;
        static int lines = 0;
        if (fabs(D - lastD) > 0.5 && lines < 200)
        {
            printf("UA_RENDER_V2: far-v2 D %.1f px (r %.2f z %.1f) fade %.3f\n", D, r, c.z, f);
            lastD = D;
            lines++;
        }
    }
    return f;
}

void GFXEngine::RenderingMeshV2Setup(TRenderNode *nod)
{
    const TPbrMaterial *pbr = nod->Pbr;

    V2SunOverride();

    Glext::GLVertexAttribPointer(_v2ShaderProg.NormalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(TVertex), (void *)offsetof(TVertex, Normal));
    Glext::GLVertexAttribPointer(_v2ShaderProg.TangentLoc, 4, GL_FLOAT, GL_FALSE, sizeof(TVertex), (void *)offsetof(TVertex, Tangent));

    const Atmos::State &atmS = Atmos::Get();
    const vec3d sunW = atmS.active ? vec3d(atmS.sunDir[0], atmS.sunDir[1], atmS.sunDir[2]) : kV2SunWorld;
    vec3d sun = sunW;
    vec3d up(0.0, -1.0, 0.0); // UA world is y DOWN
    if ( TF::TForm3D *view = TF::Engine.GetViewPoint() )
    {
        sun = view->CalcSclRot.Transform(sunW);
        up = view->CalcSclRot.Transform(up);
    }
    sun.normalise();
    up.normalise();
    Glext::GLUniform1f(_v2Loc.Exposure, kV2Exposure);
    Glext::GLUniform3f(_v2Loc.HemiUp, up.x, up.y, up.z);
    Glext::GLUniform3f(_v2Loc.HemiDelta, kV2HemiHalf, kV2HemiHalf, kV2HemiHalf);
    Glext::GLUniform1i(_v2Loc.ToneMap, kV2ToneMap);
    Glext::GLUniform1i(_v2Loc.UseAo, kV2UseAo);
    Glext::GLUniform1i(_v2Loc.Debug, kV2Debug);
    Glext::GLUniform1f(_v2Loc.NormalScale, pbr->NormalScale);
    Glext::GLUniform1i(_v2Loc.NormalRG, (pbr->Normal && pbr->Normal->hwTexRG) ? 1 : 0);
    Glext::GLUniform1i(_v2Loc.Terms, kV2Terms);
    Glext::GLUniform1i(_v2Loc.NormalMip, kV2NormalMip);
    Glext::GLUniform2f(_v2Loc.FarNormLod, kV2FarNormLod0, kV2FarNormLod1);
    {
        const float f = (kV2FarEdge > 0.0f || kV2FarSpec > 0.0f || kV2FarLog || _shadowFrame) ? V2FarFade(nod) : 0.0f;
        ShadowV2Draw(f); // reforge shadows: no GL call at level 0
        Glext::GLUniform4f(_v2Loc.FarLod, kV2FarEdge * f, kV2FarEdgeN0, kV2FarEdgeN1, kV2FarSpec * f);
        Glext::GLUniform1f(_v2Loc.FarGain, 1.0f + (kV2FarGain - 1.0f) * f);
    }
    Perf::AddV2Draw();
    Glext::GLUniform1i(_v2Loc.AlbedoLinear, (nod->Tex && nod->Tex->hwTexSrgb) ? 1 : 0);

    Glext::GLUniform3f(_v2Loc.SunDir, sun.x, sun.y, sun.z);
    if (atmS.active)
    {
        // ATMOSPHERE: the level's sky light (tints carry the strength), emissive x hostLight on night maps
        Glext::GLUniform3f(_v2Loc.SunColor, kV2SunLin * atmS.sunTint[0], kV2SunLin * atmS.sunTint[1],
                           kV2SunLin * atmS.sunTint[2]);
        Glext::GLUniform3f(_v2Loc.Ambient, kV2AmbLin * atmS.ambTint[0], kV2AmbLin * atmS.ambTint[1],
                           kV2AmbLin * atmS.ambTint[2]);
        Glext::GLUniform3f(_v2Loc.Emission, pbr->Emission[0] * atmS.hostLight, pbr->Emission[1] * atmS.hostLight,
                           pbr->Emission[2] * atmS.hostLight);
        float sv[3] = {(float)sun.x, (float)sun.y, (float)sun.z};
        AtmosV2Setup(nod, sv);
    }
    else
    {
    Glext::GLUniform3f(_v2Loc.SunColor, kV2SunLin, kV2SunLin, kV2SunLin);
    Glext::GLUniform3f(_v2Loc.Ambient, kV2AmbLin, kV2AmbLin, kV2AmbLin);
    Glext::GLUniform3f(_v2Loc.Emission, pbr->Emission[0], pbr->Emission[1], pbr->Emission[2]);
    }
    Glext::GLUniform2f(_v2Loc.RoughMetal, pbr->Roughness, pbr->Metallic);

    const ResBitmap *maps[3] = {kV2NormalMap ? pbr->Normal : NULL, pbr->Orm, pbr->Emissive};
    const int32_t has[3] = {_v2Loc.HasNormal, _v2Loc.HasOrm, _v2Loc.HasEmissive};
    static const uint8_t kDefaultTexel[3][4] = {{128, 128, 255, 255}, {255, 255, 255, 255}, {0, 0, 0, 255}};
    for (int i = 0; i < 3; ++i)
    {
        GLuint t = (maps[i] && maps[i]->hwTex) ? maps[i]->hwTex : 0;
        Glext::GLUniform1i(has[i], t ? 1 : 0);
        Glext::GLActiveTexture(GL_TEXTURE1 + i);
        if (!t)
        {
            if (!_v2DefaultTex[i])
            {
                glGenTextures(1, &_v2DefaultTex[i]);
                glBindTexture(GL_TEXTURE_2D, _v2DefaultTex[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kDefaultTexel[i]);
            }
            t = _v2DefaultTex[i];
        }
        glBindTexture(GL_TEXTURE_2D, t);
    }
    Glext::GLActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// Pre-compressed maps. DDS, DX10 header (or legacy
// DXT1/DXT5/ATI2 FourCC), full mip chain stored in the file.
// ---------------------------------------------------------------------------
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif

namespace
{
typedef void (APIENTRY *PfnCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);

PfnCompressedTexImage2D CompressedTexImage2D()
{
    static PfnCompressedTexImage2D fn = NULL;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = (PfnCompressedTexImage2D)SDL_GL_GetProcAddress("glCompressedTexImage2D");
    }
    return fn;
}

// Upload one 4x4 block of the format; true when the GL accepts it.
bool ProbeCompressed(GLenum fmt, int blockBytes)
{
    PfnCompressedTexImage2D up = CompressedTexImage2D();
    if (!up)
        return false;
    uint8_t blk[16] = {0};
    while (glGetError() != GL_NO_ERROR) {}
    up(GL_TEXTURE_2D, 0, fmt, 4, 4, 0, blockBytes, blk);
    GLint comp = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &comp);
    return glGetError() == GL_NO_ERROR && comp;
}
}

const GFXEngine::TexCompCaps &GFXEngine::GetTexCompCaps()
{
    static TexCompCaps caps;
    static bool done = false;
    if (done)
        return caps;
    done = true;

    GLuint t = 0;
    glGenTextures(1, &t);
    _states.Tex = t;
    _states.TexMip = false;
    SetRenderStates(0);
    caps.s3tc = ProbeCompressed(GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8) && ProbeCompressed(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16);
    caps.s3tcSrgb = caps.s3tc && ProbeCompressed(GL_COMPRESSED_SRGB_S3TC_DXT1_EXT, 8);
    caps.rgtc = ProbeCompressed(GL_COMPRESSED_RG_RGTC2, 16);
    caps.bptc = ProbeCompressed(GL_COMPRESSED_RGBA_BPTC_UNORM, 16);
    ForgetTexture(t);
    glDeleteTextures(1, &t);

    printf("texcomp: GL '%s' / '%s': s3tc(BC1/BC3)=%d s3tc_srgb=%d rgtc(BC5)=%d bptc(BC7)=%d\n",
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION),
           caps.s3tc, caps.s3tcSrgb, caps.rgtc, caps.bptc);
    return caps;
}

ResBitmap *GFXEngine::LoadDdsTexture(const std::string &path, bool srgb)
{
    std::vector<uint8_t> buf;
    {
        Perf::Scope perfDec(Perf::DEC);
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return NULL;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 148)
        {
            buf.resize(sz);
            if (fread(buf.data(), 1, sz, f) != (size_t)sz)
                buf.clear();
        }
        fclose(f);
    }
    auto rd32 = [&buf](size_t o) -> uint32_t
    { return buf[o] | (buf[o + 1] << 8) | (buf[o + 2] << 16) | ((uint32_t)buf[o + 3] << 24); };

    if (buf.size() < 128 || memcmp(buf.data(), "DDS ", 4) != 0 || rd32(4) != 124)
    {
        printf("texcomp: '%s' is not a DDS file\n", path.c_str());
        return NULL;
    }
    const int h = rd32(4 + 8), w = rd32(4 + 12);
    int levels = std::max(1, (int)rd32(4 + 24));
    const uint32_t fourcc = rd32(4 + 80);
    size_t off = 4 + 124;

    enum { F_NONE, F_BC1, F_BC3, F_BC5, F_BC7 } kind = F_NONE;
    bool fileSrgb = false;
    if (fourcc == 0x30315844) // 'DX10'
    {
        if (buf.size() < off + 20)
            return NULL;
        switch (rd32(off))
        {
        case 71: kind = F_BC1; break;
        case 72: kind = F_BC1; fileSrgb = true; break;
        case 77: kind = F_BC3; break;
        case 78: kind = F_BC3; fileSrgb = true; break;
        case 83: kind = F_BC5; break;
        case 98: kind = F_BC7; break;
        case 99: kind = F_BC7; fileSrgb = true; break;
        default: break;
        }
        off += 20;
    }
    else if (fourcc == 0x31545844) kind = F_BC1;               // 'DXT1'
    else if (fourcc == 0x35545844) kind = F_BC3;               // 'DXT5'
    else if (fourcc == 0x32495441 || fourcc == 0x55354342) kind = F_BC5; // 'ATI2' / 'BC5U'
    (void)fileSrgb; // the material decides sRGB (same bits either way)

    const TexCompCaps &caps = GetTexCompCaps();
    GLenum fmt = 0;
    int blockBytes = 16;
    switch (kind)
    {
    case F_BC1:
        if (!caps.s3tc || (srgb && !caps.s3tcSrgb)) break;
        fmt = srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        blockBytes = 8;
        break;
    case F_BC3:
        if (!caps.s3tc || (srgb && !caps.s3tcSrgb)) break;
        fmt = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        break;
    case F_BC5:
        if (!caps.rgtc) break;
        fmt = GL_COMPRESSED_RG_RGTC2;
        break;
    case F_BC7:
        if (!caps.bptc) break;
        fmt = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
        break;
    default:
        break;
    }
    if (!fmt || w <= 0 || h <= 0 || w > 16384 || h > 16384)
    {
        printf("texcomp: '%s' format not supported here (fourcc %08x) - falling back\n", path.c_str(), fourcc);
        return NULL;
    }

    // Validate the chain fits in the file before touching GL.
    size_t need = off;
    for (int i = 0, lw = w, lh = h; i < levels; ++i, lw = std::max(1, lw / 2), lh = std::max(1, lh / 2))
        need += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * blockBytes;
    if (need > buf.size())
    {
        printf("texcomp: '%s' truncated (%zu < %zu bytes) - falling back\n", path.c_str(), buf.size(), need);
        return NULL;
    }

    PfnCompressedTexImage2D up = CompressedTexImage2D();
    ResBitmap *bm = new ResBitmap();
    bm->width = w;
    bm->height = h;

    Perf::Scope perfTex(Perf::TEX);
    glGenTextures(1, &bm->hwTex);
    _states.Tex = bm->hwTex;
    _states.TexMip = false;
    SetRenderStates(0);

    const bool mips = levels > 1;
    bm->hwTexMips = mips;
    bm->hwTexSrgb = srgb && (kind != F_BC5);
    bm->hwTexRG = (kind == F_BC5);
    bm->hwHd = true;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    if (mips && _maxAniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, _maxAniso);

    while (glGetError() != GL_NO_ERROR) {}
    uint32_t bytes = 0;
    for (int i = 0, lw = w, lh = h; i < levels; ++i, lw = std::max(1, lw / 2), lh = std::max(1, lh / 2))
    {
        GLsizei n = ((lw + 3) / 4) * ((lh + 3) / 4) * blockBytes;
        up(GL_TEXTURE_2D, i, fmt, lw, lh, 0, n, buf.data() + off);
        off += n;
        bytes += n;
    }
    if (glGetError() != GL_NO_ERROR)
    {
        printf("texcomp: '%s' upload failed - falling back\n", path.c_str());
        ForgetTexture(bm->hwTex);
        glDeleteTextures(1, &bm->hwTex);
        delete bm;
        return NULL;
    }
    bm->hwBytes = bytes;
    Perf::TexAlloc(true, bytes);
    return bm;
}

bool GFXEngine::V2AlbedoSrgb() const
{
    if (!_renderV2)
        return false;
    const char *v = getenv("UA_R2_SRGBTEX");
    return !(v && v[0] == '0');
}

ResBitmap *GFXEngine::LoadRawTexture(SDL_Surface *src, const std::vector<SDL_Surface *> *customMips, bool srgb)
{
    if (!src)
        return NULL;

    SDL_Surface *conv = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0); // bytes R,G,B,A
    if (!conv)
        return NULL;

    ResBitmap *bm = new ResBitmap();
    bm->width = src->w;
    bm->height = src->h;

    Perf::Scope perfTex(Perf::TEX);
    glGenTextures(1, &bm->hwTex);

    // Through the state cache so unit 0's cached binding stays truthful.
    _states.Tex = bm->hwTex;
    _states.TexMip = false;
    SetRenderStates(0);

    // Authored levels 1..nCustom (reforge far-read mips). Without
    // glGenerateMipmap the chain is capped at the last authored level.
    const int nCustom = customMips ? (int)customMips->size() : 0;
    bool mips = Glext::GLGenerateMipmap != NULL || nCustom > 0;
    bm->hwTexMips = mips;
    bm->hwTexSrgb = srgb;
    bm->hwBytes = (uint32_t)Perf::RgbaBytes(src->w, src->h, mips);
    bm->hwHd = true;
    Perf::TexAlloc(true, bm->hwBytes);
    const GLint ifmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (mips && _maxAniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, _maxAniso);

    SDL_LockSurface(conv);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, conv->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, conv->w, conv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, conv->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);

    if (nCustom == 0)
    {
        if (mips)
            Glext::GLGenerateMipmap(GL_TEXTURE_2D);
        return bm;
    }

    for (int i = 0; i < nCustom; ++i)
    {
        SDL_Surface *lv = SDL_ConvertSurfaceFormat((*customMips)[i], SDL_PIXELFORMAT_ABGR8888, 0);
        if (!lv)
            continue;
        SDL_LockSurface(lv);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, lv->pitch / 4);
        glTexImage2D(GL_TEXTURE_2D, i + 1, ifmt, lv->w, lv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv->pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        SDL_UnlockSurface(lv);
        SDL_FreeSurface(lv);
    }

    if (Glext::GLGenerateMipmap)
    {
        // Generate only the levels below the last authored one, from it.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, nCustom);
        Glext::GLGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    }
    else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nCustom);

    return bm;
}

// UA_FRAMETIME_LOG=<file>: appends "<ms since first frame> <frame ms>" per
// finished 3D frame. Inert (one cached getenv) when unset.
void GFXEngine::LogFrameTime()
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *p = getenv("UA_FRAMETIME_LOG");
        enabled = (p && p[0]) ? 1 : 0;
        if (enabled)
        {
            _frameLog = fopen(p, "w");
            if (!_frameLog)
                enabled = 0;
            else
                fprintf(_frameLog, "# t_ms frame_ms tex_ms vbo_ms shd_ms dec_ms polys v2draws proc_ms rnd_ms swap_ms v2=%d\n", _renderV2 ? 1 : 0);
        }
    }

    if (!enabled)
        return;

    static uint64_t first = 0;
    uint64_t now = SDL_GetPerformanceCounter();
    double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (!first)
        first = now;
    Perf::FrameCounters fc;
    Perf::TakeFrame(&fc);
    if (_frameLogLast)
    {
        static uint32_t n = 0;
        static double nextMem = 0.0;
        double t = (now - first) * f;
        fprintf(_frameLog, "%.3f %.3f %.2f %.2f %.2f %.2f %d %d %.2f %.2f %.2f\n", t, (now - _frameLogLast) * f,
                fc.ms[Perf::TEX], fc.ms[Perf::VBO], fc.ms[Perf::SHD], fc.ms[Perf::DEC], fc.polys, fc.v2draws,
                fc.ms[Perf::PROC], fc.ms[Perf::RND], fc.ms[Perf::SWAP]);
        if (t >= nextMem)
        {
            double rss, fp;
            Perf::MemoryMB(&rss, &fp);
            fprintf(_frameLog, "# mem t=%.0f rss_mb=%.0f footprint_mb=%.0f tex_orig_mb=%.1f tex_hd_mb=%.1f\n", t, rss, fp,
                    Perf::TexBytes(false) / 1048576.0, Perf::TexBytes(true) / 1048576.0);
            nextMem = t + 5000.0;
        }
        if ((++n % 120) == 0)
            fflush(_frameLog);
    }
    _frameLogLast = now;
}

}
