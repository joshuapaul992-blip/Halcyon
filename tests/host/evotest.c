/* Host test: the NVIDIA EVO display engine code of M10b (kernel/drivers/nv/
 * evo.c, evodp.c, evopll.c, evoscript.c), built with ASan/UBSan, against
 * a simulated display engine.
 *
 *   - generations: every chipset from NV50 to GP108 to its core class and
 *     method set, nothing for older or newer chips
 *   - method streams for one head per method set (507d, 827d, 907d, 917d)
 *     compared word by word with sequences written out here from nouveau's
 *     dispnv50/head*.c, sor*.c, dac*.c and core507d.c; head off, OR off
 *   - CVT / CEA timings to the EVO raster (nouveau's bias), bad timings
 *   - the object table hash and context DMAs (NV50 and GF119 layouts),
 *     read back by the simulator the way the hardware resolves handles
 *   - the VRAM allocator: holes, alignment, exhaustion, free
 *   - the video PLL: limits tables 3.0, 4.0 and 5.0, the GT215 calculator
 *     against values worked out by hand and a brute-force check of the
 *     result, the NV04 two-stage calculator against a brute-force search
 *   - DisplayPort: link configurations for a data rate, mode bandwidth,
 *     the transfer unit / watermark against hand-computed values, link
 *     training against a simulated sink (fine, needs more swing, never
 *     locks, no sink), and the drive levels from the VBIOS table
 *   - VBIOS tables: IED output / settings / clock scripts, DP output and
 *     drive tables, and the init script interpreter: conditions, NOT,
 *     RESUME, sub-scripts, REPEAT, register writes with the head / OR /
 *     link selectors, a refused write outside the display registers, an
 *     unknown opcode, an endless JUMP, reads outside the image
 *   - the whole takeover and mode sets on the simulated engine (GP102,
 *     GM200, GF119, GK104, NV50, G94, GT215): the firmware's state read,
 *     the VBIOS ownership, the object table, the core channel, an update
 *     with three supervisor rounds (IED scripts, VPLL, SOR clock, DP link
 *     training and TU, GM200 routing), the armed state afterwards, the
 *     firmware's mode programmed again (restore) and every register saved
 *     at takeover put back; and every way the hardware can hang: a channel
 *     that never starts, never fetches, an update without notifier, a
 *     channel error, a VBIOS that never lets go, a dead card, a BAR flush
 *     that never ends — each ends in an error within the time limits.
 *
 *   evotest [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "core/lib.h"
#include "drivers/nv/evo.h"

const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }

static int checks, failed, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("evotest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 rs = 0x243F6A8885A308D3ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)rs; }

/* ======================================================================
 * a video BIOS with the display tables
 * ====================================================================== */

static u8 R[0x10000];
static u32 RS;
static void w8(u32 o, u32 v) { if (o < sizeof R) R[o] = (u8)v; if (o + 1 > RS) RS = o + 1; }
static void w16(u32 o, u32 v) { w8(o, v); w8(o + 1, v >> 8); }
static void w32(u32 o, u32 v) { w16(o, v); w16(o + 2, v >> 16); }
static void wstr(u32 o, const char *s) { while (*s) w8(o++, (u8)*s++); }

/* table addresses */
#define A_BIT   0x0100
#define A_DCB   0x0600
#define A_CCB   0x0700
#define A_CONN  0x0740
#define A_GPIO  0x0780
#define A_PLL   0x0800
#define A_IED   0x0A00
#define A_DP    0x0C00
#define A_INIT  0x0E00
#define A_SCR   0x1000   /* scripts */

/* script addresses used by the tests */
static u16 s_off1, s_off2, s_on2, s_on3, s_dp_before, s_dp_after, s_dp_lnk, s_cond_test, s_bad_opc, s_loop, s_refuse, s_sub,
    s_oob, s_dp_disable;

static nv_bios_t BIOS;
static int pll_ver;

