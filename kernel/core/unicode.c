/* Halcyon OS - Unicode helpers (see unicode.h) */
#include "unicode.h"
#include "lib.h"

static int cont(u8 c) { return (c & 0xC0) == 0x80; }

/* length of a valid sequence starting at s (s has at least `avail` bytes),
 * 0 if invalid */
static int seq_len(const u8 *s, int avail, u32 *cp) {
    u8 c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (avail < 2 || !cont(s[1])) return 0;
        *cp = ((u32)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (avail < 3 || !cont(s[1]) || !cont(s[2])) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;     /* overlong */
        if (c == 0xED && s[1] >= 0xA0) return 0;    /* surrogate */
        *cp = ((u32)(c & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (avail < 4 || !cont(s[1]) || !cont(s[2]) || !cont(s[3])) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;
        if (c == 0xF4 && s[1] >= 0x90) return 0;
        *cp = ((u32)(c & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) | ((u32)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}

u32 utf8_next(const char **s) {
    const u8 *p = (const u8 *)*s;
    if (!*p) return 0;
    int avail = 1;
    while (avail < 4 && p[avail]) avail++;
    u32 cp;
    int n = seq_len(p, avail, &cp);
    if (!n) { *s += 1; return p[0]; }       /* not UTF-8: Latin-1 */
    *s += n;
    return cp;
}

u32 utf8_get(const char *s, int len, int *pos) {
    if (*pos >= len) return 0;
    const u8 *p = (const u8 *)s + *pos;
    u32 cp;
    int n = seq_len(p, MIN(4, len - *pos), &cp);
    if (!n) { *pos += 1; return p[0]; }
    *pos += n;
    return cp;
}

int utf8_put(u32 cp, char *o) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = UC_REPLACEMENT;
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_valid(const char *s) {
    const u8 *p = (const u8 *)s;
    int len = (int)strlen(s);
    for (int i = 0; i < len;) {
        u32 cp;
        int n = seq_len(p + i, MIN(4, len - i), &cp);
        if (!n) return 0;
        i += n;
    }
    return 1;
}

int utf8_count(const char *s, int n) {
    int len = (int)strnlen(s, (size_t)MAX(n, 0)), c = 0, pos = 0;
    while (pos < len) { utf8_get(s, len, &pos); c++; }
    return c;
}

int utf8_offset(const char *s, int ncp) {
    const char *p = s;
    for (int i = 0; i < ncp && *p; i++) utf8_next(&p);
    return (int)(p - s);
}

int utf8_prev(const char *s, int pos) {
    if (pos <= 0) return 0;
    /* walk back over up to 3 continuation bytes, then check that the
     * sequence really ends at pos (otherwise the last byte stood alone) */
    int start = pos - 1;
    while (start > 0 && pos - start < 4 && cont((u8)s[start])) start--;
    int q = start;
    utf8_get(s, pos, &q);
    return q == pos ? start : pos - 1;
}

int utf8_after(const char *s, int len, int pos) {
    if (pos >= len) return len;
    utf8_get(s, len, &pos);
    return pos;
}

u32 uc_upper(u32 c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c < 0x100) {
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        if (c == 0xFF) return 0x178;
        if (c == 0xB5) return 0x39C;
        return c;
    }
    if (c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c == 0x17F) return 'S';
        return c;
    }
    if (c >= 0x370 && c <= 0x3FF) {
        if ((c >= 0x3B1 && c <= 0x3C1) || (c >= 0x3C3 && c <= 0x3CB)) return c - 32;
        if (c == 0x3C2) return 0x3A3;
        if (c == 0x3AC) return 0x386;
        if (c >= 0x3AD && c <= 0x3AF) return c - 37;
        if (c == 0x3CC) return 0x38C;
        if (c == 0x3CD || c == 0x3CE) return c - 63;
        return c;
    }
    if (c >= 0x400 && c <= 0x4FF) {
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF)) return (c & 1) ? c - 1 : c;
        if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x1E00 && c <= 0x1EFF) return (c & 1) ? c - 1 : c;   /* Latin Extended Additional */
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}

int utf8_casecmp(const char *a, const char *b) {
    for (;;) {
        u32 x = uc_upper(utf8_next(&a)), y = uc_upper(utf8_next(&b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

int utf16_to_utf8(const u16 *in, int n, char *out, int outsz) {
    int o = 0;
    for (int i = 0; i < n && in[i]; i++) {
        u32 cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= n || in[i + 1] < 0xDC00 || in[i + 1] > 0xDFFF) return -1;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return -1;
        }
        if (cp < 0x20) return -1;
        char tmp[4];
        int l = utf8_put(cp, tmp);
        if (o + l > outsz - 1) return -1;
        memcpy(out + o, tmp, l);
        o += l;
    }
    out[o] = 0;
    return o;
}

int utf8_to_utf16(const char *in, u16 *out, int max) {
    int n = 0;
    for (;;) {
        u32 cp = utf8_next(&in);
        if (!cp) break;
        if (cp >= 0x10000) {
            if (n + 2 > max) return -1;
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > max) return -1;
            out[n++] = (u16)cp;
        }
    }
    return n;
}

static const u16 cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

u32 cp437_to_uc(u8 c) { return c < 0x80 ? c : cp437_hi[c - 0x80]; }
