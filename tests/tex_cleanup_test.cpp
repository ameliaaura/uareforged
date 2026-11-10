// The graffiti and signature clean-up on the original city sheets (src/tex_cleanup.cpp).
// Made-up images only, no game data, no SDL:
//   c++ -std=c++14 -Wall -Isrc tests/tex_cleanup_test.cpp src/tex_cleanup.cpp -o /tmp/tct && /tmp/tct
#include "tex_cleanup.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace TexCleanup;

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { checks++; if ( !(cond) ) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const int N = 256;

struct Img
{
    int stride;
    std::vector<uint8_t> px;

    explicit Img(int pad = 0) : stride(N * 4 + pad), px((size_t)stride * N, 0) {}

    uint8_t *at(int x, int y) { return &px[(size_t)y * stride + (size_t)x * 4]; }

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        uint8_t *p = at(x, y);
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }

    void rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                set(x, y, r, g, b, a);
    }

    bool opaque(int x, int y) { return at(x, y)[3] != 0; }

    uint64_t hash() { return HashRGBA(px.data(), N, N, stride); }
};

// ---------------------------------------------------------------- names
static void TestClassify()
{
    CHECK(Classify("city1.ilbm") == SHEET_CITY1);
    CHECK(Classify("CITY1.ILBM") == SHEET_CITY1);
    CHECK(Classify("set1/city1.ilbm") == SHEET_CITY1);
    CHECK(Classify("fcity1.ilbm") == SHEET_OTHER);   // the battle-damaged sheet is clean
    CHECK(Classify("city10.ilbm") == SHEET_OTHER);
    CHECK(Classify("shrttwnd.ilbm") == SHEET_SIGNATURE);
    CHECK(Classify("FHRTTWND.ILBM") == SHEET_SIGNATURE);
    CHECK(Classify("hrttwnd.ilbm") == SHEET_OTHER);
    CHECK(Classify("berud.ilbm") == SHEET_OTHER);
    CHECK(Classify("") == SHEET_OTHER);
}

// ---------------------------------------------------------------- hash
static void TestHash()
{
    // FNV-1a 64 of nothing is the offset basis; of one pixel (1,2,3,4) a fixed number
    uint8_t one[4] = { 1, 2, 3, 4 };
    CHECK(HashRGBA(one, 0, 0, 0) == 0xcbf29ce484222325ULL);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 4; i++) { h ^= one[i]; h *= 0x100000001b3ULL; }
    CHECK(HashRGBA(one, 1, 1, 4) == h);

    // row padding is not part of the hash
    Img a(0), b(12);
    a.rect(10, 10, 20, 20, 200, 30, 30);
    b.rect(10, 10, 20, 20, 200, 30, 30);
    for (int y = 0; y < N; y++)
        std::memset(&b.px[(size_t)y * b.stride + N * 4], 0xAB, 12);
    CHECK(a.hash() == b.hash());

    // one byte different -> a different hash
    b.at(0, 0)[2] = 1;
    CHECK(a.hash() != b.hash());

    // the real constants are filled in (not the placeholder 0)
    CHECK(kCity1WarmHash != 0 && kCity1NightHash != 0 && kCity1WarmHash != kCity1NightHash);
}

// ---------------------------------------------------------------- city1
// A made-up "tagged" sheet: buildings (big opaque blocks) with transparent gaps, lettering in
// the gaps, and transparent padding whose colour bytes are not zero (they must stay as they are).
static Img MakeCity1()
{
    Img m;
    // transparent padding with leftover colour bytes
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            m.set(x, y, 7, 8, 9, 0);
    // buildings
    m.rect(40, 0, 239, 255, 120, 110, 100);   // big middle block: x40 and x239 are artwork
    m.rect(0, 0, 39, 115, 90, 90, 90);        // above zone A
    m.rect(240, 84, 249, 255, 80, 80, 80);    // facade strip below zone B (x <= 249)
    // lettering: columns 250-255 (touching the facade strip at x249, as on the real sheet)
    m.rect(250, 75, 253, 247, 230, 230, 230);
    m.set(254, 3, 230, 230, 230, 255);
    // zone A: a red box, letters, a 1-px dot
    // the GURU MEDITATION box: a 1-px red outline (182 px) with letters inside
    m.rect(1, 172, 16, 172, 220, 20, 20);
    m.rect(1, 248, 16, 248, 220, 20, 20);
    m.rect(1, 172, 1, 248, 220, 20, 20);
    m.rect(16, 172, 16, 248, 220, 20, 20);
    for (int y = 176; y <= 240; y += 8)
        m.rect(4, y, 13, y + 4, 220, 20, 20);
    // INSANE IN THE MEMBRANE: letter-sized pieces
    for (int y = 180; y <= 238; y += 7)
        m.rect(19, y, 30, y + 4, 240, 240, 240);
    m.set(5, 130, 255, 255, 0, 255);
    // zone B: glyphs
    m.rect(241, 3, 248, 20, 40, 60, 220);
    m.rect(243, 40, 247, 70, 40, 60, 220);
    // zone C: the night set's bottom line, in a gap along the bottom edge (on the real sheet
    // x 40-71 is empty padding for y 248-255)
    m.rect(40, 248, 71, 255, 7, 8, 9, 0);     // gap
    m.rect(45, 250, 66, 254, 200, 200, 255);
    return m;
}

