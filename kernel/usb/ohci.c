/* Halcyon OS - OHCI (USB 1.1) host controller driver, polled
 *
 * Design:
 *  - every endpoint gets its own ED with the "dummy tail TD" pattern: work
 *    is queued by filling the current dummy, appending a new dummy and then
 *    moving TailP. The HC owns HeadP; the driver only writes HeadP while the
 *    ED is skipped and a frame boundary has passed.
 *  - control and bulk lists start with a static (skipped) head ED, so the
 *    list heads never change. The periodic schedule is a static binary tree
 *    of 63 skipped EDs (intervals 32,16,8,4,2,1 ms); interrupt EDs hang off
 *    the static ED of their interval.
 *  - data toggles are kept in software (ep->toggle); every TD carries an
 *    explicit DATA0/DATA1 and the toggle after a transfer is computed from
 *    the number of packets that completed, so the ED toggle carry is unused.
 *  - completion is detected by polling ED HeadP (== TailP: all retired;
 *    H bit: halted). TDs use DelayInterrupt 7, the done queue is not used.
 *  - interrupts stay disabled (MIE off); every wait has a timeout.
 */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/kernel.h"

/* ---------- registers ---------- */
#define HC_REVISION      0x00
#define HC_CONTROL       0x04
#define HC_CMDSTATUS     0x08
#define HC_INTSTATUS     0x0C
#define HC_INTENABLE     0x10
#define HC_INTDISABLE    0x14
#define HC_HCCA          0x18
#define HC_PERIODCUR     0x1C
#define HC_CTRLHEAD      0x20
#define HC_CTRLCUR       0x24
#define HC_BULKHEAD      0x28
#define HC_BULKCUR       0x2C
#define HC_DONEHEAD      0x30
#define HC_FMINTERVAL    0x34
#define HC_FMREMAINING   0x38
#define HC_FMNUMBER      0x3C
#define HC_PERIODICSTART 0x40
#define HC_LSTHRESHOLD   0x44
#define HC_RHDESCA       0x48
#define HC_RHDESCB       0x4C
#define HC_RHSTATUS      0x50
#define HC_RHPORT        0x54

#define CTL_CBSR3        0x00000003
#define CTL_PLE          0x00000004
#define CTL_IE           0x00000008
#define CTL_CLE          0x00000010
#define CTL_BLE          0x00000020
#define CTL_HCFS         0x000000C0
#define CTL_HCFS_RESET   0x00000000
#define CTL_HCFS_RESUME  0x00000040
#define CTL_HCFS_OPER    0x00000080
#define CTL_HCFS_SUSPEND 0x000000C0
#define CTL_IR           0x00000100
#define CTL_RWC          0x00000200

#define CS_HCR 0x01
#define CS_CLF 0x02
#define CS_BLF 0x04
#define CS_OCR 0x08

#define INT_SO   0x00000001
#define INT_WDH  0x00000002
#define INT_SF   0x00000004
#define INT_UE   0x00000010
#define INT_RHSC 0x00000040
#define INT_MIE  0x80000000u
#define INT_ALL  0xC000007F     /* all sources + OC + MIE */

#define FMI_FIT  0x80000000u

#define RHA_NDP   0x000000FF
#define RHA_PSM   0x00000100
#define RHA_NPS   0x00000200

#define RHS_LPSC  0x00010000    /* write: SetGlobalPower */

/* HcRhPortStatus. Writes: 1 triggers the action, 0 has no effect. */
#define PS_CCS   0x00000001     /* read: connected   write: ClearPortEnable */
#define PS_PES   0x00000002     /* read: enabled     write: SetPortEnable */
#define PS_PSS   0x00000004     /* read: suspended   write: SetPortSuspend */
#define PS_POCI  0x00000008     /* read: overcurrent write: ClearSuspendStatus */
#define PS_PRS   0x00000010     /* read: in reset    write: SetPortReset */
#define PS_PPS   0x00000100     /* read: powered     write: SetPortPower */
#define PS_LSDA  0x00000200     /* read: low speed   write: ClearPortPower */
#define PS_CSC   0x00010000
#define PS_PESC  0x00020000
#define PS_PSSC  0x00040000
#define PS_OCIC  0x00080000
#define PS_PRSC  0x00100000
#define PS_CHG_OTHER (PS_PESC | PS_PSSC | PS_OCIC | PS_PRSC)

/* ED dword 0 */
#define ED_S      (1u << 13)
#define ED_K      (1u << 14)
#define ED_D_OUT  (1u << 11)
#define ED_D_IN   (2u << 11)
/* ED HeadP low bits */
#define ED_H      1u
#define ED_C      2u
#define PTR_MASK  0xFFFFFFF0u

/* TD dword 0 */
#define TD_R          (1u << 18)
#define TD_DP_SETUP   0u
#define TD_DP_OUT     1u
#define TD_DP_IN      2u
#define TD_DI_NONE    (7u << 21)
#define TD_DI_NOW     (0u << 21)     /* WritebackDoneHead interrupt when retired */
#define TD_T_EXPLICIT (2u << 24)
#define TD_CC_NA      (15u << 28)

#define CC_NOERROR  0
#define CC_STALL    4
#define CC_NORESP   5
#define CC_UNDERRUN 9

#define TDK_SETUP  0
#define TDK_DATA   1
#define TDK_STATUS 2

#define MAX_TDS_PER_RUN 16
#define NUM_STATIC_INTR 63

