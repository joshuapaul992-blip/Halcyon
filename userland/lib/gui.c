/* Halcyon user-mode GUI calls (window system in the kernel, like Win32's
 * user32 / GDI): thin wrappers over the system calls */
#include "hal.h"

int  win_create(const char *t, int w, int h, int flags) { return (int)SC4(SYS_WIN_CREATE, t, w, h, flags); }
void win_destroy(int win) { SC1(SYS_WIN_DESTROY, win); }
void win_set_title(int win, const char *t) { SC2(SYS_WIN_SET_TITLE, win, t); }
void win_invalidate(int win) { SC1(SYS_WIN_INVALIDATE, win); }
/* events of other windows that arrived while a modal helper (inputbox) waited */
#define DEFER_MAX 64
static hal_event_t deferred[DEFER_MAX];
static int ndeferred;

static void defer(const hal_event_t *ev) {
    if (ev->type == EVT_MOUSE_MOVE || ev->type == EVT_TIMER) return;      /* transient; timers come again */
    if (ev->type == EVT_PAINT || ev->type == EVT_RESIZE)
        for (int i = 0; i < ndeferred; i++)
            if (deferred[i].type == ev->type && deferred[i].win == ev->win) { deferred[i] = *ev; return; }
    if (ndeferred < DEFER_MAX) deferred[ndeferred++] = *ev;
}

int  win_get_event(hal_event_t *ev, int timeout_ms) {
    if (ndeferred) {
        *ev = deferred[0];
        memmove(deferred, deferred + 1, sizeof deferred[0] * (size_t)--ndeferred);
        return 1;
    }
    return (int)SC2(SYS_WIN_GET_EVENT, ev, timeout_ms);
}
void win_size(int win, int *w, int *h) {
    long r = SC1(SYS_WIN_SIZE, win);
    if (r < 0) r = 0;
    if (w) *w = (int)(r >> 16);
    if (h) *h = (int)(r & 0xFFFF);
}
void win_set_timer(int win, int ms) { SC2(SYS_WIN_SET_TIMER, win, ms); }
void win_resize(int win, int w, int h) { SC3(SYS_WIN_RESIZE, win, w, h); }
void win_set_icon(int win, int icon) { SC2(SYS_WIN_SET_ICON, win, icon); }

void gfx_fill(int win, int x, int y, int w, int h, unsigned c) { SC6(SYS_GFX_FILL, win, x, y, w, h, c); }
void gfx_rect(int win, int x, int y, int w, int h, unsigned c) { SC6(SYS_GFX_RECT, win, x, y, w, h, c); }
void gfx_line(int win, int x0, int y0, int x1, int y1, unsigned c) { SC6(SYS_GFX_LINE, win, x0, y0, x1, y1, c); }
int  gfx_text(int win, int x, int y, const char *s, unsigned c, int font) { return (int)SC6(SYS_GFX_TEXT, win, x, y, s, c, font); }
int  gfx_text_width(const char *s, int font) { return (int)SC2(SYS_GFX_TEXT_WIDTH, s, font); }
int  gfx_font_height(int font) { return (int)SC1(SYS_GFX_FONT_HEIGHT, font); }
void gfx_blit(int win, int x, int y, int w, int h, const unsigned *px) { SC6(SYS_GFX_BLIT, win, x, y, w, h, px); }
void gfx_icon(int win, int x, int y, int icon) { SC4(SYS_GFX_ICON, win, x, y, icon); }
void gfx_bevel(int win, int x, int y, int w, int h, int raised) { SC6(SYS_GFX_BEVEL, win, x, y, w, h, raised); }
void gfx_circle(int win, int cx, int cy, int r, unsigned c, int filled) { SC6(SYS_GFX_CIRCLE, win, cx, cy, r, c, filled); }

int ctl_create(int win, int type, int x, int y, int w, int h, const char *text, int id, int style) {
    /* seven arguments: geometry packed into one */
    long geom = ((long)(x & 0xFFFF) << 48) | ((long)(y & 0xFFFF) << 32) | ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    return (int)SC6(SYS_CTL_CREATE, win, type, geom, text, id, style);
}
void ctl_set_text(int win, int id, const char *t) { SC3(SYS_CTL_SET_TEXT, win, id, t); }
int  ctl_get_text(int win, int id, char *buf, int n) { return (int)SC4(SYS_CTL_GET_TEXT, win, id, buf, n); }
void ctl_set_value(int win, int id, int v) { SC3(SYS_CTL_SET_VALUE, win, id, v); }
int  ctl_get_value(int win, int id) { return (int)SC2(SYS_CTL_GET_VALUE, win, id); }
void ctl_set_style(int win, int id, int s) { SC3(SYS_CTL_SET_STYLE, win, id, s); }
void ctl_list_set(int win, int id, const char *const *items, int n) { SC4(SYS_CTL_LIST_SET, win, id, items, n); }

