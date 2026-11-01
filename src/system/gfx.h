#ifndef ENGINE_GFX_H_INCLUDED
#define ENGINE_GFX_H_INCLUDED

#include <array>
#include <deque>
#include <math.h>
#include <inttypes.h>

#include "system.h"
#include "../ini.h"
#include "../vectors.h"
#include "../matrix.h"
#include "../IFFile.h"
#include "../nucleas.h"
#include "common/common.h"
#include "common/cachestor.h"
#include "common/line.h"


#define GFX_MAX_VERTEX 12

class NC_STACK_bitmap;

typedef std::array<SDL_Color, 256> UA_PALETTE;

struct ResBitmap
{
    int16_t width = 0;
    int16_t height = 0;
    UA_PALETTE *palette = NULL;
    SDL_Surface *swTex = NULL;
    // Optional high-resolution replacement (hd-textures/ override). Only the
    // GL texture upload uses it; game logic and software blits keep swTex and
    // the original width/height.
    SDL_Surface *hdTex = NULL;
    GLuint hwTex = 0;
    // True when AllocTexture generated a mipmap chain for hwTex. The render
    // state machine needs this to pick a mipmapped MIN filter: sampling a
    // mip filter from a texture without a chain is "incomplete" in GL and
    // reads as black.
    bool hwTexMips = false;
    // True when hwTex is a GL sRGB texture (V2 albedo): sampling returns
    // linear values, filtering and mip generation happen in linear light.
    bool hwTexSrgb = false;
    // Perf accounting (perf.h): bytes charged for hwTex incl. mips, and the
    // source (reforge disk map vs original game texture).
    uint32_t hwBytes = 0;
    bool hwHd = false;
    // hwTex is a two-channel (BC5/RGTC2) normal map: the V2 shader rebuilds z.
    bool hwTexRG = false;
    // Reforge ground (UA_R2_GROUND, ground.cpp): 1..5 when this is a set's
    // ground atlas boden<k>.ilbm, with the set it was loaded for. Tag only -
    // nothing reads it unless UA_RENDER_V2=1 and UA_R2_GROUND=1.
    int8_t groundAtlas = 0;
    int8_t groundSet = 0;
    // UI graphics (UI level >= 2): hi-density twin of a 2D background picture
    // (shell, briefing, debrief, loading art) from hd-textures/ui-art/, exactly
    // uiArtScale times swTex, converted to the screen format. Looked up once, on
    // first draw; NULL = none (classic resample). Nothing reads it below UI 2.
    SDL_Surface *uiArt = NULL;
    int8_t uiArtScale = 0;
    bool uiArtTried = false;
    // UI graphics: a region MASK (8-bit, campaign map) replicated uiMaskScale times
    // (nearest), so BlitScaleMasked can cut a k x picture twin by the same ids.
    SDL_Surface *uiMaskK = NULL;
    int8_t uiMaskScale = 0;
    // Reforge ATMOSPHERE (atmos_gl.cpp): a set's building facade atlas (city1/hype/macht and their f-variants):
    // lit windows on night maps at ATMOS 2+. Tag only - nothing reads it at ATMOS 0/1.
    bool facadeAtlas = false;
};

// Reforge ground: the set whose resources are loading now (tags boden atlases).
void GroundSetCurrentSet(int setId);
void GroundTagBitmap(ResBitmap *bitm, const std::string &rsrcName);
// Reforge ATMOSPHERE: facade atlas file names (night windows); n = lower case, no path (atmos_gl.cpp).
bool AtmosFacadeAtlasName(const std::string &n);
// obj3d.cpp: every disk map in the shared map cache with the file it was uploaded from (<stem>.dds when the
// compressed sidecar was used, else the map's own path; atmos night windows enumerate the HD trims at level load).
void Obj3DForEachCachedMap(void (*fn)(ResBitmap *b, const std::string &path, void *ctx), void *ctx);


typedef std::vector<uint8_t> CmdStream;
typedef std::vector< CmdStream* > CmdIncludes;

struct TileMap
{
    NC_STACK_bitmap *img = NULL;
    std::array<Common::PointRect, 256> map;
    int h = 0;

    // Hi-density twin of img->GetSwTex(), exactly hiScale times bigger in both
    // axes, colour-keyed the same way. Built by GFXEngine::AttachUiSheet() from
    // hd-textures/ui/ when a pack is installed, else by exact pixel
    // replication. `map` stays in ORIGINAL sheet pixels - the blit multiplies.
    SDL_Surface *hiTex = NULL;
    int hiScale = 1;

    TileMap();
    ~TileMap();

    void Draw(SDL_Surface *surface, const Common::Point &pos, uint8_t c);
    void Draw(SDL_Surface *surface, const Common::PointRect &posRect, uint8_t c);
    void Draw(SDL_Surface *surface, const Common::Rect &posRect, uint8_t c);
    
    void Fill(SDL_Surface *surface, const Common::Rect &rect, uint8_t c);
    void Fill(SDL_Surface *surface, const Common::PointRect &rect, uint8_t c);
    
    //Special fill use 1pix column
    void FillColumn(SDL_Surface *surface, const Common::Rect &rect, uint8_t c);
    void FillColumn(SDL_Surface *surface, const Common::PointRect &rect, uint8_t c);
    
    int GetWidth(uint8_t c) const;
    int GetWidth(const std::string &str) const;
    Common::Point GetSize(uint8_t c) const;
};

struct w3d_a209
{
    char *cmdbuf = NULL;
    char **includ = NULL;
};

struct tUtV
{
    float tu = -1.0;
    float tv = -1.0;
    
    tUtV() = default;
    tUtV(float u, float v): tu(u), tv(v) {};
};


struct windd_arg256
{
    int sort_id;
    int width;
    int height;
    char name[32];
};