static void build_rom(int pllv) {
    memset(R, 0, sizeof R);
    RS = 0;
    pll_ver = pllv;
    /* PC-AT image, 64 KiB, PCIR, last */
    w16(0, 0xAA55); w8(2, 0x10000 / 512); w16(0x18, 0x80);
    wstr(0x80, "PCIR"); w16(0x84, 0x10DE); w16(0x86, 0x1B81); w16(0x8A, 0x18); w8(0x8C, 3); w8(0x8F, 3);
    w16(0x90, 0x10000 / 512); w8(0x94, 0); w8(0x95, 0x80);
    /* BIT: i, C, U, d, I */
    static const u8 ids[] = { 'i', 'C', 'U', 'd', 'I' };
    int n = (int)sizeof ids;
    w16(A_BIT, 0xB8FF); wstr(A_BIT + 2, "BIT"); w16(A_BIT + 6, 0x0100); w8(A_BIT + 8, 12); w8(A_BIT + 9, 6); w8(A_BIT + 10, (u32)n);
    u32 data = A_BIT + 12 + (u32)n * 6 + 0x10;
    for (int i = 0; i < n; i++) {
        u32 e = A_BIT + 12 + (u32)i * 6;
        w8(e, ids[i]);
        w8(e + 1, ids[i] == 'i' ? 2 : 1);
        w16(e + 2, ids[i] == 'i' ? 0x10 : ids[i] == 'I' ? 0x12 : 0x10);
        w16(e + 4, data + (u32)i * 0x20);
    }
    u32 v = data;                                    /* 'i': 86.04.50.00.70 */
    w8(v + 0, 0x00); w8(v + 1, 0x50); w8(v + 2, 0x04); w8(v + 3, 0x86); w8(v + 4, 0x70);
    w16(data + 0x20 + 8, A_PLL);                     /* 'C' v1: the PLL table at +8 */
    w16(data + 0x40, A_IED);                         /* 'U' */
    w16(data + 0x60, A_DP);                          /* 'd' */
    w16(data + 0x80 + 0x06, A_INIT + 0x40);          /* 'I': condition table */
    w16(data + 0x80 + 0x00, A_INIT + 0x80);          /* script table */
    w16(data + 0x80 + 0x04, A_INIT + 0xA0);          /* macro table */
    u8 s = 0;
    for (int i = 0; i < 11; i++) s = (u8)(s + R[A_BIT + i]);
    w8(A_BIT + 11, (u8)(0x100 - s));

    /* DCB 4.0: DP on SOR 0 (macro 0), TMDS on SOR 1 (DVI-D, dual link possible), CRT on DAC 0, TMDS (HDMI) on SOR 2 */
    w16(0x36, A_DCB);
    static const struct { u8 type, i2c, conn, or_; u32 conf; } o[] = {
        { 6, 0, 0, 1, 0x04200010 },                  /* DP, 2.7 GHz, 4 lanes, link A */
        { 2, 1, 1, 2, 0x00000030 },                  /* TMDS, links A+B */
        { 0, 1, 1, 1, 0x00000000 },                  /* CRT on the same connector (DVI-I) */
        { 2, 2, 2, 4, 0x00020010 },                  /* TMDS, HDMI allowed, link A */
    };
    int no = 4;
    u32 hdr = 27;
    w8(A_DCB, 0x40); w8(A_DCB + 1, hdr); w8(A_DCB + 2, (u32)no + 1); w8(A_DCB + 3, 8);
    w32(A_DCB + 6, 0x4EDCBDCB);
    w16(A_DCB + 4, A_CCB); w16(A_DCB + 0x0A, A_GPIO); w16(A_DCB + 0x14, A_CONN);
    for (int i = 0; i < no; i++) {
        u32 e = A_DCB + hdr + (u32)i * 8;
        w32(e, o[i].type | (u32)o[i].i2c << 4 | 0xFu << 8 | (u32)o[i].conn << 12 | (u32)o[i].or_ << 24);
        w32(e + 4, o[i].conf);
    }
    w32(A_DCB + hdr + (u32)no * 8, 0x0000000E);
    /* CCB 4.0: entry 0 AUX channel 0 (hybrid pad 0), 1-2 I2C ports */
    w8(A_CCB, 0x40); w8(A_CCB + 1, 5); w8(A_CCB + 2, 3); w8(A_CCB + 3, 4); w8(A_CCB + 4, 0x10);
    w8(A_CCB + 5, 0); w8(A_CCB + 6, 0x01); w8(A_CCB + 8, 6);
    w8(A_CCB + 9, 1); w8(A_CCB + 12, 5);
    w8(A_CCB + 13, 2); w8(A_CCB + 16, 5);
    /* connectors 4.0: DP, DVI-I, HDMI */
    w8(A_CONN, 0x40); w8(A_CONN + 1, 5); w8(A_CONN + 2, 3); w8(A_CONN + 3, 4);
    w8(A_CONN + 5, 0x46); w8(A_CONN + 9, 0x30); w8(A_CONN + 13, 0x61);
    /* GPIO 4.0: none */
    w8(A_GPIO, 0x40); w8(A_GPIO + 1, 6); w8(A_GPIO + 2, 0); w8(A_GPIO + 3, 4);

    /* PLL limits: VPLL0 and VPLL1 */
    if (pllv == 0x40) {
        w8(A_PLL, 0x40); w8(A_PLL + 1, 4); w8(A_PLL + 2, 11); w8(A_PLL + 3, 2);
        for (int i = 0; i < 2; i++) {
            u32 e = A_PLL + 4 + (u32)i * 11;
            w8(e, 0x80 + (u32)i); w16(e + 1, A_PLL + 0x40); w32(e + 3, 0x00e800 + (u32)i * 0x40); w16(e + 9, 27);
        }
        u32 l = A_PLL + 0x40;                        /* VCO 1600-3200 MHz, input 13.5-38 MHz, M 1-255, N 8-255, P 1-63 */
        w16(l, 1600); w16(l + 2, 3200); w16(l + 4, 13); w16(l + 6, 38); w8(l + 8, 1); w8(l + 9, 255); w8(l + 10, 8); w8(l + 11, 255);
        w8(l + 12, 1); w8(l + 13, 63);
    } else if (pllv == 0x50) {
        w8(A_PLL, 0x50); w8(A_PLL + 1, 4); w8(A_PLL + 2, 19); w8(A_PLL + 3, 1);
        u32 e = A_PLL + 4;
        w8(e, 0x80); w16(e + 1, 27); w16(e + 5, 1600); w16(e + 7, 3200); w16(e + 9, 13); w16(e + 11, 38);
        w8(e + 13, 1); w8(e + 14, 255); w8(e + 15, 8); w8(e + 16, 255); w8(e + 17, 1); w8(e + 18, 63);
    } else {                                         /* 0x30: the NV50 two-stage VPLLs at 0x614100 / 0x614900 */
        w8(A_PLL, 0x30); w8(A_PLL + 1, 4); w8(A_PLL + 2, 7); w8(A_PLL + 3, 2);
        for (int i = 0; i < 2; i++) {
            u32 e = A_PLL + 4 + (u32)i * 7;
            w8(e, 0x80 + (u32)i); w16(e + 1, A_PLL + 0x40); w32(e + 3, 0x614100 + (u32)i * 0x800);
        }
        u32 l = A_PLL + 0x40;
        w16(l + 0, 100); w16(l + 2, 400); w16(l + 4, 400); w16(l + 6, 1000);   /* vco1 100-400, vco2 400-1000 MHz */
        w16(l + 8, 2); w16(l + 10, 50); w16(l + 12, 100); w16(l + 14, 200);    /* inputs: 2-100, 50-200 MHz */
        w8(l + 16, 1); w8(l + 17, 255); w8(l + 18, 1); w8(l + 19, 255);       /* N1, M1 */
        w8(l + 20, 1); w8(l + 21, 31); w8(l + 22, 1); w8(l + 23, 31);         /* N2, M2 */
        w8(l + 25, 6); w8(l + 27, 0); w32(l + 28, 27000);
    }

    /* scripts */
    u32 p = A_SCR;
#define OP_ZM_REG(r, v) do { w8(p, 0x7a); w32(p + 1, r); w32(p + 5, v); p += 9; } while (0)
#define OP_DONE() do { w8(p++, 0x71); } while (0)
    s_off1 = (u16)p; OP_ZM_REG(0x61c180, 0x0000a001); OP_DONE();
    s_off2 = (u16)p; OP_ZM_REG(0x61c180, 0x0000a002); OP_DONE();
    /* OnInt2: OR-relative register (0x40000000: + or * 0x800, 0x20000000: + link * 0x80), a refused write */
    s_on2 = (u16)p; OP_ZM_REG(0x6061c184, 0x0000b002); OP_ZM_REG(0x100000, 0xdead); OP_DONE();
    s_on3 = (u16)p; OP_ZM_REG(0x8061c188, 0x0000b003); OP_DONE();      /* head-relative */
    s_dp_before = (u16)p; OP_ZM_REG(0x61c190, 0x0000c000); OP_DONE();
    s_dp_after = (u16)p; OP_ZM_REG(0x61c194, 0x0000c001); OP_DONE();
    s_dp_lnk = (u16)p; OP_ZM_REG(0x61c198, 0x0000c0a0); OP_DONE();
    s_dp_disable = (u16)p; OP_ZM_REG(0x61c19c, 0x0000c0ff); OP_DONE();
    /* conditions: cond 0 is (R[0x61c1a0] & 0xff) == 0x5a, cond 1 never */
    s_sub = (u16)p; OP_ZM_REG(0x61c1b0, 0x5b5b5b5b); OP_DONE();
    s_cond_test = (u16)p;
    w8(p, 0x75); w8(p + 1, 0); p += 2;               /* CONDITION 0 (true) */
    OP_ZM_REG(0x61c1a4, 1);
    w8(p, 0x75); w8(p + 1, 1); p += 2;               /* CONDITION 1 (false): skipped from here */
    OP_ZM_REG(0x61c1a8, 2);
    w8(p, 0x38); p += 1;                             /* NOT: executing again */
    OP_ZM_REG(0x61c1ac, 3);
    w8(p, 0x38); p += 1;                             /* NOT: skipping */
    OP_ZM_REG(0x61c1a8, 4);
    w8(p, 0x72); p += 1;                             /* RESUME */
    w8(p, 0x5b); w16(p + 1, s_sub); p += 3;          /* SUB_DIRECT */
    w8(p, 0x33); w8(p + 1, 3); p += 2;               /* REPEAT 3 */
    w8(p, 0x97); w32(p + 1, 0x61c1b4); w32(p + 5, 0xFFFF0000); w32(p + 9, 1); p += 13;   /* ZM_MASK_ADD: += 1 in the low half */
    w8(p, 0x36); p += 1;                             /* END_REPEAT */
    w8(p, 0x6e); w32(p + 1, 0x61c1b8); w32(p + 5, 0xFFFFFF00); w32(p + 9, 0x77); p += 13;   /* NV_REG */
    w8(p, 0x58); w32(p + 1, 0x61c1c0); w8(p + 5, 2); w32(p + 6, 0x11); w32(p + 10, 0x22); p += 14;   /* ZM_REG_SEQUENCE */
    w8(p, 0x6f); w8(p + 1, 0); p += 2;               /* MACRO 0 */
    w8(p, 0x74); w16(p + 1, 50); p += 3;             /* TIME 50 us */
    w8(p, 0x6b); w8(p + 1, 0); p += 2;               /* SUB 0 (script table) */
    OP_DONE();
    s_bad_opc = (u16)p; OP_ZM_REG(0x61c1d0, 1); w8(p++, 0xee); OP_ZM_REG(0x61c1d4, 1); OP_DONE();
    s_loop = (u16)p; w8(p, 0x5c); w16(p + 1, (u16)p); p += 3;            /* JUMP to itself */
    s_refuse = (u16)p; OP_ZM_REG(0x000200, 0); OP_ZM_REG(0x009000, 1); OP_ZM_REG(0x61c1e0, 1); OP_DONE();
    s_oob = (u16)p; w8(p, 0x5c); w16(p + 1, 0xFFFE); p += 3;              /* JUMP to a ZM_REG whose operands are past the end */
    w8(0xFFFE, 0x7a);
    /* the condition table, script table, macro table */
    w32(A_INIT + 0x40, 0x61c1a0); w32(A_INIT + 0x44, 0xff); w32(A_INIT + 0x48, 0x5a);
    w32(A_INIT + 0x4c, 0x61c1a0); w32(A_INIT + 0x50, 0xff); w32(A_INIT + 0x54, 0x00);
    w16(A_INIT + 0x80, s_sub);
    w32(A_INIT + 0xA0, 0x61c1c8); w32(A_INIT + 0xA4, 0x0000beef);

    /* IED (disp) table 2.1: one entry per output, settings for protocols 1 (TMDS A), 5 (dual), 8 (DP A), 0 (DAC: CRT) */
    w8(A_IED, 0x21); w8(A_IED + 1, 6); w8(A_IED + 2, 2); w8(A_IED + 3, (u32)no); w8(A_IED + 4, 0x0c);
    u32 oe = A_IED + 0x40;
    for (int i = 0; i < no; i++, oe += 0x40) {
        w16(A_IED + 6 + (u32)i * 2, oe);
        nv_dcb_output_t d;
        memset(&d, 0, sizeof d);
        d.type = o[i].type; d.or_mask = o[i].or_; d.link = (o[i].conf >> 4) & 3; d.heads = 0xF;
        u32 hm = 0x0f00 | 0x00c0 | (o[i].or_ & 0x3F);
        w16(oe, nv_dcb_hasht(&d));
        w32(oe + 2, hm | 2u << 24);                  /* two settings entries (byte 5) */
        w16(oe + 6, 0); w16(oe + 8, s_off1); w16(oe + 0x0a, s_off2);   /* (script 0 unused), OffInt1, OffInt2 */
        static const u8 protos[4] = { 8, 1, 0, 1 };
        u32 se = oe + 0x0c;
        for (int k = 0; k < 2; k++, se += 6) {
            w8(se, k == 0 ? protos[i] : 5); w8(se + 1, k == 0 ? 0 : 1);
            w16(se + 2, oe + 0x20); w16(se + 4, oe + 0x30);
        }
        /* clock compare lists: >= 165 MHz: on2; else on2 too; then terminator */
        w16(oe + 0x20, 16500); w16(oe + 0x22, s_on2); w16(oe + 0x24, 0); w16(oe + 0x26, s_on2);
        w16(oe + 0x30, 0); w16(oe + 0x32, s_on3);
    }
    /* DP table 4.0: one output (DCB 0), 4 levels of swing x pre-emphasis */
    w8(A_DP, 0x40); w8(A_DP + 1, 8); w8(A_DP + 2, 2); w8(A_DP + 3, 1); w8(A_DP + 4, 0x10); w8(A_DP + 5, 1); w8(A_DP + 6, 4);
    w8(A_DP + 7, 10);
    w16(A_DP + 8, A_DP + 0x80);
    {
        nv_dcb_output_t d;
        memset(&d, 0, sizeof d);
        d.type = 6; d.or_mask = 1; d.link = 1; d.heads = 0xF;
        u32 x = A_DP + 0x80;
        w16(x, nv_dcb_hasht(&d)); w16(x + 2, nv_dcb_hashm(&d)); w8(x + 4, 0);
        w16(x + 5, s_dp_before); w16(x + 7, s_dp_after); w16(x + 9, A_DP + 0xC0);
        w16(x + 0x0b, 0); w16(x + 0x0d, 0); w16(x + 0x0f, s_dp_disable);
        /* lnkcmp: >= 0x14: lnk script; >= 0x06: lnk script */
        w8(A_DP + 0xC0, 0x14); w16(A_DP + 0xC1, s_dp_lnk); w8(A_DP + 0xC3, 0x06); w16(A_DP + 0xC4, s_dp_lnk);
        /* drive levels after the 1 output pointer: hdr 8 + 2 * 1 = 10 */
        for (int idx = 0; idx < 10; idx++) {
            u32 e = A_DP + 10 + (u32)idx * 4;
            w8(e, 0); w8(e + 1, 0x10 + idx); w8(e + 2, 0x20 + idx); w8(e + 3, 0x30 + idx);
        }
    }
    int score = nv_bios_parse(R, sizeof R, 0x134, &BIOS);
    if (!score || BIOS.noutputs != 4) printf("evotest: the test ROM did not parse (score %d, %d outputs, problems %x)\n", score, BIOS.noutputs,
                                            BIOS.problems);
}

/* ======================================================================
 * the simulated display engine
 * ====================================================================== */

#define VRAM_SIZE (32u << 20)
typedef struct sim {
    const nv_evo_gen_t *g;
    u16 chipset;
    int nr;
    u32 rk[8192], rv[8192];
    u64 t;
    u8 *vram;
    /* the core channel */
    int chan;                   /* 0 off, 1 starting, 2 running */
    int polls;
    u32 put, get;
    u32 asy[0x400], arm[0x400]; /* core methods / 4 */
    u32 changed;                /* heads */
    u32 pclk;                   /* heads whose clock changed */
    int stage;                  /* supervisor: 0 none, 1-3 waiting */
    u32 status;                 /* 0x6100ac / 0x610024 */
    int ntfy;                   /* SET_NOTIFIER_CONTROL as last written */
    int ntfy_upd;               /* ...as it was at the UPDATE being processed (the hardware latches it) */
    u32 ntfy_ctl, ntfy_handle;
    int vbios_owned;
    int flush;
    u32 err_stat;
    /* faults */
    int f_nostart, f_noget, f_nonotify, f_errmthd, f_vbios_stuck, f_dead, f_flush_stuck, f_nosuper;
    /* writes */
    int nw;
    u32 wr_r[16384], wr_v[16384];
    int resolve_fail;
    int updates;
} sim_t;

static sim_t S;

static u32 *slot(sim_t *s, u32 r) {
    for (int i = 0; i < s->nr; i++) if (s->rk[i] == r) return &s->rv[i];
    if (s->nr >= 8192) return NULL;
    s->rk[s->nr] = r;
    s->rv[s->nr] = 0;
    return &s->rv[s->nr++];
}
static u32 get_reg(sim_t *s, u32 r) { u32 *p = slot(s, r); return p ? *p : 0; }
static void set_reg(sim_t *s, u32 r, u32 v) { u32 *p = slot(s, r); if (p) *p = v; }

static int gf(sim_t *s) { return s->g->gf119; }
static u32 ctrl_reg(sim_t *s) { return gf(s) ? 0x610490 : 0x610200; }

/* NV50-family armed state addresses (nv50_disp_core_mthd_head, written out again) */
static const struct { u16 m; u32 a; } arm50[] = {
    { 0x0804, 0x610ad0 }, { 0x0808, 0x610a48 }, { 0x0810, 0x610ac0 }, { 0x0814, 0x610af8 }, { 0x0818, 0x610b00 },
    { 0x081c, 0x610ae8 }, { 0x0820, 0x610af0 }, { 0x0824, 0x610b08 }, { 0x0828, 0x610b10 }, { 0x082c, 0x610a68 },
    { 0x0860, 0x610a84 }, { 0x0868, 0x610b18 }, { 0x086c, 0x610b20 }, { 0x0870, 0x610ac8 }, { 0x0874, 0x610a38 },
};

