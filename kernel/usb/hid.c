/* Halcyon OS - USB HID class driver: keyboards (including n-key rollover
 * and multimedia keys), mice and tablets.
 *
 * Every HID interface whose report descriptor (hidparse.c) contains a
 * keyboard, mouse/pointer, consumer control or system control application
 * is attached in report protocol, whatever its subclass, as Windows does.
 * One interface may carry several applications (wireless receivers); its
 * reports are told apart by report ID. Interfaces whose descriptor cannot be
 * read or parsed fall back to the boot protocol when they are boot
 * keyboards or boot mice.
 *
 * Key changes go to input_key() through a reference count per key code, so
 * a key reported by two interfaces (a gaming keyboard's boot and NKRO
 * interfaces, two keyboards holding Shift) is pressed and released once.
 */
#include "usb.h"
#include "hidparse.h"
#include "core/irq.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "drivers/input.h"

#define HID_SET_REPORT   0x09
#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B

#define HID_REQ_TIMEOUT  500     /* ms; SET_IDLE / SET_PROTOCOL may be NAKed or STALLed */
#define MAX_REPORT_DESC  4096
#define IN_BUF           64

typedef struct hid {
    hid_desc_t desc;
    hid_state_t st;
    usb_endpoint_t *ep, *out_ep;
    u8 *buf;
    int in_len;            /* bytes requested per interrupt IN transfer */
    int pending;
    u8 intf;
    u8 report_mode;        /* 0 = boot protocol */
    u8 boot_mouse;         /* boot protocol mouse: fixed 3/4-byte reports */
    /* keyboard LEDs */
    u32 led_gen;
    u8 led_buf[64];
    u8 led_intr;           /* send them on the interrupt OUT endpoint */
    u8 led_intr_tried;
    u8 led_pending;
    u64 led_deadline;
    char name[48];
} hid_t;

/* ---------- key events (shared by all HID interfaces) ---------- */
static u8 key_refs[256];
static u8 repeat_key;
static u64 repeat_at;

static int repeats(u8 key) { return key < 0xE0 && key != 0x7F; }   /* not modifiers, Mute, or 0xE8+ */

static void sink_key(void *ctx, u8 key, int pressed) {
    if (pressed) {
        if (key_refs[key]) { if (key_refs[key] < 255) key_refs[key]++; return; }
        key_refs[key] = 1;
    } else {
        if (!key_refs[key] || --key_refs[key]) return;
    }
    input_key(key, pressed);
    const char *name = hid_key_name(key);
    if (name) kprintf("usb-hid: debug: %s key %s\n", name, pressed ? "pressed" : "released");
    if (pressed && repeats(key)) {
        repeat_key = key;
        repeat_at = time_ms() + 500;
    } else if (!pressed && repeat_key == key) {
        repeat_key = 0;
    }
}

static void sink_rel(void *ctx, int dx, int dy, int dz, u8 buttons) { input_mouse_rel(dx, dy, dz, buttons); }
static void sink_abs(void *ctx, int x, int y, int dz, u8 buttons) { input_mouse_abs(x, y, dz, buttons); }

static const hid_sink_t sink = { NULL, sink_key, sink_rel, sink_abs };

/* ---------- probe ---------- */
static int class_request(usb_device_t *dev, u8 req, u16 value, u16 ifn) {
    return usb_control_timeout(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, req, value, ifn,
                               NULL, 0, HID_REQ_TIMEOUT);
}

/* wDescriptorLength of the report descriptor, from the HID descriptor */
static u16 report_desc_len(usb_device_t *dev, const usb_interface_desc_t *intf) {
    const u8 *p = (const u8 *)intf + intf->bLength, *end = dev->config + dev->config_len;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end && p[1] != USB_DT_INTERFACE) {
        if (p[1] == USB_DT_HID && p[0] >= 9) {
            int n = p[5];
            for (int i = 0; i < n && 6 + 3 * i + 2 < p[0]; i++)
                if (p[6 + 3 * i] == USB_DT_REPORT) return (u16)(p[7 + 3 * i] | (p[8 + 3 * i] << 8));
            return 0;
        }
        p += p[0];
    }
    return 0;
}

static u8 hid_apps(const hid_t *h) { return h->boot_mouse ? HID_APP_MOUSE : h->desc.apps; }

