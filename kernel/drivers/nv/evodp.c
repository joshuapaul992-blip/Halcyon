/* Halcyon OS - NVIDIA GPUs (M10b): DisplayPort on the EVO display engine:
 * choosing the link (lanes x rate), the transfer unit and watermark, and
 * link training over AUX. Ported from nouveau (MIT, Copyright 2012-2016
 * Red Hat Inc.; THIRD_PARTY_NOTICES.md): nvkm/engine/disp/dp.c (training),
 * nv50.c (nv50_disp_super_2_2_dp), g94.c, gf119.c, gm107.c, gm200.c (the
 * SOR's DP registers), nvkm/subdev/bios/dp.c (the drive levels).
 *
 * Every loop is bounded: configurations (at most 8), clock recovery and
 * equalisation tries (5 each, 40 in all), AUX DEFERs (32 per transfer). */
#include "evo.h"
#include "core/lib.h"

/* DPCD addresses and bits (DP 1.4) */
#define DPCD_REV            0x000
#define DPCD_MAX_DOWNSPREAD 0x003
#define DPCD_RD_INTERVAL    0x00e
#define DPCD_LINK_BW_SET    0x100
#define DPCD_TRAINING_PATTERN_SET 0x102
#define DPCD_LANE0_SET      0x103
#define DPCD_LANE0_1_SET2   0x10f
#define DPCD_LANE0_1_STATUS 0x202
#define DPCD_ADJUST_REQ     0x206
#define DPCD_ADJUST_REQ_PC2 0x20c
#define DPCD_SET_POWER      0x600
#define DPCD_EXT_CAPS       0x2200

#define LANE_CR_DONE        0x01
#define LANE_EQ_DONE        0x02
#define LANE_SYMBOL_LOCKED  0x04
#define ALIGN_DONE          0x01
#define MAX_SWING_REACHED   0x04

/* ---------------- pure parts ---------------- */

int nv_dp_configs(int lanes_max, u8 bw_max, u32 data_kBps, int nr[], u8 bw[], int max) {
    static const u8 rates[] = { 0x1e, 0x14, 0x0a, 0x06 };
    int n = 0;
    if (lanes_max > 4) lanes_max = 4;
    if (lanes_max == 3) lanes_max = 2;
    for (int l = lanes_max; l > 0 && n < max; l >>= 1)
        for (size_t r = 0; r < ARRAY_SIZE(rates) && n < max; r++) {
            if (rates[r] > bw_max) continue;
            if ((u64)rates[r] * 27000u * (u64)l < data_kBps) continue;
            nr[n] = l;
            bw[n] = rates[r];
            n++;
        }
    return n;
}

int nv_dp_fits(int lanes, u8 bw, u32 clock_khz, int bpp) {
    u64 link = (u64)lanes * bw * 27000u;                 /* kB/s after 8b/10b */
    u64 need = ((u64)clock_khz * (u32)bpp + 7) / 8;
    return lanes > 0 && bw && need <= link;
}

