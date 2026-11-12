/* Halcyon OS - NVIDIA GPUs (M10a): what was found, in words.
 *
 *   - nv_log_summary: the "nvN:" lines of BOOT.LOG
 *   - nv_report_text: the full report NVREPORT.EXE saves (also the raw
 *     tables and a decoded, read-only dump of the display registers that
 *     M10b will program)
 *   - nv_sysinfo: System Information's "NVIDIA graphics" rows
 *   - SYS_NV_QUERY for NVREPORT.EXE, with the frame buffer speed test
 *
 * Nothing here writes to the GPU (the CRTC index register of pre-NV50
 * cards, needed to read CRTC registers, is put back). */
#include "nv.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/proc.h"
#include "core/sysabi.h"
#include "version.h"

void gui_full_redraw(void) __attribute__((weak));

/* ---------------- text helpers ---------------- */

typedef struct sb { char *b; int n, cap; } sb_t;

static void sp(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sp(sb_t *s, const char *fmt, ...) {
    if (s->n >= s->cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s->b + s->n, (size_t)(s->cap - s->n), fmt, ap);
    va_end(ap);
    if (r > 0) s->n += MIN(r, s->cap - 1 - s->n);
}

const char *nv_link_speed_str(int s) {
    static const char *n[] = { "?", "2.5 GT/s", "5 GT/s", "8 GT/s", "16 GT/s", "32 GT/s" };
    return s >= 1 && s <= 5 ? n[s] : "?";
}

static const char *src_short(int i) {
    static const char *n[NV_SRC_COUNT] = { "PRAMIN", "PROM", "ROMBAR", "ACPI", "SHADOW" };
    return i >= 0 && i < NV_SRC_COUNT ? n[i] : "?";
}

static void link_str(nv_device_t *d, char *b, int n) {
    if (d->fake) snprintf(b, (size_t)n, "(simulated card)");
    else if (d->link.pcie == 1)
        snprintf(b, (size_t)n, "PCI Express %s x%d (the card can do %s x%d)", nv_link_speed_str(d->link.speed), d->link.width,
                 nv_link_speed_str(d->link.max_speed), d->link.max_width);
    else if (d->link.pcie == 2) {
        u32 c = d->link.agp_cmd;
        int rate = (d->link.agp_status & 8) ? (c & 7) * 4 : (c & 7) == 4 ? 4 : (c & 7) == 2 ? 2 : 1;   /* AGP 3.0 mode: x4 / x8 */
        snprintf(b, (size_t)n, "AGP %s, %dx", (c & 0x100) ? "enabled" : "not enabled", rate);
    } else snprintf(b, (size_t)n, "PCI");
}

static void vram_str(nv_device_t *d, char *b, int n) {
    if (d->vram) snprintf(b, (size_t)n, "%llu MiB", (unsigned long long)(d->vram >> 20));
    else snprintf(b, (size_t)n, "%s", d->chip.igp ? "shared with the system" : "unknown");
}

static const char *rom_source(nv_device_t *d) { return d->rom_best >= 0 ? d->rom[d->rom_best].source : "none"; }

static const char *driver_line(nv_device_t *d) {
    static char b[160];
    return nv_display_state(d, b, sizeof b);
}

/* the connector's name: "DisplayPort", "HDMI", "DVI-I" */
static const char *conn_name(nv_device_t *d, const nv_connector_t *c) { (void)d; return nv_conn_type_str(c->type); }

static void monitor_str(const nv_connector_t *c, char *b, int n) {
    if (c->edid_len <= 0) { snprintf(b, (size_t)n, "%s", c->note[0] ? c->note : "no monitor"); return; }
    char name[64];
    edid_describe(&c->mon, name, sizeof name);
    if (c->mon.native_w) snprintf(b, (size_t)n, "%s, %dx%d at %d Hz", name, c->mon.native_w, c->mon.native_h, c->mon.native_hz);
    else snprintf(b, (size_t)n, "%s", name);
}

/* ---------------- BOOT.LOG ---------------- */

void nv_log_summary(nv_device_t *d) {
    pci_dev_t *p = d->pci;
    char l[96], v[48];
    link_str(d, l, sizeof l);
    vram_str(d, v, sizeof v);
    kprintf("nv%d: %s: %s (chipset %03x, rev %02x), %s; PCI %02x:%02x.%x %04x:%04x%s\n", d->index, d->name, d->chip.codename,
            d->chip.chipset, d->chip.rev, d->chip.family_name, p->bus, p->dev, p->func, p->vendor, p->device,
            d->fake ? " (QEMU test hook)" : "");
    kprintf("nv%d: PMC_BOOT_0 %08x BOOT_42 %08x ENABLE %08x INTR_EN %08x%s; BAR0 %llx (%s), BAR1 %llu MiB; VRAM %s [%s]; %s\n", d->index,
            d->chip.boot0, d->chip.boot42, d->pmc_enable, d->intr_en, d->irq_on ? " (I2C / AUX left alone)" : "",
            (unsigned long long)d->mmio_phys,
            d->uncached ? "uncached" : "firmware's memory type", (unsigned long long)(d->bar1_size >> 20), v, d->vram_how, l);
    for (int i = 0; i < NV_SRC_COUNT; i++)
        kprintf("nv%d: video BIOS %s%s: %s (%u ms)\n", d->index, d->rom[i].source, i == d->rom_best ? " [used]" : "",
                d->rom[i].status[0] ? d->rom[i].status : "not tried", d->rom[i].us / 1000);
    nv_bios_t *b = d->bios;
    if (!b) {
        kprintf("nv%d: no usable video BIOS: outputs and connectors unknown\n", d->index);
    } else {
        kprintf("nv%d: VBIOS %s, %s %d.%d, DCB %x.%x (%d outputs), I2C/CCB %x.%x (%d), connectors %x.%x (%d), GPIO %x.%x (%d)%s\n",
                d->index, b->version_str[0] ? b->version_str : "?", b->bit_offset ? "BIT" : "BMP",
                b->bit_offset ? b->bit_ver_major : b->bmp_major, b->bit_offset ? b->bit_ver_minor : b->bmp_minor,
                b->dcb_ver >> 4, b->dcb_ver & 15, b->noutputs, b->ccb_ver >> 4, b->ccb_ver & 15, b->nccb,
                b->conn_ver >> 4, b->conn_ver & 15, b->nconn, b->gpio_ver >> 4, b->gpio_ver & 15, b->ngpio,
                b->problems ? " (malformed parts: see NVREPORT)" : "");
        for (int i = 0; i < b->noutputs; i++) {
            const nv_dcb_output_t *o = &b->out[i];
            kprintf("nv%d: output %d: %s, connector %d, I2C %d, heads %x, OR %x, link %x%s [%08x %08x]\n", d->index, o->index,
                    nv_out_type_str(o->type), o->connector, o->i2c, o->heads, o->or_mask, o->link,
                    o->location ? " (external encoder)" : "", o->raw[0], o->raw[1]);
        }
    }
    for (int i = 0; i < d->nconn; i++) {
        nv_connector_t *c = &d->conn[i];
        char m[96];
        monitor_str(c, m, sizeof m);
        kprintf("nv%d: connector %d: %s, hot plug %s: %s%s%s%s\n", d->index, c->index, conn_name(d, c),
                c->hpd > 0 ? "yes" : c->hpd == 0 ? "no" : "?", m, c->via[0] ? " (" : "", c->via, c->via[0] ? ")" : "");
    }
    kprintf("nv%d: identified in %u ms, %u timeouts\n", d->index, d->init_ms, d->timeouts);
}

/* ---------------- registers for M10b (read only) ---------------- */

static u32 R(nv_device_t *d, u32 reg) { return nv_rd32(d, reg); }

static void dump(sb_t *s, nv_device_t *d, u32 from, u32 to) {
    for (u32 a = from; a < to; a += 16)
        sp(s, "    %06x: %08x %08x %08x %08x\n", a, R(d, a), R(d, a + 4), R(d, a + 8), R(d, a + 12));
}

static const char *sor_proto(int p, int dp) {
    switch (p) {
    case 0: return "LVDS";
    case 1: return "TMDS link A";
    case 2: return "TMDS link B";
    case 5: return "TMDS dual link";
    case 8: return dp ? "DP link A" : "?";
    case 9: return dp ? "DP link B" : "?";
    }
    return "?";
}

/* 10d: each head's base (page flips) and cursor channel, and its scanline
 * counter. A channel's PUT / GET or FREE are read only while its control
 * register says it runs. */
static void chans(sb_t *s, nv_device_t *d, int gf, u32 heads) {
    u32 c0 = gf ? 0x610490 : 0x610200;
    int gp102 = d->chip.chipset >= 0x132;
    sp(s, "    base and cursor channels, scanline (10d):\n");
    for (int h = 0; h < 4; h++) {
        if (!(heads & (1u << h))) continue;
        u32 bc = R(d, c0 + (u32)(1 + h) * 0x10), bp = R(d, (gp102 ? 0x611494 : gf ? 0x610494 : 0x610204) + (u32)(1 + h) * 0x10);
        int cctl = gf ? 13 + h : 7 + h, cuser = gf ? (gp102 ? 17 : 13) + h : 7 + h;
        u32 cc = R(d, c0 + (u32)cctl * 0x10);
        sp(s, "    head %d: base channel %08x push %08x", h, bc, bp);
        if (!nv_bad_read(bc) && (bc & 3)) sp(s, " PUT %x GET %x", R(d, 0x640000 + (u32)(1 + h) * 0x1000), R(d, 0x640004 + (u32)(1 + h) * 0x1000));
        sp(s, "; cursor channel %08x", cc);
        if (!nv_bad_read(cc) && (cc & 1)) sp(s, " FREE %08x", R(d, 0x640008 + (u32)cuser * 0x1000));
        sp(s, "; scanline %u\n", R(d, 0x616340 + (u32)h * 0x800) & 0xFFFF);
    }
}

static void regs_gf119(sb_t *s, nv_device_t *d) {
    u32 caps = R(d, 0x612004), nheads = R(d, 0x022448);
    sp(s, "  Display engine (GF119 / Kepler / Maxwell / Pascal layout), armed state of the core channel:\n");
    sp(s, "    0x612004 caps %08x: heads %x, DACs %x, SORs %02x; 0x022448 heads %u\n", caps, caps & 0xF, (caps >> 4) & 0xF,
       (caps >> 8) & 0xFF, nheads);
    sp(s, "    0x610490 core channel %08x, 0x610010 %08x, 0x6100ac %08x, 0x610088 %08x\n", R(d, 0x610490), R(d, 0x610010),
       R(d, 0x6100AC), R(d, 0x610088));
    for (int h = 0; h < 4; h++) {
        if (!(caps & (1u << h)) && h >= 2) continue;
        u32 o = (u32)h * 0x300;
        u32 tot = R(d, 0x640414 + o), se = R(d, 0x640418 + o), be = R(d, 0x64041C + o), bs = R(d, 0x640420 + o);
        u32 clk = R(d, 0x640450 + o), ctl = R(d, 0x640404 + o);
        int aw = (int)(bs & 0xFFFF) - (int)(be & 0xFFFF), ah = (int)(bs >> 16) - (int)(be >> 16);
        u64 t = (u64)(tot & 0xFFFF) * (tot >> 16);
        sp(s, "    head %d: total %ux%u, sync end %u,%u, blank end %u,%u, blank start %u,%u -> active %dx%d, pixel clock %u Hz",
           h, tot & 0xFFFF, tot >> 16, se & 0xFFFF, se >> 16, be & 0xFFFF, be >> 16, bs & 0xFFFF, bs >> 16, aw, ah, clk);
        if (t && clk && !nv_bad_read(clk)) sp(s, " (%llu Hz refresh)", (unsigned long long)((clk + t / 2) / t));
        sp(s, ", control %08x (depth field %u)\n", ctl, (ctl >> 6) & 0xF);
        dump(s, d, 0x640400 + o, 0x640500 + o);
    }
    for (int i = 0; i < 8; i++) {
        if (!(caps & (1u << (8 + i)))) continue;
        u32 c = R(d, 0x640200 + (u32)i * 0x20);
        sp(s, "    SOR%d: control %08x (%s, heads %x), power 0x%06x = %08x, sequencer 0x%06x = %08x\n", i, c,
           (c & 0xF) ? sor_proto((c >> 8) & 0xF, 1) : "off", c & 0xF, 0x61C004 + i * 0x800, R(d, 0x61C004 + (u32)i * 0x800),
           0x61C030 + i * 0x800, R(d, 0x61C030 + (u32)i * 0x800));
    }
    for (int i = 0; i < 4; i++) {
        if (!(caps & (1u << (4 + i)))) continue;
        u32 c = R(d, 0x640180 + (u32)i * 0x20);
        sp(s, "    DAC%d: control %08x (%s, heads %x), power 0x%06x = %08x\n", i, c, (c & 0xF) ? "CRT" : "off", c & 0xF,
           0x61A004 + i * 0x800, R(d, 0x61A004 + (u32)i * 0x800));
    }
    sp(s, "    core channel methods (armed, 0x640000):\n");
    dump(s, d, 0x640000, 0x640100);
    sp(s, "    PDISP 0x610000:\n");
    dump(s, d, 0x610000, 0x610100);
    chans(s, d, 1, caps & 0xF);
}

static void regs_nv50(sb_t *s, nv_device_t *d) {
    u32 caps = R(d, 0x610184);
    sp(s, "  Display engine (NV50 layout: G80-GT21x, GF100-GF116), armed state:\n");
    sp(s, "    0x610184 caps %08x: DACs %x, SORs %x, PIORs %x; 0x610200 core channel %08x; 0x610024 %08x; 0x610030 %08x\n",
       caps, (caps >> 20) & 7, (caps >> 24) & 0xF, (caps >> 28) & 7, R(d, 0x610200), R(d, 0x610024), R(d, 0x610030));
    for (int h = 0; h < 2; h++) {
        u32 o = (u32)h * 0x540 + 4;
        u32 be = R(d, 0x610AE8 + o), bs = R(d, 0x610AF0 + o), tot = R(d, 0x610AF8 + o), se = R(d, 0x610B00 + o);
        u32 khz = R(d, 0x610AD0 + o) & 0x3FFFFF;
        u64 t = (u64)(tot & 0xFFFF) * (tot >> 16);
        sp(s, "    head %d: total %ux%u, sync end %u,%u, blank end %u,%u, blank start %u,%u -> active %dx%d, pixel clock %u kHz",
           h, tot & 0xFFFF, tot >> 16, se & 0xFFFF, se >> 16, be & 0xFFFF, be >> 16, bs & 0xFFFF, bs >> 16,
           (int)(bs & 0xFFFF) - (int)(be & 0xFFFF), (int)(bs >> 16) - (int)(be >> 16), khz);
        if (t && khz) sp(s, " (%llu Hz)", (unsigned long long)(((u64)khz * 1000 + t / 2) / t));
        sp(s, "\n");
    }
    for (int i = 0; i < 4; i++) {
        u32 c = R(d, 0x610B70 + (u32)i * 8 + 4);
        sp(s, "    SOR%d: control %08x (%s, heads %x)\n", i, c, (c & 3) ? sor_proto((c >> 8) & 0xF, d->chip.chipset >= 0x94) : "off", c & 3);
    }
    for (int i = 0; i < 3; i++) {
        u32 c = R(d, 0x610B58 + (u32)i * 8 + 4), p = R(d, 0x610B80 + (u32)i * 8 + 4);
        sp(s, "    DAC%d: control %08x (heads %x)   PIOR%d: control %08x (heads %x)\n", i, c, c & 3, i, p, p & 3);
    }
    sp(s, "    PDISP 0x610000 and 0x610a80:\n");
    dump(s, d, 0x610000, 0x610100);
    dump(s, d, 0x610A80, 0x610C00);
    chans(s, d, 0, 3);
}

static void regs_nv04(sb_t *s, nv_device_t *d) {
    sp(s, "  Display (VGA-style CRTCs and RAMDACs, NV04 layout):\n");
    for (int h = 0; h < 2; h++) {
        u32 o = (u32)h * 0x2000;
        sp(s, "    PCRTC%d: start %08x, config %08x, intr_en %08x, 0x600808 %08x\n", h, R(d, 0x600800 + o), R(d, 0x600804 + o),
           R(d, 0x600140 + o), R(d, 0x600808 + o));
        sp(s, "    PRAMDAC%d: VPLL %08x, PLL select %08x, general %08x; flat panel:\n", h, R(d, 0x680508 + o), R(d, 0x68050C),
           R(d, 0x680600 + o));
        dump(s, d, 0x680800 + o, 0x680850 + o);
    }
    /* CRTC registers through the index register at 0x6013d4 (put back) */
    u8 crx = nv_rd08(d, 0x6013D4);
    nv_wr08(d, 0x6013D4, 0x1F);
    u8 lock = nv_rd08(d, 0x6013D5);
    sp(s, "    CRTC registers (head 0, CR1F lock %02x)%s:\n", lock, lock ? "" : " (extended registers locked: read as is)");
    for (int i = 0; i < 0xA0; i += 16) {
        sp(s, "    CR%02X:", i);
        for (int k = 0; k < 16; k++) { nv_wr08(d, 0x6013D4, (u8)(i + k)); sp(s, " %02x", nv_rd08(d, 0x6013D5)); }
        sp(s, "\n");
    }
    nv_wr08(d, 0x6013D4, crx);
}

static void regs_i2c(sb_t *s, nv_device_t *d) {
    switch (d->chip.i2c) {
    case NV_I2C_GM200:
        sp(s, "  I2C ports 0x00d014 + 0x20 n, AUX channels 0x00d950 + 0x50 n (addr, ctrl, stat), pads 0x00d970 / 0x00d97c + 0x50 n:\n");
        for (int i = 0; i < 8; i++)
            sp(s, "    %d: i2c %08x  aux %08x %08x %08x  pad %08x %08x\n", i, R(d, 0xD014 + (u32)i * 0x20),
               R(d, 0xD950 + (u32)i * 0x50), R(d, 0xD954 + (u32)i * 0x50), R(d, 0xD958 + (u32)i * 0x50),
               R(d, 0xD970 + (u32)i * 0x50), R(d, 0xD97C + (u32)i * 0x50));
        break;
    case NV_I2C_GF119: case NV_I2C_G94:
        sp(s, "  I2C ports, AUX channels 0x00e4e0 + 0x50 n (addr, ctrl, stat), pads 0x00e500 / 0x00e50c + 0x50 n:\n");
        for (int i = 0; i < 4; i++)
            sp(s, "    %d: i2c %08x  aux %08x %08x %08x  pad %08x %08x\n", i,
               d->chip.i2c == NV_I2C_GF119 ? R(d, 0xD014 + (u32)i * 0x20) : R(d, 0xE138 + (u32)i * 0x18),
               R(d, 0xE4E0 + (u32)i * 0x50), R(d, 0xE4E4 + (u32)i * 0x50), R(d, 0xE4E8 + (u32)i * 0x50),
               R(d, 0xE500 + (u32)i * 0x50), R(d, 0xE50C + (u32)i * 0x50));
        break;
    case NV_I2C_NV50:
        sp(s, "  I2C ports 0x00e138 + 0x18 n: %08x %08x %08x %08x\n", R(d, 0xE138), R(d, 0xE150), R(d, 0xE168), R(d, 0xE180));
        break;
    }
    if (d->chip.gpio == NV_GPIO_GF119) {
        sp(s, "  GPIO 0x00d610 + 4 n:\n");
        dump(s, d, 0xD610, 0xD690);
    } else if (d->chip.gpio == NV_GPIO_NV50) {
        sp(s, "  GPIO 0x00e104 %08x, 0x00e108 %08x, 0x00e280 %08x, 0x00e284 %08x\n", R(d, 0xE104), R(d, 0xE108), R(d, 0xE280), R(d, 0xE284));
    } else if (d->chip.gpio == NV_GPIO_NV10) {
        sp(s, "  GPIO 0x600818 %08x, 0x60081c %08x, 0x600850 %08x\n", R(d, 0x600818), R(d, 0x60081C), R(d, 0x600850));
    }
}

static void regs(sb_t *s, nv_device_t *d) {
    sp(s, "\nRegisters (read only; for writing the M10b driver)\n");
    if (d->fake) { sp(s, "  (simulated card: no registers)\n"); return; }
    sp(s, "  PMC: BOOT_0 %08x, BOOT_1 %08x, BOOT_42 %08x, ENABLE %08x, INTR %08x, INTR_EN %08x; strap 0x101000 %08x\n", R(d, 0),
       R(d, 4), d->chip.boot42, R(d, 0x200), R(d, 0x100), R(d, 0x140), R(d, 0x101000));
    sp(s, "  PCI command %04x, status %04x\n", pci_read16(d->pci, 0x04), pci_read16(d->pci, 0x06));
    sp(s, "  PFB: 0x100200 %08x, 0x100204 %08x, 0x10020c %08x", R(d, 0x100200), R(d, 0x100204), R(d, 0x10020C));
    if (d->chip.family >= NV_FAM_FERMI)
        sp(s, "; FBP 0x022438 %08x 0x02243c %08x 0x022554 %08x 0x022450 %08x 0x022458 %08x 0x021c14 %08x 0x021d38 %08x",
           R(d, 0x022438), R(d, 0x02243C), R(d, 0x022554), R(d, 0x022450), R(d, 0x022458), R(d, 0x021C14), R(d, 0x021D38));
    if (d->chip.family >= NV_FAM_MAXWELL) sp(s, "; 0x100ce0 %08x", R(d, 0x100CE0));
    sp(s, "\n");
    if (d->chip.family >= NV_FAM_FERMI)
        sp(s, "  display fuse: 0x022500 %08x, 0x021c04 %08x\n", R(d, 0x022500), d->chip.chipset >= 0x110 ? R(d, 0x021C04) : 0);
    if (d->chip.disp == NV_DISP_NV50 || d->chip.disp == NV_DISP_GF119)
        sp(s, "  VBIOS shadow window 0x619f04 %08x, PRAMIN window 0x001700 %08x\n", R(d, 0x619F04), R(d, 0x001700));
    int fused = d->chip.family >= NV_FAM_FERMI && (R(d, d->chip.chipset >= 0x110 ? 0x021C04 : 0x022500) & 1) &&
                !nv_bad_read(R(d, d->chip.chipset >= 0x110 ? 0x021C04 : 0x022500));
    if (fused) sp(s, "  The display engine is fused off: no display registers read.\n");
    else if (d->chip.disp == NV_DISP_GF119) regs_gf119(s, d);
    else if (d->chip.disp == NV_DISP_NV50) regs_nv50(s, d);
    else if (d->chip.disp == NV_DISP_NV04) regs_nv04(s, d);
    regs_i2c(s, d);
}

/* ---------------- the report ---------------- */

static void hexdump(sb_t *s, const u8 *p, int n) {
    for (int i = 0; i < n; i += 16) {
        sp(s, "      %03x:", i);
        for (int k = 0; k < 16 && i + k < n; k++) sp(s, " %02x", p[i + k]);
        sp(s, "\n");
    }
}

static void bios_part(sb_t *s, nv_device_t *d) {
    sp(s, "\nVideo BIOS\n");
    for (int i = 0; i < NV_SRC_COUNT; i++) {
        nv_rom_copy_t *c = &d->rom[i];
        sp(s, "  %-20s %s%s", c->source, i == d->rom_best ? "USED: " : "", c->status[0] ? c->status : "not tried");
        if (c->data && c->size) sp(s, "  -> VBIOS_%s.ROM (%u bytes, %u ms)", src_short(i), c->size, c->us / 1000);
        sp(s, "\n");
    }
    nv_bios_t *b = d->bios;
    if (!b) { sp(s, "  No usable image: the outputs and connectors are unknown.\n"); return; }
    nv_rom_t *r = &b->rom;
    sp(s, "  Version %s, %s", b->version_str[0] ? b->version_str : "unknown", b->bit_offset ? "BIT" : b->bmp_offset ? "BMP" : "no BIT / BMP");
    if (b->bit_offset) sp(s, " %d.%d at 0x%x", b->bit_ver_major, b->bit_ver_minor, b->bit_offset);
    if (b->bmp_offset) sp(s, " %d.%d at 0x%x", b->bmp_major, b->bmp_minor, b->bmp_offset);
    sp(s, ", feature byte %02x%s\n", b->feature, (b->feature & 0x10) ? " (mobile)" : "");
    if (r->ifr_version) sp(s, "  IFR header version %u: the PCI images start at 0x%x\n", r->ifr_version, r->ifr_offset);
    for (int i = 0; i < r->nimages; i++) {
        nv_rom_image_t *im = &r->img[i];
        sp(s, "  image %d at 0x%05x: %u bytes, type %02x (%s), PCI %04x:%04x%s%s%s\n", i, im->offset, im->size, im->type,
           im->type == 0 ? "PC-AT" : im->type == 3 ? "EFI" : im->type == 0xE0 ? "NVIDIA data" : im->type == 0x70 ? "NBSI" : "?",
           im->vendor, im->device, im->npde ? ", NPDE" : "", im->type == 0 ? (im->checksum_ok ? ", checksum ok" : ", CHECKSUM WRONG") : "",
           im->last ? ", last" : "");
    }
    if (r->ext_base) sp(s, "  pointers past 0x%x go to the data image at 0x%x\n", r->legacy_size, r->ext_base);
    if (b->ntokens) {
        sp(s, "  BIT tokens:");
        for (int i = 0; i < b->ntokens; i++)
            sp(s, "%s '%c' v%d %u@%04x", i % 6 ? "," : "\n   ", b->tok[i].id >= 32 && b->tok[i].id < 127 ? b->tok[i].id : '?',
               b->tok[i].ver, b->tok[i].size, b->tok[i].ptr);
        sp(s, "\n");
    }
    if (b->problems) {
        sp(s, "  Problems found:");
        static const char *pn[] = { "no BIT or BMP", "BIT header", "no DCB", "DCB header", "I2C/CCB table", "connector table",
                                    "GPIO table", "pointers outside the image", "more entries than kept", "BMP checksum" };
        for (int i = 0; i < 10; i++) if (b->problems & (1u << i)) sp(s, " [%s]", pn[i]);
        sp(s, " (%u out of range reads)\n", b->oob);
    }
    sp(s, "\nDevice Control Block (outputs)\n");
    if (!b->dcb_offset) { sp(s, "  none\n"); return; }
    sp(s, "  DCB %d.%d at 0x%x: header %u bytes, %u entries of %u bytes, flags %02x\n", b->dcb_ver >> 4, b->dcb_ver & 15,
       b->dcb_offset, b->dcb_hdr, b->dcb_cnt, b->dcb_len, b->dcb_flags);
    for (int i = 0; i < b->noutputs; i++) {
        const nv_dcb_output_t *o = &b->out[i];
        sp(s, "  #%d %-18s connector %d, I2C/CCB %d, heads %x, bus %d, OR/pad %x, link %x%s", o->index, nv_out_type_str(o->type),
           o->connector, o->i2c, o->heads, o->bus, o->or_mask, o->link, o->location ? ", external encoder" : "");
        if (o->extdev) sp(s, " %02x", o->extdev);
        if (o->type == NV_OUT_DP) {
            static const char *rate[] = { "1.62", "2.7", "5.4", "8.1", "?", "?", "?", "?" };
            sp(s, ", %d lanes up to %s Gbit/s", o->dp_max_lanes, rate[o->dp_max_rate & 7]);
        }
        if (o->hdmi) sp(s, ", HDMI");
        if (o->edid_src) sp(s, ", EDID from %s", o->edid_src == 1 ? "straps / VBIOS" : "ACPI / SBIOS");
        sp(s, "  [%08x %08x]\n", o->raw[0], o->raw[1]);
    }
    sp(s, "\nI2C / Communications Control Block\n");
    if (b->ccb_offset) sp(s, "  version %d.%d at 0x%x, %u entries, primary %u, secondary %u\n", b->ccb_ver >> 4, b->ccb_ver & 15,
                          b->ccb_offset, b->ccb_cnt, b->ccb_primary, b->ccb_secondary);
    for (int i = 0; i < b->nccb; i++) {
        const nv_ccb_entry_t *c = &b->ccb[i];
        if (c->type == NV_CCB_UNUSED) continue;
        sp(s, "  #%d %s", i, nv_ccb_type_str(c->type));
        if (c->drive != 0xFF) sp(s, ", %s %d", c->type == NV_CCB_NV04_BIT ? "CRTC registers" : "I2C port", c->drive);
        if (c->type == NV_CCB_NV04_BIT) sp(s, "/%d", c->sense);
        if (c->auxch != 0xFF) sp(s, ", AUX channel %d", c->auxch);
        if (c->share != 0xFF) sp(s, ", hybrid pad %d", c->share);
        sp(s, "  [%08x]%s\n", c->raw, d->fake || nv_ccb_has_i2c(d, i) || nv_ccb_has_aux(d, i) ? "" : "  (not driven by this code)");
    }
    sp(s, "\nConnectors\n");
    if (b->conn_offset) sp(s, "  table %d.%d at 0x%x, %u entries, platform %02x\n", b->conn_ver >> 4, b->conn_ver & 15, b->conn_offset,
                           b->conn_cnt, b->platform);
    else sp(s, "  (no connector table: made up from the outputs)\n");
    for (int i = 0; i < b->nconn; i++) {
        const nv_conn_entry_t *c = &b->conn[i];
        if (c->type == 0xFF) continue;
        sp(s, "  #%d %s, location %d", i, nv_conn_type_str(c->type), c->location);
        if (c->hpd) {
            sp(s, ", hot plug");
            for (int k = 0; k < 7; k++) if (c->hpd & (1 << k)) sp(s, " %c", 'A' + k);
        }
        if (c->dp2dvi) sp(s, ", DP-to-DVI %x", c->dp2dvi);
        if (c->di) sp(s, ", AUX/I2C select %x", c->di);
        sp(s, "  [%08x]\n", c->raw);
    }
    if (b->ngpio) {
        sp(s, "\nGPIO (version %d.%d, %u entries)\n", b->gpio_ver >> 4, b->gpio_ver & 15, b->gpio_cnt);
        for (int i = 0; i < b->ngpio; i++) {
            const nv_gpio_entry_t *g = &b->gpio[i];
            const char *f = nv_gpio_func_str(g->func);
            sp(s, "  line %2d: function %02x %-20s off %d on %d%s  [%08x]\n", g->line, g->func, f ? f : "", g->log[0], g->log[1],
               g->param ? " pwm" : "", g->raw);
        }
    }
}

static void monitors(sb_t *s, nv_device_t *d) {
    sp(s, "\nMonitors\n");
    if (!d->nconn) { sp(s, "  no connectors known\n"); return; }
    if (d->i2c_off) sp(s, "  (I2C and AUX skipped: nv.i2c=0)\n");
    for (int i = 0; i < d->nconn; i++) {
        nv_connector_t *c = &d->conn[i];
        char m[96];
        monitor_str(c, m, sizeof m);
        sp(s, "  connector %d (%s), hot plug sense %s: %s\n", c->index, conn_name(d, c),
           c->hpd > 0 ? "plugged in" : c->hpd == 0 ? "nothing" : "unknown", m);
        if (c->edid_len <= 0) continue;
        sp(s, "    EDID: %d bytes, read through %s, file EDID%d.BIN\n", c->edid_len, c->via, c->index);
        sp(s, "    manufacturer %s, product %04X, year %d, size %d x %d mm\n", c->mon.vendor, c->mon.product, c->mon.year,
           c->mon.width_mm, c->mon.height_mm);
        int blocks = edid_block_count(c->edid, c->edid_len);
        for (int k = 1; k < blocks; k++) sp(s, "    block %d: %s\n", k, edid_ext_name(c->edid[k * 128]));
        edid_timing_t t[24];
        int n = edid_detailed_timings(c->edid, c->edid_len, t, 24);
        for (int k = 0; k < n; k++) {
            char ts[64];
            edid_timing_str(&t[k], ts, sizeof ts);
            sp(s, "    %s%s, h %d+%d+%d, v %d+%d+%d (block %d)\n", ts, t[k].preferred ? " (preferred)" : "", t[k].hfront, t[k].hsync,
               t[k].hblank - t[k].hfront - t[k].hsync, t[k].vfront, t[k].vsync, t[k].vblank - t[k].vfront - t[k].vsync, t[k].block);
        }
    }
}

int nv_report_text(nv_device_t *d, char *buf, int size) {
    sb_t s = { buf, 0, size };
    if (size < 1) return 0;
    buf[0] = 0;
    pci_dev_t *p = d->pci;
    char l[96], v[48];
    link_str(d, l, sizeof l);
    vram_str(d, v, sizeof v);
    sp(&s, "Card %d: %s%s\n", d->index, d->name, d->fake ? "   *** simulated by the QEMU test hook ***" : "");
    sp(&s, "  PCI %02x:%02x.%x, ID %04x:%04x, subsystem %04x:%04x, revision %02x, class %02x.%02x.%02x\n", p->bus, p->dev, p->func,
       p->vendor, p->device, p->subsys_vendor, p->subsys_id, p->revision, p->class_code, p->subclass, p->prog_if);
    sp(&s, "  Chip: %s (chipset 0x%03x, revision %02x), %s family, %s%s\n", d->chip.codename, d->chip.chipset, d->chip.rev,
       d->chip.family_name, d->chip.series, d->chip.igp ? ", integrated" : "");
    if (d->chip.note) sp(&s, "  %s\n", d->chip.note);
    sp(&s, "  PMC_BOOT_0 %08x%s, PMC_BOOT_42 %08x\n", d->chip.boot0, d->chip.from_boot42 ? " (chip taken from BOOT_42)" : "", d->chip.boot42);
    sp(&s, "  Video memory: %s (%s)\n", v, d->vram_how[0] ? d->vram_how : "not read");
    for (int i = 0; i < 6; i++)
        if (p->bar[i].addr || p->bar[i].size)
            sp(&s, "  BAR%d: %s %llx, %llu KiB%s%s\n", i, p->bar[i].is_io ? "I/O" : "memory", (unsigned long long)p->bar[i].addr,
               (unsigned long long)(p->bar[i].size >> 10), p->bar[i].is_64 ? ", 64-bit" : "", p->bar[i].prefetch ? ", prefetchable" : "");
    sp(&s, "  BAR0 mapping: %s\n", d->fake ? "none" : d->uncached ? "uncached (set by Halcyon)" : "the firmware's memory type (MTRRs)");
    sp(&s, "  Bus: %s%s\n", l, d->link.pcie == 1 && d->chip.family >= NV_FAM_KEPLER ? " (the GPU slows the link down when idle)" : "");
    static const char *dn[] = { "none", "NV04 (VGA CRTCs; modesetting is M10c, deferred)", "NV50 EVO (M10b)", "GF119 EVO (M10b)",
                                "newer than Pascal (not supported)" };
    static const char *in[] = { "none", "CRTC-indexed", "NV4E", "NV50", "G94", "GF119", "GM200" };
    sp(&s, "  Display engine: %s; I2C/AUX layout: %s\n", dn[d->chip.disp], in[d->chip.i2c]);
    sp(&s, "  Display driver: %s\n", driver_line(d));
    sp(&s, "  Probe took %u ms, %u timeouts\n", d->init_ms, d->timeouts);
    if (d->chip.in_range || d->fake) {
        bios_part(&s, d);
        monitors(&s, d);
        regs(&s, d);
        nv_bios_t *b = d->bios;
        if (b && b->dcb_offset) {
            sp(&s, "\nRaw DCB header and entries\n");
            u8 tmp[256];
            int n = MIN(256, b->dcb_hdr + b->dcb_cnt * b->dcb_len);
            for (int i = 0; i < n; i++) tmp[i] = nv_rom_rd8(&b->rom, b->dcb_offset + (u32)i, NULL);
            hexdump(&s, tmp, n);
        }
        char hb[1536];
        if (nv_display_heads(d, hb, sizeof hb) > 0) sp(&s, "\nHeads: page flips, hardware cursor, vertical blank (M10d)\n%s", hb);
        const char *lg = nv_display_log(d);
        if (lg) sp(&s, "\nNative modesetting (M10b, M10d): the driver's log of this start\n%s", lg);
    }
    return s.n;
}

/* ---------------- System Information ---------------- */

void nv_sysinfo(nv_row_fn add, void *ctx) {
    char *dt = kmalloc(2048), row[128];
    if (!dt) return;
    for (int g = 0; g < nv_count(); g++) {
        nv_device_t *d = nv_get(g);
        char l[96], v[48];
        link_str(d, l, sizeof l);
        vram_str(d, v, sizeof v);
        sb_t s = { dt, 0, 2048 };
        sp(&s, "%s%s\n\nChip: %s (chipset %03X), %s\n", d->name, d->fake ? " (simulated by the QEMU test hook)" : "",
           d->chip.codename, d->chip.chipset, d->chip.family_name);
        sp(&s, "Product family: %s\n", d->chip.series);
        if (d->chip.note) sp(&s, "%s\n", d->chip.note);
        sp(&s, "Video memory: %s\n", v);
        if (d->bios) sp(&s, "Video BIOS: %s (from %s)\n", d->bios->version_str[0] ? d->bios->version_str : "unknown version", rom_source(d));
        else if (d->chip.in_range) sp(&s, "Video BIOS: not found\n");
        sp(&s, "Bus: %s\n", l);
        sp(&s, "Connectors: %d\n\nDisplay driver: %s\n\nNVREPORT in the Command Prompt saves a full report in "
           "C:\\HALCYON\\NVREPORT.", d->nconn, driver_line(d));
        if (d->vram) snprintf(row, sizeof row, "%s (%s, %s)", d->name, d->chip.codename, v);
        else snprintf(row, sizeof row, "%s (%s)", d->name, d->chip.codename);
        add(ctx, 1, d->chip.in_range ? NV_ICON_CARD : NV_ICON_WARNING, dt, row);
        for (int i = 0; i < d->nconn; i++) {
            nv_connector_t *c = &d->conn[i];
            char m[96];
            monitor_str(c, m, sizeof m);
            s.n = 0;
            sp(&s, "Connector %d: %s\n\nOutputs:", c->index, conn_name(d, c));
            for (int k = 0; k < c->nout; k++) sp(&s, "%s %s", k ? "," : "", nv_out_type_str(d->bios->out[c->out[k]].type));
            sp(&s, "\nHot plug sense: %s\n", c->hpd > 0 ? "plugged in" : c->hpd == 0 ? "nothing plugged in" : "unknown");
            if (c->edid_len > 0) {
                char name[64];
                edid_describe(&c->mon, name, sizeof name);
                sp(&s, "\nMonitor: %s\nManufacturer: %s, product %04X", name, c->mon.vendor, c->mon.product);
                if (c->mon.year) sp(&s, ", %d", c->mon.year);
                if (c->mon.native_w) sp(&s, "\nNative mode: %dx%d at %d Hz", c->mon.native_w, c->mon.native_h, c->mon.native_hz);
                if (c->mon.width_mm) sp(&s, "\nSize: %d x %d mm", c->mon.width_mm, c->mon.height_mm);
                sp(&s, "\nEDID: %d bytes, read through %s", c->edid_len, c->via);
            } else {
                sp(&s, "\nMonitor: %s", m);
            }
            snprintf(row, sizeof row, "%s: %s", conn_name(d, c), c->edid_len > 0 ? m : c->hpd > 0 ? "plugged in, no EDID" :
                     c->hpd == 0 ? "nothing connected" : "no monitor found");
            add(ctx, 2, c->edid_len > 0 ? NV_ICON_MONITOR : NV_ICON_CONNECTOR, dt, row);
        }
    }
    kfree(dt);
}

/* ---------------- SYS_NV_QUERY and the frame buffer speed test ---------------- */

/* the system part of the report: every display adapter, the frame buffer */
static int system_text(char *buf, int size) {
    sb_t s = { buf, 0, size };
    if (size < 1) return 0;
    buf[0] = 0;
    sp(&s, "Halcyon %s, booted by %s\n", OS_VERSION, g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "a PC BIOS");
    sp(&s, "Frame buffer: %ux%u, %u bpp, pitch %u, at %llx (%s)\n", g_bootinfo.fb_width, g_bootinfo.fb_height, g_bootinfo.fb_bpp,
       g_bootinfo.fb_pitch, (unsigned long long)g_bootinfo.fb_addr, mm_cache_type_str(mm_cache_type(g_bootinfo.fb_addr)));
    sp(&s, "Display adapters on the PCI bus:\n");
    int n = 0;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 3) continue;
        n++;
        sp(&s, "  %02x:%02x.%x %04x:%04x (%s), class %02x.%02x, subsystem %04x:%04x\n", p->bus, p->dev, p->func, p->vendor,
           p->device, pci_vendor_name(p->vendor), p->class_code, p->subclass, p->subsys_vendor, p->subsys_id);
    }
    if (!n) sp(&s, "  none\n");
    sp(&s, "NVIDIA cards identified: %d\n", nv_count());
    return s.n;
}

