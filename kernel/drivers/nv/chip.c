/* Halcyon OS - NVIDIA chip identification: PMC_BOOT_0 / PMC_BOOT_42 to a
 * chipset, its family, code name and register layouts; PCI device IDs to
 * card names; the VRAM size from the memory controller's registers.
 *
 * The chipset decoding and the per-generation VRAM registers follow
 * nouveau's nvkm/engine/device/base.c and nvkm/subdev/fb/ram*.c (MIT,
 * Copyright 2012 Red Hat Inc.; see THIRD_PARTY_NOTICES.md); the tables and
 * the code are Halcyon's own. Runs on the host too
 * (tests/host/nvtest.c). */
#include "nvhw.h"
#include "core/lib.h"

typedef struct chip_row {
    u16 chipset;
    const char *code;
    const char *series;
    u8 igp;
} chip_row_t;

/* every chipset nouveau knows up to Pascal, with the products built on it */
static const chip_row_t chips[] = {
    { 0x004, "NV04", "Riva TNT", 0 },
    { 0x005, "NV05", "Riva TNT2", 0 },
    { 0x010, "NV10", "GeForce 256", 0 },
    { 0x011, "NV11", "GeForce2 MX", 0 },
    { 0x015, "NV15", "GeForce2 GTS / Ti / Ultra", 0 },
    { 0x017, "NV17", "GeForce4 MX", 0 },
    { 0x018, "NV18", "GeForce4 MX (AGP 8x)", 0 },
    { 0x01a, "NV1A", "GeForce2 integrated (nForce)", 1 },
    { 0x01f, "NV1F", "GeForce4 MX integrated (nForce2)", 1 },
    { 0x020, "NV20", "GeForce3", 0 },
    { 0x025, "NV25", "GeForce4 Ti", 0 },
    { 0x028, "NV28", "GeForce4 Ti (AGP 8x)", 0 },
    { 0x02a, "NV2A", "Xbox graphics", 0 },
    { 0x030, "NV30", "GeForce FX 5800", 0 },
    { 0x031, "NV31", "GeForce FX 5600", 0 },
    { 0x034, "NV34", "GeForce FX 5200", 0 },
    { 0x035, "NV35", "GeForce FX 5900", 0 },
    { 0x036, "NV36", "GeForce FX 5700", 0 },
    { 0x040, "NV40", "GeForce 6800", 0 },
    { 0x041, "NV41", "GeForce 6800", 0 },
    { 0x042, "NV42", "GeForce 6800 (PCIe)", 0 },
    { 0x043, "NV43", "GeForce 6600", 0 },
    { 0x044, "NV44", "GeForce 6200 TurboCache", 0 },
    { 0x045, "NV45", "GeForce 6800 (PCIe)", 0 },
    { 0x046, "G72", "GeForce 7300 / 7200", 0 },
    { 0x047, "G70", "GeForce 7800", 0 },
    { 0x049, "G71", "GeForce 7900 / 7950", 0 },
    { 0x04a, "NV44A", "GeForce 6200 (AGP)", 0 },
    { 0x04b, "G73", "GeForce 7600", 0 },
    { 0x04c, "C61", "GeForce 6100 / 7025 integrated (MCP61)", 1 },
    { 0x04e, "C51", "GeForce 6100 / 6150 integrated (MCP51)", 1 },
    { 0x063, "C73", "GeForce 7050 / 7100 integrated (MCP73)", 1 },
    { 0x067, "C67", "GeForce 7050 integrated (MCP67)", 1 },
    { 0x068, "C68", "GeForce 7025 / 7050 integrated (MCP68)", 1 },
    { 0x050, "G80", "GeForce 8800", 0 },
    { 0x084, "G84", "GeForce 8600", 0 },
    { 0x086, "G86", "GeForce 8400 / 8500", 0 },
    { 0x092, "G92", "GeForce 8800 GT / 9800", 0 },
    { 0x094, "G94", "GeForce 9600", 0 },
    { 0x096, "G96", "GeForce 9500", 0 },
    { 0x098, "G98", "GeForce 8400 GS / 9300 GE", 0 },
    { 0x0a0, "GT200", "GeForce GTX 260 / 280", 0 },
    { 0x0a3, "GT215", "GeForce GT 240 / 340", 0 },
    { 0x0a5, "GT216", "GeForce GT 220 / 320", 0 },
    { 0x0a8, "GT218", "GeForce 210 / 310", 0 },
    { 0x0aa, "MCP77/MCP78", "GeForce 8100 / 8200 / 8300 integrated", 1 },
    { 0x0ac, "MCP79/MCP7A", "GeForce 9300 / 9400 / ION integrated", 1 },
    { 0x0af, "MCP89", "GeForce 320M integrated", 1 },
    { 0x0c0, "GF100", "GeForce GTX 465 / 470 / 480", 0 },
    { 0x0c1, "GF108", "GeForce GT 430 / 440 / 630", 0 },
    { 0x0c3, "GF106", "GeForce GTS 450 / GT 640", 0 },
    { 0x0c4, "GF104", "GeForce GTX 460", 0 },
    { 0x0c8, "GF110", "GeForce GTX 570 / 580", 0 },
    { 0x0ce, "GF114", "GeForce GTX 560", 0 },
    { 0x0cf, "GF116", "GeForce GTX 550 Ti / GT 545", 0 },
    { 0x0d7, "GF117", "GeForce 620M / 710M", 0 },
    { 0x0d9, "GF119", "GeForce GT 520 / 610", 0 },
    { 0x0e4, "GK104", "GeForce GTX 660 Ti / 670 / 680 / 760 / 770", 0 },
    { 0x0e6, "GK106", "GeForce GTX 650 Ti / 660", 0 },
    { 0x0e7, "GK107", "GeForce GT 640 / GTX 650", 0 },
    { 0x0ea, "GK20A", "Tegra K1", 1 },
    { 0x0f0, "GK110", "GeForce GTX 780 / Titan", 0 },
    { 0x0f1, "GK110B", "GeForce GTX 780 Ti / Titan Black", 0 },
    { 0x106, "GK208B", "GeForce GT 710 / 720", 0 },
    { 0x108, "GK208", "GeForce GT 630 / 640 (rev. 2)", 0 },
    { 0x117, "GM107", "GeForce GTX 745 / 750 / 750 Ti", 0 },
    { 0x118, "GM108", "GeForce 830M / 840M / 940M", 0 },
    { 0x120, "GM200", "GeForce GTX 980 Ti / Titan X", 0 },
    { 0x124, "GM204", "GeForce GTX 970 / 980", 0 },
    { 0x126, "GM206", "GeForce GTX 950 / 960", 0 },
    { 0x12b, "GM20B", "Tegra X1", 1 },
    { 0x130, "GP100", "Tesla P100 / Quadro GP100", 0 },
    { 0x132, "GP102", "GeForce GTX 1080 Ti / Titan X / Titan Xp", 0 },
    { 0x134, "GP104", "GeForce GTX 1060 / 1070 / 1080", 0 },
    { 0x136, "GP106", "GeForce GTX 1060", 0 },
    { 0x137, "GP107", "GeForce GTX 1050 / 1050 Ti", 0 },
    { 0x138, "GP108", "GeForce GT 1030", 0 },
    { 0x13b, "GP10B", "Tegra X2", 1 },
    { 0x140, "GV100", "Titan V / Quadro GV100", 0 },
    { 0x162, "TU102", "GeForce RTX 2080 Ti", 0 },
    { 0x164, "TU104", "GeForce RTX 2070 Super / 2080", 0 },
    { 0x166, "TU106", "GeForce RTX 2060 / 2070", 0 },
    { 0x167, "TU117", "GeForce GTX 1650", 0 },
    { 0x168, "TU116", "GeForce GTX 1650 Super / 1660", 0 },
    { 0x170, "GA100", "A100", 0 },
    { 0x172, "GA102", "GeForce RTX 3080 / 3090", 0 },
    { 0x173, "GA103", "GeForce RTX 3060 Ti (GA103)", 0 },
    { 0x174, "GA104", "GeForce RTX 3060 Ti / 3070", 0 },
    { 0x176, "GA106", "GeForce RTX 3060", 0 },
    { 0x177, "GA107", "GeForce RTX 3050", 0 },
    { 0x180, "GH100", "H100", 0 },
    { 0x192, "AD102", "GeForce RTX 4090", 0 },
    { 0x193, "AD103", "GeForce RTX 4080", 0 },
    { 0x194, "AD104", "GeForce RTX 4070", 0 },
    { 0x196, "AD106", "GeForce RTX 4060 Ti", 0 },
    { 0x197, "AD107", "GeForce RTX 4060", 0 },
};

