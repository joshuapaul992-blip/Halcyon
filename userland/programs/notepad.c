/* NOTEPAD.EXE - Notepad, the text editor, as a user program (the port of
 * kernel/apps/notepad.c, which stays as the fallback).
 *
 * The text is UTF-8 (bytes that are not show as Latin-1, as the kernel draws
 * them, and are saved unchanged). It lives in a gap buffer, so typing costs
 * the same anywhere in a file of several megabytes. The display lines are a
 * second gap array: entries before the gap hold their offset, entries after
 * it their distance from the end of the text, so an edit renumbers nothing
 * after it. A line longer than LINE_BYTES bytes continues on the next display
 * line (as Windows Notepad does past 1024 characters), which bounds the work
 * of drawing and measuring a line. Widths come from the font metrics
 * cached in the program; only the visible lines are drawn. */
#include "hal.h"

#define STATUS_H 20
#define SB_W 17
#define MAX_TEXT (16 * 1024 * 1024)     /* bytes (the kernel's Notepad: 4 MiB) */
#ifndef LINE_BYTES
#define LINE_BYTES 2048                 /* bytes per display line */
#endif
#define SOFT 0x40000000                 /* line entry flag: continues the line before (no newline) */
#define OFFM 0x3FFFFFFF
#define PATHN 520
#define FONT FONT_MONO
#define X0 4                            /* text origin in the client area */
#define Y0 3
#define BLINK_MS 530
#define E_TOOBIG (-1000)
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { NP_NEW = 1, NP_OPEN, NP_SAVE, NP_SAVEAS, NP_EXIT, NP_CUT, NP_COPY, NP_PASTE, NP_DEL, NP_SELALL, NP_TIME,
       NP_ABOUT, NP_FIND, NP_FINDNEXT };
enum { C_VSCROLL = 100, C_STATUS };

