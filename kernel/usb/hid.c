/* Halcyon OS - USB HID: keyboards (boot protocol) and mice/tablets
 * (report descriptor parsing with boot-protocol fallback) */
#include "usb.h"
#include "core/irq.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "drivers/input.h"

#define HID_GET_REPORT   0x01
#define HID_SET_REPORT   0x09
#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B

enum { HID_KBD = 1, HID_MOUSE };

typedef struct field { u16 bit; u8 size; u8 valid; i32 lmin, lmax; } field_t;

typedef struct hid {
    int kind;
    usb_endpoint_t *ep;
    u8 *buf;
    int pending;
    u8 intf;
    /* keyboard */
    u8 prev[8];
    u8 repeat_key;
    u64 repeat_at;
    u32 led_gen;
    /* mouse (report protocol) */
    int report_mode;
    u8 report_id;          /* 0 = no report IDs */
    field_t x, y, wheel;
    field_t btn[5];
    int absolute;
    u8 last_buttons;
} hid_t;

/* ---------- report descriptor parser (pointer devices) ---------- */
static i32 sext(u32 v, int bytes) {
    if (bytes == 1) return (i8)v;
    if (bytes == 2) return (i16)v;
    return (i32)v;
}

static int parse_report(hid_t *h, const u8 *d, int len) {
    u32 upage = 0, rsize = 0, rcount = 0, rid = 0;
    i32 lmin = 0, lmax = 0;
    u32 usages[16]; int nus = 0; u32 umin = 0; int have_range = 0;
    u32 bitpos[256] = { 0 };   /* per report id */
    int found_xy = 0;
    u8 field_rid = 0;
    for (int i = 0; i < len;) {
        u8 pfx = d[i];
        if (pfx == 0xFE) { if (i + 1 < len) i += 3 + d[i + 1]; else break; continue; }  /* long item */
        int sz = pfx & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > len) break;
        u32 v = 0;
        for (int k = 0; k < sz; k++) v |= (u32)d[i + 1 + k] << (8 * k);
        int type = (pfx >> 2) & 3, tag = pfx >> 4;
        i += 1 + sz;
        if (type == 1) {            /* global */
            switch (tag) {
            case 0: upage = v; break;
            case 1: lmin = sext(v, sz); break;
            case 2: lmax = sz < 4 && lmin >= 0 ? (i32)v : sext(v, sz); break;
            case 7: rsize = v; break;
            case 8: rid = v & 0xFF; break;
            case 9: rcount = v; break;
            }
        } else if (type == 2) {     /* local */
            u32 u = sz == 4 ? v : ((upage << 16) | v);
            if (tag == 0 && nus < 16) usages[nus++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { have_range = 1; }
        } else if (type == 0) {     /* main */
            if (tag == 8) {         /* Input */
                int is_const = v & 1, is_rel = (v >> 2) & 1;
                for (u32 n = 0; n < rcount; n++) {
                    u32 u = 0;
                    if (have_range) u = umin + n;
                    else if (nus) u = usages[n < (u32)nus ? n : (u32)nus - 1];
                    u32 pos = bitpos[rid];
                    if (!is_const && (field_rid == rid || !found_xy)) {
                        u16 pg = u >> 16, us = u & 0xFFFF;
                        field_t f = { (u16)pos, (u8)rsize, 1, lmin, lmax };
                        if (pg == 0x01 && us == 0x30 && !h->x.valid) { h->x = f; h->absolute = !is_rel; found_xy = 1; field_rid = (u8)rid; }
                        else if (pg == 0x01 && us == 0x31 && !h->y.valid) h->y = f;
                        else if (pg == 0x01 && us == 0x38 && !h->wheel.valid) h->wheel = f;
                        else if (pg == 0x09 && us >= 1 && us <= 5 && !h->btn[us - 1].valid) h->btn[us - 1] = f;
                    }
                    bitpos[rid] += rsize;
                }
            } else if (tag == 9 || tag == 11) {   /* Output / Feature: no input bits */
            }
            nus = 0; have_range = 0;
        }
    }
    h->report_id = field_rid;
    return (h->x.valid && h->y.valid) ? 0 : -1;
}