static int head_of(sim_t *s, u32 m) {
    if (gf(s)) return m >= 0x400 && m < 0x400 + 4 * 0x300 ? (int)((m - 0x400) / 0x300) : -1;
    return m >= 0x800 && m < 0x1000 ? (int)((m - 0x800) / 0x400) : -1;
}
static u32 sor_mthd(sim_t *s, int id) { return gf(s) ? 0x200 + (u32)id * 0x20 : 0x600 + (u32)id * 0x40; }
static u32 dac_mthd(sim_t *s, int id) { return gf(s) ? 0x180 + (u32)id * 0x20 : 0x400 + (u32)id * 0x80; }

/* the object table as the hardware reads it */
static u32 vr32(u64 a) { return a + 4 <= VRAM_SIZE ? (u32)S.vram[a] | (u32)S.vram[a + 1] << 8 | (u32)S.vram[a + 2] << 16 | (u32)S.vram[a + 3] << 24 : 0; }
static void vw32(u64 a, u32 v) { if (a + 4 <= VRAM_SIZE) { S.vram[a] = (u8)v; S.vram[a + 1] = (u8)(v >> 8); S.vram[a + 2] = (u8)(v >> 16); S.vram[a + 3] = (u8)(v >> 24); } }

static int resolve(sim_t *s, u32 handle, u64 *start, u64 *limit) {
    u32 i10 = get_reg(s, 0x610010);
    if ((i10 & 0xFF) != 9) return 0;
    u64 inst = (u64)(i10 & ~0xFFu) << 8;
    /* nvkm_ramht_hash, 9 bits, channel 0 */
    u32 h = 0, x = handle;
    while (x) { h ^= x & 511; x >>= 9; }
    for (u32 n = 0; n < 512; n++) {
        u32 co = (h + n) % 512;
        u32 hd = vr32(inst + co * 8), ctx = vr32(inst + co * 8 + 4);
        if (hd != handle) continue;
        u32 obj = gf(s) ? (ctx >> 9) & 0xFFFF : (ctx >> 10) & 0xFFFF;
        if (gf(s) && !(ctx & 1)) return 0;
        u64 o = inst + obj;
        if (gf(s)) {
            if (vr32(o) != 0x49) return 0;
            *start = (u64)vr32(o + 4) << 8;
            *limit = ((u64)vr32(o + 8) << 8) | 0xFF;
        } else {
            if (vr32(o) != 0x0019003D || vr32(o + 0x14) != 0x00010000) return 0;
            *limit = vr32(o + 4) | (u64)(vr32(o + 0xc) >> 24) << 32;
            *start = vr32(o + 8) | (u64)(vr32(o + 0xc) & 0xFF) << 32;
        }
        return 1;
    }
    return 0;
}

static void sim_error(sim_t *s, u32 m, u32 d) {
    if (gf(s)) {
        set_reg(s, 0x61009c, 1);
        set_reg(s, s->g->push_gp102 ? 0x6111f0 : 0x6101f0, 0x80000000u | (5u << 12) | m);
        set_reg(s, (s->g->push_gp102 ? 0x6111f0 : 0x6101f0) + 4, d);
    } else {
        set_reg(s, 0x610020, 0x00010000);
        set_reg(s, 0x610080, (5u << 12) | m);
        set_reg(s, 0x610084, d);
    }
}

static void super_masks(sim_t *s) {
    if (gf(s)) {
        for (int h = 0; h < 4; h++)
            set_reg(s, 0x6101d4 + (u32)h * 0x800, (s->changed & (1u << h) ? 0x1000u : 0) | (s->pclk & (1u << h) ? 0x10000u : 0));
    } else {
        u32 m = 0;
        for (int h = 0; h < 2; h++) {
            if (s->changed & (1u << h)) m |= (0x80u | 0x20u) << h;
            if (s->pclk & (1u << h)) m |= 0x200u << h;
        }
        set_reg(s, 0x610030, m);
    }
}

static void super_next(sim_t *s) {
    if (s->stage == 0) return;
    if (s->stage < 3) {
        s->stage++;
        s->status |= gf(s) ? (1u << (s->stage - 1)) : (0x10u << (s->stage - 1));
        super_masks(s);
        return;
    }
    /* done: the assembly state is armed, the notifier written */
    memcpy(s->arm, s->asy, sizeof s->arm);
    s->stage = 0;
    s->changed = s->pclk = 0;
    s->updates++;
    if (s->ntfy_upd && !s->f_nonotify) {
        u64 st, lim;
        if (resolve(s, s->ntfy_handle, &st, &lim)) vw32(st + ((s->ntfy_ctl >> 2) & 0x3FF) * 4, 0x00000001);
        else s->resolve_fail++;
    }
}

static void apply(sim_t *s, u32 m, u32 d) {
    if (m >= 0x1000) { sim_error(s, m, d); return; }
    if ((int)m == s->f_errmthd) { sim_error(s, m, d); return; }
    u32 old = s->asy[m / 4];
    s->asy[m / 4] = d;
    int h = head_of(s, m);
    if (h >= 0 && old != d) {
        s->changed |= 1u << h;
        u32 clk = gf(s) ? 0x450 : 0x804, base = gf(s) ? 0x400 + (u32)h * 0x300 : 0x800 + (u32)h * 0x400;
        if (m == base + (clk - (gf(s) ? 0x400 : 0x800))) s->pclk |= 1u << h;
    }
    for (int i = 0; i < 8; i++)
        if (m == sor_mthd(s, i) || m == dac_mthd(s, i)) s->changed |= (old | d) & (gf(s) ? 0xF : 3);
    u32 iso = gf(s) ? 0x474 : 0x874, stride = gf(s) ? 0x300 : 0x400;
    if (m == 0x88 || (h >= 0 && m == iso + (u32)h * stride && d)) {
        u64 st, lim;
        if (!resolve(s, d, &st, &lim)) { s->resolve_fail++; sim_error(s, m, d); }
    }
    if (m == 0x84) s->ntfy = (int)(d >> 31);
    if (m == 0x80) { s->ntfy_upd = s->ntfy; s->ntfy_ctl = s->asy[0x84 / 4]; s->ntfy_handle = s->asy[0x88 / 4]; }
    if (m == 0x80 && !s->f_nosuper) {
        s->stage = 1;
        s->status |= gf(s) ? 1u : 0x10u;
        super_masks(s);
    }
}

static void process(sim_t *s) {
    if (s->chan != 2 || s->f_noget) return;
    u32 push = (get_reg(s, s->g->push_gp102 ? 0x611494 : gf(s) ? 0x610494 : 0x610204) & ~0xFFu) << 8;
    for (int guard = 0; s->get != s->put && guard < 4096; guard++) {
        u32 w = vr32(push + s->get);
        if ((w & 0xE0000000u) == 0x20000000u) { s->get = w & 0xFFC; continue; }
        u32 cnt = (w >> 18) & 0x7FF, m = w & 0xFFC;
        s->get = (s->get + 4) & 0xFFF;
        for (u32 i = 0; i < cnt; i++) {
            apply(s, m + i * 4, vr32(push + s->get));
            s->get = (s->get + 4) & 0xFFF;
        }
    }
}

static u32 s_rd(void *c, u32 r) {
    sim_t *s = c;
    s->t += 1;
    if (s->f_dead) return 0xFFFFFFFF;
    if (r == ctrl_reg(s)) {
        u32 v = get_reg(s, r);
        if (s->chan == 1) {
            if (!s->f_nostart && ++s->polls > 3) s->chan = 2;
            else return v | 0x80000000u;
        }
        return s->chan == 2 ? v | 0x00010000u : v & ~0x001e0000u;
    }
    if (r == 0x640004) return s->get;
    if (r == 0x640000) return s->put;
    if (r == (gf(s) ? 0x6100ac : 0x610024)) return s->status | (s->vbios_owned ? 0x100u : 0);
    if (r == 0x6194e8) return s->vbios_owned ? 3 : 0;
    if (r == 0x070000 || r == 0x00330c) { if (s->flush && !s->f_flush_stuck) s->flush--; return s->flush ? 2 : 0; }
    if (gf(s)) {
        if (r >= 0x640000 && r < 0x641000) return s->arm[(r - 0x640000) / 4];
        if (r >= 0x660000 && r < 0x661000) return s->asy[(r - 0x660000) / 4];
    } else {
        for (int h = 0; h < 2; h++)
            for (size_t i = 0; i < ARRAY_SIZE(arm50); i++) {
                u32 a = arm50[i].a + (u32)h * 0x540;
                if (r == a + 4) return s->arm[(arm50[i].m + (u32)h * 0x400) / 4];
                if (r == a) return s->asy[(arm50[i].m + (u32)h * 0x400) / 4];
            }
        u32 sb = s->g->sor_state == EVO_SOR_G94 ? 0x610794 : 0x610b70;
        for (int i = 0; i < 4; i++) {
            if (r == sb + (u32)i * 8 + 4) return s->arm[sor_mthd(s, i) / 4];
            if (r == sb + (u32)i * 8) return s->asy[sor_mthd(s, i) / 4];
            if (r == 0x610b58 + (u32)i * 8 + 4) return s->arm[dac_mthd(s, i) / 4];
            if (r == 0x610b58 + (u32)i * 8) return s->asy[dac_mthd(s, i) / 4];
        }
    }
    return get_reg(s, r);
}

static void s_wr(void *c, u32 r, u32 v) {
    sim_t *s = c;
    s->t += 1;
    if (s->nw < 16384) { s->wr_r[s->nw] = r; s->wr_v[s->nw] = v; s->nw++; }
    if (s->f_dead) return;
    if (r == ctrl_reg(s)) {
        set_reg(s, r, v);
        if (v == 0x01000013) { s->chan = 1; s->polls = 0; }
        else if (!(v & 3)) s->chan = 0;
        return;
    }
    if (r == 0x640000) { s->put = v & 0xFFC; process(s); return; }
    if (r == (gf(s) ? 0x6100ac : 0x610024)) {
        s->status &= ~(v & (gf(s) ? 7u : 0x70u));
        if (v & 0x100) set_reg(s, 0xEEEE00, 1);     /* (the VBIOS bit acknowledged) */
        return;
    }
    if (r == 0x6194e8) { if (!(v & 1) && !s->f_vbios_stuck) s->vbios_owned = 0; return; }
    if (r == (gf(s) ? 0x6101d0 : 0x610030)) { if (v == 0x80000000u) super_next(s); return; }
    if (r == 0x070000 || r == 0x00330c) { s->flush = 2; return; }
    /* the OR power sequencers finish at once (bit 31: busy) */
    if ((r >= 0x61a004 && r < 0x61e000 && (r & 0x7FF) == 0x004) || (r >= 0x61c034 && r < 0x620000 && (r & 0x7FF) == 0x034)) {
        set_reg(s, r, v & ~0x80000000u);
        return;
    }
    if (r == 0x61009c) { set_reg(s, r, get_reg(s, r) & ~v); return; }
    if (r == 0x610020) { set_reg(s, r, get_reg(s, r) & ~v); return; }
    set_reg(s, r, v);
}

static void s_delay(void *c, u32 us) { ((sim_t *)c)->t += us; }
static u64 s_now(void *c) { return ((sim_t *)c)->t; }
static nv_regs_t SREGS = { &S, s_rd, s_wr, s_delay, s_now };

