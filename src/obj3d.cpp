#include "system/gfx_glow.h"
#include "obj3d.h"
#include "image.h"
#include "env.h"
#include "system/inivals.h"
#include "loaders.h"
#include "fmtlib/printf.h"
#include <SDL2/SDL_image.h>
#include <map>
#include <unordered_map>
#include <sys/stat.h>
#include "system/perf.h"
#include <cmath>
#include <algorithm>
#include <atomic>

extern std::atomic<int> g_statsMaterialMapsMissing; // main.cpp (anonymous usage stats)
#ifdef _WIN32
#include "winport.h"
#define UA_ABSPATH(s) uaIsAbsPath(s)
#else
#define UA_ABSPATH(s) ((s)[0] == '/')
#endif


NC_STACK_Obj3D::NC_STACK_Obj3D()
{}

NC_STACK_Obj3D::~NC_STACK_Obj3D()
{}

size_t NC_STACK_Obj3D::Init(IDVList &stak)
{
    if ( !NC_STACK_base::Init(stak) )
        return 0;

    return 1;
}

size_t NC_STACK_Obj3D::Deinit()
{
    return NC_STACK_base::Deinit();
}

bool NC_STACK_Obj3D::LoadFromFile(const std::string &filename)
{
    FSMgr::FileHandle fil = FSMgr::iDir::openFile(filename, "r");

    if (!fil.OK())
        return false;
    
    fname = filename;

    return LoadFromFile(&fil);
}

