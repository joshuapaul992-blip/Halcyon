/* NOTEPAD.EXE - Notepad, the text editor, as a user program (the port of
 * kernel/apps/notepad.c, which stays as the fallback).
 *
 * The text is UTF-8 (bytes that are not show as Latin-1, as the kernel draws
 * them, and are saved unchanged). The text and its display rows live in
 * notepad/doc.c (a gap buffer and a gap array of row starts, so typing costs
 * the same anywhere in a file of several megabytes), with word wrap that is
 * rebuilt a slice at a time when the window is resized; undo and redo in
 * notepad/undo.c. Both are tested on the host (tests/host/notepadtest.c).
 * Widths come from the font metrics cached in the program; only the
 * visible rows are drawn. Word wrap, the font and the status bar are kept
 * in SETTINGS.INI (notepad.wrap, notepad.font, notepad.statusbar). See
 * docs/DESIGN-M8-NOTEPAD-HELP.md. */
#include "hal.h"
#include "notepad/doc.h"
#include "notepad/undo.h"

#define STATUS_H 20
#define SB_W 17
#define PATHN 520
#define X0 4                            /* text origin in the client area */
#define Y0 3
#define BLINK_MS 530
#define UNDO_LIMIT (4u << 20)           /* bytes of text kept for undo */
#define REWRAP_SLICE (64 << 10)         /* bytes of text rewrapped between events */
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { NP_NEW = 1, NP_OPEN, NP_SAVE, NP_SAVEAS, NP_EXIT, NP_CUT, NP_COPY, NP_PASTE, NP_DEL, NP_SELALL, NP_TIME,
       NP_ABOUT, NP_FIND, NP_FINDNEXT, NP_UNDO, NP_REDO, NP_REPLACE, NP_GOTO, NP_WRAP, NP_FONT, NP_STATUSBAR,
       NP_HELP };
enum { C_VSCROLL = 100, C_HSCROLL, C_STATUS };

