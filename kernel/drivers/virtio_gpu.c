/* Halcyon OS - virtio GPU (QEMU -device virtio-gpu-pci,max_outputs=N, and
 * -vga virtio, which is the same device with a VGA part for the firmware).
 *
 * Reference: Virtual I/O Device (VIRTIO) Version 1.2 (OASIS), sections 2
 * (split virtqueues), 4.1 (PCI transport) and 5.7 (GPU device).
 *
 * 2D only (no virgl, no blob resources). One head per scanout (up to 16,
 * QEMU's max_outputs), connector "Virtual-<n>" each. A scanout buffer is a
 * resource (B8G8R8X8) whose backing is guest RAM (one contiguous range);
 * the core writes the pixels and calls flush, which sends
 * TRANSFER_TO_HOST_2D (and RESOURCE_FLUSH when the resource is on screen)
 * for the damaged rectangle. Page flips: the core fills the back buffer and
 * flips; the device has no vertical blank, so a timer at the mode's refresh
 * rate stands in for it: at the next "vblank" poll() sends SET_SCANOUT to
 * the new resource and RESOURCE_FLUSH, and tells the core the flip is done.
 *
 * The hardware cursor goes through the cursor queue (UPDATE_CURSOR with a
 * 64 x 64 B8G8R8A8 resource, MOVE_CURSOR), fire and forget. Connectors:
 * GET_DISPLAY_INFO (a scanout's "enabled" bit is the monitor; its size the
 * window the host shows) and GET_EDID (VIRTIO_GPU_F_EDID). Hotplug: the
 * configuration change interrupt sets VIRTIO_GPU_EVENT_DISPLAY in
 * events_read (resizing QEMU's window, a VNC client's SetDesktopSize,
 * enabling an output); poll() checks it on every wake-up and every 500 ms
 * without interrupts. Control commands are synchronous: queued and the used
 * ring polled (QEMU answers at once; half a second is allowed, after which
 * the queue is not used again). Interrupts (MSI-X, or INTx with the ISR
 * read to clear) only wake the GUI thread. */
#include "display.h"
#include "pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

#define F_EDID            (1ull << 1)
#define F_VERSION_1       (1ull << 32)
#define F_ACCESS_PLATFORM (1ull << 33)

#define S_ACK          0x01
#define S_DRIVER       0x02
#define S_DRIVER_OK    0x04
#define S_FEATURES_OK  0x08
#define S_FAILED       0x80

#define C_DFSEL   0x00
#define C_DF      0x04
#define C_GFSEL   0x08
#define C_GF      0x0C
#define C_MSIX    0x10
#define C_NUMQ    0x12
#define C_STATUS  0x14
#define C_GEN     0x15
#define C_QSEL    0x16
#define C_QSIZE   0x18
#define C_QMSIX   0x1A
#define C_QEN     0x1C
#define C_QNOFF   0x1E
#define C_QDESC   0x20
#define C_QAVAIL  0x28
#define C_QUSED   0x30
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4
#define NO_VECTOR 0xFFFF
#define D_NEXT  1
#define D_WRITE 2
#define USED_NO_NOTIFY 1

/* GPU commands (5.7.6) */
#define CMD_GET_DISPLAY_INFO     0x0100
#define CMD_RESOURCE_CREATE_2D   0x0101
#define CMD_RESOURCE_UNREF       0x0102
#define CMD_SET_SCANOUT          0x0103
#define CMD_RESOURCE_FLUSH       0x0104
#define CMD_TRANSFER_TO_HOST_2D  0x0105
#define CMD_ATTACH_BACKING       0x0106
#define CMD_DETACH_BACKING       0x0107
#define CMD_GET_EDID             0x010A
#define CMD_UPDATE_CURSOR        0x0300
#define CMD_MOVE_CURSOR          0x0301
#define RESP_OK_NODATA           0x1100
#define RESP_OK_DISPLAY_INFO     0x1101
#define RESP_OK_EDID             0x1104
#define FMT_B8G8R8A8             1
#define FMT_B8G8R8X8             2
#define EVENT_DISPLAY            1
#define MAX_SCANOUTS             16

#define CTRLQ 0
#define CURSORQ 1
#define QSIZE 64
#define NCUR 16                      /* cursor commands in flight */
#define SLOT 128                     /* request / response slot */
#define NSLOT 16                     /* requests in one batch */

