/* Halcyon OS - System Information / device manager */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/vfs.h"
#include "fs/block.h"
#include "drivers/pci.h"
#include "drivers/input.h"
#include "usb/usb.h"
#include "fileops.h"

void app_notepad_text(const char *title, const char *text);

#define ROW_H 20
#define SB_W 17
#define MAX_ROWS 512

typedef struct row {
    int depth, icon, cat;
    char text[128];
    char *detail;
} row_t;

typedef struct si {
    row_t *rows; int n;
    int sel, top;
    u8 collapsed[16];
    u32 ugen, vgen;
    int sb_drag;
    window_t *win;
} si_t;

enum { CAT_COMPUTER, CAT_DISPLAY, CAT_DISKS, CAT_VOLUMES, CAT_STORAGE, CAT_USB, CAT_INPUT, CAT_NET, CAT_PCI, CAT_COUNT };
static const char *cat_names[CAT_COUNT] = { "Computer", "Display adapters", "Disk drives", "Volumes", "Storage controllers",
                                            "Universal Serial Bus controllers", "Keyboards and pointing devices", "Network adapters", "PCI devices" };

static void add(si_t *s, int depth, int icon, int cat, const char *detail, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void add(si_t *s, int depth, int icon, int cat, const char *detail, const char *fmt, ...) {
    if (s->n >= MAX_ROWS) return;
    if (depth > 0 && s->collapsed[cat]) return;
    row_t *r = &s->rows[s->n++];
    r->depth = depth; r->icon = icon; r->cat = cat;
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->text, sizeof r->text, fmt, ap);
    va_end(ap);
    r->detail = strdup(detail ? detail : r->text);
}

static void pci_detail(pci_dev_t *d, char *b, int n) {
    int o = snprintf(b, n, "%s\n%s\n\nLocation: PCI bus %u, device %u, function %u\nVendor ID: %04x  Device ID: %04x\n"
                     "Subsystem: %04x:%04x\nClass: %02x.%02x.%02x  Revision: %02x\nDriver: %s\n",
                     pci_class_name(d->class_code, d->subclass, d->prog_if), pci_vendor_name(d->vendor), d->bus, d->dev, d->func,
                     d->vendor, d->device, d->subsys_vendor, d->subsys_id, d->class_code, d->subclass, d->prog_if, d->revision,
                     d->driver ? d->driver : "(none)");
    for (int i = 0; i < 6 && o < n; i++) {
        if (!d->bar[i].addr) continue;
        o += snprintf(b + o, n - o, "BAR%d: %s %#lx (%lu KB)\n", i, d->bar[i].is_io ? "I/O" : "memory", d->bar[i].addr, d->bar[i].size >> 10);
    }
}

