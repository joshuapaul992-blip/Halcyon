/* Halcyon OS - EDID base block (VESA E-EDID 1.3/1.4) */
#include "edid.h"
#include "core/lib.h"

int edid_parse(const u8 *e, int size, edid_info_t *o) {
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memset(o, 0, sizeof *o);
    if (!e || size < 128 || memcmp(e, hdr, 8)) return 0;
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum = (u8)(sum + e[i]);
    if (sum) return 0;
    u16 m = (u16)(e[8] << 8 | e[9]);
    o->vendor[0] = (char)('A' - 1 + ((m >> 10) & 31));
    o->vendor[1] = (char)('A' - 1 + ((m >> 5) & 31));
    o->vendor[2] = (char)('A' - 1 + (m & 31));
    for (int i = 0; i < 3; i++) if (o->vendor[i] < 'A' || o->vendor[i] > 'Z') o->vendor[i] = '?';
    o->product = (u16)(e[10] | e[11] << 8);
    o->year = e[17] ? 1990 + e[17] : 0;
    for (int d = 0; d < 4; d++) {
        const u8 *p = e + 54 + d * 18;
        if (p[0] || p[1]) {
            if (d == 0) {
                /* detailed timing: pixel clock in 10 kHz units */
                u32 clk = (u32)(p[0] | p[1] << 8) * 10000u;
                int ha = p[2] | (p[4] & 0xF0) << 4, hb = p[3] | (p[4] & 0x0F) << 8;
                int va = p[5] | (p[7] & 0xF0) << 4, vb = p[6] | (p[7] & 0x0F) << 8;
                o->native_w = ha;
                o->native_h = va;
                if (p[17] & 0x80) o->native_h *= 2;          /* interlaced: field height */
                u32 tot = (u32)(ha + hb) * (u32)(va + vb);
                o->native_hz = tot ? (int)((clk + tot / 2) / tot) : 0;
                o->width_mm = p[12] | (p[14] & 0xF0) << 4;
                o->height_mm = p[13] | (p[14] & 0x0F) << 8;
            }
            continue;
        }
        if (p[3] == 0xFC) {                                 /* monitor name */
            int n = 0;
            for (int i = 5; i < 18 && p[i] != 0x0A && n < 13; i++) o->name[n++] = (char)(p[i] >= 32 && p[i] < 127 ? p[i] : '?');
            while (n > 0 && o->name[n - 1] == ' ') n--;
            o->name[n] = 0;
        }
    }
    o->valid = 1;
    return 1;
}

void edid_describe(const edid_info_t *i, char *out, int n) {
    if (!i->valid) { strlcpy(out, "Default Monitor", (size_t)n); return; }
    if (i->name[0]) snprintf(out, (size_t)n, "%s", i->name);
    else snprintf(out, (size_t)n, "Plug and Play Monitor (%s%04X)", i->vendor, i->product);
}

/* ---------------- detailed timings of every block ---------------- */

/* an 18-byte detailed timing descriptor; 0 if it is none */
static int dtd(const u8 *p, edid_timing_t *t, int block) {
    u32 clk = (u32)(p[0] | p[1] << 8) * 10;         /* kHz */
    if (!clk) return 0;
    memset(t, 0, sizeof *t);
    t->clock_khz = clk;
    t->w = p[2] | (p[4] & 0xF0) << 4;
    t->hblank = p[3] | (p[4] & 0x0F) << 8;
    t->h = p[5] | (p[7] & 0xF0) << 4;
    t->vblank = p[6] | (p[7] & 0x0F) << 8;
    t->hfront = p[8] | (p[11] & 0xC0) << 2;
    t->hsync = p[9] | (p[11] & 0x30) << 4;
    t->vfront = (p[10] >> 4) | (p[11] & 0x0C) << 2;
    t->vsync = (p[10] & 0x0F) | (p[11] & 0x03) << 4;
    t->interlaced = (p[17] & 0x80) != 0;
    u64 tot = (u64)(t->w + t->hblank) * (u64)(t->h + t->vblank);
    t->hz = tot ? (int)(((u64)clk * 1000 + tot / 2) / tot) : 0;
    if (t->interlaced) { t->h *= 2; t->hz *= 2; }
    t->block = block;
    return t->w > 0 && t->h > 0;
}

