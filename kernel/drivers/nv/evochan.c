/* Halcyon OS - NVIDIA GPUs (M10d): page flips, the hardware cursor, the
 * vertical blank and several heads on the EVO display engine (Tesla to
 * Pascal), without the kernel. See evo.h ("10d") and
 * docs/DESIGN-M10-NVIDIA.md, "10d".
 *
 * Ported from nouveau (MIT; Copyright 2012-2020 Red Hat Inc., authors Ben
 * Skeggs and others; the notice is in THIRD_PARTY_NOTICES.md):
 *   - dispnv50/base507c.c, base827c.c, base907c.c, wndw.c: the base
 *     channel's methods (notifier, image, LUT and CSC off, update)
 *   - dispnv50/curs507a.c: the cursor channel (FREE, position, update)
 *   - dispnv50/head507d.c, head827d.c, head907d.c, head917d.c: the cursor
 *     and the base usage bounds in the core channel
 *   - nvkm/engine/disp/nv50.c, gf119.c, gp102.c: channel ids, dmac / pioc
 *     init and fini, error reporting; head rgpos (the scanline position)
 *   - nvkm/engine/disp/outp.c: choosing an output resource
 *   - include/nvhw/class/cl507a.h, cl507c.h, cl827c.h, cl907c.h (NVIDIA,
 *     MIT): method and field layouts
 *
 * Everything is polled: a flip is done when its notifier in the sync page
 * says BEGUN (or FINISHED) and the channel fetched every word (GET = PUT);
 * the vertical blank is read from the scanline counter. Every wait is
 * bounded; a channel that does not start, a notifier that is never written,
 * a scanline counter that does not move or a cursor channel that never has
 * room all end in an error the driver turns into the 10b behaviour
 * (single buffering, the software cursor). */
#include "evo.h"
#include "core/lib.h"

#define M nv_evo_mthd

static u32 rd(nv_evo_t *e, u32 reg) { return e->r->rd32(e->r->ctx, reg); }
static void wr(nv_evo_t *e, u32 reg, u32 v) { e->r->wr32(e->r->ctx, reg, v); }
static u32 mask(nv_evo_t *e, u32 reg, u32 m, u32 v) {
    u32 o = rd(e, reg);
    wr(e, reg, (o & ~m) | v);
    return o;
}
static u64 now(nv_evo_t *e) { return e->r->now_us(e->r->ctx); }
static void delay(nv_evo_t *e, u32 us) { if (e->r->delay_us) e->r->delay_us(e->r->ctx, us); }

static int vram_ok(const nv_evo_t *e, u64 off, u64 n) {
    return e->vram && off >= e->vram_base && off + n <= e->vram_base + e->vram_size && off + n >= off;
}
static void vwr32(nv_evo_t *e, u64 off, u32 v) {
    if (vram_ok(e, off, 4)) *(volatile u32 *)(e->vram + (off - e->vram_base)) = v;
}
static u32 vrd32(nv_evo_t *e, u64 off) {
    return vram_ok(e, off, 4) ? *(volatile u32 *)(e->vram + (off - e->vram_base)) : 0xFFFFFFFFu;
}

static void cfail(nv_evo_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cfail(nv_evo_t *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
    nv_evo_log(e, "%s", e->why);
}

static u32 wh(u32 w, u32 h) { return (h << 16) | (w & 0xFFFF); }

/* ---------------- classes and channel ids ---------------- */

u16 nv_evo_base_class(const nv_evo_gen_t *g) {
    switch (g->kind) {
    case EVO_NV50: return 0x507c;
    case EVO_G84: return 0x827c;
    case EVO_G94: case EVO_GT200: case EVO_MCP77: return 0x837c;
    case EVO_GT215: case EVO_MCP89: return 0x857c;
    case EVO_GF119: return 0x907c;
    case EVO_GK104: return 0x917c;
    default: return 0x927c;                          /* GK110 to GP102 (nouveau: GK110_DISP_BASE_CHANNEL_DMA) */
    }
}

u16 nv_evo_curs_class(const nv_evo_gen_t *g) {
    switch (g->kind) {
    case EVO_NV50: return 0x507a;
    case EVO_G84: case EVO_G94: case EVO_GT200: case EVO_MCP77: return 0x827a;
    case EVO_GT215: case EVO_MCP89: return 0x857a;
    case EVO_GF119: return 0x907a;
    default: return 0x917a;                          /* GK104 to GP102 (GK104_DISP_CURSOR) */
    }
}

void nv_evo_chan_ids(const nv_evo_gen_t *g, int kind, int head, int *ctrl, int *user) {
    if (kind == EVO_CHAN_BASE) { *ctrl = *user = 1 + head; return; }       /* nv50 / gf119 / gp102_disp_base */
    if (!g->gf119) { *ctrl = *user = 7 + head; return; }                   /* nv50_disp_curs */
    *ctrl = 13 + head;                                                      /* gf119_disp_curs */
    *user = (g->push_gp102 ? 17 : 13) + head;                               /* gp102_disp_curs: user 17 */
}

/* ---------------- method streams ---------------- */

/* the base method layouts: 507c (G80), 827c (G84-GF116: 827c / 837c / 857c), 907c (GF119+: 907c / 917c / 927c) */
static int base_907c(const nv_evo_gen_t *g) { return g->mthd >= MTHD_907D; }

void nv_evo_base_init_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s) {
    if (base_907c(g)) {
        /* base907c_xlut_clr, base907c_csc_clr */
        M(s, 0x00E0, 1, 0u);                         /* SET_BASE_LUT_LO: disable */
        M(s, 0x00E8, 1, 0u);                         /* SET_OUTPUT_LUT_LO: disable */
        M(s, 0x00FC, 1, 0u);                         /* SET_CONTEXT_DMA_LUT */
        M(s, 0x0140, 1, 0u);                         /* SET_CSC_RED2RED: owner core */
    } else {
        M(s, 0x00E0, 1, 0u);                         /* base507c_xlut_clr */
    }
}