typedef struct vdesc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed)) vdesc_t;
typedef struct vq {
    u16 index, size;
    vdesc_t *desc;
    volatile u16 *avail;
    volatile u8 *used;
    u16 avail_idx, used_idx;
    volatile u16 *notify;
    void *mem;
} vq_t;

typedef struct { u32 type, flags; u64 fence; u32 ctx; u8 ring, pad[3]; } __attribute__((packed)) hdr_t;
typedef struct { u32 x, y, w, h; } __attribute__((packed)) vrect_t;
typedef struct { hdr_t h; struct { vrect_t r; u32 enabled, flags; } pm[MAX_SCANOUTS]; } __attribute__((packed)) display_info_t;
typedef struct { hdr_t h; u32 res, format, w, hgt; } __attribute__((packed)) create_2d_t;
typedef struct { hdr_t h; u32 res, pad; } __attribute__((packed)) res_only_t;
typedef struct { hdr_t h; vrect_t r; u32 scanout, res; } __attribute__((packed)) set_scanout_t;
typedef struct { hdr_t h; vrect_t r; u32 res, pad; } __attribute__((packed)) flush_t;
typedef struct { hdr_t h; vrect_t r; u64 offset; u32 res, pad; } __attribute__((packed)) xfer_t;
typedef struct { hdr_t h; u32 res, nents; u64 addr; u32 len, pad; } __attribute__((packed)) backing_t;
typedef struct { hdr_t h; u32 scanout, pad; } __attribute__((packed)) get_edid_t;
typedef struct { hdr_t h; u32 size, pad; u8 edid[1024]; } __attribute__((packed)) edid_resp_t;
typedef struct { hdr_t h; u32 scanout, x, y, pad; u32 res, hot_x, hot_y, pad2; } __attribute__((packed)) cursor_t;

typedef struct vgpu {
    pci_dev_t *pci;
    volatile u8 *common, *isr, *devcfg, *notify_base;
    u32 notify_mult;
    u64 features;
    int msix;
    vq_t q[2];
    u8 *req, *resp;                  /* NSLOT slots each */
    u8 *big;                         /* display info / EDID responses */
    cursor_t *cur;                   /* NCUR cursor commands */
    u8 cur_busy[NCUR];
    int cur_inflight;
    u32 nscanouts;
    display_adapter_t *a;
    u32 next_res;
    u32 shown[MAX_SCANOUTS];         /* resource on each scanout */
    int sw[MAX_SCANOUTS], sh[MAX_SCANOUTS];
    /* flips: the resource to show at the next "vertical blank" */
    u32 flip_res[MAX_SCANOUTS];
    u64 flip_due[MAX_SCANOUTS];
    u64 vbl_t0[MAX_SCANOUTS];        /* time_ms of a vertical blank (the mode set) */
    u32 vbl_us[MAX_SCANOUTS];        /* refresh period */
    /* cursor */
    u32 cursor_res;
    u32 *cursor_px;
    int cx[MAX_SCANOUTS], cy[MAX_SCANOUTS], cvis[MAX_SCANOUTS];
    u64 last_cfg_ms;
    u32 cmds, errors;
    int dead;
    irq_handle_t irq;
    char irq_name[32];
} vgpu_t;

/* ---------------- transport ---------------- */
static u8 get_status(vgpu_t *v) { return mmio_read8(v->common + C_STATUS); }
static void set_status(vgpu_t *v, u8 s) { mmio_write8(v->common + C_STATUS, s); }
static u32 cfg32(vgpu_t *v, u32 off) {
    for (int tries = 0; tries < 8; tries++) {
        u8 gen = mmio_read8(v->common + C_GEN);
        u32 x = mmio_read32(v->devcfg + off);
        if (mmio_read8(v->common + C_GEN) == gen) return x;
    }
    return 0;
}
static int vg_ack(void *ctx) { vgpu_t *v = ctx; return (mmio_read8(v->isr) & 3) != 0; }
static u16 used_idx(vq_t *q) { return mmio_read16((volatile void *)(q->used + 2)); }
static void avail_push(vq_t *q, u16 head) {
    q->avail[2 + q->avail_idx % q->size] = head;
    wmb();
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
}
static void notify(vq_t *q) {
    mb();
    if (mmio_read16((volatile void *)q->used) & USED_NO_NOTIFY) return;
    mmio_write16(q->notify, q->index);
}
static int vq_next_used(vq_t *q, u32 *len) {
    if (q->used_idx == used_idx(q)) return -1;
    rmb();
    volatile u8 *e = q->used + 4 + 8u * (q->used_idx % q->size);
    u32 id = mmio_read32(e);
    if (len) *len = mmio_read32(e + 4);
    q->used_idx++;
    return id < q->size ? (int)id : -2;
}

