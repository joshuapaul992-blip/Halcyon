/* Halcyon OS - virtio sound device (QEMU -device virtio-sound-pci).
 *
 * Reference: Virtual I/O Device (VIRTIO) Version 1.2 (OASIS), sections 2
 * (feature bits, split virtqueues), 4.1 (PCI transport) and 5.14 (sound
 * device).
 *
 * Modern (virtio 1.0+) PCI interface only: device 1AF4:1059 has no legacy
 * interface. Features: VERSION_1, ACCESS_PLATFORM (accepted: no IOMMU).
 * Queues: controlq (0), eventq (1) and txq (2); the rxq (3) is left disabled
 * (no capture). The driver queries the jacks, PCM streams and channel maps,
 * and plays through the first output stream that takes S16 stereo at
 * 48000 Hz (else 44100 Hz). Control requests are synchronous: the request
 * and its response are queued and the used ring is polled (the device
 * answers at once in QEMU; up to half a second is allowed).
 *
 * Playback: start sends PCM_SET_PARAMS (a buffer of NBUF periods of 10 ms),
 * PCM_PREPARE, queues NBUF transfer buffers (each: the virtio_snd_pcm_xfer
 * header and one period of samples, device-readable, then the
 * virtio_snd_pcm_status the device writes) and sends PCM_START. A buffer the
 * device hands back is a finished period: poll mixes the next period into it
 * and queues it again, so NBUF periods are always in flight. An underrun is
 * a PCM_XRUN event, or a poll that finds every buffer back after more time
 * than they hold (the device ran dry). stop sends PCM_STOP and PCM_RELEASE,
 * after which the device returns every buffer. The event queue is kept full
 * of buffers and drained on every poll.
 *
 * Interrupts: MSI-X (one vector for every queue and configuration changes)
 * or INTx (ISR read-to-clear); they only wake the audio thread. Tested in
 * QEMU 8.2 (MSI-X, INTx with the I/O APIC and the PIC, and polled). */
#include "sound/snddev.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* feature bits */
#define F_VERSION_1       (1ull << 32)
#define F_ACCESS_PLATFORM (1ull << 33)

/* device status */
#define S_ACK          0x01
#define S_DRIVER       0x02
#define S_DRIVER_OK    0x04
#define S_FEATURES_OK  0x08
#define S_NEEDS_RESET  0x40
#define S_FAILED       0x80

/* common configuration */
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

/* sound device: request codes, status, formats, rates (5.14.6) */
#define R_JACK_INFO       0x0001
#define R_PCM_INFO        0x0100
#define R_PCM_SET_PARAMS  0x0101
#define R_PCM_PREPARE     0x0102
#define R_PCM_RELEASE     0x0103
#define R_PCM_START       0x0104
#define R_PCM_STOP        0x0105
#define R_CHMAP_INFO      0x0200
#define EVT_JACK_CONNECTED    0x1000
#define EVT_JACK_DISCONNECTED 0x1001
#define EVT_PCM_PERIOD        0x1100
#define EVT_PCM_XRUN          0x1101
#define ST_OK             0x8000
#define ST_BAD_MSG        0x8001
#define ST_NOT_SUPP       0x8002
#define ST_IO_ERR         0x8003
#define DIR_OUTPUT        0
#define FMT_S16           5
#define RATE_44100        6
#define RATE_48000        7

#define CTRLQ 0
#define EVENTQ 1
#define TXQ 2

#define NBUF     4                  /* periods in flight */
#define NEVT     8                  /* event buffers */
#define MAXSTREAMS 16
#define MAXJACKS 16
#define MAXCHMAPS 16

typedef struct vdesc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed)) vdesc_t;

typedef struct vq {
    u16 index, size;
    vdesc_t *desc;
    volatile u16 *avail;            /* flags, idx, ring[size] */
    volatile u8 *used;              /* flags, idx, { u32 id, u32 len }[size] */
    u16 avail_idx, used_idx;
    volatile u16 *notify;
    void *mem;
} vq_t;

