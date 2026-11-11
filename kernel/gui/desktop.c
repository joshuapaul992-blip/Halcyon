/* Halcyon OS - desktop shell: wallpaper, desktop icons, taskbar, start menu,
 * notification balloons, shutdown */
#include "wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/kmath.h"
#include "core/io.h"
#include "fs/vfs.h"
#include "usb/usb.h"

static surface_t *wallpaper;
static int wp_style;

/* ---------------- wallpaper ---------------- */
static u32 hash2(int x, int y) {
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static double vnoise(double x, double y) {
    int xi = (int)k_floor(x), yi = (int)k_floor(y);
    double fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    double a = (hash2(xi, yi) & 0xFFFF) / 65535.0, b = (hash2(xi + 1, yi) & 0xFFFF) / 65535.0;
    double c = (hash2(xi, yi + 1) & 0xFFFF) / 65535.0, d = (hash2(xi + 1, yi + 1) & 0xFFFF) / 65535.0;
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
static double fbm(double x, double y) {
    double s = 0, amp = 0.5;
    for (int i = 0; i < 4; i++) { s += vnoise(x, y) * amp; x *= 2.03; y *= 2.03; amp *= 0.5; }
    return s;
}
static color_t lerp3(color_t a, color_t b, color_t c, double t) {
    if (t < 0.5) return color_mix(a, b, (int)(t * 2 * 255));
    return color_mix(b, c, (int)((t - 0.5) * 2 * 255));
}

static void wp_bay(surface_t *s) {
    int w = s->w, h = s->h;
    int horizon = h * 64 / 100;
    for (int y = 0; y < h; y++) {
        double t = (double)y / horizon;
        color_t sky = t < 1 ? lerp3(HEX(0x123E8C), HEX(0x4F9BDB), HEX(0xF6D7A4), t * t * 0.6 + t * 0.4) : HEX(0xF6D7A4);
        for (int x = 0; x < w; x++) {
            color_t c = sky;
            if (y < horizon) {
                /* sun glow */
                double dx = (x - w * 0.72) / (double)w, dy = (y - horizon * 0.93) / (double)h;
                double glow = k_exp(-(dx * dx + dy * dy * 3) * 28);
                c = color_mix(c, HEX(0xFFF4D2), (int)(glow * 200));
                /* clouds */
                double n = fbm(x / 210.0, y / 70.0 + 3.1);
                double cl = (n - 0.52) * 3.2 * (1 - t * 0.6);
                if (cl > 0) c = color_mix(c, HEX(0xFFFFFF), (int)(MIN(cl, 0.75) * 255));
            }
            s->px[y * s->stride + x] = c;
        }
    }
    /* layered waves */
    static const u32 cols[4] = { 0x3E86C2, 0x2A6AA6, 0x1D5088, 0x123866 };
    for (int layer = 0; layer < 4; layer++) {
        double base = horizon + layer * h * 0.075;
        double amp = 10 + layer * 9, freq = 0.006 - layer * 0.0009, ph = layer * 1.7;
        color_t top = HEX(cols[layer]), bot = color_lighten(HEX(cols[layer]), -50);
        for (int x = 0; x < w; x++) {
            double yy = base + k_sin(x * freq + ph) * amp + k_sin(x * freq * 2.7 + ph * 2) * amp * 0.35;
            int y0 = (int)yy;
            if (y0 < 0) y0 = 0;
            for (int y = y0; y < h; y++) {
                double t = (double)(y - y0) / (h - y0 + 1);
                color_t c = color_mix(top, bot, (int)(t * 255));
                if (y == y0) c = color_mix(s->px[y * s->stride + x], c, (int)((1 - (yy - y0)) * 255));
                else if (y < y0 + 2) c = color_lighten(c, 40);
                s->px[y * s->stride + x] = c;
            }
        }
    }
}

static void wp_aurora(surface_t *s) {
    int w = s->w, h = s->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double t = (double)y / h;
            color_t c = lerp3(HEX(0x040A1C), HEX(0x0B1E40), HEX(0x173A5E), t);
            u32 hh = hash2(x, y);
            if ((hh & 1023) == 0 && y < h * 0.8) c = color_mix(c, HEX(0xFFFFFF), 120 + (hh >> 24) / 2);
            s->px[y * s->stride + x] = c;
        }
    for (int band = 0; band < 3; band++) {
        color_t bc = band == 1 ? HEX(0x8A5CF0) : HEX(0x3CF0A8);
        double cy = h * (0.28 + band * 0.1), ph = band * 2.1;
        for (int x = 0; x < w; x++) {
            double center = cy + k_sin(x * 0.004 + ph) * h * 0.08 + k_sin(x * 0.011 + ph * 3) * h * 0.025;
            double inten = 0.55 + 0.45 * k_sin(x * 0.02 + ph) * k_sin(x * 0.0035 + band);
            if (inten < 0) inten = 0;
            for (int y = MAX(0, (int)(center - h * 0.25)); y < MIN(h, (int)(center + h * 0.06)); y++) {
                double d = (y - center) / (h * 0.25);
                double a = d < 0 ? k_exp(-d * d * 6) : k_exp(-d * d * 400);
                a *= inten * (band == 1 ? 0.45 : 0.7);
                u32 *p = &s->px[y * s->stride + x];
                *p = color_mix(*p, bc, (int)(MIN(a, 1.0) * 255));
            }
        }
    }
    /* mountains */
    for (int x = 0; x < w; x++) {
        double m = h * 0.82 - fbm(x / 160.0, 7.7) * h * 0.22;
        for (int y = (int)m; y < h; y++) s->px[y * s->stride + x] = color_mix(HEX(0x050A14), HEX(0x0A1426), (y - (int)m) * 255 / MAX(1, h - (int)m));
    }
}

static void wp_dunes(surface_t *s) {
    int w = s->w, h = s->h;
    for (int y = 0; y < h; y++) {
        double t = (double)y / (h * 0.6);
        color_t c = t < 1 ? lerp3(HEX(0x3A2C6E), HEX(0xE0607A), HEX(0xFFC27A), t) : HEX(0xFFC27A);
        for (int x = 0; x < w; x++) s->px[y * s->stride + x] = c;
    }
    for (int x = 0; x < w; x++) {
        double dx = (x - w * 0.3) / (double)w;
        for (int y = 0; y < h * 0.6; y++) {
            double dy = (y - h * 0.55) / (double)h;
            double g = k_exp(-(dx * dx + dy * dy) * 60);
            u32 *p = &s->px[y * s->stride + x];
            *p = color_mix(*p, HEX(0xFFF0C0), (int)(g * 230));
        }
    }
    static const u32 cols[4] = { 0xE8945A, 0xD27840, 0xB85E2E, 0x8E4420 };
    for (int layer = 0; layer < 4; layer++) {
        double base = h * (0.58 + layer * 0.09);
        for (int x = 0; x < w; x++) {
            double yy = base + k_sin(x * (0.004 + layer * 0.001) + layer * 2.2) * (24 + layer * 8);
            for (int y = MAX(0, (int)yy); y < h; y++) {
                color_t c = color_mix(HEX(cols[layer]), color_lighten(HEX(cols[layer]), -40), (y - (int)yy) * 255 / MAX(1, h - (int)yy));
                s->px[y * s->stride + x] = c;
            }
        }
    }
}

static void wp_solid(surface_t *s) {
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = HEX(0x2F6E8E);
}

void desktop_wallpaper_set(int style) {
    wp_style = style;
    if (!wallpaper) wallpaper = surface_new(screen_w(), screen_h());
    switch (style) {
    case 1: wp_aurora(wallpaper); break;
    case 2: wp_dunes(wallpaper); break;
    case 3: wp_solid(wallpaper); break;
    default: wp_bay(wallpaper); break;
    }
    gui_full_redraw();
}
int desktop_wallpaper_style(void) { return wp_style; }

/* ---------------- desktop icons ---------------- */
typedef struct dicon { const char *label; int icon; void (*launch)(void); } dicon_t;

static void l_computer(void) { app_explorer(""); }
static void l_docs(void) { app_explorer("C:/Documents"); }
static void l_term(void) { app_terminal(); }
static void l_notepad(void) { app_notepad(NULL); }
static void l_paint(void) { app_paint(NULL); }
static void l_sysinfo(void) { app_sysinfo(); }
static void l_calc(void) { app_calc(); }
static void l_control(void) { app_control(); }
static void l_mines(void) { app_mines(); }
static void l_about(void) { app_about(); }

static const dicon_t dicons[] = {
    { "My Computer", ICON_COMPUTER, l_computer },
    { "My Documents", ICON_DOCUMENTS, l_docs },
    { "Command Prompt", ICON_TERMINAL, l_term },
    { "Notepad", ICON_NOTEPAD, l_notepad },
    { "Paint", ICON_PAINT, l_paint },
    { "System Information", ICON_SYSINFO, l_sysinfo },
};
#define NDICONS ((int)ARRAY_SIZE(dicons))
static int dsel = -1;

static rect_t dicon_rect(int i) {
    int rows = MAX(1, (screen_h() - TASKBAR_H - 10) / 84);
    return mkrect(8 + (i / rows) * 84, 10 + (i % rows) * 84, 80, 78);
}

void desktop_paint(gc_t *g, rect_t clip) {
    if (wallpaper) gfx_blit(g, clip.x, clip.y, wallpaper, clip);
    else gfx_fill(g, clip, HEX(0x2F6E8E));
    for (int i = 0; i < NDICONS; i++) {
        rect_t r = dicon_rect(i);
        if (rect_empty(rect_intersect(r, clip))) continue;
        int sel = i == dsel;
        int ix = r.x + (r.w - 32) / 2, iy = r.y + 4;
        if (sel) gfx_icon_tinted(g, ix, iy, &icons32[dicons[i].icon], g_theme->select, 110);
        else gfx_icon(g, ix, iy, &icons32[dicons[i].icon]);
        /* label: up to two lines, centred, with shadow */
        const char *lbl = dicons[i].label;
        int tw = gfx_text_width(&font_ui, lbl);
        char l1[64], l2[64] = "";
        strlcpy(l1, lbl, sizeof l1);
        if (tw > r.w - 4) {
            const char *sp = strrchr(lbl, ' ');
            if (sp) { int n = (int)(sp - lbl); memcpy(l1, lbl, n); l1[n] = 0; strlcpy(l2, sp + 1, sizeof l2); }
        }
        for (int line = 0; line < 2; line++) {
            const char *t = line ? l2 : l1;
            if (!*t) continue;
            int w = gfx_text_width(&font_ui, t);
            int tx = r.x + (r.w - w) / 2, ty = r.y + 42 + line * 14;
            if (sel) gfx_fill(g, mkrect(tx - 2, ty - 1, w + 4, 14), g_theme->select);
            else gfx_text(g, &font_ui, tx + 1, ty + 1, t, HEX(0x000000));
            gfx_text(g, &font_ui, tx, ty, t, HEX(0xFFFFFF));
        }
    }
}

static menu_item_t desk_menu_items[] = {
    { "Refresh", NULL, 1, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Open Command Prompt here", NULL, 2, 0, ICON_TERMINAL },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Properties", NULL, 3, MI_BOLD, ICON_CONTROL },
};
static menu_t desk_menu = { "Desktop", desk_menu_items, 5 };

static window_t *desk_dummy;   /* owner for desktop context menus */
static void desk_menu_cmd(window_t *w, int id) {
    if (id == 1) gui_full_redraw();
    else if (id == 2) app_terminal();
    else if (id == 3) app_control();
}

int desktop_event(gui_event_t *e) {
    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        int hit = -1;
        for (int i = 0; i < NDICONS; i++) if (rect_contains(dicon_rect(i), e->x, e->y)) hit = i;
        if (hit != dsel) {
            if (dsel >= 0) wm_invalidate_rect(dicon_rect(dsel));
            dsel = hit;
            if (hit >= 0) wm_invalidate_rect(dicon_rect(hit));
        }
        if (e->type == GE_DBLCLICK && hit >= 0 && e->button == MOUSE_LEFT) dicons[hit].launch();
        if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_RIGHT && hit < 0) {
            if (!desk_dummy) { desk_dummy = kzalloc(sizeof(window_t)); }
            desk_dummy->on_menu = desk_menu_cmd;
            menu_popup(desk_dummy, &desk_menu, e->x, e->y);
        }
        return 1;
    }
    if (e->type == GE_KEY && e->key.pressed) {
        if (e->key.key == KEY_ENTER && dsel >= 0) dicons[dsel].launch();
        else if (e->key.key == KEY_DOWN || e->key.key == KEY_UP) {
            int n = dsel < 0 ? 0 : CLAMP(dsel + (e->key.key == KEY_DOWN ? 1 : -1), 0, NDICONS - 1);
            if (dsel >= 0) wm_invalidate_rect(dicon_rect(dsel));
            dsel = n;
            wm_invalidate_rect(dicon_rect(dsel));
        }
        return 1;
    }
    return 0;
}

