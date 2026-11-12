/* Halcyon OS - NVIDIA GPUs (M10e): PFIFO, the GPU's virtual memory and a
 * copy engine without the kernel. See fifo.h; ported from nouveau (MIT,
 * Copyright 2012-2018 Red Hat Inc.; the notice is in THIRD_PARTY_NOTICES.md).
 *
 * Every wait is bounded (now_us); every failure says why in f->why and the
 * log. A register reading all ones or a PRI error, a runlist that stays
 * pending, a preempt that never finishes, a semaphore that never comes back,
 * an MMU fault, a PBDMA or copy engine error: each ends in an error, never a
 * loop, and nv_fifo_stop puts back every register written. */
#include "fifo.h"
#include "core/lib.h"

/* ---------------- generations ---------------- */

/*                                kind        name     ce      chan    vmm           bind runl_inst tsg tmo regs ceGP chids */
static const nv_fifo_gen_t gk104 = { FIFO_GK104, "GK104", 0xa0b5, 0xa06f, NV_VMM_GF100, 1, 0, 0, 0, 0, 0, 4096 };
static const nv_fifo_gen_t gk110 = { FIFO_GK110, "GK110", 0xa0b5, 0xa16f, NV_VMM_GF100, 1, 0, 0, 0, 0, 0, 4096 };
static const nv_fifo_gen_t gk208 = { FIFO_GK208, "GK208", 0xa0b5, 0xa06f, NV_VMM_GF100, 1, 0, 0, 1, 0, 0, 1024 };
static const nv_fifo_gen_t gm107 = { FIFO_GM107, "GM107", 0xb0b5, 0xa16f, NV_VMM_GF100, 0, 1, 0, 1, 0, 0, 2048 };
static const nv_fifo_gen_t gm200 = { FIFO_GM200, "GM200", 0xb0b5, 0xb06f, NV_VMM_GM200, 0, 1, 0, 1, 1, 0, 0 };
static const nv_fifo_gen_t gp100 = { FIFO_GP100, "GP100", 0xc0b5, 0xc06f, NV_VMM_GP100, 0, 1, 1, 1, 1, 1, 0 };
static const nv_fifo_gen_t gp102 = { FIFO_GP100, "GP100 (GP102 copy engine)", 0xc1b5, 0xc06f, NV_VMM_GP100, 0, 1, 1, 1, 1, 1, 0 };

/* nouveau's device table (nvkm/engine/device/base.c): which fifo and copy
 * engine each chipset has */
const nv_fifo_gen_t *nv_fifo_gen_for(u16 cs, const char **why) {
    const char *w = NULL;
    const nv_fifo_gen_t *g = NULL;
    switch (cs) {
    case 0xe4: case 0xe6: case 0xe7: g = &gk104; break;
    case 0xf0: case 0xf1: g = &gk110; break;
    case 0x106: case 0x108: g = &gk208; break;
    case 0x117: case 0x118: g = &gm107; break;
    case 0x120: case 0x124: case 0x126: g = &gm200; break;
    case 0x130: g = &gp100; break;
    case 0x132: case 0x134: case 0x136: case 0x137: case 0x138: g = &gp102; break;
    case 0xea: case 0x12b: case 0x13b: w = "a Tegra GPU (no PCI display)"; break;
    default:
        if (cs < 0x50) w = "before Tesla: no GPFIFO channels";
        else if (cs < 0xc0) w = "Tesla: the copy engine (GT215 on) runs falcon microcode, which is not loaded";
        else if (cs < 0xe0) w = "Fermi: the copy engines run falcon microcode (nouveau's gf100.fuc3), which is not loaded";
        else if (cs >= 0x140) w = "Volta or later: not supported until Halcyon 1.1";
        else w = "a chipset nouveau's device table does not list";
        break;
    }
    if (why) *why = g ? NULL : w;
    return g;
}

/* ---------------- small helpers ---------------- */

void nv_fifo_log(nv_fifo_t *f, const char *fmt, ...) {
    if (!f->ops.log) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    f->ops.log(f->ops.ctx, line);
}

static int fail(nv_fifo_t *f, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(nv_fifo_t *f, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->why, sizeof f->why, fmt, ap);
    va_end(ap);
    nv_fifo_log(f, "copy engine: %s", f->why);
    return err;
}

static u32 rd(nv_fifo_t *f, u32 reg) { return f->r->rd32(f->r->ctx, reg); }
static void wr(nv_fifo_t *f, u32 reg, u32 v) { f->r->wr32(f->r->ctx, reg, v); }
static u32 mask(nv_fifo_t *f, u32 reg, u32 m, u32 v) {
    u32 o = rd(f, reg);
    wr(f, reg, (o & ~m) | v);
    return o;
}
static u64 now(nv_fifo_t *f) { return f->r->now_us(f->r->ctx); }

/* remember a register before the first write (nv_fifo_stop puts it back) */
static void save(nv_fifo_t *f, u32 reg) {
    for (int i = 0; i < f->nsaved; i++) if (f->saved[i].reg == reg) return;
    if (f->nsaved >= FIFO_SAVE_MAX) { nv_fifo_log(f, "copy engine: no room to save %06x", reg); return; }
    f->saved[f->nsaved].reg = reg;
    f->saved[f->nsaved].val = rd(f, reg);
    f->nsaved++;
}
static void swr(nv_fifo_t *f, u32 reg, u32 v) { save(f, reg); wr(f, reg, v); }
static void smask(nv_fifo_t *f, u32 reg, u32 m, u32 v) { save(f, reg); mask(f, reg, m, v); }

/* wait until (reg & m) == v, at most us; 0 or E_TIMEOUT */
static int wait(nv_fifo_t *f, u32 reg, u32 m, u32 v, u32 us, u32 *last) {
    u64 end = now(f) + us;
    for (;;) {
        u32 x = rd(f, reg);
        if (last) *last = x;
        if (nv_bad_read(x)) return E_IO;
        if ((x & m) == v) return 0;
        if (now(f) >= end) return E_TIMEOUT;
        if (f->r->delay_us) f->r->delay_us(f->r->ctx, 1);
    }
}

/* wait until some bit of m is set, at most us; 0 or E_TIMEOUT / E_IO */
static int wait_any(nv_fifo_t *f, u32 reg, u32 m, u32 us, u32 *last) {
    u64 end = now(f) + us;
    for (;;) {
        u32 x = rd(f, reg);
        if (last) *last = x;
        if (nv_bad_read(x)) return E_IO;
        if (x & m) return 0;
        if (now(f) >= end) return E_TIMEOUT;
        if (f->r->delay_us) f->r->delay_us(f->r->ctx, 1);
    }
}

