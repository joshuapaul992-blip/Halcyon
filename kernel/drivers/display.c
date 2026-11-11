/* Halcyon OS - display adapters: firmware frame buffer and Bochs/QEMU VBE */
#include "display.h"
#include "pci.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

#define MAX_MODES 64

static display_mode_t modes[MAX_MODES];
static int nmodes;
static const char *adapter = "Firmware frame buffer (VESA/GOP)";
static int live;
static edid_info_t monitor;
int (*display_changed_hook)(void);

static int (*hw_set)(int w, int h);       /* live driver */

static void add_mode(int w, int h) {
    if (nmodes >= MAX_MODES || w < 640 || h < 480) return;
    for (int i = 0; i < nmodes; i++) if (modes[i].w == w && modes[i].h == h) return;
    int i = nmodes++;
    while (i > 0 && (modes[i - 1].w > w || (modes[i - 1].w == w && modes[i - 1].h > h))) {
        modes[i] = modes[i - 1];
        i--;
    }
    modes[i].w = w;
    modes[i].h = h;
}

const char *display_adapter(void) { return adapter; }
int display_live(void) { return live; }
int display_mode_count(void) { return nmodes; }
display_mode_t display_mode(int i) { return (i >= 0 && i < nmodes) ? modes[i] : (display_mode_t){ 0, 0 }; }
const edid_info_t *display_monitor(void) { return &monitor; }

int display_find(int w, int h) {
    for (int i = 0; i < nmodes; i++) if (modes[i].w == w && modes[i].h == h) return i;
    return -1;
}

int display_native(int *w, int *h) {
    if (!monitor.valid || !monitor.native_w || display_find(monitor.native_w, monitor.native_h) < 0) return 0;
    *w = monitor.native_w;
    *h = monitor.native_h;
    return 1;
}

static void enable_wc(void) {
    u64 size = (u64)g_bootinfo.fb_pitch * g_bootinfo.fb_height;
    /* only when the whole range (rounded to 2 MiB pages) is a display BAR:
     * never make other device memory write-combining */
    u64 a = g_bootinfo.fb_addr & ~0x1FFFFFull, e = (g_bootinfo.fb_addr + size + 0x1FFFFF) & ~0x1FFFFFull;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code != 0x03) continue;
        for (int b = 0; b < 6; b++) {
            pci_bar_t *bar = &d->bar[b];
            if (bar->is_io || !bar->addr || bar->addr > a || bar->addr + bar->size < e) continue;
            if (mm_set_write_combining(a, e - a)) {
                kprintf("display: frame buffer %llx-%llx write-combining\n", (unsigned long long)a, (unsigned long long)e - 1);
                return;
            }
        }
    }
    kprintf("display: frame buffer stays uncached (no PAT, or not inside a display BAR)\n");
}

/* ---------------- Bochs / QEMU / VirtualBox VBE ("DISPI") ---------------- */
#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_BANK 5
#define DISPI_VIRT_W 6
#define DISPI_VIRT_H 7
#define DISPI_XOFF 8
#define DISPI_YOFF 9
#define DISPI_VRAM_64K 10
#define DISPI_ENABLED 0x01
#define DISPI_GETCAPS 0x02
#define DISPI_LFB 0x40
#define DISPI_NOCLEAR 0x80

static volatile u8 *dispi_mmio;            /* BAR2 (QEMU std-vga, bochs-display) */
static u64 dispi_fb;
static u64 dispi_vram;

static u16 dispi_rd(int r) {
    if (dispi_mmio) return mmio_read16(dispi_mmio + 0x500 + r * 2);
    outw(0x1CE, (u16)r);
    return inw(0x1CF);
}
static void dispi_wr(int r, u16 v) {
    if (dispi_mmio) { mmio_write16(dispi_mmio + 0x500 + r * 2, v); return; }
    outw(0x1CE, (u16)r);
    outw(0x1CF, v);
}

static int dispi_set(int w, int h) {
    if ((u64)w * h * 4 > dispi_vram) return E_NOSPC;
    dispi_wr(DISPI_ENABLE, 0);
    dispi_wr(DISPI_BPP, 32);
    dispi_wr(DISPI_XRES, (u16)w);
    dispi_wr(DISPI_YRES, (u16)h);
    dispi_wr(DISPI_BANK, 0);
    dispi_wr(DISPI_VIRT_W, (u16)w);
    dispi_wr(DISPI_VIRT_H, (u16)h);
    dispi_wr(DISPI_XOFF, 0);
    dispi_wr(DISPI_YOFF, 0);
    dispi_wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB | DISPI_NOCLEAR);
    int rw = dispi_rd(DISPI_XRES), rh = dispi_rd(DISPI_YRES), vw = dispi_rd(DISPI_VIRT_W);
    if (rw != w || rh != h || dispi_rd(DISPI_BPP) != 32) return E_IO;
    g_bootinfo.fb_addr = dispi_fb;
    g_bootinfo.fb_width = (u32)w;
    g_bootinfo.fb_height = (u32)h;
    g_bootinfo.fb_pitch = (u32)(vw ? vw : w) * 4;
    g_bootinfo.fb_bpp = 32;
    g_bootinfo.fb_red_pos = 16; g_bootinfo.fb_green_pos = 8; g_bootinfo.fb_blue_pos = 0;
    g_bootinfo.fb_red_size = g_bootinfo.fb_green_size = g_bootinfo.fb_blue_size = 8;
    return 0;
}

