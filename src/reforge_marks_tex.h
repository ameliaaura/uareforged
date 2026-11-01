#ifndef REFORGE_MARKS_TEX_H_INCLUDED
#define REFORGE_MARKS_TEX_H_INCLUDED

// Reforge MARKS: the battle-marks atlas, generated procedurally
// at run time (no image files, no copied or generated art). Header-only so the
// same code also builds the look-dev CLI (tools/reforge/marks_atlas.cpp).
//
// Atlas = 4 x 4 cells of N/4 px (N = 1024 at levels 2-3, 512 at level 1),
// straight (non-premultiplied) RGBA, every cell fades to alpha 0 at its border
// (mips never bleed between cells):
//   row 0: scorch 0..3       (soot, blast streaks, ash flecks)
//   row 1: crater 0..1, track 0..1 (track = one tread-pair tile along v)
//   row 2: smoke puff 0..3   (alpha; rgb = soft top-lit shading, tinted by vertex colour)
//   row 3: ember, spare

#include <stdint.h>
#include <math.h>
#include <vector>

namespace ReforgeMarksTex
{

enum
{
    CELL_SCORCH = 0,  // + 0..3
    CELL_CRATER = 4,  // + 0..1
    CELL_TRACK = 6,   // + 0..1
    CELL_SMOKE = 8,   // + 0..3
    CELL_EMBER = 12,
};

inline uint32_t H(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}
inline float H01(uint32_t x) { return (H(x) & 0xffffff) / 16777215.0f; }
inline float Lattice(int x, int y, uint32_t s) { return H01((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ s * 83492791u); }
inline float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float SStep(float a, float b, float x) { return Smooth(Clamp01((x - a) / (b - a))); }
inline float Mix(float a, float b, float t) { return a + (b - a) * t; }

inline float Noise(float x, float y, uint32_t s)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = Smooth(x - ix), fy = Smooth(y - iy);
    float a = Lattice(ix, iy, s), b = Lattice(ix + 1, iy, s), c = Lattice(ix, iy + 1, s), d = Lattice(ix + 1, iy + 1, s);
    return Mix(Mix(a, b, fx), Mix(c, d, fx), fy);
}
inline float Fbm(float x, float y, uint32_t s, int oct = 5)
{
    float v = 0.0f, a = 0.5f, n = 0.0f;
    for (int i = 0; i < oct; ++i)
    {
        v += a * Noise(x, y, s + i * 1013u);
        n += a;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f + 9.7f;
        a *= 0.5f;
    }
    return v / n;
}
// Periodic 1D noise over the angle (n cells around the circle).
inline float AngNoise(float th, int n, uint32_t s)
{
    float x = (th / 6.2831853f + 0.5f) * n;
    int i = (int)floorf(x);
    float f = Smooth(x - i);
    int a = ((i % n) + n) % n, b = (a + 1) % n;
    return Mix(H01(a * 2654435761u ^ s), H01(b * 2654435761u ^ s), f);
}

struct Px { float r, g, b, a; };

// Scorch: dark soot core, blotchy irregular edge (domain-warped), a few soft blast streaks,
// mottled ash. No sprinkles (look rule: battle-worn realism, nothing toy-like).
inline Px Scorch(float u, float v, uint32_t s)
{
    const float wx = Fbm(u * 2.2f + 1.3f, v * 2.2f, s + 1) - 0.5f, wy = Fbm(u * 2.2f, v * 2.2f + 4.1f, s + 2) - 0.5f;
    const float uw = (u + 0.16f * wx) * 0.82f, vw = (v + 0.16f * wy) * 0.82f;
    const float rho = sqrtf(uw * uw + vw * vw), th = atan2f(vw, uw);
    const float rho0 = sqrtf(u * u + v * v);
    // soft streaks: few, wide at the root, broken up
    // streaks taper outwards: the exponent grows with the radius, so a wedge of the angular noise becomes a spike
    const float sr = Clamp01((AngNoise(th, 17, s + 3) - 0.45f) / 0.55f);
    const float sr2 = Clamp01((AngNoise(th, 37, s + 5) - 0.55f) / 0.45f);
    float sp = powf(sr, 1.2f + 6.0f * rho);
    float sp2 = powf(sr2, 1.5f + 7.0f * rho);
    const float bu = Fbm(u * 4.0f + 3.0f, v * 4.0f, s + 11);
    const float mott = Fbm(u * 11.0f, v * 11.0f, s + 29, 4);
    const float edge = 0.56f + 0.10f * (bu - 0.5f);
    float core = 1.0f - SStep(0.0f, edge, rho);
    core = 0.35f * core + 0.65f * powf(core, 0.5f) * (1.0f - SStep(edge * 0.55f, edge, rho)) ;
    const float reach = edge + 0.50f * sp + 0.30f * sp2;
    float streak = (1.0f - SStep(edge * 0.6f, reach, rho)) * (0.75f * sp + 0.45f * sp2);
    streak *= 0.6f + 0.6f * Fbm(u * 7.0f, v * 7.0f, s + 13, 3);
    // faint soot haze around it all
    const float haze = (1.0f - SStep(0.35f, 0.98f, rho0)) * 0.26f;
    float d = fmaxf(fmaxf(core, streak), haze);
    d *= 0.50f + 0.75f * mott;
    Px p;
    const float t = Clamp01(rho / 0.9f);
    const float k = 0.75f + 0.5f * mott;
    // Lighter, dustier look: sooty brown-grey core, dusty ochre-grey edge, ash pan; less opaque
    p.r = Mix(0.095f, 0.27f, t * t) * k;
    p.g = Mix(0.085f, 0.23f, t * t) * k;
    p.b = Mix(0.074f, 0.175f, t * t) * k;
    // light ash-grey dust pan in the middle, mottled
    const float pan = (1.0f - SStep(0.04f, 0.26f + 0.08f * bu, rho0)) * Clamp01((mott - 0.30f) * 2.0f);
    p.r = Mix(p.r, 0.33f, pan * 0.65f); p.g = Mix(p.g, 0.31f, pan * 0.65f); p.b = Mix(p.b, 0.28f, pan * 0.65f);
    p.a = Clamp01(d) * 0.80f;
    return p;
}

// Crater: shaded bowl + raised rim of thrown-up earth + scattered clods.
// Light from the texture's -u/-v corner (the decal is laid without rotation).
inline Px Crater(float u, float v, uint32_t s)
{
    const float th = atan2f(v, u);
    const float wob = 0.12f * (AngNoise(th, 7, s) - 0.5f) + 0.08f * (Fbm(u * 4.0f, v * 4.0f, s + 3) - 0.5f);
    auto height = [&](float x, float y) {
        float r = sqrtf(x * x + y * y) - wob;
        float h = 0.0f;
        if (r < 0.42f) h = -0.55f * (1.0f - (r / 0.42f) * (r / 0.42f));
        const float rimk = 0.55f + 0.9f * Fbm(x * 5.0f + 2.0f, y * 5.0f, s + 23, 3);
        h += 0.30f * rimk * expf(-((r - 0.47f) / 0.09f) * ((r - 0.47f) / 0.09f));
        h += 0.10f * expf(-((r - 0.62f) / 0.12f) * ((r - 0.62f) / 0.12f));
        h += 0.05f * (Fbm(x * 9.0f, y * 9.0f, s + 17, 4) - 0.5f);
        return h;
    };
    const float e = 0.01f;
    const float hx = (height(u + e, v) - height(u - e, v)) / (2 * e);
    const float hy = (height(u, v + e) - height(u, v - e)) / (2 * e);
    float nx = -hx * 0.35f, ny = -hy * 0.35f, nz = 1.0f;
    const float nl = sqrtf(nx * nx + ny * ny + nz * nz);
    nx /= nl; ny /= nl; nz /= nl;
    const float lx = -0.55f, ly = -0.45f, lz = 0.70f;
    const float sh = Clamp01(nx * lx + ny * ly + nz * lz);  // 0..1, flat = 0.70
    const float rho = sqrtf(u * u + v * v) - wob;
    const float fine = Fbm(u * 22.0f, v * 22.0f, s + 31, 3);
    Px p;
    // bowl: dark scorched soil; rim: dry disturbed earth; outside: clods
    const float bowl = 1.0f - SStep(0.36f, 0.46f, rho);
    float br = Mix(0.25f, 0.17f, bowl), bg = Mix(0.205f, 0.14f, bowl), bb = Mix(0.155f, 0.105f, bowl);
    const float lit = 0.62f + 0.75f * sh * (0.8f + 0.4f * fine);
    p.r = br * lit; p.g = bg * lit; p.b = bb * lit;
    float a = 0.0f;
    if (rho < 0.56f)
        a = Mix(0.80f, 0.93f, SStep(0.2f, 0.5f, rho));
    else
        a = 0.93f * (1.0f - SStep(0.56f, 0.74f + 0.1f * Fbm(u * 6.0f, v * 6.0f, s + 5), rho));
    // clods / ejecta specks out to the edge
    // ejecta: soft dark earth spray, then a few bigger clods
    const float spray = (1.0f - SStep(0.5f, 0.92f, rho)) * Clamp01((Fbm(u * 7.0f, v * 7.0f, s + 61, 4) - 0.42f) * 2.2f);
    if (rho >= 0.56f)
    {
        const float k = 0.8f + 0.3f * fine;
        p.r = Mix(p.r, 0.13f * k, spray); p.g = Mix(p.g, 0.11f * k, spray); p.b = Mix(p.b, 0.09f * k, spray);
        a = fmaxf(a, 0.75f * spray);
    }
    const float cl = Noise(u * 18.0f, v * 18.0f, s + 51);
    if (rho > 0.55f && rho < 0.9f && cl > 0.86f)
    {
        const float k = 0.5f + 0.5f * Noise(u * 34.0f + 0.5f, v * 34.0f + 0.5f, s + 52);
        p.r = 0.17f * k + 0.05f; p.g = 0.14f * k + 0.045f; p.b = 0.11f * k + 0.04f;
        a = fmaxf(a, 0.8f * (1.0f - SStep(0.8f, 0.95f, rho)));
    }
    p.a = Clamp01(a * (0.85f + 0.2f * fine));
    return p;
}

// Track tile: two tread bands along v (u = across), transverse cleats, crushed edges.
inline Px Track(float u, float v, uint32_t s)
{
    // u -1..1 across, v -1..1 along. Bands centred at +-0.55, width 0.36.
    const float au = fabsf(u);
    const float band = SStep(0.80f, 0.70f, au) * SStep(0.30f, 0.40f, au);
    const float lip = expf(-((au - 0.83f) / 0.04f) * ((au - 0.83f) / 0.04f)) + expf(-((au - 0.27f) / 0.04f) * ((au - 0.27f) / 0.04f));
    const float cle = 0.5f + 0.5f * cosf((v + 1.0f) * 3.14159265f * 4.0f); // 4 cleats per tile -> tiles seamlessly
    const float n = Fbm(u * 8.0f, (v + 1.0f) * 8.0f, s + 3, 4);
    const float vfade = 1.0f - SStep(0.92f, 1.0f, fabsf(v)) * 0.0f; // tiles end to end (no fade along v)
    Px p;
    const float k = 0.75f + 0.5f * n;
    p.r = 0.11f * k; p.g = 0.095f * k; p.b = 0.08f * k;
    // cleat ridges a touch darker/denser
    float a = band * (0.30f + 0.28f * cle) + lip * 0.10f;
    a *= 0.6f + 0.6f * n;
    p.a = Clamp01(a) * vfade;
    return p;
}

// Smoke puff: soft billowy blob, rgb carries top-lit shading.
inline Px Smoke(float u, float v, uint32_t s)
{
    const float rho = sqrtf(u * u + v * v);
    const float n = Fbm(u * 2.6f + 5.0f, v * 2.6f, s + 7, 5);
    const float m = Fbm(u * 6.0f, v * 6.0f + 3.0f, s + 9, 4);
    const float r = 0.62f + 0.30f * (n - 0.5f) * 2.0f;
    float a = 1.0f - SStep(r * 0.25f, r, rho);
    a *= 0.65f + 0.55f * m;
    // top lit (v < 0 = up on screen in view space? the billboard's +v is up): shading from v
    const float sh = Clamp01(0.70f + 0.30f * (-v) + 0.25f * (m - 0.5f));
    Px p;
    p.r = sh; p.g = sh; p.b = sh;
    p.a = Clamp01(a) * 0.95f;
    return p;
}

inline Px Ember(float u, float v, uint32_t)
{
    const float rho = sqrtf(u * u + v * v);
    const float core = expf(-rho * rho / 0.010f);
    const float halo = expf(-rho * rho / 0.09f);
    Px p;
    p.r = 1.0f; p.g = Clamp01(0.42f + 0.5f * core); p.b = Clamp01(0.10f + 0.5f * core);
    p.a = Clamp01(core + 0.45f * halo);
    return p;
}

// Fill an N x N RGBA8 atlas.
inline void Build(std::vector<uint8_t> &out, int N)
{
    out.assign((size_t)N * N * 4, 0);
    const int C = N / 4;
    for (int cell = 0; cell < 16; ++cell)
    {
        const int cx = (cell % 4) * C, cy = (cell / 4) * C;
        for (int y = 0; y < C; ++y)
            for (int x = 0; x < C; ++x)
            {
                // cell coords -1..1 with a 2 px transparent border
                const float u = ((x + 0.5f) / C) * 2.0f - 1.0f;
                const float v = ((y + 0.5f) / C) * 2.0f - 1.0f;
                Px p = {0, 0, 0, 0};
                const float uu = u * 1.04f, vv = v * 1.04f; // keep content inside the border
                if (cell < CELL_CRATER)
                    p = Scorch(uu, vv, 101u + cell * 977u);
                else if (cell < CELL_TRACK)
                    p = Crater(uu, vv, 307u + cell * 577u);
                else if (cell < CELL_SMOKE)
                    p = Track(u, v, 509u + cell * 311u);
                else if (cell < CELL_EMBER)
                    p = Smoke(uu, vv, 701u + cell * 433u);
                else if (cell == CELL_EMBER)
                    p = Ember(uu, vv, 0);
                if (cell < CELL_TRACK || cell >= CELL_SMOKE)
                {
                    // round cells: alpha 0 outside the unit circle
                    const float rho = sqrtf(uu * uu + vv * vv);
                    p.a *= 1.0f - SStep(0.96f, 1.0f, rho);
                }
                if (x < 2 || y < 2 || x >= C - 2 || y >= C - 2)
                    p.a = 0.0f;
                if (cell >= CELL_TRACK && cell < CELL_SMOKE && (y < 2 || y >= C - 2))
                    p.a = p.a; // track tiles keep their v ends (they abut); u border handled above
                uint8_t *o = &out[((size_t)(cy + y) * N + (cx + x)) * 4];
                o[0] = (uint8_t)(Clamp01(p.r) * 255.0f + 0.5f);
                o[1] = (uint8_t)(Clamp01(p.g) * 255.0f + 0.5f);
                o[2] = (uint8_t)(Clamp01(p.b) * 255.0f + 0.5f);
                o[3] = (uint8_t)(Clamp01(p.a) * 255.0f + 0.5f);
            }
    }
}

} // namespace ReforgeMarksTex

#endif