static int wrote(u32 r, u32 v) {
    for (int i = 0; i < S.nw; i++) if (S.wr_r[i] == r && S.wr_v[i] == v) return 1;
    return 0;
}
static int wrote_reg(u32 r) {
    for (int i = 0; i < S.nw; i++) if (S.wr_r[i] == r) return 1;
    return 0;
}

/* ---------------- a DisplayPort sink on AUX ---------------- */

typedef struct sink {
    int present;
    u8 dpcd[0x800];
    int need_swing;             /* clock recovery needs at least this voltage swing */
    int never_eq;
    int writes;
} sink_t;
static sink_t K;

static void sink_init(int lanes, u8 bw) {
    memset(&K, 0, sizeof K);
    K.present = 1;
    K.dpcd[0] = 0x12; K.dpcd[1] = bw; K.dpcd[2] = (u8)(lanes | 0x80 | 0x40); K.dpcd[3] = 0x01; K.dpcd[0x0e] = 0;
    K.dpcd[0x600] = 2;                               /* asleep (D3) */
}

static void sink_status(void) {
    int tp = K.dpcd[0x102] & 0x0F, nr = K.dpcd[0x101] & 0x1F;
    u8 st[2] = { 0, 0 }, align = 0;
    int swing = K.dpcd[0x103] & 3;
    for (int l = 0; l < nr && l < 4; l++) {
        u8 v = 0;
        if (tp >= 1 && swing >= K.need_swing) v |= 1;                    /* CR */
        if (tp >= 2 && swing >= K.need_swing && !K.never_eq) v |= 6;      /* EQ, symbol lock */
        st[l >> 1] |= (u8)(v << ((l & 1) * 4));
    }
    if (tp >= 2 && swing >= K.need_swing && !K.never_eq) align = 1;
    K.dpcd[0x202] = st[0]; K.dpcd[0x203] = st[1]; K.dpcd[0x204] = align;
    /* ask for one more step of swing */
    u8 req = (u8)MIN(swing + 1, 3);
    K.dpcd[0x206] = (u8)(req | req << 4); K.dpcd[0x207] = (u8)(req | req << 4);
}

static int sink_aux(void *ctx, int ccb, u8 req, u32 addr, u8 *buf, u8 *len) {
    (void)ctx;
    S.t += 50;
    if (!K.present || ccb != 0) return E_NODEV;
    if (addr + *len > sizeof K.dpcd) return NV_AUX_NACK;
    if (req == NV_AUX_NATIVE_RD) { sink_status(); memcpy(buf, K.dpcd + addr, *len); return NV_AUX_ACK; }
    if (req == NV_AUX_NATIVE_WR) { memcpy(K.dpcd + addr, buf, *len); K.writes++; return NV_AUX_ACK; }
    return NV_AUX_NACK;
}

static char LOG[1 << 16];
static int loglen;
static void t_log(void *ctx, const char *line) {
    (void)ctx;
    if (verbose) printf("  | %s\n", line);
    int l = (int)strlen(line);
    if (loglen + l + 2 < (int)sizeof LOG) { memcpy(LOG + loglen, line, (size_t)l); loglen += l; LOG[loglen++] = '\n'; LOG[loglen] = 0; }
}
static int logged(const char *s) { return strstr(LOG, s) != NULL; }

/* ======================================================================
 * tests
 * ====================================================================== */

static void test_gens(void) {
    static const struct { u16 cs; int kind; u16 cls; int mthd; } t[] = {
        { 0x50, EVO_NV50, 0x507d, MTHD_507D }, { 0x84, EVO_G84, 0x827d, MTHD_827D }, { 0x92, EVO_G84, 0x827d, MTHD_827D },
        { 0x94, EVO_G94, 0x887d, MTHD_827D }, { 0x98, EVO_G94, 0x887d, MTHD_827D }, { 0xa0, EVO_GT200, 0x837d, MTHD_827D },
        { 0xa3, EVO_GT215, 0x857d, MTHD_827D }, { 0xa8, EVO_GT215, 0x857d, MTHD_827D }, { 0xaa, EVO_MCP77, 0x887d, MTHD_827D },
        { 0xaf, EVO_MCP89, 0x857d, MTHD_827D }, { 0xc0, EVO_GT215, 0x857d, MTHD_827D }, { 0xcf, EVO_GT215, 0x857d, MTHD_827D },
        { 0xd9, EVO_GF119, 0x907d, MTHD_907D }, { 0xe4, EVO_GK104, 0x917d, MTHD_917D }, { 0xf0, EVO_GK110, 0x927d, MTHD_917D },
        { 0x108, EVO_GK110, 0x927d, MTHD_917D }, { 0x117, EVO_GM107, 0x947d, MTHD_917D }, { 0x124, EVO_GM200, 0x957d, MTHD_917D },
        { 0x130, EVO_GP100, 0x977d, MTHD_917D }, { 0x134, EVO_GP102, 0x987d, MTHD_917D }, { 0x138, EVO_GP102, 0x987d, MTHD_917D },
    };
    for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
        const nv_evo_gen_t *g = nv_evo_gen_for(t[i].cs);
        CHECK(g && g->kind == t[i].kind && g->core_class == t[i].cls && g->mthd == t[i].mthd, "chipset %x", t[i].cs);
    }
    static const u16 none[] = { 0x04, 0x10, 0x4b, 0x63, 0x68, 0x140, 0x162, 0x170, 0x2a, 0x99 };
    for (size_t i = 0; i < ARRAY_SIZE(none); i++) CHECK(!nv_evo_gen_for(none[i]), "chipset %x has no EVO", none[i]);
    const nv_evo_gen_t *gc = nv_evo_gen_for(0xc4);
    CHECK(gc->pll == EVO_PLL_GF100 && !gc->gf119, "GF104: GT215 display with GF100 PLLs");
    CHECK(nv_evo_gen_for(0x134)->push_gp102 && !nv_evo_gen_for(0x130)->push_gp102 && nv_evo_gen_for(0x124)->route &&
          !nv_evo_gen_for(0x117)->route && !nv_evo_gen_for(0x134)->has_dac, "GP102 / GM200 specifics");
}

static void timing_1080(nv_evo_timing_t *t) {
    *t = (nv_evo_timing_t){ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 0, 0 };
}

static void test_raster(void) {
    nv_evo_timing_t t;
    nv_evo_raster_t r;
    timing_1080(&t);
    CHECK(!nv_evo_raster(&t, &r), "1080p");
    CHECK(r.htotal == 2200 && r.hsynce == 43 && r.hblanke == 191 && r.hblanks == 2111, "h %u %u %u %u", r.htotal, r.hsynce, r.hblanke, r.hblanks);
    CHECK(r.vtotal == 1125 && r.vsynce == 4 && r.vblanke == 40 && r.vblanks == 1120, "v %u %u %u %u", r.vtotal, r.vsynce, r.vblanke, r.vblanks);
    CHECK(r.blankus == 637 && r.vblank2s == 1 && r.vblank2e == 0, "blank %u", r.blankus);
    /* CVT-RB 2560x1440@60: 241.5 MHz 2560 2608 2640 2720 / 1440 1443 1448 1481, +h -v */
    nv_evo_timing_t c = { 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, 0, 1 };
    CHECK(!nv_evo_raster(&c, &r) && r.hsynce == 31 && r.hblanke == 111 && r.hblanks == 2671 && r.vsynce == 4 && r.vblanke == 37 &&
          r.vblanks == 1477 && r.nvsync == 1, "CVT-RB 1440p");
    nv_evo_timing_t bad = t;
    bad.hsync_end = bad.hsync_start;
    CHECK(nv_evo_raster(&bad, &r) == E_INVAL, "sync width 0");
    bad = t; bad.clock_khz = 0;
    CHECK(nv_evo_raster(&bad, &r) == E_INVAL, "clock 0");
    bad = t; bad.vtotal = bad.vsync_end;
    CHECK(nv_evo_raster(&bad, &r) == E_INVAL, "no back porch");
    bad = t; bad.htotal = 40000;
    CHECK(nv_evo_raster(&bad, &r) == E_INVAL, "too wide");
}

static int same(const nv_evo_stream_t *s, const u32 *want, int n, const char *what) {
    int ok = s->n == n && !s->overflow;
    for (int i = 0; ok && i < n; i++) if (s->w[i] != want[i]) { ok = 0; printf("evotest: %s word %d: %08x, want %08x\n", what, i, s->w[i], want[i]); }
    if (!ok && s->n != n) printf("evotest: %s: %d words, want %d\n", what, s->n, n);
    return ok;
}
#define H(m, c) (((u32)(c) << 18) | (m))

