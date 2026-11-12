/* Halcyon OS - Control Panel: appearance (theme, wallpaper) with a live
 * preview, date and time, and a system summary. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/kmath.h"
#include "core/settings.h"
#include "sound/sound.h"
#include "version.h"

#define CP_W 620
#define COL_W ((CP_W - 36) / 2)  /* two columns of groups */
#define COL2 (24 + COL_W)
#define LB_X (12 + (COL_W - 238) / 2)   /* the theme and wallpaper lists, centred under the preview */
#define CP_H 420
#define BANNER_H 58
#define NWP 4
#define TH_W 184           /* preview thumbnail */
#define TH_H 138

enum { ID_THEMES = 10, ID_WALLS, ID_APPLY, ID_TZLIST };

static const char *wp_names[NWP] = { "Halcyon Bay", "Aurora", "Dunes", "Solid Teal" };

typedef struct cpanel {
    int theme, wall;               /* pending selection */
    surface_t *thumb[NWP];         /* cached wallpaper thumbnails */
    int last_sec;
} cpanel_t;

static window_t *cp_win;

/* ---------------- helpers ---------------- */
static void trimmed(const char *s, char *out, int outsz) {
    while (*s == ' ') s++;
    strlcpy(out, s, outsz);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static surface_t *thumbnail(cpanel_t *c, int style) {
    if (style < 0 || style >= NWP) return NULL;
    if (c->thumb[style]) return c->thumb[style];
    int sw = screen_w(), sh = screen_h();
    surface_t *full = surface_new(sw, sh);
    surface_t *t = surface_new(TH_W, TH_H);
    if (!full || !t) { surface_free(full); surface_free(t); return NULL; }
    wm_set_busy(1);
    desktop_wallpaper_render(style, full);
    /* box filter down to the thumbnail size */
    for (int y = 0; y < TH_H; y++) {
        int y0 = y * sh / TH_H, y1 = MAX(y0 + 1, (y + 1) * sh / TH_H);
        for (int x = 0; x < TH_W; x++) {
            int x0 = x * sw / TH_W, x1 = MAX(x0 + 1, (x + 1) * sw / TH_W);
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += 2)
                for (int xx = x0; xx < x1; xx += 2) {
                    u32 p = full->px[yy * full->stride + xx];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                }
            t->px[y * t->stride + x] = RGB(r / n, g / n, b / n);
        }
    }
    surface_free(full);
    wm_set_busy(0);
    c->thumb[style] = t;
    return t;
}

