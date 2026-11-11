/* Halcyon OS - UHCI (USB 1.1) host controller driver, polled */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/kernel.h"

#define USBCMD   0x00
#define USBSTS   0x02
#define USBINTR  0x04
#define FRNUM    0x06
#define FLBASE   0x08
#define SOFMOD   0x0C
#define PORTSC   0x10

#define CMD_RS     0x0001
#define CMD_HCRESET 0x0002
#define CMD_GRESET 0x0004
#define CMD_CF     0x0040
#define CMD_MAXP   0x0080
#define STS_HCH    0x0020

#define PORT_CCS  0x0001
#define PORT_CSC  0x0002
#define PORT_PE   0x0004
#define PORT_PEC  0x0008
#define PORT_LSDA 0x0100
#define PORT_PR   0x0200
#define PORT_W1C  (PORT_CSC | PORT_PEC)

#define TD_ACTIVE  (1u << 23)
#define TD_STALLED (1u << 22)
#define TD_DBE     (1u << 21)
#define TD_BABBLE  (1u << 20)
#define TD_NAK     (1u << 19)
#define TD_CRCTO   (1u << 18)
#define TD_BITSTUFF (1u << 17)
#define TD_ERRMASK (TD_STALLED | TD_DBE | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)
#define TD_IOC     (1u << 24)
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)
#define TD_SPD     (1u << 29)

#define LINK_T  1u
#define LINK_Q  2u
#define LINK_VF 4u

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

typedef struct uhci_td { volatile u32 link, status, token, buffer; u32 sw[4]; } uhci_td_t;
typedef struct uhci_qh { volatile u32 head, element; u32 sw[2]; } uhci_qh_t;

typedef struct uhci_intr {
    uhci_qh_t *qh;
    uhci_td_t *td;
    int len;
    struct uhci_intr *prev, *next;
} uhci_intr_t;

typedef struct uhci {
    usb_hc_t hc;
    u16 io;
    u32 *frame_list;
    uhci_qh_t *int_qh;      /* anchor for interrupt QHs */
    uhci_qh_t *async_qh;    /* control/bulk transfers (one at a time) */
    u8 *setup_buf;
    uhci_intr_t *intr_list;
    usb_device_t *port_dev[8];
    u8 port_failed[8];
} uhci_t;

static u32 phys(const void *p) { return (u32)virt_to_phys(p); }

static void wait_frame(uhci_t *u) {
    u16 f = inw(u->io + FRNUM) & 0x3FF;
    for (int i = 0; i < 300 && (inw(u->io + FRNUM) & 0x3FF) == f; i++) udelay(10);
}

static u32 td_token(u8 pid, usb_device_t *dev, u8 ep, int toggle, int len) {
    u32 ml = len ? (u32)(len - 1) : 0x7FF;
    return pid | ((u32)dev->address << 8) | ((u32)(ep & 0xF) << 15) | ((u32)(toggle & 1) << 19) | (ml << 21);
}
static int td_actlen(u32 st) { return (int)((st + 1) & 0x7FF); }
static int td_maxlen(u32 tok) { u32 m = (tok >> 21) & 0x7FF; return m == 0x7FF ? 0 : (int)m + 1; }
static int td_err(u32 st) {
    if (st & TD_STALLED) return (st & (TD_BABBLE | TD_CRCTO)) ? E_IO : E_STALL;
    return E_IO;
}

/* Run a chain of TDs on the async QH and wait. For control transfers
 * status_idx is the index of the status TD (short data packet skips to it);
 * for bulk it is -1. Returns data bytes or error. last_done receives the
 * index of the last completed TD. */
static int run_tds(uhci_t *u, uhci_td_t *tds, int n, int status_idx, int timeout_ms, int *last_done) {
    for (int i = 0; i < n; i++) tds[i].link = (i + 1 < n) ? (phys(&tds[i + 1]) | LINK_VF) : LINK_T;
    mb();
    u->async_qh->element = phys(&tds[0]);
    mb();
    u64 deadline = time_ms() + (u64)timeout_ms;
    int bytes = 0, result = E_TIMEOUT, i = 0;
    *last_done = -1;
    while (time_ms() < deadline) {
        rmb();
        u32 st = tds[i].status;
        if (st & TD_ACTIVE) {
            if (inw(u->io + USBSTS) & 0x18) { result = E_IO; break; }   /* HSE / HCPE */
            cpu_relax();
            continue;
        }
        if (st & TD_ERRMASK) { result = td_err(st); *last_done = i - 1; break; }
        int al = td_actlen(st), ml = td_maxlen(tds[i].token);
        u8 pid = tds[i].token & 0xFF;
        if (pid != PID_SETUP && i != status_idx) bytes += al;
        *last_done = i;
        if (i == n - 1) { result = bytes; break; }
        if (pid == PID_IN && al < ml && i != status_idx) {
            /* short packet: queue halted (SPD) */
            if (status_idx < 0) { result = bytes; break; }
            i = status_idx;
            u->async_qh->element = phys(&tds[status_idx]);
            mb();
            continue;
        }
        i++;
    }
    u->async_qh->element = LINK_T;
    mb();
    wait_frame(u);
    return result;
}

