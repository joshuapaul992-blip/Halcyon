/* NOTEPAD.EXE: the text (a gap buffer) and its display rows, with or
 * without word wrap. See doc.h. */
#include "doc.h"

npdoc_t D;

/* ---------------- memory ---------------- */
#ifdef NP_HOST
#define mmove memmove
#else
/* overlapping copy with the string instructions (the C library's memmove goes byte by byte) */
static void mmove(void *d, const void *s, size_t n) {
    if (!n || d == s) return;
    if ((char *)d < (const char *)s || (char *)d >= (const char *)s + n) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        char *de = (char *)d + n - 1;
        const char *se = (const char *)s + n - 1;
        __asm__ volatile("std; rep movsb; cld" : "+D"(de), "+S"(se), "+c"(n) : : "memory");
    }
}
#endif

/* ---------------- the text ---------------- */
void move_gap(int p) {
    if (p < D.gs) { int n = D.gs - p; mmove(D.tb + D.ge - n, D.tb + p, (size_t)n); D.gs = p; D.ge -= n; }
    else if (p > D.gs) { int n = p - D.gs; mmove(D.tb + D.gs, D.tb + D.ge, (size_t)n); D.gs += n; D.ge += n; }
}
static int text_reserve(int n) {
    if (D.ge - D.gs >= n) return 0;
    int len = tlen();
    long nc = (long)len + n + len / 2 + 65536;
    char *nb = malloc((size_t)nc);
    if (!nb) return -1;
    int after = D.tcap - D.ge;
    mmove(nb, D.tb, (size_t)D.gs);
    mmove(nb + nc - after, D.tb + D.ge, (size_t)after);
    free(D.tb);
    D.tb = nb; D.ge = (int)nc - after; D.tcap = (int)nc;
    return 0;
}
void text_get(int a, int n, char *out) {
    if (n <= 0) return;
    if (a < D.gs) {
        int k = D.gs - a < n ? D.gs - a : n;
        mmove(out, D.tb + a, (size_t)k);
        out += k; a += k; n -= k;
    }
    if (n > 0) mmove(out, D.tb + a + (D.ge - D.gs), (size_t)n);
}
const char *text_span(int a, int b) {
    if (a < D.gs && b > D.gs) move_gap(D.gs - a < b - D.gs ? a : b);
    return b <= D.gs ? D.tb + a : D.tb + a + (D.ge - D.gs);
}
/* n bytes at a: a pointer into the text, or a copy in tmp if the gap is inside */
static const char *piece(int a, int n, char *tmp) {
    if (a + n <= D.gs) return D.tb + a;
    if (a >= D.gs) return D.tb + a + (D.ge - D.gs);
    text_get(a, n, tmp);
    return tmp;
}
int next_ch(int p) {
    char t[4];
    int n = tlen() - p < 4 ? tlen() - p : 4, q = 0;
    if (n <= 0) return p;
    text_get(p, n, t);
    utf8_get(t, n, &q);
    return p + q;
}
int prev_ch(int p) {
    if (p <= 0) return 0;
    char t[4];
    int s = p - 4 < 0 ? 0 : p - 4;
    text_get(s, p - s, t);
    return s + utf8_prev(t, p - s);
}
/* the first newline in [p, lim), or lim */
static int find_nl(int p, int lim) {
    while (p < lim) {
        int end = p < D.gs ? (lim < D.gs ? lim : D.gs) : lim;
        const char *base = p < D.gs ? D.tb : D.tb + (D.ge - D.gs);
        for (const char *q = base + p, *e = base + end; q < e; q++) if (*q == '\n') return (int)(q - base);
        p = end;
    }
    return lim;
}
static int count_nl(int a, int b) {
    int n = 0;
    while (a < b) {
        a = find_nl(a, b);
        if (a < b) { n++; a++; }
    }
    return n;
}

/* ---------------- rows ---------------- */
int line_of(int off) {
    int lo = 0, hi = nlines() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ls(mid) <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}
int line_end(int l) {
    if (l + 1 >= nlines()) return tlen();
    return lsoft(l + 1) ? ls(l + 1) : ls(l + 1) - 1;
}
int hard_newline_after(int l) { return l + 1 < nlines() && !lsoft(l + 1); }
int logical_start(int row) {
    while (row > 0 && lsoft(row)) row--;
    return row;
}
int logical_line(int row) {
    int h = logical_start(row);
    if (!D.nsoft) return h;
    /* walk from the cached line (edits and moves are local, so the walk is short) */
    int c = line_of(D.lc_off), ln = D.lc_ln;
    if (h >= c) { for (int i = c + 1; i <= h; i++) if (!lsoft(i)) ln++; }
    else { for (int i = c; i > h; i--) if (!lsoft(i)) ln--; }
    D.lc_off = ls(h);
    D.lc_ln = ln;
    return ln;
}

