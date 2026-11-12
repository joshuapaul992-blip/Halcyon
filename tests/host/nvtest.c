/* Host test: the NVIDIA identification code (kernel/drivers/nv/chip.c,
 * vbios.c, i2c.c, kernel/drivers/edid.c), built with ASan/UBSan.
 *
 *   - chip identification for every family and bad PMC_BOOT_0 values;
 *     VRAM sizing against simulated memory controller registers
 *   - a generator of synthetic video BIOS images in every layout the parser
 *     reads (BMP < 5 with fixed I2C pairs, BMP 5 with DCB 1.5 and 2.2, BIT
 *     with DCB 2.2, 3.0, 4.0 and 4.1, Pascal-style chains of PC-AT + EFI +
 *     NVIDIA data images with NPDE, tables reached through pointers into
 *     the data image, IFR-prefixed PROM dumps of versions 2 and 3, a ROM at
 *     a 512-byte boundary), each checked field by field; then malformed
 *     ones: every truncation, bad checksums, signatures, versions and
 *     pointers, absurd counts, and 20,000 random mutations per layout
 *   - EDID: all detailed timings of base, CEA-861 and DisplayID blocks
 *   - the bit-banged I2C master against a simulated bus with an E-DDC
 *     EEPROM (segment pointer 0x30): reads of 1 to 4 blocks, NAKs, clock
 *     stretching (accepted, and too long), a slave stuck mid-byte (freed by
 *     clocking), SDA stuck low for good
 *   - the AUX channel in the G94 and GM200 register layouts against a
 *     simulated DisplayPort sink (DPCD, I2C over AUX with MOT and segment
 *     pointer): DEFER and I2C DEFER, NACKs, no sink, a channel that never
 *     grants or never finishes, receive errors; every case must leave the
 *     channel's request and auto-DPCD bits as they were
 *
 *   nvtest [-v]                  run everything
 *   nvtest --write-rom FILE      write the Pascal-style test image (QEMU test hook)
 *   nvtest --write-edid FILE     write a 2-block test EDID
 *   nvtest --seeds DIR           write fuzzer seeds (every generated layout)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "core/lib.h"
#include "drivers/nv/nvhw.h"
#include "drivers/edid.h"

const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }

static int checks, failed, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("nvtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* deterministic random numbers */
static u64 rs = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)rs; }

/* ======================================================================
 * chip identification
 * ====================================================================== */

static void test_chips(void) {
    static const struct { u32 boot0; u16 cs; const char *code; int fam; int disp; int i2c; } t[] = {
        { 0x20004000, 0x004, "NV04", NV_FAM_FAHRENHEIT, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x20154000, 0x005, "NV05", NV_FAM_FAHRENHEIT, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x010100a1, 0x010, "NV10", NV_FAM_CELSIUS, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x017200a2, 0x017, "NV17", NV_FAM_CELSIUS, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x018100a2, 0x018, "NV18", NV_FAM_CELSIUS, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x020200a5, 0x020, "NV20", NV_FAM_KELVIN, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x025000a2, 0x025, "NV25", NV_FAM_KELVIN, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x034100a1, 0x034, "NV34", NV_FAM_RANKINE, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x049200a2, 0x049, "G71", NV_FAM_CURIE, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x04e000a1, 0x04e, "C51", NV_FAM_CURIE, NV_DISP_NV04, NV_I2C_NV4E },
        { 0x063000a2, 0x063, "C73", NV_FAM_CURIE, NV_DISP_NV04, NV_I2C_NV04 },
        { 0x050000a2, 0x050, "G80", NV_FAM_TESLA, NV_DISP_NV50, NV_I2C_NV50 },
        { 0x092000a2, 0x092, "G92", NV_FAM_TESLA, NV_DISP_NV50, NV_I2C_NV50 },
        { 0x094000a1, 0x094, "G94", NV_FAM_TESLA, NV_DISP_NV50, NV_I2C_G94 },
        { 0x0a8000a2, 0x0a8, "GT218", NV_FAM_TESLA, NV_DISP_NV50, NV_I2C_G94 },
        { 0x0ac000b1, 0x0ac, "MCP79/MCP7A", NV_FAM_TESLA, NV_DISP_NV50, NV_I2C_G94 },
        { 0x0c4000a1, 0x0c4, "GF104", NV_FAM_FERMI, NV_DISP_NV50, NV_I2C_G94 },
        { 0x0d9000a1, 0x0d9, "GF119", NV_FAM_FERMI, NV_DISP_GF119, NV_I2C_GF119 },
        { 0x0e4000a2, 0x0e4, "GK104", NV_FAM_KEPLER, NV_DISP_GF119, NV_I2C_GF119 },
        { 0x106000a1, 0x106, "GK208B", NV_FAM_KEPLER, NV_DISP_GF119, NV_I2C_GF119 },
        { 0x117000a2, 0x117, "GM107", NV_FAM_MAXWELL, NV_DISP_GF119, NV_I2C_GF119 },
        { 0x124000a1, 0x124, "GM204", NV_FAM_MAXWELL, NV_DISP_GF119, NV_I2C_GM200 },
        { 0x134000a1, 0x134, "GP104", NV_FAM_PASCAL, NV_DISP_GF119, NV_I2C_GM200 },
        { 0x138000a1, 0x138, "GP108", NV_FAM_PASCAL, NV_DISP_GF119, NV_I2C_GM200 },
        { 0x13a000a1, 0x13a, "NV13A", NV_FAM_PASCAL, NV_DISP_GF119, NV_I2C_GM200 },    /* not in the table */
        { 0x140000a1, 0x140, "GV100", NV_FAM_VOLTA, NV_DISP_LATER, NV_I2C_NONE },
        { 0x164000a1, 0x164, "TU104", NV_FAM_LATER, NV_DISP_LATER, NV_I2C_NONE },
        { 0x194000a1, 0x194, "AD104", NV_FAM_LATER, NV_DISP_LATER, NV_I2C_NONE },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(t); i++) {
        nv_chip_t c;
        int r = nv_chip_identify(t[i].boot0, 0, &c);
        CHECK(!r && c.chipset == t[i].cs && !strcmp(c.codename, t[i].code) && c.family == t[i].fam && c.disp == t[i].disp &&
              c.i2c == t[i].i2c, "boot0 %08x: r %d chipset %03x '%s' family %d disp %d i2c %d", t[i].boot0, r, c.chipset,
              c.codename, c.family, c.disp, c.i2c);
        int in = t[i].fam >= NV_FAM_CELSIUS && t[i].fam <= NV_FAM_PASCAL;
        CHECK(c.in_range == in, "boot0 %08x in range %d", t[i].boot0, c.in_range);
        if (t[i].fam == NV_FAM_LATER)
            CHECK(c.note && !strcmp(c.note, "Turing or later: not supported until Halcyon 1.1"), "note for %s", c.codename);
        if (verbose) printf("  %08x -> %s %s, %s\n", t[i].boot0, c.codename, c.family_name, c.series);
    }
    nv_chip_t c;
    CHECK(nv_chip_identify(0, 0, &c) == E_NODEV, "boot0 0");
    CHECK(nv_chip_identify(0xFFFFFFFF, 0, &c) == E_NODEV, "boot0 ~0");
    CHECK(nv_chip_identify(0xBADF1100, 0, &c) == E_NODEV, "PRI error");
    CHECK(nv_chip_identify(0x00001234, 0, &c) == E_NODEV, "garbage");
    /* BOOT_42 names a newer chip than BOOT_0 can */
    CHECK(!nv_chip_identify(0x160000a1, 0x1B2000a1, &c) && c.chipset == 0x1B2 && c.from_boot42, "boot42 %03x", c.chipset);
    CHECK(!nv_chip_identify(0x134000a1, 0x1B2000a1, &c) && c.chipset == 0x134 && !c.from_boot42, "boot42 ignored before Turing");
    /* card names */
    CHECK(nv_card_name(0x1B81) && !strcmp(nv_card_name(0x1B81), "GeForce GTX 1070"), "1b81");
    CHECK(nv_card_name(0x0171) && !strcmp(nv_card_name(0x0171), "GeForce4 MX 440"), "0171");
    CHECK(!nv_card_name(0xFFFF), "ffff");
    /* the GTX 1070 */
    CHECK(!nv_chip_identify(0x134000a1, 0, &c) && !strcmp(c.family_name, "Pascal") && c.vram == NV_VRAM_GP102, "gp104");
}

/* simulated registers for the VRAM probe */
typedef struct { u32 reg[32]; u32 val[32]; int n; } regmap_t;
static u32 map_rd(void *ctx, u32 r) {
    regmap_t *m = ctx;
    for (int i = 0; i < m->n; i++) if (m->reg[i] == r) return m->val[i];
    return 0xBADF1100;
}
static void map_set(regmap_t *m, u32 r, u32 v) { m->reg[m->n] = r; m->val[m->n++] = v; }

