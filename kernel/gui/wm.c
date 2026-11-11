/* Halcyon OS - window manager: compositor, input routing, main loop */
#include "wm.h"
#include "core/irq.h"
#include "core/thread.h"
#include "fs/block.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/io.h"

/* ---------------- themes ---------------- */
const theme_t g_themes[] = {
    { "Halcyon Blue",
      HEX(0x8FC3FF), HEX(0x2E6FDB), HEX(0x1A4AB0),
      HEX(0x9DB9EB), HEX(0x7D98CF),
      HEX(0x1F55C0), HEX(0x7C97CF),
      HEX(0xFFFFFF), HEX(0xDCE6F8),
      HEX(0xEEEBE0),
      HEX(0x3D7BE0), HEX(0x1B48A8), HEX(0x6FA2F5),
      HEX(0x2BC4B0), HEX(0x127A6C),
      HEX(0x316AC5), HEX(0xFFFFFF),
      HEX(0x7AA2E6), HEX(0x6378D8),
      HEX(0x4F8EEA), HEX(0x1B4FB2),
      HEX(0x2F4E87), HEX(0xF5B547) },
    { "Evergreen",
      HEX(0xD2E6A6), HEX(0x8BA85A), HEX(0x5C7A30),
      HEX(0xC9D3B0), HEX(0xAAB78E),
      HEX(0x6F8A42), HEX(0xAAB78E),
      HEX(0xFFFFFF), HEX(0xF2F5E8),
      HEX(0xEEEBE0),
      HEX(0x9AB46A), HEX(0x5E7A34), HEX(0xC8DD9A),
      HEX(0xF0A848), HEX(0xC06A18),
      HEX(0x93A070), HEX(0xFFFFFF),
      HEX(0xC2D29C), HEX(0x96AA6E),
      HEX(0x9AB46A), HEX(0x5E7A34),
      HEX(0x4C5E2C), HEX(0xF5B547) },
    { "Graphite",
      HEX(0xF4F5F8), HEX(0xBFC3CE), HEX(0x9EA3B2),
      HEX(0xE6E8EE), HEX(0xD2D5DD),
      HEX(0xA6ABBA), HEX(0xD2D5DD),
      HEX(0x1C2230), HEX(0x707684),
      HEX(0xEEEEEC),
      HEX(0xD5D8E0), HEX(0x9EA3B2), HEX(0xFFFFFF),
      HEX(0x8C93A6), HEX(0x5B6275),
      HEX(0x5A6C92), HEX(0xFFFFFF),
      HEX(0xD8DCE6), HEX(0xB2B8C8),
      HEX(0xA6ACBC), HEX(0x6C7386),
      HEX(0x5B6275), HEX(0x6F9AE6) },
};
const int g_theme_count = 3;
const theme_t *g_theme = &g_themes[0];
static int theme_idx;
void theme_set(int i) {
    if (i < 0 || i >= g_theme_count) return;
    theme_idx = i;
    g_theme = &g_themes[i];
    gui_full_redraw();
}
int theme_index(void) { return theme_idx; }

/* ---------------- state ---------------- */
static window_t *windows;          /* bottom -> top */
static int mx, my, mbuttons;
static window_t *capture;          /* window receiving all mouse events while a button is held */
static int capture_taskbar, capture_desktop;
static window_t *hover_win;
static int busy;
static int mouse_speed = 10;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_BUTTON };
static int drag_mode, drag_dx, drag_dy, drag_edge;
static window_t *drag_win;
static int title_btn_hover = -1, title_btn_pressed = -1;
static window_t *title_btn_win;

#define ABS_DIFF_OK(a, b) (((a) - (b)) < 5 && ((b) - (a)) < 5)
#define ABSV(v) ((v) < 0 ? -(v) : (v))

/* double click */
static u64 last_click_ms; static int last_click_x, last_click_y, last_click_btn;

/* dirty rectangles */
#define MAX_DIRTY 24
static rect_t dirty[MAX_DIRTY];
static int ndirty;

int wm_mouse_x(void) { return mx; }
int wm_mouse_y(void) { return my; }

void wm_invalidate_rect(rect_t r) {
    r = rect_intersect(r, mkrect(0, 0, screen_w(), screen_h()));
    if (rect_empty(r)) return;
    for (int i = 0; i < ndirty; i++) {
        rect_t u = rect_union(dirty[i], r);
        /* merge if the union doesn't waste much area */
        if ((long)u.w * u.h <= (long)dirty[i].w * dirty[i].h + (long)r.w * r.h + 4096) { dirty[i] = u; return; }
    }
    if (ndirty < MAX_DIRTY) { dirty[ndirty++] = r; return; }
    for (int i = 1; i < ndirty; i++) dirty[0] = rect_union(dirty[0], dirty[i]);
    dirty[0] = rect_union(dirty[0], r);
    ndirty = 1;
}
void gui_full_redraw(void) {
    for (window_t *w = windows; w; w = w->next) w->needs_paint = 1;
    wm_invalidate_rect(mkrect(0, 0, screen_w(), screen_h()));
}

/* ---------------- geometry ---------------- */
static int framed(window_t *w) { return !(w->flags & WF_POPUP); }

rect_t wm_client_rect(window_t *w) {
    if (!framed(w)) return w->frame;
    return mkrect(w->frame.x + BORDER, w->frame.y + TITLE_H, w->frame.w - 2 * BORDER, w->frame.h - TITLE_H - BORDER);
}

static rect_t vis_rect(window_t *w) {
    /* area that changes when the window changes (incl. shadow) */
    rect_t r = w->frame;
    r.w += 6; r.h += 6;
    return r;
}

