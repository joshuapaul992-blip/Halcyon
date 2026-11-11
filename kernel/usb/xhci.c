/* Halcyon OS - xHCI (USB 3.x) host controller driver, polled
 *
 * Implements usb_hc_ops_t (see usb.h) for eXtensible Host Controllers as
 * described by the xHCI 1.2 specification. Everything is polled: the event
 * ring is drained from every wait loop and from the root hub poll, and a
 * single dispatcher records the outcome of commands and of transfers per
 * (slot, DCI), so interrupt transfers of several devices, command
 * completions and port events can arrive in any order.
 *
 * Memory layout per controller: DCBAA, scratchpad buffers, one command
 * ring, one event ring segment (interrupter 0, IMAN.IE off). Per device:
 * an input and an output device context, and one transfer ring per
 * endpoint. Every ring is a single 4 KiB segment closed by a link TRB with
 * Toggle Cycle set.
 */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/kernel.h"

/* ---------- registers ---------- */
/* capability registers */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* operational registers */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_DNCTRL   0x14
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))

#define CMD_RS      (1u << 0)
#define CMD_HCRST   (1u << 1)
#define CMD_INTE    (1u << 2)
#define CMD_HSEE    (1u << 3)

#define STS_HCH     (1u << 0)
#define STS_HSE     (1u << 2)
#define STS_EINT    (1u << 3)
#define STS_PCD     (1u << 4)
#define STS_CNR     (1u << 11)
#define STS_HCE     (1u << 12)

#define CRCR_RCS    (1u << 0)
#define CRCR_CS     (1u << 1)
#define CRCR_CA     (1u << 2)
#define CRCR_CRR    (1u << 3)

/* PORTSC */
#define PORT_CCS    (1u << 0)
#define PORT_PED    (1u << 1)
#define PORT_OCA    (1u << 3)
#define PORT_PR     (1u << 4)
#define PORT_PLS_SHIFT 5
#define PORT_PLS_MASK  (0xFu << 5)
#define PORT_PP     (1u << 9)
#define PORT_SPEED(v) (((v) >> 10) & 0xF)
#define PORT_PIC_MASK (3u << 14)
#define PORT_LWS    (1u << 16)
#define PORT_CSC    (1u << 17)
#define PORT_PEC    (1u << 18)
#define PORT_WRC    (1u << 19)
#define PORT_OCC    (1u << 20)
#define PORT_PRC    (1u << 21)
#define PORT_PLC    (1u << 22)
#define PORT_CEC    (1u << 23)
#define PORT_WCE    (1u << 25)
#define PORT_WDE    (1u << 26)
#define PORT_WOE    (1u << 27)
#define PORT_WPR    (1u << 31)
#define PORT_CHANGE (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)
/* bits that must be written back unchanged (RWS); every RW1C bit, PED and
 * the strobe bits (PR, LWS, WPR) are written as 0 unless explicitly set */
#define PORT_PRESERVE (PORT_PP | PORT_PIC_MASK | PORT_WCE | PORT_WDE | PORT_WOE)

#define PLS_U0       0
#define PLS_U3       3
#define PLS_DISABLED 4
#define PLS_RXDETECT 5
#define PLS_INACTIVE 6
#define PLS_POLLING  7
#define PLS_RECOVERY 8
#define PLS_COMPLIANCE 10

/* runtime registers: interrupter 0 */
#define RT_IR0      0x20
#define IR_IMAN     0x00
#define IR_IMOD     0x04
#define IR_ERSTSZ   0x08
#define IR_ERSTBA   0x10
#define IR_ERDP     0x18
#define ERDP_EHB    (1u << 3)

/* extended capabilities */
#define XCAP_LEGACY   1
#define XCAP_PROTOCOL 2
#define LEGSUP_BIOS   (1u << 16)
#define LEGSUP_OS     (1u << 24)

/* ---------- TRBs ---------- */
typedef struct trb { volatile u32 p0, p1, status, control; } trb_t;

#define TRB_C       (1u << 0)
#define TRB_TC      (1u << 1)     /* link: toggle cycle */
#define TRB_ENT     (1u << 1)
#define TRB_ISP     (1u << 2)
#define TRB_CH      (1u << 4)
#define TRB_IOC     (1u << 5)
#define TRB_IDT     (1u << 6)
#define TRB_BSR     (1u << 9)     /* address device: block SET_ADDRESS */
#define TRB_DC      (1u << 9)     /* configure endpoint: deconfigure */
#define TRB_TSP     (1u << 9)     /* reset endpoint: transfer state preserve */
#define TRB_DIR_IN  (1u << 16)
#define TRB_TYPE(t) ((u32)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)
#define TRB_SLOT(s) ((u32)(s) << 24)
#define TRB_EP(e)   ((u32)(e) << 16)

enum {
    TT_NORMAL = 1, TT_SETUP = 2, TT_DATA = 3, TT_STATUS = 4, TT_ISOCH = 5, TT_LINK = 6,
    TT_EVENT_DATA = 7, TT_NOOP = 8,
    TT_ENABLE_SLOT = 9, TT_DISABLE_SLOT = 10, TT_ADDRESS_DEVICE = 11, TT_CONFIGURE_EP = 12,
    TT_EVALUATE_CTX = 13, TT_RESET_EP = 14, TT_STOP_EP = 15, TT_SET_TR_DEQ = 16,
    TT_RESET_DEVICE = 17, TT_NOOP_CMD = 23,
    TT_EV_TRANSFER = 32, TT_EV_CMD_COMPLETE = 33, TT_EV_PORT_STATUS = 34, TT_EV_BANDWIDTH = 35,
    TT_EV_DOORBELL = 36, TT_EV_HOST_CTRL = 37, TT_EV_DEVICE_NOTIFY = 38, TT_EV_MFINDEX_WRAP = 39,
};

enum {
    CC_INVALID = 0, CC_SUCCESS = 1, CC_DATA_BUFFER = 2, CC_BABBLE = 3, CC_TRANSACTION = 4,
    CC_TRB = 5, CC_STALL = 6, CC_RESOURCE = 7, CC_BANDWIDTH = 8, CC_NO_SLOTS = 9,
    CC_SLOT_NOT_ENABLED = 11, CC_EP_NOT_ENABLED = 12, CC_SHORT_PACKET = 13,
    CC_PARAMETER = 17, CC_CONTEXT_STATE = 19, CC_EVENT_RING_FULL = 21,
    CC_CMD_RING_STOPPED = 24, CC_CMD_ABORTED = 25, CC_STOPPED = 26,
    CC_STOPPED_LEN_INVALID = 27, CC_STOPPED_SHORT = 28, CC_SPLIT_TRANSACTION = 36,
};

/* endpoint context states */
enum { EPS_DISABLED = 0, EPS_RUNNING = 1, EPS_HALTED = 2, EPS_STOPPED = 3, EPS_ERROR = 4 };

#define RING_TRBS   256           /* one 4 KiB segment, last entry is the link TRB */
#define EVT_TRBS    256
#define CMD_TIMEOUT 5000
#define BULK_TD_MAX (1024 * 1024) /* bytes per TD (<= 18 TRBs) */

/* ---------- driver state ---------- */
typedef struct ring {
    trb_t *trb;
    u64 phys;
    u32 enq;
    u32 cycle;
} ring_t;

typedef struct xep {
    ring_t ring;
    usb_endpoint_t *uep;      /* NULL for ep0 */
    u8 dci;
    u8 kind;                  /* USB_EP_* */
    /* current TD (one outstanding per endpoint) */
    u8 active;
    u8 done;
    u8 cc;
    u8 have_bytes;
    u32 td_start, td_end;     /* TRB indices: first and last TRB of the TD */
    u32 td_len;               /* data bytes queued */
    int bytes;                /* data bytes transferred (valid if have_bytes) */
} xep_t;

typedef struct xslot {
    u8 id;
    u8 max_dci;
    u16 mps0;
    u8 *in_ctx;
    u8 *out_ctx;
    xep_t *ep[32];            /* by DCI */
    usb_device_t *dev;
} xslot_t;

typedef struct xproto {
    u8 major, minor, first, count, slot_type, psic;
    u32 psi[16];
} xproto_t;

