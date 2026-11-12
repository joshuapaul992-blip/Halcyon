/* Halcyon OS - NVIDIA GPUs (M10e): a copy engine through PFIFO, Kepler to
 * Pascal, without the kernel.
 *
 * One GPFIFO channel on a copy engine's own runlist, its instance block,
 * RAMFC and USERD, a ring of push buffers, a small GPU virtual address space
 * (VRAM mapped 1:1 at 4 GiB, a read-only window onto system RAM at 8 GiB)
 * in the page-table format of the generation, and the copy engine's
 * rectangle copies and constant fills; completion by a host semaphore
 * release into VRAM, polled. No firmware is loaded: from GK104 on the copy
 * engines are fixed-function (nouveau creates them without microcode);
 * Fermi's and Tesla's run falcon microcode, so they are not covered.
 *
 * Like evo.c this works on register callbacks (nv_regs_t) and a CPU view of
 * VRAM (BAR1, which 10b checked maps VRAM 1:1), so it is compiled for the
 * host and tested against a simulated PFIFO, MMU and copy engine
 * (tests/host/fifotest.c). The kernel side is accel.c.
 *
 * Ported from nouveau (MIT; Copyright 2012-2018 Red Hat Inc., author Ben
 * Skeggs, the notice is in THIRD_PARTY_NOTICES.md):
 *   - nvkm/engine/fifo: gk104.c, gk110.c, gk208.c, gm107.c, gm200.c,
 *     gp100.c, gf100.c, base.c, chan.c, runl.c (PBDMAs, runlists, channel
 *     groups, the instance block and RAMFC, USERD through BAR1, bind /
 *     start / stop / preempt, the interrupt status decoding)
 *   - nvkm/subdev/top/gk104.c (the device topology: engines, runlists,
 *     reset bits), nvkm/subdev/mc (PMC_ENABLE)
 *   - nvkm/subdev/mmu: vmmgf100.c, vmmgk104.c, vmmgm200.c, vmmgp100.c
 *     (page-table formats, the page directory base, TLB invalidation),
 *     nvkm/subdev/fb/gf100.c, gm200.c (big page size, the flush page)
 *   - nvkm/engine/ce: gk104.c, gp100.c (status registers)
 *   - nouveau_boa0b5.c, nouveau_dmem.c (copy and clear methods),
 *     nvc0_fence.c (the semaphore release), nouveau_dma.c / .h (GPFIFO
 *     entries, the initial NOPs)
 * Method names and fields: NVIDIA's MIT-licensed class headers
 * (open-gpu-doc: cla0b5.h, clb0b5.h, clc0b5.h, clc1b5.h, cla06f.h,
 * clc06f.h). See docs/DESIGN-M10-NVIDIA.md, "10e". */
#ifndef FIFO_H
#define FIFO_H
#include "core/types.h"
#include "drivers/nv/nvhw.h"

/* ---------------- generations ---------------- */

enum { FIFO_NONE, FIFO_GK104, FIFO_GK110, FIFO_GK208, FIFO_GM107, FIFO_GM200, FIFO_GP100 };
/* page-table formats: GF100 (two levels, also Kepler and GM10x), GM200
 * (the same with a per-instance big page size), GP100 (five levels, "VER2") */
enum { NV_VMM_GF100 = 1, NV_VMM_GM200, NV_VMM_GP100 };

typedef struct nv_fifo_gen {
    u8  kind;                   /* FIFO_* */
    const char *name;           /* nouveau's fifo implementation: "GP100" */
    u16 ce_class;               /* the copy engine class we bind: a0b5, b0b5, c0b5, c1b5 */
    u16 chan_class;             /* the channel class (a06f ... c06f): names the method set, the hardware never sees it */
    u8  vmm;                    /* NV_VMM_* */
    u8  bind_runl;              /* 0x800004 bits 19:16 carry the runlist (gk104 / gk110 / gk208) */
    u8  runl_inst;              /* the runlist's channel entry carries the instance block (gm107+) */
    u8  tsg;                    /* every channel in a channel group: a group entry first (gp100, cgrp.force) */
    u8  runq_timeout;           /* gk208_runq_init: 0x04012c = 0x000f4240 */
    u8  regs_count;             /* gm200+: channels from 0x002008, PBDMAs from 0x002004 */
    u8  ce_gp100;               /* copy engine status at 0x104410 + inst * 0x80 (else 0x104908 + inst * 0x1000) */
    u16 chids;                  /* channels when not read from 0x002008 */
} nv_fifo_gen_t;

