/* Halcyon OS - Mouse Properties (Control Panel): swap the primary and
 * secondary buttons, the double-click speed (with a folder to try it on) and
 * the pointer speed. Changes take effect at once so they can be tried, and
 * are kept in SETTINGS.INI (mouse.swap, mouse.doubleclick in ms, mouse.speed
 * 1..20) by OK or Apply; Cancel puts the old ones back. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

#define MW 380
#define MH 382
#define ID_SWAP 30
#define ID_APPLY 31
#define DBL_POS(ms) ((900 - (ms)) / 70)          /* slider 0 (slow, 900 ms) .. 10 (fast, 200 ms) */
#define DBL_MS(pos) (900 - (pos) * 70)

typedef struct { rect_t r; int min, max, value; } track_t;
typedef struct mouse_dlg {
    int speed0, dbl0, swap0;                 /* as saved: Cancel goes back to them */
    track_t dbl, spd;
    track_t *drag;
    int folder_open;
} mouse_dlg_t;

static window_t *mouse_win;

/* ---------------- a slider (trackbar) ---------------- */
static int track_x(track_t *t, int v) { return t->r.x + 6 + (t->r.w - 12) * (v - t->min) / MAX(1, t->max - t->min); }
static int track_value(track_t *t, int x) {
    int span = t->r.w - 12, rel = CLAMP(x - t->r.x - 6, 0, span);
    return t->min + (rel * (t->max - t->min) + span / 2) / MAX(1, span);
}
static void track_paint(gc_t *g, track_t *t) {
    int cy = t->r.y + 8;
    gfx_sunken_field(g, mkrect(t->r.x + 2, cy - 2, t->r.w - 4, 4), HEX(0xF4F3EE));
    for (int v = t->min; v <= t->max; v++) {
        int x = track_x(t, v);
        gfx_vline(g, x, cy + 11, 4, HEX(0x9A9A90));
    }
    /* the thumb: a rounded tab pointing down */
    int x = track_x(t, t->value);
    rect_t th = mkrect(x - 5, cy - 9, 11, 16);
    gfx_rounded(g, th, 2, 3, HEX(0xFFFFFF), color_lighten(g_theme->sidebar_top, 60));
    gfx_fill(g, mkrect(th.x, th.y + 4, th.w, 8), color_lighten(g_theme->sidebar_top, 60));
    for (int i = 0; i < 5; i++) gfx_hline(g, x - 5 + i, cy + 7 + i, 11 - 2 * i, color_lighten(g_theme->sidebar_top, 40));
    gfx_rounded_outline(g, mkrect(th.x, th.y, th.w, 12), 2, 3, color_lighten(g_theme->sidebar_bottom, -20));
    gfx_line(g, th.x, cy + 6, x, cy + 11, color_lighten(g_theme->sidebar_bottom, -20));
    gfx_line(g, th.x + th.w - 1, cy + 6, x, cy + 11, color_lighten(g_theme->sidebar_bottom, -20));
    gfx_hline(g, th.x + 2, th.y + 1, th.w - 4, color_lighten(g_theme->hover, 30));
}

/* ---------------- painting ---------------- */
static rect_t folder_rect(void) { return mkrect(MW - 72, 158, 40, 40); }

static void draw_mouse(gc_t *g, int x, int y, int swap) {
    /* a mouse seen from above, the primary button lit */
    rect_t body = mkrect(x, y, 46, 70);
    gfx_rounded(g, body, 20, 15, HEX(0xF4F6FA), HEX(0xC8CED8));
    gfx_rounded_outline(g, body, 20, 15, HEX(0x6A7280));
    rect_t lb = mkrect(x + 3, y + 3, 19, 28), rb = mkrect(x + 24, y + 3, 19, 28);
    rect_t prim = swap ? rb : lb;
    gfx_rounded(g, prim, 8, swap ? 2 : 1, color_lighten(g_theme->select, 80), g_theme->select);
    gfx_vline(g, x + 23, y + 2, 30, HEX(0x6A7280));
    gfx_hline(g, x + 4, y + 31, 38, HEX(0x8A92A0));
    gfx_rounded(g, mkrect(x + 20, y + 10, 6, 13), 3, 15, HEX(0x8A92A0), HEX(0x5A6270));   /* the wheel */
}