/* the test patterns: dark greys, so the screen only flickers a little */
static const u32 fills[2] = { 0x00303030, 0x00404040 };

static void fill64(volatile u8 *fb, u64 n, u32 c) {
    u64 v = (u64)c << 32 | c, q = n / 8;
    __asm__ volatile("rep stosq" : "+D"(fb), "+c"(q) : "a"(v) : "memory");
}

static void fill32(volatile u8 *fb, u64 n, u32 c) {
    volatile u32 *p = (volatile u32 *)fb;
    for (u64 i = 0; i < n / 4; i++) p[i] = c;
}

static u64 readall(volatile u8 *fb, u64 n) {
    volatile u64 *p = (volatile u64 *)fb;
    u64 x = 0;
    for (u64 i = 0; i < n / 8; i++) x += p[i];
    return x;
}

/* one test, about 200 ms (at least one pass) */
static int fbtest(int test, hal_nvfbtest_t *t) {
    volatile u8 *fb = (volatile u8 *)(uptr)g_bootinfo.fb_addr;
    u64 size = (u64)g_bootinfo.fb_pitch * g_bootinfo.fb_height;
    memset(t, 0, sizeof *t);
    t->test = test;
    t->width = g_bootinfo.fb_width;
    t->height = g_bootinfo.fb_height;
    t->pitch = g_bootinfo.fb_pitch;
    t->bpp = g_bootinfo.fb_bpp;
    t->frame_bytes = size;
    t->cache_type = mm_cache_type(g_bootinfo.fb_addr);
    if (!fb || !size) return E_NODEV;
    static const char *names[NV_FBTESTS] = { "fill, rep stosq", "fill, 32-bit stores", "RAM to screen copy (the compositor)",
                                             "screen to screen copy", "screen read", "RAM to RAM copy (for comparison)" };
    if (test < 0 || test >= NV_FBTESTS) return E_INVAL;
    strlcpy(t->name, names[test], sizeof t->name);
    u8 *ram = NULL, *ram2 = NULL;
    if (test == 2 || test == 5) {
        ram = kmalloc(size);
        if (!ram) return E_NOMEM;
        memset(ram, 0x40, size);
        if (test == 5 && !(ram2 = kmalloc(size))) { kfree(ram); return E_NOMEM; }
    }
    u64 t0 = time_us(), now = t0, bytes = 0, sink = 0;
    int pass = 0;
    do {
        switch (test) {
        case 0: fill64(fb, size, fills[pass & 1]); bytes += size; break;
        case 1: fill32(fb, size, fills[pass & 1]); bytes += size; break;
        case 2: memcpy((void *)fb, ram, size); bytes += size; break;
        case 3: {                                   /* top half <-> bottom half */
            u64 half = (u64)g_bootinfo.fb_pitch * (g_bootinfo.fb_height / 2);
            if (pass & 1) memcpy((void *)fb, (const void *)(fb + half), half);
            else memcpy((void *)(fb + half), (const void *)fb, half);
            bytes += half;
            break;
        }
        case 4: sink += readall(fb, size); bytes += size; break;
        case 5: memcpy(ram2, ram, size); bytes += size; break;
        }
        pass++;
        now = time_us();
    } while (now - t0 < 200000 && pass < 10000);
    t->bytes = bytes;
    t->us = now - t0;
    t->passes = (u32)pass;
    t->sink = (u32)sink;
    kfree(ram);
    kfree(ram2);
    if (test <= 3 && gui_full_redraw) gui_full_redraw();     /* the desktop paints itself again */
    return 0;
}

