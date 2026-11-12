/* Halcyon OS - Display Properties: the screen saver and the monitors, on
 * two tabs.
 *
 * Screen Saver: the installed savers (C:\PROGRAMS\*.SCR, by the display
 * names in their files) in a drop-down list, the chosen one running in the
 * little monitor ("/p <handle>": its window is drawn inside the box, see
 * gui/saver.c), Settings... ("/c"), Preview (full screen until input) and
 * the wait in minutes; Apply / OK keep them in SETTINGS.INI (screensaver.*).
 *
 * Settings (docs/DESIGN-M10-DISPLAY.md): a picture of the monitors as they
 * are arranged, numbered; drag one to put it elsewhere (it snaps to an edge
 * of the others), click one (or pick it in the Display list) to choose its
 * resolution from its own list (its EDID and the adapter's limits) and its
 * refresh rate (the Refresh rate drop-down: the rates the monitor lists at
 * that resolution and the adapter can drive; a new resolution takes the
 * monitor's preferred rate if it is offered there, else the highest; on
 * adapters that cannot change it the drop-down says "Default", disabled),
 * make it the primary monitor (the taskbar and the desktop icons) or extend
 * the desktop onto it. Identify shows each monitor's number on it. Apply sets
 * everything at once on adapters that switch live and asks "Do you want to
 * keep these settings?" for 15 seconds, going back if nobody answers (a
 * mode the monitor cannot show leaves it black); the arrangement is kept in
 * SETTINGS.INI (display.<connector>.*, display.primary). With only the
 * firmware frame buffer the choice is saved and the loader sets it at the
 * next start. */
#include "gui/wm.h"
#include "gui/layout.h"
#include "fonts.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/proc.h"
#include "core/settings.h"
#include "drivers/display.h"

#define DW 400
#define TY 36                      /* the page starts below the tabs */
#define DH (476 + TY)
#define DP_LIST 10
#define DP_OK 11
#define DP_CANCEL 12
#define DP_APPLY 13
#define DP_PRIMARY 14
#define DP_EXTEND 15
#define DP_IDENTIFY 16
#define DP_DRIVER 17               /* turn a native driver on again (the start guard) */
#define SS_SETTINGS 20
#define SS_PREVIEW 21
#define SS_WAIT 22
#define PREV_W 168
#define PREV_H 104
#define MON_Y (TY + 12)            /* the monitor's top */
#define MENU_MON 100               /* Display list: 100 + monitor */
#define MAXMON (DISPLAY_MAX_ADAPTERS * DISPLAY_MAX_CONNECTORS)
#define MENU_RATE (MENU_MON + MAXMON)   /* Refresh rate list: + the rate's index */
#define MAXRATES 16

enum { TAB_SAVER = 0, TAB_SETTINGS, NTABS };
static const char *const tab_names[NTABS] = { "Screen Saver", "Settings" };

/* a monitor as Display Properties shows it: the settings it would get */
typedef struct dmon {
    display_connector_t *conn;
    int attach, w, h, hz, x, y;    /* pending */
    rect_t box;                    /* in the arrangement picture (client coordinates) */
    surface_t *thumb;              /* its wallpaper, box sized */
} dmon_t;

