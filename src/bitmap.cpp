#include "includes.h"
#include "nucleas.h"
#include "rsrc.h"
#include "bitmap.h"
#include "system/gfx_glow.h"
#include "utils.h"
#include "gfxsettings.h"

#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include "fmtlib/printf.h"
#ifdef _WIN32
#include "winport.h"
#endif

// ---- HD texture foundation -------------------------------------------------
// Dump mode: set UA_TEX_DUMP=1 in the environment and every texture the
// engine uploads is written once as PNG to
// <cwd>/hd-dump/<envkey>/<name>_<w>x<h>.png, where <name> is the sanitized
// resource name (source container + name) — stable and deterministic across
// runs — and <envkey> names the level being loaded (HDTex::SetEnvKey).
// Override: <cwd>/hd-textures/<envkey>/<name>_<w>x<h>.png is used for the GL
// upload if it exists, else <cwd>/hd-textures/<name>_<w>x<h>.png (any
// resolution; UVs are normalized so game logic is unaffected).
//
// The per-level split is not cosmetic: dumped pixels are palette-resolved and
// the engine re-palettises environment art per level, so a pack built from
// level A paints level A's lighting onto level B (measured: 17 of 28 shared
// textures differ between two levels, newsky1 by 50/255). The flat directory
// stays as the shared fallback for packs that are level-independent.