/* VRAM through the CPU window */
static int vok(nv_fifo_t *f, u64 off, u64 n) { return f->vram && off + n <= f->vram_size && off + n >= off; }
static void vwr32(nv_fifo_t *f, u64 off, u32 v) { if (vok(f, off, 4)) *(volatile u32 *)(f->vram + off) = v; }
static u32 vrd32(nv_fifo_t *f, u64 off) { return vok(f, off, 4) ? *(volatile u32 *)(f->vram + off) : 0xFFFFFFFFu; }
static void vzero(nv_fifo_t *f, u64 off, u32 n) { for (u32 i = 0; i < n; i += 4) vwr32(f, off + i, 0); }

static void wmb(nv_fifo_t *f) { if (f->ops.wmb) f->ops.wmb(f->ops.ctx); }

/* writes through BAR1 reach VRAM before the GPU reads (nvkm_bar_flush: 0x070000) */
static int bar_flush(nv_fifo_t *f) {
    wmb(f);
    wr(f, 0x070000, 0x00000001);
    u32 last;
    int r = wait(f, 0x070000, 0x00000002, 0, 2000, &last);
    if (r == E_IO) return fail(f, E_IO, "the card's registers are unreadable (070000 = %08x)", last);
    if (r) return fail(f, E_TIMEOUT, "the BAR flush (070000) did not finish: %08x", last);
    return 0;
}

/* ---------------- the device topology ---------------- */

int nv_top_read(const nv_regs_t *r, nv_top_t *t) {
    memset(t, 0, sizeof *t);
    nv_top_dev_t *info = NULL;
    u32 type = ~0u;
    int inst = 0;
    for (int i = 0; i < 64; i++) {
        u32 data = r->rd32(r->ctx, 0x022700 + (u32)i * 4);
        t->raw[i] = data;
        if (nv_bad_read(data)) break;
        if (!info) {
            if (t->n >= NV_TOP_MAX) break;
            info = &t->dev[t->n];
            memset(info, 0, sizeof *info);
            info->engine = info->runlist = info->reset = info->fault = info->intr = -1;
            type = ~0u;
            inst = 0;
        }
        switch (data & 3) {
        case 0: continue;                              /* NOT_VALID */
        case 1:                                        /* DATA */
            inst = (int)((data & 0x3c000000) >> 26);
            if (data & 4) info->fault = (int)((data & 0x000003f8) >> 3);
            break;
        case 2:                                        /* ENUM */
            if (data & 0x20) info->engine = (int)((data & 0x3c000000) >> 26);
            if (data & 0x10) info->runlist = (int)((data & 0x01e00000) >> 21);
            if (data & 0x08) info->intr = (int)((data & 0x000f8000) >> 15);
            if (data & 0x04) info->reset = (int)((data & 0x00003e00) >> 9);
            break;
        case 3:                                        /* ENGINE_TYPE */
            type = (data & 0x7ffffffc) >> 2;
            break;
        }
        if (data & 0x80000000) continue;              /* the entry goes on */
        info->raw_type = type;
        info->type = NV_ENG_OTHER;
        info->inst = 0;
        switch (type) {
        case 0x00: info->type = NV_ENG_GR; break;
        case 0x01: case 0x02: case 0x03: info->type = NV_ENG_CE; info->inst = (int)type - 1; break;
        case 0x13: info->type = NV_ENG_CE; info->inst = inst; break;   /* LCE (Pascal) */
        }
        t->n++;
        info = NULL;
    }
    return t->n;
}

int nv_top_pick_ce(const nv_top_t *t) {
    int best = -1;
    for (int i = 0; i < t->n; i++) {
        const nv_top_dev_t *d = &t->dev[i];
        if (d->type != NV_ENG_CE || d->runlist < 0 || d->engine < 0) continue;
        int gr = 0;
        for (int k = 0; k < t->n; k++) if (t->dev[k].type == NV_ENG_GR && t->dev[k].runlist == d->runlist) gr = 1;
        if (gr) continue;                             /* GRCE: its runlist needs FECS for context switches */
        if (best < 0 || d->inst < t->dev[best].inst) best = i;
    }
    return best;
}

/* ---------------- method streams ---------------- */

static void put(nv_fifo_stream_t *s, u32 w) {
    if (s->n >= (int)(sizeof s->w / sizeof s->w[0])) { s->overflow = 1; return; }
    s->w[s->n++] = w;
}

void nv_ce_object_methods(nv_fifo_stream_t *s, u16 ce_class) {
    put(s, nv_fifo_incr(NV_SUBC_CE, 0x0000, 1));
    put(s, ce_class);
}

/* nve0_bo_move_copy, with a rectangle instead of pages */
void nv_ce_copy_methods(nv_fifo_stream_t *s, u64 dst, u32 dst_pitch, u64 src, u32 src_pitch, u32 w_bytes, u32 h) {
    put(s, nv_fifo_incr(NV_SUBC_CE, CE_OFFSET_IN_UPPER, 8));
    put(s, (u32)(src >> 32));
    put(s, (u32)src);
    put(s, (u32)(dst >> 32));
    put(s, (u32)dst);
    put(s, src_pitch);
    put(s, dst_pitch);
    put(s, w_bytes);                                   /* LINE_LENGTH_IN: bytes without remapping */
    put(s, h);
    put(s, nv_fifo_immd(NV_SUBC_CE, CE_LAUNCH_DMA, CE_LAUNCH_COPY));
}

/* nvc0b5_migrate_clear, with one 32-bit component, a rectangle and virtual
 * addresses; the source is set to the destination (never read: every
 * component is a constant) so no pitch check can trip on it */
void nv_ce_fill_methods(nv_fifo_stream_t *s, u64 dst, u32 pitch, u32 w, u32 h, u32 xrgb) {
    put(s, nv_fifo_incr(NV_SUBC_CE, CE_SET_REMAP_CONST_A, 3));
    put(s, xrgb);
    put(s, 0);
    put(s, CE_REMAP_FILL32);
    put(s, nv_fifo_incr(NV_SUBC_CE, CE_OFFSET_IN_UPPER, 8));
    put(s, (u32)(dst >> 32));
    put(s, (u32)dst);
    put(s, (u32)(dst >> 32));
    put(s, (u32)dst);
    put(s, pitch);
    put(s, pitch);
    put(s, w);                                         /* LINE_LENGTH_IN: components when remapping */
    put(s, h);
    put(s, nv_fifo_immd(NV_SUBC_CE, CE_LAUNCH_DMA, CE_LAUNCH_COPY | CE_LAUNCH_REMAP));
}

/* nvc0_fence_emit32 without NON_STALL_INTERRUPT (nothing would answer it) */
void nv_host_release_methods(nv_fifo_stream_t *s, u64 va, u32 payload) {
    put(s, nv_fifo_incr(NV_SUBC_HOST, 0x0010, 4));
    put(s, (u32)(va >> 32) & 0xFF);
    put(s, (u32)va);
    put(s, payload);
    put(s, HOST_SEM_RELEASE);
}

