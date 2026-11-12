/* Halcyon OS - NVIDIA video BIOS: the PCI image chain (with NVIDIA's IFR
 * header and NPDE extension), BIT and BMP, and the Device Control Block
 * (DCB 1.5 / 2.x / 3.0 / 4.x) with its I2C / CCB, connector and GPIO tables.
 *
 * Written from NVIDIA's open-gpu-doc (BIOS-Information-Table, DCB 4.x
 * specification), the kernel's nova-core VBIOS notes (IFR header) and, for
 * the pre-4.0 layouts, nouveau's nouveau_bios.c and nvkm/subdev/bios
 * (Copyright 2012 Red Hat Inc., MIT; see THIRD_PARTY_NOTICES.md).
 * Every read is bounds checked (nv_rom_rd*); a malformed table is noted in
 * nv_bios_t.problems and parsing goes on with what is sane. Runs on the
 * host too (tests/host/nvtest.c, fuzz_nv.c). */
#include "nvhw.h"
#include "core/lib.h"

/* ---------------- raw access ---------------- */

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u32 raw16(const u8 *d, u32 size, u32 a) { return a <= size && size - a >= 2 ? le16(d + a) : 0; }
static u32 raw32(const u8 *d, u32 size, u32 a) { return a <= size && size - a >= 4 ? le32(d + a) : 0; }
static u32 raw8(const u8 *d, u32 size, u32 a) { return a < size ? d[a] : 0; }

#define SIG_NVGI 0x4947564Eu    /* "NVGI" */
#define SIG_RFRD 0x44524652u    /* "RFRD" */
#define SIG_PCIR 0x52494350u    /* "PCIR" */
#define SIG_NPDS 0x5344504Eu    /* "NPDS" */
#define SIG_RGIS 0x53494752u    /* "RGIS" */
#define SIG_NPDE 0x4544504Eu    /* "NPDE" */

/* where the PCIR structure of an image at base is, or 0 */
static u32 pcir_at(const u8 *d, u32 size, u32 base) {
    u32 p = raw16(d, size, base + 0x18);
    if (!p || p < 0x1A) return 0;
    p += base;
    u32 sig = raw32(d, size, p);
    if (sig != SIG_PCIR && sig != SIG_NPDS && sig != SIG_RGIS) return 0;
    if (p > size || size - p < 0x18) return 0;
    return p;
}

static int rom_sig_ok(u32 sig, int first) {
    return sig == 0xAA55 || (!first && (sig == 0xBB77 || sig == 0x4E56));
}

/* The start of the PCI ROM in raw bytes: 0, the offset an IFR header gives,
 * or the first 512-byte boundary with 0x55AA and a PCIR structure. */
static int find_start(const u8 *raw, u32 size, u32 *start, u32 *ifr_ver) {
    *ifr_ver = 0;
    if (raw16(raw, size, 0) == 0xAA55 && pcir_at(raw, size, 0)) { *start = 0; return 1; }
    if (raw32(raw, size, 0) == SIG_NVGI) {
        u32 fixed1 = raw32(raw, size, 4), ver = (fixed1 >> 8) & 0xFF, off = 0;
        *ifr_ver = ver ? ver : 0xFF;
        if (ver == 1 || ver == 2) {
            u32 data = (fixed1 >> 16) & 0x7FFF;
            off = raw32(raw, size, data + 4);
        } else if (ver == 3) {
            u32 total = raw32(raw, size, 8) & 0xFFFFF;
            u32 dir = raw32(raw, size, total);
            if (dir < size && size - dir > 4096) {
                dir += 4096;
                if (raw32(raw, size, dir) == SIG_RFRD) off = raw32(raw, size, dir + 8);
            }
        }
        if (off && !(off & 3) && off < size && raw16(raw, size, off) == 0xAA55 && pcir_at(raw, size, off)) {
            *start = off;
            return 1;
        }
    }
    /* "the start of the PCI Expansion ROM can be found by checking 512 byte
     * boundaries" (BIT specification) */
    for (u32 o = 512; o + 0x1A < size; o += 512)
        if (raw16(raw, size, o) == 0xAA55 && pcir_at(raw, size, o)) { *start = o; return 1; }
    return 0;
}