long nv_syscall(int nr, u64 op, u64 arg, u64 buf, u64 size, u64 a5, u64 a6) {
    (void)nr; (void)a5; (void)a6;
    int gpu = (int)(arg >> 8), idx = (int)(arg & 0xFF);
    nv_device_t *d = NULL;
    switch (op) {
    case NVQ_COUNT:
        return nv_count();
    case NVQ_REPORT: {
        int cap = 256 * 1024;
        char *t = kmalloc((size_t)cap);
        if (!t) return E_NOMEM;
        int n;
        if ((i64)arg < 0) n = system_text(t, cap);
        else if ((d = nv_get((int)arg))) n = nv_report_text(d, t, cap);
        else { kfree(t); return E_INVAL; }
        int r = copyout(buf, t, MIN((u64)n, size));
        kfree(t);
        return r ? r : n;
    }
    case NVQ_ROM:
        if (!(d = nv_get(gpu)) || idx >= NV_SRC_COUNT) return E_INVAL;
        if (!d->rom[idx].data || !d->rom[idx].size) return E_NOENT;
        {
            int r = copyout(buf, d->rom[idx].data, MIN((u64)d->rom[idx].size, size));
            return r ? r : (long)d->rom[idx].size;
        }
    case NVQ_ROMNAME:
        if (!(d = nv_get(gpu)) || idx >= NV_SRC_COUNT) return E_INVAL;
        {
            char nm[16];
            strlcpy(nm, src_short(idx), sizeof nm);
            int r = copyout(buf, nm, MIN((u64)strlen(nm) + 1, size));
            return r ? r : (long)strlen(nm);
        }
    case NVQ_EDID:
        if (!(d = nv_get(gpu)) || idx >= d->nconn) return E_INVAL;
        if (d->conn[idx].edid_len <= 0) return E_NOENT;
        {
            int r = copyout(buf, d->conn[idx].edid, MIN((u64)d->conn[idx].edid_len, size));
            return r ? r : (d->conn[idx].index << 16) | d->conn[idx].edid_len;
        }
    case NVQ_FBTEST: {
        hal_nvfbtest_t t;
        int r = fbtest((int)arg, &t);
        if (r == E_INVAL) return r;
        if (size < sizeof t) return E_INVAL;
        int c = copyout(buf, &t, sizeof t);
        return c ? c : r;
    }
    }
    return E_INVAL;
}
