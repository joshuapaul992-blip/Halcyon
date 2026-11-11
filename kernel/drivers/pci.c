/* Halcyon OS - PCI configuration space (mechanism #1) and enumeration */
#include "pci.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"

#define MAX_PCI 256
static pci_dev_t devs[MAX_PCI];
static int ndevs;

static u32 cfg_addr(u8 bus, u8 dev, u8 func, u8 off) {
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (off & 0xFC);
}
static u32 raw_read32(u8 b, u8 d, u8 f, u8 off) { outl(0xCF8, cfg_addr(b, d, f, off)); return inl(0xCFC); }
static void raw_write32(u8 b, u8 d, u8 f, u8 off, u32 v) { outl(0xCF8, cfg_addr(b, d, f, off)); outl(0xCFC, v); }

u32 pci_read32(pci_dev_t *d, u8 off) { return raw_read32(d->bus, d->dev, d->func, off); }
u16 pci_read16(pci_dev_t *d, u8 off) { return (u16)(pci_read32(d, off) >> ((off & 2) * 8)); }
u8  pci_read8(pci_dev_t *d, u8 off)  { return (u8)(pci_read32(d, off) >> ((off & 3) * 8)); }
void pci_write32(pci_dev_t *d, u8 off, u32 v) { raw_write32(d->bus, d->dev, d->func, off, v); }
void pci_write16(pci_dev_t *d, u8 off, u16 v) {
    outl(0xCF8, cfg_addr(d->bus, d->dev, d->func, off));
    outw(0xCFC + (off & 2), v);
}
void pci_write8(pci_dev_t *d, u8 off, u8 v) {
    outl(0xCF8, cfg_addr(d->bus, d->dev, d->func, off));
    outb(0xCFC + (off & 3), v);
}

static void probe_bars(pci_dev_t *d) {
    int nbars = (d->header_type & 0x7F) == 0 ? 6 : ((d->header_type & 0x7F) == 1 ? 2 : 0);
    u16 cmd = pci_read16(d, 4);
    pci_write16(d, 4, cmd & ~3);    /* disable decode while sizing */
    for (int i = 0; i < nbars; i++) {
        u8 off = 0x10 + i * 4;
        u32 orig = pci_read32(d, off);
        pci_write32(d, off, 0xFFFFFFFF);
        u32 sz = pci_read32(d, off);
        pci_write32(d, off, orig);
        if (orig & 1) {
            d->bar[i].is_io = 1;
            d->bar[i].addr = orig & ~3u;
            sz &= ~3u;
            d->bar[i].size = sz ? ((~sz + 1) & 0xFFFF) : 0;
        } else {
            int type = (orig >> 1) & 3;
            d->bar[i].prefetch = (orig >> 3) & 1;
            u64 addr = orig & ~0xFu;
            u64 mask = sz & ~0xFu;
            if (type == 2 && i + 1 < nbars) {
                u32 orig_hi = pci_read32(d, off + 4);
                pci_write32(d, off + 4, 0xFFFFFFFF);
                u32 sz_hi = pci_read32(d, off + 4);
                pci_write32(d, off + 4, orig_hi);
                addr |= (u64)orig_hi << 32;
                mask |= (u64)sz_hi << 32;
                d->bar[i].is_64 = 1;
                d->bar[i].addr = addr;
                d->bar[i].size = mask ? (~mask + 1) : 0;
                i++;
                continue;
            }
            mask |= 0xFFFFFFFF00000000ull;
            d->bar[i].addr = addr;
            d->bar[i].size = (sz & ~0xFu) ? (~mask + 1) : 0;
        }
    }
    pci_write16(d, 4, cmd);
}