static int weekday(int y, int m, int d) {
    /* Sakamoto's algorithm: 0 = Sunday */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* ---------------- time zone ----------------
 * The real-time clock keeps local time (as Windows does). The offset from
 * UTC, clock.utc_offset in minutes east, lets programs work out UTC: the
 * web browser checks certificate dates with it. Daylight saving time is not
 * followed by itself: the list is of offsets, and the user picks the one in
 * effect. */
typedef struct tzone { int min; const char *places; } tzone_t;
static const tzone_t tzones[] = {
    { -720, "Baker Island" }, { -660, "American Samoa, Midway" }, { -600, "Hawaii" }, { -570, "Marquesas Islands" },
    { -540, "Alaska" }, { -480, "Pacific Time (US and Canada)" }, { -420, "Mountain Time (US and Canada)" },
    { -360, "Central Time (US and Canada), Mexico City" }, { -300, "Eastern Time (US and Canada), Bogota, Lima" },
    { -240, "Atlantic Time (Canada), Caracas, Santiago" }, { -210, "Newfoundland" },
    { -180, "Brasilia, Buenos Aires, Greenland" }, { -120, "South Georgia" }, { -60, "Azores, Cape Verde" },
    { 0, "London, Dublin, Lisbon, Reykjavik" }, { 60, "Berlin, Paris, Rome, Madrid, Lagos" },
    { 120, "Athens, Helsinki, Kyiv, Cairo, Johannesburg" }, { 180, "Moscow, Istanbul, Riyadh, Nairobi" },
    { 210, "Tehran" }, { 240, "Dubai, Baku, Samara" }, { 270, "Kabul" }, { 300, "Karachi, Tashkent, Yekaterinburg" },
    { 330, "India, Sri Lanka" }, { 345, "Nepal" }, { 360, "Dhaka, Almaty, Omsk" }, { 390, "Yangon" },
    { 420, "Bangkok, Hanoi, Jakarta, Novosibirsk" }, { 480, "Beijing, Hong Kong, Singapore, Perth, Taipei" },
    { 525, "Eucla" }, { 540, "Tokyo, Seoul, Yakutsk" }, { 570, "Adelaide, Darwin" },
    { 600, "Sydney, Melbourne, Brisbane, Vladivostok" }, { 630, "Lord Howe Island" },
    { 660, "Solomon Islands, New Caledonia, Magadan" }, { 720, "Auckland, Wellington, Fiji" },
    { 765, "Chatham Islands" }, { 780, "Tonga, Samoa" }, { 840, "Line Islands (Kiribati)" },
};
#define NTZ ((int)(sizeof tzones / sizeof tzones[0]))

static void tz_label(int i, char *out, int n) {
    if (i < 0) { strlcpy(out, "Not set", n); return; }
    int m = tzones[i].min, a = m < 0 ? -m : m;
    snprintf(out, n, "(UTC%c%02d:%02d) %s", m < 0 ? '-' : '+', a / 60, a % 60, tzones[i].places);
}
static int tz_current(void) {
    const char *v = settings_get("clock.utc_offset", NULL);
    if (!v || !*v) return -1;
    int m = settings_get_int("clock.utc_offset", 0);
    for (int i = 0; i < NTZ; i++) if (tzones[i].min == m) return i;
    return -1;
}

#define ID_TZBTN 20
static void tz_button_text(window_t *cp) {
    widget_t *b = cp ? widget_find(cp, ID_TZBTN) : NULL;
    if (!b) return;
    int i = tz_current();
    char t[32] = "Set time zone...";
    if (i >= 0) {
        int m = tzones[i].min, a = m < 0 ? -m : m;
        snprintf(t, sizeof t, "Zone: UTC%c%02d:%02d...", m < 0 ? '-' : '+', a / 60, a % 60);
    }
    widget_set_text(cp, b, t);
}

static void tz_ok(window_t *w, widget_t *b) {
    (void)b;
    widget_t *lb = widget_find(w, ID_TZLIST);
    if (lb && lb->value >= 0 && lb->value < NTZ) {
        settings_set_int("clock.utc_offset", tzones[lb->value].min);
        int r = settings_save();
        if (r) {
            char msg[120];
            snprintf(msg, sizeof msg, "The time zone could not be saved (%s).", strerror(r));
            msgbox(w, "Time Zone", msg, ICON_ERROR, MB_OK);
            return;
        }
        if (cp_win && wm_alive(cp_win)) tz_button_text(cp_win);
    }
    wm_close(w);
}
static void tz_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }

