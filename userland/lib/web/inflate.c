/* libweb: inflate (RFC 1951 deflate, RFC 1950 zlib, RFC 1952 gzip), written
 * for Halcyon after the structure of Mark Adler's "puff" description of the
 * format: canonical Huffman codes from code lengths, decoded with a 9-bit
 * lookup table and a bit-by-bit fallback for longer codes.
 *
 * The decoder works on a whole input buffer. Every read is bounds checked
 * (running out of input is INF_E_TRUNC, never a read past the end), every
 * code is checked for over-subscription (and incompleteness where the
 * format forbids it), distances may not reach before the start of the
 * output, and the output never grows past max_out (INF_E_LIMIT), which is
 * what stops "decompression bombs". tests/web/inflatetest.c checks it
 * against Python's zlib and gzip and against corrupt streams. */
#include "webplat.h"

#define MAXBITS 15
#define FASTBITS 9
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    uint16_t count[MAXBITS + 1];      /* codes of each length */
    uint16_t symbol[FIXLCODES];       /* symbols ordered by code */
    uint16_t fast[1 << FASTBITS];     /* bit-reversed code of <= FASTBITS bits -> len << 9 | symbol, 0 = slow */
} huff_t;

typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    int err;
    uint8_t *out;
    size_t outlen, outcap, max_out;
    int fixed;                        /* out is the caller's buffer: never reallocated */
} st_t;

/* make sure n bits are buffered if the input has them; 0 or INF_E_TRUNC */
static inline int need(st_t *s, int n) {
    while (s->bitcnt < n) {
        if (s->inpos >= s->inlen) return INF_E_TRUNC;
        s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    return 0;
}

static inline uint32_t bits(st_t *s, int n) {
    if (s->err) return 0;
    if (need(s, n)) { s->err = INF_E_TRUNC; return 0; }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* the whole bytes still in the bit buffer go back to the input */
static void unread(st_t *s) {
    s->inpos -= (size_t)(s->bitcnt / 8);
    s->bitbuf = 0;
    s->bitcnt = 0;
}

static int out_reserve(st_t *s, size_t n) {
    if (n > s->max_out || s->outlen > s->max_out - n) return INF_E_LIMIT;
    if (s->outlen + n <= s->outcap) return 0;
    if (s->fixed) return INF_E_LIMIT;
    size_t nc = s->outcap ? s->outcap : 16384;
    while (nc < s->outlen + n) nc = nc > (SIZE_MAX / 2) ? SIZE_MAX : nc * 2;
    if (nc > s->max_out) nc = s->max_out;
    uint8_t *p = realloc(s->out, nc + 1);           /* + 1: room for a NUL */
    if (!p) return INF_E_NOMEM;
    s->out = p;
    s->outcap = nc;
    return 0;
}

/* build a code from lengths; returns 0 complete, > 0 incomplete, < 0 over-subscribed */
static int construct(huff_t *h, const uint8_t *length, int n) {
    uint16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;                  /* no codes: complete, but decoding fails */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (length[i]) h->symbol[offs[length[i]]++] = (uint16_t)i;
    /* fast table: walk the canonical codes in order */
    int code = 0, idx = 0;
    for (int len = 1; len <= FASTBITS; len++) {
        for (int k = 0; k < h->count[len]; k++, idx++, code++) {
            int rev = 0;
            for (int b = 0; b < len; b++) if (code & (1 << b)) rev |= 1 << (len - 1 - b);
            for (int f = rev; f < (1 << FASTBITS); f += 1 << len)
                h->fast[f] = (uint16_t)(len << 9 | h->symbol[idx]);
        }
        code <<= 1;
    }
    return left;
}

/* one symbol; < 0 on error (s->err set) */
static int decode(st_t *s, const huff_t *h) {
    if (s->err) return -1;
    need(s, FASTBITS);                                /* as many as there are (not an error yet) */
    if (s->bitcnt >= 1) {
        uint16_t e = h->fast[s->bitbuf & ((1u << FASTBITS) - 1)];
        int len = e >> 9;
        if (e && len <= s->bitcnt) {
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return e & 0x1FF;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = INF_E_DATA;                              /* no such code (incomplete code) */
    return -1;
}

static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(st_t *s, const huff_t *lc, const huff_t *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return s->err;
        if (sym < 256) {
            int r = out_reserve(s, 1);
            if (r) return r;
            s->out[s->outlen++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return INF_E_DATA;             /* 286, 287 */
        size_t len = lbase[sym] + bits(s, lext[sym]);
        int ds = decode(s, dc);
        if (ds < 0) return s->err;
        if (ds >= 30) return INF_E_DATA;
        size_t dist = dbase[ds] + bits(s, dext[ds]);
        if (s->err) return s->err;
        if (dist > s->outlen) return INF_E_DATA;      /* before the start of the output */
        int r = out_reserve(s, len);
        if (r) return r;
        uint8_t *o = s->out + s->outlen;
        const uint8_t *from = o - dist;
        for (size_t i = 0; i < len; i++) o[i] = from[i];
        s->outlen += len;
    }
}

static int stored(st_t *s) {
    unread(s);
    if (s->inlen - s->inpos < 4) return INF_E_TRUNC;
    const uint8_t *p = s->in + s->inpos;
    unsigned len = p[0] | p[1] << 8, nlen = p[2] | p[3] << 8;
    if (len != (~nlen & 0xFFFF)) return INF_E_DATA;
    s->inpos += 4;
    if (s->inlen - s->inpos < len) return INF_E_TRUNC;
    int r = out_reserve(s, len);
    if (r) return r;
    if (len) memcpy(s->out + s->outlen, s->in + s->inpos, len);
    s->outlen += len;
    s->inpos += len;
    return 0;
}

static int fixed_block(st_t *s) {
    huff_t *h = malloc(2 * sizeof *h);
    if (!h) return INF_E_NOMEM;
    uint8_t l[FIXLCODES];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < FIXLCODES; i++) l[i] = 8;
    construct(&h[0], l, FIXLCODES);
    for (i = 0; i < MAXDCODES; i++) l[i] = 5;
    construct(&h[1], l, MAXDCODES);
    int r = codes(s, &h[0], &h[1]);
    free(h);
    return r;
}

static int dynamic_block(st_t *s) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[MAXLCODES + MAXDCODES];
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err) return s->err;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return INF_E_DATA;
    huff_t *h = malloc(2 * sizeof *h);
    if (!h) return INF_E_NOMEM;
    int r = 0, idx;
    for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (uint8_t)bits(s, 3);
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (s->err) { r = s->err; goto out; }
    if (construct(&h[0], lengths, 19) != 0) { r = INF_E_DATA; goto out; }   /* must be complete */
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &h[0]);
        if (sym < 0) { r = s->err; goto out; }
        if (sym < 16) { lengths[idx++] = (uint8_t)sym; continue; }
        int len = 0, rep;
        if (sym == 16) {
            if (idx == 0) { r = INF_E_DATA; goto out; }   /* nothing to repeat */
            len = lengths[idx - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) rep = 3 + (int)bits(s, 3);
        else rep = 11 + (int)bits(s, 7);
        if (s->err) { r = s->err; goto out; }
        if (idx + rep > nlen + ndist) { r = INF_E_DATA; goto out; }
        while (rep--) lengths[idx++] = (uint8_t)len;
    }
    if (lengths[256] == 0) { r = INF_E_DATA; goto out; }   /* no end-of-block code */
    int e = construct(&h[0], lengths, nlen);
    if (e < 0 || (e > 0 && nlen - h[0].count[0] != 1)) { r = INF_E_DATA; goto out; }
    e = construct(&h[1], lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - h[1].count[0] != 1)) { r = INF_E_DATA; goto out; }
    r = codes(s, &h[0], &h[1]);
out:
    free(h);
    return r;
}