static void build(si_t *s) {
    for (int i = 0; i < s->n; i++) kfree(s->rows[i].detail);
    s->n = 0;
    s->ugen = usb_generation();
    s->vgen = vfs_generation();
    char *d = kmalloc(2048);
    struct mm_stats ms; mm_get_stats(&ms);

    /* computer */
    snprintf(d, 2048, "Halcyon %s\n\nProcessor: %s\nVendor: %s\nTSC: %lu MHz\n\nPhysical memory: %lu MB\nKernel heap: %lu MB (%lu KB in use)\n\n"
             "Boot firmware: %s\nBoot disk signature: %08x, partition at LBA %u\nDisplay: %ux%u, %u bpp\nUptime: %lu seconds",
             OS_VERSION, cpu_brand(), cpu_vendor(), tsc_khz() / 1000, ms.total_ram >> 20, ms.heap_size >> 20, ms.heap_used >> 10,
             g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "PC BIOS", g_bootinfo.boot_disk_sig, g_bootinfo.boot_part_lba,
             g_bootinfo.fb_width, g_bootinfo.fb_height, g_bootinfo.fb_bpp, time_ms() / 1000);
    add(s, 0, ICON_COMPUTER, CAT_COMPUTER, d, "%s", "This computer");
    add(s, 1, ICON_SYSINFO, CAT_COMPUTER, d, "Processor: %s", cpu_brand());
    add(s, 1, ICON_MEMORY, CAT_COMPUTER, d, "Memory: %lu MB", ms.total_ram >> 20);
    add(s, 1, ICON_LOGO, CAT_COMPUTER, d, "Firmware: %s", g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "PC BIOS");

    /* display */
    add(s, 0, ICON_DISPLAY, CAT_DISPLAY, NULL, "%s", cat_names[CAT_DISPLAY]);
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 3) continue;
        pci_detail(p, d, 2048);
        strlcat(d, "\nMode: linear framebuffer (VESA/GOP)\n", 2048);
        add(s, 1, ICON_DISPLAY, CAT_DISPLAY, d, "%s %s (%ux%u)", pci_vendor_name(p->vendor), "display adapter", g_bootinfo.fb_width, g_bootinfo.fb_height);
    }

    /* disks */
    add(s, 0, ICON_HDD, CAT_DISKS, NULL, "%s", cat_names[CAT_DISKS]);
    for (blockdev_t *b = blockdev_first(); b; b = b->next) {
        char sz[32]; format_size(b->sector_count * b->sector_size, sz, sizeof sz);
        if (b->type == BDEV_PART) {
            snprintf(d, 2048, "Partition %d of %s\n\nType: %02x\nStart: LBA %lu\nSectors: %lu\nSize: %s", b->part_index,
                     b->parent ? b->parent->name : "?", b->part_type, b->part_start, b->sector_count, sz);
            add(s, 2, ICON_FILE_SYS, CAT_DISKS, d, "%s: partition %d (%s)", b->name, b->part_index, sz);
        } else {
            snprintf(d, 2048, "%s\n\nDevice: %s\nInterface: %s\nSector size: %u bytes\nSectors: %lu\nCapacity: %s\nRemovable: %s\nWrite protected: %s",
                     b->model, b->name, b->type == BDEV_DISK_USB ? "USB mass storage" : b->type == BDEV_DISK_SATA ? "SATA (AHCI)" : "RAM",
                     b->sector_size, b->sector_count, sz, b->removable ? "yes" : "no", b->readonly ? "yes" : "no");
            add(s, 1, b->type == BDEV_DISK_USB ? ICON_USBDRIVE : ICON_HDD, CAT_DISKS, d, "%s (%s, %s)", b->model[0] ? b->model : b->name, b->name, sz);
        }
    }

    /* volumes */
    add(s, 0, ICON_DOCUMENTS, CAT_VOLUMES, NULL, "%s", cat_names[CAT_VOLUMES]);
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        u64 tot = 0, fr = 0; vfs_statfs(v->letter, &tot, &fr);
        char a[32], b2[32]; format_size(tot, a, sizeof a); format_size(fr, b2, sizeof b2);
        snprintf(d, 2048, "Drive %c:\n\nLabel: %s\nFile system: %s\nCapacity: %s\nFree space: %s\nDevice: %s (%s)\nBoot volume: %s",
                 v->letter, v->label[0] ? v->label : "(none)", v->fs_name, a, b2, v->bd ? v->bd->name : "?", v->bd ? v->bd->model : "", v->is_boot ? "yes" : "no");
        add(s, 1, drive_icon(v), CAT_VOLUMES, d, "%c:  %s  [%s, %s]", v->letter, v->label[0] ? v->label : drive_kind(v), v->fs_name, a);
    }

    /* storage controllers */
    add(s, 0, ICON_SYSINFO, CAT_STORAGE, NULL, "%s", cat_names[CAT_STORAGE]);
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 1) continue;
        pci_detail(p, d, 2048);
        add(s, 1, ICON_SYSINFO, CAT_STORAGE, d, "%s %s", pci_vendor_name(p->vendor), pci_class_name(p->class_code, p->subclass, p->prog_if));
    }

    /* USB */
    add(s, 0, ICON_USB, CAT_USB, NULL, "%s", cat_names[CAT_USB]);
    for (usb_hc_t *hc = usb_first_hc(); hc; hc = hc->next) {
        if (hc->pci) pci_detail(hc->pci, d, 2048); else strlcpy(d, hc->desc, 2048);
        char extra[64]; snprintf(extra, sizeof extra, "\nRoot hub ports: %u\n", hc->nports);
        strlcat(d, extra, 2048);
        add(s, 1, ICON_HUB, CAT_USB, d, "%s (%s)", hc->name, hc->desc);
        for (usb_device_t *u = usb_first_device(); u; u = u->next) {
            if (u->hc != hc) continue;
            snprintf(d, 2048, "%s\n%s\n\nManufacturer: %s\nVendor ID: %04x  Product ID: %04x\nSerial number: %s\nUSB version: %x.%02x\n"
                     "Speed: %s\nAddress: %u\nPort: %u (root port %u, hub depth %u)\nClass: %s\nDriver: %s",
                     u->product, u->description, u->manufacturer[0] ? u->manufacturer : "(unknown)", u->desc.idVendor, u->desc.idProduct,
                     u->serial[0] ? u->serial : "(none)", u->desc.bcdUSB >> 8, u->desc.bcdUSB & 0xFF, usb_speed_name(u->speed), u->address,
                     u->port, u->root_port, u->hub_depth, usb_class_name(u->desc.bDeviceClass),
                     u->nbind ? u->bind[0].driver->name : "(none)");
            int ic = ICON_USB;
            for (int b = 0; b < u->nbind; b++) {
                const char *dn = u->bind[b].driver->name;
                if (!strcmp(dn, "hub")) ic = ICON_HUB;
                else if (!strcmp(dn, "mass-storage")) ic = ICON_USBDRIVE;
                else if (!strcmp(dn, "hid")) ic = strstr(u->description, "keyboard") ? ICON_KEYBOARD : ICON_MOUSE;
            }
            add(s, 2 + u->hub_depth, ic, CAT_USB, d, "%s%s%s", u->product, u->description[0] ? " - " : "", u->description);
        }
    }

    /* input */
    add(s, 0, ICON_KEYBOARD, CAT_INPUT, NULL, "%s", cat_names[CAT_INPUT]);
    if (ps2_have_keyboard()) add(s, 1, ICON_KEYBOARD, CAT_INPUT, "PS/2 keyboard\n\nController: Intel 8042 compatible\nScan code set 1 (translated)", "PS/2 keyboard");
    if (ps2_have_mouse()) add(s, 1, ICON_MOUSE, CAT_INPUT, "PS/2 mouse\n\nController: Intel 8042 compatible", "PS/2 mouse");
    for (usb_device_t *u = usb_first_device(); u; u = u->next)
        for (int b = 0; b < u->nbind; b++)
            if (!strcmp(u->bind[b].driver->name, "hid"))
                add(s, 1, strstr(u->description, "keyboard") ? ICON_KEYBOARD : ICON_MOUSE, CAT_INPUT, NULL, "%s (%s)", u->description, u->product);

    /* network */
    add(s, 0, ICON_HUB, CAT_NET, NULL, "%s", cat_names[CAT_NET]);
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 2) continue;
        pci_detail(p, d, 2048);
        strlcat(d, "\nNo driver is available for this device.", 2048);
        add(s, 1, ICON_WARNING, CAT_NET, d, "%s %s", pci_vendor_name(p->vendor), pci_class_name(p->class_code, p->subclass, p->prog_if));
    }

    /* all PCI */
    add(s, 0, ICON_SYSINFO, CAT_PCI, NULL, "%s", cat_names[CAT_PCI]);
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        pci_detail(p, d, 2048);
        add(s, 1, ICON_SYSINFO, CAT_PCI, d, "%02x:%02x.%x  %s %s", p->bus, p->dev, p->func, pci_vendor_name(p->vendor),
            pci_class_name(p->class_code, p->subclass, p->prog_if));
    }
    kfree(d);
    if (s->sel >= s->n) s->sel = s->n - 1;
    wm_invalidate(s->win);
}