typedef struct xhci {
    usb_hc_t hc;
    pci_dev_t *pci;
    volatile u8 *cap, *op, *rt, *db;
    u32 hcs1, hcs2, hcc1;
    u16 version;
    u32 csz;                  /* context size: 32 or 64 */
    int max_slots;
    int nports;
    u64 *dcbaa;
    u64 *spad_array;
    void **spad_bufs;
    int nspad;
    ring_t cmd;
    trb_t *evt;
    u64 *erst;
    u32 evt_deq, evt_cycle;
    /* the outstanding command (commands are issued one at a time) */
    u64 cmd_ptr;
    u8 cmd_done, cmd_cc, cmd_slot;
    xslot_t *slots[256];
    xproto_t proto[8];
    int nproto;
    u8 port_proto[256];       /* index + 1 into proto[], 0 = unknown */
    usb_device_t *port_dev[256];
    u8 port_failed[256];
    u8 port_warm[256];        /* warm reset attempted on an unconnected, errored USB3 link */
    u8 dead;
    u8 dead_handled;
    u8 in_poll;
} xhci_t;

/* ---------- helpers ---------- */
static u32 lo32(u64 v) { return (u32)v; }
static u32 hi32(u64 v) { return (u32)(v >> 32); }

static u32 op_read(xhci_t *x, u32 off) { return mmio_read32(x->op + off); }
static void op_write(xhci_t *x, u32 off, u32 v) { mmio_write32(x->op + off, v); }
static void op_write64(xhci_t *x, u32 off, u64 v) {
    mmio_write32(x->op + off, lo32(v));
    mmio_write32(x->op + off + 4, hi32(v));
}
static u32 ir_read(xhci_t *x, u32 off) { return mmio_read32(x->rt + RT_IR0 + off); }
static void ir_write(xhci_t *x, u32 off, u32 v) { mmio_write32(x->rt + RT_IR0 + off, v); }
static void ir_write64(xhci_t *x, u32 off, u64 v) {
    mmio_write32(x->rt + RT_IR0 + off, lo32(v));
    mmio_write32(x->rt + RT_IR0 + off + 4, hi32(v));
}
static void doorbell(xhci_t *x, u32 slot, u32 target) {
    wmb();
    mmio_write32(x->db + 4 * slot, target);
}
static u32 portsc(xhci_t *x, int p) { return op_read(x, OP_PORTSC(p)); }
static void portsc_write(xhci_t *x, int p, u32 v) { op_write(x, OP_PORTSC(p), v); }

static const char *cc_name(int cc) {
    switch (cc) {
    case CC_SUCCESS: return "success";
    case CC_DATA_BUFFER: return "data buffer error";
    case CC_BABBLE: return "babble";
    case CC_TRANSACTION: return "transaction error";
    case CC_TRB: return "TRB error";
    case CC_STALL: return "stall";
    case CC_RESOURCE: return "resource error";
    case CC_BANDWIDTH: return "bandwidth error";
    case CC_NO_SLOTS: return "no slots";
    case CC_SLOT_NOT_ENABLED: return "slot not enabled";
    case CC_EP_NOT_ENABLED: return "endpoint not enabled";
    case CC_SHORT_PACKET: return "short packet";
    case CC_PARAMETER: return "parameter error";
    case CC_CONTEXT_STATE: return "context state error";
    case CC_CMD_ABORTED: return "command aborted";
    case CC_STOPPED: return "stopped";
    case CC_SPLIT_TRANSACTION: return "split transaction error";
    }
    return "error";
}

static int cc_to_err(int cc) {
    switch (cc) {
    case CC_SUCCESS: case CC_SHORT_PACKET: return 0;
    case CC_STALL: return E_STALL;
    case CC_SLOT_NOT_ENABLED: case CC_EP_NOT_ENABLED: return E_NODEV;
    }
    return E_IO;
}

/* ---------- contexts ---------- */
static volatile u32 *in_ctrl(xhci_t *x, xslot_t *s) { return (volatile u32 *)s->in_ctx; }
static volatile u32 *in_slot(xhci_t *x, xslot_t *s) { return (volatile u32 *)(s->in_ctx + x->csz); }
static volatile u32 *in_ep(xhci_t *x, xslot_t *s, int dci) { return (volatile u32 *)(s->in_ctx + (u32)(dci + 1) * x->csz); }
static volatile u32 *out_slot(xhci_t *x, xslot_t *s) { return (volatile u32 *)s->out_ctx; }
static volatile u32 *out_ep(xhci_t *x, xslot_t *s, int dci) { return (volatile u32 *)(s->out_ctx + (u32)dci * x->csz); }

static void in_ctx_clear(xhci_t *x, xslot_t *s) { memset(s->in_ctx, 0, 33 * x->csz); }

static int ep_state(xhci_t *x, xslot_t *s, int dci) {
    rmb();
    return out_ep(x, s, dci)[0] & 7;
}

/* ---------- rings ---------- */
static int ring_alloc(ring_t *r) {
    r->trb = dma_alloc(RING_TRBS * sizeof(trb_t), 4096);
    if (!r->trb) return E_NOMEM;
    r->phys = virt_to_phys(r->trb);
    r->enq = 0;
    r->cycle = 1;
    trb_t *l = &r->trb[RING_TRBS - 1];
    l->p0 = lo32(r->phys);
    l->p1 = hi32(r->phys);
    l->status = 0;
    l->control = TRB_TYPE(TT_LINK) | TRB_TC;   /* cycle 0: not yet valid */
    return 0;
}

static u64 ring_enq_phys(ring_t *r) { return r->phys + (u64)r->enq * sizeof(trb_t); }

/* Write one TRB at the enqueue position. The cycle bit is written last;
 * with 'hold' the TRB is written with the inverted cycle bit so the HC does
 * not see it until ring_release() (used for the first TRB of a TD). */
