/* Halcyon OS - minimal ACPI: find the FADT and \_S5 to power the machine off */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "acpi.h"

struct PACKED sdt { char sig[4]; u32 len; u8 rev, csum; char oem[6], oemtab[8]; u32 oemrev, creator, crev; };
struct PACKED rsdp { char sig[8]; u8 csum; char oem[6]; u8 rev; u32 rsdt; u32 len; u64 xsdt; u8 xcsum; u8 res[3]; };

static int acpi_ok;
static u32 pm1a_cnt, pm1b_cnt, smi_cmd;
static u8 acpi_enable_val;
static u16 slp_typa, slp_typb;
static u16 pm_tmr_port;
static int pm_tmr_32;

/* ACPI power-management timer (3.579545 MHz), used to calibrate the TSC */
int acpi_pm_timer(u16 *port, int *bits32) {
    if (!pm_tmr_port) return 0;
    *port = pm_tmr_port; *bits32 = pm_tmr_32;
    return 1;
}

static acpi_madt_t madt;
const acpi_madt_t *acpi_madt(void) { return madt.present ? &madt : NULL; }

static int checksum(const void *p, u32 n) {
    u8 s = 0;
    for (u32 i = 0; i < n; i++) s += ((const u8 *)p)[i];
    return s == 0;
}

static struct sdt *table(u64 addr) {
    if (!addr || addr >= 0x100000000ull) return NULL;
    struct sdt *t = (struct sdt *)(uptr)addr;
    if (t->len < sizeof *t || t->len > 0x1000000 || !checksum(t, t->len)) return NULL;
    return t;
}

static void parse_s5(struct sdt *dsdt) {
    u8 *p = (u8 *)dsdt + sizeof *dsdt, *end = (u8 *)dsdt + dsdt->len;
    for (; p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4)) continue;
        /* NameOp (0x08) [ '\' ] "_S5_" PackageOp (0x12) */
        if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
        u8 *q = p + 4;
        if (*q != 0x12) continue;
        q++;
        q += ((*q & 0xC0) >> 6) + 1;   /* PkgLength */
        q++;                           /* NumElements */
        u16 v[2];
        for (int i = 0; i < 2; i++) {
            if (*q == 0x0A) { q++; v[i] = *q++; }        /* BytePrefix */
            else if (*q == 0x00 || *q == 0x01) v[i] = *q++;  /* Zero / One */
            else if (*q == 0xFF) { v[i] = 0xFF; q++; }
            else v[i] = *q++;
        }
        slp_typa = v[0]; slp_typb = v[1];
        acpi_ok = 1;
        return;
    }
}

static void parse_madt(struct sdt *t) {
    u8 *p = (u8 *)t + sizeof *t;
    if (t->len < sizeof *t + 8) return;
    madt.lapic_addr = *(u32 *)p;
    madt.flags = *(u32 *)(p + 4);
    madt.nmi_lint = -1;
    u8 *q = p + 8, *end = (u8 *)t + t->len;
    while (q + 2 <= end && q[1] >= 2 && q + q[1] <= end) {
        u8 type = q[0], len = q[1];
        switch (type) {
        case 0:   /* processor local APIC */
            if (len >= 8 && (*(u32 *)(q + 4) & 1)) madt.ncpus++;
            break;
        case 1:   /* I/O APIC */
            if (len >= 12 && madt.nioapic < MADT_MAX_IOAPIC) {
                madt_ioapic_t *io = &madt.ioapic[madt.nioapic++];
                io->id = q[2];
                io->addr = *(u32 *)(q + 4);
                io->gsi_base = *(u32 *)(q + 8);
            }
            break;
        case 2:   /* interrupt source override */
            if (len >= 10 && madt.niso < MADT_MAX_ISO) {
                madt_iso_t *o = &madt.iso[madt.niso++];
                o->bus = q[2]; o->irq = q[3];
                o->gsi = *(u32 *)(q + 4);
                o->flags = *(u16 *)(q + 8);
            }
            break;
        case 4:   /* local APIC NMI: processor 0xFF = all */
            if (len >= 6 && (q[2] == 0xFF || q[2] == 0 || madt.nmi_lint < 0)) {
                madt.nmi_flags = *(u16 *)(q + 3);
                madt.nmi_lint = q[5] & 1;
            }
            break;
        case 5:   /* 64-bit local APIC address override */
            if (len >= 12) {
                u64 a = *(u64 *)(q + 4);
                if (a && a < 0x100000000ull) madt.lapic_addr = (u32)a;
            }
            break;
        case 9:   /* processor local x2APIC */
            if (len >= 16 && (*(u32 *)(q + 8) & 1)) madt.ncpus++;
            break;
        }
        q += len;
    }
    madt.present = 1;
    kprintf("acpi: MADT: LAPIC %#x, %d CPU(s), %d I/O APIC(s), %d override(s)%s\n", madt.lapic_addr, madt.ncpus,
            madt.nioapic, madt.niso, (madt.flags & 1) ? ", 8259 PICs present" : "");
    for (int i = 0; i < madt.nioapic; i++)
        kprintf("acpi:   I/O APIC id %u at %#x, GSI base %u\n", madt.ioapic[i].id, madt.ioapic[i].addr, madt.ioapic[i].gsi_base);
    for (int i = 0; i < madt.niso; i++)
        kprintf("acpi:   ISA IRQ %u -> GSI %u (flags %#x)\n", madt.iso[i].irq, madt.iso[i].gsi, madt.iso[i].flags);
}