/* hardware TD (first 16 bytes) + software bookkeeping; 32 bytes, 32-aligned */
typedef struct ohci_td {
    volatile u32 info, cbp, next, be;
    u32 buf;                   /* physical address of the first byte */
    u16 len;                   /* bytes queued in this TD */
    u8  tog;                   /* data toggle of the first packet */
    u8  kind;                  /* TDK_* */
    struct ohci_td *sw_next;   /* next TD in queue order */
} __attribute__((aligned(32))) ohci_td_t;
_Static_assert(sizeof(ohci_td_t) == 32, "ohci_td_t size");

/* hardware ED (first 16 bytes) + software bookkeeping */
typedef struct ohci_ed {
    volatile u32 info, tailp, headp, next;
    ohci_td_t *dummy;          /* current dummy tail TD */
    ohci_td_t *pend;           /* interrupt: first TD of the queued transfer */
    ohci_td_t *pend_tail;      /* interrupt: dummy behind the queued transfer */
    u16 mps;
    u8  type;                  /* USB_EP_* */
    u8  slot;                  /* interrupt: static ED index */
    u8  linked;
    u8  gone;                  /* ep0 ED: the device was found to be unplugged */
} __attribute__((aligned(16))) ohci_ed_t;
_Static_assert(sizeof(ohci_ed_t) % 16 == 0, "ohci_ed_t alignment");

typedef struct ohci {
    usb_hc_t hc;
    volatile u8 *regs;
    u32 *hcca;                 /* 256 bytes: 32 interrupt heads, frame no, done head */
    ohci_ed_t *ctrl_head;      /* static skipped head of the control list */
    ohci_ed_t *bulk_head;      /* static skipped head of the bulk list */
    ohci_ed_t *intr_static;    /* NUM_STATIC_INTR static skipped EDs */
    u16 intr_load[NUM_STATIC_INTR];
    u8 *setup_buf;
    ohci_td_t *td_pool;
    usb_device_t *port_dev[15];
    u8 port_failed[15];
    u8 ue_reported;
    u8 probing;                /* inside hub_port_gone() */
} ohci_t;

/* static interrupt tree: interval -> first index (32 EDs for 32 ms, 16 for 16 ms, ...) */
static int intr_base(int interval) {
    switch (interval) {
    case 32: return 0;
    case 16: return 32;
    case 8:  return 48;
    case 4:  return 56;
    case 2:  return 60;
    default: return 62;
    }
}

static inline u32 rd(ohci_t *o, u32 reg) { return mmio_read32(o->regs + reg); }
static inline void wr(ohci_t *o, u32 reg, u32 v) { mmio_write32(o->regs + reg, v); }
static inline u32 phys(const void *p) { return (u32)virt_to_phys(p); }
static inline u32 port_read(ohci_t *o, int p) { return rd(o, HC_RHPORT + 4 * p); }
static inline void port_write(ohci_t *o, int p, u32 v) { wr(o, HC_RHPORT + 4 * p, v); }

/* wait until at least one frame boundary has passed (HC no longer uses
 * whatever it cached of a skipped/unlinked ED) */
static void wait_frame(ohci_t *o) {
    u32 f = rd(o, HC_FMNUMBER) & 0xFFFF;
    for (int i = 0; i < 500; i++) {
        if ((rd(o, HC_FMNUMBER) & 0xFFFF) != f) return;
        udelay(10);
    }
}

/* ---------- TD / ED allocation ---------- */
static ohci_td_t *td_alloc(ohci_t *o) {
    ohci_td_t *td = o->td_pool;
    if (td) {
        o->td_pool = td->sw_next;
        memset(td, 0, sizeof *td);
        return td;
    }
    return dma_alloc(sizeof(ohci_td_t), 32);
}
static void td_release(ohci_t *o, ohci_td_t *td) {
    td->sw_next = o->td_pool;
    o->td_pool = td;
}

static ohci_ed_t *ed_alloc(ohci_t *o, u8 type, u32 info) {
    ohci_ed_t *ed = dma_alloc(sizeof(ohci_ed_t), 16);
    if (!ed) return NULL;
    ed->dummy = td_alloc(o);
    if (!ed->dummy) { dma_free(ed); return NULL; }
    ed->type = type;
    ed->info = info;
    ed->tailp = phys(ed->dummy);
    ed->headp = phys(ed->dummy);
    ed->next = 0;
    return ed;
}

static u32 ed_info(usb_device_t *dev, usb_endpoint_t *ep, u16 mps) {
    u32 v = (dev->address & 0x7Fu) | ((u32)usb_ep_num(ep) << 7) | ((u32)(mps & 0x7FF) << 16);
    if (dev->speed == USB_SPEED_LOW) v |= ED_S;
    if (ep->type != USB_EP_CONTROL) v |= usb_ep_is_in(ep) ? ED_D_IN : ED_D_OUT;
    return v;
}

/* refresh FA/EN/MPS/S/D of an idle ED (the HC never writes dword 0) */
static void ed_update(ohci_ed_t *ed, u32 info, u16 mps) {
    ed->mps = mps;
    if ((ed->info & ~ED_K) != info) ed->info = info | (ed->info & ED_K);
}

/* ---------- list management ---------- */
static void list_insert_after(ohci_ed_t *prev, ohci_ed_t *ed) {
    ed->next = prev->next;
    wmb();
    prev->next = phys(ed);
    wmb();
    ed->linked = 1;
}

/* remove ed from the hardware chain starting at 'start' (no wait) */
static void list_unlink(ohci_ed_t *start, ohci_ed_t *ed) {
    ohci_ed_t *p = start;
    for (int guard = 0; p && guard < 1024; guard++) {
        u32 n = p->next & PTR_MASK;
        if (n == phys(ed)) {
            p->next = ed->next;
            wmb();
            break;
        }
        p = n ? (ohci_ed_t *)phys_to_virt(n) : NULL;
    }
    ed->linked = 0;
}