static int uhci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_t *s, void *data, int timeout_ms) {
    uhci_t *u = hc->priv;
    int len = s->wLength, in = (s->bmRequestType & 0x80) != 0;
    int mps = dev->ep0.mps ? dev->ep0.mps : 8;
    int ndata = (len + mps - 1) / mps;
    int n = 2 + ndata;
    uhci_td_t *tds = dma_alloc(sizeof(uhci_td_t) * n, 16);
    if (!tds) return E_NOMEM;
    u32 ls = dev->speed == USB_SPEED_LOW ? TD_LS : 0;
    memcpy(u->setup_buf, s, 8);
    tds[0].status = TD_ACTIVE | TD_CERR3 | ls;
    tds[0].token = td_token(PID_SETUP, dev, 0, 0, 8);
    tds[0].buffer = phys(u->setup_buf);
    u8 *p = data;
    int toggle = 1;
    for (int i = 0; i < ndata; i++) {
        int chunk = MIN(mps, len - i * mps);
        uhci_td_t *t = &tds[1 + i];
        t->status = TD_ACTIVE | TD_CERR3 | ls | (in ? TD_SPD : 0);
        t->token = td_token(in ? PID_IN : PID_OUT, dev, 0, toggle, chunk);
        t->buffer = phys(p + i * mps);
        toggle ^= 1;
    }
    uhci_td_t *st = &tds[n - 1];
    st->status = TD_ACTIVE | TD_CERR3 | ls | TD_IOC;
    st->token = td_token((in && len) ? PID_OUT : PID_IN, dev, 0, 1, 0);
    st->buffer = 0;
    int last;
    int r = run_tds(u, tds, n, n - 1, timeout_ms, &last);
    dma_free(tds);
    return r;
}

static int uhci_bulk(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms) {
    uhci_t *u = hc->priv;
    int in = usb_ep_is_in(ep);
    int mps = ep->mps ? ep->mps : 64;
    int total = 0;
    u8 *p = data;
    while (len > 0 || total == 0) {
        int chunk = MIN(len, 512 * mps);
        int n = chunk ? (chunk + mps - 1) / mps : 1;
        uhci_td_t *tds = dma_alloc(sizeof(uhci_td_t) * n, 16);
        if (!tds) return E_NOMEM;
        u32 ls = dev->speed == USB_SPEED_LOW ? TD_LS : 0;
        int toggle = ep->toggle;
        for (int i = 0; i < n; i++) {
            int c = MIN(mps, chunk - i * mps);
            if (c < 0) c = 0;
            tds[i].status = TD_ACTIVE | TD_CERR3 | ls | (in ? TD_SPD : 0);
            tds[i].token = td_token(in ? PID_IN : PID_OUT, dev, usb_ep_num(ep), toggle, c);
            tds[i].buffer = phys(p + i * mps);
            toggle ^= 1;
        }
        int last;
        int r = run_tds(u, tds, n, -1, timeout_ms, &last);
        if (last >= 0) ep->toggle = (u8)(((tds[last].token >> 19) & 1) ^ 1);
        dma_free(tds);
        if (r < 0) return r;
        total += r; p += r; len -= r;
        if (r < chunk || chunk == 0) break;   /* short transfer */
    }
    return total;
}

