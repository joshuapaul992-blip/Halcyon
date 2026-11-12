/* Halcyon OS - NVIDIA GPUs (M10a): probe, video BIOS sources, I2C / AUX
 * ports, EDIDs. See nv.h for what it does and docs/DESIGN-M10-NVIDIA.md
 * for the sources of every register used here: the I2C port, hybrid pad,
 * GPIO and PRAMIN / PROM registers and rules follow nouveau
 * (nvkm/subdev/i2c, gpio, bios/shadow*.c; Copyright 2012 Red Hat Inc.,
 * MIT, the notice is in THIRD_PARTY_NOTICES.md).
 *
 * Rules: only reads, except the writes reading needs, each undone at once;
 * every wait is bounded (time_us); a timeout or an unexpected value is
 * logged ("nv: ...") and that step is given up. Kernel threads are
 * cooperative, so nothing else touches the GPU while this runs. */
#include "nv.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

static nv_device_t *gpus[NV_MAX_GPUS];
static int ngpus;

int nv_count(void) { return ngpus; }
nv_device_t *nv_get(int i) { return i >= 0 && i < ngpus ? gpus[i] : NULL; }
nv_device_t *nv_for_pci(const pci_dev_t *p) {
    for (int i = 0; i < ngpus; i++) if (gpus[i]->pci == p) return gpus[i];
    return NULL;
}

/* ---------------- registers ---------------- */

u32 nv_rd32(nv_device_t *d, u32 reg) {
    if (!d->mmio || reg > d->mmio_size - 4 || (reg & 3)) return 0xFFFFFFFFu;
    return mmio_read32(d->mmio + reg);
}
void nv_wr32(nv_device_t *d, u32 reg, u32 v) {
    if (!d->mmio || reg > d->mmio_size - 4 || (reg & 3)) return;
    mmio_write32(d->mmio + reg, v);
}
u32 nv_mask(nv_device_t *d, u32 reg, u32 mask, u32 val) {
    u32 old = nv_rd32(d, reg);
    nv_wr32(d, reg, (old & ~mask) | val);
    return old;
}
u8 nv_rd08(nv_device_t *d, u32 reg) {
    if (!d->mmio || reg >= d->mmio_size) return 0xFF;
    return mmio_read8(d->mmio + reg);
}
void nv_wr08(nv_device_t *d, u32 reg, u8 v) {
    if (!d->mmio || reg >= d->mmio_size) return;
    mmio_write8(d->mmio + reg, v);
}

static u32 r_rd32(void *c, u32 reg) { return nv_rd32(c, reg); }
static void r_wr32(void *c, u32 reg, u32 v) { nv_wr32(c, reg, v); }
static void r_delay(void *c, u32 us) { udelay(us); }
static u64 r_now(void *c) { return time_us(); }

/* ---------------- lookups ---------------- */

const nv_dcb_output_t *nv_output(nv_device_t *d, int i) {
    return d->bios && i >= 0 && i < d->bios->noutputs ? &d->bios->out[i] : NULL;
}

const nv_dcb_output_t *nv_output_on(nv_device_t *d, int conn, int type, int nth) {
    if (!d->bios) return NULL;
    for (int i = 0; i < d->bios->noutputs; i++) {
        const nv_dcb_output_t *o = &d->bios->out[i];
        if (o->connector == conn && (type < 0 || o->type == type) && nth-- == 0) return o;
    }
    return NULL;
}

nv_connector_t *nv_connector(nv_device_t *d, int conn) {
    for (int i = 0; i < d->nconn; i++) if (d->conn[i].index == conn) return &d->conn[i];
    return NULL;
}

static const nv_ccb_entry_t *ccb_get(nv_device_t *d, int ccb) {
    if (!d->bios || ccb < 0 || ccb >= d->bios->nccb) return NULL;
    const nv_ccb_entry_t *c = &d->bios->ccb[ccb];
    return c->type == NV_CCB_UNUSED ? NULL : c;
}

/* ---------------- GPIO: hot-plug sense ---------------- */

static int gpio_sense(nv_device_t *d, int line) {
    u32 v;
    switch (d->chip.gpio) {
    case NV_GPIO_NV10:
        if (line < 2) { v = nv_rd32(d, 0x600818); return nv_bad_read(v) ? -1 : !!((v >> (line * 16)) & 0x100); }
        if (line < 10) { v = nv_rd32(d, 0x60081C); return nv_bad_read(v) ? -1 : !!((v >> ((line - 2) * 4)) & 4); }
        if (line < 14) { v = nv_rd32(d, 0x600850); return nv_bad_read(v) ? -1 : !!((v >> ((line - 10) * 4)) & 4); }
        return -1;
    case NV_GPIO_NV50: {
        static const u32 regs[4] = { 0xE104, 0xE108, 0xE280, 0xE284 };
        if (line >= 32) return -1;
        v = nv_rd32(d, regs[line >> 3]);
        return nv_bad_read(v) ? -1 : !!(v & (4u << ((line & 7) * 4)));
    }
    case NV_GPIO_GF119:
        if (line >= 32) return -1;
        v = nv_rd32(d, 0xD610 + (u32)line * 4);
        return nv_bad_read(v) ? -1 : !!(v & 0x4000);
    }
    return -1;
}

int nv_hotplug(nv_device_t *d, int conn) {
    if (!d->bios || conn < 0 || conn >= d->bios->nconn || d->fake) return -1;
    u8 bits = d->bios->conn[conn].hpd;
    for (int b = 0; b < 7; b++) {
        if (!(bits & (1 << b))) continue;
        int f = nv_hpd_func(b);
        for (int g = 0; g < d->bios->ngpio; g++) {
            const nv_gpio_entry_t *e = &d->bios->gpio[g];
            if (e->func != f) continue;
            int s = gpio_sense(d, e->line);
            if (s < 0) return -1;
            return s == (e->log[1] & 1);
        }
    }
    return -1;
}

/* ---------------- I2C ports ---------------- */

enum { PK_NONE, PK_CRTC, PK_NV4E, PK_NV50, PK_GF119 };

