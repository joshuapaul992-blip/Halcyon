/* HELP.EXE - "Help and Support": Halcyon's help viewer.
 *
 * Two panes: on the left the Contents (a tree of books and pages),
 * the Index (keywords, filtered as you type) and Search (full text, ranked);
 * on the right the page, with links, Back, Forward and Home. Pages are
 * HTML files in C:\HALCYON\HELP (made from help/ in the source tree by
 * tools/mkhelp.py, which also writes CONTENTS.TXT and INDEX.TXT there),
 * parsed and laid out by the Web Browser's engine (browser/html.c,
 * browser/layout.c) and drawn here.
 *
 *   HELP.EXE [page[#anchor]]       e.g. HELP.EXE notepad, HELP.EXE notepad#wrap
 *   HELP.EXE --test [page]         check every page and the panes, report on the
 *                                  log ("helptest: all N checks passed") and exit
 *
 * Programs open it with help_open("page") (userland/lib/shell.c); the
 * kernel's windows with shell_help() on F1. A link "run:NAME" starts
 * C:\PROGRAMS\NAME.EXE ("run:control" opens the Control Panel). See
 * docs/DESIGN-M8-NOTEPAD-HELP.md. */
#include "hal.h"
#include "web.h"
#include "browser/browser.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define HELP_DIR "C:/HALCYON/HELP"
#define HOME_PAGE "welcome.htm"
#define TOOL_H 34
#define TAB_H 26
#define SPL 5                            /* the splitter between the panes */
#define SB_W 17
#define ROW_H 19                         /* contents rows */
#define PAD 8                            /* page margin added to the layout's own */
#define HIST_MAX 64
#define MAX_TOC 256
#define MAX_IDX 2000
#define MAX_PAGES 128

enum { C_BACK = 100, C_FWD, C_HOME, C_TOCSCROLL, C_PAGESCROLL,
       C_IXLABEL, C_IXFILTER, C_IXLIST, C_IXSHOW,
       C_SQLABEL, C_SQ, C_SGO, C_SRLABEL, C_SRES, C_SSHOW };
enum { TAB_CONTENTS, TAB_INDEX, TAB_SEARCH };
enum { M_TOPIC = 1000 };

/* ---------------- state ---------------- */
static int win, cw = 800, ch = 560, lp = 250;   /* client size, width of the left pane */
static hal_canvas_t cv;
static hal_theme_t th;
static int tab = TAB_CONTENTS, dirty = 1, cursor_shape = -1, test_mode;

/* contents: books and pages in file order */
typedef struct { char title[96], page[64]; int depth, open, kids; } toc_t;
static toc_t toc[MAX_TOC];
static int ntoc, toc_top, toc_sel = -1, toc_hover = -1;

/* index: keyword -> page; keywords sorted, one list entry per keyword */
typedef struct { char *kw, *page, *title; } idx_t;
static idx_t idx[MAX_IDX];
static int nidx;
static const char *ix_items[MAX_IDX];
static int ix_first[MAX_IDX], nix;         /* the first idx entry of each shown keyword */

/* search: every page as lower-case plain text */
typedef struct { char page[64]; char *title, *text, *kw; int len; } pg_t;
static pg_t pages[MAX_PAGES];
static int npages, pages_loaded;
static const char *sr_items[MAX_PAGES];
static int sr_page[MAX_PAGES], nsr;

/* the page shown */
static doc_t *doc;
static char cur_page[96];                  /* "notepad.htm#wrap" */
static int scroll_y;
static node_t *hover_link;
typedef struct { char page[96]; int scroll; } hist_t;
static hist_t hist[HIST_MAX];
static int nhist, hpos = -1;
static int drag_split, drag_x0, drag_lp0;
static const char *popup_pages[16];

/* ---------------- small helpers ---------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int icase_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
static int starts_icase(const char *s, const char *p) {
    for (; *p; s++, p++) if (lower((unsigned char)*s) != lower((unsigned char)*p)) return 0;
    return 1;
}
static void set_cursor(int c) { if (c != cursor_shape) { cursor_shape = c; win_set_cursor(win, c); } }
static int is_wordc(unsigned char c) { return isalnum(c) || c >= 0x80; }

/* "notepad", "Notepad.HTM#Wrap" -> "notepad.htm#Wrap" (lower-case file name, .htm added) */
static void norm_ref(const char *ref, char *out, int n) {
    int k = 0;
    const char *hash = strchr(ref, '#');
    int plen = hash ? (int)(hash - ref) : (int)strlen(ref);
    int dot = 0;
    for (int i = 0; i < plen && k < n - 6; i++) {
        char c = ref[i];
        if (c == '/' || c == '\\' || c == ':') { k = 0; dot = 0; continue; }   /* (no folders) */
        if (c == '.') dot = 1;
        out[k++] = (char)lower((unsigned char)c);
    }
    out[k] = 0;
    if (!k) { strlcpy(out, HOME_PAGE, (size_t)n); k = (int)strlen(out); }
    else if (!dot) { strlcat(out, ".htm", (size_t)n); }
    if (hash && hash[1]) { strlcat(out, "#", (size_t)n); strlcat(out, hash + 1, (size_t)n); }
}
/* the file part of a reference */
static void ref_file(const char *ref, char *out, int n) {
    strlcpy(out, ref, (size_t)n);
    char *h = strchr(out, '#');
    if (h) *h = 0;
}

/* ---------------- layout ---------------- */
static int page_x(void) { return lp + SPL; }
static int page_w(void) { int w = cw - page_x() - SB_W; return w > 60 ? w : 60; }
static int view_h(void) { int h = ch - TOOL_H; return h > 20 ? h : 20; }
static int list_y(void) { return TOOL_H + TAB_H; }
static int toc_h(void) { return ch - list_y() - 4; }
static int toc_rows(void) { int r = (toc_h() - 4) / ROW_H; return r > 1 ? r : 1; }

/* metrics for the layout engine */
static int m_width(const char *s, int n, int font) { return text_width_n(s, n, font); }
static int m_height(int font) { return text_height(font); }
static int m_ascent(int font) { return text_ascent(font); }
static int m_fit(const char *s, int n, int font, int w) { return text_fit(s, n, font, w); }

