/* Halcyon OS - NVIDIA GPUs (M10a: identification).
 *
 * nv_init() finds every NVIDIA display controller on the PCI bus, maps BAR0
 * (uncached), names the chip (PMC_BOOT_0 / PMC_BOOT_42), sizes the VRAM,
 * reads the video BIOS from every source it can (PRAMIN, PROM, the PCI
 * expansion ROM BAR, the legacy shadow at C0000) and keeps the best image,
 * parses BIT / BMP and the DCB, and reads each connector's EDID over I2C or
 * DisplayPort AUX. It only reads: the few writes reading needs (the PRAMIN
 * window, the PROM enable bit, I2C pad modes, the AUX channel's request
 * bits, bit-banging) are undone at once. Nothing on screen changes.
 *
 * Skipped with nv.disable=1 in SETTINGS.INI or F8 Safe Mode
 * (BOOTF_NO_DISPLAY); nv.i2c=0 skips the I2C / AUX part, nv.prom=0 and
 * nv.rombar=0 those two ROM sources.
 *
 * The modesetting driver for the Tesla to Pascal display engine (M10b)
 * builds on this interface; see docs/DESIGN-M10-NVIDIA.md. */
#ifndef NV_H
#define NV_H
#include "core/types.h"
#include "drivers/pci.h"
#include "drivers/edid.h"
#include "drivers/nv/nvhw.h"

#define NV_MAX_GPUS     4

/* where a video BIOS copy came from (nv_device_t.rom[]) */
enum { NV_SRC_PRAMIN, NV_SRC_PROM, NV_SRC_ROMBAR, NV_SRC_ACPI, NV_SRC_SHADOW, NV_SRC_COUNT };

typedef struct nv_rom_copy {
    const char *source;         /* "PRAMIN", "PROM", "PCI ROM BAR", "ACPI _ROM", "legacy shadow C0000" */
    u8  *data;                  /* raw bytes as read (NULL: nothing read) */
    u32 size;
    int score;                  /* nv_rom_scan(): 0 unusable */
    u32 us;                     /* time it took */
    char status[112];           /* "used: 3 images, 232 KiB, checksum ok" / "not available: ..." */
} nv_rom_copy_t;

typedef struct nv_connector {
    int index;                  /* in the connector table (bios->conn[]) */
    u8  type;                   /* connector table type (0x46 DisplayPort, 0x61 HDMI ...) */
    int nout;
    u8  out[6];                 /* indices into bios->out[] of the outputs wired to it */
    int hpd;                    /* hot-plug sense: 1 plugged in, 0 not, -1 unknown */
    int edid_len;               /* 0: none */
    u8  *edid;                  /* NV_EDID_MAX bytes */
    edid_info_t mon;
    char via[48];               /* "DP AUX channel 1" / "I2C port 3" */
    char note[96];              /* why there is no EDID, or what was odd */
} nv_connector_t;

typedef struct nv_device {
    pci_dev_t *pci;
    int index;                  /* 0.. */
    volatile u8 *mmio;          /* BAR0 (NULL for the QEMU test hook's simulated card) */
    u64 mmio_phys, mmio_size;
    int uncached;               /* the BAR0 pages were set uncached here (else firmware MTRRs) */
    nv_regs_t regs;             /* rd32 / wr32 / delay / clock over BAR0 */
    nv_chip_t chip;
    char name[96];              /* "NVIDIA GeForce GTX 1070" */
    u32 pmc_enable;             /* PMC_ENABLE (0x000200) */
    u64 vram;                   /* bytes, 0 = unknown */
    char vram_how[96];
    u64 bar1_size;              /* the frame buffer aperture */
    struct {
        int pcie;               /* 1 PCI Express, 2 AGP, 0 PCI */
        int speed, width;       /* link status: speed 1 = 2.5 GT/s, 2 = 5, 3 = 8, 4 = 16 */
        int max_speed, max_width;
        u32 agp_status, agp_cmd;
    } link;
    nv_rom_copy_t rom[NV_SRC_COUNT];
    int rom_best;               /* NV_SRC_*, -1 none */
    nv_bios_t *bios;            /* parsed best image, NULL if none */
    int nconn;
    nv_connector_t conn[NV_CONN_MAX];
    u32 timeouts;               /* I2C / AUX / register waits that ran out */
    int fake;                   /* simulated by the QEMU test hook (opt/halcyon/nvfake) */
    int i2c_off;                /* nv.i2c=0 */
    int irq_on;                 /* the GPU may raise interrupts (nobody would answer them): no AUX / pads */
    u32 intr_en;                /* PMC_INTR_EN (0x000140) as found */
    u32 init_ms;                /* time nv_init spent on this card */
} nv_device_t;

