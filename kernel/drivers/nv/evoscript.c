/* Halcyon OS - NVIDIA GPUs (M10b): the video BIOS's display tables (IED
 * output scripts, BIT 'U'; DisplayPort, BIT 'd') and the init script
 * interpreter that runs their scripts. Ported from nouveau (MIT, Copyright
 * 2012 Red Hat Inc.; THIRD_PARTY_NOTICES.md): nvkm/subdev/bios/disp.c,
 * dp.c, init.c and dcb.c (the output hash).
 *
 * Differences from nouveau, for safety on hardware nobody tested this on:
 *   - display scripts may only write display, I2C / GPIO and pad
 *     registers (nv_evo_script_reg_ok); any other write is refused, counted
 *     and logged, and the script goes on (as nouveau's devinit "mmio" hook
 *     drops writes)
 *   - opcodes that belong to POST (memory and clock setup, resets) stop the
 *     script; PLL and GPIO opcodes are skipped and logged
 *   - at most 4096 opcodes, 8 nested scripts and 1 s of delays per script;
 *     a read outside the image stops it */
#include "evo.h"
#include "core/lib.h"

/* ---------------- table helpers ---------------- */

static u8 r8(const nv_bios_t *b, u32 a) { return nv_rom_rd8(&b->rom, a, NULL); }
static u16 r16(const nv_bios_t *b, u32 a) { return nv_rom_rd16(&b->rom, a, NULL); }
static u32 r32(const nv_bios_t *b, u32 a) { return nv_rom_rd32(&b->rom, a, NULL); }

u16 nv_dcb_hasht(const nv_dcb_output_t *o) { return (u16)(((u16)o->extdev << 8) | ((o->location & 3) << 4) | (o->type & 0xF)); }
u16 nv_dcb_hashm(const nv_dcb_output_t *o) { return (u16)(((u16)(o->heads & 0xF) << 8) | ((o->link & 3) << 6) | (o->or_mask & 0xF)); }

/* ---------------- IED (BIT 'U') ---------------- */

static u32 disp_table(const nv_bios_t *b, u8 *ver, u8 *hdr, u8 *cnt, u8 *len, u8 *sub) {
    const nv_bit_token_t *t = nv_bios_token(b, 'U');
    if (!t || t->ver != 1 || t->size < 2) return 0;
    u32 data = r16(b, t->ptr);
    if (!data) return 0;
    *ver = r8(b, data);
    if (*ver != 0x20 && *ver != 0x21 && *ver != 0x22) return 0;
    *hdr = r8(b, data + 1);
    *len = r8(b, data + 2);
    *cnt = r8(b, data + 3);
    *sub = r8(b, data + 4);
    return data;
}

int nv_ied_match(const nv_bios_t *b, u16 type, u16 m, nv_ied_t *out) {
    u8 ver = 0, hdr = 0, cnt = 0, len = 0, sub = 0;
    memset(out, 0, sizeof *out);
    u32 tab = disp_table(b, &ver, &hdr, &cnt, &len, &sub);
    if (!tab || len < 2 || sub < 0x0a) return 0;
    for (int i = 0; i < cnt; i++) {
        u32 data = r16(b, tab + hdr + (u32)i * len);
        if (!data) continue;
        u16 ty = r16(b, data);
        u16 mk = (u16)r32(b, data + 2);
        if (ver <= 0x20) mk |= 0x00c0;                   /* match any link */
        if (ty != type || (mk & m) != m) continue;
        out->outp = data;
        out->ver = ver;
        out->hdr = sub;
        out->cnt = r8(b, data + 5);
        out->len = 6;
        out->type = ty;
        out->mask = mk;
        out->script[0] = r16(b, data + 6);
        out->script[1] = r16(b, data + 8);
        out->script[2] = sub >= 0x0c ? r16(b, data + 0x0a) : 0;
        return 1;
    }
    return 0;
}

int nv_ied_ocfg(const nv_bios_t *b, const nv_ied_t *ied, u8 proto, u8 flags, u16 clkcmp[2]) {
    for (int i = 0; i < ied->cnt; i++) {
        u32 e = ied->outp + ied->hdr + (u32)i * ied->len;
        u8 p = r8(b, e), f = r8(b, e + 1);
        if ((p == proto || p == 0xff) && f == flags) {
            clkcmp[0] = r16(b, e + 2);
            clkcmp[1] = r16(b, e + 4);
            return 1;
        }
    }
    return 0;
}

u16 nv_ied_oclk(const nv_bios_t *b, u16 cmp, u32 khz) {
    for (int i = 0; cmp && i < 64; i++, cmp = (u16)(cmp + 4))
        if (khz / 10 >= r16(b, cmp)) return r16(b, (u32)cmp + 2);
    return 0;
}

/* ---------------- DisplayPort (BIT 'd') ---------------- */