static const hal_menu_item_t bar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&New", "Ctrl+N", NP_NEW, 0, 0, 0 },
    { "&Open...", "Ctrl+O", NP_OPEN, 0, HICON_FOLDER_OPEN, 0 },
    { "&Save", "Ctrl+S", NP_SAVE, 0, 0, 0 },
    { "Save &As...", 0, NP_SAVEAS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "E&xit", 0, NP_EXIT, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "&Undo", "Ctrl+Z", NP_UNDO, MENU_DISABLED, 0, 0 },
    { "&Redo", "Ctrl+Y", NP_REDO, MENU_DISABLED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Cu&t", "Ctrl+X", NP_CUT, MENU_DISABLED, 0, 0 },
    { "&Copy", "Ctrl+C", NP_COPY, MENU_DISABLED, 0, 0 },
    { "&Paste", "Ctrl+V", NP_PASTE, 0, 0, 0 },
    { "De&lete", "Del", NP_DEL, MENU_DISABLED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Find...", "Ctrl+F", NP_FIND, 0, 0, 0 },
    { "Find &Next", "F3", NP_FINDNEXT, 0, 0, 0 },
    { "R&eplace...", "Ctrl+H", NP_REPLACE, 0, 0, 0 },
    { "&Go To...", "Ctrl+G", NP_GOTO, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Select &All", "Ctrl+A", NP_SELALL, 0, 0, 0 },
    { "Time/&Date", "F5", NP_TIME, 0, 0, 0 },
    { "F&ormat", 0, 0, MENU_TITLE, 0, 0 },
    { "&Word Wrap", 0, NP_WRAP, 0, 0, 0 },
    { "&Font...", 0, NP_FONT, 0, 0, 0 },
    { "&View", 0, 0, MENU_TITLE, 0, 0 },
    { "&Status Bar", 0, NP_STATUSBAR, MENU_CHECKED, 0, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&Help Topics", "F1", NP_HELP, 0, HICON_HELP, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&About Notepad", 0, NP_ABOUT, 0, HICON_NOTEPAD, 0 },
};
static hal_menu_item_t ctx_menu[] = {
    { "&Undo", 0, NP_UNDO, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Cu&t", 0, NP_CUT, 0, 0, 0 },
    { "&Copy", 0, NP_COPY, 0, 0, 0 },
    { "&Paste", 0, NP_PASTE, 0, 0, 0 },
    { "&Delete", 0, NP_DEL, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Select &All", 0, NP_SELALL, 0, 0, 0 },
};

/* the fonts Notepad offers (Format > Font...), by their SETTINGS.INI name */
static const struct { const char *key, *label; int font; } fonts[] = {
    { "mono", "Monospace, 12 pixels", FONT_MONO },
    { "sans9", "Sans Serif, 9 pixels", FONT_SMALL },
    { "sans", "Sans Serif, 11 pixels", FONT_UI },
    { "sansbold", "Sans Serif Bold, 11 pixels", FONT_UI_BOLD },
    { "sansbold12", "Sans Serif Bold, 12 pixels", FONT_TITLE },
    { "sans20", "Sans Serif Bold, 20 pixels (Latin-1 only)", FONT_LARGE },
};

/* ---------------- state ---------------- */
static int win, cw0 = 620, ch0 = 420;     /* client size */
static int cur, anchor, top, left, pref_x, eol;   /* eol: the caret shows at the end of the row before */
static int modified, crlf = 1;
static char path[PATHN];
static char find_text[128], repl_text[128];
static int match_case;
static int caret_on = 1, active = 1, mouse_sel, dirty = 1, cursor_shape = -1;
static uint64_t caret_t;
static int font = FONT_MONO, wrap, status_on = 1;
static int lh, cwm, tabw;                 /* line height, width of 'M', tab stop */
static int hs_max;                        /* widest row seen (horizontal scroll range) */
static hal_theme_t th;
static int sb_total = -1, sb_page = -1, sb_pos = -1, hb_total = -1, hb_page = -1, hb_pos = -1;
static int menu_state_shown = -1;
static char shown_status[160], shown_title[100];
static char lbuf[LINE_BYTES + 8];         /* one row, contiguous */
static undo_t U;

/* ---------------- character widths (all of Unicode, cached per 256) ---------------- */
static unsigned char *wpage[0x1100];
static int cwidth(unsigned cp) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    unsigned char *pg = wpage[cp >> 8];
    if (!pg) {
        pg = malloc(256);
        if (!pg) return cwm;
        if (gfx_font_metrics(font, cp & ~0xFFu, 256, pg, NULL) < 0) memset(pg, cwm, 256);
        wpage[cp >> 8] = pg;
    }
    return pg[cp & 0xFF];
}
static int adv(unsigned cp, int x) { return cp == '\t' ? (x / tabw + 1) * tabw - x : cwidth(cp); }
static void set_metrics(void) {
    for (int i = 0; i < N(wpage); i++) { free(wpage[i]); wpage[i] = NULL; }
    lh = text_height(font) + 1;
    cwm = text_char_width('M', font);
    if (cwm <= 0) cwm = 7;
    tabw = 4 * cwm;
    D.tabw = tabw;
    D.cwidth = cwidth;
    hs_max = 0;
}

/* ---------------- rows on screen ---------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the row the caret is drawn in: at a soft break it may belong to the row before */
static int caret_row(void) {
    int l = line_of(cur);
    return eol && l > 0 && lsoft(l) && ls(l) == cur ? l - 1 : l;
}
/* after placing the caret at the end of row l */
static void set_eol(int l) { eol = l + 1 < nlines() && lsoft(l + 1) && cur == ls(l + 1); }

/* row l into lbuf; returns its length, *a = its offset */
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
static int caret_x(void) { return x_in_line(caret_row(), cur); }
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
/* put the caret in row l at x (Up, Down, clicks) */
static void caret_to(int l, int px) { cur = off_at_x(l, px); set_eol(l); }

/* ---------------- editing (every change goes through edit()) ---------------- */
static int is_word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }   /* non-ASCII: letters */
static int sel_lo(void) { return cur < anchor ? cur : anchor; }
static int sel_hi(void) { return cur < anchor ? anchor : cur; }
static void update_modified(void) { modified = !undo_is_saved(&U); }

static void too_big(void) { msgbox(win, "Notepad", "The text is too large for Notepad.", MBX_ICON_WARNING, MBX_OK); }

/* replace [a, b) with s (n bytes), as one undo kind (UNDO_*); the caret goes after it */
static int edit(int kind, int a, int b, const char *s, int n) {
    if (tlen() - (b - a) + n > MAX_TEXT) { too_big(); return E_TOOBIG; }
    int c1 = a + n;
    int r = undo_record(&U, kind, a, b > a ? text_span(a, b) : "", b - a, s, n, cur, anchor, c1, c1, time_ms());
    int r2 = doc_replace(a, b, s, n);
    if (r2 < 0) {                        /* (no memory) the history no longer matches the text */
        undo_clear(&U);
        U.saved = -1;
        if (r2 == E_TOOBIG) too_big();
        else msgbox(win, "Notepad", "Not enough memory for this change.", MBX_ICON_ERROR, MBX_OK);
        update_modified();
        return r2;
    }
    if (r < 0) msgbox(win, "Notepad", "Not enough memory to keep this change for Undo.\n\nUndo starts again from here.",
                      MBX_ICON_WARNING, MBX_OK);
    cur = anchor = c1;
    eol = 0;
    update_modified();
    return 0;
}
static int del_sel(void) {
    if (cur == anchor) return 0;
    edit(UNDO_OTHER, sel_lo(), sel_hi(), "", 0);
    return 1;
}
static int insert(int kind, const char *s, int n) { return edit(kind, sel_lo(), sel_hi(), s, n); }

static int undo_fn(void *ctx, int a, int b, const char *s, int n) { (void)ctx; return doc_replace(a, b, s, n); }
static void do_undo(int redo) {
    int c, an, r = redo ? undo_redo(&U, undo_fn, NULL, &c, &an) : undo_undo(&U, undo_fn, NULL, &c, &an);
    if (r < 0) msgbox(win, "Notepad", "Not enough memory to undo this change.", MBX_ICON_ERROR, MBX_OK);
    if (r == 1) {
        int len = tlen();
        cur = clampi(c, 0, len);
        anchor = clampi(an, 0, len);
        eol = 0;
    }
    update_modified();
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
static int status_h(void) { return status_on ? STATUS_H : 0; }
static int text_w(void) { return cw0 - SB_W > 8 ? cw0 - SB_W : 8; }
static int text_h(void) { int h = ch0 - status_h() - (wrap ? 0 : SB_W); return h > 8 ? h : 8; }
static int vis_lines(void) { int v = (text_h() - 4) / lh; return v < 1 ? 1 : v; }
static int wrap_width(void) { int w = text_w() - X0 - 6; return w > 8 ? w : 8; }

static void scroll_to_caret(void) {
    int l = caret_row(), vl = vis_lines();
    if (l < top) top = l;
    if (l >= top + vl) top = l - vl + 1;
    if (wrap) left = 0;
    else {
        int x = caret_x(), tw = text_w() - 8;
        if (x < left) left = x - tw / 3 > 0 ? x - tw / 3 : 0;
        if (x > left + tw - cwm) left = x - tw + tw / 3;
    }
    caret_on = 1;
    caret_t = time_ms();
}

/* stale rows (after a resize): rebuild those in view now, the rest between events */
static void rewrap_slice(int budget) {
    int top_off = ls(top);
    doc_rewrap(budget);
    top = clampi(line_of(top_off), 0, nlines() - 1);
}
static void sync_view(void) {
    if (doc_stale_at() < 0) return;
    int bottom = clampi(top + vis_lines() + 1, 0, nlines() - 1);
    int st = doc_stale_at();
    if (st > line_end(bottom)) {
        /* the next stale rows are below the view: are any in it? */
        for (int i = 0; i < D.npend; i++) if (D.pa[i] <= line_end(bottom) && D.pb[i] > ls(top)) st = -2;
        if (st != -2) return;
    }
    doc_rewrap_prefer(ls(top));
    for (int i = 0; i < 400; i++) {
        st = doc_stale_at();
        if (st < 0 || st < ls(logical_start(top)) || st > line_end(clampi(top + vis_lines() + 1, 0, nlines() - 1))) break;
        rewrap_slice(16 << 10);
    }
}
static void apply_wrap(void) {
    int w = wrap ? wrap_width() : 0;
    if (w == D.wrap) return;
    doc_set_wrap(w, ls(top));
    sync_view();
}

static void layout(void) {
    ctl_move(win, C_VSCROLL, cw0 - SB_W, 0, SB_W, text_h());
    ctl_move(win, C_HSCROLL, 0, text_h(), text_w(), SB_W);
    ctl_set_style(win, C_HSCROLL, wrap ? CTL_HIDDEN : 0);
    ctl_set_style(win, C_STATUS, status_on ? 0 : CTL_HIDDEN);
    sb_total = hb_total = -1;
    apply_wrap();
    top = clampi(top, 0, nlines() - 1);
}

/* one row at screen row `row`: selection, then the text in runs of one colour */
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
        fetch(l, &a);
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
            gfx_text_n(win, run_x, y, lbuf + run, pos - run, run_sel ? tsel : 0x000000, font);
            run = -1;
        }
        if (pos >= n) break;
        if (sx >= vw) { x += row_width(lbuf + pos, n - pos); break; }     /* (the rest: for the scroll range) */
        if (visible && run < 0) { run = pos; run_x = sx; run_sel = sel; }
        x += w;
        pos = p2;
    }
    if (x > hs_max) hs_max = x;
}
static void paint_caret(void) {
    if (!active || !caret_on) return;
    int l = caret_row();
    if (l < top || l > top + vis_lines()) return;
    gfx_fill(win, X0 + x_in_line(l, cur) - left, Y0 + (l - top) * lh, 2, lh, 0x000000);
}