void nv_evo_base_image_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, u32 ntfy, u32 sync_handle, u32 iso_handle, u64 offset,
                               int w, int h, u32 pitch) {
    /* base507c_ntfy_set: mode WRITE, the notifier's offset in the sync page */
    M(s, 0x00A0, 2, ntfy & 0xFFCu, sync_handle);
    if (!iso_handle) {
        /* base507c_image_clr: NON_TEARING, interval 0, no surface */
        M(s, 0x0084, 1, 0u);
        M(s, 0x00C0, 1, 0u);
    } else {
        /* SET_PRESENT_CONTROL: BEGIN_MODE NON_TEARING, MIN_PRESENT_INTERVAL 1 (at the next vertical blank) */
        M(s, 0x0084, 1, 0x00000010u);
        M(s, 0x00C0, 1, iso_handle);                 /* SET_CONTEXT_DMA(S)_ISO(0) */
        u32 size = wh((u32)w, (u32)h);
        if (base_907c(g)) {
            /* base907c_image_set: SURFACE_SET_OFFSET(0,0..1), SIZE, STORAGE (pitch 20:8, PITCH layout 24), PARAMS A8R8G8B8 */
            M(s, 0x0400, 5, (u32)(offset >> 8), 0u, size, 0x01000000u | (((pitch >> 8) & 0x1FFF) << 8), 0x0000CF00u);
        } else if (g->kind == EVO_NV50) {
            /* base507c_image_set: SET_PROCESSING / CONVERSION off, OFFSET(0,0), SIZE, STORAGE (pitch 17:8, layout 20), PARAMS (kind pitch) */
            M(s, 0x0110, 2, 0u, 0u);
            M(s, 0x0800, 1, (u32)(offset >> 8));
            M(s, 0x0808, 3, size, 0x00100000u | (((pitch >> 8) & 0x3FF) << 8), 0x0000CF00u);
        } else {
            /* base827c_image_set */
            M(s, 0x0110, 2, 0u, 0u);
            M(s, 0x0800, 5, (u32)(offset >> 8), 0u, size, 0x00100000u | (((pitch >> 8) & 0x3FF) << 8), 0x0000CF00u);
        }
    }
    M(s, 0x0080, 1, 0u);                             /* UPDATE: not interlocked with the core */
}

void nv_evo_curs_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, int head, int on, u64 offset, u32 handle) {
    /* HEAD_SET_CONTROL_CURSOR: ENABLE (31), FORMAT A8R8G8B8 (24), SIZE W64_H64 (26), hot spot 0, ALPHA_BLEND */
    u32 ctl = on ? 0x85000000u : 0x05000000u;
    if (g->mthd >= MTHD_907D) {                      /* head907d / head917d_curs_set, head907d_curs_clr */
        u32 i = (u32)head * 0x300;
        if (on) M(s, 0x0480 + i, 2, ctl, (u32)(offset >> 8));
        else M(s, 0x0480 + i, 1, ctl);
        M(s, 0x048C + i, 1, on ? handle : 0u);       /* HEAD_SET_CONTEXT_DMA_CURSOR */
        return;
    }
    u32 i = (u32)head * 0x400;
    if (on) M(s, 0x0880 + i, 2, ctl, (u32)(offset >> 8));
    else M(s, 0x0880 + i, 1, ctl);
    if (g->mthd == MTHD_827D) M(s, 0x089C + i, 1, on ? handle : 0u);   /* head827d: the cursor's context DMA */
}

u32 nv_evo_base_bounds(const nv_evo_gen_t *g, int usable) {
    if (!usable) return 0;
    /* USABLE, PIXEL_DEPTH BPP_32 (11:8 = 3); 917d also BASE_LUT USAGE_1025 (17:16 = 2) */
    return g->mthd == MTHD_917D ? 0x00020301u : 0x00000301u;
}