static void test_streams(void) {
    nv_evo_timing_t t;
    timing_1080(&t);
    nv_evo_head_cfg_t c;
    memset(&c, 0, sizeof c);
    nv_evo_raster(&t, &c.r);
    c.on = 1; c.w = 1920; c.h = 1080; c.offset = 0x400000; c.pitch = 7680; c.vram_handle = 0xF0000001; c.depth = EVO_DEPTH_24;
    nv_evo_stream_t s;

    /* GP102 (917d), head 0, DP A on SOR 0 (head907d_*, head917d_*, sor907d_ctrl, core507d_update) */
    c.head = 0; c.or_type = 0; c.or_id = 0; c.proto = EVO_PROTO_DP_A; c.or_heads = 1;
    memset(&s, 0, sizeof s);
    nv_evo_head_methods(nv_evo_gen_for(0x134), &s, &c);
    nv_evo_update(nv_evo_gen_for(0x134), &s, 1);
    static const u32 w917[] = {
        H(0x448, 1), 0, H(0x45c, 1), 0, H(0x480, 1), 0x05000000, H(0x48c, 1), 0,
        H(0x494, 1), 0, H(0x4b8, 1), 0x04380780, H(0x4c0, 3), 0x04380780, 0x04380780, 0x04380780,
        H(0x410, 6), 0, 0x04650898, 0x0004002B, 0x002800BF, 0x0460083F, 0x00000001, H(0x42c, 1), 0,
        H(0x450, 3), 148500000, 0x00200000, 148500000,
        H(0x460, 1), 0x4000, H(0x468, 4), 0x04380780, 0x01001E00, 0x0000CF00, 0xF0000001, H(0x4b0, 1), 0,
        H(0x4d0, 1), 0, H(0x4d4, 1), 0x100, H(0x4a0, 1), 0, H(0x498, 1), 0,
        H(0x404, 2), 0x00000140, 0x31EC6000,
        H(0x200, 1), 0x00000801,
        H(0x084, 1), 0x80000000, H(0x080, 2), 0, 0,
    };
    CHECK(same(&s, w917, (int)ARRAY_SIZE(w917), "917d"), "917d head 0 stream");

    /* GF119 (907d), head 2, TMDS B on SOR 3, negative syncs: dither at 0x490 */
    c.head = 2; c.or_id = 3; c.proto = EVO_PROTO_TMDS_B; c.or_heads = 4; c.depth = 0; c.r.nhsync = 1; c.r.nvsync = 1;
    memset(&s, 0, sizeof s);
    nv_evo_head_methods(nv_evo_gen_for(0xd9), &s, &c);
    const u32 o = 2 * 0x300;
    u32 w907[] = {
        H(0x448 + o, 1), 0, H(0x45c + o, 1), 0, H(0x480 + o, 1), 0x05000000, H(0x48c + o, 1), 0,
        H(0x494 + o, 1), 0, H(0x4b8 + o, 1), 0x04380780, H(0x4c0 + o, 3), 0x04380780, 0x04380780, 0x04380780,
        H(0x410 + o, 6), 0, 0x04650898, 0x0004002B, 0x002800BF, 0x0460083F, 0x00000001, H(0x42c + o, 1), 0,
        H(0x450 + o, 3), 148500000, 0x00200000, 148500000,
        H(0x460 + o, 1), 0x4000, H(0x468 + o, 4), 0x04380780, 0x01001E00, 0x0000CF00, 0xF0000001, H(0x4b0 + o, 1), 0,
        H(0x4d0 + o, 1), 0, H(0x4d4 + o, 1), 0x100, H(0x490 + o, 1), 0, H(0x498 + o, 1), 0,
        H(0x404 + o, 2), 0x00000018, 0x31EC6000 | 2u << 25,
        H(0x260, 1), 0x00000204,
    };
    CHECK(same(&s, w907, (int)ARRAY_SIZE(w907), "907d"), "907d head 2 stream");

    /* NV50 (507d), head 1, TMDS A on SOR 1, -h +v */
    c.head = 1; c.or_id = 1; c.proto = EVO_PROTO_TMDS_A; c.or_heads = 2; c.r.nhsync = 1; c.r.nvsync = 0;
    memset(&s, 0, sizeof s);
    nv_evo_head_methods(nv_evo_gen_for(0x50), &s, &c);
    const u32 p = 0x400;
    u32 w507[] = {
        H(0x840 + p, 1), 0, H(0x880 + p, 1), 0x05000000,
        H(0x8a4 + p, 1), 0, H(0x8c8 + p, 1), 0x04380780, H(0x8d8 + p, 2), 0x04380780, 0x04380780,
        H(0x804 + p, 2), 0x00800000 | 148500, 0,
        H(0x810 + p, 7), 0, 0x04650898, 0x0004002B, 0x002800BF, 0x0460083F, 0x00000001, 637, H(0x82c + p, 1), 0,
        H(0x860 + p, 1), 0x4000, H(0x868 + p, 4), 0x04380780, 0x00101E00, 0x0000CF00, 0xF0000001, H(0x8c0 + p, 1), 0,
        H(0x900 + p, 1), 0, H(0x904 + p, 1), 0x100, H(0x8a0 + p, 1), 0, H(0x8a8 + p, 1), 0,
        H(0x640, 1), 0x00001102,
    };
    CHECK(same(&s, w507, (int)ARRAY_SIZE(w507), "507d"), "507d head 1 stream");

    /* G94 (827d), head 0, DAC 0 (RGB CRT): the LUT and cursor context DMAs too, DAC polarity */
    c.head = 0; c.or_type = 1; c.or_id = 0; c.proto = EVO_PROTO_CRT; c.or_heads = 1; c.r.nhsync = 0; c.r.nvsync = 1;
    memset(&s, 0, sizeof s);
    nv_evo_head_methods(nv_evo_gen_for(0x94), &s, &c);
    u32 w827[] = {
        H(0x840, 1), 0, H(0x85c, 1), 0, H(0x880, 1), 0x05000000, H(0x89c, 1), 0,
        H(0x8a4, 1), 0, H(0x8c8, 1), 0x04380780, H(0x8d8, 2), 0x04380780, 0x04380780,
        H(0x804, 2), 0x00800000 | 148500, 0,
        H(0x810, 7), 0, 0x04650898, 0x0004002B, 0x002800BF, 0x0460083F, 0x00000001, 637, H(0x82c, 1), 0,
        H(0x860, 1), 0x4000, H(0x868, 4), 0x04380780, 0x00101E00, 0x0000CF00, 0xF0000001, H(0x8c0, 1), 0,
        H(0x900, 1), 0, H(0x904, 1), 0x100, H(0x8a0, 1), 0, H(0x8a8, 1), 0,
        H(0x400, 2), 0x00000001, 0x00000002,
    };
    CHECK(same(&s, w827, (int)ARRAY_SIZE(w827), "827d"), "827d head 0 DAC stream");

    /* head off (917d): clear LUT, cursor, surface; the OR without owners */
    memset(&c, 0, sizeof c);
    c.head = 1; c.or_type = 0; c.or_id = 2;
    memset(&s, 0, sizeof s);
    nv_evo_head_methods(nv_evo_gen_for(0x134), &s, &c);
    u32 woff[] = { H(0x748, 1), 0, H(0x75c, 1), 0, H(0x780, 1), 0x05000000, H(0x78c, 1), 0, H(0x774, 1), 0, H(0x7d0, 1), 0,
                   H(0x7d4, 1), 0x100, H(0x240, 1), 0 };
    CHECK(same(&s, woff, (int)ARRAY_SIZE(woff), "off"), "917d head 1 off");
    /* NV50 DAC off: control and polarity */
    memset(&s, 0, sizeof s);
    nv_evo_or_off(nv_evo_gen_for(0x50), &s, 1, 2);
    u32 wdac[] = { H(0x500, 2), 0, 0 };
    CHECK(same(&s, wdac, 3, "dac off"), "507d DAC 2 off");
    /* overflow is reported, never written past */
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 200; i++) nv_evo_mthd(&s, 0x80, 2, 0u, 0u);
    CHECK(s.overflow && s.n <= 512, "stream overflow");
}

static void test_ramht(void) {
    /* nvkm_ramht_hash for 512 entries, channel 0 */
    CHECK(nv_evo_ramht_hash(0xF0000000, 0) == ((0xF0000000u >> 27) ^ 0), "hash sync %x", nv_evo_ramht_hash(0xF0000000, 0));
    CHECK(nv_evo_ramht_hash(0xF0000001, 0) == (1u ^ (0xF0000001u >> 27)), "hash vram");
    CHECK(nv_evo_ramht_hash(0x12345678, 1) == (((0x12345678u & 511) ^ ((0x12345678u >> 9) & 511) ^ ((0x12345678u >> 18) & 511) ^ (0x12345678u >> 27)) ^ 32), "hash chid 1");
    static u8 inst[EVO_INST_BYTES];
    memset(inst, 0, sizeof inst);
    const nv_evo_gen_t *g = nv_evo_gen_for(0x134);
    CHECK(!nv_evo_dmaobj(g, inst, 0x1000, EVO_HANDLE_SYNC, 0x120000, 0x120FFF), "gf119 sync");
    CHECK(inst[0x1000] == 0x49 && inst[0x1004] == 0x00 && inst[0x1005] == 0x12 && inst[0x1008] == 0x0F && inst[0x1009] == 0x12, "gf119 object");
    u32 co = nv_evo_ramht_hash(EVO_HANDLE_SYNC, 0) * 8;
    u32 ctx = (u32)inst[co + 4] | (u32)inst[co + 5] << 8 | (u32)inst[co + 6] << 16 | (u32)inst[co + 7] << 24;
    CHECK(ctx == (0x1000u << 9 | 1), "gf119 context %08x", ctx);
    CHECK(nv_evo_dmaobj(g, inst, 0x800, 1, 0, 0) == E_INVAL && nv_evo_dmaobj(g, inst, 0x1004, 1, 0, 0) == E_INVAL, "bad object offsets");
    /* collisions: 600 handles with the same hash fill the table, then E_NOSPC */
    int full = 0;
    for (u32 i = 0; i < 600 && !full; i++) full = nv_evo_dmaobj(g, inst, 0x2000, 0x80000000u + (i << 18), 0, 0xFFF) == E_NOSPC;
    CHECK(full, "the table fills up");
    memset(inst, 0, sizeof inst);
    const nv_evo_gen_t *g50 = nv_evo_gen_for(0x50);
    CHECK(!nv_evo_dmaobj(g50, inst, 0x1020, EVO_HANDLE_VRAM, 0, 0x1FFFFFFFFull), "nv50 vram");
    u32 f0 = (u32)inst[0x1020] | (u32)inst[0x1021] << 8 | (u32)inst[0x1022] << 16 | (u32)inst[0x1023] << 24;
    CHECK(f0 == 0x0019003D && inst[0x1024] == 0xFF && inst[0x102F] == 0x01 && inst[0x1036] == 0x01, "nv50 object %08x", f0);
}

static void test_vram(void) {
    nv_evo_vram_t v;
    nv_evo_vram_init(&v, 0, 256u << 20);
    nv_evo_vram_hole(&v, 0, 2u << 20, "start");
    nv_evo_vram_hole(&v, 4u << 20, 8u << 20, "fw fb");
    u64 a = nv_evo_vram_alloc(&v, 2u << 20, 2u << 20);
    CHECK(a == (2u << 20), "first fit %llx", (unsigned long long)a);
    u64 b = nv_evo_vram_alloc(&v, 4u << 20, 2u << 20);
    CHECK(b == (12u << 20), "after the hole %llx", (unsigned long long)b);
    u64 c = nv_evo_vram_alloc(&v, 4096, 64 << 10);
    CHECK(c == (16u << 20), "aligned %llx", (unsigned long long)c);
    nv_evo_vram_free(&v, b);
    CHECK(nv_evo_vram_alloc(&v, 2u << 20, 2u << 20) == (12u << 20), "reuse");
    CHECK(nv_evo_vram_alloc(&v, 300u << 20, 2u << 20) == ~0ull, "too big");
    CHECK(nv_evo_vram_alloc(&v, 4096, 3) == ~0ull && nv_evo_vram_alloc(&v, 0, 4096) == ~0ull, "bad arguments");
    int n = 0;
    while (nv_evo_vram_alloc(&v, 1u << 20, 1u << 20) != ~0ull && n < 1000) n++;
    CHECK(n > 0 && n < 1000, "exhaustion after %d", n);
    /* random: allocations never overlap a hole or each other */
    for (int round = 0; round < 200; round++) {
        nv_evo_vram_init(&v, 1u << 20, 64u << 20);
        for (int i = 0; i < 4; i++) nv_evo_vram_hole(&v, (rnd() % 64) << 20, ((rnd() % 4) + 1) << 20, "x");
        u64 got[16];
        int k = 0;
        for (int i = 0; i < 16; i++) {
            u64 sz = ((u64)(rnd() % 6) + 1) << 19, al = 1ull << (12 + rnd() % 10);
            u64 at = nv_evo_vram_alloc(&v, sz, al);
            if (at == ~0ull) continue;
            CHECK(!(at & (al - 1)) && at >= v.lo && at + sz <= v.hi, "inside, aligned");
            for (int h = 0; h < v.nholes; h++) CHECK(at + sz <= v.hole[h].at || at >= v.hole[h].at + v.hole[h].size, "not in a hole");
            got[k++] = at;
        }
        for (int i = 0; i < v.nused; i++)
            for (int j = i + 1; j < v.nused; j++)
                CHECK(v.used[i].at + v.used[i].size <= v.used[j].at || v.used[j].at + v.used[j].size <= v.used[i].at, "no overlap");
        (void)got;
    }
}