typedef struct port {
    nv_device_t *d;
    int kind;
    u32 reg;                    /* NV4E / NV50 / GF119: the port register */
    u32 data;                   /* NV50: the lines as last driven */
    u32 saved;                  /* the port register (CRTC: the drive register) as found */
    u8  drive, sense;           /* CRTC: register indices */
    u8  crx, waslocked;         /* CRTC: the index register, the extended registers' lock */
} port_t;

static u8 cr_rd(nv_device_t *d, u8 i) { nv_wr08(d, 0x6013D4, i); return nv_rd08(d, 0x6013D5); }
static void cr_wr(nv_device_t *d, u8 i, u8 v) { nv_wr08(d, 0x6013D4, i); nv_wr08(d, 0x6013D5, v); }

static void p_scl(void *c, int st) {
    port_t *p = c;
    nv_device_t *d = p->d;
    switch (p->kind) {
    case PK_CRTC: { u8 v = cr_rd(d, p->drive); v = st ? (v | 0x20) : (v & ~0x20); cr_wr(d, p->drive, v | 0x01); break; }
    case PK_NV4E: nv_mask(d, p->reg, 0x2F, st ? 0x21 : 0x01); break;
    case PK_NV50: p->data = st ? (p->data | 1) : (p->data & ~1u); nv_wr32(d, p->reg, p->data); break;
    case PK_GF119: nv_mask(d, p->reg, 0x1, st ? 0x1 : 0); break;
    }
}
static void p_sda(void *c, int st) {
    port_t *p = c;
    nv_device_t *d = p->d;
    switch (p->kind) {
    case PK_CRTC: { u8 v = cr_rd(d, p->drive); v = st ? (v | 0x10) : (v & ~0x10); cr_wr(d, p->drive, v | 0x01); break; }
    case PK_NV4E: nv_mask(d, p->reg, 0x1F, st ? 0x11 : 0x01); break;
    case PK_NV50: p->data = st ? (p->data | 2) : (p->data & ~2u); nv_wr32(d, p->reg, p->data); break;
    case PK_GF119: nv_mask(d, p->reg, 0x2, st ? 0x2 : 0); break;
    }
}
static int p_sense_scl(void *c) {
    port_t *p = c;
    switch (p->kind) {
    case PK_CRTC: return !!(cr_rd(p->d, p->sense) & 0x04);
    case PK_NV4E: return !!(nv_rd32(p->d, p->reg) & 0x00040000);
    case PK_NV50: return !!(nv_rd32(p->d, p->reg) & 0x1);
    case PK_GF119: return !!(nv_rd32(p->d, p->reg) & 0x10);
    }
    return 1;
}
static int p_sense_sda(void *c) {
    port_t *p = c;
    switch (p->kind) {
    case PK_CRTC: return !!(cr_rd(p->d, p->sense) & 0x08);
    case PK_NV4E: return !!(nv_rd32(p->d, p->reg) & 0x00080000);
    case PK_NV50: return !!(nv_rd32(p->d, p->reg) & 0x2);
    case PK_GF119: return !!(nv_rd32(p->d, p->reg) & 0x20);
    }
    return 1;
}

static int port_kind(nv_device_t *d, const nv_ccb_entry_t *c) {
    switch (d->chip.i2c) {
    case NV_I2C_NV04: return c->type == NV_CCB_NV04_BIT ? PK_CRTC : PK_NONE;
    case NV_I2C_NV4E: return c->type == NV_CCB_NV4E_BIT ? PK_NV4E : PK_NONE;
    case NV_I2C_NV50: case NV_I2C_G94:
        return (c->type == NV_CCB_NVIO_BIT || (c->type == NV_CCB_PMGR && c->drive != 0xFF)) && c->drive < 10 ? PK_NV50 : PK_NONE;
    case NV_I2C_GF119: case NV_I2C_GM200:
        return (c->type == NV_CCB_NVIO_BIT || (c->type == NV_CCB_PMGR && c->drive != 0xFF)) && c->drive < 16 ? PK_GF119 : PK_NONE;
    }
    return PK_NONE;
}

int nv_ccb_has_i2c(nv_device_t *d, int ccb) {
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    return c && !d->fake && port_kind(d, c) != PK_NONE;
}

int nv_ccb_has_aux(nv_device_t *d, int ccb) {
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    if (!c || d->fake || c->auxch > 7) return 0;
    if (c->type != NV_CCB_NVIO_AUX && c->type != NV_CCB_PMGR) return 0;
    return d->chip.i2c == NV_I2C_G94 || d->chip.i2c == NV_I2C_GF119 || d->chip.i2c == NV_I2C_GM200;
}

/* take the port: CRTC registers unlocked, NV50+ ports in bit-bang mode */
static int port_open(nv_device_t *d, const nv_ccb_entry_t *c, port_t *p) {
    static const u32 nv50_regs[10] = { 0xE138, 0xE150, 0xE168, 0xE180, 0xE254, 0xE274, 0xE764, 0xE780, 0xE79C, 0xE7B8 };
    memset(p, 0, sizeof *p);
    p->d = d;
    p->kind = port_kind(d, c);
    switch (p->kind) {
    case PK_CRTC:
        p->drive = c->drive;
        p->sense = c->sense;
        p->crx = nv_rd08(d, 0x6013D4);
        p->waslocked = cr_rd(d, 0x1F) == 0;
        cr_wr(d, 0x1F, 0x57);                       /* unlock the extended CRTC registers */
        p->saved = cr_rd(d, p->drive);
        return 0;
    case PK_NV4E:
        p->reg = 0x600800 + c->drive;
        p->saved = nv_rd32(d, p->reg);
        return nv_bad_read(p->saved) ? E_NODEV : 0;
    case PK_NV50:
        p->reg = nv50_regs[c->drive];
        p->saved = nv_rd32(d, p->reg);
        if (nv_bad_read(p->saved)) return E_NODEV;
        nv_wr32(d, p->reg, p->data = 7);
        return 0;
    case PK_GF119:
        p->reg = 0xD014 + (u32)c->drive * 0x20;
        p->saved = nv_rd32(d, p->reg);
        if (nv_bad_read(p->saved)) return E_NODEV;
        nv_wr32(d, p->reg, 7);
        return 0;
    }
    return E_NOTSUP;
}