namespace GFX
{

#ifndef GFXSHORTINDEX    
    
typedef uint32_t IndexType;
#define GLINDEXTYPE GL_UNSIGNED_INT

#else

typedef uint16_t IndexType;
#define GLINDEXTYPE GL_UNSIGNED_SHORT

#endif

#ifndef __EMSCRIPTEN__
#define FBOTEXTYPE GL_RGBA32F
#else
#define FBOTEXTYPE GL_RGBA
#endif

enum RFLAGS
{
    RFLAGS_TEXTURED  =      (1 << 0),
    RFLAGS_DYNAMIC_TEXTURE = (1 << 1),
    RFLAGS_SHADED    =      (1 << 2),
    RFLAGS_FOG       =      (1 << 3),
    RFLAGS_ZEROTRACY =      (1 << 4),
    RFLAGS_LUMTRACY  =      (1 << 5),
    RFLAGS_SKY       =      (1 << 6),
    RFLAGS_FALLOFF   =      (1 << 7),
    RFLAGS_IGNORE_FALLOFF = (1 << 8),
    RFLAGS_COMPUTED_COLOR = (1 << 9),
    RFLAGS_DISABLE_ZWRITE = (1 << 10),
    RFLAGS_FX_ALPHA  =      (1 << 11), // reforge fx (UA_FX pack): alpha-blended sprite, only set by reforge_fx
    RFLAGS_SHADOW_HOST =    (1 << 12), // reforge shadows: host station / its guns = lighter caster; only set when shadows are on
};    

struct __attribute__((packed)) TGLColor
{
    float r = 1.0;
    float g = 1.0;
    float b = 1.0;
    float a = 1.0;
    
    TGLColor() = default;
    TGLColor(float _r, float _g, float _b, float _a = 1.0)
        : r(_r), g(_g), b(_b), a(_a) {};
};

struct TBoundBox
{
    vec3d Min;
    vec3d Max;
    
    bool Inited = false;
    
    void Add(const vec3d &pt) 
    {
        if (Inited)
        {
            if (pt.x < Min.x) Min.x = pt.x;
            if (pt.y < Min.y) Min.y = pt.y;
            if (pt.z < Min.z) Min.z = pt.z;
            if (pt.x > Max.x) Max.x = pt.x;
            if (pt.y > Max.y) Max.y = pt.y;
            if (pt.z > Max.z) Max.z = pt.z;
        }
        else
        {
            Inited = true;
            Min = pt;
            Max = pt;
        }
    }
    
    void Add(const TBoundBox &box) 
    {
        Add(box.Min);
        Add(box.Max);
    }
};

// Reforge (UA_RENDER_V2) material maps for modern assets. Owned by whoever
// loaded them (NC_STACK_Obj3D::Mtl); meshes/render nodes only borrow the
// pointer. NULL on every original .bas mesh, so the legacy path never sees it.
// Convention: _albedo / _normal / _orm maps next to the OBJ.
struct TPbrMaterial
{
    ResBitmap *Normal = NULL;   // tangent-space, OpenGL +Y (green up), linear
    ResBitmap *Orm = NULL;      // R=AO G=roughness B=metallic, linear
    ResBitmap *Emissive = NULL; // sRGB
    float Roughness = 1.0;      // multiplies ORM.g (or used alone)
    float Metallic = 0.0;       // multiplies ORM.b (or used alone)
    float Emission[3] = {0.0, 0.0, 0.0}; // Ke, multiplies map_Ke
    float NormalScale = 1.0;    // `norm -bm s` (Blender Normal Map strength)
};

struct TRenderParams
{
    NC_STACK_bitmap *TexSource = NULL;
    ResBitmap *Tex = NULL;
    const TPbrMaterial *Pbr = NULL;
    
    uint32_t Flags = 0;
    TGLColor Color;
    
    TRenderParams() = default;
    TRenderParams(const TRenderParams &) = default;
    TRenderParams(TRenderParams &&) = default;
    
    TRenderParams(uint32_t flags)
        : Flags(flags){};
    
    TRenderParams(NC_STACK_bitmap *tex, uint32_t flags)
        : TexSource(tex), Flags(flags){};
        
    TRenderParams(ResBitmap *tex, uint32_t flags)
        : Tex(tex), Flags(flags){};
    
    TRenderParams& operator=(const TRenderParams &) = default;
    TRenderParams& operator=(TRenderParams &&) = default;
    
    bool operator==(const TRenderParams &b);
};

struct TCoordsCache
{
    ResBitmap *Tex = NULL;
    std::vector<tUtV> Coords;
    
    // OpenGL
    int32_t BufferPos = 0;
};

struct TVertex
{
    vec3f Pos;
    //vec3f Normal;
    tUtV  TexCoord;
    TGLColor Color;
    TGLColor ComputedColor;
    uint32_t TexCoordId = 0;
    // Reforge (UA_RENDER_V2) shading frame. Only filled by loaders of modern
    // assets (obj3d); original .bas meshes leave them zero and the legacy
    // shader never binds them.
    vec3f Normal;
    float Tangent[4] = {0.0, 0.0, 0.0, 0.0}; // xyz + handedness (w = +-1)
    
    TVertex() = default;
    TVertex(const vec3f &p): Pos(p) {};
    TVertex(const vec3f &p, const tUtV &uv): Pos(p), TexCoord(uv) {};
    TVertex(const vec3f &p, const tUtV &uv, const TGLColor &clr): Pos(p), TexCoord(uv), Color(clr) {};
};

class TMesh
{
public:
    TRenderParams Mat;
    std::vector<TVertex> Vertexes;
    std::vector<IndexType> Indixes;
    
    std::vector<TCoordsCache> CoordsCache;
    
    TBoundBox BoundBox;
    
