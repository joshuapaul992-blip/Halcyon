/* Halcyon OS - NVIDIA GPUs (M10e): the copy engine for the display core.
 * See accel.h and docs/DESIGN-M10-NVIDIA.md, "10e".
 *
 * Safety: nothing is written before the start guard of the display driver
 * is done with the adapter and the acceleration marker is saved; the
 * channel, the page tables and the semaphore live in two 2 MiB blocks of
 * VRAM of our own (BAR1 is checked to map them 1:1, as 10b checks its
 * own); the copy engine reads system RAM only through a read-only window
 * onto the desktop surface; every wait is bounded (fifo.c); a failure
 * stops the channel, puts the registers back and leaves presenting to the
 * CPU. The GPU's interrupts stay masked (10b): everything is polled. */
#include "accel.h"
#include "fifo.h"
#include "drivers/display.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

#define MiB (1ull << 20)
#define TEST_W 64
#define TEST_H 16

struct nv_accel {
    nv_device_t *d;
    display_adapter_t *a;
    const nv_fifo_gen_t *g;
    const char *no_why;         /* why this generation has none (NULL: it has) */
    int want;                   /* nv.accel */
    int attached, tried, running;
    nv_accel_host_t h;
    nv_fifo_t *f;
    u64 userd, blk;             /* our VRAM blocks (kept for the session once used) */
    u8 *flush_page, *test;      /* system RAM: the flush page, the self test's source */
    const void *src;            /* the desktop surface mapped */
    u64 src_phys, src_bytes;
    u16 pci_cmd;
    int bm_set;                 /* we turned bus mastering on */
    u64 start_us;
    char why[112];
};