static void port_close(port_t *p) {
    nv_device_t *d = p->d;
    switch (p->kind) {
    case PK_CRTC:
        cr_wr(d, p->drive, (u8)p->saved);
        if (p->waslocked) cr_wr(d, 0x1F, 0x99);
        nv_wr08(d, 0x6013D4, p->crx);
        break;
    case PK_NV4E: case PK_NV50: case PK_GF119:
        nv_wr32(d, p->reg, p->saved);
        break;
    }
}

/* hybrid pads (I2C or AUX on the same pins) */
typedef struct pad { u32 a, b; u32 va, vb; int used; } pad_t;

static void pad_take(nv_device_t *d, const nv_ccb_entry_t *c, int aux, pad_t *s) {
    memset(s, 0, sizeof *s);
    if (c->share == 0xFF || c->share > 7) return;
    u32 off = (u32)c->share * 0x50;
    if (d->chip.i2c == NV_I2C_G94 || d->chip.i2c == NV_I2C_GF119) { s->a = 0xE500 + off; s->b = 0xE50C + off; }
    else if (d->chip.i2c == NV_I2C_GM200) { s->a = 0xD970 + off; s->b = 0xD97C + off; }
    else return;
    s->va = nv_rd32(d, s->a);
    s->vb = nv_rd32(d, s->b);
    if (nv_bad_read(s->va) || nv_bad_read(s->vb)) return;
    s->used = 1;
    nv_wr32(d, s->a, (s->va & ~0xC003u) | (aux ? 0x0002u : 0xC001u));
    nv_wr32(d, s->b, s->vb & ~1u);
    udelay(10);
}

static void pad_give(nv_device_t *d, pad_t *s) {
    if (!s->used) return;
    nv_wr32(d, s->a, s->va);
    nv_wr32(d, s->b, s->vb);
}

/* M10b: keep a CCB entry's hybrid pad in AUX (DisplayPort) mode while an
 * output on it is driven; the values found the first time are kept for
 * nv_pad_restore */
static struct { nv_device_t *d; u32 a, b, va, vb; } held[8];
static int nheld;

int nv_pad_hold_aux(nv_device_t *d, int ccb) {
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    if (!c || d->fake || c->share == 0xFF || c->share > 7) return 0;
    pad_t p;
    pad_take(d, c, 1, &p);                          /* (AUX mode, powered: stays so) */
    if (!p.used) return 0;
    for (int i = 0; i < nheld; i++) if (held[i].d == d && held[i].a == p.a) return 0;
    if (nheld < (int)ARRAY_SIZE(held)) {
        held[nheld].d = d; held[nheld].a = p.a; held[nheld].b = p.b; held[nheld].va = p.va; held[nheld].vb = p.vb;
        nheld++;
    }
    return 1;
}

void nv_pad_restore(nv_device_t *d) {
    for (int i = 0; i < nheld; i++) {
        if (held[i].d != d) continue;
        nv_wr32(d, held[i].a, held[i].va);
        nv_wr32(d, held[i].b, held[i].vb);
        held[i] = held[--nheld];
        i--;
    }
}

static void bb_init(nv_i2c_bb_t *bb, port_t *p) {
    memset(bb, 0, sizeof *bb);
    bb->ctx = p;
    bb->drive_scl = p_scl;
    bb->drive_sda = p_sda;
    bb->sense_scl = p_sense_scl;
    bb->sense_sda = p_sense_sda;
    bb->delay_us = r_delay;
    bb->now_us = r_now;
    bb->half_us = 5;                                /* 100 kHz (DDC's speed) */
    bb->stretch_us = 2200;
    bb->total_us = 200000;
}

int nv_i2c_xfer(nv_device_t *d, int ccb, nv_i2c_msg_t *msgs, int n) {
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    if (!c || d->fake) return E_NODEV;
    port_t p;
    int r = port_open(d, c, &p);
    if (r) return r;
    pad_t pad;
    pad_take(d, c, 0, &pad);
    nv_i2c_bb_t bb;
    bb_init(&bb, &p);
    r = nv_i2c_bb_xfer(&bb, msgs, n);
    pad_give(d, &pad);
    port_close(&p);
    if (r == E_TIMEOUT || r == E_BUSY) d->timeouts++;
    return r;
}

/* ---------------- AUX ---------------- */

static void aux_hw(nv_device_t *d, const nv_ccb_entry_t *c, nv_aux_hw_t *a) {
    memset(a, 0, sizeof *a);
    a->r = &d->regs;
    a->layout = d->chip.i2c == NV_I2C_GM200 ? NV_AUXHW_GM200 : NV_AUXHW_G94;
    a->ch = c->auxch;
}

int nv_aux_xfer(nv_device_t *d, int ccb, u8 req, u32 addr, u8 *buf, u8 *len) {
    if (!nv_ccb_has_aux(d, ccb)) return E_NODEV;
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    nv_aux_hw_t a;
    aux_hw(d, c, &a);
    pad_t pad;
    pad_take(d, c, 1, &pad);
    int r = nv_aux_hw_xfer(&a, req, addr, buf, len);
    pad_give(d, &pad);
    if (r == E_TIMEOUT) d->timeouts++;
    return r;
}

static int aux_fn(void *ctx, u8 req, u32 addr, u8 *buf, u8 *len) {
    nv_aux_hw_t *a = ctx;
    return nv_aux_hw_xfer(a, req, addr, buf, len);
}

static void aux_ops(nv_aux_t *ops, nv_aux_hw_t *a) {
    memset(ops, 0, sizeof *ops);
    ops->ctx = a;
    ops->xfer = aux_fn;
    ops->delay_us = r_delay;
}

int nv_aux_read(nv_device_t *d, int ccb, u32 addr, u8 *buf, int len) {
    if (!nv_ccb_has_aux(d, ccb)) return E_NODEV;
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    nv_aux_hw_t a;
    aux_hw(d, c, &a);
    nv_aux_t ops;
    aux_ops(&ops, &a);
    pad_t pad;
    pad_take(d, c, 1, &pad);
    int r = nv_aux_native_read(&ops, addr, buf, len);
    pad_give(d, &pad);
    if (r == E_TIMEOUT) d->timeouts++;
    return r;
}