static void layout_controls(void) {
    int x = 4;
    static const struct { int id, w; } tb[] = { { C_BACK, 70 }, { C_FWD, 84 }, { C_HOME, 70 } };
    for (int i = 0; i < N(tb); i++) { ctl_move(win, tb[i].id, x, 3, tb[i].w, TOOL_H - 6); x += tb[i].w + 2; }
    int y = list_y() + 8, w = lp - 16;
    ctl_move(win, C_TOCSCROLL, lp - SB_W - 3, list_y() + 2, SB_W, toc_h() - 2);
    /* index */
    ctl_move(win, C_IXLABEL, 8, y, w, 16);
    ctl_move(win, C_IXFILTER, 8, y + 18, w, 22);
    ctl_move(win, C_IXLIST, 8, y + 46, w, ch - (y + 46) - 40);
    ctl_move(win, C_IXSHOW, lp - 8 - 80, ch - 32, 80, 23);
    /* search */
    ctl_move(win, C_SQLABEL, 8, y, w, 16);
    ctl_move(win, C_SQ, 8, y + 18, w, 22);
    ctl_move(win, C_SGO, lp - 8 - 90, y + 46, 90, 23);
    ctl_move(win, C_SRLABEL, 8, y + 78, w, 16);
    ctl_move(win, C_SRES, 8, y + 96, w, ch - (y + 96) - 40);
    ctl_move(win, C_SSHOW, lp - 8 - 80, ch - 32, 80, 23);
    ctl_move(win, C_PAGESCROLL, cw - SB_W, TOOL_H, SB_W, view_h());
}
static void show_tab_controls(void) {
    static const int ix[] = { C_IXLABEL, C_IXFILTER, C_IXLIST, C_IXSHOW };
    static const int sr[] = { C_SQLABEL, C_SQ, C_SGO, C_SRLABEL, C_SRES, C_SSHOW };
    for (int i = 0; i < N(ix); i++) ctl_set_style(win, ix[i], tab == TAB_INDEX ? (ix[i] == C_IXFILTER ? CTL_NOTIFY | CTL_CHANGES : ix[i] == C_IXLIST ? CTL_NOTIFY : 0) : CTL_HIDDEN);
    for (int i = 0; i < N(sr); i++) ctl_set_style(win, sr[i], tab == TAB_SEARCH ? (sr[i] == C_SQ || sr[i] == C_SRES ? CTL_NOTIFY : sr[i] == C_SGO ? CTL_DEFAULT : 0) : CTL_HIDDEN);
    ctl_set_style(win, C_TOCSCROLL, tab == TAB_CONTENTS ? 0 : CTL_HIDDEN);
    if (tab == TAB_INDEX) ctl_focus(win, C_IXFILTER);
    else if (tab == TAB_SEARCH) ctl_focus(win, C_SQ);
    else ctl_focus(win, 0);
}

/* ---------------- loading the contents and the index ---------------- */
static char *next_line(char **p) {
    char *s = *p;
    if (!*s) return NULL;
    char *e = s;
    while (*e && *e != '\n') e++;
    *p = *e ? e + 1 : e;
    if (e > s && e[-1] == '\r') e[-1] = 0;
    *e = 0;
    return s;
}
/* CONTENTS.TXT: "  Title|page.htm", two spaces of indentation per level; a line
 * whose next line is deeper is a book */
static void load_contents(void) {
    char *data;
    long size;
    if (read_file(HELP_DIR "/CONTENTS.TXT", &data, &size) < 0) return;
    char *p = data, *l;
    while ((l = next_line(&p)) && ntoc < MAX_TOC) {
        int d = 0;
        while (l[d] == ' ') d++;
        if (!l[d] || l[d] == '#') continue;
        toc_t *t = &toc[ntoc];
        memset(t, 0, sizeof *t);
        t->depth = d / 2;
        char *bar = strchr(l + d, '|');
        if (bar) { *bar = 0; norm_ref(bar + 1, t->page, sizeof t->page); }
        strlcpy(t->title, l + d, sizeof t->title);
        if (ntoc && toc[ntoc - 1].depth < t->depth) toc[ntoc - 1].kids = 1;
        ntoc++;
    }
    free(data);
}
/* INDEX.TXT: "keyword<TAB>page.htm#anchor<TAB>page title", sorted by keyword */
static void load_index(void) {
    char *data;
    long size;
    if (read_file(HELP_DIR "/INDEX.TXT", &data, &size) < 0) return;
    char *p = data, *l;
    while ((l = next_line(&p)) && nidx < MAX_IDX) {
        char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        *t1 = 0; *t2 = 0;
        char ref[96];
        norm_ref(t1 + 1, ref, sizeof ref);
        idx[nidx].kw = strdup(l);
        idx[nidx].page = strdup(ref);
        idx[nidx].title = strdup(t2 + 1);
        if (idx[nidx].kw && idx[nidx].page && idx[nidx].title) nidx++;
    }
    free(data);
}
static void fill_index(void) {
    char f[64] = "";
    ctl_get_text(win, C_IXFILTER, f, sizeof f);
    nix = 0;
    for (int pass = 0; pass < 2 && !nix; pass++)       /* keywords starting with it, else containing it */
        for (int i = 0; i < nidx; i++) {
            if (i && !icase_cmp(idx[i].kw, idx[i - 1].kw)) continue;
            int ok = !f[0] || (pass == 0 ? starts_icase(idx[i].kw, f) : 0);
            if (pass == 1 && f[0]) for (const char *s = idx[i].kw; *s && !ok; s++) ok = starts_icase(s, f);
            if (!ok) continue;
            ix_items[nix] = idx[i].kw;
            ix_first[nix++] = i;
        }
    ctl_list_set(win, C_IXLIST, ix_items, nix);
    ctl_set_value(win, C_IXLIST, nix ? 0 : -1);
}