static window_t *tz_win;
static void tz_destroy(window_t *w) { if (tz_win == w) tz_win = NULL; }
static void app_timezone(window_t *owner) {
    if (tz_win && wm_alive(tz_win)) { wm_show(tz_win); return; }
    int W = 420, H = 330;
    window_t *w = wm_create("Time Zone", ICON_CONTROL, -1, -1, W, H, WF_DIALOG | WF_CENTER);
    if (w) w->help = "timezone";              /* F1 */
    if (!w) return;
    tz_win = w;
    w->owner = owner;
    w->on_destroy = tz_destroy;
    widget_t *lbl = widget_add(w, W_LABEL, mkrect(14, 10, W - 28, 46),
                               "Choose the difference between this computer's clock and Coordinated Universal Time (UTC). "
                               "Where clocks change for summer, choose the offset in effect now.", 0);
    lbl->style |= WS_WRAP;
    char *items[NTZ];
    char buf[NTZ][72];
    for (int i = 0; i < NTZ; i++) { tz_label(i, buf[i], sizeof buf[i]); items[i] = buf[i]; }
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(14, 60, W - 28, H - 60 - 48), "", ID_TZLIST);
    widget_list_set(w, lb, items, NTZ);
    int cur = tz_current();
    lb->value = cur >= 0 ? cur : 14;             /* UTC+00:00 */
    int vis = lb->r.h / 18;
    lb->top = MAX(0, lb->value - vis / 2);
    lb->on_activate = tz_ok;
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(W - 12 - 2 * 75 - 8, H - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = tz_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(W - 12 - 75, H - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = tz_cancel;
    w->focus = lb;
}

/* ---------------- painting ---------------- */
static void draw_preview(gc_t *g, cpanel_t *c, int x, int y) {
    const theme_t *th = &g_themes[CLAMP(c->theme, 0, g_theme_count - 1)];
    /* monitor */
    rect_t mon = mkrect(x, y, TH_W + 16, TH_H + 16);
    gfx_rounded(g, mon, 6, 15, HEX(0x5A6170), HEX(0x2A2F3A));
    gfx_rounded_outline(g, mon, 6, 15, HEX(0x1A1E26));
    gfx_hline(g, mon.x + 6, mon.y + 1, mon.w - 12, HEX(0x8890A0));
    gfx_fill(g, mkrect(mon.x + mon.w / 2 - 16, mon.y + mon.h, 32, 8), HEX(0x4A505C));
    gfx_rounded(g, mkrect(mon.x + mon.w / 2 - 44, mon.y + mon.h + 8, 88, 7), 3, 15, HEX(0x6A7080), HEX(0x343842));
    gfx_fill(g, mkrect(mon.x + mon.w - 16, mon.y + mon.h - 6, 4, 2), HEX(0x60E080));   /* power LED */
    rect_t scr = mkrect(x + 8, y + 8, TH_W, TH_H);
    gc_t sg = gc_sub(g, scr);
    surface_t *t = c->wall >= 0 && c->wall < NWP ? c->thumb[c->wall] : NULL;
    if (t) gfx_blit(&sg, 0, 0, t, mkrect(0, 0, TH_W, TH_H));
    else gfx_fill(&sg, mkrect(0, 0, TH_W, TH_H), HEX(0x2F6E8E));
    /* desktop icons */
    for (int i = 0; i < 3; i++) gfx_icon(&sg, 4, 4 + i * 22, &icons16[i == 0 ? ICON_COMPUTER : i == 1 ? ICON_DOCUMENTS : ICON_TERMINAL]);
    /* mini window */
    rect_t wr = mkrect(34, 18, 112, 72);
    gfx_fill_alpha(&sg, mkrect(wr.x + 2, wr.y + 3, wr.w, wr.h), 0xFF000000, 60);
    gfx_rounded(&sg, mkrect(wr.x, wr.y, wr.w, 12), 4, 3, th->title_top, th->title_bottom);
    gfx_fill(&sg, mkrect(wr.x, wr.y + 12, wr.w, wr.h - 12), th->frame);
    gfx_fill(&sg, mkrect(wr.x + 2, wr.y + 12, wr.w - 4, wr.h - 14), th->face);
    gfx_text_box(&sg, &font_small, mkrect(wr.x + 5, wr.y, 70, 12), "Window", th->title_text, 0);
    gfx_rounded(&sg, mkrect(wr.x + wr.w - 11, wr.y + 2, 9, 8), 2, 15, HEX(0xF59A80), HEX(0xC53A1C));
    gfx_fill(&sg, mkrect(wr.x + 6, wr.y + 18, wr.w - 12, 30), HEX(0xFFFFFF));
    gfx_rect(&sg, mkrect(wr.x + 6, wr.y + 18, wr.w - 12, 30), HEX(0x7F9DB9));
    gfx_fill(&sg, mkrect(wr.x + 10, wr.y + 22, 50, 8), th->select);
    gfx_rounded(&sg, mkrect(wr.x + wr.w - 38, wr.y + 53, 32, 13), 2, 15, HEX(0xFFFFFF), HEX(0xE3E1D8));
    gfx_rounded_outline(&sg, mkrect(wr.x + wr.w - 38, wr.y + 53, 32, 13), 2, 15, th->button_border);
    /* taskbar */
    rect_t tb = mkrect(0, TH_H - 10, TH_W, 10);
    gfx_gradient_v(&sg, tb, th->taskbar_top, th->taskbar_bottom);
    gfx_hline(&sg, 0, tb.y, TH_W, th->taskbar_line);
    gfx_rounded(&sg, mkrect(0, tb.y, 26, 10), 4, 6, th->start_top, th->start_bottom);
    gfx_rounded(&sg, mkrect(30, tb.y + 2, 30, 7), 2, 15, color_lighten(th->taskbar_bottom, -20), th->taskbar_bottom);
    gfx_fill(&sg, mkrect(TH_W - 24, tb.y + 1, 24, 9), color_lighten(th->sidebar_bottom, -20));
    /* glass reflection */
    for (int i = 0; i < 40; i++) gfx_fill_alpha(&sg, mkrect(TH_W - 70 + i, 0, 1, 40 - i), HEX(0xFFFFFF), 10);
}

static void draw_clock(gc_t *g, int cx, int cy, int r, struct rtc_time *tm) {
    const theme_t *th = g_theme;
    gfx_circle(g, cx + 1, cy + 2, r + 2, HEX(0xB0B0A8));
    gfx_circle(g, cx, cy, r + 2, color_lighten(th->menu_header_bottom, -10));
    gfx_circle(g, cx, cy, r, HEX(0xFFFFFF));
    gfx_circle(g, cx, cy - r / 3, r * 2 / 3, HEX(0xFAFBFD));
    for (int i = 0; i < 60; i++) {
        double a = i * K_PI / 30;
        double s = k_sin(a), co = k_cos(a);
        int big = i % 5 == 0;
        int r0 = big ? r - 8 : r - 4;
        if (big) {
            for (int d = -1; d <= 0; d++)
                gfx_line(g, cx + (int)(s * r0) + d, cy - (int)(co * r0), cx + (int)(s * (r - 2)) + d, cy - (int)(co * (r - 2)), HEX(0x2A3240));
        } else {
            gfx_pixel(g, cx + (int)(s * (r - 3)), cy - (int)(co * (r - 3)), HEX(0x8A90A0));
        }
    }
    double sec = tm->second, min = tm->minute + sec / 60.0, hr = (tm->hour % 12) + min / 60.0;
    struct { double a; int len, w; color_t c; } hands[3] = {
        { hr * K_PI / 6, r * 5 / 10, 2, HEX(0x1C2230) },
        { min * K_PI / 30, r * 8 / 10, 1, HEX(0x1C2230) },
        { sec * K_PI / 30, r * 85 / 100, 0, HEX(0xD03A20) },
    };
    for (int h = 0; h < 3; h++) {
        int x1 = cx + (int)(k_sin(hands[h].a) * hands[h].len), y1 = cy - (int)(k_cos(hands[h].a) * hands[h].len);
        int bx = cx - (int)(k_sin(hands[h].a) * (h == 2 ? 8 : 3)), by = cy + (int)(k_cos(hands[h].a) * (h == 2 ? 8 : 3));
        for (int dx = -hands[h].w / 2; dx <= hands[h].w / 2; dx++)
            for (int dy = -hands[h].w / 2; dy <= hands[h].w / 2; dy++)
                gfx_line(g, bx + dx, by + dy, x1 + dx, y1 + dy, hands[h].c);
    }
    gfx_circle(g, cx, cy, 3, HEX(0xD03A20));
    gfx_pixel(g, cx, cy, HEX(0xFFFFFF));
}

static void cp_paint(window_t *w, gc_t *g) {
    cpanel_t *c = w->data;
    const theme_t *th = g_theme;
    int cw = w->client->w;
    /* banner */
    gfx_gradient_v(g, mkrect(0, 0, cw, BANNER_H), th->menu_header_top, th->menu_header_bottom);
    gfx_gradient_v(g, mkrect(0, BANNER_H, cw, 2), th->hover, color_lighten(th->hover, -40));
    gfx_icon(g, 14, (BANNER_H - 32) / 2, &icons32[ICON_CONTROL]);
    color_t sh = color_lighten(th->menu_header_bottom, -70);
    gfx_text(g, &font_large, 57, 9, "Control Panel", sh);
    gfx_text(g, &font_large, 56, 8, "Control Panel", HEX(0xFFFFFF));
    gfx_text(g, &font_ui, 57, 34, "Personalize Halcyon and review your system.", color_lighten(th->menu_header_top, 150));

    /* appearance */
    draw_group(g, mkrect(12, 68, COL_W, 304), "Appearance");
    draw_preview(g, c, 12 + (COL_W - TH_W - 16) / 2, 88);

    /* date and time */
    struct rtc_time tm;
    rtc_read(&tm);
    c->last_sec = tm.second;
    rect_t dg = mkrect(COL2, 68, COL_W, 118);
    draw_group(g, dg, "Date and Time");
    draw_clock(g, dg.x + 54, dg.y + 64, 40, &tm);
    static const char *days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };
    char t1[32], t2[32], t3[32];
    int h12 = tm.hour % 12 == 0 ? 12 : tm.hour % 12;
    snprintf(t1, sizeof t1, "%d:%02d:%02d", h12, tm.minute, tm.second);
    int tx = dg.x + 112;
    int ex = gfx_text(g, &font_large, tx, dg.y + 22, t1, color_lighten(th->menu_header_bottom, -60));
    gfx_text(g, &font_ui_bold, ex + 4, dg.y + 30, tm.hour < 12 ? "AM" : "PM", color_lighten(th->menu_header_bottom, -30));
    snprintf(t2, sizeof t2, "%s,", days[weekday(tm.year, tm.month, tm.day)]);
    snprintf(t3, sizeof t3, "%s %d, %d", tm.month >= 1 && tm.month <= 12 ? months[tm.month - 1] : "?", tm.day, tm.year);
    gfx_text(g, &font_ui, tx, dg.y + 52, t2, HEX(0x1C2230));
    gfx_text(g, &font_ui, tx, dg.y + 67, t3, HEX(0x1C2230));

    /* system */
    rect_t sg = mkrect(COL2, 196, COL_W, 176);
    draw_group(g, sg, "System");
    struct mm_stats st;
    mm_get_stats(&st);
    char cpu[64], mem[64], heap[64], disp[48], boot[32];
    trimmed(cpu_brand(), cpu, sizeof cpu);
    if (!cpu[0]) trimmed(cpu_vendor(), cpu, sizeof cpu);
    snprintf(mem, sizeof mem, "%u MB", (unsigned)((st.total_ram + (1 << 19)) >> 20));
    snprintf(heap, sizeof heap, "%u of %u MB in use", (unsigned)(st.heap_used >> 20), (unsigned)(st.heap_size >> 20));
    snprintf(disp, sizeof disp, "%d x %d, %u-bit", screen_w(), screen_h(), (unsigned)g_bootinfo.fb_bpp);
    strlcpy(boot, g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "BIOS", sizeof boot);
    int y = sg.y + 22;
    gfx_icon(g, sg.x + 12, y, &icons32[ICON_COMPUTER]);
    gfx_text(g, &font_ui_bold, sg.x + 52, y + 2, "Halcyon " OS_VERSION, HEX(0x1C2230));
    gfx_text_box(g, &font_ui, mkrect(sg.x + 52, y + 17, sg.w - 60, 14), cpu, HEX(0x4A5060), 0);
    y += 44;
    const char *lbl[4] = { "Memory:", "Kernel heap:", "Display:", "Boot mode:" };
    const char *val[4] = { mem, heap, disp, boot };
    for (int i = 0; i < 4; i++) {
        gfx_text(g, &font_ui, sg.x + 14, y, lbl[i], color_lighten(th->menu_header_bottom, -40));
        gfx_text_box(g, &font_ui, mkrect(sg.x + 96, y, sg.w - 106, font_ui.height), val[i], HEX(0x1C2230), 0);
        y += 20;
    }
    gfx_text(g, &font_small, sg.x + 14, y + 4, "Theme and wallpaper apply to the whole desktop.", HEX(0x8A8A8A));
}

