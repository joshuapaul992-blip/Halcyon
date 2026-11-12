/* Halcyon OS - NVIDIA GPUs (M10b): the video PLLs (pixel clocks) of the
 * EVO display engine. The limits come from the video BIOS (BIT 'C', the
 * PLL limits table), the calculators and the register sequences are
 * nouveau's (MIT, Copyright 2012 Red Hat Inc.; THIRD_PARTY_NOTICES.md):
 * nvkm/subdev/bios/pll.c, nvkm/subdev/clk/pllnv04.c and pllgt215.c,
 * nvkm/subdev/devinit/nv50.c, gt215.c and gf100.c (pll_set). */
#include "evo.h"
#include "core/lib.h"

static u8 r8(const nv_bios_t *b, u32 a) { return nv_rom_rd8(&b->rom, a, NULL); }
static u16 r16(const nv_bios_t *b, u32 a) { return nv_rom_rd16(&b->rom, a, NULL); }
static u32 r32(const nv_bios_t *b, u32 a) { return nv_rom_rd32(&b->rom, a, NULL); }

/* the fixed maps of the oldest tables (pll.c: nv50_pll_mapping, g84_pll_mapping) */
static const struct { u8 type; u32 reg; } map_nv50[] = {
    { 0x01, 0x004028 }, { 0x02, 0x004020 }, { 0x03, 0x004000 }, { 0x04, 0x004008 }, { 0x40, 0x00e810 },
    { 0x41, 0x00e818 }, { 0x42, 0x00e824 }, { 0x80, 0x614100 }, { 0x81, 0x614900 },
};
static const struct { u8 type; u32 reg; } map_g84[] = {
    { 0x01, 0x004028 }, { 0x02, 0x004020 }, { 0x04, 0x004008 }, { 0x05, 0x004030 }, { 0x41, 0x00e818 },
    { 0x80, 0x614100 }, { 0x81, 0x614900 },
};

static u32 limits_table(const nv_bios_t *b, u8 *ver, u8 *hdr, u8 *cnt, u8 *len) {
    const nv_bit_token_t *t = nv_bios_token(b, 'C');
    u32 data = 0;
    if (t) {
        if (t->ver == 1 && t->size >= 10) data = r16(b, (u32)t->ptr + 8);
        if (t->ver == 2 && t->size >= 4) data = r32(b, t->ptr);
    }
    if (!data) { *ver = 0; return 0; }
    *ver = r8(b, data);
    *hdr = r8(b, data + 1);
    *len = r8(b, data + 2);
    *cnt = r8(b, data + 3);
    return data;
}

