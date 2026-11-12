/* Host test: the copy engine through PFIFO of M10e (kernel/drivers/nv/
 * fifo.c), built with ASan/UBSan, against a simulated GPU.
 *
 *   - generations: every chipset from GK104 to GP108 to its fifo, copy
 *     engine class and page-table format; Tesla, Fermi, Tegra and Volta are
 *     refused with the reason
 *   - the device topology (0x022700) of a GK104 and a GP104 board: the
 *     copy engine on a runlist without GR is chosen (never the GRCE)
 *   - method streams word by word against nouveau's usage: SET_OBJECT
 *     (nve0_bo_move_init), a copy (nve0_bo_move_copy), a fill
 *     (nvc0b5_migrate_clear, one 32-bit component, a rectangle), the
 *     semaphore release (nvc0_fence_emit32), GPFIFO entries (nv50_dma_push)
 *   - page tables per format against entries worked out by hand from
 *     nouveau's vmmgf100.c / vmmgm200.c / vmmgp100.c, then walked by the
 *     simulator's own MMU
 *   - the whole bring-up on simulated GK104, GK110, GK208, GM107, GM204,
 *     GP100 and GP104: topology, PMC_ENABLE, the PBDMA, USERD through BAR1,
 *     the instance block and RAMFC (checked by the simulated PBDMA the way
 *     the hardware reads them), bind / start, the runlist (channel groups on
 *     Pascal, instance pointers from GM107), SET_OBJECT, the semaphore;
 *     then thousands of random copies from system RAM and within VRAM and
 *     fills, executed by the simulated copy engine on simulated memory and
 *     compared pixel by pixel with a reference, wrapping the push ring and
 *     the GPFIFO many times; the system RAM window remapped (the TLB is
 *     invalidated or the copies would go wrong); every register put back
 *   - every way it can hang or fail: a dead card, no copy engine, the copy
 *     engine only behind GR, no free channel, a BAR flush or TLB
 *     invalidate that never finishes, a runlist that stays pending, a
 *     channel that never fetches, a copy that never ends, a lost
 *     semaphore, a PBDMA error, a launch error, an MMU fault, a preempt
 *     that never finishes: each ends in an error within its time limit, and
 *     stop puts the registers back.
 *
 *   fifotest [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "core/lib.h"
#include "drivers/nv/fifo.h"

const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }

static int checks, failed, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("fifotest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 rs = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)rs; }

/* ======================================================================
 * the simulated GPU
 * ====================================================================== */

#define VRAM_SIZE   (64u << 20)
#define SYS_BASE    0x20000000ull               /* "physical" address of the system RAM arena */
#define SYS_SIZE    (24u << 20)
#define NREG        64

enum { H_DEAD = 1, H_BARFLUSH = 2, H_TLB = 4, H_RUNLIST = 8, H_NOFETCH = 16, H_CE = 32, H_SEMLOST = 64, H_PBDMA = 128,
       H_LAUNCH = 256, H_PREEMPT = 512, H_NOCE = 1024, H_GRCE = 2048, H_NOCHAN = 4096, H_FAULTRO = 8192, H_NOINVAL = 16384 };

typedef struct sim {
    const nv_fifo_gen_t *g;
    u16 chipset;
    int pascal;                 /* LCE topology, gp100 registers */
    u32 hang;
    u64 t;                      /* microseconds */
    u8 *vram, *sys;
    /* registers */
    u32 pmc, pbmask, pb_present, c80, c10, x2254, intr, intren, runl_base, runl_pend[16], preempt, preempt_t;
    u64 runl_t[16];
    u32 top[64];
    u32 pbdma[4][0x200 / 4];    /* PBDMA register windows (0x040000 + n * 0x2000, first 0x200 bytes) */
    u32 ce_stat[4], ce_launch[4];
    u32 fault_units, fault[32][4];
    u32 chan_inst[64], chan_ctl[64];
    u32 bar_busy;
    u32 cb8, cec;
    /* channel state */
    int runl_chan[16];          /* the channel each runlist schedules (-1) */
    int runl_tsg[16];
    u32 gp_get[64];
    int stalled;                /* a fault / error stopped the channel */
    int object;                 /* SET_OBJECT done */
    u32 ce[0x200];              /* copy engine method values (by method / 4) */
    u32 sem_a, sem_b, sem_c;
    u64 ce_busy;                /* the copy engine is busy until */
    u32 ce_runl, ce_pbdma, ce_fault;
    /* TLB: VA page -> (PA page | flags) */
    struct { u64 va; u64 pa; int aper, ro; } tlb[256];
    int ntlb;
    u32 invalidates, walks, launches, releases, faults_seen;
    char err[160];              /* what the simulator found wrong in the driver's use */
    /* register log for "saved and put back" checks */
    u32 written[512]; int nwritten;
} sim_t;

static sim_t S;