bool NC_STACK_Obj3D::LoadFromFile(FSMgr::FileHandle *fil)
{
    if (!ParseObj(fil))
        return false;

    for (GFX::TMesh &m : Meshes)
        if (ReforgeFlipbook *fb = dynamic_cast<ReforgeFlipbook *>(m.Mat.TexSource))
            fb->Bind(&m);

    MakeCoordsCache();
        
    _skeleton = Nucleus::CInit<NC_STACK_skeleton>( {
        {NC_STACK_rsrc::RSRC_ATT_NAME, std::string("Obj3D_sklt")},
        {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
        {NC_STACK_skeleton::SKEL_ATT_POINTSCNT, (int32_t)_v.size()}});  
    
    UAskeleton::Data *dat = _skeleton->GetSkelet();

    for (int32_t i = 0; i < (int32_t)_v.size(); ++i)
        dat->POO[i] = _v[i];
    
    dat->polygons.resize(_f.size());

    for (int32_t i = 0; i < (int32_t)_f.size(); i++)
    {
        UAskeleton::Polygon &pol = dat->polygons[i];
        
        for (int j = 0; j < GFX_MAX_VERTEX; ++j )
        {
            FaceVT &f = _f[i].at(j);
            if (f.V == 0)
            {
                break;
            }
            
            pol.v[j] = f.V - 1;
            pol.num_vertices = j + 1;
        }
    }
    
    return true;
}

NC_STACK_Obj3D *NC_STACK_Obj3D::LoadObj3D(const std::string &filename)
{
    FSMgr::FileHandle fil = uaOpenFile(filename, "r");
    if (!fil.OK())
        return NULL;
    
    NC_STACK_Obj3D *tmp = Nucleus::CInit<NC_STACK_Obj3D>();
    tmp->fname = filename;
    if(tmp->LoadFromFile(&fil))
        return tmp;
    
    tmp->Delete();
    return NULL;
}

NC_STACK_Obj3D *NC_STACK_Obj3D::LoadObj3DDisk(const std::string &diskPath)
{
    FSMgr::FileHandle fil(diskPath, "r");
    if (!fil.OK())
        return NULL;

    NC_STACK_Obj3D *tmp = Nucleus::CInit<NC_STACK_Obj3D>();
    tmp->fname = diskPath;
    if(tmp->LoadFromFile(&fil))
        return tmp;

    tmp->Delete();
    return NULL;
}

bool NC_STACK_Obj3D::ParseObj(FSMgr::FileHandle *fil)
{
    if (!fil->OK())
        return false;
    
    std::string line;
    while ( fil->ReadLine(&line) && !line.empty() )
    {
        size_t pos = line.find_first_of("#\n\r");
        if ( pos != std::string::npos )
            line.erase(pos);

        if (line.empty())
            continue;
        
        std::vector<std::string> tokens = Stok::Split(line, " \t");
        
        if (tokens.empty())
            continue;
        
        if (!StriCmp(tokens[0], "v"))
        {
            _v.emplace_back( std::stof(tokens[1]), -std::stof(tokens[2]), std::stof(tokens[3]) );
            // HD buildings: optional per-vertex colour "v x y z r g b" (0..1) multiplies the material
            // colour (baked soot / grime on hd legos). Absent -> 1,1,1 (every existing OBJ unchanged).
            if (tokens.size() >= 7)
                _vc.emplace_back( std::stof(tokens[4]), std::stof(tokens[5]), std::stof(tokens[6]) );
            else
                _vc.emplace_back( 1.0, 1.0, 1.0 );
        }
        else if (!StriCmp(tokens[0], "vt"))
        {
            if (tokens.size() == 3)
                _vt.emplace_back( std::stof(tokens[1]), std::stof(tokens[2]), 0.0 );
            else 
                _vt.emplace_back( std::stof(tokens[1]), std::stof(tokens[2]), std::stof(tokens[3]) );
        }
        else if (!StriCmp(tokens[0], "vn"))
        {
            // Same axis flip as positions (UA y points down).
            _vn.emplace_back( std::stof(tokens[1]), -std::stof(tokens[2]), std::stof(tokens[3]) );
        }
        else if (!StriCmp(tokens[0], "f"))
        {
            _f.emplace_back();            
            auto &f = _f.back();
            
            int numvtx = tokens.size() - 1;
            
            for (int32_t i = 1; i < (int32_t)tokens.size(); ++i )
                f[i - 1] = ParseFaceVT(tokens[i]);
            
            if (numvtx >= 3)
            {
                GFX::TRenderParams prm;
                if (_currentMtl)
                    prm = _currentMtl->GenParams();
                else
                    prm.Flags |= GFX::RFLAGS_FOG | GFX::RFLAGS_SHADED;

                GFX::TMesh *msh = FindMeshByRenderParams(&Meshes, prm);
                if (!msh)
                {
                    Meshes.emplace_back();
                    msh = &Meshes.back();
                    msh->Mat = prm;
                }
                
                uint32_t fid = msh->Vertexes.size();
                std::vector<std::array<int32_t, 3>> &corners = _corners[msh];
                
                for(int j = 0; j < numvtx; ++j)
                {
                    FaceVT &ftx = f[j];
                    corners.push_back({ftx.V, ftx.T, ftx.N});
                    msh->Vertexes.emplace_back();
                    if (ftx.N > 0 && ftx.N <= (int32_t)_vn.size())
                        msh->Vertexes.back().Normal = _vn.at(ftx.N - 1);
                    msh->Vertexes.back().Pos = _v.at(ftx.V - 1);
                    msh->Vertexes.back().TexCoordId = j;
                    msh->Vertexes.back().Color = prm.Color;
                    if (ftx.V > 0 && ftx.V <= (int32_t)_vc.size())
                    {
                        const vec3d &vc = _vc.at(ftx.V - 1);
                        msh->Vertexes.back().Color.r *= vc.x;
                        msh->Vertexes.back().Color.g *= vc.y;
                        msh->Vertexes.back().Color.b *= vc.z;
                    }

                    msh->BoundBox.Add( _v.at(ftx.V - 1) );

                    if (ftx.T > 0)
                        msh->Vertexes.back().TexCoord = tUtV(_vt.at(ftx.T - 1).x, 1.0 - _vt.at(ftx.T - 1).y);
                }

                for(int j = 2; j < numvtx; ++j)
                {
                    msh->Indixes.push_back(fid + 0);
                    msh->Indixes.push_back(fid + j);
                    msh->Indixes.push_back(fid + j - 1);
                }
            }
        }
        else if (!StriCmp(tokens[0], "mtllib"))
        {
            ParseMtl(tokens[1]);
        }
        else if (!StriCmp(tokens[0], "usemtl"))
        {
            _currentMtl = FindMtl(tokens[1]);
        }
    }

    if (_anyPbr)
        BuildShadingFrames();
    _corners.clear();
    return true;
}


NC_STACK_Obj3D::FaceVT NC_STACK_Obj3D::ParseFaceVT(const std::string &l)
{
    int s = 0;
    std::string t[3];
    for(auto c : l)
    {
        if (c == '/')
            s++;
        else
            t[s] += c;
    }
    
    int32_t v[3] {0, 0, 0};

    for(int i = 0; i < 3; ++i)
    {
        if (!t[i].empty())
            v[i] = std::stoi(t[i]);
    }
    
    return FaceVT( v[0], v[1], v[2] );
}
        

bool NC_STACK_Obj3D::ParseMtl(const std::string &filename)
{
    // Disk-path OBJ (reforge test hook / hd-models loaded by absolute path):
    // the .mtl and every map live beside it on disk, outside the game VFS.
    if (!fname.empty() && UA_ABSPATH(fname))
    {
        std::string dir = fname.substr(0, fname.find_last_of('/') + 1);
        std::string disk = (!filename.empty() && UA_ABSPATH(filename)) ? filename : dir + filename;
        FSMgr::FileHandle dfil(disk, "r");
        if (!dfil.OK())
            return false;
        return ParseMtlStream(&dfil, disk.substr(0, disk.find_last_of('/') + 1));
    }

    std::string fl = filename;
    if (!uaFileExist(fl))
    {
        if (fl.size() > 2 && fl[0] == '.' && fl[1] == '/')
            fl = fl.substr(2);
        
        if (!uaFileExist(fl))
        {
            size_t pos = fname.find_last_of("/\\:");
            if (pos != std::string::npos)
                fl = fname.substr(0, pos + 1) + fl;
            else
                fl = fname + fl;
        }
    }
    
    FSMgr::FileHandle fil = uaOpenFile(fl, "r");
    if (!fil.OK())
        return false;

    size_t sl = fl.find_last_of("/\\:");
    return ParseMtlStream(&fil, sl == std::string::npos ? std::string() : fl.substr(0, sl + 1));
}

// Map statements may carry options before the file name (e.g. Blender's
// "map_Bump -bm 1.000000 file.png"); the file name is the last token.
static std::string MapFileToken(const std::vector<std::string> &tokens)
{
    return tokens.size() >= 2 ? tokens.back() : std::string();
}

bool NC_STACK_Obj3D::ParseMtlStream(FSMgr::FileHandle *filp, const std::string &mtlDir)
{
    FSMgr::FileHandle &fil = *filp;
    Mtl *current = NULL;
    
    std::string line;
    while ( fil.ReadLine(&line) && !line.empty() )
    {
        size_t pos = line.find_first_of("#\n\r");
        if ( pos != std::string::npos )
            line.erase(pos);

        if (line.empty())
            continue;
        
        std::vector<std::string> tokens = Stok::Split(line, " \t");
        
        if (tokens.empty())
            continue;
        
        if (!StriCmp(tokens[0], "newmtl"))
        {
            if (current)
                FinishMtl(current);
            _mtls.emplace_back();
            current = &_mtls.back();
            current->name = tokens[1];
        }
        else if (!StriCmp(tokens[0], "Ns"))
        {
            
        }
        else if (!StriCmp(tokens[0], "Ka"))
        {
            
        }
        else if (!StriCmp(tokens[0], "Kd"))
        {
            if (current && tokens.size() >= 4)
            {
                current->diffuse[0] = std::stof(tokens[1]);
                current->diffuse[1] = std::stof(tokens[2]);
                current->diffuse[2] = std::stof(tokens[3]);
            }
        }
        else if (!StriCmp(tokens[0], "Ks"))
        {

        }
        else if (!StriCmp(tokens[0], "Ke"))
        {
            if (current && tokens.size() >= 4)
            {
                current->pbr.Emission[0] = std::stof(tokens[1]);
                current->pbr.Emission[1] = std::stof(tokens[2]);
                current->pbr.Emission[2] = std::stof(tokens[3]);
                current->keGiven = true;
            }
        }
        else if (!StriCmp(tokens[0], "Pr"))
        {
            if (current && tokens.size() >= 2)
            {
                current->pbr.Roughness = std::stof(tokens[1]);
                current->hasPbr = true;
            }
        }
        else if (!StriCmp(tokens[0], "Pm"))
        {
            if (current && tokens.size() >= 2)
            {
                current->pbr.Metallic = std::stof(tokens[1]);
                current->pmGiven = true;
                current->hasPbr = true;
            }
        }
        else if (!StriCmp(tokens[0], "map_ORM"))
        {
            if (current && (current->pbr.Orm = LoadMap(mtlDir, MapFileToken(tokens))))
                current->hasPbr = true;
        }
        else if (!StriCmp(tokens[0], "Ni"))
        {

        }
        else if (!StriCmp(tokens[0], "Ns"))
        {

        }
        else if (!StriCmp(tokens[0], "d"))
        {
            if (current)
                current->d = std::stof(tokens[1]);
        }
        else if (!StriCmp(tokens[0], "illum"))
        {

        }
        else if (!StriCmp(tokens[0], "map_Kd"))
        {
            // Beside the .mtl first (reforge convention); else the legacy
            // rsrc lookup below.
            // A disk map is loaded when the material is complete
            // (FinishMtl): a V2 material's albedo goes up as an sRGB texture.
            if (current && !mtlDir.empty())
            {
                std::string nm = MapFileToken(tokens);
                std::string dp = (!nm.empty() && UA_ABSPATH(nm)) ? nm : mtlDir + nm;
                FILE *probe = (!dp.empty() && UA_ABSPATH(dp)) ? fopen(dp.c_str(), "rb") : NULL;
                if (probe)
                {
                    fclose(probe);
                    current->kdDiskPath = dp;
                    continue;
                }
                current->rawDiffuse = LoadMap(mtlDir, nm);
            }

            if (current && !current->rawDiffuse)
            {
                std::string txname = tokens[1];
                if (!uaFileExist(std::string("rsrc:") + txname))
                {
                    txname = "objects/" + tokens[1];
                    
                    if (!uaFileExist(std::string("rsrc:") + txname))
                        txname = "rsrcpool/" + tokens[1];
                }
                
                current->diffuseMap = Utils::ProxyLoadImage( {
                    {NC_STACK_rsrc::RSRC_ATT_NAME, txname},
                    {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
                    {NC_STACK_bitmap::BMD_ATT_CONVCOLOR, (int32_t)1}} );
                    
                if (current->diffuseMap)
                    current->diffuseMap->PrepareTexture();
                    
            }
        }
        else if (!StriCmp(tokens[0], "ua_flipbook") || !StriCmp(tokens[0], "ua_flipbook_cell"))
        {
            // ua_flipbook <cols> <rows> <frames...>  /  ua_flipbook_cell <cell> <frames...>
            // frame = <cell>:<ms>[:h|v|hv]  (ReforgeFlipbook)
            const bool head = !StriCmp(tokens[0], "ua_flipbook");
            if (current && tokens.size() >= (head ? 4u : 3u))
            {
                int group = 0;
                size_t first = 3;
                if (head)
                {
                    current->flipCols = std::max(1, atoi(tokens[1].c_str()));
                    current->flipRows = std::max(1, atoi(tokens[2].c_str()));
                }
                else
                {
                    group = atoi(tokens[1].c_str());
                    first = 2;
                }
                const int ncell = std::max(1, current->flipCols * current->flipRows);
                std::vector<ReforgeFlipbook::Frame> &seq = current->flipGroups[group];
                seq.clear();
                for (size_t i = first; i < tokens.size(); ++i)
                {
                    std::vector<std::string> f = Stok::Split(tokens[i], ":");
                    if (f.size() < 2)
                        continue;
                    ReforgeFlipbook::Frame fr;
                    fr.cell = std::max(0, std::min(atoi(f[0].c_str()), ncell - 1));
                    fr.ms = std::max(1, atoi(f[1].c_str()));
                    if (f.size() >= 3)
                    {
                        fr.mirU = f[2].find_first_of("hH") != std::string::npos;
                        fr.mirV = f[2].find_first_of("vV") != std::string::npos;
                    }
                    seq.push_back(fr);
                }
                if (seq.empty())
                    current->flipGroups.erase(group);
            }
        }
        else if (!StriCmp(tokens[0], "map_Ks"))
        {

        }
        else if (!StriCmp(tokens[0], "map_Ke"))
        {
            if (current && (current->pbr.Emissive = LoadMap(mtlDir, MapFileToken(tokens))))
                current->hasPbr = true;
        }
        else if (!StriCmp(tokens[0], "norm") || !StriCmp(tokens[0], "map_bump") || !StriCmp(tokens[0], "bump"))
        {
            // Tangent-space normal map, OpenGL convention (+Y = green up).
            // `-bm s` scales the tangent xy (Blender Normal Map strength).
            if (current && (current->pbr.Normal = LoadMap(mtlDir, MapFileToken(tokens))))
                current->hasPbr = true;
            if (current)
                for (size_t i = 1; i + 2 < tokens.size(); ++i)
                    if (!StriCmp(tokens[i], "-bm"))
                        current->pbr.NormalScale = std::stof(tokens[i + 1]);
        }
        else if (!StriCmp(tokens[0], "map_d"))
        {

        }
        else if (!StriCmp(tokens[0], "Tr"))
        {

        }
    }

    if (current)
        FinishMtl(current);

    for (Mtl &m : _mtls)
        if (m.hasPbr)
            _anyPbr = true;
    
    return true;
}

NC_STACK_Obj3D::Mtl *NC_STACK_Obj3D::FindMtl(const std::string &mtlname)
{
    for(Mtl &m : _mtls)
    {
        if (!StriCmp(m.name, mtlname))
            return &m;
    }
    return NULL;
}




// glTF-style factor defaults: a factor that is not written defaults to 1
// when its map is present (so the map is used as authored), else to the
// scalar default (metallic 0).
void NC_STACK_Obj3D::Mtl::Finish()
{
    if (pbr.Emissive && !keGiven)
        pbr.Emission[0] = pbr.Emission[1] = pbr.Emission[2] = 1.0;
    if (pbr.Orm && !pmGiven)
        pbr.Metallic = 1.0;
}

namespace
{
// Reforge performance: one GL texture per (disk path, sRGB flag) however many
// OBJs reference the file - VP_PANZ1, VPfPANZ1 and VPwPANZ1 all use the same
// three maps, which used to be decoded and uploaded three times each.
// Refcounted; the last Mtl to go frees it. UA_TEXCACHE=0 restores one upload
// per reference (the pre-perf behaviour, for before/after measurements).
struct SharedMap
{
    ResBitmap *bm;
    int refs;
    std::string src; // the file actually uploaded (<stem>.dds when LoadDdsTexture took it, else the map's path)
};
std::map<std::string, SharedMap> gMapCache;
std::map<ResBitmap *, std::string> gMapKey;
// UA_TEXCACHE=0 only: disk path of every live uncached disk map (Obj3DForEachCachedMap covers them too).
std::map<ResBitmap *, std::string> gMapPathLive;

bool MapCacheOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("UA_TEXCACHE");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on == 1;
}

// A <stem>.dds beside a disk map is used instead of the PNG when the GL can
// sample its format (BC1/BC3/BC5/BC7). UA_TEXCOMP=0 ignores .dds files.
bool TexCompOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("UA_TEXCOMP");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on == 1;
}

void ReleaseMap(ResBitmap *b)
{
    gMapPathLive.erase(b);
    auto k = gMapKey.find(b);
    if (k != gMapKey.end())
    {
        auto it = gMapCache.find(k->second);
        if (it != gMapCache.end() && --it->second.refs > 0)
            return;
        if (it != gMapCache.end())
            gMapCache.erase(it);
        gMapKey.erase(k);
    }
    GFX::Engine.FreeTexture(b);
    delete b;
}
}

