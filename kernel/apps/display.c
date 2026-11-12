/* Halcyon OS - Display Properties: screen resolution.
 *
 * With an adapter that switches modes at once (display_live), Apply changes
 * the mode and asks "Do you want to keep these settings?" for 15 seconds,
 * going back if nobody answers (a mode the monitor cannot show leaves the
 * screen black). With only the firmware frame buffer, the choice is saved and
 * the loader sets it at the next start. */
#include "gui/wm.h"
#include "fonts.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "drivers/display.h"

#define DW 400
#define DH 452
#define DP_LIST 10
#define DP_OK 11
#define DP_CANCEL 12
#define DP_APPLY 13
#define PREV_W 168
#define PREV_H 104

typedef struct disp {
    int sel;                   /* mode index selected in the list */
    int applied_w, applied_h;  /* the mode in use (or saved for the next start) */
    surface_t *thumb;
    int thumb_w, thumb_h;
} disp_t;

static window_t *dp_win;

static void mode_text(display_mode_t m, int native, char *out, int n) {
    snprintf(out, (size_t)n, "%d by %d pixels%s", m.w, m.h, native ? "  (recommended)" : "");
}

static void make_thumb(disp_t *d) {
    display_mode_t m = display_mode(d->sel);
    if (!m.w) return;
    /* the preview screen has the selected mode's shape */
    int tw = PREV_W, th = PREV_W * m.h / m.w;
    if (th > PREV_H) { th = PREV_H; tw = PREV_H * m.w / m.h; }
    if (d->thumb && d->thumb_w == tw && d->thumb_h == th) return;
    if (d->thumb) surface_free(d->thumb);
    d->thumb = surface_new(tw, th);
    d->thumb_w = tw; d->thumb_h = th;
    if (d->thumb) desktop_wallpaper_render(desktop_wallpaper_style(), d->thumb);
}

static void dp_paint(window_t *w, gc_t *g) {
    disp_t *d = w->data;
    gfx_fill(g, mkrect(0, 0, DW, DH), g_theme->face);
    /* monitor */
    int mx = DW / 2, my = 20;
    rect_t bezel = mkrect(mx - PREV_W / 2 - 14, my, PREV_W + 28, PREV_H + 30);
    gfx_rounded(g, bezel, 6, 15, HEX(0x3A3F4B), HEX(0x3A3F4B));
    gfx_rounded(g, rect_inset(bezel, 2), 5, 15, HEX(0x555B69), HEX(0x555B69));
    rect_t scr = mkrect(mx - PREV_W / 2, my + 12, PREV_W, PREV_H);
    gfx_fill(g, scr, HEX(0x101318));
    if (d->thumb) {
        int tx = scr.x + (PREV_W - d->thumb_w) / 2, ty = scr.y + (PREV_H - d->thumb_h) / 2;
        gfx_blit(g, tx, ty, d->thumb, mkrect(0, 0, d->thumb_w, d->thumb_h));
        /* a little taskbar */
        gfx_fill(g, mkrect(tx, ty + d->thumb_h - 5, d->thumb_w, 5), g_theme->taskbar_top);
        gfx_fill(g, mkrect(tx, ty + d->thumb_h - 5, 14, 5), HEX(0x3C9A3C));
    }
    gfx_fill(g, mkrect(bezel.x + bezel.w / 2 - 2, bezel.y + bezel.h - 9, 4, 4), HEX(0x7FD06A));   /* power light */
    gfx_fill(g, mkrect(mx - 26, bezel.y + bezel.h, 52, 8), HEX(0x4A505E));
    gfx_rounded(g, mkrect(mx - 50, bezel.y + bezel.h + 8, 100, 8), 3, 15, HEX(0x3A3F4B), HEX(0x3A3F4B));

    /* display line */
    char mon[64], line[160];
    edid_describe(display_monitor(), mon, sizeof mon);
    snprintf(line, sizeof line, "%s on %s", mon, display_adapter());
    gfx_text(g, &font_ui_bold, 16, 178, "Display:", HEX(0x000000));
    gfx_text_box(g, &font_ui, mkrect(16, 196, DW - 32, 16), line, HEX(0x000000), 0);

    draw_group(g, mkrect(12, 222, DW - 24, 170), "Screen resolution");
    gfx_text(g, &font_ui, 24, 356, "Color quality: Highest (32 bit)", HEX(0x000000));
    const char *note = display_live()
        ? "The new resolution is used as soon as you click Apply."
        : "Your display adapter takes the new resolution when Halcyon starts.";
    gfx_text_box(g, &font_small, mkrect(24, 374, DW - 48, 14), note, HEX(0x6A6A6A), 0);
}