int nv_rom_scan(const u8 *raw, u32 rawsize, nv_rom_t *r) {
    memset(r, 0, sizeof *r);
    if (!raw || rawsize < 0x20) { strlcpy(r->why, "too small", sizeof r->why); return 0; }
    if (rawsize > NV_ROM_MAX) rawsize = NV_ROM_MAX;
    u32 start;
    if (!find_start(raw, rawsize, &start, &r->ifr_version)) {
        strlcpy(r->why, r->ifr_version ? "IFR header points to no ROM image" : "no 55AA / PCIR signature", sizeof r->why);
        return 0;
    }
    r->ifr_offset = start;
    r->data = raw + start;
    r->size = rawsize - start;
    const u8 *d = r->data;
    u32 size = r->size, base = 0;
    int complete = 0, score = 1;
    while (r->nimages < NV_ROM_IMAGES) {
        u32 sig = raw16(d, size, base);
        u32 p = rom_sig_ok(sig, r->nimages == 0) ? pcir_at(d, size, base) : 0;
        if (!p) {
            if (!r->nimages) strlcpy(r->why, "no PCIR structure", sizeof r->why);
            break;
        }
        nv_rom_image_t *im = &r->img[r->nimages];
        im->offset = base;
        im->vendor = (u16)raw16(d, size, p + 4);
        im->device = (u16)raw16(d, size, p + 6);
        im->pcir_rev = (u8)raw8(d, size, p + 0x0C);
        im->size = raw16(d, size, p + 0x10) * 512;
        im->type = (u8)raw8(d, size, p + 0x14);
        im->last = (raw8(d, size, p + 0x15) & 0x80) != 0;
        if (im->type != 0x70) {
            /* NPDE (NVIDIA's PCI data extension) right after PCIR, 16-byte aligned:
             * its size and "last" win (the legacy image says "last" for PC BIOSes) */
            u32 hdr = raw16(d, size, p + 0x0A);
            u32 n = (p + hdr + 0x0F) & ~0x0Fu;
            if (hdr && n < size && raw32(d, size, n) == SIG_NPDE) {
                u32 sz = raw16(d, size, n + 8) * 512;
                if (sz) im->size = sz;
                im->last = (raw8(d, size, n + 0x0A) & 0x80) != 0;
                im->npde = 1;
            }
        } else {
            im->last = 1;
        }
        if (!im->size) {
            if (!r->nimages) strlcpy(r->why, "image size 0", sizeof r->why);
            break;
        }
        r->nimages++;
        if (im->size > size - base) {               /* cut short: keep what is there */
            im->size = size - base;
            if (r->nimages == 1) strlcpy(r->why, "legacy image cut short", sizeof r->why);
            im->checksum_ok = 0;
            r->chain_size = size;
            break;
        }
        im->checksum_ok = 1;
        if (im->type == 0x00) {
            u8 sum = 0;
            for (u32 i = 0; i < im->size; i++) sum = (u8)(sum + d[base + i]);
            im->checksum_ok = sum == 0;
        }
        score += im->type == 0x00 ? (im->checksum_ok ? 3 : 1) : 3;
        base += im->size;
        r->chain_size = base;
        if (im->last) { complete = 1; break; }
        if (base >= size) break;
    }
    if (!r->nimages) return 0;
    if (r->img[0].vendor != 0x10DE) {
        snprintf(r->why, sizeof r->why, "PCIR vendor %04x is not NVIDIA", r->img[0].vendor);
        return 0;
    }
    if (r->img[0].type == 0x00) r->legacy_size = r->img[0].size;
    for (int i = 1; i < r->nimages; i++)
        if (r->img[i].type == 0xE0) { r->ext_base = r->img[i].offset; break; }
    if (!r->ext_base && r->nimages >= 3 && r->img[1].type == 0x03) r->ext_base = r->img[2].offset;
    r->complete = (u8)complete;
    if (complete) score += 1;
    else if (!r->why[0]) strlcpy(r->why, "image chain incomplete", sizeof r->why);
    r->score = score;
    return score;
}

/* BIT pointer rule: past the legacy image, pointers go into the 0xE0 data image */
static int xlate(const nv_rom_t *r, u32 *a, u32 n) {
    u32 v = *a;
    if (r->ext_base && r->legacy_size && v >= r->legacy_size) {
        v = v - r->legacy_size + r->ext_base;
        if (v < r->ext_base) return 0;              /* wrapped */
    }
    if (v > r->size || r->size - v < n) return 0;
    *a = v;
    return 1;
}