static std::string hdSanitize(const std::string &name)
{
    std::string out;
    out.reserve(name.size());

    for (char c : name)
    {
        if ( (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' )
            out += c;
        else if (c >= 'A' && c <= 'Z')
            out += (char)(c - 'A' + 'a');
        else
            out += '_';
    }

    return out;
}

static std::string hdTexFileName(const std::string &rsrcName, int w, int h)
{
    std::string out = hdSanitize(rsrcName);

    if (out.empty())
        out = "unnamed";

    out += fmt::sprintf("_%dx%d.png", w, h);
    return out;
}

static bool hdFileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void hdMakeDir(const std::string &path)
{
    if ( !hdFileExists(path) )
        mkdir(path.c_str(), 0755);
}

namespace HDTex
{

static std::string hdEnvKey = "shell";

void SetEnvKey(const std::string &key)
{
    std::string sane = hdSanitize(key);

    if ( sane.empty() )
        sane = "shell";

    if ( sane == hdEnvKey )
        return;

    hdEnvKey = sane;
    fmt::printf("hd-textures: env key = %s\n", hdEnvKey);
}

const std::string &GetEnvKey()
{
    return hdEnvKey;
}

bool DumpEnabled()
{
    static int on = -1;

    if ( on < 0 )
    {
        const char *v = getenv("UA_TEX_DUMP");
        on = ( v && v[0] && strcmp(v, "0") != 0 ) ? 1 : 0;
    }

    return on != 0;
}

// Pack-builder diagnostic, not a code path the game depends on: print each
// animated bitmap's frame count and the distinct sheets its frames index into,
// once per animation. Those sheet names are ordinary bitmap resources, so they
// appear in hd-dump/<envkey>/ and are overridden from hd-textures/<envkey>/
// like any other texture — animations ride the existing hook.
void ReportAnim(const std::string &anmName,
                const std::vector<std::string> &frameBitmaps,
                size_t frameCount)
{
    if ( !DumpEnabled() )
        return;

    static std::set<std::string> reported;

    if ( !reported.insert(anmName).second )
        return;

    std::string sheets;
    std::set<std::string> distinct;

    for (const std::string &n : frameBitmaps)
    {
        if ( !distinct.insert(n).second )
            continue;

        if ( !sheets.empty() )
            sheets += ", ";
        sheets += n;
    }

    fmt::printf("hd-anim: %s — %zu frames over %zu sheet(s): %s\n",
                anmName.empty() ? "<unnamed>" : anmName,
                frameCount, distinct.size(), sheets);
}

// ---- Wall signatures (see bitmap.h) ----------------------------------------

namespace {

struct SigEntry
{
    std::string strip;   // file name inside hd-textures/signatures/
    int x = 0;
    int y = 0;
};

bool sigEnabled = false;
bool sigManifestRead = false;

// Keyed by the pack path relative to hd-textures/, e.g.
// "levels-l0101/shrttwnd.ilbm__256x256.png".
std::map<std::string, SigEntry> *sigMap = NULL;

// One decoded surface per distinct strip — ten of them for four hundred pack
// files, so caching turns four hundred PNG decodes into ten.
std::map<std::string, SDL_Surface *> *sigStrips = NULL;

bool sigDumpEnabled()
{
    static int on = -1;

    if ( on < 0 )
    {
        const char *v = getenv("UA_SIG_DUMP");
        on = ( v && v[0] && strcmp(v, "0") != 0 ) ? 1 : 0;
    }

    return on != 0;
}

void sigLoadManifest()
{
    if ( sigManifestRead )
        return;

    sigManifestRead = true;
    sigMap = new std::map<std::string, SigEntry>();
    sigStrips = new std::map<std::string, SDL_Surface *>();

    FILE *f = fopen("hd-textures/signatures/manifest.txt", "r");

    if ( !f )
    {
        fmt::printf("sig: no hd-textures/signatures/manifest.txt, nothing to restore\n");
        return;
    }

    char line[1024];

    while ( fgets(line, sizeof(line), f) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
            continue;

        char pack[512];
        char strip[256];
        int x = 0;
        int y = 0;

        // "<pack-path> <strip> <x> <y>" — no field can contain a space, the
        // generator writes sanitized pack names only.
        if ( sscanf(line, "%511s %255s %d %d", pack, strip, &x, &y) != 4 )
            continue;

        SigEntry e;
        e.strip = strip;
        e.x = x;
        e.y = y;
        (*sigMap)[pack] = e;
    }

    fclose(f);

    fmt::printf("sig: manifest loaded, %zu pack file(s) carry a signature\n", sigMap->size());
}

// Decoded strip, or NULL. Cached both ways: a strip that fails to load is
// remembered as NULL so a broken pack does not retry the decode per texture.
SDL_Surface *sigStrip(const std::string &name)
{
    std::map<std::string, SDL_Surface *>::iterator it = sigStrips->find(name);

    if ( it != sigStrips->end() )
        return it->second;

    SDL_Surface *s = IMG_Load(("hd-textures/signatures/" + name).c_str());

    if ( !s )
        fmt::printf("sig: strip %s failed to load (%s)\n", name, IMG_GetError());
    else
        // A straight copy, not a blend: the strip carries the pristine pixels
        // for that band and has to land byte-for-byte, alpha included.
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);

    (*sigStrips)[name] = s;
    return s;
}

}

bool RestoreSignature(SDL_Surface *dst, const std::string &fullPath)
{
    // Off is genuinely free: no manifest read, no map lookup, no allocation.
    if ( !sigEnabled || !dst )
        return false;

    sigLoadManifest();

    // The manifest is keyed relative to hd-textures/, which is also the only
    // place these sheets are ever loaded from.
    static const std::string prefix = "hd-textures/";

    if ( fullPath.compare(0, prefix.size(), prefix) != 0 )
        return false;

    const std::string packPath = fullPath.substr(prefix.size());
    const std::string &logPath = fullPath;

    std::map<std::string, SigEntry>::iterator it = sigMap->find(packPath);

    if ( it == sigMap->end() )
        return false;

    SDL_Surface *strip = sigStrip(it->second.strip);

    if ( !strip )
        return false;

    SDL_Rect to;
    to.x = it->second.x;
    to.y = it->second.y;
    to.w = strip->w;
    to.h = strip->h;

    // The manifest is built against the installed pack, so a mismatch here
    // means the pack was replaced underneath it. Say so rather than blit off
    // the edge of a smaller sheet.
    if ( to.x + strip->w > dst->w || to.y + strip->h > dst->h )
    {
        fmt::printf("sig: %s is %dx%d, strip wants %d,%d+%dx%d - skipped\n",
                    logPath, dst->w, dst->h, to.x, to.y, strip->w, strip->h);
        return false;
    }

    if ( SDL_BlitSurface(strip, NULL, dst, &to) != 0 )
    {
        fmt::printf("sig: blit failed on %s (%s)\n", logPath, SDL_GetError());
        return false;
    }

    fmt::printf("sig: restored on %s\n", logPath);

    // Pack-builder / verification aid, off unless asked for: UA_SIG_DUMP=1
    // writes the composited sheet to hd-dump/sig/ so the restored band can be
    // diffed against the pristine original without a screenshot. Same shape as
    // UA_TEX_DUMP and UA_UI_DUMP above.
    if ( sigDumpEnabled() )
    {
        hdMakeDir("hd-dump");
        hdMakeDir("hd-dump/sig");

        std::string out = "hd-dump/sig/" + hdSanitize(packPath);

        if ( out.size() < 4 || out.compare(out.size() - 4, 4, ".png") != 0 )
            out += ".png";

        IMG_SavePNG(dst, out.c_str());
        fmt::printf("sig: dumped %s\n", out);
    }

    return true;
}

void SetSignaturesEnabled(bool on)
{
    sigEnabled = on;
}

bool SignaturesEnabled()
{
    return sigEnabled;
}

// ---- UI tile sheet pack (see bitmap.h) -------------------------------------
std::string FileName(const std::string &rsrcName, int w, int h)
{
    return hdTexFileName(rsrcName, w, h);
}

// UI sheet names are CONTENT-ADDRESSED, unlike the level texture pack.
// Measured: of the 48 sheets the game loads, Metropolis Dawn replaces exactly
// one - h_finder.ilbm 316x205 - keeping the same resource name and the same
// dimensions. A name+size key would silently paint the base campaign's finder
// panel over the expansion's (and whichever ran the dump would win). Hashing
// the pixels makes the two distinct files, so an override can only ever land on
// the art it was built from; a sheet that changes upstream simply stops
// matching instead of matching wrongly.
std::string UiFileName(const std::string &rsrcName, SDL_Surface *s)
{
    uint32_t hash = 2166136261u;   // FNV-1a over the visible pixel bytes

    if ( SDL_LockSurface(s) == 0 )
    {
        const int rowbytes = s->w * s->format->BytesPerPixel;

        for (int y = 0; y < s->h; y++)
        {
            const uint8_t *p = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
            for (int i = 0; i < rowbytes; i++)
            {
                hash ^= p[i];
                hash *= 16777619u;
            }
        }

        SDL_UnlockSurface(s);
    }

    std::string out = hdSanitize(rsrcName);
    if (out.empty())
        out = "unnamed";

    out += fmt::sprintf("_%dx%d_%08x.png", s->w, s->h, hash);
    return out;
}

bool FileExists(const std::string &path)
{
    return hdFileExists(path);
}

void MakeDir(const std::string &path)
{
    hdMakeDir(path);
}

bool UiDumpEnabled()
{
    static int on = -1;

    if ( on < 0 )
    {
        const char *v = getenv("UA_UI_DUMP");
        on = ( v && v[0] && strcmp(v, "0") != 0 ) ? 1 : 0;
    }

    return on != 0;
}

int UiLevel()
{
    static int lv = -1;
    if ( lv < 0 )
    {
        lv = gfxLevel("UI", 3);
        if ( lv < 0 ) lv = 0;
        if ( lv > 3 ) lv = 3;
    }
    return lv;
}

static std::string s_uiPackDir = "hd-textures/ui/";

const std::string &UiRoot()
{
    static std::string root;
    if ( root.empty() )
    {
        root = "hd-textures/";
        if ( UiLevel() > 0 )
        {
            const char *v = getenv("UA_UI_DIR");
            struct stat st;
            if ( v && *v && stat(v, &st) == 0 && S_ISDIR(st.st_mode) )
            {
                root = v;
                if ( root.back() != '/' )
                    root += "/";
                fmt::printf("ui-hd: UI packs from %s\n", root);
            }
            else if ( v && *v )
                fmt::printf("ui-hd: UA_UI_DIR %s is not a directory - using hd-textures/\n", v);
        }
    }
    return root;
}

const std::string &UiPackDir()
{
    return s_uiPackDir;
}

int UiStyle()
{
    static int st = -1;
    if ( st < 0 )
    {
        st = gfxLevel("UISTYLE", 0);
        if ( st < 0 || st > 1 ) st = 0;
    }
    return st;
}

const std::vector<std::string> &UiArtDirs()
{
    static std::vector<std::string> dirs;
    static bool done = false;
    if ( !done )
    {
        done = true;
        const int lv = UiLevel();
        if ( lv >= 2 )
        {
            const std::string &r = UiRoot();
            if ( UiStyle() == 1 )
            {
                if ( lv >= 3 ) dirs.push_back(r + "ui-modern/art4/");
                dirs.push_back(r + "ui-modern/art/");
            }
            if ( lv >= 3 ) dirs.push_back(r + "ui-art4/");
            dirs.push_back(r + "ui-art/");
        }
    }
    return dirs;
}

int UiPackScale()
{
    static int scale = -1;

    if ( scale >= 0 )
        return scale;

    scale = 0;

    // UI 3 (Ultra): a 4x pack (hd-textures/ui4/, vector icons make it free) when
    // installed, else the same 3x pack as UI 1-2.
    // Packs under UiRoot() (game-dir hd-textures/, or the release's UA_UI_DIR at UI >= 1).
    if ( UiLevel() > 0 )
        s_uiPackDir = UiRoot() + "ui/";
    if ( UiLevel() >= 3 && hdFileExists(UiRoot() + "ui4/pack.txt") )
        s_uiPackDir = UiRoot() + "ui4/";

    if ( !hdFileExists(s_uiPackDir.substr(0, s_uiPackDir.size() - 1)) )
        return scale;

    // Graphics settings: UI 0 (preset Original) = the 1998 menu sheets. Read once
    // at startup (the UI scale must never change during a run), so a change in
    // the Graphics menu applies at the next start.
    if ( UiLevel() == 0 )
    {
        fmt::printf("ui-hd: UI pack off (graphics settings UI 0)\n");
        return scale;
    }

    // pack.txt is written by the pack installer and is authoritative: the
    // engine's UI scale and the sheets' magnification MUST agree exactly,
    // because DrawFill() REPEATS a source rect over the destination rect
    // rather than scaling it. A mismatched pair would tile a tile.
    FILE *f = fopen((s_uiPackDir + "pack.txt").c_str(), "r");
    if ( f )
    {
        char line[256];
        while ( fgets(line, sizeof(line), f) )
        {
            int v = 0;
            const char *p = strstr(line, "scale");
            if ( !p )
                continue;
            p = strchr(p, '=');
            if ( p && sscanf(p + 1, "%d", &v) == 1 && v >= 1 && v <= 8 )
            {
                scale = v;
                break;
            }
        }
        fclose(f);
    }

    if ( scale == 0 )
    {
        fmt::printf("ui-hd: hd-textures/ui exists but declares no scale "
                    "(missing/!unreadable pack.txt) - UI sheets left classic\n");
        return scale;
    }

    fmt::printf("ui-hd: pack scale = %dx (%s)\n", scale, s_uiPackDir);
    return scale;
}

}

static void hdTexHooks(ResBitmap *bitm, const std::string &rsrcName)
{
    if ( !bitm || !bitm->swTex )
        return;

    const std::string base = hdTexFileName(rsrcName, bitm->swTex->w, bitm->swTex->h);
    const std::string &env = HDTex::GetEnvKey();

    if ( HDTex::DumpEnabled() )
    {
        const std::string dir = "hd-dump/" + env;

        hdMakeDir("hd-dump");
        hdMakeDir(dir);

        std::string path = dir + "/" + base;
        if ( !hdFileExists(path) )
            IMG_SavePNG(bitm->swTex, path.c_str());
    }

    // Graphics settings: HDTEX 0 (preset Original) = the 1998 textures; no pack
    // file is opened. No preset in force = on, as before the settings existed.
    if ( gfxLevel("HDTEX", 1) == 0 )
    {
        if ( bitm->hdTex )
        {
            SDL_FreeSurface(bitm->hdTex);
            bitm->hdTex = NULL;
        }
        return;
    }

    if ( !bitm->hdTex )
    {
        // Level-specific pack wins; flat hd-textures/ is the shared fallback.
        std::string path = "hd-textures/" + env + "/" + base;

        if ( !hdFileExists(path) )
            path = "hd-textures/" + base;

        if ( hdFileExists(path) )
        {
            bitm->hdTex = IMG_Load(path.c_str());
            if ( bitm->hdTex )
            {
                fmt::printf("hd-textures: %s (%dx%d -> %dx%d)\n", path,
                            bitm->swTex->w, bitm->swTex->h,
                            bitm->hdTex->w, bitm->hdTex->h);

                // Before the GL upload, and only for the two wall sheets the
                // manifest knows about: put the artists' signature back.
                HDTex::RestoreSignature(bitm->hdTex, path);
            }
        }
    }
}
// ---------------------------------------------------------------------------



size_t NC_STACK_bitmap::Init(IDVList &stak)
{
    if ( !NC_STACK_rsrc::Init(stak) )
        return 0;

    std::vector<tUtV> *v9 = stak.Get<std::vector<tUtV> *>(BMD_ATT_OUTLINE, NULL);

    if ( v9 )
        outline_coords = *v9;

    bitm_intern = (ResBitmap *)getRsrc_pData();

    return 1;
}

size_t NC_STACK_bitmap::Deinit()
{
    return NC_STACK_rsrc::Deinit();
}

// Create bitmap resource node and fill rsrc field data
rsrc * NC_STACK_bitmap::rsrc_func64(IDVList &stak)
{
    rsrc *res = NC_STACK_rsrc::rsrc_func64(stak);// rsrc_func64
    if ( res )
    {
        int width = stak.Get<int32_t>(BMD_ATT_WIDTH, 0);
        int height = stak.Get<int32_t>(BMD_ATT_HEIGHT, 0);
        int colormap = stak.Get<int32_t>(BMD_ATT_HAS_COLORMAP, 0);
        //int create_texture = stak.Get<int32_t>(BMD_ATT_TEXTURE, 0);


        if ( width && height )
        {
            ResBitmap *intern = new ResBitmap();

            if ( intern )
            {
                if (colormap)
                    intern->palette = new UA_PALETTE;

                intern->width = width;
                intern->height = height;

                if ( !colormap )
                {
                    intern->width = width;
                    intern->height = height;

                    // allocate buffer, create palette, surface and texture
                    intern->swTex = GFX::Engine.CreateSurfaceScreenFormat(width, height);
                    if (!intern->swTex)
                    {
                        delete intern;
                        return res;
                    }
                }
                else
                {
                    intern->swTex = SDL_CreateRGBSurface(0, width, height, 8, 0, 0, 0, 0);
                    if ( !intern->swTex )
                    {
                        delete intern;
                        return res;
                    }
                }
                res->data = intern;
            }
        }
    }
    return res;
}

size_t NC_STACK_bitmap::rsrc_func65(rsrc *res)
{
    ResBitmap *intern = (ResBitmap *)(res)->data;

    if ( intern )
    {
        if ( intern->hwTex )
            GFX::Engine.FreeTexture(intern);

        if ( intern->swTex )
            SDL_FreeSurface(intern->swTex);

        if ( intern->hdTex )
            SDL_FreeSurface(intern->hdTex);

        if ( intern->uiArt )
            SDL_FreeSurface(intern->uiArt);

        if ( intern->uiMaskK ) // UI graphics: campaign-map region mask at k x (gfx.cpp uiMaskFor)
            SDL_FreeSurface(intern->uiMaskK);

        if ( intern->palette )
            delete intern->palette;

        delete intern;
        res->data = NULL;
    }

    return NC_STACK_rsrc::rsrc_func65(res);
}

void NC_STACK_bitmap::setBMD_palette(UA_PALETTE *newPal)
{
    if ( bitm_intern->palette )
        *bitm_intern->palette = *newPal;
}



ResBitmap * NC_STACK_bitmap::GetBitmap(int frameid)
{
    return bitm_intern;
}

std::vector<tUtV> &NC_STACK_bitmap::GetOutline(int frameid /* = -1 */)
{
    return outline_coords;
}

int NC_STACK_bitmap::getBMD_width()
{
    if (bitm_intern)
        return bitm_intern->width;

    return 0;
}

int NC_STACK_bitmap::getBMD_height()
{
    if (bitm_intern)
        return bitm_intern->height;

    return 0;
}

int NC_STACK_bitmap::getBMD_hasPalette()
{
    if (bitm_intern)
        return bitm_intern->palette != NULL;

    return 0;
}

UA_PALETTE *NC_STACK_bitmap::getBMD_palette()
{
    if (bitm_intern)
        return bitm_intern->palette;

    return NULL;
}

SDL_Surface * NC_STACK_bitmap::GetSwTex()
{
    if (bitm_intern)
        return bitm_intern->swTex;

    return NULL;
}

void NC_STACK_bitmap::PrepareTexture( bool force )
{
    if (!bitm_intern)
        return;

    if (bitm_intern->hwTex && !force)
        return;

    if (bitm_intern->hwTex)
        GFX::Engine.FreeTexture(bitm_intern);

    hdTexHooks(bitm_intern, getRsrc_name());
    GroundTagBitmap(bitm_intern, getRsrc_name());
    GFX::GlowPass::TagBitmap(bitm_intern, getRsrc_name()); // reforge glow: exclusion tag, no-op at level 0

    GFX::Engine.AllocTexture(bitm_intern);
}