static ohci_ed_t *list_start(ohci_t *o, ohci_ed_t *ed) {
    if (ed->type == USB_EP_CONTROL) return o->ctrl_head;
    if (ed->type == USB_EP_BULK) return o->bulk_head;
    return &o->intr_static[ed->slot];
}

static void intr_link(ohci_t *o, ohci_ed_t *ed, int binterval) {
    int interval = 1;
    if (binterval < 1) binterval = 1;
    while (interval * 2 <= binterval && interval < 32) interval *= 2;
    int base = intr_base(interval), best = base;
    for (int i = base; i < base + interval; i++)
        if (o->intr_load[i] < o->intr_load[best]) best = i;
    o->intr_load[best]++;
    ed->slot = (u8)best;
    list_insert_after(&o->intr_static[best], ed);
}

/* After an ED was unlinked and a frame passed: make sure the HC does not
 * resume a control/bulk list at it in the next frame. */
static void list_park(ohci_t *o, ohci_ed_t *ed) {
    u32 cur_reg, en;
    if (ed->type == USB_EP_CONTROL) { cur_reg = HC_CTRLCUR; en = CTL_CLE; }
    else if (ed->type == USB_EP_BULK) { cur_reg = HC_BULKCUR; en = CTL_BLE; }
    else return;
    if ((rd(o, cur_reg) & PTR_MASK) != phys(ed)) return;
    u32 ctl = rd(o, HC_CONTROL);
    wr(o, HC_CONTROL, ctl & ~en);
    wait_frame(o);
    wr(o, cur_reg, 0);
    wr(o, HC_CONTROL, ctl);
}

/* ---------- transfers ---------- */
typedef struct xfer {
    ohci_ed_t *ed;
    ohci_td_t *first;      /* == the ED's dummy at build time */
    ohci_td_t *cur;        /* TD being filled; after submit: the new dummy */
    int ntd;
    int nomem;
} xfer_t;

typedef struct xres {
    int bytes;             /* data-stage bytes */
    int tog;               /* toggle for the next packet */
    int cc;                /* condition code of the TD that stopped the queue */
    int short_pkt;         /* a short IN packet ended the data stage */
    ohci_td_t *stop;       /* TD with error / underrun (NULL if none) */
} xres_t;

static void xfer_begin(xfer_t *x, ohci_ed_t *ed) {
    x->ed = ed;
    x->first = x->cur = ed->dummy;
    x->ntd = 0;
    x->nomem = 0;
}

static void xfer_add(ohci_t *o, xfer_t *x, u32 dp, int tog, int rounding, u32 buf, int len, u8 kind) {
    if (x->nomem) return;
    ohci_td_t *nt = td_alloc(o);
    if (!nt) { x->nomem = 1; return; }
    ohci_td_t *td = x->cur;
    td->info = TD_CC_NA | TD_T_EXPLICIT | ((u32)(tog & 1) << 24) | TD_DI_NOW | (dp << 19) | (rounding ? TD_R : 0);
    td->cbp = len ? buf : 0;
    td->be = len ? buf + (u32)len - 1 : 0;
    td->next = phys(nt);
    td->buf = buf;
    td->len = (u16)len;
    td->tog = (u8)(tog & 1);
    td->kind = kind;
    td->sw_next = nt;
    x->cur = nt;
    x->ntd++;
}

/* free the TDs of a transfer (everything before the new dummy) */
static void xfer_free_tds(ohci_t *o, ohci_td_t *first, ohci_td_t *end) {
    ohci_td_t *td = first;
    while (td && td != end) {
        ohci_td_t *n = td->sw_next;
        td_release(o, td);
        td = n;
    }
}

/* undo a partially built transfer (allocation failure) */
static void xfer_abort(ohci_t *o, xfer_t *x) {
    xfer_free_tds(o, x->first->sw_next, NULL);
    ohci_td_t *d = x->first;
    memset(d, 0, sizeof *d);
}

static void xfer_submit(ohci_t *o, xfer_t *x) {
    x->ed->dummy = x->cur;
    wmb();
    x->ed->tailp = phys(x->cur);
    mb();
    if (x->ed->type == USB_EP_CONTROL) wr(o, HC_CMDSTATUS, CS_CLF);
    else if (x->ed->type == USB_EP_BULK) wr(o, HC_CMDSTATUS, CS_BLF);
}

static int td_dir_in(const ohci_td_t *td) { return ((td->info >> 19) & 3) == TD_DP_IN; }

/* evaluate the TDs of a finished (or stopped) transfer */
static void xfer_eval(xfer_t *x, int start_tog, xres_t *r) {
    int mps = x->ed->mps ? x->ed->mps : 8;
    r->bytes = 0;
    r->tog = start_tog;
    r->cc = CC_NOERROR;
    r->short_pkt = 0;
    r->stop = NULL;
    rmb();
    for (ohci_td_t *td = x->first; td && td != x->cur; td = td->sw_next) {
        u32 info = td->info;
        int cc = (int)(info >> 28);
        if (cc >= 14) break;                       /* not accessed */
        u32 cbp = td->cbp;
        int n = td->len ? (cbp ? (int)(cbp - td->buf) : td->len) : 0;
        if (n < 0 || n > td->len) n = 0;
        int pk;
        if (cc == CC_NOERROR) {
            if (!cbp || !td->len) pk = td->len ? (td->len + mps - 1) / mps : 1;
            else pk = td_dir_in(td) ? n / mps + 1 : n / mps;    /* short IN (R=1) / OUT in progress */
        } else if (cc == CC_UNDERRUN) {
            pk = n / mps + 1;
        } else {
            pk = n / mps;
        }
        if (td->kind == TDK_DATA) {
            r->bytes += n;
            r->tog = td->tog ^ (pk & 1);
        }
        if (cc == CC_NOERROR && cbp && td->len && td_dir_in(td) && td->kind == TDK_DATA) r->short_pkt = 1;
        if (cc != CC_NOERROR) {
            r->cc = cc;
            r->stop = td;
            if (cc == CC_UNDERRUN && td->kind == TDK_DATA) r->short_pkt = 1;
            break;
        }
    }
}

