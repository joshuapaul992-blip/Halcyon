/* Halcyon user-mode GUI calls (window system in the kernel, like Win32's
 * user32 / GDI): thin wrappers over the system calls */
#include "hal.h"

int  win_create(const char *t, int w, int h, int flags) { return (int)SC4(SYS_WIN_CREATE, t, w, h, flags); }
void win_destroy(int win) { SC1(SYS_WIN_DESTROY, win); }
void win_set_title(int win, const char *t) { SC2(SYS_WIN_SET_TITLE, win, t); }
void win_invalidate(int win) { SC1(SYS_WIN_INVALIDATE, win); }
int  win_get_event(hal_event_t *ev, int timeout_ms) { return (int)SC2(SYS_WIN_GET_EVENT, ev, timeout_ms); }
void win_size(int win, int *w, int *h) {
    long r = SC1(SYS_WIN_SIZE, win);
    if (r < 0) r = 0;
    if (w) *w = (int)(r >> 16);
    if (h) *h = (int)(r & 0xFFFF);
}
void win_set_timer(int win, int ms) { SC2(SYS_WIN_SET_TIMER, win, ms); }

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
    /* out and n packed: out pointer, size in the 6th argument */
    return (int)SC6(SYS_FILE_DIALOG, win, save, title, dir, ext, out) >= 0 ? (out[n - 1] = 0, 1) : 0;
}
void clipboard_set(const char *t) { SC1(SYS_CLIPBOARD_SET, t); }
int  clipboard_get(char *buf, int n) { return (int)SC2(SYS_CLIPBOARD_GET, buf, n); }
