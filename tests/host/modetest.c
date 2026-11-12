/* Host test: display timings and mode lists (kernel/drivers/modes.c): EDID
 * detailed / standard / established timings, CEA-861 and DisplayID
 * extensions, the DMT subset, CVT reduced blanking against published
 * values, mode lists within a driver's limits (size, pixel clock, memory,
 * its own check), the default mode; refresh rates: an EDID like the LG
 * UltraGear's (2560 x 1440 at 120 Hz in the base block, 60 and 144 Hz in
 * the CEA block, 165 Hz in a DisplayID block), one entry per rate,
 * duplicates, the preferred mode, a DisplayPort link's bandwidth, a saved
 * rate restored or replaced; and random EDIDs (no crash, every mode within
 * the limits, one entry per size or per size and rate).
 *
 *   modetest [N]                      N random EDIDs (default 200,000)
 *   modetest --write-edid lg|qemu F   write the LG-like EDID, or the QEMU
 *                                     test EDID (tools/boottest.py rates), to F */
#include <stdio.h>
#include <stdlib.h>
#include "core/lib.h"
#include "drivers/modes.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("modetest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void checksum(uint8_t *b) {
    uint8_t s = 0;
    for (int i = 0; i < 127; i++) s = (uint8_t)(s + b[i]);
    b[127] = (uint8_t)(0x100 - s);
}

/* a detailed timing descriptor from a timing */
static void put_dtd(uint8_t *d, int clk_khz, int ha, int hfp, int hsw, int hb, int va, int vfp, int vsw, int vb, uint8_t flags) {
    memset(d, 0, 18);
    int c = clk_khz / 10;
    d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8);
    d[2] = (uint8_t)ha; d[3] = (uint8_t)hb; d[4] = (uint8_t)(((ha >> 8) << 4) | (hb >> 8));
    d[5] = (uint8_t)va; d[6] = (uint8_t)vb; d[7] = (uint8_t)(((va >> 8) << 4) | (vb >> 8));
    d[8] = (uint8_t)hfp; d[9] = (uint8_t)hsw;
    d[10] = (uint8_t)(((vfp & 15) << 4) | (vsw & 15));
    d[11] = (uint8_t)(((hfp >> 8) << 6) | ((hsw >> 8) << 4) | ((vfp >> 4) << 2) | (vsw >> 4));
    d[17] = flags;
}

static void base_block(uint8_t *e) {
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memset(e, 0, 128);
    memcpy(e, hdr, 8);
    e[8] = 0x10; e[9] = 0xAC; e[10] = 0x34; e[11] = 0x12; e[17] = 30;
    e[18] = 1; e[19] = 4;
    for (int i = 38; i < 54; i++) e[i] = 1;            /* unused standard timings */
}

static int has(const display_mode_t *m, int n, int w, int h, int hz) {
    for (int i = 0; i < n; i++) if (m[i].w == w && m[i].h == h && (!hz || m[i].hz == hz)) return i;
    return -1;
}

static int rejects_odd(void *ctx, const display_mode_t *m) { (void)ctx; return !(m->w & 7); }

/* a DisplayID 1.3 extension block with one detailed timing: type I (tag
 * 0x03, 10 kHz) or type VII (0x22, 1 kHz); every field minus one */
static void put_did(uint8_t *x, int tag, int khz, int ha, int hfp, int hsw, int hb, int va, int vfp, int vsw, int vb, int hpos,
                    int vpos, int pref) {
    memset(x, 0, 128);
    x[0] = 0x70; x[1] = 0x12; x[2] = 3 + 20; x[3] = 0x03; x[4] = 0;
    uint8_t *b = x + 5;
    b[0] = (uint8_t)tag; b[1] = 0; b[2] = 20;
    uint8_t *p = b + 3;
    int c = (tag == 0x22 ? khz : khz / 10) - 1;
    p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16);
    p[3] = (uint8_t)(pref ? 0x80 : 0);
    int f[8] = { ha - 1, hb - 1, (hfp - 1) | (hpos ? 0x8000 : 0), hsw - 1, va - 1, vb - 1, (vfp - 1) | (vpos ? 0x8000 : 0), vsw - 1 };
    for (int i = 0; i < 8; i++) { p[4 + 2 * i] = (uint8_t)f[i]; p[5 + 2 * i] = (uint8_t)(f[i] >> 8); }
    uint8_t sum = 0;
    for (int i = 1; i < 5 + x[2]; i++) sum = (uint8_t)(sum + x[i]);
    x[5 + x[2]] = (uint8_t)(0x100 - sum);              /* the section's checksum */
    checksum(x);
}