/* remove all queued TDs from an ED that halted or that we gave up on;
 * the HC is kept away from it with sKip while HeadP is rewritten */
static void ed_flush(ohci_t *o, ohci_ed_t *ed, ohci_td_t *tail) {
    ed->info |= ED_K;
    mb();
    wait_frame(o);
    ed->headp = phys(tail);       /* clears Halted and toggle carry */
    wmb();
    ed->info &= ~ED_K;
    mb();
}

static int cc_to_err(int cc) {
    if (cc == CC_STALL) return E_STALL;
    return E_IO;
}

static int root_port_gone(ohci_t *o, usb_device_t *dev) {
    int p = dev->root_port - 1;
    if (p < 0 || p >= o->hc.nports) return 0;
    return !(port_read(o, p) & PS_CCS);
}

/* wait for a submitted control/bulk transfer. Returns 0 (all TDs retired),
 * 1 (ED halted), or E_TIMEOUT / E_NODEV / E_IO (transfer still queued). */
static int xfer_wait(ohci_t *o, xfer_t *x, usb_device_t *dev, int timeout_ms) {
    u32 tail = phys(x->cur);
    u64 now = time_ms(), deadline = now + (u64)(timeout_ms > 0 ? timeout_ms : 1), next_check = now + 2;
    for (;;) {
        u32 gen = irq_generation();
        rmb();
        u32 h = x->ed->headp;
        if (h & ED_H) return 1;
        if ((h & PTR_MASK) == tail) return 0;
        now = time_ms();
        if (now >= next_check) {
            next_check = now + 2;
            if (rd(o, HC_INTSTATUS) & INT_UE) return E_IO;
            if (root_port_gone(o, dev)) return E_NODEV;
        }
        if (now >= deadline) return E_TIMEOUT;
        irq_wait(&o->hc.irq, gen, deadline);
    }
}

static int ohci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *s, void *data, int timeout_ms);

static void mark_gone(usb_device_t *dev) {
    ohci_ed_t *ed = dev->ep0.hc_priv;
    if (ed) ed->gone = 1;
}
static int dev_gone(usb_device_t *dev) {
    ohci_ed_t *ed = dev->ep0.hc_priv;
    return ed && ed->gone;
}

/* A transfer to a device behind a hub timed out: ask the hub whether the
 * device is still connected. (Real controllers report DeviceNotResponding
 * for an unplugged device; some emulations just never complete the TD.) */
static int hub_port_gone(ohci_t *o, usb_device_t *dev) {
    usb_device_t *hub = dev->parent;
    if (!hub || o->probing) return 0;
    u8 st[4] = { 0 };
    usb_setup_t s = { USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, dev->port, 4 };
    o->probing = 1;
    int r = ohci_control(&o->hc, hub, &s, st, 250);
    o->probing = 0;
    if (r < 4) return r == E_NODEV;
    return !(st[0] & 1) || (st[2] & 1);    /* not connected, or connection changed */
}

/* submit + wait + evaluate + clean up. Returns 0 or a negative error;
 * r is filled in either case. */
static int xfer_run(ohci_t *o, xfer_t *x, usb_device_t *dev, int start_tog, int timeout_ms, xres_t *r) {
    if (x->nomem) {
        xfer_abort(o, x);
        memset(r, 0, sizeof *r);
        r->tog = start_tog;
        return E_NOMEM;
    }
    xfer_submit(o, x);
    int w = xfer_wait(o, x, dev, timeout_ms);
    if (w != 0) ed_flush(o, x->ed, x->cur);
    xfer_eval(x, start_tog, r);
    xfer_free_tds(o, x->first, x->cur);
    if (w == E_TIMEOUT && hub_port_gone(o, dev)) w = E_NODEV;
    if (w == E_NODEV) mark_gone(dev);
    if (w < 0) return w;
    if (r->cc != CC_NOERROR && r->cc != CC_UNDERRUN) {
        if (r->cc == CC_NORESP && (root_port_gone(o, dev) || hub_port_gone(o, dev))) { mark_gone(dev); return E_NODEV; }
        return cc_to_err(r->cc);
    }
    return 0;
}

static ohci_ed_t *ep0_ed(ohci_t *o, usb_device_t *dev) {
    ohci_ed_t *ed = dev->ep0.hc_priv;
    u16 mps = dev->ep0.mps ? dev->ep0.mps : 8;
    u32 info = ed_info(dev, &dev->ep0, mps);
    if (!ed) {
        ed = ed_alloc(o, USB_EP_CONTROL, info);
        if (!ed) return NULL;
        ed->mps = mps;
        list_insert_after(o->ctrl_head, ed);
        dev->ep0.hc_priv = ed;
    }
    ed_update(ed, info, mps);
    return ed;
}