static void ensure_client(window_t *w) {
    rect_t c = wm_client_rect(w);
    if (c.w < 1) c.w = 1;
    if (c.h < 1) c.h = 1;
    if (w->client && w->client->w == c.w && w->client->h == c.h) return;
    surface_free(w->client);
    w->client = surface_new(c.w, c.h);
    w->needs_paint = 1;
}

window_t *wm_first(void) { return windows; }
int wm_alive(window_t *w) {
    for (window_t *x = windows; x; x = x->next) if (x == w) return 1;
    return 0;
}

window_t *wm_active(void) {
    window_t *a = NULL;
    for (window_t *w = windows; w; w = w->next)
        if (w->state != WS_MINIMIZED && !(w->flags & WF_POPUP)) a = w;
    return a;
}

static void unlink(window_t *w) {
    window_t **pp = &windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    w->next = NULL;
}
static void link_top(window_t *w) {
    /* topmost windows stay above normal ones */
    window_t **pp = &windows;
    if (!(w->flags & (WF_TOPMOST | WF_POPUP))) {
        while (*pp && !((*pp)->flags & (WF_TOPMOST | WF_POPUP))) pp = &(*pp)->next;
    } else {
        while (*pp) pp = &(*pp)->next;
    }
    w->next = *pp;
    *pp = w;
}

static window_t *prev_active;
static void activation_changed(void) {
    window_t *a = wm_active();
    if (a == prev_active) return;
    if (prev_active && wm_alive(prev_active)) {
        wm_invalidate_rect(prev_active->frame);
        if (prev_active->on_event) { gui_event_t e = { .type = GE_BLUR }; prev_active->on_event(prev_active, &e); }
    }
    prev_active = a;
    if (a) {
        wm_invalidate_rect(a->frame);
        if (a->on_event) { gui_event_t e = { .type = GE_FOCUS }; a->on_event(a, &e); }
    }
    taskbar_invalidate();
}

void wm_raise(window_t *w) {
    if (!wm_alive(w)) return;
    unlink(w);
    link_top(w);
    wm_invalidate_rect(vis_rect(w));
    activation_changed();
}

static int auto_pos;
window_t *wm_create(const char *title, int icon, int x, int y, int cw, int ch, u32 flags) {
    window_t *w = kzalloc(sizeof *w);
    strlcpy(w->title, title ? title : "", sizeof w->title);
    w->icon = icon;
    w->flags = flags;
    if (flags & (WF_DIALOG)) w->flags |= WF_NO_MAXIMIZE | WF_NO_MINIMIZE;
    int fw = (flags & WF_POPUP) ? cw : cw + 2 * BORDER;
    int fh = (flags & WF_POPUP) ? ch : ch + TITLE_H + BORDER;
    int sw = screen_w(), sh = screen_h() - TASKBAR_H;
    if (fw > sw) fw = sw;
    if (fh > sh) fh = sh;
    if (flags & WF_CENTER) { x = (sw - fw) / 2; y = (sh - fh) / 2; }
    else if (x < 0 || y < 0) {
        x = 60 + (auto_pos % 8) * 28;
        y = 40 + (auto_pos % 8) * 24;
        auto_pos++;
        if (x + fw > sw) x = MAX(0, sw - fw);
        if (y + fh > sh) y = MAX(0, sh - fh);
    }
    w->frame = mkrect(x, y, fw, fh);
    w->min_w = 160; w->min_h = 80;
    w->needs_paint = 1;
    ensure_client(w);
    link_top(w);
    wm_invalidate_rect(vis_rect(w));
    activation_changed();
    return w;
}

static void free_widgets(window_t *w) {
    widget_t *wd = w->widgets;
    while (wd) {
        widget_t *n = wd->next;
        if (wd->items) { for (int i = 0; i < wd->nitems; i++) kfree(wd->items[i]); kfree(wd->items); }
        kfree(wd);
        wd = n;
    }
    w->widgets = NULL;
}

void wm_destroy(window_t *w) {
    if (!wm_alive(w)) return;
    /* destroy owned dialogs first */
    for (;;) {
        window_t *c = NULL;
        for (window_t *x = windows; x; x = x->next) if (x->owner == w) { c = x; break; }
        if (!c) break;
        wm_destroy(c);
    }
    if (w->owner && wm_alive(w->owner) && w->owner->modal == w) w->owner->modal = NULL;
    wm_invalidate_rect(vis_rect(w));
    unlink(w);
    w->closed = 1;
    if (capture == w) capture = NULL;
    if (hover_win == w) hover_win = NULL;
    if (drag_win == w) { drag_win = NULL; drag_mode = DRAG_NONE; }
    if (title_btn_win == w) title_btn_win = NULL;
    if (prev_active == w) prev_active = NULL;
    if (w->on_destroy) w->on_destroy(w);
    free_widgets(w);
    surface_free(w->client);
    w->client = NULL;
    /* keep the struct alive for a while: nested loops may still look at w->closed */
    static window_t *graveyard[16];
    static int gi;
    if (graveyard[gi]) kfree(graveyard[gi]);
    graveyard[gi] = w;
    gi = (gi + 1) % 16;
    activation_changed();
    taskbar_invalidate();
}

void wm_close(window_t *w) {
    if (!wm_alive(w)) return;
    if (w->modal && wm_alive(w->modal)) { wm_raise(w->modal); return; }
    if (w->on_close && !w->on_close(w)) return;
    wm_destroy(w);
}

void wm_set_title(window_t *w, const char *t) {
    strlcpy(w->title, t, sizeof w->title);
    wm_invalidate_rect(mkrect(w->frame.x, w->frame.y, w->frame.w, TITLE_H));
    taskbar_invalidate();
}

