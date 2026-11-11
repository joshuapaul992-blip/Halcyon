/* Halcyon OS - USB hub class driver (USB 2.0 and SuperSpeed hubs) */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/irq.h"

#define HUB_REQ_GET_STATUS    0
#define HUB_REQ_CLEAR_FEATURE 1
#define HUB_REQ_SET_FEATURE   3
#define HUB_REQ_SET_HUB_DEPTH 12

#define PORT_CONNECTION   0
#define PORT_ENABLE       1
#define PORT_RESET        4
#define PORT_POWER        8
#define C_HUB_LOCAL_POWER 0
#define C_HUB_OVER_CURRENT 1
#define BH_PORT_RESET     28
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE     17
#define C_PORT_SUSPEND    18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET      20
#define C_PORT_LINK_STATE 25
#define C_PORT_CONFIG_ERROR 26
#define C_BH_PORT_RESET   29

#define PS_CONNECTION 0x0001
#define PS_ENABLE     0x0002
#define PS_RESET      0x0010
#define PS_LOW_SPEED  0x0200
#define PS_HIGH_SPEED 0x0400

typedef struct hub {
    usb_endpoint_t *ep;
    u8 *buf;
    int nports;
    int ss;                 /* SuperSpeed hub */
    int pending;
    u8 failed[256];         /* ports where enumeration failed (until disconnect) */
    u64 next_scan;
} hub_t;

#define RT_PORT (USB_TYPE_CLASS | USB_RECIP_OTHER)
#define RT_HUB  (USB_TYPE_CLASS | USB_RECIP_DEVICE)

static int port_status(usb_device_t *dev, int port, u16 *status, u16 *change) {
    u8 st[4] = { 0 };
    int r = usb_control(dev, USB_DIR_IN | RT_PORT, HUB_REQ_GET_STATUS, 0, (u16)port, st, 4);
    if (r < 4) return r < 0 ? r : E_IO;
    *status = st[0] | (st[1] << 8);
    *change = st[2] | (st[3] << 8);
    return 0;
}

static int port_feature(usb_device_t *dev, int port, int set, int feature) {
    return usb_control(dev, USB_DIR_OUT | RT_PORT, set ? HUB_REQ_SET_FEATURE : HUB_REQ_CLEAR_FEATURE,
                       (u16)feature, (u16)port, NULL, 0);
}

static void port_connected(usb_device_t *dev, hub_t *h, int port) {
    mdelay(100);   /* debounce */
    u16 st, ch;
    if (port_status(dev, port, &st, &ch) < 0 || !(st & PS_CONNECTION)) return;
    /* SuperSpeed hub port whose link sits in SS.Inactive or Compliance Mode
     * (wPortStatus bits 8..5): only a warm (BH) reset recovers it, a hot
     * PORT_RESET is not honoured there (USB 3.2 ch. 7.5, 10.16.2.6).
     * Completion is then reported through C_BH_PORT_RESET (wPortChange
     * bit 5). */
    int pls = (st >> 5) & 0xF;
    int warm = h->ss && (pls == 6 || pls == 10);
    port_feature(dev, port, 1, warm ? BH_PORT_RESET : PORT_RESET);
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        mdelay(10);
        if (port_status(dev, port, &st, &ch) < 0) return;
        if (!(st & PS_RESET) && (ch & ((1 << 4) | (warm ? 1 << 5 : 0)))) { ok = 1; break; }
        if (!(st & PS_CONNECTION) && !(warm && (st & PS_RESET))) return;   /* link retrains during a warm reset */
    }
    port_feature(dev, port, 0, C_PORT_RESET);
    if (h->ss) port_feature(dev, port, 0, C_BH_PORT_RESET);
    if (!ok || !(st & PS_ENABLE)) {
        kprintf("usb-hub: port %d reset failed (status %04x)\n", port, st);
        h->failed[port] = 1;
        return;
    }
    mdelay(10);   /* reset recovery */
    int speed;
    if (h->ss) speed = USB_SPEED_SUPER;
    else if (st & PS_LOW_SPEED) speed = USB_SPEED_LOW;
    else if (st & PS_HIGH_SPEED) speed = USB_SPEED_HIGH;
    else speed = USB_SPEED_FULL;
    if (!usb_attach_device(dev->hc, dev, port, speed)) {
        h->failed[port] = 1;
        port_feature(dev, port, 0, PORT_ENABLE);
    }
}

static void service_port(usb_device_t *dev, hub_t *h, int port) {
    u16 st, ch;
    if (port_status(dev, port, &st, &ch) < 0) return;
    if (ch & (1 << 0)) port_feature(dev, port, 0, C_PORT_CONNECTION);
    if (ch & (1 << 1)) port_feature(dev, port, 0, C_PORT_ENABLE);
    if (ch & (1 << 2)) port_feature(dev, port, 0, C_PORT_SUSPEND);
    if (ch & (1 << 3)) port_feature(dev, port, 0, C_PORT_OVER_CURRENT);
    if (ch & (1 << 4)) port_feature(dev, port, 0, C_PORT_RESET);
    if (h->ss) {
        if (ch & (1 << 5)) port_feature(dev, port, 0, C_BH_PORT_RESET);
        if (ch & (1 << 6)) port_feature(dev, port, 0, C_PORT_LINK_STATE);
        if (ch & (1 << 7)) port_feature(dev, port, 0, C_PORT_CONFIG_ERROR);
    }
    usb_device_t *child = usb_find_child(dev->hc, dev, port);
    int connected = st & PS_CONNECTION;
    if (child && (!connected || (ch & 1))) {
        usb_port_disconnected(dev->hc, dev, port);
        child = NULL;
    }
    /* a new connection (unplug + replug between two polls shows up as a
     * connect change with the port still connected) gets a fresh attempt */
    if (!connected || (ch & 1)) h->failed[port] = 0;
    if (connected && !child && !h->failed[port]) port_connected(dev, h, port);
}