/* Ln and Col of the line as in the file (wrapped and continued rows count as one) */
static void line_col(int *ln, int *col) {
    int l = caret_row(), h = logical_start(l);
    *ln = logical_line(l) + 1;
    int c = 1;
    for (int i = h; i <= l; i++) {
        int a, len = fetch(i, &a), pos = 0;
        while (pos < len && a + pos < cur) { utf8_get(lbuf, len, &pos); c++; }
    }
    *col = c;
}

static void update_menus(void) {
    int st = undo_can_undo(&U) | undo_can_redo(&U) << 1 | (cur != anchor) << 2 | wrap << 3 | status_on << 4;
    if (st == menu_state_shown) return;
    int ch = st ^ (menu_state_shown < 0 ? ~0 : menu_state_shown);
    menu_state_shown = st;
    if (ch & 1) menu_enable(win, NP_UNDO, st & 1);
    if (ch & 2) menu_enable(win, NP_REDO, (st >> 1) & 1);
    if (ch & 4) { menu_enable(win, NP_CUT, cur != anchor); menu_enable(win, NP_COPY, cur != anchor); menu_enable(win, NP_DEL, cur != anchor); }
    if (ch & 8) menu_check(win, NP_WRAP, wrap);
    if (ch & 16) menu_check(win, NP_STATUSBAR, status_on);
}

