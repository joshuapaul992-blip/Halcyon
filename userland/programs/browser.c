/* BROWSER.EXE - "Web Browser": a minimal web browser as a user program.
 *
 * The window: a menu bar, tool buttons (Back, Forward, Stop, Reload,
 * Home), an address bar with Go, the page, a vertical scroll bar and a
 * status bar (the link under the pointer, progress, errors, the zone).
 * Pages come over HTTP and HTTPS (libweb: a worker thread per request,
 * cancelled by Stop), from files (file:///C:/... or C:\...), data: URLs and
 * built-in about: pages. HTML is parsed (browser/html.c), laid out
 * (browser/layout.c) into a display list that is drawn here: text with the
 * system fonts, images (browser/image.c: PNG, GIF, BMP) straight into the
 * window's mapped pixels, and the form controls, which the browser draws
 * and edits itself so they scroll with the page. No JavaScript, no style
 * sheets beyond a few properties of the style attribute.
 *
 * Security: the browser is an ordinary program in its own address space;
 * a page can at worst crash it. Certificates are checked by libweb and a
 * rejected one shows an error page with the reason, with no way around it.
 * See docs/DESIGN-M6-WEB.md. */
#include "hal.h"
#include "web.h"
#include "browser/browser.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define TOOL_H 34
#define ADDR_H 28
#define TOP (TOOL_H + ADDR_H)
#define STATUS_H 22
#define SB_W 17
#define LINE_STEP 40
#define HIST_MAX 50
#define CACHE_MAX (24u << 20)
#define PAGE_MAX (32u << 20)
#define IMAGE_MAX_BYTES (8u << 20)
#define HOME_URL "about:home"

enum { C_BACK = 100, C_FWD, C_STOP, C_RELOAD, C_HOME, C_ADDR_LABEL, C_ADDR, C_GO, C_VSCROLL, C_STATUS };
enum { M_OPEN = 1, M_SAVE, M_EXIT, M_COPYURL, M_STOP, M_RELOAD, M_SOURCE, M_BACK, M_FWD, M_HOME, M_ABOUT, M_HELP,
       M_LINK_OPEN, M_LINK_COPY, M_LINK_SAVE, M_IMG_OPEN, M_SELECT_BASE = 1000 };

static const hal_menu_item_t bar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&Open...", "Ctrl+O", M_OPEN, 0, HICON_FOLDER_OPEN, 0 },
    { "&Save Page As...", "Ctrl+S", M_SAVE, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Close", 0, M_EXIT, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "&Copy Address", 0, M_COPYURL, 0, 0, 0 },
    { "&View", 0, 0, MENU_TITLE, 0, 0 },
    { "&Stop", "Esc", M_STOP, 0, HICON_STOP, 0 },
    { "&Reload", "F5", M_RELOAD, 0, HICON_RELOAD, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Page S&ource", 0, M_SOURCE, 0, 0, 0 },
    { "&Go", 0, 0, MENU_TITLE, 0, 0 },
    { "&Back", "Alt+Left", M_BACK, 0, HICON_BACK, 0 },
    { "&Forward", "Alt+Right", M_FWD, 0, HICON_FORWARD, 0 },
    { "&Home Page", "Alt+Home", M_HOME, 0, HICON_HOME, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&Help Topics", "F1", M_HELP, 0, HICON_HELP, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&About Web Browser", 0, M_ABOUT, 0, HICON_WEB, 0 },
};

/* ---------------- state ---------------- */
static int win, cw = 800, ch = 600;
static hal_canvas_t cv;
static hal_theme_t th;
static web_session_t *sess;

typedef struct {
    char *url;
    char *title;
    int scroll;
    uint8_t *body;               /* the page's bytes (cache), or NULL */
    size_t len;
    char ctype[64], charset[32];
    int secure;
    char *post;                  /* a POST's body: not sent again without asking */
} hist_t;
static hist_t hist[HIST_MAX];
static int nhist, hpos = -1;

/* the page shown */
static doc_t *doc;
static char cur_url[URL_MAX];
static uint8_t *src;             /* its bytes */
static size_t srclen;
static char src_ctype[64], src_charset[32];
static int secure, scroll_y, page_error;
static char tls_text[96];

/* loading */
static web_job_t *job;
static int job_kind;             /* 1 page, 2 download, 3 image */
static int job_hist;             /* page: 0 new history entry, 1 replace, 2 history move */
static int job_img = -1;
static char job_url[URL_MAX];
static char *job_post;
static hal_mutex_t prog_lock;
static char prog_phase[200];
static long long prog_got, prog_total;
static int loading, refresh_count;
static uint64_t refresh_at;
static char refresh_target[URL_MAX];

/* interaction */
static fctl_t *focus;
static node_t *hover_link;
static int cursor_shape = -1;
static char status_main[300], shown_status[400];
static fctl_t *popup_ctl;
static node_t *ctx_link;
static int dirty = 1;
static char shown_title[300];

static int ncase(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static int view_w(void) { return cw - SB_W > 50 ? cw - SB_W : 50; }
static int view_h(void) { int h = ch - TOP - STATUS_H; return h > 10 ? h : 10; }

/* ---------------- metrics for layout ---------------- */
static int m_width(const char *s, int n, int font) { return text_width_n(s, n, font); }
static int m_height(int font) { return text_height(font); }
static int m_ascent(int font) { return text_ascent(font); }
static int m_fit(const char *s, int n, int font, int w) { return text_fit(s, n, font, w); }

/* ---------------- status ---------------- */
static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_main, sizeof status_main, fmt, ap);
    dirty = 1;
    va_end(ap);
}

static void update_status(void) {
    char s[400];
    const char *zone = !strncmp(cur_url, "file:", 5) || !strncmp(cur_url, "about:", 6) ? "My Computer" :
                       secure ? "Secure (TLS 1.2)" : "Internet";
    if (hover_link) {
        const char *h = node_attr(hover_link, "href");
        if (!h) h = node_attr(hover_link, "src");
        char abs[URL_MAX];
        if (h && !url_resolve(doc ? doc->base : cur_url, h, abs, sizeof abs)) snprintf(s, sizeof s, "%s\t%s", abs, zone);
        else snprintf(s, sizeof s, "%s\t%s", h ? h : "", zone);
    } else snprintf(s, sizeof s, "%s\t%s", status_main[0] ? status_main : "Done", zone);
    if (strcmp(s, shown_status)) {
        strlcpy(shown_status, s, sizeof shown_status);
        ctl_set_text(win, C_STATUS, s);
    }
}

static void update_title(void) {
    char t[300];
    const char *tt = doc && doc->title && doc->title[0] ? doc->title : 0;
    if (tt) {
        /* collapse white space, at most 120 bytes */
        char b[128];
        int k = 0, sp = 0;
        for (const char *p = tt; *p && k < 120; p++) {
            if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') { sp = k > 0; continue; }
            if (sp) { b[k++] = ' '; sp = 0; }
            b[k++] = *p;
        }
        b[k] = 0;
        snprintf(t, sizeof t, "%s - Web Browser", b);
    } else if (cur_url[0] && strcmp(cur_url, HOME_URL)) snprintf(t, sizeof t, "%.200s - Web Browser", cur_url);
    else strlcpy(t, "Web Browser", sizeof t);
    if (strcmp(t, shown_title)) { strlcpy(shown_title, t, sizeof shown_title); win_set_title(win, t); }
}

static void update_buttons(void) {
    ctl_set_style(win, C_BACK, hpos > 0 ? 0 : CTL_DISABLED);
    ctl_set_style(win, C_FWD, hpos + 1 < nhist ? 0 : CTL_DISABLED);
    ctl_set_style(win, C_STOP, loading ? 0 : CTL_DISABLED);
    menu_enable(win, M_BACK, hpos > 0);
    menu_enable(win, M_FWD, hpos + 1 < nhist);
    menu_enable(win, M_STOP, loading);
}

static void set_address(const char *url) {
    char t[256];
    strlcpy(t, url, sizeof t);
    ctl_set_text(win, C_ADDR, t);
}

/* ---------------- drawing ---------------- */
static int clip_x0, clip_y0, clip_x1, clip_y1;     /* canvas coordinates of the page view */

static void cfill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1 || !cv.pixels) return;
    for (int j = y0; j < y1; j++) {
        unsigned *row = cv.pixels + (size_t)j * (size_t)cv.stride;
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}
static void cframe(int x, int y, int w, int h, uint32_t c, int t) {
    cfill(x, y, w, t, c);
    cfill(x, y + h - t, w, t, c);
    cfill(x, y, t, h, c);
    cfill(x + w - t, y, t, h, c);
}