static rect_t list_rect(window_t *w) { return mkrect(8, 8, w->client->w * 3 / 5 - 12, w->client->h - 50); }
static rect_t detail_rect(window_t *w) { rect_t l = list_rect(w); return mkrect(l.x + l.w + 8, 8, w->client->w - l.w - 24, l.h); }
static int vis(window_t *w) { return (list_rect(w).h - 4) / ROW_H; }

static void si_paint(window_t *w, gc_t *g) {
    si_t *s = w->data;
    rect_t lr = list_rect(w);
    gfx_sunken_field(g, lr, HEX(0xFFFFFF));
    gc_t sub = gc_sub(g, mkrect(lr.x + 1, lr.y + 1, lr.w - 2 - SB_W, lr.h - 2));
    int v = vis(w);
    for (int i = 0; i <= v && s->top + i < s->n; i++) {
        row_t *r = &s->rows[s->top + i];
        int y = 2 + i * ROW_H, x = 6 + r->depth * 18;
        if (r->depth == 0) {
            /* expander box */
            rect_t b = mkrect(x, y + 5, 9, 9);
            gfx_fill(&sub, b, HEX(0xFFFFFF));
            gfx_rect(&sub, b, HEX(0x919B9C));
            gfx_hline(&sub, b.x + 2, b.y + 4, 5, HEX(0x000000));
            if (s->collapsed[r->cat]) gfx_vline(&sub, b.x + 4, b.y + 2, 5, HEX(0x000000));
        }
        x += r->depth == 0 ? 14 : 0;
        gfx_icon(&sub, x, y + 2, &icons16[r->icon]);
        int sel = s->top + i == s->sel;
        int tw = gfx_text_width(r->depth == 0 ? &font_ui_bold : &font_ui, r->text);
        if (sel) gfx_fill(&sub, mkrect(x + 20, y + 1, tw + 6, ROW_H - 2), g_theme->select);
        gfx_text(&sub, r->depth == 0 ? &font_ui_bold : &font_ui, x + 23, y + 4, r->text, sel ? g_theme->select_text : HEX(0x000000));
    }
    draw_scrollbar(g, mkrect(lr.x + lr.w - SB_W - 1, lr.y + 1, SB_W, lr.h - 2), s->n, v, s->top, 1);
    /* details */
    rect_t dr = detail_rect(w);
    gfx_rounded(g, dr, 4, 15, HEX(0xFFFFFF), color_lighten(g_theme->sidebar_top, 160));
    gfx_rounded_outline(g, dr, 4, 15, color_lighten(g_theme->sidebar_bottom, 30));
    gfx_text(g, &font_ui_bold, dr.x + 10, dr.y + 8, "Properties", color_lighten(g_theme->menu_header_bottom, -40));
    gfx_hline(g, dr.x + 8, dr.y + 26, dr.w - 16, color_lighten(g_theme->sidebar_bottom, 60));
    if (s->sel >= 0 && s->sel < s->n) {
        gc_t ds = gc_sub(g, mkrect(dr.x + 10, dr.y + 34, dr.w - 20, dr.h - 40));
        gfx_text_wrap(&ds, &font_ui, mkrect(0, 0, dr.w - 20, dr.h - 40), s->rows[s->sel].detail, HEX(0x000000));
    } else {
        gfx_text(g, &font_ui, dr.x + 10, dr.y + 34, "Select a device to see its properties.", HEX(0x6D6D6D));
    }
}

