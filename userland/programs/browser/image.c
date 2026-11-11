/* BROWSER.EXE: image decoders for PNG, GIF and BMP, written for Halcyon.
 *
 * The data come from the network, so every size, offset and count is
 * checked before it is used: dimensions are at most IMG_MAX_DIM and
 * IMG_MAX_PIXELS in all, every read is inside the buffer, PNG's compressed
 * data must inflate to exactly the size the header implies (libweb's
 * inflate, into a buffer of that size) and its chunks' CRCs must match,
 * GIF's LZW codes are checked against the table and the frame, and BMP's
 * rows must lie inside the file. A broken image is an error (< 0); the
 * browser then shows the alternative text. Output is 0xAARRGGBB with
 * straight alpha. tests/web/imagetest.c compares the results with Pillow's
 * and feeds the decoders mutated files. */
#include "browser.h"
#include "web.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

static int dims_ok(long w, long h) {
    return w > 0 && h > 0 && w <= IMG_MAX_DIM && h <= IMG_MAX_DIM && (unsigned long)w * (unsigned long)h <= IMG_MAX_PIXELS;
}

/* ---------------- PNG ---------------- */
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* undo the filters of h rows of rb bytes (each after its filter byte), bpp bytes per pixel */
static int unfilter(uint8_t *raw, int h, size_t rb, int bpp) {
    uint8_t *prev = 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (rb + 1);
        int f = row[0];
        uint8_t *r = row + 1;
        uint8_t *u = prev ? prev + 1 : 0;
        switch (f) {
        case 0: break;
        case 1: for (size_t i = (size_t)bpp; i < rb; i++) r[i] = (uint8_t)(r[i] + r[i - (size_t)bpp]); break;
        case 2: if (u) for (size_t i = 0; i < rb; i++) r[i] = (uint8_t)(r[i] + u[i]); break;
        case 3:
            for (size_t i = 0; i < rb; i++) {
                int a = i >= (size_t)bpp ? r[i - (size_t)bpp] : 0, b = u ? u[i] : 0;
                r[i] = (uint8_t)(r[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < rb; i++) {
                int a = i >= (size_t)bpp ? r[i - (size_t)bpp] : 0, b = u ? u[i] : 0, c = u && i >= (size_t)bpp ? u[i - (size_t)bpp] : 0;
                r[i] = (uint8_t)(r[i] + paeth(a, b, c));
            }
            break;
        default: return -1;
        }
        prev = row;
    }
    return 0;
}

typedef struct {
    int w, h, depth, ctype, channels;
    uint32_t pal[256];
    int npal;
    int trns_gray, trns_r, trns_g, trns_b, has_trns;
} pnginfo_t;

static int sample(const uint8_t *r, int x, int depth, int idx) {
    /* sample number idx of row r */
    if (depth == 8) return r[idx];
    if (depth == 16) return r[idx * 2];                 /* the high byte */
    int per = 8 / depth;
    int byte = r[idx / per];
    int shift = 8 - depth - (idx % per) * depth;
    (void)x;
    return (byte >> shift) & ((1 << depth) - 1);
}
static int sample16(const uint8_t *r, int idx) { return r[idx * 2] << 8 | r[idx * 2 + 1]; }

/* rows of one (sub)image into the output at (x0 + i*dx, y0 + j*dy) */
static void png_put(const pnginfo_t *pi, const uint8_t *raw, int pw, int ph, size_t rb, uint32_t *out, int x0, int y0, int dx, int dy) {
    int scale = pi->depth < 8 ? 255 / ((1 << pi->depth) - 1) : 1;
    for (int j = 0; j < ph; j++) {
        const uint8_t *r = raw + (size_t)j * (rb + 1) + 1;
        uint32_t *o = out + (size_t)(y0 + j * dy) * (size_t)pi->w;
        for (int i = 0; i < pw; i++) {
            int x = x0 + i * dx;
            uint32_t a = 255, R, G, B;
            int c = pi->channels;
            switch (pi->ctype) {
            case 0: {
                int v = sample(r, i, pi->depth, i);
                if (pi->has_trns && (pi->depth == 16 ? sample16(r, i) : v) == pi->trns_gray) a = 0;
                if (pi->depth < 8) v *= scale;
                R = G = B = (uint32_t)v;
                break;
            }
            case 2:
                R = (uint32_t)sample(r, i, pi->depth, i * 3);
                G = (uint32_t)sample(r, i, pi->depth, i * 3 + 1);
                B = (uint32_t)sample(r, i, pi->depth, i * 3 + 2);
                if (pi->has_trns) {
                    int rr = pi->depth == 16 ? sample16(r, i * 3) : (int)R, gg = pi->depth == 16 ? sample16(r, i * 3 + 1) : (int)G,
                        bb = pi->depth == 16 ? sample16(r, i * 3 + 2) : (int)B;
                    if (rr == pi->trns_r && gg == pi->trns_g && bb == pi->trns_b) a = 0;
                }
                break;
            case 3: {
                int v = sample(r, i, pi->depth, i);
                uint32_t p = v < pi->npal ? pi->pal[v] : 0;
                a = p >> 24; R = (p >> 16) & 255; G = (p >> 8) & 255; B = p & 255;
                break;
            }
            case 4:
                R = G = B = (uint32_t)sample(r, i, pi->depth, i * 2);
                a = (uint32_t)sample(r, i, pi->depth, i * 2 + 1);
                break;
            default:
                R = (uint32_t)sample(r, i, pi->depth, i * 4);
                G = (uint32_t)sample(r, i, pi->depth, i * 4 + 1);
                B = (uint32_t)sample(r, i, pi->depth, i * 4 + 2);
                a = (uint32_t)sample(r, i, pi->depth, i * 4 + 3);
                break;
            }
            (void)c;
            o[x] = a << 24 | R << 16 | G << 8 | B;
        }
    }
}

static int png_decode(const uint8_t *p, size_t n, int *W, int *H, uint32_t **px) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n < 8 || memcmp(p, sig, 8)) return -1;
    pnginfo_t pi;
    memset(&pi, 0, sizeof pi);
    int interlace = 0, seen_ihdr = 0;
    uint8_t *idat = 0;
    size_t idn = 0, idcap = 0;
    size_t pos = 8;
    int r = -1;
    for (int i = 0; i < 256; i++) pi.pal[i] = 0xFF000000u;
    while (pos + 12 <= n) {
        uint32_t len = be32(p + pos);
        const uint8_t *type = p + pos + 4, *data = p + pos + 8;
        if (len > n - pos - 12) goto out;
        uint32_t crc = be32(data + len);
        if (web_crc32(0, type, len + 4) != crc) goto out;
        pos += 12 + (size_t)len;
        if (!memcmp(type, "IHDR", 4)) {
            if (len != 13 || seen_ihdr) goto out;
            seen_ihdr = 1;
            uint32_t w = be32(data), h = be32(data + 4);
            if (!dims_ok((long)w, (long)h)) goto out;
            pi.w = (int)w; pi.h = (int)h;
            pi.depth = data[8];
            pi.ctype = data[9];
            interlace = data[12];
            if (data[10] || data[11] || interlace > 1) goto out;
            int d = pi.depth, t = pi.ctype;
            int ok = (t == 0 && (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)) || (t == 3 && (d == 1 || d == 2 || d == 4 || d == 8)) ||
                     ((t == 2 || t == 4 || t == 6) && (d == 8 || d == 16));
            if (!ok) goto out;
            pi.channels = t == 0 || t == 3 ? 1 : t == 2 ? 3 : t == 4 ? 2 : 4;
        } else if (!seen_ihdr) {
            goto out;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 || len / 3 > 256 || len == 0) goto out;
            pi.npal = (int)(len / 3);
            for (int i = 0; i < pi.npal; i++) pi.pal[i] = 0xFF000000u | (uint32_t)data[i * 3] << 16 | (uint32_t)data[i * 3 + 1] << 8 | data[i * 3 + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (pi.ctype == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++) pi.pal[i] = (pi.pal[i] & 0xFFFFFF) | (uint32_t)data[i] << 24;
            } else if (pi.ctype == 0 && len >= 2) {
                pi.trns_gray = (int)(data[0] << 8 | data[1]);
                if (pi.depth < 16) pi.trns_gray &= (1 << pi.depth) - 1;
                pi.has_trns = 1;
            } else if (pi.ctype == 2 && len >= 6) {
                pi.trns_r = data[0] << 8 | data[1];
                pi.trns_g = data[2] << 8 | data[3];
                pi.trns_b = data[4] << 8 | data[5];
                if (pi.depth == 8) { pi.trns_r &= 255; pi.trns_g &= 255; pi.trns_b &= 255; }
                pi.has_trns = 1;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idn + len > (64u << 20)) goto out;
            if (idn + len > idcap) {
                size_t nc = idcap ? idcap : 65536;
                while (nc < idn + len) nc *= 2;
                uint8_t *q = realloc(idat, nc);
                if (!q) goto out;
                idat = q;
                idcap = nc;
            }
            memcpy(idat + idn, data, len);
            idn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        } else if (!(type[0] & 0x20)) {
            goto out;                     /* an unknown critical chunk */
        }
    }
    if (!seen_ihdr || !idn || (pi.ctype == 3 && !pi.npal)) goto out;
    int bits = pi.channels * pi.depth;
    int bpp = bits >= 8 ? bits / 8 : 1;
    static const int ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 },
                     adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    size_t total = 0;
    int pw[7], ph[7];
    size_t prb[7];
    for (int k = 0; k < passes; k++) {
        pw[k] = interlace ? (pi.w - ax0[k] + adx[k] - 1) / adx[k] : pi.w;
        ph[k] = interlace ? (pi.h - ay0[k] + ady[k] - 1) / ady[k] : pi.h;
        if (pw[k] < 0) pw[k] = 0;
        if (ph[k] < 0) ph[k] = 0;
        prb[k] = ((size_t)pw[k] * (size_t)bits + 7) / 8;
        if (pw[k] && ph[k]) total += (size_t)ph[k] * (prb[k] + 1);
    }
    uint8_t *raw = malloc(total ? total : 1);
    uint32_t *out = calloc((size_t)pi.w * (size_t)pi.h, 4);
    if (!raw || !out || inflate_zlib_into(idat, idn, raw, total)) { free(raw); free(out); goto out; }
    size_t off = 0;
    for (int k = 0; k < passes; k++) {
        if (!pw[k] || !ph[k]) continue;
        if (unfilter(raw + off, ph[k], prb[k], bpp)) { free(raw); free(out); goto out; }
        png_put(&pi, raw + off, pw[k], ph[k], prb[k], out, interlace ? ax0[k] : 0, interlace ? ay0[k] : 0,
                interlace ? adx[k] : 1, interlace ? ady[k] : 1);
        off += (size_t)ph[k] * (prb[k] + 1);
    }
    free(raw);
    *W = pi.w;
    *H = pi.h;
    *px = out;
    r = 0;