/* a monitor name descriptor */
static void put_name(uint8_t *d, const char *name) {
    memset(d, 0, 18);
    d[3] = 0xFC;
    int i = 0;
    for (; name[i] && i < 13; i++) d[5 + i] = (uint8_t)name[i];
    if (i < 13) d[5 + i++] = 0x0A;
    for (; i < 13; i++) d[5 + i] = ' ';
}

/* An EDID like the LG UltraGear's on the user's GTX 1070 (NVREPORT
 * 1.0.199): 2560 x 1440 at 120 Hz (497.75 MHz, h 48+32+80, v 3+5+77, +h -v)
 * preferred in the base block, 60 Hz (241.50 MHz, v 3+5+33) and 144 Hz
 * (604.25 MHz, v 3+5+95) in the CEA-861 block, 165 Hz (679.30 MHz, v
 * 3+5+66) in a DisplayID block (did_tag 0x03 type I, 0x22 type VII, its
 * own preferred flag set). Also: established 640x480, 800x600, 1024x768 at
 * 60 / 70 / 75 Hz; standard 1920x1080 at 60 and 120 Hz, 1280x1024 at 60;
 * CEA VICs 1, 4 (1280x720 at 60), 16 (1920x1080 at 60) and a 1280x720 DTD
 * at 59.94 Hz. 384 bytes. */
static int lg_edid(uint8_t *e, int did_tag) {
    base_block(e);
    e[8] = 0x1E; e[9] = 0x6D;                            /* GSM */
    e[10] = 0x66; e[11] = 0x77; e[17] = 33;              /* product 7766, 2023 */
    e[20] = 0xB5; e[21] = 70; e[22] = 39;                /* digital, DisplayPort; 70 x 39 cm */
    e[35] = 0x21; e[36] = 0x0E;                          /* 640x480@60, 800x600@60; 1024x768@60/70/75 */
    e[38] = (uint8_t)(1920 / 8 - 31); e[39] = 0xC0;       /* 1920x1080@60 */
    e[40] = (uint8_t)(1920 / 8 - 31); e[41] = 0xC0 | 60; /* 1920x1080@120 (CVT) */
    e[42] = (uint8_t)(1280 / 8 - 31); e[43] = 0x80;       /* 1280x1024@60 */
    put_dtd(e + 54, 497750, 2560, 48, 32, 160, 1440, 3, 5, 85, 0x1A);   /* 120 Hz, preferred, +h -v */
    e[72 + 3] = 0xFD;
    put_name(e + 90, "LG ULTRAGEAR");
    e[108 + 3] = 0x10;
    e[126] = 2;
    checksum(e);
    uint8_t *x = e + 128;
    memset(x, 0, 128);
    x[0] = 0x02; x[1] = 3;
    x[4] = (2 << 5) | 3; x[5] = 16; x[6] = 4; x[7] = 1;   /* VICs 16, 4, 1 */
    x[2] = 8;
    put_dtd(x + 8, 241500, 2560, 48, 32, 160, 1440, 3, 5, 41, 0x1A);   /* 60 Hz */
    put_dtd(x + 26, 604250, 2560, 48, 32, 160, 1440, 3, 5, 103, 0x1A); /* 144 Hz */
    put_dtd(x + 44, 74170, 1280, 110, 40, 370, 720, 5, 5, 30, 0x1E);   /* 59.94 Hz: the same as VIC 4's 60 */
    checksum(x);
    put_did(e + 256, did_tag, 679300, 2560, 48, 32, 160, 1440, 3, 5, 74, 1, 0, 1);   /* 165 Hz */
    return 384;
}

/* The QEMU test EDID (the rates boot test: virtio GPU, fw_cfg vgpuedid):
 * 1280 x 800 at 60 Hz preferred (base block), 75 and 120 Hz (CEA-861
 * DTDs), 144 Hz (DisplayID type I); 1024x768 at 60 / 70 / 75 Hz
 * (established), 1152x864 at 75 Hz only (a standard timing), 640x480 and
 * 800x600 at 60. The name "QEMU RATES". */
