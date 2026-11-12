/* Halcyon OS - NVIDIA GPUs: talking to monitors.
 *
 *   - a bit-banged I2C master over open-drain line callbacks (the
 *     CRTC-indexed ports of pre-NV50 cards and the NV50+ I2C port registers
 *     both reduce to "drive / sense SCL and SDA")
 *   - one DisplayPort AUX transaction on the display engine's AUX channel,
 *     in the G94 (G94 to GM10x) and GM200 (GM20x, Pascal) register layouts
 *   - native AUX reads and I2C over AUX (DisplayPort 1.2, 2.7.7.1)
 *   - EDID over DDC (address 0x50, E-DDC segment pointer 0x30)
 *
 * The AUX register sequence is ported from nouveau's
 * nvkm/subdev/i2c/auxg94.c and auxgm200.c (Copyright 2012 Red Hat Inc.,
 * Ben Skeggs; MIT, the notice is in THIRD_PARTY_NOTICES.md), changed so
 * that the channel's control bits are restored to what they were. Every
 * wait is bounded. Runs on the host too (tests/host/nvtest.c simulates the
 * buses). */
#include "nvhw.h"
#include "core/lib.h"

/* ---------------- bit-banged I2C ---------------- */

typedef struct bbs { nv_i2c_bb_t *b; u64 t0; } bbs_t;

static void hd(bbs_t *s) { s->b->delay_us(s->b->ctx, s->b->half_us); }

static int expired(bbs_t *s) { return s->b->now_us(s->b->ctx) - s->t0 > s->b->total_us; }

/* release SCL and wait for it to go high (a slave may stretch the clock) */
static int scl_up(bbs_t *s) {
    nv_i2c_bb_t *b = s->b;
    b->drive_scl(b->ctx, 1);
    u64 t = b->now_us(b->ctx);
    while (!b->sense_scl(b->ctx)) {
        if (b->now_us(b->ctx) - t > b->stretch_us || expired(s)) return E_TIMEOUT;
        b->delay_us(b->ctx, 1);
    }
    return 0;
}

static int put_bit(bbs_t *s, int v) {
    s->b->drive_sda(s->b->ctx, v);
    hd(s);
    int r = scl_up(s);
    if (r) return r;
    hd(s);
    s->b->drive_scl(s->b->ctx, 0);
    return 0;
}

static int get_bit(bbs_t *s) {
    s->b->drive_sda(s->b->ctx, 1);
    hd(s);
    int r = scl_up(s);
    if (r) return r;
    hd(s);
    int v = s->b->sense_sda(s->b->ctx) ? 1 : 0;
    s->b->drive_scl(s->b->ctx, 0);
    return v;
}

static int put_byte(bbs_t *s, u8 v) {
    for (int i = 7; i >= 0; i--) {
        int r = put_bit(s, (v >> i) & 1);
        if (r) return r;
    }
    int ack = get_bit(s);
    if (ack < 0) return ack;
    return ack ? E_IO : 0;                          /* 1 = NAK */
}

static int get_byte(bbs_t *s, u8 *v, int last) {
    u8 x = 0;
    for (int i = 7; i >= 0; i--) {
        int r = get_bit(s);
        if (r < 0) return r;
        x = (u8)(x | r << i);
    }
    *v = x;
    return put_bit(s, last ? 1 : 0);
}

static void stop(bbs_t *s) {
    nv_i2c_bb_t *b = s->b;
    b->drive_scl(b->ctx, 0);
    hd(s);
    b->drive_sda(b->ctx, 0);
    hd(s);
    scl_up(s);                                      /* (a timeout here changes nothing) */
    hd(s);
    b->drive_sda(b->ctx, 1);
    hd(s);
}

/* (repeated) START; the first one makes sure the bus is free: a slave left
 * in the middle of a byte (SDA low) gets up to nine clocks to let go */
static int start(bbs_t *s, int repeated) {
    nv_i2c_bb_t *b = s->b;
    if (repeated) {
        b->drive_sda(b->ctx, 1);
        hd(s);
        int r = scl_up(s);
        if (r) return r;
        hd(s);
    } else {
        b->drive_sda(b->ctx, 1);
        int r = scl_up(s);
        if (r) return r;
        for (int i = 0; i < 9 && !b->sense_sda(b->ctx); i++) {
            b->drive_scl(b->ctx, 0);
            hd(s);
            if ((r = scl_up(s))) return r;
            hd(s);
        }
        if (!b->sense_sda(b->ctx)) return E_BUSY;
        hd(s);
    }
    b->drive_sda(b->ctx, 0);
    hd(s);
    b->drive_scl(b->ctx, 0);
    hd(s);
    return 0;
}