/* ---------------- page tables ---------------- */

u64 nv_vmm_gf100_pte(u64 addr, int aper, int ro, int vol) {
    return (addr >> 8) | 1 | ((u64)(ro ? 1 : 0) << 2) | ((u64)(vol ? 1 : 0) << 32) | ((u64)(aper & 3) << 33);
}
/* gf100_vmm_pgd_pde, page tables in VRAM: the big one in the low word, the small one in the high word */
u64 nv_vmm_gf100_pde(u64 lpt, u64 spt) {
    u64 d = 0;
    if (lpt) d |= 1 | (lpt >> 8);
    if (spt) d |= (1ull << 32) | (spt << 24);
    return d;
}
u64 nv_vmm_gp100_pte(u64 addr, int aper, int ro, int vol) {
    return (addr >> 4) | 1 | ((u64)(aper & 3) << 1) | ((u64)(vol ? 1 : 0) << 3) | ((u64)(ro ? 1 : 0) << 6);
}
u64 nv_vmm_gp100_pde(u64 pt) { return (pt >> 4) | (1ull << 1); }

u64 nv_vmm_pdb(const nv_vmm_t *v) {
    u64 b = v->pd;                                     /* aperture 0: VRAM */
    if (v->fmt == NV_VMM_GM200 && v->pdb64k) b |= 1ull << 11;
    if (v->fmt == NV_VMM_GP100) b |= (1ull << 10) | (1ull << 11);   /* VER2, 64 KiB big pages */
    return b;
}

static void pw64(nv_vmm_t *v, u64 off, u64 val) {
    if (off < v->base || off + 8 > v->base + v->size) { v->err = E_INVAL; return; }
    volatile u32 *p = (volatile u32 *)(v->cpu + (off - v->base));
    p[0] = (u32)val;
    p[1] = (u32)(val >> 32);
    v->writes++;
}

static u64 pt_alloc(nv_vmm_t *v, u64 bytes) {
    u64 at = ALIGN_UP(v->used, 0x1000ull);
    if (at + bytes > v->size) { v->err = E_NOMEM; return 0; }
    v->used = at + bytes;
    volatile u32 *p = (volatile u32 *)(v->cpu + at);
    for (u64 i = 0; i < bytes / 4; i++) p[i] = 0;
    return v->base + at;
}

static int pde_shift(const nv_vmm_t *v) { return v->bigpage == 16 ? 26 : 27; }

int nv_vmm_init(nv_vmm_t *v, int fmt, int bigpage, u8 *cpu, u64 base, u64 size, u64 limit) {
    memset(v, 0, sizeof *v);
    v->fmt = fmt;
    v->bigpage = bigpage == 16 ? 16 : 17;
    v->pdb64k = fmt == NV_VMM_GM200 && v->bigpage == 16;
    v->cpu = cpu;
    v->base = base;
    v->size = size;
    v->limit = limit;
    if (!cpu || (base & 0xFFF) || limit > NV_FIFO_VA_LIMIT) return v->err = E_INVAL;
    if (fmt == NV_VMM_GP100) {
        v->pd = pt_alloc(v, 0x1000);                   /* PD3: 4 entries (VA bits 48:47) */
        v->pd2 = pt_alloc(v, 0x1000);                  /* PD2: VA 46:38 */
        v->pd1 = pt_alloc(v, 0x1000);                  /* PD1: VA 37:29 */
        if (v->err) return v->err;
        pw64(v, v->pd, nv_vmm_gp100_pde(v->pd2));
        pw64(v, v->pd2, nv_vmm_gp100_pde(v->pd1));
    } else {
        v->pd = pt_alloc(v, 0x1000);                   /* 512 entries: 64 / 32 GiB, more than the limit */
    }
    return v->err;
}

/* the small page table of the region holding va (GP100: 2 MiB, GF100: a directory entry) */
static u64 spt_for(nv_vmm_t *v, u64 va) {
    u64 span = v->fmt == NV_VMM_GP100 ? (1ull << 21) : (1ull << pde_shift(v));
    u64 key = va & ~(span - 1);
    for (int i = 0; i < v->nspt; i++) if (v->spt[i].va == key) return v->spt[i].off;
    if (v->nspt >= NV_VMM_SPTS) { v->err = E_NOMEM; return 0; }
    u64 bytes = v->fmt == NV_VMM_GP100 ? 0x1000 : (span >> 12) * 8;
    u64 off = pt_alloc(v, bytes);
    if (!off) return 0;
    v->spt[v->nspt].va = key;
    v->spt[v->nspt].off = off;
    v->nspt++;
    if (v->fmt == NV_VMM_GP100) {
        u64 pd0 = v->pd0[va >> 29];
        if (!pd0) return 0;
        u64 e = pd0 + ((va >> 21) & 255) * 16;
        pw64(v, e, 0);                                 /* no big page table */
        pw64(v, e + 8, nv_vmm_gp100_pde(off));         /* the small one */
    } else {
        u32 i = (u32)(va >> pde_shift(v));
        pw64(v, v->pd + i * 8, nv_vmm_gf100_pde(v->lpt[i], off));
    }
    return off;
}

static u64 pd0_for(nv_vmm_t *v, u64 va) {
    u32 i = (u32)(va >> 29);
    if (i >= sizeof v->pd0 / sizeof v->pd0[0]) { v->err = E_INVAL; return 0; }
    if (!v->pd0[i]) {
        v->pd0[i] = pt_alloc(v, 0x1000);               /* 256 dual entries of 16 bytes */
        if (!v->pd0[i]) return 0;
        pw64(v, v->pd1 + i * 8, nv_vmm_gp100_pde(v->pd0[i]));
    }
    return v->pd0[i];
}

int nv_vmm_map_vram(nv_vmm_t *v, u64 va, u64 off, u64 size) {
    if (v->err) return v->err;
    if (va + size > v->limit) return E_INVAL;
    if (v->fmt == NV_VMM_GP100) {
        if ((va | off | size) & 0x1FFFFF) return E_INVAL;
        for (u64 o = 0; o < size; o += 1ull << 21) {
            u64 pd0 = pd0_for(v, va + o);
            if (!pd0) return v->err ? v->err : E_NOMEM;
            u64 e = pd0 + (((va + o) >> 21) & 255) * 16;
            pw64(v, e, nv_vmm_gp100_pte(off + o, 0, 0, 0));   /* a 2 MiB page */
            pw64(v, e + 8, 0);
        }
        return v->err;
    }
    u64 bp = 1ull << v->bigpage;
    if ((va | off | size) & (bp - 1)) return E_INVAL;
    int sh = pde_shift(v);
    for (u64 o = 0; o < size; o += bp) {
        u64 a = va + o;
        u32 pi = (u32)(a >> sh);
        if (pi >= 256) return E_INVAL;
        if (!v->lpt[pi]) {
            v->lpt[pi] = pt_alloc(v, (1ull << (sh - v->bigpage)) * 8);
            if (!v->lpt[pi]) return v->err;
            u64 spt = 0;
            for (int k = 0; k < v->nspt; k++) if (v->spt[k].va == (a & ~((1ull << sh) - 1))) spt = v->spt[k].off;
            pw64(v, v->pd + pi * 8, nv_vmm_gf100_pde(v->lpt[pi], spt));
        }
        pw64(v, v->lpt[pi] + ((a >> v->bigpage) & ((1ull << (sh - v->bigpage)) - 1)) * 8, nv_vmm_gf100_pte(off + o, 0, 0, 0));
    }
    return v->err;
}