void nv_evo_core_layer_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, int head, u64 offset, int w, int h, u32 pitch, u32 handle,
                               int base, int curs_on, u64 curs_offset) {
    if (g->mthd >= MTHD_907D) {
        u32 i = (u32)head * 0x300;
        M(s, 0x0460 + i, 1, (u32)(offset >> 8));
        M(s, 0x0468 + i, 4, wh((u32)w, (u32)h), ((pitch >> 8) << 8) | 0x01000000u, 0x0000CF00u, handle);
        M(s, 0x04D0 + i, 1, nv_evo_base_bounds(g, base));
    } else {
        u32 i = (u32)head * 0x400;
        M(s, 0x0860 + i, 1, (u32)(offset >> 8));
        M(s, 0x0868 + i, 4, wh((u32)w, (u32)h), ((pitch >> 8) << 8) | 0x00100000u, 0x0000CF00u, handle);
        M(s, 0x0900 + i, 1, nv_evo_base_bounds(g, base));
    }
    /* EVO complains (INVALID_STATE) when the ISO surface is specified again
     * with an active cursor and no HEAD_SET_OFFSET_CURSOR (head507d_core_set) */
    if (curs_on) nv_evo_curs_methods(g, s, head, 1, curs_offset, handle);
}

/* ---------------- channel control ---------------- */

static u32 ctrl_reg(nv_evo_t *e, int ctrl) { return nv_evo_core_ctrl_reg(e->g) + (u32)ctrl * 0x10; }
static u32 push_reg(nv_evo_t *e, int ctrl) {
    return (e->g->push_gp102 ? 0x611494u : e->g->gf119 ? 0x610494u : 0x610204u) + (u32)ctrl * 0x10;
}
static u32 user_reg(int user, u32 off) { return 0x640000u + (u32)user * 0x1000 + off; }

/* nv50_disp_chan_intr / gf119_disp_chan_intr: the channel's error reporting
 * (its status bit is polled; the GPU's interrupts stay masked) */
static void chan_intr(nv_evo_t *e, int user, int on) {
    if (e->g->gf119) {
        nv_evo_save(e, 0x6100a0);
        mask(e, 0x6100a0, 1u << user, on ? 1u << user : 0);
        if (!on) { nv_evo_save(e, 0x610090); mask(e, 0x610090, 1u << user, 0); }
    } else {
        nv_evo_save(e, 0x610028);
        mask(e, 0x610028, 0x00010001u << user, on ? 0x00010000u << user : 0);
    }
}

/* nv50_disp_dmac_fini / gf119_disp_dmac_fini */
static int dmac_fini(nv_evo_t *e, int ctrl) {
    u32 c = ctrl_reg(e, ctrl), last;
    mask(e, c, 0x00001010, 0x00001000);
    mask(e, c, 0x00000003, 0x00000000);
    if (nv_evo_wait(e, c, 0x001e0000, 0, e->chan_us, &last)) {
        nv_evo_log(e, "channel %d did not stop: %08x", ctrl, last);
        return E_TIMEOUT;
    }
    return 0;
}

/* nv50_disp_pioc_fini / gf119_disp_pioc_fini */
static int pioc_fini(nv_evo_t *e, int ctrl) {
    u32 c = ctrl_reg(e, ctrl), last;
    mask(e, c, 0x00000001, 0x00000000);
    if (nv_evo_wait(e, c, 0x00030000, 0, e->chan_us, &last)) {
        nv_evo_log(e, "channel %d did not stop: %08x", ctrl, last);
        return E_TIMEOUT;
    }
    return 0;
}

static int head_ok(const nv_evo_t *e, int head) {
    return head >= 0 && head < EVO_HEADS && head < e->nheads && (e->head_mask & (1u << head));
}

/* ---------------- the base channel ---------------- */

static int base_wait_idle(nv_evo_t *e, nv_evo_chan_t *ch, u32 us) {
    u64 end = now(e) + us;
    for (;;) {
        if (nv_evo_chan_error(e, ch->user, "base")) { ch->errors++; return E_IO; }
        u32 get = rd(e, user_reg(ch->user, 4));
        if (get == ch->put) return 0;
        if (nv_bad_read(get) || now(e) >= end) { e->timeouts++; return E_TIMEOUT; }
        delay(e, 10);
    }
}

