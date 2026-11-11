/* Halcyon OS - EHCI (USB 2.0) host controller driver, polled
 *
 * Design:
 *  - Async schedule: a static, permanently halted head QH (H bit) linking to
 *    itself. Every control/bulk endpoint gets a persistent QH with a small
 *    ring of qTDs (ep0: dev->ep0.hc_priv, bulk: ep->hc_priv), linked on first
 *    use. The QH is always parked on an inactive terminator qTD; a transfer
 *    is queued by building the chain starting *at* the terminator and
 *    activating its first qTD last ("dummy qTD" technique), so the hot path
 *    never writes to a linked QH and needs no doorbell. Transfers are
 *    synchronous: the qTD tokens are polled with a deadline.
 *  - A QH is taken out of the schedule (Interrupt-on-Async-Advance doorbell,
 *    USBSTS.IAA polled with a timeout) only when it has to be modified: ep0
 *    address / max packet size changed (they are compared on every control
 *    transfer), halt or timeout recovery (all qTDs deactivated, overlay
 *    reset, toggle kept), reset_endpoint, device removal.
 *  - Data toggles: control uses DTC=1 (toggle from the qTD); bulk uses
 *    QH-managed toggles (DTC=0), reset to DATA0 by reset_endpoint.
 *    Interrupt QHs use DTC=1 with ep->toggle advanced by the packet count.
 *  - Short packets: IN data qTDs point their alternate-next at the control
 *    status stage, or for bulk at the terminator, where the queue stops.
 *  - Periodic schedule: 1024-entry frame list pointing into a tree of six
 *    halted dummy QHs (periods 32,16,8,4,2,1 ms). Each interrupt endpoint gets
 *    a persistent QH behind the anchor for its period with two qTDs swapped
 *    as transfer/terminator. It is unlinked (waiting for the frame index to
 *    advance) on cancel, error and device removal.
 *  - Root ports: high-speed devices are enumerated here; low-speed (K-state
 *    line status) and full-speed (port not enabled after reset) devices are
 *    released to the companion controller via PORT_OWNER. A released port is
 *    ignored until the device is unplugged and ownership returns to EHCI.
 *  - LS/FS devices behind a high-speed hub use split transactions (hub
 *    address / port in the QH, C bit for control, S-mask 0x01 / C-mask 0x1C
 *    for interrupt endpoints).
 */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/kernel.h"

/* capability registers */
#define CAP_LENGTH     0x00
#define CAP_HCSPARAMS  0x04
#define CAP_HCCPARAMS  0x08

/* operational registers */
#define USBCMD        0x00
#define USBSTS        0x04
#define USBINTR       0x08
#define FRINDEX       0x0C
#define CTRLDSSEGMENT 0x10
#define PERIODICLIST  0x14
#define ASYNCLIST     0x18
#define CONFIGFLAG    0x40
#define PORTSC        0x44

#define CMD_RS      (1u << 0)
#define CMD_HCRESET (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define CMD_IAAD    (1u << 6)
#define CMD_ITC(x)  ((u32)(x) << 16)

#define STS_USBINT  (1u << 0)
#define STS_ERRINT  (1u << 1)
#define STS_PCD     (1u << 2)
#define STS_FLR     (1u << 3)
#define STS_HSE     (1u << 4)
#define STS_IAA     (1u << 5)
#define STS_HALTED  (1u << 12)
#define STS_PSS     (1u << 14)
#define STS_ASS     (1u << 15)

#define PORT_CCS    (1u << 0)
#define PORT_CSC    (1u << 1)
#define PORT_PE     (1u << 2)
#define PORT_PEC    (1u << 3)
#define PORT_OCC    (1u << 5)
#define PORT_PR     (1u << 8)
#define PORT_LS_SHIFT 10
#define PORT_PP     (1u << 12)
#define PORT_OWNER  (1u << 13)
#define PORT_W1C    (PORT_CSC | PORT_PEC | PORT_OCC)

#define HCS_NPORTS(x) ((x) & 0xF)
#define HCS_PPC(x)    (((x) >> 4) & 1)
#define HCS_NCC(x)    (((x) >> 12) & 0xF)
#define HCC_64BIT(x)  ((x) & 1)
#define HCC_EECP(x)   (((x) >> 8) & 0xFF)

/* link pointers */
#define LP_T   1u
#define LP_QH  (1u << 1)
#define LP_MASK 0xFFFFFFE0u

/* qTD token */
#define QTD_PING    (1u << 0)
#define QTD_STS_MMF (1u << 2)
#define QTD_XACT    (1u << 3)
#define QTD_BABBLE  (1u << 4)
#define QTD_DBE     (1u << 5)
#define QTD_HALTED  (1u << 6)
#define QTD_ACTIVE  (1u << 7)
#define QTD_PID_OUT   (0u << 8)
#define QTD_PID_IN    (1u << 8)
#define QTD_PID_SETUP (2u << 8)
#define QTD_CERR(x) ((u32)(x) << 10)
#define QTD_IOC     (1u << 15)
#define QTD_LEN(x)  ((u32)(x) << 16)
#define QTD_DT      (1u << 31)
#define QTD_GET_CERR(t) (((t) >> 10) & 3)
#define QTD_GET_LEN(t)  (((t) >> 16) & 0x7FFF)

/* QH endpoint characteristics / capabilities */
#define QH_EPS_FULL 0u
#define QH_EPS_LOW  1u
#define QH_EPS_HIGH 2u
#define QH_DTC      (1u << 14)
#define QH_HEAD     (1u << 15)
#define QH_CTRL     (1u << 27)
#define QH_RL(x)    ((u32)(x) << 28)
#define QH_MULT(x)  ((u32)(x) << 30)