int nv_dp_tu(const nv_evo_head_state_t *h, int nr, u8 bw, u8 ef, int depth, int activesym, nv_dp_tu_t *out) {
    memset(out, 0, sizeof *out);
    u32 khz = h->hz / 1000;
    if (!khz || nr <= 0 || !bw || depth <= 0) return E_INVAL;
    const i64 linkKBps = (i64)bw * 27000, symbol = 100000;
    /* symbols per hblank / vblank (from the comments in the tegra driver, as nouveau) */
    i64 hs = (i64)h->hblanke + h->htotal - h->hblanks - 7;
    hs = hs * linkKBps / khz - 3 * ef - 12 / nr;
    i64 vs = (i64)h->vblanks - h->vblanke - 25;
    vs = vs * linkKBps / khz - (36 / nr + 3) - 1;
    out->hsym = (u16)(hs < 0 ? 0 : hs > 0xFFFF ? 0xFFFF : hs);
    out->vsym = (u32)(vs < 0 ? 0 : vs > 0xFFFFFF ? 0xFFFFFF : vs);
    i64 link_data_rate = ((i64)khz * depth / 8) / nr;
    i64 link_ratio = link_data_rate * symbol / linkKBps;
    if (link_ratio >= symbol) return E_INVAL;            /* does not fit the link */
    int bestTU = 0, bestVTUi = 0, bestVTUf = 0, bestVTUa = 0;
    i64 best_diff = 64 * symbol;
    for (int TU = 64; activesym && TU >= 32; TU--) {
        i64 tu_valid = link_ratio * TU, calc, diff;
        int VTUi = (int)(tu_valid / symbol), VTUf, VTUa;
        calc = (i64)VTUi * symbol;
        diff = tu_valid - calc;
        if (diff) {
            if (diff >= symbol / 2) {
                VTUf = (int)(symbol / (symbol - diff));
                if (symbol - VTUf * diff) VTUf++;
                if (VTUf <= 15) { VTUa = 1; calc += symbol - symbol / VTUf; }
                else { VTUa = 0; VTUf = 1; calc += symbol; }
            } else {
                VTUa = 0;
                VTUf = (int)MIN(symbol / diff, (i64)15);
                calc += symbol / VTUf;
            }
            diff = calc - tu_valid;
        } else {
            /* no remainder: the hardware wants a non-zero fraction */
            VTUa = 0; VTUf = 1; VTUi--;
        }
        if (diff < best_diff) {
            best_diff = diff; bestTU = TU; bestVTUa = VTUa; bestVTUf = VTUf; bestVTUi = VTUi;
            if (!diff) break;
        }
    }
    if (activesym && !bestTU) return E_INVAL;
    if (!activesym) bestTU = 64;
    out->TU = bestTU; out->VTUa = bestVTUa; out->VTUf = bestVTUf; out->VTUi = bestVTUi;
    i64 unk = (symbol - link_ratio) * bestTU;
    unk = unk * link_ratio / symbol / symbol + 6;
    out->watermark = (u32)unk;
    return 0;
}

/* ---------------- AUX ---------------- */

static void dly(nv_evo_t *e, u32 us) { if (e->r->delay_us) e->r->delay_us(e->r->ctx, us); }

int nv_dp_rd(nv_evo_t *e, int ccb, u32 addr, u8 *buf, int len) {
    if (!e->ops.aux) return E_NODEV;
    int done = 0, tries = 0;
    while (done < len) {
        u8 n = (u8)MIN(16, len - done);
        int r = e->ops.aux(e->ops.ctx, ccb, NV_AUX_NATIVE_RD, addr + (u32)done, buf + done, &n);
        if (r < 0) { if (r == E_TIMEOUT && ++tries < 3) continue; return r; }
        if ((r & 3) == NV_AUX_NACK) return E_IO;
        if ((r & 3) == NV_AUX_DEFER || !n) { if (++tries >= 32) return E_TIMEOUT; dly(e, 400); continue; }
        done += n;
        tries = 0;
    }
    return 0;
}

int nv_dp_wr(nv_evo_t *e, int ccb, u32 addr, const u8 *buf, int len) {
    if (!e->ops.aux) return E_NODEV;
    int done = 0, tries = 0;
    while (done < len) {
        u8 tmp[16], n = (u8)MIN(16, len - done);
        memcpy(tmp, buf + done, n);
        int r = e->ops.aux(e->ops.ctx, ccb, NV_AUX_NATIVE_WR, addr + (u32)done, tmp, &n);
        if (r < 0) { if (r == E_TIMEOUT && ++tries < 3) continue; return r; }
        if ((r & 3) == NV_AUX_NACK) return E_IO;
        if ((r & 3) == NV_AUX_DEFER) { if (++tries >= 32) return E_TIMEOUT; dly(e, 400); continue; }
        done += MIN(16, len - done);
        tries = 0;
    }
    return 0;
}

/* ---------------- detect ---------------- */