int nv_evo_base_start(nv_evo_t *e, int head) {
    if (!e->chan_us) e->chan_us = 200000;
    if (!head_ok(e, head)) { cfail(e, "base channel: no head %d", head); return E_INVAL; }
    nv_evo_chan_t *ch = &e->base[head];
    if (ch->up) return 0;
    if (!e->chan_up) { cfail(e, "base channel %d: the core channel is not running", head); return E_IO; }
    u64 push = e->basepush + (u64)head * 0x1000;
    if (!e->basepush || (push & 0xFFF) || !vram_ok(e, push, 0x1000) || !vram_ok(e, e->sync, 0x1000)) {
        cfail(e, "base channel %d: no push buffer in the VRAM window", head);
        return E_INVAL;
    }
    nv_evo_chan_ids(e->g, EVO_CHAN_BASE, head, &ch->ctrl, &ch->user);
    /* the notifier's and the surfaces' context DMAs, bound to this channel (nv50_dmac_create) */
    u32 obj = nv_evo_base_obj(head);
    int r = nv_evo_bind(e, obj, EVO_HANDLE_SYNC, ch->user, e->sync, e->sync + 0xFFF);
    if (!r) r = nv_evo_bind(e, obj + 0x20, EVO_HANDLE_VRAM, ch->user, 0, (e->vram_total ? e->vram_total : e->vram_base + e->vram_size) - 1);
    if (r) { cfail(e, "base channel %d: its context DMAs do not fit the object table (%d)", head, r); return r; }
    for (u32 i = 0; i < 0x1000; i += 4) vwr32(e, push + i, 0);
    for (int sl = 0; sl < 2; sl++) { vwr32(e, e->sync + nv_evo_base_ntfy(head, sl), 0); vwr32(e, e->sync + nv_evo_base_ntfy(head, sl) + 4, 0); }
    if ((r = nv_evo_bar_flush(e))) { cfail(e, "base channel %d: BAR flush timed out", head); return r; }
    u32 c = ctrl_reg(e, ch->ctrl), v = rd(e, c), last;
    if (nv_bad_read(v)) { cfail(e, "base channel %d: control register %06x unreadable (%08x)", head, c, v); return E_NODEV; }
    if (v & 3) {
        nv_evo_log(e, "base channel %d is active (%08x): stopping it first", head, v);
        if (dmac_fini(e, ch->ctrl)) { cfail(e, "base channel %d: the firmware's channel did not stop", head); return E_TIMEOUT; }
    }
    chan_intr(e, ch->user, 1);
    /* nv50_disp_dmac_init / gf119_disp_dmac_init / gp102_disp_dmac_init */
    u32 p = push_reg(e, ch->ctrl);
    wr(e, p, 0x00000001u | (u32)(push >> 8));        /* VRAM */
    wr(e, p + 4, 0x00010000);
    wr(e, p + 8, e->g->gf119 ? 0x00000001u : (u32)ch->ctrl);
    mask(e, c, 0x00000010, 0x00000010);
    wr(e, user_reg(ch->ctrl, 0), 0);                 /* PUT */
    wr(e, c, 0x00000013);
    if (nv_evo_wait(e, c, 0x80000000, 0, e->chan_us, &last)) {
        cfail(e, "base channel %d did not start (%06x = %08x)", head, c, last);
        dmac_fini(e, ch->ctrl);
        chan_intr(e, ch->user, 0);
        return E_TIMEOUT;
    }
    ch->up = 1;
    ch->put = 0;
    ch->pending = ch->shown = ch->slot = 0;
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_base_init_methods(e->g, &s);
    r = nv_evo_kick(e, push, &ch->put, ch->user, &s);
    if (!r) r = base_wait_idle(e, ch, e->chan_us);
    if (r) {
        cfail(e, "base channel %d does not fetch its commands (GET %x, PUT %x)", head, rd(e, user_reg(ch->user, 4)), ch->put);
        nv_evo_base_stop(e, head);
        return r ? r : E_IO;
    }
    nv_evo_log(e, "base channel %d (class %04x, ids %d/%d) running for head %d: push buffer at %llx, notifiers at %x/%x", ch->ctrl,
               nv_evo_base_class(e->g), ch->ctrl, ch->user, head, (unsigned long long)push, nv_evo_base_ntfy(head, 0),
               nv_evo_base_ntfy(head, 1));
    return 0;
}

void nv_evo_base_stop(nv_evo_t *e, int head) {
    if (head < 0 || head >= EVO_HEADS) return;
    nv_evo_chan_t *ch = &e->base[head];
    if (!ch->up) return;
    if (!e->chan_us) e->chan_us = 200000;
    dmac_fini(e, ch->ctrl);
    chan_intr(e, ch->user, 0);
    ch->up = ch->pending = ch->shown = 0;
    nv_evo_log(e, "base channel %d stopped (%u images shown, %u errors)", ch->ctrl, ch->flips, ch->errors);
}

static int base_queue(nv_evo_t *e, int head, u32 iso, u64 offset, int w, int h, u32 pitch) {
    nv_evo_chan_t *ch = &e->base[head];
    ch->slot ^= 1;
    u32 ntfy = nv_evo_base_ntfy(head, ch->slot);
    vwr32(e, e->sync + ntfy, 0);                     /* base507c_ntfy_reset: NOT_BEGUN */
    vwr32(e, e->sync + ntfy + 4, 0);
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_base_image_methods(e->g, &s, ntfy, EVO_HANDLE_SYNC, iso, offset, w, h, pitch);
    int r = nv_evo_kick(e, e->basepush + (u64)head * 0x1000, &ch->put, ch->user, &s);
    if (r) return r;
    ch->ntfy = ntfy;
    ch->pending = iso ? 1 : 2;
    ch->queued_us = now(e);
    return 0;
}