int nv_i2c_bb_xfer(nv_i2c_bb_t *b, nv_i2c_msg_t *msgs, int n) {
    if (!b->half_us) b->half_us = 5;
    if (!b->stretch_us) b->stretch_us = 2200;
    if (!b->total_us) b->total_us = 100000;
    bbs_t s = { b, b->now_us(b->ctx) };
    int r = 0;
    for (int m = 0; m < n && !r; m++) {
        nv_i2c_msg_t *g = &msgs[m];
        if ((r = start(&s, m > 0))) break;
        if ((r = put_byte(&s, (u8)(g->addr << 1 | (g->flags & NV_I2C_RD ? 1 : 0))))) break;
        for (u32 i = 0; i < g->len && !r; i++) {
            if (expired(&s)) { r = E_TIMEOUT; break; }
            if (g->flags & NV_I2C_RD) r = get_byte(&s, &g->buf[i], i + 1 == g->len);
            else r = put_byte(&s, g->buf[i]);
        }
    }
    if (r != E_BUSY) stop(&s);
    return r;
}

/* ---------------- AUX channel (display engine) ---------------- */

typedef struct aux_layout {
    u32 base, stride;
    u32 busy;                   /* "a transaction or a request is pending" */
    u32 ureq_mask, urep_mask;   /* software owns the channel: request / grant */
    u32 autodpcd;               /* register offset of the auto-DPCD enable (0: none) */
} aux_layout_t;

static const aux_layout_t layouts[2] = {
    [NV_AUXHW_G94]   = { 0x00E4C0, 0x50, 0x03010000, 0x00300000, 0x03000000, 0 },
    [NV_AUXHW_GM200] = { 0x00D930, 0x50, 0x07010000, 0x00700000, 0x07000000, 0x38 },
};

#define AUX_OUT   0x00
#define AUX_IN    0x10
#define AUX_ADDR  0x20
#define AUX_CTRL  0x24
#define AUX_STAT  0x28

static int wait_ctrl(const nv_regs_t *r, u32 reg, u32 mask, u32 want, u32 us, u32 *last) {
    u64 t = r->now_us(r->ctx);
    for (;;) {
        u32 v = r->rd32(r->ctx, reg);
        *last = v;
        if ((v & mask) == want) return 0;
        if (nv_bad_read(v) || r->now_us(r->ctx) - t > us) return E_TIMEOUT;
        r->delay_us(r->ctx, 1);
    }
}

