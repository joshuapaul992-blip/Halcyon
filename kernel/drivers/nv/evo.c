/* Halcyon OS - NVIDIA GPUs (M10b): the EVO display engine without the
 * kernel: generations, method streams, the object table, the VRAM we use,
 * the core channel, the supervisor. See evo.h; ported from nouveau (MIT,
 * Copyright 2012-2016 Red Hat Inc.; the notice is in THIRD_PARTY_NOTICES.md):
 * nvkm/engine/disp/nv50.c, gf119.c, gp102.c, g94.c, gm200.c, outp.c,
 * nvkm/engine/dma/user*.c, nvkm/core/ramht.c and dispnv50/core*.c,
 * head*.c, sor*.c, dac*.c.
 *
 * Every wait is bounded (now_us); every failure says why in e->why and
 * the log. Nothing here assumes the hardware answers sensibly: a register
 * that reads all ones or a PRI error, a channel that never fetches, a
 * supervisor that never comes, a notifier that is never written all end
 * in an error, never a loop. */
#include "evo.h"
#include "core/lib.h"

/* ---------------- generations ---------------- */

/*                                kind        name     class   mthd       gf119 gp102 sor_state      dp            pll            route war dac heads sors dpbw  tmds    hdmi    dac     max_w max_h */
static const nv_evo_gen_t gens[] = {
    { EVO_NV50,  "NV50",  0x507d, MTHD_507D, 0, 0, EVO_SOR_NV50,  EVO_DP_NONE,  EVO_PLL_NV50,  0, 0, 1, 2, 2, 0x00, 165000, 165000, 400000, 4096, 4096 },
    { EVO_G84,   "G84",   0x827d, MTHD_827D, 0, 0, EVO_SOR_NV50,  EVO_DP_NONE,  EVO_PLL_NV50,  0, 0, 1, 2, 2, 0x00, 165000, 165000, 400000, 4096, 4096 },
    { EVO_G94,   "G94",   0x887d, MTHD_827D, 0, 0, EVO_SOR_G94,   EVO_DP_G94,   EVO_PLL_NV50,  0, 1, 1, 2, 4, 0x0a, 165000, 165000, 400000, 4096, 4096 },
    { EVO_GT200, "GT200", 0x837d, MTHD_827D, 0, 0, EVO_SOR_NV50,  EVO_DP_NONE,  EVO_PLL_NV50,  0, 0, 1, 2, 2, 0x00, 165000, 165000, 400000, 4096, 4096 },
    { EVO_GT215, "GT215", 0x857d, MTHD_827D, 0, 0, EVO_SOR_G94,   EVO_DP_G94,   EVO_PLL_GT215, 0, 0, 1, 2, 4, 0x0a, 165000, 165000, 400000, 4096, 4096 },
    { EVO_MCP77, "MCP77", 0x887d, MTHD_827D, 0, 0, EVO_SOR_G94,   EVO_DP_G94,   EVO_PLL_NV50,  0, 0, 1, 2, 4, 0x0a, 165000, 165000, 400000, 4096, 4096 },
    { EVO_MCP89, "MCP89", 0x857d, MTHD_827D, 0, 0, EVO_SOR_G94,   EVO_DP_MCP89, EVO_PLL_GT215, 0, 0, 1, 2, 4, 0x0a, 165000, 165000, 400000, 4096, 4096 },
    { EVO_GF119, "GF119", 0x907d, MTHD_907D, 1, 0, EVO_SOR_GF119, EVO_DP_GF119, EVO_PLL_GF100, 0, 0, 1, 4, 8, 0x14, 165000, 225000, 400000, 8192, 8192 },
    { EVO_GK104, "GK104", 0x917d, MTHD_917D, 1, 0, EVO_SOR_GF119, EVO_DP_GF119, EVO_PLL_GF100, 0, 0, 1, 4, 8, 0x14, 165000, 297000, 400000, 8192, 8192 },
    { EVO_GK110, "GK110", 0x927d, MTHD_917D, 1, 0, EVO_SOR_GF119, EVO_DP_GF119, EVO_PLL_GF100, 0, 0, 1, 4, 8, 0x14, 165000, 297000, 400000, 8192, 8192 },
    { EVO_GM107, "GM107", 0x947d, MTHD_917D, 1, 0, EVO_SOR_GF119, EVO_DP_GM107, EVO_PLL_GF100, 0, 0, 1, 4, 8, 0x14, 165000, 297000, 400000, 8192, 8192 },
    { EVO_GM200, "GM200", 0x957d, MTHD_917D, 1, 0, EVO_SOR_GF119, EVO_DP_GM200, EVO_PLL_GF100, 1, 0, 1, 4, 8, 0x14, 165000, 340000, 400000, 8192, 8192 },
    { EVO_GP100, "GP100", 0x977d, MTHD_917D, 1, 0, EVO_SOR_GF119, EVO_DP_GM200, EVO_PLL_GF100, 1, 0, 0, 4, 8, 0x1e, 165000, 340000, 0,      8192, 8192 },
    { EVO_GP102, "GP102", 0x987d, MTHD_917D, 1, 1, EVO_SOR_GF119, EVO_DP_GM200, EVO_PLL_GF100, 1, 0, 0, 4, 8, 0x1e, 165000, 340000, 0,      8192, 8192 },
};
/* Fermi GF100-GF116 have the GT215 display with GF100 PLLs (nouveau's device table) */
static const nv_evo_gen_t gen_gf100 = { EVO_GT215, "GT215 (GF100)", 0x857d, MTHD_827D, 0, 0, EVO_SOR_G94, EVO_DP_G94, EVO_PLL_GF100,
                                        0, 0, 1, 2, 4, 0x0a, 165000, 225000, 400000, 4096, 4096 };

static const nv_evo_gen_t *gen(int kind) {
    for (size_t i = 0; i < ARRAY_SIZE(gens); i++) if (gens[i].kind == kind) return &gens[i];
    return NULL;
}

const nv_evo_gen_t *nv_evo_gen_for(u16 cs) {
    if (cs == 0x50) return gen(EVO_NV50);
    if (cs == 0x84 || cs == 0x86 || cs == 0x92) return gen(EVO_G84);
    if (cs == 0x94 || cs == 0x96 || cs == 0x98) return gen(EVO_G94);
    if (cs == 0xa0) return gen(EVO_GT200);
    if (cs == 0xa3 || cs == 0xa5 || cs == 0xa8) return gen(EVO_GT215);
    if (cs == 0xaa || cs == 0xac) return gen(EVO_MCP77);
    if (cs == 0xaf) return gen(EVO_MCP89);
    if (cs >= 0xc0 && cs <= 0xcf) return &gen_gf100;
    if (cs == 0xd7 || cs == 0xd9) return gen(EVO_GF119);
    if (cs == 0xe4 || cs == 0xe6 || cs == 0xe7) return gen(EVO_GK104);
    if (cs == 0xf0 || cs == 0xf1 || cs == 0x106 || cs == 0x108) return gen(EVO_GK110);
    if (cs == 0x117 || cs == 0x118) return gen(EVO_GM107);
    if (cs == 0x120 || cs == 0x124 || cs == 0x126) return gen(EVO_GM200);
    if (cs == 0x130) return gen(EVO_GP100);
    if (cs >= 0x132 && cs <= 0x138) return gen(EVO_GP102);
    return NULL;
}

/* ---------------- log ---------------- */

void nv_evo_log(nv_evo_t *e, const char *fmt, ...) {
    if (!e->ops.log) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->ops.log(e->ops.ctx, line);
}

