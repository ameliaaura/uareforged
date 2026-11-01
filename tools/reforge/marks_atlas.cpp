// marks_atlas <out_prefix> [N]  (MARKS) look-dev: writes the procedural marks atlas
// <prefix>_rgba.pam (straight RGBA) and <prefix>_over.ppm (composited over three ground tones:
// khaki / green / blue-grey bands, so the decals can be judged on UA's set palettes).
#include "../../src/reforge_marks_tex.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: marks_atlas <prefix> [N]\n"); return 1; }
    int N = argc > 2 ? atoi(argv[2]) : 1024;
    std::vector<uint8_t> a;
    ReforgeMarksTex::Build(a, N);
    char fn[1024];
    snprintf(fn, sizeof fn, "%s_rgba.pam", argv[1]);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", N, N);
    fwrite(a.data(), 1, a.size(), f); fclose(f);
    snprintf(fn, sizeof fn, "%s_over.ppm", argv[1]);
    f = fopen(fn, "wb");
    fprintf(f, "P6\n%d %d\n255\n", N, N);
    const float bg[3][3] = {{0.46f, 0.40f, 0.30f}, {0.30f, 0.38f, 0.22f}, {0.30f, 0.34f, 0.40f}};
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            const uint8_t *p = &a[((size_t)y * N + x) * 4];
            const float *b = bg[(x * 3) / N];
            const float al = p[3] / 255.0f;
            const bool smoke = y >= N / 2 && y < 3 * N / 4;
            for (int c = 0; c < 3; ++c)
            {
                float s = p[c] / 255.0f;
                if (smoke) s *= 0.36f; // engine tint for smoke
                float o = b[c] * (1 - al) + s * al;
                fputc((int)(o * 255.0f + 0.5f), f);
            }
        }
    fclose(f);
    return 0;
}