static void update_bars(void) {
    int vl = vis_lines(), total = nlines() + vl - 1;
    if (total != sb_total || vl != sb_page || top != sb_pos) {
        sb_total = total; sb_page = vl; sb_pos = top;
        ctl_scroll_set(win, C_VSCROLL, total, vl, top, 1);
    }
    if (!wrap) {
        int tw = text_w() - X0, ht = (hs_max + cwm * 4 > left + tw ? hs_max + cwm * 4 : left + tw);
        if (ht != hb_total || tw != hb_page || left != hb_pos) {
            hb_total = ht; hb_page = tw; hb_pos = left;
            ctl_scroll_set(win, C_HSCROLL, ht, tw, left, cwm * 2);
        }
    }
    if (status_on) {
        int ln, col;
        line_col(&ln, &col);
        char st[160];
        snprintf(st, sizeof st, "%d bytes, %d lines%s\tLn %d, Col %d", tlen(), nlines() - D.nsoft, crlf ? ", CR/LF" : "", ln, col);
        if (strcmp(st, shown_status)) { strlcpy(shown_status, st, sizeof shown_status); ctl_set_text(win, C_STATUS, st); }
    }
    update_menus();
}

static void paint(void) {
    gfx_theme(&th);
    sync_view();
    int tw = text_w(), tht = text_h(), vl = vis_lines() + 1;
    gfx_clip_reset(win);
    gfx_fill(win, 0, 0, tw, tht, 0xFFFFFF);
    gfx_rect(win, 0, 0, tw, tht, 0x7F9DB9);
    if (!wrap) gfx_fill(win, tw, tht, SB_W, SB_W, th.face);         /* the corner between the scroll bars */
    gfx_clip(win, 2, 2, tw - 4, tht - 4);
    for (int i = 0; i < vl && top + i < nlines(); i++) paint_line(top + i, i);
    paint_caret();
    gfx_clip_reset(win);
    update_bars();
    win_invalidate(win);
    dirty = 0;
}
/* the caret's row only (blinking) */
static void paint_caret_line(void) {
    int l = caret_row(), tw = text_w(), tht = text_h();
    if (l < top || l > top + vis_lines()) return;
    gfx_clip(win, 2, 2, tw - 4, tht - 4);
    gfx_fill(win, 2, Y0 + (l - top) * lh, tw - 4, lh, 0xFFFFFF);
    paint_line(l, l - top);
    paint_caret();
    gfx_clip_reset(win);
    win_invalidate(win);
}

/* ---------------- settings ---------------- */
static void load_settings(void) {
    wrap = setting_get_int("notepad.wrap", 0) != 0;
    status_on = setting_get_int("notepad.statusbar", 1) != 0;
    char f[32];
    if (setting_get("notepad.font", f, sizeof f) > 0)
        for (int i = 0; i < N(fonts); i++) if (!strcmp(f, fonts[i].key)) font = fonts[i].font;
}
static void save_font(void) {
    for (int i = 0; i < N(fonts); i++) if (fonts[i].font == font) setting_set("notepad.font", fonts[i].key);
}

/* ---------------- files ---------------- */
static void error_box(const char *what, const char *p, int r) {
    char msg[PATHN + 120];
    if (r == E_TOOBIG) snprintf(msg, sizeof msg, "The file %s is too large for Notepad.\n\nNotepad opens files of up to %d MB.", base_name(p), MAX_TEXT >> 20);
    else snprintf(msg, sizeof msg, "Cannot %s %s.\n\n%s", what, p, hal_strerror(r));
    msgbox(win, "Notepad", msg, MBX_ICON_ERROR, MBX_OK);
}

static void new_text(void) {
    doc_reset();
    undo_clear(&U);
    cur = anchor = top = left = pref_x = eol = 0;
    hs_max = 0;
    modified = 0;
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
    new_text();
    int r = doc_replace(0, 0, data, o);
    free(data);
    if (r < 0) { new_text(); path[0] = 0; update_title(); dirty = 1; return r; }
    crlf = cr || got == 0;
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
        r = put_text(fd, D.tb, D.gs, out, &o);
        if (!r) r = put_text(fd, D.tb + D.ge, D.tcap - D.ge, out, &o);
        if (!r && o) { long w = write(fd, out, (size_t)o); if (w != o) r = w < 0 ? w : E_NOSPC; }
        close(fd);
    }
    free(out);
    set_cursor(prev);
    if (r < 0) { error_box("save", p, (int)r); return (int)r; }
    strlcpy(path, p, sizeof path);
    undo_mark_saved(&U);
    update_modified();
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

/* ---------------- clipboard ---------------- */
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
    if (o) insert(UNDO_OTHER, b, o);
    free(b);
}