static int run(st_t *s) {
    int last;
    do {
        last = (int)bits(s, 1);
        int type = (int)bits(s, 2);
        if (s->err) return s->err;
        int r = type == 0 ? stored(s) : type == 1 ? fixed_block(s) : type == 2 ? dynamic_block(s) : INF_E_DATA;
        if (r) return r;
    } while (!last);
    unread(s);
    return 0;
}

static int inflate_st(st_t *s, const uint8_t *in, size_t inlen) {
    s->in = in;
    s->inlen = inlen;
    s->inpos = 0;
    s->bitbuf = 0;
    s->bitcnt = 0;
    s->err = 0;
    return run(s);
}

int inflate_raw(const uint8_t *in, size_t inlen, size_t *used, uint8_t **out, size_t *outlen, size_t max_out) {
    st_t s;
    memset(&s, 0, sizeof s);
    s.max_out = max_out;
    int r = inflate_st(&s, in, inlen);
    if (!r && !s.out) r = out_reserve(&s, 0) ? INF_E_NOMEM : 0;
    if (!r && !s.out) { s.out = malloc(1); if (!s.out) r = INF_E_NOMEM; }
    if (r) { free(s.out); *out = 0; *outlen = 0; return r; }
    s.out[s.outlen] = 0;
    *out = s.out;
    *outlen = s.outlen;
    if (used) *used = s.inpos;
    return 0;
}

/* ---------------- checksums ---------------- */
static uint32_t crc_table[256];
static volatile int crc_ready;
uint32_t web_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t web_adler32(uint32_t a, const uint8_t *p, size_t n) {
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521;
        s2 %= 65521;
    }
    return s2 << 16 | s1;
}