void wm_invalidate(window_t *w) {
    if (!wm_alive(w)) return;
    w->needs_paint = 1;
    wm_invalidate_rect(wm_client_rect(w));
}

static void set_frame(window_t *w, rect_t f) {
    wm_invalidate_rect(vis_rect(w));
    int resized = f.w != w->frame.w || f.h != w->frame.h;
    w->frame = f;
    if (resized) {
        ensure_client(w);
        if (w->on_resize) w->on_resize(w);
        w->needs_paint = 1;
    }
    wm_invalidate_rect(vis_rect(w));
}

void wm_resize_client(window_t *w, int cw, int ch) {
    rect_t f = w->frame;
    f.w = framed(w) ? cw + 2 * BORDER : cw;
    f.h = framed(w) ? ch + TITLE_H + BORDER : ch;
    set_frame(w, f);
}

void wm_minimize(window_t *w) {
    if (w->state == WS_MINIMIZED) return;
    w->restore = w->state == WS_MAXIMIZED ? w->restore : w->frame;
    w->state = WS_MINIMIZED;
    wm_invalidate_rect(vis_rect(w));
    activation_changed();
    taskbar_invalidate();
}

void wm_maximize(window_t *w, int on) {
    if (on) {
        if (w->state != WS_MAXIMIZED) w->restore = w->frame;
        w->state = WS_MAXIMIZED;
        set_frame(w, mkrect(-BORDER, -0, screen_w() + 2 * BORDER, screen_h() - TASKBAR_H + BORDER));
    } else {
        w->state = WS_NORMAL;
        set_frame(w, w->restore);
    }
}

void wm_show(window_t *w) {
    if (!wm_alive(w)) return;
    if (w->state == WS_MINIMIZED) {
        w->state = WS_NORMAL;
        if (w->restore.w) {
            /* restore the previous normal or maximized geometry */
            if (w->frame.w == w->restore.w && w->frame.h == w->restore.h) set_frame(w, w->restore);
        }
    }
    wm_raise(w);
}

void wm_set_busy(int b) {
    busy = b;
    wm_invalidate_rect(mkrect(mx - 2, my - 2, 40, 40));
}

/* ---------------- frame drawing ---------------- */
static rect_t title_button_rect(window_t *w, int idx) {
    /* idx 0 = close, 1 = maximize, 2 = minimize (right to left) */
    int s = 21;
    int x = w->frame.x + w->frame.w - 6 - s - idx * (s + 2);
    return mkrect(x, w->frame.y + 3, s, s);
}
static int title_buttons(window_t *w) {
    if (w->flags & WF_NO_CLOSE) return 0;
    if (w->flags & WF_DIALOG) return 1;
    return 3;
}

static void draw_title_button(gc_t *g, rect_t r, int kind, int state, int active) {
    color_t top, bot;
    if (kind == 0) { top = HEX(0xF59A80); bot = HEX(0xC53A1C); }
    else { top = color_lighten(g_theme->title_mid, 90); bot = g_theme->title_mid; }
    if (!active) { top = color_mix(top, HEX(0xFFFFFF), 90); bot = color_mix(bot, HEX(0xFFFFFF), 90); }
    if (state == 1) { top = color_lighten(top, 50); bot = color_lighten(bot, 40); }
    if (state == 2) { top = color_lighten(top, -40); bot = color_lighten(bot, -30); }
    gfx_rounded(g, r, 4, 15, top, bot);
    gfx_rounded_outline(g, r, 4, 15, HEX(0xFFFFFF));
    color_t c = HEX(0xFFFFFF);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (kind) {
    case 0:
        for (int d = -1; d <= 1; d++) {
            gfx_line(g, cx - 4 + d, cy - 4, cx + 4 + d, cy + 4, c);
            gfx_line(g, cx - 4 + d, cy + 4, cx + 4 + d, cy - 4, c);
        }
        break;
    case 1:
        gfx_rect(g, mkrect(cx - 5, cy - 5, 11, 10), c);
        gfx_hline(g, cx - 5, cy - 4, 11, c);
        gfx_hline(g, cx - 5, cy - 3, 11, c);
        break;
    case 3: /* restore */
        gfx_rect(g, mkrect(cx - 2, cy - 6, 8, 7), c);
        gfx_hline(g, cx - 2, cy - 5, 8, c);
        gfx_fill(g, mkrect(cx - 6, cy - 2, 8, 7), top);
        gfx_rect(g, mkrect(cx - 6, cy - 2, 8, 7), c);
        gfx_hline(g, cx - 6, cy - 1, 8, c);
        break;
    case 2:
        gfx_fill(g, mkrect(cx - 5, cy + 2, 7, 3), c);
        break;
    }
}