static void fail(nv_evo_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(nv_evo_t *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
    nv_evo_log(e, "failed: %s", e->why);
}

/* ---------------- registers ---------------- */

static u32 rd(nv_evo_t *e, u32 reg) { return e->r->rd32(e->r->ctx, reg); }
static void wr(nv_evo_t *e, u32 reg, u32 v) { e->r->wr32(e->r->ctx, reg, v); }
static u32 mask(nv_evo_t *e, u32 reg, u32 m, u32 v) {
    u32 o = rd(e, reg);
    wr(e, reg, (o & ~m) | v);
    return o;
}
static u64 now(nv_evo_t *e) { return e->r->now_us(e->r->ctx); }
static void delay(nv_evo_t *e, u32 us) { if (e->r->delay_us) e->r->delay_us(e->r->ctx, us); }

/* wait until (reg & m) == v, at most us; 0 or E_TIMEOUT (*last: the last value) */
static int wait(nv_evo_t *e, u32 reg, u32 m, u32 v, u32 us, u32 *last) {
    u64 end = now(e) + us;
    for (;;) {
        u32 x = rd(e, reg);
        if (last) *last = x;
        if ((x & m) == v) return 0;
        if (now(e) >= end) { e->timeouts++; return E_TIMEOUT; }
        delay(e, 5);
    }
}

/* remember a register's value before the first write to it (nv_evo_release puts it back) */
static void save(nv_evo_t *e, u32 reg) {
    for (int i = 0; i < e->saved.n; i++) if (e->saved.r[i].reg == reg) return;
    if (e->saved.n >= EVO_SAVE_MAX) return;
    e->saved.r[e->saved.n].reg = reg;
    e->saved.r[e->saved.n].val = rd(e, reg);
    e->saved.n++;
}
static void swr(nv_evo_t *e, u32 reg, u32 v) { save(e, reg); wr(e, reg, v); }
static void smask(nv_evo_t *e, u32 reg, u32 m, u32 v) { save(e, reg); mask(e, reg, m, v); }

u32 nv_evo_core_ctrl_reg(const nv_evo_gen_t *g) { return g->gf119 ? 0x610490 : 0x610200; }

/* ---------------- VRAM through the CPU window ---------------- */

static int vram_ok(nv_evo_t *e, u64 off, u64 n) {
    return e->vram && off >= e->vram_base && off + n <= e->vram_base + e->vram_size && off + n >= off;
}
static void vwr32(nv_evo_t *e, u64 off, u32 v) {
    if (!vram_ok(e, off, 4)) return;
    *(volatile u32 *)(e->vram + (off - e->vram_base)) = v;
}
static u32 vrd32(nv_evo_t *e, u64 off) {
    if (!vram_ok(e, off, 4)) return 0xFFFFFFFFu;
    return *(volatile u32 *)(e->vram + (off - e->vram_base));
}

/* writes through BAR1 reach VRAM before the display engine fetches
 * (nv50_dmac_kick: 0x070000; NV50 itself: 0x00330c) */
static int bar_flush(nv_evo_t *e) {
    u32 reg = e->chipset == 0x50 ? 0x00330c : 0x070000;
    if (e->ops.wmb) e->ops.wmb(e->ops.ctx);
    wr(e, reg, 0x00000001);
    u32 last;
    if (wait(e, reg, 0x00000002, 0, 2000, &last)) {
        nv_evo_log(e, "BAR flush (%06x) did not finish: %08x", reg, last);
        return E_TIMEOUT;
    }
    return 0;
}

/* ---------------- the VRAM allocator ---------------- */

void nv_evo_vram_init(nv_evo_vram_t *v, u64 lo, u64 hi) {
    memset(v, 0, sizeof *v);
    v->lo = lo;
    v->hi = hi > lo ? hi : lo;
}

void nv_evo_vram_hole(nv_evo_vram_t *v, u64 at, u64 size, const char *what) {
    if (!size || v->nholes >= EVO_VRAM_HOLES) return;
    v->hole[v->nholes].at = at;
    v->hole[v->nholes].size = size;
    strlcpy(v->hole[v->nholes].what, what, sizeof v->hole[0].what);
    v->nholes++;
}

static int overlaps(u64 a, u64 an, u64 b, u64 bn) { return a < b + bn && b < a + an; }

u64 nv_evo_vram_alloc(nv_evo_vram_t *v, u64 size, u64 align) {
    if (!size || v->nused >= (int)ARRAY_SIZE(v->used) || !align || (align & (align - 1))) return ~0ull;
    u64 at = ALIGN_UP(v->lo, align);
    while (at + size <= v->hi && at + size > at) {
        u64 next = 0;
        for (int i = 0; i < v->nholes; i++)
            if (overlaps(at, size, v->hole[i].at, v->hole[i].size)) next = MAX(next, v->hole[i].at + v->hole[i].size);
        for (int i = 0; i < v->nused; i++)
            if (overlaps(at, size, v->used[i].at, v->used[i].size)) next = MAX(next, v->used[i].at + v->used[i].size);
        if (!next) {
            v->used[v->nused].at = at;
            v->used[v->nused].size = size;
            v->nused++;
            return at;
        }
        at = ALIGN_UP(next, align);
    }
    return ~0ull;
}

void nv_evo_vram_free(nv_evo_vram_t *v, u64 at) {
    for (int i = 0; i < v->nused; i++)
        if (v->used[i].at == at) {
            v->used[i] = v->used[--v->nused];
            return;
        }
}

/* ---------------- method streams ---------------- */

void nv_evo_mthd(nv_evo_stream_t *s, u32 m, int count, ...) {
    va_list ap;
    if (s->n + 1 + count > (int)ARRAY_SIZE(s->w) || count < 1 || count > 64) { s->overflow = 1; return; }
    s->w[s->n++] = nv_evo_hdr(m, count);
    va_start(ap, count);
    for (int i = 0; i < count; i++) s->w[s->n++] = va_arg(ap, u32);
    va_end(ap);
}
#define M nv_evo_mthd

int nv_evo_raster(const nv_evo_timing_t *t, nv_evo_raster_t *r) {
    memset(r, 0, sizeof *r);
    if (!t->clock_khz || !t->hactive || !t->vactive || t->hsync_start < t->hactive || t->hsync_end <= t->hsync_start ||
        t->htotal <= t->hsync_end || t->vsync_start < t->vactive || t->vsync_end <= t->vsync_start || t->vtotal <= t->vsync_end ||
        t->htotal > 0x7FFF || t->vtotal > 0x7FFF)
        return E_INVAL;
    /* nv50_head_atomic_check_mode: DRM modes start with the active area, the
     * hardware one unit into the sync pulse */
    r->clock_khz = t->clock_khz;
    r->htotal = t->htotal;
    r->hsynce = (u16)(t->hsync_end - t->hsync_start - 1);
    r->hblanke = (u16)(t->htotal - t->hsync_start - 1);
    r->hblanks = (u16)(r->hblanke + t->hactive);
    r->vtotal = t->vtotal;
    r->vsynce = (u16)(t->vsync_end - t->vsync_start - 1);
    r->vblanke = (u16)(t->vtotal - t->vsync_start - 1);
    r->vblanks = (u16)(r->vblanke + t->vactive);
    u64 us = (u64)(t->vtotal - t->vactive - 2) * t->htotal * 1000 / t->clock_khz;
    r->blankus = (u32)us;
    r->vblank2e = 0;
    r->vblank2s = 1;
    r->nhsync = t->nhsync;
    r->nvsync = t->nvsync;
    return 0;
}

static u32 wh(u32 w, u32 h) { return (h << 16) | (w & 0xFFFF); }

void nv_evo_core_init_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, u32 sync_handle) {
    (void)g;
    M(s, 0x0088, 1, sync_handle);                     /* SET_CONTEXT_DMA_NOTIFIER */
}

void nv_evo_update(const nv_evo_gen_t *g, nv_evo_stream_t *s, int notify) {
    (void)g;
    /* core507d_update: notifier on (offset 0, write, notify), UPDATE, notifier off */
    if (notify) M(s, 0x0084, 1, 0x80000000u);
    M(s, 0x0080, 2, 0x00000000u, 0x00000000u);
}

void nv_evo_or_off(const nv_evo_gen_t *g, nv_evo_stream_t *s, int or_type, int or_id) {
    if (g->mthd >= MTHD_907D) {
        if (or_type == 1) M(s, 0x0180 + (u32)or_id * 0x20, 1, 0u);
        else M(s, 0x0200 + (u32)or_id * 0x20, 1, 0u);
    } else {
        if (or_type == 1) M(s, 0x0400 + (u32)or_id * 0x80, 2, 0u, 0u);
        else M(s, 0x0600 + (u32)or_id * 0x40, 1, 0u);
    }
}

/* the OR control word for owner mask heads */
static void or_ctrl(const nv_evo_gen_t *g, nv_evo_stream_t *s, const nv_evo_head_cfg_t *c) {
    u32 owner = c->or_heads & 0xF;
    if (g->mthd >= MTHD_907D) {
        if (c->or_type == 1) M(s, 0x0180 + (u32)c->or_id * 0x20, 1, owner);   /* RGB_CRT */
        else M(s, 0x0200 + (u32)c->or_id * 0x20, 1, owner | ((c->proto & 0xF) << 8));
        return;
    }
    if (c->or_type == 1) {
        /* dac507d_ctrl: control and polarity */
        M(s, 0x0400 + (u32)c->or_id * 0x80, 2, owner, (u32)c->r.nhsync | ((u32)c->r.nvsync << 1));
        return;
    }
    /* sor507d_ctrl: polarities and (837d) pixel depth in the OR control */
    M(s, 0x0600 + (u32)c->or_id * 0x40, 1,
      owner | ((c->proto & 0xF) << 8) | ((u32)c->r.nhsync << 12) | ((u32)c->r.nvsync << 13) | ((u32)(c->depth & 0xF) << 16));
}

/* HEAD_SET_PROCAMP: COLOR_SPACE RGB, SAT_COS (bits 19:8) 1024 = unity
 * saturation, SAT_SINE 0, VESA range. nouveau computes the same 1024 from
 * its default colour vibrance (150) and hue (90); the firmware leaves its
 * unused heads at this value. SAT_COS 0 removes all colour. */
#define NV_EVO_PROCAMP (1024u << 8)

