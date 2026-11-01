/* uatexc - reforge map -> DDS (DX10 header) with a full mip chain.
 *
 *   uatexc <in.png> <out.dds> albedo|normal|orm [max_size]
 *
 *   albedo  BC1, DXGI BC1_UNORM_SRGB; mips box-filtered in LINEAR light (same as
 *           the engine's GL_SRGB8_ALPHA8 + glGenerateMipmap path).
 *   orm     BC1, DXGI BC1_UNORM; mips box-filtered per channel.
 *   normal  BC5 (RG only), DXGI BC5_UNORM; mips box-filtered and renormalised;
 *           the engine's V2 shader rebuilds z (NormalRG uniform).
 *   max_size (optional, e.g. 2048) drops the top levels until the base fits.
 *
 * Authored far-read mips (<stem>_mip<N>.png beside the input, consecutive from
 * 1, level N = max(1, W>>N)) replace the generated level N and seed the levels
 * below it, exactly like the engine's PNG path.
 *
 * Build: cc -O2 -o uatexc uatexc.c -lm      (stb_image / stb_dxt: public domain)
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_ALBEDO, K_NORMAL, K_ORM };

typedef struct { int w, h; uint8_t *px; } Img; /* RGBA8 */

static float lin_of[256];
static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static uint8_t lin_to_srgb8(float l)
{
    float c = l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    int v = (int)lrintf(c * 255.0f);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static Img half(const Img *s, int kind)
{
    Img d;
    d.w = s->w > 1 ? s->w / 2 : 1;
    d.h = s->h > 1 ? s->h / 2 : 1;
    d.px = (uint8_t *)malloc((size_t)d.w * d.h * 4);
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x)
        {
            int xs[2] = {x * 2 < s->w ? x * 2 : s->w - 1, x * 2 + 1 < s->w ? x * 2 + 1 : s->w - 1};
            int ys[2] = {y * 2 < s->h ? y * 2 : s->h - 1, y * 2 + 1 < s->h ? y * 2 + 1 : s->h - 1};
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const uint8_t *p = s->px + ((size_t)ys[j] * s->w + xs[i]) * 4;
                    for (int c = 0; c < 4; ++c)
                        acc[c] += (kind == K_ALBEDO && c < 3) ? lin_of[p[c]] : (float)p[c];
                }
            uint8_t *q = d.px + ((size_t)y * d.w + x) * 4;
            if (kind == K_ALBEDO)
            {
                for (int c = 0; c < 3; ++c)
                    q[c] = lin_to_srgb8(acc[c] * 0.25f);
                q[3] = (uint8_t)lrintf(acc[3] * 0.25f);
            }
            else if (kind == K_NORMAL)
            {
                float n[3];
                for (int c = 0; c < 3; ++c)
                    n[c] = acc[c] * 0.25f / 127.5f - 1.0f;
                float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l < 1e-6f) { n[0] = 0; n[1] = 0; n[2] = 1; l = 1; }
                for (int c = 0; c < 3; ++c)
                {
                    int v = (int)lrintf((n[c] / l + 1.0f) * 127.5f);
                    q[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                q[3] = 255;
            }
            else
                for (int c = 0; c < 4; ++c)
                    q[c] = (uint8_t)lrintf(acc[c] * 0.25f);
        }
    return d;
}