int nv_evo_base_flip(nv_evo_t *e, int head, u64 offset, int w, int h, u32 pitch) {
    if (!head_ok(e, head) || !e->base[head].up || !e->chan_up) return E_IO;
    if (e->base[head].pending) return E_BUSY;
    if ((offset & 0xFF) || (pitch & 0xFF) || !pitch || w <= 0 || h <= 0 || w > 0x7FFF || h > 0x7FFF) return E_INVAL;
    return base_queue(e, head, EVO_HANDLE_VRAM, offset, w, h, pitch);
}

int nv_evo_base_poll(nv_evo_t *e, int head) {
    if (!head_ok(e, head)) return E_INVAL;
    nv_evo_chan_t *ch = &e->base[head];
    if (!ch->up) return E_IO;
    if (!ch->pending) return 1;
    if (nv_evo_chan_error(e, ch->user, "base")) { ch->errors++; ch->pending = 0; return E_IO; }
    /* NV_DISP_BASE_NOTIFIER_1: STATUS (31:30) 1 BEGUN, 2 FINISHED (base507c_ntfy_wait_begun) */
    u32 st = vrd32(e, e->sync + ch->ntfy) >> 30;
    if (st != 1 && st != 2) return 0;
    if (rd(e, user_reg(ch->user, 4)) != ch->put) return 0;
    ch->shown = ch->pending == 1;
    ch->pending = 0;
    ch->flips++;
    return 1;
}

int nv_evo_base_off(nv_evo_t *e, int head, u32 us) {
    if (!head_ok(e, head) || !e->base[head].up) return E_IO;
    nv_evo_chan_t *ch = &e->base[head];
    u64 end = now(e) + us;
    int r;
    while ((r = nv_evo_base_poll(e, head)) == 0) {      /* an image still queued: let it finish first */
        if (now(e) >= end) { e->timeouts++; cfail(e, "base channel %d: the queued image never completed", head); return E_TIMEOUT; }
        delay(e, 20);
    }
    if (r < 0) { cfail(e, "base channel %d reported an error", head); return r; }
    if ((r = base_queue(e, head, 0, 0, 0, 0, 0))) return r;
    while ((r = nv_evo_base_poll(e, head)) == 0) {
        if (now(e) >= end) {
            e->timeouts++;
            cfail(e, "base channel %d: the base layer did not go off (notifier %08x, GET %x, PUT %x)", head, vrd32(e, e->sync + ch->ntfy),
                  rd(e, user_reg(ch->user, 4)), ch->put);
            return E_TIMEOUT;
        }
        delay(e, 20);
    }
    if (r < 0) { cfail(e, "base channel %d reported an error", head); return r; }
    return 0;
}

/* ---------------- the cursor channel ---------------- */

int nv_evo_curs_start(nv_evo_t *e, int head) {
    if (!e->chan_us) e->chan_us = 200000;
    if (!head_ok(e, head)) { cfail(e, "cursor channel: no head %d", head); return E_INVAL; }
    nv_evo_chan_t *ch = &e->curs[head];
    if (ch->up) return 0;
    if (!e->cursimg || !vram_ok(e, nv_evo_curs_offset(e, head, 1), EVO_CURS_BYTES) || (e->cursimg & 0xFF)) {
        cfail(e, "cursor channel %d: no image memory in the VRAM window", head);
        return E_INVAL;
    }
    nv_evo_chan_ids(e->g, EVO_CHAN_CURS, head, &ch->ctrl, &ch->user);
    u32 c = ctrl_reg(e, ch->ctrl), v = rd(e, c), last;
    if (nv_bad_read(v)) { cfail(e, "cursor channel %d: control register %06x unreadable (%08x)", head, c, v); return E_NODEV; }
    if (v & 1) {
        nv_evo_log(e, "cursor channel %d is active (%08x): stopping it first", head, v);
        if (pioc_fini(e, ch->ctrl)) { cfail(e, "cursor channel %d: the firmware's channel did not stop", head); return E_TIMEOUT; }
    }
    chan_intr(e, ch->user, 1);
    if (!e->g->gf119) {
        /* nv50_disp_pioc_init: out of reset first */
        wr(e, c, 0x00002000);
        if (nv_evo_wait(e, c, 0x00030000, 0, e->chan_us, &last)) {
            cfail(e, "cursor channel %d: reset did not finish (%06x = %08x)", head, c, last);
            chan_intr(e, ch->user, 0);
            return E_TIMEOUT;
        }
    }
    wr(e, c, 0x00000001);
    if (nv_evo_wait(e, c, 0x00030000, 0x00010000, e->chan_us, &last)) {
        cfail(e, "cursor channel %d did not start (%06x = %08x)", head, c, last);
        pioc_fini(e, ch->ctrl);
        chan_intr(e, ch->user, 0);
        return E_TIMEOUT;
    }
    ch->up = 1;
    ch->busy = 0;
    nv_evo_log(e, "cursor channel %d (class %04x, user %d) running for head %d, images at %llx", ch->ctrl, nv_evo_curs_class(e->g),
               ch->user, head, (unsigned long long)nv_evo_curs_offset(e, head, 0));
    return 0;
}