/* DisplayID type I (clock in 10 kHz) and type VII (1 kHz) timings */
static int did_timing(const u8 *p, edid_timing_t *t, int block, int khz_units) {
    u32 clk = (u32)(p[0] | p[1] << 8 | p[2] << 16) + 1;
    memset(t, 0, sizeof *t);
    t->clock_khz = khz_units ? clk : clk * 10;
    t->preferred = (p[3] & 0x80) != 0;
    t->interlaced = (p[3] & 0x10) != 0;
    t->w = (p[4] | p[5] << 8) + 1;
    t->hblank = (p[6] | p[7] << 8) + 1;
    t->hfront = ((p[8] | p[9] << 8) & 0x7FFF) + 1;
    t->hsync = (p[10] | p[11] << 8) + 1;
    t->h = (p[12] | p[13] << 8) + 1;
    t->vblank = (p[14] | p[15] << 8) + 1;
    t->vfront = ((p[16] | p[17] << 8) & 0x7FFF) + 1;
    t->vsync = (p[18] | p[19] << 8) + 1;
    u64 tot = (u64)(t->w + t->hblank) * (u64)(t->h + t->vblank);
    t->hz = tot ? (int)(((u64)t->clock_khz * 1000 + tot / 2) / tot) : 0;
    t->block = block;
    return 1;
}

int edid_block_count(const u8 *e, int size) {
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!e || size < 128 || memcmp(e, hdr, 8)) return 0;
    int n = 1 + e[126];
    return MIN(n, size / 128);
}

int edid_detailed_timings(const u8 *e, int size, edid_timing_t *out, int max) {
    int blocks = edid_block_count(e, size), n = 0;
    for (int b = 0; b < blocks && n < max; b++) {
        const u8 *p = e + b * 128;
        if (b == 0) {
            for (int d = 0; d < 4 && n < max; d++)
                if (dtd(p + 54 + d * 18, &out[n], 0)) { out[n].preferred = n == 0; n++; }
        } else if (p[0] == 0x02) {                  /* CEA-861: DTDs from byte p[2] on */
            int off = p[2];
            if (off < 4 || off > 127) continue;
            for (; off + 18 <= 127 && n < max; off += 18) {
                if (!dtd(p + off, &out[n], b)) break;
                n++;
            }
        } else if (p[0] == 0x70) {                  /* DisplayID section */
            int end = MIN(5 + p[2], 127);
            for (int o = 5; o + 3 <= end && n < max;) {
                int tag = p[o], blen = p[o + 2];
                if (o + 3 + blen > end || (!tag && !blen)) break;
                if ((tag == 0x03 || tag == 0x22) && blen % 20 == 0)
                    for (int k = 0; k < blen / 20 && n < max; k++)
                        if (did_timing(p + o + 3 + k * 20, &out[n], b, tag == 0x22)) n++;
                o += 3 + blen;
            }
        }
    }
    return n;
}

const char *edid_ext_name(u8 tag) {
    switch (tag) {
    case 0x02: return "CEA-861 (HDMI / audio)";
    case 0x10: return "video timing block";
    case 0x40: return "display information";
    case 0x50: return "localized string";
    case 0x60: return "digital packet video link";
    case 0x70: return "DisplayID";
    case 0xF0: return "block map";
    case 0xFF: return "manufacturer";
    }
    return "unknown";
}

void edid_timing_str(const edid_timing_t *t, char *out, int n) {
    snprintf(out, (size_t)n, "%dx%d%s at %d Hz (%u.%02u MHz)", t->w, t->h, t->interlaced ? "i" : "", t->hz,
             t->clock_khz / 1000, (t->clock_khz % 1000) / 10);
}
