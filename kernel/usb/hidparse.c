/* Halcyon OS - HID report descriptor parser and input report decoder
 * (see hidparse.h). No kernel services are used besides memset/memcpy/
 * snprintf, so tests/host/hidtest.c compiles this file unchanged. */
#include "hidparse.h"
#include "core/lib.h"
#include "drivers/input.h"

#define PAGE_GD        0x01
#define PAGE_KBD       0x07
#define PAGE_LED       0x08
#define PAGE_BUTTON    0x09
#define PAGE_CONSUMER  0x0C
#define PAGE_DIGITIZER 0x0D

#define U(page, id) (((u32)(page) << 16) | (id))

/* keyboard-page keys the consumer volume usages map to */
#define HKEY_MUTE    0x7F
#define HKEY_VOLUP   0x80
#define HKEY_VOLDOWN 0x81

#define MAX_BITS     (HID_MAX_REPORT * 8)
#define MAX_LOCAL    32      /* Usage items kept per main item */
#define MAX_PUSH     8
#define MAX_DEPTH    16

/* ---------- usage -> key code ---------- */
u8 hid_usage_to_key(u32 usage) {
    u16 page = usage >> 16, id = usage & 0xFFFF;
    if (page == PAGE_KBD) return (id >= 0x04 && id <= 0xE7) ? (u8)id : 0;
    if (page == PAGE_CONSUMER) {
        switch (id) {
        case 0x30:  return KEY_POWER;              /* Power */
        case 0x32:  return KEY_SLEEP;              /* Sleep */
        case 0xB0: case 0xB1: case 0xCD:           /* Play, Pause, Play/Pause */
                    return KEY_MEDIA_PLAYPAUSE;
        case 0xB5:  return KEY_MEDIA_NEXT;         /* Scan Next Track */
        case 0xB6:  return KEY_MEDIA_PREV;         /* Scan Previous Track */
        case 0xB7: case 0xCC:                      /* Stop, Stop/Eject */
                    return KEY_MEDIA_STOP;
        case 0xE2:  return HKEY_MUTE;
        case 0xE9:  return HKEY_VOLUP;
        case 0xEA:  return HKEY_VOLDOWN;
        case 0x18A: return KEY_MAIL;               /* AL Email Reader */
        case 0x192: return KEY_CALCULATOR;         /* AL Calculator */
        case 0x194: return KEY_MY_COMPUTER;        /* AL Local Machine Browser */
        case 0x221: return KEY_BROWSER_SEARCH;     /* AC Search */
        case 0x223: return KEY_BROWSER_HOME;       /* AC Home */
        case 0x224: return KEY_BROWSER_BACK;       /* AC Back */
        case 0x225: return KEY_BROWSER_FORWARD;    /* AC Forward */
        case 0x227: return KEY_BROWSER_REFRESH;    /* AC Refresh */
        case 0x22A: return KEY_BROWSER_FAVORITES;  /* AC Bookmarks */
        }
        return 0;
    }
    if (page == PAGE_GD) {
        if (id == 0x81) return KEY_POWER;          /* System Power Down */
        if (id == 0x82) return KEY_SLEEP;          /* System Sleep */
        if (id == 0x83) return KEY_WAKE;           /* System Wake Up */
    }
    return 0;
}

const char *hid_key_name(u8 key) {
    switch (key) {
    case HKEY_MUTE: return "Mute";
    case HKEY_VOLUP: return "Volume Up";
    case HKEY_VOLDOWN: return "Volume Down";
    case KEY_MEDIA_PLAYPAUSE: return "Play/Pause";
    case KEY_MEDIA_NEXT: return "Next Track";
    case KEY_MEDIA_PREV: return "Previous Track";
    case KEY_MEDIA_STOP: return "Stop";
    case KEY_BROWSER_HOME: return "Browser Home";
    case KEY_MAIL: return "Mail";
    case KEY_CALCULATOR: return "Calculator";
    case KEY_POWER: return "Power";
    case KEY_SLEEP: return "Sleep";
    case KEY_WAKE: return "Wake";
    case KEY_BROWSER_BACK: return "Browser Back";
    case KEY_BROWSER_FORWARD: return "Browser Forward";
    case KEY_BROWSER_REFRESH: return "Browser Refresh";
    case KEY_BROWSER_SEARCH: return "Browser Search";
    case KEY_BROWSER_FAVORITES: return "Browser Favorites";
    case KEY_MY_COMPUTER: return "My Computer";
    }
    return NULL;
}