int nv_aux_i2c(nv_device_t *d, int ccb, nv_i2c_msg_t *msgs, int n) {
    if (!nv_ccb_has_aux(d, ccb)) return E_NODEV;
    const nv_ccb_entry_t *c = ccb_get(d, ccb);
    nv_aux_hw_t a;
    aux_hw(d, c, &a);
    nv_aux_t ops;
    aux_ops(&ops, &a);
    pad_t pad;
    pad_take(d, c, 1, &pad);
    int r = nv_aux_i2c_xfer(&ops, msgs, n);
    pad_give(d, &pad);
    if (r == E_TIMEOUT) d->timeouts++;
    return r;
}

/* ---------------- EDID ---------------- */

static int bb_fn(void *ctx, nv_i2c_msg_t *m, int n) { return nv_i2c_bb_xfer(ctx, m, n); }

int nv_read_edid(nv_device_t *d, const nv_dcb_output_t *o, u8 *buf, int max, char *via, int vian) {
    char note[64];
    if (via && vian) via[0] = 0;
    const nv_ccb_entry_t *c = o ? ccb_get(d, o->i2c) : NULL;
    if (!c) return E_NODEV;
    int r;
    if (o->type == NV_OUT_DP) {
        if (!nv_ccb_has_aux(d, o->i2c)) return E_NOTSUP;
        nv_aux_hw_t a;
        aux_hw(d, c, &a);
        nv_aux_t ops;
        aux_ops(&ops, &a);
        pad_t pad;
        pad_take(d, c, 1, &pad);
        u8 dpcd[16];
        r = nv_aux_native_read(&ops, 0x000, dpcd, 16);
        if (r >= 1) {
            if (via) snprintf(via, (size_t)vian, "DP AUX channel %d (DPCD %d.%d, %d lanes)", c->auxch, dpcd[0] >> 4,
                              dpcd[0] & 15, dpcd[2] & 0x1F);
            r = nv_ddc_read_edid(nv_aux_i2c_fn, &ops, buf, max, note, sizeof note);
        }
        pad_give(d, &pad);
        if (r == E_TIMEOUT) d->timeouts++;
        if (r < 0 && via && r != E_NODEV) snprintf(via, (size_t)vian, "DP AUX channel %d: %s", c->auxch, strerror(r));
        return r;
    }
    port_t p;
    if ((r = port_open(d, c, &p))) return r;
    pad_t pad;
    pad_take(d, c, 0, &pad);
    nv_i2c_bb_t bb;
    bb_init(&bb, &p);
    r = nv_ddc_read_edid(bb_fn, &bb, buf, max, note, sizeof note);
    pad_give(d, &pad);
    port_close(&p);
    if (r == E_TIMEOUT || r == E_BUSY) d->timeouts++;
    if (via) {
        if (p.kind == PK_CRTC) snprintf(via, (size_t)vian, "CRTC I2C (CR%02X / CR%02X)", c->drive, c->sense);
        else snprintf(via, (size_t)vian, "I2C port %d%s", c->drive, c->share != 0xFF ? " (hybrid pad)" : "");
        if (r < 0) {
            size_t l = strlen(via);
            snprintf(via + l, (size_t)vian - l, ": %s", note[0] ? note : strerror(r));
        }
    }
    return r;
}

/* ---------------- video BIOS sources ---------------- */

typedef int (*rd_word_fn)(void *ctx, u32 off, u32 *v);

/* read 32 KiB at a time until the image chain is complete (or 1 MiB) */
static void read_rom(nv_rom_copy_t *c, rd_word_fn rd, void *ctx, u32 limit) {
    u64 t0 = time_us();
    u8 *buf = kmalloc(NV_ROM_MAX);
    if (!buf) { strlcpy(c->status, "out of memory", sizeof c->status); return; }
    if (limit > NV_ROM_MAX) limit = NV_ROM_MAX;
    u32 got = 0;
    nv_rom_t scan;
    memset(&scan, 0, sizeof scan);
    while (got < limit) {
        u32 end = MIN(got + 0x8000, limit), bad = 0;
        for (; got < end; got += 4) {
            u32 v;
            if (rd(ctx, got, &v)) { bad = 1; break; }
            memcpy(buf + got, &v, 4);
        }
        if (bad) break;
        nv_rom_scan(buf, got, &scan);
        if (scan.score && scan.complete) break;
        u32 sig = (u32)buf[0] | (u32)buf[1] << 8 | (u32)buf[2] << 16 | (u32)buf[3] << 24;
        if (!scan.score && got >= 0x40000 && sig != 0x4947564Eu) break;     /* no ROM in sight */
        if (time_us() - t0 > 3000000) { strlcpy(c->status, "gave up after 3 s", sizeof c->status); break; }
    }
    nv_rom_scan(buf, got, &scan);
    u32 keep = scan.score ? scan.ifr_offset + scan.chain_size : MIN(got, 0x10000u);
    c->data = kmalloc(keep ? keep : 1);
    if (c->data) {
        memcpy(c->data, buf, keep);
        c->size = keep;
    }
    kfree(buf);
    c->score = scan.score;
    c->us = (u32)(time_us() - t0);
    if (c->status[0]) return;
    if (!scan.score) snprintf(c->status, sizeof c->status, "no valid image (%s)", scan.why);
    else {
        int types = 0;
        char t[48] = "";
        for (int i = 0; i < scan.nimages && types < 6; i++, types++) {
            size_t l = strlen(t);
            snprintf(t + l, sizeof t - l, "%s%s", i ? "+" : "", scan.img[i].type == 0 ? "PC" : scan.img[i].type == 3 ? "EFI" :
                     scan.img[i].type == 0xE0 ? "NV" : "?");
        }
        snprintf(c->status, sizeof c->status, "%d image%s (%s), %u KiB%s%s%s", scan.nimages, scan.nimages == 1 ? "" : "s", t,
                 scan.chain_size >> 10, scan.img[0].checksum_ok ? ", checksum ok" : ", checksum wrong",
                 scan.ifr_offset ? ", after IFR data" : "", scan.complete ? "" : ", chain incomplete");
    }
}