static void TestCity1Gating()
{
    Img m = MakeCity1();
    std::vector<uint8_t> before = m.px;
    int changed = -1;

    // not one of the two known bitmaps -> untouched, byte for byte
    Result r = ApplyWithHashes(1, 2, SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed);
    CHECK(r == UNCHANGED);
    CHECK(changed == 0);
    CHECK(m.px == before);

    // the real game's constants don't match a made-up sheet either
    r = Apply(SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed);
    CHECK(r == UNCHANGED && m.px == before);

    // the right hash but a different sheet kind -> untouched
    r = ApplyWithHashes(m.hash(), 2, SHEET_OTHER, m.px.data(), N, N, m.stride, false, &changed);
    CHECK(r == UNCHANGED && m.px == before);

    // wrong size -> untouched
    r = ApplyWithHashes(HashRGBA(m.px.data(), 128, 128, m.stride), 2, SHEET_CITY1,
                        m.px.data(), 128, 128, m.stride, false, &changed);
    CHECK(r == UNCHANGED && m.px == before);
    CHECK(Apply(SHEET_CITY1, NULL, N, N, N * 4, false, &changed) == UNCHANGED);
}

static void TestCity1Rules()
{
    Img m = MakeCity1();
    Img orig = m;
    int changed = 0;

    Result r = ApplyWithHashes(9, m.hash(), SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed);
    CHECK(r == CITY1_NIGHT);
    CHECK(changed > 0);

    int counted = 0;
    bool onlyOpaqueToClear = true, onlyInZones = true;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
        {
            if ( std::memcmp(m.at(x, y), orig.at(x, y), 4) == 0 )
                continue;
            counted++;
            const uint8_t *a = m.at(x, y);
            if ( !orig.opaque(x, y) || a[0] || a[1] || a[2] || a[3] )
                onlyOpaqueToClear = false;
            bool inside = (x >= 250) ||
                          (x <= 39 && y >= 116) ||
                          (x >= 240 && x <= 249 && y <= 83) ||
                          (x <= 71 && y >= 248);
            if ( !inside )
                onlyInZones = false;
        }
    CHECK(counted == changed);
    CHECK(onlyOpaqueToClear);   // opaque -> (0,0,0,0), nothing else
    CHECK(onlyInZones);

    // all lettering gone
    for (int y = 0; y < N; y++)
        for (int x = 250; x < N; x++)
            CHECK(!m.opaque(x, y));
    CHECK(!m.opaque(1, 172) && !m.opaque(16, 248) && !m.opaque(30, 242) && !m.opaque(5, 130) && !m.opaque(8, 178));
    CHECK(!m.opaque(241, 3) && !m.opaque(247, 70));
    CHECK(!m.opaque(45, 250) && !m.opaque(66, 254));

    // artwork at the zone edges survives: x40 and x239 columns (above the bottom gap), the
    // facade strip at x249, the block above zone A
    bool artKept = true;
    for (int y = 0; y < N; y++)
    {
        for (int x : { 40, 41, 49, 232, 239 })
            if ( y <= 247 && std::memcmp(m.at(x, y), orig.at(x, y), 4) != 0 )
                artKept = false;
        if ( y >= 84 && std::memcmp(m.at(249, y), orig.at(249, y), 4) != 0 )
            artKept = false;
        if ( y <= 115 && std::memcmp(m.at(0, y), orig.at(0, y), 4) != 0 )
            artKept = false;
    }
    CHECK(artKept);

    // transparent padding keeps its colour bytes (only opaque pixels are ever changed)
    CHECK(m.at(35, 120)[0] == 7 && m.at(35, 120)[2] == 9 && m.at(35, 120)[3] == 0);
    CHECK(m.at(255, 0)[0] == 7 && m.at(255, 0)[3] == 0);
}