static i32 get_field(const u8 *buf, int len, const field_t *f, int is_signed) {
    if (!f->valid || f->size == 0 || f->size > 32) return 0;
    u32 v = 0;
    for (int b = 0; b < f->size; b++) {
        int bit = f->bit + b;
        if (bit / 8 >= len) break;
        if (buf[bit / 8] & (1 << (bit % 8))) v |= 1u << b;
    }
    if (is_signed && f->size < 32 && (v & (1u << (f->size - 1)))) v |= ~0u << f->size;
    return (i32)v;
}

/* ---------- probe ---------- */
static int hid_probe(usb_device_t *dev, const usb_interface_desc_t *intf, usb_binding_t *b) {
    if (intf->bInterfaceClass != USB_CLASS_HID) return E_NOTSUP;
    int kind = 0;
    if (intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 1) kind = HID_KBD;
    else if (intf->bInterfaceProtocol == 2 || intf->bInterfaceSubClass == 0) kind = HID_MOUSE;
    if (!kind) return E_NOTSUP;
    usb_endpoint_t *ep = usb_find_endpoint(dev, intf->bInterfaceNumber, USB_EP_INTR, USB_DIR_IN);
    if (!ep) return E_NOTSUP;

    hid_t *h = kzalloc(sizeof *h);
    h->kind = kind;
    h->ep = ep;
    h->intf = intf->bInterfaceNumber;
    h->buf = kzalloc(MAX(64, ep->mps));
    u16 ifn = intf->bInterfaceNumber;

    if (kind == HID_KBD) {
        usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_PROTOCOL, 0, ifn, NULL, 0);
        usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_IDLE, 0, ifn, NULL, 0);
        h->led_gen = ~0u;
        snprintf(dev->description, sizeof dev->description, "USB keyboard");
    } else {
        /* find the report descriptor length in the HID descriptor */
        u16 rlen = 0;
        const u8 *p = (const u8 *)intf + intf->bLength, *end = dev->config + dev->config_len;
        while (p + 2 <= end && p[0] >= 2 && p[1] != USB_DT_INTERFACE) {
            if (p[1] == USB_DT_HID && p[0] >= 9) { rlen = p[7] | (p[8] << 8); break; }
            p += p[0];
        }
        int ok = -1;
        if (rlen && rlen <= 1024) {
            u8 *rd = kzalloc(rlen);
            int r = usb_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                                USB_DT_REPORT << 8, ifn, rd, rlen);
            if (r > 0) ok = parse_report(h, rd, r);
            kfree(rd);
        }
        if (ok == 0) {
            h->report_mode = 1;
        } else if (intf->bInterfaceSubClass == 1 && intf->bInterfaceProtocol == 2) {
            usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_PROTOCOL, 0, ifn, NULL, 0);
            h->report_mode = 0;
        } else {
            kfree(h->buf); kfree(h);
            return E_NOTSUP;
        }
        usb_control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_IDLE, 0, ifn, NULL, 0);
        snprintf(dev->description, sizeof dev->description, "%s", h->absolute ? "USB tablet (absolute pointer)" : "USB mouse");
    }
    b->priv = h;
    kprintf("usb-hid: %s (%s protocol)\n", dev->description, h->report_mode ? "report" : "boot");
    return 0;
}

/* ---------- reports ---------- */
static int in_report(const u8 *r, u8 key) {
    for (int i = 2; i < 8; i++) if (r[i] == key) return 1;
    return 0;
}