    TMesh() = default;
    ~TMesh();
    TMesh(const TMesh&);
    TMesh(TMesh&&) = default;
    
    TMesh &operator=(const TMesh&);
    TMesh &operator=(TMesh&&) = default;
    
    void RecalcBoundBox()
    {
        BoundBox = TBoundBox();
        for (const TVertex &v : Vertexes)
            BoundBox.Add(v.Pos);
    }
    
    // OpenGL
    // Do not copy it!
    GLuint glDataBuf = 0;
    GLuint glIndexBuf = 0;
    //GLuint glVao = 0;
};

struct TRenderNode
{
    enum
    {
        TYPE_MESH,
        TYPE_PARTICLE
    };
    
    uint8_t Type = TYPE_MESH;
    
    TMesh *Mesh = NULL;
    TMesh LocalMesh;
    
    int32_t coordsID = -1;
    
    ResBitmap *Tex = NULL;
    const TPbrMaterial *Pbr = NULL;
    
    uint32_t Flags = 0;
    TGLColor Color;
    
    mat4x4 TForm;
    int32_t TimeStamp = 0; 
    int32_t FrameTime = 0;
    
    float FogStart = 0.0;
    float FogLength = 0.0;
    
    float Distance = 0.0;
    
    float ParticleSize = 0.0;
    
    TRenderNode() = default;
    TRenderNode(TRenderNode &&) = default;
    TRenderNode(const TRenderNode &) = default;
    
    TRenderNode& operator=(const TRenderNode &) = default;
    TRenderNode& operator=(TRenderNode &&) = default;
    
    TRenderNode(uint8_t tp): Type(tp) {};
    
    static bool CompareSolid(TRenderNode *a, TRenderNode *b);
    static bool CompareTransparent(TRenderNode *a, TRenderNode *b);
    static bool CompareDistance(TRenderNode *a, TRenderNode *b);
};
    
enum
{
    DEFAULT_WIDTH = 640,
    DEFAULT_HEIGHT = 480,
};

struct TShaderProg
{
    uint32_t ID = 0;
    int32_t PosLoc = -1;
    int32_t ColorLoc = -1;
    int32_t UVLoc = -1;
    int32_t NormalLoc = -1;  // V2 program only
    int32_t TangentLoc = -1; // V2 program only

    TShaderProg() = default;
    TShaderProg(uint32_t id);
};

struct TColorEffectsProg : TShaderProg
{
    int32_t NormLoc = -1;
    int32_t InvLoc = -1;
    int32_t RandLoc = -1;
    int32_t ScrSizeLoc = -1;
    int32_t MillisecsLoc = -1;
    
    TColorEffectsProg() = default;
    TColorEffectsProg(uint32_t id);
};


struct GfxStates
{
    uint32_t Tex = 0;
    // Whether Tex carries a mipmap chain (ResBitmap::hwTexMips). Must be set
    // at EVERY site that assigns Tex — raw GL textures (screen/text/FBO
    // quads) have no chain and must stay on plain GL_LINEAR.
    bool TexMip = false;
    bool Shaded = true;
    bool Stipple = false;
    uint32_t SrcBlend = GL_ONE;
    uint32_t DstBlend = GL_ZERO;
    int8_t TexBlend = 2;
    bool AlphaBlend = false;
    bool Zwrite = true;
    bool LinearFilter = true;
    bool AlphaTest = false;
    bool Fog = false;
    float FogStart = 3496.0;
    float FogLength = 600.0;
    bool AFog = false;
    float AFogStart = 3496.0;
    float AFogLength = 600.0;
    
    bool DepthTest = true;
    
    uint32_t DataBuf = 0;
    uint32_t IndexBuf = 0;
    TShaderProg Prog;
};

struct rstr_arg204
{
    ResBitmap *pbitm = NULL;
    Common::FRect float4;
    Common::FRect float14;
};

struct rstr_218_arg
{
    ResBitmap *bitm_intern;
    ResBitmap *bitm_intern2;
    int flg;
    Common::FRect rect1;
    Common::FRect rect2;
};

struct ColorFx
{
    int Id = 0;
    float Pwr = 1.0;
    
    ColorFx() {};
    ColorFx(int id, float pwr) : Id(id), Pwr(pwr) {};
};

struct displ_arg263
{
    ResBitmap *bitm = NULL;
    int pointer_id = 0;
};

struct windd_dlgBox
{
    const char *title;
    const char *ok;
    const char *cancel;
    const char *startText;
    int time;
    void (*timer_func)(int, int, int);
    void *timer_context;
    int replace;
    int maxLen;
    char *result;
};


struct TGFXDeviceInfo
{
    std::string name;
    std::string guid;
    bool isCurrent = false;
    
    TGFXDeviceInfo(const std::string &__name, const std::string &__guid)
    : name(__name), guid(__guid) {}
};

struct ScreenText
{
    std::string string;
    int p1;
    int p2;
    int p3;
    int p4;
    int flag;
};

struct ScreenFont
{
    TTF_Font *ttfFont = NULL;
    int height = 0;
    uint8_t r = 0, g = 0, b = 0;

    // Hi-res twin of ttfFont, opened at height * (drawable / logical). Only the
    // GLYPH RASTER lives at that size: every layout decision (widths, clip box,
    // alignment, vertical centering) is still made with ttfFont in logical
    // pixels, so the authored layout boxes are bit-identical to the classic
    // path. Lazily (re)opened by EnsureHiFont() when the drawable changes.
    TTF_Font *ttfFontHi = NULL;
    std::string face;       // face name as parsed from the font descriptor
    int hiHeight = 0;       // pt size ttfFontHi was opened at (0 = not open)