static u64 vram_of(u32 boot0, regmap_t *m) {
    nv_chip_t c;
    nv_chip_identify(boot0, 0, &c);
    nv_regs_t r = { m, map_rd, NULL, NULL, NULL };
    char how[96];
    return nv_vram_probe(&c, &r, how, sizeof how);
}

static void test_vram(void) {
    regmap_t m;
    memset(&m, 0, sizeof m); map_set(&m, 0x100CE0, (8 << 4) | 10);              /* GP104: 8 << 30 */
    CHECK(vram_of(0x134000a1, &m) == 8ull << 30, "gp104 8 GiB: %llu", (unsigned long long)vram_of(0x134000a1, &m));
    memset(&m, 0, sizeof m); map_set(&m, 0x100CE0, 0x40000000 | (8 << 4) | 10);
    CHECK(vram_of(0x134000a1, &m) == (8ull << 30) / 16 * 15, "gp104 ECC");
    memset(&m, 0, sizeof m); map_set(&m, 0x100CE0, 0xBADF5040);
    CHECK(vram_of(0x134000a1, &m) == 0, "gp104 PRI error");
    memset(&m, 0, sizeof m); map_set(&m, 0x10020C, 0x10000000);                 /* NV40: 256 MiB */
    CHECK(vram_of(0x040000a1, &m) == 256ull << 20, "nv40");
    memset(&m, 0, sizeof m); map_set(&m, 0x10020C, 0x40000001);                 /* G92 style: 4 GiB + 1 GiB */
    CHECK(vram_of(0x092000a2, &m) == (1ull << 32) + (1ull << 30), "nv50 %llx", (unsigned long long)vram_of(0x092000a2, &m));
    memset(&m, 0, sizeof m);                                                    /* GF110: 6 partitions, one off */
    map_set(&m, 0x022438, 6); map_set(&m, 0x022554, 0x20);
    for (int i = 0; i < 6; i++) map_set(&m, 0x11020C + i * 0x1000, 256);
    CHECK(vram_of(0x0c8000a1, &m) == 1280ull << 20, "gf110 %llu", (unsigned long long)vram_of(0x0c8000a1, &m) >> 20);
    memset(&m, 0, sizeof m);                                                    /* GK104: 4 FBPs x 1 FBPA, 512 MiB each */
    map_set(&m, 0x022438, 4); map_set(&m, 0x02243C, 4); map_set(&m, 0x022554, 0);
    for (int i = 0; i < 4; i++) map_set(&m, 0x11020C + i * 0x1000, 512);
    CHECK(vram_of(0x0e4000a2, &m) == 2048ull << 20, "gk104");
    memset(&m, 0, sizeof m);                                                    /* GM204: 4 FBPs x 2 FBPAs x 512 MiB */
    map_set(&m, 0x022438, 4); map_set(&m, 0x022458, 2); map_set(&m, 0x021C14, 0); map_set(&m, 0x021D38, 0);
    for (int i = 0; i < 8; i++) map_set(&m, 0x11020C + i * 0x1000, 512);
    CHECK(vram_of(0x124000a1, &m) == 4096ull << 20, "gm204 %llu", (unsigned long long)vram_of(0x124000a1, &m) >> 20);
    memset(&m, 0, sizeof m);
    map_set(&m, 0x100000, 0x00000002);                                          /* NV04: 8 MiB */
    CHECK(vram_of(0x20004000, &m) == 8ull << 20, "nv04");
    memset(&m, 0, sizeof m); map_set(&m, 0x10020C, 0x00000123);                 /* not plausible */
    CHECK(vram_of(0x040000a1, &m) == 0, "implausible");
}

/* ======================================================================
 * video BIOS generator
 * ====================================================================== */

typedef struct img { u8 d[0x40000]; u32 size; } img_t;
static img_t G;                                     /* the image being built */
static u32 org;                                     /* where the PCI ROM starts in G (after IFR data) */

static void w8(u32 o, u32 v) { o += org; if (o < sizeof G.d) G.d[o] = (u8)v; if (o + 1 > G.size) G.size = o + 1; }
static void w16(u32 o, u32 v) { w8(o, v); w8(o + 1, v >> 8); }
static void w32(u32 o, u32 v) { w16(o, v); w16(o + 2, v >> 16); }
static void wstr(u32 o, const char *s) { while (*s) w8(o++, (u8)*s++); }

/* a PCI expansion ROM image header at base: size bytes, code type, last */
static void pci_image(u32 base, u32 size, int type, int last, u16 sig, int npde, int npde_last) {
    w16(base, sig);
    w8(base + 2, size / 512);
    w16(base + 0x18, 0x80);
    u32 p = base + 0x80;
    wstr(p, "PCIR");
    w16(p + 4, 0x10DE);
    w16(p + 6, 0x1B81);
    w16(p + 0x0A, 0x18);
    w8(p + 0x0C, 3);
    w8(p + 0x0F, 0x03);
    w16(p + 0x10, size / 512);
    w8(p + 0x14, type);
    w8(p + 0x15, last ? 0x80 : 0);
    if (npde) {
        u32 n = (p + 0x18 + 15) & ~15u;
        wstr(n, "NPDE");
        w16(n + 4, 0x0100);
        w16(n + 6, 0x10);
        w16(n + 8, size / 512);
        w8(n + 0x0A, npde_last ? 0x80 : 0);
    }
}

static void checksum(u32 base, u32 size, u32 at) {
    G.d[org + at] = 0;
    u8 s = 0;
    for (u32 i = 0; i < size; i++) s = (u8)(s + G.d[org + base + i]);
    G.d[org + at] = (u8)(0x100 - s);
}

typedef struct layout {
    const char *name;
    int chipset;
    int kind;                   /* 0 BIT, 1 BMP */
    int bmp_major;
    int dcb;                    /* 0x15, 0x20, 0x22, 0x30, 0x40, 0x41, 0: none */
    int chain;                  /* 0 legacy only, 1 legacy + EFI + NV data (NPDE), tables in the data image */
    int ifr;                    /* 0, 2 or 3 */
    int at512;                  /* junk before the ROM, found at a 512-byte boundary */
} layout_t;

/* what the parser must find */
typedef struct expect {
    int noutputs, nccb, nconn, ngpio;
    u8 out_type[16], out_conn[16], out_i2c[16];
    u8 conn_type[16];
    u8 ccb_type[16], ccb_drive[16], ccb_aux[16];
} expect_t;

static expect_t E;

/* BIT at 0x100 with tokens i, B, I, P, C, d */
static void bit_table(u32 at) {
    static const u8 ids[] = { 'i', 'B', 'I', 'P', 'C', 'd', 'p', 'u' };
    int n = (int)sizeof ids;
    w16(at, 0xB8FF);
    wstr(at + 2, "BIT");
    w8(at + 5, 0);
    w16(at + 6, 0x0100);
    w8(at + 8, 12);
    w8(at + 9, 6);
    w8(at + 10, (u32)n);
    u32 data = at + 12 + (u32)n * 6 + 0x10;
    for (int i = 0; i < n; i++) {
        u32 e = at + 12 + (u32)i * 6;
        w8(e, ids[i]);
        w8(e + 1, ids[i] == 'i' ? 2 : 1);
        w16(e + 2, ids[i] == 'i' ? 0x10 : 8);
        w16(e + 4, data + (u32)i * 0x20);
    }
    u32 v = data;                                   /* 'i': version 86.04.50.00.70 */
    w8(v + 0, 0x00); w8(v + 1, 0x50); w8(v + 2, 0x04); w8(v + 3, 0x86); w8(v + 4, 0x70); w8(v + 5, 0x00);
    u8 s = 0;
    for (int i = 0; i < 11; i++) s = (u8)(s + G.d[org + at + (u32)i]);
    w8(at + 11, (u8)(0x100 - s));
}

/* a GTX 1070-like DCB 4.1 board, a Fermi-like DCB 4.0 board, a G80-like
 * DCB 3.0 board, an NV40-like DCB 2.2 board and an NV11-like DCB 1.5 board */