static u32 ring_push(ring_t *r, u32 p0, u32 p1, u32 status, u32 control, int hold) {
    u32 idx = r->enq;
    trb_t *t = &r->trb[idx];
    t->p0 = p0;
    t->p1 = p1;
    t->status = status;
    wmb();
    t->control = (control & ~TRB_C) | (hold ? (r->cycle ^ 1) : r->cycle);
    if (++r->enq == RING_TRBS - 1) {
        trb_t *l = &r->trb[RING_TRBS - 1];
        /* the link TRB carries the chain bit of the TD it is part of */
        wmb();
        l->control = TRB_TYPE(TT_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return idx;
}

static void ring_release(ring_t *r, u32 idx) {
    wmb();
    r->trb[idx].control ^= TRB_C;
    wmb();
}

static u32 ring_next(u32 idx) { return (idx + 1 >= RING_TRBS - 1) ? 0 : idx + 1; }

/* ---------- event handling ---------- */
static void handle_transfer_event(xhci_t *x, const trb_t *ev) {
    u32 slot = ev->control >> 24, dci = (ev->control >> 16) & 0x1F;
    u32 cc = ev->status >> 24, residual = ev->status & 0xFFFFFF;
    u64 ptr = ((u64)ev->p1 << 32) | ev->p0;
    xslot_t *s = slot < 256 ? x->slots[slot] : NULL;
    xep_t *e = s ? s->ep[dci] : NULL;
    if (!e || !e->active || e->done) return;   /* stale or for a freed slot */
    if (ev->control & (1u << 2)) return;        /* event data TRB: not used */
    u64 off = ptr - e->ring.phys;
    if (ptr < e->ring.phys || off >= (RING_TRBS - 1) * sizeof(trb_t)) {
        /* no TRB pointer (e.g. ring underrun) or not ours: an error still ends the TD */
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) { e->cc = (u8)cc; e->done = 1; }
        return;
    }
    u32 idx = (u32)(off / sizeof(trb_t));
    /* is idx inside the current TD (indices wrap around the link TRB)? */
    u32 span = (e->td_end + RING_TRBS - 1 - e->td_start) % (RING_TRBS - 1);
    u32 pos = (idx + RING_TRBS - 1 - e->td_start) % (RING_TRBS - 1);
    if (pos > span) return;

    u32 type = TRB_GET_TYPE(e->ring.trb[idx].control);
    if (type == TT_NORMAL || type == TT_DATA) {
        /* bytes = all data TRBs before idx + this one minus the residue */
        int bytes = 0;
        for (u32 i = e->td_start; i != idx; i = ring_next(i)) {
            u32 t = TRB_GET_TYPE(e->ring.trb[i].control);
            if (t == TT_NORMAL || t == TT_DATA) bytes += e->ring.trb[i].status & 0x1FFFF;
        }
        u32 len = e->ring.trb[idx].status & 0x1FFFF;
        if (residual > len) residual = len;
        if (cc == CC_STOPPED_LEN_INVALID) residual = len;
        bytes += (int)(len - residual);
        e->bytes = bytes;
        e->have_bytes = 1;
    }
    if (cc == CC_SUCCESS) {
        if (idx == e->td_end) { e->cc = CC_SUCCESS; e->done = 1; }
    } else if (cc == CC_SHORT_PACKET) {
        /* a control transfer continues with its status stage */
        if (e->kind != USB_EP_CONTROL || idx == e->td_end) { e->cc = CC_SHORT_PACKET; e->done = 1; }
    } else {
        e->cc = (u8)cc;
        e->done = 1;
    }
}

static void handle_event(xhci_t *x, const trb_t *ev) {
    u32 type = TRB_GET_TYPE(ev->control);
    switch (type) {
    case TT_EV_TRANSFER:
        handle_transfer_event(x, ev);
        break;
    case TT_EV_CMD_COMPLETE: {
        u64 ptr = ((u64)ev->p1 << 32) | ev->p0;
        u32 cc = ev->status >> 24;
        if (x->cmd_ptr && ptr == x->cmd_ptr && !x->cmd_done) {
            x->cmd_cc = (u8)cc;
            x->cmd_slot = (u8)(ev->control >> 24);
            x->cmd_done = 1;
        }
        break;
    }
    case TT_EV_PORT_STATUS:
        /* root ports are polled through PORTSC */
        break;
    case TT_EV_HOST_CTRL:
        kprintf("xhci: host controller event, code %u (%s)\n", ev->status >> 24, cc_name(ev->status >> 24));
        break;
    default:
        break;
    }
}

static void process_events(xhci_t *x) {
    int n = 0;
    for (;;) {
        trb_t *ev = &x->evt[x->evt_deq];
        rmb();
        u32 ctl = ev->control;
        if ((ctl & TRB_C) != x->evt_cycle) break;
        rmb();
        handle_event(x, ev);
        n++;
        if (++x->evt_deq == EVT_TRBS) { x->evt_deq = 0; x->evt_cycle ^= 1; }
    }
    if (n) ir_write64(x, IR_ERDP, virt_to_phys(&x->evt[x->evt_deq]) | ERDP_EHB);
}

/* Host controller health: HSE / HCE / unexpected halt. Logged once. */
static int hc_check(xhci_t *x) {
    if (x->dead) return -1;
    u32 sts = op_read(x, OP_USBSTS);
    if (sts == 0xFFFFFFFF || (sts & (STS_HSE | STS_HCE | STS_HCH))) {
        kprintf("xhci: controller failure: USBSTS=%08x%s%s%s\n", sts,
                (sts & STS_HSE) ? " host-system-error" : "", (sts & STS_HCE) ? " host-controller-error" : "",
                (sts & STS_HCH) ? " halted" : "");
        x->dead = 1;
        return -1;
    }
    return 0;
}

/* ---------- commands ---------- */
static void cmd_abort(xhci_t *x) {
    kprintf("xhci: command timeout, aborting command ring\n");
    mmio_write32(x->op + OP_CRCR, CRCR_CA);
    mmio_write32(x->op + OP_CRCR + 4, 0);
    u64 deadline = time_ms() + 5000;
    while (time_ms() < deadline) {
        process_events(x);
        if (!(op_read(x, OP_CRCR) & CRCR_CRR)) break;
        mdelay(1);
    }
    process_events(x);
    if (op_read(x, OP_CRCR) & CRCR_CRR) {
        kprintf("xhci: command ring does not stop, controller disabled\n");
        x->dead = 1;
        return;
    }
    /* restart point: the next free TRB */
    op_write64(x, OP_CRCR, ring_enq_phys(&x->cmd) | x->cmd.cycle);
}

/* Issue one command and wait for its completion event. Returns the
 * completion code (or 0 on timeout / dead controller). */
static int command(xhci_t *x, u32 p0, u32 p1, u32 control, int timeout_ms, u8 *slot_out) {
    if (x->dead) return CC_INVALID;
    process_events(x);
    u32 idx = ring_push(&x->cmd, p0, p1, 0, control, 0);
    x->cmd_ptr = x->cmd.phys + (u64)idx * sizeof(trb_t);
    x->cmd_done = 0;
    doorbell(x, 0, 0);
    u64 deadline = time_ms() + (u64)timeout_ms, next_check = time_ms() + 10;
    while (!x->cmd_done) {
        process_events(x);
        if (x->cmd_done) break;
        u64 now = time_ms();
        if (now >= next_check) {
            next_check = now + 10;
            if (hc_check(x) < 0) { x->cmd_ptr = 0; return CC_INVALID; }
        }
        if (now >= deadline) {
            cmd_abort(x);
            int cc = x->cmd_done ? x->cmd_cc : CC_INVALID;
            x->cmd_ptr = 0;
            return cc == CC_SUCCESS ? CC_SUCCESS : CC_INVALID;
        }
        cpu_relax();
    }
    x->cmd_ptr = 0;
    if (slot_out) *slot_out = x->cmd_slot;
    return x->cmd_cc;
}

static int cmd_ctx(xhci_t *x, int type, xslot_t *s, u32 flags) {
    u64 p = virt_to_phys(s->in_ctx);
    wmb();
    return command(x, lo32(p), hi32(p), TRB_TYPE(type) | TRB_SLOT(s->id) | flags, CMD_TIMEOUT, NULL);
}

static int cmd_set_deq(xhci_t *x, xslot_t *s, xep_t *e) {
    u64 p = ring_enq_phys(&e->ring) | e->ring.cycle;
    return command(x, lo32(p), hi32(p), TRB_TYPE(TT_SET_TR_DEQ) | TRB_SLOT(s->id) | TRB_EP(e->dci), CMD_TIMEOUT, NULL);
}

static int cmd_ep(xhci_t *x, int type, xslot_t *s, xep_t *e, u32 flags) {
    return command(x, 0, 0, TRB_TYPE(type) | TRB_SLOT(s->id) | TRB_EP(e->dci) | flags, CMD_TIMEOUT, NULL);
}

/* Bring an endpoint to the Stopped state with its dequeue pointer at the
 * ring's enqueue position (abandoning any queued TD). Halted endpoints are
 * reset (tsp: preserve the data toggle / sequence number). */
static void ep_recover(xhci_t *x, xslot_t *s, xep_t *e, int tsp) {
    int st = ep_state(x, s, e->dci);
    if (st == EPS_DISABLED) { e->active = 0; return; }
    if (st == EPS_RUNNING) {
        int cc = cmd_ep(x, TT_STOP_EP, s, e, 0);
        if (cc == CC_CONTEXT_STATE) st = ep_state(x, s, e->dci);
        else st = EPS_STOPPED;
    }
    if (st == EPS_HALTED) {
        int cc = cmd_ep(x, TT_RESET_EP, s, e, tsp ? TRB_TSP : 0);
        if (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE)
            kprintf("xhci: slot %u ep %u: reset endpoint failed (%s)\n", s->id, e->dci, cc_name(cc));
    }
    int cc = cmd_set_deq(x, s, e);
    if (cc != CC_SUCCESS && !x->dead) {
        /* the endpoint may have been running after all: stop and retry */
        if (cc == CC_CONTEXT_STATE && ep_state(x, s, e->dci) == EPS_RUNNING) {
            cmd_ep(x, TT_STOP_EP, s, e, 0);
            cc = cmd_set_deq(x, s, e);
        }
        if (cc != CC_SUCCESS)
            kprintf("xhci: slot %u ep %u: set TR dequeue failed (%s)\n", s->id, e->dci, cc_name(cc));
    }
    process_events(x);
    e->active = 0;
    e->done = 0;
}

/* ---------- transfers ---------- */
static void td_begin(xep_t *e) {
    e->active = 1;
    e->done = 0;
    e->cc = 0;
    e->have_bytes = 0;
    e->bytes = 0;
    e->td_len = 0;
    e->td_start = e->ring.enq;
}

/* Queue data TRBs for buf/len: never crossing a 64 KiB boundary. first_type
 * is TT_DATA (control data stage, carries DIR) or TT_NORMAL. The last TRB
 * gets IOC if 'ioc'. Returns the index of the last TRB. */
static u32 queue_data(xep_t *e, u8 *buf, u32 len, u32 mps, u32 first_type, u32 first_flags, int ioc, int hold_first) {
    u64 addr = virt_to_phys(buf);
    u32 done = 0, idx = 0;
    u32 total_packets = mps ? (len + mps - 1) / mps : 0;
    int first = 1;
    if (len == 0) {
        u32 ctl = TRB_TYPE(first_type) | first_flags | (ioc ? TRB_IOC : 0);
        return ring_push(&e->ring, 0, 0, 0, ctl, hold_first);
    }
    while (done < len) {
        u64 a = addr + done;
        u32 chunk = 0x10000 - (u32)(a & 0xFFFF);
        if (chunk > len - done) chunk = len - done;
        int last = done + chunk == len;
        /* TD Size: packets remaining after this TRB (0 on the last one) */
        u32 tdsize = 0;
        if (!last && mps) {
            u32 sent = (done + chunk) / mps;
            tdsize = total_packets > sent ? total_packets - sent : 0;
            if (tdsize > 31) tdsize = 31;
        }
        u32 ctl = TRB_TYPE(first ? first_type : TT_NORMAL) | TRB_ISP | (first ? first_flags : 0);
        if (!last) ctl |= TRB_CH;
        else if (ioc) ctl |= TRB_IOC;
        idx = ring_push(&e->ring, lo32(a), hi32(a), chunk | (tdsize << 17), ctl, first && hold_first);
        first = 0;
        done += chunk;
    }
    e->td_len += len;
    return idx;
}

static int root_connected(xhci_t *x, usb_device_t *dev) {
    if (!dev || dev->root_port < 1 || dev->root_port > x->nports) return 1;
    return (portsc(x, dev->root_port) & PORT_CCS) != 0;
}

/* wait for the endpoint's TD to finish. 0 = done (see e->cc), or
 * E_TIMEOUT / E_NODEV / E_IO (controller failure). */
static int wait_td(xhci_t *x, xslot_t *s, xep_t *e, int timeout_ms) {
    u64 start = time_ms();
    u64 deadline = start + (u64)(timeout_ms > 0 ? timeout_ms : 1), next_check = start + 10;
    for (;;) {
        process_events(x);
        if (e->done) return 0;
        u64 now = time_ms();
        if (now >= next_check) {
            next_check = now + 10;
            if (hc_check(x) < 0) return E_IO;
            if (!root_connected(x, s->dev)) {
                process_events(x);
                if (e->done) return 0;
                return E_NODEV;
            }
        }
        if (now >= deadline) {
            process_events(x);
            return e->done ? 0 : E_TIMEOUT;
        }
        cpu_relax();
    }
}

static int xhci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *setup, void *data, int timeout_ms) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    if (x->dead) return E_IO;
    if (!s || !s->ep[1]) return E_NODEV;
    xep_t *e = s->ep[1];
    u32 len = setup->wLength;
    int in = (setup->bmRequestType & USB_DIR_IN) != 0;
    if (len && !data) return E_INVAL;
    if (e->active) ep_recover(x, s, e, 0);

    u32 sp[2];
    memcpy(sp, setup, 8);
    u32 trt = len ? (in ? 3u : 2u) : 0u;
    td_begin(e);
    u32 first = ring_push(&e->ring, sp[0], sp[1], 8, TRB_TYPE(TT_SETUP) | TRB_IDT | (trt << 16), 1);
    if (len) queue_data(e, data, len, s->mps0, TT_DATA, in ? TRB_DIR_IN : 0, 0, 0);
    e->td_end = ring_push(&e->ring, 0, 0, 0, TRB_TYPE(TT_STATUS) | TRB_IOC | ((len && in) ? 0 : TRB_DIR_IN), 0);
    ring_release(&e->ring, first);
    doorbell(x, s->id, 1);

    int r = wait_td(x, s, e, timeout_ms);
    if (r < 0) {
        ep_recover(x, s, e, 0);
        return r;
    }
    e->active = 0;
    int err = cc_to_err(e->cc);
    if (err == 0) return e->have_bytes ? e->bytes : (int)len;
    /* STALL or another error halted ep0: reset it so the next request works */
    if (e->cc != CC_STALL) kprintf("xhci: slot %u control transfer failed: %s\n", s->id, cc_name(e->cc));
    ep_recover(x, s, e, 0);
    return err;
}

