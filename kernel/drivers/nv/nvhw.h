/* Halcyon OS - NVIDIA GPUs: the parts that do not touch hardware.
 *
 * Everything here works on bytes and on register callbacks only, so it is
 * compiled for the host too and tested there (tests/host/nvtest.c, the
 * fuzzer fuzz_nv.c): chip identification, the video BIOS (image chain,
 * BIT / BMP, DCB 2.x-4.x with its I2C / CCB, connector and GPIO tables),
 * the bit-banged I2C engine, the DisplayPort AUX channel (the G94 and
 * GM200 register layouts, I2C over AUX) and EDID over DDC.
 *
 * Written from NVIDIA's open-gpu-doc (BIT and DCB 4.x specifications), the
 * kernel's nova-core VBIOS notes and nouveau (MIT) as a reference; see
 * docs/DESIGN-M10-NVIDIA.md and THIRD_PARTY_NOTICES.md. */
#ifndef NVHW_H
#define NVHW_H
#include "core/types.h"

/* ---------------- registers ---------------- */

/* register access through callbacks: BAR0 in the kernel, a simulation on
 * the host */
typedef struct nv_regs {
    void *ctx;
    u32  (*rd32)(void *ctx, u32 reg);
    void (*wr32)(void *ctx, u32 reg, u32 val);
    void (*delay_us)(void *ctx, u32 us);
    u64  (*now_us)(void *ctx);
} nv_regs_t;

/* a read that hit a missing or powered-off unit (NV50+ "PRI" errors) */
static inline int nv_bad_read(u32 v) { return v == 0xFFFFFFFFu || (v & 0xFFF00000u) == 0xBAD00000u; }

/* ---------------- chip identification ---------------- */

typedef enum nv_family {
    NV_FAM_UNKNOWN = 0,
    NV_FAM_FAHRENHEIT,  /* NV04/NV05: Riva TNT / TNT2 (older than M10's range) */
    NV_FAM_CELSIUS,     /* NV1x: GeForce 256, GeForce2, GeForce4 MX (NV17/18) */
    NV_FAM_KELVIN,      /* NV2x: GeForce3, GeForce4 Ti */
    NV_FAM_RANKINE,     /* NV3x: GeForce FX */
    NV_FAM_CURIE,       /* NV4x, G7x: GeForce 6 and 7 */
    NV_FAM_TESLA,       /* G80-GT21x, MCP7x: GeForce 8, 9, 200, 300 */
    NV_FAM_FERMI,       /* GF1xx: GeForce 400, 500 */
    NV_FAM_KEPLER,      /* GK1xx, GK20x: GeForce 600, 700 */
    NV_FAM_MAXWELL,     /* GM1xx, GM20x: GeForce 750, 900 */
    NV_FAM_PASCAL,      /* GP10x: GeForce 10 series */
    NV_FAM_VOLTA,       /* GV100 */
    NV_FAM_LATER,       /* Turing and newer */
} nv_family_t;

/* display engine */
enum { NV_DISP_NONE, NV_DISP_NV04, NV_DISP_NV50, NV_DISP_GF119, NV_DISP_LATER };
/* I2C / AUX register blocks */
enum { NV_I2C_NONE, NV_I2C_NV04, NV_I2C_NV4E, NV_I2C_NV50, NV_I2C_G94, NV_I2C_GF119, NV_I2C_GM200 };
/* GPIO (hot-plug sense) */
enum { NV_GPIO_NONE, NV_GPIO_NV10, NV_GPIO_NV50, NV_GPIO_GF119 };
/* how the VRAM size is read */
enum { NV_VRAM_NONE, NV_VRAM_NV04, NV_VRAM_NV10, NV_VRAM_STOLEN, NV_VRAM_NV50, NV_VRAM_MCP77, NV_VRAM_GF100,
       NV_VRAM_GF108, NV_VRAM_GM107, NV_VRAM_GM200, NV_VRAM_GP100, NV_VRAM_GP102 };