#define QTD_MAX_BYTES (5 * 4096)
#define NQTD_BULK 97            /* ring per bulk endpoint: >= 1 MiB per batch at any alignment */
#define NQTD_CTRL 8             /* setup + up to 5 data + status + terminator */

/* 64-bit capable controllers use the extended (Appendix B) layouts; the high
 * buffer pointer dwords stay zero since all DMA memory is below 4 GiB. */
typedef struct ehci_qtd {
    volatile u32 next, alt, token, buf[5];
    volatile u32 buf_hi[5];
    u32 len;                    /* software: bytes requested */
    u32 pad[2];
} ehci_qtd_t;

typedef struct ehci_qh {
    volatile u32 horiz, ep_char, ep_cap, cur;
    volatile u32 next, alt, token, buf[5];     /* transfer overlay */
    volatile u32 buf_hi[5];
    u32 sw[15];
} ehci_qh_t;

_Static_assert(sizeof(ehci_qtd_t) == 64, "qTD size");
_Static_assert(sizeof(ehci_qh_t) == 128, "QH size");

/* persistent async queue (one per control/bulk endpoint) */
typedef struct ehci_queue {
    ehci_qh_t *qh;
    ehci_qtd_t *ring;           /* qTDs, used round-robin */
    int nring;
    int dummy;                  /* inactive terminator the QH is parked on */
    int linked;
} ehci_queue_t;

#define NANCHOR 6               /* periods 1,2,4,8,16,32 ms */

typedef struct ehci_intr {
    ehci_qh_t *qh;
    ehci_qtd_t *td;             /* td[0], td[1]: active transfer / terminator */
    int cur;                    /* index of the qTD the QH will execute next */
    int len;
    int pending;
    int slot;                   /* anchor index */
} ehci_intr_t;

typedef struct ehci {
    usb_hc_t hc;
    volatile u8 *cap, *op;
    u32 hcs, hcc;
    int nports, ncc;
    u32 *frame_list;
    ehci_qh_t *async_head;
    ehci_qh_t *anchor[NANCHOR];
    u8 *setup_buf;
    int busy, dead;
    usb_device_t *port_dev[16];
    u8 port_failed[16];
} ehci_t;

static u32 phys(const void *p) { return (u32)virt_to_phys(p); }

static u32 op_rd(ehci_t *e, u32 reg) { return mmio_read32(e->op + reg); }
static void op_wr(ehci_t *e, u32 reg, u32 v) { mmio_write32(e->op + reg, v); }
static u32 port_rd(ehci_t *e, int p) { return op_rd(e, PORTSC + 4 * p); }
static void port_wr(ehci_t *e, int p, u32 v) { op_wr(e, PORTSC + 4 * p, v); }

/* ---------- helpers ---------- */

/* Fill a qTD for up to len bytes at buf. Unless the whole remainder fits, the
 * length is rounded down to a multiple of mps so no packet spans two qTDs.
 * Returns the number of bytes the qTD covers; the caller writes the token. */
static int qtd_fill(ehci_qtd_t *t, const u8 *buf, int len, int mps) {
    u32 a = phys(buf);
    int n = QTD_MAX_BYTES - (int)(a & 0xFFF);
    if (len <= n) n = len;
    else n -= n % mps;
    t->buf[0] = len ? a : 0;
    for (int k = 1; k < 5; k++) t->buf[k] = len ? (a & ~0xFFFu) + 4096u * (u32)k : 0;
    for (int k = 0; k < 5; k++) t->buf_hi[k] = 0;
    t->len = (u32)n;
    return n;
}

static int qtd_error(u32 tok) {
    if (tok & (QTD_BABBLE | QTD_DBE)) return E_IO;
    if (QTD_GET_CERR(tok)) return E_STALL;      /* halted with retries left: STALL handshake */
    return E_IO;                                /* transaction errors / missed microframes */
}

/* toggle bookkeeping for a retired qTD (DTC=1): count the packets moved */
static void toggle_advance(usb_endpoint_t *ep, int len, int done, int mps) {
    int pk;
    if (done >= len) pk = len ? (len + mps - 1) / mps : 1;
    else pk = done / mps + 1;              /* short packet terminated it */
    ep->toggle ^= (u8)(pk & 1);
}

static u32 qh_speed(usb_device_t *dev) {
    if (dev->speed == USB_SPEED_HIGH) return QH_EPS_HIGH;
    return dev->speed == USB_SPEED_LOW ? QH_EPS_LOW : QH_EPS_FULL;
}

/* endpoint capabilities: hub address/port for split transactions */
static int qh_tt(usb_device_t *dev, u32 *cap) {
    if (dev->speed == USB_SPEED_HIGH) return 0;
    if (!dev->tt_hub) return E_NOTSUP;     /* LS/FS on a root port belongs to the companion */
    *cap |= ((u32)(dev->tt_hub->address & 0x7F) << 16) | ((u32)(dev->tt_port & 0x7F) << 23);
    return 0;
}

/* root port of dev gone or handed to the companion? */
static int dev_gone(ehci_t *e, usb_device_t *dev) {
    int p = dev->root_port - 1;
    if (p < 0 || p >= e->nports) return 0;
    u32 v = port_rd(e, p);
    return !(v & PORT_CCS) || (v & PORT_OWNER);
}

/* ---------- async schedule ---------- */
static void async_link(ehci_t *e, ehci_qh_t *qh) {
    qh->horiz = e->async_head->horiz;
    mb();
    e->async_head->horiz = phys(qh) | LP_QH;
    mb();
}

/* Remove qh from the async ring (no doorbell). Returns 1 if it was linked. */
static int async_unlink_nowait(ehci_t *e, ehci_qh_t *qh) {
    ehci_qh_t *p = e->async_head;
    for (int guard = 0; (p->horiz & LP_MASK) != phys(qh); guard++) {
        p = phys_to_virt(p->horiz & LP_MASK);
        if (p == e->async_head || guard > 1024) return 0;   /* not linked */
    }
    p->horiz = qh->horiz;
    mb();
    return 1;
}