static void si_event(window_t *w, gui_event_t *e) {
    si_t *s = w->data;
    rect_t lr = list_rect(w);
    int v = vis(w);
    rect_t sbr = mkrect(lr.x + lr.w - SB_W - 1, lr.y + 1, SB_W, lr.h - 2);
    if (e->type == GE_WHEEL) { s->top = CLAMP(s->top - e->dz * 3, 0, MAX(0, s->n - v)); wm_invalidate(w); return; }
    if (e->type == GE_MOUSE_UP) { s->sb_drag = 0; return; }
    if (e->type == GE_MOUSE_MOVE && s->sb_drag && (e->buttons & MOUSE_LEFT)) {
        s->top = CLAMP((e->y - sbr.y - SB_W) * MAX(0, s->n - v) / MAX(1, sbr.h - 2 * SB_W), 0, MAX(0, s->n - v));
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        if (rect_contains(sbr, e->x, e->y)) {
            int part;
            s->top = scrollbar_hit(sbr, s->n, v, s->top, e->x, e->y, 1, &part);
            s->sb_drag = part == 5;
            wm_invalidate(w);
            return;
        }
        if (!rect_contains(lr, e->x, e->y)) return;
        int i = s->top + (e->y - lr.y - 3) / ROW_H;
        if (i < 0 || i >= s->n) return;
        s->sel = i;
        row_t *r = &s->rows[i];
        if (r->depth == 0 && (e->type == GE_DBLCLICK || e->x < lr.x + 22)) {
            s->collapsed[r->cat] = !s->collapsed[r->cat];
            build(s);
        }
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_KEY && e->key.pressed) {
        int k = e->key.key;
        if (k == KEY_UP) s->sel = MAX(0, s->sel - 1);
        else if (k == KEY_DOWN) s->sel = MIN(s->n - 1, s->sel + 1);
        else if ((k == KEY_LEFT || k == KEY_RIGHT || k == KEY_ENTER) && s->sel >= 0 && s->rows[s->sel].depth == 0) {
            s->collapsed[s->rows[s->sel].cat] = k == KEY_LEFT ? 1 : k == KEY_RIGHT ? 0 : !s->collapsed[s->rows[s->sel].cat];
            build(s);
        } else if (k == KEY_F5) build(s);
        else return;
        if (s->sel < s->top) s->top = s->sel;
        if (s->sel >= s->top + v) s->top = s->sel - v + 1;
        wm_invalidate(w);
    }
}

