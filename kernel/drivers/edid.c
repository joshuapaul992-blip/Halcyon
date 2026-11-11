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