static const hal_menu_item_t bar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&New", "Ctrl+N", NP_NEW, 0, 0, 0 },
    { "&Open...", "Ctrl+O", NP_OPEN, 0, 0, 0 },
    { "&Save", "Ctrl+S", NP_SAVE, 0, 0, 0 },
    { "Save &As...", 0, NP_SAVEAS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "E&xit", 0, NP_EXIT, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "Cu&t", "Ctrl+X", NP_CUT, 0, 0, 0 },
    { "&Copy", "Ctrl+C", NP_COPY, 0, 0, 0 },
    { "&Paste", "Ctrl+V", NP_PASTE, 0, 0, 0 },
    { "De&lete", "Del", NP_DEL, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Find...", "Ctrl+F", NP_FIND, 0, 0, 0 },
    { "Find &Next", "F3", NP_FINDNEXT, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Select &All", "Ctrl+A", NP_SELALL, 0, 0, 0 },
    { "Time/&Date", "F5", NP_TIME, 0, 0, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About Notepad", 0, NP_ABOUT, 0, HICON_NOTEPAD, 0 },
};
static const hal_menu_item_t ctx_menu[] = {
    { "Cu&t", 0, NP_CUT, 0, 0, 0 },
    { "&Copy", 0, NP_COPY, 0, 0, 0 },
    { "&Paste", 0, NP_PASTE, 0, 0, 0 },
    { "&Delete", 0, NP_DEL, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Select &All", 0, NP_SELALL, 0, 0, 0 },
};

/* ---------------- state ---------------- */
static int win, cw0 = 620, ch0 = 420;     /* client size */
static char *tb;                          /* text: [0, gs) gap [ge, tcap) */
static int tcap, gs, ge;
static int *la;                           /* display line starts: [0, lg) gap [lge, lcap) */
static int lcap, lg, lge, nsoft;
static int cur, anchor, top, left, pref_x;
static int modified, crlf = 1;
static char path[PATHN];
static char find_text[128];
static int caret_on = 1, active = 1, mouse_sel, dirty = 1, cursor_shape = -1;
static uint64_t caret_t;
static int lh, cwm, tabw;                 /* line height, width of 'M', tab stop */
static hal_theme_t th;
static int sb_total = -1, sb_page = -1, sb_pos = -1;
static char shown_status[160], shown_title[100];
static char lbuf[LINE_BYTES + 8];         /* one display line, contiguous */

/* ---------------- memory ---------------- */
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

/* ---------------- character widths (all of Unicode, cached per 256) ---------------- */
static unsigned char *wpage[0x1100];
static int cwidth(unsigned cp) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    unsigned char *pg = wpage[cp >> 8];
    if (!pg) {
        pg = malloc(256);
        if (!pg) return cwm;
        if (gfx_font_metrics(FONT, cp & ~0xFFu, 256, pg, NULL) < 0) memset(pg, cwm, 256);
        wpage[cp >> 8] = pg;
    }
    return pg[cp & 0xFF];
}
static int adv(unsigned cp, int x) { return cp == '\t' ? (x / tabw + 1) * tabw - x : cwidth(cp); }

/* ---------------- text gap buffer ---------------- */
static inline int tlen(void) { return tcap - (ge - gs); }
static inline unsigned char at(int p) { return (unsigned char)tb[p < gs ? p : p + (ge - gs)]; }

static void move_gap(int p) {
    if (p < gs) { int n = gs - p; mmove(tb + ge - n, tb + p, (size_t)n); gs = p; ge -= n; }
    else if (p > gs) { int n = p - gs; mmove(tb + gs, tb + ge, (size_t)n); gs += n; ge += n; }
}
static int text_reserve(int n) {
    if (ge - gs >= n) return 0;
    int len = tlen();
    long nc = (long)len + n + len / 2 + 65536;
    char *nb = malloc((size_t)nc);
    if (!nb) return -1;
    int after = tcap - ge;
    mmove(nb, tb, (size_t)gs);
    mmove(nb + nc - after, tb + ge, (size_t)after);
    free(tb);
    tb = nb; ge = (int)nc - after; tcap = (int)nc;
    return 0;
}
/* n bytes from a into out */
static void text_get(int a, int n, char *out) {
    if (n <= 0) return;
    if (a < gs) {
        int k = gs - a < n ? gs - a : n;
        mmove(out, tb + a, (size_t)k);
        out += k; a += k; n -= k;
    }
    if (n > 0) mmove(out, tb + a + (ge - gs), (size_t)n);
}
/* [a, b) as one piece of memory (moves the gap out of it if needed) */
static const char *text_span(int a, int b) {
    if (a < gs && b > gs) move_gap(gs - a < b - gs ? a : b);
    return b <= gs ? tb + a : tb + a + (ge - gs);
}
static int next_ch(int p) {
    char t[4];
    int n = tlen() - p < 4 ? tlen() - p : 4, q = 0;
    if (n <= 0) return p;
    text_get(p, n, t);
    utf8_get(t, n, &q);
    return p + q;
}
static int prev_ch(int p) {
    if (p <= 0) return 0;
    char t[4];
    int s = p - 4 < 0 ? 0 : p - 4;
    text_get(s, p - s, t);
    return s + utf8_prev(t, p - s);
}

/* ---------------- display lines ---------------- */
static inline int nlines(void) { return lcap - (lge - lg); }
static inline int lraw(int i) { return i < lg ? la[i] : la[i + (lge - lg)]; }
static inline int ls(int i) { return i < lg ? (la[i] & OFFM) : tlen() - (la[i + (lge - lg)] & OFFM); }
static inline int lsoft(int i) { return lraw(i) & SOFT; }

static int line_of(int off) {
    int lo = 0, hi = nlines() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ls(mid) <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}
/* end of line l's text (before its newline) */
static int line_end(int l) {
    if (l + 1 >= nlines()) return tlen();
    return lsoft(l + 1) ? ls(l + 1) : ls(l + 1) - 1;
}
static int hard_newline_after(int l) { return l + 1 < nlines() && !lsoft(l + 1); }

/* move the line gap to index k (entries change between the two forms) */
static void lgap_move(int k) {
    int len = tlen();
    while (lg > k) { int v = la[--lg]; la[--lge] = (len - (v & OFFM)) | (v & SOFT); }
    while (lg < k) { int v = la[lge++]; la[lg++] = (len - (v & OFFM)) | (v & SOFT); }
}
static int lreserve(int n) {
    if (lge - lg >= n) return 0;
    long nc = (long)lcap + n + lcap / 2 + 1024;
    int *na = malloc((size_t)nc * sizeof(int));
    if (!na) return -1;
    int after = lcap - lge;
    mmove(na, la, (size_t)lg * sizeof(int));
    mmove(na + nc - after, la + lge, (size_t)after * sizeof(int));
    free(la);
    la = na; lge = (int)nc - after; lcap = (int)nc;
    return 0;
}
static void ladd(int pos, int flag) {
    if (lg >= lge) return;              /* never: reserved by the caller */
    la[lg++] = pos | flag;
    if (flag) nsoft++;
}
/* display lines for the text [s, ce) of one line (no newline inside) */
static void break_span(int s, int ce) {
    while (ce - s > LINE_BYTES) {
        int q = s + LINE_BYTES;
        for (int k = 0; k < 3 && (at(q) & 0xC0) == 0x80; k++) q--;    /* not inside a character */
        ladd(q, SOFT);
        s = q;
    }
}

/* Replace [a, b) with n bytes of s (which must not point into the text).
 * Only the lines from the start of a's line to the end of b's are built
 * again; s is scanned for newlines, the rest of the text is not. */
static int replace(int a, int b, const char *s, int n) {
    int len = tlen();
    if (len - (b - a) + n > MAX_TEXT) return E_TOOBIG;
    int L0 = line_of(a);
    while (L0 > 0 && lsoft(L0)) L0--;
    int H = ls(L0);
    int L2 = line_of(b) + 1;
    while (L2 < nlines() && lsoft(L2)) L2++;
    int has_next = L2 < nlines();
    int H2 = has_next ? ls(L2) : len;
    int region = H2 + n - (b - a) - H;
    int nl = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\n') nl++;
    if (text_reserve(n) || lreserve(nl + region / (LINE_BYTES - 3) + 4)) return E_NOMEM;
    /* drop the old entries [L0, L2) */
    lgap_move(L2);
    for (int i = L0; i < L2; i++) if (la[i] & SOFT) nsoft--;
    lg = L0;
    /* the text */
    move_gap(b);
    gs = a;
    mmove(tb + gs, s, (size_t)n);
    gs += n;
    /* new entries */
    int len2 = tlen();
    int E = has_next ? len2 - (la[lge] & OFFM) : len2;
    int s0 = H;
    ladd(H, 0);
    for (int i = 0; i < n; i++) {
        if (s[i] != '\n') continue;
        break_span(s0, a + i);
        s0 = a + i + 1;
        ladd(s0, 0);
    }
    break_span(s0, has_next ? E - 1 : E);
    modified = 1;
    return 0;
}

static void reset_text(void) {
    /* give the memory of a large document back to the heap */
    char *t = tcap > (1 << 20) ? malloc(65536) : NULL;
    if (t) { free(tb); tb = t; tcap = 65536; }
    int *l = lcap > (1 << 16) ? malloc(1024 * sizeof(int)) : NULL;
    if (l) { free(la); la = l; lcap = 1024; }
    gs = 0; ge = tcap;
    la[0] = 0; lg = 1; lge = lcap; nsoft = 0;
    cur = anchor = top = left = pref_x = 0;
}

/* line l into lbuf; returns its length, *a = its offset */
static int fetch(int l, int *a) {
    *a = ls(l);
    int n = line_end(l) - *a;
    if (n > LINE_BYTES + 4) n = LINE_BYTES + 4;
    if (n < 0) n = 0;
    text_get(*a, n, lbuf);
    return n;
}
static int x_in_line(int l, int off) {
    int a, n = fetch(l, &a), x = 0, pos = 0;
    while (pos < n && a + pos < off) x += adv(utf8_get(lbuf, n, &pos), x);
    return x;
}
static int x_of(int off) { return x_in_line(line_of(off), off); }
static int off_at_x(int l, int px) {
    int a, n = fetch(l, &a), x = 0, pos = 0;
    while (pos < n) {
        int p2 = pos;
        int w = adv(utf8_get(lbuf, n, &p2), x);
        if (px < x + w / 2) return a + pos;
        x += w;
        pos = p2;
    }
    return a + n;
}

/* ---------------- editing ---------------- */
static int is_word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }   /* non-ASCII: letters */
static int sel_lo(void) { return cur < anchor ? cur : anchor; }
static int sel_hi(void) { return cur < anchor ? anchor : cur; }

static void del_range(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == b) return;
    if (replace(a, b, "", 0) == 0) cur = anchor = a;
}
static int del_sel(void) {
    if (cur == anchor) return 0;
    del_range(cur, anchor);
    return 1;
}
static int insert(const char *s, int n) {
    int a = sel_lo(), r = replace(a, sel_hi(), s, n);
    if (r == 0) cur = anchor = a + n;
    return r;
}