static void TestCity1IslandCap()
{
    // inside zone A (x0-39, y116-255): a 700-px island goes, a 701-px island stays
    Img m;
    m.rect(0, 120, 19, 154, 200, 0, 0);       // 20 x 35 = 700
    m.rect(22, 120, 39, 157, 0, 200, 0);      // 18 x 38 = 684 ...
    m.rect(22, 158, 38, 158, 0, 200, 0);      // ... + 17 = 701
    Img orig = m;
    int changed = 0;
    CHECK(ApplyWithHashes(m.hash(), 0, SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed) == CITY1_WARM);
    CHECK(changed == 700);
    CHECK(!m.opaque(0, 120) && !m.opaque(19, 154));
    CHECK(m.opaque(22, 120) && m.opaque(38, 158));

    // 8 neighbours: two 400-px blocks touching only at a corner make one 800-px group -> kept
    Img d;
    d.rect(0, 120, 19, 139, 1, 2, 3);         // 20 x 20 = 400, last pixel (19,139)
    d.rect(20, 140, 39, 159, 1, 2, 3);        // first pixel (20,140): corner contact
    CHECK(ApplyWithHashes(d.hash(), 0, SHEET_CITY1, d.px.data(), N, N, d.stride, false, &changed) == UNCHANGED);
    CHECK(changed == 0 && d.opaque(0, 120) && d.opaque(39, 159));

    // a large building that crosses into a zone: only its part inside the zone is looked at,
    // and that part (10 x 132 = 1320 px) is over the cap -> kept
    Img e;
    e.rect(30, 116, 80, 247, 50, 50, 50);   // (zone C, y 248-255, is empty on the real sheet)
    Img eo = e;
    CHECK(ApplyWithHashes(e.hash(), 0, SHEET_CITY1, e.px.data(), N, N, e.stride, false, &changed) == UNCHANGED);
    CHECK(e.px == eo.px);
}

static void TestCity1Idempotent()
{
    Img m = MakeCity1();
    int changed = 0;
    const uint64_t h = m.hash();
    CHECK(ApplyWithHashes(h, 0, SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed) == CITY1_WARM);
    std::vector<uint8_t> once = m.px;

    // a second load: the sheet no longer has the tagged hash -> nothing happens
    CHECK(ApplyWithHashes(h, 0, SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed) == UNCHANGED);
    CHECK(changed == 0 && m.px == once);

    // and even forced through the rules again (the clean sheet's own hash), nothing is left to do
    CHECK(ApplyWithHashes(m.hash(), 0, SHEET_CITY1, m.px.data(), N, N, m.stride, false, &changed) == UNCHANGED);
    CHECK(changed == 0 && m.px == once);
}

static void TestCity1Stride()
{
    // the same sheet with row padding gives the same result
    Img a = MakeCity1();
    Img b(16);
    for (int y = 0; y < N; y++)
        std::memcpy(b.at(0, y), a.at(0, y), N * 4);
    int ca = 0, cb = 0;
    ApplyWithHashes(a.hash(), 0, SHEET_CITY1, a.px.data(), N, N, a.stride, false, &ca);
    ApplyWithHashes(b.hash(), 0, SHEET_CITY1, b.px.data(), N, N, b.stride, false, &cb);
    CHECK(ca == cb && ca > 0);
    bool same = true;
    for (int y = 0; y < N; y++)
        if ( std::memcmp(a.at(0, y), b.at(0, y), N * 4) != 0 )
            same = false;
    CHECK(same);
    CHECK(a.hash() == b.hash());
}

// ---------------------------------------------------------------- signature
// The bottom strip: a dark checker of two colours by (x + y) parity, like the dither,
// with bright "writing" in x 1-92, y 252-255.
static uint8_t Dither(int x, int y) { return ((x + y) & 1) ? 40 : 12; }