int nv_vmm_map_sys(nv_vmm_t *v, u64 va, u64 phys, u64 size) {
    if (v->err) return v->err;
    if ((va | phys | size) & 0xFFF || va + size > v->limit) return E_INVAL;
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 a = va + o;
        if (v->fmt == NV_VMM_GP100 && !pd0_for(v, a)) return v->err ? v->err : E_NOMEM;
        u64 spt = spt_for(v, a);
        if (!spt) return v->err ? v->err : E_NOMEM;
        if (v->fmt == NV_VMM_GP100) pw64(v, spt + ((a >> 12) & 511) * 8, nv_vmm_gp100_pte(phys + o, 2, 1, 1));
        else pw64(v, spt + ((a >> 12) & ((1ull << (pde_shift(v) - 12)) - 1)) * 8, nv_vmm_gf100_pte(phys + o, 2, 1, 1));
    }
    return v->err;
}

void nv_vmm_unmap_sys(nv_vmm_t *v, u64 va, u64 size) {
    for (u64 o = 0; o < size; o += 0x1000) {
        u64 a = va + o;
        u64 span = v->fmt == NV_VMM_GP100 ? (1ull << 21) : (1ull << pde_shift(v));
        for (int i = 0; i < v->nspt; i++) {
            if (v->spt[i].va != (a & ~(span - 1))) continue;
            u64 idx = v->fmt == NV_VMM_GP100 ? ((a >> 12) & 511) : ((a >> 12) & ((1ull << (pde_shift(v) - 12)) - 1));
            pw64(v, v->spt[i].off + idx * 8, 0);
        }
    }
}

/* gf100_vmm_invalidate for our page directory (never ALL_PDB: the
 * firmware's BAR1 mappings are left alone) */
static int tlb_flush(nv_fifo_t *f) {
    nv_vmm_t *v = &f->vmm;
    u32 last;
    if (bar_flush(f)) return E_TIMEOUT;
    if (wait_any(f, 0x100c80, 0x00ff0000, f->timeout_us, &last))   /* a free slot: the field is not 0 */
        return fail(f, E_TIMEOUT, "the MMU has no free invalidate slot (100c80 = %08x)", last);
    u32 type = 0x00000001;                             /* PAGE_ALL */
    if (v->fmt == NV_VMM_GP100) type |= 5u << 24;      /* CACHE_LEVEL_UP_TO_PDE3: every level */
    wr(f, 0x100cb8, (u32)((v->pd >> 12) << 4));        /* aperture 0: VRAM */
    if (v->fmt == NV_VMM_GP100) wr(f, 0x100cec, (u32)(v->pd >> 36));
    wr(f, 0x100cbc, 0x80000000u | type);
    if (wait_any(f, 0x100c80, 0x00008000, f->timeout_us, &last))
        return fail(f, E_TIMEOUT, "the MMU invalidate was not queued (100c80 = %08x)", last);
    v->writes = 0;
    return 0;
}

/* ---------------- the channel ---------------- */

static u64 inst_off(nv_fifo_t *f) { return f->blk + FIFO_BLK_INST; }
static u64 userd_off(nv_fifo_t *f) { return f->userd + (u64)f->chid * 0x200; }
static u64 sem_va(nv_fifo_t *f) { return NV_FIFO_VA_VRAM + f->blk + FIFO_BLK_SEM; }

static int runl_commit(nv_fifo_t *f, int count) {
    u64 rl = f->blk + FIFO_BLK_RUNL;
    if (bar_flush(f)) return E_TIMEOUT;
    wr(f, 0x002270, (u32)(rl >> 12));                  /* target 0: VRAM */
    wr(f, 0x002274, ((u32)f->runl << 20) | (u32)count);
    u32 last;
    int r = wait(f, 0x002284 + (u32)f->runl * 8, 0x00100000, 0, f->timeout_us, &last);
    if (r) return fail(f, r, "runlist %d stays pending (%06x = %08x)", f->runl, 0x002284 + f->runl * 8, last);
    return 0;
}

/* gk104_chan_ramfc_write with the instance block's page directory (gf100_vmm_join_) */
static void ramfc(nv_fifo_t *f) {
    u64 inst = inst_off(f), ud = userd_off(f);
    u64 gp = NV_FIFO_VA_VRAM + f->blk + FIFO_BLK_GPFIFO;
    u64 pdb = nv_vmm_pdb(&f->vmm);
    vzero(f, inst, 0x1000);
    vwr32(f, inst + 0x08, (u32)ud);
    vwr32(f, inst + 0x0c, (u32)(ud >> 32));
    vwr32(f, inst + 0x10, 0x0000face);
    vwr32(f, inst + 0x30, 0xfffff902);
    vwr32(f, inst + 0x48, (u32)gp);
    vwr32(f, inst + 0x4c, (u32)(gp >> 32) | (9u << 16));       /* ilog2(512 entries) */
    vwr32(f, inst + 0x84, 0x20400000);
    vwr32(f, inst + 0x94, 0x30000000 | 0xfff);
    vwr32(f, inst + 0x9c, 0x00000100);
    vwr32(f, inst + 0xac, 0x0000001f);
    vwr32(f, inst + 0xe4, 0x00000020);                         /* a kernel channel (priv) */
    vwr32(f, inst + 0xe8, (u32)f->chid);
    vwr32(f, inst + 0xb8, 0xf8000000);
    vwr32(f, inst + 0xf8, 0x10003080);
    vwr32(f, inst + 0xfc, 0x10000010);
    vwr32(f, inst + 0x200, (u32)pdb);
    vwr32(f, inst + 0x204, (u32)(pdb >> 32));
    vwr32(f, inst + 0x208, (u32)(f->vmm.limit - 1));
    vwr32(f, inst + 0x20c, (u32)((f->vmm.limit - 1) >> 32));
    vzero(f, ud, 0x200);                                       /* gf100_chan_userd_clear and the rest */
}