typedef struct disp {
    int tab;
    /* the settings tab */
    dmon_t mon[MAXMON];
    int nmon;                      /* the connected (or attached) ones come first */
    int npic;                      /* how many are in the picture */
    int msel;                      /* the selected one */
    int size_mode[DISPLAY_MAX_MODES];   /* the resolution list's rows: a mode of each size */
    int nsizes;
    display_connector_t *primary;  /* pending */
    display_conf_t applied;        /* in use (or saved for the next start) */
    int scale256, offx, offy;      /* desktop -> picture: x * scale / 256 + off */
    int drag, drag_i, drag_dx, drag_dy, drag_moved, drag_x0, drag_y0;
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
/* the settings tab */
static rect_t arr_rect(void) { return mkrect(16, TY + 34, DW - 32, 136); }
static rect_t dcombo_rect(void) { return mkrect(16, TY + 202, DW - 32, 22); }
static rect_t rate_rect(void) { return mkrect(104, TY + 332, 112, 22); }

/* the monitor around the preview screen (the screen saver tab) */
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


/* ---------------- the settings tab: monitors ---------------- */
static void mon_name(dmon_t *m, char *out, int n) {
    char mn[64];
    display_connector_t *c = m->conn;
    if (c->status == CONN_DISCONNECTED && !m->attach) strlcpy(mn, "(no monitor connected)", sizeof mn);
    else edid_describe(&c->monitor, mn, sizeof mn);
    snprintf(out, (size_t)n, "%d. %s on %s", c->number, mn, c->adapter->name);
}

static int mon_index(disp_t *d, display_connector_t *c) {
    for (int i = 0; i < d->nmon; i++) if (d->mon[i].conn == c) return i;
    return -1;
}

/* the monitors from a configuration */
static void load_monitors(disp_t *d, const display_conf_t *cf) {
    for (int i = 0; i < d->nmon; i++) if (d->mon[i].thumb) surface_free(d->mon[i].thumb);
    d->nmon = 0;
    /* connected or attached ones first (in the picture), the others after (in the list) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < cf->n && d->nmon < MAXMON; i++) {
            const display_conf_out_t *o = &cf->out[i];
            int shown = o->attach || o->conn->status != CONN_DISCONNECTED;
            if (shown != (pass == 0) || !o->conn->nmodes) continue;
            dmon_t *m = &d->mon[d->nmon++];
            memset(m, 0, sizeof *m);
            m->conn = o->conn;
            m->attach = o->attach;
            m->w = o->w; m->h = o->h; m->hz = o->hz;
            m->x = o->x; m->y = o->y;
            if (pass == 0) d->npic = d->nmon;
        }
    d->primary = cf->primary;
    if (d->msel >= d->nmon) d->msel = 0;
}

static void conf_from(disp_t *d, display_conf_t *cf) {
    display_get_conf(cf);
    for (int i = 0; i < cf->n; i++) {
        int k = mon_index(d, cf->out[i].conn);
        if (k < 0) continue;
        dmon_t *m = &d->mon[k];
        cf->out[i].attach = m->attach;
        cf->out[i].w = m->w; cf->out[i].h = m->h; cf->out[i].hz = m->hz;
        cf->out[i].x = m->x; cf->out[i].y = m->y;
    }
    cf->primary = d->primary;
}

static int conf_differs(const display_conf_t *a, const display_conf_t *b) {
    if (a->primary != b->primary) return 1;
    for (int i = 0; i < a->n; i++)
        for (int k = 0; k < b->n; k++) {
            if (a->out[i].conn != b->out[k].conn) continue;
            const display_conf_out_t *p = &a->out[i], *q = &b->out[k];
            if (p->attach != q->attach ||
                (p->attach && (p->w != q->w || p->h != q->h || p->hz != q->hz || p->x != q->x || p->y != q->y))) return 1;
        }
    return 0;
}

/* the refresh rates of a monitor's pending resolution; whether it offers a
 * choice (an adapter that programs the timing and switches at once) */
static int mon_rates(const dmon_t *m, int *hz, int max) {
    return modes_rates(m->conn->modes, m->conn->nmodes, m->w, m->h, hz, max);
}
static int rate_choice(const dmon_t *m) {
    int hz[MAXRATES];
    return m && m->conn->adapter->rates && m->conn->adapter->live && mon_rates(m, hz, MAXRATES) > 0;
}

/* the attached monitors in a valid arrangement (they touch, nothing
 * overlaps); the primary keeps its place */
static void arrange(disp_t *d) {
    rect_t r[LAYOUT_MAX];
    int idx[LAYOUT_MAX], n = 0, p = -1;
    for (int i = 0; i < d->npic && n < LAYOUT_MAX; i++) {
        if (!d->mon[i].attach) continue;
        if (d->mon[i].conn == d->primary) p = n;
        idx[n] = i;
        r[n++] = mkrect(d->mon[i].x, d->mon[i].y, d->mon[i].w, d->mon[i].h);
    }
    layout_normalize(r, n, p < 0 ? 0 : p, NULL, NULL);
    for (int k = 0; k < n; k++) { d->mon[idx[k]].x = r[k].x; d->mon[idx[k]].y = r[k].y; }
    /* the others to the right of them, where they would go */
    rect_t b = layout_bounds(r, n);
    int x = b.x + b.w + 24;
    for (int i = 0; i < d->npic; i++)
        if (!d->mon[i].attach) { d->mon[i].x = x; d->mon[i].y = b.y; x += d->mon[i].w + 24; }
}

/* desktop -> picture scale, fitting every monitor in the box */
static void picture_scale(disp_t *d) {
    rect_t a = arr_rect(), b = mkrect(0, 0, 0, 0);
    for (int i = 0; i < d->npic; i++) b = rect_union(b, mkrect(d->mon[i].x, d->mon[i].y, d->mon[i].w, d->mon[i].h));
    if (rect_empty(b)) b = mkrect(0, 0, 1024, 768);
    int sx = (a.w - 40) * 256 / b.w, sy = (a.h - 28) * 256 / b.h;
    d->scale256 = MAX(4, MIN(MIN(sx, sy), 256 * 120 / 1024));      /* a single monitor: about 120 pixels wide */
    d->offx = a.x + (a.w - b.w * d->scale256 / 256) / 2 - b.x * d->scale256 / 256;
    d->offy = a.y + (a.h - b.h * d->scale256 / 256) / 2 - b.y * d->scale256 / 256;
}

static void place_boxes(disp_t *d) {
    for (int i = 0; i < d->npic; i++) {
        dmon_t *m = &d->mon[i];
        if (d->drag && i == d->drag_i) continue;
        rect_t nb = mkrect(d->offx + m->x * d->scale256 / 256, d->offy + m->y * d->scale256 / 256,
                           MAX(12, m->w * d->scale256 / 256), MAX(10, m->h * d->scale256 / 256));
        if (nb.w != m->box.w || nb.h != m->box.h) { surface_free(m->thumb); m->thumb = NULL; }
        m->box = nb;
    }
}

static void refresh_picture(disp_t *d) {
    picture_scale(d);
    place_boxes(d);
}

static void draw_mon(gc_t *g, disp_t *d, int i) {
    dmon_t *m = &d->mon[i];
    rect_t b = m->box, s = rect_inset(b, 3);
    int sel = i == d->msel;
    if (m->attach) {
        if (!m->thumb && s.w > 2 && s.h > 2) {
            m->thumb = surface_new(s.w, s.h);
            if (m->thumb) desktop_wallpaper_render(desktop_wallpaper_style(), m->thumb);
        }
        gfx_fill(g, b, HEX(0x1E2430));
        if (m->thumb) gfx_blit(g, s.x, s.y, m->thumb, mkrect(0, 0, m->thumb->w, m->thumb->h));
        if (m->conn == d->primary)              /* the taskbar */
            gfx_fill(g, mkrect(s.x, s.y + s.h - MAX(2, s.h / 14), s.w, MAX(2, s.h / 14)), g_theme->taskbar_top);
    } else {
        /* not part of the desktop: a dim, hatched outline */
        gfx_fill(g, b, HEX(0xD4D2CA));
        gc_t hg = *g;
        hg.clip = rect_intersect(g->clip, mkrect(b.x + g->ox, b.y + g->oy, b.w, b.h));
        for (int k = -b.h; k < b.w; k += 6) gfx_line(&hg, b.x + k, b.y + b.h - 1, b.x + k + b.h - 1, b.y, HEX(0xC4C2BA));
        gfx_rect(g, b, HEX(0x8C8A82));
    }
    gfx_rect(g, b, sel ? g_theme->select : HEX(0x3A3F4B));
    if (sel) gfx_rect(g, rect_inset(b, 1), g_theme->select);
    /* the number */
    char num[8];
    snprintf(num, sizeof num, "%d", m->conn->number);
    int scale = CLAMP(b.h * 16 / 60, 8, 32);
    int tw = gfx_text_scaled_width(&font_huge, num, scale), th = font_huge.height * scale / 16;
    int tx = b.x + (b.w - tw) / 2, ty = b.y + (b.h - th) / 2;
    if (m->attach) {
        gfx_text_scaled(g, &font_huge, tx + 1, ty + 1, num, HEX(0x000000), scale);
        gfx_text_scaled(g, &font_huge, tx, ty, num, HEX(0xFFFFFF), scale);
    } else {
        gfx_text_scaled(g, &font_huge, tx, ty, num, HEX(0x8C8A82), scale);
    }
}

static void paint_settings_tab(window_t *w, gc_t *g, disp_t *d) {
    gfx_text_wrap(g, &font_ui, mkrect(16, TY + 4, DW - 32, 30),
                  d->npic > 1 ? "Drag the monitor icons to match the physical arrangement of your monitors."
                              : "This is the monitor Halcyon shows its desktop on. Choose its resolution below.",
                  HEX(0x000000));
    rect_t a = arr_rect();
    gfx_sunken_field(g, a, color_lighten(g_theme->face, 30));
    gc_t ag = gc_sub(g, mkrect(0, 0, DW, DH));
    ag.clip = rect_intersect(ag.clip, rect_inset(mkrect(a.x + ag.ox, a.y + ag.oy, a.w, a.h), 2));
    for (int i = 0; i < d->npic; i++) if (i != d->msel) draw_mon(&ag, d, i);
    if (d->msel < d->npic) draw_mon(&ag, d, d->msel);         /* the selected one on top */
    /* the Display list */
    gfx_text(g, &font_ui_bold, 16, TY + 184, "Display:", HEX(0x000000));
    rect_t c = dcombo_rect();
    gfx_sunken_field(g, c, HEX(0xFFFFFF));
    char line[160] = "";
    if (d->msel < d->nmon) mon_name(&d->mon[d->msel], line, sizeof line);
    gfx_text_box(g, &font_ui, mkrect(c.x + 5, c.y, c.w - 26, c.h), line, HEX(0x000000), 0);
    rect_t bt = mkrect(c.x + c.w - 19, c.y + 2, 17, c.h - 4);
    gfx_rounded(g, bt, 2, 15, color_lighten(g_theme->sidebar_top, 140), color_lighten(g_theme->sidebar_bottom, 60));
    gfx_rounded_outline(g, bt, 2, 15, color_lighten(g_theme->sidebar_bottom, 20));
    for (int i = 0; i < 4; i++) gfx_hline(g, bt.x + bt.w / 2 - 3 + i, bt.y + bt.h / 2 - 1 + i, 7 - 2 * i, HEX(0x4D6185));

    draw_group(g, mkrect(12, TY + 232, DW - 24, 128), "Screen resolution");
    dmon_t *m = d->msel < d->nmon ? &d->mon[d->msel] : NULL;
    /* Refresh rate: [120 Hertz v]   (disabled "Default" where the adapter cannot change it) */
    int on = rate_choice(m);
    rect_t rr = rate_rect();
    gfx_text(g, &font_ui, 24, rr.y + 4, "Refresh rate:", on ? HEX(0x000000) : HEX(0x8C8A82));
    gfx_sunken_field(g, rr, on ? HEX(0xFFFFFF) : color_lighten(g_theme->face, 12));
    char rt[24] = "Default";
    if (on && m->hz) snprintf(rt, sizeof rt, "%d Hertz", m->hz);
    gfx_text_box(g, &font_ui, mkrect(rr.x + 5, rr.y, rr.w - 26, rr.h), rt, on ? HEX(0x000000) : HEX(0x8C8A82), 0);
    rect_t rb = mkrect(rr.x + rr.w - 19, rr.y + 2, 17, rr.h - 4);
    gfx_rounded(g, rb, 2, 15, color_lighten(g_theme->sidebar_top, on ? 140 : 160), color_lighten(g_theme->sidebar_bottom, on ? 60 : 100));
    gfx_rounded_outline(g, rb, 2, 15, color_lighten(g_theme->sidebar_bottom, on ? 20 : 80));
    for (int i = 0; i < 4; i++)
        gfx_hline(g, rb.x + rb.w / 2 - 3 + i, rb.y + rb.h / 2 - 1 + i, 7 - 2 * i, on ? HEX(0x4D6185) : HEX(0xA8A8A8));
    gfx_text(g, &font_ui, rr.x + rr.w + 14, rr.y + 4, "Colors: Highest (32 bit)", HEX(0x000000));
    const char *note = "The new settings are used as soon as you click Apply.";
    if (m && !m->conn->adapter->live) note = "This display adapter takes a new resolution when Halcyon starts.";
    gfx_text_box(g, &font_small, mkrect(16, TY + 414, DW - 32, 14), note, HEX(0x6A6A6A), 0);
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
static int settings_event(window_t *w, gui_event_t *e);
static void select_monitor(window_t *w, int i);
static void show_tab(window_t *w, int tab) {
    disp_t *d = w->data;
    d->tab = tab;
    d->drag = 0;
    for (widget_t *wd = w->widgets; wd; wd = wd->next) {
        int t = (wd->id >= DP_PRIMARY && wd->id <= DP_DRIVER) || wd->id == DP_LIST ? TAB_SETTINGS :
                (wd->id >= SS_SETTINGS && wd->id <= SS_WAIT) ? TAB_SAVER : -1;
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


static void select_rate(window_t *w, int k);
static void dp_menu(window_t *w, int id) {
    if (id >= MENU_RATE) { select_rate(w, id - MENU_RATE); return; }
    if (id >= MENU_MON) { select_monitor(w, id - MENU_MON); return; }
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
    if (d->tab == TAB_SETTINGS && settings_event(w, e)) return;
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

/* ---- "Do you want to keep these settings?" ---- */
typedef struct keep { volatile int done; int keep; u64 deadline; window_t *w; } keep_t;
static void keep_click(window_t *w, widget_t *wd) {
    keep_t *k = w->data;
    k->keep = wd->id == ID_YES;
    k->done = 1;
}
static int keep_close(window_t *w) { keep_t *k = w->data; k->done = 1; return 0; }
static int keep_auto_ms;               /* the test hook answers Yes after this long */
static void keep_tick(window_t *w) {
    keep_t *k = w->data;
    u64 now = time_ms();
    if (now >= k->deadline) { k->done = 1; return; }
    if (keep_auto_ms && now + 15000 >= k->deadline + (u64)keep_auto_ms) {
        kprintf("display test: \"keep these settings?\" answered Yes\n");
        k->keep = 1;
        k->done = 1;
        return;
    }
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


/* ---- a native driver's first picture: "can you see this?" ---- */
int display_ask_native(const char *adapter) {
    keep_t k = { 0 };
    k.deadline = time_ms() + 15000;
    window_t *w = wm_create("Display Driver", ICON_DISPLAY, -1, -1, 360, 132, WF_DIALOG | WF_CENTER | WF_TOPMOST);
    if (!w) return 0;
    k.w = w;
    w->data = &k;
    w->on_tick = keep_tick;
    w->on_close = keep_close;
    widget_t *ic = widget_add(w, W_ICON, mkrect(14, 14, 32, 32), "", 0);
    ic->value = ICON_QUESTION;
    char t[200];
    snprintf(t, sizeof t, "Halcyon now uses its own driver for the %s. Can you see this message?", adapter);
    widget_add(w, W_LABEL, mkrect(58, 14, 290, 44), t, 0)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(58, 62, 290, 16), "Reverting in 15 seconds.", 20);
    widget_t *y = widget_add(w, W_BUTTON, mkrect(360 - 2 * 83, 98, 75, 23), "Yes", ID_YES);
    y->on_click = keep_click;
    widget_t *n = widget_add(w, W_BUTTON, mkrect(360 - 83, 98, 75, 23), "No", ID_NO);
    n->on_click = keep_click;
    n->style |= WS_DEFAULT | WS_CANCEL;   /* Enter on a black screen goes back */
    w->focus = n;
    gui_run_until(&k.done);
    if (wm_alive(w)) { w->on_close = NULL; w->data = NULL; wm_destroy(w); }
    return k.keep;
}

/* the native driver a guard turned off (NULL: none) */
static display_adapter_t *guard_off_adapter(void) {
    for (int i = 0; i < display_adapter_count(); i++) {
        display_adapter_t *a = display_adapter_at(i);
        if (a->guarded && display_guard_state(a->driver)) return a;
    }
    return NULL;
}

static void on_driver(window_t *w, widget_t *b) {
    (void)b;
    display_adapter_t *a = guard_off_adapter();
    if (!a) return;
    char msg[400];
    snprintf(msg, sizeof msg, "Halcyon's own driver for the %s is %s.\n\n"
             "Do you want to turn it on again? It starts with the next restart. If the screen stays dark, "
             "it turns itself off again.", a->name, display_guard_state(a->driver));
    if (msgbox(w, "Display Driver", msg, ICON_QUESTION, MB_YESNO) != ID_YES) return;
    display_guard_reenable(a->driver);
    widget_t *bt = widget_find(w, DP_DRIVER);
    if (bt) bt->style |= WS_DISABLED;
    if (msgbox(w, "System Settings Change", "The driver is used from the next start.\n\nDo you want to restart your computer now?",
               ICON_QUESTION, MB_YESNO) == ID_YES)
        desktop_restart();
}

/* ---------------- the settings tab: choosing ---------------- */
static void update_buttons(window_t *w) {
    disp_t *d = w->data;
    display_conf_t cf;
    conf_from(d, &cf);
    widget_t *ap = widget_find(w, DP_APPLY);
    int pending = conf_differs(&cf, &d->applied) || saver_pending(d);
    if (ap) {
        if (pending) ap->style &= ~WS_DISABLED;
        else ap->style |= WS_DISABLED;
    }
    wm_invalidate(w);
}

/* a row of the resolution list: a size ("recommended": the preferred mode's) */
static void size_text(const display_connector_t *c, const display_mode_t *m, char *out, int n) {
    int p = modes_preferred(c->modes, c->nmodes);
    int rec = p >= 0 && c->modes[p].w == m->w && c->modes[p].h == m->h;
    snprintf(out, (size_t)n, "%d by %d pixels%s", m->w, m->h, rec ? "  (recommended)" : "");
}

/* the widgets show the selected monitor */
static void select_monitor(window_t *w, int i) {
    disp_t *d = w->data;
    if (i < 0 || i >= d->nmon) return;
    d->msel = i;
    dmon_t *m = &d->mon[i];
    display_connector_t *c = m->conn;
    /* one row per size (the list has a mode per size and refresh rate) */
    d->nsizes = 0;
    for (int k = 0; k < c->nmodes && d->nsizes < DISPLAY_MAX_MODES; k++)
        if (!k || c->modes[k - 1].w != c->modes[k].w || c->modes[k - 1].h != c->modes[k].h) d->size_mode[d->nsizes++] = k;
    widget_t *lb = widget_find(w, DP_LIST);
    if (lb) {
        char **items = kzalloc(sizeof(char *) * (size_t)(d->nsizes + 1));
        int sel = 0;
        for (int k = 0; k < d->nsizes && items; k++) {
            const display_mode_t *md = &c->modes[d->size_mode[k]];
            items[k] = kmalloc(64);
            if (items[k]) size_text(c, md, items[k], 64);
            if (md->w == m->w && md->h == m->h) sel = k;
        }
        if (items) widget_list_set(w, lb, items, d->nsizes);
        for (int k = 0; k < d->nsizes && items; k++) kfree(items[k]);
        kfree(items);
        lb->value = sel;
        lb->top = MAX(0, MIN(sel - 2, d->nsizes - 4));      /* the mode in use in view */
    }
    widget_t *p = widget_find(w, DP_PRIMARY), *x = widget_find(w, DP_EXTEND);
    int is_primary = c == d->primary, gone = c->status == CONN_DISCONNECTED && !m->attach;
    if (p) {
        p->value = is_primary;
        if (is_primary || gone) p->style |= WS_DISABLED; else p->style &= ~WS_DISABLED;
    }
    if (x) {
        x->value = m->attach;
        if (is_primary || gone) x->style |= WS_DISABLED; else x->style &= ~WS_DISABLED;
    }
    update_buttons(w);
}

/* a resolution chosen: the monitor's preferred refresh rate if it is
 * offered at that size, else the highest there (modes_find) */
static void on_select(window_t *w, widget_t *lb) {
    disp_t *d = w->data;
    if (d->msel >= d->nmon) return;
    dmon_t *m = &d->mon[d->msel];
    if (lb->value < 0 || lb->value >= d->nsizes) return;
    const display_mode_t *md = &m->conn->modes[d->size_mode[lb->value]];
    int same = m->w == md->w && m->h == md->h;
    m->w = md->w;
    m->h = md->h;
    int k = modes_find(m->conn->modes, m->conn->nmodes, m->w, m->h);
    if (!same && k >= 0) m->hz = m->conn->modes[k].hz;
    arrange(d);
    refresh_picture(d);
    update_buttons(w);
}

/* the Refresh rate drop-down */
static menu_item_t rate_items[MAXRATES];
static menu_t rate_menu = { "Refresh rate", rate_items, 0 };
static char rate_text[MAXRATES][24];
static void rate_open(window_t *w) {
    disp_t *d = w->data;
    dmon_t *m = d->msel < d->nmon ? &d->mon[d->msel] : NULL;
    if (!rate_choice(m)) return;
    int hz[MAXRATES], n = mon_rates(m, hz, MAXRATES);
    for (int i = 0; i < n; i++) {
        snprintf(rate_text[i], sizeof rate_text[i], "%d Hertz", hz[i]);
        rate_items[i] = (menu_item_t){ rate_text[i], NULL, MENU_RATE + i, hz[i] == m->hz ? MI_CHECKED : 0, 0 };
    }
    rate_menu.count = n;
    rect_t r = rate_rect(), cl = wm_client_rect(w);
    menu_popup(w, &rate_menu, cl.x + r.x, cl.y + r.y + r.h);
}

static void select_rate(window_t *w, int k) {
    disp_t *d = w->data;
    if (d->msel >= d->nmon) return;
    dmon_t *m = &d->mon[d->msel];
    int hz[MAXRATES], n = mon_rates(m, hz, MAXRATES);
    if (k < 0 || k >= n || !rate_choice(m)) return;
    m->hz = hz[k];
    update_buttons(w);
}

static void on_primary(window_t *w, widget_t *cb) {
    disp_t *d = w->data;
    if (d->msel >= d->nmon) return;
    dmon_t *m = &d->mon[d->msel];
    if (cb->value) {
        d->primary = m->conn;
        m->attach = 1;
    }
    arrange(d);
    refresh_picture(d);
    select_monitor(w, d->msel);
}

static void on_extend(window_t *w, widget_t *cb) {
    disp_t *d = w->data;
    if (d->msel >= d->nmon) return;
    dmon_t *m = &d->mon[d->msel];
    if (m->conn == d->primary) { cb->value = 1; return; }
    m->attach = cb->value;
    if (m->attach) {
        /* it joins on the right of the others */
        rect_t b = mkrect(0, 0, 0, 0);
        for (int i = 0; i < d->npic; i++) if (d->mon[i].attach && i != d->msel) b = rect_union(b, mkrect(d->mon[i].x, d->mon[i].y, d->mon[i].w, d->mon[i].h));
        m->x = b.x + b.w; m->y = b.y;
    }
    arrange(d);
    refresh_picture(d);
    select_monitor(w, d->msel);
}

/* ---- Identify: each monitor's number on it for a few seconds ---- */
typedef struct ident { char num[8]; u64 until; } ident_t;
static void ident_paint(window_t *w, gc_t *g) {
    ident_t *id = w->data;
    rect_t r = mkrect(0, 0, w->client->w, w->client->h);
    gfx_gradient_v(g, r, HEX(0x3D7BE0), HEX(0x1B48A8));
    gfx_rect(g, r, HEX(0x0A1E4A));
    gfx_rect(g, rect_inset(r, 1), HEX(0xFFFFFF));
    int scale = 72;
    int tw = gfx_text_scaled_width(&font_huge, id->num, scale), th = font_huge.height * scale / 16;
    int tx = (r.w - tw) / 2, ty = (r.h - th) / 2;
    gfx_text_scaled(g, &font_huge, tx + 4, ty + 4, id->num, HEX(0x0A1E4A), scale);
    gfx_text_scaled(g, &font_huge, tx, ty, id->num, HEX(0xFFFFFF), scale);
}
static void ident_tick(window_t *w) {
    ident_t *id = w->data;
    if (time_ms() >= id->until) wm_destroy(w);
}
static void ident_event(window_t *w, gui_event_t *e) { if (e->type == GE_MOUSE_DOWN) wm_destroy(w); }
static void ident_destroy(window_t *w) { kfree(w->data); w->data = NULL; }

static void on_identify(window_t *w, widget_t *b) {
    (void)w; (void)b;
    display_output_t o[DISPLAY_MAX_OUTPUTS];
    int n = display_outputs(o, DISPLAY_MAX_OUTPUTS);
    for (int i = 0; i < n; i++) {
        ident_t *id = kzalloc(sizeof *id);
        if (!id) continue;
        snprintf(id->num, sizeof id->num, "%d", o[i].conn->number);
        id->until = time_ms() + 3000;
        int ww = 200, wh = 220;
        window_t *iw = wm_create("", -1, o[i].x + 48, o[i].y + (o[i].h - wh) / 3, ww, wh, WF_POPUP | WF_TOPMOST);
        if (!iw) { kfree(id); continue; }
        iw->data = id;
        iw->on_paint = ident_paint;
        iw->on_tick = ident_tick;
        iw->on_event = ident_event;
        iw->on_destroy = ident_destroy;
    }
    kprintf("display: identify: %d monitor%s\n", n, n == 1 ? "" : "s");
}

/* ---- the picture: click to select, drag to arrange ---- */
static int pic_hit(disp_t *d, int x, int y) {
    if (d->msel < d->npic && rect_contains(d->mon[d->msel].box, x, y)) return d->msel;
    for (int i = d->npic - 1; i >= 0; i--) if (rect_contains(d->mon[i].box, x, y)) return i;
    return -1;
}

static void pic_drop(window_t *w) {
    disp_t *d = w->data;
    dmon_t *m = &d->mon[d->drag_i];
    d->drag = 0;
    if (!d->drag_moved) { refresh_picture(d); return; }
    /* picture -> desktop, then onto an edge of the others */
    m->x = (m->box.x - d->offx) * 256 / d->scale256;
    m->y = (m->box.y - d->offy) * 256 / d->scale256;
    if (!m->attach && m->conn->status != CONN_DISCONNECTED) m->attach = 1;   /* dragged next to them: extend onto it */
    if (m->attach) {
        rect_t r[LAYOUT_MAX];
        int idx[LAYOUT_MAX], n = 0, me = -1;
        for (int i = 0; i < d->npic && n < LAYOUT_MAX; i++) {
            if (!d->mon[i].attach) continue;
            if (i == d->drag_i) me = n;
            idx[n] = i;
            r[n++] = mkrect(d->mon[i].x, d->mon[i].y, d->mon[i].w, d->mon[i].h);
        }
        if (me >= 0) layout_snap(r, n, me);
        for (int k = 0; k < n; k++) { d->mon[idx[k]].x = r[k].x; d->mon[idx[k]].y = r[k].y; }
    }
    arrange(d);
    refresh_picture(d);
    select_monitor(w, d->drag_i);
}

static menu_item_t dm_items[MAXMON];
static menu_t dm_menu = { "Display", dm_items, 0 };
static char dm_text[MAXMON][96];
static void dcombo_open(window_t *w) {
    disp_t *d = w->data;
    int n = 0;
    for (int i = 0; i < d->nmon && n < MAXMON; i++) {
        mon_name(&d->mon[i], dm_text[n], sizeof dm_text[n]);
        dm_items[n] = (menu_item_t){ dm_text[n], NULL, MENU_MON + i, d->msel == i ? MI_CHECKED : 0, 0 };
        n++;
    }
    dm_menu.count = n;
    rect_t c = dcombo_rect(), cl = wm_client_rect(w);
    menu_popup(w, &dm_menu, cl.x + c.x, cl.y + c.y + c.h);
}

static int settings_event(window_t *w, gui_event_t *e) {
    disp_t *d = w->data;
    if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_LEFT) {
        if (rect_contains(dcombo_rect(), e->x, e->y)) { dcombo_open(w); return 1; }
        if (rect_contains(rate_rect(), e->x, e->y)) { rate_open(w); return 1; }
        int i = pic_hit(d, e->x, e->y);
        if (i >= 0) {
            select_monitor(w, i);
            d->drag = 1;
            d->drag_i = i;
            d->drag_moved = 0;
            d->drag_dx = e->x - d->mon[i].box.x;
            d->drag_dy = e->y - d->mon[i].box.y;
            d->drag_x0 = e->x; d->drag_y0 = e->y;
            return 1;
        }
    }
    if (e->type == GE_MOUSE_MOVE && d->drag) {
        dmon_t *m = &d->mon[d->drag_i];
        if (!d->drag_moved && (e->x - d->drag_x0) * (e->x - d->drag_x0) + (e->y - d->drag_y0) * (e->y - d->drag_y0) < 9) return 1;
        d->drag_moved = 1;
        rect_t a = rect_inset(arr_rect(), 2);
        m->box.x = CLAMP(e->x - d->drag_dx, a.x - m->box.w / 2, a.x + a.w - m->box.w / 2);
        m->box.y = CLAMP(e->y - d->drag_dy, a.y - m->box.h / 2, a.y + a.h - m->box.h / 2);
        wm_invalidate(w);
        return 1;
    }
    if (e->type == GE_MOUSE_UP && d->drag) { pic_drop(w); return 1; }
    return 0;
}

/* ---- Apply ---- */
static int apply_display(window_t *w) {
    disp_t *d = w->data;
    display_conf_t cf;
    conf_from(d, &cf);
    if (!conf_differs(&cf, &d->applied)) return 1;
    if (!display_conf_live(&cf)) {
        int r = display_save_conf(&cf);
        if (r) {
            char msg[160];
            snprintf(msg, sizeof msg, "The settings could not be saved (%s).", strerror(r));
            msgbox(w, "Display Properties", msg, ICON_ERROR, MB_OK);
            return 0;
        }
        d->applied = cf;
        update_buttons(w);
        if (msgbox(w, "System Settings Change",
                   "Your display adapter can only change the resolution when Halcyon starts.\n\n"
                   "Do you want to restart your computer now?", ICON_QUESTION, MB_YESNO) == ID_YES)
            desktop_restart();
        return 1;
    }
    display_conf_t old;
    display_get_conf(&old);
    wm_set_busy(1);
    int r = display_set_conf(&cf);
    wm_set_busy(0);
    if (r) {
        char msg[160];
        snprintf(msg, sizeof msg, "Halcyon could not use these display settings (%s).", strerror(r));
        msgbox(w, "Display Properties", msg, ICON_ERROR, MB_OK);
        if (wm_alive(w)) { display_get_conf(&d->applied); load_monitors(d, &d->applied); arrange(d); refresh_picture(d); select_monitor(w, d->msel); }
        return 0;
    }
    kprintf("display: Display Properties applied new settings\n");
    if (!ask_keep(w)) {
        display_set_conf(&old);
        kprintf("display: settings not kept: back to the old ones\n");
        if (wm_alive(w)) {
            display_get_conf(&d->applied);
            load_monitors(d, &d->applied);
            arrange(d);
            refresh_picture(d);
            select_monitor(w, d->msel);
        }
        return 0;
    }
    /* as applied (the core normalized the places) */
    display_conf_t now;
    display_get_conf(&now);
    display_save_conf(&now);
    if (!wm_alive(w)) return 1;
    d->applied = now;
    load_monitors(d, &now);
    arrange(d);
    refresh_picture(d);
    select_monitor(w, d->msel);
    return 1;
}

static int apply(window_t *w) {
    if (!apply_saver(w)) return 0;
    return wm_alive(w) ? apply_display(w) : 1;
}

static void on_ok(window_t *w, widget_t *b) { (void)b; if (apply(w) && wm_alive(w)) wm_close(w); }
static void on_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }
static void on_apply(window_t *w, widget_t *b) { (void)b; apply(w); }

static void dp_destroy(window_t *w) {
    disp_t *d = w->data;
    if (d) {
        preview_stop(d);
        for (int i = 0; i < d->nmon; i++) if (d->mon[i].thumb) surface_free(d->mon[i].thumb);
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
    if (w) w->help = "display";              /* F1 */
    if (!w) { kfree(d); return; }
    dp_win = w;
    w->data = d;
    w->on_paint = dp_paint;
    w->on_destroy = dp_destroy;
    w->on_event = dp_event;
    w->on_menu = dp_menu;
    w->on_tick = dp_tick;

    /* the monitors as they are (or as saved for the next start) */
    display_get_conf(&d->applied);
    if (!display_live()) {
        int sw = settings_get_int("display.width", 0), sh = settings_get_int("display.height", 0);
        for (int i = 0; i < d->applied.n; i++)
            if (d->applied.out[i].conn == d->applied.primary && sw && sh &&
                modes_find(d->applied.out[i].conn->modes, d->applied.out[i].conn->nmodes, sw, sh) >= 0) {
                d->applied.out[i].w = sw; d->applied.out[i].h = sh;
            }
    }
    load_monitors(d, &d->applied);
    arrange(d);
    display_conf_t cf;
    conf_from(d, &cf);
    /* (the hidden ones' places as arranged count as the applied ones) */
    for (int i = 0; i < cf.n; i++)
        for (int k = 0; k < d->applied.n; k++)
            if (d->applied.out[k].conn == cf.out[i].conn && !cf.out[i].attach) { d->applied.out[k].x = cf.out[i].x; d->applied.out[k].y = cf.out[i].y; }
    refresh_picture(d);
    d->msel = MAX(0, mon_index(d, d->primary));

    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(24, TY + 250, DW - 48, 76), "", DP_LIST);
    lb->on_change = on_select;
    widget_t *idb = widget_add(w, W_BUTTON, mkrect(DW - 16 - 75, TY + 175, 75, 23), "Identify", DP_IDENTIFY);
    idb->on_click = on_identify;
    strlcpy(idb->tip, "Shows each monitor's number on it", sizeof idb->tip);
    if (guard_off_adapter()) {
        widget_t *drv = widget_add(w, W_BUTTON, mkrect(DW - 16 - 75 - 8 - 112, TY + 175, 112, 23), "Turn on driver...", DP_DRIVER);
        drv->on_click = on_driver;
        strlcpy(drv->tip, "A display driver was turned off because its start did not finish", sizeof drv->tip);
    }
    widget_t *pc = widget_add(w, W_CHECKBOX, mkrect(16, TY + 368, DW - 32, 18), "Use this device as the primary monitor.", DP_PRIMARY);
    pc->on_change = on_primary;
    widget_t *xc = widget_add(w, W_CHECKBOX, mkrect(16, TY + 390, DW - 32, 18), "Extend my desktop onto this monitor.", DP_EXTEND);
    xc->on_change = on_extend;

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
    select_monitor(w, d->msel);
    show_tab(w, TAB_SETTINGS);
    wm_place_restore(w, "display");
}

/* Display Properties on its Screen Saver tab (the Command Prompt's "display saver") */
void app_display_saver(void) {
    app_display();
    if (dp_win && wm_alive(dp_win)) show_tab(dp_win, TAB_SAVER);
}

/* ---- test hook: QEMU fw_cfg "opt/halcyon/displaytest" = "WxH[@HZ],..." ----
 * Opens Display Properties on the primary monitor, picks each resolution
 * through the list's handler (the refresh rate it then takes is logged)
 * and a rate through the drop-down menu's, logs what the drop-down offers,
 * waits 2 seconds (a screendump), applies, and answers "Do you want to keep
 * these settings?" with Yes after 3 seconds (tools/boottest.py rates). */
static void test_log(disp_t *d, const char *what) {
    dmon_t *m = d->msel < d->nmon ? &d->mon[d->msel] : NULL;
    if (!m) return;
    int hz[MAXRATES], n = mon_rates(m, hz, MAXRATES);
    char r[96] = "";
    for (int i = 0; i < n; i++) snprintf(r + strlen(r), sizeof r - strlen(r), "%s%d", i ? ", " : "", hz[i]);
    kprintf("display test: %s: %s %dx%d, refresh rate %d Hertz (offered: %s Hertz), drop-down %s, %d resolutions\n", what, m->conn->name,
            m->w, m->h, m->hz, r, rate_choice(m) ? "enabled" : "disabled (Default)", d->nsizes);
}

static void test_pause(int ms) {
    for (u64 end = time_ms() + (u64)ms; time_ms() < end;) gui_step();
}

void display_test_hook(void) {
    char spec[96];
    if (!qemu_fw_file("opt/halcyon/displaytest", spec, sizeof spec) || !spec[0]) return;
    app_display();
    window_t *w = dp_win;
    if (!w || !wm_alive(w)) return;
    disp_t *d = w->data;
    show_tab(w, TAB_SETTINGS);
    test_log(d, "opened");
    for (char *tok = spec; tok && *tok;) {
        char *next = strchr(tok, ',');
        if (next) *next++ = 0;
        char *x = strchr(tok, 'x'), *at = strchr(tok, '@');
        int tw = (int)strtol(tok, NULL, 10), th = x ? (int)strtol(x + 1, NULL, 10) : 0, thz = at ? (int)strtol(at + 1, NULL, 10) : 0;
        dmon_t *m = &d->mon[d->msel];
        widget_t *lb = widget_find(w, DP_LIST);
        int row = -1;
        for (int k = 0; k < d->nsizes; k++)
            if (m->conn->modes[d->size_mode[k]].w == tw && m->conn->modes[d->size_mode[k]].h == th) row = k;
        if (row < 0 || !lb) { kprintf("display test: %dx%d is not in the list\n", tw, th); return; }
        lb->value = row;
        lb->top = MAX(0, MIN(row - 2, d->nsizes - 4));
        on_select(w, lb);
        test_log(d, "resolution chosen");
        if (thz) {
            int hz[MAXRATES], n = mon_rates(m, hz, MAXRATES), k = -1;
            for (int i = 0; i < n; i++) if (hz[i] == thz) k = i;
            if (k < 0) { kprintf("display test: %d Hertz is not offered at %dx%d\n", thz, tw, th); return; }
            dp_menu(w, MENU_RATE + k);
            test_log(d, "refresh rate chosen");
        }
        tok = next;
    }
    test_pause(2000);
    keep_auto_ms = 3000;
    int ok = apply(w);
    keep_auto_ms = 0;
    if (!wm_alive(w)) return;
    dmon_t *m = &d->mon[d->msel];
    display_head_t *h = m->conn->head;
    char k[SETTINGS_KEY_MAX], key[SETTINGS_KEY_MAX] = "display.";
    for (const char *s = m->conn->name; *s && strlen(key) < sizeof key - 4; s++) {
        size_t l = strlen(key);
        key[l] = (char)tolower((u8)*s);
        key[l + 1] = 0;
    }
    snprintf(k, sizeof k, "%s.hz", key);
    kprintf("display test: applied (%s): %s %dx%d at %d Hz, %s=%s\n", ok ? "kept" : "not kept", m->conn->name, h ? h->mode.w : 0,
            h ? h->mode.h : 0, h ? h->mode.hz : 0, k, settings_get(k, "(none)"));
}