/* ---------- interrupt transfers ---------- */
static int uhci_intr_start(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len) {
    uhci_t *u = hc->priv;
    if (ep->hc_priv) return E_BUSY;
    uhci_intr_t *x = kzalloc(sizeof *x);
    x->qh = dma_alloc(sizeof(uhci_qh_t), 16);
    x->td = dma_alloc(sizeof(uhci_td_t), 16);
    x->len = MIN(len, (int)ep->mps);
    int in = usb_ep_is_in(ep);
    x->td->link = LINK_T;
    x->td->status = TD_ACTIVE | TD_CERR3 | (dev->speed == USB_SPEED_LOW ? TD_LS : 0) | (in ? TD_SPD : 0);
    x->td->token = td_token(in ? PID_IN : PID_OUT, dev, usb_ep_num(ep), ep->toggle, x->len);
    x->td->buffer = phys(buf);
    x->qh->element = phys(x->td);
    /* insert right after the anchor */
    x->qh->head = u->int_qh->head;
    x->next = u->intr_list;
    if (x->next) x->next->prev = x;
    x->prev = NULL;
    u->intr_list = x;
    mb();
    u->int_qh->head = phys(x->qh) | LINK_Q;
    mb();
    ep->hc_priv = x;
    return 0;
}

static void intr_remove(uhci_t *u, usb_endpoint_t *ep) {
    uhci_intr_t *x = ep->hc_priv;
    if (!x) return;
    /* predecessor in hardware order is x->prev (list order == hw order) */
    if (x->prev) x->prev->qh->head = x->qh->head;
    else u->int_qh->head = x->qh->head;
    if (x->next) x->next->prev = x->prev;
    if (x->prev) x->prev->next = x->next; else u->intr_list = x->next;
    mb();
    wait_frame(u);
    dma_free(x->td);
    dma_free(x->qh);
    kfree(x);
    ep->hc_priv = NULL;
}

static int uhci_intr_poll(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    uhci_t *u = hc->priv;
    uhci_intr_t *x = ep->hc_priv;
    if (!x) return E_INVAL;
    rmb();
    u32 st = x->td->status;
    if (st & TD_ACTIVE) return E_AGAIN;
    int r;
    if (st & TD_ERRMASK) r = td_err(st);
    else { r = td_actlen(st); ep->toggle ^= 1; }
    intr_remove(u, ep);
    return r;
}

static void uhci_intr_cancel(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) {
    intr_remove(hc->priv, ep);
}

static int uhci_device_init(usb_hc_t *hc, usb_device_t *dev) { return 0; }
static void uhci_device_free(usb_hc_t *hc, usb_device_t *dev) {
    for (int i = 0; i < 32; i++) if (dev->eps[i] && dev->eps[i]->hc_priv) intr_remove(hc->priv, dev->eps[i]);
}
static int uhci_configure(usb_hc_t *hc, usb_device_t *dev) {
    for (int i = 0; i < 32; i++) if (dev->eps[i]) dev->eps[i]->toggle = 0;
    return 0;
}
static int uhci_reset_ep(usb_hc_t *hc, usb_device_t *dev, usb_endpoint_t *ep) { ep->toggle = 0; return 0; }

/* ---------- root hub ---------- */
static u16 port_read(uhci_t *u, int p) { return inw(u->io + PORTSC + 2 * p); }
static void port_write(uhci_t *u, int p, u16 v) { outw(u->io + PORTSC + 2 * p, v); }

static int port_reset(uhci_t *u, int p) {
    u16 v = port_read(u, p) & ~PORT_W1C;
    port_write(u, p, v | PORT_PR);
    mdelay(50);
    v = port_read(u, p) & ~PORT_W1C;
    port_write(u, p, v & ~PORT_PR);
    udelay(50);
    for (int i = 0; i < 10; i++) {
        mdelay(10);
        v = port_read(u, p);
        if (!(v & PORT_CCS)) return -1;
        if (v & (PORT_CSC | PORT_PEC)) { port_write(u, p, (v & ~PORT_W1C) | (v & PORT_W1C)); continue; }
        if (v & PORT_PE) return 0;
        port_write(u, p, (v & ~PORT_W1C) | PORT_PE);
    }
    return -1;
}