u8 nv_rom_rd8(const nv_rom_t *r, u32 a, u32 *oob) {
    if (!r->data || !xlate(r, &a, 1)) { if (oob) (*oob)++; return 0; }
    return r->data[a];
}
u16 nv_rom_rd16(const nv_rom_t *r, u32 a, u32 *oob) {
    if (!r->data || !xlate(r, &a, 2)) { if (oob) (*oob)++; return 0; }
    return (u16)le16(r->data + a);
}
u32 nv_rom_rd32(const nv_rom_t *r, u32 a, u32 *oob) {
    if (!r->data || !xlate(r, &a, 4)) { if (oob) (*oob)++; return 0; }
    return le32(r->data + a);
}

/* ---------------- parsing ---------------- */

#define RD8(a)  nv_rom_rd8(&b->rom, (a), &b->oob)
#define RD16(a) nv_rom_rd16(&b->rom, (a), &b->oob)
#define RD32(a) nv_rom_rd32(&b->rom, (a), &b->oob)

/* signature search inside [0, limit) of the ROM (no pointer translation) */
static u32 find(const nv_rom_t *r, u32 limit, const char *s, u32 n) {
    if (limit > r->size) limit = r->size;
    for (u32 i = 0; i + n <= limit; i++)
        if (!memcmp(r->data + i, s, n)) return i;
    return 0xFFFFFFFFu;
}

static void parse_bit(nv_bios_t *b) {
    u32 o = b->bit_offset;
    u32 hsize = RD8(o + 8), tsize = RD8(o + 9), n = RD8(o + 10);
    b->bit_ver_major = RD8(o + 7);
    b->bit_ver_minor = RD8(o + 6);
    u8 sum = 0;
    for (u32 i = 0; i < hsize && i < 32; i++) sum = (u8)(sum + RD8(o + i));
    if (hsize < 12 || hsize > 32 || tsize < 6) {
        b->problems |= NVB_BAD_BIT;
        if (hsize < 12 || hsize > 32) hsize = 12;
        if (tsize < 6) return;
    } else if (sum) {
        b->problems |= NVB_BAD_BIT;                 /* noted; many images still parse */
    }
    for (u32 i = 0; i < n; i++) {
        u32 e = o + hsize + i * tsize;
        if (b->ntokens >= NV_BIT_MAX) { b->problems |= NVB_TRUNC; break; }
        nv_bit_token_t *t = &b->tok[b->ntokens];
        u32 before = b->oob;
        t->id = RD8(e);
        t->ver = RD8(e + 1);
        t->size = RD16(e + 2);
        t->ptr = RD16(e + 4);
        if (b->oob != before) { b->problems |= NVB_BAD_BIT; break; }
        if (t->id == 0 && t->ptr == 0) continue;
        b->ntokens++;
    }
}

const nv_bit_token_t *nv_bios_token(const nv_bios_t *b, u8 id) {
    for (int i = 0; i < b->ntokens; i++)
        if (b->tok[i].id == id && b->tok[i].ptr) return &b->tok[i];
    return NULL;
}

static void version_from(nv_bios_t *b, u32 at, int patch) {
    b->version[0] = RD8(at + 3);
    b->version[1] = RD8(at + 2);
    b->version[2] = RD8(at + 1);
    b->version[3] = RD8(at + 0);
    b->version[4] = patch >= 0 ? (u8)patch : 0;
    snprintf(b->version_str, sizeof b->version_str, "%02x.%02x.%02x.%02x.%02x", b->version[0], b->version[1],
             b->version[2], b->version[3], b->version[4]);
}

