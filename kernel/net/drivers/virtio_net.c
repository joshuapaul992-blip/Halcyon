/* Halcyon OS - virtio network adapter (QEMU/KVM "virtio-net-pci", and what
 * most VM hosts offer).
 *
 * Reference: Virtual I/O Device (VIRTIO) Version 1.0 (OASIS), sections 2
 * (feature bits, split virtqueues), 4.1 (PCI transport: the vendor-specific
 * capabilities and the legacy I/O-port interface) and 5.1 (network device).
 *
 * Interfaces: the virtio 1.0 ("modern") PCI capabilities (common, notify,
 * ISR and device configuration in memory BARs) when the device has them, and
 * the legacy I/O-port interface (BAR0) as the fallback for transitional
 * devices (PCI device 0x1000) without usable capabilities. Device 0x1041 is
 * modern only. Features: MAC, STATUS (link state), SPEED_DUPLEX, VERSION_1,
 * ANY_LAYOUT (legacy), ACCESS_PLATFORM (accepted: there is no IOMMU, bus
 * addresses are physical ones); no offloads, no mergeable receive buffers, so
 * every receive buffer holds a whole frame. Legacy devices without
 * ANY_LAYOUT get the virtio-net header in a descriptor of its own.
 *
 * Queues: receiveq1 (0) and transmitq1 (1), split virtqueues of up to 256
 * entries. Interrupts: MSI-X (vector 0 for the queues and configuration
 * changes) or INTx (the ISR register is read-to-clear); they only wake the
 * net thread, which polls the used rings (DESIGN-M2). Tested in QEMU (modern,
 * transitional and legacy-only devices; MSI-X, INTx with the I/O APIC and the
 * PIC, and polled). */
#include "net/netdev.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* feature bits */
#define F_MAC           (1ull << 5)
#define F_MRG_RXBUF     (1ull << 15)
#define F_STATUS        (1ull << 16)
#define F_ANY_LAYOUT    (1ull << 27)
#define F_VERSION_1     (1ull << 32)
#define F_ACCESS_PLATFORM (1ull << 33)
#define F_SPEED_DUPLEX  (1ull << 63)

/* device status */
#define S_ACK          0x01
#define S_DRIVER       0x02
#define S_DRIVER_OK    0x04
#define S_FEATURES_OK  0x08
#define S_NEEDS_RESET  0x40
#define S_FAILED       0x80

/* legacy I/O BAR */
#define L_HOST_F   0x00
#define L_GUEST_F  0x04
#define L_QPFN     0x08
#define L_QSIZE    0x0C
#define L_QSEL     0x0E
#define L_QNOTIFY  0x10
#define L_STATUS   0x12
#define L_ISR      0x13
#define L_MSIX_CFG 0x14    /* only while MSI-X is enabled; the device config follows at 0x18, else at 0x14 */
#define L_MSIX_Q   0x16

/* modern common configuration */
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

/* PCI capability types (cfg_type) */
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

#define NO_VECTOR 0xFFFF
#define D_NEXT  1
#define D_WRITE 2
#define USED_NO_NOTIFY 1

#define QMAX   256          /* ring entries we use (the legacy interface fixes the size: larger rings get fewer buffers) */
#define BUFSZ  2048
#define RXQ 0
#define TXQ 1

typedef struct vdesc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed)) vdesc_t;

typedef struct vq {
    u16 index, size;        /* queue number, ring entries */
    u16 step;               /* descriptors per buffer: 1, or 2 (header, then frame) */
    u16 nbuf;               /* buffers */
    vdesc_t *desc;
    volatile u16 *avail;    /* flags, idx, ring[size] */
    volatile u8 *used;      /* flags, idx, { u32 id, u32 len }[size] */
    u16 avail_idx;          /* free-running, as the device counts */
    u16 used_idx;
    u8 *buf;                /* nbuf * BUFSZ */
    u8 *busy;               /* buffer is with the device */
    u16 *freelist;          /* transmit: free buffers */
    int nfree;
    volatile u16 *notify;   /* modern */
    void *mem;
} vq_t;

typedef struct vnet {
    netdev_t nd;
    pci_dev_t *pci;
    int modern;
    u16 io;                          /* legacy */
    volatile u8 *common, *isr, *dev; /* modern */
    volatile u8 *notify_base;
    u32 notify_mult;
    u64 features;
    u32 hdrlen;                      /* virtio_net_hdr: 12 (VERSION_1), 10 (legacy) */
    int msix;
    vq_t q[2];
    irq_handle_t irq;
    char irq_name[28];
} vnet_t;