static int setup_queue(vgpu_t *v, u16 index, u16 want) {
    vq_t *q = &v->q[index];
    mmio_write16(v->common + C_QSEL, index);
    u16 max = mmio_read16(v->common + C_QSIZE);
    if (!max || mmio_read16(v->common + C_QEN)) return E_IO;
    u16 size = 1;
    while (size * 2 <= max && size < want) size *= 2;
    u32 avail_off = 16u * size, used_off = (avail_off + 6 + 2u * size + 3) & ~3u;
    q->mem = dma_alloc(used_off + 6 + 8u * size, 4096);
    if (!q->mem) return E_NOMEM;
    q->index = index;
    q->size = size;
    q->desc = q->mem;
    q->avail = (volatile u16 *)((u8 *)q->mem + avail_off);
    q->used = (volatile u8 *)q->mem + used_off;
    mmio_write16(v->common + C_QSIZE, size);
    mmio_write64(v->common + C_QDESC, virt_to_phys(q->desc));
    mmio_write64(v->common + C_QAVAIL, virt_to_phys((const void *)q->avail));
    mmio_write64(v->common + C_QUSED, virt_to_phys((const void *)q->used));
    if (v->msix) {
        mmio_write16(v->common + C_QMSIX, 0);
        if (mmio_read16(v->common + C_QMSIX) == NO_VECTOR) kprintf("virtio-gpu: queue %u: no MSI-X vector, polled\n", index);
    }
    q->notify = (volatile u16 *)(v->notify_base + (u32)mmio_read16(v->common + C_QNOFF) * v->notify_mult);
    mmio_write16(v->common + C_QEN, 1);
    return 0;
}

/* ---------------- control commands ---------------- */
typedef struct { void *req; u32 reqlen; void *resp; u32 resplen; } cmd_t;

/* queue the commands and wait for every answer; returns the number that
 * answered OK (the response types in their headers) */
static int submit(vgpu_t *v, const cmd_t *c, int n) {
    if (v->dead || n <= 0) return 0;
    vq_t *q = &v->q[CTRLQ];
    for (int i = 0; i < n; i++) {
        memset(c[i].resp, 0, c[i].resplen);
        vdesc_t *d = &q->desc[2 * i];
        d[0].addr = virt_to_phys(c[i].req);
        d[0].len = c[i].reqlen;
        d[0].flags = D_NEXT;
        d[0].next = (u16)(2 * i + 1);
        d[1].addr = virt_to_phys(c[i].resp);
        d[1].len = c[i].resplen;
        d[1].flags = D_WRITE;
        d[1].next = 0;
        avail_push(q, (u16)(2 * i));
    }
    notify(q);
    v->cmds += (u32)n;
    int done = 0;
    u64 end = time_ms() + 500;
    while (done < n) {
        int id = vq_next_used(q, NULL);
        if (id >= 0) { done++; continue; }
        if (time_ms() > end) {
            kprintf("virtio-gpu: command %#x not answered: the device is not used any more\n", ((hdr_t *)c[0].req)->type);
            v->dead = 1;
            return 0;
        }
        cpu_relax();
    }
    rmb();
    int ok = 0;
    for (int i = 0; i < n; i++) {
        u32 t = ((hdr_t *)c[i].resp)->type;
        if (t >= RESP_OK_NODATA && t < 0x1200) ok++;
        else if (v->errors++ < 8) kprintf("virtio-gpu: command %#x failed (%#x)\n", ((hdr_t *)c[i].req)->type, t);
    }
    return ok;
}

static void *slot_req(vgpu_t *v, int i, u32 type) {
    u8 *r = v->req + i * SLOT;
    memset(r, 0, SLOT);
    ((hdr_t *)r)->type = type;
    return r;
}
static cmd_t mkcmd(vgpu_t *v, int i, u32 len) { return (cmd_t){ v->req + i * SLOT, len, v->resp + i * SLOT, sizeof(hdr_t) }; }