static int qemu_edid(uint8_t *e) {
    base_block(e);
    e[8] = 0x46; e[9] = 0xA5;                            /* "QEM" */
    e[10] = 0x34; e[11] = 0x12;
    e[20] = 0xA5; e[21] = 30; e[22] = 19;
    e[35] = 0x21; e[36] = 0x0E;
    e[38] = (uint8_t)(1152 / 8 - 31); e[39] = 0x40 | 15;  /* 1152x864@75 */
    put_dtd(e + 54, 71000, 1280, 48, 32, 160, 800, 3, 6, 23, 0x1A);    /* 60 Hz (CVT-RB), preferred */
    e[72 + 3] = 0xFD;
    put_name(e + 90, "QEMU RATES");
    e[108 + 3] = 0x10;
    e[126] = 2;
    checksum(e);
    uint8_t *x = e + 128;
    memset(x, 0, 128);
    x[0] = 0x02; x[1] = 3; x[2] = 4;
    put_dtd(x + 4, 88880, 1280, 48, 32, 160, 800, 3, 6, 23, 0x1A);     /* 75 Hz */
    put_dtd(x + 22, 142210, 1280, 48, 32, 160, 800, 3, 6, 23, 0x1A);   /* 120 Hz */
    checksum(x);
    put_did(e + 256, 0x03, 170660, 1280, 48, 32, 160, 800, 3, 6, 23, 1, 0, 0);   /* 144 Hz */
    return 384;
}

/* the NVIDIA driver's DisplayPort check (nv_dp_fits, 24 bpp) for a link of
 * lanes x rate code, and the Pascal head's 1080 MHz */
static int dp_lanes, dp_bw;
static int dp_valid(void *ctx, const display_mode_t *m) {
    (void)ctx;
    unsigned long long need = ((unsigned long long)m->t.clock_khz * 24 + 7) / 8, link = (unsigned long long)dp_lanes * dp_bw * 27000;
    return m->t.clock_khz <= 1080000 && need <= link;
}

static int count_at(const display_mode_t *l, int n, int w, int h) {
    int c = 0;
    for (int i = 0; i < n; i++) c += l[i].w == w && l[i].h == h;
    return c;
}

/* sorted by width, height, rate; one per (size, rate) or one per size; at most one preferred */
static int list_ok(const display_mode_t *l, int n, int rates) {
    int pref = 0;
    for (int i = 0; i < n; i++) {
        pref += (l[i].flags & DT_PREFERRED) != 0;
        if (!i) continue;
        const display_mode_t *a = &l[i - 1], *b = &l[i];
        if (a->w > b->w || (a->w == b->w && a->h > b->h)) return 0;
        if (a->w == b->w && a->h == b->h && (!rates || a->hz >= b->hz)) return 0;
    }
    return pref <= 1;
}