/* ---------------- register access ---------------- */
static u8 get_status(vnet_t *v) { return v->modern ? mmio_read8(v->common + C_STATUS) : inb(v->io + L_STATUS); }
static void set_status(vnet_t *v, u8 s) {
    if (v->modern) mmio_write8(v->common + C_STATUS, s); else outb(v->io + L_STATUS, s);
}

static u8 cfg8(vnet_t *v, u32 off) {
    if (v->modern) return mmio_read8(v->dev + off);
    return inb((u16)(v->io + (v->msix ? 0x18 : 0x14) + off));
}

/* device configuration bytes, consistent across a config generation change */
static void cfg_read(vnet_t *v, u32 off, u8 *out, int n) {
    for (int tries = 0; tries < 8; tries++) {
        u8 gen = v->modern ? mmio_read8(v->common + C_GEN) : 0;
        for (int i = 0; i < n; i++) out[i] = cfg8(v, off + (u32)i);
        if (!v->modern || mmio_read8(v->common + C_GEN) == gen) return;
    }
}

static void notify(vnet_t *v, vq_t *q) {
    mb();
    if (mmio_read16((volatile void *)(q->used)) & USED_NO_NOTIFY) return;
    if (v->modern) mmio_write16(q->notify, q->index);
    else outw(v->io + L_QNOTIFY, q->index);
}

static int vnet_ack(void *ctx) {
    vnet_t *v = ctx;
    u8 isr = v->modern ? mmio_read8(v->isr) : inb(v->io + L_ISR);   /* read-to-clear */
    return (isr & 3) != 0;
}

/* ---------------- virtqueues ---------------- */
static u16 used_idx(vq_t *q) { return mmio_read16((volatile void *)(q->used + 2)); }

static void avail_push(vq_t *q, u16 head) {
    q->avail[2 + q->avail_idx % q->size] = head;
    wmb();
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
}

/* lay out buffer b's descriptors (rx: device-writable) */
static void vq_fill(vnet_t *v, vq_t *q, int b, u32 len, int rx) {
    u8 *buf = q->buf + (u32)b * BUFSZ;
    vdesc_t *d = &q->desc[b * q->step];
    if (q->step == 2) {
        d[0].addr = virt_to_phys(buf);
        d[0].len = v->hdrlen;
        d[0].flags = D_NEXT | (rx ? D_WRITE : 0);
        d[0].next = (u16)(b * 2 + 1);
        d[1].addr = virt_to_phys(buf + v->hdrlen);
        d[1].len = len - v->hdrlen;
        d[1].flags = rx ? D_WRITE : 0;
        d[1].next = 0;
    } else {
        d->addr = virt_to_phys(buf);
        d->len = len;
        d->flags = rx ? D_WRITE : 0;
        d->next = 0;
    }
}

static int vq_alloc(vq_t *q, u16 index, u16 size, u16 step, int tx) {
    q->index = index;
    q->size = size;
    q->step = step;
    q->nbuf = size / step;
    if (q->nbuf > QMAX) q->nbuf = QMAX;      /* a big legacy ring: the rest of it stays unused */
    u32 avail_off = 16u * size, used_off = (avail_off + 6 + 2u * size + 4095) & ~4095u;
    q->mem = dma_alloc(used_off + 6 + 8u * size, 4096);
    q->buf = dma_alloc((u32)q->nbuf * BUFSZ, 4096);
    q->busy = kzalloc(q->nbuf);
    q->freelist = tx ? kmalloc(sizeof(u16) * q->nbuf) : NULL;
    if (!q->mem || !q->buf || !q->busy || (tx && !q->freelist)) return E_NOMEM;
    q->desc = q->mem;
    q->avail = (volatile u16 *)((u8 *)q->mem + avail_off);
    q->used = (volatile u8 *)q->mem + used_off;
    q->avail_idx = q->used_idx = 0;
    if (tx) {
        for (int i = 0; i < q->nbuf; i++) q->freelist[i] = (u16)(q->nbuf - 1 - i);
        q->nfree = q->nbuf;
    }
    return 0;
}