static int one(vgpu_t *v, u32 len) { cmd_t c = mkcmd(v, 0, len); return submit(v, &c, 1) == 1 ? 0 : E_IO; }

static int res_create(vgpu_t *v, u32 res, u32 fmt, int w, int h, void *px, u32 bytes) {
    create_2d_t *c = slot_req(v, 0, CMD_RESOURCE_CREATE_2D);
    c->res = res; c->format = fmt; c->w = (u32)w; c->hgt = (u32)h;
    backing_t *b = slot_req(v, 1, CMD_ATTACH_BACKING);
    b->res = res; b->nents = 1; b->addr = virt_to_phys(px); b->len = bytes;
    cmd_t cm[2] = { mkcmd(v, 0, sizeof *c), mkcmd(v, 1, sizeof *b) };
    return submit(v, cm, 2) == 2 ? 0 : E_IO;
}

static void res_destroy(vgpu_t *v, u32 res) {
    res_only_t *d = slot_req(v, 0, CMD_DETACH_BACKING);
    d->res = res;
    res_only_t *u = slot_req(v, 1, CMD_RESOURCE_UNREF);
    u->res = res;
    cmd_t cm[2] = { mkcmd(v, 0, sizeof *d), mkcmd(v, 1, sizeof *u) };
    submit(v, cm, 2);
}

/* slot i: transfer x, y, w, h of res (pitch bytes per line) */
static void xfer_cmd(vgpu_t *v, int i, u32 res, u32 pitch, int x, int y, int w, int h) {
    xfer_t *t = slot_req(v, i, CMD_TRANSFER_TO_HOST_2D);
    t->r = (vrect_t){ (u32)x, (u32)y, (u32)w, (u32)h };
    t->offset = (u64)y * pitch + (u64)x * 4;
    t->res = res;
}
static void flush_cmd(vgpu_t *v, int i, u32 res, int x, int y, int w, int h) {
    flush_t *f = slot_req(v, i, CMD_RESOURCE_FLUSH);
    f->r = (vrect_t){ (u32)x, (u32)y, (u32)w, (u32)h };
    f->res = res;
}
static void scanout_cmd(vgpu_t *v, int i, u32 scanout, u32 res, int w, int h) {
    set_scanout_t *s = slot_req(v, i, CMD_SET_SCANOUT);
    s->r = (vrect_t){ 0, 0, (u32)w, (u32)h };
    s->scanout = scanout;
    s->res = res;
}

/* ---------------- display_ops ---------------- */
static void vg_detect(display_adapter_t *a) {
    vgpu_t *v = a->priv;
    hdr_t *r = slot_req(v, 0, CMD_GET_DISPLAY_INFO);
    cmd_t c = { r, sizeof *r, v->big, sizeof(display_info_t) };
    display_info_t info;
    memset(&info, 0, sizeof info);
    if (submit(v, &c, 1) == 1) memcpy(&info, v->big, sizeof info);
    for (int k = 0; k < a->nconns; k++) {
        display_connector_t *cn = &a->conns[k];
        int i = k;
        u8 edid[DISPLAY_EDID_MAX];
        int elen = 0;
        if (v->features & F_EDID) {
            get_edid_t *g = slot_req(v, 0, CMD_GET_EDID);
            g->scanout = (u32)i;
            cmd_t ce = { g, sizeof *g, v->big, sizeof(edid_resp_t) };
            if (submit(v, &ce, 1) == 1) {
                edid_resp_t *er = (edid_resp_t *)v->big;
                elen = (int)MIN(er->size, (u32)DISPLAY_EDID_MAX);
                memcpy(edid, er->edid, (size_t)elen);
            }
        }
        int on = info.pm[i].enabled != 0;
        display_connector_update(cn, on ? CONN_CONNECTED : CONN_DISCONNECTED, elen ? edid : NULL, elen,
                                 on ? (int)info.pm[i].r.w : 0, on ? (int)info.pm[i].r.h : 0);
    }
}