/* ---------------- paths and the title ---------------- */
static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return b;
}
static void dir_name(const char *p, char *out, int n) {
    strlcpy(out, p, (size_t)n);
    char *b = (char *)base_name(out);
    if (b > out && (b[-1] == '/' || b[-1] == '\\')) b--;
    *b = 0;
    if (strlen(out) == 2 && out[1] == ':') strlcat(out, "/", (size_t)n);
}
static void doc_name(char *out, int n) {
    const char *b = path[0] ? base_name(path) : "Untitled";
    int len = (int)strlen(b);
    if (len >= n) { len = n - 1; while (len > 0 && ((unsigned char)b[len] & 0xC0) == 0x80) len--; }
    memcpy(out, b, (size_t)len);
    out[len] = 0;
}
static void update_title(void) {
    char name[80], t[100];
    doc_name(name, sizeof name);
    snprintf(t, sizeof t, "%s%s - Notepad", modified ? "*" : "", name);
    if (strcmp(t, shown_title)) { strlcpy(shown_title, t, sizeof shown_title); win_set_title(win, t); }
}
static void set_cursor(int c) {
    if (c != cursor_shape) { cursor_shape = c; win_set_cursor(win, c); }
}

/* ---------------- view ---------------- */
static int text_w(void) { return cw0 - SB_W > 8 ? cw0 - SB_W : 8; }
static int text_h(void) { return ch0 - STATUS_H > 8 ? ch0 - STATUS_H : 8; }
static int vis_lines(void) { int v = (text_h() - 4) / lh; return v < 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void scroll_to_caret(void) {
    int l = line_of(cur), vl = vis_lines();
    if (l < top) top = l;
    if (l >= top + vl) top = l - vl + 1;
    int x = x_of(cur), tw = text_w() - 8;
    if (x < left) left = x - tw / 3 > 0 ? x - tw / 3 : 0;
    if (x > left + tw - cwm) left = x - tw + tw / 3;
    caret_on = 1;
    caret_t = time_ms();
}

static void layout(void) {
    ctl_move(win, C_VSCROLL, cw0 - SB_W, 0, SB_W, text_h());
    sb_total = -1;
    top = clampi(top, 0, nlines() - 1);
}

/* one display line at row; selection, then the text in runs of one colour */
static void paint_line(int l, int row) {
    int y = Y0 + row * lh, vw = text_w() - 2;
    int a, n = fetch(l, &a);
    int s0 = sel_lo(), s1 = sel_hi();
    unsigned selc = active ? th.select : 0xC8C8C8;
    unsigned tsel = active ? th.select_text : 0x000000;
    if (s1 > a && s0 <= a + n) {
        int xa = s0 > a ? x_in_line(l, s0) : 0, xb = s1 < a + n ? x_in_line(l, s1) : x_in_line(l, a + n);
        if (s1 > a + n && hard_newline_after(l)) xb += cwm;      /* the newline is selected too */
        if (xb > xa) gfx_fill(win, X0 + xa - left, y, xb - xa, lh, selc);
    }
    int x = 0, pos = 0, run = -1, run_x = 0, run_sel = 0;
    while (pos <= n) {
        int p2 = pos;
        unsigned cp = pos < n ? utf8_get(lbuf, n, &p2) : 0;
        int w = pos < n ? adv(cp, x) : 0;
        int sx = X0 + x - left;
        int sel = a + pos >= s0 && a + pos < s1;
        int visible = pos < n && cp != '\t' && sx + w > 0 && sx < vw;
        if (run >= 0 && (!visible || sel != run_sel || pos - run >= 1024)) {
            gfx_text_n(win, run_x, y, lbuf + run, pos - run, run_sel ? tsel : 0x000000, FONT);
            run = -1;
        }
        if (pos >= n || sx >= vw) break;
        if (visible && run < 0) { run = pos; run_x = sx; run_sel = sel; }
        x += w;
        pos = p2;
    }
}
static void paint_caret(void) {
    if (!active || !caret_on) return;
    int l = line_of(cur);
    if (l < top || l > top + vis_lines()) return;
    gfx_fill(win, X0 + x_in_line(l, cur) - left, Y0 + (l - top) * lh, 2, lh, 0x000000);
}

static void update_bars(void) {
    int vl = vis_lines(), total = nlines() + vl - 1;
    if (total != sb_total || vl != sb_page || top != sb_pos) {
        sb_total = total; sb_page = vl; sb_pos = top;
        ctl_scroll_set(win, C_VSCROLL, total, vl, top, 1);
    }
    /* Ln and Col of the line as in the file (continued display lines count as one) */
    int l = line_of(cur), h = l;
    while (h > 0 && lsoft(h)) h--;
    int ln = h + 1;
    if (nsoft) for (int i = 1; i <= h; i++) if (lsoft(i)) ln--;
    int col = 1;
    for (int i = h; i <= l; i++) {
        int a, len = fetch(i, &a), pos = 0;
        while (pos < len && a + pos < cur) { utf8_get(lbuf, len, &pos); col++; }
    }
    char st[160];
    snprintf(st, sizeof st, "%d bytes, %d lines%s\tLn %d, Col %d", tlen(), nlines() - nsoft, crlf ? ", CR/LF" : "", ln, col);
    if (strcmp(st, shown_status)) { strlcpy(shown_status, st, sizeof shown_status); ctl_set_text(win, C_STATUS, st); }
}

static void paint(void) {
    gfx_theme(&th);
    int tw = text_w(), tht = text_h(), vl = vis_lines() + 1;
    gfx_clip_reset(win);
    gfx_fill(win, 0, 0, tw, tht, 0xFFFFFF);
    gfx_rect(win, 0, 0, tw, tht, 0x7F9DB9);
    gfx_clip(win, 2, 2, tw - 4, tht - 4);
    for (int i = 0; i < vl && top + i < nlines(); i++) paint_line(top + i, i);
    paint_caret();
    gfx_clip_reset(win);
    update_bars();
    win_invalidate(win);
    dirty = 0;
}
/* the caret's line only (blinking) */
static void paint_caret_line(void) {
    int l = line_of(cur), tw = text_w(), tht = text_h();
    if (l < top || l > top + vis_lines()) return;
    gfx_clip(win, 2, 2, tw - 4, tht - 4);
    gfx_fill(win, 2, Y0 + (l - top) * lh, tw - 4, lh, 0xFFFFFF);
    paint_line(l, l - top);
    paint_caret();
    gfx_clip_reset(win);
    win_invalidate(win);
}

/* ---------------- files ---------------- */
static void error_box(const char *what, const char *p, int r) {
    char msg[PATHN + 120];
    if (r == E_TOOBIG) snprintf(msg, sizeof msg, "The file %s is too large for Notepad.\n\nNotepad opens files of up to %d MB.", base_name(p), MAX_TEXT >> 20);
    else snprintf(msg, sizeof msg, "Cannot %s %s.\n\n%s", what, p, hal_strerror(r));
    msgbox(win, "Notepad", msg, MBX_ICON_ERROR, MBX_OK);
}

static int load_file(const char *p) {
    int fd = open(p, O_READ);
    if (fd < 0) return fd;
    long size = fsize(fd);
    if (size < 0) { close(fd); return (int)size; }
    if (size > MAX_TEXT) { close(fd); return E_TOOBIG; }
    char *data = malloc((size_t)size + 1);
    if (!data) { close(fd); return E_NOMEM; }
    set_cursor(CUR_WAIT);
    long got = read(fd, data, (size_t)size);
    close(fd);
    if (got < 0) { free(data); return (int)got; }
    /* CR LF and lone CR become LF (saved as CR LF again); NUL becomes a space */
    int o = 0, cr = 0;
    for (long i = 0; i < got; i++) {
        char c = data[i];
        if (c == '\r') { if (i + 1 < got && data[i + 1] == '\n') { cr = 1; continue; } c = '\n'; }
        if (c == 0) c = ' ';
        data[o++] = c;
    }
    reset_text();
    int r = replace(0, 0, data, o);
    free(data);
    if (r < 0) { reset_text(); path[0] = 0; modified = 0; update_title(); dirty = 1; return r; }
    crlf = cr || got == 0;
    modified = 0;
    strlcpy(path, p, sizeof path);
    update_title();
    dirty = 1;
    return 0;
}

/* write [s, s+n) with CR LF line ends through a 64 KiB buffer */
static long put_text(int fd, const char *s, int n, char *out, int *o) {
    for (int i = 0; i < n; i++) {
        if (*o >= 65536 - 2) {
            long w = write(fd, out, (size_t)*o);
            if (w != *o) return w < 0 ? w : E_NOSPC;
            *o = 0;
        }
        if (crlf && s[i] == '\n') out[(*o)++] = '\r';
        out[(*o)++] = s[i];
    }
    return 0;
}

static int save_to(const char *p) {
    char *out = malloc(65536);
    if (!out) { error_box("save", p, E_NOMEM); return E_NOMEM; }
    int prev = cursor_shape;
    set_cursor(CUR_WAIT);
    long r = open(p, O_WRITE | O_CREATE | O_TRUNC);
    if (r >= 0) {
        int fd = (int)r, o = 0;
        r = put_text(fd, tb, gs, out, &o);
        if (!r) r = put_text(fd, tb + ge, tcap - ge, out, &o);
        if (!r && o) { long w = write(fd, out, (size_t)o); if (w != o) r = w < 0 ? w : E_NOSPC; }
        close(fd);
    }
    free(out);
    set_cursor(prev);
    if (r < 0) { error_box("save", p, (int)r); return (int)r; }
    strlcpy(path, p, sizeof path);
    modified = 0;
    update_title();
    dirty = 1;
    return 0;
}

static void start_dir(char *dir, int n) {
    hal_dirent_t st;
    if (path[0]) dir_name(path, dir, n);
    else { strlcpy(dir, "C:/Documents", (size_t)n); if (stat(dir, &st) < 0) strlcpy(dir, "C:/", (size_t)n); }
}

static int save_as(void) {
    char p[PATHN] = "", dir[PATHN];
    start_dir(dir, sizeof dir);
    if (!file_dialog(win, 1, "Save As", dir, NULL, p, sizeof p)) return -1;
    if (!strchr(base_name(p), '.')) strlcat(p, ".txt", sizeof p);
    hal_dirent_t st;
    if (stat(p, &st) == 0) {
        char msg[PATHN + 64];
        snprintf(msg, sizeof msg, "%s already exists.\nDo you want to replace it?", base_name(p));
        if (msgbox(win, "Save As", msg, MBX_ICON_WARNING, MBX_YESNO) != IDR_YES) return -1;
    }
    return save_to(p);
}
static int save(void) { return path[0] ? save_to(path) : save_as(); }

static int confirm_discard(void) {
    if (!modified) return 1;
    char name[80], msg[200];
    doc_name(name, sizeof name);
    snprintf(msg, sizeof msg, "The text in the %s file has changed.\n\nDo you want to save the changes?", name);
    int r = msgbox(win, "Notepad", msg, MBX_ICON_WARNING, MBX_YESNOCANCEL);
    if (r == IDR_YES) return save() == 0;
    return r == IDR_NO;
}

static void open_initial(const char *p) {
    char full[PATHN];
    if (!strchr(p, ':') && p[0] != '/' && p[0] != '\\' && getcwd(full, sizeof full) >= 0) {
        size_t l = strlen(full);
        if (l && full[l - 1] != '/' && full[l - 1] != '\\') strlcat(full, "/", sizeof full);
        strlcat(full, p, sizeof full);
    } else {
        strlcpy(full, p, sizeof full);
    }
    int r = load_file(full);
    if (r == E_NOENT) {             /* a new file at that path */
        strlcpy(path, full, sizeof path);
        update_title();
    } else if (r < 0) {
        error_box("open", full, r);
    }
}

/* ---------------- clipboard and find ---------------- */
static int copy_sel(void) {
    int s0 = sel_lo(), s1 = sel_hi();
    if (s1 <= s0) return 0;
    if (s1 - s0 > (1 << 20)) {
        msgbox(win, "Notepad", "The selection is too large for the clipboard.\n\nYou can copy up to 1 MB of text.", MBX_ICON_WARNING, MBX_OK);
        return 0;
    }
    int r = clipboard_set_n(text_span(s0, s1), s1 - s0);
    if (r < 0) { msgbox(win, "Notepad", "Cannot copy the text: not enough memory.", MBX_ICON_ERROR, MBX_OK); return 0; }
    return 1;
}
static void paste(void) {
    int n = clipboard_get(NULL, 0);
    if (n <= 0) return;
    char *b = malloc((size_t)n + 1);
    if (!b) return;
    n = clipboard_get(b, n + 1);
    int o = 0;
    for (int i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\r') { if (i + 1 < n && b[i + 1] == '\n') continue; c = '\n'; }
        if (c == 0) c = ' ';
        b[o++] = c;
    }
    int r = o ? insert(b, o) : 0;
    free(b);
    if (r == E_TOOBIG) msgbox(win, "Notepad", "The text is too large for Notepad.", MBX_ICON_WARNING, MBX_OK);
    else if (r < 0) msgbox(win, "Notepad", "Not enough memory to paste the text.", MBX_ICON_ERROR, MBX_OK);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static void do_find(int ask) {
    if ((ask || !find_text[0]) && (!inputbox(win, "Find", "Find what:", find_text, sizeof find_text) || !find_text[0])) return;
    int fl = (int)strlen(find_text), len = tlen();
    move_gap(len);
    const char *t = tb;
    int f0 = lower((unsigned char)find_text[0]);
    int start = sel_hi();
    for (int k = 0; k < len; k++) {
        int i = start + k;
        if (i >= len) i -= len;
        if (i + fl > len || lower((unsigned char)t[i]) != f0) continue;
        int j = 1;
        while (j < fl && lower((unsigned char)t[i + j]) == lower((unsigned char)find_text[j])) j++;
        if (j == fl) {
            anchor = i; cur = i + fl;
            pref_x = x_of(cur);
            scroll_to_caret();
            dirty = 1;
            return;
        }
    }
    msgbox(win, "Notepad", "Cannot find the text.", MBX_ICON_INFO, MBX_OK);
}

/* ---------------- commands ---------------- */
static void quit(void) {
    win_destroy(win);
    exit(0);
}

static void command(int id) {
    switch (id) {
    case NP_NEW:
        if (!confirm_discard()) return;
        reset_text();
        path[0] = 0; modified = 0; crlf = 1;
        break;
    case NP_OPEN: {
        if (!confirm_discard()) return;
        char p[PATHN], dir[PATHN];
        if (path[0]) dir_name(path, dir, sizeof dir); else strlcpy(dir, "C:/Documents", sizeof dir);
        if (file_dialog(win, 0, "Open", dir, NULL, p, sizeof p)) {
            int r = load_file(p);
            set_cursor(CUR_IBEAM);
            if (r < 0) error_box("open", p, r);
        }
        break;
    }
    case NP_SAVE: save(); break;
    case NP_SAVEAS: save_as(); break;
    case NP_EXIT: if (confirm_discard()) quit(); return;
    case NP_CUT: if (copy_sel()) del_sel(); break;
    case NP_COPY: copy_sel(); break;
    case NP_PASTE: paste(); break;
    case NP_DEL: if (!del_sel() && cur < tlen()) del_range(cur, next_ch(cur)); break;
    case NP_SELALL: anchor = 0; cur = tlen(); break;
    case NP_FIND: do_find(1); return;
    case NP_FINDNEXT: do_find(0); return;
    case NP_TIME: {
        hal_datetime_t d;
        if (datetime(&d) < 0) return;
        char s[48];
        snprintf(s, sizeof s, "%d:%02d %s %d/%d/%d", d.hour % 12 ? d.hour % 12 : 12, d.minute, d.hour < 12 ? "AM" : "PM",
                 d.month, d.day, d.year);
        insert(s, (int)strlen(s));
        break;
    }
    case NP_ABOUT:
        msgbox(win, "About Notepad", "Halcyon Notepad\nA simple text editor.\n\nFiles are saved with Windows-style line endings.",
               MBX_ICON_INFO, MBX_OK);
        return;
    default: return;
    }
    pref_x = x_of(cur);
    scroll_to_caret();
    update_title();
    dirty = 1;
}

/* ---------------- input ---------------- */
static int in_text(int x, int y) { return x >= 0 && y >= 0 && x < text_w() && y < text_h(); }

static int pos_at(int x, int y) {
    int dy = y - Y0;
    int l = top + (dy >= 0 ? dy / lh : -1 - (-dy - 1) / lh);
    l = clampi(l, 0, nlines() - 1);
    return off_at_x(l, x - X0 + left);
}

static void on_mouse(const hal_event_t *ev) {
    switch (ev->type) {
    case EVT_MOUSE_DOWN:
    case EVT_DBLCLICK:
        if (!in_text(ev->x, ev->y)) return;
        if (ev->button == MOUSE_RIGHT) { menu_popup(win, ctx_menu, N(ctx_menu), ev->x, ev->y); return; }
        if (ev->button != MOUSE_LEFT) return;
        cur = pos_at(ev->x, ev->y);
        if (ev->type == EVT_DBLCLICK) {
            int a = cur, b = cur, len = tlen();
            while (a > 0 && is_word(at(a - 1))) a--;
            while (b < len && is_word(at(b))) b++;
            anchor = a; cur = b;
        } else if (!(ev->mods & MOD_SHIFT)) {
            anchor = cur;
        }
        mouse_sel = 1;
        pref_x = x_of(cur);
        scroll_to_caret();
        dirty = 1;
        return;
    case EVT_MOUSE_MOVE:
        if (mouse_sel && (ev->buttons & MOUSE_LEFT)) {
            int c = pos_at(ev->x, ev->y);
            if (c != cur) { cur = c; pref_x = x_of(cur); scroll_to_caret(); dirty = 1; }
        }
        set_cursor(in_text(ev->x, ev->y) || mouse_sel ? CUR_IBEAM : CUR_ARROW);
        return;
    case EVT_MOUSE_UP:
        mouse_sel = 0;
        return;
    }
}

static void on_key(const hal_event_t *ev) {
    if (!ev->pressed) return;
    /* Ctrl+Alt is AltGr when the key types a character */
    int text = ev->ch >= 32 && ev->ch != 127;
    int shift = ev->mods & MOD_SHIFT, ctrl = (ev->mods & MOD_CTRL) && !text;
    int len = tlen(), l = line_of(cur), vl = vis_lines();
    int moved = 1, keep_pref = 0;
    if (ctrl) {
        static const struct { char letter; int id; } keys[] = {
            { 'N', NP_NEW }, { 'O', NP_OPEN }, { 'S', NP_SAVE }, { 'F', NP_FIND }, { 'A', NP_SELALL },
            { 'C', NP_COPY }, { 'X', NP_CUT }, { 'V', NP_PASTE },
        };
        for (int i = 0; i < N(keys); i++) if (key_shortcut(ev, keys[i].letter)) { command(keys[i].id); return; }
        switch (ev->id) {
        case KEY_HOME: cur = 0; break;
        case KEY_END: cur = len; break;
        case KEY_LEFT:
            while (cur > 0 && !is_word(at(cur - 1))) cur--;
            while (cur > 0 && is_word(at(cur - 1))) cur--;
            break;
        case KEY_RIGHT:
            while (cur < len && is_word(at(cur))) cur++;
            while (cur < len && !is_word(at(cur))) cur++;
            break;
        case KEY_INSERT: command(NP_COPY); return;
        default: return;
        }
    } else {
        switch (ev->id) {
        case KEY_LEFT:
            if (!shift && cur != anchor) cur = sel_lo(); else cur = prev_ch(cur);
            break;
        case KEY_RIGHT:
            if (!shift && cur != anchor) cur = sel_hi(); else cur = next_ch(cur);
            break;
        case KEY_UP: if (l > 0) cur = off_at_x(l - 1, pref_x); keep_pref = 1; break;
        case KEY_DOWN: if (l + 1 < nlines()) cur = off_at_x(l + 1, pref_x); keep_pref = 1; break;
        case KEY_PAGEUP:
            cur = off_at_x(l - vl > 0 ? l - vl : 0, pref_x);
            top = top - vl > 0 ? top - vl : 0;
            keep_pref = 1;
            break;
        case KEY_PAGEDOWN:
            cur = off_at_x(l + vl < nlines() - 1 ? l + vl : nlines() - 1, pref_x);
            top = clampi(top + vl, 0, nlines() - 1);
            keep_pref = 1;
            break;
        case KEY_HOME: {
            /* smart home: first non-blank, then column 0 */
            int s = ls(l), e = line_end(l), fnb = s;
            while (fnb < e && (at(fnb) == ' ' || at(fnb) == '\t')) fnb++;
            cur = cur == fnb ? s : fnb;
            break;
        }
        case KEY_END: cur = line_end(l); break;
        case KEY_BACKSPACE:
            if (!del_sel() && cur > 0) del_range(prev_ch(cur), cur);
            moved = 0;
            break;
        case KEY_DELETE:
            if (shift) { command(NP_CUT); return; }
            if (!del_sel() && cur < len) del_range(cur, next_ch(cur));
            moved = 0;
            break;
        case KEY_INSERT:
            if (shift) command(NP_PASTE);
            return;
        case KEY_ENTER: case KEY_KP_ENTER: {
            /* keep the indentation */
            char ind[64];
            int ni = 0;
            ind[ni++] = '\n';
            for (int i = ls(l); i < cur && ni < 63 && (at(i) == ' ' || at(i) == '\t'); i++) ind[ni++] = (char)at(i);
            insert(ind, ni);
            moved = 0;
            break;
        }
        case KEY_TAB: insert("\t", 1); moved = 0; break;
        case KEY_F3: command(NP_FINDNEXT); return;
        case KEY_F5: command(NP_TIME); return;
        case KEY_ESC: return;
        default:
            if (text) { char u[4]; insert(u, utf8_put(ev->ch, u)); moved = 0; }
            else return;
        }
    }
    if (moved && !shift) anchor = cur;
    if (!moved) anchor = cur;
    if (!keep_pref) pref_x = x_of(cur);
    scroll_to_caret();
    update_title();
    dirty = 1;
}

static void handle(const hal_event_t *ev) {
    switch (ev->type) {
    case EVT_PAINT: dirty = 1; break;
    case EVT_CLOSE: if (confirm_discard()) quit(); break;
    case EVT_RESIZE: cw0 = ev->x; ch0 = ev->y; layout(); dirty = 1; break;
    case EVT_FOCUS: active = ev->pressed; caret_on = 1; caret_t = time_ms(); dirty = 1; break;
    case EVT_MENU: command(ev->id); break;
    case EVT_SCROLL:
        if (ev->id == C_VSCROLL) { top = clampi(ev->x, 0, nlines() - 1); sb_pos = ev->x; dirty = 1; }
        break;
    case EVT_WHEEL: top = clampi(top - ev->dz * 3, 0, nlines() - 1); dirty = 1; break;
    case EVT_KEY: on_key(ev); break;
    case EVT_MOUSE_DOWN: case EVT_DBLCLICK: case EVT_MOUSE_MOVE: case EVT_MOUSE_UP: on_mouse(ev); break;
    case EVT_MOUSE_LEAVE: if (!mouse_sel) set_cursor(CUR_ARROW); break;
    case EVT_DRAG: {                            /* a file dropped on the window opens */
        char p[PATHN];
        if (dnd_open_target(ev, NULL, p, sizeof p) && confirm_discard()) {
            int r = load_file(p);
            set_cursor(CUR_IBEAM);
            if (r < 0) error_box("open", p, r);
        }
        break;
    }
    }
}

int main(int argc, char **argv) {
    tcap = 65536;
    tb = malloc((size_t)tcap);
    lcap = 1024;
    la = malloc((size_t)lcap * sizeof(int));
    if (!tb || !la) return 1;
    lh = text_height(FONT) + 1;
    cwm = text_char_width('M', FONT);
    if (cwm <= 0) cwm = 7;
    tabw = 4 * cwm;
    reset_text();

    win = win_create("Untitled - Notepad", cw0, ch0, WIN_RESIZABLE);
    if (win < 0) return 1;
    win_set_icon(win, HICON_NOTEPAD);
    win_set_min_size(win, 200, 100);
    menu_set(win, bar, N(bar));
    ctl_create(win, CTL_VSCROLL, cw0 - SB_W, 0, SB_W, text_h(), 0, C_VSCROLL, 0);
    ctl_create(win, CTL_STATUSBAR, 0, 0, 0, STATUS_H, "", C_STATUS, 0);
    ctl_set_value(win, C_STATUS, 160);
    ctl_focus(win, 0);
    win_size(win, &cw0, &ch0);
    layout();
    strlcpy(shown_title, "Untitled - Notepad", sizeof shown_title);
    set_cursor(CUR_IBEAM);

    if (argc > 1) {
        /* one quoted argument from the shell; unquoted words are one name with spaces */
        char p[PATHN] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(p, " ", sizeof p);
            strlcat(p, argv[i], sizeof p);
        }
        if (p[0]) open_initial(p);
        set_cursor(CUR_IBEAM);
    }
    caret_t = time_ms();

    for (;;) {
        hal_event_t ev;
        if (dirty) {
            /* take what is queued first (key repeat, wheel...), then draw once */
            int n = 0;
            uint64_t t0 = time_ms();
            while (n < 64 && time_ms() - t0 < 40 && win_get_event(&ev, 0)) { handle(&ev); n++; }
            if (dirty) paint();
            continue;
        }
        int timeout = -1;
        if (active) {
            long t = (long)(caret_t + BLINK_MS - time_ms());
            timeout = t < 0 ? 0 : (int)t;
        }
        if (win_get_event(&ev, timeout)) { handle(&ev); continue; }
        if (active && time_ms() - caret_t >= BLINK_MS) {
            caret_on = !caret_on;
            caret_t = time_ms();
            paint_caret_line();
        }
    }
}