static int xhci_bulk(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    xep_t *e = ep->hc_priv;
    if (x->dead) return E_IO;
    if (!s || !e) return E_NODEV;
    if (len < 0 || (len && !data)) return E_INVAL;
    if (e->active) ep_recover(x, s, e, 1);
    if (ep_state(x, s, e->dci) == EPS_HALTED) return E_STALL;   /* until reset_endpoint */
    u8 *p = data;
    int total = 0;
    do {
        u32 chunk = (u32)MIN(len - total, BULK_TD_MAX);
        td_begin(e);
        u32 first = e->ring.enq;
        e->td_end = queue_data(e, p + total, chunk, ep->mps, TT_NORMAL, 0, 1, 1);
        ring_release(&e->ring, first);
        doorbell(x, s->id, e->dci);
        int r = wait_td(x, s, e, timeout_ms);
        if (r < 0) {
            ep_recover(x, s, e, 1);
            return r;
        }
        e->active = 0;
        int err = cc_to_err(e->cc);
        if (err == E_STALL) return E_STALL;   /* stays halted until reset_endpoint */
        if (err < 0) {
            kprintf("xhci: slot %u ep %u bulk transfer failed: %s\n", s->id, e->dci, cc_name(e->cc));
            ep_recover(x, s, e, 1);
            return err;
        }
        int got = e->have_bytes ? e->bytes : (int)chunk;
        total += got;
        if ((u32)got < chunk) break;          /* short packet ends the transfer */
    } while (total < len);
    return total;
}

static int xhci_intr_start(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    xep_t *e = ep->hc_priv;
    if (x->dead) return E_IO;
    if (!s || !e) return E_NODEV;
    if (e->active) return E_BUSY;
    if (len < 0 || (len && !buf)) return E_INVAL;
    if (ep_state(x, s, e->dci) == EPS_HALTED) return E_STALL;   /* until reset_endpoint */
    td_begin(e);
    u32 first = e->ring.enq;
    e->td_end = queue_data(e, buf, (u32)len, ep->mps, TT_NORMAL, 0, 1, 1);
    ring_release(&e->ring, first);
    doorbell(x, s->id, e->dci);
    return 0;
}

static int xhci_intr_poll(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    xep_t *e = ep->hc_priv;
    if (!s || !e || !e->active) return E_INVAL;
    process_events(x);
    if (!e->done) {
        if (x->dead) { e->active = 0; return E_IO; }
        return E_AGAIN;
    }
    e->active = 0;
    int err = cc_to_err(e->cc);
    if (err == 0) return e->have_bytes ? e->bytes : (int)e->td_len;
    if (err == E_STALL) return E_STALL;       /* class driver clears the halt */
    /* transaction error etc.: the endpoint halted; make it usable again */
    ep_recover(x, s, e, 1);
    return err;
}

static void xhci_intr_cancel(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    xep_t *e = ep->hc_priv;
    if (!s || !e || !e->active) return;
    process_events(x);
    ep_recover(x, s, e, 1);
}

/* ---------- device setup ---------- */
static u32 slot_speed(int speed) {
    switch (speed) {
    case USB_SPEED_FULL: return 1;
    case USB_SPEED_LOW: return 2;
    case USB_SPEED_HIGH: return 3;
    case USB_SPEED_SUPER: return 4;
    case USB_SPEED_SUPERPLUS: return 5;
    }
    return 0;
}

static xep_t *xep_alloc(u8 dci, u8 kind, usb_endpoint_t *uep) {
    xep_t *e = kzalloc(sizeof *e);
    if (!e) return NULL;
    if (ring_alloc(&e->ring) < 0) { kfree(e); return NULL; }
    e->dci = dci;
    e->kind = kind;
    e->uep = uep;
    return e;
}