/* ---------------- behaviour ---------------- */
static int pending(cpanel_t *c) { return c->theme != theme_index() || c->wall != desktop_wallpaper_style(); }

static void update_apply(window_t *w) {
    widget_t *a = widget_find(w, ID_APPLY);
    if (!a) return;
    if (pending(w->data)) a->style &= ~WS_DISABLED;
    else {
        a->style |= WS_DISABLED;
        if (w->focus == a) w->focus = widget_find(w, ID_OK);
    }
    wm_invalidate(w);
}

static void apply(window_t *w) {
    cpanel_t *c = w->data;
    if (c->theme != theme_index()) theme_set(c->theme);
    if (c->wall != desktop_wallpaper_style()) {
        wm_set_busy(1);
        desktop_wallpaper_set(c->wall);
        wm_set_busy(0);
    }
    settings_set_int("desktop.theme", theme_index());
    settings_set_int("desktop.wallpaper", desktop_wallpaper_style());
    settings_save();
    update_apply(w);
}

static void list_change(window_t *w, widget_t *lb) {
    cpanel_t *c = w->data;
    if (lb->value < 0) return;
    if (lb->id == ID_THEMES) c->theme = lb->value;
    else { c->wall = lb->value; thumbnail(c, c->wall); }
    update_apply(w);
}
static void on_apply(window_t *w, widget_t *b) { apply(w); }
static void on_display(window_t *w, widget_t *b) { (void)w; (void)b; app_display(); }
static void on_ok(window_t *w, widget_t *b) { apply(w); wm_close(w); }
static void on_cancel(window_t *w, widget_t *b) { wm_close(w); }
static void on_keyboard(window_t *w, widget_t *b) { app_keyboard(); }
static void on_network(window_t *w, widget_t *b) { (void)w; (void)b; app_network(); }
static void on_sound(window_t *w, widget_t *b) { (void)w; (void)b; app_sound(); }
static void on_timezone(window_t *w, widget_t *b) { (void)b; app_timezone(w); }