static void head_507d(const nv_evo_gen_t *g, nv_evo_stream_t *s, const nv_evo_head_cfg_t *c) {
    u32 i = (u32)c->head * 0x400;
    int is827 = g->mthd == MTHD_827D;
    /* clear: LUT, cursor (head507d_olut_clr / curs_clr, head827d_*) */
    if (is827) {
        M(s, 0x0840 + i, 1, 0u);
        M(s, 0x085C + i, 1, 0u);
        M(s, 0x0880 + i, 1, 0x05000000u);
        M(s, 0x089C + i, 1, 0u);
    } else {
        M(s, 0x0840 + i, 1, 0u);
        M(s, 0x0880 + i, 1, 0x05000000u);
    }
    if (!c->on) {
        M(s, 0x0874 + i, 1, 0u);                      /* core_clr: no surface */
        M(s, 0x0900 + i, 1, 0u);                      /* base bounds */
        M(s, 0x0904 + i, 1, 0x00000100u);             /* overlay bounds: BPP_16, unusable */
        return;
    }
    const nv_evo_raster_t *r = &c->r;
    u32 ow = c->out_w ? c->out_w : c->w, oh = c->out_h ? c->out_h : c->h;
    /* head507d_view */
    M(s, 0x08A4 + i, 1, 0u);
    M(s, 0x08C8 + i, 1, wh(c->w, c->h));
    M(s, 0x08D8 + i, 2, wh(ow, oh), wh(ow, oh));
    /* head507d_mode */
    M(s, 0x0804 + i, 2, 0x00800000u | (r->clock_khz & 0x3FFFFF), (u32)r->interlace << 1);
    M(s, 0x0810 + i, 7, 0u, wh(r->htotal, r->vtotal), wh(r->hsynce, r->vsynce), wh(r->hblanke, r->vblanke),
      wh(r->hblanks, r->vblanks), wh(r->vblank2s, r->vblank2e), r->blankus & 0xFFF);
    M(s, 0x082C + i, 1, 0u);
    /* core layer (head507d_core_set / head827d_core_set): pitch linear A8R8G8B8 */
    M(s, 0x0860 + i, 1, (u32)(c->offset >> 8));
    u32 storage = c->storage_raw ? c->storage_raw : ((c->pitch >> 8) << 8) | 0x00100000u;
    u32 params = is827 ? 0x0000CF00u : 0x0000CF00u;  /* 507d: kind pitch, part stride 256; 827d: X1_AA, linear gamma */
    M(s, 0x0868 + i, 4, wh(c->w, c->h), storage, params, c->vram_handle);
    M(s, 0x08C0 + i, 1, 0u);
    /* base / overlay usage bounds, dither, procamp */
    M(s, 0x0900 + i, 1, 0u);
    M(s, 0x0904 + i, 1, 0x00000100u);
    M(s, 0x08A0 + i, 1, 0u);
    M(s, 0x08A8 + i, 1, NV_EVO_PROCAMP);              /* RGB, saturation 1024 (unity): 0 is grayscale */
}

static void head_907d(const nv_evo_gen_t *g, nv_evo_stream_t *s, const nv_evo_head_cfg_t *c) {
    u32 i = (u32)c->head * 0x300;
    /* clear: LUT, cursor (head907d_olut_clr / curs_clr) */
    M(s, 0x0448 + i, 1, 0u);
    M(s, 0x045C + i, 1, 0u);
    M(s, 0x0480 + i, 1, 0x05000000u);
    M(s, 0x048C + i, 1, 0u);
    if (!c->on) {
        M(s, 0x0474 + i, 1, 0u);                      /* core_clr */
        M(s, 0x04D0 + i, 1, 0u);
        M(s, 0x04D4 + i, 1, 0x00000100u);
        return;
    }
    const nv_evo_raster_t *r = &c->r;
    u32 ow = c->out_w ? c->out_w : c->w, oh = c->out_h ? c->out_h : c->h;
    /* head907d_view */
    M(s, 0x0494 + i, 1, 0u);
    M(s, 0x04B8 + i, 1, wh(c->w, c->h));
    M(s, 0x04C0 + i, 3, wh(ow, oh), wh(ow, oh), wh(ow, oh));
    /* head907d_mode */
    M(s, 0x0410 + i, 6, 0u, wh(r->htotal, r->vtotal), wh(r->hsynce, r->vsynce), wh(r->hblanke, r->vblanke),
      wh(r->hblanks, r->vblanks), wh(r->vblank2s, r->vblank2e));
    M(s, 0x042C + i, 1, 0u);
    M(s, 0x0450 + i, 3, (r->clock_khz * 1000u) & 0x7FFFFFFFu, 0x00200000u, (r->clock_khz * 1000u) & 0x7FFFFFFFu);
    /* head907d_core_set */
    M(s, 0x0460 + i, 1, (u32)(c->offset >> 8));
    M(s, 0x0468 + i, 4, wh(c->w, c->h), c->storage_raw ? c->storage_raw : ((c->pitch >> 8) << 8) | 0x01000000u, 0x0000CF00u,
      c->vram_handle);
    M(s, 0x04B0 + i, 1, 0u);
    /* base (917d: no base channel, bounds 0) / overlay bounds, dither, procamp */
    M(s, 0x04D0 + i, 1, 0u);
    M(s, 0x04D4 + i, 1, 0x00000100u);
    M(s, (g->mthd == MTHD_917D ? 0x04A0 : 0x0490) + i, 1, 0u);
    M(s, 0x0498 + i, 1, NV_EVO_PROCAMP);              /* RGB, saturation 1024 (unity): 0 is grayscale */
    /* head907d_or: output resource (polarities, depth), control */
    M(s, 0x0404 + i, 2, ((u32)r->nhsync << 3) | ((u32)r->nvsync << 4) | ((u32)(c->depth & 0xF) << 6),
      0x31EC6000u | ((u32)c->head << 25) | (r->interlace ? 1u : 0u));
}

void nv_evo_head_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, const nv_evo_head_cfg_t *c) {
    if (g->mthd >= MTHD_907D) head_907d(g, s, c);
    else head_507d(g, s, c);
    if (c->or_id >= 0) {
        if (c->on || c->or_heads) or_ctrl(g, s, c);
        else nv_evo_or_off(g, s, c->or_type, c->or_id);
    }
}

/* ---------------- the object table and context DMAs ---------------- */

