/* Halcyon OS - HID report descriptor parser and input report decoder.
 *
 * Kernel-independent (no USB, no allocation): hid.c drives it for USB HID
 * interfaces and tests/host/hidtest.c runs it on the host.
 *
 * hid_parse() walks a report descriptor and records, per top-level
 * application collection:
 *   keyboard / keypad (Generic Desktop 0x06 / 0x07): modifier bitmaps, key
 *       arrays (usage range or usage list, logical minimum/maximum), key
 *       bitmaps (n-key rollover), and the Num/Caps/Scroll Lock LED outputs
 *   consumer control (Consumer 0x01) and system control (Generic Desktop
 *       0x80): the same field kinds on the consumer / generic desktop pages
 *   mouse / pointer / digitizer: buttons, X, Y (relative or absolute), wheel
 * with report IDs (one interface may carry several applications, told apart
 * by the first byte of each report), padding and constant fields, push/pop,
 * extended (32-bit) usages and long items. Malformed descriptors never make
 * it read out of bounds or loop: limits are clamped and excess fields are
 * dropped (d->overflow).
 *
 * hid_input() decodes one input report and reports key changes per field
 * (press/release tracking for arrays and bitmaps, modifiers first) and
 * pointer motion through a hid_sink_t. Keys are Halcyon key codes
 * (drivers/input.h): keyboard-page usages as they are, consumer and system
 * control usages translated by hid_usage_to_key().
 */
#ifndef HIDPARSE_H
#define HIDPARSE_H
#include "core/types.h"

/* application collections (hid_desc_t.apps) */
#define HID_APP_KEYBOARD 0x01
#define HID_APP_MOUSE    0x02
#define HID_APP_CONSUMER 0x04
#define HID_APP_SYSTEM   0x08

#define HID_MAX_FIELDS   48      /* key fields per interface */
#define HID_MAX_USAGES   256     /* explicit usage lists, all fields together */
#define HID_MAX_REPORT   512     /* bytes per report the parser places fields in */

/* key field kinds */
enum { HID_F_ARRAY = 1,          /* count slots, each holding a usage index */
       HID_F_BITMAP,             /* count 1-bit variables: bit set = key down */
       HID_F_VALUE };            /* count multi-bit variables: non-zero = down */

typedef struct hid_field {
    u8  kind;        /* HID_F_* */
    u8  app;         /* HID_APP_* of the enclosing application */
    u8  rid;         /* report ID (0 = none) */
    u8  size;        /* bits per element, 1..32 */
    u16 count;       /* elements */
    u16 bit;         /* offset in the report, after the report ID byte */
    i32 lmin, lmax;  /* logical range */
    u32 umin, umax;  /* usage range (page << 16 | id) when nlist == 0 */
    u16 list, nlist; /* else: explicit usages d->usage[list .. list + nlist) */
} hid_field_t;

typedef struct hid_value { u16 bit; u8 size; u8 valid; i32 lmin, lmax; } hid_value_t;

typedef struct hid_pointer {
    u8 valid;        /* X and Y found */
    u8 rid;
    u8 absolute;     /* tablet / touch screen */
    u8 started;      /* rid is fixed */
    hid_value_t x, y, wheel, btn[5];
} hid_pointer_t;

typedef struct hid_desc {
    u8  apps;        /* HID_APP_* with usable fields */
    u8  has_ids;     /* reports start with a report ID byte */
    u8  boot;        /* layout of the boot protocol (hid_boot_keyboard) */
    u8  overflow;    /* some fields did not fit and were ignored */
    u8  nfields;
    u8  app_rid[4];  /* first report ID of each application (bit order of HID_APP_*) */
    hid_field_t f[HID_MAX_FIELDS];
    u16 nusage;
    u32 usage[HID_MAX_USAGES];
    hid_pointer_t ptr;
    /* keyboard LED output report */
    u8  led_valid, led_rid;
    u16 led_len;     /* bytes, without the report ID */
    u16 led_bit[3];  /* Num, Caps, Scroll Lock; 0xFFFF = not present */
    u16 max_in;      /* longest input report in bytes, with the report ID byte */
} hid_desc_t;

/* key state of one interface (the previous report, per field) */
typedef struct hid_state {
    u8 down[HID_MAX_FIELDS][32];      /* key codes held, bitmap */
    u8 next[HID_MAX_FIELDS][32];      /* scratch */
} hid_state_t;

typedef struct hid_sink {
    void *ctx;
    void (*key)(void *ctx, u8 key, int pressed);
    void (*mouse_rel)(void *ctx, int dx, int dy, int dz, u8 buttons);
    void (*mouse_abs)(void *ctx, int x, int y, int dz, u8 buttons);   /* 0..32767 */
} hid_sink_t;

/* Parse a report descriptor into d (cleared first). Returns d->apps:
 * the applications with usable fields, 0 if none (or not parseable). */
int  hid_parse(hid_desc_t *d, const u8 *rd, int len);
/* d = the fixed boot protocol keyboard layout (8-byte reports, 1-byte LEDs) */
void hid_boot_keyboard(hid_desc_t *d);
/* decode one input report (as received: with the report ID byte if d->has_ids) */
void hid_input(const hid_desc_t *d, hid_state_t *s, const u8 *r, int len, const hid_sink_t *k);
/* release every key s holds (device gone) */
void hid_release_all(const hid_desc_t *d, hid_state_t *s, const hid_sink_t *k);
/* build the LED output report for LED_* bits `leds` (with the report ID
 * byte if d->has_ids) into buf; returns its length, 0 if no LEDs */
int  hid_led_report(const hid_desc_t *d, u8 leds, u8 *buf, int max);
/* "keyboard (report protocol, NKRO bitmap, report ID 1) + consumer control (report ID 3)" */
void hid_describe(const hid_desc_t *d, const char *protocol, char *buf, int size);
/* usage (page << 16 | id) -> Halcyon key code, 0 if none */
u8   hid_usage_to_key(u32 usage);
/* name of a consumer / system / volume key code, NULL for other keys */
const char *hid_key_name(u8 key);

#endif