typedef struct win { nv_device_t *d; u32 base; } win_t;
static int rd_win(void *ctx, u32 off, u32 *v) {
    win_t *w = ctx;
    *v = nv_rd32(w->d, w->base + off);
    return 0;
}
static int rd_mem(void *ctx, u32 off, u32 *v) {
    *v = mmio_read32((volatile u8 *)ctx + off);
    return 0;
}

static void rom_pramin(nv_device_t *d) {
    nv_rom_copy_t *c = &d->rom[NV_SRC_PRAMIN];
    win_t w = { d, 0x700000 };
    if (d->chip.disp == NV_DISP_NV04) {             /* before NV50 the window always shows the copy */
        if ((nv_rd32(d, 0x700000) & 0xFFFF) != 0xAA55) { strlcpy(c->status, "no image in the window", sizeof c->status); return; }
        read_rom(c, rd_win, &w, NV_ROM_MAX);
        return;
    }
    if (d->chip.family >= NV_FAM_FERMI) {
        u32 v = nv_rd32(d, d->chip.chipset >= 0x110 ? 0x021C04 : 0x022500);
        if (!nv_bad_read(v) && (v & 1)) { strlcpy(c->status, "not available: display engine disabled", sizeof c->status); return; }
    }
    u32 v = nv_rd32(d, 0x619F04);
    if (nv_bad_read(v) || !(v & 8)) { snprintf(c->status, sizeof c->status, "not available: shadow window off (0x619f04 = %08x)", v); return; }
    if ((v & 3) != 1) { snprintf(c->status, sizeof c->status, "not available: shadow not in VRAM (0x619f04 = %08x)", v); return; }
    u64 addr = (u64)(v & 0xFFFFFF00u) << 8;
    u32 save = nv_rd32(d, 0x001700);
    if (!addr) addr = ((u64)save << 16) + 0xF0000;
    if ((addr >> 16) > 0xFFFFFFFFull) { strlcpy(c->status, "not available: address out of range", sizeof c->status); return; }
    nv_wr32(d, 0x001700, (u32)(addr >> 16));
    if (nv_rd32(d, 0x001700) != (u32)(addr >> 16)) {
        nv_wr32(d, 0x001700, save);
        strlcpy(c->status, "not available: the window did not move", sizeof c->status);
        return;
    }
    read_rom(c, rd_win, &w, NV_ROM_MAX);
    nv_wr32(d, 0x001700, save);
    if (nv_rd32(d, 0x001700) != save) kprintf("nv%d: PRAMIN window not restored (%08x)\n", d->index, nv_rd32(d, 0x001700));
    size_t l = strlen(c->status);
    snprintf(c->status + l, sizeof c->status - l, " [VRAM %llx]", (unsigned long long)addr);
}

static void rom_prom(nv_device_t *d) {
    nv_rom_copy_t *c = &d->rom[NV_SRC_PROM];
    if (d->chip.igp) { strlcpy(c->status, "not available: integrated GPUs have no PROM", sizeof c->status); return; }
    if (!settings_get_int("nv.prom", 1)) { strlcpy(c->status, "skipped (nv.prom=0)", sizeof c->status); return; }
    /* NV_PBUS_PCI_NV_20 (config 0x50) bit 0: the window shows the shadow
     * instead of the ROM. It must not be part of a standard capability. */
    pci_dev_t *p = d->pci;
    if (pci_read16(p, 0x06) & (1u << 4)) {
        u8 off = pci_read8(p, 0x34) & 0xFC;
        for (int n = 0; off >= 0x40 && n < 48; n++) {
            u8 id = pci_read8(p, off), len;
            switch (id) {
            case 0x01: len = 8; break;              /* power management */
            case 0x05: len = 24; break;             /* MSI */
            case 0x10: len = 60; break;             /* PCI Express */
            case 0x11: len = 12; break;             /* MSI-X */
            case 0x09: len = pci_read8(p, (u8)(off + 2)); break;   /* vendor specific */
            default: len = 16; break;
            }
            if (off <= 0x53 && off + len > 0x50) {
                snprintf(c->status, sizeof c->status, "skipped: config 0x50 is inside capability %02x at %02x", id, off);
                return;
            }
            off = pci_read8(p, (u8)(off + 1)) & 0xFC;
        }
    }
    u32 save = pci_read32(p, 0x50);
    if (save == 0xFFFFFFFFu) { strlcpy(c->status, "skipped: config 0x50 unreadable", sizeof c->status); return; }
    if (save & 1) pci_write32(p, 0x50, save & ~1u);
    win_t w = { d, 0x300000 };
    read_rom(c, rd_win, &w, NV_ROM_MAX);
    if (save & 1) pci_write32(p, 0x50, save);
    if (pci_read32(p, 0x50) != save) kprintf("nv%d: config 0x50 not restored (%08x, was %08x)\n", d->index, pci_read32(p, 0x50), save);
}

static void rom_bar(nv_device_t *d) {
    nv_rom_copy_t *c = &d->rom[NV_SRC_ROMBAR];
    pci_dev_t *p = d->pci;
    if (!settings_get_int("nv.rombar", 1)) { strlcpy(c->status, "skipped (nv.rombar=0)", sizeof c->status); return; }
    if ((p->header_type & 0x7F) != 0) { strlcpy(c->status, "not available", sizeof c->status); return; }
    u32 orig = pci_read32(p, 0x30);
    u32 base = orig & 0xFFFFF800u;
    if (!base || orig == 0xFFFFFFFFu) { strlcpy(c->status, "not available: no address assigned by the firmware", sizeof c->status); return; }
    pci_write32(p, 0x30, 0xFFFFF800u);
    u32 mask = pci_read32(p, 0x30) & 0xFFFFF800u;
    pci_write32(p, 0x30, orig & ~1u);
    u32 size = mask ? ~mask + 1 : 0;
    if (!size || (size & (size - 1)) || size > (16u << 20) || (base & (size - 1))) {
        pci_write32(p, 0x30, orig);
        snprintf(c->status, sizeof c->status, "not available: no expansion ROM (size mask %08x)", mask);
        return;
    }
    for (int i = 0; i < 6; i++) {
        pci_bar_t *b = &p->bar[i];
        if (b->is_io || !b->addr || !b->size) continue;
        if (base < b->addr + b->size && b->addr < (u64)base + size) {
            pci_write32(p, 0x30, orig);
            snprintf(c->status, sizeof c->status, "skipped: %08x overlaps BAR%d", base, i);
            return;
        }
    }
    void *m = mm_map_mmio(base, size);
    if (!m) { pci_write32(p, 0x30, orig); strlcpy(c->status, "skipped: cannot map", sizeof c->status); return; }
    /* while the ROM decodes, BAR0 is not touched (the decoder may be shared) */
    pci_write32(p, 0x30, base | 1);
    read_rom(c, rd_mem, m, MIN(size, NV_ROM_MAX));
    pci_write32(p, 0x30, orig);
    size_t l = strlen(c->status);
    snprintf(c->status + l, sizeof c->status - l, " [at %08x, %u KiB]", base, size >> 10);
}

