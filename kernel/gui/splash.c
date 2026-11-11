/* Halcyon OS - boot splash screen with progress bar, or (F8 -> verbose boot)
 * a scrolling on-screen console showing the kernel log */
#include "core/irq.h"
#include "gfx.h"
#include "core/lib.h"
#include "core/kernel.h"
#include "drivers/input.h"

static int splash_on, verbose;

/* ---------------- verbose console ---------------- */
#define CON_TOP 28
static int con_x, con_y, con_cw, con_lh, con_rows, con_cols;

static void con_header(void) {
    gc_t g = gc_for(g_screen);
    gfx_gradient_v(&g, mkrect(0, 0, screen_w(), CON_TOP - 4), HEX(0x2E6FDB), HEX(0x1A4AB0));
    gfx_icon(&g, 6, 4, &icons16[ICON_LOGO]);
    gfx_text(&g, &font_ui_bold, 28, 6, "Halcyon " OS_VERSION " - verbose boot", HEX(0xFFFFFF));
    gfx_text(&g, &font_ui, screen_w() - 330, 6, "The log is saved to \\HALCYON\\BOOT.LOG", HEX(0xDCE6F8));
    screen_present(mkrect(0, 0, screen_w(), CON_TOP));
}

static void con_newline(void) {
    con_x = 0;
    if (++con_y < con_rows) return;
    /* scroll the console area up one line */
    con_y = con_rows - 1;
    int y0 = CON_TOP, h = con_rows * con_lh;
    memmove(g_screen->px + y0 * g_screen->stride, g_screen->px + (y0 + con_lh) * g_screen->stride,
            (size_t)(h - con_lh) * g_screen->stride * 4);
    gc_t g = gc_for(g_screen);
    gfx_fill(&g, mkrect(0, y0 + h - con_lh, screen_w(), con_lh), HEX(0x000000));
    screen_present(mkrect(0, y0, screen_w(), h));
}

static void con_write(const char *s, int n) {
    gc_t g = gc_for(g_screen);
    int dirty_y0 = con_y;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') { con_newline(); dirty_y0 = MIN(dirty_y0, con_y); continue; }
        if (c == '\r') continue;
        if (con_x >= con_cols) { con_newline(); dirty_y0 = MIN(dirty_y0, con_y); }
        char t[2] = { c, 0 };
        color_t col = HEX(0xC8D0DC);
        gfx_text(&g, &font_mono, 4 + con_x * con_cw, CON_TOP + con_y * con_lh, t, col);
        con_x++;
    }
    screen_present(mkrect(0, CON_TOP + dirty_y0 * con_lh, screen_w(), (con_y - dirty_y0 + 1) * con_lh));
}

static void console_start(void) {
    gc_t g = gc_for(g_screen);
    gfx_fill(&g, mkrect(0, 0, screen_w(), screen_h()), HEX(0x000000));
    screen_present(mkrect(0, 0, screen_w(), screen_h()));
    con_cw = gfx_char_width(&font_mono, 'M');
    con_lh = font_mono.height;
    con_cols = (screen_w() - 8) / con_cw;
    con_rows = (screen_h() - CON_TOP - 4) / con_lh;
    con_header();
    /* replay what was logged before the console existed */
    static char early[16384];
    size_t n = klog_read(early, sizeof early);
    con_write(early, (int)n);
    klog_hook = con_write;
}

/* ---------------- splash ---------------- */
static void draw(int pct, const char *msg) {
    gc_t g = gc_for(g_screen);
    int w = screen_w(), h = screen_h();
    int cx = w / 2, cy = h / 2 - 40;
    rect_t area = mkrect(0, cy + 70, w, 90);
    gfx_fill(&g, area, HEX(0x000000));
    rect_t tr = mkrect(cx - 110, cy + 90, 220, 18);
    gfx_rounded(&g, tr, 5, 15, HEX(0x1A1A1A), HEX(0x101010));
    gfx_rounded_outline(&g, tr, 5, 15, HEX(0x5A5A5A));
    int fill = (tr.w - 8) * CLAMP(pct, 0, 100) / 100;
    for (int x = 0; x + 10 <= fill; x += 13)
        gfx_rounded(&g, mkrect(tr.x + 4 + x, tr.y + 4, 10, 10), 2, 15, HEX(0x8FD0FF), HEX(0x2465D8));
    int tw = gfx_text_width(&font_ui, msg);
    gfx_text(&g, &font_ui, cx - tw / 2, cy + 122, msg, HEX(0x8A9AB8));
    if (g_bootinfo.boot_flags) {
        const char *m = "Advanced startup options are active";
        gfx_text(&g, &font_small, cx - gfx_text_width(&font_small, m) / 2, cy + 142, m, HEX(0xF5B547));
    }
    screen_present(area);
}

void splash_init(void) {
    screen_init();
    if (g_bootinfo.boot_flags & BOOTF_VERBOSE) {
        verbose = 1;
        console_start();
        return;
    }
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
    if (verbose) { kprintf("[boot] %s\n", msg); return; }
    if (!splash_on) return;
    draw(pct, msg);
}

/* verbose boot: leave the log on screen until a key is pressed (max 15 s) */
void splash_finish(void) {
    if (!verbose) return;
    kprintf("\nBoot complete. Press any key or click to start the desktop (continuing in 15 seconds)...\n");
    u64 end = time_ms() + 15000;
    while (time_ms() < end) {
        u32 gen = irq_generation();
        irq_service();
        input_poll_devices();
        u64 wake = idle_take_deadline();
        input_event_t e;
        int go = 0;
        while (input_get(&e)) if ((e.type == EV_KEY && e.pressed) || (e.type == EV_MOUSE && e.buttons)) go = 1;
        if (go) break;
        idle_wait(gen, MIN(wake, end));
    }
    klog_hook = NULL;
}