    std::list<ScreenText *> entries;
};
    
struct GfxMode
{
    int w = 0;
    int h = 0;
    int bpp = 0;
    bool windowed = false;
    SDL_DisplayMode mode = {0};
    std::string name;
    
    GfxMode() {};
    GfxMode(GfxMode &&g);
    GfxMode(const GfxMode &g);
    GfxMode(const Common::Point &sz);

    GfxMode& operator=(const GfxMode &g);    
    operator bool() const;
    
    bool operator==(const GfxMode &g) const;
    bool operator==(const Common::Point &g) const;
    bool operator!=(const GfxMode &g) const;
    bool operator!=(const Common::Point &g) const;
        
    GfxMode WithWindowed(bool state) const
    {
        GfxMode t = *this;
        t.windowed = state;
        return t;
    }
    
    operator Common::Point() const { return Common::Point(w, h); };
    
    static bool SortCompare(const GfxMode &a, const GfxMode &b);
    
    static std::string GenName(int w, int h);
    
};

enum WDD_ATT
{
    WDD_ATT_CURSOR = 0x80002000,
    WDD_ATT_TEXFILT = 0x80002001,
    WDD_ATT_DIS_LOWRES = 0x80002002,
    WDD_ATT_16BIT_TEX = 0x80002003,
    WDD_ATT_DRAW_PRIM = 0x80002004
};

enum DISP_ATT
{
    ATT_FGPEN       = 0x80001000,
    ATT_BGPEN       = 0x80001001,
    ATT_SHADE_RMP   = 0x80001002,
    ATT_TRACY_RMP   = 0x80001003,
    ATT_FGAPEN      = 0x80001004,
    ATT_WIDTH       = 0x80001005,
    ATT_HEIGHT      = 0x80001006,
    ATT_PALETTE     = 0x80001007,
    ATT_DISPLAY_ID  = 0x80001008,
    ATT_DISPLAY_INF = 0x80001009,
    ATT_DISPLAY_WIN = 0x8000100A,
};

enum RASTER
{
    RASTER_SOLID   = (1 << 0),
    RASTER_SKY     = (1 << 1),
    RASTER_ZEROTR  = (1 << 2),
    RASTER_LUMATR  = (1 << 3),
    RASTER_PARTICL = (1 << 4),
    
    RASTER_ALL = (RASTER_SOLID | RASTER_SKY | RASTER_ZEROTR | RASTER_LUMATR | RASTER_PARTICL)
};
    
class GFXEngine
{
public:
    static const std::array<vec3d, 8> _clrEff;
    
    static int can_srcblend;
    static int can_destblend;
    static int can_stippling;
    static uint32_t FpsMaxTicks;

protected:
    static SDL_PixelFormat *_pixfmt;
    static GLint _glPixfmt, _glPixtype;
    static bool _staticInited;
    
    static void StaticInit();
    
public:
    void Init();
    void Deinit();
    
    void RecreateScreenSurface();
    void DrawScreenSurface();

    // Hi-res UI text layer (see gfx.cpp for the full rationale).
    void RecreateTextSurface();
    void DrawTextLayer();
    bool TextLayerActive() const { return TextSurface != NULL; }

    // Hi-density UI bitmap layer: ScreenSurface is UiScale() times the logical
    // 2D mode, every 2D primitive multiplies its DESTINATION by that factor at
    // the final SDL call, and tile sheets supply a matching hi-density source.
    // Layout, mouse mapping and game logic stay in logical pixels throughout.
    int  UiScale();
    void AttachUiSheet(TileMap *tileset, const std::string &sheetName);
    SDL_Surface *UiSheetFor(TileMap *tileset);

    // Aspect-correct 2D (off by default) - see gfx.cpp.
    bool UiLetterbox();
    void UiQuadExtent(float *ex, float *ey);

    uint32_t LoadShader(int32_t type, const std::string &fl);
    uint32_t CompileShader(int32_t type, const std::string &string);
    
    void SetFBOBlending(int mode);
    void DrawFBO();
    void UpdateFBOSizes();
    
    
    SDL_Surface *Screen();
    

    uint8_t *MakeScreenCopy(int *ow, int *oh);
    uint8_t *MakeDepthScreenCopy(int *ow, int *oh);

    
    GfxMode windd_func0__sub0(const std::string &file);
    GfxMode sub_41F68C();
    
    int  GetGfxModeIndex(const Common::Point &res);

    void SetResolution(const Common::Point &res, bool windowed);
    void SetTracyRmp(ResBitmap *rmp);
    void SetShadeRmp(ResBitmap *rmp);

    void SetCursor(int curID, int force);
    GfxMode GetGfxMode();
    const std::vector<GfxMode> &GetAvailableModes();

    void SetTileset(TileMap *tileset, int id);
    TileMap * GetTileset(int id);

    bool LoadPalette(const std::string &palette_ilbm);
    SDL_Cursor *LoadCursor(const std::string &name);
    
    
    static SDL_PixelFormat *GetPixelFormat() { return _pixfmt; };
    static uint32_t GetPixelFormatU32() { return _pixfmt->format; };
    static void GetGlPixTypeFmt(GLint *format, GLint *type);
    static GLint GetGlPixFormat() { return _glPixfmt; };
    static GLint GetGlPixType() { return _glPixtype; };
    
    
    static void DrawLine(SDL_Surface *surface, const Common::Line &line, uint8_t cr, uint8_t cg, uint8_t cb);
    static void BlitScaleMasked(SDL_Surface *src, Common::Rect sRect, SDL_Surface *mask, uint8_t index, SDL_Surface *dst, Common::Rect dRect);
    static void DrawFill(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect);
    
    static void Draw(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, Common::Point dPoint);
    static void Draw(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, Common::PointRect dRect);
    
