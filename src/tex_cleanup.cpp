#include "tex_cleanup.h"

#include <ctype.h>
#include <string.h>
#include <vector>

namespace TexCleanup
{

// Measured on UA_TEX_DUMP=1 dumps of the original game data: every level that
// uses one of these bitmaps gives exactly this number (daylight set e.g. on
// levels 1, 4, 10, 20, 25; night set on levels 2, 11, 30, 107). Their MD5s
// match the two tagged bitmaps in the August log, 27c5a84b... and 2a6fc3cc...
const uint64_t kCity1WarmHash  = 0x260937305574d179ULL;
const uint64_t kCity1NightHash = 0x00557e551678b893ULL;

namespace
{

const int kSheet = 256;

// A rectangle, both ends included.
struct Zone
{
    int x0, y0, x1, y1;
};

// Rule 1: the lettering columns.
const Zone kInkColumns = { 250, 0, 255, 255 };

// Rule 2: the three zones where islands are looked for.
const Zone kZones[] =
{
    {   0, 116,  39, 255 }, // A
    { 240,   0, 249,  83 }, // B
    {   0, 248,  71, 255 }, // C
};

const int kIslandMax = 700;

// The signature box and the strip it is copied from.
const int kSigX1 = 92, kSigY0 = 252, kSigY1 = 255;
const int kDonorX0 = 94, kDonorPeriod = 50;
// The donor window (x 94-143) is a dark dither in every palette of the game:
// no channel above 51. Anything brighter is not the sheet this was made for.
const int kDonorMaxChannel = 51;

std::string Lower(const std::string &s)
{
    std::string out(s);
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

// The file part of the name, without a path and without the extension.
std::string Stem(const std::string &name)
{
    std::string n = Lower(name);
    size_t slash = n.find_last_of("/\\:");
    if ( slash != std::string::npos )
        n = n.substr(slash + 1);
    size_t dot = n.find('.');
    if ( dot != std::string::npos )
        n = n.substr(0, dot);
    return n;
}

inline uint8_t *Px(uint8_t *rgba, int stride, int x, int y)
{
    return rgba + (size_t)y * stride + (size_t)x * 4;
}

inline void Clear(uint8_t *p)
{
    p[0] = p[1] = p[2] = p[3] = 0;
}

// Opaque here means "not fully transparent": city1's alpha is either 0 or 255.
inline bool Opaque(const uint8_t *p)
{
    return p[3] != 0;
}

int ClearColumns(uint8_t *rgba, int stride, const Zone &z)
{
    int n = 0;
    for (int y = z.y0; y <= z.y1; y++)
        for (int x = z.x0; x <= z.x1; x++)
        {
            uint8_t *p = Px(rgba, stride, x, y);
            if ( Opaque(p) )
            {
                Clear(p);
                n++;
            }
        }
    return n;
}

// Finds the groups of opaque pixels inside the zone (a pixel joins a group
// through any of its 8 neighbours, but only neighbours inside the zone count)
// and clears every group of at most kIslandMax pixels.
//
// Why 8 neighbours and not 4: it is the careful choice. A group can only grow
// when corners count too, so a piece of artwork that touches lettering by a
// corner makes one big group and is kept, and every group that is cleared is
// at least as isolated as it would be with 4 neighbours. On the two graffiti
// sheets both choices clear exactly the same pixels (checked on every dump).
int ClearIslands(uint8_t *rgba, int stride, const Zone &z)
{
    const int zw = z.x1 - z.x0 + 1;
    const int zh = z.y1 - z.y0 + 1;
    std::vector<int> label(zw * zh, -1);
    std::vector<int> stack;
    std::vector<int> group;
    int n = 0;

    for (int start = 0; start < zw * zh; start++)
    {
        if ( label[start] >= 0 )
            continue;
        if ( !Opaque(Px(rgba, stride, z.x0 + start % zw, z.y0 + start / zw)) )
        {
            label[start] = 0;
            continue;
        }

        group.clear();
        stack.clear();
        stack.push_back(start);
        label[start] = 1;

        while ( !stack.empty() )
        {
            int i = stack.back();
            stack.pop_back();
            group.push_back(i);
            int cx = i % zw, cy = i / zw;

            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = cx + dx, ny = cy + dy;
                    if ( (dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= zw || ny >= zh )
                        continue;
                    int j = ny * zw + nx;
                    if ( label[j] >= 0 )
                        continue;
                    if ( Opaque(Px(rgba, stride, z.x0 + nx, z.y0 + ny)) )
                    {
                        label[j] = 1;
                        stack.push_back(j);
                    }
                    else
                        label[j] = 0;
                }
        }

        if ( (int)group.size() <= kIslandMax )
        {
            for (int i : group)
                Clear(Px(rgba, stride, z.x0 + i % zw, z.y0 + i / zw));
            n += (int)group.size();
        }
    }
    return n;
}

int CoverSignature(uint8_t *rgba, int stride, bool *skipped)
{
    // Check the whole donor window first; change nothing unless it is the
    // plain dark pattern.
    for (int y = kSigY0; y <= kSigY1; y++)
        for (int x = kDonorX0; x < kDonorX0 + kDonorPeriod; x++)
        {
            const uint8_t *p = Px(rgba, stride, x, y);
            if ( p[0] > kDonorMaxChannel || p[1] > kDonorMaxChannel || p[2] > kDonorMaxChannel )
            {
                *skipped = true;
                return 0;
            }
        }

    int n = 0;
    for (int y = kSigY0; y <= kSigY1; y++)
        for (int x = 0; x <= kSigX1; x++)
        {
            uint8_t *d = Px(rgba, stride, x, y);
            const uint8_t *s = Px(rgba, stride, kDonorX0 + x % kDonorPeriod, y);
            if ( memcmp(d, s, 4) != 0 )
            {
                memcpy(d, s, 4);
                n++;
            }
        }
    return n;
}

}

Sheet Classify(const std::string &name)
{
    const std::string s = Stem(name);
    if ( s == "city1" )
        return SHEET_CITY1;
    if ( s == "shrttwnd" || s == "fhrttwnd" )
        return SHEET_SIGNATURE;
    return SHEET_OTHER;
}

uint64_t HashRGBA(const uint8_t *rgba, int w, int h, int stride)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = rgba + (size_t)y * stride;
        for (int i = 0; i < w * 4; i++)
        {
            hash ^= row[i];
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

Result Apply(Sheet sheet, uint8_t *rgba, int w, int h, int stride,
             bool keepSignature, int *changed)
{
    return ApplyWithHashes(kCity1WarmHash, kCity1NightHash, sheet, rgba, w, h,
                           stride, keepSignature, changed);
}

Result ApplyWithHashes(uint64_t warmHash, uint64_t nightHash, Sheet sheet,
                       uint8_t *rgba, int w, int h, int stride,
                       bool keepSignature, int *changed)
{
    if ( changed )
        *changed = 0;

    if ( !rgba || w != kSheet || h != kSheet || stride < w * 4 )
        return UNCHANGED;

    if ( sheet == SHEET_CITY1 )
    {
        const uint64_t hash = HashRGBA(rgba, w, h, stride);
        Result r;
        if ( hash == warmHash )
            r = CITY1_WARM;
        else if ( hash == nightHash )
            r = CITY1_NIGHT;
        else
            return UNCHANGED; // another building set, or already cleaned

        int n = ClearColumns(rgba, stride, kInkColumns);
        for (const Zone &z : kZones)
            n += ClearIslands(rgba, stride, z);
        if ( changed )
            *changed = n;
        return n ? r : UNCHANGED;
    }

    if ( sheet == SHEET_SIGNATURE )
    {
        if ( keepSignature )
            return SIGNATURE_KEPT;
        bool skipped = false;
        int n = CoverSignature(rgba, stride, &skipped);
        if ( skipped )
            return SIGNATURE_SKIPPED;
        if ( changed )
            *changed = n;
        return n ? SIGNATURE_COVERED : UNCHANGED;
    }

    return UNCHANGED;
}

const char *Describe(Result r)
{
    switch ( r )
    {
    case CITY1_WARM:        return "city1 tags removed (daylight set)";
    case CITY1_NIGHT:       return "city1 tags removed (night set)";
    case SIGNATURE_COVERED: return "wall signature covered";
    case SIGNATURE_KEPT:    return "wall signature kept (Wall Signatures on)";
    case SIGNATURE_SKIPPED: return "wall signature left alone (not the expected sheet)";
    default:                return "nothing to do";
    }
}

}
