/* Halcyon OS - USB core: controller registry, enumeration, driver binding */
#include "usb.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

static usb_hc_t *hcs;
static usb_device_t *devices;
static u32 generation;
static u8 next_hc_id, next_dev_id;

static const usb_driver_t *drivers[] = { &usb_hub_driver, &usb_hid_driver, &usb_msc_driver };

usb_device_t *usb_first_device(void) { return devices; }
usb_hc_t *usb_first_hc(void) { return hcs; }
u32 usb_generation(void) { return generation; }

const char *usb_speed_name(int s) {
    switch (s) {
    case USB_SPEED_LOW: return "Low speed (1.5 Mb/s)";
    case USB_SPEED_FULL: return "Full speed (12 Mb/s)";
    case USB_SPEED_HIGH: return "High speed (480 Mb/s)";
    case USB_SPEED_SUPER: return "SuperSpeed (5 Gb/s)";
    case USB_SPEED_SUPERPLUS: return "SuperSpeed+ (10 Gb/s)";
    }
    return "Unknown speed";
}

const char *usb_class_name(u8 c) {
    switch (c) {
    case 0x00: return "Composite device";
    case 0x01: return "Audio";
    case 0x02: return "Communications";
    case 0x03: return "Human Interface Device";
    case 0x05: return "Physical";
    case 0x06: return "Imaging";
    case 0x07: return "Printer";
    case 0x08: return "Mass Storage";
    case 0x09: return "Hub";
    case 0x0A: return "CDC Data";
    case 0x0B: return "Smart Card";
    case 0x0E: return "Video";
    case 0x0F: return "Healthcare";
    case 0x10: return "Audio/Video";
    case 0xDC: return "Diagnostic";
    case 0xE0: return "Wireless controller";
    case 0xEF: return "Miscellaneous";
    case 0xFE: return "Application specific";
    case 0xFF: return "Vendor specific";
    }
    return "Unknown class";
}

/* ---------- controllers ---------- */
void usb_register_hc(usb_hc_t *hc) {
    hc->id = next_hc_id++;
    memset(hc->addr_used, 0, sizeof hc->addr_used);
    hc->addr_used[0] = 1;
    hc->next = NULL;
    usb_hc_t **pp = &hcs;
    while (*pp) pp = &(*pp)->next;
    *pp = hc;
    kprintf("usb: registered %s controller #%u (%s), %u ports\n", hc->name, hc->id, hc->desc, hc->nports);
}

#define WEAK __attribute__((weak))
void xhci_init(void) WEAK;
void ehci_init(void) WEAK;
void ohci_init(void) WEAK;
void uhci_init(void) WEAK;

void usb_init(void) {
    /* Order matters: xHCI first (Intel port switching moves ports away from
     * EHCI), then EHCI before its UHCI/OHCI companions. */
    if (xhci_init) xhci_init();
    if (ehci_init) ehci_init();
    if (ohci_init) ohci_init();
    if (uhci_init) uhci_init();
    /* initial root hub scan so devices present at boot are enumerated now */
    for (int pass = 0; pass < 3; pass++) {
        for (usb_hc_t *hc = hcs; hc; hc = hc->next) hc->ops->poll(hc);
        usb_poll();
    }
}

static u64 last_hc_poll;
void usb_poll(void) {
    u64 now = time_ms();
    if (now - last_hc_poll >= 20 || last_hc_poll == 0) {
        last_hc_poll = now;
        for (usb_hc_t *hc = hcs; hc; hc = hc->next) hc->ops->poll(hc);
    }
    for (usb_device_t *d = devices; d; d = d->next) {
        if (!d->alive) continue;
        for (int i = 0; i < d->nbind; i++) {
            usb_binding_t *b = &d->bind[i];
            if (b->driver && b->driver->poll) b->driver->poll(d, b);
            if (!d->alive) break;
        }
    }
}

/* ---------- transfers ---------- */
int usb_control_timeout(usb_device_t *dev, u8 reqtype, u8 req, u16 value, u16 index, void *data, u16 len, int timeout_ms) {
    if (!dev->alive && dev->address != 0 && !(dev->hc->flags & USB_HC_ASSIGNS_ADDRESS)) {
        /* still allowed during enumeration; alive is set at the end */
    }
    usb_setup_t s = { reqtype, req, value, index, len };
    return dev->hc->ops->control(dev->hc, dev, &s, data, timeout_ms);
}