static void rom_shadow(nv_device_t *d) {
    nv_rom_copy_t *c = &d->rom[NV_SRC_SHADOW];
    volatile u8 *m = (volatile u8 *)(uptr)0xC0000;
    if (mmio_read8(m) != 0x55 || mmio_read8(m + 1) != 0xAA) { strlcpy(c->status, "not available: nothing at C0000", sizeof c->status); return; }
    read_rom(c, rd_mem, (void *)m, 0x20000);
    if (c->score && c->data) {
        nv_rom_t s;
        nv_rom_scan(c->data, c->size, &s);
        if (s.img[0].device != d->pci->device) {
            c->score = 0;
            snprintf(c->status, sizeof c->status, "belongs to another card (PCI device %04x)", s.img[0].device);
        }
    }
}

static void read_vbios(nv_device_t *d) {
    static const char *names[NV_SRC_COUNT] = { "PRAMIN", "PROM", "PCI ROM BAR", "ACPI _ROM", "legacy shadow C0000" };
    for (int i = 0; i < NV_SRC_COUNT; i++) d->rom[i].source = names[i];
    rom_pramin(d);
    rom_prom(d);
    rom_bar(d);
    strlcpy(d->rom[NV_SRC_ACPI].status, "not available: _ROM is an ACPI method and Halcyon has no AML interpreter",
            sizeof d->rom[NV_SRC_ACPI].status);
    if (d->index == 0) rom_shadow(d);
    else strlcpy(d->rom[NV_SRC_SHADOW].status, "not available: only the first card's is there", sizeof d->rom[NV_SRC_SHADOW].status);
    d->rom_best = -1;
    for (int i = 0; i < NV_SRC_COUNT; i++)
        if (d->rom[i].score && (d->rom_best < 0 || d->rom[i].score > d->rom[d->rom_best].score)) d->rom_best = i;
}

/* ---------------- connectors and EDIDs ---------------- */

static void connectors(nv_device_t *d) {
    nv_bios_t *b = d->bios;
    if (!b) return;
    for (int i = 0; i < b->nconn && d->nconn < NV_CONN_MAX; i++) {
        if (b->conn[i].type == 0xFF) continue;
        nv_connector_t *c = &d->conn[d->nconn];
        memset(c, 0, sizeof *c);
        c->index = i;
        c->type = b->conn[i].type;
        for (int k = 0; k < b->noutputs && c->nout < 6; k++)
            if (b->out[k].connector == i) c->out[c->nout++] = (u8)k;
        if (!c->nout) continue;                     /* listed but wired to nothing */
        c->hpd = nv_hotplug(d, i);
        d->nconn++;
    }
}

static void read_edids(nv_device_t *d) {
    u64 t0 = time_us();
    for (int i = 0; i < d->nconn; i++) {
        nv_connector_t *c = &d->conn[i];
        if (d->timeouts >= 6 || time_us() - t0 > 6000000) {
            strlcpy(c->note, "not read: too many timeouts on this card, gave up", sizeof c->note);
            continue;
        }
        if (!c->edid) c->edid = kzalloc(NV_EDID_MAX);
        if (!c->edid) break;
        /* DisplayPort first (AUX finds out quickly whether a sink is there),
         * then the I2C outputs (DVI, HDMI, VGA, a DP++ adapter); a pad
         * whose DP sink answered is never switched to I2C */
        int tried = 0, r = E_NODEV;
        u32 dp_sink = 0;
        for (int pass = 0; pass < 2 && c->edid_len <= 0; pass++) {
            for (int k = 0; k < c->nout && c->edid_len <= 0; k++) {
                const nv_dcb_output_t *o = &d->bios->out[c->out[k]];
                if ((pass == 0) != (o->type == NV_OUT_DP)) continue;
                if (o->type == NV_OUT_TV) continue;
                if (o->i2c >= d->bios->nccb) continue;
                int dup = 0;                        /* same port as an output already tried */
                for (int j = 0; j < k; j++) {
                    const nv_dcb_output_t *q = &d->bios->out[c->out[j]];
                    if (q->i2c == o->i2c && (q->type == NV_OUT_DP) == (o->type == NV_OUT_DP)) dup = 1;
                }
                if (dup) continue;
                if (o->type != NV_OUT_DP && o->i2c < 32 && (dp_sink & (1u << o->i2c))) continue;
                tried++;
                if (o->type == NV_OUT_DP) {
                    u8 rev;
                    if (nv_aux_read(d, o->i2c, 0x000, &rev, 1) == 1 && o->i2c < 32) dp_sink |= 1u << o->i2c;
                }
                char via[48];
                r = nv_read_edid(d, o, c->edid, NV_EDID_MAX, via, sizeof via);
                if (r > 0) { c->edid_len = r; strlcpy(c->via, via, sizeof c->via); }
                else if (r != E_NODEV && via[0]) strlcpy(c->note, via, sizeof c->note);
            }
        }
        if (c->edid_len > 0) {
            edid_parse(c->edid, c->edid_len, &c->mon);
            c->note[0] = 0;
        } else if (!c->note[0]) {
            strlcpy(c->note, !tried ? "no I2C or AUX port this driver reads" : c->hpd == 0 ? "nothing plugged in" :
                    "no monitor answered", sizeof c->note);
        }
    }
}

