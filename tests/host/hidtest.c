/* Host tests for the USB HID report descriptor parser and report decoder
 * (kernel/usb/hidparse.c, compiled unmodified; ASan + UBSan).
 *
 * Realistic report descriptors:
 *   QEMU usb-kbd, usb-tablet and usb-mouse (dumped from QEMU)
 *   the boot protocol keyboard layout (fallback)
 *   an n-key-rollover gaming keyboard: boot interface + an interface with a
 *       120-key bitmap, consumer control, system control and a vendor
 *       collection, told apart by report ID
 *   a wireless receiver: keyboard + mouse (12-bit X/Y, wheel, AC Pan) +
 *       consumer control + system control + vendor reports on one interface
 *   a consumer-control-only interface (16 one-bit usages from a usage list),
 *       and one with a relative Volume control, and a keyboard with a Power key
 * and hostile ones: every truncation of the above, 255 report IDs, huge
 * report counts and sizes, unbalanced collections, push/pop abuse, long
 * items, and random mutations fed with random reports (short, long, unknown
 * report IDs).
 *
 * Checks presses, releases (arrays and bitmaps), modifiers ahead of keys,
 * report order, rollover beyond six keys, ErrorRollOver, short reports,
 * report ID dispatch, consumer / system keys, pointer decoding, LED output
 * reports and the descriptions logged by the driver.
 *
 * usage: hidtest [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include "core/lib.h"     /* memcpy, strcmp, ... (string.h clashes with core/types.h) */
#include "usb/hidparse.h"
#include "drivers/input.h"

static int g_fail, g_checks, g_verbose;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define EXPECT_STR(got, want) do { const char *_g = (got), *_w = (want); \
    CHECK(!strcmp(_g, _w), "got \"%s\", want \"%s\"", _g, _w); } while (0)
#define EXPECT(got, want) do { long _g = (long)(got), _w = (long)(want); \
    CHECK(_g == _w, "%s = %ld, want %ld", #got, _g, _w); } while (0)

/* ---------- event recorder ---------- */
static char g_ev[4096];

static void ev_add(const char *fmt, int a, int b, int c, int d) {
    size_t n = strlen(g_ev);
    if (n >= sizeof g_ev - 64) return;
    snprintf(g_ev + n, sizeof g_ev - n, n ? " " : "");
    n = strlen(g_ev);
    snprintf(g_ev + n, sizeof g_ev - n, fmt, a, b, c, d);
}
static void rec_key(void *ctx, u8 key, int pressed) { ev_add(pressed ? "+%02X" : "-%02X", key, 0, 0, 0); }
static void rec_rel(void *ctx, int dx, int dy, int dz, u8 b) { ev_add("R(%d,%d,%d,%d)", dx, dy, dz, b); }
static void rec_abs(void *ctx, int x, int y, int dz, u8 b) { ev_add("A(%d,%d,%d,%d)", x, y, dz, b); }
static const hid_sink_t g_sink = { NULL, rec_key, rec_rel, rec_abs };

/* feed one report from an exactly sized heap buffer (ASan sees overreads) */
static const char *feed(const hid_desc_t *d, hid_state_t *s, const u8 *r, int len) {
    u8 *b = malloc(len ? len : 1);
    if (len) memcpy(b, r, len);
    g_ev[0] = 0;
    hid_input(d, s, b, len, &g_sink);
    free(b);
    if (g_verbose) printf("    report (%d bytes) -> %s\n", len, g_ev);
    return g_ev;
}
#define BYTES(...) (const u8[]){ __VA_ARGS__ }, (int)sizeof((const u8[]){ __VA_ARGS__ })
#define REPORT(d, s, ...) feed(d, s, BYTES(__VA_ARGS__))

static hid_desc_t *new_desc(void) { return calloc(1, sizeof(hid_desc_t)); }
static hid_state_t *new_state(void) { return calloc(1, sizeof(hid_state_t)); }

static const char *describe(const hid_desc_t *d, const char *proto) {
    static char buf[256];
    hid_describe(d, proto, buf, sizeof buf);
    return buf;
}

static const char *led(const hid_desc_t *d, u8 leds) {
    static char s[64];
    u8 b[16];
    int n = hid_led_report(d, leds, b, sizeof b);
    s[0] = 0;
    for (int i = 0; i < n; i++) snprintf(s + strlen(s), sizeof s - strlen(s), "%s%02X", i ? " " : "", b[i]);
    return s;
}

static const char *release_all(const hid_desc_t *d, hid_state_t *s) {
    g_ev[0] = 0;
    hid_release_all(d, s, &g_sink);
    return g_ev;
}

/* ---------- descriptors ---------- */
/* QEMU hw/usb/dev-hid.c, as read from the devices */
static const u8 qemu_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};
static const u8 qemu_tablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};
static const u8 qemu_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xc0, 0xc0,
};