int nv_dp_detect(nv_evo_t *e, nv_evo_dp_t *dp) {
    const nv_dcb_output_t *o = e->bios && dp->dcb >= 0 && dp->dcb < e->bios->noutputs ? &e->bios->out[dp->dcb] : NULL;
    if (!o) return E_INVAL;
    int r = nv_dp_rd(e, dp->ccb, DPCD_REV, dp->dpcd, 16);
    if (r) { snprintf(dp->why, sizeof dp->why, "no DisplayPort sink answers (%d)", r); return r; }
    if (dp->dpcd[DPCD_RD_INTERVAL] & 0x80) {
        /* DP 1.3: the real capabilities may be in the extended field */
        u8 ext[16];
        if (!nv_dp_rd(e, dp->ccb, DPCD_EXT_CAPS, ext, 16) && ext[0] >= dp->dpcd[0]) memcpy(dp->dpcd, ext, 16);
    }
    static const u8 dcb_bw[] = { 0x06, 0x0a, 0x14, 0x1e };
    u8 dbw = o->dp_max_rate < 4 ? dcb_bw[o->dp_max_rate] : 0x1e;
    dp->bw_max = MIN(dp->dpcd[1], dbw);
    if (e->g->dp_max_bw && dp->bw_max > e->g->dp_max_bw) dp->bw_max = e->g->dp_max_bw;
    dp->lanes_max = MIN(dp->dpcd[2] & 0x1F, o->dp_max_lanes ? o->dp_max_lanes : 4);
    if (dp->lanes_max == 3) dp->lanes_max = 2;
    dp->link = o->link & 1 ? 1 : 2;
    dp->have_info = e->bios ? nv_dpout_match(e->bios, nv_dcb_hasht(o), nv_dcb_hashm(o), &dp->info) : 0;
    if (!dp->lanes_max || dp->bw_max < 0x06) {
        snprintf(dp->why, sizeof dp->why, "sink capabilities unusable (rate %02x, lanes %d)", dp->dpcd[1], dp->dpcd[2] & 0x1F);
        return E_NOTSUP;
    }
    return 0;
}

/* ---------------- the SOR's DP registers per generation ---------------- */

typedef struct lt {
    nv_evo_t *e;
    nv_evo_dp_t *dp;
    int id;                     /* SOR */
    u32 soff, loff;
    const u8 *lanes;            /* logical lane -> pad lane */
    u8 stat[6], conf[4], pc2stat, pc2conf[2];
    int pc2;
    int iters;
} lt_t;

static u32 R(lt_t *l, u32 reg) { return l->e->r->rd32(l->e->r->ctx, reg); }
static void W(lt_t *l, u32 reg, u32 v) { l->e->r->wr32(l->e->r->ctx, reg, v); }
static void MK(lt_t *l, u32 reg, u32 m, u32 v) { W(l, reg, (R(l, reg) & ~m) | v); }

static void sor_links(lt_t *l) {
    nv_evo_t *e = l->e;
    nv_evo_dp_t *dp = l->dp;
    u32 dpctrl = (((1u << dp->nr) - 1) << 16) | (dp->ef ? 0x4000u : 0);
    if (e->g->gf119) {
        MK(l, 0x612300 + l->soff, 0x007c0000, (u32)dp->bw << 18);
        MK(l, 0x61c10c + l->loff, 0x401f4000, dpctrl);
    } else {
        MK(l, 0x614300 + l->soff, 0x000c0000, dp->bw > 0x06 ? 0x00040000 : 0);
        MK(l, 0x61c10c + l->loff, 0x001f4000, dpctrl);
    }
}

static void sor_power(lt_t *l, int nr) {
    u32 m = 0;
    for (int i = 0; i < nr; i++) m |= 1u << l->lanes[i];
    MK(l, 0x61c130 + l->loff, 0x0000000f, m);
    MK(l, 0x61c034 + l->soff, 0x80000000, 0x80000000);
    u64 end = l->e->r->now_us(l->e->r->ctx) + 2000000;
    while ((R(l, 0x61c034 + l->soff) & 0x80000000) && l->e->r->now_us(l->e->r->ctx) < end) dly(l->e, 10);
}