static void mouse_paint(window_t *w, gc_t *g) {
    mouse_dlg_t *m = w->data;
    int swap;
    wm_mouse_get(NULL, NULL, &swap);
    draw_group(g, mkrect(12, 10, MW - 24, 110), "Button configuration");
    draw_mouse(g, MW - 80, 30, swap);
    gfx_text_wrap(g, &font_ui, mkrect(24, 56, MW - 124, 56),
                  swap ? "The right button is the primary one: it selects and drags. Use the left button for menus."
                       : "The left button is the primary one: it selects and drags. Use the right button for menus.",
                  HEX(0x404040));
    draw_group(g, mkrect(12, 130, MW - 24, 100), "Double-click speed");
    gfx_text_wrap(g, &font_ui, mkrect(24, 148, MW - 120, 32), "Double-click the folder to test your setting.", HEX(0x404040));
    rect_t f = folder_rect();
    gfx_icon(g, f.x + 4, f.y + 4, &icons32[m->folder_open ? ICON_FOLDER_OPEN : ICON_FOLDER]);
    gfx_text(g, &font_ui, 24, m->dbl.r.y + 2, "Slow", HEX(0x000000));
    gfx_text(g, &font_ui, m->dbl.r.x + m->dbl.r.w + 8, m->dbl.r.y + 2, "Fast", HEX(0x000000));
    track_paint(g, &m->dbl);
    draw_group(g, mkrect(12, 240, MW - 24, 92), "Pointer speed");
    gfx_icon(g, 24, 262, &icons32[ICON_MOUSE]);
    gfx_text(g, &font_ui, 64, 260, "Select a pointer speed:", HEX(0x404040));
    gfx_text(g, &font_ui, 64, m->spd.r.y + 2, "Slow", HEX(0x000000));
    gfx_text(g, &font_ui, m->spd.r.x + m->spd.r.w + 8, m->spd.r.y + 2, "Fast", HEX(0x000000));
    track_paint(g, &m->spd);
}

/* ---------------- behaviour ---------------- */
static void live(window_t *w) {
    mouse_dlg_t *m = w->data;
    widget_t *sw = widget_find(w, ID_SWAP);
    wm_mouse_config(m->spd.value, DBL_MS(m->dbl.value), sw && sw->value);
    widget_t *ap = widget_find(w, ID_APPLY);
    int s, d, x;
    wm_mouse_get(&s, &d, &x);
    int changed = s != m->speed0 || d != m->dbl0 || x != m->swap0;
    if (ap) ap->style = changed ? (ap->style & ~WS_DISABLED) : (ap->style | WS_DISABLED);
    wm_invalidate(w);
}

static void save(window_t *w) {
    mouse_dlg_t *m = w->data;
    int s, d, x;
    wm_mouse_get(&s, &d, &x);
    settings_set_int("mouse.speed", s);
    settings_set_int("mouse.doubleclick", d);
    settings_set_int("mouse.swap", x);
    settings_save();
    m->speed0 = s; m->dbl0 = d; m->swap0 = x;
    live(w);
}

static void mouse_event(window_t *w, gui_event_t *e) {
    mouse_dlg_t *m = w->data;
    if (e->type == GE_DBLCLICK && rect_contains(folder_rect(), e->x, e->y)) {
        m->folder_open = !m->folder_open;
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_DOWN && (e->buttons & (MOUSE_LEFT | MOUSE_RIGHT))) {
        track_t *t = rect_contains(m->dbl.r, e->x, e->y) ? &m->dbl : rect_contains(m->spd.r, e->x, e->y) ? &m->spd : NULL;
        if (t) { m->drag = t; t->value = track_value(t, e->x); live(w); }
        return;
    }
    if (e->type == GE_MOUSE_MOVE && m->drag && e->buttons) {
        int v = track_value(m->drag, e->x);
        if (v != m->drag->value) { m->drag->value = v; live(w); }
        return;
    }
    if (e->type == GE_MOUSE_UP) m->drag = NULL;
}

static void on_swap(window_t *w, widget_t *b) { (void)b; live(w); }
static void on_ok(window_t *w, widget_t *b) { (void)b; save(w); wm_close(w); }
static void on_apply(window_t *w, widget_t *b) { (void)b; save(w); }
static void on_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }

static void mouse_destroy(window_t *w) {
    mouse_dlg_t *m = w->data;
    if (m) {
        wm_mouse_config(m->speed0, m->dbl0, m->swap0);    /* what was not saved goes back */
        kfree(m);
    }
    w->data = NULL;
    if (mouse_win == w) mouse_win = NULL;
}

void mouse_dialog(void) {
    if (mouse_win && wm_alive(mouse_win)) { wm_show(mouse_win); return; }
    mouse_dlg_t *m = kzalloc(sizeof *m);
    if (!m) return;
    wm_mouse_get(&m->speed0, &m->dbl0, &m->swap0);
    window_t *w = wm_create("Mouse Properties", ICON_MOUSE, -1, -1, MW, MH, WF_NO_MAXIMIZE | WF_NO_MINIMIZE);
    if (!w) { kfree(m); return; }
    mouse_win = w;
    w->data = m;
    w->on_paint = mouse_paint;
    w->on_event = mouse_event;
    w->on_destroy = mouse_destroy;
    m->dbl = (track_t){ mkrect(62, 196, 180, 24), 0, 10, CLAMP(DBL_POS(m->dbl0), 0, 10) };
    m->spd = (track_t){ mkrect(100, 290, 180, 24), 1, 20, m->speed0 };
    widget_t *sw = widget_add(w, W_CHECKBOX, mkrect(24, 30, MW - 124, 20), "Switch primary and secondary buttons", ID_SWAP);
    sw->value = m->swap0;
    sw->on_change = on_swap;
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(MW - 12 - 3 * 75 - 2 * 8, MH - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(MW - 12 - 2 * 75 - 8, MH - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(MW - 12 - 75, MH - 35, 75, 23), "Apply", ID_APPLY);
    ap->style |= WS_DISABLED; ap->on_click = on_apply;
    w->focus = sw;
    wm_place_restore(w, "mouse");
}