static int ohci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *s, void *data, int timeout_ms) {
    ohci_t *o = hc->priv;
    int len = s->wLength, in = (s->bmRequestType & 0x80) != 0;
    if (len && !data) return E_INVAL;
    if (dev_gone(dev)) return E_NODEV;
    ohci_ed_t *ed = ep0_ed(o, dev);
    if (!ed) return E_NOMEM;
    int mps = ed->mps;

    /* a single TD covers any buffer that spans at most two pages; larger
     * (very rare) requests go through a page-aligned bounce buffer split in
     * 8 KiB TDs */
    u8 *buf = data, *bounce = NULL;
    if (len && (phys(buf) & 0xFFF) + (u32)len > 8192) {
        bounce = dma_alloc((size_t)len, 4096);
        if (!bounce) return E_NOMEM;
        if (!in) memcpy(bounce, data, (size_t)len);
        buf = bounce;
    }
    memcpy(o->setup_buf, s, 8);

    xfer_t x;
    xres_t r;
    xfer_begin(&x, ed);
    xfer_add(o, &x, TD_DP_SETUP, 0, 0, phys(o->setup_buf), 8, TDK_SETUP);
    int tog = 1, off = 0;
    while (off < len) {
        int c = MIN(len - off, 8192);
        int last = off + c >= len;
        xfer_add(o, &x, in ? TD_DP_IN : TD_DP_OUT, tog, in && last, phys(buf + off), c, TDK_DATA);
        tog ^= ((c + mps - 1) / mps) & 1;
        off += c;
    }
    int status_in = !(in && len);
    xfer_add(o, &x, status_in ? TD_DP_IN : TD_DP_OUT, 1, 0, 0, 0, TDK_STATUS);
    int ret = xfer_run(o, &x, dev, 1, timeout_ms, &r);

    if (ret == 0 && r.cc == CC_UNDERRUN && r.stop && r.stop->kind == TDK_DATA) {
        /* short packet in a non-final data TD (bounce case only): the
         * remaining TDs were flushed, run the status stage on its own */
        xfer_t xs;
        xres_t rs;
        xfer_begin(&xs, ed);
        xfer_add(o, &xs, TD_DP_OUT, 1, 0, 0, 0, TDK_STATUS);
        ret = xfer_run(o, &xs, dev, 1, timeout_ms, &rs);
    }
    if (ret == 0) {
        ret = r.bytes;
        if (bounce && in) memcpy(data, bounce, (size_t)r.bytes);
    }
    if (bounce) dma_free(bounce);
    return ret;
}

static ohci_ed_t *ep_ed(ohci_t *o, usb_device_t *dev, usb_endpoint_t *ep) {
    ohci_ed_t *ed = ep->hc_priv;
    u16 mps = ep->mps ? ep->mps : 8;
    u32 info = ed_info(dev, ep, mps);
    if (!ed) {
        ed = ed_alloc(o, ep->type == USB_EP_INTR ? USB_EP_INTR : USB_EP_BULK, info);
        if (!ed) return NULL;
        ed->mps = mps;
        if (ed->type == USB_EP_INTR) intr_link(o, ed, ep->interval);
        else list_insert_after(o->bulk_head, ed);
        ep->hc_priv = ed;
    }
    ed_update(ed, info, mps);
    return ed;
}

static int ohci_bulk(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms) {
    ohci_t *o = hc->priv;
    if (len < 0 || (len && !data)) return E_INVAL;
    if (dev_gone(dev)) return E_NODEV;
    ohci_ed_t *ed = ep_ed(o, dev, ep);
    if (!ed) return E_NOMEM;
    int in = usb_ep_is_in(ep);
    int mps = ed->mps;
    u8 *p = data;
    int total = 0, first = 1;
    do {
        xfer_t x;
        xres_t r;
        xfer_begin(&x, ed);
        int tog = ep->toggle & 1, seg = 0;
        /* IN: TDs before the last one have bufferRounding off, so a short
         * packet halts the ED instead of letting the HC issue further INs
         * (which would steal the next transfer's data, e.g. a BOT CSW).
         * The byte count of such a DataUnderrun TD comes from its CBP, which
         * not every implementation updates (QEMU leaves it unchanged), so the
         * first TD of an IN transfer runs alone with rounding on: short
         * replies, which normally fit in it, are always counted exactly. */
        int maxtd = (in && first) ? 1 : MAX_TDS_PER_RUN;
        first = 0;
        if (len == 0) {
            xfer_add(o, &x, in ? TD_DP_IN : TD_DP_OUT, tog, in, 0, 0, TDK_DATA);
        }
        while (seg < len - total && x.ntd < maxtd) {
            u32 a = phys(p + seg);
            int room = 8192 - (int)(a & 0xFFF);
            int c = len - total - seg;
            if (c > room) c = room - room % mps;   /* full packets unless final */
            int last = (seg + c >= len - total) || x.ntd + 1 >= maxtd;
            xfer_add(o, &x, in ? TD_DP_IN : TD_DP_OUT, tog, in && last, a, c, TDK_DATA);
            tog ^= ((c + mps - 1) / mps) & 1;
            seg += c;
        }
        int ret = xfer_run(o, &x, dev, ep->toggle & 1, timeout_ms, &r);
        ep->toggle = (u8)(r.tog & 1);
        if (ret < 0) return ret;
        total += r.bytes;
        p += r.bytes;
        if (r.short_pkt || r.bytes < seg || len == 0) break;
    } while (total < len);
    return total;
}

/* ---------- interrupt transfers ---------- */
static int ohci_intr_start(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len) {
    ohci_t *o = hc->priv;
    ohci_ed_t *ed = ep->hc_priv;
    if (ed && ed->pend) return E_BUSY;
    if (dev_gone(dev)) return E_NODEV;
    ed = ep_ed(o, dev, ep);
    if (!ed) return E_NOMEM;
    if (len < 0 || (len && !buf)) return E_INVAL;
    u32 a = phys(buf);
    len = MIN(len, 8192 - (int)(a & 0xFFF));
    int in = usb_ep_is_in(ep);
    xfer_t x;
    xfer_begin(&x, ed);
    xfer_add(o, &x, in ? TD_DP_IN : TD_DP_OUT, ep->toggle & 1, in, a, len, TDK_DATA);
    if (x.nomem) { xfer_abort(o, &x); return E_NOMEM; }
    ed->pend = x.first;
    ed->pend_tail = x.cur;
    xfer_submit(o, &x);
    return 0;
}

