/* Halcyon OS - minimal ACPI: find the FADT and \_S5 to power the machine off */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"

struct PACKED sdt { char sig[4]; u32 len; u8 rev, csum; char oem[6], oemtab[8]; u32 oemrev, creator, crev; };
struct PACKED rsdp { char sig[8]; u8 csum; char oem[6]; u8 rev; u32 rsdt; u32 len; u64 xsdt; u8 xcsum; u8 res[3]; };

static int acpi_ok;
static u32 pm1a_cnt, pm1b_cnt, smi_cmd;
static u8 acpi_enable_val;
static u16 slp_typa, slp_typb;

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
        if (!t || memcmp(t->sig, "FACP", 4)) continue;
        u8 *f = (u8 *)t;
        smi_cmd = *(u32 *)(f + 48);
        acpi_enable_val = f[52];
        pm1a_cnt = *(u32 *)(f + 64);
        pm1b_cnt = *(u32 *)(f + 68);
        u64 dsdt = *(u32 *)(f + 40);
        if (t->len >= 148 && *(u64 *)(f + 140)) dsdt = *(u64 *)(f + 140);
        struct sdt *d = table(dsdt);
        if (d) parse_s5(d);
    }
    kprintf("acpi: %s (PM1a_CNT %#x, SLP_TYP %u/%u)\n", acpi_ok ? "S5 available" : "no S5 info", pm1a_cnt, slp_typa, slp_typb);
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