/* dev->description covers all HID interfaces of the device */
static void describe_device(usb_device_t *dev, const hid_t *cur) {
    u8 apps = hid_apps(cur), abs = cur->desc.ptr.absolute;
    for (int i = 0; i < dev->nbind; i++) {
        const hid_t *o = dev->bind[i].driver == &usb_hid_driver ? dev->bind[i].priv : NULL;
        if (o) { apps |= hid_apps(o); abs |= o->desc.ptr.absolute; }
    }
    const char *ptr = abs ? "tablet" : "mouse";
    if ((apps & HID_APP_KEYBOARD) && (apps & HID_APP_MOUSE))
        snprintf(dev->description, sizeof dev->description, "USB keyboard and %s", ptr);
    else if (apps & HID_APP_KEYBOARD)
        snprintf(dev->description, sizeof dev->description, "USB keyboard");
    else if (apps & HID_APP_MOUSE)
        snprintf(dev->description, sizeof dev->description, "%s", abs ? "USB tablet (absolute pointer)" : "USB mouse");
    else
        snprintf(dev->description, sizeof dev->description, "USB keyboard (media keys only)");
}

static int hid_probe(usb_device_t *dev, const usb_interface_desc_t *intf, usb_binding_t *b) {
    if (intf->bInterfaceClass != USB_CLASS_HID) return E_NOTSUP;
    usb_endpoint_t *ep = usb_find_endpoint(dev, intf->bInterfaceNumber, USB_EP_INTR, USB_DIR_IN);
    if (!ep) return E_NOTSUP;
    u16 ifn = intf->bInterfaceNumber;
    int boot_kbd = intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 1;
    int boot_mouse = intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 2;

    hid_t *h = kzalloc(sizeof *h);
    if (!h) return E_NOMEM;
    h->ep = ep;
    h->intf = (u8)ifn;
    if (dev->product[0]) snprintf(h->name, sizeof h->name, "%s (interface %u)", dev->product, ifn);
    else snprintf(h->name, sizeof h->name, "device %u interface %u", dev->id, ifn);

    /* report descriptor */
    const char *why = NULL;
    u16 rlen = report_desc_len(dev, intf);
    if (!rlen) why = "no report descriptor";
    else if (rlen > MAX_REPORT_DESC) why = "report descriptor too long";
    else {
        u8 *rd = kzalloc(rlen);
        int r = E_NOMEM;
        for (int tries = 0; rd && tries < 2; tries++) {
            r = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                            USB_DT_REPORT << 8, ifn, rd, rlen);
            if (r > 0) break;
            mdelay(20);
        }
        if (r <= 0) why = "cannot read the report descriptor";
        else if (!hid_parse(&h->desc, rd, r)) why = "no keyboard, mouse or consumer control in the report descriptor";
        kfree(rd);
    }

    const char *proto;
    if (!why) {
        /* devices start in report protocol; boot devices may have been left
         * in boot protocol by the firmware */
        h->report_mode = 1;
        proto = "report protocol";
        if (intf->bInterfaceSubClass == 1) class_request(dev, HID_SET_PROTOCOL, 1, ifn);
    } else if (boot_kbd || boot_mouse) {
        if (boot_kbd) hid_boot_keyboard(&h->desc);
        else h->boot_mouse = 1;
        proto = "boot protocol";
        int r = class_request(dev, HID_SET_PROTOCOL, 0, ifn);
        if (r < 0) kprintf("usb-hid: %s: SET_PROTOCOL(boot) failed (%s)\n", h->name, strerror(r));
    } else {
        kprintf("usb-hid: %s: not used (%s)\n", h->name, why);
        kfree(h);
        return E_NOTSUP;
    }
    /* report only on change; a STALL here is harmless */
    int r = class_request(dev, HID_SET_IDLE, 0, ifn);
    if (r < 0 && r != E_STALL) kprintf("usb-hid: %s: SET_IDLE failed (%s), ignored\n", h->name, strerror(r));

    int mps = ep->mps ? ep->mps : 8;
    h->in_len = MIN(mps, IN_BUF);
    /* a report longer than a packet arrives in several (low-speed devices) */
    if (h->report_mode && h->desc.max_in > h->in_len) h->in_len = MIN((int)h->desc.max_in, IN_BUF);
    h->buf = kzalloc(IN_BUF);
    if (h->desc.led_valid) {
        h->out_ep = usb_find_endpoint(dev, intf->bInterfaceNumber, USB_EP_INTR, USB_DIR_OUT);
        h->led_gen = input_led_generation() - 1;    /* set the LEDs on the first poll */
    }
    b->priv = h;
    describe_device(dev, h);

    char what[160];
    if (h->boot_mouse) snprintf(what, sizeof what, "mouse (boot protocol)");
    else hid_describe(&h->desc, proto, what, sizeof what);
    kprintf("usb-hid: %s: %s%s%s\n", h->name, what, why ? " - " : "", why ? why : "");
    return 0;
}