static u32 dcb_build(const layout_t *L, u32 dcb, u32 tables, u32 legacy, u32 ext_file) {
    memset(&E, 0, sizeof E);
    u32 t = tables;                                 /* where the CCB / connector / GPIO tables go (pointer value) */
    u32 tf = tables;                                /* ...and in the file */
    if (L->chain) { t = legacy + 0x100; tf = ext_file + 0x100; }   /* in the NV data image */
    int v = L->dcb;
    if (v >= 0x30) {
        static const struct { u8 type, i2c, conn, or_; u32 conf; } o41[] = {
            { 6, 0, 0, 1, 0x0F400010 }, { 2, 0, 0, 1, 0x00020010 }, { 6, 1, 1, 2, 0x0F400010 }, { 2, 1, 1, 2, 0x00020010 },
            { 6, 2, 2, 4, 0x04200010 }, { 2, 3, 3, 8, 0x00020010 }, { 2, 4, 4, 4, 0x00000030 },
        };
        static const struct { u8 type, i2c, conn, or_; u32 conf; } o30[] = {
            { 0, 0, 0, 1, 0 }, { 2, 0, 0, 1, 0 }, { 0, 1, 1, 2, 0 }, { 2, 1, 1, 2, 0 }, { 1, 2, 2, 1, 0 },
        };
        int n = v >= 0x40 ? 7 : 5;
        u32 hdr = v >= 0x40 ? 27 : 0x17;
        w8(dcb, v); w8(dcb + 1, hdr); w8(dcb + 2, (u32)n + 1); w8(dcb + 3, 8);
        w32(dcb + 6, 0x4EDCBDCB);
        u32 ccb = t, conn = t + 0x40, gpio = t + 0x80;
        w16(dcb + 4, ccb);
        w16(dcb + 0x0A, gpio);
        w16(dcb + 0x14, conn);
        for (int i = 0; i < n; i++) {
            u32 e = dcb + hdr + (u32)i * 8;
            u8 ty = v >= 0x40 ? o41[i].type : o30[i].type, i2c = v >= 0x40 ? o41[i].i2c : o30[i].i2c;
            u8 cn = v >= 0x40 ? o41[i].conn : o30[i].conn, orr = v >= 0x40 ? o41[i].or_ : o30[i].or_;
            u32 heads = v >= 0x40 ? 0xF : 0x3;
            w32(e, ty | (u32)i2c << 4 | heads << 8 | (u32)cn << 12 | (u32)orr << 24);
            w32(e + 4, v >= 0x40 ? o41[i].conf : o30[i].conf);
            E.out_type[i] = ty; E.out_conn[i] = cn; E.out_i2c[i] = i2c;
        }
        w32(dcb + hdr + (u32)n * 8, 0x0000000E);    /* EOL */
        E.noutputs = n;
        /* CCB */
        u32 cf = tf;
        if (v == 0x41) {
            w8(cf, 0x41); w8(cf + 1, 6); w8(cf + 2, 5); w8(cf + 3, 4); w8(cf + 4, 0); w8(cf + 5, 1);
            static const u8 port[5] = { 0, 1, 2, 3, 4 }, aux[5] = { 0, 1, 2, 0x1F, 0x1F };
            for (int i = 0; i < 5; i++) {
                w32(cf + 6 + (u32)i * 4, port[i] | (u32)aux[i] << 5);
                E.ccb_type[i] = NV_CCB_PMGR; E.ccb_drive[i] = port[i]; E.ccb_aux[i] = aux[i] == 0x1F ? 0xFF : aux[i];
            }
            E.nccb = 5;
        } else {
            u8 cv = v == 0x40 ? 0x40 : 0x30;
            w8(cf, cv); w8(cf + 1, 5); w8(cf + 2, 5); w8(cf + 3, 4); w8(cf + 4, 0x10);
            for (int i = 0; i < 5; i++) {
                u32 e = cf + 5 + (u32)i * 4;
                if (v == 0x40 && i < 3) {           /* DP AUX on a hybrid pad, its I2C on the same pad */
                    w8(e, (u32)i); w8(e + 1, 0x01); w8(e + 3, 6);
                    E.ccb_type[i] = NV_CCB_NVIO_AUX; E.ccb_drive[i] = 0xFF; E.ccb_aux[i] = (u8)i;
                } else {
                    w8(e, (u32)i); w8(e + 1, 0); w8(e + 3, 5);
                    E.ccb_type[i] = NV_CCB_NVIO_BIT; E.ccb_drive[i] = (u8)i; E.ccb_aux[i] = 0xFF;
                }
            }
            E.nccb = 5;
        }
        /* connectors */
        u32 nf = tf + 0x40;
        static const u8 ct41[5] = { 0x46, 0x46, 0x46, 0x61, 0x31 }, ct30[3] = { 0x30, 0x30, 0x11 };
        int nc = v >= 0x40 ? 5 : 3;
        w8(nf, v >= 0x40 ? 0x40 : 0x30); w8(nf + 1, 5); w8(nf + 2, (u32)nc); w8(nf + 3, 4); w8(nf + 4, 0);
        for (int i = 0; i < nc; i++) {
            u32 e = nf + 5 + (u32)i * 4;
            u8 ty = v >= 0x40 ? ct41[i] : ct30[i];
            w8(e, ty);
            w8(e + 1, i == 0 ? 0x10 : i == 1 ? 0x20 : 0);       /* hot plug A, B */
            w8(e + 2, i == 2 ? 0x01 : i == 3 ? 0x02 : 0);       /* C, D */
            w8(e + 3, i == 4 ? 0x01 : 0);                       /* E */
            E.conn_type[i] = ty;
        }
        E.nconn = nc;
        /* GPIO */
        u32 gf = tf + 0x80;
        static const u8 gl[5] = { 8, 9, 17, 18, 19 }, gfn[5] = { 0x07, 0x08, 0x51, 0x52, 0x5E };
        if (v == 0x41) {
            w8(gf, 0x41); w8(gf + 1, 6); w8(gf + 2, 6); w8(gf + 3, 5);
            for (int i = 0; i < 5; i++) { w32(gf + 6 + (u32)i * 5, gl[i] | (u32)gfn[i] << 8); w8(gf + 6 + (u32)i * 5 + 4, 0x40); }
            w32(gf + 6 + 25, 0x0000FF00);           /* unused entry */
            E.ngpio = 5;
        } else if (v == 0x40) {
            w8(gf, 0x40); w8(gf + 1, 6); w8(gf + 2, 3); w8(gf + 3, 4);
            for (int i = 0; i < 3; i++) w32(gf + 6 + (u32)i * 4, gl[i] | (u32)gfn[i] << 8 | 0x20000000u);
            E.ngpio = 3;
        } else {
            w8(gf, 0x30); w8(gf + 1, 4); w8(gf + 2, 2); w8(gf + 3, 2);
            for (int i = 0; i < 2; i++) w16(gf + 4 + (u32)i * 2, gl[i] | (u32)gfn[i] << 5);
            E.ngpio = 2;
        }
        return tables + 0x100;
    }
    if (v >= 0x20) {                                /* DCB 2.2: header 8, entries 8, I2C table after them */
        int n = 3;
        u32 i2c = dcb + 8 + (u32)n * 8 + 8;         /* (room for a terminator) */
        w8(dcb, v); w16(dcb + 2, i2c); w32(dcb + 4, 0x4EDCBDCB);
        static const u8 ty[3] = { 0, 2, 1 }, i2[3] = { 0, 0, 1 }, cn[3] = { 0, 0, 1 };
        for (int i = 0; i < n; i++) {
            w32(dcb + 8 + (u32)i * 8, ty[i] | (u32)i2[i] << 4 | 1u << 8 | (u32)cn[i] << 12 | 1u << 24);
            E.out_type[i] = ty[i]; E.out_conn[i] = cn[i]; E.out_i2c[i] = i2[i];
        }
        w32(dcb + 8 + (u32)n * 8, 0xFFFFFFFF);
        E.noutputs = n;
        /* the 16 fixed I2C entries: CRTC pairs 3f/3e, 37/36, the rest unused */
        for (int i = 0; i < 16; i++) {
            u32 e = i2c + (u32)i * 4;
            if (i < 2) { w8(e, i ? 0x37 : 0x3F); w8(e + 1, i ? 0x36 : 0x3E); w8(e + 3, 0x00); }
            else w8(e + 3, 0x07);
            E.ccb_type[i] = i < 2 ? NV_CCB_NV04_BIT : NV_CCB_UNUSED;
            E.ccb_drive[i] = i == 0 ? 0x3F : i == 1 ? 0x37 : 0xFF;
            E.ccb_aux[i] = 0xFF;
        }
        E.nccb = 16;
        /* GPIO through dcb - 0x0f (DCB 2.2 with an extended header) */
        w8(dcb - 1, 0x13);
        u32 g = i2c + 0x50;
        w16(dcb - 0x0F, g);
        w8(g, 0x22); w8(g + 1, 2); w8(g + 2, 1); w16(g + 3, 5 | 0x07 << 5);
        E.ngpio = 1;
        /* no connector table: made up (connector 0: VGA + DVI = DVI-I, 1: TV) */
        E.nconn = 2; E.conn_type[0] = 0x30; E.conn_type[1] = 0x11;
        return g + 0x10;
    }
    if (v == 0x15) {                                /* DCB 1.5: "DEV_REC", entries of 10 bytes */
        wstr(dcb - 7, "DEV_REC");
        int n = 2;
        u32 i2c = dcb + 4 + (u32)n * 10 + 10;
        w8(dcb, 0x15); w16(dcb + 2, i2c);
        w32(dcb + 4, 0x00000000 | 1u << 25);            /* CRT, I2C 0, head 1, bus 1 (all zero ends the table) */
        w32(dcb + 14, 0x00000002 | 1u << 14 | 1u << 18);  /* TMDS, I2C 1, head 2 */
        E.out_type[0] = 0; E.out_i2c[0] = 0; E.out_conn[0] = 0;
        E.out_type[1] = 2; E.out_i2c[1] = 1; E.out_conn[1] = 1;
        E.noutputs = 2;
        for (int i = 0; i < 16; i++) {
            u32 e = i2c + (u32)i * 4;
            if (i < 2) { w8(e, i ? 0x37 : 0x3F); w8(e + 1, i ? 0x36 : 0x3E); w8(e + 3, 0x00); }
            else w8(e + 3, 0x07);
            E.ccb_type[i] = i < 2 ? NV_CCB_NV04_BIT : NV_CCB_UNUSED;
            E.ccb_drive[i] = i == 0 ? 0x3F : i == 1 ? 0x37 : 0xFF;
            E.ccb_aux[i] = 0xFF;
        }
        E.nccb = 16;
        E.nconn = 2; E.conn_type[0] = 0x00; E.conn_type[1] = 0x31;
        return i2c + 0x50;
    }
    return tables;
}