static void si_tick(window_t *w) {
    si_t *s = w->data;
    if (usb_generation() != s->ugen || vfs_generation() != s->vgen) build(s);
}

static void si_refresh(window_t *w, widget_t *b) { build(w->data); }
static void si_log(window_t *w, widget_t *b) {
    char *buf = kmalloc(128 * 1024 + 1);
    size_t n = klog_read(buf, 128 * 1024);
    buf[n] = 0;
    app_notepad_text("Kernel Log", buf);
    kfree(buf);
}
static void si_close(window_t *w, widget_t *b) { wm_close(w); }

static void si_resize(window_t *w) {
    int y = w->client->h - 34;
    widget_t *a = widget_find(w, 1), *b = widget_find(w, 2), *c = widget_find(w, 3);
    if (a) { a->r.y = y; }
    if (b) { b->r.y = y; }
    if (c) { c->r.y = y; c->r.x = w->client->w - 88; }
}

static void si_destroy(window_t *w) {
    si_t *s = w->data;
    for (int i = 0; i < s->n; i++) kfree(s->rows[i].detail);
    kfree(s->rows);
    kfree(s);
}

void app_sysinfo(void) {
    window_t *w = wm_create("System Information", ICON_SYSINFO, -1, -1, 760, 500, WF_RESIZABLE);
    w->min_w = 480; w->min_h = 300;
    si_t *s = kzalloc(sizeof *s);
    s->rows = kzalloc(sizeof(row_t) * MAX_ROWS);
    s->win = w;
    s->sel = 0;
    s->collapsed[CAT_PCI] = 1;
    s->collapsed[CAT_STORAGE] = 0;
    w->data = s;
    w->on_paint = si_paint;
    w->on_event = si_event;
    w->on_tick = si_tick;
    w->on_resize = si_resize;
    w->on_destroy = si_destroy;
    int y = w->client->h - 34;
    widget_t *a = widget_add(w, W_BUTTON, mkrect(8, y, 90, 24), "Refresh", 1);
    a->on_click = si_refresh;
    widget_t *b = widget_add(w, W_BUTTON, mkrect(104, y, 110, 24), "Kernel Log...", 2);
    b->on_click = si_log;
    widget_t *c = widget_add(w, W_BUTTON, mkrect(w->client->w - 88, y, 80, 24), "Close", 3);
    c->on_click = si_close; c->style |= WS_CANCEL;
    build(s);
}