static void parse_dcb_entries(nv_bios_t *b) {
    u32 dcb = b->dcb_offset;
    int n = b->dcb_cnt;
    if (n > NV_DCB_MAX) { n = NV_DCB_MAX; b->problems |= NVB_TRUNC; }
    for (int i = 0; i < n; i++) {
        u32 e = dcb + b->dcb_hdr + (u32)i * b->dcb_len;
        u32 before = b->oob;
        u32 conn = RD32(e), conf = b->dcb_len >= 8 ? RD32(e + 4) : 0;
        if (b->oob != before) { b->problems |= NVB_BAD_DCB | NVB_OOB; break; }
        if (conn == 0x00000000 || conn == 0xFFFFFFFF) break;    /* NV11 DCB 1.5 / NV17 DCB 2.0 terminators */
        nv_dcb_output_t o;
        memset(&o, 0, sizeof o);
        o.index = (u8)i;
        o.raw[0] = conn;
        o.raw[1] = conf;
        if (b->dcb_ver >= 0x20) {
            o.type = conn & 0xF;
            if (o.type == NV_OUT_EOL) break;
            if (o.type == NV_OUT_SKIP) continue;
            o.i2c = (conn >> 4) & 0xF;
            o.heads = (conn >> 8) & 0xF;
            o.connector = (conn >> 12) & 0xF;
            o.bus = (conn >> 16) & 0xF;
            o.location = (conn >> 20) & 0x3;
            o.or_mask = (conn >> 24) & 0xF;
            switch (o.type) {
            case NV_OUT_TMDS: case NV_OUT_LVDS: case NV_OUT_DP: case NV_OUT_SDI:
                if (b->dcb_ver >= 0x40) {
                    o.edid_src = conf & 3;
                    o.link = (conf >> 4) & 3;
                    o.extdev = o.location ? (conf >> 8) & 0xFF : 0;
                    o.hdmi = (conf >> 17) & 1;
                } else if (b->dcb_ver >= 0x22 && o.type == NV_OUT_LVDS) {
                    o.link = (conf >> 4) & 3;
                    o.edid_src = (conf & 1) ? 1 : (conf & 2) ? 2 : 0;
                }
                if (o.type == NV_OUT_DP) {
                    o.link = (conf >> 4) & 3;
                    o.dp_max_rate = (conf >> 21) & 7;
                    switch ((conf >> 24) & 0xF) {
                    case 0xF: case 0x4: o.dp_max_lanes = 4; break;
                    case 0x3: case 0x2: o.dp_max_lanes = 2; break;
                    default: o.dp_max_lanes = 1; break;
                    }
                }
                break;
            }
        } else {                                    /* DCB 1.5 */
            switch (conn & 0xF) {
            case 0: o.type = NV_OUT_CRT; break;
            case 1: o.type = NV_OUT_TV; break;
            case 2: case 4: o.type = (conn & 0x10) ? NV_OUT_LVDS : NV_OUT_TMDS; break;
            case 3: o.type = NV_OUT_LVDS; break;
            default: continue;
            }
            o.i2c = (conn >> 14) & 0xF;
            o.heads = (u8)(((conn >> 18) & 7) + 1);
            o.or_mask = o.heads;
            o.location = (conn >> 21) & 0xF;
            o.bus = (conn >> 25) & 0x7;
            o.connector = (u8)i;
        }
        b->out[b->noutputs++] = o;
    }
}

static void parse_ccb(nv_bios_t *b) {
    u32 dcb = b->dcb_offset, i2c = 0;
    if (b->dcb_ver >= 0x15) i2c = RD16(dcb + 2);
    if (b->dcb_ver >= 0x30) i2c = RD16(dcb + 4);
    u8 ver, hdr, len;
    int cnt;
    if (!i2c) {
        b->problems |= NVB_BAD_CCB;
        return;
    }
    if (b->dcb_ver >= 0x30) {
        ver = RD8(i2c); hdr = RD8(i2c + 1); cnt = RD8(i2c + 2); len = RD8(i2c + 3);
        if (ver >= 0x42 || ver < 0x30 || len < 4 || hdr < 4) { b->problems |= NVB_BAD_CCB; return; }
        if (ver >= 0x41) {
            b->ccb_primary = hdr >= 5 ? RD8(i2c + 4) : 0;
            b->ccb_secondary = hdr >= 6 ? RD8(i2c + 5) : 0;
        } else if (hdr >= 5) {
            u8 v = RD8(i2c + 4);
            b->ccb_primary = v & 0xF;
            b->ccb_secondary = v >> 4;
        }
    } else {
        ver = b->dcb_ver; hdr = 0; cnt = 16; len = 4;    /* 16 fixed entries before DCB 3.0 */
    }
    b->ccb_offset = i2c;
    b->ccb_ver = ver; b->ccb_hdr = hdr; b->ccb_cnt = (u8)cnt; b->ccb_len = len;
    if (cnt > NV_CCB_MAX) { cnt = NV_CCB_MAX; b->problems |= NVB_TRUNC; }
    for (int i = 0; i < cnt; i++) {
        u32 e = i2c + hdr + (u32)i * len;
        nv_ccb_entry_t *c = &b->ccb[i];
        u32 before = b->oob;
        u32 v = RD32(e);
        if (b->oob != before) { b->problems |= NVB_BAD_CCB | NVB_OOB; break; }
        memset(c, 0, sizeof *c);
        c->raw = v;
        c->drive = c->sense = c->auxch = c->share = 0xFF;
        u8 b0 = v & 0xFF, b1 = (v >> 8) & 0xFF;
        if (ver >= 0x41) {
            u8 port = v & 0x1F, aux = (v >> 5) & 0x1F;
            c->type = port == 0x1F && aux == 0x1F ? NV_CCB_UNUSED : NV_CCB_PMGR;
            if (c->type == NV_CCB_PMGR) {
                c->drive = port == 0x1F ? 0xFF : port;
                c->auxch = aux == 0x1F ? 0xFF : aux;
                c->share = c->auxch;                /* the pad that switches between I2C and AUX */
                c->speed = (v >> 28) & 0xF;
            }
        } else {
            u8 t = (v >> 24) & 0xFF;
            if (ver < 0x30) { t &= 7; if (t == 7) t = NV_CCB_UNUSED; }
            c->type = t;
            switch (t) {
            case NV_CCB_NV04_BIT: c->drive = b0; c->sense = b1; break;
            case NV_CCB_NV4E_BIT: c->drive = b1; break;
            case NV_CCB_NVIO_BIT:
                c->drive = b0 & 0xF;
                c->speed = b0 >> 4;
                if (b1 & 1) c->share = b1 >> 1;
                break;
            case NV_CCB_NVIO_AUX:
                c->auxch = b0 & 0xF;
                if (b1 & 1) c->share = c->auxch;
                break;
            case NV_CCB_UNUSED: break;
            default: c->type = NV_CCB_UNUSED; break;
            }
        }
        b->nccb = i + 1;
    }
}