/* ---------------- find, replace, go to ---------------- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* the next match of find_text at or after start (wrapping around), or -1 */
static int find_from(int start, int around) {
    int fl = (int)strlen(find_text), len = tlen();
    if (!fl || fl > len) return -1;
    const char *t = text_span(0, len);
    int f0 = match_case ? (unsigned char)find_text[0] : lower((unsigned char)find_text[0]);
    for (int k = 0; k < len; k++) {
        int i = start + k;
        if (i >= len) { if (!around) return -1; i -= len; }
        if (i + fl > len) continue;
        int c = (unsigned char)t[i];
        if ((match_case ? c : lower(c)) != f0) continue;
        int j = 1;
        if (match_case) while (j < fl && t[i + j] == find_text[j]) j++;
        else while (j < fl && lower((unsigned char)t[i + j]) == lower((unsigned char)find_text[j])) j++;
        if (j == fl) return i;
    }
    return -1;
}
static void select_range(int a, int b) {
    anchor = a; cur = b; eol = 0;
    undo_break(&U);
    pref_x = caret_x();
    scroll_to_caret();
    dirty = 1;
}
static int find_next(int quiet) {
    int i = find_from(sel_hi(), 1);
    if (i < 0) {
        if (!quiet) {
            char msg[180];
            snprintf(msg, sizeof msg, "Cannot find \"%s\"", find_text);
            msgbox(win, "Notepad", msg, MBX_ICON_INFO, MBX_OK);
        }
        return 0;
    }
    select_range(i, i + (int)strlen(find_text));
    return 1;
}
/* the selection is a match */
static int sel_matches(void) {
    int fl = (int)strlen(find_text), a = sel_lo();
    if (!fl || sel_hi() - a != fl) return 0;
    const char *t = text_span(a, a + fl);
    for (int j = 0; j < fl; j++)
        if (match_case ? t[j] != find_text[j] : lower((unsigned char)t[j]) != lower((unsigned char)find_text[j])) return 0;
    return 1;
}
/* every match between the first and the last replaced in one edit (one undo step) */
static int replace_all(void) {
    int fl = (int)strlen(find_text), rl = (int)strlen(repl_text), len = tlen();
    int first = find_from(0, 0);
    if (first < 0) return 0;
    long cap = 4096;
    char *out = malloc((size_t)cap);
    if (!out) return 0;
    int o = 0, pos = first, count = 0, last = first;
    for (int i = first; i >= 0; i = find_from(pos, 0)) {
        const char *t = text_span(0, len);
        long need = (long)o + (i - pos) + rl + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char *n = realloc(out, (size_t)cap);
            if (!n) { free(out); msgbox(win, "Notepad", "Not enough memory to replace the text.", MBX_ICON_ERROR, MBX_OK); return 0; }
            out = n;
        }
        memcpy(out + o, t + pos, (size_t)(i - pos)); o += i - pos;
        memcpy(out + o, repl_text, (size_t)rl); o += rl;
        pos = i + fl;
        last = pos;
        count++;
        if (pos >= len) break;
    }
    anchor = cur = first;
    edit(UNDO_OTHER, first, last, out, o);        /* (the caret after the last replacement) */
    free(out);
    undo_break(&U);
    scroll_to_caret();
    return count;
}

static void main_window_event(const hal_event_t *ev);

/* the Find and Replace dialogs (modal) */
static void find_dialog(int replace) {
    enum { D_FIND = 10, D_REPL, D_CASE, D_NEXT, D_ONE, D_ALL, D_CANCEL };
    int h = replace ? 132 : 92;
    int d = win_create(replace ? "Replace" : "Find", 380, h, WIN_DIALOG);
    if (d < 0) return;
    win_set_owner(d, win, 1);
    if (sel_hi() > sel_lo() && sel_hi() - sel_lo() < (int)sizeof find_text) {      /* the selection as the text to find */
        int a = sel_lo(), n = sel_hi() - a, nl = 0;
        const char *t = text_span(a, a + n);
        for (int i = 0; i < n; i++) if (t[i] == '\n') nl = 1;
        if (!nl) { memcpy(find_text, t, (size_t)n); find_text[n] = 0; }
    }
    ctl_create(d, CTL_LABEL, 10, 14, 80, 16, "Find what:", 1, 0);
    ctl_create(d, CTL_TEXTBOX, 92, 10, 186, 22, find_text, D_FIND, 0);
    int y = 42;
    if (replace) {
        ctl_create(d, CTL_LABEL, 10, 46, 80, 16, "Replace with:", 2, 0);
        ctl_create(d, CTL_TEXTBOX, 92, 42, 186, 22, repl_text, D_REPL, 0);
        y = 74;
    }
    ctl_create(d, CTL_CHECKBOX, 10, h - 28, 120, 20, "Match case", D_CASE, 0);
    ctl_set_value(d, D_CASE, match_case);
    ctl_create(d, CTL_BUTTON, 290, 10, 80, 23, "Find Next", D_NEXT, CTL_DEFAULT);
    if (replace) {
        ctl_create(d, CTL_BUTTON, 290, 38, 80, 23, "Replace", D_ONE, 0);
        ctl_create(d, CTL_BUTTON, 290, 66, 80, 23, "Replace All", D_ALL, 0);
    }
    ctl_create(d, CTL_BUTTON, 290, replace ? 94 : 38, 80, 23, "Cancel", D_CANCEL, CTL_CANCEL);
    (void)y;
    ctl_focus(d, D_FIND);
    for (;;) {
        hal_event_t ev;
        if (dirty) paint();
        if (!win_get_event(&ev, -1)) continue;
        if (ev.win != d) { main_window_event(&ev); continue; }
        if (ev.type == EVT_CLOSE) break;
        if (ev.type != EVT_COMMAND) continue;
        if (ev.id == D_CANCEL) break;
        ctl_get_text(d, D_FIND, find_text, sizeof find_text);
        if (replace) ctl_get_text(d, D_REPL, repl_text, sizeof repl_text);
        match_case = ctl_get_value(d, D_CASE);
        if (!find_text[0] || ev.id == D_CASE) continue;
        if (ev.id == D_NEXT) find_next(0);
        else if (ev.id == D_ONE) {
            if (sel_matches()) {
                int a = sel_lo();
                edit(UNDO_OTHER, a, sel_hi(), repl_text, (int)strlen(repl_text));
                undo_break(&U);
                scroll_to_caret();
            }
            find_next(0);
        } else if (ev.id == D_ALL) {
            int n = replace_all();
            char msg[64];
            snprintf(msg, sizeof msg, n ? "Replaced %d occurrence%s." : "Cannot find the text.", n, n == 1 ? "" : "s");
            dirty = 1;
            paint();
            msgbox(d, "Replace", msg, MBX_ICON_INFO, MBX_OK);
        }
    }
    win_destroy(d);
    dirty = 1;
}

