/* Halcyon OS - display timings and mode lists (see modes.h).
 *
 * References: VESA E-EDID 1.4 (detailed / standard / established timings),
 * CTA-861 (extension blocks, short video descriptors), VESA DMT 1.13 (the
 * table below is a subset of its timings) and VESA CVT 1.2 (reduced
 * blanking, version 1). */
#include "modes.h"
#include "core/lib.h"

#define P DT_HSYNC_POS
#define V DT_VSYNC_POS
typedef struct { u16 w, h, hz; u32 clk; u16 h1, h2, h3, h4, v1, v2, v3, v4; u16 flags; } tdef_t;

/* VESA DMT (and CTA-861 for the TV sizes) */
static const tdef_t dmt[] = {
    { 640, 480, 60, 25175, 640, 656, 752, 800, 480, 490, 492, 525, 0 },
    { 640, 480, 72, 31500, 640, 664, 704, 832, 480, 489, 492, 520, 0 },
    { 640, 480, 75, 31500, 640, 656, 720, 840, 480, 481, 484, 500, 0 },
    { 720, 400, 70, 28322, 720, 738, 846, 900, 400, 412, 414, 449, V },
    { 800, 600, 56, 36000, 800, 824, 896, 1024, 600, 601, 603, 625, P | V },
    { 800, 600, 60, 40000, 800, 840, 968, 1056, 600, 601, 605, 628, P | V },
    { 800, 600, 72, 50000, 800, 856, 976, 1040, 600, 637, 643, 666, P | V },
    { 800, 600, 75, 49500, 800, 816, 896, 1056, 600, 601, 604, 625, P | V },
    { 1024, 768, 60, 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, 0 },
    { 1024, 768, 70, 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, 0 },
    { 1024, 768, 75, 78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, P | V },
    { 1152, 864, 75, 108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, P | V },
    { 1280, 720, 60, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, P | V },
    { 1280, 768, 60, 79500, 1280, 1344, 1472, 1664, 768, 771, 778, 798, V },
    { 1280, 800, 60, 83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, V },
    { 1280, 960, 60, 108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, P | V },
    { 1280, 1024, 60, 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, P | V },
    { 1280, 1024, 75, 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, P | V },
    { 1360, 768, 60, 85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, P | V },
    { 1366, 768, 60, 85500, 1366, 1436, 1579, 1792, 768, 771, 774, 798, P | V },
    { 1400, 1050, 60, 121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, V },
    { 1440, 900, 60, 106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, V },
    { 1600, 900, 60, 108000, 1600, 1624, 1704, 1800, 900, 901, 904, 1000, P | V },
    { 1600, 1200, 60, 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, P | V },
    { 1680, 1050, 60, 146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, V },
    { 1920, 1080, 60, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, P | V },
    { 1920, 1200, 60, 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, P },
    { 2560, 1440, 60, 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, P },
    { 2560, 1600, 60, 268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, P },
    { 3840, 2160, 60, 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, P | V },
};

/* CTA-861 video identification codes (progressive formats only) */
static const struct { u8 vic; tdef_t t; } cea[] = {
    { 1, { 640, 480, 60, 25175, 640, 656, 752, 800, 480, 490, 492, 525, 0 } },
    { 2, { 720, 480, 60, 27000, 720, 736, 798, 858, 480, 489, 495, 525, 0 } },
    { 3, { 720, 480, 60, 27000, 720, 736, 798, 858, 480, 489, 495, 525, 0 } },
    { 4, { 1280, 720, 60, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, P | V } },
    { 16, { 1920, 1080, 60, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, P | V } },
    { 17, { 720, 576, 50, 27000, 720, 732, 796, 864, 576, 581, 586, 625, 0 } },
    { 18, { 720, 576, 50, 27000, 720, 732, 796, 864, 576, 581, 586, 625, 0 } },
    { 19, { 1280, 720, 50, 74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, P | V } },
    { 31, { 1920, 1080, 50, 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, P | V } },
    { 95, { 3840, 2160, 30, 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, P | V } },
    { 97, { 3840, 2160, 60, 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, P | V } },
};
#undef P
#undef V