/* evaluate + free the queued interrupt TD; returns bytes or error */
static int intr_retire(ohci_t *o, usb_endpoint_t *ep, ohci_ed_t *ed) {
    xfer_t x = { ed, ed->pend, ed->pend_tail, 1, 0 };
    xres_t r;
    xfer_eval(&x, ep->toggle & 1, &r);
    ep->toggle = (u8)(r.tog & 1);
    xfer_free_tds(o, ed->pend, ed->pend_tail);
    ed->pend = ed->pend_tail = NULL;
    if (r.cc != CC_NOERROR && r.cc != CC_UNDERRUN) return cc_to_err(r.cc);
    return r.bytes;
}

static int ohci_intr_poll(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    ohci_t *o = hc->priv;
    ohci_ed_t *ed = ep->hc_priv;
    if (!ed || !ed->pend) return E_INVAL;
    rmb();
    u32 h = ed->headp;
    if (!(h & ED_H) && (h & PTR_MASK) != phys(ed->pend_tail)) return E_AGAIN;
    if (h & ED_H) ed_flush(o, ed, ed->pend_tail);
    return intr_retire(o, ep, ed);
}

static void ohci_intr_cancel(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    ohci_t *o = hc->priv;
    ohci_ed_t *ed = ep->hc_priv;
    if (!ed || !ed->pend) return;
    ed_flush(o, ed, ed->pend_tail);
    intr_retire(o, ep, ed);
}

/* ---------- device / endpoint management ---------- */
static int ohci_device_init(usb_hc_t *hc, usb_device_t *dev) {
    return ep0_ed(hc->priv, dev) ? 0 : E_NOMEM;
}

static int ohci_device_update(usb_hc_t *hc, usb_device_t *dev) {
    return ep0_ed(hc->priv, dev) ? 0 : E_NOMEM;
}

static void ohci_device_free(usb_hc_t *hc, usb_device_t *dev) {
    ohci_t *o = hc->priv;
    ohci_ed_t *eds[33];
    int n = 0;
    if (dev->ep0.hc_priv) { eds[n++] = dev->ep0.hc_priv; dev->ep0.hc_priv = NULL; }
    for (int i = 0; i < 32; i++)
        if (dev->eps[i] && dev->eps[i]->hc_priv) { eds[n++] = dev->eps[i]->hc_priv; dev->eps[i]->hc_priv = NULL; }
    if (!n) return;
    /* stop and unlink everything, then one frame for the HC to let go */
    for (int i = 0; i < n; i++) {
        eds[i]->info |= ED_K;
        mb();
        if (eds[i]->linked) {
            list_unlink(list_start(o, eds[i]), eds[i]);
            if (eds[i]->type == USB_EP_INTR && o->intr_load[eds[i]->slot]) o->intr_load[eds[i]->slot]--;
        }
    }
    wait_frame(o);
    for (int i = 0; i < n; i++) {
        ohci_ed_t *ed = eds[i];
        list_park(o, ed);
        if (ed->pend) xfer_free_tds(o, ed->pend, ed->pend_tail);
        /* any TDs still hanging between HeadP and the dummy (aborted
         * control/bulk transfers are always flushed, so normally none) */
        td_release(o, ed->dummy);
        dma_free(ed);
    }
}

static int ohci_configure(usb_hc_t *hc, usb_device_t *dev) {
    for (int i = 0; i < 32; i++) if (dev->eps[i]) dev->eps[i]->toggle = 0;
    return 0;
}

static int ohci_reset_ep(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    /* toggles are explicit in every TD, the ED carry is not used */
    ep->toggle = 0;
    return 0;
}

/* ---------- root hub ---------- */
static int port_reset(ohci_t *o, int p) {
    /* The root hub drives SetPortReset for only 10 ms, but resets from a
     * root port must total >= 50 ms with gaps < 3 ms (USB 2.0 7.1.7.5,
     * TDRSTR): issue five back-to-back pulses, as Linux does. */
    u32 v = 0;
    for (int pulse = 0; pulse < 5; pulse++) {
        port_write(o, p, PS_PRSC);
        port_write(o, p, PS_PRS);         /* SetPortReset */
        int ok = 0;
        for (int i = 0; i < 100; i++) {
            mdelay(1);
            v = port_read(o, p);
            if (!(v & PS_CCS)) return -1;
            if ((v & PS_PRSC) && !(v & PS_PRS)) { ok = 1; break; }
        }
        port_write(o, p, PS_PRSC);
        if (!ok) return -1;
    }
    if (!(v & PS_PES)) {
        port_write(o, p, PS_PES);     /* SetPortEnable */
        mdelay(2);
        if (!(port_read(o, p) & PS_PES)) return -1;
    }
    return 0;
}

