// Reforge ATMOSPHERE night windows on HD (V2) trims: window detection + DDS BC1 mip decode (see atmos_winmask.h).
// Compiled at -O2 on its own (CMakeLists: the engine builds at -O0; at -O0 this took ~45 ms per 512 px trim at
// level load, ~6 ms at -O2). Pure CPU code, no GL, no state.
#include "atmos_winmask.h"

#include <algorithm>
#include <math.h>

namespace AtmosWin
{
static float Ss(float e0, float e1, float x)
{
    float t = std::min(1.0f, std::max(0.0f, (x - e0) / (e1 - e0)));
    return t * t * (3.0f - 2.0f * t);
}


// rgba: w x h RGBA8 (stored encoding = display), row 0 = first row in GL memory. out: w x h RGBA8 mask.
// One mip of a DDS file holding BC1 (DX10 DXGI 71/72 or FourCC DXT1) decoded to RGBA8 (stored encoding): the
// trims' .dds (2048 px BC1 sRGB, full chain). Picks the first level with w <= maxW. Reading the mip back from GL
// instead cost ~200 ms per BC1 trim at level load (driver-side decode); this is ~1 ms. false = not BC1 / short.
bool DecodeDdsBc1Mip(const std::vector<uint8_t> &f, int maxW, std::vector<uint8_t> &rgba, int &w, int &h)
{
    auto u32 = [&](size_t o) { return (uint32_t)f[o] | (uint32_t)f[o + 1] << 8 | (uint32_t)f[o + 2] << 16 | (uint32_t)f[o + 3] << 24; };
    if (f.size() < 128 || u32(0) != 0x20534444u) // "DDS "
        return false;
    h = (int)u32(12);
    w = (int)u32(16);
    const uint32_t mips = std::max(1u, u32(28));
    const uint32_t four = u32(84);
    size_t off = 128;
    if (four == 0x30315844u) // "DX10"
    {
        if (f.size() < 148)
            return false;
        const uint32_t dxgi = u32(128);
        if (dxgi != 71 && dxgi != 72)
            return false;
        off = 148;
    }
    else if (four != 0x31545844u) // "DXT1"
        return false;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return false;
    uint32_t lv = 0;
    while (w > maxW && lv + 1 < mips)
    {
        off += (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 8;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        ++lv;
    }
    const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    if (w > maxW || off + (size_t)bw * bh * 8 > f.size())
        return false;
    rgba.assign((size_t)w * h * 4, 255);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx)
        {
            const uint8_t *b = &f[off + ((size_t)by * bw + bx) * 8];
            const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            int pal[4][3];
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t c = k ? c1 : c0;
                pal[k][0] = ((c >> 11) & 31) * 255 / 31;
                pal[k][1] = ((c >> 5) & 63) * 255 / 63;
                pal[k][2] = (c & 31) * 255 / 31;
            }
            for (int ch = 0; ch < 3; ++ch)
            {
                if (c0 > c1)
                {
                    pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
                    pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
                }
                else
                {
                    pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
                    pal[3][ch] = 0;
                }
            }
            const uint32_t idx = u32(off + ((size_t)by * bw + bx) * 8 + 4);
            for (int py = 0; py < 4; ++py)
                for (int px = 0; px < 4; ++px)
                {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x >= w || y >= h)
                        continue;
                    const int i = (idx >> (2 * (py * 4 + px))) & 3;
                    uint8_t *o = &rgba[((size_t)y * w + x) * 4];
                    o[0] = (uint8_t)pal[i][0];
                    o[1] = (uint8_t)pal[i][1];
                    o[2] = (uint8_t)pal[i][2];
                    o[3] = (c0 <= c1 && i == 3) ? 0 : 255;
                }
        }
    return true;
}