static void xep_free(xep_t *e) {
    if (!e) return;
    dma_free(e->ring.trb);
    kfree(e);
}

static int ep_dci(const usb_endpoint_t *ep) {
    int num = usb_ep_num(ep);
    if (ep->type == USB_EP_CONTROL) return num * 2 + 1;
    return num * 2 + (usb_ep_is_in(ep) ? 1 : 0);
}

/* xHCI Interval: period = 2^Interval * 125 us */
static u32 ep_interval(usb_device_t *dev, usb_endpoint_t *ep) {
    int bi = ep->interval;
    if (ep->type == USB_EP_CONTROL || ep->type == USB_EP_BULK) return 0;
    if (dev->speed >= USB_SPEED_HIGH) {
        int v = bi - 1;
        return (u32)CLAMP(v, 0, 15);
    }
    if (ep->type == USB_EP_ISO) {
        int v = bi + 2;
        return (u32)CLAMP(v, 3, 15);
    }
    /* FS/LS interrupt: bInterval in frames */
    if (bi < 1) bi = 1;
    u32 t = (u32)bi * 8;
    int v = 0;
    while (t >>= 1) v++;
    return (u32)CLAMP(v, 3, 10);
}

static void fill_ep_ctx(xhci_t *x, xslot_t *s, usb_device_t *dev, usb_endpoint_t *ep, xep_t *e) {
    volatile u32 *c = in_ep(x, s, e->dci);
    int in = usb_ep_is_in(ep);
    u32 type;
    switch (ep->type) {
    case USB_EP_CONTROL: type = 4; break;
    case USB_EP_ISO: type = in ? 5 : 1; break;
    case USB_EP_BULK: type = in ? 6 : 2; break;
    default: type = in ? 7 : 3; break;
    }
    u32 mps = ep->mps ? ep->mps : 8;
    u32 burst = 0, mult = 0;
    if (dev->speed >= USB_SPEED_SUPER) burst = ep->max_burst;
    else if (dev->speed == USB_SPEED_HIGH && (ep->type == USB_EP_INTR || ep->type == USB_EP_ISO)) burst = ep->mult;
    if (burst > 15) burst = 15;
    int periodic = ep->type == USB_EP_INTR || ep->type == USB_EP_ISO;
    u32 esit = periodic ? mps * (burst + 1) * (mult + 1) : 0;
    u32 avg = ep->type == USB_EP_CONTROL ? 8 : (periodic ? MIN(esit, 3072u) : 3072);
    u32 cerr = ep->type == USB_EP_ISO ? 0 : 3;
    u64 deq = ring_enq_phys(&e->ring) | e->ring.cycle;
    c[0] = (mult << 8) | (ep_interval(dev, ep) << 16) | ((esit >> 16) << 24);
    c[1] = (cerr << 1) | (type << 3) | (burst << 8) | (mps << 16);
    c[2] = lo32(deq);
    c[3] = hi32(deq);
    c[4] = avg | ((esit & 0xFFFF) << 16);
}

static void fill_ep0_ctx(xhci_t *x, xslot_t *s, xep_t *e, u32 mps) {
    volatile u32 *c = in_ep(x, s, 1);
    u64 deq = ring_enq_phys(&e->ring) | e->ring.cycle;
    c[0] = 0;
    c[1] = (3u << 1) | (4u << 3) | (mps << 16);
    c[2] = lo32(deq);
    c[3] = hi32(deq);
    c[4] = 8;
}

/* input slot context = current output slot context (fields an input
 * context must not carry cleared) */
static void copy_slot_ctx(xhci_t *x, xslot_t *s) {
    volatile u32 *i = in_slot(x, s), *o = out_slot(x, s);
    rmb();
    i[0] = o[0];
    i[1] = o[1];
    i[2] = o[2];
    i[3] = 0;
}

static int proto_slot_type(xhci_t *x, int port) {
    if (port < 1 || port > 255 || !x->port_proto[port]) return 0;
    return x->proto[x->port_proto[port] - 1].slot_type;
}

static int xhci_device_init(usb_hc_t *hc, usb_device_t *dev) {
    xhci_t *x = hc->priv;
    if (x->dead) return E_IO;
    u8 id = 0;
    int cc = command(x, 0, 0, TRB_TYPE(TT_ENABLE_SLOT) | ((u32)proto_slot_type(x, dev->root_port) << 16), CMD_TIMEOUT, &id);
    if (cc != CC_SUCCESS || id == 0 || id > x->max_slots) {
        kprintf("xhci: enable slot failed (%s)\n", cc_name(cc));
        return E_IO;
    }
    xslot_t *s = kzalloc(sizeof *s);
    if (!s) { command(x, 0, 0, TRB_TYPE(TT_DISABLE_SLOT) | TRB_SLOT(id), CMD_TIMEOUT, NULL); return E_NOMEM; }
    s->id = id;
    s->dev = dev;
    dev->hc_priv = s;
    x->slots[id] = s;
    s->in_ctx = dma_alloc(33 * x->csz, 64);
    s->out_ctx = dma_alloc(32 * x->csz, 64);
    s->ep[1] = xep_alloc(1, USB_EP_CONTROL, NULL);
    if (!s->in_ctx || !s->out_ctx || !s->ep[1]) return E_NOMEM;   /* device_free cleans up */
    dev->ep0.hc_priv = s->ep[1];
    x->dcbaa[id] = virt_to_phys(s->out_ctx);
    s->mps0 = dev->ep0.mps;
    s->max_dci = 1;

    in_ctx_clear(x, s);
    in_ctrl(x, s)[1] = 0x3;     /* add slot + ep0 */
    volatile u32 *sc = in_slot(x, s);
    sc[0] = (dev->route & 0xFFFFF) | (slot_speed(dev->speed) << 20) | (1u << 27);
    sc[1] = (u32)dev->root_port << 16;
    sc[2] = 0;
    if (dev->tt_hub && dev->tt_hub->hc_priv && dev->speed <= USB_SPEED_FULL) {
        xslot_t *hs = dev->tt_hub->hc_priv;
        sc[2] = hs->id | ((u32)dev->tt_port << 8);
        if (dev->tt_hub->hub_multi_tt) sc[0] |= 1u << 25;
    }
    fill_ep0_ctx(x, s, s->ep[1], dev->ep0.mps);
    cc = cmd_ctx(x, TT_ADDRESS_DEVICE, s, 0);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: address device failed on port %u (%s)\n", dev->port, cc_name(cc));
        return cc == CC_INVALID ? E_TIMEOUT : E_IO;
    }
    rmb();
    dev->address = (u8)(out_slot(x, s)[3] & 0xFF);
    mdelay(2);                  /* SET_ADDRESS recovery interval */
    return 0;
}

static int xhci_device_update(usb_hc_t *hc, usb_device_t *dev) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    if (!s) return E_NODEV;
    if (dev->ep0.mps == s->mps0) return 0;
    in_ctx_clear(x, s);
    in_ctrl(x, s)[1] = 0x2;     /* evaluate ep0 */
    fill_ep0_ctx(x, s, s->ep[1], dev->ep0.mps);
    int cc = cmd_ctx(x, TT_EVALUATE_CTX, s, 0);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: evaluate context (mps0 %u) failed (%s)\n", dev->ep0.mps, cc_name(cc));
        return E_IO;
    }
    s->mps0 = dev->ep0.mps;
    return 0;
}