u32 mode_refresh_mhz(const display_timing_t *t) {
    u64 tot = (u64)t->htotal * t->vtotal;
    if (!t->clock_khz || !tot) return 0;
    return (u32)(((u64)t->clock_khz * 1000000ull + tot / 2) / tot);
}

void mode_set_timing(display_mode_t *m, const display_timing_t *t, u32 flags) {
    m->t = *t;
    m->w = t->hactive;
    m->h = t->vactive;
    m->flags = flags;
    m->hz = (int)((mode_refresh_mhz(t) + 500) / 1000);
}

static void from_def(const tdef_t *d, u32 flags, display_mode_t *out) {
    display_timing_t t = { d->clk, d->h1, d->h2, d->h3, d->h4, d->v1, d->v2, d->v3, d->v4 };
    mode_set_timing(out, &t, flags | d->flags);
}

int mode_dmt(int w, int h, int hz, display_mode_t *out) {
    const tdef_t *best = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(dmt); i++) {
        if (dmt[i].w != w || dmt[i].h != h) continue;
        if (hz ? dmt[i].hz == hz : (!best || (dmt[i].hz == 60) || (best->hz != 60 && dmt[i].hz < best->hz))) best = &dmt[i];
        if (hz && best) break;
    }
    if (!best) return 0;
    from_def(best, DT_DMT, out);
    return 1;
}

void mode_cvt_rb(int w, int h, int hz, display_mode_t *out) {
    if (hz <= 0) hz = 60;
    int vsync = (w * 3 == h * 4) ? 4 : (w * 9 == h * 16) ? 5 : (w * 10 == h * 16) ? 6 :
                (w * 4 == h * 5 || w * 9 == h * 15) ? 7 : 10;
    /* lines in the 460 us minimum vertical blank, at the estimated line period */
    u64 num = 460ull * (u64)h * (u64)hz, den = 1000000ull - 460ull * (u64)hz;
    int vbi = (int)(num / den) + 1, minvbi = 3 + vsync + 6;
    if (vbi < minvbi) vbi = minvbi;
    display_timing_t t;
    t.hactive = (u16)w;
    t.hsync_start = (u16)(w + 48);
    t.hsync_end = (u16)(w + 80);
    t.htotal = (u16)(w + 160);
    t.vactive = (u16)h;
    t.vsync_start = (u16)(h + 3);
    t.vsync_end = (u16)(h + 3 + vsync);
    t.vtotal = (u16)(h + vbi);
    u64 khz = (u64)hz * t.vtotal * t.htotal / 1000;
    t.clock_khz = (u32)(khz - khz % 250);            /* 0.25 MHz steps */
    mode_set_timing(out, &t, DT_CVT | DT_HSYNC_POS);
}