static int vg_fb_alloc(display_adapter_t *a, int w, int h, display_fb_t *fb) {
    vgpu_t *v = a->priv;
    u32 pitch = (u32)w * 4, bytes = (u32)ALIGN_UP((u64)pitch * h, 4096);
    if (mm_heap_free() < (u64)bytes + (16u << 20)) return E_NOMEM;
    void *px = kmalloc_aligned(bytes, 4096);
    if (!px) return E_NOMEM;
    u32 res = v->next_res++;
    if (res_create(v, res, FMT_B8G8R8X8, w, h, px, bytes)) { kfree(px); return E_IO; }
    memset(fb, 0, sizeof *fb);
    fb->px = px;
    fb->w = w;
    fb->h = h;
    fb->pitch = pitch;
    fb->bpp = 32;
    fb->rpos = 16; fb->gpos = 8; fb->bpos = 0;
    fb->handle = res;
    return 0;
}

static void vg_fb_free(display_adapter_t *a, display_fb_t *fb) {
    vgpu_t *v = a->priv;
    if (!fb->handle) return;
    for (u32 s = 0; s < v->nscanouts; s++) if (v->flip_res[s] == fb->handle) v->flip_res[s] = 0;
    res_destroy(v, fb->handle);
    kfree(fb->px);
    memset(fb, 0, sizeof *fb);
}

static int vg_set_mode(display_head_t *h, display_connector_t *c, const display_mode_t *m, display_fb_t *fb) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    if (v->dead) return E_IO;
    v->flip_res[s] = 0;
    if (!m) {
        scanout_cmd(v, 0, s, 0, 0, 0);
        one(v, sizeof(set_scanout_t));
        v->shown[s] = 0;
        return 0;
    }
    scanout_cmd(v, 0, s, fb->handle, m->w, m->h);
    xfer_cmd(v, 1, fb->handle, fb->pitch, 0, 0, m->w, m->h);
    flush_cmd(v, 2, fb->handle, 0, 0, m->w, m->h);
    cmd_t cm[3] = { mkcmd(v, 0, sizeof(set_scanout_t)), mkcmd(v, 1, sizeof(xfer_t)), mkcmd(v, 2, sizeof(flush_t)) };
    if (submit(v, cm, 3) != 3) return E_IO;
    v->shown[s] = fb->handle;
    v->sw[s] = m->w; v->sh[s] = m->h;
    v->vbl_t0[s] = time_ms();
    v->vbl_us[s] = m->hz > 0 ? (u32)(1000000 / m->hz) : 16667;
    return 0;
}

static void vg_flush(display_head_t *h, display_fb_t *fb, int x, int y, int w, int hgt) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    xfer_cmd(v, 0, fb->handle, fb->pitch, x, y, w, hgt);
    int n = 1;
    if (v->shown[s] == fb->handle) { flush_cmd(v, 1, fb->handle, x, y, w, hgt); n = 2; }
    cmd_t cm[2] = { mkcmd(v, 0, sizeof(xfer_t)), mkcmd(v, 1, sizeof(flush_t)) };
    submit(v, cm, n);
}

/* the next emulated vertical blank after now (ms) */
static u64 next_vblank(vgpu_t *v, u32 s) {
    u64 now = time_ms(), t0 = v->vbl_t0[s], per = v->vbl_us[s] ? v->vbl_us[s] : 16667;
    u64 k = ((now - t0) * 1000 + per) / per;           /* frames since t0, rounded up */
    return t0 + (k * per + 999) / 1000;
}

static int vg_flip(display_head_t *h, display_fb_t *fb) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    if (v->dead) return E_IO;
    v->flip_res[s] = fb->handle;
    v->flip_due[s] = next_vblank(v, s);
    idle_wake_at(v->flip_due[s]);
    return 0;
}

static int vg_wait_vblank(display_head_t *h) {
    vgpu_t *v = h->adapter->priv;
    u64 due = next_vblank(v, (u32)h->index);
    while (time_ms() < due) idle_wait(irq_generation(), due);
    return 0;
}

static void cursor_reap(vgpu_t *v) {
    vq_t *q = &v->q[CURSORQ];
    for (int budget = 0; budget < q->size; budget++) {
        int id = vq_next_used(q, NULL);
        if (id == -1) break;
        if (id >= 0 && id < NCUR && v->cur_busy[id]) { v->cur_busy[id] = 0; v->cur_inflight--; }
    }
}