static void vq_free(vq_t *q) {
    dma_free(q->mem); dma_free(q->buf); kfree(q->busy); kfree(q->freelist);
    q->mem = q->buf = NULL; q->busy = NULL; q->freelist = NULL;
}

/* the next used element: its buffer number, or -1 when the ring is empty;
 * -2 for an element that names no buffer the device holds (a device bug:
 * skipped, never trusted) */
static int vq_next_used(vq_t *q, u32 *len) {
    if (q->used_idx == used_idx(q)) return -1;
    rmb();
    volatile u8 *e = q->used + 4 + 8u * (q->used_idx % q->size);
    u32 id = mmio_read32(e), l = mmio_read32(e + 4);
    q->used_idx++;
    if (id >= q->size || id % q->step || id / q->step >= q->nbuf || !q->busy[id / q->step]) return -2;
    q->busy[id / q->step] = 0;
    *len = l;
    return (int)(id / q->step);
}

/* ---------------- receive / transmit ---------------- */
static void rx_post(vnet_t *v, int b) {
    vq_t *q = &v->q[RXQ];
    vq_fill(v, q, b, BUFSZ, 1);
    q->busy[b] = 1;
    avail_push(q, (u16)(b * q->step));
}

static int tx_reap(vnet_t *v) {
    vq_t *q = &v->q[TXQ];
    int n = 0;
    for (int budget = 0; budget < q->nbuf; budget++) {
        u32 len;
        int b = vq_next_used(q, &len);
        if (b == -1) break;
        if (b < 0) { v->nd.tx_errors++; continue; }
        u32 flen = q->step == 2 ? q->desc[b * 2 + 1].len : q->desc[b].len - v->hdrlen;
        v->nd.tx_packets++;
        v->nd.tx_bytes += flen;
        q->freelist[q->nfree++] = (u16)b;
        n++;
    }
    return n;
}

static int vnet_xmit(netdev_t *nd, const void *frame, u32 len) {
    vnet_t *v = nd->priv;
    vq_t *q = &v->q[TXQ];
    if (len < ETH_HDR || len > ETH_MAX_FRAME) return E_INVAL;
    if (!q->nfree) tx_reap(v);
    if (!q->nfree) return E_BUSY;                    /* ring full: the caller keeps the frame */
    int b = q->freelist[--q->nfree];
    u8 *buf = q->buf + (u32)b * BUFSZ;
    memset(buf, 0, v->hdrlen);                       /* no checksum or segmentation offload */
    memcpy(buf + v->hdrlen, frame, len);
    if (len < 60) { memset(buf + v->hdrlen + len, 0, 60 - len); len = 60; }
    vq_fill(v, q, b, v->hdrlen + len, 0);
    q->busy[b] = 1;
    avail_push(q, (u16)(b * q->step));
    notify(v, q);
    return 0;
}

static void link_update(vnet_t *v) {
    netdev_t *nd = &v->nd;
    if (v->features & F_STATUS) {
        u8 st[2];
        cfg_read(v, 6, st, 2);
        nd->link_up = st[0] & 1;
    } else {
        nd->link_up = 1;                             /* no link state: always up */
    }
    nd->speed_mbps = 0;
    nd->full_duplex = 1;
    if (v->features & F_SPEED_DUPLEX) {
        u8 b[5];
        cfg_read(v, 12, b, 5);
        u32 sp = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
        if (sp && sp != 0xFFFFFFFFu) nd->speed_mbps = sp > 0x7FFFFFFF ? 0x7FFFFFFF : (int)sp;
        if (b[4] == 0) nd->full_duplex = 0;
    }
}

static void vnet_poll(netdev_t *nd) {
    vnet_t *v = nd->priv;
    vq_t *q = &v->q[RXQ];
    int work = 0, posted = 0;
    for (int budget = 0; budget < q->nbuf; budget++) {
        u32 len;
        int b = vq_next_used(q, &len);
        if (b == -1) break;
        work = 1;
        if (b < 0) { nd->rx_errors++; continue; }
        /* the length comes from the other side: check it against the buffer */
        if (len < v->hdrlen + ETH_HDR || len > BUFSZ || len - v->hdrlen > ETH_MAX_FRAME + 4) {
            nd->rx_errors++;
        } else {
            nd->rx_packets++;
            nd->rx_bytes += len - v->hdrlen;
            netdev_rx(nd, q->buf + (u32)b * BUFSZ + v->hdrlen, len - v->hdrlen);
        }
        rx_post(v, b);
        posted = 1;
    }
    if (posted) notify(v, q);
    if (tx_reap(v)) work = 1;
    if (work) irq_note_work(&v->irq);
    if (get_status(v) & S_NEEDS_RESET) {
        if (nd->link_up) kprintf("virtio-net: %s: the device needs a reset (not supported), link down\n", nd->name);
        nd->link_up = 0;
    } else {
        link_update(v);
    }
    nd->irq_live = irq_live(&v->irq);
}

