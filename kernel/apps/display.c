/* Halcyon OS - Display Properties: the screen saver and the screen
 * resolution, on two tabs.
 *
 * Screen Saver: the installed savers (C:\PROGRAMS\*.SCR, by the display
 * names in their files) in a drop-down list, the chosen one running in the
 * little monitor ("/p <handle>": its window is drawn inside the box, see
 * gui/saver.c), Settings... ("/c"), Preview (full screen until input) and
 * the wait in minutes; Apply / OK keep them in SETTINGS.INI (screensaver.*).
 *
 * Settings: with an adapter that switches modes at once (display_live),
 * Apply changes the mode and asks "Do you want to keep these settings?" for
 * 15 seconds, going back if nobody answers (a mode the monitor cannot show
 * leaves the screen black). With only the firmware frame buffer, the choice
 * is saved and the loader sets it at the next start. */
#include "gui/wm.h"
#include "fonts.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/proc.h"
#include "core/settings.h"
#include "drivers/display.h"

#define DW 400
#define TY 36                      /* the page starts below the tabs */
#define DH (452 + TY - 8)
#define DP_LIST 10
#define DP_OK 11
#define DP_CANCEL 12
#define DP_APPLY 13
#define SS_SETTINGS 20
#define SS_PREVIEW 21
#define SS_WAIT 22
#define PREV_W 168
#define PREV_H 104
#define MON_Y (TY + 12)            /* the monitor's top */

enum { TAB_SAVER = 0, TAB_SETTINGS, NTABS };
static const char *const tab_names[NTABS] = { "Screen Saver", "Settings" };

typedef struct disp {
    int tab;
    int sel;                   /* mode index selected in the list */
    int applied_w, applied_h;  /* the mode in use (or saved for the next start) */
    surface_t *thumb;
    int thumb_w, thumb_h;
    /* the screen saver tab */
    saver_info_t savers[SAVER_MAX];
    int nsavers;
    int ss_sel;                /* -1 = (None) */
    int ss_wait;               /* minutes */
    int preview;               /* handle of the running preview, 0 = none */
    int preview_sel;           /* which saver it shows */
    u32 config_pid;            /* a "/c" dialog: the preview restarts when it ends */
    surface_t *desk;           /* the desktop in the monitor, for (None) */
} disp_t;

static window_t *dp_win;

static rect_t tab_rect(int i) {
    int x = 8;
    for (int k = 0; k < i; k++) x += gfx_text_width(&font_ui, tab_names[k]) + 22;
    return mkrect(x, 8, gfx_text_width(&font_ui, tab_names[i]) + 20, TY - 12);
}
static rect_t page_rect(void) { return mkrect(6, TY - 4, DW - 12, DH - TY - 40); }
static rect_t mon_screen(void) { return mkrect(DW / 2 - PREV_W / 2, MON_Y + 12, PREV_W, PREV_H); }
static rect_t combo_rect(void) { return mkrect(24, MON_Y + 186, 190, 22); }
static rect_t spin_rect(int up) { return mkrect(108, MON_Y + 218 + (up ? 0 : 11), 15, 11); }

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

/* the monitor around the preview screen (both tabs) */
static void draw_monitor(gc_t *g) {
    int mx = DW / 2, my = MON_Y;
    rect_t bezel = mkrect(mx - PREV_W / 2 - 14, my, PREV_W + 28, PREV_H + 30);
    gfx_rounded(g, bezel, 6, 15, HEX(0x3A3F4B), HEX(0x3A3F4B));
    gfx_rounded(g, rect_inset(bezel, 2), 5, 15, HEX(0x555B69), HEX(0x555B69));
    gfx_fill(g, mon_screen(), HEX(0x101318));
    gfx_fill(g, mkrect(bezel.x + bezel.w / 2 - 2, bezel.y + bezel.h - 9, 4, 4), HEX(0x7FD06A));   /* power light */
    gfx_fill(g, mkrect(mx - 26, bezel.y + bezel.h, 52, 8), HEX(0x4A505E));
    gfx_rounded(g, mkrect(mx - 50, bezel.y + bezel.h + 8, 100, 8), 3, 15, HEX(0x3A3F4B), HEX(0x3A3F4B));
}