Stats BuildTrimMask(const uint8_t *rgba, int w, int h, std::vector<uint8_t> &out)
{
    Stats st;
    const size_t n = (size_t)w * h;
    std::vector<float> L(n);
    for (size_t i = 0; i < n; ++i)
        L[i] = (0.299f * rgba[i * 4] + 0.587f * rgba[i * 4 + 1] + 0.114f * rgba[i * 4 + 2]) / 255.0f;
    // summed-area table, wrapped in x (trims tile horizontally) by clamping - edges only matter at the border
    std::vector<double> sat((size_t)(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y)
    {
        double row = 0.0;
        for (int x = 0; x < w; ++x)
        {
            row += L[(size_t)y * w + x];
            sat[(size_t)(y + 1) * (w + 1) + x + 1] = sat[(size_t)y * (w + 1) + x + 1] + row;
        }
    }
    auto box = [&](int x, int y, int r) {
        int x0 = std::max(0, x - r), y0 = std::max(0, y - r), x1 = std::min(w, x + r + 1), y1 = std::min(h, y + r + 1);
        double s = sat[(size_t)y1 * (w + 1) + x1] - sat[(size_t)y0 * (w + 1) + x1] - sat[(size_t)y1 * (w + 1) + x0] +
                   sat[(size_t)y0 * (w + 1) + x0];
        return (float)(s / ((x1 - x0) * (y1 - y0)));
    };
    const int rb = std::max(6, w / 16); // wall level: ~2 windows wide (32 px at 512)
    std::vector<float> wall(n), dk(n);
    std::vector<uint8_t> dark(n, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const size_t i = (size_t)y * w + x;
            const float lb = box(x, y, rb);
            wall[i] = lb;
            // pane: clearly darker than the wall around it, dark in absolute terms, on a lit wall
            const float d = Ss(0.62f, 0.42f, L[i] / (lb + 0.02f)) * Ss(0.36f, 0.20f, L[i]) * Ss(0.12f, 0.22f, lb);
            dk[i] = d;
            dark[i] = d > 0.35f ? 1 : 0;
        }
    // close mullions / frames: 1 px dilation (3x3)
    std::vector<uint8_t> dil(n, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint8_t v = 0;
            for (int dy = -1; dy <= 1 && !v; ++dy)
                for (int dx = -1; dx <= 1 && !v; ++dx)
                {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h && dark[(size_t)yy * w + xx])
                        v = 1;
                }
            dil[(size_t)y * w + x] = v;
        }
    // connected components (4-neighbour) of the dilated mask
    std::vector<int> lab(n, -1), stack;
    out.assign(n * 4, 0);
    const int minS = std::max(3, w / 64), maxS = std::max(minS + 1, w / 6);
    std::vector<int> pix;
    int nComp = 0;
    for (size_t s = 0; s < n; ++s)
    {
        if (!dil[s] || lab[s] >= 0)
            continue;
        pix.clear();
        stack.assign(1, (int)s);
        lab[s] = nComp;
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        double wallSum = 0.0;
        while (!stack.empty())
        {
            int p = stack.back();
            stack.pop_back();
            pix.push_back(p);
            int px = p % w, py = p / w;
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
            wallSum += wall[p];
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto &d : nb)
            {
                int xx = px + d[0], yy = py + d[1];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h)
                    continue;
                size_t q = (size_t)yy * w + xx;
                if (dil[q] && lab[q] < 0)
                {
                    lab[q] = nComp;
                    stack.push_back((int)q);
                }
            }
        }
        ++nComp;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const float fill = (float)pix.size() / (float)(bw * bh);
        const float aspect = (float)bh / (float)bw;
        const float wallMean = (float)(wallSum / pix.size());
        if (bw < minS || bh < minS || bw > maxS || bh > maxS || fill < 0.80f || aspect < 0.3f || aspect > 4.0f ||
            wallMean < 0.18f)
        {
            ++st.rejected;
            continue;
        }
        ++st.windows;
        // one hash per window (its box centre in texture fractions: resolution-independent)
        const float cu = (x0 + x1 + 1) * 0.5f / w, cv = (y0 + y1 + 1) * 0.5f / h;
        float hs = sinf(cu * 1291.9898f + cv * 7823.3f) * 43758.5453f;
        hs -= floorf(hs);
        int ch = -1;
        if (hs < 0.145f) ch = 0;
        else if (hs < 0.167f) ch = 2;
        else if (hs >= 0.5f && hs < 0.645f) ch = 1;
        else if (hs >= 0.645f && hs < 0.667f) ch = 3;
        if (ch < 0)
            continue;
        for (int p : pix)
        {
            // pane pixels glow (darker pane = more light), frames / mullions stay mostly dark
            const float k = std::min(1.0f, dk[p] * 1.15f);
            out[(size_t)p * 4 + ch] = (uint8_t)(255.0f * k);
        }
    }
    return st;
}
} // namespace AtmosWin