void nv_init(void);                     /* after settings_load (reads nv.* keys) */
int  nv_count(void);
nv_device_t *nv_get(int i);
nv_device_t *nv_for_pci(const pci_dev_t *p);

/* registers (BAR0) */
u32  nv_rd32(nv_device_t *d, u32 reg);
void nv_wr32(nv_device_t *d, u32 reg, u32 v);
u32  nv_mask(nv_device_t *d, u32 reg, u32 mask, u32 val);   /* returns the old value */
u8   nv_rd08(nv_device_t *d, u32 reg);
void nv_wr08(nv_device_t *d, u32 reg, u8 v);

/* I2C on a CCB (DCB I2C table) entry: switches a hybrid pad to I2C and
 * back, takes the port from the firmware for the transfer and gives it back.
 * 0 or E_* (E_IO: NAK / no device, E_TIMEOUT, E_BUSY, E_NODEV: no such
 * port, E_NOTSUP: a port kind this code does not drive). */
int  nv_i2c_xfer(nv_device_t *d, int ccb, nv_i2c_msg_t *msgs, int n);
/* one DisplayPort AUX transaction on a CCB entry's AUX channel (pad in AUX
 * mode for the duration): the reply nibble NV_AUX_* or E_* (see
 * nv_aux_hw_xfer) */
int  nv_aux_xfer(nv_device_t *d, int ccb, u8 req, u32 addr, u8 *buf, u8 *len);
/* DPCD read (native AUX, DEFERs retried): bytes read or E_* */
int  nv_aux_read(nv_device_t *d, int ccb, u32 addr, u8 *buf, int len);
/* I2C over the AUX channel of a CCB entry */
int  nv_aux_i2c(nv_device_t *d, int ccb, nv_i2c_msg_t *msgs, int n);
/* does this CCB entry have an I2C port / an AUX channel this code drives? */
int  nv_ccb_has_i2c(nv_device_t *d, int ccb);
int  nv_ccb_has_aux(nv_device_t *d, int ccb);

/* The EDID of a DCB output: over AUX (I2C over AUX) for DisplayPort, else
 * DDC on its I2C port. Bytes read (multiple of 128) or E_*; via says how. */
int  nv_read_edid(nv_device_t *d, const nv_dcb_output_t *o, u8 *buf, int max, char *via, int vian);

/* M10b: keep a CCB entry's hybrid pad in AUX mode (a DisplayPort output
 * being driven); 1 if a pad was switched. nv_pad_restore puts every pad
 * held on d back as it was found. */
int  nv_pad_hold_aux(nv_device_t *d, int ccb);
void nv_pad_restore(nv_device_t *d);

/* lookups */
const nv_dcb_output_t *nv_output(nv_device_t *d, int index);              /* bios->out[index] */
/* the nth output (0..) on connector conn, of type (NV_OUT_*, -1 any) */
const nv_dcb_output_t *nv_output_on(nv_device_t *d, int conn, int type, int nth);
nv_connector_t *nv_connector(nv_device_t *d, int conn);
/* hot-plug sense of a connector from its GPIO: 1, 0, or -1 unknown */
int  nv_hotplug(nv_device_t *d, int conn);

/* disp.c (M10b): the modesetting driver for the EVO display engine (Tesla
 * to Pascal). nv_display_probe registers each card with the display core
 * (called at the end of nv_init; nothing on screen changes until
 * display_start). nv_display_state: one line for System Information and
 * NVREPORT ("native, in use on DP-1" / "firmware frame buffer: ..."),
 * nv_display_log: the driver's log of this card (takeover, mode sets). */
void nv_display_probe(void);
const char *nv_display_state(nv_device_t *d, char *buf, int n);
const char *nv_display_log(nv_device_t *d);

/* report.c: text for BOOT.LOG, System Information and NVREPORT.EXE */
int  nv_report_text(nv_device_t *d, char *buf, int size);   /* bytes (truncated to size - 1) */
void nv_log_summary(nv_device_t *d);                        /* the "nv:" lines of BOOT.LOG */
const char *nv_link_speed_str(int speed);                   /* "8 GT/s" */
/* System Information's NVIDIA section: row callback (depth 1 or 2) */
typedef void (*nv_row_fn)(void *ctx, int depth, int icon_hint, const char *detail, const char *text);
void nv_sysinfo(nv_row_fn add, void *ctx);
enum { NV_ICON_CARD, NV_ICON_MONITOR, NV_ICON_CONNECTOR, NV_ICON_WARNING };

#endif