/* device structures (little-endian, packed as the spec lays them out) */
typedef struct { u32 code; u32 start_id; u32 count; u32 size; } __attribute__((packed)) query_t;
typedef struct {
    u32 hda_fn_nid, features;
    u64 formats, rates;
    u8 direction, channels_min, channels_max, pad[5];
} __attribute__((packed)) pcm_info_t;
typedef struct { u32 hda_fn_nid, features, defconf, caps; u8 connected, pad[7]; } __attribute__((packed)) jack_info_t;
typedef struct { u32 hda_fn_nid; u8 direction, channels, positions[18]; } __attribute__((packed)) chmap_info_t;
typedef struct {
    u32 code, stream_id, buffer_bytes, period_bytes, features;
    u8 channels, format, rate, pad;
} __attribute__((packed)) set_params_t;
typedef struct { u32 status, latency_bytes; } __attribute__((packed)) pcm_status_t;
typedef struct { u32 code, data; } __attribute__((packed)) event_t;

/* one transfer buffer: header + period (device-readable), status (device-writable) */
#define XFER_HDR 4

typedef struct vsnd {
    snddev_t sd;
    pci_dev_t *pci;
    volatile u8 *common, *isr, *devcfg;
    volatile u8 *notify_base;
    u32 notify_mult;
    u64 features;
    int msix;
    vq_t q[3];
    u8 *ctrl;                       /* control request / response area (4 KiB) */
    u8 *tx;                         /* NBUF transfer buffers of txstride bytes */
    u32 txstride, pbytes;
    pcm_status_t *txstatus;         /* NBUF */
    u8 busy[NBUF];                  /* with the device */
    int inflight;
    event_t *evt;                   /* NEVT */
    u32 stream;                     /* the PCM stream played */
    u8 rate_code;
    u32 jacks, streams, chmaps;
    u64 last_poll_ms;
    u32 errors;
    int ctrl_dead;                  /* a control request was never answered: the queue is not reused */
    irq_handle_t irq;
    char irq_name[32];
} vsnd_t;

/* ---------------- register access, virtqueues ---------------- */
static u8 get_status(vsnd_t *v) { return mmio_read8(v->common + C_STATUS); }
static void set_status(vsnd_t *v, u8 s) { mmio_write8(v->common + C_STATUS, s); }

static u32 cfg32(vsnd_t *v, u32 off) {
    for (int tries = 0; tries < 8; tries++) {
        u8 gen = mmio_read8(v->common + C_GEN);
        u32 x = mmio_read32(v->devcfg + off);
        if (mmio_read8(v->common + C_GEN) == gen) return x;
    }
    return 0;
}

static int vs_ack(void *ctx) {
    vsnd_t *v = ctx;
    return (mmio_read8(v->isr) & 3) != 0;          /* read-to-clear */
}

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

/* next used element: its descriptor head, or -1 when there is none */
static int vq_next_used(vq_t *q, u32 *len) {
    if (q->used_idx == used_idx(q)) return -1;
    rmb();
    volatile u8 *e = q->used + 4 + 8u * (q->used_idx % q->size);
    u32 id = mmio_read32(e);
    if (len) *len = mmio_read32(e + 4);
    q->used_idx++;
    return id < q->size ? (int)id : -2;
}

static int setup_queue(vsnd_t *v, u16 index, u16 want) {
    vq_t *q = &v->q[index];
    mmio_write16(v->common + C_QSEL, index);
    u16 max = mmio_read16(v->common + C_QSIZE);
    if (!max || mmio_read16(v->common + C_QEN)) return E_IO;
    u16 size = 1;
    while (size * 2 <= max && size < want) size *= 2;
    if (size < want) return E_IO;
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
        if (mmio_read16(v->common + C_QMSIX) == NO_VECTOR)
            kprintf("virtio-snd: queue %u: no MSI-X vector, polled\n", index);
    }
    q->notify = (volatile u16 *)(v->notify_base + (u32)mmio_read16(v->common + C_QNOFF) * v->notify_mult);
    mmio_write16(v->common + C_QEN, 1);
    return 0;
}