static void sor_pattern(lt_t *l, int p) {
    int dpf = l->e->g->dp;
    if (dpf == EVO_DP_G94 || dpf == EVO_DP_MCP89) {
        static const u32 d[3] = { 0x00001000, 0x01000000, 0x02000000 };
        if (p > 2) return;
        MK(l, 0x61c10c + l->loff, 0x0f001000, d[p]);
    } else if (dpf == EVO_DP_GF119) {
        static const u32 d[4] = { 0x10101010, 0x01010101, 0x02020202, 0x03030303 };
        if (p > 3) return;
        MK(l, 0x61c110 + l->soff, 0x1f1f1f1f, d[p]);
    } else {
        static const u32 d[5] = { 0x10101010, 0x01010101, 0x02020202, 0x03030303, 0x1b1b1b1b };
        if (p > 4) return;
        MK(l, (l->dp->link & 1 ? 0x61c110 : 0x61c12c) + l->soff, 0x1f1f1f1f, d[p]);
    }
}

static void sor_drive(lt_t *l, int ln, int pc, int dc, int pe, int pu) {
    int dpf = l->e->g->dp;
    u32 shift = (u32)l->lanes[ln] * 8, d0, d1, d2;
    u32 pum = dpf == EVO_DP_GM200 ? 0x00000f00 : 0x0000ff00;
    if (dpf == EVO_DP_GM200) pu &= 0x0f;
    d0 = R(l, 0x61c118 + l->loff) & ~(0xffu << shift);
    d1 = R(l, 0x61c120 + l->loff) & ~(0xffu << shift);
    d2 = R(l, 0x61c130 + l->loff);
    if ((d2 & pum) < ((u32)pu << 8) || ln == 0) d2 = (d2 & ~pum) | ((u32)pu << 8);
    W(l, 0x61c118 + l->loff, d0 | ((u32)dc << shift));
    W(l, 0x61c120 + l->loff, d1 | ((u32)pe << shift));
    W(l, 0x61c130 + l->loff, d2);
    if (dpf != EVO_DP_G94 && dpf != EVO_DP_MCP89) {
        u32 d3 = R(l, 0x61c13c + l->loff) & ~(0xffu << shift);
        W(l, 0x61c13c + l->loff, d3 | ((u32)pc << shift));
    }
}

/* ---------------- training (nvkm/engine/disp/dp.c) ---------------- */

static void script(lt_t *l, u16 s, const char *what) {
    if (!s) return;
    nv_init_ctx_t c = { l->dp->dcb, l->id, l->dp->link, -1 };
    nv_evo_script(l->e, s, &c, what);
}

static int lt_sense(lt_t *l, int pc, u32 delay) {
    dly(l->e, delay);
    if (nv_dp_rd(l->e, l->dp->ccb, DPCD_LANE0_1_STATUS, &l->stat[0], 3)) return E_IO;
    if (nv_dp_rd(l->e, l->dp->ccb, DPCD_ADJUST_REQ, &l->stat[4], 2)) return E_IO;
    if (pc && nv_dp_rd(l->e, l->dp->ccb, DPCD_ADJUST_REQ_PC2, &l->pc2stat, 1)) l->pc2stat = 0;
    return 0;
}

static int lt_drive(lt_t *l, int pc) {
    nv_evo_dp_t *dp = l->dp;
    l->pc2conf[0] = l->pc2conf[1] = 0;
    for (int i = 0; i < dp->nr; i++) {
        u8 lane = (l->stat[4 + (i >> 1)] >> ((i & 1) * 4)) & 0xF;
        u8 lpc2 = (l->pc2stat >> (i * 2)) & 3, lpre = (lane & 0x0c) >> 2, lvsw = lane & 3;
        u8 hivs = (u8)(3 - lpre), hipe = 3, hipc = 3;
        if (lpc2 >= hipc) lpc2 = hipc | 0x04;
        if (lpre >= hipe) { lpre = hipe | MAX_SWING_REACHED; lvsw = hivs = (u8)(3 - (lpre & 3)); }
        else if (lvsw >= hivs) lvsw = hivs | MAX_SWING_REACHED;
        l->conf[i] = (u8)((lpre << 3) | lvsw);
        l->pc2conf[i >> 1] |= (u8)(lpc2 << ((i & 1) * 4));
        nv_dpcfg_t cfg;
        if (dp->have_info && nv_dpcfg_match(l->e->bios, &dp->info, lpc2 & 3, lvsw & 3, lpre & 3, &cfg))
            sor_drive(l, i, cfg.pc, cfg.dc, cfg.pe, cfg.tx_pu);
    }
    if (nv_dp_wr(l->e, dp->ccb, DPCD_LANE0_SET, l->conf, 4)) return E_IO;
    if (pc && nv_dp_wr(l->e, dp->ccb, DPCD_LANE0_1_SET2, l->pc2conf, 2)) return E_IO;
    return 0;
}