void Obj3DForEachCachedMap(void (*fn)(ResBitmap *b, const std::string &path, void *ctx), void *ctx)
{
    for (auto &kv : gMapCache)
        if (kv.second.bm)
            fn(kv.second.bm, kv.second.src, ctx);
    for (auto &kv : gMapPathLive)
        fn(kv.first, kv.second, ctx);
}

NC_STACK_Obj3D::Mtl::~Mtl()
{
    if (diffuseMap)
        diffuseMap->Delete();

    delete flip;

    for (ResBitmap *b : {rawDiffuse, pbr.Normal, pbr.Orm, pbr.Emissive})
    {
        if (b)
            ReleaseMap(b);
    }
}

void NC_STACK_Obj3D::FinishMtl(Mtl *m)
{
    if (!m->kdDiskPath.empty() && !m->rawDiffuse)
    {
        bool srgb = m->hasPbr && GFX::Engine.V2AlbedoSrgb();
        m->rawDiffuse = LoadMap(std::string(), m->kdDiskPath, srgb, m->d < 1.0);
    }
    if (m->rawDiffuse && m->flipCols > 0 && !m->flipGroups.empty() && ReforgeFlipbook::On())
    {
        m->flip = new ReforgeFlipbook(m->rawDiffuse, m->flipCols, m->flipRows, m->flipGroups);
        printf("obj3d: material '%s' flipbook %dx%d, %d sequences, %d ticks of %d ms\n", m->name.c_str(),
               m->flipCols, m->flipRows, (int)m->flipGroups.size(), m->flip->Ticks(), m->flip->TickMs());
    }
    m->Finish();
}