/* a filled circle of radius r (centre cx, cy) */
static void cdisc(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
        cfill(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

static uint32_t blend(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    return r << 16 | g << 8 | b;
}

/* an image scaled (nearest neighbour) to w x h at x, y */
static void cimage(const image_t *im, int x, int y, int w, int h) {
    if (!im->px || w <= 0 || h <= 0 || !cv.pixels) return;
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    for (int j = y0; j < y1; j++) {
        int sy = (int)((long)(j - y) * im->h / h);
        const uint32_t *srow = im->px + (size_t)sy * (size_t)im->w;
        unsigned *row = cv.pixels + (size_t)j * (size_t)cv.stride;
        for (int i = x0; i < x1; i++) {
            uint32_t p = srow[(long)(i - x) * im->w / w];
            uint32_t a = p >> 24;
            if (a == 255) row[i] = p & 0xFFFFFF;
            else if (a) row[i] = blend(row[i], p, a);
        }
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

static int roman(int v, char *out, int upper) {
    static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *sym[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    int k = 0;
    if (v <= 0 || v > 3999) return snprintf(out, 16, "%d", v);
    for (int i = 0; i < 13; i++) while (v >= val[i] && k < 14) { for (const char *p = sym[i]; *p; p++) out[k++] = (char)(upper ? toupper(*p) : *p); v -= val[i]; }
    out[k] = 0;
    return k;
}

static void draw_control(const item_t *it, int x, int y) {
    fctl_t *k = it->node->ctl;
    if (!k) return;
    int w = it->w, h = it->h, f = FONT_UI;
    int lh = text_height(f);
    switch (k->kind) {
    case FC_TEXT: case FC_PASSWORD: case FC_TEXTAREA: {
        cfill(x, y, w, h, 0xFFFFFF);
        cframe(x, y, w, h, focus == k ? 0x3C7FB1 : 0x7F9DB9, 1);
        gfx_clip(win, x + 2 > clip_x0 ? x + 2 : clip_x0, y + 1 > clip_y0 ? y + 1 : clip_y0,
                 (x + w - 2 < clip_x1 ? x + w - 2 : clip_x1) - (x + 2 > clip_x0 ? x + 2 : clip_x0),
                 (y + h - 1 < clip_y1 ? y + h - 1 : clip_y1) - (y + 1 > clip_y0 ? y + 1 : clip_y0));
        int tx = x + 4 - k->scroll, ty = y + 4;
        if (k->kind == FC_PASSWORD) {
            char stars[256];
            int n = 0;
            for (int i = 0; i < k->vlen && n < 255; i++) if (((unsigned char)k->value[i] & 0xC0) != 0x80) stars[n++] = '*';
            ctext(tx, ty, stars, n, 0, f);
            if (focus == k) {
                int cn = 0;
                for (int i = 0; i < k->caret; i++) if (((unsigned char)k->value[i] & 0xC0) != 0x80) cn++;
                int cx = tx + text_width_n(stars, cn, f);
                cfill(cx, ty, 1, lh, 0);
            }
        } else if (k->kind == FC_TEXTAREA) {
            int ly = ty, s0 = 0;
            for (int i = 0; i <= k->vlen; i++) {
                if (i == k->vlen || k->value[i] == '\n') {
                    ctext(tx, ly, k->value + s0, i - s0, 0, f);
                    if (focus == k && k->caret >= s0 && k->caret <= i) cfill(tx + text_width_n(k->value + s0, k->caret - s0, f), ly, 1, lh, 0);
                    ly += lh;
                    s0 = i + 1;
                }
            }
        } else {
            ctext(tx, ty, k->value, k->vlen, 0, f);
            if (focus == k) cfill(tx + text_width_n(k->value, k->caret, f), ty, 1, lh, 0);
        }
        gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
        break;
    }
    case FC_CHECKBOX:
        cfill(x, y, 13, 13, 0xFFFFFF);
        cframe(x, y, 13, 13, 0x1C5180, 1);
        if (k->checked) for (int i = 0; i < 7; i++) { int yy = i < 3 ? y + 5 + i : y + 9 - (i - 2); cfill(x + 3 + i, yy, 1, 3, 0x21A121); }
        break;
    case FC_RADIO:
        cdisc(x + 6, y + 6, 6, 0x1C5180);
        cdisc(x + 6, y + 6, 5, 0xFFFFFF);
        if (k->checked) cdisc(x + 6, y + 6, 3, 0x21A121);
        break;
    case FC_SELECT: {
        cfill(x, y, w, h, 0xFFFFFF);
        cframe(x, y, w, h, 0x7F9DB9, 1);
        gfx_rounded(win, x + w - 18, y + 2, 16, h - 4, 2, 0xC1D3FB, 0x9DB9EB);
        int ax = x + w - 10, ay = y + h / 2 - 1;
        for (int i = 0; i < 4; i++) cfill(ax - 3 + i, ay + i, 7 - 2 * i, 1, 0x4D6185);
        const char *lab = k->noptions && k->selected < k->noptions ? k->opt_label[k->selected] : "";
        gfx_clip(win, x + 2 > clip_x0 ? x + 2 : clip_x0, clip_y0, (x + w - 20 < clip_x1 ? x + w - 20 : clip_x1) - (x + 2 > clip_x0 ? x + 2 : clip_x0), clip_y1 - clip_y0);
        ctext(x + 4, y + 4, lab, (int)strlen(lab), 0, f);
        gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
        break;
    }
    default: {
        gfx_rounded(win, x, y, w, h, 3, 0xFFFFFF, 0xD6D0C5);
        gfx_rounded_outline(win, x, y, w, h, 3, 0x003C74);
        const char *v = k->value && k->value[0] ? k->value : k->kind == FC_RESET ? "Reset" : "Submit";
        int tw = text_width(v, f);
        ctext(x + (w - tw) / 2, y + (h - lh) / 2, v, (int)strlen(v), 0, f);
        break;
    }
    }
}

static void paint(void) {
    gfx_theme(&th);
    int vw = view_w(), vh = view_h();
    clip_x0 = 0; clip_y0 = TOP; clip_x1 = vw; clip_y1 = TOP + vh;
    if (clip_x1 > cv.w) clip_x1 = cv.w;
    if (clip_y1 > cv.h) clip_y1 = cv.h;
    /* the tool area's background and separators */
    gfx_clip_reset(win);
    gfx_gradient(win, 0, 0, cw, TOOL_H, 0xFCFCFE, th.face);
    gfx_fill(win, 0, TOOL_H - 1, cw, 1, 0xC0C0C0);
    gfx_fill(win, 0, TOOL_H, cw, ADDR_H, th.face);
    gfx_fill(win, 0, TOP - 1, cw, 1, 0xA0A0A0);
    uint32_t bg = doc && doc->has_bg ? doc->bg : 0xFFFFFF;
    cfill(0, TOP, vw, vh, bg);
    if (doc) {
        gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
        int oy = TOP - scroll_y;
        int top = scroll_y - 4, bottom = scroll_y + vh + 4;
        for (int i = 0; i < doc->nitems; i++) {
            const item_t *it = &doc->items[i];
            if (it->y > bottom || it->y + it->h < top) {
                if (it->kind != IT_BULLET) continue;
            }
            int x = it->x, y = it->y + oy;
            switch (it->kind) {
            case IT_TEXT: {
                uint32_t c = it->color;
                ctext(x, y, it->text, it->len, c, it->font);
                if (it->flags & IF_UNDERLINE) cfill(x, y + text_ascent(it->font) + 1, it->w, 1, c);
                if (it->flags & IF_STRIKE) cfill(x, y + text_ascent(it->font) * 2 / 3, it->w, 1, c);
                break;
            }
            case IT_RECT: cfill(x, y, it->w, it->h, it->color); break;
            case IT_FRAME:
                if (it->node) {
                    /* an image that is still loading (or failed) with a size */
                    cframe(x, y, it->w, it->h, 0xC0C0C0, 1);
                    if (it->len && it->w > 20) {
                        gfx_clip(win, x > clip_x0 ? x : clip_x0, y > clip_y0 ? y : clip_y0, it->w, it->h);
                        ctext(x + 4, y + 3, it->text, it->len, 0x606060, FONT_UI);
                        gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
                    }
                } else if (it->flags == 1) {
                    cframe(x, y, it->w, it->h, 0xA0A0A0, 1);
                } else {
                    int t = it->flags ? it->flags : 1;
                    cfill(x, y, it->w, t, 0xD4D0C8);
                    cfill(x, y, t, it->h, 0xD4D0C8);
                    cfill(x, y + it->h - t, it->w, t, 0x808080);
                    cfill(x + it->w - t, y, t, it->h, 0x808080);
                }
                break;
            case IT_IMAGE: {
                const image_t *im = it->node->img ? &doc->images[it->node->img - 1] : 0;
                if (im && im->state == 2) cimage(im, x, y, it->w, it->h);
                if (it->link) cframe(x, y, it->w, it->h, doc->link_color, 1);
                break;
            }
            case IT_ALT:
                cframe(x, y, it->w, it->h, 0xA0A0A0, 1);
                gfx_clip(win, x > clip_x0 ? x : clip_x0, y > clip_y0 ? y : clip_y0, it->w, it->h);
                ctext(x + 4, y + 3, it->text, it->len, it->link ? doc->link_color : 0x404040, it->font);
                gfx_clip(win, clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
                break;
            case IT_HR:
                cfill(x, y, it->w, 1, 0x9A9A9A);
                cfill(x, y + 1, it->w, 1, 0xEEEEEE);
                break;
            case IT_BULLET: {
                if (y > clip_y1 || y + 40 < clip_y0) break;
                int f = it->font, a = text_ascent(f);
                if (it->len >= 0) {
                    char num[24];
                    int nl;
                    if (it->w == 11 || it->w == 12) {
                        int v = it->len - 1;
                        char t[8];
                        int k = 0;
                        do { t[k++] = (char)((it->w == 11 ? 'a' : 'A') + v % 26); v = v / 26 - 1; } while (v >= 0 && k < 6);
                        nl = 0;
                        while (k) num[nl++] = t[--k];
                        num[nl] = 0;
                    } else if (it->w == 13 || it->w == 14) nl = roman(it->len, num, it->w == 14);
                    else nl = snprintf(num, sizeof num, "%d", it->len);
                    num[nl++] = '.';
                    num[nl] = 0;
                    int tw = text_width(num, f);
                    ctext(x + 14 - tw, y, num, nl, it->color, f);
                } else {
                    int r = a > 12 ? 4 : 3, cx = x + 10, cy = y + a - a / 3;
                    if (it->flags & IF_SQUARE) cfill(cx - r, cy - r, 2 * r, 2 * r, it->color);
                    else {
                        cdisc(cx, cy, r, it->color);
                        if (it->flags & IF_CIRCLE) cdisc(cx, cy, r - 1, doc->has_bg ? doc->bg : 0xFFFFFF);
                    }
                }
                break;
            }
            case IT_CONTROL: draw_control(it, x, y); break;
            }
        }
        gfx_clip_reset(win);
    }
    /* the scroll bar */
    int total = doc ? doc->height : 0;
    int maxs = total - vh > 0 ? total - vh : 0;
    if (scroll_y > maxs) scroll_y = maxs;
    if (scroll_y < 0) scroll_y = 0;
    ctl_scroll_set(win, C_VSCROLL, total > vh ? total : vh, vh, scroll_y, LINE_STEP);
    update_status();
    update_title();
    win_invalidate(win);
    dirty = 0;
}

/* ---------------- history ---------------- */
static void hist_free(hist_t *h) {
    free(h->url); free(h->title); free(h->body); free(h->post);
    memset(h, 0, sizeof *h);
}

/* keep the cached pages under CACHE_MAX (the oldest go first) */
static void cache_trim(void) {
    size_t total = 0;
    for (int i = 0; i < nhist; i++) total += hist[i].len;
    for (int i = 0; i < nhist && total > CACHE_MAX; i++) {
        if (i == hpos || !hist[i].body) continue;
        total -= hist[i].len;
        free(hist[i].body);
        hist[i].body = 0;
        hist[i].len = 0;
    }
}

static void hist_save_current(void) {
    if (hpos < 0 || hpos >= nhist) return;
    hist[hpos].scroll = scroll_y;
    free(hist[hpos].title);
    hist[hpos].title = doc && doc->title ? strdup(doc->title) : 0;
}

/* the page now shown becomes a history entry */
static void hist_record(int mode, const char *post) {
    if (mode == 2) return;
    if (mode == 0 || hpos < 0) {
        for (int i = hpos + 1; i < nhist; i++) hist_free(&hist[i]);
        nhist = hpos + 1;
        if (nhist == HIST_MAX) {
            hist_free(&hist[0]);
            memmove(hist, hist + 1, sizeof hist[0] * (HIST_MAX - 1));
            nhist--;
            memset(&hist[nhist], 0, sizeof hist[0]);
        }
        hpos = nhist++;
    } else {
        free(hist[hpos].body);
        free(hist[hpos].url);
        free(hist[hpos].post);
        hist[hpos].body = 0;
        hist[hpos].url = 0;
        hist[hpos].post = 0;
    }
    hist_t *h = &hist[hpos];
    h->url = strdup(cur_url);
    h->scroll = 0;
    h->secure = secure;
    h->post = post ? strdup(post) : 0;
    strlcpy(h->ctype, src_ctype, sizeof h->ctype);
    strlcpy(h->charset, src_charset, sizeof h->charset);
    if (src && srclen && !page_error) {
        h->body = malloc(srclen);
        if (h->body) { memcpy(h->body, src, srclen); h->len = srclen; }
    }
    cache_trim();
}

/* ---------------- showing a document ---------------- */
static void start_images(void);

static void relayout(int keep_scroll) {
    if (!doc) return;
    int old_h = doc->height;
    layout_doc(doc, view_w());
    if (!keep_scroll && old_h) scroll_y = (int)((long)scroll_y * doc->height / old_h);
    dirty = 1;
}

static void show_doc(doc_t *d, const char *url) {
    if (job && job_kind == 3) { web_job_free(job); job = 0; job_kind = 0; }
    doc_free(doc);
    doc = d;
    focus = 0;
    hover_link = 0;
    if (url) strlcpy(cur_url, url, sizeof cur_url);
    if (doc) {
        /* a <base href> is relative to the page */
        char abs[URL_MAX];
        if (strcmp(doc->base, cur_url) && !url_resolve(cur_url, doc->base, abs, sizeof abs)) strlcpy(doc->base, abs, sizeof doc->base);
        else strlcpy(doc->base, cur_url, sizeof doc->base);
        layout_doc(doc, view_w());
    }
    scroll_y = 0;
    /* a fragment: scroll to it */
    const char *hash = strchr(cur_url, '#');
    if (doc && hash && hash[1]) {
        char nm[256];
        url_decode(hash + 1, strlen(hash + 1), nm, sizeof nm, 0);
        int y = anchor_y(doc, nm);
        if (y >= 0) scroll_y = y;
    }
    set_address(cur_url);
    dirty = 1;
    update_buttons();
    start_images();
}

static void set_source(const uint8_t *b, size_t n, const char *ctype, const char *charset) {
    free(src);
    src = 0;
    srclen = 0;
    if (b && n) {
        src = malloc(n + 1);
        if (src) { memcpy(src, b, n); src[n] = 0; srclen = n; }
    }
    strlcpy(src_ctype, ctype ? ctype : "", sizeof src_ctype);
    strlcpy(src_charset, charset ? charset : "", sizeof src_charset);
}

static void html_escape(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : 0;
        if (r) { size_t l = strlen(r); memcpy(out + o, r, l); o += l; }
        else out[o++] = *s;
    }
    out[o] = 0;
}

/* a page made here (errors, about:) */
static void show_html(const char *html, const char *url, int is_error) {
    set_source((const uint8_t *)html, strlen(html), "text/html", "utf-8");
    page_error = is_error;
    doc_t *d = html_parse(html, strlen(html), "utf-8", url);
    show_doc(d, url);
}

static void error_page(const char *url, int err, const char *text, int status) {
    char eu[URL_MAX * 2], et[600];
    html_escape(url, eu, sizeof eu);
    html_escape(text && text[0] ? text : web_strerror(err), et, sizeof et);
    const char *title, *head, *advice;
    switch (err) {
    case WEB_E_NONET:
        title = "No network connection"; head = "This computer is not connected to a network";
        advice = "<li>Halcyon finds no network here: there may be no network adapter it has a driver for, or the network is not set up yet.</li>"
                 "<li>Pages on this computer still work: try <a href=\"about:home\">the home page</a> or a file such as C:\\Documents\\Web\\index.htm.</li>";
        break;
    case WEB_E_DNS:
        title = "Server not found"; head = "The server could not be found";
        advice = "<li>Check the address for typing errors such as <b>ww.example.com</b> instead of <b>www.example.com</b>.</li>"
                 "<li>Check that the network connection works.</li>";
        break;
    case WEB_E_CERT:
        title = "Certificate problem"; head = "This connection is not secure";
        advice = "<li>The browser could not confirm that this is the real site. Someone may be trying to impersonate it, or it is set up wrongly.</li>"
                 "<li>The browser does not show pages whose certificate it cannot check, and there is no way to continue anyway.</li>"
                 "<li>If the reason is a date, check that the computer's clock is right.</li>";
        break;
    case WEB_E_TLS:
        title = "Secure connection failed"; head = "A secure connection could not be made";
        advice = "<li>This browser speaks TLS 1.2. Servers that only accept TLS 1.3 cannot be visited with it.</li>"
                 "<li>Try again later, or try the address with http:// if the site offers it.</li>";
        break;
    case WEB_E_TIMEOUT:
        title = "Timed out"; head = "The server took too long to answer";
        advice = "<li>The site may be busy or down. Try again in a moment.</li><li>Check the network connection.</li>";
        break;
    case WEB_E_CONNECT:
        title = "Cannot connect"; head = "The connection to the server failed";
        advice = "<li>The server may be down, or the address may be wrong.</li>";
        break;
    case WEB_E_CANCELED:
        title = "Stopped"; head = "Loading was stopped";
        advice = "<li>Click <b>Reload</b> to try again.</li>";
        break;
    case E_NOENT:
        title = "File not found"; head = "The file could not be found";
        advice = "<li>Check the file name and the folder.</li>";
        break;
    default:
        title = "Cannot open the page"; head = "The page cannot be displayed";
        advice = "<li>Click <b>Reload</b> to try again, or check the address.</li>";
        break;
    }
    char *html = malloc(sizeof eu + sizeof et + 4096);
    if (!html) return;
    snprintf(html, sizeof eu + sizeof et + 4096,
             "<html><head><title>%s</title></head><body bgcolor=\"#FFFFFF\">"
             "<table width=\"100%%\" cellpadding=\"8\"><tr><td bgcolor=\"#E8EEFA\">"
             "<h2><font color=\"#1B3F8F\">%s</font></h2></td></tr></table>"
             "<p><b>%s</b></p><p>Address: <code>%s</code>%s</p><ul>%s</ul>"
             "<hr><p><small>Web Browser for Halcyon</small></p></body></html>",
             title, head, et, eu, status ? "" : "", advice);
    show_html(html, url, 1);
    free(html);
    set_status("%s", text && text[0] ? text : web_strerror(err));
}

static const char home_html[] =
    "<html><head><title>Home</title></head><body bgcolor=\"#FFFFFF\">"
    "<table width=\"100%\" cellpadding=\"10\" cellspacing=\"0\"><tr><td bgcolor=\"#3A6EA5\">"
    "<h1><font color=\"#FFFFFF\">Welcome to the Web</font></h1>"
    "<font color=\"#DDE8F6\">Web Browser for Halcyon</font></td></tr></table>"
    "<p>Type an address in the <b>Address</b> bar above and press <b>Enter</b> or click <b>Go</b>. "
    "Addresses can be web pages (<code>http://</code>, <code>https://</code>) or files on this computer "
    "(<code>C:\\Documents\\Web\\index.htm</code>).</p>"
    "<h3>On this computer</h3><ul>"
    "<li><a href=\"file:///C:/Documents/Web/index.htm\">Sample pages</a> - text, lists, tables, images and forms</li>"
    "<li><a href=\"file:///C:/Documents/\">My Documents</a> (a folder listing)</li>"
    "</ul><h3>On the Internet</h3><ul>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> - the first web site</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a></li>"
    "<li><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo</a> (search without JavaScript)</li>"
    "<li><a href=\"http://example.com/\">example.com</a></li>"
    "</ul><h3>Good to know</h3><ul>"
    "<li>Pages are shown simply: text, links, headings, lists, tables, forms and PNG, GIF and BMP images. "
    "There is no JavaScript and almost no CSS.</li>"
    "<li>Secure sites need TLS 1.2. Certificates are always checked.</li>"
    "<li>Keys: Alt+Left / Backspace go back, F5 reloads, Esc stops, Ctrl+L goes to the address bar.</li>"
    "</ul></body></html>";

/* ---------------- loading ---------------- */
static void on_progress(void *ctx, const char *phase, long long got, long long total) {
    mutex_lock(&prog_lock);
    strlcpy(prog_phase, phase, sizeof prog_phase);
    prog_got = got;
    prog_total = total;
    mutex_unlock(&prog_lock);
}

static void stop_loading(void) {
    if (job) { web_job_free(job); job = 0; }
    job_kind = 0;
    job_img = -1;
    free(job_post);
    job_post = 0;
    loading = 0;
    win_set_timer(win, 0);
    win_set_cursor(win, CUR_ARROW);
    cursor_shape = -1;
    update_buttons();
}

static int start_job(const char *url, const char *method, const char *body, int kind) {
    web_request_t rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.method = method;
    if (body) {
        rq.body = body;
        rq.body_len = strlen(body);
        rq.content_type = "application/x-www-form-urlencoded";
    }
    rq.referer = kind == 3 || kind == 2 ? cur_url : 0;
    rq.accept = kind == 3 ? "image/png,image/gif,image/bmp,*/*;q=0.5" : "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8";
    rq.max_body = kind == 3 ? IMAGE_MAX_BYTES : PAGE_MAX;
    rq.on_progress = kind == 3 ? 0 : on_progress;
    if (strncmp(rq.referer ? rq.referer : "", "http", 4)) rq.referer = 0;
    job = web_job_start(sess, &rq);
    if (!job) return -1;
    job_kind = kind;
    win_set_timer(win, 100);
    return 0;
}

/* file:///C:/x/y.htm -> C:/x/y.htm */
static int file_path(const char *url, char *out, size_t n) {
    web_url_t u;
    if (url_parse(url, &u) || strcmp(u.scheme, "file")) return -1;
    char dec[URL_MAX];
    url_decode(u.path, strlen(u.path), dec, sizeof dec, 0);
    const char *p = dec;
    if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') p++;
    strlcpy(out, p, n);
    return 0;
}

static const char *ext_type(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *e = dot ? dot + 1 : "";
    if (!strcasecmp(e, "htm") || !strcasecmp(e, "html") || !strcasecmp(e, "xhtml")) return "text/html";
    if (!strcasecmp(e, "png")) return "image/png";
    if (!strcasecmp(e, "gif")) return "image/gif";
    if (!strcasecmp(e, "bmp")) return "image/bmp";
    if (!strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg")) return "image/jpeg";
    static const char *text[] = { "txt", "log", "ini", "cfg", "c", "h", "md", "csv", "json", "xml", "css", "js", "py", "sh", "bat", "inf", "nfo" };
    for (int i = 0; i < N(text); i++) if (!strcasecmp(e, text[i])) return "text/plain";
    return "application/octet-stream";
}

static int is_html_type(const char *t) { return !strcmp(t, "text/html") || !strcmp(t, "application/xhtml+xml"); }

/* sniff a type for bytes without one */
static const char *sniff(const uint8_t *b, size_t n) {
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P') return "image/png";
    if (n >= 6 && !memcmp(b, "GIF8", 4)) return "image/gif";
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return "image/bmp";
    size_t k = 0;
    while (k < n && k < 512 && (b[k] == ' ' || b[k] == '\n' || b[k] == '\r' || b[k] == '\t')) k++;
    if (k < n && b[k] == '<') return "text/html";
    for (size_t i = 0; i < n && i < 1024; i++) if (b[i] == 0) return "application/octet-stream";
    return "text/plain";
}

static void save_bytes(const uint8_t *b, size_t n, const char *suggest) {
    /* the dialog has no default name: the title suggests one */
    char title[120], out[520];
    snprintf(title, sizeof title, "Save \"%.60s\" As", suggest && suggest[0] ? suggest : "download");
    if (!file_dialog(win, 1, title, "C:/Documents", 0, out, sizeof out)) { set_status("Download canceled"); return; }
    int r = write_file(out, b, (long)n);
    if (r) {
        char msg[700];
        snprintf(msg, sizeof msg, "Cannot save %s.\n\n%s", out, hal_strerror(r));
        msgbox(win, "Web Browser", msg, MBX_ICON_ERROR, MBX_OK);
    } else set_status("Saved %s (%lu bytes)", out, (unsigned long)n);
}

static void image_page(const uint8_t *b, size_t n, const char *url, const char *ctype) {
    char html[URL_MAX * 2 + 512], eu[URL_MAX * 2];
    html_escape(url, eu, sizeof eu);
    const char *slash = strrchr(url, '/');
    snprintf(html, sizeof html, "<html><head><title>%s</title></head><body><img src=\"%s\" alt=\"%s (this image format cannot be shown)\"></body></html>",
             slash ? slash + 1 : url, eu, ctype);
    set_source(b, n, ctype, "");
    page_error = 0;
    doc_t *d = html_parse(html, strlen(html), "utf-8", url);
    show_doc(d, url);
}

/* bytes of a page arrived: show them by their type */
static void show_bytes(const uint8_t *b, size_t n, const char *url, const char *ctype_in, const char *charset, const char *filename) {
    const char *ctype = ctype_in && ctype_in[0] ? ctype_in : sniff(b, n);
    if (!strcmp(ctype, "application/octet-stream") && !filename) {
        const char *s = sniff(b, n);
        if (strcmp(s, "text/plain")) ctype = s;
    }
    page_error = 0;
    if (is_html_type(ctype)) {
        set_source(b, n, ctype, charset);
        doc_t *d = html_parse((const char *)b, n, charset, url);
        if (!d) { error_page(url, E_NOMEM, "The page is too large for the memory available.", 0); return; }
        show_doc(d, url);
    } else if (!strncmp(ctype, "text/", 5) || !strcmp(ctype, "application/json") || !strcmp(ctype, "application/javascript") ||
               !strcmp(ctype, "application/xml")) {
        set_source(b, n, ctype, charset);
        doc_t *d = text_doc((const char *)b, n, charset, url);
        if (d) { const char *sl = strrchr(url, '/'); d->title = arena_strndup(&d->ar, sl && sl[1] ? sl + 1 : url, strlen(sl && sl[1] ? sl + 1 : url)); }
        show_doc(d, url);
    } else if (!strncmp(ctype, "image/", 6)) {
        image_page(b, n, url, ctype);
    } else {
        /* not something to show: save it */
        const char *sl = strrchr(url, '/');
        save_bytes(b, n, filename && filename[0] ? filename : sl ? sl + 1 : "download");
    }
}

/* the folder listing for file: URLs of a directory */
static void dir_page(const char *path, const char *url) {
    hal_dirent_t *ents = malloc(sizeof(hal_dirent_t) * 512);
    if (!ents) return;
    int n = readdir(path, ents, 512);
    size_t cap = 4096 + (size_t)(n > 0 ? n : 0) * 700;
    char *html = malloc(cap);
    if (!html) { free(ents); return; }
    char ep[600];
    html_escape(path, ep, sizeof ep);
    size_t o = (size_t)snprintf(html, cap, "<html><head><title>%s</title></head><body><h2>Index of %s</h2><p><a href=\"..\">Up one level</a></p><table cellpadding=\"2\">", ep, ep);
    for (int i = 0; i < n && o + 800 < cap; i++) {
        char en[600], href[800];
        html_escape(ents[i].name, en, sizeof en);
        int k = 0;
        for (const char *p = ents[i].name; *p && k < 700; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || c == '.' || c == '-' || c == '_') href[k++] = (char)c;
            else k += snprintf(href + k, 8, "%%%02X", c);
        }
        href[k] = 0;
        char size[32] = "";
        if (!ents[i].is_dir) {
            unsigned long long b = ents[i].size;
            if (b < 10240) snprintf(size, sizeof size, "%llu bytes", b);
            else if (b < 10ULL << 20) snprintf(size, sizeof size, "%llu KB", b >> 10);
            else snprintf(size, sizeof size, "%llu MB", b >> 20);
        }
        o += (size_t)snprintf(html + o, cap - o, "<tr><td><a href=\"%s%s\">%s%s</a></td><td align=\"right\">%s</td></tr>", href,
                              ents[i].is_dir ? "/" : "", en, ents[i].is_dir ? "/" : "", size);
    }
    snprintf(html + o, cap - o, "</table></body></html>");
    show_html(html, url, 0);
    free(html);
    free(ents);
}

static void base64_decode(const char *s, size_t n, uint8_t **out, size_t *outn) {
    uint8_t *o = malloc(n / 4 * 3 + 4);
    *out = o;
    *outn = 0;
    if (!o) return;
    unsigned v = 0;
    int bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = v << 6 | (unsigned)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(v >> bits); }
    }
    *outn = k;
}

/* data:[type][;base64],data */
static int data_url(const char *url, uint8_t **b, size_t *n, char *ctype, size_t ctn, char *cs, size_t csn) {
    const char *p = url + 5, *comma = strchr(p, ',');
    if (!comma) return -1;
    char meta[200];
    size_t ml = (size_t)(comma - p) < sizeof meta - 1 ? (size_t)(comma - p) : sizeof meta - 1;
    memcpy(meta, p, ml);
    meta[ml] = 0;
    int b64 = strstr(meta, ";base64") != 0;
    char *semi = strchr(meta, ';');
    if (semi) *semi = 0;
    strlcpy(ctype, meta[0] ? meta : "text/plain", ctn);
    for (char *q = ctype; *q; q++) *q = (char)tolower((unsigned char)*q);
    const char *c = strstr(semi ? semi + 1 : "", "charset=");
    strlcpy(cs, c ? c + 8 : "", csn);
    size_t dl = strlen(comma + 1);
    if (b64) {
        char *dec = malloc(dl + 1);
        if (!dec) return -1;
        int l = url_decode(comma + 1, dl, dec, dl + 1, 0);
        base64_decode(dec, (size_t)l, b, n);
        free(dec);
    } else {
        *b = malloc(dl + 1);
        if (!*b) return -1;
        *n = (size_t)url_decode(comma + 1, dl, (char *)*b, dl + 1, 0);
    }
    return *b ? 0 : -1;
}

/* go somewhere: hist 0 = a new entry, 1 = replace the current one (reload), 2 = moving in the history */
static void navigate(const char *url_in, const char *post, int hmode) {
    char url[URL_MAX];
    if (url_from_user(url_in, url, sizeof url)) {
        char eu[URL_MAX];
        strlcpy(eu, url_in, sizeof eu);
        error_page(eu, WEB_E_URL, "This is not a valid address.", 0);
        return;
    }
    hist_save_current();
    stop_loading();
    refresh_at = 0;
    char sch[16];
    url_scheme_of(url, sch, sizeof sch);
    /* the same page with another fragment: just scroll */
    const char *h1 = strchr(url, '#'), *h2 = strchr(cur_url, '#');
    size_t l1 = h1 ? (size_t)(h1 - url) : strlen(url), l2 = h2 ? (size_t)(h2 - cur_url) : strlen(cur_url);
    if (doc && h1 && !post && hmode == 0 && l1 == l2 && !strncmp(url, cur_url, l1)) {
        char nm[256];
        url_decode(h1 + 1, strlen(h1 + 1), nm, sizeof nm, 0);
        int y = anchor_y(doc, nm);
        strlcpy(cur_url, url, sizeof cur_url);
        set_address(cur_url);
        hist_record(0, 0);
        scroll_y = y >= 0 ? y : 0;
        dirty = 1;
        update_buttons();
        return;
    }
    secure = 0;
    tls_text[0] = 0;
    set_status("%s", "");
    if (!strcmp(sch, "about")) {
        if (!strcmp(url, "about:blank")) show_html("<html><head><title>about:blank</title></head><body></body></html>", url, 0);
        else show_html(home_html, url, 0);
        hist_record(hmode, 0);
        update_buttons();
        return;
    }
    if (!strcmp(sch, "javascript")) { set_status("JavaScript is not supported"); return; }
    if (!strcmp(sch, "mailto")) { set_status("This browser cannot send e-mail (%.200s)", url); return; }
    if (!strcmp(sch, "data")) {
        uint8_t *b = 0;
        size_t n = 0;
        char ct[64], cs[32];
        if (data_url(url, &b, &n, ct, sizeof ct, cs, sizeof cs)) { error_page(url, WEB_E_URL, "A bad data: address.", 0); return; }
        show_bytes(b, n, url, ct, cs, 0);
        free(b);
        hist_record(hmode, 0);
        update_buttons();
        return;
    }
    if (!strcmp(sch, "file")) {
        char path[URL_MAX];
        if (file_path(url, path, sizeof path)) { error_page(url, WEB_E_URL, 0, 0); return; }
        hal_dirent_t st;
        if (stat(path, &st) == 0 && st.is_dir) {
            dir_page(path, url);
            hist_record(hmode, 0);
            update_buttons();
            return;
        }
        if (stat(path, &st) == 0 && st.size > PAGE_MAX) { error_page(url, WEB_E_TOOBIG, "The file is too large to show.", 0); return; }
        char *data;
        long n;
        int r = read_file(path, &data, &n);
        if (r) { error_page(url, r, r == E_NOENT ? "There is no such file." : hal_strerror(r), 0); hist_record(hmode, 0); update_buttons(); return; }
        show_bytes((uint8_t *)data, (size_t)n, url, ext_type(path), "", 0);
        free(data);
        hist_record(hmode, 0);
        update_buttons();
        return;
    }
    if (strcmp(sch, "http") && strcmp(sch, "https")) {
        error_page(url, WEB_E_SCHEME, "This kind of address is not supported.", 0);
        return;
    }
    /* the network: in a worker thread */
    strlcpy(job_url, url, sizeof job_url);
    job_hist = hmode;
    job_post = post ? strdup(post) : 0;
    if (start_job(url, post ? "POST" : "GET", post, 1)) { error_page(url, E_NOMEM, "Cannot start the request.", 0); return; }
    loading = 1;
    strlcpy(prog_phase, "Opening the page", sizeof prog_phase);
    set_status("Opening %.250s", url);
    set_address(url);
    update_buttons();
    win_set_cursor(win, CUR_WAIT);
    cursor_shape = CUR_WAIT;
}

static void page_done(web_response_t *rs) {
    char *post = job_post;
    job_post = 0;
    int hmode = job_hist;
    loading = 0;
    win_set_cursor(win, CUR_ARROW);
    cursor_shape = -1;
    if (rs->error) {
        secure = 0;
        strlcpy(cur_url, job_url, sizeof cur_url);
        error_page(job_url, rs->error, rs->error_text, rs->status);
        hist_record(hmode, post);
    } else {
        secure = rs->tls;
        strlcpy(tls_text, rs->tls_info, sizeof tls_text);
        int attach = 0;
        char cd[200];
        if (web_header(rs, "content-disposition", cd, sizeof cd) > 0 && !ncase(cd, "attachment", 10)) attach = 1;
        if (attach) {
            const char *sl = strrchr(rs->url, '/');
            save_bytes(rs->body, rs->body_len, rs->filename[0] ? rs->filename : sl ? sl + 1 : "download");
            secure = 0;
        } else {
            show_bytes(rs->body, rs->body_len, rs->url, rs->content_type, rs->charset, 0);
            hist_record(hmode, post);
            if (rs->status >= 400) set_status("%d %s", rs->status, rs->reason);
            else set_status("Done");
            if (doc && doc->refresh_url && refresh_count < 5) {
                char abs[URL_MAX];
                if (!url_resolve(doc->base, doc->refresh_url, abs, sizeof abs) && strcmp(abs, cur_url)) {
                    strlcpy(refresh_target, abs, sizeof refresh_target);
                    refresh_at = time_ms() + 1500;
                    refresh_count++;
                    win_set_timer(win, 200);
                }
            }
        }
    }
    free(post);
    update_buttons();
    dirty = 1;
}

/* ---------------- images ---------------- */
static int next_image(void) {
    if (!doc) return -1;
    for (int i = 0; i < doc->nimages; i++) if (doc->images[i].state == 0) return i;
    return -1;
}

static void image_bytes(image_t *im, const uint8_t *b, size_t n) {
    int w, h;
    uint32_t *px;
    if (b && n && image_decode(b, n, &w, &h, &px) == 0) { im->w = w; im->h = h; im->px = px; im->state = 2; }
    else im->state = 3;
}

static void start_images(void) {
    int changed = 0;
    while (!job) {
        int i = next_image();
        if (i < 0) break;
        changed = 1;
        image_t *im = &doc->images[i];
        char sch[16];
        url_scheme_of(im->url, sch, sizeof sch);
        if (!strcmp(sch, "file")) {
            char path[URL_MAX];
            char *data = 0;
            long n = 0;
            hal_dirent_t st;
            if (!file_path(im->url, path, sizeof path) && stat(path, &st) == 0 && st.size <= IMAGE_MAX_BYTES && !read_file(path, &data, &n))
                image_bytes(im, (uint8_t *)data, (size_t)n);
            else im->state = 3;
            free(data);
            continue;
        }
        if (!strcmp(sch, "data")) {
            uint8_t *b = 0;
            size_t n = 0;
            char ct[64], cs[32];
            if (!data_url(im->url, &b, &n, ct, sizeof ct, cs, sizeof cs)) image_bytes(im, b, n);
            else im->state = 3;
            free(b);
            continue;
        }
        if (strcmp(sch, "http") && strcmp(sch, "https")) { im->state = 3; continue; }
        /* a JPEG (or other format) by its name: not fetched, the alt text shows */
        const char *dot = strrchr(im->url, '.');
        if (dot && (!ncase(dot, ".jpg", 4) || !ncase(dot, ".jpeg", 5) || !ncase(dot, ".webp", 5) || !ncase(dot, ".svg", 4))) {
            im->state = 3;
            continue;
        }
        im->state = 1;
        job_img = i;
        if (start_job(im->url, "GET", 0, 3)) { im->state = 3; job_img = -1; continue; }
        loading = 1;
        update_buttons();
        break;
    }
    if (changed) relayout(1);
}

static void on_timer(void) {
    if (refresh_at && time_ms() >= refresh_at && !job) {
        refresh_at = 0;
        char t[URL_MAX];
        strlcpy(t, refresh_target, sizeof t);
        navigate(t, 0, 0);
        return;
    }
    if (!job) {
        if (!refresh_at) win_set_timer(win, 0);
        return;
    }
    if (job_kind == 1) {
        char ph[200];
        long long got, total;
        mutex_lock(&prog_lock);
        strlcpy(ph, prog_phase, sizeof ph);
        got = prog_got;
        total = prog_total;
        mutex_unlock(&prog_lock);
        if (!strcmp(ph, "Receiving")) {
            if (total > 0) set_status("Receiving %lld KB of %lld KB", got >> 10, total >> 10);
            else set_status("Receiving %lld KB", got >> 10);
        } else set_status("%s...", ph);
        update_status();
    }
    if (!web_job_done(job)) return;
    web_response_t *rs = web_job_response(job);
    int kind = job_kind;
    web_job_t *j = job;
    job = 0;
    job_kind = 0;
    if (kind == 1) {
        page_done(rs);
    } else if (kind == 2) {
        loading = 0;
        if (rs->error) {
            char msg[400];
            snprintf(msg, sizeof msg, "The download failed.\n\n%s", rs->error_text);
            msgbox(win, "Web Browser", msg, MBX_ICON_ERROR, MBX_OK);
        } else if (rs->status >= 400) {
            char msg[200];
            snprintf(msg, sizeof msg, "The server answered %d %s.", rs->status, rs->reason);
            msgbox(win, "Web Browser", msg, MBX_ICON_ERROR, MBX_OK);
        } else {
            const char *sl = strrchr(rs->url, '/');
            save_bytes(rs->body, rs->body_len, rs->filename[0] ? rs->filename : sl ? sl + 1 : "download");
        }
    } else if (kind == 3 && doc && job_img >= 0 && job_img < doc->nimages) {
        image_t *im = &doc->images[job_img];
        if (!rs->error && rs->status == 200) image_bytes(im, rs->body, rs->body_len);
        else im->state = 3;
        job_img = -1;
        loading = 0;
        relayout(1);
    }
    web_job_free(j);
    if (!job) start_images();
    if (!job) {
        loading = 0;
        if (!refresh_at) win_set_timer(win, 0);
        if (kind == 3) set_status("Done");
    }
    update_buttons();
    dirty = 1;
}

/* ---------------- history moves ---------------- */
static void go_hist(int to) {
    if (to < 0 || to >= nhist || !hist[to].url) return;
    hist_save_current();
    stop_loading();
    hpos = to;
    hist_t *h = &hist[to];
    char url[URL_MAX];
    strlcpy(url, h->url, sizeof url);
    if (h->body) {
        secure = h->secure;
        uint8_t *b = malloc(h->len);
        if (!b) return;
        memcpy(b, h->body, h->len);
        size_t n = h->len;
        char ct[64], cs[32];
        strlcpy(ct, h->ctype, sizeof ct);
        strlcpy(cs, h->charset, sizeof cs);
        int sc = h->scroll;
        show_bytes(b, n, url, ct, cs, 0);
        free(b);
        scroll_y = sc;
        set_status("Done");
        update_buttons();
        dirty = 1;
        return;
    }
    if (h->post && msgbox(win, "Web Browser", "This page was the answer to a form you sent.\n\nSend the form again?", MBX_ICON_QUESTION, MBX_YESNO) != IDR_YES)
        return;
    navigate(url, h->post, 2);
}

static void reload(void) {
    if (hpos >= 0 && hist[hpos].post &&
        msgbox(win, "Web Browser", "This page was the answer to a form you sent.\n\nSend the form again?", MBX_ICON_QUESTION, MBX_YESNO) != IDR_YES)
        return;
    char u[URL_MAX];
    strlcpy(u, cur_url, sizeof u);
    navigate(u, hpos >= 0 ? hist[hpos].post : 0, 1);
}

/* ---------------- forms ---------------- */
static int append_pair(char *out, size_t n, size_t *pos, const char *name, const char *value) {
    if (*pos) { if (*pos + 2 >= n) return -1; out[(*pos)++] = '&'; out[*pos] = 0; }
    int r = url_form_encode(name, strlen(name), out, n, *pos);
    if (r < 0) return -1;
    *pos = (size_t)r;
    if (*pos + 2 >= n) return -1;
    out[(*pos)++] = '=';
    out[*pos] = 0;
    r = url_form_encode(value, strlen(value), out, n, *pos);
    if (r < 0) return -1;
    *pos = (size_t)r;
    return 0;
}

static void submit(node_t *form, fctl_t *button) {
    if (!doc) return;
    size_t cap = 256 * 1024, pos = 0;
    char *data = malloc(cap);
    if (!data) return;
    data[0] = 0;
    for (int i = 0; i < doc->nctls; i++) {
        fctl_t *k = doc->ctls[i];
        if (k->form != form || !k->name || !k->name[0] || node_attr(k->node, "disabled")) continue;
        int r = 0;
        switch (k->kind) {
        case FC_TEXT: case FC_PASSWORD: case FC_HIDDEN: case FC_TEXTAREA: r = append_pair(data, cap, &pos, k->name, k->value); break;
        case FC_CHECKBOX: case FC_RADIO: if (k->checked) r = append_pair(data, cap, &pos, k->name, k->value); break;
        case FC_SELECT: if (k->noptions) r = append_pair(data, cap, &pos, k->name, k->opt_value[k->selected]); break;
        case FC_SUBMIT:
            if (k == button) r = append_pair(data, cap, &pos, k->name, k->node->tag == T_BUTTON && k->opt_value ? k->opt_value[0] : k->value);
            break;
        }
        if (r) break;
    }
    const char *action = form ? node_attr(form, "action") : 0;
    const char *method = form ? node_attr(form, "method") : 0;
    char abs[URL_MAX];
    if (url_resolve(doc->base, action && action[0] ? action : cur_url, abs, sizeof abs)) { free(data); return; }
    if (method && !strcasecmp(method, "post")) {
        char *h = strchr(abs, '#');
        if (h) *h = 0;
        navigate(abs, data, 0);
    } else {
        char *h = strchr(abs, '#');
        if (h) *h = 0;
        char *q = strchr(abs, '?');
        if (q) *q = 0;
        char *u = malloc(URL_MAX + 4);
        if (u) {
            if (snprintf(u, URL_MAX, "%s?%s", abs, data) < URL_MAX) navigate(u, 0, 0);
            else set_status("The form's data are too long to send this way");
            free(u);
        }
    }
    free(data);
}

static void form_reset(node_t *form) {
    for (int i = 0; i < doc->nctls; i++) {
        fctl_t *k = doc->ctls[i];
        if (k->form != form) continue;
        if (k->kind == FC_TEXT || k->kind == FC_PASSWORD) {
            const char *v = node_attr(k->node, "value");
            free(k->value);
            k->value = strdup(v ? v : "");
            k->vlen = k->value ? (int)strlen(k->value) : 0;
            k->vcap = k->vlen + 1;
            k->caret = k->vlen;
            k->scroll = 0;
        } else if (k->kind == FC_CHECKBOX || k->kind == FC_RADIO) k->checked = node_attr(k->node, "checked") != 0;
    }
    dirty = 1;
}

/* the page position of a control's item */
static item_t *ctl_item(fctl_t *k) {
    if (!doc) return 0;
    for (int i = 0; i < doc->nitems; i++) if (doc->items[i].kind == IT_CONTROL && doc->items[i].node->ctl == k) return &doc->items[i];
    return 0;
}

static void focus_ctl(fctl_t *k) {
    focus = k;
    if (k) {
        ctl_focus(win, 0);
        item_t *it = ctl_item(k);
        if (it) {
            if (it->y < scroll_y) scroll_y = it->y - 10;
            else if (it->y + it->h > scroll_y + view_h()) scroll_y = it->y + it->h - view_h() + 10;
        }
    }
    dirty = 1;
}

static void text_scroll(fctl_t *k) {
    item_t *it = ctl_item(k);
    if (!it || k->kind == FC_TEXTAREA) return;
    int cx = text_width_n(k->value, k->caret, FONT_UI);
    int inner = it->w - 8;
    if (cx - k->scroll > inner) k->scroll = cx - inner;
    if (cx - k->scroll < 0) k->scroll = cx;
}

static int edit_insert(fctl_t *k, const char *s, int n) {
    if (k->vlen + n + 1 > k->vcap) {
        int nc = (k->vlen + n + 1) * 2;
        if (nc > (1 << 20)) return -1;
        char *v = realloc(k->value, (size_t)nc);
        if (!v) return -1;
        k->value = v;
        k->vcap = nc;
    }
    memmove(k->value + k->caret + n, k->value + k->caret, (size_t)(k->vlen - k->caret + 1));
    memcpy(k->value + k->caret, s, (size_t)n);
    k->vlen += n;
    k->caret += n;
    return 0;
}

static void edit_key(fctl_t *k, const hal_event_t *ev) {
    int text = ev->ch >= 32 && ev->ch != 127;
    switch (ev->id) {
    case KEY_LEFT: if (k->caret > 0) k->caret = utf8_prev(k->value, k->caret); break;
    case KEY_RIGHT: if (k->caret < k->vlen) { int p = k->caret; utf8_get(k->value, k->vlen, &p); k->caret = p; } break;
    case KEY_HOME: k->caret = 0; break;
    case KEY_END: k->caret = k->vlen; break;
    case KEY_BACKSPACE:
        if (k->caret > 0) {
            int p = utf8_prev(k->value, k->caret);
            memmove(k->value + p, k->value + k->caret, (size_t)(k->vlen - k->caret + 1));
            k->vlen -= k->caret - p;
            k->caret = p;
        }
        break;
    case KEY_DELETE:
        if (k->caret < k->vlen) {
            int p = k->caret;
            utf8_get(k->value, k->vlen, &p);
            memmove(k->value + k->caret, k->value + p, (size_t)(k->vlen - p + 1));
            k->vlen -= p - k->caret;
        }
        break;
    case KEY_ENTER: case KEY_KP_ENTER:
        if (k->kind == FC_TEXTAREA) { edit_insert(k, "\n", 1); break; }
        {
            /* implicit submission, with the form's first submit button */
            fctl_t *btn = 0;
            for (int i = 0; i < doc->nctls && !btn; i++) if (doc->ctls[i]->form == k->form && doc->ctls[i]->kind == FC_SUBMIT) btn = doc->ctls[i];
            submit(k->form, btn);
        }
        return;
    default:
        if (key_shortcut(ev, 'V')) {
            int n = clipboard_get(0, 0);
            if (n > 0 && n < 65536) {
                char *b = malloc((size_t)n + 1);
                if (b) {
                    int got = clipboard_get(b, n + 1);
                    for (int i = 0; i < got; i++) if (b[i] == '\n' || b[i] == '\r' || b[i] == '\t') b[i] = k->kind == FC_TEXTAREA && b[i] == '\n' ? '\n' : ' ';
                    edit_insert(k, b, got);
                    free(b);
                }
            }
        } else if (text && !(ev->mods & MOD_CTRL && !(ev->mods & MOD_ALT))) {
            char u[4];
            edit_insert(k, u, utf8_put(ev->ch, u));
        } else return;
    }
    text_scroll(k);
    dirty = 1;
}

/* ---------------- hit testing ---------------- */
static item_t *hit(int x, int y) {
    if (!doc || y < TOP || y >= TOP + view_h() || x >= view_w()) return 0;
    int py = y - TOP + scroll_y;
    for (int i = doc->nitems - 1; i >= 0; i--) {
        item_t *it = &doc->items[i];
        if (!it->link && it->kind != IT_CONTROL) continue;
        if (x >= it->x && x < it->x + it->w && py >= it->y && py < it->y + it->h) return it;
    }
    return 0;
}

static const char *link_href(node_t *l) {
    const char *h = node_attr(l, "href");
    return h ? h : node_attr(l, "src");
}

static void follow(node_t *l) {
    const char *h = link_href(l);
    if (!h) return;
    char abs[URL_MAX];
    if (url_resolve(doc->base, h, abs, sizeof abs)) { set_status("A bad link: %.200s", h); dirty = 1; return; }
    navigate(abs, 0, 0);
}

static void set_cursor(int c) { if (c != cursor_shape) { cursor_shape = c; win_set_cursor(win, c); } }

static void click_control(fctl_t *k, int x) {
    switch (k->kind) {
    case FC_TEXT: case FC_PASSWORD: case FC_TEXTAREA: {
        focus_ctl(k);
        item_t *it = ctl_item(k);
        if (it && k->kind != FC_TEXTAREA) {
            int rel = x - it->x - 4 + k->scroll;
            k->caret = text_fit(k->value, k->vlen, FONT_UI, rel > 0 ? rel : 0);
        }
        break;
    }
    case FC_CHECKBOX: k->checked = !k->checked; focus = 0; dirty = 1; break;
    case FC_RADIO:
        for (int i = 0; i < doc->nctls; i++) {
            fctl_t *o = doc->ctls[i];
            if (o->kind == FC_RADIO && o->form == k->form && o->name && k->name && !strcmp(o->name, k->name)) o->checked = 0;
        }
        k->checked = 1;
        focus = 0;
        dirty = 1;
        break;
    case FC_SELECT: {
        if (!k->noptions) break;
        static hal_menu_item_t items[MENU_MAX_ITEMS];
        int n = k->noptions < MENU_MAX_ITEMS ? k->noptions : MENU_MAX_ITEMS;
        static char labels[MENU_MAX_ITEMS][64];
        for (int i = 0; i < n; i++) {
            /* '&' would be a mnemonic */
            int o = 0;
            for (const char *p = k->opt_label[i]; *p && o < 60; p++) { if (*p == '&') labels[i][o++] = '&'; labels[i][o++] = *p; }
            labels[i][o] = 0;
            items[i] = (hal_menu_item_t){ labels[i], 0, M_SELECT_BASE + i, i == k->selected ? MENU_CHECKED : 0, 0, 0 };
        }
        item_t *it = ctl_item(k);
        popup_ctl = k;
        if (it) menu_popup(win, items, n, it->x, it->y + it->h - scroll_y + TOP);
        break;
    }
    case FC_SUBMIT: submit(k->form, k); break;
    case FC_RESET: form_reset(k->form); break;
    default: break;
    }
}

/* ---------------- commands ---------------- */
static void about(void) {
    msgbox(win, "About Web Browser",
           "Web Browser for Halcyon\n\nHTML, HTTP/1.1 and HTTPS (TLS 1.2 through BearSSL 0.6), "
           "PNG, GIF and BMP images, forms. No JavaScript.\n\nTrusted certificates: Mozilla's CA list.",
           MBX_ICON_INFO, MBX_OK);
}

static void save_page(void) {
    if (!src || !srclen) { msgbox(win, "Web Browser", "There is nothing to save.", MBX_ICON_INFO, MBX_OK); return; }
    save_bytes(src, srclen, "page.htm");
}

static void view_source(void) {
    if (!src || !srclen) return;
    char u[URL_MAX];
    snprintf(u, sizeof u, "%s", cur_url);
    uint8_t *b = malloc(srclen);
    if (!b) return;
    memcpy(b, src, srclen);
    size_t n = srclen;
    char cs[32];
    strlcpy(cs, src_charset, sizeof cs);
    hist_save_current();
    doc_t *d = text_doc((const char *)b, n, cs, u);
    if (d) d->title = arena_strndup(&d->ar, "Source", 6);
    set_source(b, n, "text/plain", cs);
    free(b);
    page_error = 0;
    show_doc(d, u);
    hist_record(0, 0);
    update_buttons();
}

static void command(int id) {
    switch (id) {
    case M_OPEN: {
        char out[520];
        if (file_dialog(win, 0, "Open", "C:/Documents", 0, out, sizeof out)) navigate(out, 0, 0);
        break;
    }
    case M_SAVE: save_page(); break;
    case M_EXIT: exit(0);
    case M_COPYURL: clipboard_set_n(cur_url, (int)strlen(cur_url)); break;
    case M_STOP: case C_STOP:
        if (loading) {
            int was_page = job_kind == 1;
            stop_loading();
            if (was_page) set_status("Stopped");
            else set_status("Done");
            dirty = 1;
        }
        break;
    case M_RELOAD: case C_RELOAD: reload(); break;
    case M_SOURCE: view_source(); break;
    case M_BACK: case C_BACK: go_hist(hpos - 1); break;
    case M_FWD: case C_FWD: go_hist(hpos + 1); break;
    case M_HOME: case C_HOME: navigate(HOME_URL, 0, 0); break;
    case M_ABOUT: about(); break;
    case M_HELP: help_open("browser"); break;
    case C_GO: {
        char t[URL_MAX];
        ctl_get_text(win, C_ADDR, t, sizeof t);
        /* the box holds at most 255 bytes: a longer current address is kept as it is */
        if (strlen(t) >= 255 && !strncmp(cur_url, t, strlen(t))) strlcpy(t, cur_url, sizeof t);
        if (t[0]) navigate(t, 0, 0);
        ctl_focus(win, 0);
        break;
    }
    case M_LINK_OPEN: if (ctx_link) follow(ctx_link); break;
    case M_LINK_COPY:
        if (ctx_link && link_href(ctx_link)) {
            char abs[URL_MAX];
            if (!url_resolve(doc->base, link_href(ctx_link), abs, sizeof abs)) clipboard_set_n(abs, (int)strlen(abs));
        }
        break;
    case M_LINK_SAVE:
        if (ctx_link && link_href(ctx_link) && !job) {
            char abs[URL_MAX];
            if (!url_resolve(doc->base, link_href(ctx_link), abs, sizeof abs)) {
                if (!strncmp(abs, "http", 4)) {
                    if (!start_job(abs, "GET", 0, 2)) { loading = 1; set_status("Downloading %.250s", abs); update_buttons(); }
                } else if (!strncmp(abs, "file:", 5)) {
                    char path[URL_MAX];
                    char *data;
                    long n;
                    if (!file_path(abs, path, sizeof path) && !read_file(path, &data, &n)) { save_bytes((uint8_t *)data, (size_t)n, 0); free(data); }
                }
            }
        }
        break;
    default:
        if (id >= M_SELECT_BASE && popup_ctl && id - M_SELECT_BASE < popup_ctl->noptions) {
            popup_ctl->selected = id - M_SELECT_BASE;
            popup_ctl = 0;
            dirty = 1;
        }
        break;
    }
}

static void layout_controls(void) {
    int x = 4;
    static const struct { int id; const char *label; int icon; const char *tip; int w; } tb[] = {
        { C_BACK, "Back", HICON_BACK, "Back (Alt+Left)", 64 },
        { C_FWD, "Forward", HICON_FORWARD, "Forward (Alt+Right)", 78 },
        { C_STOP, "Stop", HICON_STOP, "Stop (Esc)", 58 },
        { C_RELOAD, "Reload", HICON_RELOAD, "Reload (F5)", 70 },
        { C_HOME, "Home", HICON_HOME, "Home page", 62 },
    };
    for (int i = 0; i < N(tb); i++) { ctl_move(win, tb[i].id, x, 3, tb[i].w, TOOL_H - 6); x += tb[i].w + 2; }
    ctl_move(win, C_ADDR_LABEL, 6, TOOL_H + 6, 52, 16);
    int gw = 44;
    ctl_move(win, C_ADDR, 60, TOOL_H + 3, cw - 60 - gw - 10, ADDR_H - 6);
    ctl_move(win, C_GO, cw - gw - 4, TOOL_H + 3, gw, ADDR_H - 6);
    ctl_move(win, C_VSCROLL, cw - SB_W, TOP, SB_W, view_h());
}

static void create_controls(void) {
    static const struct { int id; const char *label; int icon; const char *tip; } tb[] = {
        { C_BACK, "Back", HICON_BACK, "Back (Alt+Left)" },
        { C_FWD, "Forward", HICON_FORWARD, "Forward (Alt+Right)" },
        { C_STOP, "Stop", HICON_STOP, "Stop (Esc)" },
        { C_RELOAD, "Reload", HICON_RELOAD, "Reload (F5)" },
        { C_HOME, "Home", HICON_HOME, "Home page" },
    };
    for (int i = 0; i < N(tb); i++) {
        ctl_create(win, CTL_TOOLBUTTON, 0, 0, 60, TOOL_H - 6, tb[i].label, tb[i].id, 0);
        ctl_set_tool(win, tb[i].id, tb[i].icon, tb[i].tip);
    }
    ctl_create(win, CTL_LABEL, 0, 0, 50, 16, "Address", C_ADDR_LABEL, 0);
    ctl_create(win, CTL_TEXTBOX, 0, 0, 200, ADDR_H - 6, "", C_ADDR, CTL_NOTIFY);
    ctl_create(win, CTL_BUTTON, 0, 0, 44, ADDR_H - 6, "Go", C_GO, 0);
    ctl_set_tool(win, C_GO, -1, "Go to the address");
    ctl_create(win, CTL_VSCROLL, 0, 0, SB_W, 100, 0, C_VSCROLL, 0);
    ctl_create(win, CTL_STATUSBAR, 0, 0, 0, STATUS_H, "", C_STATUS, 0);
    ctl_set_value(win, C_STATUS, 130);
    layout_controls();
}

static void scroll_by(int dy) {
    scroll_y += dy;
    int maxs = doc && doc->height > view_h() ? doc->height - view_h() : 0;
    if (scroll_y > maxs) scroll_y = maxs;
    if (scroll_y < 0) scroll_y = 0;
    dirty = 1;
}

static void focus_next(int back) {
    if (!doc || !doc->nctls) return;
    int start = -1;
    for (int i = 0; i < doc->nctls; i++) if (doc->ctls[i] == focus) start = i;
    for (int s = 1; s <= doc->nctls; s++) {
        int i = ((start + (back ? -s : s)) % doc->nctls + doc->nctls) % doc->nctls;
        fctl_t *k = doc->ctls[i];
        if ((k->kind == FC_TEXT || k->kind == FC_PASSWORD || k->kind == FC_TEXTAREA) && ctl_item(k)) { focus_ctl(k); k->caret = k->vlen; return; }
    }
}

static void on_key(const hal_event_t *ev) {
    if (!ev->pressed) return;
    int alt = ev->mods & MOD_ALT, shift = ev->mods & MOD_SHIFT;
    if (key_shortcut(ev, 'L')) { ctl_focus(win, C_ADDR); return; }
    if (key_shortcut(ev, 'O')) { command(M_OPEN); return; }
    if (key_shortcut(ev, 'S')) { command(M_SAVE); return; }
    if (key_shortcut(ev, 'R')) { command(M_RELOAD); return; }
    if (alt && ev->id == KEY_LEFT) { command(M_BACK); return; }
    if (alt && ev->id == KEY_RIGHT) { command(M_FWD); return; }
    if (alt && ev->id == KEY_HOME) { command(M_HOME); return; }
    if (ev->id == KEY_F5) { command(M_RELOAD); return; }
    if (ev->id == KEY_ESC) { if (loading) command(M_STOP); else if (focus) { focus = 0; dirty = 1; } return; }
    if (ev->id == KEY_F1 + 5) { ctl_focus(win, C_ADDR); return; }             /* F6 */
    if (ev->id == KEY_F1) { command(M_HELP); return; }
    if (ev->id == KEY_TAB) { focus_next(shift); return; }
    if (focus) { edit_key(focus, ev); return; }
    int vh = view_h();
    switch (ev->id) {
    case KEY_UP: scroll_by(-LINE_STEP); break;
    case KEY_DOWN: scroll_by(LINE_STEP); break;
    case KEY_PAGEUP: scroll_by(-(vh - LINE_STEP)); break;
    case KEY_PAGEDOWN: scroll_by(vh - LINE_STEP); break;
    case KEY_SPACE: scroll_by(shift ? -(vh - LINE_STEP) : vh - LINE_STEP); break;
    case KEY_HOME: scroll_by(-(1 << 28)); break;
    case KEY_END: scroll_by(1 << 28); break;
    case KEY_BACKSPACE: command(shift ? M_FWD : M_BACK); break;
    }
}

static void on_mouse(const hal_event_t *ev) {
    item_t *it = hit(ev->x, ev->y);
    if (ev->type == EVT_MOUSE_MOVE) {
        node_t *l = it ? it->link : 0;
        if (l != hover_link) { hover_link = l; update_status(); }
        int shape = loading && job_kind == 1 ? CUR_WAIT : l ? CUR_HAND :
                    it && it->kind == IT_CONTROL && it->node->ctl &&
                    (it->node->ctl->kind == FC_TEXT || it->node->ctl->kind == FC_PASSWORD || it->node->ctl->kind == FC_TEXTAREA) ? CUR_IBEAM : CUR_ARROW;
        set_cursor(shape);
        return;
    }
    if (ev->type != EVT_MOUSE_DOWN) return;
    if (ev->y < TOP || ev->y >= TOP + view_h() || ev->x >= view_w()) return;
    if (ev->button == MOUSE_RIGHT) {
        ctx_link = it ? it->link : 0;
        static const hal_menu_item_t link_menu[] = {
            { "&Open Link", 0, M_LINK_OPEN, MENU_BOLD, 0, 0 },
            { "&Copy Link Address", 0, M_LINK_COPY, 0, 0, 0 },
            { "Save Link &As...", 0, M_LINK_SAVE, 0, 0, 0 },
        };
        static const hal_menu_item_t page_menu[] = {
            { "&Back", 0, M_BACK, 0, HICON_BACK, 0 },
            { "&Forward", 0, M_FWD, 0, HICON_FORWARD, 0 },
            { "&Reload", 0, M_RELOAD, 0, HICON_RELOAD, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "&Save Page As...", 0, M_SAVE, 0, 0, 0 },
            { "Page S&ource", 0, M_SOURCE, 0, 0, 0 },
        };
        if (ctx_link) menu_popup(win, link_menu, N(link_menu), ev->x, ev->y);
        else menu_popup(win, page_menu, N(page_menu), ev->x, ev->y);
        return;
    }
    if (ev->button != MOUSE_LEFT) return;
    ctl_focus(win, 0);
    if (it && it->kind == IT_CONTROL && it->node->ctl) { click_control(it->node->ctl, ev->x); return; }
    if (focus) { focus = 0; dirty = 1; }
    if (it && it->link) follow(it->link);
}

static void handle(const hal_event_t *ev) {
    switch (ev->type) {
    case EVT_PAINT: dirty = 1; break;
    case EVT_CLOSE: exit(0);
    case EVT_RESIZE:
        cw = ev->x;
        ch = ev->y;
        win_canvas(win, &cv);
        layout_controls();
        relayout(0);
        break;
    case EVT_COMMAND:
        if (ev->id == C_ADDR && ev->button == CMD_ACTIVATE) command(C_GO);
        else if (ev->id != C_ADDR) command(ev->id);
        break;
    case EVT_MENU: command(ev->id); break;
    case EVT_SCROLL: if (ev->id == C_VSCROLL) { scroll_y = ev->x; dirty = 1; } break;
    case EVT_WHEEL: scroll_by(-ev->dz * 3 * 20); break;
    case EVT_KEY: on_key(ev); break;
    case EVT_TIMER: on_timer(); break;
    case EVT_MOUSE_DOWN: case EVT_MOUSE_MOVE: case EVT_DBLCLICK: on_mouse(ev); break;
    case EVT_MOUSE_LEAVE: if (hover_link) { hover_link = 0; update_status(); } set_cursor(CUR_ARROW); break;
    }
}

int main(int argc, char **argv) {
    g_metrics.text_width = m_width;
    g_metrics.font_height = m_height;
    g_metrics.font_ascent = m_ascent;
    g_metrics.text_fit = m_fit;
    sess = web_session_new();
    if (!sess) return 1;
    win = win_create("Web Browser", cw, ch, WIN_RESIZABLE);
    if (win < 0) return 1;
    win_set_icon(win, HICON_WEB);
    win_set_min_size(win, 420, 240);
    menu_set(win, bar, N(bar));
    win_size(win, &cw, &ch);
    win_canvas(win, &cv);
    create_controls();
    ctl_focus(win, 0);
    char start[URL_MAX] = HOME_URL;
    if (argc > 1) {
        /* one quoted argument from the shell (a file or an address) */
        start[0] = 0;
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(start, " ", sizeof start);
            strlcat(start, argv[i], sizeof start);
        }
    }
    navigate(start, 0, 0);
    for (;;) {
        hal_event_t ev;
        if (dirty) {
            int n = 0;
            uint64_t t0 = time_ms();
            while (n < 64 && time_ms() - t0 < 30 && win_get_event(&ev, 0)) { handle(&ev); n++; }
            if (dirty) paint();
            continue;
        }
        if (win_get_event(&ev, -1)) handle(&ev);
    }
}