/* menu commands from the desktop dummy window can't use wm_alive; handled in menu via owner->on_menu */

/* ---------------- taskbar ---------------- */
#define START_W 104
#define TRAY_MIN_W 96
static int start_hover, start_pressed;
static window_t *start_menu;
static int task_hover = -1, task_pressed = -1;
static int tray_usb_hover;
static int last_minute = -1;

rect_t taskbar_rect(void) { return mkrect(0, screen_h() - TASKBAR_H, screen_w(), TASKBAR_H); }
void taskbar_invalidate(void) { wm_invalidate_rect(taskbar_rect()); }

static int usb_disks_present(void) {
    for (usb_device_t *d = usb_first_device(); d; d = d->next)
        for (int i = 0; i < d->nbind; i++) if (d->bind[i].driver == &usb_msc_driver) return 1;
    return 0;
}

static rect_t tray_rect(void) {
    int w = TRAY_MIN_W + (usb_disks_present() ? 22 : 0);
    rect_t t = taskbar_rect();
    return mkrect(t.w - w, t.y, w, t.h);
}

static int task_list(window_t **out, int max) {
    int n = 0;
    for (window_t *w = wm_first(); w && n < max; w = w->next)
        if (!(w->flags & (WF_POPUP | WF_DIALOG | WF_TOPMOST))) out[n++] = w;
    return n;
}