u32 nv_evo_ramht_hash(u32 handle, int chid) {
    const int bits = 9;                                  /* 0x1000 bytes: 512 entries */
    u32 hash = 0;
    while (handle) {
        hash ^= handle & ((1u << bits) - 1);
        handle >>= bits;
    }
    hash ^= (u32)chid << (bits - 4);
    return hash & ((1u << bits) - 1);
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int nv_evo_dmaobj(const nv_evo_gen_t *g, u8 *inst, u32 obj, u32 handle, u64 start, u64 limit) {
    if (obj < EVO_RAMHT_BYTES || obj + 24 > EVO_INST_BYTES || (obj & 15)) return E_INVAL;
    u8 *o = inst + obj;
    memset(o, 0, 24);
    u32 context;
    if (g->gf119) {
        /* gf119_dmaobj: kind pitch, small pages, VRAM */
        put32(o + 0x00, 0x00000049u);
        put32(o + 0x04, (u32)(start >> 8));
        put32(o + 0x08, (u32)(limit >> 8));
        context = 0x00000001u | ((u32)obj << 9);        /* gf119_disp_dmac_bind: chid 0 << 27 | 1, inst << 9 */
    } else {
        /* nv50_dmaobj: NV_DMA_IN_MEMORY, user, VRAM, read/write, kind pitch, 256-byte partitions */
        put32(o + 0x00, 0x0019003Du);
        put32(o + 0x04, (u32)limit);
        put32(o + 0x08, (u32)start);
        put32(o + 0x0C, (u32)(limit >> 32) << 24 | (u32)(start >> 32));
        put32(o + 0x14, 0x00010000u);
        context = (u32)obj << 10;                        /* nv50_disp_dmac_bind: chid 0 << 28 | 0, inst << 10 */
    }
    u32 h = nv_evo_ramht_hash(handle, 0);
    for (u32 n = 0; n < EVO_RAMHT_BYTES / 8; n++) {
        u32 co = (h + n) % (EVO_RAMHT_BYTES / 8);
        u8 *slot = inst + co * 8;
        u32 hd = get32(slot), ct = get32(slot + 4);
        if ((hd == 0 && ct == 0) || hd == handle) {
            put32(slot, handle);
            put32(slot + 4, context);
            return 0;
        }
    }
    return E_NOSPC;
}

/* ---------------- state of heads and output resources ---------------- */

/* armed copy of a 507d / 827d head method (nv50_disp_core_mthd_head, g84) */
static const struct { u16 m; u32 a; } nv50_arm[] = {
    { 0x0804, 0x610ad0 }, { 0x0808, 0x610a48 }, { 0x0810, 0x610ac0 }, { 0x0814, 0x610af8 }, { 0x0818, 0x610b00 },
    { 0x081c, 0x610ae8 }, { 0x0820, 0x610af0 }, { 0x0824, 0x610b08 }, { 0x0828, 0x610b10 }, { 0x082c, 0x610a68 },
    { 0x0840, 0x610a24 }, { 0x0860, 0x610a84 }, { 0x0868, 0x610b18 }, { 0x086c, 0x610b20 }, { 0x0870, 0x610ac8 },
    { 0x0874, 0x610a38 }, { 0x0880, 0x610a58 }, { 0x08a0, 0x610a70 }, { 0x08a4, 0x610a50 }, { 0x08a8, 0x610ae0 },
    { 0x08c0, 0x610b28 }, { 0x08c8, 0x610b40 }, { 0x08d8, 0x610b48 }, { 0x08dc, 0x610b50 }, { 0x0900, 0x610a18 },
    { 0x0904, 0x610ab8 },
};

u32 nv_evo_mthd_arm(const nv_evo_gen_t *g, u32 m) {
    if (g->gf119) return 0x640000 + m;
    if (m >= 0x0800 && m < 0x1000) {
        u32 head = (m - 0x0800) / 0x400, mm = m - head * 0x400;
        for (size_t i = 0; i < ARRAY_SIZE(nv50_arm); i++)
            if (nv50_arm[i].m == mm) return nv50_arm[i].a + head * 0x540 + 4;
    }
    return 0;
}

static void proto_decode(nv_evo_or_state_t *s, int dac) {
    s->proto = 0xFF;
    s->link = 0;
    if (dac) { if (s->proto_evo == 0) s->proto = NV_OUT_CRT; return; }
    switch (s->proto_evo) {
    case 0: s->proto = NV_OUT_LVDS; s->link = 1; break;
    case 1: s->proto = NV_OUT_TMDS; s->link = 1; break;
    case 2: s->proto = NV_OUT_TMDS; s->link = 2; break;
    case 5: s->proto = NV_OUT_TMDS; s->link = 3; break;
    case 8: s->proto = NV_OUT_DP; s->link = 1; break;
    case 9: s->proto = NV_OUT_DP; s->link = 2; break;
    }
}

static void sor_state(nv_evo_t *e, int id, int asy, nv_evo_or_state_t *s) {
    u32 ctrl;
    memset(s, 0, sizeof *s);
    switch (e->g->sor_state) {
    case EVO_SOR_NV50: ctrl = rd(e, 0x610b70 + (u32)id * 8 + (asy ? 0 : 4)); s->heads = ctrl & 3; break;
    case EVO_SOR_G94: ctrl = rd(e, 0x610794 + (u32)id * 8 + (asy ? 0 : 4)); s->heads = ctrl & 3; break;
    default: ctrl = rd(e, 0x640200 + (u32)id * 0x20 + (asy ? 0x20000 : 0)); s->heads = ctrl & 0xF; break;
    }
    if (nv_bad_read(ctrl)) { memset(s, 0, sizeof *s); s->proto = 0xFF; return; }
    s->ctrl = ctrl;
    s->proto_evo = (ctrl >> 8) & 0xF;
    s->depth = (ctrl >> 16) & 0xF;
    proto_decode(s, 0);
}

static void dac_state(nv_evo_t *e, int id, int asy, nv_evo_or_state_t *s) {
    u32 ctrl;
    memset(s, 0, sizeof *s);
    if (e->g->gf119) { ctrl = rd(e, 0x640180 + (u32)id * 0x20 + (asy ? 0x20000 : 0)); s->heads = ctrl & 0xF; }
    else { ctrl = rd(e, 0x610b58 + (u32)id * 8 + (asy ? 0 : 4)); s->heads = ctrl & 3; }
    if (nv_bad_read(ctrl)) { memset(s, 0, sizeof *s); s->proto = 0xFF; return; }
    s->ctrl = ctrl;
    s->proto_evo = (ctrl >> 8) & 0xF;
    proto_decode(s, 1);
}

static void head_state(nv_evo_t *e, int h, int asy, nv_evo_head_state_t *s) {
    memset(s, 0, sizeof *s);
    u32 d;
    if (e->g->gf119) {
        u32 o = (u32)h * 0x300 + (asy ? 0x20000 : 0);
        d = rd(e, 0x640414 + o); s->vtotal = (u16)(d >> 16); s->htotal = (u16)d;
        d = rd(e, 0x640418 + o); s->vsynce = (u16)(d >> 16); s->hsynce = (u16)d;
        d = rd(e, 0x64041c + o); s->vblanke = (u16)(d >> 16); s->hblanke = (u16)d;
        d = rd(e, 0x640420 + o); s->vblanks = (u16)(d >> 16); s->hblanks = (u16)d;
        s->hz = rd(e, 0x640450 + o);
        d = rd(e, 0x640404 + o);
        s->orctrl = d;
        switch ((d >> 6) & 0xF) { case 6: s->depth = 30; break; case 5: s->depth = 24; break; default: s->depth = 18; break; }
        s->offset = rd(e, 0x640460 + o); s->size = rd(e, 0x640468 + o); s->storage = rd(e, 0x64046c + o);
        s->params = rd(e, 0x640470 + o); s->ctxdma = rd(e, 0x640474 + o);
    } else {
        u32 o = (u32)h * 0x540 + (asy ? 0 : 4);
        d = rd(e, 0x610ae8 + o); s->vblanke = (u16)(d >> 16); s->hblanke = (u16)d;
        d = rd(e, 0x610af0 + o); s->vblanks = (u16)(d >> 16); s->hblanks = (u16)d;
        d = rd(e, 0x610af8 + o); s->vtotal = (u16)(d >> 16); s->htotal = (u16)d;
        d = rd(e, 0x610b00 + o); s->vsynce = (u16)(d >> 16); s->hsynce = (u16)d;
        s->hz = (rd(e, 0x610ad0 + o) & 0x003FFFFF) * 1000;
        s->depth = 24;
        s->offset = rd(e, 0x610a84 + o); s->size = rd(e, 0x610b18 + o); s->storage = rd(e, 0x610b20 + o);
        s->params = rd(e, 0x610ac8 + o); s->ctxdma = rd(e, 0x610a38 + o);
    }
}

void nv_evo_read_state(nv_evo_t *e, int asy) {
    for (int h = 0; h < e->nheads && h < EVO_HEADS; h++)
        if (e->head_mask & (1u << h)) head_state(e, h, asy, asy ? &e->head_asy[h] : &e->head_arm[h]);
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if (e->sor_mask & (1u << i)) sor_state(e, i, asy, asy ? &e->sor_asy[i] : &e->sor_arm[i]);
    for (int i = 0; i < e->ndacs && i < EVO_ORS; i++)
        if (e->dac_mask & (1u << i)) dac_state(e, i, asy, asy ? &e->dac_asy[i] : &e->dac_arm[i]);
    /* NV50-G94: the OR's pixel depth is the head's (nv50_pior_depth) */
    if (!e->g->gf119 && e->g->sor_state == EVO_SOR_G94)
        for (int i = 0; i < e->nsors && i < EVO_ORS; i++) {
            nv_evo_or_state_t *s = asy ? &e->sor_asy[i] : &e->sor_arm[i];
            if (!s->heads) continue;
            int h = __builtin_ctz(s->heads);
            nv_evo_head_state_t *hs = asy ? &e->head_asy[h] : &e->head_arm[h];
            hs->depth = s->depth == 6 ? 30 : s->depth == 5 ? 24 : 18;
        }
}

/* ---------------- routing (GM200+: SORs to pad macros) ---------------- */

static int macro_of(const nv_dcb_output_t *o) { return o->or_mask ? __builtin_ctz(o->or_mask) : -1; }

/* the SOR routed to output o, or -1 (gm200_sor_route_get); *link: link bits */
static int route_get(nv_evo_t *e, const nv_dcb_output_t *o, int *link) {
    int m = macro_of(o);
    if (m < 0 || m > 7) return -1;
    int sor[2] = { 0, 0 }, lnk[2] = { 0, 0 };
    *link = 0;
    for (int s = 0; s < 2; s++) {
        if (!(o->link & (1 << s))) continue;
        u32 d = rd(e, 0x612308 + (u32)(m * 2 + s) * 0x80);
        if (nv_bad_read(d)) return -1;
        lnk[s] = (d >> 4) & 1;
        sor[s] = d & 0xF;
        if (!sor[s]) return -1;
        *link |= lnk[s];
    }
    if (o->link == 3 && (sor[0] != sor[1] || lnk[0] || !lnk[1])) return -1;
    return ((o->link & 1) ? sor[0] : sor[1]) - 1;
}

static void route_set(nv_evo_t *e, const nv_dcb_output_t *o, int sor, int asy_link) {
    int m = macro_of(o);
    if (m < 0 || m > 7) return;
    u32 moff = (u32)m * 0x100, id = sor >= 0 ? (u32)sor + 1 : 0, link = sor >= 0 ? (asy_link == 2) : 0;
    if (o->link & 1) { mask(e, 0x612308 + moff, 0x1F, link << 4 | id); link++; }
    if (o->link & 2) mask(e, 0x612388 + moff, 0x1F, link << 4 | id);
}

/* which DCB output each armed OR drives (nvkm_outp_init_route) */
static void map_armed(nv_evo_t *e) {
    for (int i = 0; i < EVO_ORS; i++) { e->sor_outp[i] = e->dac_outp[i] = -1; e->sor_asy_outp[i] = e->dac_asy_outp[i] = -1; }
    if (!e->bios) return;
    for (int k = 0; k < e->bios->noutputs; k++) {
        const nv_dcb_output_t *o = &e->bios->out[k];
        if (o->location != 0) continue;
        int dac = o->type == NV_OUT_CRT || o->type == NV_OUT_TV;
        if (!dac && o->type != NV_OUT_TMDS && o->type != NV_OUT_LVDS && o->type != NV_OUT_DP) continue;
        int id, link = 0;
        if (!dac && e->g->route) {
            id = route_get(e, o, &link);
            if (id < 0) continue;
        } else {
            id = o->or_mask ? __builtin_ctz(o->or_mask) : -1;
        }
        if (id < 0 || id >= EVO_ORS) continue;
        nv_evo_or_state_t *s = dac ? &e->dac_arm[id] : &e->sor_arm[id];
        int want = dac ? NV_OUT_CRT : o->type;
        if (!s->heads || s->proto != want) continue;
        if (!dac && o->type != NV_OUT_DP && !(s->link & o->link)) continue;
        if (dac) e->dac_outp[id] = k; else e->sor_outp[id] = k;
    }
}

/* ---------------- the firmware's display ---------------- */

void nv_evo_find_fw(nv_evo_t *e) {
    memset(&e->fw, 0, sizeof e->fw);
    e->fw.head = -1;
    e->fw.dcb = -1;
    for (int h = 0; h < e->nheads && h < EVO_HEADS && e->fw.head < 0; h++) {
        if (!(e->head_mask & (1u << h))) continue;
        nv_evo_head_state_t *s = &e->head_arm[h];
        if (!s->hz || !s->htotal || !s->vtotal) continue;
        int or_type = -1, or_id = -1;
        for (int i = 0; i < e->nsors && i < EVO_ORS && or_id < 0; i++)
            if (e->sor_arm[i].heads & (1u << h)) { or_type = 0; or_id = i; }
        for (int i = 0; i < e->ndacs && i < EVO_ORS && or_id < 0; i++)
            if (e->dac_arm[i].heads & (1u << h)) { or_type = 1; or_id = i; }
        if (or_id < 0) continue;
        e->fw.head = h;
        e->fw.hs = *s;
        e->fw.or_type = or_type;
        e->fw.or_id = or_id;
        e->fw.os = or_type ? e->dac_arm[or_id] : e->sor_arm[or_id];
        e->fw.dcb = or_type ? e->dac_outp[or_id] : e->sor_outp[or_id];
        e->fw.valid = 1;
    }
}

/* ---------------- the supervisor ---------------- */

static const nv_dcb_output_t *dcb(nv_evo_t *e, int k) {
    return e->bios && k >= 0 && k < e->bios->noutputs ? &e->bios->out[k] : NULL;
}

/* nv50_disp_super_iedt */
static int iedt(nv_evo_t *e, int head, int k, nv_ied_t *ied) {
    const nv_dcb_output_t *o = dcb(e, k);
    if (!o || !e->bios) return 0;
    u16 l = o->link ? (u16)(__builtin_ctz(o->link) + 1) : 0;
    u16 m = (u16)((0x0100 << head) | (l << 6) | (o->or_mask & 0x3F));
    if (!nv_ied_match(e->bios, nv_dcb_hasht(o), m, ied)) {
        nv_evo_log(e, "head %d: no IED entry for %04x:%04x", head, nv_dcb_hasht(o), m);
        return 0;
    }
    return 1;
}

static void ied_off(nv_evo_t *e, int head, int or_type, int or_id, int id) {
    int k = or_type ? e->dac_outp[or_id] : e->sor_outp[or_id];
    nv_ied_t ied;
    if (k < 0 || !iedt(e, head, k, &ied) || !ied.script[id]) return;
    const nv_evo_or_state_t *s = or_type ? &e->dac_arm[or_id] : &e->sor_arm[or_id];
    nv_init_ctx_t c = { k, or_id, s->link, head };
    char what[24];
    snprintf(what, sizeof what, "OffInt%d", id);
    nv_evo_script(e, ied.script[id], &c, what);
}

static void ied_on(nv_evo_t *e, int head, int or_type, int or_id, int id, u32 khz) {
    int k = or_type ? e->dac_asy_outp[or_id] : e->sor_asy_outp[or_id];
    nv_ied_t ied;
    if (k < 0 || !iedt(e, head, k, &ied)) return;
    const nv_evo_or_state_t *s = or_type ? &e->dac_asy[or_id] : &e->sor_asy[or_id];
    u8 flags = 0;
    if (!or_type) {
        if (s->proto == NV_OUT_LVDS && e->head_asy[head].depth == 24) flags |= 0x02;
        if (s->link == 3) flags |= 0x01;
    }
    u16 clk[2];
    if (!nv_ied_ocfg(e->bios, &ied, s->proto_evo, flags, clk)) {
        nv_evo_log(e, "head %d: no IED settings for protocol %02x flags %02x", head, s->proto_evo, flags);
        return;
    }
    u16 script = nv_ied_oclk(e->bios, clk[id], khz);
    if (!script) { nv_evo_log(e, "head %d: no IED script %d for %u kHz", head, id, khz); return; }
    nv_init_ctx_t c = { k, or_id, s->link, head };
    char what[24];
    snprintf(what, sizeof what, "OnInt%d", id + 2);
    nv_evo_script(e, script, &c, what);
}

/* the OR a head drives in the armed (asy = 0) or assembly state */
static int head_or(nv_evo_t *e, int head, int asy, int *or_type) {
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if ((asy ? e->sor_asy[i] : e->sor_arm[i]).heads & (1u << head)) { *or_type = 0; return i; }
    for (int i = 0; i < e->ndacs && i < EVO_ORS; i++)
        if ((asy ? e->dac_asy[i] : e->dac_arm[i]).heads & (1u << head)) { *or_type = 1; return i; }
    return -1;
}

static void super_1_0(nv_evo_t *e, int head) {
    int t, id = head_or(e, head, 0, &t);
    nv_evo_log(e, "supervisor 1.0: head %d %s", head, id < 0 ? "had nothing attached" : t ? "detaching its DAC" : "detaching its SOR");
    if (id >= 0) ied_off(e, head, t, id, 1);
}

static void super_2_0(nv_evo_t *e, int head) {
    int t, id = head_or(e, head, 0, &t);
    nv_evo_log(e, "supervisor 2.0: head %d", head);
    if (id < 0) return;
    ied_off(e, head, t, id, 2);
    /* the output path's disable: DisableLT for DisplayPort (nvkm_dp_disable) */
    const nv_evo_or_state_t *s = t ? &e->dac_arm[id] : &e->sor_arm[id];
    int k = t ? e->dac_outp[id] : e->sor_outp[id];
    const nv_dcb_output_t *o = dcb(e, k);
    if (!t && o && o->type == NV_OUT_DP && s->heads == (1u << head) && e->bios) {
        nv_dpout_t dp;
        if (nv_dpout_match(e->bios, nv_dcb_hasht(o), nv_dcb_hashm(o), &dp) && dp.script[4]) {
            nv_init_ctx_t c = { k, id, s->link, -1 };
            nv_evo_script(e, dp.script[4], &c, "DisableLT");
        }
    }
}

/* nvkm_outp_route */
static void super_route(nv_evo_t *e) {
    if (!e->g->route) return;
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if (e->sor_outp[i] >= 0 && e->sor_outp[i] != e->sor_asy_outp[i]) {
            nv_evo_log(e, "route: SOR%d released from DCB output %d", i, e->sor_outp[i]);
            route_set(e, dcb(e, e->sor_outp[i]), -1, 0);
            e->sor_outp[i] = -1;
        }
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++)
        if (e->sor_asy_outp[i] >= 0 && e->sor_asy_outp[i] != e->sor_outp[i]) {
            nv_evo_log(e, "route: SOR%d to DCB output %d", i, e->sor_asy_outp[i]);
            route_set(e, dcb(e, e->sor_asy_outp[i]), i, e->sor_asy[i].link);
            e->sor_outp[i] = e->sor_asy_outp[i];
        }
}