static const char *fam_names[] = { "unknown", "Fahrenheit", "Celsius", "Kelvin", "Rankine", "Curie", "Tesla",
                                   "Fermi", "Kepler", "Maxwell", "Pascal", "Volta", "Turing or later" };

const char *nv_family_str(int f) { return f >= 0 && f < (int)ARRAY_SIZE(fam_names) ? fam_names[f] : "unknown"; }

static int family_of(u16 cs) {
    if (cs < 0x010) return NV_FAM_FAHRENHEIT;
    if (cs < 0x020) return NV_FAM_CELSIUS;
    if (cs < 0x030) return NV_FAM_KELVIN;
    if (cs < 0x040) return NV_FAM_RANKINE;
    if (cs < 0x050 || (cs >= 0x060 && cs < 0x070)) return NV_FAM_CURIE;     /* 0x6x: C67/C68/C73 IGPs */
    if (cs < 0x0c0) return NV_FAM_TESLA;
    if (cs < 0x0e0) return NV_FAM_FERMI;
    if (cs < 0x110) return NV_FAM_KEPLER;
    if (cs < 0x130) return NV_FAM_MAXWELL;
    if (cs < 0x140) return NV_FAM_PASCAL;
    if (cs < 0x160) return NV_FAM_VOLTA;
    return NV_FAM_LATER;
}