/* build a whole image for a layout into G; returns its size. The ROM:
 * legacy image 0x0000-0x1fff (PCIR 0x80, NPDE 0xa0, BIT 0x100 or BMP 0x180,
 * DCB pointer at 0x36 -> 0x600, tables at 0x700), and for the chains an EFI
 * image at 0x2000 and the NVIDIA data image at 0x3000, whose tables (at
 * 0x3100) are reached through pointers 0x2100... (past the legacy image). */
static u32 build(const layout_t *L) {
    memset(&G, 0, sizeof G);
    org = 0;
    u32 pre = L->ifr ? 0x4000 : L->at512 ? 0x1200 : 0;
    const u32 legacy = 0x2000, efi_at = 0x2000, nvd_at = 0x3000;
    if (L->ifr == 2) {
        w32(0, 0x4947564E);
        w32(4, (0x200u << 16) | (2u << 8));
        w32(0x200 + 4, pre);
    } else if (L->ifr == 3) {
        w32(0, 0x4947564E);
        w32(4, 3u << 8);
        w32(8, 0x300);
        w32(0x300, 0x800);
        wstr(0x800 + 4096, "RFRD");
        w32(0x800 + 4096 + 8, pre);
    } else if (L->at512) {
        for (u32 i = 0; i < pre; i++) w8(i, rnd() & 0x7F);    /* (no 0x55AA by accident: bytes < 0x80) */
    }
    org = pre;
    pci_image(0, legacy, 0x00, !L->chain, 0xAA55, L->chain, !L->chain);
    if (L->kind == 0) bit_table(0x100);
    else {
        u32 o = 0x180;                              /* BMP */
        w8(o, 0xFF); w8(o + 1, 0x7F); wstr(o + 2, "NV"); w8(o + 4, 0);
        w8(o + 5, (u32)L->bmp_major); w8(o + 6, L->bmp_major < 5 ? 1 : 0x10);
        w8(o + 10, 0x07); w8(o + 11, 0x20); w8(o + 12, 0x17); w8(o + 13, 0x04);   /* 04.17.20.07 */
        u8 s = 0;
        for (int i = 0; i < 7; i++) s = (u8)(s + G.d[org + o + (u32)i]);
        w8(o + 7, (u8)(0x100 - s));
    }
    if (L->dcb) {
        w16(0x36, 0x600);
        dcb_build(L, 0x600, 0x700, legacy, nvd_at);
    } else {
        memset(&E, 0, sizeof E);
        if (L->kind == 1 && L->bmp_major < 5) {     /* the BMP's two CRTC pairs (defaults) */
            E.nccb = 2;
            E.ccb_type[0] = E.ccb_type[1] = NV_CCB_NV04_BIT;
            E.ccb_drive[0] = 0x3F; E.ccb_drive[1] = 0x37;
            E.ccb_aux[0] = E.ccb_aux[1] = 0xFF;
        }
    }
    if (L->chain) {
        pci_image(efi_at, 0x1000, 0x03, 0, 0xAA55, 1, 0);
        pci_image(nvd_at, 0x1000, 0xE0, 1, 0xBB77, 1, 1);
        w8(nvd_at + 0xFFF, 0);
    } else {
        w8(legacy - 1, 0);
    }
    checksum(0, legacy, legacy - 1);
    org = 0;
    return G.size;
}

static const layout_t layouts[] = {
    { "BMP 2.1 (no DCB)", 0x10, 1, 2, 0, 0, 0, 0 },
    { "BMP 5.x + DCB 1.5", 0x11, 1, 5, 0x15, 0, 0, 0 },
    { "BMP 5.x + DCB 2.2", 0x17, 1, 5, 0x22, 0, 0, 0 },
    { "BIT + DCB 2.2", 0x40, 0, 0, 0x22, 0, 0, 0 },
    { "BIT + DCB 3.0", 0x50, 0, 0, 0x30, 0, 0, 0 },
    { "BIT + DCB 4.0", 0xc4, 0, 0, 0x40, 0, 0, 0 },
    { "BIT + DCB 4.1", 0x134, 0, 0, 0x41, 0, 0, 0 },
    { "Pascal chain (PC + EFI + NV data)", 0x134, 0, 0, 0x41, 1, 0, 0 },
    { "Pascal chain behind IFR v2", 0x134, 0, 0, 0x41, 1, 2, 0 },
    { "Pascal chain behind IFR v3", 0x134, 0, 0, 0x41, 1, 3, 0 },
    { "ROM at a 512-byte boundary", 0x124, 0, 0, 0x41, 0, 0, 1 },
};

static void check_layout(const layout_t *L, const u8 *img, u32 size) {
    static nv_bios_t b;
    int score = nv_bios_parse(img, size, L->chipset, &b);
    CHECK(score > 0, "%s: score %d (%s)", L->name, score, b.rom.why);
    if (score <= 0) return;
    CHECK(b.rom.complete && b.rom.img[0].checksum_ok, "%s: complete %d checksum %d", L->name, b.rom.complete, b.rom.img[0].checksum_ok);
    CHECK(b.rom.nimages == (L->chain ? 3 : 1), "%s: %d images", L->name, b.rom.nimages);
    if (L->chain) {
        CHECK(b.rom.img[1].type == 0x03 && b.rom.img[2].type == 0xE0 && b.rom.img[0].npde, "%s: chain types", L->name);
        CHECK(b.rom.ext_base == 0x3000 && b.rom.legacy_size == 0x2000, "%s: ext %x legacy %x", L->name, b.rom.ext_base, b.rom.legacy_size);
    }
    CHECK(b.rom.ifr_offset == (L->ifr ? 0x4000u : L->at512 ? 0x1200u : 0u), "%s: start %x", L->name, b.rom.ifr_offset);
    CHECK(b.rom.ifr_version == (u32)L->ifr, "%s: IFR %u", L->name, b.rom.ifr_version);
    if (L->kind == 0) {
        CHECK(b.bit_offset == 0x100 && b.ntokens == 8 && !(b.problems & NVB_BAD_BIT), "%s: BIT at %x, %d tokens, problems %x",
              L->name, b.bit_offset, b.ntokens, b.problems);
        CHECK(!strcmp(b.version_str, "86.04.50.00.70"), "%s: version %s", L->name, b.version_str);
        CHECK(nv_bios_token(&b, 'd') && !nv_bios_token(&b, 'Z'), "%s: tokens", L->name);
    } else {
        CHECK(b.bmp_offset == 0x180 && b.bmp_major == L->bmp_major && !(b.problems & NVB_BAD_BMP), "%s: BMP %x %d problems %x",
              L->name, b.bmp_offset, b.bmp_major, b.problems);
        CHECK(!strcmp(b.version_str, "04.17.20.07.00"), "%s: version %s", L->name, b.version_str);
    }
    CHECK(!(b.problems & (NVB_OOB | NVB_BAD_DCB | NVB_BAD_CCB | NVB_BAD_CONN | NVB_BAD_GPIO)), "%s: problems %x (oob %u)",
          L->name, b.problems, b.oob);
    CHECK(b.dcb_ver == L->dcb, "%s: DCB %x", L->name, b.dcb_ver);
    CHECK(b.noutputs == E.noutputs, "%s: %d outputs (want %d)", L->name, b.noutputs, E.noutputs);
    for (int i = 0; i < b.noutputs && i < E.noutputs; i++)
        CHECK(b.out[i].type == E.out_type[i] && b.out[i].connector == E.out_conn[i] && b.out[i].i2c == E.out_i2c[i],
              "%s: output %d: type %d conn %d i2c %d", L->name, i, b.out[i].type, b.out[i].connector, b.out[i].i2c);
    CHECK(b.nccb == E.nccb, "%s: %d CCB entries (want %d)", L->name, b.nccb, E.nccb);
    for (int i = 0; i < b.nccb && i < E.nccb; i++)
        CHECK(b.ccb[i].type == E.ccb_type[i] && (b.ccb[i].type == NV_CCB_UNUSED || b.ccb[i].drive == E.ccb_drive[i]) &&
              b.ccb[i].auxch == E.ccb_aux[i], "%s: CCB %d: type %02x drive %02x aux %02x", L->name, i, b.ccb[i].type,
              b.ccb[i].drive, b.ccb[i].auxch);
    CHECK(b.nconn == E.nconn, "%s: %d connectors (want %d)", L->name, b.nconn, E.nconn);
    for (int i = 0; i < b.nconn && i < E.nconn; i++)
        CHECK(b.conn[i].type == E.conn_type[i], "%s: connector %d type %02x (want %02x)", L->name, i, b.conn[i].type, E.conn_type[i]);
    CHECK(b.ngpio == E.ngpio, "%s: %d GPIOs (want %d)", L->name, b.ngpio, E.ngpio);
    if (L->dcb >= 0x40) {
        CHECK(b.conn[0].hpd == 1 && b.conn[1].hpd == 2 && b.conn[2].hpd == 4 && b.conn[3].hpd == 8 && b.conn[4].hpd == 16,
              "%s: hot plug bits %x %x %x %x %x", L->name, b.conn[0].hpd, b.conn[1].hpd, b.conn[2].hpd, b.conn[3].hpd, b.conn[4].hpd);
        CHECK(b.gpio[0].func == 0x07 && b.gpio[0].line == 8, "%s: gpio 0 %02x line %d", L->name, b.gpio[0].func, b.gpio[0].line);
        CHECK(b.out[0].dp_max_lanes == 4 && b.out[0].dp_max_rate == 2 && b.out[4].dp_max_lanes == 4 && b.out[4].dp_max_rate == 1,
              "%s: DP lanes %d rate %d", L->name, b.out[0].dp_max_lanes, b.out[0].dp_max_rate);
        CHECK(b.out[1].hdmi && !b.out[6].hdmi && b.out[6].link == 3, "%s: hdmi / dual link", L->name);
    }
    if (L->dcb == 0x41) CHECK(b.ccb_primary == 0 && b.ccb_secondary == 1 && b.ccb[0].share == 0, "%s: CCB ports", L->name);
    if (L->dcb == 0x40) CHECK(b.ccb[0].share == 0 && b.ccb[3].share == 0xFF, "%s: hybrid pads", L->name);
    if (verbose) printf("  %-36s score %d, %d outputs, %d CCB, %d connectors, %d GPIOs, %u bytes\n", L->name, score, b.noutputs,
                        b.nccb, b.nconn, b.ngpio, size);
}

