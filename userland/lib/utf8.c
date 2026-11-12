/* UTF-8 with the kernel's rules (an invalid byte is one Latin-1 character),
 * as declared in hal.h. Separate from gui.c so that host tests (tests/host
 * notepadtest) can compile it: no system calls, no includes. */
unsigned utf8_get(const char *s, int len, int *pos);
int utf8_put(unsigned cp, char *o);
int utf8_prev(const char *s, int pos);

static int cont(unsigned char c) { return (c & 0xC0) == 0x80; }
static int seq_len(const unsigned char *s, int avail, unsigned *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (avail < 2 || !cont(s[1])) return 0;
        *cp = ((unsigned)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (avail < 3 || !cont(s[1]) || !cont(s[2])) return 0;
        if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] >= 0xA0)) return 0;
        *cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (avail < 4 || !cont(s[1]) || !cont(s[2]) || !cont(s[3])) return 0;
        if ((c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] >= 0x90)) return 0;
        *cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}
unsigned utf8_get(const char *s, int len, int *pos) {
    if (*pos >= len) return 0;
    const unsigned char *p = (const unsigned char *)s + *pos;
    unsigned cp;
    int n = seq_len(p, len - *pos < 4 ? len - *pos : 4, &cp);
    if (!n) { *pos += 1; return p[0]; }
    *pos += n;
    return cp;
}
int utf8_put(unsigned cp, char *o) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
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
int utf8_prev(const char *s, int pos) {
    if (pos <= 0) return 0;
    int start = pos - 1;
    while (start > 0 && pos - start < 4 && cont((unsigned char)s[start])) start--;
    int q = start;
    utf8_get(s, pos, &q);
    return q == pos ? start : pos - 1;
}