static rect_t task_rect(int i, int n) {
    rect_t t = taskbar_rect();
    int x0 = START_W + 12, x1 = tray_rect().x - 8;
    int w = n ? MIN(170, (x1 - x0) / n) : 170;
    return mkrect(x0 + i * w, t.y + 3, w - 3, t.h - 5);
}

void taskbar_paint(gc_t *g, rect_t clip) {
    rect_t t = taskbar_rect();
    if (rect_empty(rect_intersect(t, clip))) return;
    const theme_t *th = g_theme;
    gfx_gradient_v(g, mkrect(t.x, t.y, t.w, 6), th->taskbar_line, th->taskbar_top);
    gfx_gradient_v(g, mkrect(t.x, t.y + 6, t.w, t.h - 6), th->taskbar_top, th->taskbar_bottom);
    gfx_hline(g, t.x, t.y, t.w, color_lighten(th->taskbar_line, 40));

    /* start button */
    rect_t sb = mkrect(0, t.y, START_W, t.h);
    color_t st = th->start_top, sbm = th->start_bottom;
    if (start_hover) { st = color_lighten(st, 35); sbm = color_lighten(sbm, 25); }
    if (start_pressed || start_menu) { st = color_lighten(st, -30); sbm = color_lighten(sbm, -25); }
    gfx_rounded(g, sb, 12, 2 | 4, color_lighten(st, 50), st);
    gfx_rounded(g, mkrect(sb.x, sb.y + 10, sb.w, sb.h - 10), 12, 4, st, sbm);
    gfx_rounded_outline(g, sb, 12, 2 | 4, color_lighten(sbm, -40));
    gfx_icon(g, 10, t.y + 3, &icons32[ICON_LOGO]);
    gfx_icon(g, 9, t.y + 6, NULL);
    gfx_text(g, &font_large, 44 + 1, t.y + 4 + 1, "start", color_lighten(sbm, -70));
    gfx_text(g, &font_large, 44, t.y + 4, "start", HEX(0xFFFFFF));

    /* task buttons */
    window_t *tasks[32];
    int n = task_list(tasks, 32);
    window_t *active = wm_active();
    for (int i = 0; i < n; i++) {
        rect_t r = task_rect(i, n);
        int act = tasks[i] == active && tasks[i]->state != WS_MINIMIZED;
        color_t a = color_lighten(th->taskbar_top, 30), b = th->taskbar_top;
        if (act) { a = color_lighten(th->taskbar_bottom, -30); b = color_lighten(th->taskbar_bottom, -10); }
        else if (i == task_hover) { a = color_lighten(a, 30); b = color_lighten(b, 25); }
        gfx_rounded(g, r, 3, 15, a, b);
        gfx_rounded_outline(g, r, 3, 15, color_lighten(th->taskbar_bottom, -40));
        if (tasks[i]->icon >= 0) gfx_icon(g, r.x + 6, r.y + (r.h - 16) / 2, &icons16[tasks[i]->icon]);
        color_t tc = th == &g_themes[2] ? HEX(0x1C2230) : HEX(0xFFFFFF);
        gfx_text_box(g, act ? &font_ui_bold : &font_ui, mkrect(r.x + 26, r.y, r.w - 30, r.h), tasks[i]->title, tc, 0);
    }

    /* tray */
    rect_t tr = tray_rect();
    gfx_gradient_v(g, tr, color_lighten(th->sidebar_top, -10), color_lighten(th->sidebar_bottom, -20));
    gfx_vline(g, tr.x, tr.y, tr.h, color_lighten(th->taskbar_bottom, -50));
    gfx_vline(g, tr.x + 1, tr.y, tr.h, color_lighten(th->taskbar_line, 30));
    int x = tr.x + 8;
    if (usb_disks_present()) {
        if (tray_usb_hover) gfx_fill_alpha(g, mkrect(x - 3, tr.y + 5, 22, 20), HEX(0xFFFFFF), 70);
        gfx_icon_tinted(g, x, tr.y + 7, &icons16[ICON_USB], HEX(0xFFFFFF), 255);
        x += 22;
    }
    struct rtc_time tm;
    rtc_read(&tm);
    char clock[16];
    int h12 = tm.hour % 12 == 0 ? 12 : tm.hour % 12;
    snprintf(clock, sizeof clock, "%d:%02d %s", h12, tm.minute, tm.hour < 12 ? "AM" : "PM");
    gfx_text_box(g, &font_ui, mkrect(x, tr.y, tr.x + tr.w - x - 6, tr.h), clock, th == &g_themes[2] ? HEX(0x1C2230) : HEX(0xFFFFFF), 1);
    last_minute = tm.minute;
}