/* ---------------- setup ---------------- */
static int modern_caps(vnet_t *v) {
    pci_dev_t *pd = v->pci;
    if (!(pci_read16(pd, 0x06) & (1u << 4))) return 0;
    u8 off = pci_read8(pd, 0x34) & 0xFC;
    for (int n = 0; off >= 0x40 && n < 48; n++, off = pci_read8(pd, (u8)(off + 1)) & 0xFC) {
        if (pci_read8(pd, off) != 0x09) continue;
        u8 type = pci_read8(pd, (u8)(off + 3)), bar = pci_read8(pd, (u8)(off + 4));
        u32 o = pci_read32(pd, (u8)(off + 8)), len = pci_read32(pd, (u8)(off + 12));
        if (type < CAP_COMMON || type > CAP_DEVICE || bar > 5) continue;
        pci_bar_t *b = &pd->bar[bar];
        if (b->is_io || !b->addr || (u64)o + len > b->size) continue;   /* only memory BARs */
        volatile u8 *base = pci_map_bar(pd, bar);
        if (!base) continue;
        switch (type) {
        case CAP_COMMON: if (!v->common && len >= 0x38) v->common = base + o; break;
        case CAP_NOTIFY:
            if (!v->notify_base && len >= 2) { v->notify_base = base + o; v->notify_mult = pci_read32(pd, (u8)(off + 16)); }
            break;
        case CAP_ISR: if (!v->isr && len >= 1) v->isr = base + o; break;
        case CAP_DEVICE: if (!v->dev && len >= 6) v->dev = base + o; break;
        }
    }
    return v->common && v->notify_base && v->isr && v->dev;
}

static u64 device_features(vnet_t *v) {
    if (!v->modern) return inl(v->io + L_HOST_F);
    mmio_write32(v->common + C_DFSEL, 0);
    u64 f = mmio_read32(v->common + C_DF);
    mmio_write32(v->common + C_DFSEL, 1);
    return f | (u64)mmio_read32(v->common + C_DF) << 32;
}

static int setup_queue(vnet_t *v, u16 index) {
    vq_t *q = &v->q[index];
    int step = 1;
    if (!(v->features & (F_VERSION_1 | F_ANY_LAYOUT))) step = 2;     /* legacy framing: header apart */
    if (v->modern) {
        mmio_write16(v->common + C_QSEL, index);
        u16 max = mmio_read16(v->common + C_QSIZE);
        if (!max || mmio_read16(v->common + C_QEN)) return E_IO;
        u16 size = 1;
        while (size * 2 <= max && size * 2 <= QMAX) size *= 2;          /* a power of two for split rings */
        if (vq_alloc(q, index, size, (u16)step, index == TXQ)) return E_NOMEM;
        mmio_write16(v->common + C_QSIZE, size);
        mmio_write64(v->common + C_QDESC, virt_to_phys(q->desc));
        mmio_write64(v->common + C_QAVAIL, virt_to_phys((const void *)q->avail));
        mmio_write64(v->common + C_QUSED, virt_to_phys((const void *)q->used));
        if (v->msix) {
            mmio_write16(v->common + C_QMSIX, 0);
            if (mmio_read16(v->common + C_QMSIX) == NO_VECTOR)
                kprintf("virtio-net: queue %u: no MSI-X vector, polled\n", index);
        }
        u16 noff = mmio_read16(v->common + C_QNOFF);
        q->notify = (volatile u16 *)(v->notify_base + (u32)noff * v->notify_mult);
        mmio_write16(v->common + C_QEN, 1);
    } else {
        outw(v->io + L_QSEL, index);
        u16 size = inw(v->io + L_QSIZE);
        if (!size || (size & (size - 1)) || inl(v->io + L_QPFN)) return E_IO;
        if (size > 32768 || vq_alloc(q, index, size, (u16)step, index == TXQ)) return E_NOMEM;
        if (v->msix) {
            outw(v->io + L_MSIX_Q, 0);
            if (inw(v->io + L_MSIX_Q) == NO_VECTOR) kprintf("virtio-net: queue %u: no MSI-X vector, polled\n", index);
        }
        outl(v->io + L_QPFN, (u32)(virt_to_phys(q->mem) >> 12));
    }
    return 0;
}