static void update_buttons(window_t *w) {
    disp_t *d = w->data;
    display_mode_t m = display_mode(d->sel);
    widget_t *ap = widget_find(w, DP_APPLY);
    int pending = m.w && (m.w != d->applied_w || m.h != d->applied_h);
    if (ap) {
        if (pending) ap->style &= ~WS_DISABLED;
        else ap->style |= WS_DISABLED;
    }
    wm_invalidate(w);
}

/* ---- "Do you want to keep these settings?" ---- */
typedef struct keep { volatile int done; int keep; u64 deadline; window_t *w; } keep_t;
static void keep_click(window_t *w, widget_t *wd) {
    keep_t *k = w->data;
    k->keep = wd->id == ID_YES;
    k->done = 1;
}
static int keep_close(window_t *w) { keep_t *k = w->data; k->done = 1; return 0; }
static void keep_tick(window_t *w) {
    keep_t *k = w->data;
    u64 now = time_ms();
    if (now >= k->deadline) { k->done = 1; return; }
    widget_t *l = widget_find(w, 20);
    char t[64];
    snprintf(t, sizeof t, "Reverting in %d seconds.", (int)((k->deadline - now + 999) / 1000));
    if (l && strcmp(l->text, t)) widget_set_text(w, l, t);
}

static int ask_keep(window_t *owner) {
    keep_t k = { 0 };
    k.deadline = time_ms() + 15000;
    window_t *w = wm_create("Monitor Settings", ICON_DISPLAY, -1, -1, 330, 120, WF_DIALOG | WF_CENTER);
    if (!w) return 0;
    k.w = w;
    w->data = &k;
    w->on_tick = keep_tick;
    w->on_close = keep_close;
    widget_t *ic = widget_add(w, W_ICON, mkrect(14, 14, 32, 32), "", 0);
    ic->value = ICON_QUESTION;
    widget_add(w, W_LABEL, mkrect(58, 14, 260, 32), "Your desktop has been reconfigured. Do you want to keep these settings?", 0)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(58, 50, 260, 16), "Reverting in 15 seconds.", 20);
    widget_t *y = widget_add(w, W_BUTTON, mkrect(330 - 2 * 83, 86, 75, 23), "Yes", ID_YES);
    y->on_click = keep_click;
    widget_t *n = widget_add(w, W_BUTTON, mkrect(330 - 83, 86, 75, 23), "No", ID_NO);
    n->on_click = keep_click;
    n->style |= WS_DEFAULT | WS_CANCEL;   /* Enter on a black screen goes back */
    w->focus = n;
    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    gui_run_until(&k.done);
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    if (wm_alive(w)) { w->on_close = NULL; w->data = NULL; wm_destroy(w); }
    return k.keep;
}

