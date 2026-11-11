/* Host test: EDID parsing (kernel/drivers/edid.c) and the loaders' video mode
 * choice (boot/modesel.h). */
#include <stdio.h>
#include "core/lib.h"
#include "../../boot/modesel.h"
#include "drivers/edid.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("displaytest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* an EDID base block with a preferred timing of w x h at ~60 Hz */
static void make_edid(uint8_t *e, int w, int h, const char *name) {
    memset(e, 0, 128);
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(e, hdr, 8);
    e[8] = 0x10; e[9] = 0xAC;                    /* "DEL" */
    e[10] = 0x34; e[11] = 0x12;
    e[17] = 30;                                  /* 2020 */
    e[18] = 1; e[19] = 4;
    uint8_t *d = e + 54;
    int hb = 160, vb = 30;
    unsigned clk = (unsigned)((w + hb) * (h + vb) * 60 / 10000);
    d[0] = clk & 0xFF; d[1] = clk >> 8;
    d[2] = w & 0xFF; d[3] = hb & 0xFF; d[4] = (uint8_t)(((w >> 8) << 4) | (hb >> 8));
    d[5] = h & 0xFF; d[6] = vb & 0xFF; d[7] = (uint8_t)(((h >> 8) << 4) | (vb >> 8));
    d[12] = 0x08; d[13] = 0x4F; d[14] = 0x21;    /* 520 x 335 mm */
    uint8_t *n = e + 72;
    n[3] = 0xFC;
    int i = 0;
    for (; name[i] && i < 13; i++) n[5 + i] = (uint8_t)name[i];
    if (i < 13) { n[5 + i++] = 0x0A; while (i < 13) n[5 + i++] = ' '; }
    uint8_t sum = 0;
    for (int k = 0; k < 127; k++) sum = (uint8_t)(sum + e[k]);
    e[127] = (uint8_t)(0x100 - sum);
}

static void set_modes(struct bootinfo *bi, const int (*m)[2], int n) {
    memset(bi, 0, sizeof *bi);
    for (int i = 0; i < n; i++) {
        bi->modes[i].width = (uint16_t)m[i][0]; bi->modes[i].height = (uint16_t)m[i][1];
        bi->modes[i].bpp = 32; bi->modes[i].id = (uint16_t)(100 + i);
    }
    bi->mode_count = (uint32_t)n;
}

static const char *picked(const struct bootinfo *bi, int i) {
    static char b[32];
    if (i < 0) return "default";
    snprintf(b, sizeof b, "%dx%d", bi->modes[i].width, bi->modes[i].height);
    return b;
}

int main(void) {
    uint8_t e[128];
    edid_info_t ei;
    make_edid(e, 1920, 1200, "U2412M");
    CHECK(edid_parse(e, 128, &ei) && ei.native_w == 1920 && ei.native_h == 1200, "parse native %dx%d", ei.native_w, ei.native_h);
    CHECK(!strcmp(ei.vendor, "DEL") && ei.product == 0x1234 && ei.year == 2020, "vendor %s %04X %d", ei.vendor, ei.product, ei.year);
    CHECK(!strcmp(ei.name, "U2412M"), "name '%s'", ei.name);
    CHECK(ei.native_hz >= 59 && ei.native_hz <= 61, "refresh %d", ei.native_hz);
    CHECK(ei.width_mm == 520 && ei.height_mm == 335, "size %dx%d", ei.width_mm, ei.height_mm);
    e[127]++;
    CHECK(!edid_parse(e, 128, &ei), "bad checksum rejected");
    e[127]--; e[0] = 1;
    CHECK(!edid_parse(e, 128, &ei), "bad header rejected");
    CHECK(!edid_parse(e, 64, &ei), "short EDID rejected");

    static const int std[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 1920, 1080 },
                                  { 1920, 1200 }, { 2560, 1440 }, { 3840, 2160 } };
    struct bootinfo bi;
    const char *ini = "# comment\r\ndisplay.width = 1280\r\nDISPLAY.HEIGHT=1024\r\nother=5\r\n";

    set_modes(&bi, std, 8);
    CHECK(modesel_choose(&bi, NULL, 0) == -1 && bi.mode_reason == MODE_BY_DEFAULT, "no EDID, no setting -> default");

    set_modes(&bi, std, 8);
    make_edid(bi.edid, 1920, 1200, "A"); bi.edid_size = 128;
    int i = modesel_choose(&bi, NULL, 0);
    CHECK(!strcmp(picked(&bi, i), "1920x1200") && bi.mode_reason == MODE_BY_EDID, "native 1920x1200 -> %s", picked(&bi, i));

    i = modesel_choose(&bi, ini, (unsigned)strlen(ini));
    CHECK(!strcmp(picked(&bi, i), "1280x1024") && bi.mode_reason == MODE_BY_SETTING, "setting wins -> %s", picked(&bi, i));

    bi.boot_flags = BOOTF_SAFE_VIDEO;
    i = modesel_choose(&bi, ini, (unsigned)strlen(ini));
    CHECK(!strcmp(picked(&bi, i), "800x600") && bi.mode_reason == MODE_BY_SAFE, "safe video -> %s", picked(&bi, i));
    bi.boot_flags = 0;

    const char *ini2 = "display.width=1366\ndisplay.height=768\n";      /* not offered */
    i = modesel_choose(&bi, ini2, (unsigned)strlen(ini2));
    CHECK(!strcmp(picked(&bi, i), "1920x1200"), "unavailable setting -> native, got %s", picked(&bi, i));

    set_modes(&bi, std, 8);
    make_edid(bi.edid, 3840, 2160, "4K"); bi.edid_size = 128;
    i = modesel_choose(&bi, NULL, 0);
    CHECK(!strcmp(picked(&bi, i), "1920x1080"), "4K panel -> half its size (sharp), got %s", picked(&bi, i));

    static const int no1080[][2] = { { 1024, 768 }, { 1600, 900 }, { 2560, 1440 }, { 3840, 2160 } };
    set_modes(&bi, no1080, 4);
    make_edid(bi.edid, 3840, 2160, "4K"); bi.edid_size = 128;
    i = modesel_choose(&bi, NULL, 0);
    CHECK(!strcmp(picked(&bi, i), "2560x1440"), "4K panel without 1080p -> largest 16:9, got %s", picked(&bi, i));

    static const int few[][2] = { { 800, 600 }, { 1024, 768 }, { 1920, 1080 }, { 3840, 2160 } };
    set_modes(&bi, few, 4);
    make_edid(bi.edid, 3840, 2160, "4K"); bi.edid_size = 128;
    i = modesel_choose(&bi, NULL, 0);
    CHECK(!strcmp(picked(&bi, i), "1920x1080"), "4K panel, few modes -> %s", picked(&bi, i));

    set_modes(&bi, few, 4);
    make_edid(bi.edid, 1366, 768, "laptop"); bi.edid_size = 128;
    i = modesel_choose(&bi, NULL, 0);
    CHECK(i == -1 && bi.mode_reason == MODE_BY_DEFAULT, "native not offered, no same shape -> default, got %s", picked(&bi, i));

    CHECK(modesel_setting("a=1\nb = 22 \n", 11, "b") == 22, "setting parse");
    CHECK(modesel_setting("display.widthx=5\n", 17, "display.width") == -1, "no prefix match");

    if (failed) printf("displaytest: %d of %d checks FAILED\n", failed, checks);
    else printf("displaytest: all %d checks passed\n", checks);
    return failed != 0;
}