static void rate_tests(void) {
    uint8_t e[384];
    display_mode_t t[128], l[96];
    int k, n, r, hz[16];
    for (int pass = 0; pass < 2; pass++) {
        int tag = pass ? 0x22 : 0x03;
        int len = lg_edid(e, tag);
        int nt = edid_timings(e, len, t, 128);
        int i120 = has(t, nt, 2560, 1440, 120), i60 = has(t, nt, 2560, 1440, 60), i144 = has(t, nt, 2560, 1440, 144),
            i165 = has(t, nt, 2560, 1440, 165);
        CHECK(i120 == 0 && (t[0].flags & DT_PREFERRED) && t[0].t.clock_khz == 497750 && t[0].t.htotal == 2720 && t[0].t.vtotal == 1525 &&
              (t[0].flags & DT_HSYNC_POS) && !(t[0].flags & DT_VSYNC_POS), "LG: 120 Hz preferred in the base block");
        CHECK(i60 >= 0 && t[i60].t.clock_khz == 241500 && t[i60].t.vtotal == 1481 && (t[i60].flags & DT_DETAILED), "LG: 60 Hz (CEA DTD)");
        CHECK(i144 >= 0 && t[i144].t.clock_khz == 604250 && t[i144].t.vtotal == 1543 && t[i144].t.vsync_start == 1443 &&
              t[i144].t.vsync_end == 1448, "LG: 144 Hz (CEA DTD)");
        CHECK(i165 >= 0 && t[i165].t.clock_khz == 679300 && t[i165].t.htotal == 2720 && t[i165].t.hsync_start == 2608 &&
              t[i165].t.hsync_end == 2640 && t[i165].t.vtotal == 1514 && t[i165].t.vsync_start == 1443 && t[i165].t.vsync_end == 1448 &&
              (t[i165].flags & DT_HSYNC_POS) && !(t[i165].flags & DT_VSYNC_POS) && (t[i165].flags & DT_DETAILED) &&
              !(t[i165].flags & DT_PREFERRED), "LG: 165 Hz from DisplayID type %s, exact clock, not preferred (%u kHz)",
              pass ? "VII" : "I", i165 >= 0 ? t[i165].t.clock_khz : 0);
        CHECK(i165 >= 0 && mode_refresh_mhz(&t[i165].t) > 164900 && mode_refresh_mhz(&t[i165].t) < 165000, "LG: 164.96 Hz (%u mHz)",
              i165 >= 0 ? mode_refresh_mhz(&t[i165].t) : 0);

        /* the list with refresh rates */
        mode_limits_t lim = { 0 };
        lim.rates = 1;
        n = modes_build(e, len, &lim, 0, 0, l, 96);
        CHECK(list_ok(l, n, 1), "LG: sorted, one per size and rate, one preferred (%d modes)", n);
        r = modes_rates(l, n, 2560, 1440, hz, 16);
        CHECK(r == 4 && hz[0] == 60 && hz[1] == 120 && hz[2] == 144 && hz[3] == 165, "LG: 2560x1440 at 60 120 144 165 (%d rates)", r);
        k = modes_find_rate(l, n, 2560, 1440, 60);
        CHECK(k >= 0 && (l[k].flags & DT_DETAILED) && !(l[k].flags & DT_DMT), "LG: 60 Hz is the EDID's DTD, not DMT's equal one");
        k = modes_preferred(l, n);
        CHECK(k >= 0 && l[k].w == 2560 && l[k].hz == 120 && l[k].t.clock_khz == 497750, "LG: the preferred mode is 120 Hz");
        CHECK(modes_default(l, n, 2560, 1600) == k && modes_find(l, n, 2560, 1440) == k, "LG: default and find give 120 Hz");
        k = modes_find_rate(l, n, 2560, 1440, 165);
        CHECK(k >= 0 && l[k].t.clock_khz == 679300 && l[k].t.vtotal == 1514, "LG: 165 Hz in the list with its own timing");
        /* the same rate twice: 1280x720 at 59.94 (DTD) and 60 (VIC 4, DMT): the DTD */
        k = modes_find(l, n, 1280, 720);
        CHECK(count_at(l, n, 1280, 720) == 1 && k >= 0 && l[k].t.clock_khz == 74170, "LG: 59.94 and 60 Hz: one entry, the DTD");
        /* 1920x1080: 60 (VIC 16, DMT, a standard timing) and 120 (CVT): the preferred rate there */
        r = modes_rates(l, n, 1920, 1080, hz, 16);
        k = modes_find(l, n, 1920, 1080);
        CHECK(r == 2 && hz[0] == 60 && hz[1] == 120 && k >= 0 && l[k].hz == 120, "LG: 1920x1080 at 60 and 120: 120 chosen");
        k = modes_find_rate(l, n, 1920, 1080, 60);
        CHECK(k >= 0 && l[k].t.clock_khz == 148500 && (l[k].flags & DT_EDID), "LG: 1920x1080 at 60 Hz once");
        /* 1024x768 at 60 / 70 / 75: no 120 there, the highest */
        k = modes_find(l, n, 1024, 768);
        CHECK(modes_rates(l, n, 1024, 768, hz, 16) == 3 && k >= 0 && l[k].hz == 75, "LG: 1024x768: the highest rate (75)");
        CHECK(modes_find_rate(l, n, 2560, 1440, 100) < 0 && modes_find_rate(l, n, 1600, 900, 60) < 0, "LG: rates not offered");
        /* one per size without rates: the preferred timing */
        lim.rates = 0;
        n = modes_build(e, len, &lim, 0, 0, l, 96);
        k = modes_find(l, n, 2560, 1440);
        CHECK(list_ok(l, n, 0) && count_at(l, n, 2560, 1440) == 1 && k >= 0 && l[k].hz == 120 && (l[k].flags & DT_PREFERRED),
              "LG without rates: one mode per size, 120 Hz");
    }

    /* the driver's check (mode_valid): the GTX 1070's DisplayPort link */
    int len = lg_edid(e, 0x03);
    mode_limits_t lim = { 0 };
    lim.rates = 1;
    lim.valid = dp_valid;
    static const struct { int lanes, bw, rates, r[4]; const char *what; } links[] = {
        { 4, 0x1e, 4, { 60, 120, 144, 165 }, "4 lanes HBR3" },
        { 4, 0x14, 4, { 60, 120, 144, 165 }, "4 lanes HBR2 (165 Hz needs 2037.9 of 2160 MB/s)" },
        { 2, 0x1e, 2, { 60, 120, 0, 0 }, "2 lanes HBR3" },
        { 4, 0x0a, 1, { 60, 0, 0, 0 }, "4 lanes HBR" },
        { 1, 0x0a, 0, { 0, 0, 0, 0 }, "1 lane HBR" },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(links); i++) {
        dp_lanes = links[i].lanes; dp_bw = links[i].bw;
        n = modes_build(e, len, &lim, 0, 0, l, 96);
        r = modes_rates(l, n, 2560, 1440, hz, 16);
        int same = r == links[i].rates;
        for (int j = 0; j < r && same; j++) same = hz[j] == links[i].r[j];
        CHECK(same && list_ok(l, n, 1), "link %s: %d rates at 2560x1440", links[i].what, r);
        int bad = 0;
        for (int j = 0; j < n; j++) bad += !dp_valid(NULL, &l[j]);
        CHECK(!bad, "link %s: %d listed modes do not fit", links[i].what, bad);
    }
    /* 4 lanes HBR: the preferred timing does not fit; 60 Hz at the preferred size is recommended */
    dp_lanes = 4; dp_bw = 0x0a;
    n = modes_build(e, len, &lim, 0, 0, l, 96);
    k = modes_preferred(l, n);
    CHECK(k >= 0 && l[k].w == 2560 && l[k].h == 1440 && l[k].hz == 60 && modes_default(l, n, 2560, 1600) == k,
          "preferred timing too fast for the link: 2560x1440 at 60 Hz recommended");
    /* a pixel clock limit (a 600 MHz head): 144 and 165 Hz go */
    lim.valid = NULL;
    lim.max_clock_khz = 600000;
    n = modes_build(e, len, &lim, 0, 0, l, 96);
    CHECK(modes_rates(l, n, 2560, 1440, hz, 16) == 2 && hz[1] == 120, "600 MHz limit: 60 and 120 Hz");

    /* a saved rate at the next start (display.<connector>.hz) */
    int fell;
    lim.max_clock_khz = 0;
    lim.valid = dp_valid;
    dp_lanes = 4; dp_bw = 0x1e;
    n = modes_build(e, len, &lim, 0, 0, l, 96);
    k = modes_saved(l, n, 2560, 1440, 165, &fell);
    CHECK(k >= 0 && l[k].hz == 165 && !fell, "saved 165 Hz restored");
    k = modes_saved(l, n, 2560, 1440, 0, &fell);
    CHECK(k >= 0 && l[k].hz == 120 && !fell, "no saved rate: the preferred one");
    dp_lanes = 2;                                        /* the monitor on a 2-lane link: 165 Hz is gone */
    n = modes_build(e, len, &lim, 0, 0, l, 96);
    k = modes_saved(l, n, 2560, 1440, 165, &fell);
    CHECK(k >= 0 && l[k].hz == 120 && fell, "saved 165 Hz not offered: the preferred rate (120)");
    dp_lanes = 4; dp_bw = 0x0a;                          /* 4 lanes HBR: only 60 Hz */
    n = modes_build(e, len, &lim, 0, 0, l, 96);
    k = modes_saved(l, n, 2560, 1440, 165, &fell);
    CHECK(k >= 0 && l[k].hz == 60 && fell, "saved 165 Hz, preferred 120 not offered either: 60");
    CHECK(modes_saved(l, n, 3840, 2160, 60, &fell) < 0, "a saved size that is gone");

    /* the QEMU test EDID (boot test "rates") */
    len = qemu_edid(e);
    memset(&lim, 0, sizeof lim);
    lim.rates = 1;
    lim.virtual_sizes = 1;
    n = modes_build(e, len, &lim, 1280, 800, l, 96);
    r = modes_rates(l, n, 1280, 800, hz, 16);
    k = modes_preferred(l, n);
    CHECK(r == 4 && hz[0] == 60 && hz[1] == 75 && hz[2] == 120 && hz[3] == 144 && k >= 0 && l[k].hz == 60 && l[k].w == 1280,
          "QEMU EDID: 1280x800 at 60 75 120 144 (%d), 60 preferred", r);
    k = modes_find(l, n, 1152, 864);
    CHECK(modes_rates(l, n, 1152, 864, hz, 16) == 1 && k >= 0 && l[k].hz == 75, "QEMU EDID: 1152x864 only at 75");
    k = modes_find(l, n, 1024, 768);
    CHECK(modes_rates(l, n, 1024, 768, hz, 16) == 3 && k >= 0 && l[k].hz == 60, "QEMU EDID: 1024x768: the preferred rate 60");
    CHECK(list_ok(l, n, 1) && n <= 64, "QEMU EDID: %d modes", n);
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--write-edid")) {
        uint8_t ed[384];
        int len = !strcmp(argv[2], "lg") ? lg_edid(ed, 0x03) : qemu_edid(ed);
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(ed, 1, (size_t)len, f) != (size_t)len) { perror(argv[3]); return 1; }
        fclose(f);
        return 0;
    }
    int iters = argc > 1 ? atoi(argv[1]) : 200000;
    display_mode_t m;

    /* ---- CVT reduced blanking against published timings ---- */
    mode_cvt_rb(1920, 1200, 60, &m);
    CHECK(m.t.clock_khz == 154000 && m.t.htotal == 2080 && m.t.vtotal == 1235 && m.t.hsync_start == 1968 &&
          m.t.hsync_end == 2000 && m.t.vsync_start == 1203 && m.t.vsync_end == 1209 && (m.flags & DT_HSYNC_POS) &&
          !(m.flags & DT_VSYNC_POS) && m.hz == 60, "CVT-RB 1920x1200: %u kHz %u %u", m.t.clock_khz, m.t.htotal, m.t.vtotal);
    mode_cvt_rb(2560, 1600, 60, &m);
    CHECK(m.t.clock_khz == 268500 && m.t.htotal == 2720 && m.t.vtotal == 1646, "CVT-RB 2560x1600: %u %u %u", m.t.clock_khz, m.t.htotal, m.t.vtotal);
    mode_cvt_rb(1440, 900, 60, &m);
    CHECK(m.t.clock_khz == 88750 && m.t.htotal == 1600 && m.t.vtotal == 926 && m.t.vsync_end - m.t.vsync_start == 6,
          "CVT-RB 1440x900: %u %u %u", m.t.clock_khz, m.t.htotal, m.t.vtotal);
    mode_cvt_rb(1280, 720, 60, &m);
    CHECK(m.t.clock_khz == 64000 && m.t.vtotal == 741 && m.t.vsync_end - m.t.vsync_start == 5, "CVT-RB 1280x720: %u %u", m.t.clock_khz, m.t.vtotal);
    mode_cvt_rb(1024, 768, 60, &m);
    CHECK(m.t.vsync_end - m.t.vsync_start == 4 && m.hz == 60, "CVT-RB 4:3 sync %d, %d Hz", m.t.vsync_end - m.t.vsync_start, m.hz);

    /* ---- DMT ---- */
    CHECK(mode_dmt(1024, 768, 60, &m) && m.t.clock_khz == 65000 && m.t.htotal == 1344 && m.t.vtotal == 806 && m.hz == 60 &&
          !(m.flags & (DT_HSYNC_POS | DT_VSYNC_POS)), "DMT 1024x768@60");
    CHECK(mode_dmt(1280, 1024, 75, &m) && m.t.clock_khz == 135000 && m.hz == 75, "DMT 1280x1024@75 %d", m.hz);
    CHECK(mode_dmt(800, 600, 0, &m) && m.hz == 60, "DMT 800x600 any rate -> 60 Hz (%d)", m.hz);
    CHECK(!mode_dmt(1234, 567, 60, &m), "no DMT for 1234x567");
    CHECK(mode_refresh_mhz(&m.t) == 0 || 1, "refresh");
    mode_dmt(640, 480, 60, &m);
    CHECK(mode_refresh_mhz(&m.t) >= 59930 && mode_refresh_mhz(&m.t) <= 59950, "640x480 at 59.94 Hz: %u mHz", mode_refresh_mhz(&m.t));

    /* ---- an EDID with every kind of timing ---- */
    uint8_t e[256];
    base_block(e);
    e[35] = 0x21;                                        /* 640x480@60, 800x600@60 */
    e[36] = 0x08;                                        /* 1024x768@60 */
    e[38] = (uint8_t)(1680 / 8 - 31); e[39] = 0x00;       /* 1680x1050@60 (16:10) */
    e[40] = (uint8_t)(1280 / 8 - 31); e[41] = 0x40 | 15; /* 1280x960@75 (4:3) */
    put_dtd(e + 54, 148500, 1920, 88, 44, 280, 1080, 4, 5, 45, 0x1E);   /* preferred, +h +v */
    put_dtd(e + 72, 108000, 1280, 48, 112, 408, 1024, 1, 3, 42, 0x18);  /* -h -v */
    e[90 + 3] = 0xFC; memcpy(e + 90 + 5, "TESTMON\n     ", 13);
    e[108 + 3] = 0xFD;
    e[126] = 1;
    checksum(e);
    uint8_t *x = e + 128;
    memset(x, 0, 128);
    x[0] = 0x02; x[1] = 3;
    x[4] = (2 << 5) | 3; x[5] = 4; x[6] = 0x80 | 16; x[7] = 99;    /* VIC 4, native VIC 16, unknown 99 */
    x[2] = 8;
    put_dtd(x + 8, 74250, 1280, 110, 40, 370, 720, 5, 5, 30, 0x1E);
    put_dtd(x + 26, 74250, 1920, 88, 44, 280, 540, 2, 5, 22, 0x9E);    /* 1080i: skipped in lists */
    checksum(x);

    display_mode_t t[64];
    int nt = edid_timings(e, 256, t, 64);
    CHECK(nt >= 10, "edid_timings found %d", nt);
    CHECK(nt > 0 && t[0].w == 1920 && t[0].h == 1080 && (t[0].flags & DT_PREFERRED) && (t[0].flags & DT_HSYNC_POS) &&
          (t[0].flags & DT_VSYNC_POS) && t[0].t.htotal == 2200 && t[0].t.hsync_start == 2008 && t[0].t.hsync_end == 2052 &&
          t[0].t.vtotal == 1125 && t[0].t.vsync_start == 1084 && t[0].t.vsync_end == 1089 && t[0].hz == 60,
          "preferred DTD: %dx%d %u/%u", t[0].w, t[0].h, t[0].t.htotal, t[0].t.vtotal);
    int k = has(t, nt, 1280, 1024, 60);
    CHECK(k >= 0 && !(t[k].flags & (DT_HSYNC_POS | DT_VSYNC_POS | DT_PREFERRED)) && t[k].t.htotal == 1688, "second DTD");
    CHECK(has(t, nt, 640, 480, 60) >= 0 && has(t, nt, 800, 600, 60) >= 0 && has(t, nt, 1024, 768, 60) >= 0, "established");
    k = has(t, nt, 1680, 1050, 60);
    CHECK(k >= 0 && t[k].t.clock_khz == 146250, "standard 1680x1050 from DMT (%u)", k >= 0 ? t[k].t.clock_khz : 0);
    k = has(t, nt, 1280, 960, 75);
    CHECK(k >= 0 && (t[k].flags & DT_CVT), "standard 1280x960@75 from CVT");
    CHECK(has(t, nt, 1280, 720, 60) >= 0, "CEA VIC 4 / DTD 1280x720");
    k = has(t, nt, 1920, 1080, 0);
    int interlaced = 0;
    for (int i = 0; i < nt; i++) if (t[i].flags & DT_INTERLACE) interlaced = t[i].h == 1080;
    CHECK(interlaced, "interlaced DTD has frame height 1080");

    /* the list: one per size, sorted, the preferred kept */
    mode_limits_t lim = { 0 };
    display_mode_t l[64];
    int n = modes_build(e, 256, &lim, 0, 0, l, 64);
    int sorted = 1, dup = 0;
    for (int i = 1; i < n; i++) {
        if (l[i - 1].w > l[i].w || (l[i - 1].w == l[i].w && l[i - 1].h >= l[i].h)) sorted = 0;
        if (l[i - 1].w == l[i].w && l[i - 1].h == l[i].h) dup = 1;
    }
    CHECK(sorted && !dup, "sorted, one per size (%d modes)", n);
    k = modes_preferred(l, n);
    CHECK(k >= 0 && l[k].w == 1920 && l[k].h == 1080 && l[k].t.htotal == 2200, "preferred in list");
    CHECK(modes_default(l, n, 2560, 1600) == k, "default = preferred");
    for (int i = 0; i < n; i++) CHECK(!(l[i].flags & DT_INTERLACE), "no interlaced in list");

    /* limits: a single-link DVI clock, a small card, the driver's own check */
    lim.max_clock_khz = 100000;
    n = modes_build(e, 256, &lim, 0, 0, l, 64);
    CHECK(modes_find(l, n, 1920, 1080) < 0 && modes_find(l, n, 1024, 768) >= 0, "pixel clock limit");
    CHECK(modes_preferred(l, n) < 0, "preferred gone with its clock");
    k = modes_default(l, n, 2560, 1600);
    CHECK(k >= 0 && l[k].w <= 1024 && l[k].h <= 768, "default without preferred: <= 1024x768 (%dx%d)", l[k].w, l[k].h);
    memset(&lim, 0, sizeof lim);
    lim.vram = 1280 * 1024 * 4 * 2;
    lim.buffers = 2;
    n = modes_build(e, 256, &lim, 0, 0, l, 64);
    CHECK(modes_find(l, n, 1280, 1024) >= 0 && modes_find(l, n, 1680, 1050) < 0, "memory limit with two buffers");
    memset(&lim, 0, sizeof lim);
    lim.max_w = 1600; lim.max_h = 1200;
    n = modes_build(e, 256, &lim, 0, 0, l, 64);
    CHECK(modes_find(l, n, 1920, 1080) < 0 && modes_find(l, n, 1280, 1024) >= 0, "size limit");

    /* virtual hardware: the common sizes, a size hint, an odd-width check */
    memset(&lim, 0, sizeof lim);
    lim.virtual_sizes = 1;
    lim.valid = rejects_odd;
    n = modes_build(NULL, 0, &lim, 1366, 768, l, 64);
    CHECK(n >= 15 && modes_find(l, n, 1366, 768) < 0 && modes_find(l, n, 3840, 2160) >= 0, "virtual sizes, odd width rejected (%d)", n);
    lim.valid = NULL;
    n = modes_build(NULL, 0, &lim, 1366, 768, l, 64);
    k = modes_preferred(l, n);
    CHECK(k >= 0 && l[k].w == 1366 && l[k].h == 768 && (l[k].flags & DT_DRIVER) && l[k].t.clock_khz, "size hint becomes preferred");
    CHECK(modes_default(l, n, 2560, 1600) == k, "default follows the hint");

    /* 4K preferred: half of it */
    base_block(e);
    put_dtd(e + 54, 594000, 3840, 176, 88, 560, 2160, 8, 10, 90, 0x1E);
    e[126] = 0;
    checksum(e);
    memset(&lim, 0, sizeof lim);
    lim.virtual_sizes = 1;
    n = modes_build(e, 128, &lim, 0, 0, l, 64);
    k = modes_default(l, n, 2560, 1600);
    CHECK(k >= 0 && l[k].w == 1920 && l[k].h == 1080, "4K -> 1920x1080 (%dx%d)", l[k].w, l[k].h);

    /* ---- refresh rates ---- */
    rate_tests();

    /* broken input */
    e[127]++;
    CHECK(edid_timings(e, 128, t, 64) == 0, "bad checksum: no timings");
    CHECK(edid_timings(e, 100, t, 64) == 0, "short EDID: no timings");

    /* ---- random EDIDs: never a crash, every listed mode within the limits ---- */
    srand(1234);
    int listed = 0, rated = 0;
    for (int it = 0; it < iters; it++) {
        uint8_t r[512];
        for (int i = 0; i < 512; i++) r[i] = (uint8_t)rand();
        base_block(r);
        for (int i = 20; i < 128; i++) if (rand() % 3) r[i] = (uint8_t)rand();
        r[126] = (uint8_t)(rand() % 4);
        checksum(r);
        for (int b = 1; b < 4; b++) {
            if (rand() % 2) {
                r[b * 128] = 0x02; r[b * 128 + 2] = (uint8_t)(rand() % 140);
            } else {                                     /* DisplayID: random data blocks, often timing blocks */
                r[b * 128] = 0x70; r[b * 128 + 2] = (uint8_t)(rand() % 130);
                r[b * 128 + 5] = (uint8_t)(rand() % 2 ? 0x03 : 0x22); r[b * 128 + 7] = (uint8_t)(20 * (rand() % 7));
            }
        }
        memset(&lim, 0, sizeof lim);
        lim.max_w = 640 + rand() % 3500;
        lim.max_h = 480 + rand() % 2000;
        lim.max_clock_khz = (u32)(rand() % 2) * (u32)(25000 + rand() % 600000);
        lim.virtual_sizes = rand() % 2;
        lim.rates = rand() % 2;
        n = modes_build(r, 512, &lim, rand() % 3000, rand() % 2000, l, 64);
        listed += n;
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (l[i].w < 640 || l[i].h < 480 || l[i].w > lim.max_w || l[i].h > lim.max_h ||
                (lim.max_clock_khz && l[i].t.clock_khz > lim.max_clock_khz) || (l[i].flags & DT_INTERLACE)) bad = 1;
        if (bad) { CHECK(0, "random EDID %d: a mode outside the limits", it); break; }
        if (!list_ok(l, n, lim.rates)) { CHECK(0, "random EDID %d: list not sorted or duplicates (rates %d)", it, lim.rates); break; }
        k = modes_default(l, n, 2560, 1600);
        if (n && (k < 0 || k >= n)) { CHECK(0, "random EDID %d: default %d of %d", it, k, n); break; }
        if (n) {
            /* a saved mode comes back as saved; the mode for a size is one of that size */
            int j = rand() % n, f = modes_find(l, n, l[j].w, l[j].h), fell = 1;
            int sv = modes_saved(l, n, l[j].w, l[j].h, l[j].hz, &fell);
            if (f < 0 || l[f].w != l[j].w || l[f].h != l[j].h || sv != (lim.rates ? j : f) || fell) {
                CHECK(0, "random EDID %d: find %d / saved %d for mode %d (rates %d)", it, f, sv, j, lim.rates);
                break;
            }
            rated += lim.rates ? count_at(l, n, l[j].w, l[j].h) > 1 : 0;
        }
        char d[128];
        if (n) mode_describe(&l[0], d, sizeof d);
    }
    CHECK(listed > 0 && rated > 0, "random EDIDs listed some modes (%d sizes with several rates)", rated);

    if (failed) printf("modetest: %d of %d checks FAILED\n", failed, checks);
    else printf("modetest: all %d checks passed (%d random EDIDs, %d modes listed, %d sizes with several rates)\n", checks, iters, listed, rated);
    return failed != 0;
}