void nv_evo_curs_stop(nv_evo_t *e, int head) {
    if (head < 0 || head >= EVO_HEADS) return;
    nv_evo_chan_t *ch = &e->curs[head];
    if (!ch->up) return;
    if (!e->chan_us) e->chan_us = 200000;
    pioc_fini(e, ch->ctrl);
    chan_intr(e, ch->user, 0);
    ch->up = 0;
    nv_evo_log(e, "cursor channel %d stopped (%u moves dropped)", ch->ctrl, ch->busy);
}

int nv_evo_curs_move(nv_evo_t *e, int head, int x, int y) {
    if (!head_ok(e, head) || !e->curs[head].up) return E_IO;
    nv_evo_chan_t *ch = &e->curs[head];
    /* curs507a_space: FREE COUNT (5:0) >= 4, at most 1 ms (nouveau: 100 ms) */
    u64 end = now(e) + 1000;
    for (;;) {
        u32 f = rd(e, user_reg(ch->user, 0x08));
        if (nv_bad_read(f)) return E_IO;
        if ((f & 0x3F) >= 4) break;
        if (now(e) >= end) { ch->busy++; return E_BUSY; }
        delay(e, 2);
    }
    wr(e, user_reg(ch->user, 0x84), nv_evo_curs_point(x, y));   /* SET_CURSOR_HOT_SPOT_POINT_OUT */
    wr(e, user_reg(ch->user, 0x80), 0);                          /* UPDATE, not interlocked with the core */
    return 0;
}

u64 nv_evo_curs_offset(const nv_evo_t *e, int head, int slot) {
    return e->cursimg + (u64)head * 0x8000 + (u64)(slot & 1) * 0x4000;
}

int nv_evo_curs_upload(nv_evo_t *e, int head, int slot, const u32 *argb, int size) {
    if (head < 0 || head >= EVO_HEADS || !argb || size <= 0 || size > EVO_CURS_SIZE) return E_INVAL;
    u64 at = nv_evo_curs_offset(e, head, slot);
    if (!e->cursimg || !vram_ok(e, at, EVO_CURS_BYTES)) return E_INVAL;
    for (int y = 0; y < EVO_CURS_SIZE; y++)
        for (int x = 0; x < EVO_CURS_SIZE; x++)
            vwr32(e, at + (u64)(y * EVO_CURS_SIZE + x) * 4, x < size && y < size ? argb[y * size + x] : 0);
    if (vrd32(e, at) != argb[0]) return E_IO;
    return nv_evo_bar_flush(e);
}

void nv_evo_chans_stop(nv_evo_t *e) {
    for (int h = 0; h < EVO_HEADS; h++) {
        if (e->base[h].up) {
            if ((e->base[h].shown || e->base[h].pending) && e->chan_up && nv_evo_base_off(e, h, 50000))
                nv_evo_log(e, "head %d: the base layer could not be switched off before the channel stops", h);
            nv_evo_base_stop(e, h);
        }
        nv_evo_curs_stop(e, h);
    }
}

/* ---------------- the vertical blank ---------------- */

static int raster(nv_evo_t *e, int head, nv_evo_head_state_t *hs) {
    if (!head_ok(e, head)) return E_INVAL;
    if (nv_bad_read(rd(e, 0x616340 + (u32)head * 0x800))) return E_IO;
    nv_evo_head_state(e, head, 0, hs);
    if (!hs->hz || !hs->htotal || !hs->vtotal || hs->vblanks >= hs->vtotal || hs->vblanke >= hs->vblanks) return E_NOTSUP;
    return 0;
}

static int in_blank(nv_evo_t *e, int head, const nv_evo_head_state_t *hs, u32 *vline) {
    /* nv50_head_rgpos: the vertical position (reading it latches the horizontal one) */
    u32 v = rd(e, 0x616340 + (u32)head * 0x800);
    if (nv_bad_read(v)) return E_IO;
    v &= 0xFFFF;
    if (vline) *vline = v;
    return v >= hs->vblanks || v <= hs->vblanke;
}

int nv_evo_in_vblank(nv_evo_t *e, int head, u32 *vline) {
    nv_evo_head_state_t hs;
    int r = raster(e, head, &hs);
    return r ? r : in_blank(e, head, &hs, vline);
}