/* family-wide product names for chipsets missing from the table */
static const char *family_series(int f) {
    switch (f) {
    case NV_FAM_FAHRENHEIT: return "Riva TNT";
    case NV_FAM_CELSIUS: return "GeForce 256 / GeForce2 / GeForce4 MX";
    case NV_FAM_KELVIN: return "GeForce3 / GeForce4 Ti";
    case NV_FAM_RANKINE: return "GeForce FX";
    case NV_FAM_CURIE: return "GeForce 6 / 7 series";
    case NV_FAM_TESLA: return "GeForce 8 / 9 / 200 / 300 series";
    case NV_FAM_FERMI: return "GeForce 400 / 500 series";
    case NV_FAM_KEPLER: return "GeForce 600 / 700 series";
    case NV_FAM_MAXWELL: return "GeForce 750 / 900 series";
    case NV_FAM_PASCAL: return "GeForce 10 series";
    case NV_FAM_VOLTA: return "Titan V";
    case NV_FAM_LATER: return "GeForce RTX / GTX 16 series or later";
    }
    return "NVIDIA GPU";
}

int nv_chip_identify(u32 boot0, u32 boot42, nv_chip_t *c) {
    memset(c, 0, sizeof *c);
    c->boot0 = boot0;
    c->boot42 = boot42;
    if (!boot0 || nv_bad_read(boot0)) return E_NODEV;
    u16 cs;
    if (boot0 & 0x1F000000u) {                      /* NV10 and later: architecture 28:24, implementation 23:20 */
        cs = (u16)((boot0 >> 20) & 0x1FF);
        c->rev = (u8)(boot0 & 0xFF);
    } else if ((boot0 & 0xFF00FFF0u) == 0x20004000u) {
        cs = (boot0 & 0x00F00000u) ? 0x05 : 0x04;
        c->rev = (u8)(boot0 & 0x0F);
    } else {
        return E_NODEV;
    }
    /* PMC_BOOT_42 names the chip on the newest GPUs (BOOT_0's format may
     * be retired there); trust it only when it decodes to a newer chip */
    if (cs >= 0x160 && boot42 && !nv_bad_read(boot42)) {
        u16 c42 = (u16)((boot42 >> 20) & 0x1FF);
        if (c42 >= 0x160 && c42 != cs) { cs = c42; c->from_boot42 = 1; }
    }
    if (cs >= 0x010 && cs < 0x020) c->rev = 0;      /* NV1x: the low bits are no revision */
    c->chipset = cs;
    c->family = (u8)family_of(cs);
    c->family_name = nv_family_str(c->family);
    c->series = family_series(c->family);
    snprintf(c->codename, sizeof c->codename, "NV%X", cs);
    for (unsigned i = 0; i < ARRAY_SIZE(chips); i++) {
        if (chips[i].chipset != cs) continue;
        strlcpy(c->codename, chips[i].code, sizeof c->codename);
        c->series = chips[i].series;
        c->igp = chips[i].igp;
        break;
    }
    c->in_range = c->family >= NV_FAM_CELSIUS && c->family <= NV_FAM_PASCAL;
    if (c->family == NV_FAM_LATER) c->note = "Turing or later: not supported until Halcyon 1.1";
    else if (c->family == NV_FAM_VOLTA) c->note = "Volta: not supported until Halcyon 1.1";
    else if (c->family == NV_FAM_FAHRENHEIT) c->note = "Riva TNT: older than the cards Halcyon supports";

    /* display engine */
    if (c->family >= NV_FAM_VOLTA) c->disp = NV_DISP_LATER;
    else if (cs < 0x050 || (cs >= 0x060 && cs < 0x070)) c->disp = NV_DISP_NV04;
    else if (cs < 0x0d0) c->disp = NV_DISP_NV50;    /* G80..GF116: the NV50 engine (GF100-GF116 too) */
    else if (cs == 0x0ea || cs == 0x12b || cs == 0x13b) c->disp = NV_DISP_NONE;   /* Tegra */
    else c->disp = NV_DISP_GF119;

    /* I2C / AUX */
    switch (cs) {
    case 0x04e: c->i2c = NV_I2C_NV4E; break;
    case 0x050: case 0x084: case 0x086: case 0x092: case 0x0a0: c->i2c = NV_I2C_NV50; break;
    default:
        if (c->disp == NV_DISP_NV04) c->i2c = NV_I2C_NV04;
        else if (c->disp == NV_DISP_NV50) c->i2c = NV_I2C_G94;          /* G94..GF116 */
        else if (c->disp == NV_DISP_GF119) c->i2c = cs >= 0x120 ? NV_I2C_GM200 : NV_I2C_GF119;
        break;
    }
    /* GPIO */
    if (cs < 0x010) c->gpio = NV_GPIO_NONE;
    else if (c->disp == NV_DISP_NV04) c->gpio = NV_GPIO_NV10;
    else if (c->disp == NV_DISP_NV50) c->gpio = NV_GPIO_NV50;
    else if (c->disp == NV_DISP_GF119) c->gpio = NV_GPIO_GF119;

    /* VRAM */
    switch (cs) {
    case 0x004: case 0x005: c->vram = NV_VRAM_NV04; break;
    case 0x01a: case 0x01f: c->vram = NV_VRAM_STOLEN; break;
    case 0x0aa: case 0x0ac: case 0x0af: c->vram = NV_VRAM_MCP77; break;
    case 0x0c1: c->vram = NV_VRAM_GF108; break;
    case 0x0ea: case 0x12b: case 0x13b: c->vram = NV_VRAM_NONE; break;
    case 0x130: c->vram = NV_VRAM_GP100; break;
    default:
        if (c->disp == NV_DISP_NV04) c->vram = NV_VRAM_NV10;
        else if (c->family == NV_FAM_TESLA) c->vram = NV_VRAM_NV50;
        else if (c->family == NV_FAM_FERMI) c->vram = NV_VRAM_GF100;
        else if (c->family == NV_FAM_KEPLER) c->vram = NV_VRAM_GF108;
        else if (cs == 0x117 || cs == 0x118) c->vram = NV_VRAM_GM107;
        else if (c->family == NV_FAM_MAXWELL) c->vram = NV_VRAM_GM200;
        else if (c->family == NV_FAM_PASCAL) c->vram = NV_VRAM_GP102;
        break;
    }
    return 0;
}