bool ReforgeFlipbook::On()
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("UA_FLIPBOOK");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on == 1;
}

static int32_t Gcd(int32_t a, int32_t b)
{
    while (b)
    {
        int32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

ReforgeFlipbook::ReforgeFlipbook(ResBitmap *tex, int cols, int rows, const Groups &groups)
: _tex(tex), _cols(cols), _rows(rows), _groups(groups)
{
    // One timeline for every group: gcd of all hold times per step, lcm of
    // the group cycles long (the exporter keeps cycles dividing 4800 ms).
    int32_t g = 0;
    int64_t cyc = 1;
    for (auto &kv : _groups)
    {
        int32_t c = 0;
        for (const Frame &f : kv.second)
        {
            g = Gcd(g, f.ms);
            c += f.ms;
        }
        cyc = cyc / Gcd((int32_t)(cyc % 0x7fffffff), c) * c;
        if (cyc > 1000000)
            cyc = 1000000;
    }
    _tick = std::max(1, g);
    _ticks = (uint32_t)std::max<int64_t>(1, std::min<int64_t>(2048, cyc / _tick));
    if (cyc / _tick > 2048)
        printf("obj3d: flipbook timeline %lld ms / %d ms ticks capped at 2048 ticks\n", (long long)cyc, _tick);
}

const ReforgeFlipbook::Frame *ReforgeFlipbook::FrameAt(const std::vector<Frame> &seq, int32_t t) const
{
    int32_t c = 0;
    for (const Frame &f : seq)
        c += f.ms;
    if (c <= 0)
        return NULL;
    t %= c;
    for (const Frame &f : seq)
    {
        if (t < f.ms)
            return &f;
        t -= f.ms;
    }
    return &seq.back();
}

void ReforgeFlipbook::Bind(GFX::TMesh *mesh)
{
    const float cw = 1.0f / _cols, ch = 1.0f / _rows;
    const size_t nv = mesh->Vertexes.size();
    // Group of every vertex = the cell its face's static UV centroid lies in
    // (this loader emits fresh vertices per face corner, so per-vertex is per-face).
    std::vector<int> cell(nv, -1);
    for (size_t i = 0; i + 2 < mesh->Indixes.size(); i += 3)
    {
        float u = 0.0f, v = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            u += mesh->Vertexes[mesh->Indixes[i + k]].TexCoord.tu;
            v += mesh->Vertexes[mesh->Indixes[i + k]].TexCoord.tv;
        }
        int cx = std::max(0, std::min(_cols - 1, (int)(u / 3.0f / cw)));
        int cy = std::max(0, std::min(_rows - 1, (int)(v / 3.0f / ch)));
        for (int k = 0; k < 3; ++k)
            cell[mesh->Indixes[i + k]] = cy * _cols + cx;
    }
    _outlines.assign(_ticks, std::vector<tUtV>(nv));
    for (size_t j = 0; j < nv; ++j)
    {
        GFX::TVertex &vx = mesh->Vertexes[j];
        const int c = cell[j];
        auto it = c >= 0 ? _groups.find(c) : _groups.end();
        // Cell-local coordinates of the static UV (engine tv: 0 = top row).
        float lu = 0.0f, lv = 0.0f;
        if (c >= 0)
        {
            lu = std::min(1.0f, std::max(0.0f, (vx.TexCoord.tu - (c % _cols) * cw) / cw));
            lv = std::min(1.0f, std::max(0.0f, (vx.TexCoord.tv - (c / _cols) * ch) / ch));
        }
        for (uint32_t t = 0; t < _ticks; ++t)
        {
            const Frame *f = it != _groups.end() ? FrameAt(it->second, (int32_t)t * _tick) : NULL;
            if (!f)
            {
                _outlines[t][j] = vx.TexCoord; // no sequence: static
                continue;
            }
            float u = f->mirU ? 1.0f - lu : lu;
            float v = f->mirV ? 1.0f - lv : lv;
            _outlines[t][j] = tUtV((f->cell % _cols + u) * cw, (f->cell / _cols + v) * ch);
        }
        vx.TexCoordId = j;
    }
}

std::vector<tUtV> &ReforgeFlipbook::GetOutline(int frameid)
{
    static std::vector<tUtV> none;
    if (_outlines.empty())
        return none;
    return _outlines.at(frameid < 0 ? _cur : frameid);
}

// Same clock as NC_STACK_bmpanim::SetTime (looping, animType 0): one advance
// per new timeStamp however many meshes / instances draw it this frame.
void ReforgeFlipbook::SetTime(int32_t timeStamp, int32_t frameTime)
{
    if (frameTime == -1)
    {
        _cur = (_cur + 1) % _ticks;
        return;
    }
    if (timeStamp == _timeStamp)
        return;
    _timeStamp = timeStamp;
    _leftTime += frameTime;
    if (_leftTime >= _tick)
    {
        _cur = (uint32_t)((_cur + _leftTime / _tick) % _ticks);
        _leftTime %= _tick;
    }
}

// Reforge (arc effects): the additive (lumtracy, MTL d < 1) maps of hd
// models get the same texel treatment the engine gives the original lumtracy
// palette pages in GFXEngine::ConvAlphaPalette. With the shipped NUCLEUS.INI
// (gfx.blending = 0) the lumtracy pass is SRC_ALPHA / ONE_MINUS_SRC_ALPHA, not
// ONE / ONE, so an opaque RGB sprite drew its black background as a black
// sheet (Mykonian arc sheets, flash quads). Emission e = rgb * a / 255:
//   src-blend modes: a' = max(e), rgb' = e * 255 / a' (max <= 8 -> clear)
//   additive (gfx.blending = 1): rgb' = e, a' = 255 (alpha unused by ONE/ONE)
// Idempotent: an RGBA sprite already in luminance-as-alpha form is unchanged.
// UA_LUMTRACY_CONV=0 restores the plain upload (before/after checks).
static bool LumTracyConvOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("UA_LUMTRACY_CONV");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on == 1;
}