int edid_detailed(const u8 *p, display_mode_t *out) {
    if (!p[0] && !p[1]) return 0;                    /* a display descriptor */
    display_timing_t t;
    int ha = p[2] | (p[4] & 0xF0) << 4, hb = p[3] | (p[4] & 0x0F) << 8;
    int va = p[5] | (p[7] & 0xF0) << 4, vb = p[6] | (p[7] & 0x0F) << 8;
    int hso = p[8] | (p[11] & 0xC0) << 2, hsw = p[9] | (p[11] & 0x30) << 4;
    int vso = (p[10] >> 4) | (p[11] & 0x0C) << 2, vsw = (p[10] & 0x0F) | (p[11] & 0x03) << 4;
    if (ha < 64 || va < 64 || !hb || !vb) return 0;
    t.clock_khz = (u32)(p[0] | p[1] << 8) * 10;
    t.hactive = (u16)ha;
    t.hsync_start = (u16)(ha + hso);
    t.hsync_end = (u16)(ha + hso + hsw);
    t.htotal = (u16)(ha + hb);
    t.vactive = (u16)va;
    t.vsync_start = (u16)(va + vso);
    t.vsync_end = (u16)(va + vso + vsw);
    t.vtotal = (u16)(va + vb);
    u32 fl = DT_DETAILED;
    if ((p[17] & 0x18) == 0x18) {                    /* digital separate sync */
        if (p[17] & 0x04) fl |= DT_VSYNC_POS;
        if (p[17] & 0x02) fl |= DT_HSYNC_POS;
    } else if ((p[17] & 0x18) == 0x10) {             /* digital composite: bit 1 is the polarity */
        if (p[17] & 0x02) fl |= DT_HSYNC_POS;
    }
    mode_set_timing(out, &t, fl);
    if (p[17] & 0x80) {                              /* interlaced: the fields make the frame */
        out->flags |= DT_INTERLACE;
        out->h *= 2;
        out->hz *= 2;
    }
    return 1;
}

static int add_timing(display_mode_t *out, int n, int max, const display_mode_t *m) {
    if (n >= max) return n;
    out[n] = *m;
    out[n].flags |= DT_EDID;
    return n + 1;
}

static int std_timing(u8 b0, u8 b1, int rev13, display_mode_t *m) {
    if ((b0 == 0x01 && b1 == 0x01) || b0 == 0x00) return 0;
    int w = (b0 + 31) * 8, h;
    switch (b1 >> 6) {
    case 0: h = rev13 ? w * 10 / 16 : w; break;
    case 1: h = w * 3 / 4; break;
    case 2: h = w * 4 / 5; break;
    default: h = w * 9 / 16; break;
    }
    int hz = (b1 & 0x3F) + 60;
    if (w == 1360 && h == 765) h = 768;              /* DMT's rounding of 16:9 */
    if (!mode_dmt(w, h, hz, m)) mode_cvt_rb(w, h, hz, m);
    return 1;
}

int edid_timings(const u8 *e, int size, display_mode_t *out, int max) {
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!e || size < 128 || memcmp(e, hdr, 8)) return 0;
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + e[i]);
    if (sum) return 0;
    int n = 0, rev13 = e[18] > 1 || (e[18] == 1 && e[19] >= 3);
    display_mode_t m;
    /* detailed timings: the first one is the preferred (native) timing */
    for (int d = 0; d < 4; d++) {
        const u8 *p = e + 54 + d * 18;
        if (edid_detailed(p, &m)) {
            if (n == 0 && d == 0) m.flags |= DT_PREFERRED;
            n = add_timing(out, n, max, &m);
        } else if (!p[0] && !p[1] && p[3] == 0xFA) {   /* more standard timings */
            for (int k = 0; k < 6; k++)
                if (std_timing(p[5 + 2 * k], p[6 + 2 * k], rev13, &m)) n = add_timing(out, n, max, &m);
        }
    }
    /* established timings */
    static const struct { u8 byte, bit; u16 w, h, hz; } est[] = {
        { 35, 5, 640, 480, 60 }, { 35, 3, 640, 480, 72 }, { 35, 2, 640, 480, 75 }, { 35, 1, 800, 600, 56 },
        { 35, 0, 800, 600, 60 }, { 36, 7, 800, 600, 72 }, { 36, 6, 800, 600, 75 }, { 36, 3, 1024, 768, 60 },
        { 36, 2, 1024, 768, 70 }, { 36, 1, 1024, 768, 75 }, { 36, 0, 1280, 1024, 75 }, { 35, 7, 720, 400, 70 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(est); i++)
        if ((e[est[i].byte] >> est[i].bit) & 1 && mode_dmt(est[i].w, est[i].h, est[i].hz, &m)) n = add_timing(out, n, max, &m);
    /* standard timings */
    for (int k = 0; k < 8; k++)
        if (std_timing(e[38 + 2 * k], e[39 + 2 * k], rev13, &m)) n = add_timing(out, n, max, &m);
    /* CTA-861 extension blocks: short video descriptors, detailed timings */
    int ext = e[126];
    for (int b = 1; b <= ext && (b + 1) * 128 <= size; b++) {
        const u8 *x = e + b * 128;
        if (x[0] != 0x02) continue;
        int dtd = x[2];
        if (dtd >= 4 && dtd <= 127) {
            for (int o = 4; o < dtd;) {
                int tag = x[o] >> 5, len = x[o] & 31;
                if (o + 1 + len > dtd) break;
                if (tag == 2)
                    for (int k = 0; k < len; k++) {
                        int vic = x[o + 1 + k], native = 0;
                        if (vic >= 129 && vic <= 192) { vic &= 0x7F; native = 1; }
                        for (unsigned c = 0; c < ARRAY_SIZE(cea); c++)
                            if (cea[c].vic == vic) {
                                from_def(&cea[c].t, 0, &m);
                                if (native && n == 0) m.flags |= DT_PREFERRED;
                                n = add_timing(out, n, max, &m);
                                break;
                            }
                    }
                o += 1 + len;
            }
            for (int o = dtd; o + 18 <= 127; o += 18)
                if (edid_detailed(x + o, &m)) n = add_timing(out, n, max, &m);
        }
    }
    return n;
}