typedef struct nv_chip {
    u32 boot0, boot42;          /* PMC_BOOT_0 (0x000000), PMC_BOOT_42 (0x000a00, Fermi+; 0 if not read) */
    u16 chipset;                /* 0x134 = GP104 */
    u8  rev;
    u8  family;                 /* nv_family_t */
    u8  disp, i2c, gpio, vram;  /* NV_DISP_*, NV_I2C_*, NV_GPIO_*, NV_VRAM_* */
    u8  igp;                    /* integrated in a chipset (nForce / MCP): shares system memory */
    u8  in_range;               /* Celsius to Pascal: identified by 10a */
    u8  from_boot42;            /* the chipset came from PMC_BOOT_42 */
    char codename[16];          /* "GP104" */
    const char *family_name;    /* "Pascal" */
    const char *series;         /* "GeForce 10 series" */
    const char *note;           /* "Turing or later: not supported until Halcyon 1.1", or NULL */
} nv_chip_t;

/* 0, or E_NODEV when boot0 is no NVIDIA chip ID (0, all ones, a PRI error) */
int nv_chip_identify(u32 boot0, u32 boot42, nv_chip_t *out);
const char *nv_card_name(u16 pci_device);       /* "GeForce GTX 1070", or NULL if not in the table */
const char *nv_family_str(int family);          /* "Pascal" */
/* VRAM size from the PFB / FBPA registers (0 = unknown); how: which registers */
u64 nv_vram_probe(const nv_chip_t *c, const nv_regs_t *r, char *how, int hown);

/* ---------------- video BIOS: images ---------------- */

#define NV_ROM_MAX      (1u << 20)      /* the PROM / PRAMIN windows are 1 MiB */
#define NV_ROM_IMAGES   8

typedef struct nv_rom_image {
    u32 offset, size;           /* in the PCI ROM (after any IFR data) */
    u8  type;                   /* PCIR code type: 0x00 PC-AT (legacy), 0x03 EFI, 0x70, 0xE0 NVIDIA data */
    u8  last;                   /* last image of the chain */
    u8  npde;                   /* NVIDIA's NPDE extension gave size / last */
    u8  checksum_ok;            /* bytes sum to 0 (checked for the legacy image only; 1 otherwise) */
    u16 vendor, device;         /* from PCIR */
    u8  pcir_rev;
} nv_rom_image_t;

typedef struct nv_rom {
    const u8 *data;             /* the PCI expansion ROM (raw + ifr_offset) */
    u32 size;                   /* bytes available from data */
    u32 ifr_offset;             /* where the first image starts in the raw bytes (IFR / NVGI data before it) */
    u32 ifr_version;            /* 0: no IFR header */
    int nimages;
    nv_rom_image_t img[NV_ROM_IMAGES];
    u32 chain_size;             /* end of the last image */
    u8  complete;               /* the chain ends with an image marked "last", all of it present */
    u32 legacy_size;            /* image 0 (pointers in BIT tables are offsets into it...) */
    u32 ext_base;               /* ...or, beyond it, into the 0xE0 data image (0 = none) */
    int score;                  /* 0 = unusable; higher is better (checksums, complete chain) */
    char why[64];               /* why it is unusable, or "" */
} nv_rom_t;

/* Find the PCI ROM in raw bytes (an IFR header, or 0x55AA at a 512-byte
 * boundary), walk the image chain (PCIR + NPDE), check signatures, sizes and
 * the legacy image's checksum. Returns the score (0 = no usable image). */
int nv_rom_scan(const u8 *raw, u32 rawsize, nv_rom_t *out);

/* bounds-checked reads of the ROM, with the BIT pointer rules applied
 * (pointers past the legacy image go into the extension image); an out of
 * range read returns 0 and counts in *oob (may be NULL) */
u8  nv_rom_rd8(const nv_rom_t *r, u32 addr, u32 *oob);
u16 nv_rom_rd16(const nv_rom_t *r, u32 addr, u32 *oob);
u32 nv_rom_rd32(const nv_rom_t *r, u32 addr, u32 *oob);