/* ---------------- control requests ---------------- */
/* send the request in ctrl[0..reqlen) and wait for the response written to
 * ctrl[2048..2048+resplen); returns the device's status code, or 0 when it
 * did not answer (the device then still owns the buffers: no more control
 * requests are sent) */
static u32 ctrl_req(vsnd_t *v, u32 reqlen, u32 resplen) {
    vq_t *q = &v->q[CTRLQ];
    u8 *resp = v->ctrl + 2048;
    if (v->ctrl_dead) return 0;
    memset(resp, 0, resplen);
    q->desc[0].addr = virt_to_phys(v->ctrl);
    q->desc[0].len = reqlen;
    q->desc[0].flags = D_NEXT;
    q->desc[0].next = 1;
    q->desc[1].addr = virt_to_phys(resp);
    q->desc[1].len = resplen;
    q->desc[1].flags = D_WRITE;
    q->desc[1].next = 0;
    avail_push(q, 0);
    notify(q);
    u64 end = time_ms() + 500;
    for (;;) {
        int id = vq_next_used(q, NULL);
        if (id == 0) break;
        if (id == -1 && time_ms() > end) {
            kprintf("virtio-snd: %s: control request 0x%x not answered\n", v->sd.name[0] ? v->sd.name : "?",
                    *(u32 *)v->ctrl);
            v->ctrl_dead = 1;
            return 0;
        }
        if (id == -1) udelay(20);
    }
    rmb();
    return *(volatile u32 *)resp;
}

/* an information query: count items of size bytes from start; the items
 * land at ctrl + 2048 + 4 */
static u32 query(vsnd_t *v, u32 code, u32 start, u32 count, u32 size) {
    query_t *r = (query_t *)v->ctrl;
    r->code = code; r->start_id = start; r->count = count; r->size = size;
    return ctrl_req(v, sizeof *r, 4 + count * size);
}

static u32 pcm_simple(vsnd_t *v, u32 code) {
    u32 *r = (u32 *)v->ctrl;
    r[0] = code;
    r[1] = v->stream;
    return ctrl_req(v, 8, 4);
}

static const char *st_name(u32 st) {
    switch (st) {
    case ST_OK: return "ok";
    case ST_BAD_MSG: return "bad message";
    case ST_NOT_SUPP: return "not supported";
    case ST_IO_ERR: return "I/O error";
    case 0: return "no answer";
    }
    return "unknown status";
}

/* ---------------- transfers, events ---------------- */
static void tx_post(vsnd_t *v, int b) {
    vq_t *q = &v->q[TXQ];
    u8 *buf = v->tx + (u32)b * v->txstride;
    *(u32 *)buf = v->stream;
    snd_mix(&v->sd, (i16 *)(buf + XFER_HDR), v->sd.period_frames);
    v->txstatus[b].status = 0;
    vdesc_t *d = &q->desc[b * 2];
    d[0].addr = virt_to_phys(buf);
    d[0].len = XFER_HDR + v->pbytes;
    d[0].flags = D_NEXT;
    d[0].next = (u16)(b * 2 + 1);
    d[1].addr = virt_to_phys(&v->txstatus[b]);
    d[1].len = sizeof(pcm_status_t);
    d[1].flags = D_WRITE;
    d[1].next = 0;
    v->busy[b] = 1;
    v->inflight++;
    avail_push(q, (u16)(b * 2));
}

/* returned transfer buffers: their numbers into done[], how many (quiet:
 * stopping, the device may fail what it still held) */
static int tx_reap(vsnd_t *v, int *done, int quiet) {
    vq_t *q = &v->q[TXQ];
    int n = 0;
    for (int budget = 0; budget < q->size; budget++) {
        int id = vq_next_used(q, NULL);
        if (id == -1) break;
        if (id < 0 || id % 2 || id / 2 >= NBUF || !v->busy[id / 2]) { v->errors++; continue; }
        int b = id / 2;
        v->busy[b] = 0;
        v->inflight--;
        rmb();
        if (v->txstatus[b].status != ST_OK && !quiet && v->errors++ < 4)
            kprintf("virtio-snd: %s: transfer failed (%s)\n", v->sd.name, st_name(v->txstatus[b].status));
        if (done) done[n] = b;
        n++;
    }
    return n;
}