/* ---------------- probe ---------------- */

/* An AUX transaction or a pad switch latches status bits in the I2C / AUX
 * interrupt unit. With no driver to acknowledge them, they must not reach
 * the host: fine if the PCI function cannot interrupt (INTx disabled, no
 * MSI), if the GPU's interrupts are off (PMC_INTR_EN 0x000140 = 0) or if
 * the AUX unit's enables are all off. */
static int irq_risk(nv_device_t *d) {
    pci_dev_t *p = d->pci;
    int msi = pci_find_cap(p, 0x05), msix = pci_find_cap(p, 0x11);
    int msi_on = (msi && (pci_read16(p, (u8)(msi + 2)) & 1)) || (msix && (pci_read16(p, (u8)(msix + 2)) & 0x8000));
    if ((pci_read16(p, 0x04) & 0x400) && !msi_on) return 0;
    d->intr_en = nv_rd32(d, 0x000140);
    if (!d->intr_en) return 0;
    if (d->chip.i2c >= NV_I2C_G94) {
        u32 en = nv_rd32(d, d->chip.chipset >= 0xE0 ? 0x00DC68 : 0x00E068);
        if (!nv_bad_read(en) && !en) return 0;
    }
    return d->chip.i2c >= NV_I2C_G94;              /* (older chips have no AUX unit and no pads) */
}

static void link_info(nv_device_t *d) {
    pci_dev_t *p = d->pci;
    int cap = pci_find_cap(p, 0x10);
    if (cap) {
        u32 lcap = pci_read32(p, (u8)(cap + 0x0C));
        u16 lsta = pci_read16(p, (u8)(cap + 0x12));
        d->link.pcie = 1;
        d->link.speed = lsta & 0xF;
        d->link.width = (lsta >> 4) & 0x3F;
        d->link.max_speed = lcap & 0xF;
        d->link.max_width = (lcap >> 4) & 0x3F;
        return;
    }
    cap = pci_find_cap(p, 0x02);
    if (cap) {
        d->link.pcie = 2;
        d->link.agp_status = pci_read32(p, (u8)(cap + 4));
        d->link.agp_cmd = pci_read32(p, (u8)(cap + 8));
    }
}

static void card_name(nv_device_t *d) {
    const char *n = nv_card_name(d->pci->device);
    if (n) snprintf(d->name, sizeof d->name, "NVIDIA %s", n);
    else snprintf(d->name, sizeof d->name, "NVIDIA %s (%s)", d->chip.series, d->chip.codename);
}

static void finish(nv_device_t *d) {
    if (d->rom_best >= 0) {
        d->bios = kzalloc(sizeof *d->bios);
        nv_rom_copy_t *c = &d->rom[d->rom_best];
        if (d->bios && !nv_bios_parse(c->data, c->size, d->chip.chipset, d->bios)) { kfree(d->bios); d->bios = NULL; }
    }
    connectors(d);
}

static void probe(pci_dev_t *p) {
    u64 t0 = time_us();
    if (ngpus >= NV_MAX_GPUS) return;
    pci_bar_t *b0 = &p->bar[0];
    if (b0->is_io || !b0->addr || b0->size < (16u << 20)) {
        kprintf("nv: %02x:%02x.%x %04x: BAR0 missing or too small (%llu KiB), skipped\n", p->bus, p->dev, p->func, p->device,
                (unsigned long long)(b0->size >> 10));
        return;
    }
    if (!(pci_read16(p, 0x04) & 0x2)) {
        kprintf("nv: %02x:%02x.%x: memory decoding off (the firmware did not set this card up), skipped\n", p->bus, p->dev, p->func);
        return;
    }
    nv_device_t *d = kzalloc(sizeof *d);
    if (!d) return;
    d->pci = p;
    d->index = ngpus;
    d->mmio_phys = b0->addr;
    d->mmio_size = b0->size;
    d->regs = (nv_regs_t){ d, r_rd32, r_wr32, r_delay, r_now };
    if (b0->addr + b0->size <= 0x100000000ull) d->uncached = mm_set_uncached(b0->addr, b0->size);
    d->mmio = pci_map_bar(p, 0);
    if (!d->mmio) { kprintf("nv: BAR0 at %llx cannot be mapped\n", (unsigned long long)b0->addr); kfree(d); return; }
    u32 boot0 = nv_rd32(d, 0x000000), boot1 = nv_rd32(d, 0x000004);
    if (nv_bad_read(boot0) || !boot0) {
        kprintf("nv: %02x:%02x.%x: PMC_BOOT_0 reads %08x, card not responding; skipped\n", p->bus, p->dev, p->func, boot0);
        kfree(d);
        return;
    }
    if (boot1 & 0x01000001u) {
        kprintf("nv: %02x:%02x.%x: registers in big-endian mode (PMC_BOOT_1 %08x); skipped\n", p->bus, p->dev, p->func, boot1);
        kfree(d);
        return;
    }
    u32 boot42 = 0;
    if (((boot0 >> 20) & 0x1FF) >= 0xC0 && (boot0 & 0x1F000000u)) boot42 = nv_rd32(d, 0x000A00);
    if (nv_chip_identify(boot0, boot42, &d->chip)) {
        kprintf("nv: %02x:%02x.%x: PMC_BOOT_0 %08x is no NVIDIA chip ID; skipped\n", p->bus, p->dev, p->func, boot0);
        kfree(d);
        return;
    }
    gpus[ngpus++] = d;                              /* (pci->driver stays: 10a drives nothing) */
    card_name(d);
    d->pmc_enable = nv_rd32(d, 0x000200);
    d->bar1_size = p->bar[1].is_io ? 0 : p->bar[1].size;
    link_info(d);
    d->rom_best = -1;
    if (!d->chip.in_range) {
        kprintf("nv%d: %s, %s (PMC_BOOT_0 %08x): %s\n", d->index, d->name, d->chip.codename, boot0,
                d->chip.note ? d->chip.note : "not supported");
        return;
    }
    d->vram = nv_vram_probe(&d->chip, &d->regs, d->vram_how, sizeof d->vram_how);
    /* cross-check with BAR1: up to Curie it maps all of the VRAM (256 MiB at
     * most), from Tesla on it is a window into it */
    size_t hl = strlen(d->vram_how);
    if (d->chip.family <= NV_FAM_CURIE && !d->chip.igp && d->bar1_size) {
        if (!d->vram && d->bar1_size <= (512ull << 20)) {
            d->vram = d->bar1_size;
            snprintf(d->vram_how + hl, sizeof d->vram_how - hl, "; taken from the BAR1 size");
        } else if (d->vram && d->vram != d->bar1_size && d->vram <= (256ull << 20)) {
            snprintf(d->vram_how + hl, sizeof d->vram_how - hl, "; BAR1 is %llu MiB", (unsigned long long)(d->bar1_size >> 20));
        }
    } else if (d->vram && d->bar1_size > d->vram) {
        snprintf(d->vram_how + hl, sizeof d->vram_how - hl, "; BAR1 (%llu MiB) is larger: suspect", (unsigned long long)(d->bar1_size >> 20));
    }
    read_vbios(d);
    finish(d);
    d->i2c_off = !settings_get_int("nv.i2c", 1);
    d->irq_on = irq_risk(d);
    if (d->irq_on) {
        for (int i = 0; i < d->nconn; i++)
            snprintf(d->conn[i].note, sizeof d->conn[i].note, "not read: the GPU's interrupts are on (0x000140 = %08x)", d->intr_en);
    } else if (!d->i2c_off && d->chip.disp != NV_DISP_NONE) {
        read_edids(d);
    }
    d->init_ms = (u32)((time_us() - t0) / 1000);
    nv_log_summary(d);
}