/* ---------------- search ---------------- */
/* the page as lower-case text without tags, its title and keywords */
static void page_text(const char *html, long n, pg_t *pg) {
    char *out = malloc((size_t)n + 1);
    if (!out) return;
    int o = 0, sp = 1;
    for (long i = 0; i < n; i++) {
        char c = html[i];
        if (c == '<') {
            /* <title>, <meta name="keywords" content="..."> */
            if (!pg->title && !strncmp(html + i, "<title>", 7)) {
                const char *e = strstr(html + i + 7, "</title>");
                if (e) { int l = (int)(e - (html + i + 7)); pg->title = malloc((size_t)l + 1); if (pg->title) { memcpy(pg->title, html + i + 7, (size_t)l); pg->title[l] = 0; } }
            }
            if (!pg->kw && !strncmp(html + i, "<meta name=\"keywords\" content=\"", 31)) {
                const char *s = html + i + 31, *e = strchr(s, '"');
                if (e) { int l = (int)(e - s); pg->kw = malloc((size_t)l + 1); if (pg->kw) { for (int k = 0; k < l; k++) pg->kw[k] = (char)lower((unsigned char)s[k]); pg->kw[l] = 0; } }
            }
            int skip = !strncmp(html + i, "<title>", 7) || !strncmp(html + i, "<style", 6);
            const char *close = skip ? (html[i + 1] == 't' ? "</title>" : "</style>") : ">";
            const char *e = strstr(html + i, close);
            i = e ? (long)(e - html) + (long)strlen(close) - 1 : n;
            if (!sp) { out[o++] = ' '; sp = 1; }
            continue;
        }
        if (c == '&') {
            static const struct { const char *e; char c; } ents[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&nbsp;", ' ' }, { "&quot;", '"' } };
            for (int k = 0; k < N(ents); k++) {
                int l = (int)strlen(ents[k].e);
                if (!strncmp(html + i, ents[k].e, (size_t)l)) { c = ents[k].c; i += l - 1; break; }
            }
        }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') { if (sp) continue; sp = 1; } else sp = 0;
        out[o++] = (char)lower((unsigned char)c);
    }
    out[o] = 0;
    pg->text = out;
    pg->len = o;
}
static void load_pages(void) {
    if (pages_loaded) return;
    pages_loaded = 1;
    static hal_dirent_t ents[MAX_PAGES + 16];
    int n = readdir(HELP_DIR, ents, N(ents));
    for (int i = 0; i < n && npages < MAX_PAGES; i++) {
        const char *dot = strrchr(ents[i].name, '.');
        if (ents[i].is_dir || !dot || icase_cmp(dot, ".htm")) continue;
        char path[160], *data;
        long size;
        snprintf(path, sizeof path, "%s/%s", HELP_DIR, ents[i].name);
        if (read_file(path, &data, &size) < 0) continue;
        pg_t *pg = &pages[npages];
        memset(pg, 0, sizeof *pg);
        norm_ref(ents[i].name, pg->page, sizeof pg->page);
        page_text(data, size, pg);
        free(data);
        if (pg->text && pg->title) npages++;
    }
}
/* occurrences of w at the start of a word in s */
static int count_word(const char *s, const char *w) {
    int n = 0, wl = (int)strlen(w);
    for (const char *p = strstr(s, w); p; p = strstr(p + 1, w))
        if (p == s || !is_wordc((unsigned char)p[-1])) n++;
    (void)wl;
    return n;
}
static int rank[MAX_PAGES];
static void do_search(void) {
    char q[128];
    ctl_get_text(win, C_SQ, q, sizeof q);
    load_pages();
    char words[8][32];
    int nw = 0;
    for (const char *s = q; *s && nw < 8;) {
        while (*s && !is_wordc((unsigned char)*s)) s++;
        int k = 0;
        while (*s && is_wordc((unsigned char)*s)) { if (k < 31) words[nw][k++] = (char)lower((unsigned char)*s); s++; }
        if (k) { words[nw][k] = 0; nw++; }
    }
    char phrase[128];
    int pl = 0;
    for (int i = 0; i < nw; i++) { if (i) phrase[pl++] = ' '; int l = (int)strlen(words[i]); memcpy(phrase + pl, words[i], (size_t)l); pl += l; }
    phrase[pl] = 0;
    nsr = 0;
    for (int p = 0; p < npages && nw; p++) {
        int score = 0, all = 1;
        char tl[160];
        int k = 0;
        for (const char *t = pages[p].title; *t && k < 159; t++) tl[k++] = (char)lower((unsigned char)*t);
        tl[k] = 0;
        for (int i = 0; i < nw && all; i++) {
            int c = count_word(pages[p].text, words[i]), in_t = count_word(tl, words[i]) > 0;
            int in_k = pages[p].kw && count_word(pages[p].kw, words[i]) > 0;
            if (!c && !in_t && !in_k) all = 0;
            score += (c > 20 ? 20 : c) + 15 * in_t + 8 * in_k;
        }
        if (!all) continue;
        if (nw > 1) score += 10 * count_word(pages[p].text, phrase) + 30 * (count_word(tl, phrase) > 0);
        rank[nsr] = score;
        sr_page[nsr++] = p;
    }
    /* by score, then title */
    for (int i = 1; i < nsr; i++)
        for (int j = i; j > 0 && (rank[j] > rank[j - 1] || (rank[j] == rank[j - 1] && icase_cmp(pages[sr_page[j]].title, pages[sr_page[j - 1]].title) < 0)); j--) {
            int t = rank[j]; rank[j] = rank[j - 1]; rank[j - 1] = t;
            t = sr_page[j]; sr_page[j] = sr_page[j - 1]; sr_page[j - 1] = t;
        }
    for (int i = 0; i < nsr; i++) sr_items[i] = pages[sr_page[i]].title;
    ctl_list_set(win, C_SRES, sr_items, nsr);
    ctl_set_value(win, C_SRES, nsr ? 0 : -1);
    char lab[64];
    if (!nw) strlcpy(lab, "Type the words to search for.", sizeof lab);
    else snprintf(lab, sizeof lab, nsr == 1 ? "1 topic found:" : "%d topics found:", nsr);
    ctl_set_text(win, C_SRLABEL, lab);
}

/* ---------------- pages ---------------- */
static void sync_contents(void);
static void update_buttons(void) {
    ctl_set_style(win, C_BACK, hpos > 0 ? 0 : CTL_DISABLED);
    ctl_set_style(win, C_FWD, hpos + 1 < nhist ? 0 : CTL_DISABLED);
}
static void update_title(void) {
    char t[160];
    if (doc && doc->title && doc->title[0]) snprintf(t, sizeof t, strstr(doc->title, "Help and Support") ? "%s" : "%s - Help and Support", doc->title);
    else strlcpy(t, "Help and Support", sizeof t);
    win_set_title(win, t);
}
static const char missing_page[] =
    "<html><head><title>Page not found</title></head><body>"
    "<h2 style=\"color:#1f4e9a\">This page cannot be found</h2>"
    "<p>The help page <b>%s</b> is not in C:\\HALCYON\\HELP.</p>"
    "<p>Go to the <a href=\"welcome.htm\">Help and Support home page</a>, or look for the topic on the "
    "Contents, Index or Search tab on the left.</p></body></html>";

static void relayout(void) {
    if (!doc) return;
    int old = doc->height;
    layout_doc(doc, page_w() - PAD);
    if (old) scroll_y = (int)((long)scroll_y * doc->height / old);
    dirty = 1;
}
/* show a page: hmode 0 new history entry, 1 moving in the history (keep it) */
static int show_page(const char *ref_in, int hmode) {
    char ref[96], file[96], path[160];
    norm_ref(ref_in, ref, sizeof ref);
    ref_file(ref, file, sizeof file);
    /* the same page: only the anchor changes */
    char curfile[96];
    ref_file(cur_page, curfile, sizeof curfile);
    if (!doc || strcmp(file, curfile)) {
        snprintf(path, sizeof path, "%s/%s", HELP_DIR, file);
        char *data;
        long size;
        doc_t *d;
        if (read_file(path, &data, &size) == 0) {
            d = html_parse(data, (size_t)size, "utf-8", path);
            free(data);
        } else {
            char html[sizeof missing_page + 96];
            snprintf(html, sizeof html, missing_page, file);
            d = html_parse(html, strlen(html), "utf-8", path);
        }
        if (!d) return -1;
        doc_free(doc);
        doc = d;
        hover_link = NULL;
        layout_doc(doc, page_w() - PAD);
    }
    if (hmode == 0) {
        if (hpos >= 0) hist[hpos].scroll = scroll_y;
        if (hpos + 1 >= HIST_MAX) { memmove(hist, hist + 1, sizeof hist[0] * (HIST_MAX - 1)); hpos--; }
        hpos++;
        nhist = hpos + 1;
        strlcpy(hist[hpos].page, ref, sizeof hist[hpos].page);
        hist[hpos].scroll = 0;
    }
    strlcpy(cur_page, ref, sizeof cur_page);
    scroll_y = 0;
    const char *hash = strchr(ref, '#');
    if (hash && hash[1]) { int y = anchor_y(doc, hash + 1); if (y >= 0) scroll_y = y > 4 ? y - 4 : 0; }
    update_buttons();
    update_title();
    sync_contents();
    dirty = 1;
    return 0;
}
static void go_hist(int to) {
    if (to < 0 || to >= nhist) return;
    hist[hpos].scroll = scroll_y;
    hpos = to;
    show_page(hist[hpos].page, 1);
    scroll_y = hist[hpos].scroll;
    dirty = 1;
}
static void follow(node_t *l) {
    const char *h = node_attr(l, "href");
    if (!h || !h[0]) return;
    if (!strncmp(h, "run:", 4)) {
        const char *prog = h + 4;
        if (!icase_cmp(prog, "control")) { shell_app(SHELL_APP_CONTROL, NULL); return; }
        char path[96];
        int k = 0;
        for (const char *p = prog; *p && k < 40; p++) if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') path[k++] = *p;
        path[k] = 0;
        char full[128];
        snprintf(full, sizeof full, "C:/PROGRAMS/%s.EXE", path);
        if (spawn(full, NULL) < 0) msgbox(win, "Help and Support", "The program cannot be started.", MBX_ICON_ERROR, MBX_OK);
        return;
    }
    if (!strncmp(h, "http:", 5) || !strncmp(h, "https:", 6)) { spawn("C:/PROGRAMS/BROWSER.EXE", h); return; }
    if (h[0] == '#') {
        char ref[96], file[96];
        ref_file(cur_page, file, sizeof file);
        snprintf(ref, sizeof ref, "%s%s", file, h);
        show_page(ref, 0);
        return;
    }
    show_page(h, 0);
}

/* ---------------- drawing the page ---------------- */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static void cfill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1 || !cv.pixels) return;
    for (int j = y0; j < y1; j++) {
        unsigned *row = cv.pixels + (size_t)j * (size_t)cv.stride;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}