static void cursor_cmd(vgpu_t *v, u32 type, u32 s, int x, int y, u32 res, int hx, int hy) {
    if (v->dead) return;
    cursor_reap(v);
    int slot = -1;
    for (u64 end = time_ms() + 50; slot < 0; ) {
        for (int i = 0; i < NCUR; i++) if (!v->cur_busy[i]) { slot = i; break; }
        if (slot >= 0) break;
        if (time_ms() > end) return;                   /* the device is slow: drop this move */
        cpu_relax();
        cursor_reap(v);
    }
    cursor_t *c = &v->cur[slot];
    memset(c, 0, sizeof *c);
    c->h.type = type;
    c->scanout = s;
    c->x = (u32)(x < 0 ? 0 : x);
    c->y = (u32)(y < 0 ? 0 : y);
    c->res = res;
    c->hot_x = (u32)hx;
    c->hot_y = (u32)hy;
    vq_t *q = &v->q[CURSORQ];
    q->desc[slot].addr = virt_to_phys(c);
    q->desc[slot].len = sizeof *c;
    q->desc[slot].flags = 0;
    q->desc[slot].next = 0;
    v->cur_busy[slot] = 1;
    v->cur_inflight++;
    avail_push(q, (u16)slot);
    notify(q);
}

static int vg_cursor_image(display_head_t *h, const u32 *argb, int size, int hot_x, int hot_y) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    if (size != 64 || v->dead) return E_INVAL;
    if (!v->cursor_res) {
        v->cursor_px = kmalloc_aligned(64 * 64 * 4, 4096);
        if (!v->cursor_px) return E_NOMEM;
        u32 res = v->next_res++;
        if (res_create(v, res, FMT_B8G8R8A8, 64, 64, v->cursor_px, 64 * 64 * 4)) { kfree(v->cursor_px); v->cursor_px = NULL; return E_IO; }
        v->cursor_res = res;
    }
    /* one image for every head: upload it when it changed */
    if (memcmp(v->cursor_px, argb, 64 * 64 * 4)) {
        memcpy(v->cursor_px, argb, 64 * 64 * 4);
        xfer_cmd(v, 0, v->cursor_res, 64 * 4, 0, 0, 64, 64);
        if (one(v, sizeof(xfer_t))) return E_IO;
    }
    cursor_cmd(v, CMD_UPDATE_CURSOR, s, v->cx[s], v->cy[s], v->cursor_res, hot_x, hot_y);
    if (!v->cvis[s]) cursor_cmd(v, CMD_MOVE_CURSOR, s, v->cx[s], v->cy[s], 0, 0, 0);
    return 0;
}

static void vg_cursor_move(display_head_t *h, int x, int y, int visible) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    v->cx[s] = x; v->cy[s] = y; v->cvis[s] = visible;
    cursor_cmd(v, CMD_MOVE_CURSOR, s, x, y, visible ? v->cursor_res : 0, 0, 0);
}

static void vg_poll(display_adapter_t *a) {
    vgpu_t *v = a->priv;
    if (v->dead) return;
    u64 now = time_ms();
    cursor_reap(v);
    /* flips due at this vertical blank */
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *h = &a->heads[k];
        u32 s = (u32)k, res = v->flip_res[s];
        if (!res || !h->flip_pending) continue;
        if (now < v->flip_due[s]) { idle_wake_at(v->flip_due[s]); continue; }
        scanout_cmd(v, 0, s, res, v->sw[s], v->sh[s]);
        flush_cmd(v, 1, res, 0, 0, v->sw[s], v->sh[s]);
        cmd_t cm[2] = { mkcmd(v, 0, sizeof(set_scanout_t)), mkcmd(v, 1, sizeof(flush_t)) };
        submit(v, cm, 2);
        v->shown[s] = res;
        v->flip_res[s] = 0;
        display_flip_done(h);
    }
    /* hotplug: the interrupt, or every 500 ms */
    int fired = irq_fired(&v->irq);
    if (fired || now - v->last_cfg_ms >= 500) {
        v->last_cfg_ms = now;
        u32 ev = cfg32(v, 0);
        if (ev & EVENT_DISPLAY) {
            mmio_write32(v->devcfg + 4, EVENT_DISPLAY);      /* events_clear */
            kprintf("virtio-gpu: display configuration changed%s\n", fired ? " (interrupt)" : " (polled)");
            if (fired) irq_note_work(&v->irq);
            display_hotplug(a);
        }
    }
}

static void vg_panic_flush(display_head_t *h, display_fb_t *fb) {
    vgpu_t *v = h->adapter->priv;
    u32 s = (u32)h->index;
    if (v->dead) return;
    if (v->shown[s] != fb->handle) {
        scanout_cmd(v, 0, s, fb->handle, fb->w, fb->h);
        one(v, sizeof(set_scanout_t));
        v->shown[s] = fb->handle;
    }
    vg_flush(h, fb, 0, 0, fb->w, fb->h);
}