static u32 dp_table(const nv_bios_t *b, u8 *ver, u8 *hdr, u8 *cnt, u8 *len) {
    const nv_bit_token_t *t = nv_bios_token(b, 'd');
    if (!t || t->ver != 1 || t->size < 2) return 0;
    u32 data = r16(b, t->ptr);
    if (!data) return 0;
    *ver = r8(b, data);
    switch (*ver) {
    case 0x20: case 0x21: case 0x30: case 0x40: case 0x41: case 0x42: break;
    default: return 0;
    }
    *hdr = r8(b, data + 1);
    *len = r8(b, data + 2);
    *cnt = r8(b, data + 3);
    return data;
}

int nv_dpout_match(const nv_bios_t *b, u16 type, u16 m, nv_dpout_t *out) {
    u8 ver = 0, hdr = 0, cnt = 0, len = 0;
    memset(out, 0, sizeof *out);
    u32 tab = dp_table(b, &ver, &hdr, &cnt, &len);
    if (!tab) return 0;
    for (int i = 0; i < cnt; i++) {
        u32 o = r16(b, tab + hdr + (u32)i * len);
        if (!o) continue;
        nv_dpout_t d;
        memset(&d, 0, sizeof d);
        d.outp = o;
        d.ver = ver;
        d.type = r16(b, o);
        d.mask = r16(b, o + 2);
        switch (ver) {
        case 0x20: d.mask |= 0x00c0;                     /* fall through: match any link */
            /* fallthrough */
        case 0x21: case 0x30:
            d.hdr = r8(b, tab + 4);
            d.len = r8(b, tab + 5);
            d.cnt = r8(b, o + 4);
            d.flags = r8(b, o + 5);
            d.script[0] = r16(b, o + 6);
            d.script[1] = r16(b, o + 8);
            if (len >= 0x0c) d.lnkcmp = r16(b, o + 0x0a);
            if (len >= 0x0f) { d.script[2] = r16(b, o + 0x0c); d.script[3] = r16(b, o + 0x0e); }
            if (len >= 0x11) d.script[4] = r16(b, o + 0x10);
            break;
        default:                                         /* 0x40-0x42 */
            d.hdr = r8(b, tab + 4);
            d.flags = r8(b, o + 4);
            d.script[0] = r16(b, o + 5);
            d.script[1] = r16(b, o + 7);
            d.lnkcmp = r16(b, o + 9);
            d.script[2] = r16(b, o + 0x0b);
            d.script[3] = r16(b, o + 0x0d);
            d.script[4] = r16(b, o + 0x0f);
            break;
        }
        if (d.type == type && (d.mask & m) == m) { *out = d; return 1; }
    }
    return 0;
}

int nv_dpcfg_match(const nv_bios_t *b, const nv_dpout_t *o, u8 pc, u8 vs, u8 pe, nv_dpcfg_t *out) {
    u8 ver = o->ver, hdr = o->hdr, cnt = o->cnt, len = o->len;
    u32 base = o->outp;
    memset(out, 0, sizeof *out);
    if (vs > 3 || pe > 3 || pc > 3) return 0;
    if (ver >= 0x40) {
        u8 thdr, tcnt, tlen, tver;
        base = dp_table(b, &tver, &thdr, &tcnt, &tlen);
        if (!base) return 0;
        hdr = (u8)(thdr + tlen * tcnt);
        len = r8(b, base + 6);
        cnt = (u8)(r8(b, base + 7) * r8(b, base + 5));
    }
    int idx = -1;
    if (ver >= 0x30) {
        static const u8 vsoff[] = { 0, 4, 7, 9 };
        idx = pc * 10 + vsoff[vs] + pe;
        if (ver >= 0x40 && ver <= 0x41 && o->hdr >= 0x12) idx += r8(b, o->outp + 0x11) * 40;
        else if (ver >= 0x42) idx += r8(b, o->outp + 0x11) * 10;
    } else {
        for (int i = 0; i < cnt; i++) {
            u32 d = base + hdr + (u32)i * len;
            if (r8(b, d) == vs && r8(b, d + 1) == pe) { idx = i; break; }
        }
    }
    if (idx < 0 || idx >= cnt) return 0;
    u32 d = base + hdr + (u32)idx * len;
    switch (ver) {
    case 0x20: case 0x21: out->dc = r8(b, d + 2); out->pe = r8(b, d + 3); out->tx_pu = r8(b, d + 4); break;
    case 0x30: case 0x40: case 0x41:
        out->pc = r8(b, d); out->dc = r8(b, d + 1); out->pe = r8(b, d + 2); out->tx_pu = r8(b, d + 3); break;
    case 0x42: out->dc = r8(b, d); out->pe = r8(b, d + 1); out->tx_pu = r8(b, d + 2); break;
    default: return 0;
    }
    return 1;
}

u16 nv_dp_lnkcmp(const nv_bios_t *b, const nv_dpout_t *o, u8 bw) {
    u32 l = o->lnkcmp;
    if (!l) return 0;
    for (int i = 0; i < 16; i++) {
        if (o->ver < 0x30) {
            if ((u32)bw * 2700 >= r16(b, l)) return r16(b, l + 2);
            l += 4;
        } else {
            if (bw >= r8(b, l)) return r16(b, l + 1);
            l += 3;
        }
    }
    return 0;
}

/* ---------------- the interpreter ---------------- */