static void add_func(u8 bus, u8 dev, u8 func) {
    u32 id = raw_read32(bus, dev, func, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs >= MAX_PCI) return;
    pci_dev_t *d = &devs[ndevs++];
    memset(d, 0, sizeof *d);
    d->bus = bus; d->dev = dev; d->func = func;
    d->vendor = id & 0xFFFF; d->device = id >> 16;
    u32 cr = raw_read32(bus, dev, func, 8);
    d->revision = cr & 0xFF; d->prog_if = (cr >> 8) & 0xFF;
    d->subclass = (cr >> 16) & 0xFF; d->class_code = cr >> 24;
    d->header_type = (raw_read32(bus, dev, func, 0x0C) >> 16) & 0xFF;
    d->irq_line = raw_read32(bus, dev, func, 0x3C) & 0xFF;
    if ((d->header_type & 0x7F) == 0) {
        u32 ss = raw_read32(bus, dev, func, 0x2C);
        d->subsys_vendor = ss & 0xFFFF; d->subsys_id = ss >> 16;
    }
    probe_bars(d);
}

void pci_init(void) {
    ndevs = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            u32 id = raw_read32(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            u8 ht = (raw_read32(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            int nf = (ht & 0x80) ? 8 : 1;
            for (int f = 0; f < nf; f++) add_func(bus, dev, f);
        }
    }
    kprintf("pci: %d functions\n", ndevs);
    for (int i = 0; i < ndevs; i++) {
        pci_dev_t *d = &devs[i];
        kprintf("  %02x:%02x.%x %04x:%04x class %02x.%02x.%02x %s\n", d->bus, d->dev, d->func,
                d->vendor, d->device, d->class_code, d->subclass, d->prog_if,
                pci_class_name(d->class_code, d->subclass, d->prog_if));
    }
}

int pci_count(void) { return ndevs; }
pci_dev_t *pci_get(int i) { return (i >= 0 && i < ndevs) ? &devs[i] : NULL; }

pci_dev_t *pci_find_class(int cls, int sub, int progif, int index) {
    for (int i = 0; i < ndevs; i++) {
        pci_dev_t *d = &devs[i];
        if (d->class_code == cls && d->subclass == sub && (progif < 0 || d->prog_if == progif)) {
            if (index-- == 0) return d;
        }
    }
    return NULL;
}

void pci_enable(pci_dev_t *d) {
    u16 cmd = pci_read16(d, 4);
    cmd |= 0x0007;          /* I/O, memory, bus master */
    cmd |= 0x0400;          /* INTx off until irq_attach() wants it */
    pci_write16(d, 4, cmd);
}

/* capability list walk: offset of capability `id`, 0 if absent */
int pci_find_cap(pci_dev_t *d, u8 id) {
    if (!(pci_read16(d, 0x06) & (1u << 4))) return 0;
    u8 off = pci_read8(d, 0x34) & 0xFC;
    for (int n = 0; off >= 0x40 && n < 48; n++) {
        if (pci_read8(d, off) == id) return off;
        off = pci_read8(d, off + 1) & 0xFC;
    }
    return 0;
}

/* MSI with a single message (multiple-message enable = 0) */
int pci_enable_msi(pci_dev_t *d, u64 addr, u32 data) {
    int c = pci_find_cap(d, 0x05);
    if (!c) return -1;
    u16 ctl = pci_read16(d, (u8)(c + 2));
    int is64 = (ctl >> 7) & 1, pvm = (ctl >> 8) & 1;
    pci_write16(d, (u8)(c + 2), ctl & ~0x0071);         /* disabled, MME = 0 */
    pci_write32(d, (u8)(c + 4), (u32)addr);
    if (is64) {
        pci_write32(d, (u8)(c + 8), (u32)(addr >> 32));
        pci_write16(d, (u8)(c + 12), (u16)data);
        if (pvm) pci_write32(d, (u8)(c + 16), 0);       /* unmask vector 0 */
    } else {
        pci_write16(d, (u8)(c + 8), (u16)data);
        if (pvm) pci_write32(d, (u8)(c + 12), 0);
    }
    pci_write16(d, 4, pci_read16(d, 4) | 0x0400);       /* INTx off */
    pci_write16(d, (u8)(c + 2), (ctl & ~0x0070) | 1);
    return 0;
}

/* MSI-X: table entry 0 only, every other entry masked */
int pci_enable_msix(pci_dev_t *d, u64 addr, u32 data) {
    int c = pci_find_cap(d, 0x11);
    if (!c) return -1;
    u16 ctl = pci_read16(d, (u8)(c + 2));
    u32 tbl = pci_read32(d, (u8)(c + 4));
    int bir = tbl & 7, n = (ctl & 0x7FF) + 1;
    if (bir > 5 || d->bar[bir].is_io || !d->bar[bir].addr) return -1;
    volatile u32 *t = (volatile u32 *)((u8 *)pci_map_bar(d, bir) + (tbl & ~7u));
    if (!pci_map_bar(d, bir)) return -1;
    /* enable with the function masked while the table is written */
    pci_write16(d, (u8)(c + 2), ctl | 0xC000);
    for (int i = 0; i < n && i < 2048; i++) t[i * 4 + 3] |= 1;
    t[0] = (u32)addr;
    t[1] = (u32)(addr >> 32);
    t[2] = data;
    t[3] &= ~1u;
    pci_write16(d, 4, pci_read16(d, 4) | 0x0400);
    pci_write16(d, (u8)(c + 2), (ctl | 0x8000) & ~0x4000);
    return 0;
}

void pci_disable_msi(pci_dev_t *d) {
    int c = pci_find_cap(d, 0x11);
    if (c) pci_write16(d, (u8)(c + 2), pci_read16(d, (u8)(c + 2)) & ~0x8000);
    c = pci_find_cap(d, 0x05);
    if (c) pci_write16(d, (u8)(c + 2), pci_read16(d, (u8)(c + 2)) & ~1);
}

void *pci_map_bar(pci_dev_t *d, int bar) {
    pci_bar_t *b = &d->bar[bar];
    if (b->is_io || !b->addr) return NULL;
    return mm_map_mmio(b->addr, b->size ? b->size : 0x1000);
}

const char *pci_class_name(u8 c, u8 s, u8 p) {
    switch (c) {
    case 0x00: return "Unclassified device";
    case 0x01:
        switch (s) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return p == 1 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return "NVM Express controller";
        }
        return "Mass storage controller";
    case 0x02: return s == 0 ? "Ethernet controller" : (s == 0x80 ? "Network controller" : "Network controller");
    case 0x03: return s == 0 ? "VGA compatible display" : "Display controller";
    case 0x04: return s == 3 ? "High Definition Audio" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x80: return "System bridge";
        }
        return "Bridge device";
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (s) {
        case 0x03:
            switch (p) {
            case 0x00: return "USB 1.1 UHCI controller";
            case 0x10: return "USB 1.1 OHCI controller";
            case 0x20: return "USB 2.0 EHCI controller";
            case 0x30: return "USB 3.x xHCI controller";
            }
            return "USB controller";
        case 0x05: return "SMBus controller";
        case 0x00: return "FireWire controller";
        }
        return "Serial bus controller";
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    }
    return "Unknown device";
}

const char *pci_vendor_name(u16 v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU/Bochs";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1106: return "VIA";
    case 0x10EC: return "Realtek";
    case 0x1033: return "NEC";
    case 0x1912: return "Renesas";
    case 0x1B21: return "ASMedia";
    case 0x1B73: return "Fresco Logic";
    case 0x197B: return "JMicron";
    case 0x1039: return "SiS";
    case 0x10B9: return "ALi";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x1969: return "Qualcomm Atheros";
    case 0x1283: return "ITE";
    case 0x104C: return "Texas Instruments";
    case 0x1000: return "LSI/Broadcom";
    case 0x144D: return "Samsung";
    case 0x1C5C: return "SK hynix";
    case 0x15B7: return "Sandisk/WD";
    case 0x1E0F: return "KIOXIA";
    case 0x1D6A: return "Aquantia";
    case 0x11AB: return "Marvell";
    case 0x1B4B: return "Marvell";
    }
    return "Unknown vendor";
}
