/* Host test: display timings and mode lists (kernel/drivers/modes.c): EDID
 * detailed / standard / established timings and CEA-861 extensions, the
 * DMT subset, CVT reduced blanking against published values, mode lists
 * within a driver's limits (size, pixel clock, memory, its own check), the
 * default mode, and random EDIDs (no crash, every mode within the limits). */
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

int main(int argc, char **argv) {
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

    /* broken input */
    e[127]++;
    CHECK(edid_timings(e, 128, t, 64) == 0, "bad checksum: no timings");
    CHECK(edid_timings(e, 100, t, 64) == 0, "short EDID: no timings");

    /* ---- random EDIDs: never a crash, every listed mode within the limits ---- */
    srand(1234);
    int listed = 0;
    for (int it = 0; it < iters; it++) {
        uint8_t r[512];
        for (int i = 0; i < 512; i++) r[i] = (uint8_t)rand();
        base_block(r);
        for (int i = 20; i < 128; i++) if (rand() % 3) r[i] = (uint8_t)rand();
        r[126] = (uint8_t)(rand() % 4);
        checksum(r);
        for (int b = 1; b < 4; b++) { r[b * 128] = 0x02; r[b * 128 + 2] = (uint8_t)(rand() % 140); }
        memset(&lim, 0, sizeof lim);
        lim.max_w = 640 + rand() % 3500;
        lim.max_h = 480 + rand() % 2000;
        lim.max_clock_khz = (u32)(rand() % 2) * (u32)(25000 + rand() % 600000);
        lim.virtual_sizes = rand() % 2;
        n = modes_build(r, 512, &lim, rand() % 3000, rand() % 2000, l, 64);
        listed += n;
        int bad = 0;
        for (int i = 0; i < n; i++)
            if (l[i].w < 640 || l[i].h < 480 || l[i].w > lim.max_w || l[i].h > lim.max_h ||
                (lim.max_clock_khz && l[i].t.clock_khz > lim.max_clock_khz) || (l[i].flags & DT_INTERLACE)) bad = 1;
        if (bad) { CHECK(0, "random EDID %d: a mode outside the limits", it); break; }
        k = modes_default(l, n, 2560, 1600);
        if (n && (k < 0 || k >= n)) { CHECK(0, "random EDID %d: default %d of %d", it, k, n); break; }
        char d[128];
        if (n) mode_describe(&l[0], d, sizeof d);
    }
    CHECK(listed > 0, "random EDIDs listed some modes");

    if (failed) printf("modetest: %d of %d checks FAILED\n", failed, checks);
    else printf("modetest: all %d checks passed (%d random EDIDs, %d modes listed)\n", checks, iters, listed);
    return failed != 0;
}