/* BMP-era cards: two CRTC-indexed I2C pairs in the BMP structure */
static void bmp_i2c(nv_bios_t *b) {
    u32 ent = b->bmp_major < 4 ? 0x48 : b->bmp_offset + 0x36;
    static const u8 defs[2][2] = { { 0x3F, 0x3E }, { 0x37, 0x36 } };
    for (int i = 0; i < 2; i++) {
        nv_ccb_entry_t *c = &b->ccb[i];
        memset(c, 0, sizeof *c);
        c->type = NV_CCB_NV04_BIT;
        c->drive = RD8(ent + 4 + i * 2);
        c->sense = RD8(ent + 5 + i * 2);
        if (!c->drive) c->drive = defs[i][0];
        if (!c->sense) c->sense = defs[i][1];
        c->auxch = c->share = 0xFF;
    }
    b->nccb = 2;
    b->ccb_ver = 0;
}

static void parse_conn(nv_bios_t *b) {
    u32 dcb = b->dcb_offset;
    if (b->dcb_ver < 0x30 || b->dcb_hdr < 0x16) return;
    u32 t = RD16(dcb + 0x14);
    if (!t) return;
    u8 ver = RD8(t), hdr = RD8(t + 1), len = RD8(t + 3);
    int cnt = RD8(t + 2);
    if ((ver != 0x30 && ver != 0x40) || hdr < 4 || len < 2) { b->problems |= NVB_BAD_CONN; return; }
    b->conn_offset = t;
    b->conn_ver = ver; b->conn_hdr = hdr; b->conn_cnt = (u8)cnt; b->conn_len = len;
    b->platform = hdr >= 5 ? RD8(t + 4) : 0;
    if (cnt > NV_CONN_MAX) { cnt = NV_CONN_MAX; b->problems |= NVB_TRUNC; }
    for (int i = 0; i < cnt; i++) {
        u32 e = t + hdr + (u32)i * len;
        u32 before = b->oob;
        u8 b0 = RD8(e), b1 = RD8(e + 1), b2 = len >= 4 ? RD8(e + 2) : 0, b3 = len >= 4 ? RD8(e + 3) : 0;
        if (b->oob != before) { b->problems |= NVB_BAD_CONN | NVB_OOB; break; }
        nv_conn_entry_t *c = &b->conn[i];
        memset(c, 0, sizeof *c);
        c->raw = (u32)b0 | (u32)b1 << 8 | (u32)b2 << 16 | (u32)b3 << 24;
        c->type = b0;
        c->location = b1 & 0xF;
        c->hpd = (u8)(((b1 >> 4) & 3) | (b2 & 3) << 2 | (b3 & 7) << 4);
        c->dp2dvi = (u8)(((b1 >> 6) & 3) | (b2 & 0x0C));
        c->di = b2 >> 4;
        c->lcdid = (b3 >> 4) & 7;
        b->nconn = i + 1;
    }
}