int  msgbox(int win, const char *title, const char *text, int icon, int buttons) {
    return (int)SC5(SYS_MSGBOX, win, title, text, icon, buttons);
}
int  file_dialog(int win, int save, const char *title, const char *dir, const char *ext, char *out, int n) {
    char path[520];                       /* the kernel writes a whole path */
    if (SC6(SYS_FILE_DIALOG, win, save, title, dir, ext, path) != 1) return 0;
    strlcpy(out, path, (size_t)n);
    return 1;
}
void clipboard_set(const char *t) { SC1(SYS_CLIPBOARD_SET, t); }
int  clipboard_get(char *buf, int n) { return (int)SC2(SYS_CLIPBOARD_GET, buf, n); }
int  clipboard_set_n(const char *t, int len) { return (int)SC2(SYS_CLIPBOARD_SET_N, t, len); }

/* ---- menus ---- */
int  menu_set(int win, const hal_menu_item_t *items, int n) { return (int)SC3(SYS_MENU_SET, win, items, n); }
int  menu_popup(int win, const hal_menu_item_t *items, int n, int x, int y) { return (int)SC5(SYS_MENU_POPUP, win, items, n, x, y); }
int  menu_state(int win, int id, int mask, int flags) { return (int)SC4(SYS_MENU_STATE, win, id, mask, flags); }

/* ---- more window calls ---- */
void win_set_cursor(int win, int shape) { SC2(SYS_WIN_SET_CURSOR, win, shape); }
int  win_get_rect(int win, hal_rect_t *r) { return (int)SC2(SYS_WIN_GET_RECT, win, r); }
void win_set_min_size(int win, int w, int h) { SC3(SYS_WIN_SET_MIN_SIZE, win, w, h); }
int  win_set_owner(int win, int owner, int modal) { return (int)SC3(SYS_WIN_SET_OWNER, win, owner, modal); }

/* ---- clipping, text, shapes ---- */
void gfx_clip(int win, int x, int y, int w, int h) { SC6(SYS_GFX_SET_CLIP, win, x, y, w, h, 0); }
void gfx_clip_origin(int win, int x, int y, int w, int h) { SC6(SYS_GFX_SET_CLIP, win, x, y, w, h, CLIP_ORIGIN); }
void gfx_clip_reset(int win) { SC6(SYS_GFX_SET_CLIP, win, 0, 0, 0, 0, CLIP_RESET); }
int  gfx_text_n(int win, int x, int y, const char *s, int n, unsigned c, int font) {
    return (int)SC6(SYS_GFX_TEXT_N, win, x, y, s, n, (c & 0xFFFFFF) | ((unsigned)(font & 0xFF) << 24));
}
static long pack_geom(int x, int y, int w, int h) {
    return ((long)(x & 0xFFFF) << 48) | ((long)(y & 0xFFFF) << 32) | ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF);
}
void gfx_shade(int win, int x, int y, int w, int h, unsigned c1, unsigned c2, int kind, int param) {
    SC6(SYS_GFX_SHADE, win, pack_geom(x, y, w, h), c1, c2, kind, param);
}
int  gfx_theme(hal_theme_t *t) { return (int)SC1(SYS_GFX_THEME, t); }

/* UTF-8: utf8.c (also compiled for the host tests) */

/* ---- text measurement: advance widths cached per font and 256 code points ---- */
#define NFONTS 7
#define CACHED_PAGES 0x30                /* U+0000..U+2FFF; other characters ask the kernel */
static unsigned char *adv_page[NFONTS][CACHED_PAGES];
static hal_fontinfo_t finfo[NFONTS];
static char finfo_ok[NFONTS];

int gfx_font_metrics(int font, unsigned first, int count, unsigned char *adv, hal_fontinfo_t *info) {
    return (int)SC5(SYS_GFX_FONT_METRICS, font, first, count, adv, info);
}
static int font_index(int font) { return font >= 0 && font < NFONTS ? font : FONT_UI; }   /* as the kernel */
static const hal_fontinfo_t *font_info(int font) {
    font = font_index(font);
    if (!finfo_ok[font] && gfx_font_metrics(font, 0, 0, NULL, &finfo[font]) == 0) finfo_ok[font] = 1;
    return &finfo[font];
}
int text_height(int font) { return font_info(font)->height; }
int text_ascent(int font) { return font_info(font)->ascent; }