static int dispi_probe(void) {
    pci_dev_t *d = NULL;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *x = pci_get(i);
        if ((x->vendor == 0x1234 && x->device == 0x1111) || (x->vendor == 0x80EE && x->device == 0xBEEF)) { d = x; break; }
    }
    if (!d || d->bar[0].is_io || !d->bar[0].addr) return 0;
    pci_enable(d);
    if (d->vendor == 0x1234 && !d->bar[2].is_io && d->bar[2].addr && d->bar[2].size >= 0x1000)
        dispi_mmio = pci_map_bar(d, 2);
    u16 id = dispi_rd(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF) {
        kprintf("display: %04x:%04x has no Bochs VBE interface (id %04x)\n", d->vendor, d->device, id);
        return 0;
    }
    dispi_fb = d->bar[0].addr;
    dispi_vram = (u64)dispi_rd(DISPI_VRAM_64K) * 65536;
    if (!dispi_vram || dispi_vram > d->bar[0].size) dispi_vram = d->bar[0].size;
    /* the frame buffer must be the one the firmware used, or be mappable */
    if (dispi_fb + dispi_vram > 0x100000000ull && !mm_map_mmio(dispi_fb, dispi_vram)) return 0;
    u16 en = dispi_rd(DISPI_ENABLE);
    dispi_wr(DISPI_ENABLE, en | DISPI_GETCAPS);
    int maxw = dispi_rd(DISPI_XRES), maxh = dispi_rd(DISPI_YRES);
    dispi_wr(DISPI_ENABLE, en);
    if (dispi_mmio && mmio_read8(dispi_mmio) == 0x00 && mmio_read8(dispi_mmio + 1) == 0xFF) {
        u8 e[128];
        for (int i = 0; i < 128; i++) e[i] = mmio_read8(dispi_mmio + i);
        edid_info_t ei;
        if (edid_parse(e, 128, &ei)) monitor = ei;
    }
    static const u16 std[][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 },
        { 1280, 960 }, { 1280, 1024 }, { 1360, 768 }, { 1400, 1050 }, { 1440, 900 }, { 1600, 900 },
        { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2048, 1152 }, { 2560, 1440 },
        { 2560, 1600 }, { 3840, 2160 },
    };
    nmodes = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(std); i++)
        if (std[i][0] <= maxw && std[i][1] <= maxh && (u64)std[i][0] * std[i][1] * 4 <= dispi_vram) add_mode(std[i][0], std[i][1]);
    if (monitor.valid && monitor.native_w <= maxw && monitor.native_h <= maxh && !(monitor.native_w & 7) &&
        (u64)monitor.native_w * monitor.native_h * 4 <= dispi_vram)
        add_mode(monitor.native_w, monitor.native_h);
    adapter = d->vendor == 0x80EE ? "VirtualBox display adapter" : "QEMU/Bochs display adapter";
    live = 1;
    hw_set = dispi_set;
    d->driver = "display";
    kprintf("display: %s (Bochs VBE %04x, %llu MiB, up to %dx%d, %s registers)\n", adapter, id,
            (unsigned long long)(dispi_vram >> 20), maxw, maxh, dispi_mmio ? "MMIO" : "I/O port");
    return 1;
}

/* ---------------- common ---------------- */
int display_set_mode(int w, int h) {
    if (!live || !hw_set) return E_NOTSUP;
    if (display_find(w, h) < 0) return E_INVAL;
    if ((int)g_bootinfo.fb_width == w && (int)g_bootinfo.fb_height == h) return 0;
    int ow = (int)g_bootinfo.fb_width, oh = (int)g_bootinfo.fb_height;
    int r = hw_set(w, h);
    if (r) {
        kprintf("display: %dx%d failed (%s), back to %dx%d\n", w, h, strerror(r), ow, oh);
        hw_set(ow, oh);
        return r;
    }
    enable_wc();
    if (display_changed_hook && (r = display_changed_hook()) != 0) {
        /* the desktop could not follow (memory): go back */
        kprintf("display: %dx%d: the desktop cannot use it (%s), back to %dx%d\n", w, h, strerror(r), ow, oh);
        hw_set(ow, oh);
        display_changed_hook();
        return r;
    }
    kprintf("display: mode %dx%d\n", w, h);
    return 0;
}

int display_save_mode(int w, int h) {
    settings_set_int("display.width", w);
    settings_set_int("display.height", h);
    return settings_save();
}

/* before the desktop starts: a live adapter can use a saved mode the
 * firmware did not offer */
void display_apply_saved(void) {
    int w = settings_get_int("display.width", 0), h = settings_get_int("display.height", 0);
    if (!live || !w || !h || ((int)g_bootinfo.fb_width == w && (int)g_bootinfo.fb_height == h)) return;
    if (display_find(w, h) < 0 || hw_set(w, h)) return;
    kprintf("display: saved mode %dx%d\n", w, h);
    enable_wc();
}

void display_init(void) {
    edid_info_t ei;
    if (g_bootinfo.edid_size && edid_parse(g_bootinfo.edid, (int)g_bootinfo.edid_size, &ei)) monitor = ei;
    if ((g_bootinfo.boot_flags & (BOOTF_SAFE_VIDEO | BOOTF_NO_DISPLAY)) || !dispi_probe()) {
        for (u32 i = 0; i < g_bootinfo.mode_count && i < BOOTINFO_MAX_MODES; i++)
            add_mode(g_bootinfo.modes[i].width, g_bootinfo.modes[i].height);
        add_mode((int)g_bootinfo.fb_width, (int)g_bootinfo.fb_height);
        kprintf("display: %s, %d modes (changes apply at the next start)\n", adapter, nmodes);
    }
    enable_wc();
}
