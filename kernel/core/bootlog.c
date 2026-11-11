/* Halcyon OS - hardware summary and \HALCYON\BOOT.LOG
 *
 * Real machines rarely have a serial port, so the kernel log is written to the
 * boot drive: once the desktop is up, on shutdown/restart, and (best effort)
 * after a crash. The previous boot's log is kept as BOOTPREV.LOG. */
#include "kernel.h"
#include "lib.h"
#include "irq.h"
#include "mm.h"
#include "io.h"
#include "fs/vfs.h"
#include "fs/block.h"
#include "usb/usb.h"
#include "drivers/pci.h"

static void irq_report(void);

static int rotated;
static int saving;

static const char *mmap_type(u32 t) {
    switch (t) {
    case MMAP_USABLE: return "usable";
    case MMAP_ACPI: return "ACPI reclaim";
    case MMAP_NVS: return "ACPI NVS";
    case MMAP_BAD: return "bad";
    }
    return "reserved";
}

/* early: firmware-provided information (called right after mm/time init) */
void hwreport_early(void) {
    struct bootinfo *b = &g_bootinfo;
    u32 a, bx, c, d;
    cpuid(1, 0, &a, &bx, &c, &d);
    u32 fam = (a >> 8) & 15, model = (a >> 4) & 15, step = a & 15;
    if (fam == 15) fam += (a >> 20) & 0xFF;
    if (fam == 6 || fam >= 15) model |= ((a >> 16) & 15) << 4;
    kprintf("hw: CPU family %u model %u stepping %u, features%s%s%s%s\n", fam, model, step,
            (d & (1u << 25)) ? " sse" : "", (d & (1u << 26)) ? " sse2" : "", (c & 1) ? " sse3" : "", (c & (1u << 28)) ? " avx" : "");
    kprintf("hw: boot flags %#x%s, firmware %s, boot disk sig %08x part LBA %u\n", b->boot_flags,
            b->boot_flags ? " (advanced startup options)" : "", b->boot_type == BOOT_TYPE_UEFI ? "UEFI" : "BIOS",
            b->boot_disk_sig, b->boot_part_lba);
    kprintf("hw: video %ux%u %ubpp pitch %u, R%u/%u G%u/%u B%u/%u\n", b->fb_width, b->fb_height, b->fb_bpp, b->fb_pitch,
            b->fb_red_pos, b->fb_red_size, b->fb_green_pos, b->fb_green_size, b->fb_blue_pos, b->fb_blue_size);
    kprintf("hw: memory map (%u entries):\n", b->mmap_count);
    for (u32 i = 0; i < b->mmap_count; i++)
        kprintf("  %016lx - %016lx %s\n", b->mmap[i].base, b->mmap[i].base + b->mmap[i].length - 1, mmap_type(b->mmap[i].type));
}

/* late: what the drivers found (called when the desktop starts) */
void hwreport_late(void) {
    kprintf("hw: ---- device summary ----\n");
    for (usb_hc_t *hc = usb_first_hc(); hc; hc = hc->next) {
        kprintf("hw: USB %s, %u ports\n", hc->desc, hc->nports);
        for (usb_device_t *u = usb_first_device(); u; u = u->next) {
            if (u->hc != hc) continue;
            kprintf("hw:   port %u depth %u: %04x:%04x %s, %s, class %02x, %s\n", u->port, u->hub_depth, u->desc.idVendor,
                    u->desc.idProduct, u->product, usb_speed_name(u->speed), u->desc.bDeviceClass,
                    u->description[0] ? u->description : "(no driver)");
        }
    }
    for (blockdev_t *bd = blockdev_first(); bd; bd = bd->next)
        kprintf("hw: disk %s: %s, %lu x %u%s\n", bd->name, bd->model, bd->sector_count, bd->sector_size,
                bd->type == BDEV_PART ? " (partition)" : "");
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        kprintf("hw: volume %c: %s '%s' on %s%s\n", v->letter, v->fs_name, v->label, v->bd ? v->bd->name : "?", v->is_boot ? " (boot)" : "");
    }
    irq_report();
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (!p->driver && (p->class_code == 0x0C || p->class_code == 0x01))
            kprintf("hw: no driver for %02x:%02x.%x %04x:%04x %s\n", p->bus, p->dev, p->func, p->vendor, p->device,
                    pci_class_name(p->class_code, p->subclass, p->prog_if));
    }
}

/* interrupt table into the log, one "hw: irq:" line each */
static void irq_report(void) {
    char *buf = kmalloc(4096);
    if (!buf) return;
    irq_list(buf, 4096);
    char *s = buf;
    while (*s) {
        char *e = strchr(s, '\n');
        if (e) *e = 0;
        kprintf("hw: irq: %s\n", s);
        if (!e) break;
        s = e + 1;
    }
    kfree(buf);
}

/* Write the kernel log to C:\HALCYON\BOOT.LOG. Returns 0 on success. */
int bootlog_save(const char *reason) {
    if (saving) return E_BUSY;
    volume_t *v = NULL;
    for (int i = 0; i < vfs_volume_count(); i++) if (vfs_volume(i)->is_boot) v = vfs_volume(i);
    if (!v) return E_NODEV;
    saving = 1;
    char dir[16], path[32], prev[32];
    snprintf(dir, sizeof dir, "%c:/HALCYON", v->letter);
    snprintf(path, sizeof path, "%c:/HALCYON/BOOT.LOG", v->letter);
    snprintf(prev, sizeof prev, "%c:/HALCYON/BOOTPREV.LOG", v->letter);
    vfs_mkdir(dir);
    if (!rotated) {
        vfs_unlink(prev);
        vfs_rename(path, prev);
        rotated = 1;
    }
    if (strcmp(reason, "desktop starting")) irq_report();
    kprintf("bootlog: saving (%s)\n", reason);
    size_t n = klog_size();
    char *buf = kmalloc(n + 1);
    int r = E_NOMEM;
    if (buf) {
        n = klog_read(buf, n);
        /* CRLF so it opens nicely in any editor */
        size_t lines = 0;
        for (size_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
        char *out = kmalloc(n + lines + 1);
        if (out) {
            size_t o = 0;
            for (size_t i = 0; i < n; i++) { if (buf[i] == '\n') out[o++] = '\r'; out[o++] = buf[i]; }
            r = vfs_write_file(path, out, (u32)o);
            if (r > 0) r = 0;
            kfree(out);
        }
        kfree(buf);
    }
    if (r == 0) vfs_sync_all();
    saving = 0;
    return r;
}
