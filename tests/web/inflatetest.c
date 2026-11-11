/* Host test of libweb's inflate: the vectors from gen_inflate.py (Python's
 * zlib / gzip output, corrupt streams), output limits, the fixed-buffer
 * variant PNG uses, and random mutations (run under ASan and UBSan).
 *   inflatetest DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

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

static int run(const char *kind, const unsigned char *in, size_t n, unsigned char **out, size_t *outn, size_t max) {
    if (!strcmp(kind, "raw")) {
        size_t used;
        return inflate_raw(in, n, &used, out, outn, max);
    }
    if (!strcmp(kind, "zlib")) return inflate_zlib(in, n, out, outn, max);
    return inflate_gzip(in, n, out, outn, max);
}

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: inflatetest DIR\n"); return 2; }
    int checks = 0, fails = 0;
    char path[4096];
    struct { char kind[8]; unsigned char *in; size_t n; } good[512];
    int ngood = 0;
    for (int i = 0;; i++) {
        snprintf(path, sizeof path, "%s/%03d.txt", argv[1], i);
        FILE *f = fopen(path, "r");
        if (!f) break;
        char kind[16], exp[16], name[128];
        if (fscanf(f, "%15s %15s %127s", kind, exp, name) != 3) { fclose(f); break; }
        fclose(f);
        size_t n, en = 0;
        snprintf(path, sizeof path, "%s/%03d.in", argv[1], i);
        unsigned char *in = slurp(path, &n);
        snprintf(path, sizeof path, "%s/%03d.out", argv[1], i);
        unsigned char *want = !strcmp(exp, "ok") ? slurp(path, &en) : 0;
        unsigned char *out = 0;
        size_t outn = 0;
        int r = run(kind, in, n, &out, &outn, 64u << 20);
        int any = strstr(name, "-any") != 0;
        checks++;
        if (want) {
            if (r || outn != en || memcmp(out, want, en)) { printf("FAIL %s: r=%d len %zu want %zu\n", name, r, outn, en); fails++; }
            else if (out[outn] != 0) { printf("FAIL %s: no NUL after the output\n", name); fails++; }
            if (ngood < 512 && n > 4) {
                strcpy(good[ngood].kind, kind);
                good[ngood].in = malloc(n);
                memcpy(good[ngood].in, in, n);
                good[ngood].n = n;
                ngood++;
            }
            /* an output limit just below the size must fail with INF_E_LIMIT */
            if (en > 0) {
                unsigned char *o2; size_t n2;
                int r2 = run(kind, in, n, &o2, &n2, en - 1);
                checks++;
                if (r2 != INF_E_LIMIT) { printf("FAIL %s: limit %zu gave %d\n", name, en - 1, r2); fails++; }
                if (!r2) free(o2);
            }
            /* zlib into an exact buffer (as PNG does), and a buffer one byte short */
            if (!strcmp(kind, "zlib")) {
                unsigned char *b = malloc(en + 1);
                int r3 = inflate_zlib_into(in, n, b, en);
                checks++;
                if (r3 || memcmp(b, want, en)) { printf("FAIL %s: inflate_zlib_into %d\n", name, r3); fails++; }
                if (en > 1) {
                    r3 = inflate_zlib_into(in, n, b, en - 1);
                    checks++;
                    if (r3 != INF_E_LIMIT) { printf("FAIL %s: short buffer gave %d\n", name, r3); fails++; }
                }
                r3 = inflate_zlib_into(in, n, b, en + 1);
                checks++;
                if (r3 == 0) { printf("FAIL %s: long buffer accepted\n", name); fails++; }
                free(b);
            }
        } else if (strstr(name, "bomb")) {
            unsigned char *o2; size_t n2;
            int r2 = run(kind, in, n, &o2, &n2, 1u << 20);
            checks++;
            if (r2 != INF_E_LIMIT) { printf("FAIL %s: bomb gave %d\n", name, r2); fails++; }
            if (!r2) free(o2);
        } else if (!any && !r) {
            printf("FAIL %s: corrupt stream accepted (%zu bytes)\n", name, outn);
            fails++;
        }
        if (!r) free(out);
        free(in);
        free(want);
    }
    /* random mutations of the good streams: must never crash, never exceed the limit */
    int muts = 0;
    for (int it = 0; it < 30000 && ngood; it++) {
        int g = (int)(rnd() % (unsigned)ngood);
        size_t n = good[g].n;
        unsigned char *b = malloc(n);
        memcpy(b, good[g].in, n);
        int kind = rnd() % 4;
        if (kind == 0) b[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8));
        else if (kind == 1) b[rnd() % n] = (unsigned char)rnd();
        else if (kind == 2) n = rnd() % n;
        else for (int k = 0; k < 8; k++) b[rnd() % n] = (unsigned char)rnd();
        unsigned char *out;
        size_t outn;
        size_t lim = 1u << 20;
        int r = run(good[g].kind, b, n, &out, &outn, lim);
        if (!r) {
            if (outn > lim) { printf("FAIL mutation: output %zu over the limit\n", outn); fails++; }
            free(out);
        }
        free(b);
        muts++;
    }
    checks += muts;
    for (int g = 0; g < ngood; g++) free(good[g].in);
    printf("inflate: %d checks (%d mutations), %d failures\n", checks, muts, fails);
    return fails != 0;
}