/* malformed: every truncation, then random mutations; only "no crash"
 * and sane counts are required (ASan / UBSan check the rest) */
static void abuse(const layout_t *L, const u8 *img, u32 size) {
    static nv_bios_t b;
    u8 *m = malloc(size);
    u32 step = size > 0x4000 ? 7 : 1;
    for (u32 n = 0; n <= size; n += step) {
        memcpy(m, img, n);
        int s = nv_bios_parse(m, n, L->chipset, &b);
        CHECK(b.noutputs <= NV_DCB_MAX && b.nccb <= NV_CCB_MAX && b.nconn <= NV_CONN_MAX && b.ngpio <= NV_GPIO_MAX,
              "%s cut at %u: counts", L->name, n);
        if (n < size && s > 0 && n < (u32)(b.rom.ifr_offset + b.rom.chain_size)) CHECK(!b.rom.complete, "%s cut at %u: complete", L->name, n);
    }
    for (int k = 0; k < 20000; k++) {
        memcpy(m, img, size);
        int flips = 1 + (int)(rnd() % 8);
        for (int f = 0; f < flips; f++) {
            u32 at = rnd() % size;
            switch (rnd() % 4) {
            case 0: m[at] = (u8)rnd(); break;
            case 1: m[at] ^= (u8)(1u << (rnd() % 8)); break;
            case 2: m[at] = 0xFF; break;
            default: m[at] = 0; break;
            }
        }
        nv_bios_parse(m, size - (rnd() % 16 == 0 ? rnd() % size : 0), L->chipset, &b);
    }
    free(m);
}

static void hostile(void) {
    static nv_bios_t b;
    layout_t L = layouts[6];                        /* BIT + DCB 4.1 */
    u32 size = build(&L);
    u8 *img = malloc(size);
    memcpy(img, G.d, size);
    u32 dcb = 0x600;
    /* bad legacy checksum: still used, scored lower */
    nv_rom_t r1, r2;
    int good = nv_rom_scan(img, size, &r1);
    img[0x1000] ^= 1;
    int bad = nv_rom_scan(img, size, &r2);
    CHECK(bad > 0 && bad < good && !r2.img[0].checksum_ok, "checksum: %d < %d", bad, good);
    img[0x1000] ^= 1;
    /* not NVIDIA */
    memcpy(G.d, img, size); w16(0x84, 0x1002);
    CHECK(!nv_bios_parse(G.d, size, 0x134, &b) && strstr(b.rom.why, "not NVIDIA"), "vendor: %s", b.rom.why);
    /* image size 0 */
    memcpy(G.d, img, size); w16(0x90, 0);
    CHECK(!nv_bios_parse(G.d, size, 0x134, &b), "size 0");
    /* PCIR pointer outside */
    memcpy(G.d, img, size); w16(0x18, 0xFFF0);
    CHECK(!nv_bios_parse(G.d, size, 0x134, &b), "PCIR outside");
    /* DCB signature */
    memcpy(G.d, img, size); w32(dcb + 6, 0x12345678);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK((b.problems & NVB_BAD_DCB) && b.noutputs == 0, "DCB signature: %x", b.problems);
    /* DCB version 4.2 */
    memcpy(G.d, img, size); w8(dcb, 0x42);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK(b.problems & NVB_BAD_DCB, "DCB 4.2");
    /* 255 entries of 255 bytes: kept to the maximum, reads bounded */
    memcpy(G.d, img, size); w8(dcb + 2, 255); w8(dcb + 3, 255);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK(b.noutputs <= NV_DCB_MAX && (b.problems & (NVB_TRUNC | NVB_OOB | NVB_BAD_DCB)), "DCB 255x255: %d, %x", b.noutputs, b.problems);
    /* pointers past the end */
    memcpy(G.d, img, size); w16(dcb + 4, 0xFFF0); w16(dcb + 0x0A, 0xFFFE); w16(dcb + 0x14, 0xFFFF);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK(b.nccb == 0 && b.ngpio == 0 && (b.problems & NVB_OOB), "pointers past the end: %d %d %x", b.nccb, b.ngpio, b.problems);
    /* CCB version 4.2, GPIO version 5.0, connector version 2.0 */
    memcpy(G.d, img, size); w8(0x700, 0x42); w8(0x780, 0x50); w8(0x740, 0x20);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK((b.problems & NVB_BAD_CCB) && (b.problems & NVB_BAD_GPIO) && (b.problems & NVB_BAD_CONN), "table versions: %x", b.problems);
    /* BIT header checksum */
    memcpy(G.d, img, size); G.d[0x100 + 11] ^= 0x55;
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK((b.problems & NVB_BAD_BIT) && b.ntokens == 8, "BIT checksum: %x", b.problems);
    /* BIT token count 255, token size 255 */
    memcpy(G.d, img, size); w8(0x100 + 10, 255); w8(0x100 + 9, 255);
    nv_bios_parse(G.d, size, 0x134, &b);
    CHECK(b.ntokens <= NV_BIT_MAX, "BIT 255 tokens");
    /* IFR pointing nowhere: the 512-byte scan still finds the ROM */
    layout_t I = layouts[8];
    u32 s2 = build(&I);
    w32(0x200 + 4, 0x7FFFF);
    CHECK(nv_bios_parse(G.d, s2, 0x134, &b) > 0 && b.rom.ifr_offset == 0x4000, "IFR bad offset: start %x", b.rom.ifr_offset);
    /* IFR v3 directory signature wrong */
    layout_t I3 = layouts[9];
    s2 = build(&I3);
    w32(0x800 + 4096, 0);
    CHECK(nv_bios_parse(G.d, s2, 0x134, &b) > 0, "IFR v3 without RFRD (512-byte scan)");
    /* empty and tiny inputs */
    CHECK(!nv_bios_parse(img, 0, 0x134, &b) && !nv_bios_parse(img, 3, 0x134, &b) && !nv_bios_parse(NULL, 100, 0, &b), "tiny");
    free(img);
}

static void test_vbios(void) {
    for (unsigned i = 0; i < ARRAY_SIZE(layouts); i++) {
        u32 size = build(&layouts[i]);
        u8 *img = malloc(size);
        memcpy(img, G.d, size);
        expect_t keep = E;
        check_layout(&layouts[i], img, size);
        E = keep;
        abuse(&layouts[i], img, size);
        free(img);
    }
    hostile();
}