int nv_evo_script_reg_ok(u32 reg) {
    return (reg >= 0x00d000 && reg < 0x00f000) ||       /* GPIO, I2C, AUX, pads (GF119+ / NV50) and their PLLs */
           (reg >= 0x0c0000 && reg < 0x0c1000) ||       /* VGA I/O (PRMVIO) */
           (reg >= 0x601000 && reg < 0x604000) ||       /* VGA CRTC (PRMCIO) */
           (reg >= 0x610000 && reg < 0x620000);         /* the display engine: heads, SORs, DACs, PIORs */
}

typedef struct ini {
    nv_evo_t *e;
    const nv_bios_t *b;
    nv_init_ctx_t c;
    u32 off;
    int execute;
    int nested;
    u32 repeat, repend;
    int ops, writes, refused, skipped;
    u32 slept_us;
    int stop;
    char why[64];
} ini_t;

static int ex(ini_t *s) { return s->execute == 1 || (s->execute & 5) == 5; }
static void ex_set(ini_t *s, int on) { if (on) s->execute &= 0xfd; else s->execute |= 0x02; }
static void ex_inv(ini_t *s) { s->execute ^= 0x02; }
static void ex_force(ini_t *s, int on) { if (on) s->execute |= 0x04; else s->execute &= 0xfb; }

static void stop(ini_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void stop(ini_t *s, const char *fmt, ...) {
    if (s->stop) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->why, sizeof s->why, fmt, ap);
    va_end(ap);
    s->stop = 1;
    s->off = 0;
}

static u8 b8(ini_t *s, u32 a) {
    u32 oob = 0;
    u8 v = nv_rom_rd8(&s->b->rom, a, &oob);
    if (oob) stop(s, "read outside the image at %04x", a);
    return v;
}
static u16 b16(ini_t *s, u32 a) { return (u16)(b8(s, a) | b8(s, a + 1) << 8); }
static u32 b32(ini_t *s, u32 a) { return (u32)b16(s, a) | (u32)b16(s, a + 2) << 16; }

static u32 nvreg(ini_t *s, u32 reg) {
    reg &= ~3u;
    if (reg & 0x80000000u) {
        if (s->c.head < 0 && ex(s)) stop(s, "script needs a head");
        reg += (u32)(s->c.head > 0 ? s->c.head : 0) * 0x800;
        reg &= ~0x80000000u;
    }
    if (reg & 0x40000000u) {
        if (s->c.or_id < 0 && ex(s)) stop(s, "script needs an output resource");
        reg += (u32)(s->c.or_id > 0 ? s->c.or_id : 0) * 0x800;
        reg &= ~0x40000000u;
        if (reg & 0x20000000u) {
            reg += (s->c.link == 2) * 0x80u;
            reg &= ~0x20000000u;
        }
    }
    return reg & 0x00FFFFFCu;
}

static u32 i_rd(ini_t *s, u32 reg) {
    reg = nvreg(s, reg);
    if (!ex(s) || s->stop) return 0;
    return s->e->r->rd32(s->e->r->ctx, reg);
}
static void i_wr(ini_t *s, u32 reg, u32 v) {
    reg = nvreg(s, reg);
    if (!ex(s) || s->stop) return;
    if (!nv_evo_script_reg_ok(reg)) {
        if (s->refused++ < 6) nv_evo_log(s->e, "script: write %06x = %08x refused (not a display register)", reg, v);
        s->e->script_refused++;
        return;
    }
    s->writes++;
    s->e->r->wr32(s->e->r->ctx, reg, v);
}
static u32 i_mask(ini_t *s, u32 reg, u32 m, u32 v) {
    u32 o = i_rd(s, reg);
    if (ex(s) && !s->stop) i_wr(s, reg, (o & ~m) | v);
    return o;
}

/* VGA ports through PRMCIO (NV50+: 0x601000 + port) */
static u8 i_rdport(ini_t *s, u16 port) {
    if (!ex(s) || s->stop) return 0;
    if (!s->e->ops.rd8) { stop(s, "no 8-bit register access for VGA port %04x", port); return 0; }
    return s->e->ops.rd8(s->e->ops.ctx, 0x601000u + port);
}
static void i_wrport(ini_t *s, u16 port, u8 v) {
    if (!ex(s) || s->stop) return;
    if (!s->e->ops.wr8) { stop(s, "no 8-bit register access for VGA port %04x", port); return; }
    if (port < 0x3b0 || port > 0x3df) { stop(s, "VGA port %04x", port); return; }
    s->writes++;
    s->e->ops.wr8(s->e->ops.ctx, 0x601000u + port, v);
}
static u8 i_rdvgai(ini_t *s, u16 port, u8 index) {
    if (!ex(s) || s->stop) return 0;
    i_wrport(s, port, index);
    return i_rdport(s, (u16)(port + 1));
}
static void i_wrvgai(ini_t *s, u16 port, u8 index, u8 v) {
    if (!ex(s) || s->stop) return;
    i_wrport(s, port, index);
    i_wrport(s, (u16)(port + 1), v);
}