static menu_item_t usb_menu_items[] = {
    { "Safely remove USB storage", NULL, 1, MI_BOLD, ICON_USBDRIVE },
};
static menu_t usb_menu = { "USB", usb_menu_items, 1 };
static window_t *tray_dummy;
static void tray_cmd(window_t *w, int id) {
    if (id == 1) {
        vfs_sync_all();
        msgbox(NULL, "Safely Remove Hardware", "All data has been written to the USB storage devices.\nIt is now safe to remove them.", ICON_INFO, MB_OK);
    }
}

int taskbar_event(gui_event_t *e) {
    rect_t t = taskbar_rect();
    window_t *tasks[32];
    int n = task_list(tasks, 32);
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int sh = e->x >= 0 && e->x < START_W && e->y >= t.y;
        int th = -1;
        for (int i = 0; i < n; i++) if (rect_contains(task_rect(i, n), e->x, e->y)) th = i;
        rect_t tr = tray_rect();
        int uh = rect_contains(mkrect(tr.x + 4, tr.y, 22, tr.h), e->x, e->y) && usb_disks_present();
        if (sh != start_hover || th != task_hover || uh != tray_usb_hover) {
            start_hover = sh; task_hover = th; tray_usb_hover = uh;
            taskbar_invalidate();
        }
        return 1;
    }
    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        if (e->x < START_W) { start_pressed = 1; start_menu_toggle(); taskbar_invalidate(); return 1; }
        for (int i = 0; i < n; i++)
            if (rect_contains(task_rect(i, n), e->x, e->y)) { task_pressed = i; return 1; }
        rect_t tr = tray_rect();
        if (usb_disks_present() && rect_contains(mkrect(tr.x + 4, tr.y, 22, tr.h), e->x, e->y)) {
            if (!tray_dummy) tray_dummy = kzalloc(sizeof(window_t));
            tray_dummy->on_menu = tray_cmd;
            menu_popup(tray_dummy, &usb_menu, tr.x, t.y - 26);
            return 1;
        }
        return 1;
    }
    if (e->type == GE_MOUSE_UP) {
        start_pressed = 0;
        if (task_pressed >= 0 && task_pressed < n && rect_contains(task_rect(task_pressed, n), e->x, e->y)) {
            window_t *w = tasks[task_pressed];
            if (w == wm_active() && w->state != WS_MINIMIZED) wm_minimize(w);
            else wm_show(w);
        }
        task_pressed = -1;
        taskbar_invalidate();
        return 1;
    }
    return 0;
}

