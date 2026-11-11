/* Halcyon OS - video mode choice shared by the BIOS (stage2) and UEFI loaders.
 * Self-contained: no C library. Include once per loader.
 *
 * Policy, first match wins:
 *   1. F8 "safe video": 800 x 600.
 *   2. display.width / display.height from \HALCYON\SETTINGS.INI, if offered.
 *   3. The monitor's native mode from its EDID, if offered and at most
 *      MODESEL_AUTO_MAX_W x MODESEL_AUTO_MAX_H (the UI has no scaling, so a 4K
 *      panel gets a mode it shows sharply instead, e.g. 1920 x 1080).
 *   4. The loader's own default. */
#ifndef MODESEL_H
#define MODESEL_H
#include "bootinfo.h"

#define MODESEL_AUTO_MAX_W 2560
#define MODESEL_AUTO_MAX_H 1600

/* "key=value" lookup in a settings file; returns the value as a number or -1 */
static int modesel_setting(const char *buf, unsigned len, const char *key) {
    unsigned i = 0;
    while (i < len) {
        unsigned ls = i;
        while (i < len && buf[i] != '\n') i++;
        unsigned le = i++;
        unsigned p = ls;
        while (p < le && (buf[p] == ' ' || buf[p] == '\t')) p++;
        unsigned k = 0;
        while (key[k] && p < le && (buf[p] | 0x20) == (key[k] | 0x20)) { p++; k++; }
        if (key[k]) continue;
        while (p < le && (buf[p] == ' ' || buf[p] == '\t')) p++;
        if (p >= le || buf[p] != '=') continue;
        p++;
        while (p < le && (buf[p] == ' ' || buf[p] == '\t')) p++;
        int v = 0, digits = 0;
        while (p < le && buf[p] >= '0' && buf[p] <= '9' && digits < 6) { v = v * 10 + (buf[p] - '0'); p++; digits++; }
        return digits ? v : -1;
    }
    return -1;
}

/* the preferred (native) mode in an EDID base block; 0 if none */
static int modesel_edid_native(const uint8_t *e, unsigned size, int *w, int *h) {
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (size < 128) return 0;
    for (int i = 0; i < 8; i++) if (e[i] != hdr[i]) return 0;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + e[i]);
    if (sum) return 0;
    const uint8_t *d = e + 54;                 /* first detailed timing = preferred */
    if (d[0] == 0 && d[1] == 0) return 0;
    *w = d[2] | ((d[4] & 0xF0) << 4);
    *h = d[5] | ((d[7] & 0xF0) << 4);
    return *w >= 320 && *h >= 200;
}

static int modesel_find(const struct bootinfo *bi, int w, int h) {
    int best = -1;
    for (unsigned i = 0; i < bi->mode_count; i++) {
        const struct bootinfo_mode *m = &bi->modes[i];
        if (m->width != w || m->height != h) continue;
        if (best < 0 || (m->bpp == 32 && bi->modes[best].bpp != 32)) best = (int)i;
    }
    return best;
}

/* largest mode of the same shape no bigger than the cap (a 3840 x 2160 panel
 * gets 1920 x 1080, which it scales by exactly two) */
static int modesel_scaled(const struct bootinfo *bi, int nw, int nh) {
    int best = -1;
    unsigned long barea = 0;
    for (unsigned i = 0; i < bi->mode_count; i++) {
        const struct bootinfo_mode *m = &bi->modes[i];
        if (m->width > MODESEL_AUTO_MAX_W || m->height > MODESEL_AUTO_MAX_H) continue;
        if ((unsigned long)m->width * (unsigned)nh != (unsigned long)m->height * (unsigned)nw) continue;
        unsigned long a = (unsigned long)m->width * m->height;
        if (a > barea || (a == barea && m->bpp == 32)) { barea = a; best = (int)i; }
    }
    return best;
}

/* returns the index of the mode to use, or -1 for "the loader's default";
 * sets bi->mode_reason */
static int modesel_choose(struct bootinfo *bi, const char *settings, unsigned slen) {
    int i;
    if (bi->boot_flags & BOOTF_SAFE_VIDEO) {
        bi->mode_reason = MODE_BY_SAFE;
        return modesel_find(bi, 800, 600);
    }
    if (settings) {
        int w = modesel_setting(settings, slen, "display.width");
        int h = modesel_setting(settings, slen, "display.height");
        if (w > 0 && h > 0 && (i = modesel_find(bi, w, h)) >= 0) { bi->mode_reason = MODE_BY_SETTING; return i; }
    }
    int nw, nh;
    if (modesel_edid_native(bi->edid, bi->edid_size, &nw, &nh)) {
        if (nw <= MODESEL_AUTO_MAX_W && nh <= MODESEL_AUTO_MAX_H && (i = modesel_find(bi, nw, nh)) >= 0) {
            bi->mode_reason = MODE_BY_EDID;
            return i;
        }
        if ((i = modesel_scaled(bi, nw, nh)) >= 0) { bi->mode_reason = MODE_BY_EDID; return i; }
    }
    bi->mode_reason = MODE_BY_DEFAULT;
    return -1;
}

#endif