static void test_pll(void) {
    nv_pll_limits_t p;
    int N, fN, M, P;
    build_rom(0x40);
    CHECK(!nv_pll_limits(&BIOS, 0x134, 0x80, 27000, &p) && p.reg == 0x00e800 && p.refclk == 27000 && p.vco1.max_freq == 3200000 &&
          p.max_p == 63 && p.vco1.min_n == 8, "4.0 limits");
    CHECK(!nv_pll_limits(&BIOS, 0x134, 0x81, 27000, &p) && p.reg == 0x00e840, "4.0 VPLL1");
    CHECK(nv_pll_limits(&BIOS, 0x134, 0x82, 27000, &p) == E_NOENT, "no VPLL2");
    /* 148.5 MHz: P = 3200 / 148.5 = 21, M = 1, N = 115.5 -> N 115, fN 0 (worked out by hand) */
    nv_pll_limits(&BIOS, 0x134, 0x80, 27000, &p);
    CHECK(nv_pll_calc_gt215(&p, 148500, &N, &fN, &M, &P) == 148500 && N == 115 && fN == 0 && M == 1 && P == 21,
          "148.5 MHz: N %d fN %04x M %d P %d", N, fN, M, P);
    /* 25.175 MHz: P = 63 (3200 / 25.175 = 127, capped), N*... = 25175 * 63 = 1586025 / 27000 = 58.74 */
    nv_pll_limits(&BIOS, 0x134, 0x80, 27000, &p);
    CHECK(nv_pll_calc_gt215(&p, 25175, &N, &fN, &M, &P) == 25175 && P == 63 && M == 1 && N == 58, "25.175: N %d fN %04x P %d", N, fN, P);
    /* every clock from 25 to 600 MHz: the fractional PLL lands within 0.01% */
    for (u32 khz = 25000; khz <= 600000; khz += 1337) {
        if (nv_pll_calc_gt215(&p, khz, &N, &fN, &M, &P) <= 0) { CHECK(0, "no PLL for %u", khz); continue; }
        double n = N + (double)((fN + 4096) & 0xFFFF) / 8192.0;
        if (((fN + 4096) & 0xFFFF) >= 0x8000) n -= 8.0;
        double out = 27000.0 * n / M / P;
        CHECK(out > khz * 0.9999 && out < khz * 1.0001 && P >= 1 && P <= 63 && N >= 8 && N <= 255, "%u kHz: %.1f", khz, out);
    }
    /* table 5.0: no register (left to the display engine) */
    build_rom(0x50);
    CHECK(!nv_pll_limits(&BIOS, 0x134, 0x80, 27000, &p) && p.reg == 0 && p.refclk == 27000 && p.max_p == 63, "5.0 limits");
    /* table 3.0 (NV50): the two-stage calculator against a brute-force search */
    build_rom(0x30);
    CHECK(!nv_pll_limits(&BIOS, 0x50, 0x80, 27000, &p) && p.reg == 0x614100 && p.vco2.max_freq == 1000000, "3.0 limits");
    for (u32 khz = 25000; khz <= 400000; khz += 9973) {
        int N1, M1, N2, M2, lp;
        int got = nv_pll_calc_nv04(&p, 0x50, khz, &N1, &M1, &N2, &M2, &lp);
        CHECK(got > 0, "two-stage %u", khz);
        if (got <= 0) continue;
        int c1 = 27000 * N1 / M1, c2 = c1 * N2 / M2, out = c2 >> lp;
        CHECK(out == got && c1 >= 100000 && c1 <= 400000 && c2 >= 400000 && (u32)abs(out - (int)khz) <= khz / 200 + 30,
              "two-stage %u: %d (c1 %d c2 %d)", khz, out, c1, c2);
    }
    build_rom(0x40);
}

static void test_dp_math(void) {
    int nr[8];
    u8 bw[8];
    /* 1080p at 24 bpp: 445500 kB/s; 4 lanes first, highest rate first */
    int n = nv_dp_configs(4, 0x14, 445500, nr, bw, 8);
    CHECK(n == 6 && nr[0] == 4 && bw[0] == 0x14 && nr[1] == 4 && bw[1] == 0x0a && nr[2] == 4 && bw[2] == 0x06 && nr[3] == 2 &&
          bw[3] == 0x14 && nr[4] == 2 && bw[4] == 0x0a && nr[5] == 1 && bw[5] == 0x14, "configs %d", n);
    n = nv_dp_configs(4, 0x0a, 2000000, nr, bw, 8);
    CHECK(n == 0, "4K60 does not fit 4 x 2.7");
    n = nv_dp_configs(3, 0x1e, 100, nr, bw, 8);
    CHECK(n == 8 && nr[0] == 2 && bw[0] == 0x1e, "3 lanes count as 2");
    CHECK(nv_dp_fits(4, 0x14, 533250, 24) && !nv_dp_fits(2, 0x0a, 241500, 24) && nv_dp_fits(4, 0x0a, 241500, 24) && !nv_dp_fits(0, 0x14, 1, 24),
          "fits");
    /* 1080p60 on 4 lanes at 2.7 GHz (nv50_disp_super_2_2_dp worked out by hand):
     * h = (191 + 2200 - 2111 - 7) * 270000 / 148500 - 3 - 3 = 490, v = (1120 - 40 - 25) * 270000 / 148500 - 13 = 1905,
     * ratio = (148500 * 24 / 8 / 4) * 100000 / 270000 = 41250, TU 64 (no activesym): watermark (100000-41250)*64*41250/1e10 + 6 = 21 */
    nv_evo_head_state_t h;
    memset(&h, 0, sizeof h);
    h.hz = 148500000; h.htotal = 2200; h.hblanke = 191; h.hblanks = 2111; h.vblanke = 40; h.vblanks = 1120; h.vtotal = 1125;
    nv_dp_tu_t tu;
    CHECK(!nv_dp_tu(&h, 4, 0x0a, 1, 24, 0, &tu) && tu.TU == 64 && tu.hsym == 490 && tu.vsym == 1905 && tu.watermark == 21,
          "TU: %d h %u v %u wm %u", tu.TU, tu.hsym, tu.vsym, tu.watermark);
    /* with activesym (G94): every result keeps valid symbols within a symbol of the data */
    CHECK(!nv_dp_tu(&h, 4, 0x0a, 1, 24, 1, &tu) && tu.TU >= 32 && tu.TU <= 64 && tu.VTUf >= 1 && tu.VTUf <= 15, "activesym TU %d", tu.TU);
    for (int i = 0; i < 2000; i++) {
        nv_evo_head_state_t r = h;
        r.hz = 25000000 + (rnd() % 500000) * 1000;
        int lanes = 1 << (rnd() % 3);
        u8 rate = (u8[]){ 0x06, 0x0a, 0x14 }[rnd() % 3];
        if (!nv_dp_fits(lanes, rate, r.hz / 1000, 24)) continue;
        int e = nv_dp_tu(&r, lanes, rate, 1, 24, 1, &tu);
        CHECK(!e && tu.TU >= 32 && tu.TU <= 64 && tu.watermark < 64 + 6, "random TU %u kHz x%d %02x", r.hz / 1000, lanes, rate);
    }
    CHECK(nv_dp_tu(&h, 1, 0x06, 1, 24, 1, &tu) == E_INVAL, "1080p does not fit 1 x 1.62");
}

static nv_evo_t E;

static void evo_setup(u16 chipset) {
    memset(&S, 0, sizeof S);
    static u8 *vram;
    if (!vram) vram = calloc(1, VRAM_SIZE);
    memset(vram, 0, VRAM_SIZE);
    S.vram = vram;
    S.chipset = chipset;
    S.g = nv_evo_gen_for(chipset);
    memset(&E, 0, sizeof E);
    E.g = S.g;
    E.r = &SREGS;
    E.bios = &BIOS;
    E.chipset = chipset;
    E.ops = (nv_evo_ops_t){ NULL, NULL, sink_aux, t_log, NULL, NULL, NULL };
    E.vram = vram;
    E.vram_base = 0;
    E.vram_size = VRAM_SIZE;
    E.vram_total = 8ull << 30;
    E.scripts = 1;
    E.inst = 16u << 20;
    E.push = E.inst + EVO_INST_BYTES;
    E.sync = E.push + EVO_PUSH_BYTES;
    loglen = 0;
    LOG[0] = 0;
    sink_init(4, 0x14);
}

/* the firmware: head 0 shows 1024x768 on (DP on SOR 0 for GF119+, TMDS on SOR 1 otherwise) */
static void fw_state(void) {
    sim_t *s = &S;
    s->vbios_owned = 1;
    if (gf(s)) {
        set_reg(s, 0x612004, 0x00000F0F | (s->g->has_dac ? 0x10 : 0));
        set_reg(s, 0x022448, 4);
        u32 o = 0x400;
        s->arm[(o + 0x14) / 4] = 806u << 16 | 1344;          /* raster: 1024x768@60 DMT */
        s->arm[(o + 0x18) / 4] = 5u << 16 | 135;
        s->arm[(o + 0x1c) / 4] = 34u << 16 | 295;
        s->arm[(o + 0x20) / 4] = 802u << 16 | 1319;
        s->arm[(o + 0x50) / 4] = 65000000;
        s->arm[(o + 0x04) / 4] = 5u << 6 | 3u << 3;
        s->arm[(o + 0x60) / 4] = 0x2000;                     /* surface at 2 MiB */
        s->arm[(o + 0x68) / 4] = 768u << 16 | 1024;
        s->arm[(o + 0x6c) / 4] = 0x01001000;
        s->arm[0x200 / 4] = 0x00000801;                       /* SOR 0: head 0, DP A */
        if (s->g->route) set_reg(s, 0x612308, 0x00000001);   /* macro 0 link A: SOR 0 */
    } else {
        set_reg(s, 0x610184, 0x03000000 | 0x00100000 | (s->g->sor_state == EVO_SOR_G94 ? 0x0c000000 : 0));
        s->arm[0x814 / 4] = 806u << 16 | 1344;
        s->arm[0x818 / 4] = 5u << 16 | 135;
        s->arm[0x81c / 4] = 34u << 16 | 295;
        s->arm[0x820 / 4] = 802u << 16 | 1319;
        s->arm[0x804 / 4] = 0x00800000 | 65000;
        s->arm[0x860 / 4] = 0x2000;
        s->arm[0x868 / 4] = 768u << 16 | 1024;
        s->arm[0x86c / 4] = 0x00101000;
        s->arm[0x640 / 4] = 0x00003101;                       /* SOR 1: head 0, TMDS A, -h -v */
    }
    memcpy(s->asy, s->arm, sizeof s->asy);
    /* a few registers takeover saves */
    set_reg(s, 0x000140, 0x00000001);
    set_reg(s, 0x610010, 0x00ABCD09);
}

static int set_1080(int head, int k, int or_id, u32 proto, nv_evo_dp_t *dp) {
    nv_evo_timing_t t;
    timing_1080(&t);
    nv_evo_head_cfg_t c;
    memset(&c, 0, sizeof c);
    nv_evo_raster(&t, &c.r);
    c.head = head; c.on = 1; c.w = 1920; c.h = 1080; c.offset = 8u << 20; c.pitch = 7680; c.vram_handle = EVO_HANDLE_VRAM;
    c.or_type = proto == EVO_PROTO_CRT; c.or_id = or_id; c.proto = proto; c.or_heads = 1u << head;
    c.depth = proto == EVO_PROTO_DP_A ? EVO_DEPTH_24 : 0;
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    if (head != E.fw.head && E.fw.valid) {
        nv_evo_head_cfg_t off;
        memset(&off, 0, sizeof off);
        off.head = E.fw.head; off.or_id = -1;
        nv_evo_head_methods(E.g, &s, &off);
    }
    if (E.fw.valid && (E.fw.or_id != or_id || E.fw.or_type != c.or_type)) nv_evo_or_off(E.g, &s, E.fw.or_type, E.fw.or_id);
    nv_evo_head_methods(E.g, &s, &c);
    nv_evo_update(E.g, &s, 1);
    if (c.or_type) E.dac_asy_outp[or_id] = k; else E.sor_asy_outp[or_id] = k;
    if (dp) E.dp[or_id] = dp;
    int r = nv_evo_submit(&E, &s, 1);
    E.dp[or_id] = NULL;
    return r;
}