static void runlist_entries(nv_fifo_t *f, int *count) {
    u64 rl = f->blk + FIFO_BLK_RUNL, o = rl;
    if (f->g->tsg) {                                           /* gk110_runl_insert_cgrp: 1 channel, timeslice 128 << 3 */
        vwr32(f, o, (1u << 26) | (128u << 18) | (3u << 14) | 0x00002000 | (u32)f->chid);
        vwr32(f, o + 4, 0);
        o += 8;
    }
    vwr32(f, o, (u32)f->chid | (f->g->tsg ? (0u << 14) : 0));  /* gp100: runqueue 0 */
    vwr32(f, o + 4, f->g->runl_inst ? (u32)(inst_off(f) >> 12) : 0);
    o += 8;
    *count = (int)((o - rl) / 8);
}

static int find_pbdma(nv_fifo_t *f) {
    u32 present;
    if (f->g->regs_count) {
        u32 n = rd(f, 0x002004);
        if (nv_bad_read(n)) return fail(f, E_IO, "PFIFO registers unreadable (002004 = %08x)", n);
        f->npbdma = (int)(n & 0xff);
        present = f->npbdma >= 32 ? ~0u : (1u << f->npbdma) - 1;
    } else {
        save(f, 0x000204);
        u32 old = rd(f, 0x000204);
        wr(f, 0x000204, 0xffffffffu);
        present = rd(f, 0x000204);
        wr(f, 0x000204, old);
        if (nv_bad_read(present)) return fail(f, E_IO, "PMC_ENABLE_PB unreadable (%08x)", present);
        f->npbdma = 0;
        for (u32 m = present; m; m &= m - 1) f->npbdma++;
    }
    f->pbdma = -1;
    for (int i = 0; i < 32 && f->pbdma < 0; i++) {
        if (!(present & (1u << i))) continue;
        u32 m = rd(f, 0x002390 + (u32)i * 4);
        if (!nv_bad_read(m) && (m & (1u << f->runl))) f->pbdma = i;
    }
    if (f->pbdma < 0) return fail(f, E_NODEV, "no PBDMA serves runlist %d (%d PBDMAs)", f->runl, f->npbdma);
    u32 pb = rd(f, 0x000204);
    if (!(pb & (1u << f->pbdma))) swr(f, 0x000204, pb | (1u << f->pbdma));
    return 0;
}

static void clear_ce_status(nv_fifo_t *f) {
    if (f->g->ce_gp100) {
        u32 b = (u32)f->ce_inst * 0x80, s = rd(f, 0x104410 + b);
        if (s && !nv_bad_read(s)) { nv_fifo_log(f, "copy engine %d: status %08x left from before, cleared", f->ce_inst, s); wr(f, 0x104410 + b, s); }
    } else {
        u32 b = (u32)f->ce_inst * 0x1000, s = rd(f, 0x104908 + b);
        if (s && !nv_bad_read(s)) { nv_fifo_log(f, "copy engine %d: status %08x left from before, cleared", f->ce_inst, s); wr(f, 0x104908 + b, s); }
    }
}