static void vg_restore(display_adapter_t *a) {
    vgpu_t *v = a->priv;
    /* a reset turns the scanouts off: virtio-vga shows its VGA part (the
     * firmware's frame buffer) again */
    set_status(v, 0);
    for (int i = 0; i < 1000 && get_status(v); i++) udelay(100);
    v->dead = 1;
    kprintf("virtio-gpu: device reset (back to the firmware's frame buffer)\n");
}

static const display_ops_t vg_ops = {
    .restore = vg_restore,
    .detect = vg_detect,
    .fb_alloc = vg_fb_alloc,
    .fb_free = vg_fb_free,
    .set_mode = vg_set_mode,
    .flush = vg_flush,
    .flip = vg_flip,
    .wait_vblank = vg_wait_vblank,
    .cursor_image = vg_cursor_image,
    .cursor_move = vg_cursor_move,
    .poll = vg_poll,
    .panic_flush = vg_panic_flush,
};

/* ---------------- setup ---------------- */
static int modern_caps(vgpu_t *v) {
    pci_dev_t *pd = v->pci;
    if (!(pci_read16(pd, 0x06) & (1u << 4))) return 0;
    u8 off = pci_read8(pd, 0x34) & 0xFC;
    for (int n = 0; off >= 0x40 && n < 48; n++, off = pci_read8(pd, (u8)(off + 1)) & 0xFC) {
        if (pci_read8(pd, off) != 0x09) continue;
        u8 type = pci_read8(pd, (u8)(off + 3)), bar = pci_read8(pd, (u8)(off + 4));
        u32 o = pci_read32(pd, (u8)(off + 8)), len = pci_read32(pd, (u8)(off + 12));
        if (type < CAP_COMMON || type > CAP_DEVICE || bar > 5) continue;
        pci_bar_t *b = &pd->bar[bar];
        if (b->is_io || !b->addr || (u64)o + len > b->size) continue;
        volatile u8 *base = pci_map_bar(pd, bar);
        if (!base) continue;
        switch (type) {
        case CAP_COMMON: if (!v->common && len >= 0x38) v->common = base + o; break;
        case CAP_NOTIFY:
            if (!v->notify_base && len >= 2) { v->notify_base = base + o; v->notify_mult = pci_read32(pd, (u8)(off + 16)); }
            break;
        case CAP_ISR: if (!v->isr && len >= 1) v->isr = base + o; break;
        case CAP_DEVICE: if (!v->devcfg && len >= 16) v->devcfg = base + o; break;
        }
    }
    return v->common && v->notify_base && v->isr && v->devcfg;
}

/* the firmware frame buffer is in RAM (OVMF's GOP on a virtio GPU without
 * VGA): no display BAR holds it, so this device is the one that showed it */
static int fw_fb_in_ram(void) {
    u64 fb = g_bootinfo.fb_addr;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code != 0x03) continue;
        for (int b = 0; b < 6; b++)
            if (!d->bar[b].is_io && d->bar[b].addr && fb >= d->bar[b].addr && fb < d->bar[b].addr + d->bar[b].size) return 0;
    }
    return fb != 0;
}