int nv_evo_wait_vblank(nv_evo_t *e, int head, u32 max_us) {
    nv_evo_head_state_t hs;
    int r = raster(e, head, &hs);
    if (r) return r;
    u64 line_ns = (u64)hs.htotal * 1000000000ull / hs.hz;
    u64 frame_us = line_ns * hs.vtotal / 1000 + 1;
    u64 lim = MIN((u64)max_us, 2 * frame_us + 2000);
    u64 t = now(e), end = t + lim, changed = t, still = 4 * line_ns / 1000 + 1000;
    u32 line, last;
    int in = in_blank(e, head, &hs, &last), leaving = in == 1;
    if (in < 0) return in;
    for (;;) {
        delay(e, 2);
        in = in_blank(e, head, &hs, &line);
        if (in < 0) return in;
        if (leaving && !in) leaving = 0;
        else if (!leaving && in) return 0;
        t = now(e);
        if (line != last) { last = line; changed = t; }
        else if (t - changed > still) {
            nv_evo_log(e, "head %d: the scanline counter stays at %u", head, line);
            return E_TIMEOUT;
        }
        if (t >= end) return E_TIMEOUT;
    }
}

u32 nv_evo_us_to_vblank(nv_evo_t *e, int head) {
    nv_evo_head_state_t hs;
    u32 line;
    if (raster(e, head, &hs)) return 0;
    int in = in_blank(e, head, &hs, &line);
    if (in) return 0;
    u64 line_ns = (u64)hs.htotal * 1000000000ull / hs.hz;
    return (u32)((u64)(hs.vblanks - line) * line_ns / 1000);
}

/* ---------------- several heads: output resources and limits ---------------- */

static const nv_dcb_output_t *dcb_out(const nv_evo_t *e, int k) {
    return e->bios && k >= 0 && k < e->bios->noutputs ? &e->bios->out[k] : NULL;
}
static int ctz8(u32 m) { return m ? __builtin_ctz(m) : -1; }

static int used_by(const nv_evo_use_t use[EVO_HEADS], int head, int type, int id) {
    for (int h = 0; h < EVO_HEADS; h++)
        if (h != head && use[h].on && use[h].or_type == type && use[h].or_id == id) return h;
    return -1;
}

int nv_evo_pick_or(const nv_evo_t *e, const nv_evo_use_t use[EVO_HEADS], int k, int head, int *or_type, char *why, int whyn) {
    const nv_dcb_output_t *o = dcb_out(e, k);
    if (whyn > 0) why[0] = 0;
    if (!o) { snprintf(why, (size_t)whyn, "no DCB output %d", k); return E_INVAL; }
    if (!head_ok(e, head)) { snprintf(why, (size_t)whyn, "the display engine has no head %d", head); return E_INVAL; }
    if (!(o->heads & (1u << head))) { snprintf(why, (size_t)whyn, "DCB output %d cannot use head %d (heads %x)", k, head, o->heads); return E_INVAL; }
    int h;
    if (o->type == NV_OUT_CRT || o->type == NV_OUT_TV) {
        int id = ctz8(o->or_mask);
        *or_type = 1;
        if (!e->ndacs || id < 0 || id >= e->ndacs || id >= EVO_ORS || !(e->dac_mask & (1u << id))) {
            snprintf(why, (size_t)whyn, "DCB output %d: DAC%d does not exist on this chip", k, id);
            return E_NODEV;
        }
        if ((h = used_by(use, head, 1, id)) >= 0) {
            snprintf(why, (size_t)whyn, "DAC%d already drives DCB output %d on head %d", id, use[h].dcb, h);
            return E_NOSPC;
        }
        return id;
    }
    if (o->type != NV_OUT_TMDS && o->type != NV_OUT_DP && o->type != NV_OUT_LVDS) {
        snprintf(why, (size_t)whyn, "DCB output %d: not a SOR output", k);
        return E_INVAL;
    }
    *or_type = 0;
    if (!e->g->route) {
        int id = ctz8(o->or_mask);
        if (id < 0 || id >= e->nsors || id >= EVO_ORS || !(e->sor_mask & (1u << id))) {
            snprintf(why, (size_t)whyn, "DCB output %d: SOR%d does not exist on this chip", k, id);
            return E_NODEV;
        }
        if ((h = used_by(use, head, 0, id)) >= 0) {
            snprintf(why, (size_t)whyn, "SOR%d already drives DCB output %d on head %d (the outputs share it)", id, use[h].dcb, h);
            return E_NOSPC;
        }
        return id;
    }
    /* GM200+: SORs are routed to pad macros; a macro's sub-link carries one output */
    int m = ctz8(o->or_mask);
    for (h = 0; h < EVO_HEADS; h++) {
        if (h == head || !use[h].on || use[h].or_type) continue;
        const nv_dcb_output_t *q = dcb_out(e, use[h].dcb);
        if (q && ctz8(q->or_mask) == m && (q->link & o->link)) {
            snprintf(why, (size_t)whyn, "pad macro %d link %x already carries DCB output %d on head %d", m, q->link & o->link, use[h].dcb, h);
            return E_NOSPC;
        }
    }
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if ((e->sor_mask & (1u << i)) && e->sor_outp[i] == k && used_by(use, head, 0, i) < 0) return i;
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if ((e->sor_mask & (1u << i)) && e->sor_outp[i] < 0 && !e->sor_arm[i].heads && used_by(use, head, 0, i) < 0) return i;
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if ((e->sor_mask & (1u << i)) && used_by(use, head, 0, i) < 0) return i;
    snprintf(why, (size_t)whyn, "no free SOR (SORs %02x, all in use)", e->sor_mask);
    return E_NOSPC;
}