/* hub-level change (wHubChange: local power, over-current): reported as
 * bit 0 of the status change bitmap until acknowledged (USB 2.0 11.24.2.6) */
static void hub_status_change(usb_device_t *dev) {
    u8 st[4] = { 0 };
    if (usb_control(dev, USB_DIR_IN | RT_HUB, HUB_REQ_GET_STATUS, 0, 0, st, 4) < 4) return;
    u16 ch = st[2] | (st[3] << 8);
    if (ch & 1) usb_control(dev, USB_DIR_OUT | RT_HUB, HUB_REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, NULL, 0);
    if (ch & 2) {
        kprintf("usb-hub: hub over-current change (status %04x)\n", st[0] | (st[1] << 8));
        usb_control(dev, USB_DIR_OUT | RT_HUB, HUB_REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, NULL, 0);
    }
}

static int hub_probe(usb_device_t *dev, const usb_interface_desc_t *intf, usb_binding_t *b) {
    if (intf->bInterfaceClass != USB_CLASS_HUB) return E_NOTSUP;
    hub_t *h = kzalloc(sizeof *h);
    h->ss = dev->speed >= USB_SPEED_SUPER;
    u8 desc[16] = { 0 };
    int r = usb_control(dev, USB_DIR_IN | RT_HUB, USB_REQ_GET_DESCRIPTOR,
                        (u16)((h->ss ? USB_DT_SS_HUB : USB_DT_HUB) << 8), 0, desc, sizeof desc);
    if (r < 7) { kprintf("usb-hub: cannot read hub descriptor (%d)\n", r); kfree(h); return E_IO; }
    h->nports = desc[2];
    u16 chars = desc[3] | (desc[4] << 8);
    int pwr_delay = desc[5] * 2;
    dev->is_hub = 1;
    dev->hub_nports = (u8)h->nports;
    dev->hub_ttt = (chars >> 5) & 3;
    /* a multi-TT hub runs single-TT until alternate setting 1 is selected */
    dev->hub_multi_tt = 0;
    if (dev->speed == USB_SPEED_HIGH && dev->desc.bDeviceProtocol == 2 &&
        usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
                    1, intf->bInterfaceNumber, NULL, 0) >= 0)
        dev->hub_multi_tt = 1;
    if (dev->hc->ops->hub_init && dev->hc->ops->hub_init(dev->hc, dev) < 0)
        kprintf("usb-hub: HC hub_init failed\n");
    if (h->ss)
        usb_control(dev, USB_DIR_OUT | RT_HUB, HUB_REQ_SET_HUB_DEPTH, dev->hub_depth, 0, NULL, 0);
    for (int p = 1; p <= h->nports; p++) port_feature(dev, p, 1, PORT_POWER);
    mdelay(MAX(pwr_delay, 20) + 20);

    h->ep = usb_find_endpoint(dev, intf->bInterfaceNumber, USB_EP_INTR, USB_DIR_IN);
    h->buf = kzalloc(64);
    b->priv = h;
    snprintf(dev->description, sizeof dev->description, "%s hub, %d ports",
             h->ss ? "SuperSpeed" : (dev->speed == USB_SPEED_HIGH ? "USB 2.0" : "USB 1.1"), h->nports);
    kprintf("usb-hub: %s\n", dev->description);
    /* scan all ports now: devices present at power-up */
    for (int p = 1; p <= h->nports && dev->alive; p++) service_port(dev, h, p);
    h->next_scan = time_ms() + 1000;
    return 0;
}

static void hub_poll(usb_device_t *dev, usb_binding_t *b) {
    hub_t *h = b->priv;
    if (!h) return;
    if (h->ep) {
        if (!h->pending) {
            int len = MIN((h->nports + 8) / 8, (int)h->ep->mps);
            if (usb_intr_start(dev, h->ep, h->buf, len) >= 0) h->pending = 1;
        } else {
            int r = usb_intr_poll(dev, h->ep);
            if (r == E_AGAIN) {
                /* fall through to the periodic safety scan */
            } else {
                h->pending = 0;
                if (r > 0) {
                    if (h->buf[0] & 1) hub_status_change(dev);
                    for (int p = 1; p <= h->nports && dev->alive; p++)
                        if (h->buf[p / 8] & (1 << (p % 8))) service_port(dev, h, p);
                } else if (r == E_STALL) {
                    usb_clear_halt(dev, h->ep);
                }
            }
        }
    }
    /* periodic full scan (also covers hubs without a usable status endpoint) */
    idle_wake_at(h->next_scan);
    if (time_ms() >= h->next_scan) {
        h->next_scan = time_ms() + (h->ep ? 2000 : 250);
        for (int p = 1; p <= h->nports && dev->alive; p++) {
            u16 st, ch;
            if (port_status(dev, p, &st, &ch) < 0) break;
            usb_device_t *child = usb_find_child(dev->hc, dev, p);
            if (ch || (!!(st & PS_CONNECTION) != !!child && !h->failed[p])) service_port(dev, h, p);
        }
    }
}

static void hub_disconnect(usb_device_t *dev, usb_binding_t *b) {
    hub_t *h = b->priv;
    if (!h) return;
    if (h->pending && h->ep) usb_intr_cancel(dev, h->ep);
    kfree(h->buf);
    kfree(h);
    b->priv = NULL;
}

const usb_driver_t usb_hub_driver = { "hub", hub_probe, hub_poll, hub_disconnect };