/* ---------------- QEMU test hook: a simulated card ---------------- */

/* fw_cfg opt/halcyon/nvfake = "boot0,pci-device" (e.g. "0x134000a1,0x1b81"),
 * opt/halcyon/nvrom = a video BIOS image (tests/host/nvtest --write-rom),
 * opt/halcyon/nvedid = an EDID for the first digital connector. There are
 * no registers: everything else reads as absent. */
static pci_dev_t fake_pci;

static void fake_card(void) {
    char spec[64];
    if (!qemu_fw_file("opt/halcyon/nvfake", spec, sizeof spec) || !spec[0] || ngpus >= NV_MAX_GPUS) return;
    char *comma = strchr(spec, ',');
    u32 boot0 = (u32)strtol(spec, NULL, 0);
    memset(&fake_pci, 0, sizeof fake_pci);
    fake_pci.bus = 0xFF;
    fake_pci.vendor = 0x10DE;
    fake_pci.device = comma ? (u16)strtol(comma + 1, NULL, 0) : 0x1B81;
    fake_pci.class_code = 3;
    nv_device_t *d = kzalloc(sizeof *d);
    if (!d) return;
    d->pci = &fake_pci;
    d->index = ngpus;
    d->fake = 1;
    d->regs = (nv_regs_t){ d, r_rd32, r_wr32, r_delay, r_now };
    if (nv_chip_identify(boot0, 0, &d->chip)) { kprintf("nv: test hook: bad PMC_BOOT_0 '%s'\n", spec); kfree(d); return; }
    gpus[ngpus++] = d;
    card_name(d);
    static const char *names[NV_SRC_COUNT] = { "PRAMIN", "PROM", "PCI ROM BAR", "ACPI _ROM", "legacy shadow C0000" };
    for (int i = 0; i < NV_SRC_COUNT; i++) {
        d->rom[i].source = names[i];
        strlcpy(d->rom[i].status, "not available (simulated card)", sizeof d->rom[i].status);
    }
    d->rom_best = -1;
    u8 *rom = kmalloc(NV_ROM_MAX);
    int n = rom ? qemu_fw_blob("opt/halcyon/nvrom", rom, NV_ROM_MAX) : -1;
    if (n > 0) {
        nv_rom_copy_t *c = &d->rom[NV_SRC_PROM];
        c->data = rom;
        c->size = (u32)MIN(n, (int)NV_ROM_MAX);
        nv_rom_t s;
        c->score = nv_rom_scan(c->data, c->size, &s);
        snprintf(c->status, sizeof c->status, "%d image%s, %u KiB (from the QEMU test hook)", s.nimages, s.nimages == 1 ? "" : "s",
                 c->size >> 10);
        if (c->score) d->rom_best = NV_SRC_PROM;
    } else {
        kfree(rom);
    }
    finish(d);
    u8 *e = kzalloc(NV_EDID_MAX);
    n = e ? qemu_fw_blob("opt/halcyon/nvedid", e, NV_EDID_MAX) : -1;
    for (int i = 0; i < d->nconn && n >= 128; i++) {
        nv_connector_t *c = &d->conn[i];
        const nv_dcb_output_t *o = &d->bios->out[c->out[0]];
        if (o->type != NV_OUT_DP && o->type != NV_OUT_TMDS) continue;
        c->edid = e;
        c->edid_len = MIN(n, (int)NV_EDID_MAX) & ~127;
        edid_parse(e, c->edid_len, &c->mon);
        strlcpy(c->via, "QEMU test hook", sizeof c->via);
        e = NULL;
        break;
    }
    kfree(e);
    for (int i = 0; i < d->nconn; i++)
        if (!d->conn[i].edid_len) strlcpy(d->conn[i].note, "simulated card: no I2C", sizeof d->conn[i].note);
    nv_log_summary(d);
}

void nv_init(void) {
    if (g_bootinfo.boot_flags & BOOTF_NO_DISPLAY) { kprintf("nv: skipped (display drivers off: F8 Safe Mode)\n"); return; }
    if (settings_get_int("nv.disable", 0)) { kprintf("nv: skipped (nv.disable=1 in SETTINGS.INI)\n"); return; }
    int seen = 0;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (p->vendor != 0x10DE || p->class_code != 0x03) continue;
        seen++;
        probe(p);
    }
    fake_card();
    if (!seen && !ngpus) kprintf("nv: no NVIDIA graphics card\n");
    /* M10b: the modesetting driver registers with the display core now that
     * the cards are known (display_init ran before the settings were loaded) */
    nv_display_probe();
}