int usb_control(usb_device_t *dev, u8 reqtype, u8 req, u16 value, u16 index, void *data, u16 len) {
    return usb_control_timeout(dev, reqtype, req, value, index, data, len, 5000);
}

int usb_get_descriptor(usb_device_t *dev, u8 type, u8 index, u16 lang, void *buf, u16 len) {
    return usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                       (u16)((type << 8) | index), lang, buf, len);
}

int usb_bulk(usb_device_t *dev, usb_endpoint_t *ep, void *data, int len, int timeout_ms) {
    return dev->hc->ops->bulk(dev->hc, dev, ep, data, len, timeout_ms);
}

int usb_clear_halt(usb_device_t *dev, usb_endpoint_t *ep) {
    int r = usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                        USB_FEATURE_ENDPOINT_HALT, ep->address, NULL, 0);
    if (dev->hc->ops->reset_endpoint) dev->hc->ops->reset_endpoint(dev->hc, dev, ep);
    else ep->toggle = 0;
    return r;
}

int usb_intr_start(usb_device_t *dev, usb_endpoint_t *ep, void *buf, int len) {
    return dev->hc->ops->intr_start(dev->hc, dev, ep, buf, len);
}
int usb_intr_poll(usb_device_t *dev, usb_endpoint_t *ep) {
    return dev->hc->ops->intr_poll(dev->hc, dev, ep);
}
void usb_intr_cancel(usb_device_t *dev, usb_endpoint_t *ep) {
    if (dev->hc->ops->intr_cancel) dev->hc->ops->intr_cancel(dev->hc, dev, ep);
}

usb_endpoint_t *usb_find_endpoint(usb_device_t *dev, u8 interface, u8 type, u8 dir) {
    for (int i = 1; i < 32; i++) {
        usb_endpoint_t *ep = dev->eps[i];
        if (ep && ep->interface == interface && ep->type == type && (ep->address & 0x80) == dir) return ep;
    }
    return NULL;
}

/* ---------- enumeration ---------- */
static int alloc_address(usb_hc_t *hc) {
    for (int a = 1; a < 128; a++) if (!hc->addr_used[a]) { hc->addr_used[a] = 1; return a; }
    return -1;
}

static void read_string(usb_device_t *dev, u8 index, u16 lang, char *out, int outsz) {
    out[0] = 0;
    if (!index) return;
    u8 *buf = kzalloc(256);
    if (!buf) return;
    int r = usb_get_descriptor(dev, USB_DT_STRING, index, lang, buf, 255);
    if (r >= 2 && buf[1] == USB_DT_STRING) {
        int n = MIN(r, buf[0]);
        int o = 0;
        for (int i = 2; i + 1 < n && o < outsz - 1; i += 2) {
            u16 ch = buf[i] | (buf[i + 1] << 8);
            out[o++] = (ch >= 32 && ch < 127) ? (char)ch : (ch == 0 ? 0 : '?');
            if (!ch) break;
        }
        out[o] = 0;
        /* trim trailing spaces */
        while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    }
    kfree(buf);
}

static void parse_config(usb_device_t *dev) {
    u8 *p = dev->config, *end = dev->config + dev->config_len;
    u8 cur_intf = 0, cur_alt = 0;
    usb_endpoint_t *last = NULL;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            cur_intf = p[2]; cur_alt = p[3];
            last = NULL;
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && cur_alt == 0) {
            usb_endpoint_desc_t *e = (usb_endpoint_desc_t *)p;
            int idx = (e->bEndpointAddress & 0x0F) + ((e->bEndpointAddress & 0x80) ? 16 : 0);
            if (idx > 0 && idx < 32 && !dev->eps[idx]) {
                usb_endpoint_t *ep = kzalloc(sizeof *ep);
                ep->address = e->bEndpointAddress;
                ep->type = e->bmAttributes & 3;
                ep->mps = e->wMaxPacketSize & 0x7FF;
                ep->mult = (e->wMaxPacketSize >> 11) & 3;
                ep->interval = e->bInterval;
                ep->interface = cur_intf;
                dev->eps[idx] = ep;
                last = ep;
            }
        } else if (p[1] == USB_DT_SS_EP_COMPANION && p[0] >= 6 && last) {
            last->max_burst = p[2];
        }
        p += p[0];
    }
}