static void serr(const char *fmt, ...) {
    if (S.err[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.err, sizeof S.err, fmt, ap);
    va_end(ap);
    if (verbose) printf("  sim: %s\n", S.err);
}

/* ---------------- topology tables ---------------- */

static void top_build(void) {
    int n = 0;
    memset(S.top, 0, sizeof S.top);
#define ENUM(eng, rl, rst, chain) (2u | 0x20 | 0x10 | 0x04 | ((u32)(eng) << 26) | ((u32)(rl) << 21) | ((u32)(rst) << 9) | ((chain) ? 0x80000000u : 0))
#define DATA(inst, fault) (1u | 0x04 | ((u32)(inst) << 26) | ((u32)(fault) << 3) | 0x80000000u)
#define TYPE(t) (3u | ((u32)(t) << 2))
    /* GR: engine 0, runlist 0, reset 12 */
    S.top[n++] = DATA(0, 0x00);
    S.top[n++] = ENUM(0, 0, 12, 1);
    S.top[n++] = TYPE(0);
    if (S.hang & H_NOCE) return;
    if (S.pascal) {
        /* LCEs (type 0x13): LCE0 runlist 1, LCE1 runlist 2, LCE2 the GRCE (runlist 0) */
        if (!(S.hang & H_GRCE)) {
            S.top[n++] = DATA(0, 0x15); S.top[n++] = ENUM(1, 1, 6, 1); S.top[n++] = TYPE(0x13);
            S.top[n++] = DATA(1, 0x16); S.top[n++] = ENUM(2, 2, 7, 1); S.top[n++] = TYPE(0x13);
        }
        S.top[n++] = DATA(2, 0x1b); S.top[n++] = ENUM(3, 0, 21, 1); S.top[n++] = TYPE(0x13);
    } else {
        if (!(S.hang & H_GRCE)) {
            S.top[n++] = DATA(0, 0x15); S.top[n++] = ENUM(1, 1, 6, 1); S.top[n++] = TYPE(1);
            S.top[n++] = DATA(0, 0x16); S.top[n++] = ENUM(2, 2, 7, 1); S.top[n++] = TYPE(2);
        }
        S.top[n++] = DATA(0, 0x1b); S.top[n++] = ENUM(3, 0, 21, 1); S.top[n++] = TYPE(3);
    }
    /* an engine without a runlist (PMU-like), then an invalid word */
    S.top[n++] = ENUM(5, 0, 13, 1) & ~0x10u;
    S.top[n++] = TYPE(0x0a);
#undef ENUM
#undef DATA
#undef TYPE
}

/* ---------------- memory and the MMU ---------------- */

static u8 *aper_ptr(int aper, u64 pa, u64 n) {
    if (aper == 0) return pa + n <= VRAM_SIZE ? S.vram + pa : NULL;
    if (aper == 2 || aper == 3) {
        if (pa < SYS_BASE || pa + n > SYS_BASE + SYS_SIZE) { serr("DMA to system RAM outside the arena: %llx", (unsigned long long)pa); return NULL; }
        return S.sys + (pa - SYS_BASE);
    }
    return NULL;
}
static u32 v32(u64 off) { return off + 4 <= VRAM_SIZE ? *(u32 *)(S.vram + off) : 0; }
static u64 v64(u64 off) { return (u64)v32(off) | (u64)v32(off + 4) << 32; }

static int chan_of_runl(void);
static void fault(u64 va, u32 reason, int write) {
    int c = chan_of_runl();
    u32 u = S.ce_fault;
    S.fault_units |= 1u << u;
    S.fault[u][0] = c >= 0 ? (S.chan_inst[c] & 0x0fffffff) : 0;
    S.fault[u][1] = (u32)va;
    S.fault[u][2] = (u32)(va >> 32);
    S.fault[u][3] = reason | (write ? 0x80 : 0);
    S.intr |= 0x10000000;
    S.stalled = 1;
    S.faults_seen++;
    if (verbose) printf("  sim: MMU fault at %llx reason %u\n", (unsigned long long)va, reason);
}

static int bigpage_sim(u64 pdb) {
    if (S.g->vmm == NV_VMM_GF100) return (S.c80 & 1) ? 16 : 17;
    if (S.g->vmm == NV_VMM_GM200) {
        if ((S.c80 & 0x1800) == 0x1800) return (pdb & (1ull << 11)) ? 16 : 17;
        return (S.c80 & 1) ? 16 : 17;
    }
    return 16;
}

/* translate va for the channel whose instance block is at inst: 0 ok */
static int walk(u64 inst, u64 va, int write, int *aper, u64 *pa) {
    u64 page = va & ~0xFFFull;
    for (int i = 0; i < S.ntlb; i++)
        if (S.tlb[i].va == page) {
            if (write && S.tlb[i].ro) { fault(va, 6, write); return -1; }
            *aper = S.tlb[i].aper;
            *pa = S.tlb[i].pa | (va & 0xFFF);
            return 0;
        }
    S.walks++;
    u64 pdb = v64(inst + 0x200), limit = v64(inst + 0x208);
    if (va > limit) { fault(va, 3, write); return -1; }
    if (pdb & 3) { serr("page directory not in VRAM (%llx)", (unsigned long long)pdb); fault(va, 0, write); return -1; }
    u64 pd = pdb & ~0xFFFull;
    u64 pte;
    int ap, ro, valid;
    u64 addr;
    if (S.g->vmm == NV_VMM_GP100) {
        if (!(pdb & (1ull << 10))) { serr("PDB without VER2 on Pascal"); fault(va, 0, write); return -1; }
        static const int sh[3] = { 47, 38, 29 };
        static const u64 msk[3] = { 3, 511, 511 };
        u64 t = pd;
        for (int l = 0; l < 3; l++) {
            u64 e = v64(t + ((va >> sh[l]) & msk[l]) * 8);
            if ((e & 1) || ((e >> 1) & 3) != 1) { fault(va, 0, write); return -1; }   /* not a PDE in VRAM */
            t = ((e >> 8) & ((1ull << 46) - 1)) << 12;
        }
        u64 e0 = t + ((va >> 21) & 255) * 16;
        u64 lo = v64(e0), hi = v64(e0 + 8);
        if (lo & 1) {                                   /* a 2 MiB page */
            ap = (int)((lo >> 1) & 3); ro = (int)((lo >> 6) & 1);
            addr = (((lo >> 8) & ((1ull << 46) - 1)) << 12) + (va & 0x1FF000);
            valid = 1;
        } else {
            if (((lo >> 1) & 3) != 0) { serr("a big page table on Pascal (not used by the driver)"); fault(va, 0, write); return -1; }
            if (((hi >> 1) & 3) != 1) { fault(va, 0, write); return -1; }
            u64 spt = ((hi >> 8) & ((1ull << 46) - 1)) << 12;
            pte = v64(spt + ((va >> 12) & 511) * 8);
            valid = (int)(pte & 1);
            ap = (int)((pte >> 1) & 3); ro = (int)((pte >> 6) & 1);
            addr = ((pte >> 8) & ((1ull << 46) - 1)) << 12;
        }
    } else {
        int bp = bigpage_sim(pdb), psh = bp == 16 ? 26 : 27;
        u64 e = v64(pd + (va >> psh) * 8);
        u32 lo = (u32)e, hi = (u32)(e >> 32);
        int svalid = 0, bvalid = 0;
        u64 spte = 0, bpte = 0;
        if (hi & 3) {
            if ((hi & 3) != 1) { serr("small page table not in VRAM"); fault(va, 0, write); return -1; }
            u64 spt = (u64)(hi & 0xFFFFFFF0u) << 8;
            spte = v64(spt + ((va >> 12) & ((1ull << (psh - 12)) - 1)) * 8);
            svalid = (int)(spte & 1);
        }
        if (lo & 3) {
            if ((lo & 3) != 1) { serr("big page table not in VRAM"); fault(va, 0, write); return -1; }
            u64 lpt = (u64)(lo & 0xFFFFFFF0u) << 8;
            bpte = v64(lpt + ((va >> bp) & 1023) * 8);
            bvalid = (int)(bpte & 1);
        }
        if (!(hi & 3) && !(lo & 3)) { fault(va, 0, write); return -1; }
        if (svalid && bvalid) { fault(va, 14, write); return -1; }
        if (svalid) { pte = spte; addr = ((u64)((u32)pte & 0xFFFFFFF0u) << 8); }
        else { pte = bpte; addr = ((u64)((u32)pte & 0xFFFFFFF0u) << 8) + (va & ((1ull << bp) - 1) & ~0xFFFull); }
        valid = (int)(pte & 1);
        ap = (int)((pte >> 33) & 3); ro = (int)((pte >> 2) & 1);
    }
    if (!valid) { fault(va, 2, write); return -1; }
    if (write && ro) { fault(va, 6, write); return -1; }
    if (S.ntlb < 256) { S.tlb[S.ntlb].va = page; S.tlb[S.ntlb].pa = addr & ~0xFFFull; S.tlb[S.ntlb].aper = ap; S.tlb[S.ntlb].ro = ro; S.ntlb++; }
    *aper = ap;
    *pa = (addr & ~0xFFFull) | (va & 0xFFF);
    return 0;
}

/* copy n bytes between GPU virtual addresses (page by page); 0 ok */
static int vmem(u64 inst, u64 va, void *buf, u64 n, int write) {
    u8 *b = buf;
    while (n) {
        u64 chunk = 0x1000 - (va & 0xFFF);
        if (chunk > n) chunk = n;
        int ap;
        u64 pa;
        if (walk(inst, va, write, &ap, &pa)) return -1;
        u8 *p = aper_ptr(ap, pa, chunk);
        if (!p) { fault(va, 10, write); return -1; }
        if (write) memcpy(p, b, chunk);
        else memcpy(b, p, chunk);
        va += chunk; b += chunk; n -= chunk;
    }
    return 0;
}

/* ---------------- the copy engine ---------------- */

static void ce_error(u32 stat, u32 launch) {
    int i = S.pascal ? 0 : 0;
    S.ce_stat[i] |= stat;
    S.ce_launch[i] = launch;
    S.stalled = 1;
}

static void launch(u64 inst, u32 flags) {
    S.launches++;
    if (S.hang & H_LAUNCH) { ce_error(4, 0x3); return; }
    if ((flags & 3) == 0 || (flags & 3) == 3) { ce_error(4, 0x9); return; }
    if (!(flags & 0x80) || !(flags & 0x100)) { serr("block-linear layout requested"); ce_error(4, 0x9); return; }
    if (flags & 0x3000) { serr("physical addressing requested"); ce_error(4, 0x9); return; }
    if (flags & 0x18) { serr("copy engine semaphore requested (the driver uses the host's)"); }
    u64 src = (u64)(S.ce[0x400 / 4] & 0x1FFFF) << 32 | S.ce[0x404 / 4];
    u64 dst = (u64)(S.ce[0x408 / 4] & 0x1FFFF) << 32 | S.ce[0x40c / 4];
    u32 pin = S.ce[0x410 / 4], pout = S.ce[0x414 / 4], len = S.ce[0x418 / 4], lines = (flags & 0x200) ? S.ce[0x41c / 4] : 1;
    int remap = (flags & 0x400) != 0;
    u32 comp = S.ce[0x708 / 4];
    u32 csize = ((comp >> 16) & 3) + 1, nsrc = ((comp >> 20) & 3) + 1, ndst = ((comp >> 24) & 3) + 1;
    u32 in_bytes = remap ? len * nsrc * csize : len, out_bytes = remap ? len * ndst * csize : len;
    if (lines > 1 && (in_bytes > pin)) { ce_error(4, 0x4); return; }    /* SRC_LINE_EXCEEDS_PITCH */
    if (lines > 1 && (out_bytes > pout)) { ce_error(4, 0x6); return; }  /* DST_LINE_EXCEEDS_PITCH */
    u8 *ib = malloc(in_bytes + 16), *ob = malloc(out_bytes + 16);
    int uses_src = !remap;
    if (remap) for (u32 c = 0; c < ndst; c++) if (((comp >> (c * 4)) & 7) < 4) uses_src = 1;
    for (u32 y = 0; y < lines && !S.stalled; y++) {
        if (uses_src && vmem(inst, src + (u64)y * pin, ib, in_bytes, 0)) break;
        if (!remap) memcpy(ob, ib, len);
        else {
            for (u32 e = 0; e < len; e++)
                for (u32 c = 0; c < ndst; c++) {
                    u32 sel = (comp >> (c * 4)) & 7;
                    u8 *o = ob + (e * ndst + c) * csize;
                    if (sel == 4) memcpy(o, &S.ce[0x700 / 4], csize);
                    else if (sel == 5) memcpy(o, &S.ce[0x704 / 4], csize);
                    else if (sel < 4 && sel < nsrc) memcpy(o, ib + (e * nsrc + sel) * csize, csize);
                    else if (sel == 6) { /* NO_WRITE: keep; read first */ }
                }
        }
        if (vmem(inst, dst + (u64)y * pout, ob, out_bytes, 1)) break;
    }
    free(ib);
    free(ob);
    if (S.hang & H_CE) S.ce_busy = ~0ull;
    else S.ce_busy = S.t + 1 + (u64)out_bytes * lines / 65536;
}

/* ---------------- the PBDMA ---------------- */

static int chan_of_runl(void) { return S.runl_chan[S.ce_runl]; }

static void pbdma_err(u32 s0, u32 s1, int subc, u32 mthd, u32 data) {
    u32 *p = S.pbdma[S.ce_pbdma];
    p[0x108 / 4] |= s0;
    p[0x148 / 4] |= s1;
    p[0x0c0 / 4] = ((u32)subc << 16) | mthd;
    p[0x0c4 / 4] = data;
    p[0x120 / 4] = (u32)chan_of_runl();
    S.intr |= 0x20000000;
    S.stalled = 1;
}

/* one method; 0 continue, 1 wait (the host waits for the engine) */
static int method(u64 inst, int subc, u32 mthd, u32 data) {
    if (subc == 0) {
        switch (mthd) {
        case 0x10: S.sem_a = data; return 0;
        case 0x14: S.sem_b = data; return 0;
        case 0x18: S.sem_c = data; return 0;
        case 0x1c: {
            if ((data & 0x1f) != 2) { serr("semaphore operation %x", data); pbdma_err(0x00200000, 0, 0, mthd, data); return 0; }
            if (!(data & (1u << 20)) && S.ce_busy > S.t) return 1;           /* RELEASE_WFI: wait for the engine */
            u64 va = (u64)(S.sem_a & 0xFF) << 32 | S.sem_b;
            u32 w[4] = { S.sem_c, 0, (u32)S.t, (u32)(S.t >> 32) };
            if (!(data & (1u << 24)) && (va & 0xF)) serr("16-byte semaphore not 16-byte aligned");
            S.releases++;
            if (!(S.hang & H_SEMLOST)) vmem(inst, va, w, (data & (1u << 24)) ? 4 : 16, 1);
            return 0;
        }
        }
        serr("unknown host method %04x", mthd);
        pbdma_err(0x00200000, 0, subc, mthd, data);
        return 0;
    }
    if (subc != NV_SUBC_CE) { serr("method %04x on subchannel %d", mthd, subc); pbdma_err(0x00200000, 0, subc, mthd, data); return 0; }
    if (mthd == 0) {
        if (data != S.g->ce_class) { pbdma_err(0, 0x10, subc, mthd, data); return 0; }   /* HCE_ILLEGAL_CLASS */
        S.object = 1;
        return 0;
    }
    if (!S.object) { serr("copy engine method %04x before SET_OBJECT", mthd); pbdma_err(0, 0x08, subc, mthd, data); return 0; }
    if (mthd >= 0x800) { pbdma_err(0, 0x08, subc, mthd, data); return 0; }
    if (mthd == 0x300 && S.ce_busy > S.t) return 1;    /* (one launch at a time) */
    S.ce[mthd / 4] = data;
    if (mthd == 0x300) launch(inst, data);
    return 0;
}

/* where the channel stands inside a GPFIFO entry */
static struct { int active; u64 va; u32 n, i, hdr_left, mthd; int subc, kind; u32 skip; } cur;

static void run_channel(int budget) {
    int c = chan_of_runl();
    if (c < 0 || S.stalled || (S.hang & H_NOFETCH)) return;
    if (!(S.chan_ctl[c] & 1) || !(S.chan_inst[c] & 0x80000000u)) return;
    if (!(S.pmc & 0x100) || !(S.pbmask & (1u << S.ce_pbdma))) return;
    u64 inst = (u64)(S.chan_inst[c] & 0x0fffffff) << 12;
    /* RAMFC as the PBDMA reads it */
    if (v32(inst + 0x10) != 0xface) { pbdma_err(0x80000000u, 0, 0, 0, 0); return; }      /* SIGNATURE */
    if (S.g->bind_runl && ((S.chan_ctl[c] >> 16) & 15) != S.ce_runl) { serr("channel bound to runlist %u", (S.chan_ctl[c] >> 16) & 15); return; }
    u64 userd = v64(inst + 0x08);
    if (!(S.x2254 & 0x10000000) || userd != ((u64)(S.x2254 & 0x0fffffff) << 12) + (u64)c * 0x200) {
        serr("USERD %llx is not where BAR1 is snooped (002254 = %08x, channel %d)", (unsigned long long)userd, S.x2254, c);
        return;
    }
    if (v32(inst + 0xe8) != (u32)c) serr("RAMFC channel id %u, bound as %d", v32(inst + 0xe8), c);
    u64 gp = (u64)v32(inst + 0x48) | (u64)(v32(inst + 0x4c) & 0xFF) << 32;
    u32 entries = 1u << ((v32(inst + 0x4c) >> 16) & 0x1F);
    u32 put = v32(userd + 0x8c);
    while (budget-- > 0 && !S.stalled) {
        if (!cur.active) {
            if (S.gp_get[c] == put) return;
            if (put >= entries) { pbdma_err(0x00004000, 0, 0, 0, put); return; }  /* GPPTR */
            u32 e[2];
            if (vmem(inst, gp + (u64)S.gp_get[c] * 8, e, 8, 0)) return;
            cur.active = 1;
            cur.va = (u64)e[0] | (u64)(e[1] & 0xFF) << 32;
            cur.n = (e[1] >> 10) & 0x1FFFFF;
            cur.i = 0;
            cur.hdr_left = 0;
            if (e[0] & 3) { pbdma_err(0x00008000, 0, 0, 0, e[0]); return; }      /* GPENTRY */
        }
        if (cur.i >= cur.n) {
            if (cur.hdr_left) { pbdma_err(0x00040000, 0, cur.subc, cur.mthd, 0); return; }   /* PBENTRY: method cut short */
            cur.active = 0;
            S.gp_get[c] = (S.gp_get[c] + 1) % entries;
            *(u32 *)(S.vram + userd + 0x88) = S.gp_get[c];
            continue;
        }
        u32 w;
        if (vmem(inst, cur.va + (u64)cur.i * 4, &w, 4, 0)) return;
        if (cur.hdr_left) {
            if (method(inst, cur.subc, cur.mthd, w)) return;            /* (stays on this word) */
            cur.i++;
            cur.hdr_left--;
            if (cur.kind == 1 || (cur.kind == 5 && cur.skip == 0)) cur.mthd += 4;
            if (cur.kind == 5) cur.skip = 1;
            continue;
        }
        cur.i++;
        if (w == 0) continue;                                            /* NOP */
        u32 op = w >> 29;
        int subc = (int)((w >> 13) & 7);
        u32 mt = (w & 0xFFF) << 2;
        switch (op) {
        case 1: case 3: case 5:
            cur.kind = (int)op; cur.subc = subc; cur.mthd = mt; cur.hdr_left = (w >> 16) & 0x1FFF; cur.skip = 0;
            break;
        case 4:
            if (method(inst, subc, mt, (w >> 16) & 0x1FFF)) { cur.i--; return; }
            break;
        default:
            pbdma_err(0x00200000, 0, subc, mt, w);
            return;
        }
    }
}

static void sim_tick(u64 us) {
    for (u64 k = 0; k < us; k++) {
        S.t++;
        for (int r = 0; r < 16; r++)
            if ((S.runl_pend[r] & 0x00100000) && !(S.hang & H_RUNLIST) && S.t >= S.runl_t[r]) S.runl_pend[r] &= ~0x00100000u;
        if ((S.preempt & 0x00100000) && !(S.hang & H_PREEMPT) && S.t >= S.preempt_t) S.preempt &= ~0x00100000u;
        if (S.bar_busy && !(S.hang & H_BARFLUSH)) S.bar_busy = 0;
        run_channel(64);
    }
}

/* ---------------- registers ---------------- */

static void runlist_submit(u32 v) {
    int r = (int)((v >> 20) & 15), count = (int)(v & 0xFFFF);
    u64 rl = (u64)(S.runl_base & 0x0fffffff) << 12;
    if ((S.runl_base >> 28) & 3) serr("runlist not in VRAM");
    S.runl_pend[r] |= 0x00100000;
    S.runl_t[r] = S.t + 5;
    S.runl_chan[r] = -1;
    S.runl_tsg[r] = 0;
    for (int i = 0; i < count; i++) {
        u32 w0 = v32(rl + (u64)i * 8), w1 = v32(rl + (u64)i * 8 + 4);
        if (w0 & 0x2000) {
            if (!S.g->tsg) { serr("channel group entry on a chip without forced groups"); continue; }
            if (((w0 >> 26) & 0x3f) != 1 || ((w0 >> 18) & 0xff) != 128 || ((w0 >> 14) & 3) != 3) serr("channel group entry %08x", w0);
            S.runl_tsg[r] = 1;
            continue;
        }
        int c = (int)(w0 & 0xFFF);
        if (S.g->tsg && !S.runl_tsg[r]) serr("Pascal channel without its group entry");
        if (S.g->runl_inst && w1 != (S.chan_inst[c & 63] & 0x0fffffff)) serr("runlist instance %08x, channel bound to %08x", w1, S.chan_inst[c & 63]);
        if (!S.g->runl_inst && w1) serr("runlist word 1 %08x on a chip without instance pointers there", w1);
        if (S.g->tsg && ((w0 >> 14) & 3)) serr("runqueue %u", (w0 >> 14) & 3);
        if (c < 64) S.runl_chan[r] = c;
    }
}

static u32 rd32(void *ctx, u32 reg) {
    (void)ctx;
    sim_tick(1);
    if (S.hang & H_DEAD) return 0xFFFFFFFFu;
    if (reg >= 0x022700 && reg < 0x022800) return S.top[(reg - 0x022700) / 4];
    if (reg >= 0x800000 && reg < 0x800000 + 64 * 8) {
        int c = (int)((reg - 0x800000) / 8);
        return (reg & 4) ? S.chan_ctl[c] : S.chan_inst[c];
    }
    if (reg >= 0x040000 && reg < 0x048000) {
        int p = (int)((reg - 0x040000) / 0x2000);
        u32 o = (reg - 0x040000) % 0x2000;
        return o < 0x200 ? S.pbdma[p][o / 4] : 0;
    }
    if (reg >= 0x002390 && reg < 0x0023a0) {
        int p = (int)((reg - 0x002390) / 4);
        return p == (int)S.ce_pbdma ? (1u << S.ce_runl) : p == 0 ? 1u : (1u << (p + 1)) & ~(1u << S.ce_runl);
    }
    if (reg >= 0x002284 && reg < 0x002284 + 16 * 8 && !((reg - 0x002284) & 7)) return S.runl_pend[(reg - 0x002284) / 8];
    if (reg >= 0x002800 && reg < 0x002a00) return S.fault[(reg - 0x002800) / 16][((reg - 0x002800) % 16) / 4];
    if (reg >= 0x002640 && reg < 0x002640 + 16 * 8) return S.ce_busy > S.t ? 0x80000000u : 0;
    switch (reg) {
    case 0x000000: return S.pascal ? 0x134000a1 : 0x0e4000a1;
    case 0x000200: return S.pmc;
    case 0x000204: return S.pbmask & S.pb_present;
    case 0x002004: return 4;
    case 0x002008: return S.hang & H_NOCHAN ? 1 : 512;
    case 0x002100: return S.intr;
    case 0x002140: return S.intren;
    case 0x002254: return S.x2254;
    case 0x002634: return S.preempt;
    case 0x00259c: return S.fault_units;
    case 0x00254c: case 0x00252c: case 0x00256c: return 0;
    case 0x070000: return S.bar_busy ? 2 : 0;
    case 0x100c80: return (S.hang & H_TLB) ? (S.c80 & 0xFFFF7FFFu & 0xFF00FFFFu) : (S.c80 | 0x00ff8000);
    case 0x100c10: return S.c10;
    case 0x104410: case 0x104490: case 0x104510: return S.pascal ? S.ce_stat[0] : 0;
    case 0x104418: return S.ce_launch[0];
    case 0x104908: case 0x105908: case 0x106908: return S.pascal ? 0 : S.ce_stat[0];
    case 0x104f14: return S.ce_launch[0];
    }
    return 0;
}

static void wr32(void *ctx, u32 reg, u32 v) {
    (void)ctx;
    if (S.nwritten < 512) S.written[S.nwritten++] = reg;
    if (S.hang & H_DEAD) return;
    if (reg >= 0x800000 && reg < 0x800000 + 64 * 8) {
        int c = (int)((reg - 0x800000) / 8);
        if (reg & 4) {
            u32 keep = S.chan_ctl[c] & 1;
            if (v & 0x400) keep = 1;
            if (v & 0x800) keep = 0;
            S.chan_ctl[c] = keep | (v & 0x000f0000);
        } else {
            S.chan_inst[c] = v;
            if (!v) { S.gp_get[c] = 0; cur.active = 0; }
        }
        return;
    }
    if (reg >= 0x040000 && reg < 0x048000) {
        int p = (int)((reg - 0x040000) / 0x2000);
        u32 o = (reg - 0x040000) % 0x2000;
        if (o == 0x108 || o == 0x148) S.pbdma[p][o / 4] &= ~v;
        else if (o < 0x200) S.pbdma[p][o / 4] = v;
        return;
    }
    switch (reg) {
    case 0x000200: S.pmc = v; return;
    case 0x000204: S.pbmask = v; return;
    case 0x002100: S.intr &= ~v; return;
    case 0x002140: S.intren = v; return;
    case 0x002254: S.x2254 = v; return;
    case 0x002270: S.runl_base = v; return;
    case 0x002274: runlist_submit(v); return;
    case 0x002634: S.preempt = (v & 0xFFFFF) | 0x00100000; S.preempt_t = S.t + 3; return;
    case 0x00259c: S.fault_units &= ~v; return;
    case 0x070000: if (v & 1) S.bar_busy = 1; return;
    case 0x100c10: S.c10 = v; return;
    case 0x100cb8: S.cb8 = v; return;
    case 0x100cec: S.cec = v; return;
    case 0x100cbc:
        if (S.hang & H_NOINVAL) return;
        if (v & 2) S.ntlb = 0;                                  /* ALL_PDB: the driver must never do this */
        else {
            /* only our directory: the TLB holds one address space here */
            S.ntlb = 0;
        }
        if (v & 2) serr("an invalidate of every address space (BAR1 too)");
        S.invalidates++;
        return;
    case 0x104410: S.ce_stat[0] &= ~v; return;
    case 0x104908: S.ce_stat[0] &= ~v; return;
    }
}

static void delay_us(void *ctx, u32 us) { (void)ctx; sim_tick(us); }
static u64 now_us(void *ctx) { (void)ctx; return S.t; }
static nv_regs_t REGS = { NULL, rd32, wr32, delay_us, now_us };

static char lastlog[256];
static void flog(void *ctx, const char *line) {
    (void)ctx;
    strlcpy(lastlog, line, sizeof lastlog);
    if (verbose) printf("  log: %s\n", line);
}

static void sim_reset(u16 chipset, u32 hang) {
    u8 *vram = S.vram, *sys = S.sys;
    memset(&S, 0, sizeof S);
    memset(&cur, 0, sizeof cur);
    S.vram = vram ? vram : calloc(1, VRAM_SIZE);
    S.sys = sys ? sys : calloc(1, SYS_SIZE);
    memset(S.vram, 0xA5, VRAM_SIZE);
    S.chipset = chipset;
    S.g = nv_fifo_gen_for(chipset, NULL);
    S.pascal = chipset >= 0x130;
    S.hang = hang;
    S.pmc = 0xFFFFFFFFu & ~0x100u & ~(1u << 6);        /* PFIFO and CE0 off: the driver turns them on */
    S.pb_present = 0xF;
    S.pbmask = 0x1;
    S.c80 = chipset == 0xe4 ? 0x00000001 : chipset == 0x124 ? 0x00001800 : 0;   /* GK104: 64 KiB big pages; GM204 per instance */
    S.ce_runl = 1;
    S.ce_pbdma = 2;
    S.ce_fault = 0x15;
    for (int r = 0; r < 16; r++) S.runl_chan[r] = -1;
    if (S.hang & H_NOCHAN) for (int c = 0; c < 64; c++) S.chan_inst[c] = 0x80000000u | 0x123;
    S.chan_inst[0] = 0x80000000u | 0x0FFFF;                /* a channel the firmware left bound: skipped */
    if (S.hang & H_CE) S.ce_busy = ~0ull;                  /* the engine never becomes idle */
    top_build();
}

/* ======================================================================
 * the driver against the simulator
 * ====================================================================== */

static nv_fifo_t F;
#define BLK    (8u << 20)
#define USERD  (4u << 20)

static void fifo_setup(void) {
    memset(&F, 0, sizeof F);
    F.g = S.g;
    F.r = &REGS;
    F.ops.log = flog;
    F.chipset = S.chipset;
    F.vram = S.vram;
    F.vram_size = VRAM_SIZE;
    F.userd = USERD;
    F.userd_bytes = 2u << 20;
    F.blk = BLK;
    F.flush_page = SYS_BASE + SYS_SIZE - 0x1000;
    F.timeout_us = 20000;
    F.fence_us = 50000;
}

/* the saved registers are back after stop (those the simulator stores) */
static void check_restored(const char *what, u32 pmc0, u32 pb0, u32 c10_0) {
    CHECK(S.pmc == pmc0, "%s: PMC_ENABLE %08x, was %08x", what, S.pmc, pmc0);
    CHECK((S.pbmask & S.pb_present) == (pb0 & S.pb_present), "%s: PBDMA enables %08x, were %08x", what, S.pbmask, pb0);
    CHECK(S.x2254 == 0, "%s: 002254 = %08x after stop", what, S.x2254);
    CHECK(S.intren == 0, "%s: 002140 = %08x after stop", what, S.intren);
    CHECK(S.c10 == c10_0, "%s: 100c10 = %08x after stop", what, S.c10);
    for (int c = 1; c < 64; c++) CHECK(!(S.chan_ctl[c] & 1), "%s: channel %d still enabled", what, c);
    CHECK(S.chan_inst[F.chid > 0 ? F.chid : 1] == 0 || F.chid <= 0, "%s: channel %d still bound", what, F.chid);
}

/* ---------------- generations and topology ---------------- */

static void test_gens(void) {
    static const struct { u16 cs; int kind; u16 ce; int vmm; } want[] = {
        { 0xe4, FIFO_GK104, 0xa0b5, NV_VMM_GF100 }, { 0xe6, FIFO_GK104, 0xa0b5, NV_VMM_GF100 }, { 0xe7, FIFO_GK104, 0xa0b5, NV_VMM_GF100 },
        { 0xf0, FIFO_GK110, 0xa0b5, NV_VMM_GF100 }, { 0xf1, FIFO_GK110, 0xa0b5, NV_VMM_GF100 }, { 0x106, FIFO_GK208, 0xa0b5, NV_VMM_GF100 },
        { 0x108, FIFO_GK208, 0xa0b5, NV_VMM_GF100 }, { 0x117, FIFO_GM107, 0xb0b5, NV_VMM_GF100 }, { 0x118, FIFO_GM107, 0xb0b5, NV_VMM_GF100 },
        { 0x120, FIFO_GM200, 0xb0b5, NV_VMM_GM200 }, { 0x124, FIFO_GM200, 0xb0b5, NV_VMM_GM200 }, { 0x126, FIFO_GM200, 0xb0b5, NV_VMM_GM200 },
        { 0x130, FIFO_GP100, 0xc0b5, NV_VMM_GP100 }, { 0x132, FIFO_GP100, 0xc1b5, NV_VMM_GP100 }, { 0x134, FIFO_GP100, 0xc1b5, NV_VMM_GP100 },
        { 0x136, FIFO_GP100, 0xc1b5, NV_VMM_GP100 }, { 0x137, FIFO_GP100, 0xc1b5, NV_VMM_GP100 }, { 0x138, FIFO_GP100, 0xc1b5, NV_VMM_GP100 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
        const char *why = "x";
        const nv_fifo_gen_t *g = nv_fifo_gen_for(want[i].cs, &why);
        CHECK(g && g->kind == want[i].kind && g->ce_class == want[i].ce && g->vmm == want[i].vmm && !why, "chipset %x", want[i].cs);
    }
    /* the fifo details from nouveau: TSG forced on GP100 only, instance pointers in runlists from GM107, runlist in 0x800004 before GM107 */
    CHECK(nv_fifo_gen_for(0x134, NULL)->tsg && !nv_fifo_gen_for(0x124, NULL)->tsg, "groups");
    CHECK(nv_fifo_gen_for(0x117, NULL)->runl_inst && !nv_fifo_gen_for(0x108, NULL)->runl_inst, "runlist instance");
    CHECK(nv_fifo_gen_for(0xf0, NULL)->bind_runl && !nv_fifo_gen_for(0x117, NULL)->bind_runl, "bind runlist");
    CHECK(nv_fifo_gen_for(0x106, NULL)->runq_timeout && !nv_fifo_gen_for(0xf1, NULL)->runq_timeout, "runqueue timeout");
    CHECK(nv_fifo_gen_for(0x106, NULL)->chids == 1024 && nv_fifo_gen_for(0x117, NULL)->chids == 2048 && nv_fifo_gen_for(0xe7, NULL)->chids == 4096, "channels");
    static const struct { u16 cs; const char *word; } no[] = { { 0xc4, "Fermi" }, { 0xd9, "Fermi" }, { 0xa3, "Tesla" }, { 0x50, "Tesla" }, { 0x40, "before Tesla" },
                                                              { 0x140, "Volta" }, { 0x164, "Volta" }, { 0xea, "Tegra" }, { 0x12b, "Tegra" }, { 0x13b, "Tegra" } };
    for (size_t i = 0; i < ARRAY_SIZE(no); i++) {
        const char *why = NULL;
        CHECK(!nv_fifo_gen_for(no[i].cs, &why) && why && strstr(why, no[i].word), "chipset %x refused: %s", no[i].cs, why ? why : "-");
    }
}

static void test_top(void) {
    for (int pascal = 0; pascal < 2; pascal++) {
        sim_reset(pascal ? 0x134 : 0xe4, 0);
        nv_top_t t;
        int n = nv_top_read(&REGS, &t);
        CHECK(n == 5, "%s: %d devices", pascal ? "GP104" : "GK104", n);
        CHECK(t.dev[0].type == NV_ENG_GR && t.dev[0].runlist == 0 && t.dev[0].engine == 0 && t.dev[0].reset == 12, "GR entry");
        CHECK(t.dev[1].type == NV_ENG_CE && t.dev[1].inst == 0 && t.dev[1].runlist == 1 && t.dev[1].engine == 1 && t.dev[1].reset == 6 &&
              t.dev[1].fault == 0x15, "CE0 entry: inst %d runlist %d engine %d reset %d fault %x", t.dev[1].inst, t.dev[1].runlist,
              t.dev[1].engine, t.dev[1].reset, t.dev[1].fault);
        CHECK(t.dev[2].type == NV_ENG_CE && t.dev[2].inst == 1 && t.dev[2].runlist == 2, "CE1 entry");
        CHECK(t.dev[3].type == NV_ENG_CE && t.dev[3].inst == 2 && t.dev[3].runlist == 0, "CE2 (GRCE) entry");
        CHECK(t.dev[4].type == NV_ENG_OTHER && t.dev[4].runlist == -1 && t.dev[4].engine == 5, "an engine without runlist");
        CHECK(nv_top_pick_ce(&t) == 1, "CE0 is picked (%d)", nv_top_pick_ce(&t));
        sim_reset(pascal ? 0x134 : 0xe4, H_GRCE);
        nv_top_read(&REGS, &t);
        CHECK(nv_top_pick_ce(&t) == -1, "%s: only the GRCE: nothing picked", pascal ? "GP104" : "GK104");
        sim_reset(pascal ? 0x134 : 0xe4, H_NOCE);
        nv_top_read(&REGS, &t);
        CHECK(t.n == 1 && nv_top_pick_ce(&t) == -1, "no copy engine");
    }
}

/* ---------------- method streams against nouveau ---------------- */

static void test_streams(void) {
    nv_fifo_stream_t s;
    memset(&s, 0, sizeof s);
    /* nve0_bo_move_init: PUSH_NVSQ(push, NVA0B5, 0x0000, handle) on subchannel 4 */
    nv_ce_object_methods(&s, 0xc1b5);
    CHECK(s.n == 2 && s.w[0] == 0x20018000 && s.w[1] == 0xc1b5, "SET_OBJECT %08x %08x", s.w[0], s.w[1]);
    /* nve0_bo_move_copy: OFFSET_IN_UPPER .. LINE_COUNT (incr 8), then LAUNCH_DMA 0x386 (immediate) */
    memset(&s, 0, sizeof s);
    nv_ce_copy_methods(&s, 0x112345678ull, 10240, 0x20000a000ull, 7680, 1920 * 4, 1080);
    static const u32 copy[] = { 0x20088100, 0x00000002, 0x0000a000, 0x00000001, 0x12345678, 7680, 10240, 7680, 1080, 0x838680c0 };
    CHECK(s.n == (int)ARRAY_SIZE(copy) && !memcmp(s.w, copy, sizeof copy), "copy stream (%d words: %08x %08x ... %08x)", s.n, s.w[0], s.w[1],
          s.w[s.n - 1]);
    /* nvc0b5_migrate_clear with one component: REMAP_CONST_A / B / COMPONENTS (incr 3), offsets / pitches / sizes (incr 8), LAUNCH_DMA 0x786 */
    memset(&s, 0, sizeof s);
    nv_ce_fill_methods(&s, 0x100400000ull, 10240, 2560, 1440, 0x00336699);
    static const u32 fill[] = { 0x200381c0, 0x00336699, 0x00000000, 0x00030004, 0x20088100, 0x00000001, 0x00400000, 0x00000001, 0x00400000,
                                10240, 10240, 2560, 1440, 0x878680c0 };
    CHECK(s.n == (int)ARRAY_SIZE(fill) && !memcmp(s.w, fill, sizeof fill), "fill stream (%d words: %08x ... %08x)", s.n, s.w[0], s.w[s.n - 1]);
    /* nvc0_fence_emit32: SEMAPHOREA..D (incr; RELEASE, WFI, 16 bytes) */
    memset(&s, 0, sizeof s);
    nv_host_release_methods(&s, 0x108803000ull, 77);
    static const u32 rel[] = { 0x20040004, 0x00000001, 0x08803000, 77, 0x00000002 };
    CHECK(s.n == 5 && !memcmp(s.w, rel, sizeof rel), "release stream %08x %08x %08x %08x %08x", s.w[0], s.w[1], s.w[2], s.w[3], s.w[4]);
    /* nv50_dma_push: lower_32_bits(offset), upper_32_bits(offset) | bytes << 8 */
    u32 w0, w1;
    nv_fifo_gp_entry(0x108820000ull, 25, &w0, &w1);
    CHECK(w0 == 0x08820000 && w1 == (1u | (25u * 4u) << 8), "GPFIFO entry %08x %08x", w0, w1);
    CHECK(nv_fifo_immd(4, 0x300, 0x386) == 0x838680c0 && nv_fifo_incr(0, 0x10, 4) == 0x20040004, "headers");
    /* the overflow flag */
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 500; i++) nv_ce_copy_methods(&s, 1, 4, 1, 4, 4, 1);
    CHECK(s.overflow && s.n <= 4096, "overflow %d", s.n);
}

/* ---------------- page tables ---------------- */

static void test_vmm(void) {
    static nv_vmm_t v;
    u8 *pool = calloc(1, 1u << 20);
    const u64 base = 0x00700000;
    /* GP100: entries from gp100_vmm_pde / pd0_pte / pgt_pte (vmmgp100.c) */
    CHECK(nv_vmm_init(&v, NV_VMM_GP100, 17, pool, base, 1u << 20, NV_FIFO_VA_LIMIT) == 0, "gp100 init");
    CHECK(v.pd == base && v.pd2 == base + 0x1000 && v.pd1 == base + 0x2000, "gp100 directories %llx %llx %llx", (unsigned long long)v.pd,
          (unsigned long long)v.pd2, (unsigned long long)v.pd1);
    CHECK(*(u64 *)pool == 0x00000000000070102ull, "PD3[0] = %016llx (pd2 >> 4 | VRAM << 1)", (unsigned long long)*(u64 *)pool);
    CHECK(*(u64 *)(pool + 0x1000) == 0x00000000000070202ull, "PD2[0] = %016llx", (unsigned long long)*(u64 *)(pool + 0x1000));
    CHECK(nv_vmm_pdb(&v) == (base | 0xC00), "gp100 PDB %llx (VER2 | 64 KiB)", (unsigned long long)nv_vmm_pdb(&v));
    CHECK(nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM, 0, 8u << 20) == 0, "gp100 map VRAM");
    /* 4 GiB is PD1 entry 8, PD0 at base + 0x3000; a 2 MiB page: addr >> 4 | VALID, high half 0 */
    CHECK(*(u64 *)(pool + 0x2000 + 8 * 8) == ((base + 0x3000) >> 4 | 2), "PD1[8] = %016llx", (unsigned long long)*(u64 *)(pool + 0x2000 + 64));
    CHECK(*(u64 *)(pool + 0x3000 + 16 * 3) == ((6ull << 20) >> 4 | 1) && *(u64 *)(pool + 0x3000 + 16 * 3 + 8) == 0, "PD0[3] = %016llx",
          (unsigned long long)*(u64 *)(pool + 0x3000 + 48));
    CHECK(nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM + 0x1000, 0, 2u << 20) == E_INVAL, "unaligned 2 MiB page refused");
    CHECK(nv_vmm_map_sys(&v, NV_FIFO_VA_SYS, 0x23456000, 0x3000) == 0, "gp100 map system RAM");
    /* 8 GiB: PD1 entry 16, PD0 at +0x4000; its entry 0 gets the small table (+0x5000) in the high half */
    CHECK(*(u64 *)(pool + 0x4000) == 0 && *(u64 *)(pool + 0x4008) == ((base + 0x5000) >> 4 | 2), "PD0 dual entry %016llx %016llx",
          (unsigned long long)*(u64 *)(pool + 0x4000), (unsigned long long)*(u64 *)(pool + 0x4008));
    /* the PTE: addr >> 4 | VALID | SYS_COH << 1 | VOL << 3 | RO << 6 = ...2345600 | 0x4d */
    CHECK(*(u64 *)(pool + 0x5008) == (0x23457000ull >> 4 | 0x4d), "gp100 system PTE %016llx", (unsigned long long)*(u64 *)(pool + 0x5008));
    CHECK(nv_vmm_gp100_pte(0x12345000, 0, 0, 0) == 0x01234501ull, "gp100 VRAM PTE");
    nv_vmm_unmap_sys(&v, NV_FIFO_VA_SYS, 0x3000);
    CHECK(*(u64 *)(pool + 0x5008) == 0, "unmapped");

    /* GF100 (128 KiB big pages): gf100_vmm_pgd_pde, gf100_vmm_pgt_pte */
    memset(pool, 0, 1u << 20);
    CHECK(nv_vmm_init(&v, NV_VMM_GF100, 17, pool, base, 1u << 20, NV_FIFO_VA_LIMIT) == 0 && v.pd == base, "gf100 init");
    CHECK(nv_vmm_pdb(&v) == base, "gf100 PDB");
    CHECK(nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM, 0, 4u << 20) == 0, "gf100 map VRAM");
    /* 4 GiB >> 27 = PDE 32; the big table (8 KiB) at base + 0x1000: low word 1 | lpt >> 8 */
    CHECK(*(u64 *)(pool + 32 * 8) == (1 | ((base + 0x1000) >> 8)), "PDE 32 = %016llx", (unsigned long long)*(u64 *)(pool + 256));
    /* big page 3 (offset 384 KiB): addr >> 8 | VALID */
    CHECK(*(u64 *)(pool + 0x1000 + 3 * 8) == ((3ull << 17) >> 8 | 1), "big PTE %016llx", (unsigned long long)*(u64 *)(pool + 0x1018));
    CHECK(nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM, 0x10000, 0x20000) == E_INVAL, "unaligned big page refused");
    CHECK(nv_vmm_map_sys(&v, NV_FIFO_VA_SYS + 0x1000, 0x23456000, 0x1000) == 0, "gf100 map system RAM");
    /* 8 GiB >> 27 = PDE 64; small table (256 KiB) at base + 0x3000: high word 1 | spt << 24 >> 32 ... */
    CHECK(*(u64 *)(pool + 64 * 8) == ((1ull << 32) | ((u64)(base + 0x3000) << 24)), "PDE 64 = %016llx", (unsigned long long)*(u64 *)(pool + 512));
    /* PTE: addr >> 8 | VALID | RO << 2 | VOL << 32 | SYS_COH << 33 */
    CHECK(*(u64 *)(pool + 0x3000 + 8) == (0x234560ull | 1 | 4 | (1ull << 32) | (2ull << 33)), "gf100 system PTE %016llx",
          (unsigned long long)*(u64 *)(pool + 0x3008));
    /* GF100 with 64 KiB big pages: PDEs of 64 MiB */
    memset(pool, 0, 1u << 20);
    nv_vmm_init(&v, NV_VMM_GF100, 16, pool, base, 1u << 20, NV_FIFO_VA_LIMIT);
    nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM, 0, 1u << 20);
    CHECK(*(u64 *)(pool + 64 * 8) == (1 | ((base + 0x1000) >> 8)) && *(u64 *)(pool + 0x1000 + 15 * 8) == ((15ull << 16) >> 8 | 1), "64 KiB layout");
    /* GM200 with 64 KiB pages per instance: PDB bit 11 (gm200_vmm_join_) */
    memset(pool, 0, 1u << 20);
    nv_vmm_init(&v, NV_VMM_GM200, 16, pool, base, 1u << 20, NV_FIFO_VA_LIMIT);
    CHECK(nv_vmm_pdb(&v) == (base | 0x800), "gm200 64 KiB PDB");
    nv_vmm_init(&v, NV_VMM_GM200, 17, pool, base, 1u << 20, NV_FIFO_VA_LIMIT);
    CHECK(nv_vmm_pdb(&v) == base, "gm200 128 KiB PDB");
    /* the pool runs out: an error, never a write outside */
    memset(pool, 0, 1u << 20);
    nv_vmm_init(&v, NV_VMM_GF100, 17, pool, base, 0x10000, NV_FIFO_VA_LIMIT);
    CHECK(nv_vmm_map_sys(&v, NV_FIFO_VA_SYS, 0x20000000, 0x10000) == E_NOMEM, "small table does not fit in 64 KiB");
    free(pool);
}