/* NULL if the chipset has no firmware-free copy engine we drive; why
 * then says why ("Fermi: ...") */
const nv_fifo_gen_t *nv_fifo_gen_for(u16 chipset, const char **why);

/* ---------------- the device topology (0x022700, gk104_top_parse) ---------------- */

#define NV_TOP_MAX 32
enum { NV_ENG_OTHER = 0, NV_ENG_GR, NV_ENG_CE };
typedef struct nv_top_dev {
    u8  type;                   /* NV_ENG_* */
    u32 raw_type;               /* the ENGINE_TYPE field */
    int inst;                   /* copy engine instance */
    int engine, runlist, reset, fault, intr;     /* -1 when the table has none */
} nv_top_dev_t;
typedef struct nv_top {
    int n;
    nv_top_dev_t dev[NV_TOP_MAX];
    u32 raw[64];
} nv_top_t;
/* read and decode the table; returns the number of devices */
int  nv_top_read(const nv_regs_t *r, nv_top_t *t);
/* the copy engine to use: the first one whose runlist has no GR engine
 * (a channel there needs no context switch firmware); -1 none */
int  nv_top_pick_ce(const nv_top_t *t);

/* ---------------- method streams ---------------- */

#define NV_SUBC_HOST   0                         /* host methods (906f / c06f) */
#define NV_SUBC_CE     4                         /* nouveau's subchannel for a copy engine */
static inline u32 nv_fifo_incr(int subc, u32 mthd, int count) {
    return 0x20000000u | ((u32)count << 16) | ((u32)subc << 13) | ((mthd >> 2) & 0xFFF);
}
static inline u32 nv_fifo_immd(int subc, u32 mthd, u32 data) {
    return 0x80000000u | ((data & 0x1FFF) << 16) | ((u32)subc << 13) | ((mthd >> 2) & 0xFFF);
}

typedef struct nv_fifo_stream {
    u32 w[4096];
    int n;
    int overflow;
} nv_fifo_stream_t;

/* copy engine methods (a0b5 / b0b5 / c0b5 / c1b5 share these) */
#define CE_LAUNCH_DMA        0x0300
#define CE_OFFSET_IN_UPPER   0x0400
#define CE_SET_REMAP_CONST_A 0x0700
/* LAUNCH_DMA: NON_PIPELINED, FLUSH_ENABLE, SRC / DST PITCH, MULTI_LINE (nouveau's 0x386), REMAP */
#define CE_LAUNCH_COPY       0x00000386u
#define CE_LAUNCH_REMAP      0x00000400u
/* SET_REMAP_COMPONENTS: DST_X = CONST_A, COMPONENT_SIZE FOUR, one source and one destination component */
#define CE_REMAP_FILL32      0x00030004u
/* host SEMAPHORED: OPERATION RELEASE, RELEASE_WFI EN, RELEASE_SIZE 16BYTE */
#define HOST_SEM_RELEASE     0x00000002u

/* SET_OBJECT on the copy engine's subchannel */
void nv_ce_object_methods(nv_fifo_stream_t *s, u16 ce_class);
/* a rectangle: w_bytes x h from src (pitch) to dst (pitch), GPU virtual addresses */
void nv_ce_copy_methods(nv_fifo_stream_t *s, u64 dst, u32 dst_pitch, u64 src, u32 src_pitch, u32 w_bytes, u32 h);
/* a w x h rectangle of 32-bit pixels at dst (pitch) set to xrgb (remap constant A) */
void nv_ce_fill_methods(nv_fifo_stream_t *s, u64 dst, u32 pitch, u32 w, u32 h, u32 xrgb);
/* the host's semaphore release (16 bytes: payload, 0, timestamp) after the engine is idle */
void nv_host_release_methods(nv_fifo_stream_t *s, u64 va, u32 payload);
/* a GPFIFO entry for n words at GPU virtual address va */
static inline void nv_fifo_gp_entry(u64 va, u32 n, u32 *w0, u32 *w1) {
    *w0 = (u32)va & ~3u;
    *w1 = (u32)((va >> 32) & 0xFF) | (n << 10);
}