static void cframe(int x, int y, int w, int h, uint32_t c) {
    cfill(x, y, w, 1, c); cfill(x, y + h - 1, w, 1, c); cfill(x, y, 1, h, c); cfill(x + w - 1, y, 1, h, c);
}
static void cdisc(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
        cfill(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}
static void ctext(int x, int y, const char *s, int n, uint32_t c, int font) {
    while (n > 0) {
        int k = n > 2000 ? 2000 : n;
        if (k < n) while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
        if (k <= 0) k = n > 2000 ? 2000 : n;
        x = gfx_text_n(win, x, y, s, k, c, font);
        s += k;
        n -= k;
    }
}
static void paint_page(void) {
    int px = page_x(), vw = page_w(), vh = view_h();
    clip_x0 = px; clip_y0 = TOOL_H; clip_x1 = px + vw; clip_y1 = TOOL_H + vh;
    if (clip_x1 > cv.w) clip_x1 = cv.w;
    if (clip_y1 > cv.h) clip_y1 = cv.h;
    uint32_t bg = doc && doc->has_bg ? doc->bg : 0xFFFFFF;
    cfill(px, TOOL_H, vw, vh, bg);
    if (!doc) return;
    int maxs = doc->height - vh > 0 ? doc->height - vh : 0;
    scroll_y = clampi(scroll_y, 0, maxs);
    gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
    int ox = px + PAD / 2, oy = TOOL_H - scroll_y;
    for (int i = 0; i < doc->nitems; i++) {
        const item_t *it = &doc->items[i];
        if (it->y > scroll_y + vh + 4 || it->y + it->h < scroll_y - 40) continue;
        int x = it->x + ox, y = it->y + oy;
        switch (it->kind) {
        case IT_TEXT: {
            uint32_t c = it->color;
            if (it->link && it->link == hover_link) c = 0xD0451B;
            ctext(x, y, it->text, it->len, c, it->font);
            if ((it->flags & IF_UNDERLINE) || (it->link && it->link == hover_link)) cfill(x, y + text_ascent(it->font) + 1, it->w, 1, c);
            break;
        }
        case IT_RECT: cfill(x, y, it->w, it->h, it->color); break;
        case IT_FRAME:
            if (it->flags == 1) cframe(x, y, it->w, it->h, 0xA0A0A0);
            else if (!it->node) cframe(x, y, it->w, it->h, 0xB8C7DE);
            break;
        case IT_HR: cfill(x, y, it->w, 1, 0xB8C7DE); break;
        case IT_ALT: case IT_IMAGE: cframe(x, y, it->w, it->h, 0xC0C0C0); break;
        case IT_BULLET: {
            int f = it->font, a = text_ascent(f);
            if (it->len >= 0) {
                char num[16];
                int nl = snprintf(num, sizeof num, "%d.", it->len);
                ctext(x + 14 - text_width(num, f), y, num, nl, it->color, f);
            } else {
                int r = a > 12 ? 4 : 3;
                if (it->flags & IF_SQUARE) cfill(x + 10 - r, y + a - a / 3 - r, 2 * r, 2 * r, it->color);
                else cdisc(x + 10, y + a - a / 3, r - (it->flags & IF_CIRCLE ? 1 : 0), it->color);
            }
            break;
        }
        }
    }
    gfx_clip_reset(win);
    ctl_scroll_set(win, C_PAGESCROLL, doc->height > vh ? doc->height : vh, vh, scroll_y, 40);
}

/* ---------------- drawing the left pane ---------------- */
static int toc_visible(int i) {
    /* shown if every book above it is open */
    int d = toc[i].depth;
    for (int k = i - 1; k >= 0 && d > 0; k--)
        if (toc[k].depth < d) { if (!toc[k].open) return 0; d = toc[k].depth; }
    return 1;
}
static int vis_list[MAX_TOC], nvis;
static void build_vis(void) {
    nvis = 0;
    for (int i = 0; i < ntoc; i++) if (toc_visible(i)) vis_list[nvis++] = i;
}
static void paint_tabs(void) {
    static const char *names[3] = { "Contents", "Index", "Search" };
    int y = TOOL_H + 4, x = 6;
    gfx_fill(win, 0, TOOL_H, lp, ch - TOOL_H, th.face);
    gfx_fill(win, 4, list_y() - 1, lp - 8, 1, 0x919B9C);
    for (int i = 0; i < 3; i++) {
        int w = text_width(names[i], FONT_UI) + 18, sel = i == tab;
        int ty = sel ? y - 2 : y, h = list_y() - ty + (sel ? 1 : 0);
        gfx_rounded(win, x, ty, w, h, 3, 0xFFFFFF, sel ? 0xFCFCFE : 0xECEBE6);
        gfx_rounded_outline(win, x, ty, w, h + 3, 3, 0x919B9C);
        if (sel) { gfx_fill(win, x + 1, ty, w - 2, 2, 0xFFC83C); gfx_fill(win, x + 1, list_y() - 1, w - 2, 2, 0xFCFCFE); }
        gfx_text(win, x + 9, ty + (h - text_height(FONT_UI)) / 2 + 1, names[i], 0x000000, FONT_UI);
        x += w + 1;
    }
    /* the tab's page */
    gfx_fill(win, 4, list_y() + 1, lp - 8, ch - list_y() - 4, 0xFCFCFE);
    gfx_rect(win, 4, list_y() - 1, lp - 8, ch - list_y() - 2, 0x919B9C);
}
static int tab_hit(int x, int y) {
    static const char *names[3] = { "Contents", "Index", "Search" };
    if (y < TOOL_H + 2 || y >= list_y()) return -1;
    int xx = 6;
    for (int i = 0; i < 3; i++) {
        int w = text_width(names[i], FONT_UI) + 18;
        if (x >= xx && x < xx + w) return i;
        xx += w + 1;
    }
    return -1;
}
static void paint_toc(void) {
    build_vis();
    int rows = toc_rows(), x0 = 8, y0 = list_y() + 4, w = lp - SB_W - 14;
    toc_top = clampi(toc_top, 0, nvis - rows > 0 ? nvis - rows : 0);
    gfx_clip(win, 6, list_y() + 1, lp - 12, toc_h() - 2);
    gfx_fill(win, 6, list_y() + 1, lp - 12, toc_h() - 2, 0xFFFFFF);
    for (int r = 0; r < rows && toc_top + r < nvis; r++) {
        int i = vis_list[toc_top + r];
        toc_t *t = &toc[i];
        int x = x0 + t->depth * 16, y = y0 + r * ROW_H;
        int icon = t->kids ? (t->open ? HICON_FOLDER_OPEN : HICON_FOLDER) : HICON_HELP;
        gfx_icon(win, x, y + 1, icon | 1 << 16);
        int tw = text_width(t->title, FONT_UI);
        if (tw > w - (x - x0) - 22) tw = w - (x - x0) - 22;
        if (i == toc_sel) gfx_fill(win, x + 19, y, tw + 6, ROW_H - 1, th.select);
        unsigned c = i == toc_sel ? th.select_text : 0x000000;
        gfx_text_box(win, x + 22, y, w - (x - x0) - 22, ROW_H - 1, t->title, c, FONT_UI, 0);
        if (i == toc_hover && i != toc_sel) gfx_fill(win, x + 22, y + ROW_H - 4, tw, 1, 0x1F4E9A);
    }
    gfx_clip_reset(win);
    ctl_scroll_set(win, C_TOCSCROLL, nvis > rows ? nvis : rows, rows, toc_top, 1);
}

static void paint(void) {
    gfx_theme(&th);
    gfx_clip_reset(win);
    gfx_gradient(win, 0, 0, cw, TOOL_H, 0xFCFCFE, th.face);
    gfx_fill(win, 0, TOOL_H - 1, cw, 1, 0xA0A0A0);
    paint_tabs();
    if (tab == TAB_CONTENTS) paint_toc();
    /* the splitter */
    gfx_fill(win, lp, TOOL_H, SPL, ch - TOOL_H, th.face);
    gfx_fill(win, lp + SPL - 1, TOOL_H, 1, ch - TOOL_H, 0xA0A0A0);
    paint_page();
    win_invalidate(win);
    dirty = 0;
}

/* ---------------- the contents tree ---------------- */
static void sync_contents(void) {
    char file[96];
    ref_file(cur_page, file, sizeof file);
    int found = -1;
    for (int i = 0; i < ntoc && found < 0; i++) if (!strcmp(toc[i].page, file)) found = i;
    if (found < 0) return;
    toc_sel = found;
    /* open the books above it */
    int d = toc[found].depth;
    for (int k = found - 1; k >= 0 && d > 0; k--) if (toc[k].depth < d) { toc[k].open = 1; d = toc[k].depth; }
    build_vis();
    for (int r = 0; r < nvis; r++)
        if (vis_list[r] == found) {
            if (r < toc_top) toc_top = r;
            if (r >= toc_top + toc_rows()) toc_top = r - toc_rows() + 1;
        }
}
static int toc_at(int x, int y) {
    if (tab != TAB_CONTENTS || x < 6 || x >= lp - SB_W - 4 || y < list_y() + 4) return -1;
    int r = (y - list_y() - 4) / ROW_H;
    if (r >= toc_rows()) return -1;
    build_vis();
    return toc_top + r < nvis ? vis_list[toc_top + r] : -1;
}
static void toc_activate(int i, int toggle) {
    if (i < 0) return;
    toc_sel = i;
    if (toc[i].kids && toggle) toc[i].open = !toc[i].open;
    if (toc[i].page[0]) show_page(toc[i].page, 0);
    dirty = 1;
}
static void toc_key(int key) {
    build_vis();
    int r = 0;
    for (int k = 0; k < nvis; k++) if (vis_list[k] == toc_sel) r = k;
    switch (key) {
    case KEY_UP: r--; break;
    case KEY_DOWN: r++; break;
    case KEY_HOME: r = 0; break;
    case KEY_END: r = nvis - 1; break;
    case KEY_LEFT:
        if (toc_sel >= 0 && toc[toc_sel].kids && toc[toc_sel].open) { toc[toc_sel].open = 0; dirty = 1; return; }
        for (int k = toc_sel - 1; k >= 0 && toc_sel >= 0; k--) if (toc[k].depth < toc[toc_sel].depth) { toc_sel = k; break; }
        sync_contents(); dirty = 1;
        return;
    case KEY_RIGHT: if (toc_sel >= 0 && toc[toc_sel].kids) { toc[toc_sel].open = 1; dirty = 1; } return;
    case KEY_ENTER: case KEY_KP_ENTER: toc_activate(toc_sel, 1); return;
    default: return;
    }
    if (!nvis) return;
    r = clampi(r, 0, nvis - 1);
    toc_sel = vis_list[r];
    if (toc[toc_sel].page[0]) show_page(toc[toc_sel].page, 0);
    toc_sel = vis_list[r];
    if (r < toc_top) toc_top = r;
    if (r >= toc_top + toc_rows()) toc_top = r - toc_rows() + 1;
    dirty = 1;
}

/* ---------------- index ---------------- */
static void index_show(void) {
    int s = ctl_get_value(win, C_IXLIST);
    if (s < 0 || s >= nix) return;
    int first = ix_first[s], n = 0;
    while (first + n < nidx && !icase_cmp(idx[first + n].kw, idx[first].kw)) n++;
    if (n == 1) { show_page(idx[first].page, 0); return; }
    /* several topics: choose one */
    static hal_menu_item_t items[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        memset(&items[i], 0, sizeof items[i]);
        items[i].label = idx[first + i].title;
        items[i].id = M_TOPIC + i;
        popup_pages[i] = idx[first + i].page;
    }
    menu_popup(win, items, n, lp - 8 - 80, ch - 34);
}
static void search_show(void) {
    int s = ctl_get_value(win, C_SRES);
    if (s < 0 || s >= nsr) return;
    show_page(pages[sr_page[s]].page, 0);
}

/* ---------------- input ---------------- */
static item_t *hit(int x, int y) {
    if (!doc || x < page_x() || x >= page_x() + page_w() || y < TOOL_H) return NULL;
    int px = x - page_x() - PAD / 2, py = y - TOOL_H + scroll_y;
    for (int i = doc->nitems - 1; i >= 0; i--) {
        item_t *it = &doc->items[i];
        if (it->link && px >= it->x && px < it->x + it->w && py >= it->y && py < it->y + it->h) return it;
    }
    return NULL;
}
static void set_tab(int t) {
    if (t == tab) return;
    tab = t;
    if (tab == TAB_SEARCH) load_pages();
    show_tab_controls();
    dirty = 1;
}
static void scroll_by(int dy) { scroll_y += dy; dirty = 1; }

static void command(int id, int code) {
    switch (id) {
    case C_BACK: go_hist(hpos - 1); break;
    case C_FWD: go_hist(hpos + 1); break;
    case C_HOME: show_page(HOME_PAGE, 0); break;
    case C_IXFILTER: if (code == CMD_CHANGE) fill_index(); else if (code == CMD_ACTIVATE) index_show(); break;
    case C_IXLIST: if (code == CMD_ACTIVATE) index_show(); break;
    case C_IXSHOW: index_show(); break;
    case C_SQ: if (code == CMD_ACTIVATE) do_search(); break;
    case C_SGO: do_search(); break;
    case C_SRES: if (code == CMD_ACTIVATE) search_show(); break;
    case C_SSHOW: search_show(); break;
    default:
        if (id >= M_TOPIC && id < M_TOPIC + 16 && popup_pages[id - M_TOPIC]) show_page(popup_pages[id - M_TOPIC], 0);
    }
}

static void on_key(const hal_event_t *ev) {
    if (!ev->pressed) return;
    int alt = ev->mods & MOD_ALT, ctrl = ev->mods & MOD_CTRL;
    if (alt && ev->id == KEY_LEFT) { go_hist(hpos - 1); return; }
    if (alt && ev->id == KEY_RIGHT) { go_hist(hpos + 1); return; }
    if (alt && ev->id == KEY_HOME) { show_page(HOME_PAGE, 0); return; }
    if (alt && ev->vk == 'C') { set_tab(TAB_CONTENTS); return; }
    if (alt && ev->vk == 'N') { set_tab(TAB_INDEX); return; }
    if (alt && ev->vk == 'S') { set_tab(TAB_SEARCH); return; }
    if (ctrl && ev->id == KEY_TAB) { set_tab((tab + ((ev->mods & MOD_SHIFT) ? 2 : 1)) % 3); return; }
    if (ev->id == KEY_ESC) { exit(0); }
    if (tab == TAB_CONTENTS && (ev->id == KEY_UP || ev->id == KEY_DOWN || ev->id == KEY_LEFT || ev->id == KEY_RIGHT ||
                                ev->id == KEY_ENTER || ev->id == KEY_KP_ENTER || ev->id == KEY_HOME || ev->id == KEY_END)) {
        toc_key(ev->id);
        return;
    }
    int vh = view_h();
    switch (ev->id) {
    case KEY_PAGEUP: scroll_by(-(vh - 40)); break;
    case KEY_PAGEDOWN: case KEY_SPACE: scroll_by(vh - 40); break;
    case KEY_BACKSPACE: go_hist(hpos - 1); break;
    }
}

static void on_mouse(const hal_event_t *ev) {
    int x = ev->x, y = ev->y;
    if (ev->type == EVT_MOUSE_MOVE) {
        if (drag_split) {
            lp = clampi(drag_lp0 + x - drag_x0, 170, cw - 260 > 170 ? cw - 260 : 170);
            layout_controls();
            relayout();
            return;
        }
        item_t *it = hit(x, y);
        node_t *l = it ? it->link : NULL;
        if (l != hover_link) { hover_link = l; dirty = 1; }
        int th_ = toc_at(x, y);
        if (th_ != toc_hover) { toc_hover = th_; dirty = 1; }
        set_cursor(l || th_ >= 0 ? CUR_HAND : x >= lp && x < lp + SPL && y >= TOOL_H ? CUR_SIZE_WE : CUR_ARROW);
        return;
    }
    if (ev->type == EVT_MOUSE_UP) {
        if (drag_split) { drag_split = 0; setting_set_int("help.pane", lp); }
        return;
    }
    if (ev->type != EVT_MOUSE_DOWN && ev->type != EVT_DBLCLICK) return;
    if (ev->button != MOUSE_LEFT) return;
    if (x >= lp && x < lp + SPL && y >= TOOL_H) { drag_split = 1; drag_x0 = x; drag_lp0 = lp; return; }
    int t = tab_hit(x, y);
    if (t >= 0) { set_tab(t); return; }
    int i = toc_at(x, y);
    if (i >= 0) {
        int on_icon = x < 8 + toc[i].depth * 16 + 18;
        toc_activate(i, ev->type == EVT_DBLCLICK || on_icon || toc[i].kids);
        ctl_focus(win, 0);
        return;
    }
    item_t *it = hit(x, y);
    if (it && it->link) follow(it->link);
}

static void handle(const hal_event_t *ev) {
    switch (ev->type) {
    case EVT_PAINT: dirty = 1; break;
    case EVT_CLOSE: exit(0);
    case EVT_RESIZE:
        cw = ev->x; ch = ev->y;
        win_canvas(win, &cv);
        lp = clampi(lp, 170, cw - 260 > 170 ? cw - 260 : 170);
        layout_controls();
        relayout();
        break;
    case EVT_COMMAND: command(ev->id, ev->button); break;
    case EVT_MENU: command(ev->id, 0); break;
    case EVT_SCROLL:
        if (ev->id == C_PAGESCROLL) { scroll_y = ev->x; dirty = 1; }
        if (ev->id == C_TOCSCROLL) { toc_top = ev->x; dirty = 1; }
        break;
    case EVT_WHEEL:
        if (ev->x < lp) { toc_top -= ev->dz * 3; dirty = 1; }
        else scroll_by(-ev->dz * 60);
        break;
    case EVT_KEY: on_key(ev); break;
    case EVT_MOUSE_DOWN: case EVT_MOUSE_MOVE: case EVT_MOUSE_UP: case EVT_DBLCLICK: on_mouse(ev); break;
    case EVT_MOUSE_LEAVE: if (hover_link || toc_hover >= 0) { hover_link = NULL; toc_hover = -1; dirty = 1; } set_cursor(CUR_ARROW); break;
    }
}

static void create_controls(void) {
    static const struct { int id; const char *label; int icon; const char *tip; } tb[] = {
        { C_BACK, "Back", HICON_BACK, "Back (Alt+Left)" },
        { C_FWD, "Forward", HICON_FORWARD, "Forward (Alt+Right)" },
        { C_HOME, "Home", HICON_HOME, "Help and Support home (Alt+Home)" },
    };
    for (int i = 0; i < N(tb); i++) {
        ctl_create(win, CTL_TOOLBUTTON, 0, 0, 60, TOOL_H - 6, tb[i].label, tb[i].id, 0);
        ctl_set_tool(win, tb[i].id, tb[i].icon, tb[i].tip);
    }
    ctl_create(win, CTL_VSCROLL, 0, 0, SB_W, 100, 0, C_TOCSCROLL, 0);
    ctl_create(win, CTL_LABEL, 0, 0, 100, 16, "Type in the keyword to find:", C_IXLABEL, CTL_HIDDEN);
    ctl_create(win, CTL_TEXTBOX, 0, 0, 100, 22, "", C_IXFILTER, CTL_HIDDEN | CTL_NOTIFY | CTL_CHANGES);
    ctl_create(win, CTL_LISTBOX, 0, 0, 100, 100, NULL, C_IXLIST, CTL_HIDDEN);
    ctl_create(win, CTL_BUTTON, 0, 0, 80, 23, "Display", C_IXSHOW, CTL_HIDDEN);
    ctl_create(win, CTL_LABEL, 0, 0, 100, 16, "Type in the words to search for:", C_SQLABEL, CTL_HIDDEN);
    ctl_create(win, CTL_TEXTBOX, 0, 0, 100, 22, "", C_SQ, CTL_HIDDEN);
    ctl_create(win, CTL_BUTTON, 0, 0, 90, 23, "List Topics", C_SGO, CTL_HIDDEN);
    ctl_create(win, CTL_LABEL, 0, 0, 100, 16, "Select a topic to display:", C_SRLABEL, CTL_HIDDEN);
    ctl_create(win, CTL_LISTBOX, 0, 0, 100, 100, NULL, C_SRES, CTL_HIDDEN);
    ctl_create(win, CTL_BUTTON, 0, 0, 80, 23, "Display", C_SSHOW, CTL_HIDDEN);
    ctl_create(win, CTL_VSCROLL, 0, 0, SB_W, 100, 0, C_PAGESCROLL, 0);
    layout_controls();
    show_tab_controls();
}

/* ---------------- the boot test (--test) ---------------- */
static int t_checks, t_fails;
#define CHECK(c, ...) do { t_checks++; if (!(c)) { t_fails++; printf("helptest: FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int doc_has_text(const char *w) {
    for (int i = 0; doc && i < doc->nitems; i++)
        if (doc->items[i].kind == IT_TEXT && doc->items[i].len >= (int)strlen(w) && !strncmp(doc->items[i].text, w, strlen(w))) return 1;
    return 0;
}
static void run_test(const char *start) {
    printf("helptest: starting (page %s)\n", start);
    CHECK(ntoc >= 20, "contents: %d entries", ntoc);
    CHECK(nidx >= 100, "index: %d entries", nidx);
    /* the start page */
    CHECK(show_page(start, 0) == 0 && doc && doc->nitems > 20, "page %s: %d items", start, doc ? doc->nitems : -1);
    CHECK(doc && doc->title && strstr(doc->title, "Notepad"), "title of %s: %s", start, doc && doc->title ? doc->title : "(none)");
    CHECK(toc_sel >= 0 && !strcmp(toc[toc_sel].page, "notepad.htm"), "contents shows the page");
    paint();
    /* every page in the contents parses and lays out, and every link leads to a page */
    int pages_ok = 0, links = 0, bad_links = 0;
    for (int i = 0; i < ntoc; i++) {
        if (!toc[i].page[0]) continue;
        show_page(toc[i].page, 0);
        if (doc && doc->nitems > 5 && doc->title && doc->title[0] && doc->height > 50 && !strstr(doc->title, "not found")) pages_ok++;
        else CHECK(0, "page %s did not render", toc[i].page);
        for (int k = 0; doc && k < doc->nitems; k++) {
            node_t *l = doc->items[k].link;
            if (!l || (k && doc->items[k - 1].link == l)) continue;
            const char *h = node_attr(l, "href");
            if (!h || !strncmp(h, "run:", 4) || !strncmp(h, "http", 4)) continue;
            links++;
            if (h[0] == '#') { if (anchor_y(doc, h + 1) < 0) { bad_links++; printf("helptest: %s: no anchor %s\n", toc[i].page, h); } continue; }
            char ref[96], file[96], path[160];
            norm_ref(h, ref, sizeof ref);
            ref_file(ref, file, sizeof file);
            snprintf(path, sizeof path, "%s/%s", HELP_DIR, file);
            hal_dirent_t st;
            if (stat(path, &st) < 0) { bad_links++; printf("helptest: %s: link to %s\n", toc[i].page, h); }
        }
    }
    CHECK(pages_ok >= 20, "%d pages rendered", pages_ok);
    CHECK(links >= 50 && !bad_links, "%d links, %d broken", links, bad_links);
    printf("helptest: %d pages rendered, %d links\n", pages_ok, links);
    /* history */
    show_page("notepad", 0);
    show_page("paint", 0);
    go_hist(hpos - 1);
    CHECK(!strcmp(cur_page, "notepad.htm"), "Back: %s", cur_page);
    go_hist(hpos + 1);
    CHECK(!strcmp(cur_page, "paint.htm"), "Forward: %s", cur_page);
    /* an anchor */
    show_page("notepad#wrap", 0);
    CHECK(doc && scroll_y > 0, "anchor #wrap: scroll %d", scroll_y);
    /* following a link */
    show_page("welcome", 0);
    node_t *first = NULL;
    for (int k = 0; doc && k < doc->nitems && !first; k++) if (doc->items[k].link) first = doc->items[k].link;
    if (first) follow(first);
    CHECK(first && strcmp(cur_page, "welcome.htm"), "a link from the home page: %s", cur_page);
    /* the index */
    set_tab(TAB_INDEX);
    ctl_set_text(win, C_IXFILTER, "undo");
    fill_index();
    CHECK(nix >= 1 && starts_icase(ix_items[0], "undo"), "index filter \"undo\": %d", nix);
    index_show();
    CHECK(!strncmp(cur_page, "notepad.htm", 11), "index \"undo\" shows %s", cur_page);
    paint();
    /* search */
    set_tab(TAB_SEARCH);
    ctl_set_text(win, C_SQ, "word wrap");
    do_search();
    CHECK(nsr >= 1 && !strcmp(pages[sr_page[0]].page, "notepad.htm"), "search \"word wrap\": %d, first %s", nsr, nsr ? pages[sr_page[0]].page : "-");
    ctl_set_text(win, C_SQ, "recycle bin");
    do_search();
    CHECK(nsr >= 1 && !strcmp(pages[sr_page[0]].page, "recycle.htm"), "search \"recycle bin\": first %s", nsr ? pages[sr_page[0]].page : "-");
    ctl_set_text(win, C_SQ, "zzqqxx");
    do_search();
    CHECK(nsr == 0, "search for nonsense: %d", nsr);
    show_page("notepad", 0);
    paint();
    CHECK(doc_has_text("Undo and redo") && doc_has_text("Word wrap"), "the headings of the Notepad page");
    if (t_fails) printf("helptest: %d of %d checks FAILED\n", t_fails, t_checks);
    else printf("helptest: all %d checks passed\n", t_checks);
    win_destroy(win);
    exit(t_fails ? 1 : 0);
}