/* ---------------- card names ---------------- */

typedef struct card_row { u16 id; const char *name; } card_row_t;

/* desktop cards of each generation (checked against the PCI ID database;
 * a card missing here is shown as "<series> (<code name>)") */
static const card_row_t cards[] = {
    { 0x0100, "GeForce 256" }, { 0x0101, "GeForce 256 DDR" }, { 0x0110, "GeForce2 MX / MX 400" },
    { 0x0111, "GeForce2 MX 200" }, { 0x0150, "GeForce2 GTS / Pro" }, { 0x0151, "GeForce2 Ti" },
    { 0x0152, "GeForce2 Ultra" }, { 0x0170, "GeForce4 MX 460" }, { 0x0171, "GeForce4 MX 440" },
    { 0x0172, "GeForce4 MX 420" }, { 0x0181, "GeForce4 MX 440 AGP 8x" }, { 0x0185, "GeForce4 MX 4000" },
    { 0x0200, "GeForce3" }, { 0x0201, "GeForce3 Ti 200" }, { 0x0202, "GeForce3 Ti 500" },
    { 0x0250, "GeForce4 Ti 4600" }, { 0x0251, "GeForce4 Ti 4400" }, { 0x0253, "GeForce4 Ti 4200" },
    { 0x0281, "GeForce4 Ti 4200 AGP 8x" },
    { 0x0301, "GeForce FX 5800 Ultra" }, { 0x0302, "GeForce FX 5800" }, { 0x0311, "GeForce FX 5600 Ultra" },
    { 0x0312, "GeForce FX 5600" }, { 0x0322, "GeForce FX 5200" }, { 0x0330, "GeForce FX 5900 Ultra" },
    { 0x0331, "GeForce FX 5900" }, { 0x0341, "GeForce FX 5700 Ultra" }, { 0x0342, "GeForce FX 5700" },
    { 0x0040, "GeForce 6800 Ultra" }, { 0x0041, "GeForce 6800" }, { 0x0045, "GeForce 6800 GT" },
    { 0x00f9, "GeForce 6800 GT / GTO / Ultra" }, { 0x0140, "GeForce 6600 GT" }, { 0x0141, "GeForce 6600" },
    { 0x0161, "GeForce 6200 TurboCache" }, { 0x0221, "GeForce 6200" }, { 0x0091, "GeForce 7800 GTX" },
    { 0x0290, "GeForce 7900 GTX" }, { 0x0291, "GeForce 7900 GT / GTO" }, { 0x0391, "GeForce 7600 GT" },
    { 0x0392, "GeForce 7600 GS" }, { 0x01d1, "GeForce 7300 LE" }, { 0x01d3, "GeForce 7200 GS / 7300 SE" },
    { 0x0191, "GeForce 8800 GTX" }, { 0x0193, "GeForce 8800 GTS" }, { 0x0400, "GeForce 8600 GTS" },
    { 0x0402, "GeForce 8600 GT" }, { 0x0422, "GeForce 8400 GS" }, { 0x0600, "GeForce 8800 GTS 512" },
    { 0x0611, "GeForce 8800 GT" }, { 0x0612, "GeForce 9800 GTX / 9800 GTX+" }, { 0x0614, "GeForce 9800 GT" },
    { 0x0622, "GeForce 9600 GT" }, { 0x0640, "GeForce 9500 GT" }, { 0x06e4, "GeForce 8400 GS" },
    { 0x05e0, "GeForce GTX 295" }, { 0x05e1, "GeForce GTX 280" }, { 0x05e2, "GeForce GTX 260" },
    { 0x0ca3, "GeForce GT 240" }, { 0x0a20, "GeForce GT 220" }, { 0x0a65, "GeForce 210" },
    { 0x06c0, "GeForce GTX 480" }, { 0x06cd, "GeForce GTX 470" }, { 0x0de1, "GeForce GT 430" },
    { 0x0dc4, "GeForce GTS 450" }, { 0x0e22, "GeForce GTX 460" }, { 0x1080, "GeForce GTX 580" },
    { 0x1081, "GeForce GTX 570" }, { 0x1200, "GeForce GTX 560 Ti" }, { 0x1244, "GeForce GTX 550 Ti" },
    { 0x1040, "GeForce GT 520" }, { 0x104a, "GeForce GT 610" },
    { 0x1180, "GeForce GTX 680" }, { 0x1183, "GeForce GTX 660 Ti" }, { 0x1184, "GeForce GTX 770" },
    { 0x1187, "GeForce GTX 760" }, { 0x1189, "GeForce GTX 670" }, { 0x11c0, "GeForce GTX 660" },
    { 0x11c6, "GeForce GTX 650 Ti" }, { 0x0fc6, "GeForce GTX 650" }, { 0x0fc1, "GeForce GT 640" },
    { 0x1004, "GeForce GTX 780" }, { 0x1005, "GeForce GTX TITAN" }, { 0x100a, "GeForce GTX 780 Ti" },
    { 0x100c, "GeForce GTX TITAN Black" }, { 0x128b, "GeForce GT 710" }, { 0x1288, "GeForce GT 720" },
    { 0x1380, "GeForce GTX 750 Ti" }, { 0x1381, "GeForce GTX 750" }, { 0x17c2, "GeForce GTX TITAN X" },
    { 0x17c8, "GeForce GTX 980 Ti" }, { 0x13c0, "GeForce GTX 980" }, { 0x13c2, "GeForce GTX 970" },
    { 0x1401, "GeForce GTX 960" }, { 0x1402, "GeForce GTX 950" },
    { 0x1b00, "TITAN X (Pascal)" }, { 0x1b02, "TITAN Xp" }, { 0x1b06, "GeForce GTX 1080 Ti" },
    { 0x1b80, "GeForce GTX 1080" }, { 0x1b81, "GeForce GTX 1070" }, { 0x1b82, "GeForce GTX 1070 Ti" },
    { 0x1b83, "GeForce GTX 1060 6GB" }, { 0x1b84, "GeForce GTX 1060 3GB" }, { 0x1c02, "GeForce GTX 1060 3GB" },
    { 0x1c03, "GeForce GTX 1060 6GB" }, { 0x1c04, "GeForce GTX 1060 5GB" }, { 0x1c06, "GeForce GTX 1060 6GB" },
    { 0x1c81, "GeForce GTX 1050" }, { 0x1c82, "GeForce GTX 1050 Ti" }, { 0x1c83, "GeForce GTX 1050 3GB" },
    { 0x1d01, "GeForce GT 1030" }, { 0x1d02, "GeForce GT 1010" },
    { 0x1e04, "GeForce RTX 2080 Ti" }, { 0x1e87, "GeForce RTX 2080" }, { 0x1f02, "GeForce RTX 2070" },
    { 0x2204, "GeForce RTX 3090" }, { 0x2206, "GeForce RTX 3080" }, { 0x2684, "GeForce RTX 4090" },
};