out:
    free(idat);
    return r;
}

/* ---------------- GIF ---------------- */
static int gif_decode(const uint8_t *p, size_t n, int *W, int *H, uint32_t **px) {
    if (n < 13 || (memcmp(p, "GIF87a", 6) && memcmp(p, "GIF89a", 6))) return -1;
    int sw = (int)le16(p + 6), sh = (int)le16(p + 8), flags = p[10];
    size_t pos = 13;
    uint32_t gct[256], lct[256];
    int ngct = 0, transparent = -1;
    if (flags & 0x80) {
        ngct = 2 << (flags & 7);
        if (pos + (size_t)ngct * 3 > n) return -1;
        for (int i = 0; i < ngct; i++) gct[i] = 0xFF000000u | (uint32_t)p[pos + i * 3] << 16 | (uint32_t)p[pos + i * 3 + 1] << 8 | p[pos + i * 3 + 2];
        pos += (size_t)ngct * 3;
    }
    while (pos < n) {
        int b = p[pos++];
        if (b == 0x3B) return -1;                      /* the end before any image */
        if (b == 0x21) {                               /* extension */
            if (pos >= n) return -1;
            int label = p[pos++];
            for (;;) {
                if (pos >= n) return -1;
                int bl = p[pos++];
                if (!bl) break;
                if (pos + (size_t)bl > n) return -1;
                if (label == 0xF9 && bl >= 4) transparent = p[pos] & 1 ? p[pos + 3] : -1;
                pos += (size_t)bl;
            }
            continue;
        }
        if (b != 0x2C) return -1;
        if (pos + 9 > n) return -1;
        int fx = (int)le16(p + pos), fy = (int)le16(p + pos + 2), fw = (int)le16(p + pos + 4), fh = (int)le16(p + pos + 6);
        int ff = p[pos + 8];
        pos += 9;
        /* some files have a frame bigger than the screen: the screen grows */
        if (fx + fw > sw) sw = fx + fw;
        if (fy + fh > sh) sh = fy + fh;
        if (!dims_ok(sw, sh) || !dims_ok(fw, fh)) return -1;
        uint32_t *pal = gct;
        int npal = ngct;
        if (ff & 0x80) {
            npal = 2 << (ff & 7);
            if (pos + (size_t)npal * 3 > n) return -1;
            for (int i = 0; i < npal; i++) lct[i] = 0xFF000000u | (uint32_t)p[pos + i * 3] << 16 | (uint32_t)p[pos + i * 3 + 1] << 8 | p[pos + i * 3 + 2];
            pos += (size_t)npal * 3;
            pal = lct;
        }
        if (pos >= n) return -1;
        int mincode = p[pos++];
        if (mincode < 2 || mincode > 8) return -1;
        /* LZW: codes are read from the sub-blocks as they come */
        size_t npx = (size_t)fw * (size_t)fh;
        uint8_t *idx = malloc(npx);
        uint16_t *prefix = malloc(4096 * sizeof(uint16_t));
        uint8_t *suffix = malloc(4096), *stack = malloc(4097);
        uint32_t *out = calloc((size_t)sw * (size_t)sh, 4);
        int r = -1;
        if (!idx || !prefix || !suffix || !stack || !out) goto gif_out;
        memset(idx, 0, npx);
        {
            int clear = 1 << mincode, eoi = clear + 1, next = clear + 2, size = mincode + 1;
            int old = -1, first = 0;
            uint32_t bitbuf = 0;
            int bitcnt = 0, blk = 0, done = 0;
            size_t o = 0;
            for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
            while (!done) {
                while (bitcnt < size) {
                    if (!blk) {
                        if (pos >= n) { done = 2; break; }
                        blk = p[pos++];
                        if (!blk) { done = 2; break; }
                    }
                    if (pos >= n) { done = 2; break; }
                    bitbuf |= (uint32_t)p[pos++] << bitcnt;
                    bitcnt += 8;
                    blk--;
                }
                if (done) break;
                int code = (int)(bitbuf & ((1u << size) - 1));
                bitbuf >>= size;
                bitcnt -= size;
                if (code == clear) { next = clear + 2; size = mincode + 1; old = -1; continue; }
                if (code == eoi) { done = 1; break; }
                if (old < 0) {
                    if (code >= clear) goto gif_out;
                    if (o < npx) idx[o++] = (uint8_t)code;
                    old = first = code;
                    continue;
                }
                int in = code, sp = 0;
                if (code > next || code == eoi) goto gif_out;
                if (code == next) { stack[sp++] = (uint8_t)first; code = old; }
                while (code >= clear) {
                    if (code >= 4096 || sp >= 4096) goto gif_out;
                    stack[sp++] = suffix[code];
                    code = prefix[code];
                    if (code == 0xFFFF) goto gif_out;
                }
                first = code;
                stack[sp++] = (uint8_t)code;
                while (sp && o < npx) idx[o++] = stack[--sp];
                if (next < 4096) {
                    prefix[next] = (uint16_t)old;
                    suffix[next] = (uint8_t)first;
                    next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                old = in;
                if (o >= npx) done = 1;
            }
            if (o < npx / 2 && done == 2) goto gif_out;   /* far too little data */
        }
        /* rows (interlaced: 0, 8, ...; 4, 12...; 2, 6...; 1, 3...) */
        {
            int row = 0, pass = 0;
            static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
            for (int j = 0; j < fh; j++) {
                int yy;
                if (ff & 0x40) {
                    while (pass < 4 && row >= fh) { pass++; if (pass < 4) row = start[pass]; }
                    if (pass >= 4) break;
                    yy = row;
                    row += step[pass];
                } else yy = j;
                uint32_t *dst = out + (size_t)(fy + yy) * (size_t)sw + fx;
                const uint8_t *s = idx + (size_t)j * (size_t)fw;
                for (int i = 0; i < fw; i++) {
                    int v = s[i];
                    dst[i] = v == transparent || v >= npal ? 0 : pal[v];
                }
            }
        }
        *W = sw;
        *H = sh;
        *px = out;
        out = 0;
        r = 0;
gif_out:
        free(idx); free(prefix); free(suffix); free(stack); free(out);
        return r;
    }
    return -1;
}

/* ---------------- BMP ---------------- */
static int mask_shift(uint32_t m, int *bits) {
    int s = 0, b = 0;
    if (!m) { *bits = 0; return 0; }
    while (!(m & 1)) { m >>= 1; s++; }
    while (m & 1) { m >>= 1; b++; }
    *bits = b;
    return s;
}
static uint32_t mask_val(uint32_t v, uint32_t m, int s, int b) {
    if (!b) return 255;
    uint32_t x = (v & m) >> s;
    return b >= 8 ? x >> (b - 8) : (x * 255) / ((1u << b) - 1);
}

static int bmp_decode(const uint8_t *p, size_t n, int *W, int *H, uint32_t **px) {
    if (n < 26 || p[0] != 'B' || p[1] != 'M') return -1;
    uint32_t off = le32(p + 10), hs = le32(p + 14);
    long w, h;
    int bpp, comp = 0, ncolors = 0;
    uint32_t rm = 0, gm = 0, bm = 0, am = 0;
    if (hs == 12) {
        w = (long)le16(p + 18); h = (long)(int16_t)le16(p + 20); bpp = (int)le16(p + 24);
    } else if (hs >= 40 && hs <= 124 && 14 + (size_t)hs <= n) {
        w = (long)(int32_t)le32(p + 18); h = (long)(int32_t)le32(p + 22); bpp = (int)le16(p + 28);
        comp = (int)le32(p + 30);
        ncolors = (int)le32(p + 46);
        if (comp == 3 || comp == 6) {
            size_t mo = 14 + 40;
            if (hs >= 52) mo = 14 + 40;
            if (mo + 12 > n) return -1;
            rm = le32(p + mo); gm = le32(p + mo + 4); bm = le32(p + mo + 8);
            if ((hs >= 56 || comp == 6) && mo + 16 <= n) am = le32(p + mo + 12);
        }
    } else return -1;
    int topdown = h < 0;
    if (h < 0) h = -h;
    if (!dims_ok(w, h)) return -1;
    if (comp != 0 && comp != 3 && comp != 6) return -1;     /* no RLE, JPEG or PNG inside */
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return -1;
    if (bpp == 16 && comp == 0) { rm = 0x7C00; gm = 0x03E0; bm = 0x001F; }
    if (bpp == 32 && comp == 0) { rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; }
    uint32_t pal[256];
    int npal = 0;
    if (bpp <= 8) {
        npal = ncolors > 0 && ncolors <= 256 ? ncolors : 1 << bpp;
        int es = hs == 12 ? 3 : 4;
        size_t po = 14 + hs;
        if (po + (size_t)npal * (size_t)es > n) return -1;
        for (int i = 0; i < npal; i++) {
            const uint8_t *e = p + po + (size_t)i * (size_t)es;
            pal[i] = 0xFF000000u | (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0];
        }
    }
    size_t stride = (((size_t)w * (size_t)bpp + 31) / 32) * 4;
    if (off > n || stride * (size_t)h > n - off) return -1;
    int rs, rb, gs, gb, bs, bb, as, ab;
    rs = mask_shift(rm, &rb); gs = mask_shift(gm, &gb); bs = mask_shift(bm, &bb); as = mask_shift(am, &ab);
    uint32_t *out = malloc((size_t)w * (size_t)h * 4);
    if (!out) return -1;
    int any_alpha = 0;
    for (long y = 0; y < h; y++) {
        const uint8_t *row = p + off + stride * (size_t)(topdown ? y : h - 1 - y);
        uint32_t *o = out + (size_t)y * (size_t)w;
        for (long x = 0; x < w; x++) {
            uint32_t c;
            if (bpp <= 8) {
                int v;
                if (bpp == 8) v = row[x];
                else if (bpp == 4) v = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                else v = (row[x / 8] >> (7 - (x & 7))) & 1;
                c = v < npal ? pal[v] : 0xFF000000u;
            } else if (bpp == 24) {
                const uint8_t *q = row + x * 3;
                c = 0xFF000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            } else {
                uint32_t v = bpp == 16 ? le16(row + x * 2) : le32(row + x * 4);
                uint32_t a = am ? mask_val(v, am, as, ab) : 255;
                if (am && a) any_alpha = 1;
                c = a << 24 | mask_val(v, rm, rs, rb) << 16 | mask_val(v, gm, gs, gb) << 8 | mask_val(v, bm, bs, bb);
            }
            o[x] = c;
        }
    }
    /* an alpha mask that is all zero means "no alpha" (as Windows draws them) */
    if (am && !any_alpha) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) out[i] |= 0xFF000000u;
    *W = (int)w;
    *H = (int)h;
    *px = out;
    return 0;
}

int image_decode(const uint8_t *data, size_t len, int *w, int *h, uint32_t **px) {
    *px = 0;
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P') return png_decode(data, len, w, h, px);
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return gif_decode(data, len, w, h, px);
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') return bmp_decode(data, len, w, h, px);
    return -2;                                  /* a format this browser cannot show (JPEG, WebP, SVG...) */
}