static void draw_frame(gc_t *g, window_t *w, int active) {
    rect_t f = w->frame;
    const theme_t *t = g_theme;
    color_t frame = active ? t->frame : t->frame_in;
    /* soft drop shadow */
    if (w->state != WS_MAXIMIZED) gfx_shadow(g, mkrect(f.x + 2, f.y + 6, f.w - 2, f.h - 6), 6, 70);
    /* title bar */
    rect_t tb = mkrect(f.x, f.y, f.w, TITLE_H + 2);
    if (active) {
        gfx_rounded(g, mkrect(tb.x, tb.y, tb.w, 7), 8, 3, t->title_top, t->title_mid);
        gfx_gradient_v(g, mkrect(tb.x, tb.y + 7, tb.w, tb.h - 7), t->title_mid, t->title_bottom);
    } else {
        gfx_rounded(g, mkrect(tb.x, tb.y, tb.w, 7), 8, 3, color_lighten(t->title_in_top, 60), t->title_in_top);
        gfx_gradient_v(g, mkrect(tb.x, tb.y + 7, tb.w, tb.h - 7), t->title_in_top, t->title_in_bottom);
    }
    gfx_rounded_outline(g, mkrect(f.x, f.y, f.w, TITLE_H + 10), 8, 3, color_lighten(frame, -60));
    /* borders */
    gfx_fill(g, mkrect(f.x, f.y + TITLE_H, BORDER, f.h - TITLE_H), frame);
    gfx_fill(g, mkrect(f.x + f.w - BORDER, f.y + TITLE_H, BORDER, f.h - TITLE_H), frame);
    gfx_fill(g, mkrect(f.x, f.y + f.h - BORDER, f.w, BORDER), frame);
    color_t edge = color_lighten(frame, -60);
    gfx_vline(g, f.x, f.y + 8, f.h - 8, edge);
    gfx_vline(g, f.x + f.w - 1, f.y + 8, f.h - 8, edge);
    gfx_hline(g, f.x, f.y + f.h - 1, f.w, edge);
    gfx_vline(g, f.x + 1, f.y + 8, f.h - 9, color_lighten(frame, 50));
    /* icon + title */
    int tx = f.x + 8;
    if (w->icon >= 0) {
        gfx_icon(g, f.x + 6, f.y + 5, &icons16[w->icon]);
        tx = f.x + 28;
    }
    int nb = title_buttons(w);
    rect_t tr = mkrect(tx, f.y + 1, f.w - (tx - f.x) - nb * 23 - 10, TITLE_H);
    color_t tc = active ? t->title_text : t->title_in_text;
    if (active && g_theme != &g_themes[2]) gfx_text_box(g, &font_title, mkrect(tr.x + 1, tr.y + 1, tr.w, tr.h), w->title, color_lighten(t->title_bottom, -80), 0);
    gfx_text_box(g, &font_title, tr, w->title, tc, 0);
    /* buttons */
    for (int i = 0; i < nb; i++) {
        int kind = i == 0 ? 0 : i == 1 ? (w->state == WS_MAXIMIZED ? 3 : 1) : 2;
        if ((kind == 1 || kind == 3) && (w->flags & WF_NO_MAXIMIZE)) { if (i == 1) continue; }
        if (kind == 2 && (w->flags & WF_NO_MINIMIZE)) continue;
        int st = 0;
        if (title_btn_win == w && title_btn_hover == i) st = 1;
        if (title_btn_win == w && title_btn_pressed == i && title_btn_hover == i) st = 2;
        draw_title_button(g, title_button_rect(w, i), kind, st, active);
    }
}

static void paint_client(window_t *w) {
    if (!w->client) return;
    gc_t g = gc_for(w->client);
    gfx_fill(&g, mkrect(0, 0, w->client->w, w->client->h), g_theme->face);
    if (w->on_paint) w->on_paint(w, &g);
    widgets_paint(w, &g);
    if (w->menubar) menubar_paint(w, &g);
    w->needs_paint = 0;
}