static void vg_attach(pci_dev_t *pd) {
    vgpu_t *v = kzalloc(sizeof *v);
    if (!v) return;
    v->pci = pd;
    v->next_res = 1;
    pci_enable(pd);
    if (!modern_caps(v)) {
        kprintf("virtio-gpu: %02x:%02x.%x: no usable virtio capabilities\n", pd->bus, pd->dev, pd->func);
        kfree(v);
        return;
    }
    set_status(v, 0);
    for (int i = 0; i < 1000 && get_status(v); i++) udelay(100);
    set_status(v, S_ACK);
    set_status(v, S_ACK | S_DRIVER);
    mmio_write32(v->common + C_DFSEL, 0);
    u64 dev = mmio_read32(v->common + C_DF);
    mmio_write32(v->common + C_DFSEL, 1);
    dev |= (u64)mmio_read32(v->common + C_DF) << 32;
    v->features = dev & (F_VERSION_1 | F_ACCESS_PLATFORM | F_EDID);
    const char *fail = NULL;
    if (!(v->features & F_VERSION_1)) fail = "no VERSION_1 feature";
    else {
        mmio_write32(v->common + C_GFSEL, 0);
        mmio_write32(v->common + C_GF, (u32)v->features);
        mmio_write32(v->common + C_GFSEL, 1);
        mmio_write32(v->common + C_GF, (u32)(v->features >> 32));
        set_status(v, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(get_status(v) & S_FEATURES_OK)) fail = "features not accepted";
        else if (mmio_read16(v->common + C_NUMQ) < 2) fail = "fewer than 2 queues";
    }
    snprintf(v->irq_name, sizeof v->irq_name, "virtio-gpu %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    v->irq.name = v->irq_name;
    v->irq.pci = pd;
    v->irq.ack = vg_ack;
    v->irq.ctx = v;
    if (!fail) {
        v->msix = irq_attach(&v->irq) == IRQ_MSIX;
        if (v->msix) {
            mmio_write16(v->common + C_MSIX, 0);             /* configuration changes: vector 0 */
            (void)mmio_read16(v->common + C_MSIX);
        }
        v->req = dma_alloc(NSLOT * SLOT, 4096);
        v->resp = dma_alloc(NSLOT * SLOT, 4096);
        v->big = dma_alloc(4096, 4096);
        v->cur = dma_alloc(NCUR * sizeof(cursor_t), 64);
        if (!v->req || !v->resp || !v->big || !v->cur) fail = "out of memory";
        else if (setup_queue(v, CTRLQ, QSIZE) || setup_queue(v, CURSORQ, NCUR)) fail = "cannot set up the queues";
    }
    if (!fail) {
        set_status(v, get_status(v) | S_DRIVER_OK);
        v->nscanouts = cfg32(v, 8);
        if (!v->nscanouts || v->nscanouts > MAX_SCANOUTS) fail = "no scanouts";
    }
    display_adapter_t *a = NULL;
    if (!fail) {
        char name[64];
        snprintf(name, sizeof name, "Virtio GPU (%u output%s)", v->nscanouts, v->nscanouts == 1 ? "" : "s");
        a = display_register("virtio-gpu", name, &vg_ops, v, pd);
        if (!a) fail = "too many display adapters";
    }
    if (fail) {
        kprintf("virtio-gpu: %02x:%02x.%x: %s\n", pd->bus, pd->dev, pd->func, fail);
        set_status(v, S_FAILED);
        irq_detach(&v->irq);
        for (int i = 0; i < 2; i++) dma_free(v->q[i].mem);
        dma_free(v->req); dma_free(v->resp); dma_free(v->big); dma_free(v->cur);
        kfree(v);
        return;
    }
    v->a = a;
    a->live = 1;
    a->virtual_sizes = 1;
    a->max_w = 3840;
    a->max_h = 2160;
    a->vram = 128ull << 20;                  /* per head; QEMU's host memory limit is 256 MiB by default */
    a->owns_fw_fb = fw_fb_in_ram();
    int nh = (int)MIN(v->nscanouts, (u32)DISPLAY_MAX_HEADS);
    for (int i = 0; i < nh; i++) {
        display_add_head(a, HEAD_CAP_CURSOR | HEAD_CAP_FLIP | HEAD_CAP_VBLANK, 64);
        char cn[16];
        snprintf(cn, sizeof cn, "Virtual-%d", i + 1);
        display_add_connector(a, cn, CONN_VIRTUAL, 1u << i);
    }
    pd->driver = "virtio-gpu";
    v->last_cfg_ms = time_ms();
    /* a change before we looked is no hotplug: detect reads the state as it is */
    if (cfg32(v, 0) & EVENT_DISPLAY) mmio_write32(v->devcfg + 4, EVENT_DISPLAY);
    kprintf("virtio-gpu: %02x:%02x.%x: %u scanouts%s, %s, %s\n", pd->bus, pd->dev, pd->func, v->nscanouts,
            nh < (int)v->nscanouts ? " (some unused)" : "", v->features & F_EDID ? "EDID" : "no EDID",
            irq_kind_name(&v->irq));
}

void virtio_gpu_probe(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        /* 0x1050: virtio 1.0 GPU (device type 16): virtio-gpu-pci (class 03.80) and virtio-vga (03.00) */
        if (pd->vendor == 0x1AF4 && pd->device == 0x1050 && !pd->driver) vg_attach(pd);
    }
}