/* ---------- descriptor parser ---------- */
typedef struct globals {
    u32 page;
    u32 lmin_raw, lmax_raw;
    u8  lmin_sz, lmax_sz;
    u32 rsize, rcount, rid;
} globals_t;

static i32 sext(u32 v, int bytes) {
    if (bytes == 1) return (i8)v;
    if (bytes == 2) return (i16)v;
    return (i32)v;
}

/* Logical Minimum is signed. Logical Maximum is read as unsigned when the
 * minimum is not negative (0..255 written as "25 FF" is common, though
 * strictly that means -1). */
static void logical_range(const globals_t *g, i32 *lmin, i32 *lmax) {
    *lmin = g->lmin_sz ? sext(g->lmin_raw, g->lmin_sz) : 0;
    if (!g->lmax_sz) *lmax = 0;
    else if (*lmin >= 0 && g->lmax_sz < 4) *lmax = (i32)g->lmax_raw;
    else *lmax = sext(g->lmax_raw, g->lmax_sz);
}

static u8 classify_app(u32 usage) {
    u16 page = usage >> 16, id = usage & 0xFFFF;
    if (page == PAGE_GD) {
        if (id == 0x06 || id == 0x07) return HID_APP_KEYBOARD;   /* keyboard, keypad */
        if (id == 0x01 || id == 0x02) return HID_APP_MOUSE;      /* pointer, mouse */
        if (id == 0x80) return HID_APP_SYSTEM;                   /* system control */
    } else if (page == PAGE_CONSUMER) {
        if (id == 0x01) return HID_APP_CONSUMER;                 /* consumer control */
    } else if (page == PAGE_DIGITIZER) {
        if (id >= 0x01 && id <= 0x0F) return HID_APP_MOUSE;      /* pen, touch screen, ... */
    }
    return 0;
}

static int app_index(u8 app) {
    switch (app) {
    case HID_APP_KEYBOARD: return 0;
    case HID_APP_MOUSE: return 1;
    case HID_APP_CONSUMER: return 2;
    default: return 3;
    }
}

typedef struct locals {
    u32 usage[MAX_LOCAL];
    int nus;
    u32 umin, umax;
    u8  have_min, have_max;
} locals_t;

/* usage of element n of a main item, 0 if it has none */
static u32 element_usage(const locals_t *l, u32 n) {
    if (l->have_min || l->have_max) {
        u32 umin = l->have_min ? l->umin : 0;
        u32 u = umin + n;
        if (l->have_max && u > l->umax) return l->umax;
        return u;
    }
    if (!l->nus) return 0;
    return l->usage[n < (u32)l->nus ? n : (u32)l->nus - 1];
}

static void pointer_field(hid_desc_t *d, const globals_t *g, const locals_t *l, u32 pos, int is_rel) {
    hid_pointer_t *p = &d->ptr;
    if (p->started && p->rid != (u8)g->rid) return;
    if (g->rsize == 0 || g->rsize > 32) return;
    i32 lmin, lmax;
    logical_range(g, &lmin, &lmax);
    u32 n = MIN(g->rcount, 32u);
    for (u32 i = 0; i < n; i++) {
        u32 u = element_usage(l, i);
        hid_value_t v = { (u16)(pos + i * g->rsize), (u8)g->rsize, 1, lmin, lmax };
        u16 page = u >> 16, id = u & 0xFFFF;
        hid_value_t *dst = NULL;
        if (page == PAGE_GD && id == 0x30) { dst = &p->x; if (!dst->valid) p->absolute = !is_rel; }
        else if (page == PAGE_GD && id == 0x31) dst = &p->y;
        else if (page == PAGE_GD && id == 0x38) dst = &p->wheel;
        else if (page == PAGE_BUTTON && id >= 1 && id <= 5) dst = &p->btn[id - 1];
        if (dst && !dst->valid) {
            *dst = v;
            p->started = 1;
            p->rid = (u8)g->rid;
        }
    }
}