/* ---------------- cursors ---------------- */
static const char *cur_arrow[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX", NULL };
static const char *cur_ibeam[] = {
    "XXX XXX", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "   X", "XXX XXX", NULL };
static const char *cur_busy[] = {
    "XXXXXXXXXXX", "X.........X", " X.......X", " X.XXXXX.X", "  X.XXX.X", "   X.X.X", "    X.X", "    X.X", "   X...X",
    "  X..X..X", " X...X...X", " X..XXX..X", "X.XXXXXXX.X", "XXXXXXXXXXX", NULL };
static const char *cur_nwse[] = {
    "XXXXXX", "X....X", "X...X", "X....X", "X.X..X", "XX X..X", "    X..X XX", "     X..X.X", "      X....X", "       X...X", "      X....X", "      XXXXXX", NULL };
static const char *cur_h[] = {
    "   X       X", "  XX       XX", " X.XXXXXXXXX.X", "X.............X", " X.XXXXXXXXX.X", "  XX       XX", "   X       X", NULL };
static const char *cur_v[] = {
    "   X", "  X.X", " X...X", "XXX.XXX", "  X.X", "  X.X", "  X.X", "  X.X", "  X.X", "XXX.XXX", " X...X", "  X.X", "   X", NULL };
static const char *cur_hand[] = {
    "     XX", "    X..X", "    X..X", "    X..X", "    X..XXX", "    X..X..XXX", "XX  X..X..X..XX", "X..XX........X.X",
    " X..X..........X", "  X............X", "  X...........X", "   X..........X", "    X........X", "     X.......X", "     XXXXXXXXX", NULL };
static const char *cur_cross[] = {
    "     X", "     X", "     X", "     X", "", "XXXX   XXXX", "", "     X", "     X", "     X", "     X", NULL };

static int cursor_kind(void);
static void cursor_info(int kind, const char ***img, int *hx, int *hy) {
    switch (kind) {
    case CURSOR_IBEAM: *img = cur_ibeam; *hx = 3; *hy = 7; break;
    case CURSOR_BUSY: *img = cur_busy; *hx = 5; *hy = 7; break;
    case CURSOR_RESIZE_NWSE: *img = cur_nwse; *hx = 6; *hy = 6; break;
    case CURSOR_RESIZE_H: *img = cur_h; *hx = 7; *hy = 3; break;
    case CURSOR_RESIZE_V: *img = cur_v; *hx = 3; *hy = 6; break;
    case CURSOR_HAND: *img = cur_hand; *hx = 5; *hy = 0; break;
    case CURSOR_CROSS: *img = cur_cross; *hx = 5; *hy = 5; break;
    default: *img = cur_arrow; *hx = 0; *hy = 0; break;
    }
}
static int drawn_cursor = -1, drawn_cx, drawn_cy;
static rect_t cursor_rect(int kind, int x, int y) {
    const char **img; int hx, hy;
    cursor_info(kind, &img, &hx, &hy);
    return mkrect(x - hx - 1, y - hy - 1, 20, 24);
}
static void draw_cursor(gc_t *g) {
    int kind = cursor_kind();
    const char **img; int hx, hy;
    cursor_info(kind, &img, &hx, &hy);
    int x0 = mx - hx, y0 = my - hy;
    /* shadow */
    for (int j = 0; img[j]; j++)
        for (int i = 0; img[j][i]; i++)
            if (img[j][i] != ' ') {
                int ax = x0 + i + 1 + g->ox, ay = y0 + j + 1 + g->oy;
                if (rect_contains(g->clip, ax, ay)) {
                    u32 *p = &g->s->px[ay * g->s->stride + ax];
                    *p = color_mix(*p, 0xFF000000, 70);
                }
            }
    color_t outline = kind == CURSOR_IBEAM || kind == CURSOR_CROSS ? HEX(0x000000) : HEX(0x000000);
    for (int j = 0; img[j]; j++)
        for (int i = 0; img[j][i]; i++) {
            char c = img[j][i];
            if (c == 'X') gfx_pixel(g, x0 + i, y0 + j, outline);
            else if (c == '.') gfx_pixel(g, x0 + i, y0 + j, HEX(0xFFFFFF));
        }
    drawn_cursor = kind; drawn_cx = mx; drawn_cy = my;
}

/* ---------------- hit testing ---------------- */
static window_t *window_at(int x, int y) {
    window_t *hit = NULL;
    for (window_t *w = windows; w; w = w->next)
        if (w->state != WS_MINIMIZED && rect_contains(w->frame, x, y)) hit = w;
    return hit;
}
static int is_overlay(window_t *w) { return (w->flags & (WF_TOPMOST | WF_POPUP)) != 0; }

enum { HT_NONE, HT_CLIENT, HT_TITLE, HT_BUTTON, HT_RESIZE_R, HT_RESIZE_B, HT_RESIZE_RB };
static int hit_test(window_t *w, int x, int y, int *btn) {
    if (!framed(w)) return HT_CLIENT;
    rect_t f = w->frame;
    if (w->flags & WF_RESIZABLE && w->state != WS_MAXIMIZED) {
        int r = x >= f.x + f.w - BORDER - 2, b = y >= f.y + f.h - BORDER - 2;
        int corner = x >= f.x + f.w - 14 && y >= f.y + f.h - 14;
        if (corner || (r && b)) return HT_RESIZE_RB;
        if (r && y > f.y + TITLE_H) return HT_RESIZE_R;
        if (b) return HT_RESIZE_B;
    }
    if (y < f.y + TITLE_H) {
        int nb = title_buttons(w);
        for (int i = 0; i < nb; i++) {
            if (i == 1 && (w->flags & WF_NO_MAXIMIZE)) continue;
            if (i == 2 && (w->flags & WF_NO_MINIMIZE)) continue;
            if (rect_contains(title_button_rect(w, i), x, y)) { *btn = i; return HT_BUTTON; }
        }
        return HT_TITLE;
    }
    if (rect_contains(wm_client_rect(w), x, y)) return HT_CLIENT;
    return HT_NONE;
}

static int cursor_kind(void) {
    if (busy) return CURSOR_BUSY;
    if (drag_mode == DRAG_RESIZE) return drag_edge == HT_RESIZE_R ? CURSOR_RESIZE_H : drag_edge == HT_RESIZE_B ? CURSOR_RESIZE_V : CURSOR_RESIZE_NWSE;
    window_t *w = capture ? capture : window_at(mx, my);
    if (!w) return CURSOR_ARROW;
    int b;
    int ht = hit_test(w, mx, my, &b);
    if (ht == HT_RESIZE_RB) return CURSOR_RESIZE_NWSE;
    if (ht == HT_RESIZE_R) return CURSOR_RESIZE_H;
    if (ht == HT_RESIZE_B) return CURSOR_RESIZE_V;
    if (ht == HT_CLIENT) {
        rect_t c = wm_client_rect(w);
        if (my - c.y < menubar_height(w)) return CURSOR_ARROW;
        return w->cursor;
    }
    return CURSOR_ARROW;
}

/* ---------------- event dispatch ---------------- */
static void deliver(window_t *w, gui_event_t *e) {
    if (!w) return;
    if (w->menubar && menubar_event(w, e)) return;
    if (widgets_event(w, e)) return;
    if (w->on_event) w->on_event(w, e);
}

static void client_event(window_t *w, int type, int button, int dz, gui_event_t *out) {
    rect_t c = wm_client_rect(w);
    memset(out, 0, sizeof *out);
    out->type = type;
    out->x = mx - c.x; out->y = my - c.y;
    out->button = button; out->buttons = mbuttons; out->dz = dz;
}

static window_t *top_popup(void) {
    window_t *p = NULL;
    for (window_t *w = windows; w; w = w->next) if (w->flags & WF_POPUP) p = w;
    return p;
}

static void set_title_hover(window_t *w, int idx) {
    if (title_btn_win == w && title_btn_hover == idx) return;
    if (title_btn_win && wm_alive(title_btn_win)) wm_invalidate_rect(mkrect(title_btn_win->frame.x, title_btn_win->frame.y, title_btn_win->frame.w, TITLE_H));
    title_btn_win = w; title_btn_hover = idx;
    if (w) wm_invalidate_rect(mkrect(w->frame.x, w->frame.y, w->frame.w, TITLE_H));
}

static void mouse_down(int button) {
    u64 now = time_ms();
    int dbl = button == last_click_btn && now - last_click_ms < 450 &&
              ABS_DIFF_OK(mx, last_click_x) && ABS_DIFF_OK(my, last_click_y);
    last_click_ms = dbl ? 0 : now; last_click_x = mx; last_click_y = my; last_click_btn = button;

    window_t *w = window_at(mx, my);
    int in_taskbar = rect_contains(taskbar_rect(), mx, my) && (!w || !is_overlay(w));

    /* clicking outside popups closes them */
    window_t *pop = top_popup();
    if (pop && (!w || !(w->flags & WF_POPUP))) {
        menu_close_all();
        /* start menu toggling is handled by the taskbar; other clicks fall through */
        w = window_at(mx, my);
    }

    if (in_taskbar) {
        capture_taskbar = 1;
        gui_event_t e = { .type = dbl ? GE_DBLCLICK : GE_MOUSE_DOWN, .x = mx, .y = my, .button = button, .buttons = mbuttons };
        taskbar_event(&e);
        return;
    }
    if (!w) {
        capture_desktop = 1;
        gui_event_t e = { .type = dbl ? GE_DBLCLICK : GE_MOUSE_DOWN, .x = mx, .y = my, .button = button, .buttons = mbuttons };
        desktop_event(&e);
        return;
    }
    if (w->modal && wm_alive(w->modal)) {
        wm_raise(w->modal);
        return;
    }
    if (!(w->flags & WF_POPUP)) wm_raise(w);

    int btn = -1;
    int ht = hit_test(w, mx, my, &btn);
    if (button == MOUSE_LEFT) {
        if (ht == HT_BUTTON) {
            drag_mode = DRAG_BUTTON; drag_win = w;
            title_btn_pressed = btn;
            set_title_hover(w, btn);
            return;
        }
        if (ht == HT_TITLE) {
            if (dbl && !(w->flags & WF_NO_MAXIMIZE) && (w->flags & WF_RESIZABLE)) {
                wm_maximize(w, w->state != WS_MAXIMIZED);
                return;
            }
            if (w->state != WS_MAXIMIZED) {
                drag_mode = DRAG_MOVE; drag_win = w;
                drag_dx = mx - w->frame.x; drag_dy = my - w->frame.y;
            }
            return;
        }
        if (ht == HT_RESIZE_R || ht == HT_RESIZE_B || ht == HT_RESIZE_RB) {
            drag_mode = DRAG_RESIZE; drag_win = w; drag_edge = ht;
            drag_dx = w->frame.x + w->frame.w - mx; drag_dy = w->frame.y + w->frame.h - my;
            return;
        }
    }
    if (ht == HT_CLIENT) {
        capture = w;
        gui_event_t e;
        client_event(w, dbl ? GE_DBLCLICK : GE_MOUSE_DOWN, button, 0, &e);
        deliver(w, &e);
    }
}

static void mouse_up(int button) {
    if (drag_mode == DRAG_BUTTON) {
        window_t *w = drag_win;
        int idx = title_btn_pressed;
        drag_mode = DRAG_NONE; drag_win = NULL; title_btn_pressed = -1;
        if (w && wm_alive(w)) {
            wm_invalidate_rect(mkrect(w->frame.x, w->frame.y, w->frame.w, TITLE_H));
            int b = -1;
            if (hit_test(w, mx, my, &b) == HT_BUTTON && b == idx) {
                if (idx == 0) wm_close(w);
                else if (idx == 1) wm_maximize(w, w->state != WS_MAXIMIZED);
                else if (idx == 2) wm_minimize(w);
            }
        }
        return;
    }
    if (drag_mode != DRAG_NONE) { drag_mode = DRAG_NONE; drag_win = NULL; return; }
    if (capture_taskbar) {
        capture_taskbar = 0;
        gui_event_t e = { .type = GE_MOUSE_UP, .x = mx, .y = my, .button = button, .buttons = mbuttons };
        taskbar_event(&e);
        return;
    }
    if (capture_desktop) {
        capture_desktop = 0;
        gui_event_t e = { .type = GE_MOUSE_UP, .x = mx, .y = my, .button = button, .buttons = mbuttons };
        desktop_event(&e);
        return;
    }
    window_t *w = capture;
    if (!mbuttons) capture = NULL;
    if (w && wm_alive(w)) {
        gui_event_t e;
        client_event(w, GE_MOUSE_UP, button, 0, &e);
        deliver(w, &e);
    }
}

static void mouse_move(void) {
    if (drag_mode == DRAG_MOVE && drag_win) {
        rect_t f = drag_win->frame;
        f.x = mx - drag_dx;
        f.y = MAX(0, my - drag_dy);
        if (f.y > screen_h() - TASKBAR_H - 10) f.y = screen_h() - TASKBAR_H - 10;
        set_frame(drag_win, f);
        return;
    }
    if (drag_mode == DRAG_RESIZE && drag_win) {
        rect_t f = drag_win->frame;
        if (drag_edge != HT_RESIZE_B) f.w = MAX(drag_win->min_w, mx + drag_dx - f.x);
        if (drag_edge != HT_RESIZE_R) f.h = MAX(drag_win->min_h, my + drag_dy - f.y);
        set_frame(drag_win, f);
        return;
    }
    if (drag_mode == DRAG_BUTTON && drag_win) {
        int b = -1;
        set_title_hover(drag_win, hit_test(drag_win, mx, my, &b) == HT_BUTTON ? b : -1);
        return;
    }
    if (capture_taskbar || capture_desktop) {
        gui_event_t e = { .type = GE_MOUSE_MOVE, .x = mx, .y = my, .buttons = mbuttons };
        if (capture_taskbar) taskbar_event(&e); else desktop_event(&e);
        return;
    }
    if (capture && wm_alive(capture)) {
        gui_event_t e;
        client_event(capture, GE_MOUSE_MOVE, 0, 0, &e);
        deliver(capture, &e);
        return;
    }
    window_t *w = window_at(mx, my);
    /* title button hover */
    int b = -1;
    if (w && hit_test(w, mx, my, &b) == HT_BUTTON) set_title_hover(w, b);
    else if (title_btn_win) set_title_hover(NULL, -1);

    if (rect_contains(taskbar_rect(), mx, my) && (!w || !is_overlay(w))) {
        gui_event_t e = { .type = GE_MOUSE_MOVE, .x = mx, .y = my, .buttons = mbuttons };
        taskbar_event(&e);
        w = NULL;
    } else {
        gui_event_t e = { .type = GE_MOUSE_LEAVE, .x = -1, .y = -1 };
        taskbar_event(&e);
    }
    if (hover_win != w) {
        if (hover_win && wm_alive(hover_win)) {
            gui_event_t e = { .type = GE_MOUSE_LEAVE, .x = -10000, .y = -10000 };
            deliver(hover_win, &e);
        }
        hover_win = w;
    }
    if (w && hit_test(w, mx, my, &b) == HT_CLIENT) {
        gui_event_t e;
        client_event(w, GE_MOUSE_MOVE, 0, 0, &e);
        deliver(w, &e);
    } else if (!w) {
        gui_event_t e = { .type = GE_MOUSE_MOVE, .x = mx, .y = my, .buttons = mbuttons };
        desktop_event(&e);
    }
}

static void wheel(int dz) {
    window_t *w = window_at(mx, my);
    if (!w) return;
    if (w->modal && wm_alive(w->modal)) return;
    gui_event_t e;
    client_event(w, GE_WHEEL, 0, dz, &e);
    deliver(w, &e);
}

static void cycle_windows(void) {
    /* Alt+Tab: bring the bottom-most normal window to the top */
    for (window_t *w = windows; w; w = w->next) {
        if (is_overlay(w) || (w->flags & WF_DIALOG)) continue;
        if (w == wm_active()) continue;
        wm_show(w);
        return;
    }
}

static void key_event(input_event_t *k) {
    if (k->pressed) {
        if (k->key == KEY_LGUI || k->key == KEY_RGUI || (k->key == KEY_ESC && (k->mods & MOD_CTRL))) { start_menu_toggle(); return; }
        if (k->key == KEY_TAB && (k->mods & MOD_ALT)) { cycle_windows(); return; }
        if (k->key == KEY_F4 && (k->mods & MOD_ALT)) { window_t *a = wm_active(); if (a) wm_close(a); else shutdown_dialog(); return; }
    }
    window_t *pop = top_popup();
    window_t *w = pop ? pop : wm_active();
    if (!w) {
        gui_event_t e = { .type = GE_KEY, .key = *k };
        desktop_event(&e);
        return;
    }
    if (w->modal && wm_alive(w->modal)) w = w->modal;
    gui_event_t e = { .type = GE_KEY, .key = *k };
    deliver(w, &e);
}

static void handle_input(void) {
    input_event_t ev;
    while (input_get(&ev)) {
        if (ev.type == EV_KEY) {
            /* Ctrl+Alt+Del: Task Manager */
            if (ev.pressed && ev.key == KEY_DELETE && (ev.mods & MOD_CTRL) && (ev.mods & MOD_ALT)) {
                int shell_run(const char *path, const char *args);
                shell_run("C:/PROGRAMS/TASKMGR.EXE", NULL);
                continue;
            }
            /* Calculator key (USB consumer control) */
            if (ev.pressed && ev.key == KEY_CALCULATOR) {
                desktop_launch_calculator();
                continue;
            }
            key_event(&ev);
            continue;
        }
        int ox = mx, oy = my;
        if (ev.absolute) {
            mx = ev.x * screen_w() / 32768;
            my = ev.y * screen_h() / 32768;
        } else {
            int dx = ev.dx, dy = ev.dy;
            int sp = (ABSV(dx) + ABSV(dy) > 6) ? 2 : 1;
            mx += dx * mouse_speed * sp / 10;
            my += dy * mouse_speed * sp / 10;
        }
        mx = CLAMP(mx, 0, screen_w() - 1);
        my = CLAMP(my, 0, screen_h() - 1);
        if (mx != ox || my != oy) {
            wm_invalidate_rect(cursor_rect(drawn_cursor < 0 ? 0 : drawn_cursor, drawn_cx, drawn_cy));
            mouse_move();
            wm_invalidate_rect(cursor_rect(cursor_kind(), mx, my));
        }
        u8 changed = ev.buttons ^ mbuttons;
        for (int b = 1; b <= 4; b <<= 1) {
            if (!(changed & b)) continue;
            if (ev.buttons & b) { mbuttons |= b; mouse_down(b); }
            else { mbuttons &= ~b; mouse_up(b); }
        }
        if (ev.dz) wheel(ev.dz);
        int k = cursor_kind();
        if (k != drawn_cursor || mx != drawn_cx || my != drawn_cy) {
            wm_invalidate_rect(cursor_rect(drawn_cursor < 0 ? 0 : drawn_cursor, drawn_cx, drawn_cy));
            wm_invalidate_rect(cursor_rect(k, mx, my));
        }
    }
}

/* ---------------- rendering ---------------- */
static void compose(rect_t r) {
    gc_t g = gc_for(g_screen);
    g.clip = r;
    desktop_paint(&g, r);
    window_t *active = wm_active();
    int taskbar_drawn = 0;
    for (window_t *w = windows; w; w = w->next) {
        if (w->state == WS_MINIMIZED) continue;
        if (!taskbar_drawn && is_overlay(w)) { taskbar_paint(&g, r); taskbar_drawn = 1; }
        rect_t vr = vis_rect(w);
        if (rect_empty(rect_intersect(vr, r))) continue;
        if (framed(w)) draw_frame(&g, w, w == active);
        else if (!(w->flags & WF_NOSHADOW)) gfx_shadow(&g, w->frame, 4, 80);
        if (w->needs_paint) paint_client(w);
        rect_t c = wm_client_rect(w);
        if (w->client) gfx_blit(&g, c.x, c.y, w->client, mkrect(0, 0, w->client->w, w->client->h));
    }
    if (!taskbar_drawn) taskbar_paint(&g, r);
    draw_cursor(&g);
}

static int render(void) {
    /* repaint clients that asked for it even if outside dirty regions */
    for (window_t *w = windows; w; w = w->next)
        if (w->needs_paint && w->state != WS_MINIMIZED) wm_invalidate_rect(wm_client_rect(w));
    if (!ndirty) return 0;
    rect_t list[MAX_DIRTY];
    int n = ndirty;
    memcpy(list, dirty, sizeof(rect_t) * n);
    ndirty = 0;
    for (int i = 0; i < n; i++) compose(list[i]);
    for (int i = 0; i < n; i++) screen_present(list[i]);
    return 1;
}

/* ---------------- calls from other threads ---------------- */
/* Window-manager changes requested by other threads (user programs' system
 * calls) run on the GUI thread, from gui_step, so no thread changes the window
 * lists while the GUI thread is walking them. */
typedef struct gjob {
    void (*fn)(void *arg);
    void *arg;
    volatile int done;
    int async;
    struct gjob *next;
} gjob_t;
static gjob_t *jobs_head;
static thread_t *gui_thread;

static void job_add(gjob_t *j) {
    gjob_t **pp = &jobs_head;
    while (*pp) pp = &(*pp)->next;
    j->next = NULL;
    *pp = j;
}
static int job_done(void *arg) { return ((gjob_t *)arg)->done; }

void gui_call(void (*fn)(void *arg), void *arg) {
    if (!gui_thread || thread_current() == gui_thread) { fn(arg); return; }
    gjob_t j = { fn, arg, 0, 0, NULL };
    job_add(&j);
    thread_wait(job_done, &j, ~0ull);
}

void gui_post(void (*fn)(void *arg), void *arg) {
    gjob_t *j = kzalloc(sizeof *j);
    if (!j) return;
    j->fn = fn; j->arg = arg; j->async = 1;
    job_add(j);
}

static int run_jobs(void) {
    int n = 0;
    while (jobs_head) {
        gjob_t *j = jobs_head;
        jobs_head = j->next;
        j->fn(j->arg);
        n++;
        if (j->async) kfree(j);
        else j->done = 1;
    }
    return n;
}

/* ---------------- main loop ---------------- */
static u64 last_tick;

/* One iteration: poll devices, handle input, run ticks, redraw. When none of
 * that found anything to do, the CPU halts until the next interrupt or the
 * earliest deadline a device or window asked for. */
void gui_step(void) {
    int busy = 0;
    u32 gen = irq_generation();
    irq_service();
    input_poll_devices();
    if (storage_service()) busy = 1;
    if (run_jobs()) busy = 1;
    busy |= input_pending();
    handle_input();
    u64 now = time_ms();
    int ticking = 0;
    for (window_t *w = windows; w; w = w->next) if (w->on_tick) { ticking = 1; break; }
    if (now - last_tick >= 100) {
        busy = 1;
        last_tick = now;
        for (window_t *w = windows; w; w = w->next) {
            if (w->on_tick) {
                window_t *nx = w->next;
                w->on_tick(w);
                if (!wm_alive(w)) { w = nx; if (!w) break; }
            }
        }
        shell_tick();
    }
    if (render()) busy = 1;
    u64 deadline = idle_take_deadline();
    if (busy) return;
    /* windows with on_tick want ~10 Hz; the shell (clock, balloons) 1 Hz */
    u64 tick = last_tick + (ticking ? 100 : 1000);
    if (tick < deadline) deadline = tick;
    idle_wait(gen, deadline);
}

void gui_run_until(volatile int *flag) {
    while (!*flag) gui_step();
}

void usergui_init(void);
void gui_main(void) {
    gui_thread = thread_current();
    usergui_init();
    screen_init();
    mx = screen_w() / 2; my = screen_h() / 2;
    desktop_init();
    gui_full_redraw();
    kprintf("gui: desktop ready (%dx%d)\n", screen_w(), screen_h());
    /* test hook: QEMU fw_cfg "opt/halcyon/autorun" = "C:/PROGRAMS/X.EXE args" */
    char autorun[256];
    if (qemu_fw_file("opt/halcyon/autorun", autorun, sizeof autorun) && autorun[0]) {
        char *args = strchr(autorun, ' ');
        if (args) *args++ = 0;
        kprintf("gui: autorun %s\n", autorun);
        shell_run(autorun, args);
    }
    for (;;) gui_step();
}

/* ---------------- clipboard ---------------- */
static char *clip_text;
static int clip_len;
void clipboard_set(const char *t, int len) {
    kfree(clip_text);
    clip_text = kmalloc(len + 1);
    memcpy(clip_text, t, len);
    clip_text[len] = 0;
    clip_len = len;
}
const char *clipboard_get(int *len) {
    if (len) *len = clip_len;
    return clip_text;
}