/* ---------------- video BIOS: BIT, BMP and the DCB ---------------- */

#define NV_BIT_MAX      40
#define NV_DCB_MAX      16
#define NV_CCB_MAX      16
#define NV_CONN_MAX     16
#define NV_GPIO_MAX     48

typedef struct nv_bit_token { u8 id, ver; u16 size, ptr; } nv_bit_token_t;

/* DCB output types (Display Path "Type") */
enum { NV_OUT_CRT = 0, NV_OUT_TV = 1, NV_OUT_TMDS = 2, NV_OUT_LVDS = 3, NV_OUT_SDI = 5, NV_OUT_DP = 6,
       NV_OUT_WFD = 8, NV_OUT_EOL = 14, NV_OUT_SKIP = 15 };

typedef struct nv_dcb_output {
    u8  index;                  /* entry in the DCB */
    u8  type;                   /* NV_OUT_* */
    u8  i2c;                    /* CCB / I2C table index (0xF: none) */
    u8  heads;                  /* bit mask */
    u8  connector;              /* connector table index */
    u8  bus;                    /* mutual exclusion group */
    u8  location;               /* 0 on chip, 1 on board (external encoder) */
    u8  or_mask;                /* DAC / SOR / PIOR (DCB 4.1: pad macro) mask */
    u8  link;                   /* sub-link / DP link / pad link mask */
    u8  extdev;                 /* external encoder type (location 1) */
    u8  hdmi;                   /* HDMI allowed on this DFP */
    u8  dp_max_rate;            /* DisplayPort: 0 1.62, 1 2.7, 2 5.4, 3 8.1 Gbit/s per lane */
    u8  dp_max_lanes;           /* 1, 2 or 4 */
    u8  edid_src;               /* DFP: 0 DDC, 1 straps / VBIOS, 2 ACPI / SBIOS */
    u32 raw[2];
} nv_dcb_output_t;

/* I2C / CCB entry kinds */
enum { NV_CCB_NV04_BIT = 0x00, NV_CCB_NV4E_BIT = 0x04, NV_CCB_NVIO_BIT = 0x05, NV_CCB_NVIO_AUX = 0x06,
       NV_CCB_PMGR = 0x80, NV_CCB_UNUSED = 0xFF };

typedef struct nv_ccb_entry {
    u8  type;                   /* NV_CCB_* */
    u8  drive, sense;           /* NV04: CRTC register indices; NVIO / PMGR: the I2C port */
    u8  auxch;                  /* DP AUX channel, 0xFF none */
    u8  share;                  /* hybrid pad (I2C or AUX), 0xFF none */
    u8  speed;
    u32 raw;
} nv_ccb_entry_t;

typedef struct nv_conn_entry {
    u8  type;                   /* 0x00 VGA, 0x30 DVI-I, 0x31 DVI-D, 0x46 DP, 0x61 HDMI ... 0xFF skip */
    u8  location;
    u8  hpd;                    /* hot-plug interrupts A..G as bits 0..6 */
    u8  dp2dvi;                 /* DP-to-DVI dongle present A..D */
    u8  di;                     /* DPAux/I2C select A..D */
    u8  lcdid;
    u8  synth;                  /* made up from the outputs (DCB 2.x / 1.5 have no connector table) */
    u32 raw;
} nv_conn_entry_t;

typedef struct nv_gpio_entry {
    u8  line, func, io, init;   /* func 0x07/0x08/0x51/0x52/0x5E/0x5F/0x60: hot plug A..G */
    u8  log[2];                 /* logical off / on state */
    u8  param;
    u32 raw;
} nv_gpio_entry_t;