static void lt_pattern(lt_t *l, u8 p) {
    sor_pattern(l, p);
    u8 tp = 0;
    nv_dp_rd(l->e, l->dp->ccb, DPCD_TRAINING_PATTERN_SET, &tp, 1);
    tp &= ~0x0f;
    tp |= p != 4 ? p : 7;
    if (p) tp |= 0x20;                                   /* scrambling off */
    else tp &= ~0x20;
    nv_dp_wr(l->e, l->dp->ccb, DPCD_TRAINING_PATTERN_SET, &tp, 1);
}

static int lt_cr(lt_t *l) {
    nv_evo_dp_t *dp = l->dp;
    int cr = 0, abort = 0, tries = 0, voltage = l->conf[0] & 3;
    u32 us = 0;
    lt_pattern(l, 1);
    if (dp->dpcd[0] < 0x14) us = (u32)(dp->dpcd[DPCD_RD_INTERVAL] & 0x7f) * 4000;
    do {
        if (++l->iters > 40) break;
        if (lt_drive(l, 0) || lt_sense(l, 0, us ? us : 100)) break;
        cr = 1;
        for (int i = 0; i < dp->nr; i++) {
            u8 lane = (l->stat[i >> 1] >> ((i & 1) * 4)) & 0xF;
            if (!(lane & LANE_CR_DONE)) {
                cr = 0;
                if (l->conf[i] & MAX_SWING_REACHED) abort = 1;
                break;
            }
        }
        if ((l->conf[0] & 3) != voltage) { voltage = l->conf[0] & 3; tries = 0; }
    } while (!cr && !abort && ++tries < 5);
    return cr ? 0 : E_IO;
}

static int lt_eq(lt_t *l) {
    nv_evo_dp_t *dp = l->dp;
    int eq = 0, cr = 1, tries = 0;
    if (dp->dpcd[0] >= 0x14 && (dp->dpcd[3] & 0x80)) lt_pattern(l, 4);
    else if (dp->dpcd[0] >= 0x12 && (dp->dpcd[2] & 0x40)) lt_pattern(l, 3);
    else lt_pattern(l, 2);
    u32 us = (u32)(dp->dpcd[DPCD_RD_INTERVAL] & 0x7f) * 4000;
    do {
        if (++l->iters > 40) break;
        if ((tries && lt_drive(l, l->pc2)) || lt_sense(l, l->pc2, us ? us : 400)) break;
        eq = !!(l->stat[2] & ALIGN_DONE);
        for (int i = 0; i < dp->nr && eq; i++) {
            u8 lane = (l->stat[i >> 1] >> ((i & 1) * 4)) & 0xF;
            if (!(lane & LANE_CR_DONE)) cr = 0;
            if (!(lane & LANE_EQ_DONE) || !(lane & LANE_SYMBOL_LOCKED)) eq = 0;
        }
    } while (!eq && cr && ++tries <= 5);
    return eq ? 0 : E_IO;
}

static int lt_link(lt_t *l) {
    nv_evo_dp_t *dp = l->dp;
    u8 sink[2] = { dp->bw, (u8)(dp->nr | (dp->ef ? 0x80 : 0)) };
    if (nv_dp_wr(l->e, dp->ccb, DPCD_LINK_BW_SET, sink, 2)) return E_IO;
    memset(l->stat, 0, sizeof l->stat);
    memset(l->conf, 0, sizeof l->conf);
    int r = lt_cr(l);
    if (!r) r = lt_eq(l);
    lt_pattern(l, 0);
    return r;
}