int nv_pll_limits(const nv_bios_t *b, u16 chipset, u8 type, u32 crystal, nv_pll_limits_t *p) {
    memset(p, 0, sizeof *p);
    u8 ver = 0, hdr = 0, cnt = 0, len = 0;
    u32 data = b ? limits_table(b, &ver, &hdr, &cnt, &len) : 0;
    u32 reg = 0, entry = 0;
    if (!data) return E_NOENT;
    if (ver >= 0x30) {
        u32 at = data + hdr;
        for (int i = 0; i < cnt && i < 64; i++, at += len) {
            if (r8(b, at) != type) continue;
            reg = ver < 0x50 ? r32(b, at + 3) : 0;
            entry = at;
            break;
        }
        if (!entry) return E_NOENT;
    } else {
        int found = 0;
        if (chipset == 0x50) {
            for (size_t i = 0; i < ARRAY_SIZE(map_nv50); i++) if (map_nv50[i].type == type) { reg = map_nv50[i].reg; found = 1; }
        } else if (chipset < 0xa3 || chipset == 0xaa || chipset == 0xac) {
            for (size_t i = 0; i < ARRAY_SIZE(map_g84); i++) if (map_g84[i].type == type) { reg = map_g84[i].reg; found = 1; }
        }
        if (!found) return E_NOENT;
        if (ver >= 0x20) {
            entry = data + hdr;
            for (u32 at = data + hdr, i = 0; i < cnt && i < 64; i++, at += len)
                if (r32(b, at) == reg) { entry = at; break; }
        } else {
            entry = data + 1;
        }
    }
    p->ver = ver;
    p->type = type;
    p->reg = reg;
    u32 d = entry;
    switch (ver) {
    case 0x20: case 0x21:
        p->vco1.min_freq = r16(b, d + 4) * 1000u;
        p->vco1.max_freq = r16(b, d + 6) * 1000u;
        p->vco2.min_freq = r16(b, d + 8) * 1000u;
        p->vco2.max_freq = r16(b, d + 10) * 1000u;
        p->vco1.min_inputfreq = r16(b, d + 12) * 1000u;
        p->vco2.min_inputfreq = r16(b, d + 14) * 1000u;
        p->vco1.max_inputfreq = r16(b, d + 16) * 1000u;
        p->vco2.max_inputfreq = r16(b, d + 18) * 1000u;
        p->vco1.min_n = r8(b, d + 20); p->vco1.max_n = r8(b, d + 21);
        p->vco1.min_m = r8(b, d + 22); p->vco1.max_m = r8(b, d + 23);
        p->vco2.min_n = r8(b, d + 24); p->vco2.max_n = r8(b, d + 25);
        p->vco2.min_m = r8(b, d + 26); p->vco2.max_m = r8(b, d + 27);
        p->max_p = r8(b, d + 29);
        p->max_p_usable = b->version[1] < 0x60 ? 6 : p->max_p;
        p->bias_p = r8(b, d + 30);
        if (len > 0x22) p->refclk = r32(b, d + 31);
        break;
    case 0x30:
        d = r16(b, d + 1);
        p->vco1.min_freq = r16(b, d + 0) * 1000u;
        p->vco1.max_freq = r16(b, d + 2) * 1000u;
        p->vco2.min_freq = r16(b, d + 4) * 1000u;
        p->vco2.max_freq = r16(b, d + 6) * 1000u;
        p->vco1.min_inputfreq = r16(b, d + 8) * 1000u;
        p->vco2.min_inputfreq = r16(b, d + 10) * 1000u;
        p->vco1.max_inputfreq = r16(b, d + 12) * 1000u;
        p->vco2.max_inputfreq = r16(b, d + 14) * 1000u;
        p->vco1.min_n = r8(b, d + 16); p->vco1.max_n = r8(b, d + 17);
        p->vco1.min_m = r8(b, d + 18); p->vco1.max_m = r8(b, d + 19);
        p->vco2.min_n = r8(b, d + 20); p->vco2.max_n = r8(b, d + 21);
        p->vco2.min_m = r8(b, d + 22); p->vco2.max_m = r8(b, d + 23);
        p->max_p_usable = p->max_p = r8(b, d + 25);
        p->bias_p = r8(b, d + 27);
        p->refclk = r32(b, d + 28);
        break;
    case 0x40:
        p->refclk = r16(b, d + 9) * 1000u;
        d = r16(b, d + 1);
        p->vco1.min_freq = r16(b, d + 0) * 1000u;
        p->vco1.max_freq = r16(b, d + 2) * 1000u;
        p->vco1.min_inputfreq = r16(b, d + 4) * 1000u;
        p->vco1.max_inputfreq = r16(b, d + 6) * 1000u;
        p->vco1.min_m = r8(b, d + 8); p->vco1.max_m = r8(b, d + 9);
        p->vco1.min_n = r8(b, d + 10); p->vco1.max_n = r8(b, d + 11);
        p->min_p = r8(b, d + 12);
        p->max_p = r8(b, d + 13);
        break;
    case 0x50:
        p->refclk = r16(b, d + 1) * 1000u;
        p->vco1.min_freq = r16(b, d + 5) * 1000u;
        p->vco1.max_freq = r16(b, d + 7) * 1000u;
        p->vco1.min_inputfreq = r16(b, d + 9) * 1000u;
        p->vco1.max_inputfreq = r16(b, d + 11) * 1000u;
        p->vco1.min_m = r8(b, d + 13); p->vco1.max_m = r8(b, d + 14);
        p->vco1.min_n = r8(b, d + 15); p->vco1.max_n = r8(b, d + 16);
        p->min_p = r8(b, d + 17);
        p->max_p = r8(b, d + 18);
        break;
    default:
        return E_NOTSUP;
    }
    if (!p->refclk) p->refclk = crystal;
    if (!p->vco1.max_freq || !p->refclk) return E_NOTSUP;      /* (pre-limit-table BIOSes: not EVO chips) */
    return 0;
}

