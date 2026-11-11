#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "drivers/pci.h"

void panic_screen(const char *title, const char *detail) {
    (void)title; (void)detail;
    u32 *fb = (u32 *)(uptr)g_bootinfo.fb_addr;
    if (!fb) return;
    for (u32 y = 0; y < g_bootinfo.fb_height; y++)
        for (u32 x = 0; x < g_bootinfo.fb_width; x++)
            fb[y * (g_bootinfo.fb_pitch / 4) + x] = 0x800000;
}

void kmain(struct bootinfo *bi) {
    memcpy(&g_bootinfo, bi, sizeof g_bootinfo);
    serial_init();
    kprintf("\n%s %s kernel starting (boot type %u)\n", OS_NAME, OS_VERSION, g_bootinfo.boot_type);
    if (g_bootinfo.magic != BOOTINFO_MAGIC) panic("bad bootinfo magic");
    cpu_init();
    kprintf("cpu: %s (%s)\n", cpu_brand(), cpu_vendor());
    mm_init();
    time_init();
    kprintf("fb: %ux%u bpp %u pitch %u at %lx\n", g_bootinfo.fb_width, g_bootinfo.fb_height,
            g_bootinfo.fb_bpp, g_bootinfo.fb_pitch, g_bootinfo.fb_addr);
    pci_init();
    u32 *fb = (u32 *)(uptr)g_bootinfo.fb_addr;
    for (u32 y = 0; y < g_bootinfo.fb_height; y++)
        for (u32 x = 0; x < g_bootinfo.fb_width; x++)
            fb[y * (g_bootinfo.fb_pitch / 4) + x] = ((y * 255 / g_bootinfo.fb_height) << 0) | ((x * 255 / g_bootinfo.fb_width) << 8);
    kprintf("done.\n");
    for (;;) __asm__ volatile("hlt");
}