static void super_2_1(nv_evo_t *e, int head) {
    u32 khz = e->head_asy[head].hz / 1000;
    nv_evo_log(e, "supervisor 2.1: head %d pixel clock %u kHz", head, khz);
    if (khz && nv_evo_vpll_set(e, head, khz)) nv_evo_log(e, "head %d: the video PLL was not programmed", head);
}

static void sor_clock(nv_evo_t *e, int id) {
    const nv_evo_or_state_t *s = &e->sor_asy[id];
    u32 soff = (u32)id * 0x800;
    if (e->g->gf119) {
        u32 div1 = s->link == 3, div2 = s->link == 3;
        if (s->proto == NV_OUT_TMDS) mask(e, 0x612300 + soff, 0x007C0000, 0x0A << 18);   /* not high speed (no HDMI 2.0) */
        mask(e, 0x612300 + soff, 0x00000707, (div2 << 8) | div1);
    } else {
        u32 div = s->link == 3;
        mask(e, 0x614300 + soff, 0x00000707, (div << 8) | div);
    }
}

/* g94_sor_war_2 / war_3 (G94-G98 TMDS) */
static int war_needed(nv_evo_t *e, int id) {
    if (!e->g->war_g94 || e->sor_asy[id].proto != NV_OUT_TMDS) return 0;
    u32 v = rd(e, 0x614300 + (u32)id * 0x800) & 0x00030000;
    return v == 0 || v == 0x00030000;
}
static void war_2(nv_evo_t *e, int id) {
    u32 soff = (u32)id * 0x800;
    if (!war_needed(e, id)) return;
    mask(e, 0x00e840, 0x80000000, 0x80000000);
    mask(e, 0x614300 + soff, 0x03000000, 0x03000000);
    mask(e, 0x61c10c + soff, 0x00000001, 0x00000001);
    mask(e, 0x61c00c + soff, 0x0f000000, 0x00000000);
    mask(e, 0x61c008 + soff, 0xff000000, 0x14000000);
    delay(e, 400);
    mask(e, 0x61c008 + soff, 0xff000000, 0x00000000);
    mask(e, 0x61c00c + soff, 0x0f000000, 0x01000000);
    if (rd(e, 0x61c004 + soff) & 1) {
        u32 pu = rd(e, 0x61c030 + soff) & 0xF;
        wr(e, 0x61c040 + soff + pu * 4, 0x1f008000);
    }
}
static void war_3(nv_evo_t *e, int id) {
    u32 soff = (u32)id * 0x800;
    if (!war_needed(e, id)) return;
    u32 pwr = rd(e, 0x61c004 + soff);
    if (pwr & 1) {
        u32 seq = rd(e, 0x61c030 + soff), pd = (seq >> 8) & 0xF, pu = seq & 0xF;
        wr(e, 0x61c040 + soff + pd * 4, 0x1f008000);
        wait(e, 0x61c030 + soff, 0x10000000, 0, 2000000, NULL);
        mask(e, 0x61c004 + soff, 0x80000001, 0x80000000);
        wait(e, 0x61c030 + soff, 0x10000000, 0, 2000000, NULL);
        wr(e, 0x61c040 + soff + pd * 4, 0x00002000);
        wr(e, 0x61c040 + soff + pu * 4, 0x1f000000);
    }
    mask(e, 0x61c10c + soff, 0x00000001, 0x00000000);
    mask(e, 0x614300 + soff, 0x03000000, 0x00000000);
    if (pwr & 1) mask(e, 0x61c004 + soff, 0x80000001, 0x80000001);
    int used = 0;
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++) {
        u32 c = rd(e, 0x614300 + (u32)i * 0x800) & 0x03000000;
        if (c == 0x02000000 || c == 0x03000000) used = 1;
    }
    if (!used) mask(e, 0x00e840, 0x80000000, 0);
}