static int configure_eps(xhci_t *x, xslot_t *s, usb_device_t *dev, int skip_iso) {
    xep_t *neweps[32] = { 0 };
    in_ctx_clear(x, s);
    u32 add = 1, drop = 0;
    int max_dci = 1;
    for (int dci = 2; dci < 32; dci++) if (s->ep[dci]) max_dci = dci;
    for (int i = 1; i < 32; i++) {
        usb_endpoint_t *ep = dev->eps[i];
        if (!ep || ep == &dev->ep0) continue;
        if (skip_iso && ep->type == USB_EP_ISO) continue;
        int dci = ep_dci(ep);
        if (dci < 2 || dci > 31 || neweps[dci]) continue;
        xep_t *e = xep_alloc((u8)dci, ep->type, ep);
        if (!e) continue;
        neweps[dci] = e;
        add |= 1u << dci;
        if (s->ep[dci]) drop |= 1u << dci;
        if (dci > max_dci) max_dci = dci;
        fill_ep_ctx(x, s, dev, ep, e);
    }
    in_ctrl(x, s)[0] = drop;
    in_ctrl(x, s)[1] = add;
    copy_slot_ctx(x, s);
    volatile u32 *sc = in_slot(x, s);
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((u32)max_dci << 27);
    int cc = cmd_ctx(x, TT_CONFIGURE_EP, s, 0);
    if (cc != CC_SUCCESS) {
        for (int dci = 2; dci < 32; dci++) xep_free(neweps[dci]);
        return cc;
    }
    for (int dci = 2; dci < 32; dci++) {
        if (!neweps[dci]) continue;
        if (s->ep[dci]) {
            if (s->ep[dci]->uep && s->ep[dci]->uep->hc_priv == s->ep[dci]) s->ep[dci]->uep->hc_priv = NULL;
            xep_free(s->ep[dci]);
        }
        s->ep[dci] = neweps[dci];
        neweps[dci]->uep->hc_priv = neweps[dci];
    }
    s->max_dci = (u8)max_dci;
    return CC_SUCCESS;
}

static int xhci_configure(usb_hc_t *hc, usb_device_t *dev) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    if (x->dead) return E_IO;
    if (!s) return E_NODEV;
    int cc = configure_eps(x, s, dev, 0);
    if (cc != CC_SUCCESS) {
        int has_iso = 0;
        for (int i = 1; i < 32; i++) if (dev->eps[i] && dev->eps[i]->type == USB_EP_ISO) has_iso = 1;
        if (has_iso) {
            kprintf("xhci: configure endpoint failed (%s), retrying without isochronous endpoints\n", cc_name(cc));
            cc = configure_eps(x, s, dev, 1);
        }
    }
    if (cc != CC_SUCCESS) {
        kprintf("xhci: configure endpoint failed (%s)\n", cc_name(cc));
        return E_IO;
    }
    return 0;
}

static int xhci_hub_init(usb_hc_t *hc, usb_device_t *dev) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    if (x->dead) return E_IO;
    if (!s) return E_NODEV;
    for (int attempt = 0; attempt < 2; attempt++) {
        in_ctx_clear(x, s);
        in_ctrl(x, s)[1] = 0x1;     /* slot context only */
        copy_slot_ctx(x, s);
        volatile u32 *sc = in_slot(x, s);
        sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((u32)s->max_dci << 27) | (1u << 26);
        sc[0] &= ~(1u << 25);
        if (dev->speed == USB_SPEED_HIGH && dev->hub_multi_tt) sc[0] |= 1u << 25;
        sc[1] = (sc[1] & 0x00FFFFFF) | ((u32)dev->hub_nports << 24);
        sc[2] &= ~(3u << 16);
        if (dev->speed == USB_SPEED_HIGH) sc[2] |= (u32)(dev->hub_ttt & 3) << 16;
        /* xHCI > 0.95 takes hub fields through Configure Endpoint; older
         * ones (and a fallback) through Evaluate Context */
        int use_eval = (x->version <= 0x95) ^ attempt;
        int cc = cmd_ctx(x, use_eval ? TT_EVALUATE_CTX : TT_CONFIGURE_EP, s, 0);
        if (cc == CC_SUCCESS) return 0;
        kprintf("xhci: hub setup via %s failed (%s)\n", use_eval ? "evaluate context" : "configure endpoint", cc_name(cc));
        if (x->dead) break;
    }
    return E_IO;
}

/* Reset the host side of an endpoint after the core cleared the halt
 * (device data toggle / sequence number is now 0). */
static int xhci_reset_endpoint(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    xep_t *e = ep->hc_priv;
    if (x->dead) return E_IO;
    if (!s || !e) return 0;
    int st = ep_state(x, s, e->dci);
    if (st == EPS_HALTED) {
        ep_recover(x, s, e, 0);
        return 0;
    }
    if (st == EPS_DISABLED) return E_NODEV;
    /* not halted: stop it and re-add the endpoint (drop + add) so the
     * xHC's toggle / sequence state starts from zero again */
    if (st == EPS_RUNNING) cmd_ep(x, TT_STOP_EP, s, e, 0);
    process_events(x);
    e->active = 0;
    e->done = 0;
    in_ctx_clear(x, s);
    in_ctrl(x, s)[0] = 1u << e->dci;
    in_ctrl(x, s)[1] = (1u << e->dci) | 1;
    copy_slot_ctx(x, s);
    fill_ep_ctx(x, s, dev, ep, e);
    int cc = cmd_ctx(x, TT_CONFIGURE_EP, s, 0);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: slot %u ep %u: re-adding endpoint failed (%s)\n", s->id, e->dci, cc_name(cc));
        ep_recover(x, s, e, 0);
    }
    return 0;
}

static void xhci_device_free(usb_hc_t *hc, usb_device_t *dev) {
    xhci_t *x = hc->priv;
    xslot_t *s = dev->hc_priv;
    if (!s) return;
    process_events(x);
    if (!x->dead) {
        int cc = command(x, 0, 0, TRB_TYPE(TT_DISABLE_SLOT) | TRB_SLOT(s->id), CMD_TIMEOUT, NULL);
        if (cc != CC_SUCCESS) kprintf("xhci: disable slot %u: %s\n", s->id, cc_name(cc));
    }
    x->dcbaa[s->id] = 0;
    x->slots[s->id] = NULL;
    for (int dci = 1; dci < 32; dci++) {
        xep_t *e = s->ep[dci];
        if (!e) continue;
        if (e->uep && e->uep->hc_priv == e) e->uep->hc_priv = NULL;
        xep_free(e);
    }
    for (int i = 0; i < 32; i++) if (dev->eps[i]) dev->eps[i]->hc_priv = NULL;
    dev->ep0.hc_priv = NULL;
    if (s->in_ctx) dma_free(s->in_ctx);
    if (s->out_ctx) dma_free(s->out_ctx);
    kfree(s);
    dev->hc_priv = NULL;
}

/* ---------- root hub ---------- */
static xproto_t *port_protocol(xhci_t *x, int p) {
    return x->port_proto[p] ? &x->proto[x->port_proto[p] - 1] : NULL;
}

static int port_is_usb3(xhci_t *x, int p) {
    xproto_t *pr = port_protocol(x, p);
    if (pr) return pr->major >= 3;
    return PORT_SPEED(portsc(x, p)) >= 4;
}

static int port_speed(xhci_t *x, int p, u32 v) {
    u32 psiv = PORT_SPEED(v);
    xproto_t *pr = port_protocol(x, p);
    if (pr && pr->psic) {
        for (int i = 0; i < pr->psic && i < 16; i++) {
            u32 psi = pr->psi[i];
            if ((psi & 0xF) != psiv) continue;
            u64 rate = psi >> 16;
            for (u32 e = (psi >> 4) & 3; e; e--) rate *= 1000;
            if (rate <= 1500000ull) return USB_SPEED_LOW;
            if (rate <= 12000000ull) return USB_SPEED_FULL;
            if (rate <= 480000000ull) return USB_SPEED_HIGH;
            if (rate <= 5000000000ull) return USB_SPEED_SUPER;
            return USB_SPEED_SUPERPLUS;
        }
    }
    switch (psiv) {
    case 1: return USB_SPEED_FULL;
    case 2: return USB_SPEED_LOW;
    case 3: return USB_SPEED_HIGH;
    case 4: return USB_SPEED_SUPER;
    case 0: return USB_SPEED_UNKNOWN;
    }
    return USB_SPEED_SUPERPLUS;
}

static void port_clear_changes(xhci_t *x, int p) {
    u32 v = portsc(x, p);
    if (v & PORT_CHANGE) portsc_write(x, p, (v & PORT_PRESERVE) | (v & PORT_CHANGE));
}