int nv_fifo_start(nv_fifo_t *f) {
    f->why[0] = 0;
    f->up = f->dead = f->bound = 0;
    f->nsaved = 0;
    f->pmc_set = 0;
    f->seq = f->done = 0;
    f->gp_put = f->push_put = 0;
    f->nin = f->inhead = 0;
    f->q.n = f->q.overflow = 0;
    if (!f->timeout_us) f->timeout_us = 100000;
    if (!f->fence_us) f->fence_us = 200000;
    if (!f->g) return fail(f, E_NOTSUP, "no copy engine generation for this chipset");
    if ((f->blk & 0x1FFFFF) || (f->userd & 0xFFF) || !vok(f, f->blk, FIFO_BLK_BYTES) || !vok(f, f->userd, 0x200))
        return fail(f, E_INVAL, "our VRAM blocks are not usable (block %llx, USERD %llx)", (unsigned long long)f->blk,
                    (unsigned long long)f->userd);
    u32 boot0 = rd(f, 0x000000);
    if (nv_bad_read(boot0)) return fail(f, E_IO, "the card's registers are unreadable (%08x)", boot0);

    /* the copy engine and its runlist (nvkm/subdev/top) */
    nv_top_read(f->r, &f->top);
    for (int i = 0; i < f->top.n; i++) {
        nv_top_dev_t *d = &f->top.dev[i];
        nv_fifo_log(f, "topology: %s%d (type %02x): engine %d, runlist %d, reset bit %d, fault %d", d->type == NV_ENG_GR ? "GR" : d->type == NV_ENG_CE ? "CE" : "other",
                    d->inst, d->raw_type, d->engine, d->runlist, d->reset, d->fault);
    }
    f->ce = nv_top_pick_ce(&f->top);
    if (f->ce < 0) return fail(f, E_NODEV, "the device topology (022700) lists no copy engine on a runlist of its own");
    nv_top_dev_t *ce = &f->top.dev[f->ce];
    f->ce_inst = ce->inst;
    f->engine = ce->engine;
    f->runl = ce->runlist;

    /* PFIFO and the copy engine powered (nvkm_mc_enable) */
    u32 pmc = rd(f, 0x000200), need = 0x00000100;
    if (nv_bad_read(pmc)) return fail(f, E_IO, "PMC_ENABLE unreadable (%08x)", pmc);
    if (ce->reset >= 0 && ce->reset < 32) need |= 1u << ce->reset;
    if ((pmc & need) != need) {
        swr(f, 0x000200, pmc | need);
        f->pmc_set = need & ~pmc;
        nv_fifo_log(f, "PMC_ENABLE %08x -> %08x (PFIFO / copy engine %d were off)", pmc, rd(f, 0x000200), f->ce_inst);
    }
    if (find_pbdma(f)) return E_NODEV;
    u32 cn = f->g->regs_count ? rd(f, 0x002008) : f->g->chids;
    if (nv_bad_read(cn) || !cn || cn > 4096) return fail(f, E_IO, "the channel count (002008) reads %08x", cn);
    f->chids = cn;
    nv_fifo_log(f, "copy engine %d (class %04x) on runlist %d, engine %d, PBDMA %d of %d; %u channels; reset bit %d", f->ce_inst,
                f->g->ce_class, f->runl, f->engine, f->pbdma, f->npbdma, cn, ce->reset);

    /* a free channel id whose USERD slot lies in our region */
    f->chid = -1;
    for (u32 c = 0; c < cn && c < 64 && f->chid < 0; c++) {
        if ((u64)(c + 1) * 0x200 > f->userd_bytes) break;
        u32 a = rd(f, 0x800000 + c * 8), b = rd(f, 0x800004 + c * 8);
        if (nv_bad_read(a) || nv_bad_read(b)) return fail(f, E_IO, "channel %u's registers are unreadable (%08x %08x)", c, a, b);
        if (!(a & 0x80000000u) && !(b & 1)) f->chid = (int)c;
    }
    if (f->chid < 0) return fail(f, E_BUSY, "no free channel among the first 64");

    /* the PBDMA (gk104_runq_init / gk208_runq_init), PFIFO (gk104_fifo_init) */
    u32 pb = (u32)f->pbdma * 0x2000;
    smask(f, 0x04013c + pb, 0x10000100, 0x00000000);
    wr(f, 0x040108 + pb, 0xffffffffu);                 /* INTR */
    swr(f, 0x04010c + pb, 0xfffffeffu);                /* INTREN (the PMC enables stay off: polled) */
    wr(f, 0x040148 + pb, 0xffffffffu);                 /* HCE.INTR */
    swr(f, 0x04014c + pb, 0xffffffffu);
    if (f->g->runq_timeout) swr(f, 0x04012c + pb, 0x000f4240);
    u32 old = rd(f, 0x002254);
    if (old & 0x10000000) nv_fifo_log(f, "USERD through BAR1 was set up already (002254 = %08x): replaced while we run", old);
    swr(f, 0x002254, 0x10000000 | (u32)(f->userd >> 12));
    wr(f, 0x002100, 0xffffffffu);
    swr(f, 0x002140, 0x7fffffffu);
    clear_ce_status(f);
    if (f->flush_page) {
        u32 fp = rd(f, 0x100c10);
        swr(f, 0x100c10, (u32)(f->flush_page >> 8));
        nv_fifo_log(f, "flush page (100c10) %08x -> %08x", fp, (u32)(f->flush_page >> 8));
    }

    /* the address space: VRAM at 4 GiB, the system RAM window at 8 GiB (empty) */
    int bp = 17;
    u32 c80 = rd(f, 0x100c80);
    if (f->g->vmm == NV_VMM_GF100) bp = (c80 & 1) ? 16 : 17;
    else if (f->g->vmm == NV_VMM_GM200) bp = ((c80 & 0x1800) == 0x1800) ? 17 : (c80 & 1) ? 16 : 17;
    int r = nv_vmm_init(&f->vmm, f->g->vmm, bp, f->vram + f->blk + FIFO_BLK_PT, f->blk + FIFO_BLK_PT, FIFO_BLK_BYTES - FIFO_BLK_PT,
                        NV_FIFO_VA_LIMIT);
    if (!r) r = nv_vmm_map_vram(&f->vmm, NV_FIFO_VA_VRAM, 0, f->vram_size);
    if (r) return fail(f, r, "page tables: %s (%llu KiB used)", strerror(r), (unsigned long long)(f->vmm.used >> 10));
    f->sys_va = NV_FIFO_VA_SYS;
    f->sys_phys = f->sys_size = 0;
    nv_fifo_log(f, "page tables: %s format%s, directory at %llx, %llu MiB of VRAM at %llx, %llu KiB of tables (100c80 = %08x)",
                f->g->vmm == NV_VMM_GP100 ? "GP100 (5 levels, 2 MiB pages)" : f->g->vmm == NV_VMM_GM200 ? "GM200" : "GF100",
                f->g->vmm == NV_VMM_GP100 ? "" : bp == 16 ? ", 64 KiB big pages" : ", 128 KiB big pages", (unsigned long long)f->vmm.pd,
                (unsigned long long)(f->vram_size >> 20), (unsigned long long)NV_FIFO_VA_VRAM, (unsigned long long)(f->vmm.used >> 10), c80);
    if (tlb_flush(f)) return E_TIMEOUT;

    /* the channel: instance block, RAMFC, USERD (nvkm_chan_new_, gk104_chan_*) */
    vzero(f, f->blk + FIFO_BLK_SEM, 16);
    vzero(f, f->blk + FIFO_BLK_GPFIFO, FIFO_GP_ENTRIES * 8);
    ramfc(f);
    if (bar_flush(f)) return E_TIMEOUT;
    u32 c8 = (u32)f->chid * 8;
    if (f->g->bind_runl) mask(f, 0x800004 + c8, 0x000f0000, (u32)f->runl << 16);
    wr(f, 0x800000 + c8, 0x80000000u | (u32)(inst_off(f) >> 12));
    f->bound = 1;
    mask(f, 0x800004 + c8, 0x00000400, 0x00000400);   /* start */
    int count;
    runlist_entries(f, &count);
    if ((r = runl_commit(f, count))) return r;
    nv_fifo_log(f, "channel %d: instance %llx, USERD %llx, GPFIFO %d entries, push ring %u KiB, runlist %d (%d entries)%s; %08x %08x",
                f->chid, (unsigned long long)inst_off(f), (unsigned long long)userd_off(f), FIFO_GP_ENTRIES, FIFO_PUSH_BYTES >> 10, f->runl,
                count, f->g->tsg ? ", in channel group" : "", rd(f, 0x800000 + c8), rd(f, 0x800004 + c8));

    /* the copy engine object, then a semaphore that must come back */
    f->skips = 1;
    nv_ce_object_methods(&f->q, f->g->ce_class);
    u32 fence = nv_fifo_submit(f);
    if (!fence) return E_IO;
    u64 t0 = now(f);
    if ((r = nv_fifo_wait(f, fence, f->fence_us))) return r;
    f->up = 1;
    nv_fifo_log(f, "channel %d runs: the copy engine object is bound and the first semaphore came back after %llu us", f->chid,
                (unsigned long long)(now(f) - t0));
    return 0;
}

u64 nv_fifo_va_sys(const nv_fifo_t *f, u64 phys, u64 size) {
    if (!f->sys_size || phys < f->sys_phys || phys + size > f->sys_phys + f->sys_size || phys + size < phys) return 0;
    return f->sys_va + (phys - (f->sys_phys & ~0xFFFull));
}

int nv_fifo_map_sys(nv_fifo_t *f, u64 phys, u64 size) {
    if (f->dead) return E_IO;
    if (f->nin) return E_BUSY;
    u64 p0 = phys & ~0xFFFull, p1 = ALIGN_UP(phys + size, 0x1000ull);
    if (!size || p1 - p0 > NV_FIFO_SYS_MAX) return E_NOSPC;
    if (f->sys_size) {
        u64 o0 = f->sys_phys & ~0xFFFull;
        nv_vmm_unmap_sys(&f->vmm, f->sys_va, ALIGN_UP(f->sys_phys + f->sys_size, 0x1000ull) - o0);
        f->sys_size = 0;
    }
    int r = nv_vmm_map_sys(&f->vmm, f->sys_va, p0, p1 - p0);
    if (!r) r = tlb_flush(f);
    if (r) {
        f->vmm.err = 0;
        return r;
    }
    f->sys_phys = phys;
    f->sys_size = size;
    return 0;
}

