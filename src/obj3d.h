#ifndef OBJ3D_H_INCLUDED
#define OBJ3D_H_INCLUDED

#include <list>
#include <map>
#include <string>
#include "system/fsmgr.h"
#include "nucleas.h"
#include "base.h"
#include "bitmap.h"

// Reforge: a disk-map flipbook for hd-model (OBJ) materials, the
// OBJ counterpart of the original bmpanim.class (UV movement across one
// sheet). MTL directives, after map_Kd:
//   ua_flipbook <cols> <rows> <frame> <frame> ...      sequence of cell 0
//   ua_flipbook_cell <cell> <frame> <frame> ...        sequence of another cell
//   frame = <cell>:<ms>[:h|v|hv]   cell = row-major index in the cols x rows
//                                  grid of map_Kd (row 0 = top of the image),
//                                  ms = hold time, h/v = mirror inside the cell
// A face belongs to the group of the cell its static UVs address (centroid);
// every group runs its own looping sequence, so ONE material (one draw call)
// carries many independently flickering sheets (the original gives each arc
// sheet its own bmpanim). Faces in a cell without a sequence stay static, and
// an engine without the directives shows every face's static cell. The
// sequences are merged into one timeline of gcd(ms) ticks over lcm(cycles)
// (capped at 2048 ticks) and advance on the game clock exactly like a bmpanim
// (shared by every instance of the model), through the existing
// RFLAGS_DYNAMIC_TEXTURE / CoordsCache path: one VBO coordinate set per tick,
// no per-frame CPU work but SetTime.
// UA_FLIPBOOK=0 ignores the directives (static, before/after checks).
class ReforgeFlipbook: public NC_STACK_bitmap
{
public:
    struct Frame
    {
        int32_t ms = 40;
        int32_t cell = 0;
        bool mirU = false;
        bool mirV = false;
    };
    typedef std::map<int, std::vector<Frame> > Groups; // static cell -> sequence

    ReforgeFlipbook(ResBitmap *tex, int cols, int rows, const Groups &groups);
    virtual ~ReforgeFlipbook() {};

    // Per-tick UVs of a mesh drawn with this flipbook; also renumbers the
    // mesh's TexCoordId to the vertex index so NC_STACK_base's generic
    // GenerateMeshCoordsCache (any later MakeCoordsCache) reads them back.
    void Bind(GFX::TMesh *mesh);

    virtual void SetTime(int32_t timeStamp, int32_t frameTime) override;
    virtual ResBitmap *GetBitmap(int frameid = -1) override { return _tex; };
    virtual std::vector<tUtV> &GetOutline(int frameid = -1) override;
    virtual bool IsDynamic() const override { return true; };
    virtual uint32_t GetFramesCount() const override { return _ticks; };
    virtual uint32_t GetCurrentFrameID() const override { return _cur; };
    virtual void PrepareTexture(bool force = false) override {};
    // No rsrc behind it (resource == NULL for every flipbook): identity, so
    // two flipbook materials of one OBJ never merge into one mesh
    // (TRenderParams::operator== -> IsSameRes for dynamic textures).
    virtual bool IsSameRes(NC_STACK_rsrc *res) override { return res == this; };

    int Ticks() const { return _ticks; };
    int TickMs() const { return _tick; };
    static bool On();

private:
    const Frame *FrameAt(const std::vector<Frame> &seq, int32_t t) const;

    ResBitmap *_tex;
    int _cols, _rows;
    Groups _groups;
    int32_t _tick = 40;      // ms per timeline step
    uint32_t _ticks = 1;     // timeline length (steps)
    std::vector< std::vector<tUtV> > _outlines; // [tick][vertex]
    uint32_t _cur = 0;
    int32_t _timeStamp = 0;
    int32_t _leftTime = 0;
};

class NC_STACK_Obj3D: public NC_STACK_base
{        
protected:
    struct FaceVT
    {
        int32_t V = 0;
        int32_t T = 0;
        int32_t N = 0;
        
        FaceVT() = default;
        FaceVT(int32_t v, int32_t t, int32_t n)
        : V(v), T(t), N(n) {};
    };
    
    struct Mtl
    {
        std::string name;
        float diffuse[3] = {1.0, 1.0, 1.0};
        NC_STACK_bitmap *diffuseMap = NULL;
        float d = 1.0;

        // Reforge material maps (_albedo / _normal / _orm next to the OBJ).
        // Loaded as raw RGBA GL textures, owned here.
        ResBitmap *rawDiffuse = NULL;     // map_Kd found beside the .mtl
        std::string kdDiskPath;           // map_Kd on disk: loaded at FinishMtl (sRGB when V2)
                                          // (d < 1: luminance-as-alpha, see LumTracyPixels)
        GFX::TPbrMaterial pbr;
        int flipCols = 0, flipRows = 0;           // ua_flipbook (see ReforgeFlipbook)
        ReforgeFlipbook::Groups flipGroups;
        ReforgeFlipbook *flip = NULL;             // owned
        bool hasPbr = false;              // any of norm/ORM/Ke/Pr/Pm given
        bool keGiven = false;
        bool pmGiven = false;
        void Finish();

        Mtl() = default;
        Mtl(const Mtl &) = delete;
        Mtl &operator=(const Mtl &) = delete;
        ~Mtl();
        
        GFX::TRenderParams GenParams() const;
    };
        
public:
    virtual size_t Init(IDVList &stak);
    virtual size_t Deinit();

    NC_STACK_Obj3D();
    virtual ~NC_STACK_Obj3D();

    virtual const std::string ClassName() const {
        return __ClassName;
    };

    
    virtual void RecalcInternal(bool kids = false) override {};

    bool LoadFromFile(const std::string &filename);
    bool LoadFromFile(FSMgr::FileHandle *file);
    
    static NC_STACK_Obj3D *LoadObj3D(const std::string &filename);
    // Absolute disk path, outside the game VFS (.mtl + maps beside it).
    static NC_STACK_Obj3D *LoadObj3DDisk(const std::string &diskPath);
    
    static bool ExportObj(NC_STACK_base *base, const std::string &fname);

private:
    bool ParseObj(FSMgr::FileHandle *fil);
    FaceVT ParseFaceVT(const std::string &l);
    bool ParseMtl(const std::string &filename);
    bool ParseMtlStream(FSMgr::FileHandle *fil, const std::string &mtlDir);
    Mtl *FindMtl(const std::string &filename);
    ResBitmap *LoadMap(const std::string &mtlDir, const std::string &name, bool srgb = false, bool lumTracy = false);
    void FinishMtl(Mtl *m);
    void BuildShadingFrames();

public:
    //Data
    static constexpr const char * __ClassName = "obj3d.class";

private:
    std::vector<vec3d> _v;
    std::vector<vec3d> _vt;
    std::vector<vec3d> _vn;
    std::vector<vec3d> _vc;   // optional per-vertex colour (v x y z r g b), 1,1,1 if absent
    std::vector< std::array<FaceVT, GFX_MAX_VERTEX> > _f;
    std::list<Mtl> _mtls;
    
    std::string fname;
    
    Mtl *_currentMtl = NULL;

    // Per emitted mesh vertex: the OBJ (v, vt, vn) corner it came from, so
    // tangents can be accumulated across faces that share a corner.
    std::map<GFX::TMesh *, std::vector<std::array<int32_t, 3>>> _corners;
    bool _anyPbr = false;
    
    int32_t faceNum = 0;
};

#endif // OBJ3D_H_INCLUDED