u32 nv_evo_head_max_khz(const nv_evo_gen_t *g) {
    u32 m = g->dac_khz;
    if (g->hdmi_khz > m) m = g->hdmi_khz;
    if (2 * g->tmds_khz > m) m = 2 * g->tmds_khz;
    u32 dp = (u32)g->dp_max_bw * 36000u;              /* 4 lanes x rate x 8/10 at 24 bpp (nv_dp_fits) */
    return dp > m ? dp : m;
}

int nv_evo_bw_check(const nv_evo_t *e, const nv_evo_use_t use[EVO_HEADS], int head, u32 khz, char *why, int whyn) {
    u32 max = nv_evo_head_max_khz(e->g);
    if (whyn > 0) why[0] = 0;
    if (khz > max) {
        snprintf(why, (size_t)whyn, "%u kHz is more than a %s head drives (%u kHz)", khz, e->g->name, max);
        return E_NOSPC;
    }
    u64 total = khz;
    int heads = 0;
    for (int h = 0; h < EVO_HEADS; h++) {
        if (e->head_mask & (1u << h)) heads++;
        if (h != head && use[h].on) total += use[h].khz;
    }
    if (total > (u64)max * (u64)(heads ? heads : 1)) {
        snprintf(why, (size_t)whyn, "the heads together would need %llu kHz (at most %llu)", (unsigned long long)total,
                 (unsigned long long)max * (u64)heads);
        return E_NOSPC;
    }
    return 0;
}

int nv_evo_plan(const nv_evo_t *e, nv_evo_plan_t *p, int n, u32 head_mask) {
    nv_evo_use_t use[EVO_HEADS];
    memset(use, 0, sizeof use);
    int lit = 0;
    for (int i = 0; i < n; i++) {
        p[i].head = p[i].or_id = p[i].or_type = -1;
        p[i].err = 0;
        p[i].why[0] = 0;
        const nv_dcb_output_t *o = dcb_out(e, p[i].dcb);
        if (!o) { p[i].err = E_INVAL; snprintf(p[i].why, sizeof p[i].why, "no DCB output %d", p[i].dcb); continue; }
        int free_head = 0;
        for (int h = 0; h < EVO_HEADS && p[i].head < 0; h++) {
            if (!(head_mask & e->head_mask & (1u << h)) || use[h].on || !(o->heads & (1u << h))) continue;
            free_head = 1;
            int t = 0, id = nv_evo_pick_or(e, use, p[i].dcb, h, &t, p[i].why, sizeof p[i].why);
            if (id < 0) { p[i].err = id; break; }    /* (a resource conflict is the same on every head) */
            int r = nv_evo_bw_check(e, use, h, p[i].khz, p[i].why, sizeof p[i].why);
            if (r) { p[i].err = r; break; }
            if (e->bios) {
                /* the head's video PLL must be in the VBIOS PLL table when the table has its VPLL0 */
                nv_pll_limits_t pl;
                if (!nv_pll_limits(e->bios, e->chipset, NV_PLL_VPLL0, 27000, &pl) &&
                    nv_pll_limits(e->bios, e->chipset, (u8)(NV_PLL_VPLL0 + h), 27000, &pl)) {
                    snprintf(p[i].why, sizeof p[i].why, "the video BIOS has no limits for VPLL%d", h);
                    p[i].err = E_NOENT;
                    continue;
                }
            }
            p[i].err = 0;
            p[i].why[0] = 0;
            p[i].head = h;
            p[i].or_type = t;
            p[i].or_id = id;
            use[h].on = 1;
            use[h].dcb = p[i].dcb;
            use[h].or_type = t;
            use[h].or_id = id;
            use[h].khz = p[i].khz;
            lit++;
        }
        if (p[i].head < 0 && !p[i].err) {
            p[i].err = E_NOSPC;
            snprintf(p[i].why, sizeof p[i].why, free_head ? "no head can drive it" : "no free head for DCB output %d", p[i].dcb);
        }
    }
    return lit;
}