static void draw_tabs(gc_t *g, disp_t *d) {
    rect_t pg = page_rect();
    color_t edge = HEX(0x919B9C);
    gfx_fill(g, pg, color_lighten(g_theme->face, 12));
    gfx_rect(g, pg, edge);
    for (int i = 0; i < NTABS; i++) {
        rect_t r = tab_rect(i);
        int on = i == d->tab;
        if (on) { r.y -= 2; r.h += 3; }
        gfx_gradient_v(g, r, HEX(0xFFFFFF), on ? color_lighten(g_theme->face, 12) : HEX(0xECEBE6));
        gfx_vline(g, r.x, r.y + 1, r.h - 1, edge);
        gfx_vline(g, r.x + r.w - 1, r.y + 1, r.h - 1, edge);
        gfx_hline(g, r.x + 1, r.y, r.w - 2, edge);
        if (on) {               /* the warm line on the chosen tab */
            gfx_hline(g, r.x + 1, r.y, r.w - 2, color_lighten(g_theme->hover, -30));
            gfx_hline(g, r.x, r.y + 1, r.w, g_theme->hover);
            gfx_hline(g, r.x, r.y + 2, r.w, color_lighten(g_theme->hover, 40));
            gfx_hline(g, r.x + 1, pg.y, r.w - 2, color_lighten(g_theme->face, 12));
        }
        gfx_text_box(g, &font_ui, mkrect(r.x, r.y + (on ? 2 : 1), r.w, r.h - 2), tab_names[i], HEX(0x000000), 1);
    }
}

static const char *saver_name(disp_t *d, int i) { return i >= 0 && i < d->nsavers ? d->savers[i].name : "(None)"; }

static void paint_saver_tab(window_t *w, gc_t *g, disp_t *d) {
    draw_monitor(g);
    rect_t scr = mon_screen();
    if (d->ss_sel < 0) {             /* no screen saver: the desktop */
        if (d->desk) gfx_blit(g, scr.x, scr.y, d->desk, mkrect(0, 0, d->desk->w, d->desk->h));
        gfx_fill(g, mkrect(scr.x, scr.y + scr.h - 5, scr.w, 5), g_theme->taskbar_top);
        gfx_fill(g, mkrect(scr.x, scr.y + scr.h - 5, 14, 5), HEX(0x3C9A3C));
    } else {                         /* black until the saver draws (its window is drawn over this) */
        gfx_fill(g, scr, HEX(0x000000));
    }
    draw_group(g, mkrect(14, MON_Y + 164, DW - 28, 128), "Screen saver");
    /* the drop-down list */
    rect_t c = combo_rect();
    gfx_sunken_field(g, c, HEX(0xFFFFFF));
    gfx_text_box(g, &font_ui, mkrect(c.x + 5, c.y, c.w - 26, c.h), saver_name(d, d->ss_sel), HEX(0x000000), 0);
    rect_t b = mkrect(c.x + c.w - 19, c.y + 2, 17, c.h - 4);
    gfx_rounded(g, b, 2, 15, color_lighten(g_theme->sidebar_top, 140), color_lighten(g_theme->sidebar_bottom, 60));
    gfx_rounded_outline(g, b, 2, 15, color_lighten(g_theme->sidebar_bottom, 20));
    for (int i = 0; i < 4; i++) gfx_hline(g, b.x + b.w / 2 - 3 + i, b.y + b.h / 2 - 1 + i, 7 - 2 * i, HEX(0x4D6185));
    /* Wait: [ N ] minutes */
    int y = MON_Y + 222;
    gfx_text(g, &font_ui, 24, y, "Wait:", HEX(0x000000));
    rect_t up = spin_rect(1), dn = spin_rect(0);
    for (int k = 0; k < 2; k++) {
        rect_t r = k ? dn : up;
        gfx_rounded(g, r, 2, 15, color_lighten(g_theme->sidebar_top, 140), color_lighten(g_theme->sidebar_bottom, 60));
        gfx_rounded_outline(g, r, 2, 15, color_lighten(g_theme->sidebar_bottom, 20));
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        for (int i = 0; i < 3; i++) gfx_hline(g, cx - i, k ? cy + 1 - i : cy - 1 + i, 2 * i + 1, HEX(0x4D6185));
    }
    gfx_text(g, &font_ui, 130, y, "minutes", HEX(0x000000));
    int dis = d->ss_sel < 0;
    gfx_text_wrap(g, &font_small, mkrect(24, MON_Y + 250, DW - 48, 34),
                  dis ? "No screen saver: the screen stays as it is." :
                        "It starts when the keyboard and mouse have not been used for that long. "
                        "Any key, click or mouse movement ends it.",
                  HEX(0x6A6A6A));
    draw_group(g, mkrect(14, MON_Y + 298, DW - 28, 86), "About screen savers");
    gfx_icon(g, 26, MON_Y + 318, &icons32[ICON_DISPLAY]);
    gfx_text_wrap(g, &font_ui, mkrect(66, MON_Y + 314, DW - 92, 66),
                  "Screen savers are programs in C:\\PROGRAMS whose names end in .SCR; double-click one to "
                  "see it full screen. Media Player and other full-screen programs keep it away while they play.",
                  HEX(0x404040));
    (void)w;
}