    static SDL_Color Color(uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255);
    static bool ColorCmp(const SDL_Color &a, const SDL_Color &b);
    
protected:
    template <typename T> 
    static void DrawFillIntCKey(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect);
    template <typename T> 
    static void DrawFillIntACpy(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect);
    static void DrawFillIntCpy(SDL_Surface *src, const Common::Rect &sRect, SDL_Surface *dst, const Common::Rect &dRect);
    
private:
    GFXEngine();
    ~GFXEngine();
    
    void AddGfxMode(const GfxMode &md);
    uint32_t CursPix(uint8_t *data, int ofs, int bpp);
    
    static int EventsWatcher(void *, SDL_Event *event);

public:
    virtual size_t func0(IDVList &stak);
    virtual size_t raster_func198(const Common::FLine &arg);
    virtual size_t raster_func199(const Common::Line &arg);
    virtual size_t raster_func200(const Common::FLine &arg);
    virtual size_t raster_func201(const Common::Line &r);
    virtual size_t raster_func202(rstr_arg204 *arg);
    virtual size_t raster_func204(rstr_arg204 *arg);
    virtual void raster_func207(int id, TileMap *tiles);
    virtual TileMap *raster_func208(int id);
    virtual int raster_func208(TileMap *tiles);
    
    virtual void ProcessDrawSeq(const CmdStream &cmdline, const CmdIncludes *arr = NULL);
    
    virtual void raster_func210(const Common::FRect &arg);
    virtual void raster_func211(const Common::Rect &arg);
    virtual void BeginScene();
    virtual void EndScene();
    virtual size_t raster_func217(SDL_Color color);
    virtual void raster_func218(rstr_218_arg *arg);
    virtual void raster_func221(const Common::Rect &arg);
    virtual void BeginFrame();
    virtual void EndFrame();
    virtual void SetColorEffectsPowers(const std::vector<ColorFx> &arg);

    virtual bool AllocTexture(ResBitmap *arg);
    virtual void FreeTexture(ResBitmap *arg);

    // SetRenderStates only issues a glBindTexture / GLBindBuffer when the name
    // DIFFERS from the cached one. So the moment a GL name is deleted, the
    // cache has to forget it: GL hands freed names straight back to the next
    // glGenTextures / glGenBuffers, and a recycled name that still matches the
    // cache would be "bound" without ever being bound - its upload and its
    // filter/wrap parameters would land on whatever is really bound instead,
    // leaving the new object with no storage at all. Call these BEFORE the
    // delete, from every site that deletes a texture or a buffer.
    void ForgetTexture(uint32_t tex);
    void ForgetBuffer(uint32_t buf);
    virtual void display_func271(IDVPair *stak);
    virtual void SaveScreenshot(const std::string & screenName);
    virtual void windd_func320(IDVPair *);
    virtual void windd_func321(IDVPair *);
    virtual void windd_func322(windd_dlgBox *dlgBox);
    
    virtual const std::vector<TGFXDeviceInfo>& GetDevices();
    virtual void SetDeviceByGUID(const std::string &guid, bool writefile = true);

    
    void MeshMakeVBO(TMesh *);
    void MeshFreeVBO(TMesh *);
    

    virtual void setWDD_cursor(int arg);
    virtual void setWDD_disLowRes(int arg);
    virtual void setWDD_16bitTex(int arg);
    virtual void setWDD_drawPrim(int arg);

    virtual int getWDD_16bitTex();
    virtual int getWDD_drawPrim();
    
    
    
    virtual void SetPalette(UA_PALETTE &pal);
    virtual void SetPen(SDL_Color pen);
    virtual void setRSTR_BGpen(uint32_t pen) {};
    virtual void setRSTR_shdRmp(ResBitmap *rmp) {};
    virtual void setRSTR_trcRmp(ResBitmap *rmp) {};
    virtual void setRSTR_FGApen(uint32_t pen) {};

    virtual UA_PALETTE *GetPalette();

    virtual int16_t GetScreenW() const { return _resolution.x; }
    virtual int16_t GetScreenH() const { return _resolution.y; }
    virtual Common::Point GetScreenSize() const { return _resolution; };
    
    
    
    
    // windd methods
    size_t windd_func0(IDVList &stak);

    virtual void setW3D_texFilt(int arg);

    void draw2DandFlush();
    
    void DrawVtxQuad(const std::array<GFX::TVertex, 4> &vtx);

    int LoadFontByDescr(const std::string &fontname);
    void matrixAspectCorrection(mat3x3 &inout, bool invert);
    void getAspectCorrection(float &cW, float &cH, bool invert);

    void setFrustumClip(float near, float far);

    virtual void ConvAlphaPalette(UA_PALETTE *dst, const UA_PALETTE &src, bool transp);

    virtual SDL_Surface *CreateSurfaceScreenFormat(int width, int height);
    virtual SDL_Surface *ConvertToScreenFormat(SDL_Surface *src);
    
    static SDL_Surface *ConvertSDLSurface(SDL_Surface *src, const SDL_PixelFormat * fmt);
        
    void fpsLimitter(int value);
    void BenchUncap(bool on);
    
    float GetColorEffectPower(int id);
    
    Common::Point ConvertPosTo2DStuff(const Common::Point &pos);
    
    void RenderingMeshOld(TRenderNode *mesh);
    void RenderingMesh(TRenderNode *mesh);
    void RenderNode(TRenderNode *node);
    void Rasterize(uint32_t RasterEtapes = RASTER_ALL);
    
    void QueueRenderMesh(TRenderNode *mesh);
    TRenderNode &AllocRenderNode() { return _renderNodesCache.GetNext(); };
    
    float GetAlpha() const { return _alpha / 255.0; };
    
    GfxStates& States() { return _states; };
    const GfxStates& GetLastStates() { return _lastStates; };
    