/* issue PR (hot reset on USB3) or WPR (warm reset, USB3 only) and wait */
static int port_reset(xhci_t *x, int p, int warm) {
    u32 v = portsc(x, p);
    portsc_write(x, p, (v & PORT_PRESERVE) | (warm ? PORT_WPR : PORT_PR) | (warm ? PORT_WRC : 0) | PORT_PRC);
    u64 deadline = time_ms() + (warm ? 1000 : 500);
    for (;;) {
        mdelay(1);
        v = portsc(x, p);
        if (!(v & PORT_PR) && (v & (PORT_PRC | PORT_WRC))) break;
        if (time_ms() >= deadline) {
            kprintf("xhci: port %d %s reset timeout (PORTSC %08x)\n", p, warm ? "warm" : "", v);
            port_clear_changes(x, p);
            return -1;
        }
    }
    port_clear_changes(x, p);
    return 0;
}

static void port_attach(xhci_t *x, int p) {
    mdelay(100);    /* connect debounce */
    u32 v = portsc(x, p);
    if (!(v & PORT_CCS)) return;
    int usb3 = port_is_usb3(x, p);
    if (usb3) {
        /* link training: wait for U0 / enabled, warm reset if it gets stuck */
        u64 deadline = time_ms() + 500;
        while (!(v & PORT_PED) && time_ms() < deadline) {
            u32 pls = (v & PORT_PLS_MASK) >> PORT_PLS_SHIFT;
            if (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE) break;
            mdelay(5);
            v = portsc(x, p);
            if (!(v & PORT_CCS)) return;
        }
        if (!(v & PORT_PED)) {
            kprintf("xhci: port %d: USB3 link not trained (PLS %u), warm reset\n", p, (v & PORT_PLS_MASK) >> PORT_PLS_SHIFT);
            port_reset(x, p, 1);
        } else {
            /* hot reset: put the device back into the Default state (it may
             * still carry an address from the firmware) */
            if (port_reset(x, p, 0) < 0) port_reset(x, p, 1);
        }
    } else {
        port_reset(x, p, 0);
    }
    v = portsc(x, p);
    if (!(v & PORT_CCS)) return;
    if (!(v & PORT_PED)) {
        kprintf("xhci: port %d: not enabled after reset (PORTSC %08x)\n", p, v);
        x->port_failed[p] = 1;
        return;
    }
    int speed = port_speed(x, p, v);
    mdelay(10);     /* reset recovery */
    /* link (re)training after a reset may report a late connect change:
     * the device is enumerated from here on, so that one is stale */
    port_clear_changes(x, p);
    x->port_dev[p] = usb_attach_device(&x->hc, NULL, p, speed);
    if (!x->port_dev[p]) x->port_failed[p] = 1;
    /* a disconnect during enumeration must still be seen: keep CSC */
    v = portsc(x, p);
    u32 chg = v & PORT_CHANGE & ~PORT_CSC;
    if (chg) portsc_write(x, p, (v & PORT_PRESERVE) | chg);
}

static void port_service(xhci_t *x, int p) {
    u32 v = portsc(x, p);
    if (v == 0xFFFFFFFF) return;
    u32 chg = v & PORT_CHANGE;
    if (chg) portsc_write(x, p, (v & PORT_PRESERVE) | chg);
    int connected = (v & PORT_CCS) != 0;
    if (x->port_dev[p] && (!connected || (chg & PORT_CSC))) {
        usb_port_disconnected(&x->hc, NULL, p);
        x->port_dev[p] = NULL;
    }
    if (chg & PORT_CSC) x->port_failed[p] = 0;
    if (!connected) {
        x->port_failed[p] = 0;
        /* USB3 link in SS.Inactive / Compliance without a connection:
         * recover once with a warm reset */
        u32 pls = (v & PORT_PLS_MASK) >> PORT_PLS_SHIFT;
        if (port_is_usb3(x, p) && (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE)) {
            if (!x->port_warm[p]) {
                x->port_warm[p] = 1;
                kprintf("xhci: port %d: link error (PLS %u), warm reset\n", p, pls);
                port_reset(x, p, 1);
            }
        } else {
            x->port_warm[p] = 0;
        }
        return;
    }
    if (x->port_dev[p] || x->port_failed[p]) return;
    port_attach(x, p);
}

static void xhci_poll(usb_hc_t *hc) {
    xhci_t *x = hc->priv;
    if (x->in_poll) return;
    if (x->dead) {
        /* controller failed: drop its devices once so the core forgets them */
        if (!x->dead_handled) {
            x->dead_handled = 1;
            x->in_poll = 1;
            for (int p = 1; p <= x->nports; p++)
                if (x->port_dev[p]) { usb_port_disconnected(&x->hc, NULL, p); x->port_dev[p] = NULL; }
            x->in_poll = 0;
        }
        return;
    }
    x->in_poll = 1;
    process_events(x);
    if (hc_check(x) == 0) {
        u32 sts = op_read(x, OP_USBSTS);
        if (sts & (STS_EINT | STS_PCD)) op_write(x, OP_USBSTS, sts & (STS_EINT | STS_PCD));
        for (int p = 1; p <= x->nports && !x->dead; p++) port_service(x, p);
    }
    x->in_poll = 0;
}

static const usb_hc_ops_t xhci_ops = {
    .device_init = xhci_device_init,
    .device_update = xhci_device_update,
    .device_free = xhci_device_free,
    .control = xhci_control,
    .configure = xhci_configure,
    .bulk = xhci_bulk,
    .intr_start = xhci_intr_start,
    .intr_poll = xhci_intr_poll,
    .intr_cancel = xhci_intr_cancel,
    .reset_endpoint = xhci_reset_endpoint,
    .hub_init = xhci_hub_init,
    .poll = xhci_poll,
};

/* ---------- controller bring-up ---------- */
static int wait_bits(volatile u8 *reg, u32 mask, u32 want, int timeout_ms) {
    u64 deadline = time_ms() + (u64)timeout_ms;
    for (;;) {
        u32 v = mmio_read32(reg);
        if (v != 0xFFFFFFFF && (v & mask) == want) return 0;
        if (time_ms() >= deadline) return -1;
        udelay(100);
    }
}

static void parse_xcaps(xhci_t *x) {
    u32 off = (x->hcc1 >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        volatile u8 *c = x->cap + off;
        u32 v = mmio_read32(c);
        if (v == 0xFFFFFFFF) break;
        u32 id = v & 0xFF, next = (v >> 8) & 0xFF;
        if (id == XCAP_LEGACY) {
            if (v & LEGSUP_BIOS) {
                kprintf("xhci: taking ownership from BIOS\n");
                mmio_write32(c, v | LEGSUP_OS);
                if (wait_bits(c, LEGSUP_BIOS, 0, 1000) < 0) {
                    kprintf("xhci: BIOS did not release the controller, forcing\n");
                    mmio_write32(c, (mmio_read32(c) & ~LEGSUP_BIOS) | LEGSUP_OS);
                }
            } else {
                mmio_write32(c, v | LEGSUP_OS);
            }
            /* USBLEGCTLSTS: disable all SMI sources, clear SMI status (RW1C) */
            u32 ctl = mmio_read32(c + 4);
            ctl &= (0x7u << 1) | (0xFFu << 5) | (0x7u << 17);
            ctl |= 0x7u << 29;
            mmio_write32(c + 4, ctl);
        } else if (id == XCAP_PROTOCOL && x->nproto < (int)ARRAY_SIZE(x->proto)) {
            xproto_t *pr = &x->proto[x->nproto];
            u32 d2 = mmio_read32(c + 8), d3 = mmio_read32(c + 12);
            pr->major = (u8)(v >> 24);
            pr->minor = (u8)(v >> 16);
            pr->first = (u8)d2;
            pr->count = (u8)(d2 >> 8);
            pr->psic = (u8)(d2 >> 28);
            pr->slot_type = (u8)(d3 & 0x1F);
            for (int i = 0; i < pr->psic && i < 16; i++) pr->psi[i] = mmio_read32(c + 16 + 4 * i);
            for (int p = pr->first; p < pr->first + pr->count && p < 256; p++)
                if (p >= 1) x->port_proto[p] = (u8)(x->nproto + 1);
            if (pr->count)
                kprintf("xhci: USB %x.%02x ports %u-%u%s\n", pr->major, pr->minor, pr->first,
                        pr->first + pr->count - 1, pr->psic ? " (PSI)" : "");
            x->nproto++;
        }
        if (!next) break;
        off += next << 2;
    }
}