static SDL_Surface *LumTracyPixels(SDL_Surface *src)
{
    SDL_Surface *s = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ABGR8888, 0); // bytes R,G,B,A
    if (!s)
        return src;
    SDL_FreeSurface(src);
    const bool lumAlpha = !GFX::GFXEngine::can_destblend && GFX::GFXEngine::can_srcblend;
    SDL_LockSurface(s);
    for (int y = 0; y < s->h; ++y)
    {
        uint8_t *p = (uint8_t *)s->pixels + y * s->pitch;
        for (int x = 0; x < s->w; ++x, p += 4)
        {
            int e[3];
            int mx = 0;
            for (int c = 0; c < 3; ++c)
            {
                e[c] = (p[c] * p[3] + 127) / 255;
                mx = std::max(mx, e[c]);
            }
            if (!lumAlpha)
            {
                p[0] = e[0]; p[1] = e[1]; p[2] = e[2]; p[3] = 255;
            }
            else if (mx <= 8)
            {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
            else
            {
                for (int c = 0; c < 3; ++c)
                    p[c] = (uint8_t)std::min(255, (e[c] * 255 + mx / 2) / mx);
                p[3] = (uint8_t)mx;
            }
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

ResBitmap *NC_STACK_Obj3D::LoadMap(const std::string &mtlDir, const std::string &name, bool srgb, bool lumTracy)
{
    if (name.empty())
        return NULL;

    lumTracy = lumTracy && LumTracyConvOn();
    SDL_Surface *surf = NULL;
    std::string path = UA_ABSPATH(name) ? name : mtlDir + name;
    const bool onDisk = !path.empty() && UA_ABSPATH(path);
    const std::string cacheKey = path + (srgb ? "|srgb" : "|lin") + (lumTracy ? "|lum" : "");

    if (onDisk && MapCacheOn())
    {
        auto it = gMapCache.find(cacheKey);
        if (it != gMapCache.end())
        {
            it->second.refs++;
            GFX::GlowPass::TagBitmap(it->second.bm, path); // glow turned on at runtime: re-tag cached maps (no-op at level 0)
            return it->second.bm;
        }
    }

    std::string loadedFrom = path; // atmos night windows decode the trim's .dds only when it is what went up
    auto remember = [&](ResBitmap *b) -> ResBitmap *
    {
        if (b && onDisk && MapCacheOn())
        {
            gMapCache[cacheKey] = SharedMap{b, 1, loadedFrom};
            gMapKey[b] = cacheKey;
        }
        if (b)
        {
            GFX::GlowPass::TagBitmap(b, path); // reforge glow: exclusion tag by file name, no-op at level 0
            if (onDisk && !MapCacheOn())
                gMapPathLive[b] = loadedFrom; // UA_TEXCACHE=0 (dev measurement switch) only: atmos still finds the map
        }
        return b;
    };

    if (onDisk && TexCompOn() && !lumTracy) // a compressed map cannot be re-keyed: lumtracy maps load the PNG
    {
        size_t slash = path.rfind('/');
        size_t dot = path.rfind('.');
        std::string stemPath = (dot != std::string::npos && dot > slash) ? path.substr(0, dot) : path;
        std::string dds = stemPath + ".dds";
        struct stat st;
        // Quest data set: ASTC in KTX beside the map (absent on desktop data).
        std::string ktx = stemPath + ".ktx";
        if (stat(ktx.c_str(), &st) == 0)
        {
            if (ResBitmap *b = GFX::Engine.LoadKtxTexture(ktx, srgb))
            {
                printf("obj3d: '%s' <- compressed '%s'\n", path.c_str(), ktx.c_str());
                return remember(b);
            }
        }
        if (stat(dds.c_str(), &st) == 0)
        {
            if (ResBitmap *b = GFX::Engine.LoadDdsTexture(dds, srgb))
            {
                printf("obj3d: '%s' <- compressed '%s'\n", path.c_str(), dds.c_str());
                loadedFrom = dds;
                return remember(b);
            }
        }
    }

    Perf::Scope perfDec(Perf::DEC);
    if (onDisk)
        surf = IMG_Load(path.c_str());
    else
    {
        FSMgr::FileHandle fil = uaOpenFile(path, "rb");
        if (fil.OK())
        {
            std::vector<uint8_t> buf;
            uint8_t chunk[65536];
            size_t n;
            while ((n = fil.read(chunk, sizeof(chunk))) > 0)
                buf.insert(buf.end(), chunk, chunk + n);
            if (!buf.empty())
                surf = IMG_Load_RW(SDL_RWFromConstMem(buf.data(), buf.size()), 1);
        }
    }

    if (!surf)
    {
        printf("obj3d: material map '%s' not loaded\n", path.c_str());
        g_statsMaterialMapsMissing++; // anonymous usage stats: counted, never named
        return NULL;
    }

    // Authored mip chain (reforge far-read): <stem>_mip1<ext>, <stem>_mip2<ext>,
    // ... beside the map on disk, consecutive from 1, level i sized exactly
    // max(1, w >> i) x max(1, h >> i). The first missing or mis-sized level
    // ends the chain; the levels below the last authored one are generated
    // from it. No _mip1 file = the plain upload (identical to before).
    std::vector<SDL_Surface *> mips;
    if (UA_ABSPATH(path))
    {
        size_t slash = path.rfind('/');
        size_t dot = path.rfind('.');
        std::string stem = (dot != std::string::npos && dot > slash) ? path.substr(0, dot) : path;
        std::string ext = (dot != std::string::npos && dot > slash) ? path.substr(dot) : std::string();
        for (int lv = 1; (surf->w >> (lv - 1)) > 1 || (surf->h >> (lv - 1)) > 1; ++lv)
        {
            std::string mp = stem + "_mip" + std::to_string(lv) + ext;
            FILE *probe = fopen(mp.c_str(), "rb");
            if (!probe)
                break;
            fclose(probe);
            SDL_Surface *ms = IMG_Load(mp.c_str());
            int ew = std::max(1, surf->w >> lv), eh = std::max(1, surf->h >> lv);
            if (!ms || ms->w != ew || ms->h != eh)
            {
                printf("obj3d: mip '%s' %s (want %dx%d), chain ends at level %d\n", mp.c_str(),
                       ms ? "has the wrong size" : "not loaded", ew, eh, lv - 1);
                if (ms)
                    SDL_FreeSurface(ms);
                break;
            }
            mips.push_back(ms);
        }
        if (!mips.empty())
            printf("obj3d: '%s' + %d authored mip levels\n", path.c_str(), (int)mips.size());
    }

    if (lumTracy)
    {
        surf = LumTracyPixels(surf);
        for (SDL_Surface *&ms : mips)
            ms = LumTracyPixels(ms);
        printf("obj3d: '%s' lumtracy texels (%s)\n", path.c_str(),
               (!GFX::GFXEngine::can_destblend && GFX::GFXEngine::can_srcblend) ? "luminance-as-alpha" : "premultiplied");
    }

    perfDec.Stop(); // decode ends here; the upload is timed as TEX
    ResBitmap *bm = GFX::Engine.LoadRawTexture(surf, mips.empty() ? NULL : &mips, srgb);
    for (SDL_Surface *ms : mips)
        SDL_FreeSurface(ms);
    SDL_FreeSurface(surf);
    return remember(bm);
}

// Per-vertex normal (from vn, else the face plane) and tangent frame for
// meshes that carry reforge material maps. MikkTSpace-style: per-triangle
// UV derivatives, weighted by the corner angle, projected into the normal's
// plane and summed over every face corner sharing the same (v, vt, vn) - the
// same grouping Blender's MikkTSpace uses when it bakes. Handedness is the
// sign of the accumulated OpenGL bitangent (+V of the OBJ texcoords, i.e.
// the green channel) against cross(N, T). Not bit-exact with Mikk (no
// degenerate-triangle recovery), which only matters on smooth, heavily
// distorted UVs.
void NC_STACK_Obj3D::BuildShadingFrames()
{
    struct Acc { vec3f T; vec3f B; };

    for (GFX::TMesh &msh : Meshes)
    {
        if (!msh.Mat.Pbr)
            continue;

        auto cit = _corners.find(&msh);
        if (cit == _corners.end() || cit->second.size() != msh.Vertexes.size())
            continue;
        const std::vector<std::array<int32_t, 3>> &corners = cit->second;

        // Hash map (was std::map): per-key accumulation order is unchanged, so
        // the result is bit-identical; ~40 % of a 7.5k-tri OBJ load was spent
        // in std::map<std::array> compares (seen in a profile).
        struct CornerHash
        {
            size_t operator()(const std::array<int32_t, 3> &c) const
            {
                uint64_t h = (uint32_t)c[0];
                h = h * 0x9E3779B97F4A7C15ULL ^ (uint32_t)c[1];
                h = h * 0x9E3779B97F4A7C15ULL ^ (uint32_t)c[2];
                return (size_t)(h ^ (h >> 29));
            }
        };
        std::unordered_map<std::array<int32_t, 3>, Acc, CornerHash> acc;
        acc.reserve(msh.Vertexes.size());
        std::vector<GFX::TVertex> &vx = msh.Vertexes;

        for (size_t i = 0; i + 2 < msh.Indixes.size(); i += 3)
        {
            size_t id[3] = {msh.Indixes[i], msh.Indixes[i + 1], msh.Indixes[i + 2]};
            vec3f p[3];
            float u[3], v[3];
            for (int k = 0; k < 3; ++k)
            {
                p[k] = vx[id[k]].Pos;
                u[k] = vx[id[k]].TexCoord.tu;
                v[k] = -vx[id[k]].TexCoord.tv; // engine V is flipped; GL +V up
            }

            vec3f e1 = p[1] - p[0], e2 = p[2] - p[0];
            vec3f fn = e1 * e2; // cross; engine index order faces outward
            if (fn.length() > 0.0)
                fn /= fn.length();

            for (int k = 0; k < 3; ++k)
                if (corners[id[k]][2] <= 0)
                    vx[id[k]].Normal = fn;

            float du1 = u[1] - u[0], dv1 = v[1] - v[0];
            float du2 = u[2] - u[0], dv2 = v[2] - v[0];
            float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-12f)
                continue;
            float r = 1.0f / det;
            vec3f T = (e1 * dv2 - e2 * dv1) * r;
            vec3f B = (e2 * du1 - e1 * du2) * r;

            for (int k = 0; k < 3; ++k)
            {
                vec3f a = p[(k + 1) % 3] - p[k], b = p[(k + 2) % 3] - p[k];
                float la = a.length(), lb = b.length();
                if (la <= 0.0 || lb <= 0.0)
                    continue;
                float c = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
                float w = std::acos(std::max(-1.0f, std::min(1.0f, c)));
                const vec3f &N = vx[id[k]].Normal;
                vec3f Tp = T - N * (N.x * T.x + N.y * T.y + N.z * T.z);
                Acc &ac = acc[corners[id[k]]];
                ac.T += Tp * w;
                ac.B += B * w;
            }
        }

        for (size_t i = 0; i < vx.size(); ++i)
        {
            GFX::TVertex &vt = vx[i];
            vec3f N = vt.Normal;
            if (N.length() > 0.0)
                N /= N.length();
            vt.Normal = N;

            vec3f T, B;
            auto it = acc.find(corners[i]);
            if (it != acc.end())
            {
                T = it->second.T;
                B = it->second.B;
            }
            T = T - N * (N.x * T.x + N.y * T.y + N.z * T.z);
            if (T.length() < 1e-8)
            {
                // No usable UVs: any perpendicular keeps the shader finite.
                T = (std::fabs(N.x) < 0.9) ? vec3f(1, 0, 0) : vec3f(0, 1, 0);
                T = T - N * (N.x * T.x + N.y * T.y + N.z * T.z);
            }
            T /= T.length();
            vec3f NxT = N * T;
            float hand = (NxT.x * B.x + NxT.y * B.y + NxT.z * B.z) < 0.0 ? -1.0f : 1.0f;
            vt.Tangent[0] = T.x;
            vt.Tangent[1] = T.y;
            vt.Tangent[2] = T.z;
            vt.Tangent[3] = hand;
        }
    }
}

GFX::TRenderParams NC_STACK_Obj3D::Mtl::GenParams() const
{
    GFX::TRenderParams tmp;
    tmp.Flags = GFX::RFLAGS_SHADED | GFX::RFLAGS_FOG;

    if (hasPbr)
        tmp.Pbr = &pbr;

    if (rawDiffuse)
    {
        tmp.Flags |= GFX::RFLAGS_TEXTURED;
        tmp.Tex = rawDiffuse;
        if (flip)
        {
            tmp.Flags |= GFX::RFLAGS_DYNAMIC_TEXTURE;
            tmp.TexSource = flip;
        }
    }
    else if (diffuseMap)
    {
        tmp.Flags |= GFX::RFLAGS_TEXTURED;
        
        if (diffuseMap->IsDynamic())
        {
            tmp.Flags |= GFX::RFLAGS_DYNAMIC_TEXTURE;
            tmp.TexSource = diffuseMap;
        }
        else
            tmp.Tex = diffuseMap->GetBitmap();
    }
    
    tmp.Color = GFX::TGLColor(diffuse[0], diffuse[1], diffuse[2], d);
    
    if (d < 1.0)
    {
        tmp.Flags |= GFX::RFLAGS_LUMTRACY;
        // Same strength as the original lumtracy faces (amesh/area: alpha =
        // GFX::Engine.GetAlpha(), 192/255 with src blending); d only lowers it.
        if (LumTracyConvOn())
            tmp.Color.a = std::min((float)d, (float)GFX::Engine.GetAlpha());
    }
    
    return tmp;
}


bool NC_STACK_Obj3D::ExportObj(NC_STACK_base *base, const std::string &fname)
{
    if (!base)
        return false;
    
    if (base->Meshes.empty())
        return false;
    
    FSMgr::FileHandle mtl = uaOpenFile(fname + ".mtl", "w");
    
    
    FSMgr::FileHandle obj = uaOpenFile(fname, "w");
    
    obj.puts(std::string("mtllib ") + fname + ".mtl\n\n");
    
    int vid = 1;
    int tvid = 1;
    
    int matID = 0;
    for( GFX::TMesh &m : base->Meshes )
    {
        mtl.printf("newmtl Mat%d\n", matID);
        mtl.printf("Kd %f %f %f\n", m.Mat.Color.r, m.Mat.Color.g, m.Mat.Color.b);
        mtl.puts("\
Ka 1.000000 1.000000 1.000000\n\
Ks 0.500000 0.500000 0.500000\n\
Ke 0.000000 0.000000 0.000000\n\
Ni 1.450000\n");
        mtl.printf("d %f\n", m.Mat.Color.a);
        mtl.puts("illum 2\n");
        
        if (m.Mat.TexSource || m.Mat.Tex)
        {
            std::string texName = fmt::sprintf("Tex%d.png", matID);
            if (m.Mat.Tex)
                NC_STACK_image::SavePng(m.Mat.Tex, texName);
            else
                NC_STACK_image::SavePng(m.Mat.TexSource->GetBitmap(), texName);
            
            mtl.printf("map_Kd %s\n\n", texName.c_str());
        }
        else
            mtl.puts("\n");
        
        for(GFX::TVertex &v : m.Vertexes)
            obj.printf("v %f %f %f\n", v.Pos.x, -v.Pos.y, v.Pos.z);
        
        if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
        {
            for(GFX::TVertex &v : m.Vertexes)
                obj.printf("vt %f %f\n", v.TexCoord.tu, 1.0 - v.TexCoord.tv);
        }
        
        obj.printf("usemtl Mat%d\n", matID);
        
        for (uint32_t i = 0; i < m.Indixes.size(); i += 3)
        {
            if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
                obj.printf("f %d/%d %d/%d %d/%d\n", vid + m.Indixes[i], tvid + m.Indixes[i],
                        vid + m.Indixes[i + 2], tvid + m.Indixes[i + 2],
                        vid + m.Indixes[i + 1], tvid + m.Indixes[i + 1]);
            else
                obj.printf("f %d %d %d\n", vid + m.Indixes[i],
                        vid + m.Indixes[i + 2],
                        vid + m.Indixes[i + 1]);
        }
        
        vid += m.Vertexes.size();  
        if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
            tvid += m.Vertexes.size();  
        ++matID;
    }
    return true;
}