static void alog(nv_accel_t *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void alog(nv_accel_t *x, const char *fmt, ...) {
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (x->h.log) x->h.log(x->h.ctx, line);
    else kprintf("nv%d: %s\n", x->d->index, line);
}

static void flog(void *ctx, const char *line) { nv_accel_t *x = ctx; alog(x, "%s", line); }
static void fwmb(void *ctx) { (void)ctx; __asm__ volatile("sfence" ::: "memory"); }

/* ---------------- the operations (display_accel_ops_t) ---------------- */

static nv_accel_t *of(display_adapter_t *a) { return a->accel_priv; }

/* a buffer of ours, the rectangle inside it: its GPU virtual address */
static u64 fb_va(nv_accel_t *x, const display_fb_t *fb, int px, int py, int w, int h) {
    if (!fb->px || px < 0 || py < 0 || w <= 0 || h <= 0 || px + w > fb->w || py + h > fb->h) return 0;
    u64 off = (u64)fb->handle + (u64)py * fb->pitch + (u64)px * 4;
    if ((u64)fb->handle + (u64)fb->pitch * (u64)fb->h > x->f->vram_size) return 0;
    return nv_fifo_va_vram(x->f, off);
}

static int a_source(display_adapter_t *a, const void *px, u64 bytes) {
    nv_accel_t *x = of(a);
    if (!x->running) return E_IO;
    int r = nv_fifo_map_sys(x->f, virt_to_phys(px), bytes);
    if (r) {
        alog(x, "10e: the desktop surface (%llu KiB at %llx) cannot be mapped for the copy engine: %s", (unsigned long long)(bytes >> 10),
             (unsigned long long)virt_to_phys(px), strerror(r));
        x->src = NULL;
        return r;
    }
    if (!x->src) alog(x, "10e: the desktop surface (%llu KiB at %llx) is readable by the copy engine at %llx", (unsigned long long)(bytes >> 10),
                      (unsigned long long)virt_to_phys(px), (unsigned long long)x->f->sys_va);
    x->src = px;
    x->src_phys = virt_to_phys(px);
    x->src_bytes = bytes;
    return 0;
}

static int a_upload(display_adapter_t *a, display_fb_t *dst, int px, int py, int w, int h, u64 off, u32 pitch) {
    nv_accel_t *x = of(a);
    if (!x->running || !x->src) return E_IO;
    u64 d = fb_va(x, dst, px, py, w, h);
    u64 s = nv_fifo_va_sys(x->f, x->src_phys + off, (u64)(h - 1) * pitch + (u64)w * 4);
    if (!d || !s) return E_INVAL;
    return nv_fifo_copy(x->f, d, dst->pitch, s, pitch, (u32)w * 4, (u32)h);
}

static int a_copy(display_adapter_t *a, display_fb_t *dst, int dx, int dy, display_fb_t *src, int sx, int sy, int w, int h) {
    nv_accel_t *x = of(a);
    if (!x->running) return E_IO;
    if (dst->handle == src->handle && dx < sx + w && sx < dx + w && dy < sy + h && sy < dy + h) return E_INVAL;   /* (overlapping) */
    u64 d = fb_va(x, dst, dx, dy, w, h), s = fb_va(x, src, sx, sy, w, h);
    if (!d || !s) return E_INVAL;
    return nv_fifo_copy(x->f, d, dst->pitch, s, src->pitch, (u32)w * 4, (u32)h);
}

static int a_fill(display_adapter_t *a, display_fb_t *dst, int px, int py, int w, int h, u32 c) {
    nv_accel_t *x = of(a);
    if (!x->running) return E_IO;
    u64 d = fb_va(x, dst, px, py, w, h);
    if (!d) return E_INVAL;
    return nv_fifo_fill(x->f, d, dst->pitch, (u32)w, (u32)h, c & 0x00FFFFFF);
}

static u32 a_submit(display_adapter_t *a) {
    nv_accel_t *x = of(a);
    if (!x->running) return 0;
    u32 f = nv_fifo_submit(x->f);
    if (!f && x->f->dead) snprintf(a->accel_why, sizeof a->accel_why, "%.95s", x->f->why);
    return f;
}

static int a_done(display_adapter_t *a, u32 fence) {
    nv_accel_t *x = of(a);
    if (!x->running) return E_IO;
    int r = nv_fifo_poll(x->f, fence);
    if (r < 0) snprintf(a->accel_why, sizeof a->accel_why, "%.95s", x->f->why);
    return r;
}

static void stop_engine(nv_accel_t *x, const char *why) {
    if (x->f && (x->f->bound || x->f->nsaved)) nv_fifo_stop(x->f, why);
    if (x->bm_set) {
        /* (the GPU reads system RAM no more) */
        pci_write16(x->d->pci, 0x04, (u16)(pci_read16(x->d->pci, 0x04) & ~0x0004));
        x->bm_set = 0;
    }
    x->running = 0;
    x->src = NULL;
}

static void a_stop(display_adapter_t *a, const char *why) {
    nv_accel_t *x = of(a);
    alog(x, "10e: the copy engine is given up: %s; the CPU presents", why ? why : "?");
    stop_engine(x, why);
    strlcpy(x->why, why ? why : "stopped", sizeof x->why);
}

static const display_accel_ops_t nv_accel_ops = { a_source, a_upload, a_copy, a_fill, a_submit, a_done, a_stop };

/* ---------------- registration ---------------- */

/* the simulated card: what the copy engine would be sent (no hardware) */
static void dry_run(nv_accel_t *x) {
    static nv_fifo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_ce_object_methods(&s, x->g->ce_class);
    int n0 = s.n;
    nv_ce_copy_methods(&s, NV_FIFO_VA_VRAM + 4 * MiB, 7680, NV_FIFO_VA_SYS + 0x40, 7680, 1920 * 4, 1200);
    int n1 = s.n;
    nv_ce_fill_methods(&s, NV_FIFO_VA_VRAM + 4 * MiB, 7680, 1920, 1200, 0);
    int n2 = s.n;
    nv_host_release_methods(&s, NV_FIFO_VA_VRAM + 2 * MiB + FIFO_BLK_SEM, 1);
    alog(x, "10e: dry run: SET_OBJECT %04x %d words, a 1920x1200 upload %d words (%08x %08x ... %08x), a fill %d words (%08x ... %08x), "
         "the semaphore release %d words", x->g->ce_class, n0, n1 - n0, s.w[n0], s.w[n0 + 1], s.w[n1 - 1], n2 - n1, s.w[n1], s.w[n2 - 1], s.n - n2);
    u8 *pool = kzalloc(0x8000);
    if (pool) {
        static nv_vmm_t v;
        int r = nv_vmm_init(&v, x->g->vmm, 17, pool, 0x200000 + FIFO_BLK_PT, 0x8000, NV_FIFO_VA_LIMIT);
        if (!r) r = nv_vmm_map_vram(&v, NV_FIFO_VA_VRAM, 0, 4 * MiB);
        if (!r) alog(x, "10e: dry run: page tables (%s format): directory base %016llx, %llu KiB of tables for 4 MiB of VRAM",
                     x->g->vmm == NV_VMM_GP100 ? "GP100" : x->g->vmm == NV_VMM_GM200 ? "GM200" : "GF100", (unsigned long long)nv_vmm_pdb(&v),
                     (unsigned long long)(v.used >> 10));
        kfree(pool);
    }
}

nv_accel_t *nv_accel_probe(nv_device_t *d, display_adapter_t *a, void (*log)(void *ctx, const char *line), void *ctx) {
    nv_accel_t *x = kzalloc(sizeof *x);
    if (!x) return NULL;
    x->h.log = log;
    x->h.ctx = ctx;
    x->d = d;
    x->a = a;
    x->g = nv_fifo_gen_for(d->chip.chipset, &x->no_why);
    x->want = settings_get_int("nv.accel", 1) != 0;
    a->accel_ops = &nv_accel_ops;
    a->accel_priv = x;
    a->accel = 0;
    if (!x->g) {
        snprintf(a->accel_why, sizeof a->accel_why, "no copy engine for %.40s: %.40s", d->chip.codename, x->no_why);
        alog(x, "10e: no acceleration: %s", x->no_why);
    } else if (!x->want) {
        snprintf(a->accel_why, sizeof a->accel_why, "nv.accel=0 in SETTINGS.INI");
        alog(x, "10e: acceleration off (nv.accel=0 in SETTINGS.INI)");
    } else if (d->fake) {
        snprintf(a->accel_why, sizeof a->accel_why, "simulated card: no registers");
        dry_run(x);
        alog(x, "10e: no channel started, no register touched: the simulated card has no registers");
    } else {
        snprintf(a->accel_why, sizeof a->accel_why, "not started yet (it starts once the desktop runs on the driver)");
        alog(x, "10e: copy engine acceleration (%s fifo, class %04x, %s page tables): starts once the desktop runs on the driver", x->g->name,
             x->g->ce_class, x->g->vmm == NV_VMM_GP100 ? "GP100" : x->g->vmm == NV_VMM_GM200 ? "GM200" : "GF100");
    }
    return x;
}

void nv_accel_attach(nv_accel_t *x, const nv_accel_host_t *h) {
    if (!x) return;
    x->h = *h;
    x->attached = 1;
}

/* ---------------- the bring-up ---------------- */

static int vram_block(nv_accel_t *x, u64 *at, const char *what) {
    *at = nv_evo_vram_alloc(x->h.vram, 2 * MiB, 2 * MiB);
    if (*at == ~0ull) { snprintf(x->why, sizeof x->why, "no free VRAM inside BAR1 for the %s", what); return E_NOMEM; }
    if (x->h.vram_linear && (!x->h.vram_linear(x->h.ctx, *at) || !x->h.vram_linear(x->h.ctx, *at + 2 * MiB - 4096))) {
        snprintf(x->why, sizeof x->why, "BAR1 does not map the %s block (VRAM %llx) linearly", what, (unsigned long long)*at);
        return E_NOTSUP;
    }
    return 0;
}

/* fill, copy and upload a small pattern in our scratch area and read it
 * back through BAR1: the caps that work */
static u32 self_test(nv_accel_t *x) {
    nv_fifo_t *f = x->f;
    const u32 pitch = TEST_W * 4;
    u64 A = x->blk + FIFO_BLK_SCRATCH, B = A + 0x1000, C = A + 0x2000;
    volatile u32 *va = (volatile u32 *)(x->h.bar1 + A), *vb = (volatile u32 *)(x->h.bar1 + B), *vc = (volatile u32 *)(x->h.bar1 + C);
    for (int i = 0; i < TEST_W * TEST_H; i++) { va[i] = 0; vb[i] = 0; vc[i] = 0; ((u32 *)x->test)[i] = 0x01000000u * (u32)(i & 0x7F) ^ (u32)i * 2654435761u; }
    u32 caps = 0;
    int r = nv_fifo_fill(f, nv_fifo_va_vram(f, A), pitch, TEST_W, TEST_H, 0x00A5C3E1);
    if (!r) r = nv_fifo_copy(f, nv_fifo_va_vram(f, B), pitch, nv_fifo_va_vram(f, A), pitch, pitch, TEST_H);
    if (!r) r = nv_fifo_map_sys(f, virt_to_phys(x->test), 4096);
    if (!r) r = nv_fifo_copy(f, nv_fifo_va_vram(f, C), pitch, nv_fifo_va_sys(f, virt_to_phys(x->test), 4096), pitch, pitch, TEST_H);
    if (r) { snprintf(x->why, sizeof x->why, "self test: %s", f->why[0] ? f->why : strerror(r)); return 0; }
    u64 t0 = time_us();
    u32 fence = nv_fifo_submit(f);
    if (!fence || nv_fifo_wait(f, fence, 100000)) { snprintf(x->why, sizeof x->why, "self test: %s", f->why); return 0; }
    u64 us = time_us() - t0;
    int fill_ok = 1, copy_ok = 1, up_ok = 1;
    for (int i = 0; i < TEST_W * TEST_H; i++) {
        if (va[i] != 0x00A5C3E1u) fill_ok = 0;
        if (vb[i] != va[i]) copy_ok = 0;
        if (vc[i] != ((u32 *)x->test)[i]) up_ok = 0;
    }
    alog(x, "10e: self test (%dx%d, done after %llu us): fill %s (%08x), copy within VRAM %s (%08x), copy from system RAM %s (%08x, want %08x)",
         TEST_W, TEST_H, (unsigned long long)us, fill_ok ? "ok" : "WRONG", va[TEST_W + 3], copy_ok ? "ok" : "WRONG", vb[TEST_W + 3],
         up_ok ? "ok" : "WRONG", vc[TEST_W + 3], ((u32 *)x->test)[TEST_W + 3]);
    if (up_ok) caps |= ACCEL_UPLOAD;
    if (copy_ok && fill_ok) caps |= ACCEL_COPY;     /* (the copy's source was the fill) */
    if (fill_ok) caps |= ACCEL_FILL;
    if (!up_ok) snprintf(x->why, sizeof x->why, "self test: the copy from system RAM came out wrong");
    return caps;
}

static void bring_up(nv_accel_t *x) {
    display_adapter_t *a = x->a;
    nv_device_t *d = x->d;
    x->tried = 1;
    /* the marker first (a hang from here on keeps acceleration off at the next start) */
    if (display_accel_guard_begin(a)) {
        alog(x, "10e: acceleration not started: %s", a->accel_why);
        return;
    }
    x->start_us = time_us();
    int r = 0;
    if (!x->userd && (r = vram_block(x, &x->userd, "USERD"))) goto out;
    if (!x->blk && (r = vram_block(x, &x->blk, "channel"))) goto out;
    if (!x->flush_page) x->flush_page = dma_alloc(4096, 4096);
    if (!x->test) x->test = dma_alloc(4096, 4096);
    x->f = x->f ? x->f : kzalloc(sizeof *x->f);
    if (!x->flush_page || !x->test || !x->f) { snprintf(x->why, sizeof x->why, "out of memory"); r = E_NOMEM; goto out; }
    memset(x->f, 0, sizeof *x->f);
    nv_fifo_t *f = x->f;
    f->g = x->g;
    f->r = &d->regs;
    f->ops = (nv_fifo_ops_t){ x, flog, fwmb };
    f->chipset = d->chip.chipset;
    f->vram = x->h.bar1;
    f->vram_size = x->h.window;
    f->userd = x->userd;
    f->userd_bytes = 2 * MiB;
    f->blk = x->blk;
    f->flush_page = virt_to_phys(x->flush_page);
    f->timeout_us = 100000;
    f->fence_us = 200000;
    alog(x, "10e: bring-up: USERD block %llx, channel block %llx, flush page %llx, %llu MiB of VRAM mapped", (unsigned long long)x->userd,
         (unsigned long long)x->blk, (unsigned long long)f->flush_page, (unsigned long long)(f->vram_size >> 20));
    /* the copy engine reads system RAM: bus mastering on (put back when it stops) */
    x->pci_cmd = pci_read16(d->pci, 0x04);
    if (!(x->pci_cmd & 0x0004)) {
        pci_write16(d->pci, 0x04, x->pci_cmd | 0x0004);
        x->bm_set = 1;
        alog(x, "10e: PCI bus mastering was off (command %04x): on while the copy engine runs", x->pci_cmd);
    }
    r = nv_fifo_start(f);
    if (r) { snprintf(x->why, sizeof x->why, "%s", f->why); goto out; }
    x->running = 1;
    u32 caps = self_test(x);
    if (!(caps & ACCEL_UPLOAD)) { r = E_IO; goto out; }
    char name[48];
    snprintf(name, sizeof name, "copy engine %d (class %04x)", f->ce_inst, x->g->ce_class);
    alog(x, "10e: acceleration in use: %s on runlist %d, channel %d, after %llu ms", name, f->runl, f->chid,
         (unsigned long long)((time_us() - x->start_us) / 1000));
    x->why[0] = 0;
    display_accel_ready(a, caps, name);
    return;
out:
    alog(x, "10e: acceleration not available: %s; the CPU presents (the marker display.nv.accel.guard stays: no new try at the next start)",
         x->why[0] ? x->why : strerror(r));
    stop_engine(x, x->why);
    snprintf(a->accel_why, sizeof a->accel_why, "%.95s", x->why[0] ? x->why : strerror(r));
}

void nv_accel_poll(nv_accel_t *x) {
    if (!x || x->tried || !x->attached || !x->g || !x->want || x->d->fake) return;
    if (!display_guard_settled(x->a)) return;
    /* (at least one head lit: the desktop runs on the driver) */
    int lit = 0;
    for (int k = 0; k < x->a->nheads; k++) if (x->a->heads[k].conn) lit = 1;
    if (!lit) return;
    bring_up(x);
}

void nv_accel_release(nv_accel_t *x, const char *why) {
    if (!x) return;
    if (x->running || (x->f && x->f->nsaved)) {
        alog(x, "10e: the copy engine stops with the driver (%s)", why ? why : "released");
        stop_engine(x, why);
    }
    x->a->accel = 0;
    x->attached = 0;
}

/* ---------------- state and report ---------------- */

const char *nv_accel_state(nv_accel_t *x, char *buf, int n) {
    if (!x) { snprintf(buf, (size_t)n, "none"); return buf; }
    return display_accel_state(x->a, buf, n);
}

int nv_accel_report(nv_accel_t *x, char *buf, int size) {
    int o = 0;
    if (size < 1) return 0;
    buf[0] = 0;
    if (!x) return 0;
    char st[160];
    o += snprintf(buf + o, (size_t)(size - o), "  state: %s\n", display_accel_state(x->a, st, sizeof st));
    o += snprintf(buf + o, (size_t)(size - o), "  settings: nv.accel=%d; generation: %s\n", x->want,
                  x->g ? x->g->name : x->no_why ? x->no_why : "?");
    nv_fifo_t *f = x->f;
    if (f && o < size - 1) {
        o += snprintf(buf + o, (size_t)(size - o),
                      "  copy engine %d (class %04x), runlist %d, engine %d, PBDMA %d of %d, channel %d of %u; %s page tables, directory %llx\n"
                      "  %u submissions, %u copies, %u fills, %llu MB moved, %u waits for room, %u errors; last fence %u, done %u\n",
                      f->ce_inst, x->g->ce_class, f->runl, f->engine, f->pbdma, f->npbdma, f->chid, f->chids,
                      f->vmm.fmt == NV_VMM_GP100 ? "GP100" : f->vmm.fmt == NV_VMM_GM200 ? "GM200" : "GF100", (unsigned long long)f->vmm.pd,
                      f->submits, f->copies, f->fills, (unsigned long long)(f->bytes / 1000000), f->waits, f->errors, f->seq, f->done);
        if (x->running && o < size - 1) {
            char s2[200];
            nv_fifo_state(f, s2, sizeof s2);
            o += snprintf(buf + o, (size_t)(size - o), "  registers: %s\n", s2);
        }
        if (f->why[0] && o < size - 1) o += snprintf(buf + o, (size_t)(size - o), "  last problem: %s\n", f->why);
    }
    return MIN(o, size - 1);
}