/* no connector table (DCB before 3.0): one connector per distinct
 * "connector" value of the outputs, typed by what is on it */
static void synth_conn(nv_bios_t *b) {
    for (int i = 0; i < b->noutputs; i++) {
        nv_dcb_output_t *o = &b->out[i];
        if (o->connector >= NV_CONN_MAX) o->connector = NV_CONN_MAX - 1;
        if (o->connector >= b->nconn) {
            for (int k = b->nconn; k <= o->connector; k++) {
                memset(&b->conn[k], 0, sizeof b->conn[k]);
                b->conn[k].type = 0xFF;
                b->conn[k].synth = 1;
            }
            b->nconn = o->connector + 1;
        }
        nv_conn_entry_t *c = &b->conn[o->connector];
        int t = c->type;
        switch (o->type) {
        case NV_OUT_CRT: t = t == 0x31 ? 0x30 : t == 0xFF ? 0x00 : t; break;
        case NV_OUT_TMDS: t = t == 0x00 ? 0x30 : t == 0xFF ? 0x31 : t; break;
        case NV_OUT_LVDS: t = 0x40; break;
        case NV_OUT_TV: if (t == 0xFF) t = 0x11; break;
        case NV_OUT_DP: t = 0x46; break;
        }
        c->type = (u8)t;
    }
}

static void parse_gpio(nv_bios_t *b) {
    u32 dcb = b->dcb_offset, g = 0;
    if (b->dcb_ver >= 0x30 && b->dcb_hdr >= 0x0C) g = RD16(dcb + 0x0A);
    else if (b->dcb_ver >= 0x22 && dcb >= 0x0F && RD8(dcb - 1) >= 0x13) g = RD16(dcb - 0x0F);
    if (!g) return;
    u8 ver = RD8(g), hdr, len;
    int cnt;
    if (ver < 0x30) { hdr = 3; cnt = RD8(g + 2); len = RD8(g + 1); }
    else if (ver <= 0x41) { hdr = RD8(g + 1); cnt = RD8(g + 2); len = RD8(g + 3); }
    else { b->problems |= NVB_BAD_GPIO; return; }
    int need = ver < 0x40 ? 2 : ver == 0x40 ? 4 : 5;
    if (len < need || hdr < 3) { b->problems |= NVB_BAD_GPIO; return; }
    b->gpio_offset = g;
    b->gpio_ver = ver; b->gpio_hdr = hdr; b->gpio_cnt = (u8)cnt; b->gpio_len = len;
    for (int i = 0; i < cnt; i++) {
        u32 e = g + hdr + (u32)i * len;
        u32 before = b->oob;
        nv_gpio_entry_t x;
        memset(&x, 0, sizeof x);
        if (ver < 0x40) {
            u32 v = RD16(e);
            x.raw = v;
            x.line = v & 0x1F; x.func = (v >> 5) & 0x3F;
            x.log[0] = (v >> 11) & 3; x.log[1] = (v >> 13) & 3; x.param = (v >> 15) & 1;
            if (x.func == 0x3F) x.func = 0xFF;
        } else if (ver == 0x40) {
            u32 v = RD32(e);
            x.raw = v;
            x.line = v & 0x1F; x.func = (v >> 8) & 0xFF;
            x.log[0] = (v >> 27) & 3; x.log[1] = (v >> 29) & 3; x.param = v >> 31;
        } else {
            u32 v = RD32(e);
            u8 v1 = RD8(e + 4);
            x.raw = v;
            x.line = v & 0x3F; x.io = (v >> 6) & 1; x.init = (v >> 7) & 1; x.func = (v >> 8) & 0xFF;
            x.log[0] = (v1 >> 4) & 3; x.log[1] = (v1 >> 6) & 3; x.param = v >> 31;
        }
        if (b->oob != before) { b->problems |= NVB_BAD_GPIO | NVB_OOB; break; }
        if (x.func == 0xFF) continue;
        if (b->ngpio >= NV_GPIO_MAX) { b->problems |= NVB_TRUNC; break; }
        b->gpio[b->ngpio++] = x;
    }
}