int nv_aux_hw_xfer(nv_aux_hw_t *a, u8 req, u32 addr, u8 *buf, u8 *len) {
    const nv_regs_t *r = a->r;
    if (a->layout < 0 || a->layout > 1 || a->ch < 0 || a->ch > 7 || *len > 16) return E_INVAL;
    const aux_layout_t *L = &layouts[a->layout];
    u32 base = L->base + (u32)a->ch * L->stride, ctrl0, v;
    ctrl0 = r->rd32(r->ctx, base + AUX_CTRL);
    if (nv_bad_read(ctrl0)) return E_NODEV;
    /* idle, then ask for the channel (and wait for the grant), 1 ms each */
    if (wait_ctrl(r, base + AUX_CTRL, L->busy, 0, 1000, &v)) { a->last_ctrl = v; return E_TIMEOUT; }
    r->wr32(r->ctx, base + AUX_CTRL, (v & ~L->ureq_mask) | 0x00100000);
    int ret = 0;
    u32 ad = 0, stat = 0;
    if (wait_ctrl(r, base + AUX_CTRL, L->urep_mask, 0x01000000, 1000, &v)) { ret = E_TIMEOUT; goto out; }
    stat = r->rd32(r->ctx, base + AUX_STAT);
    if (!(stat & 0x10000000)) { ret = E_NODEV; goto out; }     /* hot plug sense: no sink */
    if (L->autodpcd) {
        ad = r->rd32(r->ctx, base + L->autodpcd);
        if (ad & 0x00010000) r->wr32(r->ctx, base + L->autodpcd, ad & ~0x00010000u);
    }
    if (!(req & 1)) {
        u32 x[4] = { 0, 0, 0, 0 };
        memcpy(x, buf, *len);
        for (int i = 0; i < 4; i++) r->wr32(r->ctx, base + AUX_OUT + (u32)i * 4, x[i]);
    }
    u32 ctrl = r->rd32(r->ctx, base + AUX_CTRL);
    ctrl &= ~0x0001F1FFu;
    ctrl |= (u32)(req & 0xF) << 12;
    ctrl |= *len ? (u32)(*len - 1) : 0x00000100;    /* 0x100: address only */
    r->wr32(r->ctx, base + AUX_ADDR, addr);
    r->wr32(r->ctx, base + AUX_CTRL, 0x80000000 | ctrl);       /* reset */
    r->wr32(r->ctx, base + AUX_CTRL, ctrl);
    r->wr32(r->ctx, base + AUX_CTRL, 0x00010000 | ctrl);       /* go: up to 2 ms */
    if (wait_ctrl(r, base + AUX_CTRL, 0x00010000, 0, 2000, &v)) { ret = E_TIMEOUT; goto out_ad; }
    stat = r->rd32(r->ctx, base + AUX_STAT);
    r->wr32(r->ctx, base + AUX_STAT, stat);                    /* (as nouveau: clears the sticky bits) */
    a->last_stat = stat;
    if (stat & 0x00000100) { ret = E_TIMEOUT; goto out_ad; }   /* no reply from the sink */
    if (stat & 0x00000E00) { ret = E_IO; goto out_ad; }        /* receive error */
    ret = (int)((stat >> 16) & 0xF);
    if (req & 1) {
        u32 x[4];
        for (int i = 0; i < 4; i++) x[i] = r->rd32(r->ctx, base + AUX_IN + (u32)i * 4);
        u8 got = (u8)(stat & 0x1F);
        if (got > *len) got = *len;
        memcpy(buf, x, got);
        *len = got;
    }
out_ad:
    if (L->autodpcd && (ad & 0x00010000)) r->wr32(r->ctx, base + L->autodpcd, ad);
out:
    a->last_ctrl = r->rd32(r->ctx, base + AUX_CTRL);
    /* give the channel back: the request bits as they were */
    r->wr32(r->ctx, base + AUX_CTRL, (a->last_ctrl & ~(L->ureq_mask | 0x00010000u)) | (ctrl0 & L->ureq_mask));
    return ret;
}

/* ---------------- DisplayPort AUX: native reads, I2C over AUX ---------------- */

#define AUX_TRIES 32                                /* DEFERs before giving up (DP: at least 7) */
#define AUX_DEFER_US 400

static void aux_defer(nv_aux_t *a) {
    a->defers++;
    if (a->delay_us) a->delay_us(a->ctx, AUX_DEFER_US);
}

int nv_aux_native_read(nv_aux_t *a, u32 addr, u8 *buf, int len) {
    int done = 0, tries = 0, tmo = 0;
    while (done < len) {
        u8 n = (u8)MIN(16, len - done);
        int r = a->xfer(a->ctx, NV_AUX_NATIVE_RD, addr + (u32)done, buf + done, &n);
        if (r == E_TIMEOUT && ++tmo < 3) continue;  /* a sink waking up may miss the first */
        if (r < 0) return r;
        if ((r & 3) == NV_AUX_NACK) return E_IO;
        if ((r & 3) == NV_AUX_DEFER || !n) {
            if (++tries >= AUX_TRIES) return E_TIMEOUT;
            aux_defer(a);
            continue;
        }
        done += n;
        tries = 0;
    }
    return done;
}