static void paint_settings_tab(window_t *w, gc_t *g, disp_t *d) {
    draw_monitor(g);
    rect_t scr = mon_screen();
    if (d->thumb) {
        int tx = scr.x + (PREV_W - d->thumb_w) / 2, ty = scr.y + (PREV_H - d->thumb_h) / 2;
        gfx_blit(g, tx, ty, d->thumb, mkrect(0, 0, d->thumb_w, d->thumb_h));
        /* a little taskbar */
        gfx_fill(g, mkrect(tx, ty + d->thumb_h - 5, d->thumb_w, 5), g_theme->taskbar_top);
        gfx_fill(g, mkrect(tx, ty + d->thumb_h - 5, 14, 5), HEX(0x3C9A3C));
    }
    /* display line */
    char mon[64], line[160];
    edid_describe(display_monitor(), mon, sizeof mon);
    snprintf(line, sizeof line, "%s on %s", mon, display_adapter());
    gfx_text(g, &font_ui_bold, 16, TY + 170, "Display:", HEX(0x000000));
    gfx_text_box(g, &font_ui, mkrect(16, TY + 188, DW - 32, 16), line, HEX(0x000000), 0);

    draw_group(g, mkrect(12, TY + 214, DW - 24, 170), "Screen resolution");
    gfx_text(g, &font_ui, 24, TY + 348, "Color quality: Highest (32 bit)", HEX(0x000000));
    const char *note = display_live()
        ? "The new resolution is used as soon as you click Apply."
        : "Your display adapter takes the new resolution when Halcyon starts.";
    gfx_text_box(g, &font_small, mkrect(24, TY + 366, DW - 48, 14), note, HEX(0x6A6A6A), 0);
    (void)w;
}

static void dp_paint(window_t *w, gc_t *g) {
    disp_t *d = w->data;
    gfx_fill(g, mkrect(0, 0, DW, DH), g_theme->face);
    draw_tabs(g, d);
    if (d->tab == TAB_SAVER) paint_saver_tab(w, g, d);
    else paint_settings_tab(w, g, d);
}

/* ---------------- the screen saver tab ---------------- */
static int saver_pending(disp_t *d) {
    const char *cur = settings_get("screensaver.file", "LOGO.SCR");
    const char *sel = d->ss_sel >= 0 ? d->savers[d->ss_sel].file : "";
    return strcasecmp(cur, sel) || settings_get_int("screensaver.wait", 10) != d->ss_wait;
}

static void preview_stop(disp_t *d) {
    if (d->preview) saver_preview_close(d->preview);
    d->preview = 0;
}

static void preview_start(window_t *w) {
    disp_t *d = w->data;
    preview_stop(d);
    d->preview_sel = d->ss_sel;
    if (d->tab != TAB_SAVER || d->ss_sel < 0) return;
    int h = saver_preview_open(w, mon_screen());
    if (h < 0) return;
    char path[96], args[24];
    snprintf(path, sizeof path, "C:/PROGRAMS/%s", d->savers[d->ss_sel].file);
    snprintf(args, sizeof args, "/p %d", h);
    int pid = proc_spawn(path, args, NULL, NULL);
    if (pid <= 0) { saver_preview_close(h); return; }
    saver_preview_set_pid(h, (u32)pid);
    d->preview = h;
}

static void update_buttons(window_t *w);
static void show_tab(window_t *w, int tab) {
    disp_t *d = w->data;
    d->tab = tab;
    for (widget_t *wd = w->widgets; wd; wd = wd->next) {
        int t = (wd->id == DP_LIST) ? TAB_SETTINGS : (wd->id >= SS_SETTINGS && wd->id <= SS_WAIT) ? TAB_SAVER : -1;
        if (t < 0) continue;
        if (t == tab) wd->style &= ~WS_HIDDEN;
        else wd->style |= WS_HIDDEN;
    }
    widget_t *f = widget_find(w, tab == TAB_SAVER ? SS_WAIT : DP_LIST);
    if (f) w->focus = f;
    if (tab == TAB_SAVER) preview_start(w);
    else preview_stop(d);
    update_buttons(w);
}