static void xhci_setup(pci_dev_t *pd) {
    if (pd->bar[0].is_io || !pd->bar[0].addr) { kprintf("xhci: no memory BAR\n"); return; }
    pci_enable(pd);
    volatile u8 *mmio = pci_map_bar(pd, 0);
    if (!mmio) { kprintf("xhci: cannot map BAR0\n"); return; }

    xhci_t *x = kzalloc(sizeof *x);
    if (!x) return;
    x->pci = pd;
    x->cap = mmio;
    u32 cap0 = mmio_read32(mmio + CAP_CAPLENGTH);
    if (cap0 == 0xFFFFFFFF) { kprintf("xhci: controller not responding\n"); kfree(x); return; }
    x->version = (u16)(cap0 >> 16);
    x->op = mmio + (cap0 & 0xFF);
    x->hcs1 = mmio_read32(mmio + CAP_HCSPARAMS1);
    x->hcs2 = mmio_read32(mmio + CAP_HCSPARAMS2);
    x->hcc1 = mmio_read32(mmio + CAP_HCCPARAMS1);
    x->db = mmio + (mmio_read32(mmio + CAP_DBOFF) & ~3u);
    x->rt = mmio + (mmio_read32(mmio + CAP_RTSOFF) & ~0x1Fu);
    x->csz = (x->hcc1 & (1u << 2)) ? 64 : 32;
    x->max_slots = x->hcs1 & 0xFF;
    x->nports = (x->hcs1 >> 24) & 0xFF;
    kprintf("xhci: %04x:%04x at %02x:%02x.%x, version %x.%02x, %d slots, %d ports, %u-byte contexts\n",
            pd->vendor, pd->device, pd->bus, pd->dev, pd->func, x->version >> 8, x->version & 0xFF,
            x->max_slots, x->nports, x->csz);

    /* BIOS handoff + protocol capabilities */
    parse_xcaps(x);

    /* Intel PCH: route the shared ports to xHCI. Only on chipsets where they
     * are shared with an Intel EHCI (7/8/9-series): like Linux
     * (usb_enable_intel_xhci_ports), skip it when no Intel EHCI exists, since
     * later PCHs do not define XUSB2PR/USB3_PSSEN at these offsets. */
    int intel_ehci = 0;
    for (int i = 0; pd->vendor == 0x8086; i++) {
        pci_dev_t *e = pci_find_class(0x0C, 0x03, 0x20, i);
        if (!e) break;
        if (e->vendor == 0x8086) { intel_ehci = 1; break; }
    }
    if (pd->vendor == 0x8086 && intel_ehci) {
        u32 ss = pci_read32(pd, 0xDC), hs = pci_read32(pd, 0xD4);
        pci_write32(pd, 0xD8, ss);
        pci_write32(pd, 0xD0, hs);
        kprintf("xhci: Intel port routing: USB3_PSSEN=%x XUSB2PR=%x\n", pci_read32(pd, 0xD8), pci_read32(pd, 0xD0));
    }

    /* halt and reset */
    if (wait_bits(x->op + OP_USBSTS, STS_CNR, 0, 1000) < 0) kprintf("xhci: controller not ready\n");
    op_write(x, OP_USBCMD, op_read(x, OP_USBCMD) & ~(CMD_RS | CMD_INTE));
    if (wait_bits(x->op + OP_USBSTS, STS_HCH, STS_HCH, 100) < 0) kprintf("xhci: controller does not halt\n");
    op_write(x, OP_USBCMD, CMD_HCRST);
    if (wait_bits(x->op + OP_USBCMD, CMD_HCRST, 0, 1000) < 0 ||
        wait_bits(x->op + OP_USBSTS, STS_CNR, 0, 1000) < 0) {
        kprintf("xhci: controller reset failed\n");
        kfree(x);
        return;
    }
    mdelay(10);

    if (x->max_slots == 0 || x->nports == 0) { kprintf("xhci: no slots/ports\n"); kfree(x); return; }
    op_write(x, OP_CONFIG, (op_read(x, OP_CONFIG) & ~0xFFu) | (u32)x->max_slots);
    op_write(x, OP_DNCTRL, 0);

    /* device context base address array (+ scratchpad buffers in entry 0) */
    x->dcbaa = dma_alloc(4096, 4096);
    if (!x->dcbaa) { kfree(x); return; }
    x->nspad = (int)((((x->hcs2 >> 21) & 0x1F) << 5) | ((x->hcs2 >> 27) & 0x1F));
    if (x->nspad) {
        u32 ps = op_read(x, OP_PAGESIZE) & 0xFFFF;
        u32 page = 4096;
        for (int b = 0; b < 16; b++) if (ps & (1u << b)) { page = 4096u << b; break; }
        x->spad_array = dma_alloc(ALIGN_UP((size_t)x->nspad * 8, 64), 64);
        x->spad_bufs = kzalloc(sizeof(void *) * (size_t)x->nspad);
        if (!x->spad_array || !x->spad_bufs) { kprintf("xhci: out of memory\n"); return; }
        for (int i = 0; i < x->nspad; i++) {
            x->spad_bufs[i] = dma_alloc(page, page);
            if (!x->spad_bufs[i]) { kprintf("xhci: out of memory for scratchpads\n"); return; }
            x->spad_array[i] = virt_to_phys(x->spad_bufs[i]);
        }
        x->dcbaa[0] = virt_to_phys(x->spad_array);
        kprintf("xhci: %d scratchpad buffers of %u bytes\n", x->nspad, page);
    }
    op_write64(x, OP_DCBAAP, virt_to_phys(x->dcbaa));

    /* command ring */
    if (ring_alloc(&x->cmd) < 0) return;
    op_write64(x, OP_CRCR, x->cmd.phys | CRCR_RCS);

    /* event ring: one segment on interrupter 0, interrupts off (polled) */
    x->evt = dma_alloc(EVT_TRBS * sizeof(trb_t), 4096);
    x->erst = dma_alloc(64, 64);
    if (!x->evt || !x->erst) return;
    x->erst[0] = virt_to_phys(x->evt);
    x->erst[1] = EVT_TRBS;
    x->evt_deq = 0;
    x->evt_cycle = 1;
    ir_write(x, IR_IMAN, 1);             /* clear IP, IE = 0 */
    ir_write(x, IR_IMOD, 0);
    ir_write(x, IR_ERSTSZ, (ir_read(x, IR_ERSTSZ) & 0xFFFF0000u) | 1);
    ir_write64(x, IR_ERDP, virt_to_phys(x->evt));
    ir_write64(x, IR_ERSTBA, virt_to_phys(x->erst));

    /* run */
    op_write(x, OP_USBSTS, STS_HSE | STS_EINT | STS_PCD);
    op_write(x, OP_USBCMD, CMD_RS);
    if (wait_bits(x->op + OP_USBSTS, STS_HCH, 0, 100) < 0) {
        kprintf("xhci: controller does not start (USBSTS %08x)\n", op_read(x, OP_USBSTS));
        return;
    }

    /* port power (only if the HC has port power control) */
    if (x->hcc1 & (1u << 3)) {
        int powered = 0;
        for (int p = 1; p <= x->nports; p++) {
            u32 v = portsc(x, p);
            if (!(v & PORT_PP)) { portsc_write(x, p, (v & PORT_PRESERVE) | PORT_PP); powered = 1; }
        }
        if (powered) mdelay(20);
    }

    /* sanity check: a No-Op command must complete */
    int cc = command(x, 0, 0, TRB_TYPE(TT_NOOP_CMD), 500, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: no-op command failed (%s), controller disabled\n", cc_name(cc));
        x->dead = 1;
        return;
    }

    x->hc.name = "xHCI";
    snprintf(x->hc.desc, sizeof x->hc.desc, "%s xHCI %x.%x (%02x:%02x.%x)", pci_vendor_name(pd->vendor),
             x->version >> 8, (x->version >> 4) & 0xF, pd->bus, pd->dev, pd->func);
    x->hc.ops = &xhci_ops;
    x->hc.priv = x;
    x->hc.pci = pd;
    x->hc.flags = USB_HC_ASSIGNS_ADDRESS;
    x->hc.nports = (u8)x->nports;
    pd->driver = "xhci";
    usb_register_hc(&x->hc);
}

void xhci_init(void) {
    for (int i = 0;; i++) {
        pci_dev_t *pd = pci_find_class(0x0C, 0x03, 0x30, i);
        if (!pd) break;
        xhci_setup(pd);
    }
}