/* ---------------- work ---------------- */

static int fence_done(u32 done, u32 fence) { return (i32)(done - fence) >= 0; }

int nv_fifo_copy(nv_fifo_t *f, u64 dst, u32 dst_pitch, u64 src, u32 src_pitch, u32 w_bytes, u32 h) {
    if (f->dead || !f->bound) return E_IO;
    if (!dst || !src || !w_bytes || !h || w_bytes > dst_pitch || w_bytes > src_pitch) return E_INVAL;
    if (f->q.n + 16 > (int)(sizeof f->q.w / sizeof f->q.w[0]) - 8) return E_NOSPC;
    nv_ce_copy_methods(&f->q, dst, dst_pitch, src, src_pitch, w_bytes, h);
    f->copies++;
    f->bytes += (u64)w_bytes * h;
    return 0;
}

int nv_fifo_fill(nv_fifo_t *f, u64 dst, u32 pitch, u32 w, u32 h, u32 xrgb) {
    if (f->dead || !f->bound) return E_IO;
    if (!dst || !w || !h || w * 4 > pitch) return E_INVAL;
    if (f->q.n + 20 > (int)(sizeof f->q.w / sizeof f->q.w[0]) - 8) return E_NOSPC;
    nv_ce_fill_methods(&f->q, dst, pitch, w, h, xrgb);
    f->fills++;
    f->bytes += (u64)w * 4 * h;
    return 0;
}

static void reap(nv_fifo_t *f) {
    while (f->nin && fence_done(f->done, f->inflight[f->inhead].fence)) {
        f->inhead = (f->inhead + 1) % FIFO_GP_ENTRIES;
        f->nin--;
    }
}

static u32 sem_read(nv_fifo_t *f) { return vrd32(f, f->blk + FIFO_BLK_SEM); }

static void sem_update(nv_fifo_t *f) {
    u32 v = sem_read(f);
    /* only a value between what we saw and what we sent counts */
    if ((i32)(v - f->done) > 0 && (i32)(f->seq - v) >= 0) {
        f->done = v;
        f->seq_us = now(f);                            /* progress: the next fence's clock starts */
        reap(f);
    }
}

/* where n bytes fit in the push ring (contiguous), or -1 */
static int push_room(nv_fifo_t *f, u32 bytes) {
    u32 p = f->push_put;
    if (!f->nin) return p + bytes <= FIFO_PUSH_BYTES ? (int)p : 0;
    u32 tail = f->inflight[f->inhead].push;
    if (tail <= p) {
        if (p + bytes <= FIFO_PUSH_BYTES) return (int)p;
        if (bytes < tail) return 0;
        return -1;
    }
    return p + bytes < tail ? (int)p : -1;
}

u32 nv_fifo_submit(nv_fifo_t *f) {
    if (f->dead || !f->bound) { f->q.n = 0; return 0; }
    if (!f->q.n && !f->skips) return f->seq;
    if (f->q.overflow) { f->q.n = f->q.overflow = 0; fail(f, E_NOSPC, "too much work queued at once"); return 0; }
    u32 fence = f->seq + 1;
    if (!fence) fence = 1;                             /* (0 means "nothing") */
    nv_host_release_methods(&f->q, sem_va(f), fence);
    u32 nw = (u32)f->q.n + (f->skips ? FIFO_SKIPS : 0), bytes = nw * 4;
    int at;
    u64 end = now(f) + f->fence_us;
    for (;;) {
        sem_update(f);
        at = push_room(f, bytes);
        if (at >= 0 && f->nin < FIFO_GP_ENTRIES - 2) break;
        int e = nv_fifo_check(f);
        if (e || now(f) >= end) {
            if (!e) fail(f, E_TIMEOUT, "no room in the push ring for %u ms: the copy engine stopped taking work", f->fence_us / 1000);
            f->dead = 1;
            f->q.n = 0;
            return 0;
        }
        f->waits++;
    }
    u64 base = f->blk + FIFO_BLK_PUSH + (u64)at;
    u32 o = 0;
    if (f->skips) for (int i = 0; i < FIFO_SKIPS; i++, o += 4) vwr32(f, base + o, 0);
    for (int i = 0; i < f->q.n; i++, o += 4) vwr32(f, base + o, f->q.w[i]);
    u32 w0, w1;
    nv_fifo_gp_entry(NV_FIFO_VA_VRAM + base, nw, &w0, &w1);
    u64 gpe = f->blk + FIFO_BLK_GPFIFO + (u64)f->gp_put * 8;
    vwr32(f, gpe, w0);
    vwr32(f, gpe + 4, w1);
    if (bar_flush(f)) { f->dead = 1; f->q.n = 0; return 0; }
    int slot = (f->inhead + f->nin) % FIFO_GP_ENTRIES;
    f->inflight[slot].fence = fence;
    f->inflight[slot].gp = f->gp_put;
    f->inflight[slot].push = (u32)at;
    if (!f->nin) f->seq_us = now(f);
    f->nin++;
    f->gp_put = (f->gp_put + 1) % FIFO_GP_ENTRIES;
    f->push_put = (u32)at + bytes;
    f->seq = fence;
    f->skips = 0;
    f->q.n = 0;
    f->submits++;
    /* GP_PUT through BAR1: the host sees the write (002254) */
    vwr32(f, userd_off(f) + 0x8c, f->gp_put);
    wmb(f);
    return fence;
}

static const char *fault_reason(u32 r) {
    static const char *n[] = { "PDE", "PDE_SIZE", "PTE", "VA_LIMIT_VIOLATION", "UNBOUND_INST_BLOCK", "PRIV_VIOLATION", "RO_VIOLATION",
                               "WO_VIOLATION", "PITCH_MASK_VIOLATION", "WORK_CREATION", "UNSUPPORTED_APERTURE", "COMPRESSION_FAILURE",
                               "UNSUPPORTED_KIND", "REGION_VIOLATION", "BOTH_PTES_VALID", "INFO_TYPE_POISONED" };
    return r < 16 ? n[r] : "?";
}