/* ---------------- GPU virtual memory ---------------- */

#define NV_FIFO_VA_VRAM   0x100000000ull         /* VRAM [0, map) at 4 GiB */
#define NV_FIFO_VA_SYS    0x200000000ull         /* the system RAM window at 8 GiB */
#define NV_FIFO_SYS_MAX   (256ull << 20)
#define NV_FIFO_VA_LIMIT  0x300000000ull
#define NV_VMM_SPTS       160

typedef struct nv_vmm {
    int fmt;                    /* NV_VMM_* */
    int bigpage;                /* GF100 / GM200: log2 of the big page size (16 or 17) */
    int pdb64k;                 /* GM200 per-instance mode with 64 KiB pages: page directory base bit 11 */
    u8 *cpu;                    /* CPU view of the pool */
    u64 base, size, used;       /* the pool of page tables (VRAM offsets) */
    u64 limit;
    u64 pd;                     /* the top directory */
    u64 pd2, pd1;               /* GP100 */
    u64 pd0[NV_FIFO_VA_LIMIT >> 29];
    u64 lpt[256];               /* GF100: per page directory entry */
    struct { u64 va; u64 off; } spt[NV_VMM_SPTS];
    int nspt;
    int writes;                 /* entries written since the last flush */
    int err;
} nv_vmm_t;

/* entry encodings (exposed for the host tests) */
u64  nv_vmm_gf100_pte(u64 addr, int aper, int ro, int vol);
u64  nv_vmm_gf100_pde(u64 lpt, u64 spt);
u64  nv_vmm_gp100_pte(u64 addr, int aper, int ro, int vol);
u64  nv_vmm_gp100_pde(u64 pt);
/* 0x0200 of the instance block (0x0208 holds limit - 1) */
u64  nv_vmm_pdb(const nv_vmm_t *v);

/* a pool of page tables at base (VRAM) / cpu; the top directory is made */
int  nv_vmm_init(nv_vmm_t *v, int fmt, int bigpage, u8 *cpu, u64 base, u64 size, u64 limit);
/* VRAM [off, off + size) at va: big pages (GF100) or 2 MiB pages (GP100); all aligned */
int  nv_vmm_map_vram(nv_vmm_t *v, u64 va, u64 off, u64 size);
/* system RAM [phys, phys + size) at va, 4 KiB pages, read only, coherent */
int  nv_vmm_map_sys(nv_vmm_t *v, u64 va, u64 phys, u64 size);
void nv_vmm_unmap_sys(nv_vmm_t *v, u64 va, u64 size);

/* ---------------- the channel ---------------- */

/* inside our 2 MiB block */
#define FIFO_BLK_INST    0x000000
#define FIFO_BLK_RUNL    0x001000
#define FIFO_BLK_GPFIFO  0x002000
#define FIFO_BLK_SEM     0x003000
#define FIFO_BLK_SCRATCH 0x004000                /* 16 KiB for the self test */
#define FIFO_BLK_PUSH    0x020000
#define FIFO_BLK_PT      0x040000
#define FIFO_BLK_BYTES   0x200000
#define FIFO_GP_ENTRIES  512
#define FIFO_PUSH_BYTES  0x10000
#define FIFO_SKIPS       32                      /* NOUVEAU_DMA_SKIPS */
#define FIFO_SAVE_MAX    24

typedef struct nv_fifo_ops {
    void *ctx;
    void (*log)(void *ctx, const char *line);
    /* the CPU's writes through BAR1 leave its write-combining buffers (sfence); NULL: none */
    void (*wmb)(void *ctx);
} nv_fifo_ops_t;