/* gt215_pll_calc with fractional N (the only way nouveau calls it) */
int nv_pll_calc_gt215(const nv_pll_limits_t *p, u32 freq, int *pN, int *pfN, int *pM, int *pP) {
    if (!freq || !p->refclk || !p->vco1.max_inputfreq || !p->vco1.min_inputfreq) return E_INVAL;
    i64 P = p->vco1.max_freq / freq;
    if (P > p->max_p) P = p->max_p;
    if (P < p->min_p) P = p->min_p;
    if (P < 1) P = 1;
    i64 ref = p->refclk;
    i64 lM = (ref + p->vco1.max_inputfreq) / p->vco1.max_inputfreq;
    lM = MAX(lM, (i64)p->vco1.min_m);
    i64 hM = (ref + p->vco1.min_inputfreq) / p->vco1.min_inputfreq;
    hM = MIN(hM, (i64)p->vco1.max_m);
    lM = MIN(lM, hM);
    if (lM < 1) lM = 1;
    for (i64 M = lM; M <= hM; M++) {
        u32 tmp = (u32)((i64)freq * P * M);
        i64 N = tmp / ref, fN = tmp % ref;
        if (fN < ref / 2) N--;
        fN = (i64)tmp - N * ref;
        if (N < p->vco1.min_n) continue;
        if (N > p->vco1.max_n) break;
        *pN = (int)N;
        *pM = (int)M;
        *pP = (int)P;
        i64 f = ((fN << 13) + ref / 2) / ref;
        *pfN = (int)((f - 4096) & 0xFFFF);
        return (int)freq;
    }
    return E_INVAL;
}

/* pllnv04.c: getMNP_single */
static int mnp_single(const nv_pll_limits_t *p, int clk, int *pN, int *pM, int *pP) {
    int minvco = (int)p->vco1.min_freq, maxvco = (int)p->vco1.max_freq;
    int minM = p->vco1.min_m, maxM = p->vco1.max_m, minN = p->vco1.min_n, maxN = p->vco1.max_n;
    int minU = (int)p->vco1.min_inputfreq, maxU = (int)p->vco1.max_inputfreq;
    int minP = p->min_p, maxP = p->max_p_usable, crystal = (int)p->refclk;
    int bestdelta = 0x7FFFFFFF, bestclk = 0;
    if (!maxU) maxU = 0x7FFFFFFF;
    if (maxP > 7) maxP = 7;
    int P = 1 << maxP;
    if (clk * P < minvco) { minvco = clk * maxP; maxvco = minvco * 2; }
    if (clk + clk / 200 > maxvco) maxvco = clk + clk / 200;
    for (int thisP = minP; thisP <= maxP; thisP++) {
        P = 1 << thisP;
        int clkP = clk * P;
        if (clkP < minvco) continue;
        if (clkP > maxvco) return bestclk;
        for (int M = minM ? minM : 1; M <= maxM; M++) {
            if (crystal / M < minU) return bestclk;
            if (crystal / M > maxU) continue;
            int N = (clkP * M + crystal / 2) / crystal;
            if (N < minN) continue;
            if (N > maxN) break;
            int calc = ((N * crystal + P / 2) / P + M / 2) / M, delta = calc > clk ? calc - clk : clk - calc;
            if (delta < bestdelta) {
                bestdelta = delta; bestclk = calc; *pN = N; *pM = M; *pP = thisP;
                if (!delta) return bestclk;
            }
        }
    }
    return bestclk;
}