static void saver_widgets(window_t *w) {
    disp_t *d = w->data;
    widget_t *s = widget_find(w, SS_SETTINGS), *p = widget_find(w, SS_PREVIEW);
    int dis = d->ss_sel < 0;
    if (s) s->style = dis ? (s->style | WS_DISABLED) : (s->style & ~WS_DISABLED);
    if (p) p->style = dis ? (p->style | WS_DISABLED) : (p->style & ~WS_DISABLED);
}

static void set_wait(window_t *w, int m) {
    disp_t *d = w->data;
    d->ss_wait = CLAMP(m, 1, 999);
    widget_t *t = widget_find(w, SS_WAIT);
    char b[8];
    snprintf(b, sizeof b, "%d", d->ss_wait);
    if (t && strcmp(t->text, b)) widget_set_text(w, t, b);
    update_buttons(w);
}

static void wait_changed(window_t *w, widget_t *t) {
    disp_t *d = w->data;
    int v = 0, ok = t->text[0] != 0;
    for (const char *c = t->text; *c; c++) { if (!isdigit((u8)*c)) ok = 0; else v = v * 10 + (*c - '0'); }
    if (ok && v >= 1 && v <= 999) d->ss_wait = v;
    update_buttons(w);
}

static void select_saver(window_t *w, int i) {
    disp_t *d = w->data;
    d->ss_sel = i;
    saver_widgets(w);
    if (d->preview_sel != i || !d->preview) preview_start(w);
    update_buttons(w);
}

static void on_ss_settings(window_t *w, widget_t *b) {
    disp_t *d = w->data;
    (void)b;
    if (d->ss_sel < 0) return;
    char path[96];
    snprintf(path, sizeof path, "C:/PROGRAMS/%s", d->savers[d->ss_sel].file);
    int pid = shell_run(path, "/c");
    if (pid > 0) d->config_pid = (u32)pid;
}

static void on_ss_preview(window_t *w, widget_t *b) {
    disp_t *d = w->data;
    (void)b;
    if (d->ss_sel >= 0) saver_start(d->savers[d->ss_sel].file);
}

static menu_item_t ss_items[SAVER_MAX + 1];
static menu_t ss_menu = { "Screen saver", ss_items, 0 };
static void combo_open(window_t *w) {
    disp_t *d = w->data;
    int n = 0;
    ss_items[n++] = (menu_item_t){ "(None)", NULL, 1, d->ss_sel < 0 ? MI_CHECKED : 0, 0 };
    for (int i = 0; i < d->nsavers && n <= SAVER_MAX; i++)
        ss_items[n++] = (menu_item_t){ d->savers[i].name, NULL, 2 + i, d->ss_sel == i ? MI_CHECKED : 0, 0 };
    ss_menu.count = n;
    rect_t c = combo_rect(), cl = wm_client_rect(w);
    menu_popup(w, &ss_menu, cl.x + c.x, cl.y + c.y + c.h);
}

static void dp_menu(window_t *w, int id) {
    if (id == 1) select_saver(w, -1);
    else if (id >= 2) select_saver(w, id - 2);
}

static void dp_event(window_t *w, gui_event_t *e) {
    disp_t *d = w->data;
    if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_LEFT) {
        for (int i = 0; i < NTABS; i++)
            if (rect_contains(tab_rect(i), e->x, e->y) && i != d->tab) { show_tab(w, i); return; }
        if (d->tab == TAB_SAVER) {
            if (rect_contains(combo_rect(), e->x, e->y)) { combo_open(w); return; }
            if (rect_contains(spin_rect(1), e->x, e->y)) { set_wait(w, d->ss_wait + 1); return; }
            if (rect_contains(spin_rect(0), e->x, e->y)) { set_wait(w, d->ss_wait - 1); return; }
        }
    }
    if (e->type == GE_KEY && e->key.pressed && (e->key.mods & MOD_CTRL) && e->key.key == KEY_TAB) {
        show_tab(w, (d->tab + 1) % NTABS);   /* Ctrl+Tab: the next tab */
        return;
    }
    if (e->type == GE_KEY && e->key.pressed && d->tab == TAB_SAVER && w->focus && w->focus->id == SS_WAIT) {
        if (e->key.key == KEY_UP) set_wait(w, d->ss_wait + 1);
        if (e->key.key == KEY_DOWN) set_wait(w, d->ss_wait - 1);
    }
}

static void dp_tick(window_t *w) {
    disp_t *d = w->data;
    /* the settings dialog closed: show the saver with its new settings */
    if (d->config_pid && !proc_alive(d->config_pid)) {
        d->config_pid = 0;
        if (d->tab == TAB_SAVER) preview_start(w);
    }
    widgets_tick(w);
}