/* move the row gap to index k (entries change between the two forms) */
static void lgap_move(int k) {
    int len = tlen();
    while (D.lg > k) { int v = D.la[--D.lg]; D.la[--D.lge] = (len - (v & OFFM)) | (v & SOFT); }
    while (D.lg < k) { int v = D.la[D.lge++]; D.la[D.lg++] = (len - (v & OFFM)) | (v & SOFT); }
}
static int lreserve(int n) {
    if (D.lge - D.lg >= n) return 0;
    long nc = (long)D.lcap + n + D.lcap / 2 + 1024;
    int *na = malloc((size_t)nc * sizeof(int));
    if (!na) return -1;
    int after = D.lcap - D.lge;
    mmove(na, D.la, (size_t)D.lg * sizeof(int));
    mmove(na + nc - after, D.la + D.lge, (size_t)after * sizeof(int));
    free(D.la);
    D.la = na; D.lge = (int)nc - after; D.lcap = (int)nc;
    return 0;
}

/* ---------------- breaking rows ---------------- */
static int adv(unsigned cp, int x) { return cp == '\t' ? (x / D.tabw + 1) * D.tabw - x : D.cwidth(cp); }
int row_width(const char *s, int n) {
    int x = 0, pos = 0;
    while (pos < n) x += adv(utf8_get(s, n, &pos), x);
    return x;
}
int row_break(const char *s, int n, int width) {
    int lim = n, capped = 0;
    if (lim > LINE_BYTES) {
        lim = LINE_BYTES;
        for (int k = 0; k < 3 && ((unsigned char)s[lim] & 0xC0) == 0x80; k++) lim--;   /* not inside a character */
        capped = 1;
    }
    if (width <= 0) return lim;
    int x = 0, pos = 0, brk = 0;
    while (pos < lim) {
        int p2 = pos;
        unsigned cp = utf8_get(s, lim, &p2);
        int w = adv(cp, x);
        if (cp == ' ' || cp == '\t') { x += w; pos = p2; brk = pos; continue; }   /* spaces may hang */
        if (x + w > width && pos > 0) return brk > 0 ? brk : pos;
        x += w;
        pos = p2;
    }
    return capped && brk > 0 ? brk : lim;
}

/* ---------------- rebuilding rows ----------------
 * The row gap is at the row before H; the entries after it are old rows
 * starting at H or later. New rows are made from H on; old rows they cover
 * are dropped as the new ones pass them. Stops at the first row start at
 * or after limit: a line start (the old row there is kept), a soft break
 * (entered, and the rows after it are still old: the caller continues there
 * later), or the end of the text. hard_left: line starts still to come that
 * room was reserved for. Returns where it stopped. */
static inline int after_start(void) { return tlen() - (D.la[D.lge] & OFFM); }
static void drop_after(int upto, int inclusive) {
    while (D.lge < D.lcap) {
        int s = after_start();
        if (inclusive ? s > upto : s >= upto) break;
        if (D.la[D.lge] & SOFT) D.nsoft--;
        D.lge++;
    }
}
/* room for one more row, keeping hard_left entries for the line starts to come */
static int room(int soft, int hard_left) {
    int need = soft ? hard_left + 1 : 1;
    return D.lge - D.lg >= need || !lreserve(need + 256 + D.lcap / 8) ? 0 : -1;
}
static void add_row(int pos, int soft, int *hard_left) {
    D.la[D.lg++] = pos | (soft ? SOFT : 0);
    if (soft) D.nsoft++;
    else if (*hard_left > 0) (*hard_left)--;
}
static int rebuild(int H, int limit, int hard_left) {
    static char tmp[LINE_BYTES + 8];
    int len = tlen(), pos = H, soft = H > 0 && at(H - 1) != '\n';
    int e = -1;                          /* end of the current line */
    for (;;) {
        /* (out of memory: the old rows from here on stay, or a soft break is left out) */
        int ok = room(soft, hard_left) == 0;
        if (!ok && !soft) return pos;
        drop_after(pos, 1);
        if (ok) add_row(pos, soft, &hard_left);
        if (e < pos) e = find_nl(pos, len);
        int n = e - pos, take = 0;
        if (n > 0) {
            int k = n < LINE_BYTES + 4 ? n : LINE_BYTES + 4;
            take = row_break(piece(pos, k, tmp), n, D.wrap);
        }
        int np, nsf;
        if (pos + take < e) { np = pos + take; nsf = 1; }
        else if (e < len) { np = e + 1; nsf = 0; }
        else { drop_after(len, 1); return len; }
        if (np >= limit) {
            if (!nsf) { drop_after(np, 0); return np; }
            if (room(1, hard_left)) return pos;
            drop_after(np, 1);
            add_row(np, 1, &hard_left);
            return np;
        }
        drop_after(np, 0);
        pos = np;
        soft = nsf;
    }
}

/* ---------------- editing ---------------- */
static void drop_range(int i) {
    for (int k = i; k + 1 < D.npend; k++) { D.pa[k] = D.pa[k + 1]; D.pb[k] = D.pb[k + 1]; }
    D.npend--;
}