static void parse_dcb(nv_bios_t *b, int chipset) {
    if (chipset && chipset < 0x05) return;          /* NV04 has no DCB pointer */
    if (b->bmp_offset && !b->bit_offset && b->bmp_major < 5) return;     /* 0x36 is no pointer */
    u32 dcb = RD16(0x36);
    if (!dcb) { b->problems |= NVB_NO_DCB; return; }
    u8 ver = RD8(dcb);
    b->dcb_ver = ver;
    if (ver >= 0x42) { b->problems |= NVB_BAD_DCB; return; }
    if (ver >= 0x30) {
        if (RD32(dcb + 6) != 0x4EDCBDCBu) { b->problems |= NVB_BAD_DCB; return; }
        b->dcb_hdr = RD8(dcb + 1); b->dcb_cnt = RD8(dcb + 2); b->dcb_len = RD8(dcb + 3);
        if (b->dcb_hdr < 10 || b->dcb_len < 8) { b->problems |= NVB_BAD_DCB; return; }
        if (b->dcb_hdr > 0x16) b->dcb_flags = RD8(dcb + 0x16);
    } else if (ver >= 0x20) {
        if (RD32(dcb + 4) != 0x4EDCBDCBu) { b->problems |= NVB_BAD_DCB; return; }
        u32 i2c = RD16(dcb + 2);
        b->dcb_hdr = 8; b->dcb_len = 8;
        b->dcb_cnt = i2c > dcb ? (u8)MIN((i2c - dcb - 8) / 8, 255u) : 0;
    } else if (ver >= 0x15) {
        if (dcb < 7) { b->problems |= NVB_BAD_DCB; return; }
        char rec[8];
        for (int i = 0; i < 7; i++) rec[i] = (char)RD8(dcb - 7 + (u32)i);
        if (memcmp(rec, "DEV_REC", 7)) { b->problems |= NVB_BAD_DCB; return; }
        u32 i2c = RD16(dcb + 2);
        b->dcb_hdr = 4; b->dcb_len = 10;
        b->dcb_cnt = i2c > dcb ? (u8)MIN((i2c - dcb - 4) / 10, 255u) : 0;
    } else {
        return;                                     /* DCB 1.4 and older: nothing useful */
    }
    b->dcb_offset = dcb;
    parse_dcb_entries(b);
    parse_ccb(b);
    parse_conn(b);
    if (!b->nconn) synth_conn(b);
    parse_gpio(b);
}

int nv_bios_parse(const u8 *raw, u32 size, int chipset, nv_bios_t *b) {
    memset(b, 0, sizeof *b);
    if (!nv_rom_scan(raw, size, &b->rom)) return 0;
    u32 limit = b->rom.legacy_size ? b->rom.legacy_size : b->rom.chain_size;
    b->bit_offset = find(&b->rom, limit, "\xFF\xB8" "BIT", 5);
    b->bmp_offset = find(&b->rom, limit, "\xFF\x7F" "NV\0", 5);
    if (b->bit_offset == 0xFFFFFFFFu) b->bit_offset = 0;
    if (b->bmp_offset == 0xFFFFFFFFu || b->bit_offset) b->bmp_offset = 0;
    if (b->bit_offset) {
        parse_bit(b);
        const nv_bit_token_t *t = nv_bios_token(b, 'i');
        if (t && t->size >= 5) {
            version_from(b, t->ptr, RD8(t->ptr + 4));
            if (t->size >= 6) b->feature = RD8(t->ptr + 5);
        } else if ((t = nv_bios_token(b, 'B')) && t->size >= 5) {
            version_from(b, t->ptr, RD8(t->ptr + 4));
        }
    } else if (b->bmp_offset) {
        u32 o = b->bmp_offset;
        b->bmp_major = RD8(o + 5);
        b->bmp_minor = RD8(o + 6);
        u8 sum = 0;
        for (u32 i = 0; i < 8; i++) sum = (u8)(sum + RD8(o + i));
        if (sum || b->bmp_major > 5) b->problems |= NVB_BAD_BMP;
        b->feature = RD8(o + 9);
        if (b->bmp_major >= 1) version_from(b, o + 10, -1);
    } else {
        b->problems |= NVB_NO_BIT;
    }
    parse_dcb(b, chipset);
    if (!b->nccb && b->bmp_offset) bmp_i2c(b);
    if (b->oob) b->problems |= NVB_OOB;
    return b->rom.score;
}

/* ---------------- names ---------------- */