/* Interrupt-on-Async-Advance handshake: once it completes the HC holds no
 * cached pointers to QHs unlinked before the doorbell was rung. */
static int iaa_wait(ehci_t *e) {
    u32 sts = op_rd(e, USBSTS);
    if ((sts & STS_HALTED) || !(sts & STS_ASS)) return 0;   /* schedule not running */
    op_wr(e, USBSTS, STS_IAA);
    op_wr(e, USBCMD, op_rd(e, USBCMD) | CMD_IAAD);
    u64 deadline = time_ms() + 100;
    while (!(op_rd(e, USBSTS) & STS_IAA)) {
        if (time_ms() > deadline) {
            kprintf("ehci: async advance doorbell timed out (sts %08x)\n", op_rd(e, USBSTS));
            e->dead = 1;      /* cannot know when memory is safe to reuse */
            return E_TIMEOUT;
        }
        udelay(5);
    }
    op_wr(e, USBSTS, STS_IAA);
    return 0;
}

static void queue_unlink(ehci_t *e, ehci_queue_t *qu) {
    if (!qu->linked) return;
    if (async_unlink_nowait(e, qu->qh)) iaa_wait(e);
    qu->linked = 0;
}

static ehci_queue_t *queue_create(int nring) {
    ehci_queue_t *qu = kzalloc(sizeof *qu);
    if (!qu) return NULL;
    qu->qh = dma_alloc(sizeof(ehci_qh_t), 128);
    qu->ring = dma_alloc((size_t)nring * sizeof(ehci_qtd_t), 64);
    if (!qu->qh || !qu->ring) { dma_free(qu->qh); dma_free(qu->ring); kfree(qu); return NULL; }
    qu->nring = nring;
    for (int i = 0; i < nring; i++) { qu->ring[i].next = LP_T; qu->ring[i].alt = LP_T; }
    qu->qh->next = phys(&qu->ring[0]);
    qu->qh->alt = LP_T;
    return qu;
}

static void queue_free(ehci_queue_t *qu) {
    if (!qu) return;
    dma_free(qu->ring);
    dma_free(qu->qh);
    kfree(qu);
}

/* Unlink a queue and reset its overlay after an error or timeout: all qTDs
 * inactive, the QH parked on qTD 'dummy', halt cleared, toggle preserved.
 * It is linked again by the next transfer. */
static void queue_repair(ehci_t *e, ehci_queue_t *qu, int dummy) {
    queue_unlink(e, qu);
    if (e->dead) return;
    for (int i = 0; i < qu->nring; i++) {
        qu->ring[i].token = 0;
        qu->ring[i].next = LP_T;
        qu->ring[i].alt = LP_T;
    }
    ehci_qh_t *qh = qu->qh;
    u32 dt = qh->token & QTD_DT;
    qh->cur = 0;
    qh->next = phys(&qu->ring[dummy]);
    qh->alt = LP_T;
    for (int k = 0; k < 5; k++) { qh->buf[k] = 0; qh->buf_hi[k] = 0; }
    qh->token = dt;
    qu->dummy = dummy;
    mb();
}

/* (Re)program endpoint characteristics; a linked QH is unlinked first since
 * the HC may cache them. */
static void queue_set_chars(ehci_t *e, ehci_queue_t *qu, u32 ep_char, u32 ep_cap) {
    if (qu->qh->ep_char == ep_char && qu->qh->ep_cap == ep_cap) return;
    queue_unlink(e, qu);
    qu->qh->ep_char = ep_char;
    qu->qh->ep_cap = ep_cap;
    mb();
}

static ehci_qtd_t *qslot(ehci_queue_t *qu, int i) { return &qu->ring[(qu->dummy + i) % qu->nring]; }

/* Execute n qTDs prepared at qslot(0..n-1) (tokens set, Active included
 * except for qslot(0)); qslot(n) becomes the next terminator. qslot(0) is the
 * inactive terminator the QH is parked on, so the queue is extended without
 * touching the QH (the "dummy qTD" technique). status_idx: control status
 * stage (short IN data skips to it), -1 for bulk. Returns data bytes or a
 * negative error. */
static int queue_run(ehci_t *e, usb_device_t *dev, ehci_queue_t *qu, int n, int status_idx, u64 deadline) {
    ehci_qtd_t *term = qslot(qu, n);
    term->token = 0;
    term->next = LP_T;
    term->alt = LP_T;
    for (int i = 0; i < n; i++) qslot(qu, i)->next = phys(qslot(qu, i + 1));
    wmb();
    qslot(qu, 0)->token |= QTD_ACTIVE;
    mb();
    if (!qu->linked) { async_link(e, qu->qh); qu->linked = 1; }

    int bytes = 0, result = E_TIMEOUT, i = 0;
    u64 last_chk = time_ms();
    for (;;) {
        ehci_qtd_t *t = qslot(qu, i);
        u32 tok = t->token;
        if (tok & QTD_ACTIVE) {
            u64 now = time_ms();
            if (now > deadline) break;
            /* poll memory; look at the registers about once per millisecond */
            if (now != last_chk) {
                last_chk = now;
                if (op_rd(e, USBSTS) & (STS_HSE | STS_HALTED)) {
                    kprintf("ehci: controller stopped (sts %08x)\n", op_rd(e, USBSTS));
                    e->dead = 1;
                    result = E_IO;
                    break;
                }
                if (dev_gone(e, dev)) { result = E_NODEV; break; }   /* surprise removal */
            }
            cpu_relax();
            continue;
        }
        if (tok & QTD_HALTED) { result = qtd_error(tok); break; }
        int done = (int)t->len - (int)QTD_GET_LEN(tok);
        if ((tok & (3u << 8)) != QTD_PID_SETUP && i != status_idx) bytes += done;
        if (i == n - 1) { result = bytes; break; }
        if (done < (int)t->len && i != status_idx) {      /* short packet */
            if (status_idx < 0) { result = bytes; break; }
            i = status_idx;
            continue;
        }
        i++;
    }
    int next_dummy = (qu->dummy + n) % qu->nring;
    if (result >= 0) {
        /* the HC is parked on the terminator (reached via next, or via alt
         * after a short packet) */
        qu->dummy = next_dummy;
    } else {
        queue_repair(e, qu, next_dummy);
        if ((result == E_IO || result == E_TIMEOUT) && dev_gone(e, dev)) result = E_NODEV;
    }
    return result;
}