/* ---------------- zlib ---------------- */
static int zlib_header(const uint8_t *in, size_t inlen) {
    if (inlen < 2) return INF_E_TRUNC;
    if ((in[0] & 0x0F) != 8 || (in[0] >> 4) > 7 || ((in[0] << 8) | in[1]) % 31) return INF_E_DATA;
    if (in[1] & 0x20) return INF_E_DATA;              /* preset dictionary: not supported */
    return 0;
}

int inflate_zlib(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t max_out) {
    *out = 0;
    *outlen = 0;
    int r = zlib_header(in, inlen);
    if (r) return r;
    size_t used;
    r = inflate_raw(in + 2, inlen - 2, &used, out, outlen, max_out);
    if (r) return r;
    const uint8_t *t = in + 2 + used;
    if (inlen - 2 - used < 4) r = INF_E_TRUNC;
    else if (((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3]) != web_adler32(1, *out, *outlen)) r = INF_E_CHECK;
    if (r) { free(*out); *out = 0; *outlen = 0; }
    return r;
}

int inflate_zlib_into(const uint8_t *in, size_t inlen, uint8_t *out, size_t expect) {
    int r = zlib_header(in, inlen);
    if (r) return r;
    st_t s;
    memset(&s, 0, sizeof s);
    s.out = out;
    s.outcap = expect;
    s.max_out = expect;
    s.fixed = 1;
    r = inflate_st(&s, in + 2, inlen - 2);
    if (r) return r;
    if (s.outlen != expect) return INF_E_TRUNC;
    const uint8_t *t = in + 2 + s.inpos;
    if (inlen - 2 - s.inpos < 4) return INF_E_TRUNC;
    if (((uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3]) != web_adler32(1, out, expect)) return INF_E_CHECK;
    return 0;
}

/* ---------------- gzip ---------------- */
int inflate_gzip(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t max_out) {
    *out = 0;
    *outlen = 0;
    uint8_t *acc = 0;
    size_t accn = 0, pos = 0;
    int members = 0, r = 0;
    while (pos < inlen) {
        const uint8_t *g = in + pos;
        size_t left = inlen - pos;
        if (left < 2 || g[0] != 0x1F || g[1] != 0x8B) {
            if (members) break;                       /* trailing bytes after a member: ignored */
            r = INF_E_DATA;
            goto fail;
        }
        if (left < 10) { r = INF_E_TRUNC; goto fail; }
        int flg = g[3];
        if (g[2] != 8 || (flg & 0xE0)) { r = INF_E_DATA; goto fail; }
        size_t p = 10;
        if (flg & 4) {                                /* FEXTRA */
            if (left - p < 2) { r = INF_E_TRUNC; goto fail; }
            size_t xl = g[p] | g[p + 1] << 8;
            p += 2;
            if (left - p < xl) { r = INF_E_TRUNC; goto fail; }
            p += xl;
        }
        for (int f = 8; f <= 16; f <<= 1) {           /* FNAME, FCOMMENT */
            if (!(flg & f)) continue;
            while (p < left && g[p]) p++;
            if (p >= left) { r = INF_E_TRUNC; goto fail; }
            p++;
        }
        if (flg & 2) {                                /* FHCRC */
            if (left - p < 2) { r = INF_E_TRUNC; goto fail; }
            p += 2;
        }
        uint8_t *part;
        size_t partn, used;
        r = inflate_raw(g + p, left - p, &used, &part, &partn, max_out - accn);
        if (r) goto fail;
        p += used;
        if (left - p < 8) { free(part); r = INF_E_TRUNC; goto fail; }
        uint32_t crc = (uint32_t)(g[p] | g[p + 1] << 8 | g[p + 2] << 16 | (uint32_t)g[p + 3] << 24);
        uint32_t isz = (uint32_t)(g[p + 4] | g[p + 5] << 8 | g[p + 6] << 16 | (uint32_t)g[p + 7] << 24);
        if (crc != web_crc32(0, part, partn) || isz != (uint32_t)partn) { free(part); r = INF_E_CHECK; goto fail; }
        p += 8;
        if (!acc) { acc = part; accn = partn; }
        else {
            uint8_t *n = realloc(acc, accn + partn + 1);
            if (!n) { free(part); r = INF_E_NOMEM; goto fail; }
            acc = n;
            memcpy(acc + accn, part, partn);
            accn += partn;
            acc[accn] = 0;
            free(part);
        }
        members++;
        pos += p;
    }
    if (!members) { r = INF_E_TRUNC; goto fail; }
    *out = acc;
    *outlen = accn;
    return 0;
fail:
    free(acc);
    return r;
}
