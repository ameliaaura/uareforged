#ifndef BITMAP_H_INCLUDED
#define BITMAP_H_INCLUDED

#include "rsrc.h"
#include "system/gfx.h"


class NC_STACK_bitmap: public NC_STACK_rsrc
{
    
public:
    virtual size_t Init(IDVList &stak);
    virtual size_t Deinit();
    virtual rsrc * rsrc_func64(IDVList &stak);
    virtual size_t rsrc_func65(rsrc *pres);
    virtual void SetTime(int32_t timeStamp, int32_t frameTime) {};

    NC_STACK_bitmap() {};
    virtual ~NC_STACK_bitmap() {};
    
    virtual const std::string ClassName() const {
        return __ClassName;
    };


    enum BMD_ATT
    {
        BMD_ATT_OUTLINE = 0x80002001,
        BMD_ATT_WIDTH = 0x80002002,
        BMD_ATT_HEIGHT = 0x80002003,
        BMD_ATT_HAS_COLORMAP = 0x80002006,
        BMD_ATT_PCOLORMAP = 0x80002007,
        BMD_ATT_CONVCOLOR = 0x80002008,
    };

    //Set
    virtual void setBMD_palette(UA_PALETTE *);
    
    //Get
    virtual ResBitmap  * GetBitmap(int frameid = -1);
    virtual std::vector<tUtV> &GetOutline(int frameid = -1);
    virtual int getBMD_width();
    virtual int getBMD_height();
    virtual int getBMD_hasPalette();
    virtual UA_PALETTE *getBMD_palette();
    
    virtual SDL_Surface * GetSwTex();
    
    virtual bool IsDynamic() const { return false; };
    virtual uint32_t GetFramesCount() const { return 1; };
    virtual uint32_t GetCurrentFrameID() const { return 0; };

    
    virtual void PrepareTexture( bool force = false );

public:
    //Data
    static constexpr const char * __ClassName = "bitmap.class";

public:
    ResBitmap *bitm_intern = NULL;
    std::vector<tUtV> outline_coords;
};


// HD texture packs (implementation in bitmap.cpp).
//
// The engine re-palettises environment art per level, so a dumped/upscaled
// texture is only valid for the level it came from. Both trees are therefore
// namespaced by an "env key" naming the current level:
//   hd-dump/<envkey>/<name>_<w>x<h>.png     (UA_TEX_DUMP=1 writes here)
//   hd-textures/<envkey>/<name>_<w>x<h>.png (override, checked first)
//   hd-textures/<name>_<w>x<h>.png          (override, shared fallback)
// The key is "shell" outside a level.
//
// Animated bitmaps (bmpanim.class) need NO separate mechanism. A frame there is
// a (bitmap, texcoord-set) pair: an animation is either UV movement across one
// shared sheet or a list of ordinary bitmaps, and in both shapes every frame
// bitmap is a plain NC_STACK_bitmap whose PrepareTexture runs the hook above.
// Texture coordinates are normalized, so an override at a different resolution
// keeps every frame rect where it was. ReportAnim() logs the composition under
// UA_TEX_DUMP so a pack builder can see which sheets an animation depends on.
namespace HDTex
{
    void SetEnvKey(const std::string &key);
    const std::string &GetEnvKey();

    bool DumpEnabled();
    void ReportAnim(const std::string &anmName,
                    const std::vector<std::string> &frameBitmaps,
                    size_t frameCount);

    // ---- UI tile sheets (hd-dump/ui, hd-textures/ui) -----------------------
    // The 2D layer never reaches PrepareTexture (it software-blits swTex), so
    // the .ILB/.FON sheets carrying every icon, gauge, panel frame and HUD
    // digit are invisible to the hook above. GFXEngine::AttachUiSheet() runs
    // this parallel, level-independent pair for them; these are the shared
    // filename/mkdir primitives.
    //   UA_UI_DUMP=1        -> hd-dump/ui/<name>_<w>x<h>.png
    //   hd-textures/ui/     <- override, same filename (ORIGINAL size in name)
    //   hd-textures/ui/pack.txt  "scale = N" declares the pack's magnification
    std::string FileName(const std::string &rsrcName, int w, int h);
    // <name>_<w>x<h>_<fnv1a>.png - content-addressed, see bitmap.cpp for why.
    std::string UiFileName(const std::string &rsrcName, SDL_Surface *s);
    bool FileExists(const std::string &path);
    void MakeDir(const std::string &path);

    bool UiDumpEnabled();
    // Magnification declared by an installed hd-textures/ui/ pack, or 0 when no
    // pack is present. Read once.
    int  UiPackScale();
    // UI graphics: the UI graphics level (gfxLevel("UI", 3)), read ONCE per run.
    // 0 = 1998 menus (no pack), 1 = hd-textures/ui sheet pack, 2 = + hd-textures/
    // ui-art background twins, 3 = + sharp 2D composite on a magnified drawable.
    int  UiLevel();
    // Directory of the installed sheet pack ("hd-textures/ui4/" at UI 3 when a
    // 4x pack is installed, else "hd-textures/ui/"), valid after UiPackScale().
    const std::string &UiPackDir();
    // UI graphics: menu/briefing STYLE, independent of the quality level (a look
    // choice, not a cost tier): 0 = classic (the 1998 art and font, default),
    // 1 = modern pilot (hd-textures/ui-modern/ pictures + an OFL UI font).
    // gfxLevel("UISTYLE", 0), read once; only honoured at UI >= 2.
    int  UiStyle();
    // Background-picture twin directories to search, best first, for this run's
    // UI level/style (empty below UI 2). Each holds art_<w>x<h>_<fnv1a>.png.
    const std::vector<std::string> &UiArtDirs();
    // Root of the UI packs (ui/, ui4/, ui-art/, ui-art4/, ui-modern/, fonts/): "hd-textures/" in the game dir,
    // or UA_UI_DIR + "/" when that names a directory (the release carries its own packs,
    // via UA_UI_DIR, so the game dir shared with other launchers is never touched). UA_UI_DIR is
    // only read at UI >= 1; at UI 0 this is "hd-textures/" with no env read (the 1998 path).
    const std::string &UiRoot();

    // ---- Wall signatures ("Wall Signatures" checkbox) ----------------------
    // The HD city-wall sheets shrttwnd/fhrttwnd carried the TerraTools artists'
    // signature, "(c) 95 bernd beyreuther", along their bottom edge; a 2026
    // retouch pass painted it out along with the joke graffiti on city1. This
    // puts just the signature back, and only while the player asks for it.
    //
    // hd-textures/signatures/manifest.txt maps every affected pack file to one
    // of ten strips (one per unique sheet variant) plus a paste position; the
    // strips are lifted from the pristine pre-retouch sheets, so compositing
    // one restores those pixels exactly. The graffiti is NOT restored — the
    // strips only span the signature band.
    //
    // Off is free: SetSignaturesEnabled(false) means the manifest is never
    // read and hdTexHooks does no extra work at all.
    void SetSignaturesEnabled(bool on);
    bool SignaturesEnabled();

    // Paste the signature strip onto a freshly loaded HD sheet, in place, if
    // the setting is on and this file has a manifest entry. fullPath is the
    // path the sheet was loaded from ("hd-textures/<envkey>/<name>.png").
    // Returns true when something was pasted. Must run before the GL upload.
    bool RestoreSignature(SDL_Surface *dst, const std::string &fullPath);
}

#endif // BITMAP_H_INCLUDED