const char *nv_card_name(u16 dev) {
    for (unsigned i = 0; i < ARRAY_SIZE(cards); i++)
        if (cards[i].id == dev) return cards[i].name;
    return NULL;
}

/* ---------------- VRAM ---------------- */

static u32 rd(const nv_regs_t *r, u32 reg) { return r->rd32(r->ctx, reg); }

/* per-partition amounts in MiB; a PRI error or an absurd value ends the sum */
static int fbpa_mib(const nv_regs_t *r, u32 reg, u64 *sum) {
    u32 v = rd(r, reg);
    if (nv_bad_read(v) || v > 65536) return 0;
    *sum += v;
    return 1;
}

u64 nv_vram_probe(const nv_chip_t *c, const nv_regs_t *r, char *how, int hown) {
    u64 size = 0;
    u32 v;
    if (how && hown) how[0] = 0;
#define HOW(...) do { if (how) snprintf(how, (size_t)hown, __VA_ARGS__); } while (0)
    switch (c->vram) {
    case NV_VRAM_NV04:
        v = rd(r, 0x100000);
        if (v & 0x100) size = ((u64)((v >> 12) & 0xF) * 2 + 2) << 20;
        else {
            static const u8 mb[4] = { 32, 4, 8, 16 };
            size = (u64)mb[v & 3] << 20;
        }
        HOW("PFB_BOOT_0 %08x", v);
        break;
    case NV_VRAM_NV10:
        v = rd(r, 0x10020C);
        if (!nv_bad_read(v)) size = v & 0xFF000000u;
        HOW("PFB 0x10020c = %08x%s", v, c->igp ? " (memory taken from the system)" : "");
        break;
    case NV_VRAM_NV50:
        v = rd(r, 0x10020C);
        if (!nv_bad_read(v)) size = ((u64)(v & 0xFF) << 32) | (v & 0xFFFFFF00u);
        HOW("PFB 0x10020c = %08x", v);
        break;
    case NV_VRAM_MCP77:
        v = rd(r, 0x100E14);
        if (!nv_bad_read(v)) size = (u64)v << 12;
        HOW("PFB 0x100e14 = %08x (memory taken from the system)", v);
        break;
    case NV_VRAM_GF100: {
        u32 fbps = rd(r, 0x022438), mask = rd(r, 0x022554);
        if (nv_bad_read(fbps) || fbps > 32) break;
        for (u32 i = 0; i < fbps; i++)
            if (!(mask & (1u << i)) && !fbpa_mib(r, 0x11020C + i * 0x1000, &size)) break;
        size <<= 20;
        HOW("%u FB partitions, disabled mask %08x", fbps, mask);
        break;
    }
    case NV_VRAM_GF108: case NV_VRAM_GM107: {
        u32 fbps = rd(r, 0x022438), fbpat = rd(r, 0x02243C);
        u32 mask = rd(r, c->vram == NV_VRAM_GM107 ? 0x021C14 : 0x022554);
        if (nv_bad_read(fbps) || nv_bad_read(fbpat) || !fbps || fbps > 32 || fbpat > 64) break;
        u32 per = fbpat / fbps;
        for (u32 p = 0; p < fbps; p++)
            for (u32 k = 0; k < per; k++) {
                u32 a = p * per + k;
                if (a < 32 && !(mask & (1u << a))) fbpa_mib(r, 0x11020C + a * 0x1000, &size);
            }
        size <<= 20;
        HOW("%u FBPs, %u FBPAs, disabled mask %08x", fbps, fbpat, mask);
        break;
    }
    case NV_VRAM_GM200: case NV_VRAM_GP100: {
        u32 fbps = rd(r, 0x022438), per = rd(r, 0x022458), mask = rd(r, 0x021C14), fbpoff = rd(r, 0x021D38);
        if (nv_bad_read(fbps) || nv_bad_read(per) || fbps > 32 || per > 8) break;
        for (u32 p = 0; p < fbps; p++) {
            if (fbpoff & (1u << p)) continue;
            for (u32 k = 0; k < per; k++) {
                u32 a = p * per + k;
                if (a >= 32 || (mask & (1u << a))) continue;
                fbpa_mib(r, c->vram == NV_VRAM_GP100 ? 0x90020C + a * 0x4000 : 0x11020C + a * 0x1000, &size);
            }
        }
        size <<= 20;
        HOW("%u FBPs x %u FBPAs, disabled %08x / %08x", fbps, per, fbpoff, mask);
        break;
    }
    case NV_VRAM_GP102:
        v = rd(r, 0x100CE0);                        /* PFB_PRI_MMU_LOCAL_MEMORY_RANGE */
        if (!nv_bad_read(v)) {
            size = (u64)((v >> 4) & 0x3F) << (20 + (v & 0xF));
            if (v & 0x40000000u) size = size / 16 * 15;     /* ECC on */
        }
        HOW("MMU_LOCAL_MEMORY_RANGE %08x", v);
        break;
    case NV_VRAM_STOLEN:
        HOW("memory taken from the system (set in the chipset)");
        break;
    default:
        HOW("no video memory of its own");
        break;
    }
#undef HOW
    /* plausible: at least 1 MiB, at most 64 GiB, whole MiB */
    if (size < (1u << 20) || size > (64ull << 30) || (size & 0xFFFFF)) size = 0;
    return size;
}