static void go_to(void) {
    int ln0, col;
    line_col(&ln0, &col);
    char b[24];
    snprintf(b, sizeof b, "%d", ln0);
    if (!inputbox(win, "Go To Line", "Line number:", b, sizeof b)) return;
    int want = atoi(b), lines = nlines() - D.nsoft;
    if (want < 1 || want > lines) {
        msgbox(win, "Notepad - Goto Line", "The line number is beyond the total number of lines.", MBX_ICON_INFO, MBX_OK);
        return;
    }
    /* the offset after want - 1 newlines */
    int len = tlen(), off = 0, k = want - 1;
    const char *t = text_span(0, len);
    for (int i = 0; i < len && k; i++) if (t[i] == '\n') { k--; off = i + 1; }
    select_range(off, off);
}

/* ---------------- the Font dialog ---------------- */
static void font_dialog(void) {
    enum { F_LIST = 10, F_SAMPLE_LABEL };
    int d = win_create("Font", 380, 236, WIN_DIALOG);
    if (d < 0) return;
    win_set_owner(d, win, 1);
    const char *names[N(fonts)];
    int sel = 0;
    for (int i = 0; i < N(fonts); i++) { names[i] = fonts[i].label; if (fonts[i].font == font) sel = i; }
    ctl_create(d, CTL_LABEL, 12, 10, 200, 16, "Font:", 1, 0);
    ctl_create(d, CTL_LISTBOX, 12, 28, 262, 110, NULL, F_LIST, CTL_NOTIFY);
    ctl_list_set(d, F_LIST, names, N(fonts));
    ctl_set_value(d, F_LIST, sel);
    ctl_create(d, CTL_LABEL, 12, 146, 200, 16, "Sample", F_SAMPLE_LABEL, 0);
    ctl_create(d, CTL_BUTTON, 288, 28, 80, 23, "OK", IDR_OK, CTL_DEFAULT);
    ctl_create(d, CTL_BUTTON, 288, 56, 80, 23, "Cancel", IDR_CANCEL, CTL_CANCEL);
    ctl_focus(d, F_LIST);
    int ok = -1, shown = -1;
    while (ok < 0) {
        int s = clampi(ctl_get_value(d, F_LIST), 0, N(fonts) - 1);
        if (s != shown) {
            hal_theme_t t;
            gfx_theme(&t);
            gfx_fill(d, 0, 0, 380, 236, t.face);
            gfx_shade(d, 12, 164, 262, 60, 0xFFFFFF, 0, SHADE_SUNKEN, 0);
            gfx_clip(d, 14, 166, 258, 56);
            const char *sample = "AaBbYyZz 0123";
            int f = fonts[s].font, tw = text_width(sample, f);
            gfx_text(d, 12 + (262 - tw) / 2, 164 + (60 - text_height(f)) / 2, sample, 0x000000, f);
            gfx_clip_reset(d);
            win_invalidate(d);
            shown = s;
        }
        hal_event_t ev;
        if (dirty) paint();
        if (!win_get_event(&ev, -1)) continue;
        if (ev.win != d) { main_window_event(&ev); continue; }
        if (ev.type == EVT_CLOSE) ok = 0;
        else if (ev.type == EVT_PAINT) shown = -1;
        else if (ev.type == EVT_COMMAND && (ev.id == IDR_OK || (ev.id == F_LIST && ev.button == CMD_ACTIVATE))) ok = 1;
        else if (ev.type == EVT_COMMAND && ev.id == IDR_CANCEL) ok = 0;
    }
    int s = clampi(ctl_get_value(d, F_LIST), 0, N(fonts) - 1);
    win_destroy(d);
    if (ok && fonts[s].font != font) {
        font = fonts[s].font;
        set_metrics();
        save_font();
        doc_set_wrap(wrap ? wrap_width() : 0, ls(top));   /* rows break by width: build them again */
        if (!wrap) D.npend = 0;                            /* (without wrap they break by bytes: unchanged) */
        layout();
        sync_view();
        scroll_to_caret();
    }
    dirty = 1;
}