static void dp_tu_program(nv_evo_t *e, int head, int id, const nv_evo_dp_t *dp) {
    nv_dp_tu_t tu;
    int activesym = !e->g->gf119;
    const nv_evo_head_state_t *h = &e->head_asy[head];
    if (nv_dp_tu(h, dp->nr, dp->bw, dp->ef, h->depth, activesym, &tu)) {
        nv_evo_log(e, "head %d: no DisplayPort transfer unit fits", head);
        return;
    }
    u32 soff = (u32)id * 0x800, loff = soff + (dp->link == 2 ? 0x80 : 0), hoff = (u32)head * 0x800;
    if (e->g->gf119) {
        mask(e, 0x616620 + hoff, 0x0000ffff, tu.hsym);
        mask(e, 0x616624 + hoff, 0x00ffffff, tu.vsym);
        mask(e, 0x616610 + hoff, 0x0800003f, 0x08000000 | (tu.watermark & 0x3F));
    } else {
        mask(e, 0x61c1e8 + soff, 0x0000ffff, tu.hsym);
        mask(e, 0x61c1ec + soff, 0x00ffffff, tu.vsym);
        mask(e, 0x61c10c + loff, 0x000001fc, (u32)tu.TU << 2);
        mask(e, 0x61c128 + loff, 0x010f7f00, (u32)tu.VTUa << 24 | (u32)tu.VTUf << 16 | (u32)tu.VTUi << 8);
        mask(e, 0x61c128 + loff, 0x0000003f, tu.watermark & 0x3F);
    }
    nv_evo_log(e, "head %d: DisplayPort TU %d (%d.%d/%d) watermark %u", head, tu.TU, tu.VTUi, tu.VTUa, tu.VTUf, tu.watermark);
}

static void super_2_2(nv_evo_t *e, int head) {
    int t, id = head_or(e, head, 1, &t);
    u32 khz = e->head_asy[head].hz / 1000;
    nv_evo_log(e, "supervisor 2.2: head %d %s%d", head, id < 0 ? "nothing to attach" : t ? "DAC" : "SOR", id);
    if (id < 0) return;
    /* link training (nvkm_dp_acquire) */
    nv_evo_dp_t *dp = !t ? e->dp[id] : NULL;
    if (dp) {
        u32 kbps = (khz * e->head_asy[head].depth + 7) / 8;
        if (nv_dp_train(e, dp, id, kbps)) e->errors++;
    }
    ied_on(e, head, t, id, 0, khz);
    /* the RG clock divider (0 for SORs and DACs) */
    mask(e, (e->g->gf119 ? 0x612200 : 0x614200) + (u32)head * 0x800, 0x0000000f, 0);
    if (dp && dp->trained) dp_tu_program(e, head, id, dp);
    if (t) mask(e, (e->g->gf119 ? 0x612280 : 0x614280) + (u32)id * 0x800, 0x07070707, 0);
    else sor_clock(e, id);
    if (!t) war_2(e, id);
}

static void super_3_0(nv_evo_t *e, int head) {
    int t, id = head_or(e, head, 1, &t);
    nv_evo_log(e, "supervisor 3.0: head %d", head);
    if (id < 0) return;
    ied_on(e, head, t, id, 1, e->head_asy[head].hz / 1000);
    if (!t) war_3(e, id);
}

/* one supervisor round (nv50_disp_super / gf119_disp_super) */
static void super(nv_evo_t *e, u32 pending) {
    u32 m[EVO_HEADS] = { 0 }, sup = 0;
    int nh = MIN(e->nheads, EVO_HEADS);
    e->supers++;
    if (e->g->gf119) for (int h = 0; h < nh; h++) m[h] = (e->head_mask & (1u << h)) ? rd(e, 0x6101d4 + (u32)h * 0x800) : 0;
    else sup = rd(e, 0x610030);
    int s1 = e->g->gf119 ? (pending & 1) : (pending & 0x10);
    int s2 = e->g->gf119 ? (pending & 2) : (pending & 0x20);
    int s3 = e->g->gf119 ? (pending & 4) : (pending & 0x40);
#define CHANGING(h) (e->g->gf119 ? (m[h] & 0x00001000) : (sup & (0x80u << (h))))
#define PCLK(h)     (e->g->gf119 ? (m[h] & 0x00010000) : (sup & (0x200u << (h))))
    if (s1) {
        nv_evo_read_state(e, 0);
        nv_evo_read_state(e, 1);
        for (int h = 0; h < nh; h++) {
            if (!CHANGING(h)) continue;
            if (!e->g->gf119 && !(sup & (0x20u << h))) continue;
            super_1_0(e, h);
        }
    } else if (s2) {
        for (int h = 0; h < nh; h++) if (CHANGING(h)) super_2_0(e, h);
        super_route(e);
        for (int h = 0; h < nh; h++) if (PCLK(h)) super_2_1(e, h);
        for (int h = 0; h < nh; h++) if (CHANGING(h)) super_2_2(e, h);
    } else if (s3) {
        for (int h = 0; h < nh; h++) if (CHANGING(h)) super_3_0(e, h);
    }
#undef CHANGING
#undef PCLK
    if (e->g->gf119) {
        for (int h = 0; h < nh; h++) if (e->head_mask & (1u << h)) wr(e, 0x6101d4 + (u32)h * 0x800, 0);
        wr(e, 0x6101d0, 0x80000000);
    } else {
        wr(e, 0x610030, 0x80000000);
    }
}

/* pending supervisor bits (cleared), or 0 */
static u32 super_pending(nv_evo_t *e) {
    if (e->g->gf119) {
        u32 s = rd(e, 0x6100ac);
        if (nv_bad_read(s) || !(s & 7)) return 0;
        wr(e, 0x6100ac, s & 7);
        return s & 7;
    }
    u32 s = rd(e, 0x610024);
    if (nv_bad_read(s) || !(s & 0x70)) return 0;
    wr(e, 0x610024, s & 0x70);
    return s & 0x70;
}

/* a channel error on the core channel: logged, cleared; 1 if there was one */
static int chan_error(nv_evo_t *e) {
    static const char *type[8] = { "none", "push buffer", "trap", "reserved method", "invalid argument", "invalid state", "?",
                                   "unresolvable handle" };
    if (e->g->gf119) {
        u32 st = rd(e, 0x61009c);
        if (nv_bad_read(st) || !(st & 1)) return 0;
        u32 base = e->g->push_gp102 ? 0x6111f0 : 0x6101f0;
        u32 stat = rd(e, base), data = rd(e, base + 4), code = rd(e, base + 8);
        nv_evo_log(e, "core channel error: %s, method %04x, data %08x (%08x %08x)",
                   e->g->push_gp102 ? "?" : type[(stat >> 12) & 7], stat & 0xFFC, data, stat, code);
        wr(e, 0x61009c, 1);
        wr(e, base, 0x90000000);
    } else {
        u32 st = rd(e, 0x610020);
        if (nv_bad_read(st) || !(st & 0x00010000)) return 0;
        u32 addr = rd(e, 0x610080), data = rd(e, 0x610084);
        nv_evo_log(e, "core channel error: %s, method %04x, data %08x (%08x)", type[(addr >> 12) & 7], addr & 0xFFC, data, addr);
        wr(e, 0x610020, 0x00010000);
        wr(e, 0x610080, 0x90000000);
    }
    e->errors++;
    return 1;
}

/* ---------------- the core channel ---------------- */

static u32 push_reg(nv_evo_t *e) { return e->g->push_gp102 ? 0x611494 : e->g->gf119 ? 0x610494 : 0x610204; }

static int chan_fini(nv_evo_t *e) {
    u32 c = nv_evo_core_ctrl_reg(e->g), last;
    mask(e, c, 0x00000010, 0);
    mask(e, c, 0x00000003, 0);
    if (wait(e, c, 0x001e0000, 0, e->timeout_us, &last)) {
        nv_evo_log(e, "core channel did not stop: %08x", last);
        return E_TIMEOUT;
    }
    return 0;
}

static int chan_init(nv_evo_t *e) {
    u32 c = nv_evo_core_ctrl_reg(e->g), last, p = push_reg(e);
    if (!e->g->gf119) {
        /* nv50_disp_core_init: unstick the channel from some unknown state */
        u32 v = rd(e, c);
        if ((v & 0x009f0000) == 0x00020000) mask(e, c, 0x00800000, 0x00800000);
        if ((rd(e, c) & 0x003f0000) == 0x00030000) mask(e, c, 0x00600000, 0x00600000);
    }
    save(e, p); save(e, p + 4); save(e, p + 8);
    wr(e, p, 0x00000001u | (u32)(e->push >> 8));       /* VRAM */
    wr(e, p + 4, 0x00010000);
    wr(e, p + 8, e->g->gf119 ? 0x00000001 : 0x00000000);
    mask(e, c, 0x00000010, 0x00000010);
    wr(e, 0x640000, 0);                                  /* PUT */
    wr(e, c, 0x01000013);
    if (wait(e, c, 0x80000000, 0, e->timeout_us, &last)) {
        fail(e, "the core channel did not start (%06x = %08x)", c, last);
        return E_TIMEOUT;
    }
    e->put = 0;
    return 0;
}