/* pllnv04.c: getMNP_double */
static int mnp_double(const nv_pll_limits_t *p, u8 chip, int clk, int *pN1, int *pM1, int *pN2, int *pM2, int *pP) {
    int minvco1 = (int)p->vco1.min_freq, maxvco1 = (int)p->vco1.max_freq;
    int minvco2 = (int)p->vco2.min_freq, maxvco2 = (int)p->vco2.max_freq;
    int minU1 = (int)p->vco1.min_inputfreq, minU2 = (int)p->vco2.min_inputfreq;
    int maxU1 = (int)p->vco1.max_inputfreq, maxU2 = (int)p->vco2.max_inputfreq;
    int minM1 = p->vco1.min_m, maxM1 = p->vco1.max_m, minN1 = p->vco1.min_n, maxN1 = p->vco1.max_n;
    int minM2 = p->vco2.min_m, maxM2 = p->vco2.max_m, minN2 = p->vco2.min_n, maxN2 = p->vco2.max_n;
    int maxlog2P = p->max_p_usable, crystal = (int)p->refclk;
    int fixedgain2 = minM2 == maxM2 && minN2 == maxN2;
    int bestdelta = 0x7FFFFFFF, bestclk = 0, log2P;
    if (!maxU1) maxU1 = 0x7FFFFFFF;
    if (!maxU2) maxU2 = 0x7FFFFFFF;
    int vco2 = (maxvco2 - maxvco2 / 200) / 2;
    for (log2P = 0; clk && log2P < maxlog2P && clk <= (vco2 >> log2P); log2P++)
        ;
    int clkP = clk << log2P;
    if (maxvco2 < clk + clk / 200) maxvco2 = clk + clk / 200;
    for (int M1 = minM1 ? minM1 : 1; M1 <= maxM1; M1++) {
        if (crystal / M1 < minU1) return bestclk;
        if (crystal / M1 > maxU1) continue;
        for (int N1 = minN1; N1 <= maxN1; N1++) {
            int c1 = (int)((i64)crystal * N1 / M1);
            if (c1 < minvco1) continue;
            if (c1 > maxvco1) break;
            for (int M2 = minM2 ? minM2 : 1; M2 <= maxM2; M2++) {
                if (c1 / M2 < minU2) break;
                if (c1 / M2 > maxU2) continue;
                int N2 = (int)(((i64)clkP * M2 + c1 / 2) / c1), c2;
                if (N2 < minN2) continue;
                if (N2 > maxN2) break;
                if (!fixedgain2) {
                    if (chip < 0x60 && (N2 / M2 < 4 || N2 / M2 > 10)) continue;
                    c2 = (int)((i64)c1 * N2 / M2);
                    if (c2 < minvco2) break;
                    if (c2 > maxvco2) continue;
                } else {
                    c2 = c1;
                }
                int out = c2 >> log2P, delta = out > clk ? out - clk : clk - out;
                if (delta < bestdelta) {
                    bestdelta = delta; bestclk = out;
                    *pN1 = N1; *pM1 = M1; *pN2 = N2; *pM2 = M2; *pP = log2P;
                    if (!delta) return bestclk;
                }
            }
        }
    }
    return bestclk;
}

int nv_pll_calc_nv04(const nv_pll_limits_t *p, u16 chipset, u32 khz, int *N1, int *M1, int *N2, int *M2, int *P) {
    (void)chipset;
    if (!p->refclk || !khz || khz > 2000000) return 0;
    if (!p->vco2.max_freq) {
        int r = mnp_single(p, (int)khz, N1, M1, P);
        *N2 = *M2 = 1;
        return r;
    }
    return mnp_double(p, 0x60, (int)khz, N1, M1, N2, M2, P);
}

/* a register a VPLL may be at (nothing else is written on a table's word) */
static int vpll_reg_ok(u32 reg) {
    return (reg >= 0x004000 && reg < 0x005000) || (reg >= 0x00e800 && reg < 0x00f000) || (reg >= 0x132000 && reg < 0x138000) ||
           (reg >= 0x610000 && reg < 0x620000);
}

