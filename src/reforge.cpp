// Reforge Visuals — engine-side tooling (loaders / visproto swap / viewer / export).
//
// Everything in this file is inert unless explicitly enabled:
//
// hd-models override (on when the directory exists)
//   <cwd>/hd-models/<visproto>.obj      e.g. hd-models/VP_PANZ1.obj
//   UA_HDMODELS_DIR=<disk dir>          use this directory instead (tests)
//   Every set load (load_set_base) walks the visproto kids in visproto.lst order;
//   when <dir>/<name>.obj exists that kid is replaced by the OBJ (LoadObj3DDisk; mtl + textures resolve beside it),
//   inheriting the original's root transform / ambient light. Works for both
//   the combined set.base and the per-file visproto.lst load paths. Each
//   substitution is logged once. No directory = no code path runs.
//   OBJ coordinates are the ones written by the exporter below: x, -y(up), z,
//   UA world units, origin = the visproto's own pivot.
//
// hd lego override (buildings / sector pieces; on when <dir>/lego/set<N>/ exists)
//   <dir>/lego/set<N>/<NAME>.obj        NAME = set.sdf section-1 base name without
//                                       ".base" (e.g. lego/set1/ST_CITY1.obj); every
//                                       lego slot with that name in set N is replaced.
//   <dir>/lego/set<N>/id<NNN>.obj       (optional) one slot by index, wins over NAME.
//   Same loader as the visproto override (LoadObj3DDisk: mtl, maps and .dds
//   beside the .obj), same root transform / ambient inheritance. Visual only:
//   the collision skeleton, shield and explosion list stay from set.sdf. Damage
//   stages are separate legos (ST_ / STx / STy / STz), so one file each. Lego
//   geometry is per set, hence the per-set directory.
//
// Reference export (exits when done)
//   UA_EXPORT_VISPROTO=VP_PANZ1,PANZ1,#22   names, family (matches VP?PANZ1), #lst-index
//   UA_EXPORT_LEGO=ST_CITY1,#4,all      lego pieces by set.sdf name / #index / all.
//                                       EVERY set of UA_EXPORT_SETS is written, to
//                                       <out>/set<N>/<NAME>.obj + <out>/set<N>/legos.json
//                                       (sdf line: skeleton, shield, explosion points)
//   UA_EXPORT_OUT=<disk dir>            default <cwd>/reforge-export
//   UA_EXPORT_SETS=1,2,3,4,5,6          first set is exported; all are fingerprinted
//   Writes <out>/<name>.obj/.mtl, <out>/tex/<rsrc>.png (ORIGINAL software
//   textures — ResBitmap::swTex — never the hd-textures GL override) and
//   <out>/export.json (bboxes, hierarchy, flags, per-set fingerprints).
//   The hd-models override is disabled while exporting.
//
// Turntable viewer (exits when done)
//   UA_VIEWER=VP_PANZ1[,VPfPANZ1...]    same selector syntax as the exporter
//   UA_VIEWER_LEGO=ST_CITY1[,#4...]     lego pieces of UA_VIEWER_SET (with the hd
//                                       lego override when present); output
//                                       files are named L<set>_<NAME>_*.png
//   UA_VIEWER_OUT=<disk dir>            default <cwd>/reforge-viewer
//   UA_VIEWER_SET=1                     set to load
//   UA_VIEWER_ANGLES=8                  yaw steps (plus one 3/4 top-down "rts" shot)
//   UA_VIEWER_BG=64,64,64               clear colour
//   UA_VIEWER_FAR=1 / UA_VIEWER_PX=64,32 far-read mode (replaces the turntable):
//     the unit is rendered in the full window at the camera distance where its
//     on-screen silhouette spans UA_VIEWER_PX px (max of bbox width/height,
//     mean over the yaws; default 64,32), so the GPU samples the real mip
//     levels for that size (no downscaling of a big render).
//     UA_VIEWER_FAR_PITCH=50              RTS commander pitch (as the "rts" shot)
//     UA_VIEWER_FAR_YAWS=45,135,225,315
//     UA_VIEWER_FAR_DIST=d1,d2            pin the distance per target (else it is
//                                         calibrated on the loaded model; pin the
//                                         ORIGINAL's distances to compare configs)
//     Files per target/yaw: <name>_px064_yaw045.png (full frame on UA_VIEWER_BG),
//     _crop.png (RGBA; alpha from a black/white background matte; fixed square
//     of 1.5 x target centred on the frame), _crop_x4.png (crop over the bg,
//     nearest-neighbour x4), plus <name>_far.json (distances, spans, bboxes).
//   UA_VIEWER_ANIM=<n>,<ms>            animation strip after the turntable:
//     n frames of the game clock advanced <ms> per frame (bmpanim / ua_flipbook
//     textures move; the turntable itself stays at clock 0), views "rts" and
//     "yaw090" -> <name>_anim_<view>_d<k>_f<NNN>.png
//     UA_VIEWER_ANIM_DIST=d1,d2           camera distances (default / 0 = 2 x radius)
//   Camera distance comes from the ORIGINAL model's bounding sphere, so an
//   hd-models replacement renders at the same scale/framing as the original.
//   Renders go through the normal engine path (base->Render + Rasterize), read
//   back from our own framebuffer (no screen capture). Use UA_WINDOW_SIZE for
//   a fixed image size and UA_WINDOW_HIDDEN=1 for an unattended run.

#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "system/glapi.h"

#include "includes.h"
#include "reforge.h"
#include "reforge_fx.h"
#include "gfxsettings.h"
#include "base.h"
#include "area.h"
#include "bitmap.h"
#include "loaders.h"
#include "obj3d.h"
#include "env.h"
#include "yw_internal.h"
#include "system/fsmgr.h"
#include "system/gfx.h"
#include "system/system.h"
#include "fmtlib/printf.h"
#ifdef _WIN32
#include "winport.h"
#endif

namespace Reforge
{

namespace
{

bool gOverrideSuspended = false;

// Original geometry bounding sphere (radius around the pivot) per visproto
// name, recorded before an hd-models substitution, so the viewer frames a
// replacement exactly like the original.
std::map<std::string, double> gOrigRadius;

// Lego far LOD: near (hd) lego -> far mesh, rendered beyond LegoLodDist() from
// the camera. Far = <name>.far.obj beside the hd lego, or (UA_PERF_LEGOLOD
// only) the original lego kept alive for perf comparison.
std::map<NC_STACK_base *, NC_STACK_base *> gLegoFar;
std::set<NC_STACK_base *> gLegoFarOwned;   // far meshes we loaded (not originals)
std::set<NC_STACK_base *> gLegoFarPrepped;

const char *Env(const char *k)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : NULL;
}

// UA_GFX_BUILDINGS (default 2 = the current New Look behaviour):
//   0 = original legos, the hd-lego override never runs (no files read, no
//       textures, no far meshes; draw path identical to a build without it)
//   1 = hd legos, far LOD from 900 u   2 = far LOD from 1700 u
//   3 = far LOD from 3000 u
// UA_LEGO_LODDIST / UA_PERF_LEGOLOD still override the distance (tools).
// Read through the settings helper (preset row / override / UA_GFX_BUILDINGS); no preset in
// force = 2 (the published New Look). Live (the row reloads the set), logged when it changes.
int BuildingsLevel()
{
    static int last = -1;
    static uint32_t gen = 0;
    if (last < 0 || gen != GfxSettings::Generation()) // cached: LegoLod asks per cell per frame
    {
        gen = GfxSettings::Generation();
        const int lv = ::gfxLevel("BUILDINGS", 2);
        if (lv != last)
            printf("[REFORGE] gfx: BUILDINGS level %d\n", lv);
        last = lv;
    }
    return last;
}

bool IsDir(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void MkDirs(const std::string &p)
{
    std::string cur;
    for (size_t i = 0; i < p.size(); ++i)
    {
        cur += p[i];
        if ((p[i] == '/' && i > 0) || i + 1 == p.size())
            mkdir(cur.c_str(), 0755);
    }
}

std::vector<std::string> Split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size())
    {
        size_t c = s.find(sep, at);
        std::string t = s.substr(at, c == std::string::npos ? std::string::npos : c - at);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(0, 1);
        if (!t.empty())
            out.push_back(t);
        if (c == std::string::npos)
            break;
        at = c + 1;
    }
    return out;
}

std::string Upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    return s;
}

std::string Sanitize(const std::string &name)
{
    std::string out;
    for (char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out += c;
        else if (c >= 'A' && c <= 'Z')
            out += (char)(c - 'A' + 'a');
        else
            out += '_';
    }
    return out.empty() ? std::string("unnamed") : out;
}

// visproto.lst of the current rsrc: set, same parsing as sub_44AD8C. Names
// lose their ".base" suffix; index = kid index of the visproto root.
std::vector<std::string> ReadVisprotoNames()
{
    std::vector<std::string> names;
    FSMgr::FileHandle *fil = uaOpenFileAlloc("rsrc:scripts/visproto.lst", "r");
    if (!fil)
        return names;

    std::string line;
    while (fil->ReadLine(&line) && !line.empty() && line[0] != '>')
    {
        size_t pos = line.find_first_of(" ;#\n\r\t");
        if (pos != std::string::npos)
            line.erase(pos);
        if (line.empty())
            continue;
        size_t dot = line.rfind('.');
        if (dot != std::string::npos)
            line.erase(dot);
        names.push_back(line);
    }
    delete fil;
    return names;
}