/* one piece of an I2C-over-AUX message: 0 or E_* (*got: bytes moved) */
static int aux_i2c_piece(nv_aux_t *a, u8 req, u8 addr, u8 *buf, u8 cnt, u8 *got) {
    int tries = 0, tmo = 0;
    for (;;) {
        u8 n = cnt;
        int r = a->xfer(a->ctx, req, addr, buf, &n);
        if (r == E_TIMEOUT && ++tmo < 3) continue;
        if (r < 0) return r;
        if ((r & 3) == NV_AUX_NACK || (r & 0xC) == NV_AUX_I2C_NACK) return E_IO;
        int rd = req & NV_AUX_I2C_RD;
        if ((r & 3) == NV_AUX_DEFER || (r & 0xC) == NV_AUX_I2C_DEFER || (rd && cnt && !n)) {
            if (++tries >= AUX_TRIES) return E_TIMEOUT;
            aux_defer(a);
            continue;
        }
        *got = rd ? n : cnt;                        /* a write ACK took all of it */
        return 0;
    }
}

int nv_aux_i2c_xfer(nv_aux_t *a, nv_i2c_msg_t *msgs, int n) {
    int ret = 0;
    for (int m = 0; m < n && !ret; m++) {
        nv_i2c_msg_t *g = &msgs[m];
        u8 base = (g->flags & NV_I2C_RD) ? NV_AUX_I2C_RD : NV_AUX_I2C_WR;
        u32 done = 0;
        do {
            u8 cnt = (u8)MIN(16u, (u32)g->len - done), got = 0;
            u8 req = base;
            if (m + 1 < n || g->len - done > cnt) req |= NV_AUX_I2C_MOT;
            ret = aux_i2c_piece(a, req, g->addr, g->buf + done, cnt, &got);
            done += got;
        } while (!ret && done < g->len);
    }
    if (ret) {
        /* end the I2C transaction in the sink (address only, without MOT) */
        u8 z = 0;
        a->xfer(a->ctx, NV_AUX_I2C_WR, n > 0 ? msgs[0].addr : 0x50, &z, &z);
    }
    return ret;
}

int nv_aux_i2c_fn(void *aux, nv_i2c_msg_t *msgs, int n) { return nv_aux_i2c_xfer(aux, msgs, n); }

/* ---------------- EDID over DDC ---------------- */

static int edid_block_ok(const u8 *e, int base) {
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (base && memcmp(e, hdr, 8)) return 0;
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + e[i]);
    return sum == 0;
}

static int read_block(nv_i2c_fn fn, void *ctx, int blk, u8 *out) {
    u8 seg = (u8)(blk / 2), off = (u8)((blk & 1) * 128);
    nv_i2c_msg_t m[3];
    int n = 0;
    if (seg) m[n++] = (nv_i2c_msg_t){ 0x30, 0, 1, &seg };
    m[n++] = (nv_i2c_msg_t){ 0x50, 0, 1, &off };
    m[n++] = (nv_i2c_msg_t){ 0x50, NV_I2C_RD, 128, out };
    return fn(ctx, m, n);
}

int nv_ddc_read_edid(nv_i2c_fn fn, void *ctx, u8 *buf, int max, char *note, int noten) {
    if (note && noten) note[0] = 0;
    if (max < 128) return E_INVAL;
    int r = E_IO;
    for (int t = 0; t < 3; t++) {
        r = read_block(fn, ctx, 0, buf);
        if (!r && edid_block_ok(buf, 1)) break;
        if (r == E_IO && t == 1) break;             /* NAKed twice: nobody at 0x50 */
        if (!r) r = E_INVAL;
    }
    if (r) {
        if (note) snprintf(note, (size_t)noten, "%s", r == E_IO ? "no answer at DDC address 0x50" :
                           r == E_INVAL ? "base block invalid (header or checksum)" : r == E_TIMEOUT ? "timed out" :
                           r == E_BUSY ? "bus stuck" : r == E_NODEV ? "no sink" : "error");
        return r;
    }
    int blocks = 1 + buf[126];
    if (blocks > max / 128) {
        if (note) snprintf(note, (size_t)noten, "%d extension blocks, %d kept", blocks - 1, max / 128 - 1);
        blocks = max / 128;
    }
    for (int b = 1; b < blocks; b++) {
        int ok = 0, e = 0;
        for (int t = 0; t < 3 && !ok; t++) {
            e = read_block(fn, ctx, b, buf + b * 128);
            ok = !e && edid_block_ok(buf + b * 128, 0);
        }
        if (!ok) {
            if (note) snprintf(note, (size_t)noten, "extension block %d: %s", b, e ? strerror(e) : "bad checksum");
            return b * 128;
        }
    }
    return blocks * 128;
}