static void vnet_attach(pci_dev_t *pd) {
    vnet_t *v = kzalloc(sizeof *v);
    if (!v) return;
    v->pci = pd;
    pci_enable(pd);
    const char *where = "";
    if (modern_caps(v)) v->modern = 1;
    else if (pd->device == 0x1041) where = "no usable virtio 1.0 capabilities";
    else if (!pd->bar[0].is_io || !pd->bar[0].addr) where = "no legacy I/O BAR";
    else v->io = (u16)pd->bar[0].addr;
    if (*where) {
        kprintf("virtio-net: %02x:%02x.%x: %s\n", pd->bus, pd->dev, pd->func, where);
        kfree(v);
        return;
    }
    /* reset, acknowledge, negotiate */
    set_status(v, 0);
    for (int i = 0; i < 1000 && get_status(v); i++) udelay(100);
    set_status(v, S_ACK);
    set_status(v, S_ACK | S_DRIVER);
    u64 dev = device_features(v);
    u64 want = F_MAC | F_STATUS;
    if (v->modern) want |= F_VERSION_1 | F_ACCESS_PLATFORM | F_SPEED_DUPLEX;
    else want |= F_ANY_LAYOUT;
    v->features = dev & want;
    v->hdrlen = (v->features & (F_VERSION_1 | F_MRG_RXBUF)) ? 12 : 10;
    const char *fail = NULL;
    if (v->modern) {
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
    } else {
        outl(v->io + L_GUEST_F, (u32)v->features);
    }
    /* interrupts: before the queues, which are given the MSI-X vector */
    snprintf(v->irq_name, sizeof v->irq_name, "virtio-net %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    v->irq.name = v->irq_name;
    v->irq.pci = pd;
    v->irq.ack = vnet_ack;
    v->irq.ctx = v;
    if (!fail) {
        v->msix = irq_attach(&v->irq) == IRQ_MSIX;
        if (v->msix) {
            if (v->modern) {
                mmio_write16(v->common + C_MSIX, 0);
                (void)mmio_read16(v->common + C_MSIX);
            } else {
                outw(v->io + L_MSIX_CFG, 0);
            }
        }
        if (setup_queue(v, RXQ) || setup_queue(v, TXQ)) fail = "cannot set up the queues";
    }
    if (fail) {
        kprintf("virtio-net: %02x:%02x.%x: %s\n", pd->bus, pd->dev, pd->func, fail);
        set_status(v, S_FAILED);
        irq_detach(&v->irq);
        vq_free(&v->q[RXQ]); vq_free(&v->q[TXQ]);
        kfree(v);
        return;
    }
    for (int b = 0; b < v->q[RXQ].nbuf; b++) rx_post(v, b);
    set_status(v, get_status(v) | S_DRIVER_OK);
    notify(v, &v->q[RXQ]);
    /* MAC: the device's, else a random locally administered one */
    if (v->features & F_MAC) cfg_read(v, 0, v->nd.mac, 6);
    else {
        random_bytes(v->nd.mac, 6);
        v->nd.mac[0] = (u8)((v->nd.mac[0] & 0xFE) | 0x02);
    }
    link_update(v);
    v->nd.driver = "virtio-net";
    snprintf(v->nd.desc, sizeof v->nd.desc, "Virtio network adapter (%s, %u/%u buffers)",
             v->modern ? "virtio 1.0" : "legacy", v->q[RXQ].nbuf, v->q[TXQ].nbuf);
    v->nd.xmit = vnet_xmit;
    v->nd.poll = vnet_poll;
    v->nd.priv = v;
    v->nd.pci = pd;
    v->nd.irq = &v->irq;
    pd->driver = "virtio-net";
    if (netdev_register(&v->nd)) kprintf("virtio-net: too many network adapters\n");
}

void virtio_net_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->vendor != 0x1AF4 || pd->driver) continue;
        /* 0x1000: transitional network device (subsystem 1); 0x1041: virtio 1.0 network device */
        if (pd->device == 0x1041 || pd->device == 0x1000) vnet_attach(pd);
    }
}