static int apply_saver(window_t *w) {
    disp_t *d = w->data;
    if (!saver_pending(d)) return 1;
    settings_set("screensaver.file", d->ss_sel >= 0 ? d->savers[d->ss_sel].file : "");
    settings_set_int("screensaver.wait", d->ss_wait);
    int r = settings_save();
    saver_reload();
    if (r) {
        char msg[120];
        snprintf(msg, sizeof msg, "The screen saver settings could not be saved (%s).", strerror(r));
        msgbox(w, "Display Properties", msg, ICON_ERROR, MB_OK);
        return 0;
    }
    update_buttons(w);
    return 1;
}

/* ---------------- the settings tab ---------------- */
static void update_buttons(window_t *w) {
    disp_t *d = w->data;
    display_mode_t m = display_mode(d->sel);
    widget_t *ap = widget_find(w, DP_APPLY);
    int pending = (m.w && (m.w != d->applied_w || m.h != d->applied_h)) || saver_pending(d);
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

static int apply_mode(window_t *w) {
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

static int apply(window_t *w) {
    if (!apply_saver(w)) return 0;
    return wm_alive(w) ? apply_mode(w) : 1;
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
    if (d) {
        preview_stop(d);
        if (d->thumb) surface_free(d->thumb);
        if (d->desk) surface_free(d->desk);
        kfree(d);
    }
    w->data = NULL;
    if (dp_win == w) dp_win = NULL;
}

void app_display(void) {
    if (dp_win && wm_alive(dp_win)) { wm_show(dp_win); return; }
    disp_t *d = kzalloc(sizeof *d);
    if (!d) return;
    window_t *w = wm_create("Display Properties", ICON_DISPLAY, -1, -1, DW, DH, WF_NO_MAXIMIZE);
    if (!w) { kfree(d); return; }
    dp_win = w;
    w->data = d;
    w->on_paint = dp_paint;
    w->on_destroy = dp_destroy;
    w->on_event = dp_event;
    w->on_menu = dp_menu;
    w->on_tick = dp_tick;

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
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(24, TY + 236, DW - 48, 104), "", DP_LIST);
    if (items) widget_list_set(w, lb, items, n);
    for (int i = 0; i < n && items; i++) kfree(items[i]);
    kfree(items);
    lb->value = d->sel;
    lb->top = MAX(0, MIN(d->sel - 2, n - 5));          /* the mode in use in view */
    lb->on_change = on_select;
    make_thumb(d);

    /* the screen saver tab */
    d->nsavers = saver_list(d->savers, SAVER_MAX);
    const char *cur = settings_get("screensaver.file", "LOGO.SCR");
    d->ss_sel = -1;
    for (int i = 0; i < d->nsavers; i++) if (!strcasecmp(d->savers[i].file, cur)) d->ss_sel = i;
    d->ss_wait = CLAMP(settings_get_int("screensaver.wait", 10), 1, 999);
    d->desk = surface_new(PREV_W, PREV_H);
    if (d->desk) desktop_wallpaper_render(desktop_wallpaper_style(), d->desk);
    widget_t *st = widget_add(w, W_BUTTON, mkrect(222, MON_Y + 185, 75, 23), "Settings", SS_SETTINGS);
    st->on_click = on_ss_settings;
    strlcpy(st->tip, "The screen saver's own settings", sizeof st->tip);
    widget_t *pv = widget_add(w, W_BUTTON, mkrect(303, MON_Y + 185, 75, 23), "Preview", SS_PREVIEW);
    pv->on_click = on_ss_preview;
    strlcpy(pv->tip, "Full screen until you move the mouse or press a key", sizeof pv->tip);
    char wb[8];
    snprintf(wb, sizeof wb, "%d", d->ss_wait);
    widget_t *wt = widget_add(w, W_TEXTBOX, mkrect(62, MON_Y + 218, 44, 22), wb, SS_WAIT);
    wt->on_change = wait_changed;
    saver_widgets(w);

    widget_t *ok = widget_add(w, W_BUTTON, mkrect(DW - 12 - 3 * 75 - 2 * 8, DH - 33, 75, 23), "OK", DP_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(DW - 12 - 2 * 75 - 8, DH - 33, 75, 23), "Cancel", DP_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(DW - 12 - 75, DH - 33, 75, 23), "Apply", DP_APPLY);
    ap->on_click = on_apply;
    show_tab(w, TAB_SETTINGS);
    wm_place_restore(w, "display");
}

/* Display Properties on its Screen Saver tab (the Command Prompt's "display saver") */
void app_display_saver(void) {
    app_display();
    if (dp_win && wm_alive(dp_win)) show_tab(dp_win, TAB_SAVER);
}