static u32 crystal(nv_evo_t *e) {
    u32 strap = e->r->rd32(e->r->ctx, 0x101000);
    if (nv_bad_read(strap)) return 27000;
    switch (strap & 0x00400040) {
    case 0x00000000: return 13500;
    case 0x00000040: return 14318;
    case 0x00400000: return 27000;
    default: return 25000;
    }
}

int nv_evo_vpll_set(nv_evo_t *e, int head, u32 khz) {
    nv_pll_limits_t p;
    int r = nv_pll_limits(e->bios, e->chipset, (u8)(NV_PLL_VPLL0 + head), crystal(e), &p);
    if (r) {
        nv_evo_log(e, "VPLL%d: no limits in the video BIOS (%d)", head, r);
        return r;
    }
    if (!p.reg) {
        /* table version 5.0 names no register; nouveau would write at 0 */
        nv_evo_log(e, "VPLL%d: the PLL table (version %x.%x) names no register: left to the display engine", head, p.ver >> 4,
                   p.ver & 15);
        return 0;
    }
    if (!vpll_reg_ok(p.reg)) {
        nv_evo_log(e, "VPLL%d: register %06x from the PLL table is not a PLL register: not written", head, p.reg);
        return E_INVAL;
    }
    const nv_regs_t *R = e->r;
    if (e->g->pll == EVO_PLL_NV50) {
        int N1 = 0, M1 = 0, N2 = 0, M2 = 0, P = 0;
        int got = nv_pll_calc_nv04(&p, e->chipset, khz, &N1, &M1, &N2, &M2, &P);
        if (got <= 0) { nv_evo_log(e, "VPLL%d: no coefficients for %u kHz", head, khz); return E_INVAL; }
        R->wr32(R->ctx, p.reg + 0, 0x10000611);
        u32 v = R->rd32(R->ctx, p.reg + 4);
        R->wr32(R->ctx, p.reg + 4, (v & ~0x00ff00ffu) | ((u32)M1 << 16) | (u32)N1);
        v = R->rd32(R->ctx, p.reg + 8);
        R->wr32(R->ctx, p.reg + 8, (v & ~0x7fff00ffu) | ((u32)P << 28) | ((u32)M2 << 16) | (u32)N2);
        nv_evo_log(e, "VPLL%d at %06x: %u kHz -> %d kHz (N1 %d M1 %d N2 %d M2 %d P %d, ref %u)", head, p.reg, khz, got, N1, M1, N2,
                   M2, P, p.refclk);
        return 0;
    }
    int N = 0, fN = 0, M = 0, P = 0;
    if (nv_pll_calc_gt215(&p, khz, &N, &fN, &M, &P) <= 0) { nv_evo_log(e, "VPLL%d: no coefficients for %u kHz", head, khz); return E_INVAL; }
    if (e->g->pll == EVO_PLL_GT215) {
        R->wr32(R->ctx, p.reg + 0, 0x50000610);
        u32 v = R->rd32(R->ctx, p.reg + 4);
        R->wr32(R->ctx, p.reg + 4, (v & ~0x003fffffu) | ((u32)P << 16) | ((u32)M << 8) | (u32)N);
        R->wr32(R->ctx, p.reg + 8, (u32)fN);
    } else {
        R->wr32(R->ctx, p.reg + 0x0c, R->rd32(R->ctx, p.reg + 0x0c) | 0x00000100);
        R->wr32(R->ctx, p.reg + 0x04, ((u32)P << 16) | ((u32)N << 8) | (u32)M);
        R->wr32(R->ctx, p.reg + 0x10, (u32)fN << 16);
    }
    nv_evo_log(e, "VPLL%d at %06x: %u kHz (N %d fN %04x M %d P %d, ref %u, table %x.%x)", head, p.reg, khz, N, fN, M, P, p.refclk,
               p.ver >> 4, p.ver & 15);
    return 0;
}