static int apply(window_t *w) {
    disp_t *d = w->data;
    display_mode_t m = display_mode(d->sel);
    if (!m.w || (m.w == d->applied_w && m.h == d->applied_h)) return 1;
    if (!display_live()) {
        int r = display_save_mode(m.w, m.h);
        if (r) {
            char msg[160];
            snprintf(msg, sizeof msg, "The setting could not be saved (%s).", strerror(r));
            msgbox(w, "Display Properties", msg, ICON_ERROR, MB_OK);
            return 0;
        }
        d->applied_w = m.w; d->applied_h = m.h;
        update_buttons(w);
        if (msgbox(w, "System Settings Change",
                   "Your display adapter can only change the resolution when Halcyon starts.\n\n"
                   "Do you want to restart your computer now?", ICON_QUESTION, MB_YESNO) == ID_YES)
            desktop_restart();
        return 1;
    }
    int ow = screen_w(), oh = screen_h();
    wm_set_busy(1);
    int r = display_set_mode(m.w, m.h);
    wm_set_busy(0);
    if (r) {
        char msg[160];
        snprintf(msg, sizeof msg, "Halcyon could not switch to %d by %d pixels (%s).", m.w, m.h, strerror(r));
        msgbox(w, "Display Properties", msg, ICON_ERROR, MB_OK);
        return 0;
    }
    if (!ask_keep(w)) {
        display_set_mode(ow, oh);
        if (wm_alive(w)) {
            int i = display_find(ow, oh);
            widget_t *lb = widget_find(w, DP_LIST);
            if (i >= 0 && lb) { d->sel = lb->value = i; make_thumb(d); }
            update_buttons(w);
        }
        return 0;
    }
    display_save_mode(m.w, m.h);
    if (!wm_alive(w)) return 1;
    d->applied_w = m.w; d->applied_h = m.h;
    update_buttons(w);
    return 1;
}

static void on_ok(window_t *w, widget_t *b) { (void)b; if (apply(w) && wm_alive(w)) wm_close(w); }
static void on_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }
static void on_apply(window_t *w, widget_t *b) { (void)b; apply(w); }
static void on_select(window_t *w, widget_t *lb) {
    disp_t *d = w->data;
    if (lb->value < 0 || lb->value >= display_mode_count()) return;
    d->sel = lb->value;
    make_thumb(d);
    update_buttons(w);
}

static void dp_destroy(window_t *w) {
    disp_t *d = w->data;
    if (d) { if (d->thumb) surface_free(d->thumb); kfree(d); }
    w->data = NULL;
    if (dp_win == w) dp_win = NULL;
}

void app_display(void) {
    if (dp_win && wm_alive(dp_win)) { wm_show(dp_win); return; }
    disp_t *d = kzalloc(sizeof *d);
    if (!d) return;
    window_t *w = wm_create("Display Properties", ICON_DISPLAY, -1, -1, DW, DH, WF_NO_MAXIMIZE);
    if (w) w->help = "display";              /* F1 */
    if (!w) { kfree(d); return; }
    dp_win = w;
    w->data = d;
    w->on_paint = dp_paint;
    w->on_destroy = dp_destroy;

    /* the mode in use, or the one saved for the next start */
    d->applied_w = screen_w(); d->applied_h = screen_h();
    if (!display_live()) {
        int sw = settings_get_int("display.width", 0), sh = settings_get_int("display.height", 0);
        if (sw && sh && display_find(sw, sh) >= 0) { d->applied_w = sw; d->applied_h = sh; }
    }
    int n = display_mode_count(), nw = 0, nh = 0;
    display_native(&nw, &nh);
    char **items = kzalloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n && items; i++) {
        items[i] = kmalloc(64);
        if (!items[i]) continue;
        display_mode_t m = display_mode(i);
        mode_text(m, m.w == nw && m.h == nh, items[i], 64);
        if (m.w == d->applied_w && m.h == d->applied_h) d->sel = i;
    }
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(24, 244, DW - 48, 104), "", DP_LIST);
    if (items) widget_list_set(w, lb, items, n);
    for (int i = 0; i < n && items; i++) kfree(items[i]);
    kfree(items);
    lb->value = d->sel;
    lb->on_change = on_select;
    w->focus = lb;
    make_thumb(d);

    widget_t *ok = widget_add(w, W_BUTTON, mkrect(DW - 12 - 3 * 75 - 2 * 8, DH - 35, 75, 23), "OK", DP_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(DW - 12 - 2 * 75 - 8, DH - 35, 75, 23), "Cancel", DP_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(DW - 12 - 75, DH - 35, 75, 23), "Apply", DP_APPLY);
    ap->on_click = on_apply;
    update_buttons(w);
}