/* ---------------- the whole thing on a simulated chip ---------------- */

/* a reference copy of VRAM / system RAM regions the tests draw in */
static u8 *refv;

static void ref_copy_from_vram(void) { memcpy(refv, S.vram, VRAM_SIZE); }

static int start_chip(u16 cs, u32 hang) {
    sim_reset(cs, hang);
    fifo_setup();
    return nv_fifo_start(&F);
}

static void test_chip(u16 cs) {
    char tag[16];
    snprintf(tag, sizeof tag, "%x", cs);
    sim_reset(cs, 0);
    u32 pmc0 = S.pmc, pb0 = S.pbmask, c10_0 = S.c10;
    fifo_setup();
    int r = nv_fifo_start(&F);
    CHECK(r == 0, "%s: start: %s (%s; sim: %s)", tag, strerror(r), F.why, S.err);
    if (r) return;
    CHECK(!S.err[0], "%s: simulator: %s", tag, S.err);
    CHECK(F.up && F.ce_inst == 0 && F.runl == 1 && F.pbdma == 2 && F.chid == 1, "%s: CE %d runlist %d PBDMA %d channel %d", tag, F.ce_inst, F.runl,
          F.pbdma, F.chid);
    CHECK(S.pmc & 0x100 && S.pmc & (1u << 6), "%s: PFIFO and CE0 powered", tag);
    CHECK(S.object && S.releases == 1 && v32(BLK + FIFO_BLK_SEM) == 1, "%s: object bound, first semaphore", tag);
    CHECK(S.x2254 == (0x10000000u | (USERD >> 12)), "%s: USERD through BAR1 %08x", tag, S.x2254);
    CHECK(S.c10 == (u32)((SYS_BASE + SYS_SIZE - 0x1000) >> 8), "%s: flush page", tag);
    CHECK(S.invalidates >= 1, "%s: TLB invalidated", tag);
    CHECK(S.runl_chan[1] == 1 && S.runl_tsg[1] == (F.g->tsg != 0), "%s: runlist", tag);

    /* drawing: random copies from system RAM and within VRAM, fills */
    const u32 pitch = 4096, W = 1000, H = 500;
    const u64 fb0 = 16u << 20, fb1 = 24u << 20;          /* two "buffers" in VRAM */
    for (u32 i = 0; i < SYS_SIZE / 4; i++) ((u32 *)S.sys)[i] = rnd();
    u64 src_phys = SYS_BASE + 0x123400;                    /* a desktop surface, not page aligned */
    u32 src_pitch = 1100 * 4;
    r = nv_fifo_map_sys(&F, src_phys, (u64)src_pitch * 600);
    CHECK(r == 0, "%s: map system RAM: %s", tag, F.why);
    memset(S.vram + fb0, 0, (size_t)pitch * H);
    memset(S.vram + fb1, 0, (size_t)pitch * H);
    refv = refv ? refv : malloc(VRAM_SIZE);
    ref_copy_from_vram();
    u32 ops = 0, fences = 0, last = 0;
    for (int round = 0; round < 400; round++) {
        int nops = 1 + (int)(rnd() % 12);
        for (int k = 0; k < nops; k++) {
            u32 w = 1 + rnd() % 300, h = 1 + rnd() % 200, x = rnd() % (W - w), y = rnd() % (H - h);
            u64 dst = (rnd() & 1) ? fb1 : fb0;
            u32 kind = rnd() % 3;
            if (kind == 0) {                                 /* upload from the "desktop" */
                u32 sx = rnd() % (1100 - w), sy = rnd() % (600 - h);
                u64 sp = src_phys + (u64)sy * src_pitch + sx * 4;
                r = nv_fifo_copy(&F, nv_fifo_va_vram(&F, dst + (u64)y * pitch + x * 4), pitch, nv_fifo_va_sys(&F, sp, (u64)(h - 1) * src_pitch + w * 4),
                                 src_pitch, w * 4, h);
                for (u32 j = 0; j < h; j++) memcpy(refv + dst + (u64)(y + j) * pitch + x * 4, S.sys + (sp - SYS_BASE) + (u64)j * src_pitch, w * 4);
            } else if (kind == 1) {                          /* buffer to buffer (the flip synchronisation) */
                u64 src = dst == fb0 ? fb1 : fb0;
                r = nv_fifo_copy(&F, nv_fifo_va_vram(&F, dst + (u64)y * pitch + x * 4), pitch, nv_fifo_va_vram(&F, src + (u64)y * pitch + x * 4), pitch,
                                 w * 4, h);
                for (u32 j = 0; j < h; j++) memcpy(refv + dst + (u64)(y + j) * pitch + x * 4, refv + src + (u64)(y + j) * pitch + x * 4, w * 4);
            } else {
                u32 c = rnd() & 0xFFFFFF;
                r = nv_fifo_fill(&F, nv_fifo_va_vram(&F, dst + (u64)y * pitch + x * 4), pitch, w, h, c);
                for (u32 j = 0; j < h; j++)
                    for (u32 i = 0; i < w; i++) *(u32 *)(refv + dst + (u64)(y + j) * pitch + (x + i) * 4) = c;
            }
            CHECK(r == 0, "%s: queue op: %s", tag, strerror(r));
            ops++;
        }
        u32 f = nv_fifo_submit(&F);
        CHECK(f == last + 1 || (last == 0 && f >= 2), "%s: fence %u after %u", tag, f, last);
        last = f;
        fences++;
        /* sometimes wait, sometimes let the queue build up */
        if ((rnd() & 3) == 0 && nv_fifo_wait(&F, f, 50000)) break;
    }
    r = nv_fifo_wait(&F, last, 200000);
    CHECK(r == 0, "%s: last fence: %s (%s, sim %s)", tag, strerror(r), F.why, S.err);
    CHECK(!memcmp(S.vram + fb0, refv + fb0, (size_t)pitch * H) && !memcmp(S.vram + fb1, refv + fb1, (size_t)pitch * H), "%s: pixels after %u ops",
          tag, ops);
    CHECK(!S.err[0], "%s: simulator after drawing: %s", tag, S.err);
    CHECK(F.push_put > 0 && F.submits > 400 && F.gp_put == F.submits % FIFO_GP_ENTRIES, "%s: %u submissions, ring at %u", tag, F.submits, F.push_put);
    CHECK(S.faults_seen == 0, "%s: no MMU faults", tag);

    /* the system RAM window moves (a new desktop surface): the TLB must forget the old one */
    u32 inv = S.invalidates;
    u64 src2 = SYS_BASE + 0x900000;
    r = nv_fifo_map_sys(&F, src2, 64 * 4 * 64);
    CHECK(r == 0 && S.invalidates == inv + 1, "%s: remap", tag);
    CHECK(!nv_fifo_va_sys(&F, src_phys, 16), "%s: the old window is gone", tag);
    nv_fifo_copy(&F, nv_fifo_va_vram(&F, fb0), pitch, nv_fifo_va_sys(&F, src2, 4), 256, 256, 64);
    r = nv_fifo_wait(&F, nv_fifo_submit(&F), 50000);
    int same = 1;
    for (int j = 0; j < 64; j++) if (memcmp(S.vram + fb0 + (u64)j * pitch, S.sys + 0x900000 + (u64)j * 256, 256)) same = 0;
    CHECK(r == 0 && same, "%s: copy from the new window", tag);
    CHECK(nv_fifo_map_sys(&F, src2, 512u << 20) == E_NOSPC, "%s: a window too large", tag);
    /* nothing queued: the last fence */
    CHECK(nv_fifo_submit(&F) == F.seq, "%s: empty submit", tag);
    nv_fifo_stop(&F, "test done");
    check_restored(tag, pmc0, pb0, c10_0);
    CHECK(S.runl_chan[1] == -1 && !(S.chan_ctl[1] & 1) && S.chan_inst[1] == 0, "%s: channel removed and unbound", tag);
    CHECK(nv_fifo_submit(&F) == 0 && nv_fifo_copy(&F, 1, 4, 1, 4, 4, 1) == E_IO, "%s: nothing goes after stop", tag);
    nv_fifo_stop(&F, "twice");
}