static int load(const char *path, Img *o)
{
    int n;
    o->px = stbi_load(path, &o->w, &o->h, &n, 4);
    return o->px != NULL;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: uatexc <in.png> <out.dds> albedo|normal|orm [max_size]\n");
        return 2;
    }
    const char *in = argv[1], *out = argv[2];
    int kind = !strcmp(argv[3], "albedo") ? K_ALBEDO : !strcmp(argv[3], "normal") ? K_NORMAL : !strcmp(argv[3], "orm") ? K_ORM : -1;
    if (kind < 0) { fprintf(stderr, "bad kind %s\n", argv[3]); return 2; }
    int maxSize = argc > 4 ? atoi(argv[4]) : 0;
    for (int i = 0; i < 256; ++i)
        lin_of[i] = srgb_to_lin(i / 255.0f);

    /* Full chain from the source, honouring authored _mipN levels. */
    Img lv[16];
    int n = 0, authored = 0;
    if (!load(in, &lv[0])) { fprintf(stderr, "cannot read %s\n", in); return 1; }
    n = 1;
    char stem[4096];
    snprintf(stem, sizeof(stem), "%s", in);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    int chainOpen = 1;
    while ((lv[n - 1].w > 1 || lv[n - 1].h > 1) && n < 16)
    {
        Img m;
        int got = 0;
        if (chainOpen)
        {
            char mp[4200];
            snprintf(mp, sizeof(mp), "%s_mip%d.png", stem, n);
            FILE *pr = fopen(mp, "rb");
            if (pr)
            {
                fclose(pr);
                int ew = lv[0].w >> n, eh = lv[0].h >> n;
                if (ew < 1) ew = 1;
                if (eh < 1) eh = 1;
                if (load(mp, &m) && m.w == ew && m.h == eh) { got = 1; authored++; }
                else chainOpen = 0;
            }
            else
                chainOpen = 0;
        }
        if (!got)
            m = half(&lv[n - 1], kind);
        lv[n++] = m;
    }

    int first = 0;
    while (maxSize > 0 && (lv[first].w > maxSize || lv[first].h > maxSize) && first < n - 1)
        first++;

    /* Encode. */
    const int bc5 = kind == K_NORMAL;
    const int blockBytes = bc5 ? 16 : 8;
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    const int W = lv[first].w, H = lv[first].h, levels = n - first;
    fwrite("DDS ", 1, 4, f);
    put32(f, 124);
    put32(f, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
    put32(f, H);
    put32(f, W);
    put32(f, ((W + 3) / 4) * ((H + 3) / 4) * blockBytes);
    put32(f, 0);
    put32(f, levels);
    for (int i = 0; i < 11; ++i) put32(f, 0);
    put32(f, 32); put32(f, 0x4); put32(f, 0x30315844); /* DDPF_FOURCC 'DX10' */
    for (int i = 0; i < 5; ++i) put32(f, 0);
    put32(f, 0x1000 | 0x400000 | 0x8);
    for (int i = 0; i < 4; ++i) put32(f, 0);
    put32(f, bc5 ? 83 : (kind == K_ALBEDO ? 72 : 71));
    put32(f, 3); put32(f, 0); put32(f, 1); put32(f, 0);

    size_t total = 0;
    for (int l = first; l < n; ++l)
    {
        const Img *im = &lv[l];
        for (int by = 0; by < (im->h + 3) / 4; ++by)
            for (int bx = 0; bx < (im->w + 3) / 4; ++bx)
            {
                uint8_t src[64], rg[32], blk[16];
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                    {
                        int x = bx * 4 + i < im->w ? bx * 4 + i : im->w - 1;
                        int y = by * 4 + j < im->h ? by * 4 + j : im->h - 1;
                        const uint8_t *p = im->px + ((size_t)y * im->w + x) * 4;
                        memcpy(src + (j * 4 + i) * 4, p, 4);
                        rg[(j * 4 + i) * 2] = p[0];
                        rg[(j * 4 + i) * 2 + 1] = p[1];
                    }
                if (bc5)
                    stb_compress_bc5_block(blk, rg);
                else
                    stb_compress_dxt_block(blk, src, 0, STB_DXT_HIGHQUAL);
                fwrite(blk, 1, blockBytes, f);
                total += blockBytes;
            }
    }
    fclose(f);
    printf("%s: %dx%d %s, %d levels (%d authored), %.2f MB\n", out, W, H,
           bc5 ? "BC5" : "BC1", levels, authored, total / 1048576.0);
    return 0;
}