static void test_takeover(u16 chipset) {
    char name[32];
    snprintf(name, sizeof name, "chipset %x", chipset);
    build_rom(chipset >= 0xa3 ? 0x40 : 0x30);
    evo_setup(chipset);
    fw_state();
    u64 t0 = S.t;
    int r = nv_evo_takeover(&E);
    CHECK(!r, "%s: takeover: %s", name, E.why);
    if (r) return;
    CHECK(S.t - t0 < 50000, "%s: takeover took %llu us simulated", name, (unsigned long long)(S.t - t0));
    CHECK(E.fw.valid && E.fw.head == 0 && E.fw.hs.hz == 65000000 && E.fw.hs.htotal == 1344 && E.fw.or_id == (gf(&S) ? 0 : 1), "%s: firmware found", name);
    CHECK(E.fw.dcb == (gf(&S) ? 0 : 1), "%s: firmware DCB output %d", name, E.fw.dcb);
    CHECK(!S.vbios_owned && get_reg(&S, 0x610010) == (u32)(E.inst >> 8 | 9), "%s: VBIOS let go, instance %08x", name, get_reg(&S, 0x610010));
    CHECK(gf(&S) ? get_reg(&S, 0x6100b0) == 0x307 : get_reg(&S, 0x61002c) == 0x370, "%s: supervisor enables", name);
    CHECK(chipset >= 0x130 ? wrote(0x000180, 0xFFFFFFFF) : get_reg(&S, 0x000140) == 0, "%s: GPU interrupts masked", name);
    CHECK(S.chan == 2 && E.chan_up && S.get == S.put && S.asy[0x88 / 4] == EVO_HANDLE_SYNC && !S.resolve_fail, "%s: channel running", name);
    u32 pr = S.g->push_gp102 ? 0x611494 : gf(&S) ? 0x610494 : 0x610204;
    CHECK(get_reg(&S, pr) == (u32)(1 | E.push >> 8), "%s: push buffer register", name);

    /* a 1080p mode: DP on SOR 0 (GF119+, with link training) or TMDS on SOR 1 */
    nv_evo_dp_t dp;
    memset(&dp, 0, sizeof dp);
    dp.dcb = 0; dp.ccb = 0;
    int isdp = gf(&S) || S.g->dp != EVO_DP_NONE;
    int orid = isdp ? (S.g->route ? 1 : 0) : 1;
    if (isdp) CHECK(!nv_dp_detect(&E, &dp) && dp.lanes_max == 4 && dp.bw_max == MIN(0x0a, S.g->dp_max_bw ? S.g->dp_max_bw : 0x0a) &&
                    dp.have_info, "%s: DP sink %d %02x info %d", name, dp.lanes_max, dp.bw_max, dp.have_info);
    int nw0 = S.nw;
    r = isdp ? set_1080(1, 0, orid, EVO_PROTO_DP_A, &dp) : set_1080(1, 1, orid, EVO_PROTO_TMDS_A, NULL);
    CHECK(!r, "%s: mode set: %s", name, E.why);
    CHECK(S.updates == 1 && E.supers == 3, "%s: three supervisor rounds (%u)", name, E.supers);
    u32 o1 = gf(&S) ? 0x400 + 0x300 : 0x800 + 0x400;
    CHECK(S.arm[(o1 + (gf(&S) ? 0x14 : 0x14)) / 4] == (1125u << 16 | 2200), "%s: head 1 armed raster %08x", name, S.arm[(o1 + 0x14) / 4]);
    CHECK(S.arm[(gf(&S) ? 0x474 : 0x874) / 4] == 0, "%s: firmware head 0 has no surface any more", name);
    /* the IED scripts ran with the OR / head selectors, the refused write did not happen */
    CHECK(wrote(0x61c184 + (u32)orid * 0x800, 0xb002), "%s: OnInt2 (OR-relative)", name);
    CHECK(wrote(0x61c188 + 0x800, 0xb003), "%s: OnInt3 (head-relative)", name);
    CHECK(!wrote_reg(0x100000) && E.script_refused >= 1, "%s: refused write", name);
    CHECK(wrote(0x61c180, 0xa001) && wrote(0x61c180, 0xa002), "%s: OffInt1, OffInt2 for the firmware's output", name);
    /* the video PLL of head 1 */
    if (S.g->pll == EVO_PLL_GF100) CHECK(wrote(0x00e840 + 0x04, (21u << 16) | (115u << 8) | 1u) && wrote(0x00e840 + 0x10, 0), "%s: VPLL1 (gf100)", name);
    else if (S.g->pll == EVO_PLL_GT215) CHECK(wrote(0x00e840 + 0x08, 0) && wrote(0x00e840, 0x50000610), "%s: VPLL1 (gt215)", name);
    else CHECK(wrote(0x614900, 0x10000611), "%s: VPLL1 (nv50)", name);
    if (isdp) {
        CHECK(dp.trained && dp.nr == 4 && dp.bw == dp.bw_max, "%s: DP trained %d x %02x (%s)", name, dp.nr, dp.bw, dp.why);
        CHECK(K.dpcd[0x600] == 1 && K.dpcd[0x100] == dp.bw && (K.dpcd[0x101] & 0x1F) == 4 && (K.dpcd[0x102] & 0x0F) == 0, "%s: sink programmed", name);
        CHECK(wrote(0x61c190, 0xc000) && wrote(0x61c194, 0xc001) && wrote(0x61c198, 0xc0a0), "%s: DP scripts", name);
        if (gf(&S)) CHECK(wrote_reg(0x616610 + 0x800) && wrote_reg(0x616620 + 0x800), "%s: DP watermark / symbols on head 1", name);
        else CHECK(wrote_reg(0x61c128 + (u32)orid * 0x800), "%s: DP TU (activesym)", name);
        CHECK(wrote_reg(0x61c10c + (u32)orid * 0x800) && wrote_reg(0x61c130 + (u32)orid * 0x800), "%s: DP lanes on SOR%d", name, orid);
    }
    if (S.g->route) CHECK(wrote(0x612308, 0) && wrote(0x612308, 2), "%s: SOR route moved from SOR0 to SOR1", name);
    (void)nw0;

    /* restore: the firmware's mode again, head 1 off */
    nv_evo_head_cfg_t c;
    CHECK(!nv_evo_fw_cfg(&E, 2u << 20, 4096, 1024, 768, &c) && c.head == 0 && c.r.htotal == 1344 && c.r.clock_khz == 65000 &&
          c.out_w == 1024 && c.out_h == 768, "%s: firmware configuration", name);
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_head_cfg_t off;
    memset(&off, 0, sizeof off);
    off.head = 1; off.or_id = orid == c.or_id ? -1 : orid;
    nv_evo_head_methods(E.g, &s, &off);
    if (off.or_id >= 0) E.sor_asy_outp[off.or_id] = -1;
    nv_evo_head_methods(E.g, &s, &c);
    nv_evo_update(E.g, &s, 1);
    if (c.or_type) E.dac_asy_outp[c.or_id] = E.fw.dcb; else E.sor_asy_outp[c.or_id] = E.fw.dcb;
    if (isdp) E.dp[c.or_id] = &dp;
    r = nv_evo_submit(&E, &s, 1);
    E.dp[c.or_id] = NULL;
    CHECK(!r, "%s: restore: %s", name, E.why);
    u32 o0 = gf(&S) ? 0x400 : 0x800;
    CHECK(S.arm[(o0 + 0x14) / 4] == (806u << 16 | 1344) && S.arm[(o0 + 0x20) / 4] == (802u << 16 | 1319) &&
          S.arm[(o0 + 0x60) / 4] == 0x2000, "%s: firmware raster and surface armed again", name);
    /* release: the saved registers back */
    nv_evo_release(&E);
    CHECK(gf(&S) ? get_reg(&S, 0x6100b0) == 0 : get_reg(&S, 0x61002c) == 0, "%s: interrupt enables back", name);
    CHECK(chipset >= 0x130 ? !wrote_reg(0x000160) : get_reg(&S, 0x000140) == 0, "%s: GPU interrupts stay masked", name);
    CHECK(get_reg(&S, 0x610010) == (u32)(E.inst >> 8 | 9), "%s: our instance stays while the channel runs", name);
    CHECK(S.t < 60000000, "%s: took %llu ms simulated", name, (unsigned long long)(S.t / 1000));
}

static void test_hangs(void) {
    build_rom(0x40);
    u64 t0;
    int r;
    /* a channel that never starts */
    evo_setup(0x134); fw_state(); S.f_nostart = 1;
    t0 = S.t;
    r = nv_evo_takeover(&E);
    CHECK(r == E_TIMEOUT && strstr(E.why, "did not start") && S.t - t0 < 3000000, "never starts: %d %s, %llu us", r, E.why,
          (unsigned long long)(S.t - t0));
    /* a channel that never fetches */
    evo_setup(0x134); fw_state(); S.f_noget = 1;
    CHECK(nv_evo_takeover(&E) == E_TIMEOUT && strstr(E.why, "fetch"), "never fetches: %s", E.why);
    /* an update whose notifier is never written */
    evo_setup(0x134); fw_state(); S.f_nonotify = 1;
    CHECK(!nv_evo_takeover(&E), "takeover");
    t0 = S.t;
    CHECK(set_1080(0, 3, 2, EVO_PROTO_TMDS_A, NULL) == E_TIMEOUT && S.t - t0 <= 3100000 && strstr(E.why, "did not complete"), "no notifier: %s", E.why);
    CHECK(E.chan_up && set_1080(0, 3, 2, EVO_PROTO_TMDS_A, NULL) == E_TIMEOUT && !E.chan_up && logged("given up"), "second timeout: given up");
    t0 = S.t;
    CHECK(set_1080(0, 3, 2, EVO_PROTO_TMDS_A, NULL) == E_IO && S.t - t0 < 1000, "a channel given up fails at once");
    /* no supervisor at all (and no notifier) */
    evo_setup(0x124); fw_state(); S.f_nosuper = 1; S.f_nonotify = 1;
    CHECK(!nv_evo_takeover(&E), "takeover");
    CHECK(set_1080(0, 3, 2, EVO_PROTO_TMDS_A, NULL) == E_TIMEOUT, "no supervisor");
    /* a channel error on a method */
    evo_setup(0xd9); fw_state(); S.f_errmthd = 0x0468;
    CHECK(!nv_evo_takeover(&E), "takeover");
    CHECK(set_1080(0, 3, 2, EVO_PROTO_TMDS_A, NULL) == E_IO && logged("core channel error") && get_reg(&S, 0x61009c) == 0, "channel error: %s", E.why);
    /* NV50 layout error */
    build_rom(0x30);
    evo_setup(0x50); fw_state(); S.f_errmthd = 0x0868;
    CHECK(!nv_evo_takeover(&E), "takeover nv50");
    CHECK(set_1080(0, 3, 1, EVO_PROTO_TMDS_A, NULL) == E_IO && get_reg(&S, 0x610020) == 0, "nv50 channel error: %s", E.why);
    build_rom(0x40);
    /* the VBIOS never lets go */
    evo_setup(0x134); fw_state(); S.f_vbios_stuck = 1;
    t0 = S.t;
    CHECK(nv_evo_takeover(&E) == E_TIMEOUT && strstr(E.why, "VBIOS") && S.t - t0 < 3000000, "VBIOS: %s", E.why);
    /* a dead card */
    evo_setup(0x134); fw_state(); S.f_dead = 1;
    t0 = S.t;
    CHECK(nv_evo_takeover(&E) == E_NODEV && S.t - t0 < 1000, "dead card: %s", E.why);
    /* the BAR flush never finishes */
    evo_setup(0x134); fw_state(); S.f_flush_stuck = 1;
    CHECK(nv_evo_takeover(&E) == E_TIMEOUT && strstr(E.why, "flush"), "BAR flush: %s", E.why);
    /* control structures outside the window */
    evo_setup(0x134); fw_state(); E.inst = VRAM_SIZE - 4096;
    CHECK(nv_evo_takeover(&E) == E_INVAL, "layout outside VRAM");
    /* submit without a channel */
    evo_setup(0x134);
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_update(E.g, &s, 1);
    CHECK(nv_evo_submit(&E, &s, 1) == E_IO, "no channel");
    /* a stream too long for the push buffer is refused, never written */
    evo_setup(0x134); fw_state();
    CHECK(!nv_evo_takeover(&E), "takeover");
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 160; i++) nv_evo_mthd(&s, 0x0410, 2, 0u, 0u);
    CHECK(nv_evo_submit(&E, &s, 0) == E_INVAL, "long stream");
    /* the push buffer wraps: 40 updates of ~70 words each */
    int ok = 1;
    for (int i = 0; i < 40 && ok; i++) ok = !set_1080(i & 1, 3, 2, EVO_PROTO_TMDS_A, NULL);
    CHECK(ok && S.updates == 40, "40 updates with wraps (%d): %s", S.updates, E.why);
}