static void free_device(usb_device_t *dev) {
    usb_hc_t *hc = dev->hc;
    if (hc->ops->device_free) hc->ops->device_free(hc, dev);
    if (dev->address && !(hc->flags & USB_HC_ASSIGNS_ADDRESS)) hc->addr_used[dev->address] = 0;
    for (int i = 0; i < 32; i++) kfree(dev->eps[i]);
    kfree(dev->config);
    kfree(dev);
}

static void bind_drivers(usb_device_t *dev) {
    u8 *p = dev->config, *end = dev->config + dev->config_len;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end && dev->nbind < USB_MAX_INTERFACES) {
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9 && p[3] == 0) {
            const usb_interface_desc_t *intf = (const usb_interface_desc_t *)p;
            for (size_t i = 0; i < ARRAY_SIZE(drivers); i++) {
                usb_binding_t *b = &dev->bind[dev->nbind];
                memset(b, 0, sizeof *b);
                b->interface = intf->bInterfaceNumber;
                if (drivers[i]->probe(dev, intf, b) == 0) {
                    b->driver = drivers[i];
                    dev->nbind++;
                    kprintf("usb: dev %u interface %u bound to %s\n", dev->id, intf->bInterfaceNumber, drivers[i]->name);
                    break;
                }
            }
        }
        p += p[0];
    }
}

usb_device_t *usb_attach_device(usb_hc_t *hc, usb_device_t *parent, int port, int speed) {
    usb_device_t *dev = kzalloc(sizeof *dev);
    if (!dev) return NULL;
    dev->hc = hc;
    dev->parent = parent;
    dev->port = (u8)port;
    dev->speed = (u8)speed;
    dev->id = next_dev_id++;
    if (parent) {
        dev->root_port = parent->root_port;
        dev->hub_depth = parent->hub_depth + 1;
        dev->route = parent->route | ((u32)MIN(port, 15) << (4 * parent->hub_depth));
        if (speed <= USB_SPEED_FULL) {
            if (parent->speed == USB_SPEED_HIGH) { dev->tt_hub = parent; dev->tt_port = (u8)port; }
            else { dev->tt_hub = parent->tt_hub; dev->tt_port = parent->tt_port; }
        }
    } else {
        dev->root_port = (u8)port;
        dev->hub_depth = 0;
        dev->route = 0;
    }
    dev->ep0.address = 0;
    dev->ep0.type = USB_EP_CONTROL;
    dev->ep0.mps = speed == USB_SPEED_LOW ? 8 : (speed >= USB_SPEED_SUPER ? 512 : 64);

    kprintf("usb: new %s device on %s port %d\n", usb_speed_name(speed),
            parent ? "hub" : hc->name, port);
    int r = hc->ops->device_init(hc, dev);
    if (r < 0) { kprintf("usb: device_init failed (%s)\n", strerror(r)); free_device(dev); return NULL; }

    /* first 8 bytes of the device descriptor -> bMaxPacketSize0 */
    u8 *buf = kzalloc(64);
    for (int tries = 0; tries < 3; tries++) {
        r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, buf, 8);
        if (r >= 8) break;
        mdelay(50);
    }
    if (r < 8) { kprintf("usb: GET_DESCRIPTOR(8) failed: %s\n", strerror(r)); goto fail; }
    u16 mps0 = buf[7];
    if (speed >= USB_SPEED_SUPER) mps0 = (mps0 >= 5 && mps0 <= 9) ? (u16)(1u << mps0) : 512;
    if (mps0 != 8 && mps0 != 16 && mps0 != 32 && mps0 != 64 && mps0 != 512) mps0 = 8;
    dev->ep0.mps = mps0;

    if (!(hc->flags & USB_HC_ASSIGNS_ADDRESS)) {
        int addr = alloc_address(hc);
        if (addr < 0) { kprintf("usb: out of addresses\n"); goto fail; }
        r = usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, (u16)addr, 0, NULL, 0);
        if (r < 0) { hc->addr_used[addr] = 0; kprintf("usb: SET_ADDRESS failed: %s\n", strerror(r)); goto fail; }
        dev->address = (u8)addr;
        mdelay(10);
    }
    if (hc->ops->device_update) {
        r = hc->ops->device_update(hc, dev);
        if (r < 0) { kprintf("usb: device_update failed\n"); goto fail; }
    }

    r = usb_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, sizeof dev->desc);
    if (r < (int)sizeof dev->desc) { kprintf("usb: GET_DESCRIPTOR(device) failed: %s\n", strerror(r)); goto fail; }

    r = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, buf, 9);
    if (r < 9) { kprintf("usb: GET_DESCRIPTOR(config) failed: %s\n", strerror(r)); goto fail; }
    u16 total = ((usb_config_desc_t *)buf)->wTotalLength;
    u8 cfgval = ((usb_config_desc_t *)buf)->bConfigurationValue;
    if (total < 9) total = 9;
    if (total > 4096) total = 4096;
    dev->config = kzalloc(total);
    r = usb_get_descriptor(dev, USB_DT_CONFIG, 0, 0, dev->config, total);
    if (r < 9) { kprintf("usb: reading configuration failed: %s\n", strerror(r)); goto fail; }
    dev->config_len = (u16)r;

    /* strings */
    u16 lang = 0x0409;
    if (dev->desc.iManufacturer || dev->desc.iProduct || dev->desc.iSerialNumber) {
        memset(buf, 0, 64);
        r = usb_get_descriptor(dev, USB_DT_STRING, 0, 0, buf, 64);
        if (r >= 4) {
            lang = buf[2] | (buf[3] << 8);
            for (int i = 2; i + 1 < r; i += 2) if ((buf[i] | (buf[i + 1] << 8)) == 0x0409) lang = 0x0409;
        }
        read_string(dev, dev->desc.iManufacturer, lang, dev->manufacturer, sizeof dev->manufacturer);
        read_string(dev, dev->desc.iProduct, lang, dev->product, sizeof dev->product);
        read_string(dev, dev->desc.iSerialNumber, lang, dev->serial, sizeof dev->serial);
    }
    if (!dev->product[0]) snprintf(dev->product, sizeof dev->product, "USB device %04x:%04x", dev->desc.idVendor, dev->desc.idProduct);
    kfree(buf);
    buf = NULL;

    parse_config(dev);
    if (hc->ops->configure) {
        r = hc->ops->configure(hc, dev);
        if (r < 0) { kprintf("usb: HC configure failed: %s\n", strerror(r)); goto fail; }
    }
    r = usb_control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, cfgval, 0, NULL, 0);
    if (r < 0) { kprintf("usb: SET_CONFIGURATION failed: %s\n", strerror(r)); goto fail; }

    kprintf("usb: device %u: %04x:%04x \"%s\" \"%s\" class %02x, addr %u, mps0 %u\n", dev->id,
            dev->desc.idVendor, dev->desc.idProduct, dev->manufacturer, dev->product,
            dev->desc.bDeviceClass, dev->address, dev->ep0.mps);

    dev->alive = 1;
    /* append to the device list before binding so hubs can enumerate children */
    usb_device_t **pp = &devices;
    while (*pp) pp = &(*pp)->next;
    *pp = dev;
    generation++;
    bind_drivers(dev);
    if (!dev->description[0]) {
        if (dev->nbind == 0) snprintf(dev->description, sizeof dev->description, "%s (no driver)", usb_class_name(dev->desc.bDeviceClass));
    }
    return dev;