static int ccb_of(ini_t *s, u8 index) {
    if (index == 0xff || index == 0x80) return s->b->ccb_primary;
    if (index == 0x81) return s->b->ccb_secondary;
    return index;
}
static int i_rdi2c(ini_t *s, u8 index, u8 addr, u8 reg) {
    if (!ex(s) || s->stop || !s->e->ops.i2c) return E_NODEV;
    u8 v = 0;
    nv_i2c_msg_t m[2] = { { addr, 0, 1, &reg }, { addr, NV_I2C_RD, 1, &v } };
    int r = s->e->ops.i2c(s->e->ops.ctx, ccb_of(s, index), m, 2);
    return r ? r : v;
}
static int i_wri2c(ini_t *s, u8 index, u8 addr, u8 reg, u8 val) {
    if (!ex(s) || s->stop || !s->e->ops.i2c) return E_NODEV;
    u8 buf[2] = { reg, val };
    nv_i2c_msg_t m = { addr, 0, 2, buf };
    s->writes++;
    return s->e->ops.i2c(s->e->ops.ctx, ccb_of(s, index), &m, 1);
}
static int aux_ccb(ini_t *s) {
    if (s->c.outp < 0 || s->c.outp >= s->b->noutputs) { if (ex(s)) stop(s, "script needs an output for AUX"); return -1; }
    return s->b->out[s->c.outp].i2c;
}
static u8 i_rdaux(ini_t *s, u32 addr) {
    int ccb = aux_ccb(s);
    u8 v = 0;
    if (ccb < 0 || !ex(s) || s->stop) return 0;
    if (nv_dp_rd(s->e, ccb, addr, &v, 1)) return 0;
    return v;
}
static void i_wraux(ini_t *s, u32 addr, u8 v) {
    int ccb = aux_ccb(s);
    if (ccb < 0 || !ex(s) || s->stop) return;
    s->writes++;
    nv_dp_wr(s->e, ccb, addr, &v, 1);
}

static void i_sleep(ini_t *s, u32 us) {
    if (!ex(s) || s->stop) return;
    if (s->slept_us + us > 1000000) { if (s->slept_us < 1000000) nv_evo_log(s->e, "script: delays over 1 s skipped"); s->slept_us = 1000000; return; }
    s->slept_us += us;
    if (s->e->r->delay_us) s->e->r->delay_us(s->e->r->ctx, us);
}

/* BIT 'I': the script, condition, macro and translation tables */
static u16 init_table(ini_t *s, u32 offset) {
    const nv_bit_token_t *t = nv_bios_token(s->b, 'I');
    if (!t || t->size < offset + 2) return 0;
    return b16(s, (u32)t->ptr + offset);
}
static int cond_met(ini_t *s, u8 cond) {
    u16 t = init_table(s, 0x06);
    if (!t) return 0;
    u32 reg = b32(s, t + cond * 12u), msk = b32(s, t + cond * 12u + 4), val = b32(s, t + cond * 12u + 8);
    return (i_rd(s, reg) & msk) == val;
}
static int io_cond_met(ini_t *s, u8 cond) {
    u16 t = init_table(s, 0x08);
    if (!t) return 0;
    u16 port = b16(s, t + cond * 5u);
    u8 index = b8(s, t + cond * 5u + 2), msk = b8(s, t + cond * 5u + 3), val = b8(s, t + cond * 5u + 4);
    return (i_rdvgai(s, port, index) & msk) == val;
}
static int io_flag_cond_met(ini_t *s, u8 cond) {
    u16 t = init_table(s, 0x0a);
    if (!t) return 0;
    u32 e = t + cond * 9u;
    u16 port = b16(s, e), data = b16(s, e + 5);
    u8 index = b8(s, e + 2), msk = b8(s, e + 3), shift = b8(s, e + 4), dmask = b8(s, e + 7), val = b8(s, e + 8);
    u8 io = (u8)((i_rdvgai(s, port, index) & msk) >> (shift & 7));
    return (b8(s, data + io) & dmask) == val;
}
static u32 shiftv(u32 d, u8 shift) { return shift < 0x80 ? d >> (shift & 31) : d << ((0x100 - shift) & 31); }

static void exec_script(ini_t *s);

static void skip_pll(ini_t *s, const char *what) {
    if (ex(s) && !s->stop) {
        s->skipped++;
        nv_evo_log(s->e, "script: %s at %04x skipped (PLLs are programmed by the driver)", what, s->off);
    }
}