static void test_dp_training(void) {
    build_rom(0x40);
    nv_evo_dp_t dp;
    /* the sink wants voltage swing 2: two rounds of adjust requests */
    evo_setup(0x134); fw_state();
    CHECK(!nv_evo_takeover(&E), "takeover");
    K.need_swing = 2;
    memset(&dp, 0, sizeof dp);
    CHECK(!nv_dp_detect(&E, &dp), "detect");
    CHECK(!set_1080(0, 0, 0, EVO_PROTO_DP_A, &dp) && dp.trained && (K.dpcd[0x103] & 3) >= 2, "swing 2 (%s)", dp.why);
    /* the drive levels came from the VBIOS table: swing 2, pre-emphasis 0 -> index 7: dc 0x17, pe 0x27, pu 0x37 */
    CHECK(wrote_reg(0x61c118) && ((get_reg(&S, 0x61c118) & 0xFF) == 0x17) && ((get_reg(&S, 0x61c120) & 0xFF) == 0x27), "drive %08x %08x",
          get_reg(&S, 0x61c118), get_reg(&S, 0x61c120));
    /* never locks: every configuration is tried, then the mode set fails */
    evo_setup(0x134); fw_state();
    CHECK(!nv_evo_takeover(&E), "takeover");
    K.never_eq = 1;
    memset(&dp, 0, sizeof dp);
    nv_dp_detect(&E, &dp);
    set_1080(0, 0, 0, EVO_PROTO_DP_A, &dp);
    CHECK(!dp.trained && strstr(dp.why, "training failed") && E.errors >= 1, "never locks: %s", dp.why);
    /* no sink */
    evo_setup(0x134);
    K.present = 0;
    memset(&dp, 0, sizeof dp);
    CHECK(nv_dp_detect(&E, &dp) == E_NODEV && strstr(dp.why, "no DisplayPort sink"), "no sink");
    /* a sink that wants more than the lanes give */
    evo_setup(0x134); sink_init(1, 0x06);
    memset(&dp, 0, sizeof dp);
    CHECK(!nv_dp_detect(&E, &dp) && dp.lanes_max == 1 && dp.bw_max == 0x06, "small sink");
    CHECK(!nv_dp_fits(dp.lanes_max, dp.bw_max, 148500, 24), "1080p does not fit");
    /* DCB 2.7 GHz caps a 5.4 GHz sink */
    evo_setup(0x134); sink_init(4, 0x14);
    memset(&dp, 0, sizeof dp);
    CHECK(!nv_dp_detect(&E, &dp) && dp.bw_max == 0x0a, "DCB caps the rate");
}

static u64 run_script(u16 at, nv_init_ctx_t c, int *ret) {
    int nw = S.nw;
    *ret = nv_evo_script(&E, at, &c, "test");
    return (u64)(S.nw - nw);
}

static void test_scripts(void) {
    build_rom(0x40);
    evo_setup(0x134);
    int r;
    nv_init_ctx_t c = { 0, 2, 2, 1 };
    set_reg(&S, 0x61c1a0, 0x5a);
    set_reg(&S, 0x61c1b4, 0x1234FFFF);
    run_script(s_cond_test, c, &r);
    CHECK(!r, "script ran");
    CHECK(wrote(0x61c1a4, 1) && !wrote_reg(0x61c1a8) && wrote(0x61c1ac, 3), "conditions and NOT");
    CHECK(wrote(0x61c1b0, 0x5b5b5b5b), "SUB_DIRECT / SUB");
    CHECK(get_reg(&S, 0x61c1b4) == 0x12340002, "REPEAT x ZM_MASK_ADD: %08x", get_reg(&S, 0x61c1b4));
    CHECK(get_reg(&S, 0x61c1b8) == 0x77 && wrote(0x61c1c0, 0x11) && wrote(0x61c1c4, 0x22) && wrote(0x61c1c8, 0xbeef), "NV_REG, sequence, macro");
    /* OR / link / head selectors */
    run_script(s_on2, c, &r);
    CHECK(wrote(0x61c184 + 2 * 0x800 + 0x80, 0xb002), "OR 2 link B");
    run_script(s_on3, c, &r);
    CHECK(wrote(0x61c188 + 0x800, 0xb003), "head 1");
    nv_init_ctx_t nohead = { 0, -1, 0, -1 };
    run_script(s_on3, nohead, &r);
    CHECK(r == E_IO && logged("needs a head"), "head missing");
    /* refused: PMC and the timer */
    u32 ref = E.script_refused;
    run_script(s_refuse, c, &r);
    CHECK(!r && E.script_refused == ref + 2 && !wrote_reg(0x000200) && !wrote_reg(0x009000) && wrote(0x61c1e0, 1), "refused writes");
    /* an unknown opcode stops the script after what came before */
    run_script(s_bad_opc, c, &r);
    CHECK(r == E_IO && wrote(0x61c1d0, 1) && !wrote_reg(0x61c1d4) && logged("unknown opcode ee"), "unknown opcode");
    /* an endless loop ends */
    u64 t0 = S.t;
    run_script(s_loop, c, &r);
    CHECK(r == E_IO && logged("more than 4096") && S.t - t0 < 100000, "endless JUMP");
    /* outside the image */
    run_script(s_oob, c, &r);
    CHECK(r == E_IO && logged("outside the image"), "out of bounds");
    /* nv.scripts=0 */
    E.scripts = 0;
    run_script(s_on2, c, &r);
    CHECK(!r && logged("not run (nv.scripts=0)"), "scripts off");
    E.scripts = 1;
    /* random bytes as a script: always ends, never writes outside the display registers */
    for (int i = 0; i < 3000; i++) {
        u32 at = 0x4000 + (rnd() % 0x3000);
        for (int k = 0; k < 64; k++) R[at + (u32)k] = (u8)rnd();
        R[at] = (u8)(0x32 + rnd() % 0x78);
        nv_bios_parse(R, sizeof R, 0x134, &BIOS);
        int nw = S.nw;
        nv_evo_script(&E, (u16)at, &c, "random");
        for (int k = nw; k < S.nw; k++) CHECK(nv_evo_script_reg_ok(S.wr_r[k]) || S.wr_r[k] == 0x61c1a0, "random script wrote %06x", S.wr_r[k]);
        if (S.nw > 12000) S.nw = 0;
    }
    build_rom(0x40);
    /* the IED and DP tables */
    nv_ied_t ied;
    const nv_dcb_output_t *o = &BIOS.out[0];
    u16 m = (u16)(0x0100 | 1 << 6 | 1);
    CHECK(nv_ied_match(&BIOS, nv_dcb_hasht(o), m, &ied) && ied.script[1] == s_off1 && ied.script[2] == s_off2 && ied.cnt == 2, "IED entry");
    u16 clk[2];
    CHECK(nv_ied_ocfg(&BIOS, &ied, 8, 0, clk) && nv_ied_oclk(&BIOS, clk[0], 148500) == s_on2 && nv_ied_oclk(&BIOS, clk[1], 25000) == s_on3,
          "IED settings / clock");
    CHECK(!nv_ied_ocfg(&BIOS, &ied, 9, 0, clk) && nv_ied_ocfg(&BIOS, &ied, 5, 1, clk), "IED protocol 9 absent, dual link present");
    CHECK(!nv_ied_match(&BIOS, nv_dcb_hasht(o), (u16)(0x1000 | m), &ied), "IED head mask");
    nv_dpout_t dp;
    CHECK(nv_dpout_match(&BIOS, nv_dcb_hasht(o), nv_dcb_hashm(o), &dp) && dp.script[0] == s_dp_before && dp.script[4] == s_dp_disable, "DP entry");
    nv_dpcfg_t cfg;
    CHECK(nv_dpcfg_match(&BIOS, &dp, 0, 1, 2, &cfg) && cfg.dc == 0x16 && cfg.pe == 0x26 && cfg.tx_pu == 0x36, "DP level vs 1 pe 2 (idx 6)");
    CHECK(nv_dpcfg_match(&BIOS, &dp, 0, 3, 0, &cfg) && cfg.dc == 0x19, "DP level vs 3 (idx 9)");
    CHECK(!nv_dpcfg_match(&BIOS, &dp, 1, 0, 0, &cfg) && !nv_dpcfg_match(&BIOS, &dp, 0, 4, 0, &cfg), "DP level out of the table");
    CHECK(nv_dp_lnkcmp(&BIOS, &dp, 0x14) == s_dp_lnk && nv_dp_lnkcmp(&BIOS, &dp, 0x0a) == s_dp_lnk, "lnkcmp");
    /* EDID helpers */
    u8 ed[256];
    memset(ed, 0, sizeof ed);
    ed[20] = 0x80;
    ed[128] = 0x02; ed[130] = 20; ed[132] = 3 << 5 | 5; ed[133] = 0x03; ed[134] = 0x0C; ed[135] = 0x00;
    CHECK(nv_edid_digital(ed, 256) && nv_edid_hdmi(ed, 256) && !nv_edid_hdmi(ed, 128), "HDMI sink");
    ed[133] = 0x04;
    CHECK(!nv_edid_hdmi(ed, 256), "not HDMI");
    for (int i = 0; i < 20000; i++) {
        for (int k = 128; k < 256; k++) ed[k] = (u8)rnd();
        ed[128] = 2;
        (void)nv_edid_hdmi(ed, 256);
    }
    CHECK(1, "random CEA blocks");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    build_rom(0x40);
    test_gens();
    test_raster();
    test_streams();
    test_ramht();
    test_vram();
    test_pll();
    test_dp_math();
    static const u16 chips[] = { 0x134, 0x130, 0x124, 0x117, 0xe4, 0xd9, 0xc4, 0xa3, 0x94, 0x84, 0x50 };
    for (size_t i = 0; i < ARRAY_SIZE(chips); i++) test_takeover(chips[i]);
    test_hangs();
    test_dp_training();
    test_scripts();
    if (failed) printf("evotest: %d of %d checks FAILED\n", failed, checks);
    else printf("evotest: all %d checks passed (generations, method streams of 507d / 827d / 907d / 917d, rasters, object table, VRAM, "
                "PLLs, DisplayPort, VBIOS tables and scripts, takeover / mode set / restore on %u simulated chips, hangs)\n", checks,
                (unsigned)ARRAY_SIZE(chips));
    return failed != 0;
}