int nv_fifo_check(nv_fifo_t *f) {
    u32 intr = rd(f, 0x002100);
    if (nv_bad_read(intr)) return fail(f, E_IO, "PFIFO's status is unreadable (%08x): the card stopped answering", intr);
    u32 pb = (u32)f->pbdma * 0x2000;
    if (intr & 0x10000000) {                           /* MMU_FAULT (gf100_fifo_intr_mmu_fault) */
        u32 units = rd(f, 0x00259c);
        for (int u = 0; u < 32; u++) {
            if (!(units & (1u << u))) continue;
            u32 inst = rd(f, 0x002800 + (u32)u * 0x10), lo = rd(f, 0x002804 + (u32)u * 0x10), hi = rd(f, 0x002808 + (u32)u * 0x10),
                type = rd(f, 0x00280c + (u32)u * 0x10);
            int ours = (u64)(inst & 0x0fffffff) << 12 == inst_off(f);
            u32 reason = f->g->kind >= FIFO_GP100 ? (type & 0x1f) : (type & 0x0f);
            nv_fifo_log(f, "MMU fault: unit %d, instance %08x%s, address %08x%08x, type %08x (%s)", u, inst, ours ? " (our channel)" : "", hi, lo,
                        type, fault_reason(reason));
            if (ours) {
                f->errors++;
                return fail(f, E_IO, "MMU fault at %08x%08x (%s)", hi, lo, fault_reason(reason));
            }
        }
    }
    if (intr & 0x20000000) {                           /* PBDMA_INTR */
        u32 s0 = rd(f, 0x040108 + pb), s1 = rd(f, 0x040148 + pb);
        if ((s0 && !nv_bad_read(s0)) || (s1 && !nv_bad_read(s1))) {
            u32 addr = rd(f, 0x0400c0 + pb), data = rd(f, 0x0400c4 + pb), chid = rd(f, 0x040120 + pb) & 0xfff;
            f->errors++;
            return fail(f, E_IO, "PBDMA %d error %08x / %08x: channel %u, subchannel %u, method %04x, data %08x", f->pbdma, s0, s1, chid,
                        (addr >> 16) & 7, addr & 0x3ffc, data);
        }
    }
    if (intr & 0x00000100) { f->errors++; return fail(f, E_IO, "PFIFO scheduler error %02x (00254c)", rd(f, 0x00254c) & 0xff); }
    if (intr & 0x00000001) { f->errors++; return fail(f, E_IO, "PFIFO bind error %02x (00252c)", rd(f, 0x00252c) & 0xff); }
    if (intr & 0x00010000) { f->errors++; return fail(f, E_IO, "PFIFO channel switch error %08x (00256c)", rd(f, 0x00256c)); }
    if (intr & 0x00800000) { f->errors++; return fail(f, E_IO, "PFIFO frame buffer flush timeout"); }
    /* the copy engine's own status (gk104_ce_intr / gp100_ce_intr) */
    u32 cs, le;
    if (f->g->ce_gp100) { cs = rd(f, 0x104410 + (u32)f->ce_inst * 0x80); le = rd(f, 0x104418 + (u32)f->ce_inst * 0x80); }
    else { cs = rd(f, 0x104908 + (u32)f->ce_inst * 0x1000); le = rd(f, 0x104f14 + (u32)f->ce_inst * 0x1000); }
    if (cs && !nv_bad_read(cs)) {
        f->errors++;
        return fail(f, E_IO, "copy engine %d status %08x%s (launch error %08x)", f->ce_inst, cs, cs & 4 ? ", LAUNCHERR" : "", le);
    }
    return 0;
}

int nv_fifo_poll(nv_fifo_t *f, u32 fence) {
    if (!fence) return 1;
    if (fence_done(f->done, fence)) return 1;
    if (f->dead) return E_IO;
    sem_update(f);
    if (fence_done(f->done, fence)) return 1;
    if (nv_fifo_check(f)) { f->dead = 1; return E_IO; }
    if (f->nin && now(f) - f->seq_us > f->fence_us) {
        char st[160];
        nv_fifo_state(f, st, sizeof st);
        fail(f, E_TIMEOUT, "fence %u did not complete in %u ms (done %u; %s)", f->inflight[f->inhead].fence, f->fence_us / 1000, f->done, st);
        f->dead = 1;
        return E_TIMEOUT;
    }
    return 0;
}

int nv_fifo_wait(nv_fifo_t *f, u32 fence, u32 us) {
    u64 end = now(f) + us;
    for (;;) {
        int r = nv_fifo_poll(f, fence);
        if (r == 1) return 0;
        if (r < 0) return r;
        if (now(f) >= end) {
            char st[160];
            nv_fifo_state(f, st, sizeof st);
            fail(f, E_TIMEOUT, "fence %u did not complete in %u us (%s)", fence, us, st);
            f->dead = 1;
            return E_TIMEOUT;
        }
        if (f->r->delay_us) f->r->delay_us(f->r->ctx, 2);
    }
}

int nv_fifo_state(nv_fifo_t *f, char *buf, int n) {
    if (n < 1) return 0;
    u32 c8 = (u32)(f->chid < 0 ? 0 : f->chid) * 8, pb = (u32)(f->pbdma < 0 ? 0 : f->pbdma) * 0x2000;
    return snprintf(buf, (size_t)n, "chan %08x %08x, GP get %u put %u, sem %u, engine %08x, PBDMA %08x %08x, PFIFO %08x",
                    rd(f, 0x800000 + c8), rd(f, 0x800004 + c8), vrd32(f, userd_off(f) + 0x88), f->gp_put, sem_read(f),
                    f->engine >= 0 ? rd(f, 0x002640 + (u32)f->engine * 8) : 0, rd(f, 0x040108 + pb), rd(f, 0x040148 + pb), rd(f, 0x002100));
}

void nv_fifo_stop(nv_fifo_t *f, const char *why) {
    if (!f->bound && !f->nsaved) { f->dead = 1; f->up = 0; return; }
    nv_fifo_log(f, "copy engine stops: %s", why ? why : "released");
    if (f->bound) {
        u32 c8 = (u32)f->chid * 8, last;
        mask(f, 0x800004 + c8, 0x00000800, 0x00000800);       /* disable (gk104_chan_stop) */
        wr(f, 0x002634, f->g->tsg ? 0x01000000u | (u32)f->chid : (u32)f->chid);   /* preempt the channel / its group */
        if (wait(f, 0x002634, 0x00100000, 0, f->timeout_us, &last))
            nv_fifo_log(f, "the preempt did not finish (002634 = %08x): the channel is removed anyway", last);
        wr(f, 0x002270, (u32)((f->blk + FIFO_BLK_RUNL) >> 12));
        wr(f, 0x002274, (u32)f->runl << 20);                    /* an empty runlist */
        if (wait(f, 0x002284 + (u32)f->runl * 8, 0x00100000, 0, f->timeout_us, &last))
            nv_fifo_log(f, "the empty runlist stays pending (%08x)", last);
        wr(f, 0x800000 + c8, 0);                                /* unbind */
        f->bound = 0;
    }
    for (int i = f->nsaved - 1; i >= 0; i--) wr(f, f->saved[i].reg, f->saved[i].val);
    if (f->nsaved) nv_fifo_log(f, "copy engine: %d registers put back", f->nsaved);
    f->nsaved = 0;
    f->up = 0;
    f->dead = 1;
    f->nin = 0;
    f->q.n = 0;
}