static void cp_tick(window_t *w) {
    cpanel_t *c = w->data;
    struct rtc_time tm;
    rtc_read(&tm);
    if (tm.second != c->last_sec) wm_invalidate(w);
}

static void cp_destroy(window_t *w) {
    cpanel_t *c = w->data;
    if (c) {
        for (int i = 0; i < NWP; i++) surface_free(c->thumb[i]);
        kfree(c);
    }
    w->data = NULL;
    if (cp_win == w) cp_win = NULL;
}

void app_control(void) {
    if (cp_win && wm_alive(cp_win)) { wm_show(cp_win); return; }
    cpanel_t *c = kzalloc(sizeof *c);
    if (!c) return;
    c->theme = theme_index();
    c->wall = CLAMP(desktop_wallpaper_style(), 0, NWP - 1);
    c->last_sec = -1;
    thumbnail(c, c->wall);
    window_t *w = wm_create("Control Panel", ICON_CONTROL, -1, -1, CP_W, CP_H, WF_NO_MAXIMIZE);
    if (w) w->help = "control";              /* F1 */
    w->data = c;
    w->on_paint = cp_paint;
    w->on_tick = cp_tick;
    w->on_destroy = cp_destroy;

    widget_t *l1 = widget_add(w, W_LABEL, mkrect(LB_X, 272, 110, 16), "Theme:", 0);
    widget_t *l2 = widget_add(w, W_LABEL, mkrect(LB_X + 124, 272, 110, 16), "Wallpaper:", 0);
    (void)l1; (void)l2;
    widget_t *tl = widget_add(w, W_LISTBOX, mkrect(LB_X, 290, 114, 4 * 18 + 2), "", ID_THEMES);
    char *items[8];
    int n = MIN(g_theme_count, 8);
    for (int i = 0; i < n; i++) items[i] = (char *)g_themes[i].name;
    widget_list_set(w, tl, items, n);
    tl->value = c->theme;
    tl->on_change = list_change;
    widget_t *wl = widget_add(w, W_LISTBOX, mkrect(LB_X + 124, 290, 114, 4 * 18 + 2), "", ID_WALLS);
    widget_list_set(w, wl, (char **)wp_names, NWP);
    wl->value = c->wall;
    wl->on_change = list_change;

    widget_t *ok = widget_add(w, W_BUTTON, mkrect(CP_W - 12 - 3 * 75 - 2 * 8, CP_H - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(CP_W - 12 - 2 * 75 - 8, CP_H - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(CP_W - 12 - 75, CP_H - 35, 75, 23), "Apply", ID_APPLY);
    widget_t *db = widget_add(w, W_BUTTON, mkrect(12, CP_H - 35, 80, 23), "Display...", 0);
    db->on_click = on_display;
    ap->style |= WS_DISABLED; ap->on_click = on_apply;
    widget_t *kb = widget_add(w, W_BUTTON, mkrect(98, CP_H - 35, 80, 23), "Keyboard...", 0);
    kb->on_click = on_keyboard;
    widget_t *nb = widget_add(w, W_BUTTON, mkrect(184, CP_H - 35, 80, 23), "Network...", 0);
    nb->on_click = on_network;
    widget_t *sb = widget_add(w, W_BUTTON, mkrect(270, CP_H - 35, 80, 23), "Sounds...", 0);
    sb->on_click = on_sound;
    strlcpy(sb->tip, "Sounds and Audio Devices: volume, sound cards, system sounds", sizeof sb->tip);
    widget_t *tz = widget_add(w, W_BUTTON, mkrect(COL2 + 112, 68 + 88, 138, 22), "", ID_TZBTN);
    tz->on_click = on_timezone;
    strlcpy(tz->tip, "The difference between this computer's clock and UTC", sizeof tz->tip);
    tz_button_text(w);
    w->focus = tl;
    cp_win = w;
}