// set.sdf section 1 of the current rsrc: set, parsed like yw_parse_lego: one
// entry per line that has a token (comments ; # stripped), until '>'.
// Entry index = kid index in the lego group = _legoArray id.
struct LegoLine
{
    std::string Name;               // base name without ".base"
    std::vector<std::string> Tok;   // all tokens of the line
};

std::vector<LegoLine> ReadLegoLines()
{
    std::vector<LegoLine> out;
    FSMgr::FileHandle *fil = uaOpenFileAlloc("rsrc:scripts/set.sdf", "r");
    if (!fil)
        return out;

    std::string line;
    while (fil->ReadLine(&line) && !line.empty() && line[0] != '>' && out.size() < 256)
    {
        size_t pos = line.find_first_of(";#\n\r");
        if (pos != std::string::npos)
            line.erase(pos);
        LegoLine l;
        std::string cur;
        for (size_t i = 0; i <= line.size(); ++i)
        {
            char c = i < line.size() ? line[i] : ' ';
            if (c == ' ' || c == '\t')
            {
                if (!cur.empty())
                    l.Tok.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (l.Tok.empty())
            continue;
        l.Name = l.Tok[0];
        size_t dot = l.Name.rfind('.');
        if (dot != std::string::npos)
            l.Name.erase(dot);
        out.push_back(l);
    }
    delete fil;
    return out;
}

std::vector<std::string> LegoNames(const std::vector<LegoLine> &lines)
{
    std::vector<std::string> n;
    for (const LegoLine &l : lines)
        n.push_back(l.Name);
    return n;
}

// N of the current "data:set<N>" rsrc prefix, 0 when not a set.
int CurrentSetId()
{
    std::string p = Common::Env.GetPrefix("rsrc");
    size_t i = p.size();
    while (i > 0 && isdigit((unsigned char)p[i - 1]))
        --i;
    return i < p.size() ? atoi(p.c_str() + i) : 0;
}

// Selector: "VP_PANZ1" exact name, "PANZ1" family (VP?PANZ1), "#22" index.
std::vector<int> ResolveSelectors(const std::string &spec, const std::vector<std::string> &names)
{
    std::vector<int> ids;
    std::set<int> seen;
    auto add = [&](int i) { if (seen.insert(i).second) ids.push_back(i); };

    for (const std::string &tok : Split(spec, ','))
    {
        if (tok[0] == '#')
        {
            int i = atoi(tok.c_str() + 1);
            if (i >= 0 && i < (int)names.size())
                add(i);
            continue;
        }
        std::string t = Upper(tok);
        bool found = false;
        for (size_t i = 0; i < names.size(); ++i)
            if (Upper(names[i]) == t) { add(i); found = true; }
        if (found)
            continue;
        for (size_t i = 0; i < names.size(); ++i)
        {
            std::string n = Upper(names[i]);
            if (n.size() == t.size() + 3 && n.compare(0, 2, "VP") == 0 && n.compare(3, std::string::npos, t) == 0)
                add(i);
        }
    }
    return ids;
}

NC_STACK_base *KidAt(NC_STACK_base *root, int idx)
{
    int i = 0;
    for (NC_STACK_base *k : root->GetKidList())
        if (i++ == idx)
            return k;
    return NULL;
}

// Flattened triangle soup of a visproto (root local space, kids' local
// transforms applied), plus which meshes they came from.
struct FlatMesh
{
    const GFX::TMesh *Mesh;
    mat3x3 Rot;
    vec3d Off;
    int Depth;
};

void Flatten(NC_STACK_base *b, const mat3x3 &rot, const vec3d &off, int depth, std::vector<FlatMesh> *out)
{
    for (const GFX::TMesh &m : b->Meshes)
        out->push_back({&m, rot, off, depth});

    for (NC_STACK_base *k : b->GetKidList())
    {
        TF::TForm3D &t = k->TForm();
        mat3x3 r = rot * t.SclRot;
        vec3d o = off + rot.Transform(t.Pos);
        Flatten(k, r, o, depth + 1, out);
    }
}

double BoundRadius(NC_STACK_base *vp)
{
    std::vector<FlatMesh> fm;
    Flatten(vp, mat3x3::Ident(), vec3d(0, 0, 0), 0, &fm);
    double r = 0.0;
    for (const FlatMesh &f : fm)
        for (const GFX::TVertex &v : f.Mesh->Vertexes)
        {
            double l = (f.Rot.Transform(v.Pos) + f.Off).length();
            if (l > r) r = l;
        }
    return r;
}

// ---------------------------------------------------------------------------
// hd-models override
// ---------------------------------------------------------------------------

// Performance stress aids (inert unless set):
//   UA_HDMODELS_ALIAS=VP_FLAK1=VP_PANZ1,VPwFLAK1=VPwPANZ1
//       explicit per-visproto substitution; the value is a file stem inside the
//       hd-models dir (may contain a subdir, e.g. c3/VP_PANZ1).
//   UA_HDMODELS_ALL=VP_PANZ1[:N]
//       every vehicle-style visproto VP<slot><family> (slot _ f w g) loads
//       VP<slot>PANZ1.obj instead, when that file exists - the worst case
//       "every unit type is a reforged unit". With :N the k-th family (in
//       visproto order) loads from the subdir c<k % N>/, so N sets of texture
//       copies stand in for N distinct unit types' memory.
std::string HdStem(const std::string &name)
{
    static bool parsed = false;
    static std::map<std::string, std::string> alias;
    static std::string allFam;
    static int allN = 0;
    static std::map<std::string, int> famIdx;
    if (!parsed)
    {
        parsed = true;
        if (const char *v = Env("UA_HDMODELS_ALIAS"))
        {
            std::string list(v);
            size_t at = 0;
            while (at < list.size())
            {
                size_t comma = list.find(',', at);
                std::string tok = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                size_t eq = tok.find('=');
                if (eq != std::string::npos && eq > 0 && eq + 1 < tok.size())
                    alias[tok.substr(0, eq)] = tok.substr(eq + 1);
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
        }
        if (const char *v = Env("UA_HDMODELS_ALL"))
        {
            std::string spec(v);
            size_t colon = spec.find(':');
            if (colon != std::string::npos)
            {
                allN = atoi(spec.c_str() + colon + 1);
                spec.erase(colon);
            }
            if (spec.size() > 3)
                allFam = spec.substr(3);
        }
    }

    auto a = alias.find(name);
    if (a != alias.end())
        return a->second;

    if (!allFam.empty() && name.size() > 3 && name[0] == 'V' && name[1] == 'P')
    {
        char slot = (char)tolower((unsigned char)name[2]);
        if (slot == '_' || slot == 'f' || slot == 'w' || slot == 'g')
        {
            std::string stem = std::string("VP") + slot + allFam;
            if (allN > 0)
            {
                std::string fam = name.substr(3);
                auto f = famIdx.find(fam);
                int k;
                if (f == famIdx.end())
                {
                    k = (int)famIdx.size();
                    famIdx[fam] = k;
                }
                else
                    k = f->second;
                stem = "c" + std::to_string(k % allN) + "/" + stem;
            }
            return stem;
        }
    }
    return name;
}

std::string HdModelsDir()
{
    if (const char *d = Env("UA_HDMODELS_DIR"))
        return d;
    return "hd-models";
}

} // namespace

namespace
{
// Lazy hd-models. The eager path loaded every hd state of
// the set at set load (+562 MB textures, +8.4 s on L7575) whether the level used
// it or not. Lazy: the set load only records which visproto slots have an hd file;
// at level load (HdPreloadLevel, before any unit exists) the slots the level can
// show are loaded: every vehicle type a host in the level may field or build
// (enable lists), the level's squads/robos/buddies, gem upgrades, building and
// host guns, plus each type's states, weapons, destruction / scale fx (closure).
// A slot first needed later (HdEnsure, at unit/weapon creation) is loaded then and
// logged "hd-models: LATE" - the VR build counts them (target 0). Loaded slots stay
// with the set (the set is cached across levels of the same set id).
// UA_HD_LAZY=0 = the eager path; tool modes (viewer/export) are always eager.
struct HdSlots
{
    NC_STACK_base *root = NULL;
    std::vector<std::string> names;
    std::vector<std::string> disk;      // hd file per slot ("" = none)
    std::vector<uint8_t> loaded;
    std::vector<NC_STACK_base *> graveyard; // swapped-out originals (a live unit may still hold one)
    int late = 0;
} gHd;

bool HdLazyOn()
{
    if (Env("UA_VIEWER") || Env("UA_VIEWER_LEGO") || Env("UA_EXPORT_VISPROTO") || Env("UA_EXPORT_LEGO"))
        return false;
    const char *v = Env("UA_HD_LAZY");
    return !(v && v[0] == '0');
}

double gHdObjMs = 0.0;
int gHdObjN = 0;

// Load slot idx's hd file and put it in the kid list in place of the original.
// Returns the new base, or NULL (original kept).
NC_STACK_base *HdSwapSlot(NC_STACK_base *root, BaseList::iterator it, int idx, const std::string &name,
                          const std::string &disk, std::vector<NC_STACK_base *> *keepOld)
{
    NC_STACK_base *old = *it;
    // Load straight from disk: mtllib and texture maps resolve beside the
    // .obj (a VFS path would send the textures through the rsrc lookup
    // and they would come back missing -> flat white).
    uint64_t perfT0 = SDL_GetPerformanceCounter();
    NC_STACK_base *nb = NC_STACK_Obj3D::LoadObj3DDisk(disk);
    gHdObjMs += (SDL_GetPerformanceCounter() - perfT0) * 1000.0 / SDL_GetPerformanceFrequency();
    if ( Env("UA_PERF_LOG") && (++gHdObjN <= 4 || (gHdObjN % 25) == 0) )
        printf("perf: hd-models %d obj loads, %.0f ms total (incl. maps)\n", gHdObjN, gHdObjMs);
    if (!nb)
    {
        ypa_log_out("hd-models: FAILED to load %s, keeping original\n", disk.c_str());
        return NULL;
    }

    if (!gOrigRadius.count(name))
        gOrigRadius[name] = BoundRadius(old);

    // Same slot, same parent, same root transform as the original.
    TF::TForm3D *parentTf = old->TForm().Parent;
    nb->TForm() = old->TForm();
    nb->TForm().Parent = parentTf;
    nb->SetAmbientLight(old->GetAmbientLight());
    nb->_parent = root;
    *it = nb;

    old->_parent = NULL;
    if (keepOld)
        keepOld->push_back(old);
    else
        old->Delete();

    static std::set<std::string> logged;
    if (logged.insert(name).second)
        ypa_log_out("hd-models: %s (#%d) <- %s\n", name.c_str(), idx, disk.c_str());
    return nb;
}

// Lazy: load slot idx now (level load or late) and point the world's model table at it.
bool HdLoadSlot(NC_STACK_ypaworld *yw, int idx)
{
    if (!gHd.root || idx < 0 || idx >= (int)gHd.disk.size() || gHd.disk[idx].empty() || gHd.loaded[idx])
        return false;
    gHd.loaded[idx] = 1;
    BaseList &kids = gHd.root->GetKidList();
    auto it = kids.begin();
    for (int i = 0; i < idx && it != kids.end(); ++i)
        ++it;
    if (it == kids.end())
        return false;
    NC_STACK_base *old = *it;
    NC_STACK_base *nb = HdSwapSlot(gHd.root, it, idx, gHd.names[idx], gHd.disk[idx], &gHd.graveyard);
    if (!nb)
        return false;
    // What sub_44A12C / the set's MakeVBO did for the original at set load.
    nb->SetVizLimit(old->GetVisLimit());
    nb->SetFadeLength(old->GetFadeLength());
    nb->MakeVBO();
    if (yw && idx < (int)yw->_vhclModels.size() && yw->_vhclModels[idx] == old)
        yw->_vhclModels[idx] = nb;
    return true;
}
} // namespace

void ApplyHdModels(NC_STACK_base *root)
{
    // A new set object: the previous set's swapped-out originals go now (the set
    // and every unit that could hold them are gone).
    for (NC_STACK_base *b : gHd.graveyard)
        b->Delete();
    gHd = HdSlots();

    if (!root || gOverrideSuspended)
        return;

    // Graphics settings: HDMODELS 0 = original unit models (UA_HDMODELS_DIR
    // pointing at a missing dir maps onto it); the fx pack models below still
    // follow the FX level.
    std::string dir = gfxLevel("HDMODELS", 3) > 0 ? HdModelsDir() : std::string("/nonexistent");
    // Effects pack models (<pack>/models/<visproto>.obj: genesis shells,
    // effect models) win over hd-models, only while the pack is on (UA_FX).
    std::string fxDir;
    if (ReforgeFx::On() && IsDir(ReforgeFx::PackDir() + "/models"))
        fxDir = ReforgeFx::PackDir() + "/models";
    if (!IsDir(dir) && fxDir.empty())
        return;

    // LoadObj3DDisk resolves the .mtl and maps beside the .obj only for an
    // absolute path, so canonicalise (the default "hd-models" is cwd-relative).
    if (char *abs = realpath(dir.c_str(), NULL))
    {
        dir = abs;
        free(abs);
    }

    std::vector<std::string> names = ReadVisprotoNames();
    if (names.empty())
        return;

    const bool lazy = HdLazyOn();
    if (lazy)
    {
        gHd.root = root;
        gHd.names = names;
        gHd.disk.assign(names.size(), std::string());
        gHd.loaded.assign(names.size(), 0);
    }

    BaseList &kids = root->GetKidList();
    int idx = 0, avail = 0;
    for (auto it = kids.begin(); it != kids.end(); ++it, ++idx)
    {
        if (idx >= (int)names.size())
            break;

        const std::string &name = names[idx];
        std::string disk = dir + "/" + HdStem(name) + ".obj";
        if (!fxDir.empty() && IsFile(fxDir + "/" + HdStem(name) + ".obj"))
            disk = fxDir + "/" + HdStem(name) + ".obj";
        if (!IsFile(disk))
            continue;
        avail++;

        if (lazy)
            gHd.disk[idx] = disk;
        else
            HdSwapSlot(root, it, idx, name, disk, NULL);
    }
    if (lazy)
        ypa_log_out("hd-models: lazy, %d hd states in this set (loaded per level)\n", avail);
}

void HdPreloadLevel(NC_STACK_ypaworld *yw, const TLevelDescription *mapp)
{
    if (!yw || !gHd.root)
        return;
    const int nv = (int)yw->_vhclProtos.size();
    const int nw = (int)yw->_weaponProtos.size();
    std::vector<uint8_t> vSeen(nv, 0), wSeen(nw, 0);
    std::vector<int> vq, wq;
    auto addV = [&](int id) { if (id >= 0 && id < nv && !vSeen[id]) { vSeen[id] = 1; vq.push_back(id); } };
    auto addW = [&](int id) { if (id >= 0 && id < nw && !wSeen[id]) { wSeen[id] = 1; wq.push_back(id); } };

    // Owners in the level: hosts and squads (enable lists of absent factions don't matter).
    uint32_t owners = 0;
    if (mapp)
    {
        for (const MapRobo &r : mapp->Robos)
        {
            owners |= 1u << (r.Owner & 7);
            addV(r.VhclID);
        }
        for (const MapSquad &s : mapp->Squads)
        {
            owners |= 1u << (s.Owner & 7);
            addV(s.VhclID);
        }
    }
    if (!owners)
        owners = 0xFF;
    for (int i = 0; i < nv; ++i)
        if (yw->_vhclProtos[i].disable_enable_bitmask & owners)
            addV(i);
    for (const TMapBuddy &b : yw->_levelInfo.Buddies)
        addV(b.Type);
    if (yw->_stoudsonWaveVehicleId)
        addV(yw->_stoudsonWaveVehicleId);
    // Tech upgrades ("begin_action / modify_vehicle N / enable = owner ..."): a gem can
    // enable a type mid-level.
    for (const TMapGem &g : yw->_techUpgrades)
        for (const std::string &line : g.ActionsList)
        {
            char key[32] = {0};
            int id = -1;
            if (sscanf(line.c_str(), " %31s %d", key, &id) == 2)
            {
                if (!StriCmp(key, "modify_vehicle"))
                    addV(id);
                else if (!StriCmp(key, "modify_weapon"))
                    addW(id);
            }
        }
    // Building guns (flak/radar statics) of every building proto: a few types.
    for (const World::TBuildingProto &b : yw->_buildProtos)
        for (const World::TBuildingProto::TGun &g : b.Guns)
            addV(g.VhclID);

    std::set<int> vps;
    auto addFx = [&](const std::vector<World::DestFX> &fx) { for (const World::DestFX &f : fx) if (f.ModelID) addV(f.ModelID); };
    while (!vq.empty() || !wq.empty())
    {
        while (!vq.empty())
        {
            const World::TVhclProto &v = yw->_vhclProtos[vq.back()];
            vq.pop_back();
            for (int x : {v.vp_normal, v.vp_fire, v.vp_dead, v.vp_wait, v.vp_megadeth, v.vp_genesis})
                vps.insert(x);
            for (int i = 0; i < (int)v.scale_fx_pXX.size() && v.scale_fx_pXX[i]; ++i)
                vps.insert(v.scale_fx_pXX[i]);
            addFx(v.dest_fx);
            addFx(v.ExtDestroyFX);
            if (v.weapon >= 0)
                addW(v.weapon);
            if (v.RoboProto)
                for (const World::TRoboGun &g : v.RoboProto->guns)
                    addV(g.robo_gun_type);
        }
        while (!wq.empty())
        {
            const World::TWeapProto &w = yw->_weaponProtos[wq.back()];
            wq.pop_back();
            for (int x : {w.vp_normal, w.vp_fire, w.vp_dead, w.vp_wait, w.vp_megadeth, w.vp_genesis})
                vps.insert(x);
            addFx(w.dfx);
            addFx(w.ExtDestroyFX);
        }
    }

    uint64_t t0 = SDL_GetPerformanceCounter();
    int want = 0, now = 0, had = 0, types = 0;
    for (int i = 0; i < nv; ++i)
        types += vSeen[i];
    for (int idx : vps)
    {
        if (idx < 0 || idx >= (int)gHd.disk.size() || gHd.disk[idx].empty())
            continue;
        want++;
        if (gHd.loaded[idx])
            had++;
        else if (HdLoadSlot(yw, idx))
            now++;
    }
    int avail = 0, resident = 0;
    for (size_t i = 0; i < gHd.disk.size(); ++i)
    {
        avail += !gHd.disk[i].empty();
        resident += gHd.loaded[i];
    }
    ypa_log_out("hd-models: lazy level %d: %d vehicle types, %d hd states wanted of %d in the set "
                "(%d loaded now, %d kept, %d resident), %.0f ms\n", yw->_levelInfo.LevelID, types, want, avail,
                now, had, resident, (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
}

void HdEnsure(NC_STACK_ypaworld *yw, int vpIndex)
{
    if (!gHd.root || vpIndex < 0 || vpIndex >= (int)gHd.disk.size() || gHd.loaded[vpIndex] || gHd.disk[vpIndex].empty())
        return;
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (HdLoadSlot(yw, vpIndex))
    {
        gHd.late++;
        ypa_log_out("hd-models: LATE %s (#%d) loaded mid-level in %.0f ms (late loads this set: %d)\n",
                    gHd.names[vpIndex].c_str(), vpIndex,
                    (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency(), gHd.late);
    }
}

double LegoLodDist()
{
    static double d = -1.0;
    static int dLevel = -1;
    const int bl = BuildingsLevel();
    if (d < 0.0 || bl != dLevel)
    {
        dLevel = bl;
        const char *p = getenv("UA_PERF_LEGOLOD");
        if (!p)
            p = getenv("UA_LEGO_LODDIST");
        static const double levelDist[4] = {0.0, 900.0, 1700.0, 3000.0};
        d = p ? atof(p) : levelDist[bl];   // 0 = far LOD off
        if (d < 0.0)
            d = 0.0;
    }
    return d;
}

NC_STACK_base *LegoLod(NC_STACK_base *near, const vec3d &pos)
{
    if (gLegoFar.empty() || !near)
        return near;
    const double dist = LegoLodDist();
    if (dist <= 0.0)
        return near;
    auto it = gLegoFar.find(near);
    if (it == gLegoFar.end())
        return near;
    TF::TForm3D *vp = TF::Engine.GetViewPoint();
    if (!vp)
        return near;
    vec3d dd = pos - vp->Pos;
    if (dd.dot(dd) <= dist * dist)
        return near;
    NC_STACK_base *far = it->second;
    if (gLegoFarPrepped.insert(far).second)
    {
        // same render state yw_parse_lego gave the near piece
        far->SetVizLimit(near->GetVisLimit());
        far->SetFadeLength(near->GetFadeLength());
        far->SetStatic(true);
        // not part of the set tree, so the set's MakeVBO never reached it
        far->MakeVBO();
    }
    return far;
}

#ifdef UA_VR_ENABLED
// The war-table miniature (VR): every building of the map is drawn at once, a few centimetres tall, so it always
// takes the far mesh when the pack has one (the near HD blocks cost several ms of GPU per eye on a whole city).
NC_STACK_base *LegoFarFor(NC_STACK_base *near)
{
    if (gLegoFar.empty() || !near)
        return near;
    auto it = gLegoFar.find(near);
    if (it == gLegoFar.end())
        return near;
    NC_STACK_base *far = it->second;
    if (gLegoFarPrepped.insert(far).second)
    {
        far->SetVizLimit(near->GetVisLimit());
        far->SetFadeLength(near->GetFadeLength());
        far->SetStatic(true);
        far->MakeVBO();
    }
    return far;
}
#endif

void ApplyHdLegos(NC_STACK_base *root)
{
    // a new set load: the previous set's far meshes belong to deleted legos
    for (NC_STACK_base *f : gLegoFarOwned)
        f->Delete();
    for (auto &kv : gLegoFar)
        if (!gLegoFarOwned.count(kv.second))
            kv.second->Delete();
    gLegoFar.clear();
    gLegoFarOwned.clear();
    gLegoFarPrepped.clear();

    if (!root || gOverrideSuspended || BuildingsLevel() == 0)
        return;

    const int setId = CurrentSetId();
    if (setId <= 0)
        return;

    // Graphics settings: BUILDINGS 0 = original set legos.
    if (gfxLevel("BUILDINGS", 3) == 0)
        return;

    std::string dir = HdModelsDir() + "/lego/set" + std::to_string(setId);
    if (!IsDir(dir))
        return;
    if (char *abs = realpath(dir.c_str(), NULL))  // LoadObj3DDisk needs an absolute path
    {
        dir = abs;
        free(abs);
    }

    std::vector<std::string> names = LegoNames(ReadLegoLines());
    if (names.empty())
        return;

    static std::set<std::string> logged;
    int replaced = 0, farN = 0;

    BaseList &kids = root->GetKidList();
    int idx = 0;
    for (auto it = kids.begin(); it != kids.end(); ++it, ++idx)
    {
        if (idx >= (int)names.size())
            break;

        const std::string &name = names[idx];
        std::string disk = dir + fmt::sprintf("/id%03d.obj", idx);
        if (!IsFile(disk))
            disk = dir + "/" + name + ".obj";
        if (!IsFile(disk))
            continue;

        NC_STACK_base *old = *it;
        NC_STACK_base *nb = NC_STACK_Obj3D::LoadObj3DDisk(disk);
        if (!nb)
        {
            ypa_log_out("hd-lego: FAILED to load %s, keeping original\n", disk.c_str());
            continue;
        }

        const std::string key = fmt::sprintf("lego%d:%s", setId, name);
        if (!gOrigRadius.count(key))
            gOrigRadius[key] = BoundRadius(old);

        TF::TForm3D *parentTf = old->TForm().Parent;
        nb->TForm() = old->TForm();
        nb->TForm().Parent = parentTf;
        nb->SetAmbientLight(old->GetAmbientLight());
        nb->_parent = root;
        *it = nb;

        old->_parent = NULL;

        // far LOD: <stem>.far.obj beside the near OBJ
        std::string farDisk = disk.substr(0, disk.size() - 4) + ".far.obj";
        NC_STACK_base *far = IsFile(farDisk) ? NC_STACK_Obj3D::LoadObj3DDisk(farDisk) : NULL;
        if (far)
        {
            far->TForm() = nb->TForm();
            far->SetAmbientLight(nb->GetAmbientLight());
            far->_parent = NULL;
            gLegoFar[nb] = far;
            gLegoFarOwned.insert(far);
            old->Delete();
            farN++;
        }
        else if (getenv("UA_PERF_LEGOLOD"))
            gLegoFar[nb] = old;   // perf comparison: the original lego is the far LOD
        else
            old->Delete();
        replaced++;

        if (logged.insert(fmt::sprintf("%s#%d", key, idx)).second)
            ypa_log_out("hd-lego: set %d %s (#%d) <- %s\n", setId, name.c_str(), idx, disk.c_str());
    }
    if (replaced)
        printf("[REFORGE] hd-lego: set %d, %d lego slots replaced from %s; far LOD: %d meshes, %d slots, dist %.0f\n",
               setId, replaced, dir.c_str(), farN, (int)gLegoFar.size(), LegoLodDist());
}

namespace
{

// ---------------------------------------------------------------------------
// set loading for the tool modes
// ---------------------------------------------------------------------------

struct SetLoad
{
    std::string oldRsrc;
    NC_STACK_base *set = NULL;
    NC_STACK_base *vp = NULL;
    std::vector<std::string> names;

    bool Load(int setId)
    {
        oldRsrc = Common::Env.GetPrefix("rsrc");
        Common::Env.SetPrefix("rsrc", fmt::sprintf("data:set%d", setId));
        GFX::Engine.LoadPalette("palette/standard.pal");
        names = ReadVisprotoNames();
        set = load_set_base();
        if (!set || set->GetKidList().empty())
            return false;
        set->MakeVBO();
        vp = set->GetKidList().front();
        return true;
    }

    // Lego group (kid 1) + its set.sdf lines; NULL when the set has none.
    NC_STACK_base *Lego(std::vector<LegoLine> *lines)
    {
        if (!set || set->GetKidList().size() < 2)
            return NULL;
        *lines = ReadLegoLines();
        return *std::next(set->GetKidList().begin());
    }

    ~SetLoad()
    {
        if (set)
            set->Delete();
        Common::Env.SetPrefix("rsrc", oldRsrc);
    }
};

// ---------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------

uint64_t Fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

SDL_Surface *ToRGBA(ResBitmap *bm)
{
    if (!bm || !bm->swTex)
        return NULL;
    return SDL_ConvertSurfaceFormat(bm->swTex, SDL_PIXELFORMAT_ABGR8888, 0); // RGBA byte order
}

uint64_t SurfaceHash(SDL_Surface *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s)
        return h;
    SDL_LockSurface(s);
    for (int y = 0; y < s->h; ++y)
        h = Fnv(h, (uint8_t *)s->pixels + y * s->pitch, s->w * 4);
    SDL_UnlockSurface(s);
    return h;
}

std::string Trim(std::string s)
{
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.erase(0, 1);
    return s;
}

std::string TexName(const GFX::TMesh &m, std::map<ResBitmap *, std::string> &names)
{
    if (m.Mat.TexSource)
        return Trim(m.Mat.TexSource->getRsrc_name());
    ResBitmap *bm = m.Mat.Tex;
    if (!bm && m.Mat.TexSource)
        bm = m.Mat.TexSource->GetBitmap(0);
    auto it = names.find(bm);
    return it != names.end() ? it->second : std::string();
}

// ResBitmap* -> rsrc name, collected from the visproto's ADEs.
void CollectTexNames(NC_STACK_base *b, std::map<ResBitmap *, std::string> *out)
{
    for (NC_STACK_ade *ade : *b->GetAdeList())
    {
        NC_STACK_area *a = dynamic_cast<NC_STACK_area *>(ade);
        if (a && a->_texImg)
        {
            ResBitmap *bm = a->_texImg->IsDynamic() ? a->_texImg->GetBitmap(0) : a->_texImg->GetBitmap();
            (*out)[bm] = Trim(a->_texImg->getRsrc_name());
        }
    }
    for (NC_STACK_base *k : b->GetKidList())
        CollectTexNames(k, out);
}

int CountParticles(NC_STACK_base *b)
{
    int n = 0;
    for (NC_STACK_ade *ade : *b->GetAdeList())
        if (ade->IsParticle())
            n++;
    for (NC_STACK_base *k : b->GetKidList())
        n += CountParticles(k);
    return n;
}

std::string JVec(const vec3d &v)
{
    return fmt::sprintf("[%.4f, %.4f, %.4f]", v.x, v.y, v.z);
}

std::string JStr(const std::string &s)
{
    std::string o = "\"";
    for (char c : s)
    {
        if ((unsigned char)c < 0x20)
            continue;
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

void DescribeTree(NC_STACK_base *b, int depth, std::string *out)
{
    TF::TForm3D &t = b->TForm();
    int tris = 0;
    for (const GFX::TMesh &m : b->Meshes)
        tris += m.Indixes.size() / 3;
    *out += fmt::sprintf("%s{\"depth\": %d, \"class\": %s, \"pos\": %s, \"scale\": %s, \"angles_deg\": [%d, %d, %d], "
                         "\"tform_flags\": %u, \"meshes\": %d, \"tris\": %d, \"ades\": %d, \"has_skeleton\": %s}",
                         out->empty() || out->back() == '[' ? "" : ", ",
                         depth, JStr(b->ClassName()), JVec(t.Pos), JVec(t.Scale),
                         (int)t.ax, (int)t.ay, (int)t.az, t.flags, (int)b->Meshes.size(), tris,
                         (int)b->GetAdeList()->size(), b->GetSkeleton() ? "true" : "false");
    for (NC_STACK_base *k : b->GetKidList())
        DescribeTree(k, depth + 1, out);
}

struct ExportInfo
{
    std::string Json;
    uint64_t GeoHash = 0;
    uint64_t TexHash = 0;
    vec3d BMin, BMax;
};

// Writes OBJ/MTL/PNG when outDir is non-empty; always returns the fingerprint.
ExportInfo ExportVisproto(NC_STACK_base *vp, const std::string &name, int idx, const std::string &outDir,
                          const char *what = NULL)
{
    ExportInfo info;
    std::vector<FlatMesh> fm;
    Flatten(vp, mat3x3::Ident(), vec3d(0, 0, 0), 0, &fm);

    std::map<ResBitmap *, std::string> texNames;
    CollectTexNames(vp, &texNames);

    FILE *obj = NULL, *mtl = NULL;
    if (!outDir.empty())
    {
        MkDirs(outDir + "/tex");
        obj = fopen((outDir + "/" + name + ".obj").c_str(), "w");
        mtl = fopen((outDir + "/" + name + ".mtl").c_str(), "w");
        if (obj)
        {
            if (what)
                fprintf(obj, "# Urban Assault %s %s (index %d), exported by OpenUA reforge exporter\n", what, name.c_str(), idx);
            else
                fprintf(obj, "# Urban Assault visproto %s (visproto.lst index %d), exported by OpenUA UA_EXPORT_VISPROTO\n", name.c_str(), idx);
            fprintf(obj, "# units: UA world units (1 sector = 1200). axes: x right, y up (engine y negated), z forward (engine z)\n");
            fprintf(obj, "# origin = visproto pivot. 'v x y z r g b': rgb = engine per-polygon shade (vertex colour)\n");
            fprintf(obj, "mtllib %s.mtl\n\n", name.c_str());
        }
    }

    uint64_t gh = 1469598103934665603ULL;
    uint64_t th = 1469598103934665603ULL;
    GFX::TBoundBox bb;
    int vid = 1, tvid = 1, mid = 0, tris = 0, verts = 0;
    std::string meshJson;
    std::map<std::string, std::string> pngDone;

    for (const FlatMesh &f : fm)
    {
        const GFX::TMesh &m = *f.Mesh;
        const bool textured = (m.Mat.Flags & GFX::RFLAGS_TEXTURED) != 0;
        std::string tname = TexName(m, texNames);
        std::string png;

        ResBitmap *bm = m.Mat.Tex;
        if (!bm && m.Mat.TexSource)
            bm = m.Mat.TexSource->GetBitmap(0);

        if (bm)
        {
            SDL_Surface *rgba = ToRGBA(bm);
            uint64_t sh = SurfaceHash(rgba);
            th = Fnv(th, &sh, sizeof(sh));
            png = "tex/" + Sanitize(tname.empty() ? fmt::sprintf("tex%p", (void *)bm) : tname) + ".png";
            if (!outDir.empty() && rgba && !pngDone.count(png))
            {
                IMG_SavePNG(rgba, (outDir + "/" + png).c_str());
                pngDone[png] = tname;
            }
            if (rgba)
                SDL_FreeSurface(rgba);
        }

        std::string matName = fmt::sprintf("m%d_%s", mid, Sanitize(tname.empty() ? "flat" : tname));

        if (mtl)
        {
            fprintf(mtl, "newmtl %s\n", matName.c_str());
            fprintf(mtl, "# ua_render_flags 0x%x%s%s%s%s\n", m.Mat.Flags,
                    (m.Mat.Flags & GFX::RFLAGS_SHADED) ? " SHADED" : "",
                    (m.Mat.Flags & GFX::RFLAGS_ZEROTRACY) ? " ZEROTRACY(colour-key)" : "",
                    (m.Mat.Flags & GFX::RFLAGS_LUMTRACY) ? " LUMTRACY(additive)" : "",
                    (m.Mat.Flags & GFX::RFLAGS_DYNAMIC_TEXTURE) ? " DYNAMIC_TEXTURE(frame0)" : "");
            if (!tname.empty())
                fprintf(mtl, "# ua_texture_rsrc %s\n", tname.c_str());
            fprintf(mtl, "Kd %f %f %f\nKa 1.0 1.0 1.0\nKs 0.0 0.0 0.0\nd %f\nillum 1\n",
                    textured ? 1.0 : m.Mat.Color.r, textured ? 1.0 : m.Mat.Color.g, textured ? 1.0 : m.Mat.Color.b,
                    m.Mat.Color.a);
            if (!png.empty())
                fprintf(mtl, "map_Kd %s\n", png.c_str());
            fprintf(mtl, "\n");
        }

        for (const GFX::TVertex &v : m.Vertexes)
        {
            vec3d p = f.Rot.Transform(v.Pos) + f.Off;
            bb.Add(p);
            float pf[3] = {(float)p.x, (float)p.y, (float)p.z};
            gh = Fnv(gh, pf, sizeof(pf));
            if (obj)
                fprintf(obj, "v %f %f %f %.4f %.4f %.4f\n", p.x, -p.y, p.z, v.Color.r, v.Color.g, v.Color.b);
        }
        // Animated textures keep their UVs in the per-frame outline (frame 0
        // is exported, matching the PNG), not in TVertex::TexCoord.
        std::vector<tUtV> *outline = NULL;
        if ((m.Mat.Flags & GFX::RFLAGS_DYNAMIC_TEXTURE) && m.Mat.TexSource)
            outline = &m.Mat.TexSource->GetOutline(0);

        if (textured)
            for (const GFX::TVertex &v0 : m.Vertexes)
            {
                GFX::TVertex v = v0;
                if (outline && v0.TexCoordId < outline->size())
                    v.TexCoord = outline->at(v0.TexCoordId);
                float uv[2] = {v.TexCoord.tu, v.TexCoord.tv};
                gh = Fnv(gh, uv, sizeof(uv));
                if (obj)
                    fprintf(obj, "vt %f %f\n", v.TexCoord.tu, 1.0 - v.TexCoord.tv);
            }

        if (obj)
            fprintf(obj, "g %s\nusemtl %s\n", matName.c_str(), matName.c_str());

        // Same winding flip as NC_STACK_Obj3D::ExportObj (y is negated), so the
        // engine's OBJ loader round-trips it exactly.
        for (size_t i = 0; i + 2 < m.Indixes.size(); i += 3)
        {
            uint32_t a = m.Indixes[i], b = m.Indixes[i + 2], c = m.Indixes[i + 1];
            gh = Fnv(gh, &m.Indixes[i], sizeof(uint32_t) * 3);
            if (obj)
            {
                if (textured)
                    fprintf(obj, "f %d/%d %d/%d %d/%d\n", vid + a, tvid + a, vid + b, tvid + b, vid + c, tvid + c);
                else
                    fprintf(obj, "f %d %d %d\n", vid + a, vid + b, vid + c);
            }
        }

        meshJson += fmt::sprintf("%s{\"material\": %s, \"flags\": %u, \"texture_rsrc\": %s, \"png\": %s, \"verts\": %d, \"tris\": %d, \"depth\": %d}",
                                 meshJson.empty() ? "" : ", ", JStr(matName), m.Mat.Flags, JStr(tname), JStr(png),
                                 (int)m.Vertexes.size(), (int)(m.Indixes.size() / 3), f.Depth);

        tris += m.Indixes.size() / 3;
        verts += m.Vertexes.size();
        vid += m.Vertexes.size();
        if (textured)
            tvid += m.Vertexes.size();
        mid++;
    }

    if (obj) fclose(obj);
    if (mtl) fclose(mtl);

    std::string tree;
    DescribeTree(vp, 0, &tree);

    // bbox in export (OBJ) coordinates: y negated.
    vec3d emin(bb.Min.x, -bb.Max.y, bb.Min.z), emax(bb.Max.x, -bb.Min.y, bb.Max.z);
    info.BMin = emin;
    info.BMax = emax;
    info.GeoHash = gh;
    info.TexHash = th;
    info.Json = fmt::sprintf(
        "{\"name\": %s, \"lst_index\": %d, \"verts\": %d, \"tris\": %d, \"materials\": %d, \"particle_emitters\": %d,\n"
        "     \"bbox_obj\": {\"min\": %s, \"max\": %s, \"size\": %s},\n"
        "     \"bound_radius_from_pivot\": %.4f,\n"
        "     \"geo_hash\": \"%016llx\", \"tex_hash\": \"%016llx\",\n"
        "     \"tree\": [%s],\n"
        "     \"meshes\": [%s]}",
        JStr(name), idx, verts, tris, mid, CountParticles(vp),
        JVec(emin), JVec(emax), JVec(emax - emin), BoundRadius(vp),
        (unsigned long long)gh, (unsigned long long)th, tree, meshJson);
    return info;
}

int RunExport()
{
    const char *spec = Env("UA_EXPORT_VISPROTO");
    std::string out = Env("UA_EXPORT_OUT") ? Env("UA_EXPORT_OUT") : "reforge-export";
    std::vector<int> sets;
    for (const std::string &s : Split(Env("UA_EXPORT_SETS") ? Env("UA_EXPORT_SETS") : "1", ','))
        sets.push_back(atoi(s.c_str()));

    gOverrideSuspended = true;
    MkDirs(out);

    std::string setsJson;
    std::string vpsJson;
    int exported = 0;

    for (size_t si = 0; si < sets.size(); ++si)
    {
        SetLoad sl;
        if (!sl.Load(sets[si]))
        {
            ypa_log_out("reforge-export: set %d failed to load\n", sets[si]);
            continue;
        }
        std::vector<int> ids = ResolveSelectors(spec, sl.names);
        std::string entries;
        for (int id : ids)
        {
            NC_STACK_base *vp = KidAt(sl.vp, id);
            if (!vp)
                continue;
            ExportInfo e = ExportVisproto(vp, sl.names[id], id, si == 0 ? out : std::string());
            if (si == 0)
            {
                vpsJson += (vpsJson.empty() ? "    " : ",\n    ") + e.Json;
                exported++;
                ypa_log_out("reforge-export: %s (#%d) -> %s/%s.obj\n", sl.names[id].c_str(), id, out.c_str(), sl.names[id].c_str());
            }
            entries += fmt::sprintf("%s%s: {\"lst_index\": %d, \"geo_hash\": \"%016llx\", \"tex_hash\": \"%016llx\", \"bbox_min\": %s, \"bbox_max\": %s}",
                                    entries.empty() ? "" : ", ", JStr(sl.names[id]), id,
                                    (unsigned long long)e.GeoHash, (unsigned long long)e.TexHash, JVec(e.BMin), JVec(e.BMax));
        }
        setsJson += fmt::sprintf("%s    \"%d\": {\"rsrc\": %s, \"visproto_count\": %d, \"lst_count\": %d, \"visprotos\": {%s}}",
                                 setsJson.empty() ? "" : ",\n", sets[si],
                                 JStr(Common::Env.GetPrefix("rsrc")), (int)sl.vp->GetKidList().size(), (int)sl.names.size(), entries);
    }

    FILE *j = fopen((out + "/export.json").c_str(), "w");
    if (j)
    {
        fprintf(j, "{\n  \"tool\": \"OpenUA UA_EXPORT_VISPROTO\",\n  \"selector\": %s,\n  \"data_prefix\": %s,\n"
                   "  \"coords\": \"OBJ: x, -engine_y (up), z; UA world units; origin = visproto pivot\",\n"
                   "  \"textures\": \"original software surfaces (ResBitmap::swTex), not hd-textures overrides\",\n"
                   "  \"exported_from_set\": %d,\n  \"visprotos\": [\n%s\n  ],\n  \"sets\": {\n%s\n  }\n}\n",
                JStr(spec).c_str(), JStr(Common::Env.GetPrefix("data")).c_str(), sets.empty() ? 0 : sets[0], vpsJson.c_str(), setsJson.c_str());
        fclose(j);
    }

    gOverrideSuspended = false;
    ypa_log_out("reforge-export: %d visprotos written to %s\n", exported, out.c_str());
    printf("[REFORGE] export done: %d visprotos -> %s\n", exported, out.c_str());
    fflush(stdout);
    return exported > 0 ? 0 : 1;
}

// Lego selector: set.sdf name (case-insensitive, all slots of it), "#id", "all".
std::vector<int> ResolveLegoSelectors(const std::string &spec, const std::vector<std::string> &names)
{
    std::vector<int> ids;
    std::set<int> seen;
    for (const std::string &tok : Split(spec, ','))
    {
        if (Upper(tok) == "ALL")
        {
            for (size_t i = 0; i < names.size(); ++i)
                if (seen.insert(i).second) ids.push_back(i);
            continue;
        }
        if (tok[0] == '#')
        {
            int i = atoi(tok.c_str() + 1);
            if (i >= 0 && i < (int)names.size() && seen.insert(i).second)
                ids.push_back(i);
            continue;
        }
        for (size_t i = 0; i < names.size(); ++i)
            if (Upper(names[i]) == Upper(tok) && seen.insert(i).second)
                ids.push_back(i);
    }
    return ids;
}

// UA_EXPORT_LEGO: original lego pieces of every set in UA_EXPORT_SETS.
int RunExportLego()
{
    const char *spec = Env("UA_EXPORT_LEGO");
    std::string out = Env("UA_EXPORT_OUT") ? Env("UA_EXPORT_OUT") : "reforge-export";
    std::vector<int> sets;
    for (const std::string &s : Split(Env("UA_EXPORT_SETS") ? Env("UA_EXPORT_SETS") : "1", ','))
        sets.push_back(atoi(s.c_str()));

    gOverrideSuspended = true;
    int exported = 0;
    for (int setId : sets)
    {
        SetLoad sl;
        if (!sl.Load(setId))
        {
            ypa_log_out("reforge-export: set %d failed to load\n", setId);
            continue;
        }
        std::vector<LegoLine> lines;
        NC_STACK_base *lego = sl.Lego(&lines);
        if (!lego)
            continue;
        std::vector<std::string> names = LegoNames(lines);
        const std::string dir = fmt::sprintf("%s/set%d", out, setId);
        MkDirs(dir);

        std::string json;
        std::set<std::string> written;
        for (int id : ResolveLegoSelectors(spec, names))
        {
            NC_STACK_base *b = KidAt(lego, id);
            if (!b)
                continue;
            const LegoLine &l = lines[id];
            // one OBJ per name (same name = same base file); every slot is listed
            const bool write = written.insert(Upper(l.Name)).second;
            ExportInfo e = ExportVisproto(b, l.Name, id, write ? dir : std::string(),
                                          fmt::sprintf("set %d lego", setId).c_str());
            if (write)
                exported++;

            // set.sdf: base sklt fontpage gui shield collisionmode {fx x y z}...
            // Engine position = (x, -z_tok, -y_tok) mapping as yw_parse_lego:
            // tok x -> x, tok 2 -> engine z = -tok, tok 3 -> engine y = -tok.
            // OBJ coords (x, -engine y, engine z) = (t1, t3, -t2).
            std::string fx;
            for (size_t t = 6; t < l.Tok.size(); t += 4)
            {
                double x = t + 1 < l.Tok.size() ? atof(l.Tok[t + 1].c_str()) : 0.0;
                double a = t + 2 < l.Tok.size() ? atof(l.Tok[t + 2].c_str()) : 0.0;
                double c = t + 3 < l.Tok.size() ? atof(l.Tok[t + 3].c_str()) : 0.0;
                fx += fmt::sprintf("%s{\"object\": %d, \"obj\": [%.1f, %.1f, %.1f]}", fx.empty() ? "" : ", ",
                                   (int)strtol(l.Tok[t].c_str(), NULL, 0), x, c, -a);
            }
            std::string toks;
            for (const std::string &t : l.Tok)
                toks += (toks.empty() ? "" : ", ") + JStr(t);
            json += fmt::sprintf("%s    {\"id\": %d, \"name\": %s, \"obj\": %s, \"sdf\": [%s],\n"
                                 "     \"sklt\": %s, \"shield\": %s, \"collision_mode\": %s, \"explosions\": [%s],\n"
                                 "     \"export\": %s}",
                                 json.empty() ? "" : ",\n", id, JStr(l.Name), JStr(l.Name + ".obj"), toks,
                                 JStr(l.Tok.size() > 1 ? l.Tok[1] : ""), l.Tok.size() > 4 ? l.Tok[4].c_str() : "null",
                                 l.Tok.size() > 5 ? l.Tok[5].c_str() : "null", fx, e.Json);
        }
        FILE *j = fopen((dir + "/legos.json").c_str(), "w");
        if (j)
        {
            fprintf(j, "{\n  \"tool\": \"OpenUA UA_EXPORT_LEGO\",\n  \"set\": %d,\n  \"selector\": %s,\n  \"lego_count\": %d,\n"
                       "  \"coords\": \"OBJ: x, -engine_y (up), z; UA world units; origin = lego origin (sub-sector centre, ground)\",\n"
                       "  \"explosions\": \"set.sdf fx points mapped like yw_parse_lego, in OBJ coords\",\n"
                       "  \"legos\": [\n%s\n  ]\n}\n",
                    setId, JStr(spec).c_str(), (int)names.size(), json.c_str());
            fclose(j);
        }
        ypa_log_out("reforge-export: set %d legos -> %s\n", setId, dir.c_str());
    }
    gOverrideSuspended = false;
    printf("[REFORGE] lego export done: %d lego files -> %s\n", exported, out.c_str());
    fflush(stdout);
    return exported > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// viewer
// ---------------------------------------------------------------------------

bool ReadbackPng(const std::string &path, int bgR, int bgG, int bgB)
{
    (void)bgR; (void)bgG; (void)bgB;
    Common::Point res = System::GetResolution();
    int w = res.x, h = res.y;
    std::vector<uint8_t> buf(w * h * 4);
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s)
        return false;
    SDL_LockSurface(s);
    for (int y = 0; y < h; ++y)
    {
        uint8_t *dst = (uint8_t *)s->pixels + y * s->pitch;
        const uint8_t *src = buf.data() + (h - 1 - y) * w * 4;
        for (int x = 0; x < w; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 2];
            dst[x * 4 + 3] = 255; // opaque: framebuffer alpha is not meaningful
        }
    }
    SDL_UnlockSurface(s);
    bool ok = IMG_SavePNG(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

// Game clock of the viewer renders: constant (frame 0 of every animated
// texture) except in the UA_VIEWER_ANIM strip.
int32_t gViewGlobTime = 1;
int32_t gViewFrameTime = 1;

void RenderShot(NC_STACK_base *vp, double dist, int pitch, int yaw, int bg[3])
{
    GFX::Engine.BeginFrame();
    glClearColor(bg[0] / 255.0f, bg[1] / 255.0f, bg[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);

    GFX::Engine.BeginScene();

    TF::TForm3D view;
    view.Scale = vec3d(1.0, 1.0, 1.0);
    view.SclRot = mat3x3::Ident();
    TF::Engine.SetViewPoint(&view);
    view.CalcGlobal();

    baseRender_msg arg;
    arg.frameTime = gViewFrameTime;
    arg.globTime = gViewGlobTime;
    arg.adeCount = 0;
    arg.minZ = 17.0;
    arg.maxZ = 32000.0;
    arg.flags = GFX::RFLAGS_IGNORE_FALLOFF;

    vp->SetVizLimit(16000);
    vp->SetFadeLength(100);
    vp->SetPosition(vec3d(0.0, 0.0, dist));
    vp->SetEulerRotation(pitch, yaw, 0);

    // No render instance: particle emitters (time/random driven) stay off, so
    // the image is a pure function of geometry, textures and camera.
    vp->Render(&arg, NULL);

    GFX::Engine.Rasterize();
}

// Top-down RGBA copy of the framebuffer (alpha not meaningful).
void ReadFrame(std::vector<uint8_t> &img, int &w, int &h)
{
    Common::Point res = System::GetResolution();
    w = res.x;
    h = res.y;
    std::vector<uint8_t> buf(w * h * 4);
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    img.resize(w * h * 4);
    for (int y = 0; y < h; ++y)
        memcpy(&img[y * w * 4], &buf[(h - 1 - y) * w * 4], w * 4);
}

bool SaveRgba(const std::string &path, const std::vector<uint8_t> &rgba, int w, int h)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s)
        return false;
    SDL_LockSurface(s);
    for (int y = 0; y < h; ++y)
        memcpy((uint8_t *)s->pixels + y * s->pitch, &rgba[y * w * 4], w * 4);
    SDL_UnlockSurface(s);
    bool ok = IMG_SavePNG(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

void GrabShot(NC_STACK_base *vp, double dist, int pitch, int yaw, int bg[3], std::vector<uint8_t> &img, int &w, int &h)
{
    RenderShot(vp, dist, pitch, yaw, bg);
    ReadFrame(img, w, h);
    GFX::Engine.EndScene();
    GFX::Engine.EndFrame();
}

// Unit over a transparent background: rendered on black and on white;
// alpha = 1 - (white - black) / 255, colour = black / alpha. Exact for the
// engine's (linear) coverage/MSAA blending with the clear colour.
struct Matte
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive bbox of alpha > 0
    int Span() const { return x1 < x0 ? 0 : std::max(x1 - x0 + 1, y1 - y0 + 1); }
};

Matte GrabMatte(NC_STACK_base *vp, double dist, int pitch, int yaw)
{
    int black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
    std::vector<uint8_t> b, wt;
    Matte m;
    GrabShot(vp, dist, pitch, yaw, black, b, m.w, m.h);
    GrabShot(vp, dist, pitch, yaw, white, wt, m.w, m.h);
    m.rgba.assign(m.w * m.h * 4, 0);
    m.x0 = m.w; m.y0 = m.h; m.x1 = -1; m.y1 = -1;
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x)
        {
            size_t i = (size_t)(y * m.w + x) * 4;
            int diff = (int)wt[i] - b[i] + (int)wt[i + 1] - b[i + 1] + (int)wt[i + 2] - b[i + 2];
            int a = 255 - (diff + 1) / 3;
            a = std::max(0, std::min(255, a));
            if (a == 0)
                continue;
            for (int c = 0; c < 3; ++c)
                m.rgba[i + c] = (uint8_t)std::min(255, (b[i + c] * 255 + a / 2) / a);
            m.rgba[i + 3] = (uint8_t)a;
            m.x0 = std::min(m.x0, x); m.x1 = std::max(m.x1, x);
            m.y0 = std::min(m.y0, y); m.y1 = std::max(m.y1, y);
        }
    return m;
}

std::vector<int> IntList(const char *v, const char *def)
{
    std::vector<int> out;
    for (const std::string &t : Split(v ? v : def, ','))
        out.push_back(atoi(t.c_str()));
    return out;
}

int RunFarRead(NC_STACK_base *vp, const std::string &name, bool replaced, double radius, const std::string &out, int bg[3])
{
    std::vector<int> targets = IntList(Env("UA_VIEWER_PX"), "64,32");
    std::vector<int> yaws = IntList(Env("UA_VIEWER_FAR_YAWS"), "45,135,225,315");
    int pitch = Env("UA_VIEWER_FAR_PITCH") ? atoi(Env("UA_VIEWER_FAR_PITCH")) : 50;
    std::vector<double> pinned;
    if (Env("UA_VIEWER_FAR_DIST"))
        for (const std::string &t : Split(Env("UA_VIEWER_FAR_DIST"), ','))
            pinned.push_back(atof(t.c_str()));

    int shots = 0;
    std::string json;
    for (size_t ti = 0; ti < targets.size(); ++ti)
    {
        const int T = std::max(2, targets[ti]);
        double d = radius * 2.0;
        bool pin = ti < pinned.size() && pinned[ti] > 0.0;
        std::string calib;
        if (pin)
            d = pinned[ti];
        else
        {
            // Mean silhouette span over the yaws scales ~1/d. Up to 6 passes;
            // the measured distance closest to the target wins (pixel
            // quantisation can make the updates oscillate). Deterministic:
            // same renders every run.
            double bestD = d, bestErr = 1e30;
            for (int it = 0; it < 6; ++it)
            {
                double sum = 0.0;
                for (int yaw : yaws)
                    sum += GrabMatte(vp, d, pitch, yaw).Span();
                double S = sum / yaws.size();
                calib += fmt::sprintf("%s[%.3f, %.2f]", calib.empty() ? "" : ", ", d, S);
                if (fabs(S - T) < bestErr)
                {
                    bestErr = fabs(S - T);
                    bestD = d;
                }
                if (S <= 0.0 || bestErr < 0.25)
                    break;
                d *= S / T;
            }
            d = bestD;
        }

        const int C = ((T * 3 / 2) + 1) & ~1; // crop side, even
        std::string spans;
        for (int yaw : yaws)
        {
            std::string base = fmt::sprintf("%s/%s_px%03d_yaw%03d", out, name, T, yaw);
            std::vector<uint8_t> full;
            int w, h;
            GrabShot(vp, d, pitch, yaw, bg, full, w, h);
            if (SaveRgba(base + ".png", full, w, h))
                shots++;

            Matte m = GrabMatte(vp, d, pitch, yaw);
            int cx0 = m.w / 2 - C / 2, cy0 = m.h / 2 - C / 2;
            bool fits = m.x1 < m.x0 || (m.x0 >= cx0 && m.y0 >= cy0 && m.x1 < cx0 + C && m.y1 < cy0 + C);
            std::vector<uint8_t> crop(C * C * 4, 0), big(C * 4 * C * 4 * 4, 0);
            for (int y = 0; y < C; ++y)
                for (int x = 0; x < C; ++x)
                {
                    int sx = cx0 + x, sy = cy0 + y;
                    uint8_t px[4] = {0, 0, 0, 0};
                    if (sx >= 0 && sy >= 0 && sx < m.w && sy < m.h)
                        memcpy(px, &m.rgba[(size_t)(sy * m.w + sx) * 4], 4);
                    memcpy(&crop[(size_t)(y * C + x) * 4], px, 4);
                    uint8_t o[4];
                    for (int c = 0; c < 3; ++c)
                        o[c] = (uint8_t)((px[c] * px[3] + bg[c] * (255 - px[3]) + 127) / 255);
                    o[3] = 255;
                    for (int yy = 0; yy < 4; ++yy)
                        for (int xx = 0; xx < 4; ++xx)
                            memcpy(&big[(size_t)((y * 4 + yy) * C * 4 + x * 4 + xx) * 4], o, 4);
                }
            SaveRgba(base + "_crop.png", crop, C, C);
            SaveRgba(base + "_crop_x4.png", big, C * 4, C * 4);
            spans += fmt::sprintf("%s{\"yaw\": %d, \"span\": %d, \"bbox\": [%d, %d, %d, %d], \"in_crop\": %s}",
                                  spans.empty() ? "" : ", ", yaw, m.Span(), m.x0, m.y0, m.x1, m.y1, fits ? "true" : "false");
            if (!fits)
                printf("[REFORGE] far-read %s px%d yaw%d: unit exceeds the %dx%d crop\n", name.c_str(), T, yaw, C, C);
        }
        json += fmt::sprintf("%s    {\"px\": %d, \"dist\": %.3f, \"pinned\": %s, \"crop\": %d, \"calibration\": [%s],\n     \"shots\": [%s]}",
                             json.empty() ? "" : ",\n", T, d, pin ? "true" : "false", C, calib, spans);
        printf("[REFORGE] far-read %s%s: target %d px -> dist %.3f%s\n", name.c_str(), replaced ? " [hd-models]" : "",
               T, d, pin ? " (pinned)" : "");
    }

    FILE *j = fopen(fmt::sprintf("%s/%s_far.json", out, name).c_str(), "w");
    if (j)
    {
        Common::Point res = System::GetResolution();
        fprintf(j, "{\"name\": %s, \"hd_models\": %s, \"window\": [%d, %d], \"pitch\": %d, \"orig_radius\": %.3f,\n"
                   "  \"span_def\": \"max(bbox w, bbox h) of the matte (alpha > 0), px\",\n  \"targets\": [\n%s\n  ]}\n",
                JStr(name).c_str(), replaced ? "true" : "false", res.x, res.y, pitch, radius, json.c_str());
        fclose(j);
    }
    return shots;
}

int RunViewer()
{
    const char *spec = Env("UA_VIEWER");
    std::string out = Env("UA_VIEWER_OUT") ? Env("UA_VIEWER_OUT") : "reforge-viewer";
    int setId = Env("UA_VIEWER_SET") ? atoi(Env("UA_VIEWER_SET")) : 1;
    int angles = Env("UA_VIEWER_ANGLES") ? atoi(Env("UA_VIEWER_ANGLES")) : 8;
    if (angles < 1) angles = 1;
    int bg[3] = {64, 64, 64};
    if (Env("UA_VIEWER_BG"))
    {
        std::vector<std::string> c = Split(Env("UA_VIEWER_BG"), ',');
        for (size_t i = 0; i < 3 && i < c.size(); ++i)
            bg[i] = atoi(c[i].c_str());
    }

    MkDirs(out);

    SetLoad sl;
    if (!sl.Load(setId))
    {
        ypa_log_out("reforge-viewer: set %d failed to load\n", setId);
        return 1;
    }

    // Targets: visprotos (UA_VIEWER), then lego pieces (UA_VIEWER_LEGO).
    struct Target { NC_STACK_base *b; std::string name, key; int id; };
    std::vector<Target> targets;
    if (spec)
        for (int id : ResolveSelectors(spec, sl.names))
            if (NC_STACK_base *vp = KidAt(sl.vp, id))
                targets.push_back({vp, sl.names[id], sl.names[id], id});
    if (Env("UA_VIEWER_LEGO"))
    {
        std::vector<LegoLine> lines;
        if (NC_STACK_base *lego = sl.Lego(&lines))
        {
            std::vector<std::string> ln = LegoNames(lines);
            for (int id : ResolveLegoSelectors(Env("UA_VIEWER_LEGO"), ln))
                if (NC_STACK_base *b = KidAt(lego, id))
                    targets.push_back({b, fmt::sprintf("L%d_%s", setId, ln[id]), fmt::sprintf("lego%d:%s", setId, ln[id]), id});
        }
    }

    int shots = 0;
    for (const Target &tg : targets)
    {
        NC_STACK_base *vp = tg.b;
        const int id = tg.id;
        const std::string &name = tg.name;

        auto r = gOrigRadius.find(tg.key);
        double radius = r != gOrigRadius.end() ? r->second : BoundRadius(vp);
        bool replaced = r != gOrigRadius.end();
        if (radius < 1.0)
            radius = 1.0;
        const double dist = radius * 2.0;

        if (Env("UA_VIEWER_FAR") || Env("UA_VIEWER_PX"))
        {
            shots += RunFarRead(vp, name, replaced, radius, out, bg);
            continue;
        }

        struct Shot { std::string tag; int pitch; int yaw; };
        std::vector<Shot> list;
        for (int a = 0; a < angles; ++a)
        {
            int yaw = (360 * a) / angles;
            list.push_back({fmt::sprintf("yaw%03d", yaw), 10, yaw});
        }
        list.push_back({"rts", 50, 45});

        for (const Shot &s : list)
        {
            RenderShot(vp, dist, s.pitch, s.yaw, bg);
            std::string path = fmt::sprintf("%s/%s_%s.png", out, name, s.tag);
            bool ok = ReadbackPng(path, bg[0], bg[1], bg[2]);
            GFX::Engine.EndScene();
            GFX::Engine.EndFrame();
            if (ok)
                shots++;
        }
        if (Env("UA_VIEWER_ANIM"))
        {
            std::vector<int> an = IntList(Env("UA_VIEWER_ANIM"), "48,40");
            int nfr = an.size() > 0 ? std::max(1, an[0]) : 48;
            int step = an.size() > 1 ? std::max(1, an[1]) : 40;
            std::vector<double> dists;
            if (Env("UA_VIEWER_ANIM_DIST"))
                for (const std::string &t : Split(Env("UA_VIEWER_ANIM_DIST"), ','))
                {
                    double d = atof(t.c_str());
                    dists.push_back(d > 0.0 ? d : dist); // 0 = the turntable distance (2 x radius)
                }
            if (dists.empty())
                dists.push_back(dist);
            const Shot views[2] = {{"rts", 50, 45}, {"yaw090", 10, 90}};
            int animShots = 0;
            for (size_t k = 0; k < dists.size(); ++k)
                for (const Shot &s : views)
                {
                    // Clock steps of <step> ms; the texture state carries on from the previous strip (not reset).
                    gViewGlobTime = 1;
                    gViewFrameTime = 1;
                    RenderShot(vp, dists[k], s.pitch, s.yaw, bg); // settle at clock 0 (frame 0)
                    GFX::Engine.EndScene();
                    GFX::Engine.EndFrame();
                    for (int f = 0; f < nfr; ++f)
                    {
                        if (f > 0)
                        {
                            gViewGlobTime += 1;
                            gViewFrameTime = step;
                        }
                        RenderShot(vp, dists[k], s.pitch, s.yaw, bg);
                        std::string path = fmt::sprintf("%s/%s_anim_%s_d%d_f%03d.png", out, name, s.tag, (int)k, f);
                        if (ReadbackPng(path, bg[0], bg[1], bg[2]))
                            animShots++;
                        GFX::Engine.EndScene();
                        GFX::Engine.EndFrame();
                    }
                }
            gViewGlobTime = 1;
            gViewFrameTime = 1;
            printf("[REFORGE] viewer anim %s: %d frames x %d ms, %d dists -> %d shots\n", name.c_str(), nfr, step,
                   (int)dists.size(), animShots);
        }
        ypa_log_out("reforge-viewer: %s (#%d)%s radius %.2f dist %.2f -> %d shots\n", name.c_str(), id,
                    replaced ? " [hd-models]" : "", radius, dist, (int)list.size());
        printf("[REFORGE] viewer %s%s: radius %.2f, %d shots -> %s\n", name.c_str(), replaced ? " [hd-models]" : "",
               radius, (int)list.size(), out.c_str());
    }
    fflush(stdout);
    return shots > 0 ? 0 : 1;
}

} // namespace

bool MenuFrameHook()
{
    static int state = -1; // -1 undecided, 0 inactive, >0 settle frames left
    if (state == 0)
        return false;

    if (state < 0)
    {
        state = (Env("UA_EXPORT_VISPROTO") || Env("UA_VIEWER") || Env("UA_EXPORT_LEGO") || Env("UA_VIEWER_LEGO")) ? 30 : 0;
        if (state == 0)
            return false;
    }

    if (--state > 0)
        return false;

    int rc = 0;
    if (Env("UA_EXPORT_VISPROTO"))
        rc |= RunExport();
    if (Env("UA_EXPORT_LEGO"))
        rc |= RunExportLego();
    if (Env("UA_VIEWER") || Env("UA_VIEWER_LEGO"))
        rc |= RunViewer();

    printf("[REFORGE] tool mode finished rc=%d, quitting\n", rc);
    fflush(stdout);
    state = 0;
    return true;
}

}