/* ======================================================================
 * EDID
 * ====================================================================== */

static void dtd_put(u8 *p, int w, int h, int hz) {
    int hb = 160, vb = 30;
    u32 clk = (u32)((u64)(w + hb) * (h + vb) * hz / 10000);
    p[0] = clk & 0xFF; p[1] = (u8)(clk >> 8);
    p[2] = w & 0xFF; p[3] = hb & 0xFF; p[4] = (u8)(((w >> 8) << 4) | (hb >> 8));
    p[5] = h & 0xFF; p[6] = vb & 0xFF; p[7] = (u8)(((h >> 8) << 4) | (vb >> 8));
    p[8] = 48; p[9] = 32; p[10] = 0x35; p[11] = 0;
}

static void fix(u8 *b) { u8 s = 0; for (int i = 0; i < 127; i++) s = (u8)(s + b[i]); b[127] = (u8)(0x100 - s); }

/* base block (1920x1200, "HALCYON TEST") + CEA-861 (2 DTDs) + DisplayID (type I) */
static int make_edid(u8 *e, int blocks) {
    memset(e, 0, 128 * (size_t)blocks);
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(e, hdr, 8);
    e[8] = 0x22; e[9] = 0x0E;                       /* "HAN" */
    e[10] = 0x70; e[11] = 0x10;
    e[17] = 30; e[18] = 1; e[19] = 4;
    dtd_put(e + 54, 1920, 1200, 60);
    e[54 + 12] = 0x08; e[54 + 13] = 0x4F; e[54 + 14] = 0x21;
    u8 *n = e + 72;
    n[3] = 0xFC;
    memcpy(n + 5, "HALCYON TEST\n", 13);
    e[126] = (u8)(blocks - 1);
    fix(e);
    if (blocks > 1) {
        u8 *c = e + 128;
        c[0] = 0x02; c[1] = 3; c[2] = 4;
        dtd_put(c + 4, 1920, 1080, 60);
        dtd_put(c + 22, 1280, 720, 60);
        fix(c);
    }
    if (blocks > 2) {
        u8 *d = e + 256;
        d[0] = 0x70; d[1] = 0x12; d[2] = 23; d[3] = 0; d[4] = 0;
        d[5] = 0x03; d[6] = 0; d[7] = 20;           /* type I detailed timing: 3840x2160 */
        u8 *t = d + 8;
        u32 clk = 59400 - 1;                        /* 594 MHz in 10 kHz units */
        t[0] = clk & 0xFF; t[1] = (u8)(clk >> 8); t[2] = (u8)(clk >> 16); t[3] = 0x80;
        int w = 3840 - 1, hb = 560 - 1, h = 2160 - 1, vb = 90 - 1;
        t[4] = w & 0xFF; t[5] = (u8)(w >> 8); t[6] = hb & 0xFF; t[7] = (u8)(hb >> 8); t[8] = 176 - 1; t[10] = 88 - 1;
        t[12] = h & 0xFF; t[13] = (u8)(h >> 8); t[14] = vb & 0xFF; t[15] = (u8)(vb >> 8); t[16] = 8 - 1; t[18] = 10 - 1;
        fix(d);
    }
    for (int k = 3; k < blocks; k++) { e[k * 128] = 0xF0; fix(e + k * 128); }
    return 128 * blocks;
}

static void test_edid(void) {
    u8 e[512];
    int n = make_edid(e, 3);
    edid_info_t i;
    CHECK(edid_parse(e, n, &i) && i.native_w == 1920 && i.native_h == 1200 && !strcmp(i.name, "HALCYON TEST"), "base %dx%d '%s'",
          i.native_w, i.native_h, i.name);
    CHECK(edid_block_count(e, n) == 3, "blocks %d", edid_block_count(e, n));
    edid_timing_t t[8];
    int k = edid_detailed_timings(e, n, t, 8);
    CHECK(k == 4, "timings %d", k);
    CHECK(t[0].w == 1920 && t[0].h == 1200 && t[0].preferred && t[0].hz == 60, "t0 %dx%d@%d", t[0].w, t[0].h, t[0].hz);
    CHECK(t[1].w == 1920 && t[1].h == 1080 && t[1].block == 1 && t[2].w == 1280, "cea %dx%d %dx%d", t[1].w, t[1].h, t[2].w, t[2].h);
    CHECK(t[3].w == 3840 && t[3].h == 2160 && t[3].hz == 60 && t[3].clock_khz == 594000 && t[3].block == 2, "displayid %dx%d@%d %u",
          t[3].w, t[3].h, t[3].hz, t[3].clock_khz);
    CHECK(!strcmp(edid_ext_name(0x70), "DisplayID"), "ext name");
    char s[64];
    edid_timing_str(&t[0], s, sizeof s);
    CHECK(!strcmp(s, "1920x1200 at 60 Hz (153.50 MHz)"), "str '%s'", s);
    CHECK(edid_block_count(e, 200) == 1 && edid_detailed_timings(e, 0, t, 8) == 0, "short");
    /* garbage extension blocks must not crash */
    for (int r = 0; r < 20000; r++) {
        make_edid(e, 3);
        for (int f = 0; f < 6; f++) e[128 + rnd() % 384] = (u8)rnd();
        if (r & 1) e[126] = (u8)rnd();
        edid_detailed_timings(e, 384, t, 8);
    }
}

/* ======================================================================
 * simulated I2C bus with an E-DDC EEPROM
 * ====================================================================== */

typedef struct bus {
    u64 t;                      /* simulated microseconds */
    int m_scl, m_sda;           /* master drives (1 = released) */
    int s_sda;                  /* slave drives SDA (1 = released) */
    u64 stretch_until;          /* slave holds SCL low until then */
    int scl, sda;               /* line levels last seen by the slave */
    /* slave */
    enum { S_IDLE, S_ADDR, S_ACK_ADDR, S_WRITE, S_ACK_WRITE, S_READ, S_READ_ACK, S_IGNORE } st;
    u8 shift; int bits, rw, dev, first;
    u8 seg, off;
    u8 mem[1024];
    int acked;
    /* faults */
    int present;                /* the EEPROM answers */
    u32 stretch_us;             /* hold SCL after each acknowledge */
    int stuck_sda;              /* SDA low for good */
    int bytes;                  /* statistics */
    int starts, stops;
} bus_t;

static int line_scl(bus_t *b) { return b->m_scl && b->t >= b->stretch_until; }
static int line_sda(bus_t *b) { return b->m_sda && b->s_sda && !b->stuck_sda; }

static void slave_byte_out(bus_t *b) {
    u8 v = b->mem[((u32)b->seg * 256 + b->off) % sizeof b->mem];
    b->shift = v;
    b->bits = 0;
    b->s_sda = (v >> 7) & 1;
}

static void sim(bus_t *b) {
    int scl = line_scl(b), sda = line_sda(b);
    if (scl && b->scl && sda != b->sda) {           /* SDA changed while SCL high */
        if (!sda) { b->st = S_ADDR; b->bits = 0; b->shift = 0; b->s_sda = 1; b->starts++; }
        else { b->st = S_IDLE; b->s_sda = 1; b->seg = 0; b->stops++; }
    } else if (scl && !b->scl) {                    /* rising edge: sample */
        if (b->st == S_ADDR || b->st == S_WRITE) { b->shift = (u8)(b->shift << 1 | sda); b->bits++; }
        else if (b->st == S_READ_ACK) b->acked = !sda;
    } else if (!scl && b->scl) {                    /* falling edge: drive */
        switch (b->st) {
        case S_ADDR: case S_WRITE:
            if (b->bits < 8) break;
            if (b->st == S_ADDR) {
                b->dev = b->shift >> 1;
                b->rw = b->shift & 1;
                if (b->present && (b->dev == 0x50 || (b->dev == 0x30 && !b->rw))) { b->s_sda = 0; b->st = S_ACK_ADDR; b->first = 1; }
                else { b->s_sda = 1; b->st = S_IGNORE; }
            } else {
                if (b->dev == 0x30) b->seg = b->shift;
                else if (b->first) { b->off = b->shift; b->first = 0; }
                else b->off++;
                b->s_sda = 0;
                b->st = S_ACK_WRITE;
                b->bytes++;
            }
            break;
        case S_ACK_ADDR: case S_ACK_WRITE:
            if (b->stretch_us) b->stretch_until = b->t + b->stretch_us;
            if (b->st == S_ACK_ADDR && b->rw) { b->st = S_READ; slave_byte_out(b); }
            else { b->s_sda = 1; b->st = S_WRITE; b->bits = 0; b->shift = 0; }
            break;
        case S_READ:
            b->bits++;
            if (b->bits < 8) b->s_sda = (b->shift >> (7 - b->bits)) & 1;
            else { b->s_sda = 1; b->st = S_READ_ACK; }
            break;
        case S_READ_ACK:
            if (b->acked) { b->off++; b->bytes++; b->st = S_READ; slave_byte_out(b); }
            else { b->off++; b->bytes++; b->s_sda = 1; b->st = S_IGNORE; }
            break;
        default: break;
        }
    }
    b->scl = line_scl(b);
    b->sda = line_sda(b);
}