/* errors and oddities found while parsing (bits of nv_bios_t.problems) */
#define NVB_NO_BIT       0x0001  /* neither BIT nor BMP */
#define NVB_BAD_BIT      0x0002  /* BIT header checksum or sizes wrong */
#define NVB_NO_DCB       0x0004
#define NVB_BAD_DCB      0x0008  /* signature, version or sizes */
#define NVB_BAD_CCB      0x0010
#define NVB_BAD_CONN     0x0020
#define NVB_BAD_GPIO     0x0040
#define NVB_OOB          0x0080  /* a pointer outside the image */
#define NVB_TRUNC        0x0100  /* more entries than this parser keeps */
#define NVB_BAD_BMP      0x0200

typedef struct nv_bios {
    nv_rom_t rom;
    u32 problems;               /* NVB_* */
    u32 oob;                    /* out of range reads */
    /* BIT (NV40 and later) or BMP (older) */
    u32 bit_offset, bmp_offset;
    u8  bit_ver_major, bit_ver_minor;
    u8  bmp_major, bmp_minor;
    int ntokens;
    nv_bit_token_t tok[NV_BIT_MAX];
    u8  version[5];             /* VBIOS version, major first: 86.04.50.00.70 */
    char version_str[24];
    u8  feature;                /* BMP / BIT 'i' feature byte (bit 4: mobile) */
    u16 board_id;
    /* DCB */
    u32 dcb_offset;
    u8  dcb_ver, dcb_hdr, dcb_cnt, dcb_len;
    u8  dcb_flags;
    int noutputs;
    nv_dcb_output_t out[NV_DCB_MAX];
    /* I2C table / CCB */
    u32 ccb_offset;
    u8  ccb_ver, ccb_hdr, ccb_cnt, ccb_len;
    u8  ccb_primary, ccb_secondary;
    int nccb;
    nv_ccb_entry_t ccb[NV_CCB_MAX];
    /* connector table */
    u32 conn_offset;
    u8  conn_ver, conn_hdr, conn_cnt, conn_len, platform;
    int nconn;
    nv_conn_entry_t conn[NV_CONN_MAX];
    /* GPIO assignment table */
    u32 gpio_offset;
    u8  gpio_ver, gpio_hdr, gpio_cnt, gpio_len;
    int ngpio;
    nv_gpio_entry_t gpio[NV_GPIO_MAX];
} nv_bios_t;

/* Parse raw ROM bytes (as read from the GPU) into *b. chipset: 0 if not
 * known (some rules depend on it: pre-NV10 has no DCB pointer). Returns the
 * ROM score (0 = no usable image; *b still says why in rom.why). Never
 * reads outside raw[0..size). */
int nv_bios_parse(const u8 *raw, u32 size, int chipset, nv_bios_t *b);
const nv_bit_token_t *nv_bios_token(const nv_bios_t *b, u8 id);

const char *nv_out_type_str(int type);          /* "DisplayPort" */
const char *nv_conn_type_str(int type);         /* "HDMI-A connector" */
const char *nv_ccb_type_str(int type);
const char *nv_gpio_func_str(int func);         /* "hot plug A", or NULL if not a known one */
int nv_hpd_func(int bit);                       /* hot-plug bit 0..6 -> GPIO function 0x07 ... */

/* ---------------- I2C: bit-banged master ---------------- */

#define NV_I2C_RD 1

typedef struct nv_i2c_msg { u8 addr; u8 flags; u16 len; u8 *buf; } nv_i2c_msg_t;

/* one bus: the lines are open drain (drive 1 = release); every wait is
 * bounded by the timeouts below, measured with now_us */
typedef struct nv_i2c_bb {
    void *ctx;
    void (*drive_scl)(void *ctx, int state);
    void (*drive_sda)(void *ctx, int state);
    int  (*sense_scl)(void *ctx);
    int  (*sense_sda)(void *ctx);
    void (*delay_us)(void *ctx, u32 us);
    u64  (*now_us)(void *ctx);
    u32  half_us;               /* half a clock period (5: 100 kHz) */
    u32  stretch_us;            /* longest clock stretch accepted (2200 like nouveau) */
    u32  total_us;              /* the whole transfer (default 100 ms) */
} nv_i2c_bb_t;