static Img MakeSignatureSheet()
{
    Img m;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
        {
            if ( y >= 248 )
            {
                uint8_t v = Dither(x, y);
                m.set(x, y, v, (uint8_t)(v + 3), (uint8_t)(v + 5), 255);
            }
            else
                m.set(x, y, (uint8_t)(x ^ y), (uint8_t)(x + y), 150, 255);   // the wall above
        }
    for (int x = 1; x <= 92; x += 3)
        for (int y = 252; y <= 255; y++)
            if ( (x + y) % 2 )
                m.set(x, y, 250, 250, 250, 255);
    return m;
}

static void TestSignature()
{
    Img m = MakeSignatureSheet();
    Img orig = m;
    int changed = 0;

    Result r = Apply(SHEET_SIGNATURE, m.px.data(), N, N, m.stride, false, &changed);
    CHECK(r == SIGNATURE_COVERED);
    CHECK(changed > 0);

    // the box is the donor, period 50 from x 94
    bool period = true, seamless = true, bright = false, outside = true;
    for (int y = 252; y <= 255; y++)
        for (int x = 0; x <= 92; x++)
        {
            if ( std::memcmp(m.at(x, y), orig.at(94 + x % 50, y), 4) != 0 )
                period = false;
            // even offsets keep the checker phase: the filled box is the strip's own pattern
            if ( m.at(x, y)[0] != Dither(x, y) )
                seamless = false;
            if ( m.at(x, y)[0] > 51 || m.at(x, y)[1] > 51 || m.at(x, y)[2] > 51 )
                bright = true;
        }
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            if ( !(y >= 252 && x <= 92) && std::memcmp(m.at(x, y), orig.at(x, y), 4) != 0 )
                outside = false;
    CHECK(period);
    CHECK(seamless);
    CHECK(!bright);
    CHECK(outside);

    // twice = once
    std::vector<uint8_t> once = m.px;
    CHECK(Apply(SHEET_SIGNATURE, m.px.data(), N, N, m.stride, false, &changed) == UNCHANGED);
    CHECK(changed == 0 && m.px == once);
}

static void TestSignatureKeptAndSkipped()
{
    int changed = -1;

    // Wall Signatures on: left exactly as painted
    Img m = MakeSignatureSheet();
    std::vector<uint8_t> before = m.px;
    CHECK(Apply(SHEET_SIGNATURE, m.px.data(), N, N, m.stride, true, &changed) == SIGNATURE_KEPT);
    CHECK(changed == 0 && m.px == before);

    // a sheet whose donor strip is not dark (a modded texture with the same name): left alone
    Img b = MakeSignatureSheet();
    b.set(120, 253, 52, 10, 10, 255);
    before = b.px;
    CHECK(Apply(SHEET_SIGNATURE, b.px.data(), N, N, b.stride, false, &changed) == SIGNATURE_SKIPPED);
    CHECK(changed == 0 && b.px == before);

    // the donor's last column (x 143) counts, x 144 does not
    Img c = MakeSignatureSheet();
    c.set(144, 253, 255, 255, 255, 255);
    CHECK(Apply(SHEET_SIGNATURE, c.px.data(), N, N, c.stride, false, &changed) == SIGNATURE_COVERED);
    Img d = MakeSignatureSheet();
    d.set(143, 253, 255, 255, 255, 255);
    CHECK(Apply(SHEET_SIGNATURE, d.px.data(), N, N, d.stride, false, &changed) == SIGNATURE_SKIPPED);

    // other sheets and sizes are never touched
    Img e = MakeSignatureSheet();
    before = e.px;
    CHECK(Apply(SHEET_OTHER, e.px.data(), N, N, e.stride, false, &changed) == UNCHANGED);
    CHECK(Apply(SHEET_SIGNATURE, e.px.data(), 128, 256, e.stride, false, &changed) == UNCHANGED);
    CHECK(e.px == before);
}

static void TestDescribe()
{
    CHECK(std::string(Describe(CITY1_WARM)).find("city1 tags removed") == 0);
    CHECK(std::string(Describe(CITY1_NIGHT)).find("city1 tags removed") == 0);
    CHECK(std::string(Describe(SIGNATURE_COVERED)).size() > 0);
    CHECK(std::string(Describe(UNCHANGED)) == "nothing to do");
}

int main()
{
    TestClassify();
    TestHash();
    TestCity1Gating();
    TestCity1Rules();
    TestCity1IslandCap();
    TestCity1Idempotent();
    TestCity1Stride();
    TestSignature();
    TestSignatureKeptAndSkipped();
    TestDescribe();

    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