static void evt_post(vsnd_t *v, int b) {
    vq_t *q = &v->q[EVENTQ];
    q->desc[b].addr = virt_to_phys(&v->evt[b]);
    q->desc[b].len = sizeof(event_t);
    q->desc[b].flags = D_WRITE;
    q->desc[b].next = 0;
    avail_push(q, (u16)b);
}

/* drain the event queue; returns the PCM_XRUN events for our stream */
static int evt_drain(vsnd_t *v) {
    vq_t *q = &v->q[EVENTQ];
    int xruns = 0, posted = 0;
    for (int budget = 0; budget < q->size; budget++) {
        u32 len;
        int id = vq_next_used(q, &len);
        if (id == -1) break;
        if (id < 0 || id >= NEVT) continue;
        rmb();
        event_t e = v->evt[id];
        if (len >= sizeof e) {
            if (e.code == EVT_PCM_XRUN && e.data == v->stream) xruns++;
            else if (e.code == EVT_JACK_CONNECTED || e.code == EVT_JACK_DISCONNECTED)
                kprintf("virtio-snd: %s: jack %u %s\n", v->sd.name, e.data,
                        e.code == EVT_JACK_CONNECTED ? "connected" : "disconnected");
        }
        evt_post(v, id);
        posted = 1;
    }
    if (posted) notify(q);
    return xruns;
}

/* ---------------- snddev ---------------- */
static int vs_start(snddev_t *sd) {
    vsnd_t *v = sd->priv;
    tx_reap(v, NULL, 1);
    set_params_t *p = (set_params_t *)v->ctrl;
    memset(p, 0, sizeof *p);
    p->code = R_PCM_SET_PARAMS;
    p->stream_id = v->stream;
    p->buffer_bytes = NBUF * v->pbytes;
    p->period_bytes = v->pbytes;
    p->channels = 2;
    p->format = FMT_S16;
    p->rate = v->rate_code;
    u32 st = ctrl_req(v, sizeof *p, 4);
    if (st != ST_OK) { kprintf("virtio-snd: %s: PCM_SET_PARAMS: %s\n", sd->name, st_name(st)); return E_IO; }
    st = pcm_simple(v, R_PCM_PREPARE);
    if (st != ST_OK) { kprintf("virtio-snd: %s: PCM_PREPARE: %s\n", sd->name, st_name(st)); return E_IO; }
    for (int b = 0; b < NBUF; b++) if (!v->busy[b]) tx_post(v, b);
    notify(&v->q[TXQ]);
    st = pcm_simple(v, R_PCM_START);
    if (st != ST_OK) {
        kprintf("virtio-snd: %s: PCM_START: %s\n", sd->name, st_name(st));
        pcm_simple(v, R_PCM_RELEASE);
        return E_IO;
    }
    v->last_poll_ms = time_ms();
    return 0;
}

static void vs_stop(snddev_t *sd) {
    vsnd_t *v = sd->priv;
    pcm_simple(v, R_PCM_STOP);
    pcm_simple(v, R_PCM_RELEASE);
    /* the device completes every pending transfer on release */
    u64 end = time_ms() + 200;
    while (tx_reap(v, NULL, 1), v->inflight > 0 && time_ms() < end) udelay(100);
    if (v->inflight) {
        kprintf("virtio-snd: %s: %d buffers not returned after PCM_RELEASE\n", sd->name, v->inflight);
        /* never reuse a buffer the device may still hold: it stays busy */
    }
    evt_drain(v);
}