/* keyboard and consumer pages; Generic Desktop in a system control
 * application, or its system control usages (0x81..0xB7) anywhere */
static int key_page(u32 usage, u8 app) {
    u16 page = usage >> 16, id = usage & 0xFFFF;
    return page == PAGE_KBD || page == PAGE_CONSUMER ||
           (page == PAGE_GD && (app == HID_APP_SYSTEM || (id >= 0x81 && id <= 0xB7)));
}

static void key_field(hid_desc_t *d, const globals_t *g, const locals_t *l, u32 pos, u8 app, int is_var) {
    if (g->rsize == 0 || g->rsize > 32 || g->rcount == 0) return;
    int range = l->have_min || l->have_max;
    if (!range && !l->nus) return;
    u32 first = range ? (l->have_min ? l->umin : 0) : l->usage[0];
    u32 last = range ? (l->have_max ? l->umax : first) : first;
    if (!key_page(first, app) && !key_page(last, app)) return;
    if (d->nfields >= HID_MAX_FIELDS) { d->overflow = 1; return; }
    hid_field_t *f = &d->f[d->nfields];
    memset(f, 0, sizeof *f);
    f->kind = !is_var ? HID_F_ARRAY : g->rsize == 1 ? HID_F_BITMAP : HID_F_VALUE;
    f->app = app;
    f->rid = (u8)g->rid;
    f->size = (u8)g->rsize;
    f->count = (u16)g->rcount;        /* fits: the field lies within MAX_BITS */
    f->bit = (u16)pos;
    logical_range(g, &f->lmin, &f->lmax);
    if (f->kind == HID_F_ARRAY && f->lmax < f->lmin) return;
    if (range) {
        f->umin = l->have_min ? l->umin : 0;
        if (l->have_max) f->umax = l->umax;
        else if (f->kind == HID_F_ARRAY) f->umax = f->umin + ((u32)f->lmax - (u32)f->lmin);
        else f->umax = f->umin + f->count - 1;
        if (f->umax < f->umin) return;  /* reversed range: unusable */
    } else {
        if (d->nusage + l->nus > HID_MAX_USAGES) { d->overflow = 1; return; }
        f->list = d->nusage;
        f->nlist = (u16)l->nus;
        memcpy(&d->usage[d->nusage], l->usage, l->nus * sizeof(u32));
        d->nusage += (u16)l->nus;
    }
    d->nfields++;
    int ai = app_index(app);
    if (!(d->apps & app)) d->app_rid[ai] = f->rid;
    d->apps |= app;
}

static void led_field(hid_desc_t *d, const globals_t *g, const locals_t *l, u32 pos) {
    if (g->rsize == 0 || g->rsize > 32) return;
    if (d->led_valid && d->led_rid != (u8)g->rid) return;
    u32 n = MIN(g->rcount, 8u);
    for (u32 i = 0; i < n; i++) {
        u32 u = element_usage(l, i);
        if ((u >> 16) != PAGE_LED) continue;
        u16 id = u & 0xFFFF;
        if (id < 1 || id > 3 || d->led_bit[id - 1] != 0xFFFF) continue;
        d->led_bit[id - 1] = (u16)(pos + i * g->rsize);
        d->led_valid = 1;
        d->led_rid = (u8)g->rid;
    }
}