/* ---------------- start menu ---------------- */
typedef struct sitem { const char *label; const char *sub; int icon; void (*launch)(void); } sitem_t;
static void l_docs2(void) { app_explorer("C:/Documents"); }
static const sitem_t left_items[] = {
    { "My Computer", "Browse drives and files", ICON_COMPUTER, l_computer },
    { "Command Prompt", "Halcyon command shell", ICON_TERMINAL, l_term },
    { "Notepad", "Edit text files", ICON_NOTEPAD, l_notepad },
    { "Paint", "Draw pictures", ICON_PAINT, l_paint },
    { "Calculator", NULL, ICON_CALC, l_calc },
    { "Mines", NULL, ICON_GAME, l_mines },
};
static const sitem_t right_items[] = {
    { "My Documents", NULL, ICON_DOCUMENTS, l_docs2 },
    { "My Computer", NULL, ICON_COMPUTER, l_computer },
    { "Control Panel", NULL, ICON_CONTROL, l_control },
    { "System Information", NULL, ICON_SYSINFO, l_sysinfo },
    { "About Halcyon", NULL, ICON_HELP, l_about },
};
#define SM_W 400
#define SM_H 440
#define SM_HEAD 62
#define SM_FOOT 44
static int sm_hover = -1;   /* 0..5 left, 10..14 right, 20 restart?, 21 shutdown */

static rect_t sm_item_rect(int id) {
    if (id < 10) return mkrect(6, SM_HEAD + 8 + id * 44, 196, 42);
    if (id < 20) return mkrect(212, SM_HEAD + 10 + (id - 10) * 32, 182, 30);
    return mkrect(id == 20 ? SM_W - 230 : SM_W - 120, SM_H - SM_FOOT + 8, 110, 28);
}