/* ---------------- commands ---------------- */
static void quit(void) {
    win_destroy(win);
    exit(0);
}

static void set_wrap(int on) {
    wrap = on;
    left = 0;
    setting_set_int("notepad.wrap", wrap);
    layout();
    scroll_to_caret();
}

static void command(int id) {
    switch (id) {
    case NP_NEW:
        if (!confirm_discard()) return;
        new_text();
        path[0] = 0; crlf = 1;
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
    case NP_UNDO: do_undo(0); break;
    case NP_REDO: do_undo(1); break;
    case NP_CUT: if (copy_sel()) del_sel(); break;
    case NP_COPY: copy_sel(); break;
    case NP_PASTE: paste(); break;
    case NP_DEL: if (!del_sel() && cur < tlen()) edit(UNDO_OTHER, cur, next_ch(cur), "", 0); break;
    case NP_SELALL: anchor = 0; cur = tlen(); eol = 0; undo_break(&U); break;
    case NP_FIND: find_dialog(0); return;
    case NP_FINDNEXT: if (find_text[0]) find_next(0); else find_dialog(0); return;
    case NP_REPLACE: find_dialog(1); return;
    case NP_GOTO: go_to(); return;
    case NP_TIME: {
        hal_datetime_t d;
        if (datetime(&d) < 0) return;
        char s[48];
        snprintf(s, sizeof s, "%d:%02d %s %d/%d/%d", d.hour % 12 ? d.hour % 12 : 12, d.minute, d.hour < 12 ? "AM" : "PM",
                 d.month, d.day, d.year);
        insert(UNDO_OTHER, s, (int)strlen(s));
        undo_break(&U);
        break;
    }
    case NP_WRAP: set_wrap(!wrap); break;
    case NP_FONT: font_dialog(); return;
    case NP_STATUSBAR:
        status_on = !status_on;
        setting_set_int("notepad.statusbar", status_on);
        shown_status[0] = 0;
        layout();
        break;
    case NP_HELP: help_open("notepad"); return;
    case NP_ABOUT:
        msgbox(win, "About Notepad", "Halcyon Notepad\nA simple text editor.\n\nFiles are saved with Windows-style line endings.",
               MBX_ICON_INFO, MBX_OK);
        return;
    default: return;
    }
    pref_x = caret_x();
    scroll_to_caret();
    update_title();
    dirty = 1;
}

/* ---------------- input ---------------- */
static int in_text(int x, int y) { return x >= 0 && y >= 0 && x < text_w() && y < text_h(); }

static int row_at(int y) {
    int dy = y - Y0;
    return clampi(top + (dy >= 0 ? dy / lh : -1 - (-dy - 1) / lh), 0, nlines() - 1);
}