static void uhci_poll(usb_hc_t *hc) {
    uhci_t *u = hc->priv;
    for (int p = 0; p < hc->nports; p++) {
        u16 v = port_read(u, p);
        if (v & PORT_CSC) {
            port_write(u, p, (v & ~PORT_W1C) | PORT_CSC);
            if (u->port_dev[p]) {
                usb_port_disconnected(hc, NULL, p + 1);
                u->port_dev[p] = NULL;
            }
            u->port_failed[p] = 0;
        }
        if (v & PORT_PEC) port_write(u, p, (v & ~PORT_W1C) | PORT_PEC);
        if (!(v & PORT_CCS)) {
            if (u->port_dev[p]) { usb_port_disconnected(hc, NULL, p + 1); u->port_dev[p] = NULL; }
            u->port_failed[p] = 0;
            continue;
        }
        if (u->port_dev[p] || u->port_failed[p]) continue;
        mdelay(100);   /* debounce */
        if (port_reset(u, p) < 0) { u->port_failed[p] = 1; continue; }
        v = port_read(u, p);
        int speed = (v & PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        mdelay(10);
        u->port_dev[p] = usb_attach_device(hc, NULL, p + 1, speed);
        if (!u->port_dev[p]) u->port_failed[p] = 1;
        /* clear the change bits the reset generated */
        v = port_read(u, p);
        port_write(u, p, (v & ~PORT_W1C) | (v & PORT_W1C));
    }
}

static const usb_hc_ops_t uhci_ops = {
    .device_init = uhci_device_init,
    .device_free = uhci_device_free,
    .control = uhci_control,
    .configure = uhci_configure,
    .bulk = uhci_bulk,
    .intr_start = uhci_intr_start,
    .intr_poll = uhci_intr_poll,
    .intr_cancel = uhci_intr_cancel,
    .reset_endpoint = uhci_reset_ep,
    .poll = uhci_poll,
};

static void uhci_setup(pci_dev_t *pd) {
    int bar = 4;
    if (!pd->bar[bar].is_io || !pd->bar[bar].addr) {
        for (bar = 0; bar < 6; bar++) if (pd->bar[bar].is_io && pd->bar[bar].addr) break;
        if (bar == 6) { kprintf("uhci: no I/O BAR\n"); return; }
    }
    pci_enable(pd);
    pci_write16(pd, 0xC0, 0x8F00);   /* LEGSUP: disable legacy emulation & SMIs */

    uhci_t *u = kzalloc(sizeof *u);
    u->io = (u16)pd->bar[bar].addr;
    outw(u->io + USBINTR, 0);
    outw(u->io + USBCMD, 0);
    for (int i = 0; i < 100 && !(inw(u->io + USBSTS) & STS_HCH); i++) mdelay(1);
    outw(u->io + USBCMD, CMD_GRESET);
    mdelay(20);
    outw(u->io + USBCMD, 0);
    mdelay(5);
    outw(u->io + USBCMD, CMD_HCRESET);
    for (int i = 0; i < 100 && (inw(u->io + USBCMD) & CMD_HCRESET); i++) mdelay(1);

    u->frame_list = dma_alloc(4096, 4096);
    u->int_qh = dma_alloc(sizeof(uhci_qh_t), 16);
    u->async_qh = dma_alloc(sizeof(uhci_qh_t), 16);
    u->setup_buf = dma_alloc(16, 16);
    u->async_qh->head = LINK_T;
    u->async_qh->element = LINK_T;
    u->int_qh->head = phys(u->async_qh) | LINK_Q;
    u->int_qh->element = LINK_T;
    for (int i = 0; i < 1024; i++) u->frame_list[i] = phys(u->int_qh) | LINK_Q;

    outw(u->io + USBINTR, 0);
    outl(u->io + FLBASE, phys(u->frame_list));
    outw(u->io + FRNUM, 0);
    outb(u->io + SOFMOD, 0x40);
    outw(u->io + USBSTS, 0x3F);
    outw(u->io + USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
    mdelay(2);

    /* count root ports: PORTSC bit 7 always reads 1 */
    int n = 0;
    while (n < 8) {
        u16 v = inw(u->io + PORTSC + 2 * n);
        if (!(v & 0x0080) || v == 0xFFFF) break;
        n++;
    }
    if (n == 0) n = 2;
    u->hc.name = "UHCI";
    snprintf(u->hc.desc, sizeof u->hc.desc, "%s UHCI (%02x:%02x.%x)", pci_vendor_name(pd->vendor), pd->bus, pd->dev, pd->func);
    u->hc.ops = &uhci_ops;
    u->hc.priv = u;
    u->hc.pci = pd;
    u->hc.nports = (u8)n;
    pd->driver = "uhci";
    usb_register_hc(&u->hc);
}

void uhci_init(void) {
    for (int i = 0;; i++) {
        pci_dev_t *pd = pci_find_class(0x0C, 0x03, 0x00, i);
        if (!pd) break;
        uhci_setup(pd);
    }
}