static u32 async_ep_char(usb_device_t *dev, int epnum, int mps, int control) {
    u32 eps = qh_speed(dev);
    u32 c = (dev->address & 0x7F) | ((u32)(epnum & 0xF) << 8) | (eps << 12) | ((u32)(mps & 0x7FF) << 16);
    if (eps == QH_EPS_HIGH) c |= QH_RL(4);      /* NAK throttling; 0 for split transactions */
    if (control) c |= QH_DTC | (eps != QH_EPS_HIGH ? QH_CTRL : 0);
    return c;
}

/* An async (control/bulk) split transfer that ended with an error other than
 * STALL, or that we abandoned, can leave the TT's buffer for that endpoint
 * busy; the hub must then be sent CLEAR_TT_BUFFER (USB 2.0 11.17.5,
 * 11.24.2.3), or later splits to the endpoint keep failing. Like Linux, not
 * after a STALL (cannot leave the buffer busy) and not for interrupt
 * endpoints. Called with e->busy clear (it issues a control transfer to the
 * high-speed hub, which never recurses here). */
static void clear_tt_buffer(usb_device_t *dev, int epnum, int type, int in) {
    usb_device_t *hub = dev->tt_hub;
    if (dev->speed == USB_SPEED_HIGH || !hub || !hub->alive) return;
    u16 tt = hub->hub_multi_tt ? dev->tt_port : 1;
    /* wValue: endpoint 3..0, device address 10..4, type 12..11, IN = bit 15 */
    u16 v = (u16)((epnum & 0xF) | ((dev->address & 0x7F) << 4) | ((type & 3) << 11));
    u8 rt = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER;
    if (type == USB_EP_CONTROL)   /* control: clear both directions */
        usb_control_timeout(hub, rt, 8 /* CLEAR_TT_BUFFER */, (u16)(v | (in ? 0 : 0x8000)), tt, NULL, 0, 1000);
    usb_control_timeout(hub, rt, 8 /* CLEAR_TT_BUFFER */, (u16)(v | (in ? 0x8000 : 0)), tt, NULL, 0, 1000);
}