    const TShaderProg &GetStdShaderProg() { return _stdShaderProg; }; 
    bool RenderV2() const { return _renderV2; };
    void SetRenderV2(bool on);   // gfx settings: V2 level at level load
    void SetGround(bool on);     // gfx settings: FLOOR level at level load (needs V2)
    void InitV2Program();
    // Uploads an RGBA image as-is (no palette/pixfmt conversion), mipmapped,
    // GL_REPEAT. For modern asset maps (albedo/normal/ORM/emissive).
    // customMips (optional): authored mip levels 1..N (level i must be
    // max(1,w>>i) x max(1,h>>i)); they are uploaded as-is and only the levels
    // below N are generated (from level N). Empty/NULL = today's path.
    // srgb: upload as GL_SRGB8_ALPHA8 (V2 albedo; see V2AlbedoSrgb()).
    ResBitmap *LoadRawTexture(SDL_Surface *src, const std::vector<SDL_Surface *> *customMips = NULL, bool srgb = false);
    // V2 albedo maps are uploaded as sRGB textures (linear-light filtering and
    // mips) unless UA_R2_SRGBTEX=0.
    bool V2AlbedoSrgb() const;
    // Pre-compressed map (DDS with DX10 header: BC1 / BC3 / BC5 / BC7, full
    // mip chain in the file) uploaded with glCompressedTexImage2D. srgb picks
    // the sRGB GL variant for BC1/BC3/BC7 (same bits). Returns NULL when the
    // file is malformed or the GL lacks the format - the caller falls back to
    // the PNG.
    ResBitmap *LoadDdsTexture(const std::string &path, bool srgb);
    // Compressed-format support detected at GL init (log line "texcomp: ...").
    struct TexCompCaps { bool s3tc = false, s3tcSrgb = false, rgtc = false, bptc = false; };
    const TexCompCaps &GetTexCompCaps();
    
    void SetRenderStates(int arg);
    
    void BindVBOParameters(TShaderProg &shader);
    
    void CommitUBOParameters();
    
    void SetProjectionMatrix(const mat4x4f &mat);
    void SetModelViewMatrix(const mat4x4f &mat);
    
    const mat4x4f &GetProjectionMatrix() const { return _frustum; }; 
    float GetProjectionNear() const { return _frustumNear; }; 
    float GetProjectionFar() const { return _frustumFar; }; 
    
protected:
    void initPolyEngine();

    void MaybeSaveShot();
    void DrawScreenText();
    void AddScreenText(const std::string &string, int p1, int p2, int p3, int p4, int flag);
    void DrawTextEntry(const ScreenText *txt);

    
    void win3d_func274__sub0(FSMgr::FileHandle *fil);

    static SDL_Cursor *wrapLoadCursor(const char *name);

    void _setFrustumClip(float near, float far);
    
    void sub_420EDC(Common::Line line, uint8_t r, uint8_t g, uint8_t b);
    char * windd_func322__sub0(const char *box_title, const char *box_ok, const char *box_cancel, const char *box_startText, uint32_t timer_time, void (*timer_func)(int, int, int), void *timer_context, int replace, int maxLen);
    
    void ApplyResolution();
    void UpdateAspectCorrection();

    bool SetResVariables(Common::Point res);
    

public:
    //Data
    //static constexpr const char * __ClassName = ;
    
    
public:
    static GFXEngine Instance;
    
private:
    std::vector<GfxMode> graphicsModes;
    std::array<SDL_Cursor *, 11> cursors;
    int CurrentCursorID = -1;
    GfxMode GfxSelectedMode;
    
    SDL_Surface *ScreenSurface = NULL;
    GLuint screenTex = 0;

    // Tiled screen-surface upload (only when UiScale() > 1).
    // At UI 3x the 2D surface is 1920x1440 (11 MB) and was cleared + uploaded
    // whole every frame although in-game the HUD covers ~130 of 1350 64x32 tiles
    // (Apple M-series 4K L7575: +1.3 ms per frame, the main cost of Low/Medium). Every
    // writer into Screen() marks its destination tiles (MarkScreen); only marked
    // tiles are cleared, and only tiles marked now or non-empty on the GPU are
    // uploaded; an all-empty frame skips the quad. Pixel output is identical
    // (an unmarked tile is all zero = cleared). UA_UI_TILED=0 = the old full
    // path, =2 = also scan the surface and report/repair any unmarked tile.
    bool _scrTiled = false;
    int _scrTilesX = 0, _scrTilesY = 0;
    std::vector<uint8_t> _scrTileNow;      // written since the last clear (cleared next)
    std::vector<uint8_t> _scrTileGpu;      // non-zero in the GPU texture
    bool _scrGpuAny = true;
    int _scrVerify = 0;                    // UA_UI_TILED=2: also scan, report + repair unmarked tiles
    void ScanScreenTiles(std::vector<uint8_t> &out);
    // UI graphics: bounding boxes (in tiles) of the 4-connected groups of set tiles
    // in mask, merged while they overlap - one upload / one scissored draw per HUD
    // element instead of one per tile run (100+ GL calls per frame at 4K).
    void TileBoxes(const std::vector<uint8_t> &mask, std::vector<SDL_Rect> &out);
    std::vector<SDL_Rect> _scrBoxes;
    std::vector<uint8_t> _scrMask;
    std::vector<int> _scrStack;
    void ClearScreenSurface();
public:
    // Every software write into Screen() at UiScale() > 1 reports its destination
    // rect (surface pixels; clipped here). No-op on the classic path.
    void MarkScreen(int x, int y, int w, int h);
    void MarkScreen(const SDL_Rect &r) { MarkScreen(r.x, r.y, r.w, r.h); }
private:

