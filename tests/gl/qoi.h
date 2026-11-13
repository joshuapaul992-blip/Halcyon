/* QOI ("Quite OK Image") reading and writing, RGB only, for the reference
 * pictures of the scene tests (tests/gl/ref, NAME.qoi): lossless and small for
 * flat pictures, and Python's PIL opens them, so they can be looked at.
 * Pixels are 0x00RRGGBB, rows from the top. */
#ifndef QOI_H
#define QOI_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned qoi_hash(unsigned r, unsigned g, unsigned b, unsigned a) { return (r * 3 + g * 5 + b * 7 + a * 11) & 63; }

static int qoi_write(const char *path, const unsigned *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    unsigned char hd[14] = { 'q', 'o', 'i', 'f', (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8),
                             (unsigned char)w, (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8),
                             (unsigned char)h, 3, 0 };
    fwrite(hd, 1, sizeof hd, f);
    unsigned idx[64] = { 0 };
    unsigned prev = 0xFF000000u;           /* (alpha 255 in the top byte) */
    int run = 0;
    long n = (long)w * h;
    for (long i = 0; i < n; i++) {
        unsigned p = (px[i] & 0xFFFFFF) | 0xFF000000u;
        if (p == prev) {
            if (++run == 62 || i == n - 1) { fputc(0xC0 | (run - 1), f); run = 0; }
            continue;
        }
        if (run) { fputc(0xC0 | (run - 1), f); run = 0; }
        unsigned r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        unsigned hsh = qoi_hash(r, g, b, 255);
        if (idx[hsh] == p) fputc((int)hsh, f);
        else {
            idx[hsh] = p;
            int dr = (int)r - (int)((prev >> 16) & 255), dg = (int)g - (int)((prev >> 8) & 255), db = (int)b - (int)(prev & 255);
            dr = (signed char)dr; dg = (signed char)dg; db = (signed char)db;
            int dr_dg = dr - dg, db_dg = db - dg;
            if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1)
                fputc(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2), f);
            else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                fputc(0x80 | (dg + 32), f);
                fputc((dr_dg + 8) << 4 | (db_dg + 8), f);
            } else { fputc(0xFE, f); fputc((int)r, f); fputc((int)g, f); fputc((int)b, f); }
        }
        prev = p;
    }
    static const unsigned char end[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    fwrite(end, 1, 8, f);
    return fclose(f) == 0;
}

/* NULL if missing or broken; *w, *h its size */
static unsigned *qoi_read(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = len > 22 ? malloc((size_t)len) : NULL;
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len || memcmp(d, "qoif", 4)) { fclose(f); free(d); return NULL; }
    fclose(f);
    *w = d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
    *h = d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
    if (*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192) { free(d); return NULL; }
    long n = (long)*w * *h, pos = 14, i = 0;
    unsigned *px = malloc(sizeof *px * (size_t)n);
    unsigned idx[64] = { 0 };
    unsigned r = 0, g = 0, b = 0, a = 255;
    while (px && i < n && pos < len - 8) {
        int op = d[pos++];
        int run = 1;
        if (op == 0xFE) { r = d[pos]; g = d[pos + 1]; b = d[pos + 2]; pos += 3; }
        else if (op == 0xFF) { r = d[pos]; g = d[pos + 1]; b = d[pos + 2]; a = d[pos + 3]; pos += 4; }
        else if ((op & 0xC0) == 0x00) { unsigned p = idx[op]; r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255; a = p >> 24; }
        else if ((op & 0xC0) == 0x40) { r = (r + ((op >> 4) & 3) - 2) & 255; g = (g + ((op >> 2) & 3) - 2) & 255; b = (b + (op & 3) - 2) & 255; }
        else if ((op & 0xC0) == 0x80) {
            int dg = (op & 63) - 32, x = d[pos++];
            r = (unsigned)((int)r + dg + ((x >> 4) & 15) - 8) & 255;
            g = (unsigned)((int)g + dg) & 255;
            b = (unsigned)((int)b + dg + (x & 15) - 8) & 255;
        } else run = (op & 63) + 1;
        idx[qoi_hash(r, g, b, a)] = a << 24 | r << 16 | g << 8 | b;
        for (; run > 0 && i < n; run--) px[i++] = r << 16 | g << 8 | b;
    }
    free(d);
    if (px && i < n) { free(px); return NULL; }
    return px;
}

#endif