int text_char_width(unsigned cp, int font) {
    font = font_index(font);
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < CACHED_PAGES * 256) {
        unsigned char **pg = &adv_page[font][cp >> 8];
        if (!*pg) {
            unsigned char *p = malloc(256);
            if (p && gfx_font_metrics(font, cp & ~0xFFu, 256, p, NULL) == 0) *pg = p;
            else { free(p); goto ask; }
        }
        return (*pg)[cp & 0xFF];
    }
ask:;
    unsigned char a = 0;
    gfx_font_metrics(font, cp, 1, &a, NULL);
    return a;
}
int text_width_n(const char *s, int n, int font) {
    int w = 0, pos = 0;
    while (pos < n && s[pos]) w += text_char_width(utf8_get(s, n, &pos), font);
    return w;
}
int text_width(const char *s, int font) { return text_width_n(s, (int)strlen(s), font); }
int text_fit(const char *s, int n, int font, int width) {
    int pos = 0, acc = 0;
    while (pos < n && s[pos]) {
        int p2 = pos;
        int cw = text_char_width(utf8_get(s, n, &p2), font);
        if (acc + cw > width) break;
        acc += cw;
        pos = p2;
    }
    return pos;
}
void gfx_text_box(int win, int x, int y, int w, int h, const char *s, unsigned color, int font, int align) {
    int len = (int)strlen(s), tw = text_width_n(s, len, font);
    int ty = y + (h - text_height(font)) / 2;
    if (tw <= w) {
        int tx = align == 1 ? x + (w - tw) / 2 : align == 2 ? x + w - tw : x;
        gfx_text_n(win, tx, ty, s, len > 2048 ? 2048 : len, color, font);
        return;
    }
    int n = text_fit(s, len, font, w - text_width("...", font));
    int ex = gfx_text_n(win, x, ty, s, n, color, font);
    gfx_text_n(win, ex, ty, "...", 3, color, font);
}

/* ---- more controls ---- */
void ctl_move(int win, int id, int x, int y, int w, int h) { SC3(SYS_CTL_MOVE, win, id, pack_geom(x, y, w, h)); }
int  ctl_focus(int win, int id) { return (int)SC2(SYS_CTL_FOCUS, win, id); }
void ctl_scroll_set(int win, int id, int total, int page, int pos, int line) { SC6(SYS_CTL_SCROLL_SET, win, id, total, page, pos, line); }
void ctl_set_tool(int win, int id, int icon, const char *tip) { SC4(SYS_CTL_SET_TOOL, win, id, icon, tip); }

/* ---- input box: a modal dialog built from the calls above ---- */
int inputbox(int owner, const char *title, const char *prompt, char *buf, int n) {
    enum { ID_TEXT = 100 };
    int w = win_create(title, 340, 120, WIN_DIALOG);
    if (w < 0) return 0;
    if (owner >= 0) win_set_owner(w, owner, 1);
    ctl_create(w, CTL_LABEL, 14, 12, 312, 18, prompt, 101, 0);
    ctl_create(w, CTL_TEXTBOX, 14, 36, 312, 22, buf, ID_TEXT, 0);
    ctl_create(w, CTL_BUTTON, 170, 82, 75, 23, "OK", IDR_OK, CTL_DEFAULT);
    ctl_create(w, CTL_BUTTON, 251, 82, 75, 23, "Cancel", IDR_CANCEL, CTL_CANCEL);
    ctl_focus(w, ID_TEXT);
    int ok = -1;
    while (ok < 0) {
        hal_event_t ev;
        if ((int)SC2(SYS_WIN_GET_EVENT, &ev, -1) != 1) { ok = 0; break; }
        if (ev.win != w) { defer(&ev); continue; }
        if (ev.type == EVT_CLOSE) ok = 0;
        else if (ev.type == EVT_COMMAND && ev.id == IDR_OK) ok = 1;
        else if (ev.type == EVT_COMMAND && ev.id == IDR_CANCEL) ok = 0;
    }
    if (ok && n > 0) ctl_get_text(w, ID_TEXT, buf, n);
    win_destroy(w);
    return ok;
}