    // Hi-res UI text layer: an RGBA surface the size of the real drawable that
    // only ever holds classic ScreenText glyphs, composited as a second
    // fullscreen quad right after the (logical-resolution) screen quad.
    SDL_Surface *TextSurface = NULL;
    GLuint textTex = 0;
    Common::Point _textForDrawable;        // drawable TextSurface was sized for
    std::vector<SDL_Rect> _textDirtyCur;   // regions touched this frame
    std::vector<SDL_Rect> _textDirtyPrev;  // regions to clear at BeginFrame

    bool EnsureHiFont(int targetH);
    void TextLayerDirty(const SDL_Rect &r);
    void ClearTextLayer();

    // Hi-density UI bitmap layer
    int  _uiScale = 1;
    bool _uiScaleResolved = false;
    bool _uiLetterbox = false;
    bool _uiLetterboxResolved = false;
    void ResolveUiScale();
    void DrawLineUI(const Common::Line &line, uint8_t r, uint8_t g, uint8_t b);

    static const std::string _stdPShaderText;
    static const std::string _stdVShaderText;
    static const std::string _v2PShaderText;
    static const std::string _v2VShaderText;

public:
    int _forcesoftcursor;
    int _field_38;
    int _txt16bit;
    int _use_simple_d3d;
    int _disable_lowres;
    int _export_window_mode;
    int _flags;

    // From bigdata
    // \From bigdata

    int _dither;
    int _filter;
    int _antialias;
    bool _mipMaps = false;
    float _maxAniso = 0.0f;
    int _alpha;
    int _zbuf_when_tracy;
    int _colorkey;

    
    
    std::list<TRenderNode *> _renderSolidList;
    std::list<TRenderNode *> _renderSkyBoxList;
    std::list<TRenderNode *> _renderZeroTracyList;
    std::list<TRenderNode *> _renderLumaTracyList;
    
    Common::CacheStorage<TRenderNode> _renderNodesCache;

    int _sceneBeginned;

    ScreenFont _font;

    float _corrW, _corrIW;
    float _corrH, _corrIH;
    bool _solidFont;
    
    bool _glext = false;
    bool _vbo = false;
    
    // Display class
    SDL_Color _field_4 = {0}; // Color?
    Common::Rect _clip;
    Common::Rect _inverseClip;
    std::array<TileMap *, 256> _tiles;
    int _field_54c   = 0;
    int _field_550   = 0;
    float _field_554 = 0;
    float _field_558 = 0;
    
    UA_PALETTE _palette;
    
protected:
    // Display class
    Common::Point _resolution;

    // Win3d
    mat4x4f _frustum;
    float _frustumNear;
    float _frustumFar;
    
    uint32_t _stdPsShader = 0;
    uint32_t _stdVsShader = 0;
    TShaderProg _stdShaderProg;

    // Reforge renderer (UA_RENDER_V2=1). A second program used ONLY for
    // meshes that carry a TPbrMaterial; every original mesh stays on
    // _stdShaderProg, so V2 cannot change a legacy pixel.
    bool _renderV2 = false;
    uint32_t _v2PsShader = 0;
    uint32_t _v2VsShader = 0;
    TShaderProg _v2ShaderProg;
    struct
    {
        int32_t SunDir = -1, SunColor = -1, Ambient = -1;
        int32_t HasNormal = -1, HasOrm = -1, HasEmissive = -1;
        int32_t RoughMetal = -1, Emission = -1;
        int32_t NormalTex = -1, OrmTex = -1, EmissiveTex = -1, AlbedoTex = -1;
        int32_t Exposure = -1, HemiUp = -1, HemiDelta = -1, ToneMap = -1;
        int32_t UseAo = -1, AlbedoLinear = -1, NormalScale = -1, Debug = -1;
        int32_t NormalRG = -1;
        int32_t Terms = -1, FarLod = -1, NormalMip = -1, FarNormLod = -1, FarGain = -1;
    } _v2Loc;
    void RenderingMeshV2Setup(TRenderNode *nod);
    // Reforge GLOW (UA_GFX_GLOW, gfx_glow.cpp): replays LUMATR draws into its mask.
    friend class GlowPass;
    // 1x1 stand-ins bound to units 1-3 when a V2 material has no
    // normal/ORM/emissive map (flat normal, white ORM, black emissive). The
    // shader never samples them (Has* = 0), but every active sampler must
    // see a complete texture or macOS GL logs "unit N ... is unloadable ...
    // using zero texture" (backlog #50). Created on first need, never cached
    // in the state machine, deleted with the V2 program.
    uint32_t _v2DefaultTex[3] = {0, 0, 0};

