/* Halcyon OS - "About Halcyon" dialog */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

#define AB_W 440
#define AB_H 300
#define BANNER_H 98

static window_t *about_win;

static void trimmed(const char *s, char *out, int outsz) {
    while (*s == ' ') s++;
    strlcpy(out, s, outsz);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static void about_paint(window_t *w, gc_t *g) {
    const theme_t *th = g_theme;
    int cw = w->client->w, ch = w->client->h;
    /* banner */
    rect_t b = mkrect(0, 0, cw, BANNER_H);
    gfx_gradient_v(g, b, th->menu_header_top, th->menu_header_bottom);
    gc_t bg = gc_sub(g, b);
    for (int i = 0; i < 4; i++) {
        /* soft concentric ripples behind the logo, like a kingfisher's dive */
        int r = 150 - i * 30;
        for (int dy = -r; dy <= r; dy++) {
            int y = 70 + dy;
            if (y < 0 || y >= BANNER_H) continue;
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
            gfx_fill_alpha(&bg, mkrect(cw - 70 - dx, y, 2 * dx, 1), HEX(0xFFFFFF), 12);
        }
    }
    gfx_icon(g, 26, (BANNER_H - 48) / 2, &icons48[ICON_LOGO]);
    color_t shadow = color_lighten(th->menu_header_bottom, -70);
    gfx_text(g, &font_huge, 91, 15, "Halcyon", shadow);
    gfx_text(g, &font_huge, 90, 14, "Halcyon", HEX(0xFFFFFF));
    int x = 90 + gfx_text_width(&font_huge, "Halcyon") + 10;
    gfx_text(g, &font_large, x + 1, 30, OS_VERSION, shadow);
    gfx_text(g, &font_large, x, 29, OS_VERSION, th->hover);
    gfx_text(g, &font_ui, 92, 62, "Operating System", color_lighten(th->menu_header_top, 140));
    gfx_gradient_v(g, mkrect(0, BANNER_H, cw, 3), th->hover, color_lighten(th->hover, -40));

    /* description */
    int y = BANNER_H + 16;
    y += gfx_text_wrap(g, &font_ui, mkrect(24, y, cw - 48, 40),
                       "A hobby operating system with USB 1.1/2.0/3.x, AHCI and FAT12/16/32 support.", HEX(0x1C2230));
    y += 8;
    gfx_hline(g, 24, y, cw - 48, color_lighten(th->face, -30));
    gfx_hline(g, 24, y + 1, cw - 48, HEX(0xFFFFFF));
    y += 12;

    /* system facts */
    struct mm_stats st;
    mm_get_stats(&st);
    char mem[48], cpu[64], boot[48], disp[48];
    snprintf(mem, sizeof mem, "%u MB", (unsigned)((st.total_ram + (1 << 19)) >> 20));
    trimmed(cpu_brand(), cpu, sizeof cpu);
    if (!cpu[0]) trimmed(cpu_vendor(), cpu, sizeof cpu);
    if (!cpu[0]) strlcpy(cpu, "Unknown x86-64 processor", sizeof cpu);
    strlcpy(boot, g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : g_bootinfo.boot_type == BOOT_TYPE_BIOS ? "BIOS (legacy)" : "Unknown",
            sizeof boot);
    snprintf(disp, sizeof disp, "%d x %d, %u-bit colour", screen_w(), screen_h(), (unsigned)g_bootinfo.fb_bpp);
    const char *labels[4] = { "Physical memory:", "Processor:", "Boot mode:", "Display:" };
    const char *values[4] = { mem, cpu, boot, disp };
    for (int i = 0; i < 4; i++) {
        gfx_text(g, &font_ui_bold, 24, y, labels[i], color_lighten(th->menu_header_bottom, -50));
        gfx_text_box(g, &font_ui, mkrect(140, y, cw - 164, font_ui.height), values[i], HEX(0x1C2230), 0);
        y += 21;
    }
    gfx_text(g, &font_small, 24, ch - 30, "Halcyon " OS_VERSION ", built " __DATE__, HEX(0x8A8A8A));
}

static void about_ok(window_t *w, widget_t *b) { wm_close(w); }
static void about_destroy(window_t *w) { if (about_win == w) about_win = NULL; }

void app_about(void) {
    if (about_win && wm_alive(about_win)) { wm_show(about_win); return; }
    window_t *w = wm_create("About Halcyon", ICON_HELP, 0, 0, AB_W, AB_H, WF_DIALOG | WF_CENTER);
    w->on_paint = about_paint;
    w->on_destroy = about_destroy;
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(AB_W - 89, AB_H - 37, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT | WS_CANCEL;
    ok->on_click = about_ok;
    w->focus = ok;
    about_win = w;
}