static void ohci_poll(usb_hc_t *hc) {
    ohci_t *o = hc->priv;
    if (hc->irq.kind != IRQ_NONE) {
        u32 rs = rd(o, HC_RHSTATUS);
        if (rs != 0xFFFFFFFF && (rs & 0x00020000)) wr(o, HC_RHSTATUS, 0x00020000);   /* OCIC */
        wr(o, HC_INTSTATUS, INT_RHSC);
        wr(o, HC_INTENABLE, INT_RHSC);
    }
    if (!o->ue_reported && (rd(o, HC_INTSTATUS) & INT_UE)) {
        o->ue_reported = 1;
        kprintf("ohci: %s: unrecoverable error, controller halted\n", hc->desc);
    }
    for (int p = 0; p < hc->nports; p++) {
        u32 v = port_read(o, p);
        if (v == 0xFFFFFFFF) return;
        if (v & PS_CSC) {
            port_write(o, p, PS_CSC);
            if (o->port_dev[p]) { usb_port_disconnected(hc, NULL, p + 1); o->port_dev[p] = NULL; }
            o->port_failed[p] = 0;
        }
        if (v & PS_CHG_OTHER) port_write(o, p, v & PS_CHG_OTHER);
        if (!(v & PS_CCS)) {
            if (o->port_dev[p]) { usb_port_disconnected(hc, NULL, p + 1); o->port_dev[p] = NULL; }
            o->port_failed[p] = 0;
            continue;
        }
        if (o->port_dev[p] || o->port_failed[p]) continue;
        mdelay(100);   /* debounce */
        if (!(port_read(o, p) & PS_CCS)) continue;
        if (port_reset(o, p) < 0) {
            kprintf("ohci: port %d reset failed\n", p + 1);
            o->port_failed[p] = 1;
            continue;
        }
        v = port_read(o, p);
        int speed = (v & PS_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        mdelay(10);    /* reset recovery */
        o->port_dev[p] = usb_attach_device(hc, NULL, p + 1, speed);
        if (!o->port_dev[p]) {
            o->port_failed[p] = 1;
            port_write(o, p, PS_CCS);   /* ClearPortEnable */
        }
        /* clear change bits caused by the reset; a CSC that appeared during
         * enumeration is a real (dis)connect and is handled next poll */
        port_write(o, p, port_read(o, p) & PS_CHG_OTHER);
    }
}

static const usb_hc_ops_t ohci_ops = {
    .device_init = ohci_device_init,
    .device_update = ohci_device_update,
    .device_free = ohci_device_free,
    .control = ohci_control,
    .configure = ohci_configure,
    .bulk = ohci_bulk,
    .intr_start = ohci_intr_start,
    .intr_poll = ohci_intr_poll,
    .intr_cancel = ohci_intr_cancel,
    .reset_endpoint = ohci_reset_ep,
    .poll = ohci_poll,
};

/* ---------- controller setup ---------- */
static int alloc_schedule(ohci_t *o) {
    o->hcca = dma_alloc(256, 256);
    o->ctrl_head = dma_alloc(sizeof(ohci_ed_t), 16);
    o->bulk_head = dma_alloc(sizeof(ohci_ed_t), 16);
    o->intr_static = dma_alloc(sizeof(ohci_ed_t) * NUM_STATIC_INTR, 16);
    o->setup_buf = dma_alloc(16, 16);
    if (!o->hcca || !o->ctrl_head || !o->bulk_head || !o->intr_static || !o->setup_buf) return -1;
    o->ctrl_head->info = ED_K;
    o->bulk_head->info = ED_K;
    o->ctrl_head->type = USB_EP_CONTROL;
    o->bulk_head->type = USB_EP_BULK;
    /* static tree: ED for interval I, slot k -> ED for I/2, slot k % (I/2) */
    for (int interval = 32; interval >= 1; interval /= 2) {
        int base = intr_base(interval);
        for (int k = 0; k < interval; k++) {
            ohci_ed_t *ed = &o->intr_static[base + k];
            ed->info = ED_K;
            ed->type = USB_EP_INTR;
            ed->slot = (u8)(base + k);
            ed->next = interval > 1 ? phys(&o->intr_static[intr_base(interval / 2) + k % (interval / 2)]) : 0;
        }
    }
    for (int i = 0; i < 32; i++) o->hcca[i] = phys(&o->intr_static[i]);
    return 0;
}

/* INTx acknowledge: WritebackDoneHead and RootHubStatusChange (UE stays
 * set for the driver to see; it is not an enabled source) */
static int ohci_ack(void *ctx) {
    ohci_t *o = ctx;
    u32 st = rd(o, HC_INTSTATUS);
    if (st == 0xFFFFFFFF) return 0;
    st &= INT_WDH | INT_RHSC;
    /* RHSC can behave level-triggered: disable it until ohci_poll has
     * serviced the ports (as Linux does) */
    if (st & INT_RHSC) wr(o, HC_INTDISABLE, INT_RHSC);
    if (st) wr(o, HC_INTSTATUS, st);
    return st != 0;
}

static void ohci_setup(pci_dev_t *pd) {
    if (pd->bar[0].is_io || !pd->bar[0].addr) { kprintf("ohci: %02x:%02x.%x has no MMIO BAR0\n", pd->bus, pd->dev, pd->func); return; }
    pci_enable(pd);
    ohci_t *o = kzalloc(sizeof *o);
    if (!o) return;
    o->regs = pci_map_bar(pd, 0);
    if (!o->regs || alloc_schedule(o) < 0) { kprintf("ohci: out of memory\n"); return; }
    u32 rev = rd(o, HC_REVISION) & 0xFF;

    /* --- take ownership from SMM / BIOS (OHCI spec 5.1.1.3) --- */
    u32 ctl = rd(o, HC_CONTROL);
    static const char *const hcfs_name[4] = { "reset", "resume", "operational", "suspend" };
    if (ctl & (CTL_IR | CTL_HCFS))
        kprintf("ohci: firmware left the controller %s%s\n", (ctl & CTL_IR) ? "owned by SMM, " : "",
                hcfs_name[(ctl >> 6) & 3]);
    if (ctl & CTL_IR) {
        wr(o, HC_INTENABLE, 0x40000000);          /* OwnershipChange (needed by some SMM drivers) */
        wr(o, HC_CMDSTATUS, CS_OCR);
        int i;
        for (i = 0; i < 1000 && (rd(o, HC_CONTROL) & CTL_IR); i++) mdelay(1);
        if (rd(o, HC_CONTROL) & CTL_IR) {
            kprintf("ohci: SMM driver did not release the controller, forcing\n");
            wr(o, HC_CONTROL, rd(o, HC_CONTROL) & ~CTL_IR);
        }
        ctl = rd(o, HC_CONTROL);
    } else if ((ctl & CTL_HCFS) != CTL_HCFS_RESET && (ctl & CTL_HCFS) != CTL_HCFS_OPER) {
        /* a BIOS driver left it suspended: resume first (5.1.1.3.4) */
        wr(o, HC_CONTROL, (ctl & CTL_RWC) | CTL_HCFS_RESUME);
        mdelay(20);
    }
    wr(o, HC_INTDISABLE, INT_ALL);
    /* Whatever state firmware left (it may also have powered down or
     * disabled ports it did not use), drive the bus into UsbReset so every
     * downstream device and the root hub start from a clean state. */
    wr(o, HC_CONTROL, (ctl & CTL_RWC) | CTL_HCFS_RESET);
    (void)rd(o, HC_CONTROL);
    mdelay(50);

    /* --- software reset, keeping the (BIOS-calibrated) frame interval --- */
    u32 fmi = rd(o, HC_FMINTERVAL);
    u32 fi = fmi & 0x3FFF;
    if (fi < 11000 || fi > 13000) fi = 11999;
    wr(o, HC_CMDSTATUS, CS_HCR);
    for (int i = 0; i < 100 && (rd(o, HC_CMDSTATUS) & CS_HCR); i++) udelay(1);
    if (rd(o, HC_CMDSTATUS) & CS_HCR) {
        for (int i = 0; i < 10 && (rd(o, HC_CMDSTATUS) & CS_HCR); i++) mdelay(1);
        if (rd(o, HC_CMDSTATUS) & CS_HCR) { kprintf("ohci: controller reset timed out\n"); return; }
    }
    /* now in UsbSuspend: must go operational within 2 ms */
    u32 fsmps = (6 * (fi - 210)) / 7;
    u32 fmi_new = ((rd(o, HC_FMINTERVAL) & FMI_FIT) ^ FMI_FIT) | (fsmps << 16) | fi;   /* toggle FIT */
    wr(o, HC_FMINTERVAL, fmi_new);
    wr(o, HC_PERIODICSTART, fi * 9 / 10);
    wr(o, HC_LSTHRESHOLD, 0x628);
    wr(o, HC_HCCA, phys(o->hcca));
    wr(o, HC_CTRLHEAD, phys(o->ctrl_head));
    wr(o, HC_CTRLCUR, 0);
    wr(o, HC_BULKHEAD, phys(o->bulk_head));
    wr(o, HC_BULKCUR, 0);
    wr(o, HC_INTDISABLE, INT_ALL);
    wr(o, HC_INTSTATUS, INT_ALL);
    wr(o, HC_CONTROL, CTL_CBSR3 | CTL_PLE | CTL_CLE | CTL_BLE | CTL_HCFS_OPER);
    (void)rd(o, HC_CONTROL);
    /* some controllers only latch these once operational */
    if ((rd(o, HC_FMINTERVAL) & 0x7FFF3FFF) != (fmi_new & 0x7FFF3FFF)) wr(o, HC_FMINTERVAL, fmi_new);
    if (!rd(o, HC_PERIODICSTART)) wr(o, HC_PERIODICSTART, fi * 9 / 10);
    mdelay(2);
    if ((rd(o, HC_CONTROL) & CTL_HCFS) != CTL_HCFS_OPER) { kprintf("ohci: controller did not become operational\n"); return; }
    u32 f0 = rd(o, HC_FMNUMBER) & 0xFFFF;
    mdelay(3);
    if ((rd(o, HC_FMNUMBER) & 0xFFFF) == f0) kprintf("ohci: warning: frame counter not running\n");

    /* --- root hub power --- */
    u32 rha = rd(o, HC_RHDESCA);
    int ndp = (int)(rha & RHA_NDP);
    if (ndp > 15) ndp = 15;
    /* global power for ganged ports, per-port power for the others; also
     * harmless with NPS (always powered) and undoes a firmware power-off */
    if (!(rha & RHA_NPS)) wr(o, HC_RHSTATUS, RHS_LPSC);
    for (int p = 0; p < ndp; p++) port_write(o, p, PS_PPS);
    int potpgt = (int)((rha >> 24) & 0xFF) * 2;
    mdelay(MAX(potpgt, 20));

    o->hc.name = "OHCI";
    snprintf(o->hc.desc, sizeof o->hc.desc, "%s OHCI (%02x:%02x.%x)", pci_vendor_name(pd->vendor), pd->bus, pd->dev, pd->func);
    o->hc.ops = &ohci_ops;
    o->hc.priv = o;
    o->hc.pci = pd;
    o->hc.nports = (u8)ndp;
    pd->driver = "ohci";
    kprintf("ohci: rev %x.%x, %d ports, FI %u, %s power switching\n", rev >> 4, rev & 0xF, ndp, fi,
            (rha & RHA_NPS) ? "no" : ((rha & RHA_PSM) ? "per-port" : "global"));
    if (usb_hc_irq(&o->hc, ohci_ack, o, 0) != IRQ_NONE) {
        wr(o, HC_INTSTATUS, INT_WDH | INT_RHSC);
        wr(o, HC_INTENABLE, INT_WDH | INT_RHSC | INT_MIE);
    }
    usb_register_hc(&o->hc);
}

void ohci_init(void) {
    for (int i = 0;; i++) {
        pci_dev_t *pd = pci_find_class(0x0C, 0x03, 0x10, i);
        if (!pd) break;
        ohci_setup(pd);
    }
}