static void kbd_report(hid_t *h, const u8 *r, int len) {
    if (len < 3) return;
    u8 cur[8] = { 0 };
    memcpy(cur, r, MIN(len, 8));
    if (cur[2] == 1) return;   /* rollover error */
    /* modifiers */
    u8 chg = cur[0] ^ h->prev[0];
    for (int i = 0; i < 8; i++)
        if (chg & (1 << i)) input_key((u8)(0xE0 + i), (cur[0] >> i) & 1);
    for (int i = 2; i < 8; i++)
        if (h->prev[i] > 3 && !in_report(cur, h->prev[i])) {
            input_key(h->prev[i], 0);
            if (h->repeat_key == h->prev[i]) h->repeat_key = 0;
        }
    for (int i = 2; i < 8; i++)
        if (cur[i] > 3 && !in_report(h->prev, cur[i])) {
            input_key(cur[i], 1);
            h->repeat_key = cur[i];
            h->repeat_at = time_ms() + 500;
        }
    memcpy(h->prev, cur, 8);
}

static void mouse_report(hid_t *h, const u8 *r, int len) {
    if (!h->report_mode) {
        if (len < 3) return;
        u8 btn = r[0] & 7;
        int dz = len >= 4 ? (i8)r[3] : 0;
        input_mouse_rel((i8)r[1], (i8)r[2], dz, btn);
        return;
    }
    if (h->report_id) {
        if (r[0] != h->report_id) return;
        r++; len--;
    }
    u8 btn = 0;
    for (int i = 0; i < 3; i++) if (get_field(r, len, &h->btn[i], 0)) btn |= 1 << i;
    int dz = h->wheel.valid ? get_field(r, len, &h->wheel, 1) : 0;
    if (h->absolute) {
        i32 x = get_field(r, len, &h->x, 0), y = get_field(r, len, &h->y, 0);
        i32 xr = h->x.lmax - h->x.lmin, yr = h->y.lmax - h->y.lmin;
        if (xr <= 0) xr = 32767;
        if (yr <= 0) yr = 32767;
        input_mouse_abs((int)((i64)(x - h->x.lmin) * 32767 / xr), (int)((i64)(y - h->y.lmin) * 32767 / yr), dz, btn);
    } else {
        input_mouse_rel(get_field(r, len, &h->x, 1), get_field(r, len, &h->y, 1), dz, btn);
    }
}

static void hid_poll(usb_device_t *dev, usb_binding_t *b) {
    hid_t *h = b->priv;
    if (!h) return;
    if (h->kind == HID_KBD) {
        if (h->led_gen != input_led_generation()) {
            h->led_gen = input_led_generation();
            u8 leds = input_leds();
            usb_control_timeout(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_SET_REPORT,
                                0x0200, h->intf, &leds, 1, 100);
        }
        if (h->repeat_key && time_ms() >= h->repeat_at) {
            input_key(h->repeat_key, 1);
            h->repeat_at = time_ms() + 33;
        }
        if (h->repeat_key) idle_wake_at(h->repeat_at);
    }
    if (!h->pending) {
        if (usb_intr_start(dev, h->ep, h->buf, MIN((int)h->ep->mps, 64)) >= 0) h->pending = 1;
        return;
    }
    int r = usb_intr_poll(dev, h->ep);
    if (r == E_AGAIN) return;
    h->pending = 0;
    if (r > 0) {
        if (h->kind == HID_KBD) kbd_report(h, h->buf, r);
        else mouse_report(h, h->buf, r);
    } else if (r == E_STALL) {
        usb_clear_halt(dev, h->ep);
    }
}

static void hid_disconnect(usb_device_t *dev, usb_binding_t *b) {
    hid_t *h = b->priv;
    if (!h) return;
    if (h->pending) usb_intr_cancel(dev, h->ep);
    /* release any keys still held */
    if (h->kind == HID_KBD) {
        for (int i = 0; i < 8; i++) if (h->prev[0] & (1 << i)) input_key((u8)(0xE0 + i), 0);
        for (int i = 2; i < 8; i++) if (h->prev[i] > 3) input_key(h->prev[i], 0);
    }
    kfree(h->buf);
    kfree(h);
    b->priv = NULL;
}

const usb_driver_t usb_hid_driver = { "hid", hid_probe, hid_poll, hid_disconnect };