static void b_scl(void *c, int s) { bus_t *b = c; b->m_scl = s; sim(b); }
static void b_sda(void *c, int s) { bus_t *b = c; b->m_sda = s; sim(b); }
static int b_gscl(void *c) { bus_t *b = c; sim(b); return line_scl(b); }
static int b_gsda(void *c) { bus_t *b = c; sim(b); return line_sda(b); }
static void b_delay(void *c, u32 us) { bus_t *b = c; b->t += us; sim(b); }
static u64 b_now(void *c) { bus_t *b = c; return b->t; }

static void bus_init(bus_t *b, nv_i2c_bb_t *bb, int blocks) {
    memset(b, 0, sizeof *b);
    b->m_scl = b->m_sda = b->s_sda = 1;
    b->scl = b->sda = 1;
    b->present = 1;
    make_edid(b->mem, blocks);
    memset(bb, 0, sizeof *bb);
    bb->ctx = b;
    bb->drive_scl = b_scl; bb->drive_sda = b_sda; bb->sense_scl = b_gscl; bb->sense_sda = b_gsda;
    bb->delay_us = b_delay; bb->now_us = b_now;
    bb->half_us = 5; bb->stretch_us = 2200; bb->total_us = 400000;
}

static int bb_fn(void *ctx, nv_i2c_msg_t *m, int n) { return nv_i2c_bb_xfer(ctx, m, n); }

static void test_i2c(void) {
    bus_t b;
    nv_i2c_bb_t bb;
    u8 buf[1024], want[1024];
    char note[64];
    for (int blocks = 1; blocks <= 4; blocks++) {
        bus_init(&b, &bb, blocks);
        make_edid(want, blocks);
        int r = nv_ddc_read_edid(bb_fn, &bb, buf, sizeof buf, note, sizeof note);
        CHECK(r == blocks * 128 && !memcmp(buf, want, (size_t)r), "%d blocks: r %d note '%s'", blocks, r, note);
        CHECK(b.st == S_IDLE || b.st == S_IGNORE, "%d blocks: bus left in state %d", blocks, b.st);
        if (verbose) printf("  EDID %d blocks over I2C: %llu us simulated, %d starts\n", blocks, (unsigned long long)b.t, b.starts);
    }
    /* the buffer is smaller than the EDID */
    bus_init(&b, &bb, 4);
    int r = nv_ddc_read_edid(bb_fn, &bb, buf, 256, note, sizeof note);
    CHECK(r == 256 && strstr(note, "kept"), "max 256: %d '%s'", r, note);
    /* nobody at 0x50 */
    bus_init(&b, &bb, 1);
    b.present = 0;
    r = nv_ddc_read_edid(bb_fn, &bb, buf, sizeof buf, note, sizeof note);
    CHECK(r == E_IO && strstr(note, "no answer"), "absent: %d '%s'", r, note);
    CHECK(b.t < 100000, "absent took %llu us", (unsigned long long)b.t);
    /* clock stretching of 1 ms after each byte: fine */
    bus_init(&b, &bb, 2);
    b.stretch_us = 1000;
    make_edid(want, 2);
    r = nv_ddc_read_edid(bb_fn, &bb, buf, sizeof buf, note, sizeof note);
    CHECK(r == 256 && !memcmp(buf, want, 256), "stretch 1 ms: %d '%s'", r, note);
    /* clock held 5 ms: too long */
    bus_init(&b, &bb, 1);
    b.stretch_us = 5000;
    nv_i2c_msg_t m[2];
    u8 off = 0;
    m[0] = (nv_i2c_msg_t){ 0x50, 0, 1, &off };
    m[1] = (nv_i2c_msg_t){ 0x50, NV_I2C_RD, 128, buf };
    r = nv_i2c_bb_xfer(&bb, m, 2);
    CHECK(r == E_TIMEOUT, "stretch 5 ms: %d", r);
    /* a slave stuck in the middle of sending a 0 byte: nine clocks free it */
    bus_init(&b, &bb, 1);
    b.st = S_READ; b.shift = 0x00; b.bits = 0; b.s_sda = 0; b.sda = 0;
    make_edid(want, 1);
    r = nv_ddc_read_edid(bb_fn, &bb, buf, sizeof buf, note, sizeof note);
    CHECK(r == 128 && !memcmp(buf, want, 128), "stuck mid-byte: %d '%s'", r, note);
    /* SDA low for good */
    bus_init(&b, &bb, 1);
    b.stuck_sda = 1;
    r = nv_i2c_bb_xfer(&bb, m, 2);
    CHECK(r == E_BUSY, "SDA stuck: %d", r);
    /* the whole transfer is bounded too */
    bus_init(&b, &bb, 1);
    bb.total_us = 2000;
    r = nv_i2c_bb_xfer(&bb, m, 2);
    CHECK(r == E_TIMEOUT && b.t < 10000, "total timeout: %d after %llu us", r, (unsigned long long)b.t);
    /* a write to the EEPROM's offset register, then a short read */
    bus_init(&b, &bb, 1);
    off = 8;
    m[1].len = 4;
    r = nv_i2c_bb_xfer(&bb, m, 2);
    make_edid(want, 1);
    CHECK(!r && !memcmp(buf, want + 8, 4), "offset 8: %d %02x%02x", r, buf[0], buf[1]);
}

/* ======================================================================
 * simulated AUX channel and DisplayPort sink
 * ====================================================================== */

typedef struct auxsim {
    int layout;
    u32 base;                   /* channel register block */
    u32 ctrl, stat, addr, out[4], in[4], autodpcd;
    int hpd, grant_never, busy_never, defer_n, i2c_defer_n, rx_err_n, sink_timeout_n;
    u8 dpcd[0x100];
    u8 mem[1024];
    u8 seg, off;
    int edid_present;
    int txns, mot_open;
    u32 bad_reg;                /* a register outside the channel was touched */
} auxsim_t;

static u32 a_rd(void *c, u32 reg) {
    auxsim_t *a = c;
    u32 o = reg - a->base;
    switch (o) {
    case 0x10: case 0x14: case 0x18: case 0x1C: return a->in[(o - 0x10) / 4];
    case 0x20: return a->addr;
    case 0x24: return a->ctrl;
    case 0x28: return a->stat;
    case 0x38: if (a->layout == NV_AUXHW_GM200) return a->autodpcd; break;
    }
    if (o < 0x10) return a->out[o / 4];
    a->bad_reg = reg;
    return 0xBADF1100;
}

static void a_txn(auxsim_t *a) {
    a->txns++;
    u32 c = a->ctrl;
    u8 req = (c >> 12) & 0xF;
    int len = (c & 0x100) ? 0 : (int)(c & 0xFF) + 1;
    u8 *out = (u8 *)a->out, *in = (u8 *)a->in;
    u32 reply = 0, got = 0;
    a->stat &= 0x10000000;
    if (a->sink_timeout_n > 0) { a->sink_timeout_n--; a->stat |= 0x100; return; }
    if (a->rx_err_n > 0) { a->rx_err_n--; a->stat |= 0x400; return; }
    if (req & 8) {                                  /* native */
        if (a->defer_n > 0) { a->defer_n--; reply = 2; }
        else if (req == 9) { for (int i = 0; i < len; i++) in[i] = a->dpcd[(a->addr + (u32)i) & 0xFF]; got = (u32)len; }
    } else {                                        /* I2C over AUX */
        int rd = req & 1, mot = req & 4;
        if (a->i2c_defer_n > 0) { a->i2c_defer_n--; reply = 8; }
        else if (!a->edid_present || (a->addr != 0x50 && a->addr != 0x30)) reply = 4;
        else if (rd) {
            for (int i = 0; i < len; i++) in[i] = a->mem[((u32)a->seg * 256 + a->off++) % sizeof a->mem];
            got = (u32)len;
        } else if (len) {
            if (a->addr == 0x30) a->seg = out[0];
            else a->off = out[0];
        }
        a->mot_open = mot != 0;
        if (!mot) a->seg = 0;                       /* the I2C STOP resets the segment pointer */
    }
    a->stat |= reply << 16 | got;
}