/* write the stream into the push buffer and move PUT (the channel is idle) */
static int kick(nv_evo_t *e, const nv_evo_stream_t *s) {
    /* (at most 400 words: after a wrap the new PUT can never equal GET) */
    if (s->overflow || s->n <= 0 || s->n > 400) { fail(e, "method stream overflow"); return E_INVAL; }
    u32 words = (u32)s->n;
    if (e->put / 4 + words + 1 > EVO_PUSH_MAX) {
        vwr32(e, e->push + e->put, NV_EVO_JUMP(0));      /* wrap: the hardware jumps back to 0 */
        e->put = 0;
    }
    for (u32 i = 0; i < words; i++) vwr32(e, e->push + e->put + i * 4, s->w[i]);
    if (vrd32(e, e->push + e->put) != s->w[0]) { fail(e, "the push buffer in VRAM does not read back"); return E_IO; }
    int r = bar_flush(e);
    if (r) { fail(e, "BAR flush timed out"); return r; }
    e->put += words * 4;
    wr(e, 0x640000, e->put);
    return 0;
}

static int chan_idle(nv_evo_t *e, u32 us) {
    u64 end = now(e) + us;
    for (;;) {
        u32 get = rd(e, 0x640004);
        if (get == e->put) return 0;
        if (chan_error(e)) { fail(e, "the core channel reported an error"); return E_IO; }
        if (nv_bad_read(get) || now(e) >= end) {
            e->timeouts++;
            fail(e, "the core channel did not fetch its commands (GET %x, PUT %x)", get, e->put);
            return E_TIMEOUT;
        }
        delay(e, 10);
    }
}

static int submit(nv_evo_t *e, const nv_evo_stream_t *s, int wait_update);

int nv_evo_submit(nv_evo_t *e, const nv_evo_stream_t *s, int wait_update) {
    int r = submit(e, s, wait_update);
    /* two updates in a row that never finished: the channel is given up
     * (a restore would only wait another 3 seconds) */
    if (r == E_TIMEOUT && ++e->stuck >= 2 && e->chan_up) {
        e->chan_up = 0;
        nv_evo_log(e, "the core channel is given up after %d timeouts in a row", e->stuck);
    }
    if (!r) e->stuck = 0;
    return r;
}

static int submit(nv_evo_t *e, const nv_evo_stream_t *s, int wait_update) {
    if (!e->chan_up) { fail(e, "the core channel is not running"); return E_IO; }
    if (wait_update) vwr32(e, e->sync, 0);               /* notifier: not done */
    u32 errs = e->errors;
    int r = kick(e, s);
    if (r) return r;
    if (!wait_update) return chan_idle(e, e->timeout_us);
    u64 end = now(e) + e->update_us;
    int rounds = 0;
    for (;;) {
        u32 p = super_pending(e);
        if (p) {
            super(e, p);
            if (++rounds > 12) { fail(e, "too many supervisor rounds"); return E_IO; }
            continue;
        }
        if (chan_error(e)) { fail(e, "the core channel reported an error"); return E_IO; }
        if (vrd32(e, e->sync) & 1) break;                /* NV_DISP_CORE_NOTIFIER_1 COMPLETION_0 DONE */
        if (now(e) >= end) {
            e->timeouts++;
            fail(e, "the update did not complete (GET %x, PUT %x, %d supervisor rounds)", rd(e, 0x640004), e->put, rounds);
            return E_TIMEOUT;
        }
        delay(e, 20);
    }
    /* the assembly state is armed now: the ORs drive what was attached */
    nv_evo_read_state(e, 0);
    for (int i = 0; i < EVO_ORS; i++) {
        if (i < e->nsors) e->sor_outp[i] = e->sor_arm[i].heads ? (e->sor_asy_outp[i] >= 0 ? e->sor_asy_outp[i] : e->sor_outp[i]) : -1;
        if (i < e->ndacs) e->dac_outp[i] = e->dac_arm[i].heads ? (e->dac_asy_outp[i] >= 0 ? e->dac_asy_outp[i] : e->dac_outp[i]) : -1;
    }
    if (e->errors != errs) { fail(e, "the update completed with errors (see above)"); return E_IO; }
    return 0;
}

/* ---------------- takeover and release ---------------- */

static void mask_interrupts(nv_evo_t *e) {
    if (e->chipset >= 0x130) {
        /* gp100_mc: enables are set (0x160) and cleared (0x180) per leaf; 0x140 reads them */
        save(e, 0x000140);
        save(e, 0x000144);
        wr(e, 0x000180, 0xFFFFFFFF);
        wr(e, 0x000184, 0xFFFFFFFF);
    } else {
        swr(e, 0x000140, 0);
    }
}


static void or_power(nv_evo_t *e) {
    /* nvkm_disp_init: every OR's 'normal' state fully enabled (nv50_sor_power,
     * nv50_dac_power); 200 ms per wait (nouveau: 2 s), and after one that
     * runs out the others are not waited for */
    u32 lim = 200000;
    for (int i = 0; i < e->nsors && i < EVO_ORS; i++) {
        if (!(e->sor_mask & (1u << i))) continue;
        u32 soff = (u32)i * 0x800, last = 0;
        int t = wait(e, 0x61c004 + soff, 0x80000000, 0, lim, &last);
        mask(e, 0x61c004 + soff, 0x80000001, 0x80000001);
        t |= wait(e, 0x61c004 + soff, 0x80000000, 0, lim, &last);
        t |= wait(e, 0x61c030 + soff, 0x10000000, 0, lim, &last);
        if (t) { nv_evo_log(e, "SOR%d power sequencer busy (%08x)", i, last); lim = 1000; }
    }
    for (int i = 0; i < e->ndacs && i < EVO_ORS; i++) {
        if (!(e->dac_mask & (1u << i))) continue;
        u32 doff = (u32)i * 0x800, last = 0;
        int t = wait(e, 0x61a004 + doff, 0x80000000, 0, lim, &last);
        mask(e, 0x61a004 + doff, 0xc0000055, 0x80000000);
        t |= wait(e, 0x61a004 + doff, 0x80000000, 0, lim, &last);
        if (t) { nv_evo_log(e, "DAC%d power busy (%08x)", i, last); lim = 1000; }
    }
}

static int count_resources(nv_evo_t *e) {
    if (e->g->gf119) {
        u32 caps = rd(e, 0x612004), n = rd(e, 0x022448);
        if (nv_bad_read(caps) || nv_bad_read(n)) { fail(e, "display capabilities unreadable (%08x, %08x)", caps, n); return E_NODEV; }
        e->head_mask = caps & 0xF;
        e->nheads = (int)MIN(n, (u32)EVO_HEADS);
        e->sor_mask = (caps >> 8) & 0xFF;
        e->nsors = 8;
        e->dac_mask = e->g->has_dac ? (caps >> 4) & 0xF : 0;
        e->ndacs = e->g->has_dac ? 4 : 0;
    } else {
        u32 caps = rd(e, 0x610184);
        if (nv_bad_read(caps)) { fail(e, "display capabilities unreadable (%08x)", caps); return E_NODEV; }
        e->head_mask = 3;
        e->nheads = 2;
        e->dac_mask = (caps >> 20) & 7;
        e->ndacs = 3;
        e->sor_mask = e->g->sor_state == EVO_SOR_G94 ? (caps >> 24) & 0xF : (caps >> 24) & 3;
        e->nsors = e->g->sor_state == EVO_SOR_G94 ? 4 : 2;
    }
    if (!e->head_mask || !e->nheads) { fail(e, "no heads (capabilities say none)"); return E_NODEV; }
    return 0;
}