static int lt_links(lt_t *l) {
    nv_evo_t *e = l->e;
    nv_evo_dp_t *dp = l->dp;
    nv_evo_log(e, "DP: SOR%d training %d lane%s at %u.%u GHz", l->id, dp->nr, dp->nr == 1 ? "" : "s", dp->bw * 27 / 100,
               (dp->bw * 27 % 100) / 10);
    if (dp->have_info) script(l, nv_dp_lnkcmp(e->bios, &dp->info, dp->bw), "LinkRate");
    sor_links(l);
    sor_power(l, dp->nr);
    return lt_link(l);
}

int nv_dp_train(nv_evo_t *e, nv_evo_dp_t *dp, int or_id, u32 data_kBps) {
    static const u8 lanes_g94[4] = { 2, 1, 0, 3 }, lanes_mcp89[4] = { 3, 2, 1, 0 }, lanes_lin[4] = { 0, 1, 2, 3 };
    lt_t l;
    memset(&l, 0, sizeof l);
    l.e = e; l.dp = dp; l.id = or_id;
    l.soff = (u32)or_id * 0x800;
    l.loff = l.soff + (dp->link == 2 ? 0x80 : 0);
    l.lanes = e->g->dp == EVO_DP_MCP89 ? lanes_mcp89 : (e->g->dp == EVO_DP_GM107 || e->g->dp == EVO_DP_GM200) ? lanes_lin : lanes_g94;
    dp->trained = 0;
    if (e->g->dp == EVO_DP_NONE) { snprintf(dp->why, sizeof dp->why, "no DisplayPort on this display engine"); return E_NOTSUP; }
    /* intersect the sink's patterns with the source's (nvkm_dp_train_links) */
    if (e->chipset < 0x110) dp->dpcd[3] &= ~0x80;
    if (e->chipset < 0xd0) dp->dpcd[2] &= ~0x40;
    l.pc2 = !!(dp->dpcd[2] & 0x40);
    /* the sink out of power saving */
    u8 pwr;
    if (!nv_dp_rd(e, dp->ccb, DPCD_SET_POWER, &pwr, 1) && (pwr & 3) != 1) {
        pwr = (u8)((pwr & ~3) | 1);
        nv_dp_wr(e, dp->ccb, DPCD_SET_POWER, &pwr, 1);
        dly(e, 1000);
    }
    dp->ef = !!(dp->dpcd[2] & 0x80);
    if (dp->have_info) {
        script(&l, (dp->dpcd[DPCD_MAX_DOWNSPREAD] & 1) ? dp->info.script[2] : dp->info.script[3], "Spread");
        script(&l, dp->info.script[0], "BeforeLinkTraining");
    } else {
        nv_evo_log(e, "DP: no DisplayPort table entry for this output: default drive levels");
    }
    int nr[8];
    u8 bw[8];
    int n = nv_dp_configs(dp->lanes_max, dp->bw_max, data_kBps, nr, bw, 8), r = E_NOTSUP;
    if (!n) snprintf(dp->why, sizeof dp->why, "%u kB/s needs more than %d lanes at %02x", data_kBps, dp->lanes_max, dp->bw_max);
    for (int i = 0; i < n && r; i++) {
        dp->nr = nr[i];
        dp->bw = bw[i];
        r = lt_links(&l);
        if (r) nv_evo_log(e, "DP: %d x %02x did not train (status %02x %02x %02x)", nr[i], bw[i], l.stat[0], l.stat[1], l.stat[2]);
    }
    if (dp->have_info) script(&l, dp->info.script[1], "AfterLinkTraining");
    if (r) {
        if (n) snprintf(dp->why, sizeof dp->why, "link training failed (%d configurations)", n);
        nv_evo_log(e, "DP: %s", dp->why);
        dp->nr = 0;
        return E_IO;
    }
    dp->trained = 1;
    nv_evo_log(e, "DP: SOR%d trained, %d lane%s x %02x, enhanced framing %s", or_id, dp->nr, dp->nr == 1 ? "" : "s", dp->bw,
               dp->ef ? "on" : "off");
    return 0;
}