static void sm_paint(window_t *w, gc_t *g) {
    const theme_t *th = g_theme;
    gfx_rounded(g, mkrect(0, 0, SM_W, SM_HEAD), 8, 3, th->menu_header_top, th->menu_header_bottom);
    gfx_hline(g, 0, SM_HEAD - 2, SM_W, color_lighten(th->hover, 20));
    gfx_hline(g, 0, SM_HEAD - 1, SM_W, th->hover);
    gfx_fill(g, mkrect(6, 8, 48, 48), HEX(0xFFFFFF));
    gfx_icon(g, 6, 8, &icons48[ICON_USER]);
    gfx_text(g, &font_large, 67, 22, "Halcyon User", color_lighten(th->menu_header_bottom, -80));
    gfx_text(g, &font_large, 66, 21, "Halcyon User", HEX(0xFFFFFF));
    /* columns */
    gfx_fill(g, mkrect(0, SM_HEAD, 206, SM_H - SM_HEAD - SM_FOOT), HEX(0xFFFFFF));
    gfx_fill(g, mkrect(206, SM_HEAD, SM_W - 206, SM_H - SM_HEAD - SM_FOOT), color_lighten(th->sidebar_top, 150));
    gfx_vline(g, 206, SM_HEAD, SM_H - SM_HEAD - SM_FOOT, color_lighten(th->sidebar_bottom, 80));
    for (int i = 0; i < (int)ARRAY_SIZE(left_items); i++) {
        rect_t r = sm_item_rect(i);
        int hov = sm_hover == i;
        if (hov) gfx_fill(g, r, th->select);
        gfx_icon(g, r.x + 4, r.y + 5, &icons32[left_items[i].icon]);
        color_t tc = hov ? th->select_text : HEX(0x000000);
        if (left_items[i].sub) {
            gfx_text(g, &font_ui_bold, r.x + 44, r.y + 7, left_items[i].label, tc);
            gfx_text(g, &font_ui, r.x + 44, r.y + 22, left_items[i].sub, hov ? th->select_text : HEX(0x6D6D6D));
        } else {
            gfx_text_box(g, &font_ui_bold, mkrect(r.x + 44, r.y, r.w - 46, r.h), left_items[i].label, tc, 0);
        }
    }
    gfx_hline(g, 14, SM_H - SM_FOOT - 40, 180, HEX(0xDDDDDD));
    gfx_text(g, &font_ui, 20, SM_H - SM_FOOT - 30, "Halcyon " OS_VERSION, HEX(0x8A8A8A));
    for (int i = 0; i < (int)ARRAY_SIZE(right_items); i++) {
        rect_t r = sm_item_rect(10 + i);
        int hov = sm_hover == 10 + i;
        if (hov) gfx_fill(g, r, th->select);
        gfx_icon(g, r.x + 4, r.y + 7, &icons16[right_items[i].icon]);
        gfx_text_box(g, i < 2 ? &font_ui_bold : &font_ui, mkrect(r.x + 28, r.y, r.w - 30, r.h), right_items[i].label,
                     hov ? th->select_text : color_lighten(th->menu_header_bottom, -60), 0);
    }
    /* footer */
    rect_t f = mkrect(0, SM_H - SM_FOOT, SM_W, SM_FOOT);
    gfx_rounded(g, f, 8, 12, th->menu_header_bottom, color_lighten(th->menu_header_bottom, -30));
    static const char *fl[2] = { "Restart", "Turn Off" };
    static const int fi[2] = { ICON_RESTART, ICON_SHUTDOWN };
    for (int i = 0; i < 2; i++) {
        rect_t r = sm_item_rect(20 + i);
        if (sm_hover == 20 + i) gfx_fill_alpha(g, r, HEX(0xFFFFFF), 60);
        gfx_icon(g, r.x + 4, r.y + 2, &icons16[fi[i]]);
        gfx_icon(g, r.x + 4, r.y + 6, NULL);
        gfx_text_box(g, &font_ui, mkrect(r.x + 26, r.y, r.w - 26, r.h), fl[i], HEX(0xFFFFFF), 0);
    }
    gfx_rect(g, mkrect(0, SM_HEAD, SM_W, SM_H - SM_HEAD), color_lighten(th->menu_header_bottom, -20));
}

static int sm_hit(int x, int y) {
    for (int i = 0; i < (int)ARRAY_SIZE(left_items); i++) if (rect_contains(sm_item_rect(i), x, y)) return i;
    for (int i = 0; i < (int)ARRAY_SIZE(right_items); i++) if (rect_contains(sm_item_rect(10 + i), x, y)) return 10 + i;
    for (int i = 0; i < 2; i++) if (rect_contains(sm_item_rect(20 + i), x, y)) return 20 + i;
    return -1;
}

static void restart_now(void);
static void sm_launch(int id) {
    menu_close_all();
    if (id >= 0 && id < 10) left_items[id].launch();
    else if (id >= 10 && id < 20) right_items[id - 10].launch();
    else if (id == 20) restart_now();
    else if (id == 21) shutdown_dialog();
}

static void sm_event(window_t *w, gui_event_t *e) {
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int h = sm_hit(e->x, e->y);
        if (h != sm_hover) { sm_hover = h; wm_invalidate(w); }
    } else if (e->type == GE_MOUSE_UP) {
        int h = sm_hit(e->x, e->y);
        if (h >= 0) sm_launch(h);
    } else if (e->type == GE_KEY && e->key.pressed) {
        if (e->key.key == KEY_ESC) menu_close_all();
        else if (e->key.key == KEY_DOWN || e->key.key == KEY_UP) {
            int order[] = { 0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 20, 21 };
            int n = ARRAY_SIZE(order), cur = -1;
            for (int i = 0; i < n; i++) if (order[i] == sm_hover) cur = i;
            cur = cur < 0 ? 0 : (cur + (e->key.key == KEY_DOWN ? 1 : n - 1)) % n;
            sm_hover = order[cur];
            wm_invalidate(w);
        } else if (e->key.key == KEY_ENTER && sm_hover >= 0) sm_launch(sm_hover);
    }
}
static void sm_destroy(window_t *w) { start_menu = NULL; taskbar_invalidate(); }

