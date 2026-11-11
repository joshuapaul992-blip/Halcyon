/* Host test of the browser's PNG / GIF / BMP decoders (browser/image.c):
 * the files from gen_images.py must decode to Pillow's pixels, and random
 * mutations of them must fail cleanly or decode within the limits (run
 * under ASan and UBSan).
 *   imagetest DIR */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    DIR *dir = opendir(argv[1]);
    if (!dir) { perror(argv[1]); return 2; }
    struct dirent *e;
    int checks = 0, fails = 0, nfiles = 0;
    unsigned char *files[128];
    size_t sizes[128];
    char path[4096];
    while ((e = readdir(dir))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".img")) continue;
        snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        size_t n;
        unsigned char *b = slurp(path, &n);
        if (!b) continue;
        int w, h;
        uint32_t *px;
        int r = image_decode(b, n, &w, &h, &px);
        checks++;
        if (r) { printf("FAIL %s: does not decode (%d)\n", e->d_name, r); fails++; }
        snprintf(path, sizeof path, "%s/%.*s.rgba", argv[1], (int)(l - 4), e->d_name);
        size_t rn;
        unsigned char *ref = slurp(path, &rn);
        if (!r && ref) {
            unsigned rw, rh;
            memcpy(&rw, ref, 4);
            memcpy(&rh, ref + 4, 4);
            checks++;
            if ((int)rw != w || (int)rh != h) { printf("FAIL %s: %dx%d, Pillow %ux%u\n", e->d_name, w, h, rw, rh); fails++; }
            else {
                int bad = 0;
                for (size_t i = 0; i < (size_t)w * (size_t)h && !bad; i++) {
                    const unsigned char *q = ref + 8 + i * 4;
                    uint32_t want = (uint32_t)q[3] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
                    uint32_t got = px[i];
                    if (q[3] == 0 && (got >> 24) == 0) continue;          /* transparent: colour does not matter */
                    if (got != want) { printf("FAIL %s: pixel %zu is %08x, Pillow %08x\n", e->d_name, i, got, want); bad = 1; }
                }
                fails += bad;
            }
        }
        if (!r) free(px);
        free(ref);
        if (nfiles < 128) { files[nfiles] = b; sizes[nfiles++] = n; } else free(b);
    }
    closedir(dir);
    /* mutations */
    int muts = 0, decoded = 0;
    for (int it = 0; it < 40000 && nfiles; it++) {
        int k = (int)(rnd() % (unsigned)nfiles);
        size_t n = sizes[k];
        unsigned char *b = malloc(n);
        memcpy(b, files[k], n);
        int kind = rnd() % 5;
        if (kind == 0) b[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8));
        else if (kind == 1) for (int j = 0; j < 4; j++) b[rnd() % n] = (unsigned char)rnd();
        else if (kind == 2) n = rnd() % n;
        else if (kind == 3 && n > 24) { size_t p = 8 + rnd() % 16; b[p] = (unsigned char)rnd(); b[p + 1] = (unsigned char)rnd(); }   /* header fields */
        else b[rnd() % (n < 64 ? n : 64)] = (unsigned char)rnd();
        int w, h;
        uint32_t *px;
        if (!image_decode(b, n, &w, &h, &px)) {
            decoded++;
            if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM || (unsigned long)w * (unsigned long)h > IMG_MAX_PIXELS) {
                printf("FAIL mutation: %dx%d\n", w, h);
                fails++;
            }
            /* touch every pixel (ASan checks the size) */
            volatile uint32_t sum = 0;
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) sum += px[i];
            free(px);
        }
        free(b);
        muts++;
    }
    checks += muts;
    for (int i = 0; i < nfiles; i++) free(files[i]);
    printf("images: %d files, %d checks (%d mutations, %d still decoded), %d failures\n", nfiles, checks, muts, decoded, fails);
    return fails != 0;
}