/* 0 on success; E_IO a NAK (also of the address: no device), E_TIMEOUT a
 * clock held low too long or the transfer took too long, E_BUSY the bus
 * could not be freed (SDA stuck low) */
int nv_i2c_bb_xfer(nv_i2c_bb_t *bb, nv_i2c_msg_t *msgs, int n);

/* ---------------- DisplayPort AUX ---------------- */

/* request codes (DP 1.2 table 2-54) */
#define NV_AUX_I2C_WR     0x0
#define NV_AUX_I2C_RD     0x1
#define NV_AUX_I2C_MOT    0x4
#define NV_AUX_NATIVE_WR  0x8
#define NV_AUX_NATIVE_RD  0x9
/* reply nibble: native bits 1:0, I2C bits 3:2 */
#define NV_AUX_ACK        0x0
#define NV_AUX_NACK       0x1
#define NV_AUX_DEFER      0x2
#define NV_AUX_I2C_NACK   0x4
#define NV_AUX_I2C_DEFER  0x8

/* one AUX channel of the display engine's I2C/AUX unit */
enum { NV_AUXHW_G94, NV_AUXHW_GM200 };
typedef struct nv_aux_hw {
    const nv_regs_t *r;
    int layout;                 /* NV_AUXHW_* */
    int ch;                     /* channel (DCB CCB "auxch") */
    u32 retries_done;           /* statistics: transactions repeated after DEFER */
    u32 last_stat, last_ctrl;   /* for the report */
} nv_aux_hw_t;

/* One AUX transaction (up to 16 bytes). Returns the reply nibble (>= 0,
 * NV_AUX_*), or E_NODEV (no sink: hot plug sense low), E_TIMEOUT (the
 * channel did not finish within 2 ms, or would not become idle), E_IO
 * (receive error). *len: in, bytes to send / wanted; out, bytes received.
 * The channel's registers are left as they were found. */
int nv_aux_hw_xfer(nv_aux_hw_t *a, u8 req, u32 addr, u8 *buf, u8 *len);

/* the generic DP AUX layer on top of any transaction function */
typedef int (*nv_aux_fn)(void *ctx, u8 req, u32 addr, u8 *buf, u8 *len);
typedef struct nv_aux {
    void *ctx;
    nv_aux_fn xfer;
    void (*delay_us)(void *ctx, u32 us);        /* between retries after a DEFER */
    u32 defers;                                 /* statistics */
} nv_aux_t;
/* native AUX read (DPCD) of len bytes in 16-byte pieces, DEFERs retried:
 * the number of bytes read, or E_* */
int nv_aux_native_read(nv_aux_t *a, u32 addr, u8 *buf, int len);
/* I2C over AUX (DP 1.2 2.7.7.1): messages as for nv_i2c_bb_xfer, with MOT
 * between the messages of one transfer, DEFER and I2C DEFER retried, NACK
 * reported as E_IO */
int nv_aux_i2c_xfer(nv_aux_t *a, nv_i2c_msg_t *msgs, int n);
/* the same as an nv_i2c_fn (ctx: an nv_aux_t), e.g. for nv_ddc_read_edid */
int nv_aux_i2c_fn(void *aux, nv_i2c_msg_t *msgs, int n);

/* ---------------- EDID over DDC ---------------- */

/* any I2C transfer function (a bit-banged bus or I2C over AUX) */
typedef int (*nv_i2c_fn)(void *ctx, nv_i2c_msg_t *msgs, int n);
#define NV_EDID_MAX 1024        /* 8 blocks */
/* Read the EDID at DDC address 0x50 with E-DDC segments (0x30) for blocks
 * past the first two: the base block (header and checksum checked, read
 * again up to 3 times), then its extensions up to max bytes. Returns the
 * bytes read (a multiple of 128) or E_*; *note says what went wrong with a
 * later block (the earlier ones are kept). */
int nv_ddc_read_edid(nv_i2c_fn fn, void *ctx, u8 *buf, int max, char *note, int noten);

#endif