static void op(ini_t *s) {
    u32 o = s->off;
    u8 opc = b8(s, o);
    if (s->stop) return;
    switch (opc) {
    case 0x32: {                                         /* IO_RESTRICT_PROG */
        u16 port = b16(s, o + 1);
        u8 index = b8(s, o + 3), m = b8(s, o + 4), sh = b8(s, o + 5), count = b8(s, o + 6);
        u32 reg = b32(s, o + 7);
        s->off += 11;
        u8 conf = (u8)((i_rdvgai(s, port, index) & m) >> (sh & 7));
        for (u8 i = 0; i < count && !s->stop; i++) {
            if (i == conf) i_wr(s, reg, b32(s, s->off));
            s->off += 4;
        }
        break;
    }
    case 0x33: {                                         /* REPEAT */
        u8 count = b8(s, o + 1);
        u32 repeat = s->repeat;
        s->off += 2;
        s->repeat = s->off;
        s->repend = s->off;
        while (count-- && !s->stop) {
            s->off = s->repeat;
            exec_script(s);
        }
        s->off = s->repend;
        s->repeat = repeat;
        break;
    }
    case 0x34: {                                         /* IO_RESTRICT_PLL */
        u8 count = b8(s, o + 7);
        skip_pll(s, "IO_RESTRICT_PLL");
        s->off += 12 + (u32)count * 2;
        break;
    }
    case 0x36:                                           /* END_REPEAT */
        s->off += 1;
        if (s->repeat) { s->repend = s->off; s->off = 0; }
        break;
    case 0x37: {                                         /* COPY */
        u32 reg = b32(s, o + 1);
        u8 sh = b8(s, o + 5), sm = b8(s, o + 6), index = b8(s, o + 9), m = b8(s, o + 10);
        u16 port = b16(s, o + 7);
        s->off += 11;
        u8 d = (u8)((i_rdvgai(s, port, index) & m) | (shiftv(i_rd(s, reg), sh) & sm));
        i_wrvgai(s, port, index, d);
        break;
    }
    case 0x38: s->off += 1; ex_inv(s); break;           /* NOT */
    case 0x39: {                                         /* IO_FLAG_CONDITION */
        u8 cond = b8(s, o + 1);
        s->off += 2;
        if (!io_flag_cond_met(s, cond)) ex_set(s, 0);
        break;
    }
    case 0x3a: {                                         /* GENERIC_CONDITION */
        u8 cond = b8(s, o + 1), size = b8(s, o + 2);
        s->off += 3;
        switch (cond) {
        case 0: {                                        /* internal DisplayPort (eDP) */
            u8 type = 0xff;
            if (s->c.outp >= 0 && s->c.outp < s->b->noutputs) {
                int cn = s->b->out[s->c.outp].connector;
                if (cn < s->b->nconn) type = s->b->conn[cn].type;
            }
            if (type != 0x47) ex_set(s, 0);
            break;
        }
        case 1: case 2: {                                /* spread spectrum PLL 0 / 1 */
            if (s->c.outp >= 0 && s->c.outp < s->b->noutputs) {
                const nv_dcb_output_t *d = &s->b->out[s->c.outp];
                nv_dpout_t dp;
                if (nv_dpout_match(s->b, NV_OUT_DP, (u16)((d->or_mask & 0xF) | ((d->link & 3) << 6)), &dp)) {
                    if (!(dp.flags & cond)) ex_set(s, 0);
                    break;
                }
            }
            break;
        }
        case 5: if (!(i_rdaux(s, 0x0d) & 1)) ex_set(s, 0); break;   /* ASSR */
        case 7: ex_set(s, 0); break;                     /* no panel sequence delays */
        default: s->off += size; break;
        }
        break;
    }
    case 0x3b: case 0x3c: {                              /* IO_MASK_OR, IO_OR */
        u8 index = b8(s, o + 1), orb = (u8)(s->c.or_id > 0 ? s->c.or_id : 0);
        s->off += 2;
        u8 d = i_rdvgai(s, 0x3d4, index);
        i_wrvgai(s, 0x3d4, index, opc == 0x3b ? (u8)(d & ~(1u << orb)) : (u8)(d | (1u << orb)));
        break;
    }
    case 0x47: { u32 reg = b32(s, o + 1), m = b32(s, o + 5); s->off += 9; i_mask(s, reg, m, 0); break; }   /* ANDN_REG */
    case 0x48: { u32 reg = b32(s, o + 1), m = b32(s, o + 5); s->off += 9; i_mask(s, reg, 0, m); break; }   /* OR_REG */
    case 0x49: {                                         /* INDEX_ADDRESS_LATCHED */
        u32 creg = b32(s, o + 1), dreg = b32(s, o + 5), m = b32(s, o + 9), data = b32(s, o + 13);
        u8 count = b8(s, o + 17);
        s->off += 18;
        while (count-- && !s->stop) {
            u8 ia = b8(s, s->off), id = b8(s, s->off + 1);
            s->off += 2;
            i_wr(s, dreg, id);
            i_mask(s, creg, ~m, data | ia);
        }
        break;
    }
    case 0x4a: { u8 count = b8(s, o + 6); skip_pll(s, "IO_RESTRICT_PLL2"); s->off += 11 + (u32)count * 4; break; }
    case 0x4b: skip_pll(s, "PLL2"); s->off += 9; break;
    case 0x4c: {                                         /* I2C_BYTE */
        u8 index = b8(s, o + 1), addr = (u8)(b8(s, o + 2) >> 1), count = b8(s, o + 3);
        s->off += 4;
        while (count-- && !s->stop) {
            u8 reg = b8(s, s->off), m = b8(s, s->off + 1), d = b8(s, s->off + 2);
            s->off += 3;
            int v = i_rdi2c(s, index, addr, reg);
            if (v >= 0) i_wri2c(s, index, addr, reg, (u8)((v & m) | d));
        }
        break;
    }
    case 0x4d: {                                         /* ZM_I2C_BYTE */
        u8 index = b8(s, o + 1), addr = (u8)(b8(s, o + 2) >> 1), count = b8(s, o + 3);
        s->off += 4;
        while (count-- && !s->stop) {
            u8 reg = b8(s, s->off), d = b8(s, s->off + 1);
            s->off += 2;
            i_wri2c(s, index, addr, reg, d);
        }
        break;
    }
    case 0x4e: {                                         /* ZM_I2C */
        u8 index = b8(s, o + 1), addr = (u8)(b8(s, o + 2) >> 1), count = b8(s, o + 3);
        u8 data[256];
        s->off += 4;
        for (int i = 0; i < count && !s->stop; i++) data[i] = b8(s, s->off++);
        if (ex(s) && !s->stop && s->e->ops.i2c) {
            nv_i2c_msg_t m = { addr, 0, count, data };
            s->writes++;
            s->e->ops.i2c(s->e->ops.ctx, ccb_of(s, index), &m, 1);
        }
        break;
    }
    case 0x4f: {                                         /* TMDS (pre-NV50 RAMDAC registers: refused) */
        u8 tmds = b8(s, o + 1), addr = b8(s, o + 2), m = b8(s, o + 3), d = b8(s, o + 4);
        static const u32 tab[4] = { 0x6808b0, 0x6808b8, 0x6828b0, 0x6828b8 };
        s->off += 5;
        u32 reg = tmds < 4 ? tab[tmds] : 0;
        if (!reg) break;
        i_wr(s, reg, addr | 0x00010000u);
        i_wr(s, reg + 4, d | (i_rd(s, reg + 4) & m));
        i_wr(s, reg, addr);
        break;
    }
    case 0x50: {                                         /* ZM_TMDS_GROUP */
        u8 tmds = b8(s, o + 1), count = b8(s, o + 2);
        static const u32 tab[4] = { 0x6808b0, 0x6808b8, 0x6828b0, 0x6828b8 };
        u32 reg = tmds < 4 ? tab[tmds] : 0;
        s->off += 3;
        while (count-- && !s->stop) {
            u8 a = b8(s, s->off), d = b8(s, s->off + 1);
            s->off += 2;
            if (reg) { i_wr(s, reg + 4, d); i_wr(s, reg, a); }
        }
        break;
    }
    case 0x51: {                                         /* CR_INDEX_ADDRESS_LATCHED */
        u8 a0 = b8(s, o + 1), a1 = b8(s, o + 2), base = b8(s, o + 3), count = b8(s, o + 4);
        s->off += 5;
        u8 save = i_rdvgai(s, 0x3d4, a0);
        while (count-- && !s->stop) {
            u8 d = b8(s, s->off++);
            i_wrvgai(s, 0x3d4, a0, base++);
            i_wrvgai(s, 0x3d4, a1, d);
        }
        i_wrvgai(s, 0x3d4, a0, save);
        break;
    }
    case 0x52: {                                         /* CR */
        u8 a = b8(s, o + 1), m = b8(s, o + 2), d = b8(s, o + 3);
        s->off += 4;
        u8 v = i_rdvgai(s, 0x3d4, a) & m;
        i_wrvgai(s, 0x3d4, a, v | d);
        break;
    }
    case 0x53: { u8 a = b8(s, o + 1), d = b8(s, o + 2); s->off += 3; i_wrvgai(s, 0x3d4, a, d); break; }   /* ZM_CR */
    case 0x54: {                                         /* ZM_CR_GROUP */
        u8 count = b8(s, o + 1);
        s->off += 2;
        while (count-- && !s->stop) { u8 a = b8(s, s->off), d = b8(s, s->off + 1); s->off += 2; i_wrvgai(s, 0x3d4, a, d); }
        break;
    }
    case 0x56: {                                         /* CONDITION_TIME */
        u8 cond = b8(s, o + 1), retry = b8(s, o + 2);
        u32 wait = MIN((u32)retry * 50, 100u);
        s->off += 3;
        if (!ex(s)) break;
        while (wait-- && !s->stop) {
            if (cond_met(s, cond)) goto done56;
            i_sleep(s, 20000);
            if (s->slept_us >= 1000000) break;
        }
        ex_set(s, 0);
    done56:
        break;
    }
    case 0x57: { u16 ms = b16(s, o + 1); s->off += 3; i_sleep(s, (u32)ms * 1000); break; }   /* LTIME */
    case 0x58: {                                         /* ZM_REG_SEQUENCE */
        u32 base = b32(s, o + 1);
        u8 count = b8(s, o + 5);
        s->off += 6;
        while (count-- && !s->stop) { u32 d = b32(s, s->off); s->off += 4; i_wr(s, base, d); base += 4; }
        break;
    }
    case 0x59: skip_pll(s, "PLL_INDIRECT"); s->off += 7; break;
    case 0x5a: {                                         /* ZM_REG_INDIRECT (writes at the ROM address, as nouveau) */
        u16 addr = b16(s, o + 5);
        u32 d = b32(s, addr);
        s->off += 7;
        i_wr(s, addr, d);
        break;
    }
    case 0x5b: {                                         /* SUB_DIRECT */
        u16 addr = b16(s, o + 1);
        if (ex(s)) {
            u32 save = s->off;
            s->off = addr;
            exec_script(s);
            if (s->stop) return;
            s->off = save;
        }
        s->off += 3;
        break;
    }
    case 0x5c: {                                         /* JUMP */
        u16 to = b16(s, o + 1);
        if (ex(s)) s->off = to;
        else s->off += 3;
        break;
    }
    case 0x5e: {                                         /* I2C_IF */
        u8 index = b8(s, o + 1), addr = b8(s, o + 2), reg = b8(s, o + 3), m = b8(s, o + 4), d = b8(s, o + 5);
        s->off += 6;
        ex_force(s, 1);
        int v = i_rdi2c(s, index, addr, reg);
        if (((u8)v & m) != d) ex_set(s, 0);
        ex_force(s, 0);
        break;
    }
    case 0x5f: {                                         /* COPY_NV_REG */
        u32 sreg = b32(s, o + 1), smask = b32(s, o + 6), sxor = b32(s, o + 10), dreg = b32(s, o + 14), dmask = b32(s, o + 18);
        u8 sh = b8(s, o + 5);
        s->off += 22;
        u32 d = shiftv(i_rd(s, sreg), sh);
        i_mask(s, dreg, ~dmask, (d & smask) ^ sxor);
        break;
    }
    case 0x62: { u16 port = b16(s, o + 1); u8 index = b8(s, o + 3), d = b8(s, o + 4); s->off += 5; i_wrvgai(s, port, index, d); break; }
    case 0x63: case 0x65: case 0x66: case 0x67: case 0x68: case 0x87: case 0x8f:
        stop(s, "POST opcode %02x at %04x in a display script", opc, o);
        break;
    case 0x69: {                                         /* IO */
        u16 port = b16(s, o + 1);
        u8 m = b8(s, o + 3), d = b8(s, o + 4);
        s->off += 5;
        if (port == 0x03c3 && d == 0x01) { stop(s, "the NV50 PLL reset sequence (IO 3c3) at %04x", o); break; }
        u8 v = i_rdport(s, port) & m;
        i_wrport(s, port, d | v);
        break;
    }
    case 0x6b: {                                         /* SUB */
        u8 index = b8(s, o + 1);
        u16 tab = init_table(s, 0x00), addr = tab ? b16(s, tab + index * 2u) : 0;
        if (addr && ex(s)) {
            u32 save = s->off;
            s->off = addr;
            exec_script(s);
            if (s->stop) return;
            s->off = save;
        }
        s->off += 2;
        break;
    }
    case 0x6d: {                                         /* RAM_CONDITION */
        u8 m = b8(s, o + 1), v = b8(s, o + 2);
        s->off += 3;
        if ((i_rd(s, 0x100000) & m) != v) ex_set(s, 0);
        break;
    }
    case 0x6e: { u32 reg = b32(s, o + 1), m = b32(s, o + 5), d = b32(s, o + 9); s->off += 13; i_mask(s, reg, ~m, d); break; }   /* NV_REG */
    case 0x6f: {                                         /* MACRO */
        u8 macro = b8(s, o + 1);
        u16 tab = init_table(s, 0x04);
        if (tab) i_wr(s, b32(s, tab + macro * 8u), b32(s, tab + macro * 8u + 4));
        s->off += 2;
        break;
    }
    case 0x71: s->off = 0; break;                        /* DONE */
    case 0x72: s->off += 1; ex_set(s, 1); break;        /* RESUME */
    case 0x73: {                                         /* STRAP_CONDITION */
        u32 m = b32(s, o + 1), v = b32(s, o + 5);
        s->off += 9;
        if ((i_rd(s, 0x101000) & m) != v) ex_set(s, 0);
        break;
    }
    case 0x74: { u16 us = b16(s, o + 1); s->off += 3; i_sleep(s, us < 1000 ? us : ((u32)us + 900) / 1000 * 1000); break; }   /* TIME */
    case 0x75: { u8 c = b8(s, o + 1); s->off += 2; if (!cond_met(s, c)) ex_set(s, 0); break; }     /* CONDITION */
    case 0x76: { u8 c = b8(s, o + 1); s->off += 2; if (!io_cond_met(s, c)) ex_set(s, 0); break; }  /* IO_CONDITION */
    case 0x77: { u32 reg = b32(s, o + 1); u16 d = b16(s, o + 5); s->off += 7; i_wr(s, reg, d); break; }   /* ZM_REG16 */
    case 0x78: {                                         /* INDEX_IO */
        u16 port = b16(s, o + 1);
        u8 index = b8(s, o + 3), m = b8(s, o + 4), d = b8(s, o + 5);
        s->off += 6;
        u8 v = i_rdvgai(s, port, index) & m;
        i_wrvgai(s, port, index, d | v);
        break;
    }
    case 0x79: skip_pll(s, "PLL"); s->off += 7; break;
    case 0x7a: {                                         /* ZM_REG */
        u32 reg = b32(s, o + 1), d = b32(s, o + 5);
        s->off += 9;
        if (reg == 0x000200) d |= 1;
        i_wr(s, reg, d);
        break;
    }
    case 0x8c: case 0x8d: s->off += 1; break;            /* RESET_BEGUN / RESET_END */
    case 0x8e:                                           /* GPIO: reset every line to its default */
        s->off += 1;
        if (ex(s)) { s->skipped++; nv_evo_log(s->e, "script: GPIO reset at %04x skipped", o); }
        break;
    case 0x90: { u32 sr = b32(s, o + 1), dr = b32(s, o + 5); s->off += 9; i_wr(s, dr, i_rd(s, sr)); break; }   /* COPY_ZM_REG */
    case 0x91: {                                         /* ZM_REG_GROUP */
        u32 a = b32(s, o + 1);
        u8 count = b8(s, o + 5);
        s->off += 6;
        while (count-- && !s->stop) { i_wr(s, a, b32(s, s->off)); s->off += 4; }
        break;
    }
    case 0x92: s->off += 1; break;                       /* RESERVED */
    case 0xaa: s->off += 4; break;
    case 0x96: {                                         /* XLAT */
        u32 sa = b32(s, o + 1), da = b32(s, o + 8), dm = b32(s, o + 12);
        u8 ss = b8(s, o + 5), sm = b8(s, o + 6), index = b8(s, o + 7), sh = b8(s, o + 16);
        s->off += 17;
        u32 d = shiftv(i_rd(s, sa), ss) & sm;
        u16 tab = init_table(s, 0x10), p = tab ? b16(s, tab + index * 2u) : 0;
        u32 x = p ? b8(s, p + d) : 0;
        i_mask(s, da, ~dm, x << (sh & 31));
        break;
    }
    case 0x97: {                                         /* ZM_MASK_ADD */
        u32 a = b32(s, o + 1), m = b32(s, o + 5), add = b32(s, o + 9);
        s->off += 13;
        u32 d = i_rd(s, a);
        i_wr(s, a, (d & m) | ((d + add) & ~m));
        break;
    }
    case 0x98: {                                         /* AUXCH */
        u32 a = b32(s, o + 1);
        u8 count = b8(s, o + 5);
        s->off += 6;
        while (count-- && !s->stop) {
            u8 m = b8(s, s->off), d = b8(s, s->off + 1);
            s->off += 2;
            m = i_rdaux(s, a) & m;
            i_wraux(s, a, m | d);
        }
        break;
    }
    case 0x99: {                                         /* ZM_AUXCH */
        u32 a = b32(s, o + 1);
        u8 count = b8(s, o + 5);
        s->off += 6;
        while (count-- && !s->stop) { i_wraux(s, a, b8(s, s->off)); s->off += 1; }
        break;
    }
    case 0x9a: {                                         /* I2C_LONG_IF */
        u8 index = b8(s, o + 1), addr = (u8)(b8(s, o + 2) >> 1), lo = b8(s, o + 3), hi = b8(s, o + 4), m = b8(s, o + 5), d = b8(s, o + 6);
        s->off += 7;
        if (s->e->ops.i2c) {
            u8 in[2] = { hi, lo }, out = 0;
            nv_i2c_msg_t msg[2] = { { addr, 0, 2, in }, { addr, NV_I2C_RD, 1, &out } };
            if (!s->e->ops.i2c(s->e->ops.ctx, ccb_of(s, index), msg, 2) && (out & m) == d) break;
        }
        ex_set(s, 0);
        break;
    }
    case 0xa9: {                                         /* GPIO_NE */
        u8 count = b8(s, o + 1);
        s->off += 2 + (u32)count;
        if (ex(s)) { s->skipped++; nv_evo_log(s->e, "script: GPIO_NE at %04x skipped", o); }
        break;
    }
    default:
        stop(s, "unknown opcode %02x at %04x", opc, o);
        break;
    }
}