/* ---------- LEDs ---------- */
static void led_update(usb_device_t *dev, hid_t *h) {
    if (h->led_pending) {      /* interrupt OUT transfer in flight */
        int r = usb_intr_poll(dev, h->out_ep);
        if (r == E_AGAIN) {
            if (time_ms() < h->led_deadline) return;
            usb_intr_cancel(dev, h->out_ep);
            r = E_TIMEOUT;
        }
        h->led_pending = 0;
        if (r < 0) {
            kprintf("usb-hid: %s: LED report on the interrupt OUT endpoint failed (%s), using SET_REPORT\n",
                    h->name, strerror(r));
            if (r == E_STALL) usb_clear_halt(dev, h->out_ep);
            h->led_intr = 0;
            h->led_gen--;      /* send again */
        }
        return;
    }
    u32 gen = input_led_generation();
    if (gen == h->led_gen) return;
    h->led_gen = gen;
    u8 leds = input_leds();
    int n = hid_led_report(&h->desc, leds, h->led_buf, sizeof h->led_buf);
    if (n <= 0) return;
    kprintf("usb-hid: debug: %s: LEDs%s%s%s%s\n", h->name, leds & LED_NUM ? " Num" : "",
            leds & LED_CAPS ? " Caps" : "", leds & LED_SCROLL ? " Scroll" : "", leds & 7 ? "" : " off");
    if (h->led_intr) {
        if (usb_intr_start(dev, h->out_ep, h->led_buf, n) >= 0) {
            h->led_pending = 1;
            h->led_deadline = time_ms() + 250;
            return;
        }
        h->led_intr = 0;
    }
    int r = usb_control_timeout(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_REPORT,
                                (u16)(0x0200 | h->desc.led_rid), h->intf, h->led_buf, (u16)n, 100);
    if (r < 0 && h->out_ep && !h->led_intr_tried) {
        kprintf("usb-hid: %s: SET_REPORT(LEDs) failed (%s), trying the interrupt OUT endpoint\n", h->name, strerror(r));
        h->led_intr = h->led_intr_tried = 1;
        h->led_gen--;
    }
}

/* ---------- reports ---------- */
static void boot_mouse_report(const u8 *r, int len) {
    if (len < 3) return;
    int dz = len >= 4 ? (i8)r[3] : 0;
    input_mouse_rel((i8)r[1], (i8)r[2], dz, r[0] & 7);
}

static void hid_poll(usb_device_t *dev, usb_binding_t *b) {
    hid_t *h = b->priv;
    if (!h) return;
    if (h->desc.led_valid) led_update(dev, h);
    if (repeat_key && time_ms() >= repeat_at) {
        input_key(repeat_key, 1);
        repeat_at = time_ms() + 33;
    }
    if (repeat_key) idle_wake_at(repeat_at);
    if (!h->pending) {
        int r = usb_intr_start(dev, h->ep, h->buf, h->in_len);
        if (r == E_INVAL && h->in_len > h->ep->mps) {   /* this HC cannot do multi-packet transfers */
            h->in_len = MIN((int)h->ep->mps, IN_BUF);
            r = usb_intr_start(dev, h->ep, h->buf, h->in_len);
        }
        if (r >= 0) h->pending = 1;
        return;
    }
    int r = usb_intr_poll(dev, h->ep);
    if (r == E_AGAIN) return;
    h->pending = 0;
    if (r > 0) {
        if (h->boot_mouse) boot_mouse_report(h->buf, r);
        else hid_input(&h->desc, &h->st, h->buf, r, &sink);
    } else if (r == E_STALL) {
        usb_clear_halt(dev, h->ep);
    }
}

static void hid_disconnect(usb_device_t *dev, usb_binding_t *b) {
    hid_t *h = b->priv;
    if (!h) return;
    if (h->pending) usb_intr_cancel(dev, h->ep);
    if (h->led_pending) usb_intr_cancel(dev, h->out_ep);
    hid_release_all(&h->desc, &h->st, &sink);     /* release any keys still held */
    kfree(h->buf);
    kfree(h);
    b->priv = NULL;
}

const usb_driver_t usb_hid_driver = { "hid", hid_probe, hid_poll, hid_disconnect };