/* gaming keyboard, interface 0: the HID 1.11 appendix B.1 boot keyboard */
static const u8 nkro_if0[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
/* gaming keyboard, interface 1: NKRO bitmap (report 1), consumer (2), system (3), vendor (4) */
static const u8 nkro_if1[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers */
    0x19, 0x00, 0x29, 0x77, 0x95, 0x78, 0x81, 0x02,                                                 /* 120-key bitmap */
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x91, 0x02, 0x95, 0x03, 0x91, 0x01,             /* LEDs */
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
    0x19, 0x00, 0x2A, 0x3C, 0x02, 0x15, 0x00, 0x26, 0x3C, 0x02, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, /* 16-bit array */
    0xC0,
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x03,
    0x19, 0x81, 0x29, 0x83, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01,
    0xC0,
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x04, 0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x3F, 0x81, 0x02, 0x09, 0x03, 0x91, 0x02, 0xC0,
};
/* wireless receiver: everything on one interface */
static const u8 receiver[] = {
    /* report 1: keyboard */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xA4, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xA4, 0x00, 0x81, 0x00,
    0xC0,
    /* report 2: mouse, 5 buttons, 12-bit X/Y, wheel, AC Pan */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
    /* report 3: consumer control, one 16-bit slot */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x75, 0x10, 0x95, 0x01, 0x15, 0x01, 0x26, 0x9C, 0x02,
    0x19, 0x01, 0x2A, 0x9C, 0x02, 0x81, 0x00, 0xC0,
    /* report 4: system control, 2-bit array over a usage list */
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x04, 0x75, 0x02, 0x95, 0x01, 0x15, 0x01, 0x25, 0x03,
    0x09, 0x82, 0x09, 0x81, 0x09, 0x83, 0x81, 0x60, 0x75, 0x06, 0x81, 0x03, 0xC0,
    /* report 0x10: vendor (HID++ style) */
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x10, 0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x09, 0x01, 0x81, 0x00, 0x09, 0x01, 0x91, 0x00, 0xC0,
};
/* media keys interface: 16 one-bit usages from a usage list, no report IDs */
static const u8 consumer_only[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10,
    0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7, 0x0A, 0x92, 0x01,
    0x0A, 0x23, 0x02, 0x0A, 0x8A, 0x01, 0x0A, 0x21, 0x02, 0x0A, 0x24, 0x02, 0x0A, 0x25, 0x02, 0x0A, 0x27, 0x02,
    0x0A, 0x2A, 0x02, 0x0A, 0x94, 0x01,
    0x81, 0x02, 0xC0,
};
/* relative Volume control (a knob), plus padding, plus a Long item and extended usages */
static const u8 volume_knob[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0xFE, 0x02, 0x10, 0xAA, 0xBB,                         /* long item: skipped */
    0x09, 0xE0, 0x15, 0xFF, 0x25, 0x01, 0x75, 0x02, 0x95, 0x01, 0x81, 0x06,   /* Volume, -1..1, relative */
    0x0B, 0xE2, 0x00, 0x0C, 0x00, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,   /* Mute as 32-bit usage */
    0x75, 0x05, 0x81, 0x01,
    0xC0,
};

/* laptop-style keyboard with a Power key (Generic Desktop) inside the keyboard application */
static const u8 kbd_power[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x19, 0x00, 0x29, 0x91, 0x26, 0x91, 0x00,
    0x81, 0x00,                                                  /* 1-slot array */
    0x05, 0x01, 0x09, 0x81, 0x75, 0x01, 0x95, 0x01, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,   /* Power */
    0x95, 0x07, 0x81, 0x03, 0xC0,
};

/* ---------- tests ---------- */
static void test_qemu_kbd(void) {
    printf("== QEMU usb-kbd\n");
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    EXPECT(hid_parse(d, qemu_kbd, sizeof qemu_kbd), HID_APP_KEYBOARD);
    EXPECT(d->has_ids, 0);
    EXPECT(d->nfields, 2);
    EXPECT(d->max_in, 8);
    EXPECT_STR(describe(d, "report protocol"), "keyboard (report protocol, 6-key array)");
    EXPECT_STR(REPORT(d, s, 0, 0, 0x04, 0, 0, 0, 0, 0), "+04");
    EXPECT_STR(REPORT(d, s, 0, 0, 0x04, 0, 0, 0, 0, 0), "");             /* idle repeat: no change */
    EXPECT_STR(REPORT(d, s, 0x02, 0, 0x04, 0x05, 0, 0, 0, 0), "+E1 +05"); /* Shift first */
    EXPECT_STR(REPORT(d, s, 0x02, 0, 0x05, 0, 0, 0, 0, 0), "-04");
    EXPECT_STR(REPORT(d, s, 0, 0, 0, 0, 0, 0, 0, 0), "-E1 -05");
    EXPECT_STR(REPORT(d, s, 0, 0, 0x07, 0x06, 0x05, 0, 0, 0), "+07 +06 +05");   /* report order */
    EXPECT_STR(REPORT(d, s, 0x11, 0, 0x06, 0x07, 0x05, 0x08, 0x09, 0x0A), "+E0 +E4 +08 +09 +0A");
    /* phantom state: ErrorRollOver in every slot keeps the keys as they were */
    EXPECT_STR(REPORT(d, s, 0, 0, 1, 1, 1, 1, 1, 1), "");
    EXPECT_STR(REPORT(d, s, 0x11, 0, 0x06, 0x07, 0x05, 0x08, 0x09, 0, 0), "-0A");
    EXPECT_STR(REPORT(d, s, 0, 0, 0, 0, 0, 0, 0, 0), "-E0 -E4 -05 -06 -07 -08 -09");
    /* reserved usages and usages above 0xE7 are no keys; Mute 0x7F is */
    EXPECT_STR(REPORT(d, s, 0, 0, 0x02, 0x03, 0xE8, 0xFF, 0x7F, 0), "+7F");
    EXPECT_STR(REPORT(d, s, 0, 0, 0, 0, 0, 0, 0, 0), "-7F");
    /* short reports: the modifiers only, or part of the array */
    EXPECT_STR(REPORT(d, s, 0x01), "+E0");
    EXPECT_STR(REPORT(d, s, 0x01, 0, 0x2C), "+2C");
    EXPECT_STR(REPORT(d, s, 0x00, 0), "-E0");
    EXPECT_STR(REPORT(d, s, 0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0xBB), "-2C");     /* longer than declared */
    EXPECT_STR(feed(d, s, NULL, 0), "");
    /* LEDs: Num, Caps, Scroll in bits 0..2 of a 1-byte output report */
    EXPECT_STR(led(d, LED_CAPS), "02");
    EXPECT_STR(led(d, LED_NUM | LED_CAPS | LED_SCROLL), "07");
    EXPECT_STR(led(d, 0), "00");
    REPORT(d, s, 0x20, 0, 0x04, 0x1E, 0, 0, 0, 0);
    EXPECT_STR(release_all(d, s), "-E5 -04 -1E");
    free(d); free(s);
}

static void test_boot(void) {
    printf("== boot protocol keyboard (fallback)\n");
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    hid_boot_keyboard(d);
    EXPECT(d->apps, HID_APP_KEYBOARD);
    EXPECT(d->boot, 1);
    EXPECT_STR(describe(d, "boot protocol"), "keyboard (boot protocol, 6-key array)");
    EXPECT_STR(REPORT(d, s, 0x04, 0, 0x87, 0x2A, 0, 0, 0, 0), "+E2 +87 +2A");   /* international key 0x87 */
    EXPECT_STR(REPORT(d, s, 0, 0, 0, 0, 0, 0, 0, 0), "-E2 -2A -87");
    EXPECT_STR(led(d, LED_NUM), "01");
    free(d); free(s);
}

static void test_qemu_pointers(void) {
    printf("== QEMU usb-tablet / usb-mouse\n");
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    EXPECT(hid_parse(d, qemu_tablet, sizeof qemu_tablet), HID_APP_MOUSE);
    EXPECT(d->ptr.absolute, 1);
    EXPECT_STR(describe(d, "report protocol"), "tablet (report protocol)");
    EXPECT_STR(REPORT(d, s, 0x01, 0xFF, 0x7F, 0x00, 0x00, 0x00), "A(32767,0,0,1)");
    EXPECT_STR(REPORT(d, s, 0x06, 0x00, 0x40, 0xFF, 0x3F, 0xFF), "A(16384,16383,-1,6)");
    EXPECT_STR(REPORT(d, s, 0x00, 0x00, 0x40), "");                    /* Y missing */
    EXPECT(hid_led_report(d, 7, (u8[8]){ 0 }, 8), 0);
    memset(s, 0, sizeof *s);
    EXPECT(hid_parse(d, qemu_mouse, sizeof qemu_mouse), HID_APP_MOUSE);
    EXPECT(d->ptr.absolute, 0);
    EXPECT_STR(describe(d, "report protocol"), "mouse (report protocol)");
    EXPECT_STR(REPORT(d, s, 0x11, 0xFE, 0x03, 0x01), "R(-2,3,1,1)");   /* button 5 is not reported */
    free(d); free(s);
}

static void nkro_report(u8 *r, u8 mods, const u8 *keys, int n) {
    memset(r, 0, 17);
    r[0] = 1;
    r[1] = mods;
    for (int i = 0; i < n; i++) r[2 + keys[i] / 8] |= (u8)(1 << (keys[i] % 8));
}

static void test_nkro(void) {
    printf("== NKRO gaming keyboard\n");
    hid_desc_t *d0 = new_desc(), *d = new_desc();
    hid_state_t *s0 = new_state(), *s = new_state();
    EXPECT(hid_parse(d0, nkro_if0, sizeof nkro_if0), HID_APP_KEYBOARD);
    EXPECT_STR(describe(d0, "report protocol"), "keyboard (report protocol, 6-key array)");
    EXPECT_STR(REPORT(d0, s0, 0, 0, 0x65, 0x66, 0, 0, 0, 0), "+65");    /* 0x66 is above Logical Maximum */

    EXPECT(hid_parse(d, nkro_if1, sizeof nkro_if1), HID_APP_KEYBOARD | HID_APP_CONSUMER | HID_APP_SYSTEM);
    EXPECT(d->has_ids, 1);
    EXPECT(d->overflow, 0);
    EXPECT(d->max_in, 64);       /* vendor report 4: 63 bytes + ID */
    EXPECT_STR(describe(d, "report protocol"),
               "keyboard (report protocol, NKRO bitmap, report ID 1) + consumer control (report ID 2) + system control (report ID 3)");
    u8 r[17];
    /* ten keys at once, then more: no rollover limit */
    static const u8 ten[] = { 0x04, 0x16, 0x07, 0x09, 0x2C, 0x0D, 0x0E, 0x0F, 0x33, 0x34 };
    nkro_report(r, 0, ten, 10);
    EXPECT_STR(feed(d, s, r, 17), "+04 +07 +09 +0D +0E +0F +16 +2C +33 +34");
    static const u8 more[] = { 0x04, 0x16, 0x07, 0x09, 0x2C, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x1E, 0x1F, 0x20, 0x50, 0x52, 0x77 };
    nkro_report(r, 0x02, more, 16);
    EXPECT_STR(feed(d, s, r, 17), "+E1 +1E +1F +20 +50 +52 +77");
    static const u8 fewer[] = { 0x16, 0x52 };
    nkro_report(r, 0x02, fewer, 2);
    EXPECT_STR(feed(d, s, r, 17), "-04 -07 -09 -0D -0E -0F -1E -1F -20 -2C -33 -34 -50 -77");
    /* a short report cannot say anything about the bitmap */
    EXPECT_STR(REPORT(d, s, 1, 0x00, 0x00, 0x00), "-E1");
    nkro_report(r, 0, NULL, 0);
    EXPECT_STR(feed(d, s, r, 17), "-16 -52");
    /* consumer control (report 2), system control (report 3) */
    EXPECT_STR(REPORT(d, s, 2, 0x92, 0x01), "+EE");                    /* AL Calculator */
    nkro_report(r, 0, ten, 1);
    EXPECT_STR(feed(d, s, r, 17), "+04");                              /* report 1 leaves report 2 alone */
    EXPECT_STR(REPORT(d, s, 2, 0xE9, 0x00), "-EE +80");                /* Volume Up */
    EXPECT_STR(REPORT(d, s, 2, 0x00, 0x00), "-80");
    EXPECT_STR(REPORT(d, s, 2, 0x3D, 0x02), "");                       /* above Logical Maximum */
    EXPECT_STR(REPORT(d, s, 3, 0x01), "+EF");                          /* System Power Down */
    EXPECT_STR(REPORT(d, s, 3, 0x06), "-EF +F0 +F1");                  /* Sleep + Wake */
    EXPECT_STR(REPORT(d, s, 3, 0x00), "-F0 -F1");
    /* vendor and unknown reports are ignored */
    u8 v[64] = { 4, 0xFF, 0xFF, 0xFF };
    EXPECT_STR(feed(d, s, v, 64), "");
    EXPECT_STR(REPORT(d, s, 9, 0xFF, 0xFF), "");
    EXPECT_STR(REPORT(d, s, 1), "");                                   /* report ID only */
    EXPECT_STR(led(d, LED_CAPS | LED_SCROLL), "01 06");
    EXPECT_STR(release_all(d, s), "-04");
    free(d0); free(d); free(s0); free(s);
}

static void test_receiver(void) {
    printf("== wireless receiver (keyboard + mouse + consumer + system on one interface)\n");
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    EXPECT(hid_parse(d, receiver, sizeof receiver), HID_APP_KEYBOARD | HID_APP_MOUSE | HID_APP_CONSUMER | HID_APP_SYSTEM);
    EXPECT_STR(describe(d, "report protocol"),
               "keyboard (report protocol, 6-key array, report ID 1) + mouse (report ID 2) + "
               "consumer control (report ID 3) + system control (report ID 4)");
    EXPECT(d->ptr.rid, 2);
    EXPECT(d->ptr.absolute, 0);
    EXPECT_STR(REPORT(d, s, 1, 0x40, 0, 0x2B, 0, 0, 0, 0, 0), "+E6 +2B");         /* AltGr + Tab */
    /* X = -5, Y = +7 (12 bits each), wheel +1, AC Pan ignored */
    EXPECT_STR(REPORT(d, s, 2, 0x01, 0xFB, 0x7F, 0x00, 0x01, 0x05), "R(-5,7,1,1)");
    EXPECT_STR(REPORT(d, s, 2, 0x12, 0x00, 0x00, 0x80, 0xFF, 0x00), "R(0,-2048,-1,2)");
    EXPECT_STR(REPORT(d, s, 3, 0xCD, 0x00), "+E8");                     /* Play/Pause */
    EXPECT_STR(REPORT(d, s, 3, 0xB5, 0x00), "-E8 +E9");                 /* Next Track */
    EXPECT_STR(REPORT(d, s, 3, 0x8A, 0x01), "-E9 +ED");                 /* Mail */
    EXPECT_STR(REPORT(d, s, 3, 0x23, 0x02), "-ED +EC");                 /* Browser Home */
    EXPECT_STR(REPORT(d, s, 3, 0xEA, 0x00), "-EC +81");                 /* Volume Down */
    EXPECT_STR(REPORT(d, s, 3, 0x00, 0x00), "-81");                     /* 0 is out of range: nothing */
    EXPECT_STR(REPORT(d, s, 4, 0x02), "+EF");                           /* list order: Sleep, Power, Wake */
    EXPECT_STR(REPORT(d, s, 4, 0x01), "-EF +F0");
    EXPECT_STR(REPORT(d, s, 4, 0x00), "-F0");
    EXPECT_STR(REPORT(d, s, 0x10, 1, 2, 3, 4, 5, 6), "");
    EXPECT_STR(REPORT(d, s, 1, 0x00, 0, 0, 0, 0, 0, 0, 0), "-E6 -2B");
    EXPECT_STR(led(d, LED_NUM | LED_CAPS), "01 03");
    free(d); free(s);
}

static void test_consumer(void) {
    printf("== consumer control interfaces\n");
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    EXPECT(hid_parse(d, consumer_only, sizeof consumer_only), HID_APP_CONSUMER);
    EXPECT_STR(describe(d, "report protocol"), "consumer control (report protocol)");
    EXPECT(d->led_valid, 0);
    EXPECT_STR(REPORT(d, s, 0x01, 0x00), "+80");
    EXPECT_STR(REPORT(d, s, 0x06, 0x00), "-80 +81 +7F");
    EXPECT_STR(REPORT(d, s, 0x78, 0x00), "-7F -81 +E8 +E9 +EA +EB");
    EXPECT_STR(REPORT(d, s, 0x80, 0xFF), "-E8 -E9 -EA -EB +EE +EC +ED +F5 +F2 +F3 +F4 +F6 +F7");
    EXPECT_STR(REPORT(d, s, 0x00), "");                                 /* short: unchanged */
    EXPECT_STR(REPORT(d, s, 0x00, 0x00), "-EC -ED -EE -F2 -F3 -F4 -F5 -F6 -F7");

    EXPECT(hid_parse(d, volume_knob, sizeof volume_knob), HID_APP_CONSUMER);
    memset(s, 0, sizeof *s);
    EXPECT_STR(REPORT(d, s, 0x01), "+80");
    EXPECT_STR(REPORT(d, s, 0x00), "-80");
    EXPECT_STR(REPORT(d, s, 0x03), "+81");                              /* -1 */
    EXPECT_STR(REPORT(d, s, 0x04), "-81 +7F");                          /* Mute (32-bit usage) */
    EXPECT_STR(REPORT(d, s, 0x02), "-7F");                              /* -2: out of range */

    EXPECT(hid_parse(d, kbd_power, sizeof kbd_power), HID_APP_KEYBOARD);
    memset(s, 0, sizeof *s);
    EXPECT_STR(REPORT(d, s, 0x00, 0x04, 0x01), "+04 +EF");
    EXPECT_STR(REPORT(d, s, 0x00, 0x00, 0x00), "-04 -EF");
    free(d); free(s);
}

static void test_mapping(void) {
    printf("== usage mapping\n");
    EXPECT(hid_usage_to_key(0x070004), KEY_A);
    EXPECT(hid_usage_to_key(0x0700E7), KEY_RGUI);
    EXPECT(hid_usage_to_key(0x0700E8), 0);
    EXPECT(hid_usage_to_key(0x070001), 0);
    EXPECT(hid_usage_to_key(0x0C00E2), 0x7F);
    EXPECT(hid_usage_to_key(0x0C00E9), 0x80);
    EXPECT(hid_usage_to_key(0x0C00EA), 0x81);
    EXPECT(hid_usage_to_key(0x0C00CD), KEY_MEDIA_PLAYPAUSE);
    EXPECT(hid_usage_to_key(0x0C00B5), KEY_MEDIA_NEXT);
    EXPECT(hid_usage_to_key(0x0C00B6), KEY_MEDIA_PREV);
    EXPECT(hid_usage_to_key(0x0C00B7), KEY_MEDIA_STOP);
    EXPECT(hid_usage_to_key(0x0C0223), KEY_BROWSER_HOME);
    EXPECT(hid_usage_to_key(0x0C018A), KEY_MAIL);
    EXPECT(hid_usage_to_key(0x0C0192), KEY_CALCULATOR);
    EXPECT(hid_usage_to_key(0x010081), KEY_POWER);
    EXPECT(hid_usage_to_key(0x010082), KEY_SLEEP);
    EXPECT(hid_usage_to_key(0x010030), 0);
    EXPECT(hid_usage_to_key(0x090001), 0);
    for (int k = 0xE8; k <= 0xF7; k++) CHECK(hid_key_name((u8)k) != NULL, "no name for %02X", k);
    CHECK(hid_key_name(0x04) == NULL, "KEY_A has a media key name");
    EXPECT_STR(hid_key_name(KEY_CALCULATOR), "Calculator");
}

/* ---------- hostile descriptors ---------- */
static u32 rng = 12345;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* parse, check invariants, feed random reports; returns apps */
static int torture(const u8 *rd, int len, int reports) {
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    u8 *copy = malloc(len ? len : 1);
    if (len) memcpy(copy, rd, len);
    int apps = hid_parse(d, copy, len);
    free(copy);
    CHECK(d->nfields <= HID_MAX_FIELDS && d->nusage <= HID_MAX_USAGES, "limits %u %u", d->nfields, d->nusage);
    for (int i = 0; i < d->nfields; i++) {
        const hid_field_t *f = &d->f[i];
        CHECK(f->size >= 1 && f->size <= 32 && f->count >= 1 && f->bit + (u32)f->size * f->count <= HID_MAX_REPORT * 8,
              "field %d: bit %u size %u count %u", i, f->bit, f->size, f->count);
        CHECK(f->list + f->nlist <= d->nusage, "usage list out of range");
    }
    for (int n = 0; n < reports; n++) {
        int l = (int)(rnd() % 72);
        u8 r[72];
        for (int i = 0; i < l; i++) r[i] = (u8)rnd();
        if (l && d->has_ids && (rnd() & 1) && d->nfields) r[0] = d->f[rnd() % d->nfields].rid;
        feed(d, s, r, l);
    }
    u8 out[600];
    hid_led_report(d, 7, out, sizeof out);
    char small[12], big[256];
    hid_describe(d, "report protocol", small, sizeof small);
    hid_describe(d, NULL, big, sizeof big);
    CHECK(strlen(small) < sizeof small, "describe overflow");
    release_all(d, s);
    free(d); free(s);
    return apps;
}

static void test_hostile(void) {
    printf("== broken and hostile descriptors\n");
    int before = g_fail;
    static const struct { const u8 *p; int n; } all[] = {
        { qemu_kbd, sizeof qemu_kbd }, { qemu_tablet, sizeof qemu_tablet }, { qemu_mouse, sizeof qemu_mouse },
        { nkro_if0, sizeof nkro_if0 }, { nkro_if1, sizeof nkro_if1 }, { receiver, sizeof receiver },
        { consumer_only, sizeof consumer_only }, { volume_knob, sizeof volume_knob },
        { kbd_power, sizeof kbd_power },
    };
    /* every truncation */
    for (size_t a = 0; a < sizeof all / sizeof all[0]; a++)
        for (int n = 0; n <= all[a].n; n++) torture(all[a].p, n, 8);
    /* a truncated receiver descriptor still yields what it fully describes */
    EXPECT(torture(receiver, 72, 0), HID_APP_KEYBOARD);
    hid_desc_t *e = new_desc();
    EXPECT(hid_parse(e, NULL, 0), 0);
    free(e);

    /* huge Report Count / Size: no endless loop, no field */
    static const u8 huge[] = { 0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
                               0x75, 0x08, 0x97, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x00,
                               0x75, 0x00, 0x95, 0x08, 0x81, 0x00,
                               0x77, 0xFF, 0xFF, 0x00, 0x00, 0x95, 0x01, 0x81, 0x00,
                               0x75, 0x21, 0x95, 0x01, 0x81, 0x00, 0xC0 };
    EXPECT(torture(huge, sizeof huge, 50), 0);
    /* unbalanced collections, deep nesting, push/pop abuse, long items running off the end */
    u8 b[600];
    int n = 0;
    for (int i = 0; i < 20; i++) b[n++] = 0xC0;
    b[n++] = 0x05; b[n++] = 0x01;                               /* Generic Desktop */
    for (int i = 0; i < 40; i++) { b[n++] = 0x09; b[n++] = 0x06; b[n++] = 0xA1; b[n++] = 0x01; }
    for (int i = 0; i < 30; i++) b[n++] = 0xA4;                 /* Push */
    for (int i = 0; i < 40; i++) b[n++] = 0xB4;                 /* Pop */
    memcpy(b + n, qemu_kbd + 6, sizeof qemu_kbd - 6); n += sizeof qemu_kbd - 6;
    b[n++] = 0xFE; b[n++] = 0xFF; b[n++] = 0x00;                /* long item longer than the rest */
    EXPECT(torture(b, n, 50), HID_APP_KEYBOARD);
    /* reversed usage range, Logical Maximum below Minimum */
    static const u8 reversed[] = { 0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0x65, 0x29, 0x04,
                                   0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0x15, 0x10, 0x25, 0x01, 0x19, 0x00, 0x29, 0x65,
                                   0x81, 0x00, 0xC0 };
    EXPECT(torture(reversed, sizeof reversed, 50), 0);

    /* 255 report IDs, each a keyboard: fields run out, the first ones work */
    static u8 many[255 * 40];
    n = 0;
    for (int id = 1; id <= 255; id++) {
        static const u8 tpl[] = { 0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x00, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
                                  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08,
                                  0x15, 0x00, 0x25, 0x65, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0 };
        memcpy(many + n, tpl, sizeof tpl);
        many[n + 7] = (u8)id;
        n += sizeof tpl;
    }
    hid_desc_t *d = new_desc();
    hid_state_t *s = new_state();
    EXPECT(hid_parse(d, many, n), HID_APP_KEYBOARD);
    EXPECT(d->nfields, HID_MAX_FIELDS);
    EXPECT(d->overflow, 1);
    EXPECT_STR(REPORT(d, s, 1, 0x02, 0x05), "+E1 +05");
    EXPECT_STR(REPORT(d, s, 24, 0x00, 0x06), "+06");
    EXPECT_STR(REPORT(d, s, 25, 0x00, 0x07), "");
    EXPECT_STR(REPORT(d, s, 255, 0x00, 0x07), "");
    free(d); free(s);
    torture(many, n, 200);

    /* random mutations of the real descriptors, and random bytes */
    for (int it = 0; it < 20000; it++) {
        u8 m[512];
        int len;
        if (it % 4 == 0) {
            len = (int)(rnd() % 200);
            for (int i = 0; i < len; i++) m[i] = (u8)rnd();
        } else {
            size_t a = rnd() % (sizeof all / sizeof all[0]);
            len = all[a].n;
            memcpy(m, all[a].p, len);
            int flips = 1 + (int)(rnd() % 6);
            for (int i = 0; i < flips; i++) {
                int pos = (int)(rnd() % len);
                switch (rnd() % 4) {
                case 0: m[pos] = (u8)rnd(); break;
                case 1: m[pos] ^= (u8)(1 << (rnd() % 8)); break;
                case 2: m[pos] = (u8[]){ 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0xC0, 0xA1 }[rnd() % 8]; break;
                case 3: if (len < 500) { memmove(m + pos + 1, m + pos, len - pos); m[pos] = (u8)rnd(); len++; } break;
                }
            }
        }
        torture(m, len, 6);
    }
    printf("   %s\n", g_fail == before ? "no crash, all invariants held" : "FAILED");
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_mapping();
    test_qemu_kbd();
    test_boot();
    test_qemu_pointers();
    test_nkro();
    test_receiver();
    test_consumer();
    test_hostile();
    printf("HID: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