void start_menu_toggle(void) {
    if (start_menu) { menu_close_all(); return; }
    menu_close_all();
    sm_hover = -1;
    start_menu = wm_create("Start", -1, 0, screen_h() - TASKBAR_H - SM_H, SM_W, SM_H, WF_POPUP | WF_TOPMOST);
    start_menu->on_paint = sm_paint;
    start_menu->on_event = sm_event;
    start_menu->on_destroy = sm_destroy;
    taskbar_invalidate();
}

/* ---------------- balloons ---------------- */
static window_t *balloon;
static u64 balloon_until;
static char balloon_title[64], balloon_text[160];

static void balloon_paint(window_t *w, gc_t *g) {
    rect_t r = mkrect(0, 0, w->client->w, w->client->h - 10);
    /* "transparent" background: show the wallpaper behind the balloon */
    if (wallpaper) gfx_blit(g, 0, 0, wallpaper, w->frame);
    /* the popup has no frame; draw a rounded tooltip */
    gfx_rounded(g, r, 8, 15, HEX(0xFFFFE6), HEX(0xFFF7C8));
    gfx_rounded_outline(g, r, 8, 15, HEX(0x6E6E5A));
    int tipx = w->client->w - 60;
    for (int i = 0; i < 10; i++) gfx_hline(g, tipx + i, r.h - 1 + i, 12 - i, HEX(0xFFF7C8));
    gfx_line(g, tipx, r.h - 1, tipx + 10, r.h + 9, HEX(0x6E6E5A));
    gfx_line(g, tipx + 12, r.h - 1, tipx + 10, r.h + 9, HEX(0x6E6E5A));
    gfx_icon(g, 10, 9, &icons16[ICON_INFO]);
    gfx_text(g, &font_ui_bold, 32, 10, balloon_title, HEX(0x000000));
    gfx_text_wrap(g, &font_ui, mkrect(12, 32, r.w - 24, r.h - 36), balloon_text, HEX(0x000000));
}
static void balloon_event(window_t *w, gui_event_t *e) {
    if (e->type == GE_MOUSE_DOWN) { wm_destroy(w); }
}
static void balloon_destroy(window_t *w) { balloon = NULL; }

static void show_balloon(const char *title, const char *text) {
    if (balloon) wm_destroy(balloon);
    strlcpy(balloon_title, title, sizeof balloon_title);
    strlcpy(balloon_text, text, sizeof balloon_text);
    int w = 300, h = 34 + gfx_text_wrap_height(&font_ui, w - 24, text) + 16;
    balloon = wm_create("", -1, screen_w() - w - 10, screen_h() - TASKBAR_H - h - 2, w, h, WF_TOPMOST | WF_POPUP | WF_NOSHADOW);
    balloon->on_paint = balloon_paint;
    balloon->on_event = balloon_event;
    balloon->on_destroy = balloon_destroy;
    balloon_until = time_ms() + 6000;
}

/* ---------------- shell tick ---------------- */
static u32 last_vgen, last_ugen;
static int known_letters[26];

void shell_tick(void) {
    struct rtc_time tm;
    static u64 last_clock;
    if (time_ms() - last_clock > 1000) {
        last_clock = time_ms();
        rtc_read(&tm);
        if (tm.minute != last_minute) taskbar_invalidate();
    }
    if (balloon && time_ms() > balloon_until) wm_destroy(balloon);
    if (usb_generation() != last_ugen) { last_ugen = usb_generation(); taskbar_invalidate(); }
    if (vfs_generation() != last_vgen) {
        last_vgen = vfs_generation();
        int now[26] = { 0 };
        for (int i = 0; i < vfs_volume_count(); i++) {
            volume_t *v = vfs_volume(i);
            if (v->letter >= 'A' && v->letter <= 'Z') now[v->letter - 'A'] = 1;
        }
        for (int i = 0; i < 26; i++) {
            if (now[i] && !known_letters[i]) {
                volume_t *v = vfs_volume_by_letter('A' + i);
                if (v && !v->is_boot) {
                    char t[160];
                    snprintf(t, sizeof t, "%s (%c:) is ready to use.\n%s, %s", v->label[0] ? v->label : "Removable Disk", 'A' + i,
                             v->bd && v->bd->model[0] ? v->bd->model : "Disk", v->fs_name);
                    show_balloon("Found New Hardware", t);
                }
            }
            known_letters[i] = now[i];
        }
        taskbar_invalidate();
    }
}

/* ---------------- shutdown ---------------- */
static void final_screen(const char *msg) {
    gc_t g = gc_for(g_screen);
    rect_t s = mkrect(0, 0, screen_w(), screen_h());
    gfx_gradient_v(&g, s, HEX(0x0E2A66), HEX(0x05102A));
    gfx_icon(&g, screen_w() / 2 - 24, screen_h() / 2 - 70, &icons48[ICON_LOGO]);
    int tw = gfx_text_width(&font_large, msg);
    gfx_text(&g, &font_large, (screen_w() - tw) / 2, screen_h() / 2, msg, HEX(0xF5B547));
    screen_present(s);
}