static void vs_poll(snddev_t *sd) {
    vsnd_t *v = sd->priv;
    u64 now = time_ms();
    int done[NBUF];
    int n = tx_reap(v, done, 0);
    int late = n == NBUF && v->inflight == 0 &&
               now - v->last_poll_ms > (u64)NBUF * sd->period_frames * 1000 / sd->rate;
    int xruns = evt_drain(v);
    if (late || xruns) sd->underruns += late ? 1 : xruns;
    for (int i = 0; i < n; i++) tx_post(v, done[i]);
    if (n) notify(&v->q[TXQ]);
    if (n || xruns) irq_note_work(&v->irq);
    v->last_poll_ms = now;
    if ((get_status(v) & S_NEEDS_RESET) && v->errors++ < 4)
        kprintf("virtio-snd: %s: the device needs a reset (not supported)\n", sd->name);
    sd->irq_live = irq_live(&v->irq);
}

/* ---------------- setup ---------------- */
static int modern_caps(vsnd_t *v) {
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
        case CAP_DEVICE: if (!v->devcfg && len >= 12) v->devcfg = base + o; break;
        }
    }
    return v->common && v->notify_base && v->isr && v->devcfg;
}

/* pick the output stream; log what the device has */
static int choose_stream(vsnd_t *v) {
    if (v->jacks) {
        u32 n = MIN(v->jacks, (u32)MAXJACKS);
        if (query(v, R_JACK_INFO, 0, n, sizeof(jack_info_t)) == ST_OK) {
            jack_info_t *j = (jack_info_t *)(v->ctrl + 2048 + 4);
            for (u32 i = 0; i < n; i++)
                kprintf("virtio-snd: jack %u: %s, defconf %08x\n", i, j[i].connected ? "connected" : "not connected",
                        j[i].defconf);
        }
    }
    u32 n = MIN(v->streams, (u32)MAXSTREAMS);
    if (!n || query(v, R_PCM_INFO, 0, n, sizeof(pcm_info_t)) != ST_OK) return -1;
    pcm_info_t info[MAXSTREAMS];
    memcpy(info, v->ctrl + 2048 + 4, n * sizeof(pcm_info_t));
    int pick = -1;
    for (u32 i = 0; i < n; i++) {
        pcm_info_t *p = &info[i];
        int ok = p->direction == DIR_OUTPUT && (p->formats & (1ull << FMT_S16)) &&
                 p->channels_min <= 2 && p->channels_max >= 2 &&
                 (p->rates & ((1ull << RATE_48000) | (1ull << RATE_44100)));
        kprintf("virtio-snd: stream %u: %s, %u-%u channels, formats %lx, rates %lx%s\n", i,
                p->direction == DIR_OUTPUT ? "output" : "input", p->channels_min, p->channels_max,
                p->formats, p->rates, ok && pick < 0 ? " (used)" : "");
        if (ok && pick < 0) {
            pick = (int)i;
            v->rate_code = (p->rates & (1ull << RATE_48000)) ? RATE_48000 : RATE_44100;
        }
    }
    if (v->chmaps) {
        u32 m = MIN(v->chmaps, (u32)MAXCHMAPS);
        if (query(v, R_CHMAP_INFO, 0, m, sizeof(chmap_info_t)) == ST_OK) {
            chmap_info_t *c = (chmap_info_t *)(v->ctrl + 2048 + 4);
            for (u32 i = 0; i < m; i++)
                kprintf("virtio-snd: channel map %u: %s, %u channels\n", i,
                        c[i].direction == DIR_OUTPUT ? "output" : "input", c[i].channels);
        }
    }
    strlcpy(v->sd.output, "Speakers", sizeof v->sd.output);
    return pick;
}