int main(int argc, char **argv) {
    g_metrics.text_width = m_width;
    g_metrics.font_height = m_height;
    g_metrics.font_ascent = m_ascent;
    g_metrics.text_fit = m_fit;
    char start[96] = HOME_PAGE;
    int a = 1;
    if (argc > 1 && !strcmp(argv[1], "--test")) { test_mode = 1; a = 2; strlcpy(start, "notepad", sizeof start); }
    if (argc > a && argv[a][0]) strlcpy(start, argv[a], sizeof start);
    lp = clampi(setting_get_int("help.pane", 250), 170, 500);
    load_contents();
    load_index();
    /* one Help window: a second HELP.EXE hands its page to the first and ends */
    int lsock = -1;
    if (!test_mode && (lsock = local_listen("halcyon.help", 4)) < 0) {
        int c = local_connect("halcyon.help");
        if (c >= 0) {
            send(c, start, strlen(start) + 1, 0);
            close(c);
            return 0;
        }
    }
    win = win_create("Help and Support", cw, ch, WIN_RESIZABLE);
    if (win < 0) return 1;
    win_set_icon(win, HICON_HELP);
    win_set_min_size(win, 480, 300);
    win_size(win, &cw, &ch);
    win_canvas(win, &cv);
    create_controls();
    fill_index();
    if (ntoc) toc[0].open = 1;
    if (test_mode) run_test(start);
    show_page(start, 0);
    for (;;) {
        hal_event_t ev;
        if (dirty) {
            int n = 0;
            uint64_t t0 = time_ms();
            while (n < 64 && time_ms() - t0 < 30 && win_get_event(&ev, 0)) { handle(&ev); n++; }
            if (dirty) paint();
            continue;
        }
        struct pollfd pf[2] = { { FD_WINDOWS, POLLIN, 0 }, { lsock, POLLIN, 0 } };
        poll(pf, lsock >= 0 ? 2 : 1, -1);
        if (lsock >= 0 && (pf[1].revents & POLLIN)) {
            int c = accept(lsock, NULL, NULL);
            if (c >= 0) {
                char ref[96];
                sock_timeouts(c, 1000, 1000);
                long n = recv(c, ref, sizeof ref - 1, MSG_WAITALL);
                close(c);
                if (n > 0) {
                    ref[n] = 0;
                    show_page(ref[0] ? ref : HOME_PAGE, 0);
                    SC1(SYS_WIN_SHOW, win);        /* to the front */
                }
            }
        }
        while (win_get_event(&ev, 0)) handle(&ev);
    }
}