static void a_wr(void *c, u32 reg, u32 v) {
    auxsim_t *a = c;
    u32 o = reg - a->base;
    if (o < 0x10) { a->out[o / 4] = v; return; }
    if (o == 0x20) { a->addr = v; return; }
    if (o == 0x28) { a->stat &= ~(v & 0x0000FFFFu) | 0x10000000u; return; }   /* (status write-back) */
    if (o == 0x38 && a->layout == NV_AUXHW_GM200) { a->autodpcd = v; return; }
    if (o != 0x24) { a->bad_reg = reg; return; }
    u32 req_mask = a->layout == NV_AUXHW_GM200 ? 0x00700000 : 0x00300000, rep_mask = a->layout == NV_AUXHW_GM200 ? 0x07000000 : 0x03000000;
    u32 keep = v & ~(rep_mask | 0x80000000u);
    if (!a->grant_never && (v & req_mask) == 0x00100000) keep |= 0x01000000;
    if ((v & 0x00010000) && !a->busy_never) { a->ctrl = keep; a_txn(a); keep &= ~0x00010000u; }
    a->ctrl = keep;
}

static u64 a_t;
static void a_delay(void *c, u32 us) { a_t += us; }
static u64 a_now(void *c) { return a_t += 1; }

static nv_regs_t aregs;
static nv_aux_hw_t ahw;
static int a_fn(void *ctx, u8 req, u32 addr, u8 *buf, u8 *len) { return nv_aux_hw_xfer(ctx, req, addr, buf, len); }

static void aux_init(auxsim_t *a, nv_aux_t *ops, int layout, int ch, int blocks) {
    memset(a, 0, sizeof *a);
    a->layout = layout;
    a->base = (layout == NV_AUXHW_GM200 ? 0xD930u : 0xE4C0u) + (u32)ch * 0x50;
    a->hpd = 1;
    a->stat = 0x10000000;
    a->autodpcd = 0x00010000;
    a->ctrl = 0x00000000;
    a->edid_present = 1;
    for (int i = 0; i < 0x100; i++) a->dpcd[i] = (u8)i;
    a->dpcd[0] = 0x12; a->dpcd[1] = 0x14; a->dpcd[2] = 0x84;
    make_edid(a->mem, blocks);
    aregs = (nv_regs_t){ a, a_rd, a_wr, a_delay, a_now };
    memset(&ahw, 0, sizeof ahw);
    ahw.r = &aregs;
    ahw.layout = layout;
    ahw.ch = ch;
    memset(ops, 0, sizeof *ops);
    ops->ctx = &ahw;
    ops->xfer = a_fn;
    ops->delay_us = a_delay;
}

static void aux_restored(auxsim_t *a, const char *what) {
    u32 req_mask = a->layout == NV_AUXHW_GM200 ? 0x00700000 : 0x00300000;
    CHECK((a->ctrl & (req_mask | 0x00010000)) == 0, "%s: request bits left %08x", what, a->ctrl);
    if (a->layout == NV_AUXHW_GM200) CHECK(a->autodpcd == 0x00010000, "%s: auto-DPCD %08x", what, a->autodpcd);
    CHECK(!a->bad_reg, "%s: touched register %06x", what, a->bad_reg);
}

static void test_aux(void) {
    for (int layout = 0; layout < 2; layout++) {
        const char *ln = layout ? "GM200" : "G94";
        for (int ch = 0; ch < 4; ch += 3) {
            auxsim_t a;
            nv_aux_t ops;
            u8 buf[1024], want[1024];
            char note[64], what[64];
            aux_init(&a, &ops, layout, ch, 2);
            int r = nv_aux_native_read(&ops, 0, buf, 20);
            CHECK(r == 20 && buf[0] == 0x12 && buf[19] == 19, "%s ch %d DPCD: %d", ln, ch, r);
            snprintf(what, sizeof what, "%s ch %d DPCD", ln, ch);
            aux_restored(&a, what);
            /* EDID over I2C-over-AUX, 1 to 4 blocks (segments) */
            for (int blocks = 1; blocks <= 4; blocks++) {
                aux_init(&a, &ops, layout, ch, blocks);
                make_edid(want, blocks);
                r = nv_ddc_read_edid(nv_aux_i2c_fn, &ops, buf, sizeof buf, note, sizeof note);
                CHECK(r == blocks * 128 && !memcmp(buf, want, (size_t)r) && !a.mot_open, "%s ch %d EDID %d blocks: %d '%s' mot %d",
                      ln, ch, blocks, r, note, a.mot_open);
                snprintf(what, sizeof what, "%s ch %d EDID", ln, ch);
                aux_restored(&a, what);
            }
            /* DEFERs: retried */
            aux_init(&a, &ops, layout, ch, 1);
            a.defer_n = 5;
            r = nv_aux_native_read(&ops, 0x10, buf, 4);
            CHECK(r == 4 && buf[0] == 0x10 && ops.defers == 5, "%s defer: %d (%u defers)", ln, r, ops.defers);
            aux_init(&a, &ops, layout, ch, 1);
            a.i2c_defer_n = 7;
            make_edid(want, 1);
            r = nv_ddc_read_edid(nv_aux_i2c_fn, &ops, buf, sizeof buf, note, sizeof note);
            CHECK(r == 128 && !memcmp(buf, want, 128), "%s I2C defer: %d", ln, r);
            /* DEFER forever: gives up */
            aux_init(&a, &ops, layout, ch, 1);
            a.defer_n = 1000;
            r = nv_aux_native_read(&ops, 0, buf, 1);
            CHECK(r == E_TIMEOUT && a.txns <= 40, "%s defer forever: %d after %d", ln, r, a.txns);
            /* no EDID device: I2C NACK */
            aux_init(&a, &ops, layout, ch, 1);
            a.edid_present = 0;
            r = nv_ddc_read_edid(nv_aux_i2c_fn, &ops, buf, sizeof buf, note, sizeof note);
            CHECK(r == E_IO && !a.mot_open, "%s no EDID: %d mot %d", ln, r, a.mot_open);
            aux_restored(&a, "no EDID");
            /* no sink (hot plug sense low) */
            aux_init(&a, &ops, layout, ch, 1);
            a.stat = 0;
            u8 n4 = 4;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == E_NODEV, "%s no sink: %d", ln, r);
            aux_restored(&a, "no sink");
            /* the channel is never granted */
            aux_init(&a, &ops, layout, ch, 1);
            a.grant_never = 1;
            a_t = 0;
            n4 = 4;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == E_TIMEOUT && a_t < 5000, "%s no grant: %d after %llu us", ln, r, (unsigned long long)a_t);
            aux_restored(&a, "no grant");
            /* the transaction never finishes */
            aux_init(&a, &ops, layout, ch, 1);
            a.busy_never = 1;
            a_t = 0;
            n4 = 4;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == E_TIMEOUT && a_t < 10000, "%s busy: %d after %llu us", ln, r, (unsigned long long)a_t);
            a.busy_never = 0;
            /* busy before we start (another owner): never asks */
            aux_init(&a, &ops, layout, ch, 1);
            a.ctrl = 0x00010000;
            a.busy_never = 1;
            n4 = 4;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == E_TIMEOUT, "%s busy before: %d", ln, r);
            /* receive errors and sink timeouts: reported, retried by the generic layer */
            aux_init(&a, &ops, layout, ch, 1);
            a.rx_err_n = 1;
            n4 = 4;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == E_IO, "%s rx error: %d", ln, r);
            aux_init(&a, &ops, layout, ch, 1);
            a.sink_timeout_n = 2;
            r = nv_aux_native_read(&ops, 0, buf, 4);
            CHECK(r == 4, "%s sink timeouts retried: %d", ln, r);
            aux_restored(&a, "timeouts");
            /* a request bit already set by the firmware is put back */
            aux_init(&a, &ops, layout, ch, 1);
            a.ctrl = 0x00200000;
            n4 = 2;
            r = nv_aux_hw_xfer(&ahw, NV_AUX_NATIVE_RD, 0, buf, &n4);
            CHECK(r == 0 && (a.ctrl & 0x00300000) == 0x00200000, "%s owner bits restored: %08x", ln, a.ctrl);
        }
    }
}

/* ======================================================================
 * main
 * ====================================================================== */

static int write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(d, 1, n, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--write-rom")) {
        u32 size = build(&layouts[7]);
        return write_file(argv[2], G.d, size);
    }
    if (argc == 3 && !strcmp(argv[1], "--write-edid")) {
        u8 e[256];
        return write_file(argv[2], e, (size_t)make_edid(e, 2));
    }
    if (argc == 3 && !strcmp(argv[1], "--seeds")) {
        char p[512];
        for (unsigned i = 0; i < ARRAY_SIZE(layouts); i++) {
            u32 size = build(&layouts[i]);
            snprintf(p, sizeof p, "%s/rom%02u.bin", argv[2], i);
            if (write_file(p, G.d, size)) return 1;
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    test_chips();
    test_vram();
    test_vbios();
    test_edid();
    test_i2c();
    test_aux();
    if (failed) printf("nvtest: %d of %d checks FAILED\n", failed, checks);
    else printf("nvtest: all %d checks passed (chip table, VRAM, %u VBIOS layouts with truncations and 20,000 mutations each, "
                "EDID, I2C and AUX against simulated buses)\n", checks, (unsigned)ARRAY_SIZE(layouts));
    return failed != 0;
}