static void vs_attach(pci_dev_t *pd) {
    vsnd_t *v = kzalloc(sizeof *v);
    if (!v) return;
    v->pci = pd;
    pci_enable(pd);
    if (!modern_caps(v)) {
        kprintf("virtio-snd: %02x:%02x.%x: no usable virtio capabilities\n", pd->bus, pd->dev, pd->func);
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
    v->features = dev & (F_VERSION_1 | F_ACCESS_PLATFORM);
    const char *fail = NULL;
    if (!(v->features & F_VERSION_1)) fail = "no VERSION_1 feature";
    else {
        mmio_write32(v->common + C_GFSEL, 0);
        mmio_write32(v->common + C_GF, (u32)v->features);
        mmio_write32(v->common + C_GFSEL, 1);
        mmio_write32(v->common + C_GF, (u32)(v->features >> 32));
        set_status(v, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(get_status(v) & S_FEATURES_OK)) fail = "features not accepted";
        else if (mmio_read16(v->common + C_NUMQ) < 3) fail = "fewer than 3 queues";
    }
    snprintf(v->irq_name, sizeof v->irq_name, "virtio-snd %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    v->irq.name = v->irq_name;
    v->irq.pci = pd;
    v->irq.ack = vs_ack;
    v->irq.ctx = v;
    v->sd.period_frames = 480;
    v->pbytes = 480 * 4;
    v->txstride = ALIGN_UP(XFER_HDR + v->pbytes, 64);
    if (!fail) {
        v->msix = irq_attach(&v->irq) == IRQ_MSIX;
        if (v->msix) {
            mmio_write16(v->common + C_MSIX, 0);
            (void)mmio_read16(v->common + C_MSIX);
        }
        v->ctrl = dma_alloc(4096, 4096);
        v->tx = dma_alloc((u64)NBUF * v->txstride, 4096);       /* 48 kHz periods: 44.1 kHz ones fit */
        v->txstatus = dma_alloc(NBUF * sizeof(pcm_status_t), 64);
        v->evt = dma_alloc(NEVT * sizeof(event_t), 64);
        if (!v->ctrl || !v->tx || !v->txstatus || !v->evt) fail = "out of memory";
        else if (setup_queue(v, CTRLQ, 2) || setup_queue(v, EVENTQ, NEVT) || setup_queue(v, TXQ, NBUF * 2))
            fail = "cannot set up the queues";
    }
    if (!fail) {
        set_status(v, get_status(v) | S_DRIVER_OK);
        for (int b = 0; b < NEVT; b++) evt_post(v, b);
        notify(&v->q[EVENTQ]);
        v->jacks = cfg32(v, 0);
        v->streams = cfg32(v, 4);
        v->chmaps = cfg32(v, 8);
        int s = choose_stream(v);
        if (s < 0) fail = "no output stream for S16 stereo at 48000 or 44100 Hz";
        else v->stream = (u32)s;
    }
    if (fail) {
        kprintf("virtio-snd: %02x:%02x.%x: %s\n", pd->bus, pd->dev, pd->func, fail);
        set_status(v, S_FAILED);
        irq_detach(&v->irq);
        for (int i = 0; i < 3; i++) dma_free(v->q[i].mem);
        dma_free(v->ctrl); dma_free(v->tx); dma_free(v->txstatus); dma_free(v->evt);
        kfree(v);
        return;
    }
    v->sd.rate = v->rate_code == RATE_48000 ? 48000 : 44100;
    v->sd.period_frames = v->sd.rate / 100;
    v->pbytes = (u32)v->sd.period_frames * 4;
    v->txstride = ALIGN_UP(XFER_HDR + v->pbytes, 64);
    v->sd.driver = "virtio-snd";
    snprintf(v->sd.desc, sizeof v->sd.desc, "Virtio sound device (stream %u of %u, %u jacks)",
             v->stream, v->streams, v->jacks);
    v->sd.channels = 2;
    v->sd.start = vs_start;
    v->sd.stop = vs_stop;
    v->sd.poll = vs_poll;
    v->sd.priv = v;
    v->sd.pci = pd;
    v->sd.irq = &v->irq;
    pd->driver = "virtio-snd";
    if (snddev_register(&v->sd)) kprintf("virtio-snd: too many sound devices\n");
}

void virtio_snd_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        /* 0x1059: virtio 1.0 sound device (device type 25) */
        if (pd->vendor == 0x1AF4 && pd->device == 0x1059 && !pd->driver) vs_attach(pd);
    }
}