static int ehci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *s, void *data, int timeout_ms) {
    ehci_t *e = hc->priv;
    if (e->dead) return E_IO;
    if (e->busy) return E_BUSY;
    int len = s->wLength, in = (s->bmRequestType & 0x80) != 0;
    int mps = dev->ep0.mps ? dev->ep0.mps : 64;
    u32 cap = QH_MULT(1);
    if (qh_tt(dev, &cap) < 0) return E_NOTSUP;
    if (len && !data) return E_INVAL;
    ehci_queue_t *qu = dev->ep0.hc_priv;
    if (!qu) {
        qu = queue_create(NQTD_CTRL);
        if (!qu) return E_NOMEM;
        dev->ep0.hc_priv = qu;
    }
    e->busy = 1;
    /* address and ep0 max packet size are read fresh for every transfer */
    queue_set_chars(e, qu, async_ep_char(dev, 0, mps, 1), cap);

    memcpy(e->setup_buf, s, 8);
    int n = 0;
    ehci_qtd_t *t = qslot(qu, n++);
    qtd_fill(t, e->setup_buf, 8, mps);
    t->alt = LP_T;
    t->token = QTD_PID_SETUP | QTD_CERR(3) | QTD_LEN(8);
    /* data stage starts with DATA1; every qTD but the last moves whole packets */
    int toggle = 1, off = 0, first_data = n;
    u8 *p = data;
    while (off < len && n < NQTD_CTRL - 2) {
        t = qslot(qu, n++);
        int c = qtd_fill(t, p + off, len - off, mps);
        t->token = QTD_ACTIVE | (in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR(3) | QTD_LEN(c) | (toggle ? QTD_DT : 0);
        toggle ^= ((c + mps - 1) / mps) & 1;
        off += c;
    }
    if (off < len) { e->busy = 0; return E_INVAL; }
    /* status stage: opposite direction (IN when there is no data), DATA1 */
    int status_idx = n;
    ehci_qtd_t *st = qslot(qu, n++);
    qtd_fill(st, NULL, 0, mps);
    st->alt = LP_T;
    st->token = QTD_ACTIVE | ((in && len) ? QTD_PID_OUT : QTD_PID_IN) | QTD_CERR(3) | QTD_IOC | QTD_DT;
    for (int i = first_data; i < status_idx; i++) qslot(qu, i)->alt = in ? phys(st) : LP_T;

    int r = queue_run(e, dev, qu, n, status_idx, time_ms() + (u64)timeout_ms);
    e->busy = 0;
    if (r == E_IO || r == E_TIMEOUT) clear_tt_buffer(dev, 0, USB_EP_CONTROL, in);
    return r;
}

static int ehci_bulk(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms) {
    ehci_t *e = hc->priv;
    if (e->dead) return E_IO;
    if (e->busy) return E_BUSY;
    if (ep->type != USB_EP_BULK) return E_INVAL;
    int in = usb_ep_is_in(ep);
    int mps = ep->mps ? ep->mps : 512;
    u32 cap = QH_MULT(1);
    if (qh_tt(dev, &cap) < 0) return E_NOTSUP;
    if (len < 0 || (len && !data)) return E_INVAL;
    ehci_queue_t *qu = ep->hc_priv;
    if (!qu) {
        qu = queue_create(NQTD_BULK);
        if (!qu) return E_NOMEM;
        /* QH-managed data toggle (DTC=0), starting from the software state */
        qu->qh->token = ep->toggle ? QTD_DT : 0;
        ep->hc_priv = qu;
    }
    e->busy = 1;
    queue_set_chars(e, qu, async_ep_char(dev, usb_ep_num(ep), mps, 0), cap);

    u64 deadline = time_ms() + (u64)timeout_ms;
    u8 *p = data;
    int total = 0, r = 0;
    do {
        /* one batch of up to NQTD_BULK-1 qTDs. IN: a short packet makes the
         * HC follow alt to the terminator, where the queue stops. */
        int n = 0, chunk = 0;
        do {
            ehci_qtd_t *t = qslot(qu, n);
            int c = qtd_fill(t, p + chunk, len - total - chunk, mps);
            t->token = (n ? QTD_ACTIVE : 0) | (in ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR(3) | QTD_LEN(c);
            chunk += c;
            n++;
        } while (total + chunk < len && n < NQTD_BULK - 1);
        qslot(qu, n - 1)->token |= QTD_IOC;
        u32 alt = in ? phys(qslot(qu, n)) : LP_T;
        for (int i = 0; i < n; i++) qslot(qu, i)->alt = alt;
        r = queue_run(e, dev, qu, n, -1, deadline);
        if (r < 0) break;
        total += r;
        p += r;
        if (r < chunk || chunk == 0) break;   /* short transfer or ZLP */
    } while (total < len);
    ep->toggle = (qu->qh->token & QTD_DT) ? 1 : 0;   /* informational only */
    e->busy = 0;
    if (r == E_IO || r == E_TIMEOUT) clear_tt_buffer(dev, usb_ep_num(ep), USB_EP_BULK, in);
    return r < 0 ? r : total;
}

/* ---------- periodic schedule (interrupt transfers) ---------- */

/* wait until the HC has certainly finished with unlinked periodic QHs */
static void periodic_wait(ehci_t *e) {
    u32 sts = op_rd(e, USBSTS);
    if ((sts & STS_HALTED) || !(sts & STS_PSS)) return;
    u32 f0 = op_rd(e, FRINDEX) >> 3;
    u64 deadline = time_ms() + 200;   /* emulators may only advance FRINDEX lazily */
    while ((((op_rd(e, FRINDEX) >> 3) - f0) & 0x7FF) < 2) {
        if (time_ms() > deadline) { kprintf("ehci: frame index stuck\n"); break; }
        udelay(20);
    }
}

static void intr_destroy(ehci_t *e, usb_endpoint_t *ep) {
    ehci_intr_t *x = ep->hc_priv;
    if (!x) return;
    ehci_qh_t *p = e->anchor[x->slot];
    for (int guard = 0; p && guard < 1024; guard++) {
        u32 nx = p->horiz;
        if (nx & LP_T) break;
        if ((nx & LP_MASK) == phys(x->qh)) { p->horiz = x->qh->horiz; break; }
        p = phys_to_virt(nx & LP_MASK);
    }
    mb();
    periodic_wait(e);
    if (x->pending) {
        /* a transfer that completed while being cancelled still moved a packet */
        ehci_qtd_t *t = &x->td[x->cur];
        u32 tok = t->token;
        if (!(tok & (QTD_ACTIVE | QTD_HALTED)))
            toggle_advance(ep, x->len, x->len - (int)QTD_GET_LEN(tok), ep->mps ? ep->mps : 8);
    }
    dma_free(x->td);
    dma_free(x->qh);
    kfree(x);
    ep->hc_priv = NULL;
}

static ehci_intr_t *intr_create(ehci_t *e, usb_device_t *dev, usb_endpoint_t *ep) {
    u32 cap = 0;
    if (qh_tt(dev, &cap) < 0) return NULL;
    int slot = 0;
    u32 smask = 0x01, cmask = 0, mult = 1;
    int iv = ep->interval ? ep->interval : 1;
    if (dev->speed == USB_SPEED_HIGH) {
        if (iv > 16) iv = 16;
        int uframes = 1 << (iv - 1);
        if (uframes == 1) smask = 0xFF;
        else if (uframes == 2) smask = 0x55;
        else if (uframes == 4) smask = 0x11;
        else for (int ms = uframes / 8; ms > 1 && slot < NANCHOR - 1; ms >>= 1) slot++;
        mult = 1u + (ep->mult & 3);
        if (mult > 3) mult = 3;
    } else {
        /* split transaction: start-split in uframe 0, complete-splits in 2..4 */
        smask = 0x01;
        cmask = 0x1C;
        for (int ms = iv; ms > 1 && slot < NANCHOR - 1; ms >>= 1) slot++;
    }
    ehci_intr_t *x = kzalloc(sizeof *x);
    if (!x) return NULL;
    x->qh = dma_alloc(sizeof(ehci_qh_t), 128);
    x->td = dma_alloc(2 * sizeof(ehci_qtd_t), 64);
    if (!x->qh || !x->td) { dma_free(x->qh); dma_free(x->td); kfree(x); return NULL; }
    x->slot = slot;
    ehci_qh_t *qh = x->qh;
    qh->ep_char = (dev->address & 0x7F) | ((u32)usb_ep_num(ep) << 8) | (qh_speed(dev) << 12) |
                  QH_DTC | ((u32)(ep->mps & 0x7FF) << 16);
    qh->ep_cap = cap | smask | (cmask << 8) | QH_MULT(mult);
    for (int i = 0; i < 2; i++) { x->td[i].next = LP_T; x->td[i].alt = LP_T; x->td[i].token = 0; }
    qh->cur = 0;
    qh->next = phys(&x->td[0]);
    qh->alt = LP_T;
    qh->token = 0;
    ehci_qh_t *a = e->anchor[slot];
    qh->horiz = a->horiz;
    mb();
    a->horiz = phys(qh) | LP_QH;
    mb();
    return x;
}

static int ehci_intr_start(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len) {
    ehci_t *e = hc->priv;
    if (e->dead) return E_IO;
    if (ep->type != USB_EP_INTR) return E_INVAL;
    ehci_intr_t *x = ep->hc_priv;
    if (x && x->pending) return E_BUSY;
    int mps = ep->mps ? ep->mps : 8;
    if (len < 0 || len > mps * (1 + (ep->mult & 3)) || (len && !buf)) return E_INVAL;
    if (!x) {
        x = intr_create(e, dev, ep);
        if (!x) return E_NOMEM;
        ep->hc_priv = x;
    }
    ehci_qtd_t *t = &x->td[x->cur], *nt = &x->td[x->cur ^ 1];
    /* the other qTD becomes the new (inactive) terminator */
    nt->token = 0;
    nt->next = LP_T;
    nt->alt = LP_T;
    qtd_fill(t, buf, len, mps);
    t->next = phys(nt);
    t->alt = LP_T;
    x->len = len;
    wmb();
    t->token = QTD_ACTIVE | (usb_ep_is_in(ep) ? QTD_PID_IN : QTD_PID_OUT) | QTD_CERR(3) | QTD_IOC |
               QTD_LEN(len) | (ep->toggle ? QTD_DT : 0);
    mb();
    x->pending = 1;
    return 0;
}

static int ehci_intr_poll(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    ehci_t *e = hc->priv;
    ehci_intr_t *x = ep->type == USB_EP_INTR ? ep->hc_priv : NULL;
    if (!x || !x->pending) return E_INVAL;
    ehci_qtd_t *t = &x->td[x->cur];
    u32 tok = t->token;
    if (tok & QTD_ACTIVE) {
        if (e->dead) { intr_destroy(e, ep); return E_IO; }
        return E_AGAIN;
    }
    x->pending = 0;
    if (tok & QTD_HALTED) {
        /* the QH is halted now: drop it, intr_start builds a fresh one */
        int r = qtd_error(tok);
        intr_destroy(e, ep);
        if (r == E_IO && dev_gone(e, dev)) r = E_NODEV;
        return r;
    }
    int done = x->len - (int)QTD_GET_LEN(tok);
    toggle_advance(ep, x->len, done, ep->mps ? ep->mps : 8);
    x->cur ^= 1;
    return done;
}

static void ehci_intr_cancel(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    if (ep->type == USB_EP_INTR) intr_destroy(hc->priv, ep);
}

/* ---------- device / endpoint management ---------- */
static int ehci_device_init(usb_hc_t *hc, usb_device_t *dev) {
    u32 cap = 0;
    if (qh_tt(dev, &cap) < 0) {
        kprintf("ehci: %s device without transaction translator\n", usb_speed_name(dev->speed));
        return E_NOTSUP;
    }
    return 0;
}

/* address / ep0 max packet size are picked up by the next control transfer */
static int ehci_device_update(usb_hc_t *hc, usb_device_t *dev) { return 0; }

static void ehci_device_free(usb_hc_t *hc, usb_device_t *dev) {
    ehci_t *e = hc->priv;
    /* periodic QHs */
    for (int i = 1; i < 32; i++) {
        usb_endpoint_t *ep = dev->eps[i];
        if (ep && ep->hc_priv && ep->type == USB_EP_INTR) intr_destroy(e, ep);
    }
    /* async QHs: unlink them all, one doorbell, then free */
    int unlinked = 0;
    for (int i = 0; i < 32; i++) {
        usb_endpoint_t *ep = i ? dev->eps[i] : &dev->ep0;
        if (!ep || !ep->hc_priv) continue;
        ehci_queue_t *qu = ep->hc_priv;
        if (qu->linked && async_unlink_nowait(e, qu->qh)) unlinked = 1;
        qu->linked = 0;
    }
    if (unlinked && iaa_wait(e) < 0) {
        /* the HC may still reference them: leak rather than corrupt */
        for (int i = 0; i < 32; i++) {
            usb_endpoint_t *ep = i ? dev->eps[i] : &dev->ep0;
            if (ep) ep->hc_priv = NULL;
        }
        return;
    }
    for (int i = 0; i < 32; i++) {
        usb_endpoint_t *ep = i ? dev->eps[i] : &dev->ep0;
        if (!ep || !ep->hc_priv) continue;
        queue_free(ep->hc_priv);
        ep->hc_priv = NULL;
    }
}

/* clear the toggle of an endpoint's persistent state to DATA0 */
static void ep_toggle_reset(ehci_t *e, usb_endpoint_t *ep) {
    ep->toggle = 0;
    if (ep->type == USB_EP_BULK && ep->hc_priv) {
        /* QH-managed toggle: the overlay may only be modified while the QH
         * is out of the schedule */
        ehci_queue_t *qu = ep->hc_priv;
        queue_unlink(e, qu);
        qu->qh->token &= ~QTD_DT;
        mb();
    }
    /* interrupt endpoints use DTC=1 with ep->toggle */
}

static int ehci_configure(usb_hc_t *hc, usb_device_t *dev) {
    for (int i = 1; i < 32; i++) if (dev->eps[i]) ep_toggle_reset(hc->priv, dev->eps[i]);
    return 0;
}

static int ehci_reset_ep(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    ep_toggle_reset(hc->priv, ep);
    return 0;
}

/* ---------- root hub ---------- */
static void port_clear_changes(ehci_t *e, int p) {
    u32 v = port_rd(e, p);
    if (v & PORT_W1C) port_wr(e, p, v);   /* PE written back as read: no-op */
}

static void port_release(ehci_t *e, int p, const char *why) {
    if (e->ncc == 0) {
        /* HCSPARAMS N_CC = 0: "port-ownership hand-off is not supported, only
         * high-speed devices are supported on the root ports" (EHCI 2.2.3).
         * Do not set PORT_OWNER: on controllers that implement it anyway it
         * would route the port to nothing. (Intel 5/6-series root ports carry
         * an integrated rate-matching hub, which is high-speed.) */
        kprintf("ehci: port %d: %s device, but no companion controller (n_cc 0)\n", p + 1, why);
        e->port_failed[p] = 1;
        return;
    }
    u32 v = port_rd(e, p);
    port_wr(e, p, (v & ~PORT_W1C) | PORT_OWNER);
    mdelay(2);
    v = port_rd(e, p);
    if (!(v & PORT_OWNER)) {
        kprintf("ehci: port %d: %s device, but no companion controller (n_cc %d)\n", p + 1, why, e->ncc);
        e->port_failed[p] = 1;
    } else {
        kprintf("ehci: port %d: %s device, handed over to companion controller\n", p + 1, why);
    }
}

/* returns 1 if the port is enabled (high-speed device), 0 if not, -1 if gone */
static int port_reset(ehci_t *e, int p) {
    u32 v = port_rd(e, p);
    port_wr(e, p, (v & ~(PORT_W1C | PORT_PE)) | PORT_PR);
    mdelay(50);
    v = port_rd(e, p);
    port_wr(e, p, v & ~(PORT_W1C | PORT_PR));
    u64 deadline = time_ms() + 20;
    while ((v = port_rd(e, p)) & PORT_PR) {
        if (time_ms() > deadline) { kprintf("ehci: port %d: reset did not complete\n", p + 1); return -1; }
        udelay(100);
    }
    udelay(500);
    v = port_rd(e, p);
    if (!(v & PORT_CCS)) return -1;
    return (v & PORT_PE) ? 1 : 0;
}

static void port_detach(ehci_t *e, int p) {
    if (e->port_dev[p]) {
        e->port_dev[p] = NULL;
        usb_port_disconnected(&e->hc, NULL, p + 1);
    }
}

static void ehci_poll(usb_hc_t *hc) {
    ehci_t *e = hc->priv;
    if (e->dead) return;
    for (int p = 0; p < e->nports; p++) {
        u32 v = port_rd(e, p);
        if (v & PORT_OWNER) {
            /* the companion owns it until the device is unplugged; ownership
             * then returns to us automatically */
            port_detach(e, p);
            e->port_failed[p] = 0;
            if (v & PORT_W1C) port_clear_changes(e, p);
            continue;
        }
        if (v & PORT_CSC) {
            port_wr(e, p, (v & ~PORT_W1C) | PORT_CSC);
            port_detach(e, p);
            e->port_failed[p] = 0;
        }
        if (v & (PORT_PEC | PORT_OCC)) port_wr(e, p, (v & ~PORT_W1C) | (v & (PORT_PEC | PORT_OCC)));
        if (!(v & PORT_CCS)) {
            port_detach(e, p);
            e->port_failed[p] = 0;
            continue;
        }
        if (e->port_dev[p]) {
            if (!(v & PORT_PE)) {
                kprintf("ehci: port %d disabled by the controller\n", p + 1);
                port_detach(e, p);
                e->port_failed[p] = 1;
            }
            continue;
        }
        if (e->port_failed[p]) continue;

        mdelay(100);   /* debounce */
        v = port_rd(e, p);
        if (!(v & PORT_CCS) || (v & PORT_OWNER)) continue;
        if (((v >> PORT_LS_SHIFT) & 3) == 1) {   /* K-state: low-speed device */
            port_release(e, p, "low-speed");
            continue;
        }
        int r = port_reset(e, p);
        if (r < 0) { port_clear_changes(e, p); continue; }
        if (r == 0) { port_release(e, p, "full-speed"); continue; }
        mdelay(10);    /* reset recovery */
        port_clear_changes(e, p);
        e->port_dev[p] = usb_attach_device(hc, NULL, p + 1, USB_SPEED_HIGH);
        if (!e->port_dev[p]) e->port_failed[p] = 1;
        port_clear_changes(e, p);
    }
}

static const usb_hc_ops_t ehci_ops = {
    .device_init = ehci_device_init,
    .device_update = ehci_device_update,
    .device_free = ehci_device_free,
    .control = ehci_control,
    .configure = ehci_configure,
    .bulk = ehci_bulk,
    .intr_start = ehci_intr_start,
    .intr_poll = ehci_intr_poll,
    .intr_cancel = ehci_intr_cancel,
    .reset_endpoint = ehci_reset_ep,
    .poll = ehci_poll,
};

/* ---------- controller setup ---------- */
static void bios_handoff(pci_dev_t *pd, u32 hcc) {
    u32 off = HCC_EECP(hcc);
    for (int guard = 0; off >= 0x40 && guard < 32; guard++) {
        u32 cap = pci_read32(pd, (u8)off);
        if ((cap & 0xFF) == 1) {        /* USB legacy support */
            pci_write8(pd, (u8)(off + 3), 1);          /* HC OS owned semaphore */
            u64 deadline = time_ms() + 1000;
            while (pci_read32(pd, (u8)off) & (1u << 16)) {
                if (time_ms() > deadline) {
                    kprintf("ehci: BIOS did not release the controller, taking it\n");
                    pci_write8(pd, (u8)(off + 2), 0);
                    break;
                }
                mdelay(1);
            }
            /* disable all SMIs, clear SMI status */
            pci_write32(pd, (u8)(off + 4), 0xE0000000u);
            return;
        }
        off = (cap >> 8) & 0xFF;
    }
}

static ehci_qh_t *alloc_dummy_qh(void) {
    ehci_qh_t *qh = dma_alloc(sizeof(ehci_qh_t), 128);
    if (!qh) return NULL;
    qh->ep_char = (QH_EPS_HIGH << 12) | (64u << 16);
    qh->ep_cap = QH_MULT(1);
    qh->next = LP_T;
    qh->alt = LP_T;
    qh->token = QTD_HALTED;     /* never executes */
    return qh;
}

static void ehci_setup(pci_dev_t *pd) {
    if (pd->bar[0].is_io || !pd->bar[0].addr) { kprintf("ehci: no MMIO BAR\n"); return; }
    pci_enable(pd);
    volatile u8 *cap = pci_map_bar(pd, 0);
    if (!cap) return;
    u8 caplen = mmio_read8(cap + CAP_LENGTH);
    u32 hcs = mmio_read32(cap + CAP_HCSPARAMS), hcc = mmio_read32(cap + CAP_HCCPARAMS);
    if (caplen < 0x10 || hcs == 0xFFFFFFFF) { kprintf("ehci: bad capability registers\n"); return; }

    bios_handoff(pd, hcc);

    ehci_t *e = kzalloc(sizeof *e);
    if (!e) return;
    e->cap = cap;
    e->op = cap + caplen;
    e->hcs = hcs;
    e->hcc = hcc;
    e->nports = HCS_NPORTS(hcs);
    if (e->nports > 16) e->nports = 16;
    e->ncc = HCS_NCC(hcs);

    /* stop and reset */
    op_wr(e, USBINTR, 0);
    op_wr(e, USBCMD, op_rd(e, USBCMD) & ~(CMD_RS | CMD_PSE | CMD_ASE));
    for (int i = 0; i < 50 && !(op_rd(e, USBSTS) & STS_HALTED); i++) mdelay(1);
    op_wr(e, USBCMD, CMD_HCRESET);
    int ok = 0;
    for (int i = 0; i < 500; i++) {
        if (!(op_rd(e, USBCMD) & CMD_HCRESET)) { ok = 1; break; }
        mdelay(1);
    }
    if (!ok) { kprintf("ehci: controller reset timed out\n"); kfree(e); return; }
    if (HCC_64BIT(hcc)) op_wr(e, CTRLDSSEGMENT, 0);
    op_wr(e, USBINTR, 0);
    op_wr(e, USBSTS, 0x3F);

    /* schedules */
    e->frame_list = dma_alloc(4096, 4096);
    e->async_head = alloc_dummy_qh();
    e->setup_buf = dma_alloc(64, 64);
    for (int k = 0; k < NANCHOR; k++) e->anchor[k] = alloc_dummy_qh();
    for (int k = 0; k < NANCHOR; k++) if (!e->anchor[k]) e->frame_list = NULL;
    if (!e->frame_list || !e->async_head || !e->setup_buf) {
        kprintf("ehci: out of memory\n");
        return;
    }
    e->async_head->ep_char |= QH_HEAD;
    e->async_head->horiz = phys(e->async_head) | LP_QH;
    /* anchor[k] (period 2^k ms) -> anchor[k-1] -> ... -> anchor[0] -> end */
    for (int k = 0; k < NANCHOR; k++)
        e->anchor[k]->horiz = k ? (phys(e->anchor[k - 1]) | LP_QH) : LP_T;
    for (int i = 0; i < 1024; i++) {
        int k = 0;
        while (k < NANCHOR - 1 && !(i & (1 << k))) k++;
        e->frame_list[i] = phys(e->anchor[k]) | LP_QH;
    }
    mb();
    op_wr(e, PERIODICLIST, phys(e->frame_list));
    op_wr(e, ASYNCLIST, phys(e->async_head));
    op_wr(e, FRINDEX, 0);
    op_wr(e, USBCMD, CMD_ITC(1) | CMD_PSE | CMD_ASE | CMD_RS);   /* 1024-entry frame list */
    ok = 0;
    for (int i = 0; i < 100; i++) {
        if (!(op_rd(e, USBSTS) & STS_HALTED)) { ok = 1; break; }
        mdelay(1);
    }
    if (!ok) { kprintf("ehci: controller did not start\n"); return; }
    op_wr(e, CONFIGFLAG, 1);        /* route all ports to EHCI */
    mdelay(5);
    if (HCS_PPC(hcs)) {
        for (int p = 0; p < e->nports; p++) {
            u32 v = port_rd(e, p);
            if (!(v & PORT_PP)) port_wr(e, p, (v & ~PORT_W1C) | PORT_PP);
        }
        mdelay(20);
    }
    for (int i = 0; i < 100 && (op_rd(e, USBSTS) & (STS_PSS | STS_ASS)) != (STS_PSS | STS_ASS); i++) mdelay(1);

    e->hc.name = "EHCI";
    snprintf(e->hc.desc, sizeof e->hc.desc, "%s EHCI (%02x:%02x.%x)", pci_vendor_name(pd->vendor), pd->bus, pd->dev, pd->func);
    e->hc.ops = &ehci_ops;
    e->hc.priv = e;
    e->hc.pci = pd;
    e->hc.nports = (u8)e->nports;
    pd->driver = "ehci";
    kprintf("ehci: %s: %d ports, %d companion controller(s)%s%s\n", e->hc.desc, e->nports, e->ncc,
            HCC_64BIT(hcc) ? ", 64-bit" : "", HCS_PPC(hcs) ? ", port power control" : "");
    usb_register_hc(&e->hc);
}

void ehci_init(void) {
    for (int i = 0;; i++) {
        pci_dev_t *pd = pci_find_class(0x0C, 0x03, 0x20, i);
        if (!pd) break;
        ehci_setup(pd);
    }
}