int hid_parse(hid_desc_t *d, const u8 *rd, int len) {
    memset(d, 0, sizeof *d);
    d->led_bit[0] = d->led_bit[1] = d->led_bit[2] = 0xFFFF;
    globals_t g = { 0 }, stack[MAX_PUSH];
    int sp = 0;
    locals_t l = { 0 };
    u8 apps[MAX_DEPTH + 1] = { 0 };
    int depth = 0;
    u8 app = 0;
    u16 inbit[256] = { 0 }, outbit[256] = { 0 };   /* next free bit, per report ID */
    if (!rd || len <= 0) return 0;

    for (int i = 0; i < len;) {
        u8 pfx = rd[i];
        if (pfx == 0xFE) {                 /* long item: bDataSize, bLongItemTag, data */
            if (i + 2 >= len) break;
            i += 3 + rd[i + 1];
            continue;
        }
        int sz = pfx & 3;
        if (sz == 3) sz = 4;
        if (i + 1 + sz > len) break;       /* truncated */
        u32 v = 0;
        for (int k = 0; k < sz; k++) v |= (u32)rd[i + 1 + k] << (8 * k);
        int type = (pfx >> 2) & 3, tag = pfx >> 4;
        i += 1 + sz;

        if (type == 1) {                   /* global */
            switch (tag) {
            case 0: g.page = v & 0xFFFF; break;
            case 1: g.lmin_raw = v; g.lmin_sz = (u8)sz; break;
            case 2: g.lmax_raw = v; g.lmax_sz = (u8)sz; break;
            case 7: g.rsize = v; break;
            case 8: g.rid = v & 0xFF; if (g.rid) d->has_ids = 1; break;
            case 9: g.rcount = v; break;
            case 10: if (sp < MAX_PUSH) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {            /* local */
            u32 u = sz == 4 ? v : U(g.page, v & 0xFFFF);
            switch (tag) {
            case 0: if (l.nus < MAX_LOCAL) l.usage[l.nus++] = u; break;
            case 1: l.umin = u; l.have_min = 1; break;
            case 2: l.umax = u; l.have_max = 1; break;
            }
        } else if (type == 0) {            /* main */
            if (tag == 8 || tag == 9) {    /* Input / Output */
                u16 *bitpos = tag == 8 ? inbit : outbit;
                u32 pos = bitpos[g.rid & 0xFF];
                u64 bits = (u64)g.rsize * g.rcount;
                int fits = pos + bits <= MAX_BITS;
                bitpos[g.rid & 0xFF] = (u16)MIN(pos + bits, (u64)MAX_BITS + 1);
                int is_const = v & 1, is_var = (v >> 1) & 1, is_rel = (v >> 2) & 1;
                if (fits && !is_const && bits) {
                    if (tag == 8 && app == HID_APP_MOUSE) pointer_field(d, &g, &l, pos, is_rel);
                    else if (tag == 8 && app) key_field(d, &g, &l, pos, app, is_var);
                    else if (tag == 9 && app == HID_APP_KEYBOARD) led_field(d, &g, &l, pos);
                }
            } else if (tag == 10) {        /* Collection */
                u8 a = app;
                if ((v & 0xFF) == 1) {     /* Application */
                    u32 u = l.nus ? l.usage[l.nus - 1] : l.have_min ? l.umin : 0;
                    a = classify_app(u);
                    /* a pointer application without X and Y does not keep its report ID */
                    if (!d->ptr.valid && !(d->ptr.x.valid && d->ptr.y.valid)) memset(&d->ptr, 0, sizeof d->ptr);
                }
                if (depth < MAX_DEPTH) apps[depth] = app;
                depth++;
                app = a;
            } else if (tag == 12) {        /* End Collection */
                if (depth > 0) {
                    depth--;
                    app = depth < MAX_DEPTH ? apps[depth] : 0;
                }
                if (d->ptr.x.valid && d->ptr.y.valid) d->ptr.valid = 1;
            }
            memset(&l, 0, sizeof l);       /* locals end with every main item */
        }
    }

    if (d->ptr.x.valid && d->ptr.y.valid) {
        d->ptr.valid = 1;
        if (!(d->apps & HID_APP_MOUSE)) d->app_rid[app_index(HID_APP_MOUSE)] = d->ptr.rid;
        d->apps |= HID_APP_MOUSE;
    } else {
        memset(&d->ptr, 0, sizeof d->ptr);
    }
    if (d->led_valid) d->led_len = (u16)((MIN(outbit[d->led_rid], MAX_BITS) + 7) / 8);
    u32 maxbits = 0;
    for (int r = 0; r < 256; r++) maxbits = MAX(maxbits, MIN((u32)inbit[r], (u32)MAX_BITS));
    d->max_in = (u16)((maxbits + 7) / 8 + d->has_ids);
    return d->apps;
}

/* HID 1.11 appendix B.1, with keys 0..255 like QEMU (boot devices send
 * usages above 0x65, e.g. international keys and Mute / Volume) */
static const u8 boot_keyboard_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             /* Generic Desktop, Keyboard, Application */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             /* 8 modifier bits */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /* reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,   /* 5 LEDs */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /* LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,   /* 6-key array */
    0xC0,
};

void hid_boot_keyboard(hid_desc_t *d) {
    hid_parse(d, boot_keyboard_desc, sizeof boot_keyboard_desc);
    d->boot = 1;
}

/* ---------- report decoder ---------- */
static u32 get_bits(const u8 *r, u32 bit, u32 size) {
    u32 v = 0;
    for (u32 b = 0; b < size; b++, bit++)
        if (r[bit >> 3] & (1u << (bit & 7))) v |= 1u << b;
    return v;
}

static i32 get_signed(const u8 *r, u32 bit, u32 size, int is_signed) {
    u32 v = get_bits(r, bit, size);
    if (is_signed && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (i32)v;
}

static int has(const u8 *set, u8 k) { return (set[k >> 3] >> (k & 7)) & 1; }
static void set_key(u8 *set, u8 k) { set[k >> 3] |= (u8)(1u << (k & 7)); }
static int is_modifier(u8 k) { return k >= 0xE0 && k <= 0xE7; }

typedef struct visit {
    void (*fn)(struct visit *v, u8 key);
    u8 *set;                 /* collect: keys of this report */
    const u8 *now, *before;  /* press: emit keys in now but not before */
    u8 done[32];
    int rollover;
    const hid_sink_t *sink;
} visit_t;

static void visit_collect(visit_t *v, u8 key) { set_key(v->set, key); }

static void visit_press(visit_t *v, u8 key) {
    if (is_modifier(key) || !has(v->now, key) || has(v->before, key) || has(v->done, key)) return;
    set_key(v->done, key);
    v->sink->key(v->sink->ctx, key, 1);
}

static u32 field_usage(const hid_desc_t *d, const hid_field_t *f, u32 idx) {
    if (f->nlist) return idx < f->nlist ? d->usage[f->list + idx] : 0;
    u32 u = f->umin + idx;
    return (u < f->umin || u > f->umax) ? 0 : u;
}

/* call v->fn for every key down in the field, in report order */
static void decode_field(const hid_desc_t *d, const hid_field_t *f, const u8 *r, u32 n, visit_t *v) {
    int is_signed = f->lmin < 0;
    for (u32 i = 0; i < n; i++) {
        u32 bit = f->bit + i * f->size;
        if (f->kind == HID_F_BITMAP) {
            if (!(r[bit >> 3] & (1u << (bit & 7)))) continue;
            u8 k = hid_usage_to_key(field_usage(d, f, i));
            if (k) v->fn(v, k);
            continue;
        }
        i32 x = get_signed(r, bit, f->size, is_signed);
        if (x < f->lmin || x > f->lmax) continue;          /* out of range: no key */
        if (f->kind == HID_F_ARRAY) {
            u32 u = field_usage(d, f, (u32)x - (u32)f->lmin);
            if (u == U(PAGE_KBD, 0x01)) { v->rollover = 1; continue; }   /* ErrorRollOver */
            u8 k = hid_usage_to_key(u);
            if (k) v->fn(v, k);
        } else {                                            /* HID_F_VALUE */
            u32 u = field_usage(d, f, i);
            u8 k = 0;
            if (u == U(PAGE_CONSUMER, 0xE0)) k = x > 0 ? HKEY_VOLUP : x < 0 ? HKEY_VOLDOWN : 0;   /* Volume (relative) */
            else if (x) k = hid_usage_to_key(u);
            if (k) v->fn(v, k);
        }
    }
}

/* elements of f present in a report of `bits` bits: arrays may be cut
 * short (missing slots are empty), other fields must be complete */
static u32 elements_present(const hid_field_t *f, u32 bits) {
    if (f->bit >= bits) return 0;
    if (f->kind == HID_F_ARRAY) return MIN((u32)f->count, (bits - f->bit) / f->size);
    return f->bit + (u32)f->size * f->count <= bits ? f->count : 0;
}

void hid_input(const hid_desc_t *d, hid_state_t *s, const u8 *r, int len, const hid_sink_t *k) {
    if (!r || len <= 0) return;
    u8 rid = 0;
    if (d->has_ids) { rid = r[0]; r++; len--; }
    if (len <= 0) return;
    u32 bits = (u32)MIN(len, HID_MAX_REPORT) * 8;

    /* keys */
    u32 present[HID_MAX_FIELDS];
    visit_t v;
    memset(&v, 0, sizeof v);
    v.sink = k;
    int any = 0;
    for (int i = 0; i < d->nfields; i++) {
        const hid_field_t *f = &d->f[i];
        present[i] = f->rid == rid ? elements_present(f, bits) : 0;
        if (!present[i]) continue;
        memset(s->next[i], 0, 32);
        v.fn = visit_collect;
        v.set = s->next[i];
        decode_field(d, f, r, present[i], &v);
        any = 1;
    }
    if (any && !v.rollover && k && k->key) {
        /* modifiers first, so that keys pressed in the same report see them */
        for (int i = 0; i < d->nfields; i++) {
            if (!present[i]) continue;
            for (int m = 0xE0; m <= 0xE7; m++)
                if (has(s->down[i], (u8)m) != has(s->next[i], (u8)m))
                    k->key(k->ctx, (u8)m, has(s->next[i], (u8)m));
        }
        /* releases */
        for (int i = 0; i < d->nfields; i++) {
            if (!present[i]) continue;
            for (int c = 0; c < 256; c++)
                if (!is_modifier((u8)c) && has(s->down[i], (u8)c) && !has(s->next[i], (u8)c))
                    k->key(k->ctx, (u8)c, 0);
        }
        /* presses, in the order the report lists them */
        for (int i = 0; i < d->nfields; i++) {
            if (!present[i]) continue;
            memset(v.done, 0, sizeof v.done);
            v.fn = visit_press;
            v.now = s->next[i];
            v.before = s->down[i];
            decode_field(d, &d->f[i], r, present[i], &v);
        }
    }
    if (any && !v.rollover)
        for (int i = 0; i < d->nfields; i++)
            if (present[i]) memcpy(s->down[i], s->next[i], 32);

    /* pointer */
    const hid_pointer_t *p = &d->ptr;
    if (!p->valid || p->rid != rid || !k) return;
    if (p->x.bit + p->x.size > bits || p->y.bit + p->y.size > bits) return;
    u8 btn = 0;
    for (int i = 0; i < 3; i++)          /* MOUSE_LEFT / RIGHT / MIDDLE */
        if (p->btn[i].valid && p->btn[i].bit + p->btn[i].size <= bits && get_bits(r, p->btn[i].bit, p->btn[i].size))
            btn |= (u8)(1 << i);
    int dz = 0;
    if (p->wheel.valid && p->wheel.bit + p->wheel.size <= bits) dz = get_signed(r, p->wheel.bit, p->wheel.size, 1);
    if (p->absolute) {
        if (!k->mouse_abs) return;
        i32 x = get_signed(r, p->x.bit, p->x.size, p->x.lmin < 0), y = get_signed(r, p->y.bit, p->y.size, p->y.lmin < 0);
        i64 xr = (i64)p->x.lmax - p->x.lmin, yr = (i64)p->y.lmax - p->y.lmin;
        if (xr <= 0) xr = 32767;
        if (yr <= 0) yr = 32767;
        i64 ax = ((i64)x - p->x.lmin) * 32767 / xr, ay = ((i64)y - p->y.lmin) * 32767 / yr;
        k->mouse_abs(k->ctx, (int)CLAMP(ax, 0, 32767), (int)CLAMP(ay, 0, 32767), dz, btn);
    } else {
        if (!k->mouse_rel) return;
        k->mouse_rel(k->ctx, get_signed(r, p->x.bit, p->x.size, 1), get_signed(r, p->y.bit, p->y.size, 1), dz, btn);
    }
}

void hid_release_all(const hid_desc_t *d, hid_state_t *s, const hid_sink_t *k) {
    for (int i = 0; i < d->nfields; i++) {
        for (int c = 0; c < 256; c++)
            if (has(s->down[i], (u8)c) && k && k->key) k->key(k->ctx, (u8)c, 0);
        memset(s->down[i], 0, 32);
    }
}

int hid_led_report(const hid_desc_t *d, u8 leds, u8 *buf, int max) {
    if (!d->led_valid || !d->led_len) return 0;
    int n = d->has_ids ? 1 : 0;
    int total = n + d->led_len;
    if (total > max) return 0;
    memset(buf, 0, total);
    if (d->has_ids) buf[0] = d->led_rid;
    for (int i = 0; i < 3; i++) {
        u16 b = d->led_bit[i];
        if ((leds & (1 << i)) && b != 0xFFFF) buf[n + b / 8] |= (u8)(1 << (b % 8));
    }
    return total;
}

/* ---------- description ---------- */
typedef struct sbuf { char *p; int size, len; } sbuf_t;

static void put(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sbuf_t *b, const char *fmt, ...) {
    if (b->len >= b->size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->size - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) b->len = MIN(b->len + n, b->size - 1);
}

void hid_describe(const hid_desc_t *d, const char *protocol, char *buf, int size) {
    if (size <= 0) return;
    sbuf_t b = { buf, size, 0 };
    buf[0] = 0;
    static const u8 order[4] = { HID_APP_KEYBOARD, HID_APP_MOUSE, HID_APP_CONSUMER, HID_APP_SYSTEM };
    int first = 1;
    for (int a = 0; a < 4; a++) {
        u8 app = order[a];
        if (!(d->apps & app)) continue;
        const char *name = app == HID_APP_KEYBOARD ? "keyboard" : app == HID_APP_CONSUMER ? "consumer control" :
                           app == HID_APP_SYSTEM ? "system control" : d->ptr.absolute ? "tablet" : "mouse";
        put(&b, "%s%s", first ? "" : " + ", name);
        const char *sep = " (";
        if (first && protocol) { put(&b, "%s%s", sep, protocol); sep = ", "; }
        if (app == HID_APP_KEYBOARD) {
            int nkro = 0, slots = 0;
            for (int i = 0; i < d->nfields; i++) {
                const hid_field_t *f = &d->f[i];
                if (f->app != app) continue;
                u32 lo = f->nlist ? d->usage[f->list] : f->umin;
                if ((lo >> 16) != PAGE_KBD) continue;
                if (f->kind == HID_F_ARRAY) slots += f->count;
                else if (f->kind == HID_F_BITMAP && (lo & 0xFFFF) < 0xE0) nkro = 1;
            }
            if (nkro) { put(&b, "%sNKRO bitmap", sep); sep = ", "; }
            if (slots) { put(&b, "%s%d-key array", sep, slots); sep = ", "; }
        }
        if (d->has_ids) { put(&b, "%sreport ID %u", sep, d->app_rid[app_index(app)]); sep = ", "; }
        if (sep[0] == ',') put(&b, ")");
        first = 0;
    }
    if (first) put(&b, "nothing usable");
}