const char *nv_out_type_str(int t) {
    switch (t) {
    case NV_OUT_CRT: return "analog (CRT)";
    case NV_OUT_TV: return "TV";
    case NV_OUT_TMDS: return "TMDS (DVI / HDMI)";
    case NV_OUT_LVDS: return "LVDS (panel)";
    case NV_OUT_SDI: return "SDI";
    case NV_OUT_DP: return "DisplayPort";
    case NV_OUT_WFD: return "Wifi Display";
    }
    return "unknown";
}

const char *nv_conn_type_str(int t) {
    switch (t) {
    case 0x00: return "VGA";
    case 0x01: return "DVI-A";
    case 0x02: return "VGA (pod)";
    case 0x10: return "TV composite";
    case 0x11: return "TV S-Video";
    case 0x12: return "TV S-Video breakout";
    case 0x13: return "HDTV component (YPrPb)";
    case 0x14: return "TV SCART";
    case 0x16: return "TV composite SCART (D-connector)";
    case 0x17: return "HDTV D-connector";
    case 0x18: return "HDTV component (pod)";
    case 0x19: return "TV S-Video (pod)";
    case 0x1a: return "TV composite (pod)";
    case 0x20: return "DVI-I with TV S-Video";
    case 0x21: return "DVI-I with TV composite";
    case 0x22: return "DVI-I with TV breakout";
    case 0x30: return "DVI-I";
    case 0x31: return "DVI-D";
    case 0x32: return "Apple Display Connector";
    case 0x38: return "LFH DVI-I 1";
    case 0x39: return "LFH DVI-I 2";
    case 0x3c: return "BNC";
    case 0x40: return "LVDS panel (SPWG)";
    case 0x41: return "LVDS panel";
    case 0x42: return "LVDS (SPWG, removable)";
    case 0x43: return "LVDS (removable)";
    case 0x45: return "TMDS panel";
    case 0x46: return "DisplayPort";
    case 0x47: return "DisplayPort (internal panel)";
    case 0x48: return "Mini DisplayPort";
    case 0x50: case 0x51: return "VGA (dock)";
    case 0x52: case 0x53: return "DVI-I (dock)";
    case 0x54: case 0x55: return "DVI-D (dock)";
    case 0x56: case 0x57: return "DisplayPort (dock)";
    case 0x58: case 0x59: return "Mini DisplayPort (dock)";
    case 0x60: return "HDMI";
    case 0x61: return "HDMI";
    case 0x62: return "S/PDIF audio";
    case 0x63: return "Mini HDMI";
    case 0x64: return "LFH DisplayPort 1";
    case 0x65: return "LFH DisplayPort 2";
    case 0x70: return "Wifi Display (virtual)";
    case 0x71: return "USB-C";
    case 0xff: return "(unused)";
    }
    return "unknown connector";
}

const char *nv_ccb_type_str(int t) {
    switch (t) {
    case NV_CCB_NV04_BIT: return "CRTC-indexed bit-bang";
    case NV_CCB_NV4E_BIT: return "NV4E bit-bang";
    case NV_CCB_NVIO_BIT: return "I2C port";
    case NV_CCB_NVIO_AUX: return "DP AUX channel";
    case NV_CCB_PMGR: return "PMGR port";
    case NV_CCB_UNUSED: return "unused";
    }
    return "unknown";
}

static const u8 hpd_funcs[7] = { 0x07, 0x08, 0x51, 0x52, 0x5E, 0x5F, 0x60 };
int nv_hpd_func(int bit) { return bit >= 0 && bit < 7 ? hpd_funcs[bit] : -1; }

const char *nv_gpio_func_str(int f) {
    static const char *hpd[7] = { "hot plug A", "hot plug B", "hot plug C", "hot plug D", "hot plug E", "hot plug F", "hot plug G" };
    for (int i = 0; i < 7; i++) if (hpd_funcs[i] == f) return hpd[i];
    switch (f) {
    case 0x00: return "LCD0 backlight";
    case 0x01: return "LCD0 power";
    case 0x02: return "LCD0 power status";
    case 0x03: return "VSync";
    case 0x04: case 0x05: case 0x06: return "voltage select";
    case 0x09: return "fan";
    case 0x0c: return "DAC 1 select";
    case 0x10: return "thermal / external power";
    case 0x2d: return "TV DAC";
    case 0x3d: return "fan sense";
    case 0x4c: return "power alert";
    case 0x79: return "external power low";
    case 0x84: return "logo LED";
    }
    return NULL;
}