static void on_mouse(const hal_event_t *ev) {
    switch (ev->type) {
    case EVT_MOUSE_DOWN:
    case EVT_DBLCLICK:
        if (!in_text(ev->x, ev->y)) return;
        if (ev->button == MOUSE_RIGHT) {
            ctx_menu[0].flags = undo_can_undo(&U) ? 0 : MENU_DISABLED;
            for (int i = 2; i < N(ctx_menu); i++)
                if (ctx_menu[i].id == NP_CUT || ctx_menu[i].id == NP_COPY || ctx_menu[i].id == NP_DEL)
                    ctx_menu[i].flags = cur != anchor ? 0 : MENU_DISABLED;
            menu_popup(win, ctx_menu, N(ctx_menu), ev->x, ev->y);
            return;
        }
        if (ev->button != MOUSE_LEFT) return;
        undo_break(&U);
        caret_to(row_at(ev->y), ev->x - X0 + left);
        if (ev->type == EVT_DBLCLICK) {
            int a = cur, b = cur, len = tlen();
            while (a > 0 && is_word(at(a - 1))) a--;
            while (b < len && is_word(at(b))) b++;
            anchor = a; cur = b; eol = 0;
        } else if (!(ev->mods & MOD_SHIFT)) {
            anchor = cur;
        }
        mouse_sel = 1;
        pref_x = caret_x();
        scroll_to_caret();
        dirty = 1;
        return;
    case EVT_MOUSE_MOVE:
        if (mouse_sel && (ev->buttons & MOUSE_LEFT)) {
            int c0 = cur, e0 = eol;
            caret_to(row_at(ev->y), ev->x - X0 + left);
            if (cur != c0 || eol != e0) { pref_x = caret_x(); scroll_to_caret(); dirty = 1; }
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
    int len = tlen(), l = caret_row(), vl = vis_lines();
    int moved = 1, keep_pref = 0, keep_eol = 0;
    if (ev->id == KEY_F1 && !(ev->mods & (MOD_CTRL | MOD_ALT))) { command(NP_HELP); return; }
    if ((ev->mods & MOD_ALT) && !(ev->mods & MOD_CTRL) && ev->id == KEY_BACKSPACE) { command(shift ? NP_REDO : NP_UNDO); return; }
    if (ctrl) {
        static const struct { char letter; int id; } keys[] = {
            { 'N', NP_NEW }, { 'O', NP_OPEN }, { 'S', NP_SAVE }, { 'F', NP_FIND }, { 'A', NP_SELALL },
            { 'C', NP_COPY }, { 'X', NP_CUT }, { 'V', NP_PASTE }, { 'Z', NP_UNDO }, { 'Y', NP_REDO },
            { 'H', NP_REPLACE }, { 'G', NP_GOTO },
        };
        if (shift && key_shortcut(ev, 'Z')) { command(NP_REDO); return; }
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
        case KEY_BACKSPACE: {          /* the word before */
            if (del_sel()) { moved = 0; break; }
            int a = cur;
            while (a > 0 && !is_word(at(a - 1))) a--;
            while (a > 0 && is_word(at(a - 1))) a--;
            if (a < cur) edit(UNDO_OTHER, a, cur, "", 0);
            moved = 0;
            break;
        }
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
        case KEY_UP: if (l > 0) caret_to(l - 1, pref_x); keep_pref = keep_eol = 1; break;
        case KEY_DOWN: if (l + 1 < nlines()) caret_to(l + 1, pref_x); keep_pref = keep_eol = 1; break;
        case KEY_PAGEUP:
            caret_to(l - vl > 0 ? l - vl : 0, pref_x);
            top = top - vl > 0 ? top - vl : 0;
            keep_pref = keep_eol = 1;
            break;
        case KEY_PAGEDOWN:
            caret_to(l + vl < nlines() - 1 ? l + vl : nlines() - 1, pref_x);
            top = clampi(top + vl, 0, nlines() - 1);
            keep_pref = keep_eol = 1;
            break;
        case KEY_HOME: {
            /* smart home: first non-blank, then column 0 (of the row on screen) */
            int s = ls(l), e = line_end(l), fnb = s;
            while (fnb < e && (at(fnb) == ' ' || at(fnb) == '\t')) fnb++;
            cur = cur == fnb ? s : fnb;
            break;
        }
        case KEY_END: cur = line_end(l); set_eol(l); keep_eol = 1; break;
        case KEY_BACKSPACE:
            if (!del_sel() && cur > 0) edit(UNDO_BACKSPACE, prev_ch(cur), cur, "", 0);
            moved = 0;
            break;
        case KEY_DELETE:
            if (shift) { command(NP_CUT); return; }
            if (!del_sel() && cur < len) edit(UNDO_DELETE, cur, next_ch(cur), "", 0);
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
            for (int i = ls(logical_start(l)); i < cur && ni < 63 && (at(i) == ' ' || at(i) == '\t'); i++) ind[ni++] = (char)at(i);
            insert(UNDO_TYPE, ind, ni);
            moved = 0;
            break;
        }
        case KEY_TAB: insert(UNDO_TYPE, "\t", 1); moved = 0; break;
        case KEY_F3: command(NP_FINDNEXT); return;
        case KEY_F5: command(NP_TIME); return;
        case KEY_ESC: return;
        default:
            if (text) { char u[4]; insert(UNDO_TYPE, u, utf8_put(ev->ch, u)); moved = 0; }
            else return;
        }
    }
    if (moved) undo_break(&U);
    if (moved && !shift) anchor = cur;
    if (!moved) anchor = cur;
    if (!keep_eol) eol = 0;
    if (!keep_pref) pref_x = caret_x();
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
        if (ev->id == C_HSCROLL && !wrap) { left = ev->x > 0 ? ev->x : 0; hb_pos = ev->x; dirty = 1; }
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
/* events of the main window while a dialog of Notepad is open: only drawing */
static void main_window_event(const hal_event_t *ev) {
    if (ev->type == EVT_PAINT || ev->type == EVT_RESIZE || ev->type == EVT_FOCUS) handle(ev);
}

int main(int argc, char **argv) {
    load_settings();
    set_metrics();
    if (doc_init()) return 1;
    undo_init(&U, UNDO_LIMIT);

    win = win_create("Untitled - Notepad", cw0, ch0, WIN_RESIZABLE);
    if (win < 0) return 1;
    win_set_icon(win, HICON_NOTEPAD);
    win_set_min_size(win, 200, 100);
    menu_set(win, bar, N(bar));
    ctl_create(win, CTL_VSCROLL, cw0 - SB_W, 0, SB_W, text_h(), 0, C_VSCROLL, 0);
    ctl_create(win, CTL_HSCROLL, 0, text_h(), text_w(), SB_W, 0, C_HSCROLL, 0);
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
        if (doc_stale_at() >= 0) {
            /* rows still to rewrap: a slice between events (the scroll bar follows) */
            if (win_get_event(&ev, 0)) { handle(&ev); continue; }
            rewrap_slice(REWRAP_SLICE);
            update_bars();
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