fail:
    kfree(buf);
    free_device(dev);
    return NULL;
}

usb_device_t *usb_find_child(usb_hc_t *hc, usb_device_t *parent, int port) {
    for (usb_device_t *d = devices; d; d = d->next)
        if (d->hc == hc && d->parent == parent && d->port == port) return d;
    return NULL;
}

static void detach(usb_device_t *dev) {
    /* children first */
    for (;;) {
        usb_device_t *c = NULL;
        for (usb_device_t *d = devices; d; d = d->next) if (d->parent == dev) { c = d; break; }
        if (!c) break;
        detach(c);
    }
    kprintf("usb: device %u (%s) disconnected\n", dev->id, dev->product);
    dev->alive = 0;
    for (int i = 0; i < dev->nbind; i++)
        if (dev->bind[i].driver && dev->bind[i].driver->disconnect) dev->bind[i].driver->disconnect(dev, &dev->bind[i]);
    usb_device_t **pp = &devices;
    while (*pp && *pp != dev) pp = &(*pp)->next;
    if (*pp) *pp = dev->next;
    generation++;
    free_device(dev);
}

void usb_port_disconnected(usb_hc_t *hc, usb_device_t *parent, int port) {
    usb_device_t *d = usb_find_child(hc, parent, port);
    if (d) detach(d);
}