static void exec_script(ini_t *s) {
    if (++s->nested > 8) { stop(s, "scripts nested too deeply"); return; }
    while (s->off && !s->stop) {
        if (++s->ops > 4096) { stop(s, "more than 4096 opcodes"); break; }
        op(s);
    }
    s->nested--;
}

int nv_evo_script(nv_evo_t *e, u16 offset, const nv_init_ctx_t *c, const char *what) {
    if (!offset || !e->bios) return 0;
    if (!e->scripts) {
        nv_evo_log(e, "script %s at %04x not run (nv.scripts=0)", what, offset);
        return 0;
    }
    ini_t s;
    memset(&s, 0, sizeof s);
    s.e = e;
    s.b = e->bios;
    s.c = *c;
    s.off = offset;
    s.execute = 1;
    exec_script(&s);
    e->script_ops += (u32)s.ops;
    nv_evo_log(e, "script %s at %04x (output %d, OR %d, link %d, head %d): %d opcodes, %d writes%s%s%s%s", what, offset, c->outp,
               c->or_id, c->link, c->head, s.ops, s.writes, s.refused ? ", writes refused" : "", s.skipped ? ", some skipped" : "",
               s.stop ? ", stopped: " : "", s.stop ? s.why : "");
    return s.stop ? E_IO : 0;
}