/* ---------------- the mode list ---------------- */
static int within(const mode_limits_t *l, const display_mode_t *m) {
    int minw = l->min_w ? l->min_w : 640, minh = l->min_h ? l->min_h : 480;
    if (m->flags & DT_INTERLACE) return 0;
    if (m->w < minw || m->h < minh) return 0;
    if (l->max_w && m->w > l->max_w) return 0;
    if (l->max_h && m->h > l->max_h) return 0;
    if (l->max_clock_khz && m->t.clock_khz > l->max_clock_khz) return 0;
    if (l->vram && (u64)m->w * m->h * 4 * (u64)(l->buffers > 0 ? l->buffers : 1) > l->vram) return 0;
    if (l->valid && !l->valid(l->ctx, m)) return 0;
    return 1;
}

/* is a better than b for the same size? */
static int better(const display_mode_t *a, const display_mode_t *b) {
    if ((a->flags & DT_PREFERRED) != (b->flags & DT_PREFERRED)) return (a->flags & DT_PREFERRED) != 0;
    if ((a->flags & DT_EDID) != (b->flags & DT_EDID)) return (a->flags & DT_EDID) != 0;
    int da = a->hz - 60, db = b->hz - 60;
    if (da < 0) da = -da;
    if (db < 0) db = -db;
    if (da != db) return da < db;
    if ((a->flags & DT_DMT) != (b->flags & DT_DMT)) return (a->flags & DT_DMT) != 0;
    return 0;
}

static int insert(display_mode_t *out, int n, int max, const display_mode_t *m) {
    for (int i = 0; i < n; i++)
        if (out[i].w == m->w && out[i].h == m->h) {
            if (better(m, &out[i])) {
                u32 keep = out[i].flags & DT_PREFERRED;
                out[i] = *m;
                out[i].flags |= keep;
            } else {
                out[i].flags |= m->flags & DT_PREFERRED;
            }
            return n;
        }
    if (n >= max) return n;
    int i = n++;
    while (i > 0 && (out[i - 1].w > m->w || (out[i - 1].w == m->w && out[i - 1].h > m->h))) {
        out[i] = out[i - 1];
        i--;
    }
    out[i] = *m;
    return n;
}