static void restart_now(void) {
    final_screen("Halcyon is restarting...");
    vfs_sync_all();
    mdelay(400);
    reboot();
}

static volatile int sd_done;
static int sd_choice;
static window_t *sd_win;
static void sd_paint(window_t *w, gc_t *g) {
    int cw = w->client->w, ch = w->client->h;
    gfx_gradient_v(g, mkrect(0, 0, cw, 44), g_theme->menu_header_bottom, color_lighten(g_theme->menu_header_bottom, -30));
    gfx_text(g, &font_large, 16, 11, "Turn off computer", HEX(0xFFFFFF));
    gfx_icon(g, cw - 44, 6, &icons32[ICON_LOGO]);
    gfx_gradient_v(g, mkrect(0, 44, cw, ch - 88), color_lighten(g_theme->menu_header_top, 40), g_theme->menu_header_top);
    gfx_gradient_v(g, mkrect(0, ch - 44, cw, 44), g_theme->menu_header_bottom, color_lighten(g_theme->menu_header_bottom, -30));
    static const char *lbl[2] = { "Restart", "Turn Off" };
    static const int ic[2] = { ICON_RESTART, ICON_SHUTDOWN };
    for (int i = 0; i < 2; i++) {
        int x = cw / 2 - 110 + i * 140;
        gfx_icon(g, x + 20, 70, &icons48[ic[i]]);
        gfx_text_box(g, &font_ui_bold, mkrect(x - 10, 124, 108, 18), lbl[i], HEX(0xFFFFFF), 1);
    }
}
static void sd_event(window_t *w, gui_event_t *e) {
    if (e->type == GE_MOUSE_UP) {
        int cw = w->client->w;
        for (int i = 0; i < 2; i++) {
            int x = cw / 2 - 110 + i * 140;
            if (rect_contains(mkrect(x + 10, 60, 68, 90), e->x, e->y)) { sd_choice = i + 1; sd_done = 1; }
        }
    }
    if (e->type == GE_KEY && e->key.pressed && e->key.key == KEY_ESC) { sd_choice = 0; sd_done = 1; }
}
static void sd_cancel(window_t *w, widget_t *b) { sd_choice = 0; sd_done = 1; }
static int sd_close(window_t *w) { sd_choice = 0; sd_done = 1; return 0; }

void shutdown_dialog(void) {
    if (sd_win) return;
    sd_done = 0; sd_choice = 0;
    sd_win = wm_create("Turn off computer", ICON_SHUTDOWN, 0, 0, 320, 200, WF_DIALOG | WF_CENTER);
    sd_win->on_paint = sd_paint;
    sd_win->on_event = sd_event;
    sd_win->on_close = sd_close;
    widget_t *c = widget_add(sd_win, W_BUTTON, mkrect(320 - 86, 200 - 34, 75, 23), "Cancel", ID_CANCEL);
    c->on_click = sd_cancel; c->style |= WS_CANCEL;
    while (!sd_done && wm_alive(sd_win)) gui_step();
    if (wm_alive(sd_win)) { sd_win->on_close = NULL; wm_destroy(sd_win); }
    sd_win = NULL;
    if (sd_choice == 1) restart_now();
    if (sd_choice == 2) {
        final_screen("Halcyon is shutting down...");
        vfs_sync_all();
        mdelay(500);
        shutdown();
        final_screen("It is now safe to turn off your computer.");
        for (;;) __asm__ volatile("cli; hlt");
    }
}

/* ---------------- file associations ---------------- */
void shell_open(const char *path) {
    vfs_dirent_t st;
    if (vfs_stat(path, &st) == 0 && st.is_dir) { app_explorer(path); return; }
    const char *dot = strrchr(vfs_basename(path), '.');
    const char *ext = dot ? dot + 1 : "";
    static const char *text_ext[] = { "txt", "log", "ini", "cfg", "c", "h", "md", "bat", "asm", "s", "py", "csv",
                                      "json", "xml", "htm", "html", "inf", "conf", "sh", "nfo", "diz", "me", "1st", "rtf" };
    for (size_t i = 0; i < ARRAY_SIZE(text_ext); i++) if (!strcasecmp(ext, text_ext[i])) { app_notepad(path); return; }
    if (!strcasecmp(ext, "bmp")) { app_paint(path); return; }
    char msg[VFS_MAX_PATH + 96];
    snprintf(msg, sizeof msg, "There is no program associated with \"%s\".\n\nOpen it with Notepad?", vfs_basename(path));
    if (msgbox(NULL, "Open With", msg, ICON_QUESTION, MB_YESNO) == ID_YES) app_notepad(path);
}

void desktop_init(void) {
    desktop_wallpaper_set(0);
    last_vgen = vfs_generation();
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->letter >= 'A' && v->letter <= 'Z') known_letters[v->letter - 'A'] = 1;
    }
}