/* ---------------- hangs ---------------- */

static void test_hangs(void) {
    static const struct { u32 hang; const char *word; u16 cs; } starts[] = {
        { H_DEAD, "unreadable", 0x134 }, { H_NOCE, "no copy engine", 0x134 }, { H_GRCE, "no copy engine", 0xe4 },
        { H_NOCHAN, "no free channel", 0x124 }, { H_BARFLUSH, "BAR flush", 0x134 }, { H_TLB, "invalidate", 0x117 },
        { H_RUNLIST, "stays pending", 0x134 }, { H_NOFETCH, "did not complete", 0xe4 }, { H_CE, "did not complete", 0x130 },
        { H_SEMLOST, "did not complete", 0x106 }, { H_PREEMPT | H_NOFETCH, "did not complete", 0x134 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(starts); i++) {
        sim_reset(starts[i].cs, starts[i].hang);
        u32 pmc0 = S.pmc, pb0 = S.pbmask, c10_0 = S.c10;
        fifo_setup();
        u64 t0 = S.t;
        int r = nv_fifo_start(&F);
        CHECK(r < 0 && strstr(F.why, starts[i].word), "hang %x: %s (%s)", starts[i].hang, strerror(r), F.why);
        CHECK(S.t - t0 < 400000, "hang %x: gave up after %llu us", starts[i].hang, (unsigned long long)(S.t - t0));
        u64 t1 = S.t;
        nv_fifo_stop(&F, "test");
        CHECK(S.t - t1 < 100000, "hang %x: stop took %llu us", starts[i].hang, (unsigned long long)(S.t - t1));
        if (!(starts[i].hang & H_DEAD)) check_restored("hang", pmc0, pb0, c10_0);
    }
    /* during work: a copy that never ends, a lost semaphore, a launch error, a PBDMA error, an MMU fault */
    static const u32 later[] = { H_CE, H_SEMLOST, H_LAUNCH, H_FAULTRO, H_PBDMA, H_DEAD };
    for (size_t i = 0; i < ARRAY_SIZE(later); i++) {
        int r = start_chip(0x134, 0);
        CHECK(r == 0, "start for the hang %x", later[i]);
        if (r) continue;
        nv_fifo_map_sys(&F, SYS_BASE, 0x10000);
        S.hang = later[i];
        u64 dst = nv_fifo_va_vram(&F, 16u << 20);
        if (later[i] == H_FAULTRO) nv_fifo_copy(&F, NV_FIFO_VA_SYS, 256, dst, 256, 256, 4);   /* a write to the read-only window */
        else if (later[i] == H_PBDMA) { F.q.w[F.q.n++] = nv_fifo_incr(NV_SUBC_CE, 0x0900, 1); F.q.w[F.q.n++] = 0; }   /* an illegal method */
        else nv_fifo_copy(&F, dst, 256, NV_FIFO_VA_SYS, 256, 256, 4);
        u32 f = nv_fifo_submit(&F);
        u64 t0 = S.t;
        r = f ? nv_fifo_wait(&F, f, 1000000) : E_IO;
        CHECK(r < 0 && F.dead, "hang %x during work: %s (%s)", later[i], strerror(r), F.why);
        CHECK(S.t - t0 <= 60000 + 1000, "hang %x: noticed after %llu us", later[i], (unsigned long long)(S.t - t0));
        const char *want = later[i] == H_LAUNCH ? "LAUNCHERR" : later[i] == H_FAULTRO ? "RO_VIOLATION" : later[i] == H_PBDMA ? "PBDMA" :
                           later[i] == H_DEAD ? "unreadable" : "did not complete";
        CHECK(strstr(F.why, want) != NULL, "hang %x: '%s' lacks '%s'", later[i], F.why, want);
        CHECK(nv_fifo_submit(&F) == 0, "hang %x: nothing more is sent", later[i]);
        S.hang &= ~(u32)H_DEAD;
        nv_fifo_stop(&F, "test");
        CHECK(S.x2254 == 0 && S.chan_inst[1] == 0, "hang %x: put back", later[i]);
    }
    /* a stale TLB: if the invalidate is lost, the copy reads the old window (the simulator's TLB model works) */
    if (start_chip(0x134, 0) == 0) {
        for (u32 i = 0; i < 0x2000 / 4; i++) ((u32 *)S.sys)[i] = 0x11111111u;
        for (u32 i = 0; i < 0x2000 / 4; i++) ((u32 *)(S.sys + 0x100000))[i] = 0x22222222u;
        nv_fifo_map_sys(&F, SYS_BASE, 0x1000);
        u64 dst = nv_fifo_va_vram(&F, 16u << 20);
        nv_fifo_copy(&F, dst, 256, nv_fifo_va_sys(&F, SYS_BASE, 256), 256, 256, 1);
        nv_fifo_wait(&F, nv_fifo_submit(&F), 50000);
        S.hang = H_NOINVAL;
        nv_fifo_map_sys(&F, SYS_BASE + 0x100000, 0x1000);
        nv_fifo_copy(&F, dst, 256, nv_fifo_va_sys(&F, SYS_BASE + 0x100000, 256), 256, 256, 1);
        nv_fifo_wait(&F, nv_fifo_submit(&F), 50000);
        CHECK(v32(16u << 20) == 0x11111111u, "the simulator's TLB keeps a stale translation without an invalidate (%08x)", v32(16u << 20));
        nv_fifo_stop(&F, "test");
    }
    /* the push ring full: the engine stops taking work, the submit gives up in time */
    if (start_chip(0x124, 0) == 0) {
        nv_fifo_map_sys(&F, SYS_BASE, 0x10000);
        S.hang = H_NOFETCH;
        int ok = 1;
        u64 t0 = S.t;
        for (int i = 0; i < 2000 && ok; i++) {
            for (int k = 0; k < 20; k++) nv_fifo_fill(&F, nv_fifo_va_vram(&F, 16u << 20), 256, 64, 1, 0);
            if (!nv_fifo_submit(&F)) ok = 0;
        }
        CHECK(!ok && F.dead && strstr(F.why, "push ring"), "ring full: %s", F.why);
        CHECK(S.t - t0 < 200000, "ring full noticed after %llu us", (unsigned long long)(S.t - t0));
        nv_fifo_stop(&F, "test");
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    test_gens();
    test_top();
    test_streams();
    test_vmm();
    static const u16 chips[] = { 0xe4, 0xf0, 0x106, 0x117, 0x124, 0x130, 0x134 };
    for (size_t i = 0; i < ARRAY_SIZE(chips); i++) test_chip(chips[i]);
    test_hangs();
    free(S.vram);
    free(S.sys);
    free(refv);
    if (failed) printf("fifotest: %d of %d checks FAILED\n", failed, checks);
    else printf("fifotest: all %d checks passed (generations, topology, copy / fill / semaphore streams against nouveau, page tables of "
                "GF100 / GM200 / GP100, bring-up and pixel-checked drawing on %u simulated chips, hangs and faults)\n", checks,
                (unsigned)ARRAY_SIZE(chips));
    return failed != 0;
}