int modes_build(const u8 *edid, int edid_len, const mode_limits_t *lim, int pref_w, int pref_h,
                display_mode_t *out, int max) {
    static const u16 common[][2] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 },
        { 1280, 960 }, { 1280, 1024 }, { 1360, 768 }, { 1400, 1050 }, { 1440, 900 }, { 1600, 900 },
        { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2048, 1152 }, { 2560, 1440 },
        { 2560, 1600 }, { 3840, 2160 },
    };
    display_mode_t t[96], m;
    int nt = edid_len >= 128 ? edid_timings(edid, edid_len, t, (int)ARRAY_SIZE(t)) : 0;
    int n = 0, have_pref = 0;
    for (int i = 0; i < nt; i++)
        if (within(lim, &t[i])) {
            n = insert(out, n, max, &t[i]);
            if (t[i].flags & DT_PREFERRED) have_pref = 1;
        }
    if (lim->virtual_sizes) {
        for (unsigned i = 0; i < ARRAY_SIZE(common); i++) {
            if (!mode_dmt(common[i][0], common[i][1], 60, &m) && !mode_dmt(common[i][0], common[i][1], 0, &m))
                mode_cvt_rb(common[i][0], common[i][1], 60, &m);
            m.flags |= DT_DRIVER;
            if (within(lim, &m)) n = insert(out, n, max, &m);
        }
    }
    /* the size the hardware asks for (a virtual machine's window), when the
     * EDID has no preferred timing */
    if (!have_pref && pref_w > 0 && pref_h > 0) {
        if (!mode_dmt(pref_w, pref_h, 60, &m)) mode_cvt_rb(pref_w, pref_h, 60, &m);
        m.flags |= DT_PREFERRED | DT_DRIVER;
        if (within(lim, &m)) n = insert(out, n, max, &m);
    }
    return n;
}

int modes_find(const display_mode_t *m, int n, int w, int h) {
    for (int i = 0; i < n; i++) if (m[i].w == w && m[i].h == h) return i;
    return -1;
}

int modes_preferred(const display_mode_t *m, int n) {
    for (int i = 0; i < n; i++) if (m[i].flags & DT_PREFERRED) return i;
    return -1;
}

int modes_default(const display_mode_t *m, int n, int cap_w, int cap_h) {
    if (n <= 0) return -1;
    int p = modes_preferred(m, n);
    if (p >= 0) {
        int nw = m[p].w, nh = m[p].h, i;
        if (nw <= cap_w && nh <= cap_h) return p;
        for (int k = 2; k <= 4; k++)
            if (!(nw % k) && !(nh % k) && nw / k <= cap_w && nh / k <= cap_h && (i = modes_find(m, n, nw / k, nh / k)) >= 0)
                return i;
        int best = -1;
        for (i = 0; i < n; i++)
            if (m[i].w <= cap_w && m[i].h <= cap_h && (long)m[i].w * nh == (long)m[i].h * nw &&
                (best < 0 || (long)m[i].w * m[i].h > (long)m[best].w * m[best].h)) best = i;
        if (best >= 0) return best;
    }
    int best = -1;
    for (int i = 0; i < n; i++)
        if (m[i].w <= 1024 && m[i].h <= 768 && (best < 0 || (long)m[i].w * m[i].h > (long)m[best].w * m[best].h)) best = i;
    return best >= 0 ? best : 0;
}

void mode_describe(const display_mode_t *m, char *out, int n) {
    const display_timing_t *t = &m->t;
    if (!t->clock_khz) { snprintf(out, (size_t)n, "%dx%d", m->w, m->h); return; }
    snprintf(out, (size_t)n, "%dx%d@%d %u.%02u MHz %u %u %u %u / %u %u %u %u %ch %cv%s", m->w, m->h, m->hz,
             t->clock_khz / 1000, (t->clock_khz % 1000) / 10, t->hactive, t->hsync_start, t->hsync_end, t->htotal,
             t->vactive, t->vsync_start, t->vsync_end, t->vtotal, m->flags & DT_HSYNC_POS ? '+' : '-',
             m->flags & DT_VSYNC_POS ? '+' : '-', m->flags & DT_INTERLACE ? " interlaced" : "");
}