    // Reforge ground (UA_RENDER_V2=1 + UA_R2_GROUND=1, ground.cpp). A third
    // program, used ONLY for draws whose texture is a tagged ground atlas;
    // off = never compiled, no texture loaded, draw path unchanged.
    bool _groundOn = false;
    TShaderProg _gndShaderProg;
    uint32_t _gndVs = 0, _gndPs = 0;
    void GroundInit();
    bool GroundBuildProgram();
    void GroundDeinit();
    bool GroundUse(const TRenderNode *nod) const;
    void RenderingMeshGroundSetup(TRenderNode *nod);
    int _gndAtmos = 0;          // the ground program was compiled from the ATMOS variant text

public:
    // Reforge ATMOSPHERE (atmos.h / atmos_gl.cpp). Nothing here runs at ATMOS 0.
    void AtmosRefresh();                         // level load, after Atmos::OnLevelLoad
    void AtmosDrawWeather(float dtSec);          // level 3, after the world frame (depth test, no z write)
    static std::string AtmosVariant(const std::string &src, int kind); // 1 V2 VS, 2 V2 PS, 3 ground VS, 4 ground PS
    void AtmosGroundUniforms(uint32_t prog);     // ground program: per-level atmos uniforms
protected:
    bool AtmosLegacyUse(const TRenderNode *nod, uint32_t flags) const;
    void AtmosStdSetup(const TRenderNode *nod);
    void AtmosV2Setup(const TRenderNode *nod, float sunView[3]);
    TShaderProg _atmStdProg;
    uint32_t _atmStdVs = 0, _atmStdPs = 0;
    struct { int32_t CamRot = -1, Haze0 = -1, Fog = -1, Grade = -1, Win = -1; } _atmLoc;
    uint32_t _atmLevelGen = 0;                   // bumps at every AtmosRefresh (per-level uniforms re-sent)
    int _v2Atmos = 0;                            // the V2 program was compiled from the ATMOS variant text
    int32_t _v2AtmHaze = -1, _v2AtmFog = -1;
    int32_t _v2AtmWin = -1, _v2AtmWinS = -1;      // V2 ATMOS variant: lit-window strength + mask sampler (unit 6)
    void AtmosV2Locations();                     // (re)read the V2 ATMOS variant's uniform locations
    TShaderProg _wxProg;
    uint32_t _wxVs = 0, _wxPs = 0, _wxVbo = 0, _wxVao = 0;
    int _wxCount = 0, _wxKind = 0;
    struct { int32_t ViewRot = -1, CamPos = -1, Box = -1, Vel = -1, Time = -1, Color = -1, Streak = -1,
             PtSize = -1; } _wxLoc;
    double _wxTime = 0.0;
    static const std::string _gndVShaderText;
    static const std::string _gndPShaderText;
    // Reforge sun shadows (shadows.cpp, UA_GFX_SHADOWS / gfxLevel("SHADOWS")).
    // Level 0: none of this allocates or runs; _shadowFrame stays false.
    TShaderProg _shdStdProg;     // legacy receiver program (level > 0 only)
    bool _shadowWorld = false;   // the next Rasterize is the world frame
    bool _shadowFrame = false;   // shadow map valid for the draws in progress
    static int ShadowLevel();
    std::string ShadowV2PS(const std::string &src);
    void ShadowsInit();
    void ShadowsDeinit();
    void RebuildV2Program();     // delete + InitV2Program from the current texts (keeps _renderV2)
    void RebuildGroundProgram(); // delete + GroundBuildProgram (floor on only)
    void AtmosFreeLegacyProgram();
    void ShadowPass();
    void ShadowV2Draw(float farFade);
    bool ShadowLegacyReceiver(const TRenderNode *nod) const;
    // Floor program receives shadows; programs linked after ShadowsInit
    std::string ShadowGroundVS(const std::string &src);
    std::string ShadowGroundPS(const std::string &src);
    void ShadowGroundDraw();
    std::string ShadowAtmStdPS(const std::string &src); // its VS: ShadowGroundVS (same anchor)
    void ShadowAtmDraw();
    void ShadowProgramLinked(uint32_t prog);
public:
    void ShadowWorldScene();
    void ShadowsApply();         // level load (GfxSettings::OnLevelLoad): follow a SHADOWS change
    static bool ShadowHostTag() { return ShadowLevel() > 0; } // tag host-station draws (level > 0 only)
    // V2 sun as configured (UA_R2_SUN/HEMI), world space, towards the sun. THE shared sun: when ATMOSPHERE is
    // active (atmos.h) the direction is the level's sky sun; shadows/ground/glow read it from here.
    void V2SunParams(float sun[3], float *sunLin, float *ambLin, float *hemiHalf);
protected:

    // UA_FRAMETIME_LOG=<file>: one line per finished frame, inert unless set.
    FILE *_frameLog = NULL;
    uint64_t _frameLogLast = 0;
    void LogFrameTime();
    
    uint32_t _vboParams = 0;
    
    static constexpr int32_t _vboMProjOff = 0;   // 4 * 4 * 4 = 64
    static constexpr int32_t _vboMViewOff = 64;  // 4 * 4 * 4 = 64
    static constexpr int32_t _vboFogOff   = 128; // 3 * 4 = 12  ... + 4(because next vec3)
    static constexpr int32_t _vboAFogOff  = 144; // 3 * 4 = 12
    static constexpr int32_t _vboTextured = 156; // 4
    static constexpr int32_t _vboFlat     = 160; // 4
    static constexpr int32_t _vboATest    = 164; // 4
    static constexpr int32_t _vboParamsSize = 168; 
    static constexpr int32_t _vboParamsBlockBinding = 0;
    
    struct
    {
        mat4x4f Proj;
        mat4x4f View;
        float   Fog = 0.0;
        float   FogStart = 0.0;
        float   FogLength = 0.0;
        float   _pad1 = 0.0;
        float   AFog = 0.0;
        float   AFogStart = 0.0;
        float   AFogLength = 0.0;
        int32_t Textured = 0;
        int32_t Flat = 0;
        int32_t ATest = 0;
    } _vboStatesBlock;
    
    bool _vboStatesChanged = true;
    
    uint32_t _globalVao = 0;
    
    GfxStates _states;
    GfxStates _lastStates;
    
    uint32_t _stdQuadDataBuf = 0;
    uint32_t _stdQuadIndexBuf = 0;

    vec3d _normClr;
    vec3d _invClr;
    
    int32_t _colorEffects = 0;
    
    uint32_t _fbo = 0;
    uint32_t _fbod = 0;
    uint32_t _fboTex = 0;
    int32_t _fboBlend = 0;
    
    uint32_t _psShader = 0;
    uint32_t _vsShader = 0;
    TColorEffectsProg _colorEffectsShaderProg;
    
    static std::vector<TGFXDeviceInfo> _devices;
};
    
static constexpr GFXEngine &Engine = GFXEngine::Instance;






    
}

#endif // ENGINE_GFX_H_INCLUDED
