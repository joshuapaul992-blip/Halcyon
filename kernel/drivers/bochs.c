/* Halcyon OS - Bochs / QEMU / VirtualBox display adapter ("DISPI" VBE
 * extensions): QEMU's std-vga (1234:1111, also -device bochs-display) and
 * VirtualBox's VGA (80EE:BEEF). One head, one connector (VGA-1), modes
 * switch at once, no hardware cursor, no vertical blank (single buffered).
 * The EDID is read from the MMIO BAR where QEMU puts it (offset 0). The
 * scanout buffer is the start of VRAM (BAR 0), so the firmware's frame
 * buffer and ours are the same memory. */
#include "display.h"
#include "pci.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"

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

typedef struct bochs {
    pci_dev_t *pci;
    volatile u8 *mmio;          /* BAR2 (QEMU std-vga, bochs-display); NULL: I/O ports */
    u64 fb, vram;
    int maxw, maxh;
    u16 id;
    u16 saved[10];              /* the firmware's registers (restore) */
    int saved_ok;
} bochs_t;

static bochs_t bx;

static u16 rd(int r) {
    if (bx.mmio) return mmio_read16(bx.mmio + 0x500 + r * 2);
    outw(0x1CE, (u16)r);
    return inw(0x1CF);
}
static void wr(int r, u16 v) {
    if (bx.mmio) { mmio_write16(bx.mmio + 0x500 + r * 2, v); return; }
    outw(0x1CE, (u16)r);
    outw(0x1CF, v);
}

static int program(int w, int h) {
    wr(DISPI_ENABLE, 0);
    wr(DISPI_BPP, 32);
    wr(DISPI_XRES, (u16)w);
    wr(DISPI_YRES, (u16)h);
    wr(DISPI_BANK, 0);
    wr(DISPI_VIRT_W, (u16)w);
    wr(DISPI_VIRT_H, (u16)h);
    wr(DISPI_XOFF, 0);
    wr(DISPI_YOFF, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB | DISPI_NOCLEAR);
    if (rd(DISPI_XRES) != w || rd(DISPI_YRES) != h || rd(DISPI_BPP) != 32) return E_IO;
    return 0;
}

static int bochs_enable(display_adapter_t *a) {
    for (int r = 1; r <= 9; r++) bx.saved[r] = rd(r);
    bx.saved_ok = 1;
    return 0;
}

static void bochs_restore(display_adapter_t *a) {
    if (!bx.saved_ok) return;
    wr(DISPI_ENABLE, 0);
    for (int r = 1; r <= 9; r++) if (r != DISPI_ENABLE) wr(r, bx.saved[r]);
    wr(DISPI_ENABLE, bx.saved[DISPI_ENABLE] | DISPI_NOCLEAR);
    kprintf("bochs: the firmware's mode %ux%u is back\n", bx.saved[DISPI_XRES], bx.saved[DISPI_YRES]);
}

static void bochs_detect(display_adapter_t *a) {
    display_connector_t *c = &a->conns[0];
    u8 e[256];
    int len = 0;
    if (bx.mmio && mmio_read8(bx.mmio) == 0x00 && mmio_read8(bx.mmio + 1) == 0xFF) {
        for (int i = 0; i < 256; i++) e[i] = mmio_read8(bx.mmio + i);
        len = e[126] ? 256 : 128;
    }
    display_connector_update(c, CONN_CONNECTED, len ? e : NULL, len, 0, 0);
}

static int bochs_mode_valid(display_adapter_t *a, display_connector_t *c, const display_mode_t *m) {
    /* the native size of an EDID may be odd: the adapter wants multiples of 8 */
    return !(m->w & 7) || (m->flags & DT_DRIVER);
}

static int bochs_fb_alloc(display_adapter_t *a, int w, int h, display_fb_t *fb) {
    if ((u64)w * h * 4 > bx.vram) return E_NOSPC;
    memset(fb, 0, sizeof *fb);
    fb->phys = bx.fb;
    fb->px = (void *)(uptr)bx.fb;
    fb->w = w;
    fb->h = h;
    fb->pitch = (u32)w * 4;
    fb->bpp = 32;
    fb->rpos = 16; fb->gpos = 8; fb->bpos = 0;
    return 0;
}

static int bochs_set_mode(display_head_t *h, display_connector_t *c, const display_mode_t *m, display_fb_t *fb) {
    if (!m) return 0;                       /* the only head: it stays on */
    int r = program(m->w, m->h);
    if (r) return r;
    int vw = rd(DISPI_VIRT_W);
    if (vw && (u32)vw * 4 != fb->pitch) fb->pitch = (u32)vw * 4;
    return 0;
}

static const display_ops_t bochs_ops = {
    .enable = bochs_enable,
    .restore = bochs_restore,
    .detect = bochs_detect,
    .mode_valid = bochs_mode_valid,
    .fb_alloc = bochs_fb_alloc,
    .set_mode = bochs_set_mode,
};

void bochs_display_probe(void) {
    pci_dev_t *d = NULL;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *x = pci_get(i);
        if ((x->vendor == 0x1234 && x->device == 0x1111) || (x->vendor == 0x80EE && x->device == 0xBEEF)) { d = x; break; }
    }
    if (!d || d->driver || d->bar[0].is_io || !d->bar[0].addr) return;
    pci_enable(d);
    bx.pci = d;
    if (d->vendor == 0x1234 && !d->bar[2].is_io && d->bar[2].addr && d->bar[2].size >= 0x1000)
        bx.mmio = pci_map_bar(d, 2);
    bx.id = rd(DISPI_ID);
    if (bx.id < 0xB0C0 || bx.id > 0xB0CF) {
        kprintf("display: %04x:%04x has no Bochs VBE interface (id %04x)\n", d->vendor, d->device, bx.id);
        return;
    }
    bx.fb = d->bar[0].addr;
    bx.vram = (u64)rd(DISPI_VRAM_64K) * 65536;
    if (!bx.vram || bx.vram > d->bar[0].size) bx.vram = d->bar[0].size;
    /* the frame buffer must be the one the firmware used, or be mappable */
    if (bx.fb + bx.vram > 0x100000000ull && !mm_map_mmio(bx.fb, bx.vram)) return;
    u16 en = rd(DISPI_ENABLE);
    wr(DISPI_ENABLE, en | DISPI_GETCAPS);
    bx.maxw = rd(DISPI_XRES);
    bx.maxh = rd(DISPI_YRES);
    wr(DISPI_ENABLE, en);
    const char *name = d->vendor == 0x80EE ? "VirtualBox display adapter" : "QEMU/Bochs display adapter";
    display_adapter_t *a = display_register("bochs", name, &bochs_ops, &bx, d);
    if (!a) return;
    a->live = 1;
    a->virtual_sizes = 1;
    a->max_w = bx.maxw;
    a->max_h = bx.maxh;
    a->vram = bx.vram;
    display_add_head(a, 0, 0);
    display_add_connector(a, "VGA-1", CONN_VGA, 1);
    d->driver = "display";
    kprintf("display: %s (Bochs VBE %04x, %llu MiB, up to %dx%d, %s registers)\n", name, bx.id,
            (unsigned long long)(bx.vram >> 20), bx.maxw, bx.maxh, bx.mmio ? "MMIO" : "I/O port");
}