typedef struct nv_fifo {
    /* set by the caller */
    const nv_fifo_gen_t *g;
    const nv_regs_t *r;
    nv_fifo_ops_t ops;
    u16 chipset;
    u8  *vram;                  /* CPU view of VRAM [0, vram_size) (BAR1, 1:1) */
    u64 vram_size;              /* the window: what is mapped for the copy engine too */
    u64 userd;                  /* the USERD region (2 MiB aligned, only USERD in it) */
    u64 userd_bytes;
    u64 blk;                    /* our 2 MiB block */
    u64 flush_page;             /* a 4 KiB page of system RAM for 0x100c10 (0: leave it) */
    u32 timeout_us;             /* a register wait (runlist, preempt, TLB): 100 ms */
    u32 fence_us;               /* a submission that does not complete: 200 ms */
    /* found */
    nv_top_t top;
    int ce;                     /* top.dev[] index */
    int ce_inst, engine, runl, pbdma, chid, npbdma;
    u32 chids;
    nv_vmm_t vmm;
    u64 sys_va, sys_phys, sys_size;              /* the system RAM window as mapped */
    /* state */
    int up;                     /* the channel runs and the copy engine answered */
    int dead;                   /* given up: nothing is submitted any more */
    int bound;                  /* the channel is bound / started / on the runlist */
    u32 seq;                    /* the last fence submitted */
    u32 done;                   /* the last fence seen completed */
    u64 seq_us;                 /* when the oldest fence not seen completed was submitted */
    u32 gp_put;                 /* GPFIFO entries */
    u32 push_put;               /* bytes in the push ring */
    struct { u32 fence, gp, push; } inflight[FIFO_GP_ENTRIES];
    int nin, inhead;            /* submissions not yet seen complete (a ring) */
    nv_fifo_stream_t q;         /* methods queued since the last submit */
    int skips;                  /* the first submission starts with FIFO_SKIPS NOPs */
    struct { u32 reg, val; } saved[FIFO_SAVE_MAX];
    int nsaved;
    u32 pmc_set;                /* PMC_ENABLE bits we turned on */
    u32 faults_noted;           /* MMU fault units of other clients already logged */
    /* statistics */
    u32 submits, copies, fills, waits, errors;
    u64 bytes;
    char why[112];
} nv_fifo_t;

/* bring-up: the topology, PMC_ENABLE, the copy engine's PBDMA, PFIFO and
 * USERD through BAR1, the page tables, the channel (instance block, RAMFC,
 * bind, start, runlist), then SET_OBJECT and a semaphore release that must
 * come back. Every register written is saved first. 0 or E_* (f->why). */
int  nv_fifo_start(nv_fifo_t *f);
/* the system RAM window: [phys, phys + size) read-only at f->sys_va (the
 * old mapping goes; nothing may be in flight); 0 or E_* */
int  nv_fifo_map_sys(nv_fifo_t *f, u64 phys, u64 size);
/* GPU virtual addresses of VRAM / of the mapped system RAM (0 outside) */
static inline u64 nv_fifo_va_vram(const nv_fifo_t *f, u64 off) { return off < f->vram_size ? NV_FIFO_VA_VRAM + off : 0; }
u64  nv_fifo_va_sys(const nv_fifo_t *f, u64 phys, u64 size);
/* queue work (nothing reaches the GPU before nv_fifo_submit); 0 or E_* */
int  nv_fifo_copy(nv_fifo_t *f, u64 dst, u32 dst_pitch, u64 src, u32 src_pitch, u32 w_bytes, u32 h);
int  nv_fifo_fill(nv_fifo_t *f, u64 dst, u32 pitch, u32 w, u32 h, u32 xrgb);
/* send the queued work with a semaphore release: the fence, 0 if nothing
 * was queued (or the channel is dead) */
u32  nv_fifo_submit(nv_fifo_t *f);
/* has fence completed? 1 yes, 0 not yet, E_* the channel failed (error
 * registers, a fence older than fence_us): it is given up */
int  nv_fifo_poll(nv_fifo_t *f, u32 fence);
/* poll until done, at most us; 0 or E_* */
int  nv_fifo_wait(nv_fifo_t *f, u32 fence, u32 us);
/* the error registers (PFIFO, the PBDMA, MMU faults, the copy engine):
 * 0, or E_IO with why filled */
int  nv_fifo_check(nv_fifo_t *f);
/* give up: the channel disabled, preempted, the runlist emptied, unbound,
 * the saved registers back; bounded, safe to call more than once */
void nv_fifo_stop(nv_fifo_t *f, const char *why);
/* the GPU's view for the log */
void nv_fifo_log(nv_fifo_t *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* registers worth reading after a failure, one line */
int  nv_fifo_state(nv_fifo_t *f, char *buf, int n);

#endif