int nv_evo_takeover(nv_evo_t *e) {
    e->why[0] = 0;
    if (!e->timeout_us) e->timeout_us = 2000000;
    if (!e->update_us) e->update_us = 3000000;
    if (!e->g || !e->r || !e->vram) { fail(e, "not set up"); return E_INVAL; }
    if (!vram_ok(e, e->inst, EVO_INST_BYTES) || !vram_ok(e, e->push, EVO_PUSH_BYTES) || !vram_ok(e, e->sync, 0x1000) ||
        (e->inst & 0xFFFF) || (e->push & 0xFFF) || (e->sync & 0xFFF)) {
        fail(e, "the control structures are outside the VRAM window");
        return E_INVAL;
    }
    int r = count_resources(e);
    if (r) return r;
    nv_evo_log(e, "%s display (core %04x): heads %x of %d, SORs %02x, DACs %x", e->g->name, e->g->core_class, e->head_mask,
               e->nheads, e->sor_mask, e->dac_mask);
    /* what the firmware lit */
    nv_evo_read_state(e, 0);
    map_armed(e);
    nv_evo_find_fw(e);
    if (e->fw.valid)
        nv_evo_log(e, "firmware: head %d, %ux%u total, %u kHz, %s%d (%s, link %d), DCB output %d, surface %08x << 8",
                   e->fw.head, e->fw.hs.htotal, e->fw.hs.vtotal, e->fw.hs.hz / 1000, e->fw.or_type ? "DAC" : "SOR",
                   e->fw.or_id, nv_out_type_str(e->fw.os.proto), e->fw.os.link, e->fw.dcb, e->fw.hs.offset);
    else
        nv_evo_log(e, "firmware: no lit head found in the armed state");
    u32 c = nv_evo_core_ctrl_reg(e->g);
    u32 own = rd(e, e->g->gf119 ? 0x6100ac : 0x610024);
    nv_evo_log(e, "before: core channel %08x, instance %08x, interrupts %08x, VBIOS %08x (0x6194e8)", rd(e, c), rd(e, 0x610010), own,
               rd(e, 0x6194e8));
    if (nv_bad_read(rd(e, c))) { fail(e, "the core channel registers are unreadable"); return E_NODEV; }

    /* our objects: the table, the context DMAs, the notifier, an empty push buffer */
    for (u32 i = 0; i < EVO_INST_BYTES; i += 4) vwr32(e, e->inst + i, 0);
    for (u32 i = 0; i < EVO_PUSH_BYTES; i += 4) vwr32(e, e->push + i, 0);
    for (u32 i = 0; i < 0x1000; i += 4) vwr32(e, e->sync + i, 0);
    /* the table and the two objects are built in a copy, then written out */
    static u8 shadow[EVO_INST_BYTES];
    memset(shadow, 0, sizeof shadow);
    if (nv_evo_dmaobj(e->g, shadow, 0x1000, EVO_HANDLE_SYNC, e->sync, e->sync + 0xFFF) ||
        nv_evo_dmaobj(e->g, shadow, 0x1020, EVO_HANDLE_VRAM, 0, (e->vram_total ? e->vram_total : e->vram_base + e->vram_size) - 1)) {
        fail(e, "the object table is full");
        return E_NOSPC;
    }
    for (u32 i = 0; i < 0x1040; i += 4) vwr32(e, e->inst + i, get32(shadow + i));
    if (vrd32(e, e->inst + 0x1000) != get32(shadow + 0x1000)) { fail(e, "the instance memory does not read back through BAR1"); return E_IO; }
    if ((r = bar_flush(e))) { fail(e, "BAR flush timed out"); return r; }

    /* nothing may interrupt the CPU: the GPU's interrupts are masked (polled from here on) */
    mask_interrupts(e);

    /* nv50_disp_init / gf119_disp_init */
    if (e->g->gf119) {
        for (int h = 0; h < e->nheads; h++) {
            if (!(e->head_mask & (1u << h))) continue;
            u32 ho = (u32)h * 0x800;
            swr(e, 0x6101b4 + ho, rd(e, 0x616104 + ho));
            swr(e, 0x6101b8 + ho, rd(e, 0x616108 + ho));
            swr(e, 0x6101bc + ho, rd(e, 0x61610c + ho));
        }
        for (int i = 0; i < e->ndacs; i++) if (e->dac_mask & (1u << i)) swr(e, 0x6101c0 + (u32)i * 0x800, rd(e, 0x61a000 + (u32)i * 0x800));
        for (int i = 0; i < e->nsors; i++) if (e->sor_mask & (1u << i)) swr(e, 0x6301c4 + (u32)i * 0x800, rd(e, 0x61c000 + (u32)i * 0x800));
    } else {
        swr(e, 0x610184, rd(e, 0x614004));
        for (int h = 0; h < 2; h++) {
            swr(e, 0x610190 + (u32)h * 0x10, rd(e, 0x616100 + (u32)h * 0x800));
            swr(e, 0x610194 + (u32)h * 0x10, rd(e, 0x616104 + (u32)h * 0x800));
            swr(e, 0x610198 + (u32)h * 0x10, rd(e, 0x616108 + (u32)h * 0x800));
            swr(e, 0x61019c + (u32)h * 0x10, rd(e, 0x61610c + (u32)h * 0x800));
        }
        for (int i = 0; i < e->ndacs; i++) swr(e, 0x6101d0 + (u32)i * 4, rd(e, 0x61a000 + (u32)i * 0x800));
        for (int i = 0; i < e->nsors; i++) swr(e, 0x6101e0 + (u32)i * 4, rd(e, 0x61c000 + (u32)i * 0x800));
        for (int i = 0; i < 3; i++) swr(e, 0x6101f0 + (u32)i * 4, rd(e, 0x61e000 + (u32)i * 0x800));
    }
    /* take the display away from the VBIOS */
    u32 vreg = e->g->gf119 ? 0x6100ac : 0x610024;
    if (rd(e, vreg) & 0x00000100) {
        u32 last;
        wr(e, vreg, 0x00000100);
        save(e, 0x6194e8);
        mask(e, 0x6194e8, 0x00000001, 0);
        if (wait(e, 0x6194e8, 0x00000002, 0, e->timeout_us, &last)) {
            fail(e, "the VBIOS did not let go of the display (0x6194e8 = %08x)", last);
            return E_TIMEOUT;
        }
        nv_evo_log(e, "took the display from the VBIOS");
    }
    swr(e, 0x610010, (u32)(e->inst >> 8) | 9);
    /* supervisor interrupts on (their status is polled), everything else off */
    if (e->g->gf119) {
        swr(e, 0x610090, 0);
        swr(e, 0x6100a0, 0);
        swr(e, 0x6100b0, 0x00000307);
        for (int h = 0; h < e->nheads; h++)
            if (e->head_mask & (1u << h)) smask(e, 0x616308 + (u32)h * 0x800, 0x00000111, 0x00000010);   /* underflow reporting off */
    } else {
        swr(e, 0x61002c, 0x00000370);
        swr(e, 0x610028, 0);
    }
    or_power(e);

    /* the core channel: stop the firmware's, start ours */
    save(e, c);
    if (rd(e, c) & 0x00000003) {
        nv_evo_log(e, "the firmware's core channel is active (%08x): stopping it", rd(e, c));
        if ((r = chan_fini(e))) { fail(e, "the firmware's core channel did not stop"); return r; }
    }
    if ((r = chan_init(e))) return r;
    e->chan_up = 1;
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_core_init_methods(e->g, &s, EVO_HANDLE_SYNC);
    if ((r = nv_evo_submit(e, &s, 0))) { e->chan_up = 0; return r; }
    nv_evo_log(e, "core channel running: push buffer at %llx, instance at %llx, notifier at %llx", (unsigned long long)e->push,
               (unsigned long long)e->inst, (unsigned long long)e->sync);
    return 0;
}

void nv_evo_release(nv_evo_t *e) {
    /* the registers saved at takeover, except: the channel's own, the
     * instance pointer and the VBIOS ownership while our channel runs (a
     * later update would resolve handles in the firmware's table), and the
     * GPU's interrupt enables, which stay masked (nobody answers them) */
    if (!e->g || !e->r) return;
    u32 c = nv_evo_core_ctrl_reg(e->g), p = push_reg(e);
    for (int i = e->saved.n - 1; i >= 0; i--) {
        u32 reg = e->saved.r[i].reg;
        if (e->chan_up && (reg == c || reg == p || reg == p + 4 || reg == p + 8 || reg == 0x610010 || reg == 0x6194e8)) continue;
        if (reg == 0x000140 || reg == 0x000144) continue;
        wr(e, reg, e->saved.r[i].val);
    }
}

/* ---------------- helpers for the driver ---------------- */

int nv_edid_digital(const u8 *e, int len) { return e && len >= 128 && (e[20] & 0x80); }

int nv_edid_hdmi(const u8 *e, int len) {
    if (!e || len < 256) return 0;
    for (int b = 128; b + 128 <= len; b += 128) {
        const u8 *x = e + b;
        if (x[0] != 0x02) continue;                      /* CEA-861 extension */
        int end = x[2] < 4 || x[2] > 127 ? 127 : x[2];
        for (int i = 4; i < end;) {
            int tag = x[i] >> 5, n = x[i] & 31;
            if (i + 1 + n > end) break;
            if (tag == 3 && n >= 3 && x[i + 1] == 0x03 && x[i + 2] == 0x0C && x[i + 3] == 0x00) return 1;
            i += 1 + n;
        }
    }
    return 0;
}

int nv_evo_fw_cfg(const nv_evo_t *e, u64 fb_offset, u32 fb_pitch, int fb_w, int fb_h, nv_evo_head_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->or_id = -1;
    if (!e->fw.valid || fb_w <= 0 || fb_h <= 0 || !fb_pitch) return E_NOTSUP;
    const nv_evo_head_state_t *h = &e->fw.hs;
    u32 khz = h->hz / 1000;
    int act_w = h->hblanks - h->hblanke, act_h = h->vblanks - h->vblanke;
    if (!khz || act_w <= 0 || act_h <= 0 || h->vtotal <= act_h + 2) return E_NOTSUP;
    c->head = e->fw.head;
    c->on = 1;
    c->r.clock_khz = khz;
    c->r.htotal = h->htotal; c->r.hsynce = h->hsynce; c->r.hblanke = h->hblanke; c->r.hblanks = h->hblanks;
    c->r.vtotal = h->vtotal; c->r.vsynce = h->vsynce; c->r.vblanke = h->vblanke; c->r.vblanks = h->vblanks;
    c->r.vblank2s = 1;
    c->r.blankus = (u32)((u64)(h->vtotal - act_h - 2) * h->htotal * 1000 / khz);
    if (e->g->gf119) {
        c->r.nhsync = (h->orctrl >> 3) & 1;
        c->r.nvsync = (h->orctrl >> 4) & 1;
        c->depth = (h->orctrl >> 6) & 0xF;
    } else if (!e->fw.or_type) {
        c->r.nhsync = (e->fw.os.ctrl >> 12) & 1;
        c->r.nvsync = (e->fw.os.ctrl >> 13) & 1;
        c->depth = (e->fw.os.ctrl >> 16) & 0xF;
    }
    c->w = (u16)fb_w;
    c->h = (u16)fb_h;
    c->out_w = (u16)act_w;
    c->out_h = (u16)act_h;
    c->offset = fb_offset;
    c->pitch = fb_pitch;
    if (fb_pitch & 255) c->storage_raw = h->storage;     /* the firmware's own pitch field */
    c->vram_handle = EVO_HANDLE_VRAM;
    c->or_type = e->fw.or_type;
    c->or_id = e->fw.or_id;
    c->proto = e->fw.or_type ? EVO_PROTO_CRT : e->fw.os.proto_evo;
    c->or_heads = e->fw.os.heads ? e->fw.os.heads : (1u << c->head);
    return 0;
}