int doc_replace(int a, int b, const char *s, int n) {
    int len = tlen();
    if (len - (b - a) + n > MAX_TEXT) return E_TOOBIG;
    int L0 = logical_start(line_of(a));
    int H = ls(L0);
    int L2 = line_of(b) + 1;
    while (L2 < nlines() && lsoft(L2)) L2++;
    int has_next = L2 < nlines();
    int H2 = has_next ? ls(L2) : len;
    int nl = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\n') nl++;
    if (text_reserve(n) || lreserve(nl + 5 + (H2 - H + n) / (LINE_BYTES - 3))) return E_NOMEM;
    int d = n - (b - a);
    /* the line cache and the stale ranges (in terms of the old text) */
    if (D.lc_off > a) {
        if (D.lc_off >= b) { D.lc_ln += nl - count_nl(a, b); D.lc_off += d; }
        else { D.lc_ln -= count_nl(H, D.lc_off); D.lc_off = H; }
    }
    for (int i = 0; i < D.npend; i++) {
        if (D.pa[i] >= H2) D.pa[i] += d; else if (D.pa[i] >= H) D.pa[i] = H2 + d;
        if (D.pb[i] >= H2) D.pb[i] += d; else if (D.pb[i] > H) D.pb[i] = H;
    }
    for (int i = 0; i < D.npend; i++) if (D.pa[i] >= D.pb[i]) { drop_range(i); i--; }
    /* drop the old rows [L0, L2) */
    lgap_move(L2);
    for (int i = L0; i < L2; i++) if (D.la[i] & SOFT) D.nsoft--;
    D.lg = L0;
    /* the text */
    move_gap(b);
    D.gs = a;
    if (n) mmove(D.tb + D.gs, s, (size_t)n);
    D.gs += n;
    D.gen++;
    /* new rows (room for the line starts was reserved above) */
    rebuild(H, has_next ? H2 + d : 0x7FFFFFFF, nl + 1);
    return 0;
}

void doc_reset(void) {
    /* give the memory of a large document back to the heap */
    char *t = D.tcap > (1 << 20) ? malloc(65536) : NULL;
    if (t) { free(D.tb); D.tb = t; D.tcap = 65536; }
    int *l = D.lcap > (1 << 16) ? malloc(1024 * sizeof(int)) : NULL;
    if (l) { free(D.la); D.la = l; D.lcap = 1024; }
    D.gs = 0; D.ge = D.tcap;
    D.la[0] = 0; D.lg = 1; D.lge = D.lcap; D.nsoft = 0;
    D.npend = 0;
    D.lc_off = 0; D.lc_ln = 0;
    D.gen++;
}

int doc_init(void) {
    D.tcap = 65536;
    D.tb = malloc((size_t)D.tcap);
    D.lcap = 1024;
    D.la = malloc((size_t)D.lcap * sizeof(int));
    if (!D.tb || !D.la) return E_NOMEM;
    if (D.tabw <= 0) D.tabw = 28;
    doc_reset();
    return 0;
}

/* ---------------- word wrap ---------------- */
void doc_set_wrap(int width, int from) {
    D.wrap = width > 0 ? width : 0;
    int H = ls(logical_start(line_of(from))), len = tlen();
    D.npend = 0;
    if (H < len) { D.pa[D.npend] = H; D.pb[D.npend] = len; D.npend++; }
    if (H > 0) { D.pa[D.npend] = 0; D.pb[D.npend] = H; D.npend++; }
}

int doc_stale_at(void) { return D.npend ? D.pa[0] : -1; }

void doc_rewrap_prefer(int from) {
    int H = ls(logical_start(line_of(from)));
    for (int i = 0; i < D.npend; i++) {
        if (H < D.pa[i] || H >= D.pb[i]) continue;
        int pa = D.pa[i];
        if (pa < H) {                        /* split: [H, pb) first, then [pa, H) */
            if (D.npend == NP_STALE) return;
            for (int k = D.npend; k > i + 1; k--) { D.pa[k] = D.pa[k - 1]; D.pb[k] = D.pb[k - 1]; }
            D.npend++;
            D.pa[i + 1] = pa; D.pb[i + 1] = H;
            D.pa[i] = H;
        }
        /* to the front */
        for (int k = i; k > 0; k--) {
            int ta = D.pa[k], tb = D.pb[k];
            D.pa[k] = D.pa[k - 1]; D.pb[k] = D.pb[k - 1];
            D.pa[k - 1] = ta; D.pb[k - 1] = tb;
        }
        return;
    }
}

int doc_rewrap(int budget) {
    while (D.npend && budget > 0) {
        int pa = D.pa[0], pb = D.pb[0];
        int L0 = line_of(pa);
        if (ls(L0) != pa) { L0 = logical_start(L0); pa = ls(L0); }   /* (never: pa is a row start) */
        lgap_move(L0);
        int lim = pa + budget < pb ? pa + budget : pb;
        int np = rebuild(pa, lim, 0);
        if (np <= pa) { D.npend = 0; break; }       /* out of memory: the old breaks stay */
        budget -= np - pa;
        if (np >= pb) drop_range(0);
        else D.pa[0] = np;
    }
    return D.npend > 0;
}