void acpi_init(void) {
    struct rsdp *r = (struct rsdp *)(uptr)g_bootinfo.rsdp;
    if (!r || memcmp(r->sig, "RSD PTR ", 8)) { kprintf("acpi: no RSDP\n"); return; }
    struct sdt *root = r->rev >= 2 && r->xsdt ? table(r->xsdt) : NULL;
    int wide = root != NULL;
    if (!root) root = table(r->rsdt);
    if (!root) { kprintf("acpi: bad RSDT\n"); return; }
    int n = (root->len - sizeof *root) / (wide ? 8 : 4);
    u8 *ents = (u8 *)root + sizeof *root;
    for (int i = 0; i < n; i++) {
        u64 a = wide ? *(u64 *)(ents + i * 8) : *(u32 *)(ents + i * 4);
        struct sdt *t = table(a);
        if (t && !memcmp(t->sig, "APIC", 4) && !madt.present) { parse_madt(t); continue; }
        if (!t || memcmp(t->sig, "FACP", 4)) continue;
        u8 *f = (u8 *)t;
        smi_cmd = *(u32 *)(f + 48);
        acpi_enable_val = f[52];
        pm1a_cnt = *(u32 *)(f + 64);
        pm1b_cnt = *(u32 *)(f + 68);
        u32 tmr = t->len >= 80 ? *(u32 *)(f + 76) : 0;
        if (!tmr && t->len >= 220 && f[208] == 1) tmr = (u32)*(u64 *)(f + 212);   /* X_PM_TMR_BLK in I/O space */
        if (tmr && tmr < 0x10000) pm_tmr_port = (u16)tmr;
        if (t->len >= 116) pm_tmr_32 = (*(u32 *)(f + 112) >> 8) & 1;
        u64 dsdt = *(u32 *)(f + 40);
        if (t->len >= 148 && *(u64 *)(f + 140)) dsdt = *(u64 *)(f + 140);
        struct sdt *d = table(dsdt);
        if (d) parse_s5(d);
    }
    kprintf("acpi: %s (PM1a_CNT %#x, SLP_TYP %u/%u), PM timer %#x (%d-bit)\n", acpi_ok ? "S5 available" : "no S5 info",
            pm1a_cnt, slp_typa, slp_typb, pm_tmr_port, pm_tmr_32 ? 32 : 24);
}

int acpi_poweroff(void) {
    if (!acpi_ok || !pm1a_cnt) return -1;
    if (smi_cmd && acpi_enable_val && !(inw(pm1a_cnt) & 1)) {
        outb((u16)smi_cmd, acpi_enable_val);
        for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++) mdelay(10);
    }
    outw((u16)pm1a_cnt, (u16)((slp_typa << 10) | (1 << 13)));
    if (pm1b_cnt) outw((u16)pm1b_cnt, (u16)((slp_typb << 10) | (1 << 13)));
    mdelay(500);
    return -1;
}
