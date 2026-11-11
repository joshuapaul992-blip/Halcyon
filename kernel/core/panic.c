/* Halcyon OS - fatal error screen, drawn straight to the framebuffer */
#include "kernel.h"
#include "lib.h"
#include "fonts.h"

static void fb_put(u32 x, u32 y, u32 rgb) {
    struct bootinfo *b = &g_bootinfo;
    if (!b->fb_addr || x >= b->fb_width || y >= b->fb_height) return;
    u8 *p = (u8 *)(uptr)b->fb_addr + (u64)y * b->fb_pitch + x * (b->fb_bpp / 8);
    u32 r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, bl = rgb & 255;
    u32 v = (r << b->fb_red_pos) | (g << b->fb_green_pos) | (bl << b->fb_blue_pos);
    if (b->fb_bpp == 32) *(u32 *)p = v;
    else { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
}

static u32 mix(u32 a, u32 b, u32 t) {
    u32 r = (((a >> 16) & 255) * (255 - t) + ((b >> 16) & 255) * t) / 255;
    u32 g = (((a >> 8) & 255) * (255 - t) + ((b >> 8) & 255) * t) / 255;
    u32 bl = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

static int draw_text(const font_t *f, int x, int y, const char *s, u32 fg, u32 bg) {
    int x0 = x;
    for (; *s; s++) {
        if (*s == '\n') { x = x0; y += f->height + 2; continue; }
        int c = (u8)*s;
        if (c < f->first || c > f->last) c = '?';
        const glyph_t *g = &f->glyphs[c - f->first];
        const u8 *bm = f->bitmap + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                u8 a = bm[j * g->w + i];
                if (a) fb_put(x + g->xoff + i, y + g->yoff + j, mix(bg, fg, a));
            }
        x += g->adv;
    }
    return y;
}

void panic_screen(const char *title, const char *detail) {
    struct bootinfo *b = &g_bootinfo;
    if (!b->fb_addr) return;
    const u32 bg = 0x1C2A4A;
    for (u32 y = 0; y < b->fb_height; y++)
        for (u32 x = 0; x < b->fb_width; x++) fb_put(x, y, bg);
    int x = 60, y = 60;
    draw_text(&font_large, x, y, "Halcyon has stopped to protect your computer.", 0xFFFFFF, bg);
    y += 50;
    draw_text(&font_ui_bold, x, y, title, 0xFFE080, bg);
    y += 30;
    y = draw_text(&font_mono, x, y, detail, 0xFFFFFF, bg);
    y += 40;
    draw_text(&font_ui, x, y,
              "If this is the first time you have seen this screen, restart your computer.\n"
              "The kernel log has been written to the serial port (COM1).", 0xC8D4F0, bg);
}
