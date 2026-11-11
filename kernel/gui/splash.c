/* Halcyon OS - boot splash screen with progress bar */
#include "gfx.h"
#include "core/lib.h"
#include "core/kernel.h"

static int splash_on;

static void draw(int pct, const char *msg) {
    gc_t g = gc_for(g_screen);
    int w = screen_w(), h = screen_h();
    int cx = w / 2, cy = h / 2 - 40;
    rect_t area = mkrect(0, cy + 70, w, 90);
    gfx_fill(&g, area, HEX(0x000000));
    /* progress track */
    rect_t tr = mkrect(cx - 110, cy + 90, 220, 18);
    gfx_rounded(&g, tr, 5, 15, HEX(0x1A1A1A), HEX(0x101010));
    gfx_rounded_outline(&g, tr, 5, 15, HEX(0x5A5A5A));
    /* moving blocks + fill */
    int fill = (tr.w - 8) * CLAMP(pct, 0, 100) / 100;
    for (int x = 0; x + 10 <= fill; x += 13)
        gfx_rounded(&g, mkrect(tr.x + 4 + x, tr.y + 4, 10, 10), 2, 15, HEX(0x8FD0FF), HEX(0x2465D8));
    int tw = gfx_text_width(&font_ui, msg);
    gfx_text(&g, &font_ui, cx - tw / 2, cy + 122, msg, HEX(0x8A9AB8));
    screen_present(area);
}

void splash_init(void) {
    screen_init();
    gc_t g = gc_for(g_screen);
    int w = screen_w(), h = screen_h();
    gfx_fill(&g, mkrect(0, 0, w, h), HEX(0x000000));
    int cx = w / 2, cy = h / 2 - 40;
    gfx_icon(&g, cx - 24 - 100, cy - 30, &icons48[ICON_LOGO]);
    gfx_text(&g, &font_huge, cx - 60, cy - 26, "Halcyon", HEX(0xFFFFFF));
    gfx_text(&g, &font_ui_bold, cx + 80, cy - 26, OS_VERSION, HEX(0xF5B547));
    gfx_gradient_h(&g, mkrect(cx - 160, cy + 30, 160, 2), HEX(0x000000), HEX(0x3A7BD5));
    gfx_gradient_h(&g, mkrect(cx, cy + 30, 160, 2), HEX(0x3A7BD5), HEX(0x000000));
    gfx_text(&g, &font_small, cx - 70, cy + 40, "USB  -  AHCI  -  FAT  -  BIOS & UEFI", HEX(0x6A7A98));
    screen_present(mkrect(0, 0, w, h));
    splash_on = 1;
    draw(0, "Starting Halcyon...");
}

void splash_progress(int pct, const char *msg) {
    if (!splash_on) return;
    draw(pct, msg);
}
