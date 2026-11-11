/* Halcyon OS - AHCI (SATA) disk driver
 *
 * Polled AHCI 1.x driver for ATA disks. Each port uses a single command
 * slot (slot 0); requests are split into chunks of at most AHCI_MAX_XFER
 * bytes. Buffers are DMAed to directly (everything is identity mapped)
 * unless they are odd-aligned (PRD addresses must be word aligned) or, on
 * HBAs without 64-bit addressing, reach above 4 GiB: then the request goes
 * through a per-port bounce buffer.
 */
#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "fs/block.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* ---------------- HBA registers ---------------- */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28

#define CAP_NP_MASK   0x1F
#define CAP_SCLO      (1u << 24)
#define CAP_SSS       (1u << 27)
#define CAP_S64A      (1u << 31)
#define CAP_NCS(c)    ((((c) >> 8) & 0x1F) + 1)
#define CAP_ISS(c)    (((c) >> 20) & 0xF)

#define GHC_HR        (1u << 0)
#define GHC_IE        (1u << 1)
#define GHC_AE        (1u << 31)

#define CAP2_BOH      (1u << 0)
#define BOHC_BOS      (1u << 0)
#define BOHC_OOS      (1u << 1)
#define BOHC_BB       (1u << 4)

/* ---------------- port registers ---------------- */
#define PORT_BASE(i)  (0x100 + (i) * 0x80)
#define PX_CLB      0x00
#define PX_CLBU     0x04
#define PX_FB       0x08
#define PX_FBU      0x0C
#define PX_IS       0x10
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20
#define PX_SIG      0x24
#define PX_SSTS     0x28
#define PX_SCTL     0x2C
#define PX_SERR     0x30
#define PX_SACT     0x34
#define PX_CI       0x38

#define CMD_ST        (1u << 0)
#define CMD_SUD       (1u << 1)
#define CMD_POD       (1u << 2)
#define CMD_CLO       (1u << 3)
#define CMD_FRE       (1u << 4)
#define CMD_FR        (1u << 14)
#define CMD_CR        (1u << 15)

/* PxIS error bits: TFES, HBFS, HBDS, IFS, INFS, OFS */
#define IS_TFES       (1u << 30)
#define IS_ERR_MASK   ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26) | (1u << 24))

#define TFD_ERR       0x01
#define TFD_DRQ       0x08
#define TFD_BSY       0x80

#define SSTS_DET(s)   ((s) & 0xF)
#define SSTS_SPD(s)   (((s) >> 4) & 0xF)
#define SSTS_IPM(s)   (((s) >> 8) & 0xF)

#define SIG_ATA       0x00000101u
#define SIG_ATAPI     0xEB140101u
#define SIG_SEMB      0xC33C0101u
#define SIG_PM        0x96690101u

/* ---------------- ATA commands ---------------- */
#define ATA_READ_DMA        0xC8
#define ATA_WRITE_DMA       0xCA
#define ATA_READ_DMA_EXT    0x25
#define ATA_WRITE_DMA_EXT   0x35
#define ATA_FLUSH_CACHE     0xE7
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_IDENTIFY        0xEC

#define FIS_TYPE_H2D        0x27

/* ---------------- tunables ---------------- */
#define AHCI_MAX_XFER       (128 * 1024)    /* bytes per command */
#define AHCI_BOUNCE_SIZE    (128 * 1024)    /* per-port bounce buffer */
#define AHCI_PRD_MAX        (4u * 1024 * 1024)
#define AHCI_PRDT_ENTRIES   8
#define AHCI_CMD_TIMEOUT    5000            /* ms */
#define AHCI_FLUSH_TIMEOUT  30000           /* ms */
#define AHCI_STOP_TIMEOUT   500             /* ms */

/* ---------------- DMA structures ---------------- */
typedef struct {
    u32 dw0;        /* CFL[4:0] A[5] W[6] P[7] R[8] B[9] C[10] PMP[15:12] PRDTL[31:16] */
    volatile u32 prdbc;
    u32 ctba, ctbau;
    u32 rsvd[4];
} PACKED ahci_cmd_hdr_t;

typedef struct {
    u32 dba, dbau, rsvd, dbc;   /* dbc: byte count - 1 in [21:0], I in [31] */
} PACKED ahci_prd_t;

typedef struct {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsvd[48];
    ahci_prd_t prdt[AHCI_PRDT_ENTRIES];
} PACKED ahci_cmd_tbl_t;

struct ahci_port;
typedef struct ahci_hba {
    pci_dev_t *pci;
    volatile u8 *abar;
    u32 cap, cap2, pi, vs;
    int nports;
    struct ahci_port *ports[32];
    irq_handle_t irq;
    char irq_name[24];
} ahci_hba_t;

typedef struct ahci_port {
    ahci_hba_t *hba;
    volatile u8 *regs;
    int index;
    ahci_cmd_hdr_t *clb;        /* command list (32 headers, 1 KiB) */
    u8 *fis;                    /* received FIS area (256 B) */
    ahci_cmd_tbl_t *tbl;        /* command table for slot 0 */
    u8 *bounce;                 /* AHCI_BOUNCE_SIZE bytes, also used for IDENTIFY */
    int lba48;
    u8 flush_cmd;               /* 0 = no flush needed/supported */
    u8 busy;
    u8 in_reset;                /* COMRESET issued during bring-up */
    u32 is_acc;                 /* PxIS bits cleared by the INTx acknowledge */
    u32 sector_size;
    u64 sectors;
    blockdev_t bd;
} ahci_port_t;

/* weak: the driver also links (and logs) without a block layer */
int blockdev_register(blockdev_t *bd) __attribute__((weak));

static int g_disk_count;

int ahci_disk_count(void) { return g_disk_count; }

/* ---------------- register helpers ---------------- */
static inline u32 hba_rd(ahci_hba_t *h, u32 off) { return mmio_read32(h->abar + off); }
static inline void hba_wr(ahci_hba_t *h, u32 off, u32 v) { mmio_write32(h->abar + off, v); }
static inline u32 px_rd(ahci_port_t *p, u32 off) { return mmio_read32(p->regs + off); }
static inline void px_wr(ahci_port_t *p, u32 off, u32 v) { mmio_write32(p->regs + off, v); }

/* poll until (reg & mask) == val; returns 0 or E_TIMEOUT */
static int wait_reg(volatile u8 *reg, u32 mask, u32 val, u32 timeout_ms) {
    u64 deadline = time_ms() + timeout_ms;
    for (;;) {
        if ((mmio_read32(reg) & mask) == val) return 0;
        if (time_ms() > deadline) break;
        udelay(10);
    }
    return ((mmio_read32(reg) & mask) == val) ? 0 : E_TIMEOUT;
}

static const char *port_name(ahci_port_t *p, char *buf, size_t n) {
    snprintf(buf, n, "%02x:%02x.%x port %d", p->hba->pci->bus, p->hba->pci->dev,
             p->hba->pci->func, p->index);
    return buf;
}

/* ---------------- port engine control ---------------- */
static int port_stop(ahci_port_t *p) {
    u32 cmd = px_rd(p, PX_CMD);
    int rc = 0;
    if (cmd & CMD_ST) px_wr(p, PX_CMD, cmd & ~CMD_ST);
    /* FRE must not be cleared while the command list engine still runs */
    if (wait_reg(p->regs + PX_CMD, CMD_CR, 0, AHCI_STOP_TIMEOUT)) return E_TIMEOUT;
    cmd = px_rd(p, PX_CMD);
    if (cmd & CMD_FRE) px_wr(p, PX_CMD, cmd & ~CMD_FRE);
    if (wait_reg(p->regs + PX_CMD, CMD_FR, 0, AHCI_STOP_TIMEOUT)) rc = E_TIMEOUT;
    return rc;
}

/* stop only the command list engine (FIS receive stays on) */
static int port_stop_cmd(ahci_port_t *p) {
    u32 cmd = px_rd(p, PX_CMD);
    if (cmd & CMD_ST) px_wr(p, PX_CMD, cmd & ~CMD_ST);
    return wait_reg(p->regs + PX_CMD, CMD_CR, 0, AHCI_STOP_TIMEOUT);
}

static void port_start_cmd(ahci_port_t *p) {
    /* ST must only be set while CR is clear and FRE is set */
    wait_reg(p->regs + PX_CMD, CMD_CR, 0, AHCI_STOP_TIMEOUT);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_FRE | CMD_ST);
}

static void port_clear_errors(ahci_port_t *p) {
    px_wr(p, PX_SERR, 0xFFFFFFFF);
    px_wr(p, PX_IS, 0xFFFFFFFF);
    hba_wr(p->hba, HBA_IS, 1u << p->index);
}

static int link_up(ahci_port_t *p) {
    return SSTS_DET(px_rd(p, PX_SSTS)) == 3;
}

/* Wait for the PHY to come up after a COMRESET. If nothing is detected at
 * all (DET == 0) after a short time the port is considered empty. */
static int wait_link(ahci_port_t *p, u32 timeout_ms) {
    u64 start = time_ms();
    for (;;) {
        u32 det = SSTS_DET(px_rd(p, PX_SSTS));
        if (det == 3) return 1;
        u64 el = time_ms() - start;
        if (det == 0 && el > 100) return 0;
        if (el > timeout_ms) return 0;
        udelay(100);
    }
}

/* COMRESET is DET=1 for at least 1 ms, then DET=0. IPM=3 disables
 * partial/slumber transitions; SPD is left unchanged. Requires ST clear. */
static void comreset_assert(ahci_port_t *p) {
    px_wr(p, PX_SCTL, (px_rd(p, PX_SCTL) & ~0xF0Fu) | 0x301);
}

static void comreset_release(ahci_port_t *p) {
    px_wr(p, PX_SCTL, (px_rd(p, PX_SCTL) & ~0xF0Fu) | 0x300);
}

/* Returns 1 if the link came up. */
static int port_comreset(ahci_port_t *p) {
    comreset_assert(p);
    mdelay(2);
    comreset_release(p);
    int up = wait_link(p, 1000);
    px_wr(p, PX_SERR, 0xFFFFFFFF);              /* clear SERR.DIAG.X etc. */
    return up;
}

/* wait for the device to clear BSY and DRQ */
static int wait_ready(ahci_port_t *p, u32 timeout_ms) {
    return wait_reg(p->regs + PX_TFD, TFD_BSY | TFD_DRQ, 0, timeout_ms);
}

/* Non-queued error recovery (AHCI 1.3 section 6.2.2.1). */
static void port_recover(ahci_port_t *p) {
    char nm[32];
    if (port_stop_cmd(p))
        kprintf("ahci: %s: command engine did not stop during recovery\n", port_name(p, nm, sizeof nm));
    port_clear_errors(p);
    if (px_rd(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        int ok = 0;
        if (p->hba->cap & CAP_SCLO) {
            px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_CLO);
            if (!wait_reg(p->regs + PX_CMD, CMD_CLO, 0, 500) &&
                !(px_rd(p, PX_TFD) & (TFD_BSY | TFD_DRQ)))
                ok = 1;
        }
        if (!ok) {
            kprintf("ahci: %s: device stuck (TFD %08x), COMRESET\n", port_name(p, nm, sizeof nm),
                    px_rd(p, PX_TFD));
            if (!port_comreset(p))
                kprintf("ahci: %s: link down after COMRESET\n", port_name(p, nm, sizeof nm));
            else if (wait_ready(p, 5000))
                kprintf("ahci: %s: device still busy after COMRESET (TFD %08x)\n",
                        port_name(p, nm, sizeof nm), px_rd(p, PX_TFD));
        }
        port_clear_errors(p);
    }
    port_start_cmd(p);
}

/* ---------------- command execution ---------------- */
static void idle_poll(u64 *last) {
    if (!g_idle_hook) return;
    u64 now = time_ms();
    if (now != *last) { *last = now; g_idle_hook(); }
}

/* Build and run one command in slot 0. buf/bytes describe the data phase
 * (bytes == 0: no data). buf must be word aligned and DMA reachable. */
static int port_exec(ahci_port_t *p, u8 cmd, u64 lba, u32 count, void *buf, u32 bytes,
                     int write, u32 timeout_ms) {
    char nm[32];
    ahci_cmd_hdr_t *h = &p->clb[0];
    ahci_cmd_tbl_t *t = p->tbl;
    int nprd = 0;

    /* PRDT */
    u64 addr = virt_to_phys(buf);
    u32 left = bytes;
    while (left) {
        u32 len = MIN(left, AHCI_PRD_MAX);
        if (nprd >= AHCI_PRDT_ENTRIES) return E_INVAL;
        t->prdt[nprd].dba = (u32)addr;
        t->prdt[nprd].dbau = (u32)(addr >> 32);
        t->prdt[nprd].rsvd = 0;
        t->prdt[nprd].dbc = len - 1;
        addr += len; left -= len; nprd++;
    }

    /* command FIS */
    u8 *f = t->cfis;
    memset(f, 0, 20);
    f[0] = FIS_TYPE_H2D;
    f[1] = 0x80;                                /* C: command register update */
    f[2] = cmd;
    if (cmd == ATA_READ_DMA_EXT || cmd == ATA_WRITE_DMA_EXT) {
        f[4] = (u8)lba; f[5] = (u8)(lba >> 8); f[6] = (u8)(lba >> 16);
        f[7] = 0x40;                            /* LBA mode */
        f[8] = (u8)(lba >> 24); f[9] = (u8)(lba >> 32); f[10] = (u8)(lba >> 40);
        f[12] = (u8)count; f[13] = (u8)(count >> 8);   /* 65536 -> 0 */
    } else if (cmd == ATA_READ_DMA || cmd == ATA_WRITE_DMA) {
        f[4] = (u8)lba; f[5] = (u8)(lba >> 8); f[6] = (u8)(lba >> 16);
        f[7] = 0x40 | ((lba >> 24) & 0x0F);
        f[12] = (u8)count;                      /* 256 -> 0 */
    }

    /* command header: CFL = 5 dwords */
    h->dw0 = 5 | (write ? (1u << 6) : 0) | ((u32)nprd << 16);
    h->prdbc = 0;
    h->ctba = (u32)virt_to_phys(t);
    h->ctbau = (u32)(virt_to_phys(t) >> 32);
    wmb();

    if (wait_ready(p, 1000)) {
        kprintf("ahci: %s: device busy before command %02x (TFD %08x)\n",
                port_name(p, nm, sizeof nm), cmd, px_rd(p, PX_TFD));
        port_recover(p);
        if (wait_ready(p, 1000)) return E_IO;
    }
    if (!(px_rd(p, PX_CMD) & CMD_ST)) port_start_cmd(p);

    px_wr(p, PX_IS, 0xFFFFFFFF);
    hba_wr(p->hba, HBA_IS, 1u << p->index);
    p->is_acc = 0;
    u32 gen0 = irq_generation();
    px_wr(p, PX_CI, 1);

    u64 deadline = time_ms() + timeout_ms, last = 0;
    u32 is = 0;
    int timed_out = 0;
    for (;;) {
        u32 gen = irq_generation();
        is = px_rd(p, PX_IS) | p->is_acc;
        if (is & IS_ERR_MASK) break;
        if (!(px_rd(p, PX_CI) & 1)) break;
        if (time_ms() > deadline) {
            if (!(px_rd(p, PX_CI) & 1)) break;
            timed_out = 1;
            break;
        }
        idle_poll(&last);
        irq_wait(&p->hba->irq, gen, deadline);
    }
    rmb();
    /* INTx: let irq_service() see (and learn) the interrupt before the
     * status that drives it is cleared below */
    if (p->hba->irq.kind == IRQ_INTX) {
        /* while the route is being learned, give the (posted) INTx message a
         * moment to arrive before the status that drives it is cleared */
        if (p->hba->irq.line < 0)
            for (u64 end = time_us() + 500; irq_generation() == gen0 && time_us() < end;) cpu_relax();
        irq_service();
    }
    u32 tfd = px_rd(p, PX_TFD);
    is = px_rd(p, PX_IS) | p->is_acc;
    if (timed_out || (is & IS_ERR_MASK) || (tfd & (TFD_ERR | TFD_BSY | TFD_DRQ))) {
        kprintf("ahci: %s: command %02x lba %lu count %u %s: IS %08x TFD %08x "
                "(status %02x error %02x) SERR %08x CI %08x\n",
                port_name(p, nm, sizeof nm), cmd, lba, count,
                timed_out ? "timed out" : "failed", is, tfd, tfd & 0xFF, (tfd >> 8) & 0xFF,
                px_rd(p, PX_SERR), px_rd(p, PX_CI));
        port_recover(p);
        return timed_out ? E_TIMEOUT : E_IO;
    }
    px_wr(p, PX_IS, is);
    hba_wr(p->hba, HBA_IS, 1u << p->index);    /* re-arms MSI */
    irq_note_work(&p->hba->irq);
    return 0;
}

/* ---------------- block device interface ---------------- */
static int ahci_rw(blockdev_t *bd, u64 lba, u32 count, void *buf, int write) {
    ahci_port_t *p = bd ? (ahci_port_t *)bd->priv : NULL;
    if (!p || !buf || count == 0) return E_INVAL;
    if (!bd->present) return E_NODEV;
    if (lba >= p->sectors || count > p->sectors - lba) return E_INVAL;
    if (p->busy) return E_BUSY;                 /* re-entered from the idle hook */
    p->busy = 1;

    u32 ss = p->sector_size;
    u64 total = (u64)count * ss;
    int bounce = ((uptr)buf & 1) != 0;
    if (!(p->hba->cap & CAP_S64A) && virt_to_phys(buf) + total > 0x100000000ull) bounce = 1;

    u32 max_sec = (bounce ? AHCI_BOUNCE_SIZE : AHCI_MAX_XFER) / ss;
    if (!p->lba48 && max_sec > 256) max_sec = 256;
    if (max_sec == 0) max_sec = 1;

    u8 cmd = write ? (p->lba48 ? ATA_WRITE_DMA_EXT : ATA_WRITE_DMA)
                   : (p->lba48 ? ATA_READ_DMA_EXT : ATA_READ_DMA);
    u8 *b = buf;
    int rc = 0;
    while (count) {
        u32 n = MIN(count, max_sec);
        u32 bytes = n * ss;
        void *dma = bounce ? (void *)p->bounce : (void *)b;
        if (write && bounce) memcpy(p->bounce, b, bytes);
        rc = port_exec(p, cmd, lba, n, dma, bytes, write, AHCI_CMD_TIMEOUT);
        if (rc) break;
        if (!write && bounce) memcpy(b, p->bounce, bytes);
        b += bytes; lba += n; count -= n;
    }
    if (write && rc == 0 && p->flush_cmd)
        rc = port_exec(p, p->flush_cmd, 0, 0, NULL, 0, 0, AHCI_FLUSH_TIMEOUT);
    p->busy = 0;
    return rc ? E_IO : 0;
}

static int ahci_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    return ahci_rw(bd, lba, count, buf, 0);
}

static int ahci_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    return ahci_rw(bd, lba, count, (void *)buf, 1);
}

/* ---------------- IDENTIFY ---------------- */
static void ata_string(const u16 *id, int first, int words, char *out, size_t outsz) {
    size_t n = 0;
    for (int i = 0; i < words && n + 2 < outsz; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    out[n] = 0;
    while (n && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, n - s + 1);
    for (size_t i = 0; out[i]; i++)
        if (!isprint((u8)out[i])) out[i] = '?';
}

static void fmt_size(u64 bytes, char *buf, size_t n) {
    if (bytes >= (10ull << 30)) snprintf(buf, n, "%lu GiB", bytes >> 30);
    else if (bytes >= (10ull << 20)) snprintf(buf, n, "%lu MiB", bytes >> 20);
    else snprintf(buf, n, "%lu KiB", bytes >> 10);
}

static int port_identify(ahci_port_t *p) {
    char nm[32], serial[24], fw[12], sz[24];
    u16 *id = (u16 *)p->bounce;
    memset(id, 0, 512);
    int rc = port_exec(p, ATA_IDENTIFY, 0, 0, id, 512, 0, AHCI_CMD_TIMEOUT);
    if (rc) {
        kprintf("ahci: %s: IDENTIFY DEVICE failed (%d)\n", port_name(p, nm, sizeof nm), rc);
        return rc;
    }
    if (id[0] & 0x8000) {
        kprintf("ahci: %s: IDENTIFY data says not an ATA device, skipped\n", port_name(p, nm, sizeof nm));
        return E_NOTSUP;
    }
    if (!(id[49] & (1u << 9))) {
        kprintf("ahci: %s: device has no LBA support (unsupported), skipped\n", port_name(p, nm, sizeof nm));
        return E_NOTSUP;
    }
    if (!(id[49] & (1u << 8)))
        kprintf("ahci: %s: warning: device does not report DMA support\n", port_name(p, nm, sizeof nm));

    ata_string(id, 27, 20, p->bd.model, sizeof p->bd.model);
    ata_string(id, 10, 10, serial, sizeof serial);
    ata_string(id, 23, 4, fw, sizeof fw);

    u64 lba28 = (u64)id[60] | ((u64)id[61] << 16);
    u64 lba48 = (u64)id[100] | ((u64)id[101] << 16) | ((u64)id[102] << 32) | ((u64)id[103] << 48);
    p->lba48 = (id[83] & (1u << 10)) && lba48 != 0;
    p->sectors = p->lba48 ? lba48 : lba28;

    p->sector_size = 512;
    u32 phys_per_log = 1;
    if ((id[106] & 0xC000) == 0x4000) {
        if (id[106] & (1u << 12)) {
            u32 words = (u32)id[117] | ((u32)id[118] << 16);
            if (words >= 256 && words <= 0x4000) p->sector_size = words * 2;
        }
        if (id[106] & (1u << 13)) phys_per_log = 1u << (id[106] & 0xF);
    }

    int wc_sup = (id[82] & (1u << 5)) != 0;
    int wc_en  = (id[85] & (1u << 5)) != 0;
    int fl_sup = (id[83] & (1u << 12)) != 0 || (id[86] & (1u << 12)) != 0;
    int flx_sup = (id[83] & (1u << 13)) != 0 || (id[86] & (1u << 13)) != 0;
    if (flx_sup && p->lba48) p->flush_cmd = ATA_FLUSH_CACHE_EXT;
    else if (fl_sup || wc_sup) p->flush_cmd = ATA_FLUSH_CACHE;
    else p->flush_cmd = 0;

    if (p->sectors == 0) {
        kprintf("ahci: %s: device reports zero capacity, skipped\n", port_name(p, nm, sizeof nm));
        return E_NOTSUP;
    }

    fmt_size(p->sectors * p->sector_size, sz, sizeof sz);
    kprintf("ahci: %s: \"%s\" serial \"%s\" fw \"%s\"\n", port_name(p, nm, sizeof nm),
            p->bd.model, serial, fw);
    kprintf("ahci: %s: %lu sectors x %u bytes (%s), %s, phys sector %u, write cache %s, flush %s\n",
            nm, p->sectors, p->sector_size, sz, p->lba48 ? "LBA48" : "LBA28",
            p->sector_size * phys_per_log,
            wc_sup ? (wc_en ? "on" : "off") : "n/a",
            p->flush_cmd == ATA_FLUSH_CACHE_EXT ? "EXT" : p->flush_cmd ? "28-bit" : "none");
    return 0;
}

/* ---------------- port bring-up ---------------- */
static void port_free(ahci_port_t *p) {
    if (p->hba->ports[p->index] == p) p->hba->ports[p->index] = NULL;
    dma_free(p->clb); dma_free(p->fis); dma_free(p->tbl); dma_free(p->bounce);
    kfree(p);
}

static const char *sig_name(u32 sig) {
    switch (sig) {
    case SIG_ATAPI: return "ATAPI device";
    case SIG_SEMB:  return "enclosure management bridge";
    case SIG_PM:    return "port multiplier";
    default:        return "unknown device";
    }
}

/* Bring-up, phase 1: program the port and enable FIS receive. If the link
 * is not up, COMRESET is asserted (p->in_reset) and the caller releases it
 * for all ports at once, so empty ports are probed in parallel. */
static ahci_port_t *port_prepare(ahci_hba_t *hba, int index) {
    char nm[32];
    ahci_port_t *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    p->hba = hba;
    p->index = index;
    p->regs = hba->abar + PORT_BASE(index);
    port_name(p, nm, sizeof nm);

    /* engine was stopped by the caller; make sure */
    if (px_rd(p, PX_CMD) & (CMD_ST | CMD_CR | CMD_FRE | CMD_FR)) {
        if (port_stop(p)) {
            kprintf("ahci: %s: cannot stop port engine (CMD %08x), skipped\n", nm, px_rd(p, PX_CMD));
            kfree(p);
            return NULL;
        }
    }

    p->clb = dma_alloc(1024, 1024);
    p->fis = dma_alloc(256, 256);
    p->tbl = dma_alloc(sizeof(ahci_cmd_tbl_t), 128);
    p->bounce = dma_alloc(AHCI_BOUNCE_SIZE, 4096);
    if (!p->clb || !p->fis || !p->tbl || !p->bounce) {
        kprintf("ahci: %s: out of memory\n", nm);
        port_free(p);
        return NULL;
    }
    u64 clb = virt_to_phys(p->clb), fb = virt_to_phys(p->fis);
    px_wr(p, PX_CLB, (u32)clb);
    px_wr(p, PX_CLBU, (u32)(clb >> 32));
    px_wr(p, PX_FB, (u32)fb);
    px_wr(p, PX_FBU, (u32)(fb >> 32));

    px_wr(p, PX_IE, 0);                         /* enabled once the HBA has its interrupt */
    port_clear_errors(p);

    /* power on / spin up (no-ops when CPD / SSS are not supported) */
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_POD | CMD_SUD);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_FRE);

    if (!link_up(p)) {
        comreset_assert(p);
        p->in_reset = 1;
    }
    return p;
}

/* Bring-up, phase 2 (after the link wait): check the device, IDENTIFY it
 * and register the disk. */
static void port_finish(ahci_port_t *p) {
    char nm[32];
    port_name(p, nm, sizeof nm);
    if (p->in_reset) {
        p->in_reset = 0;
        px_wr(p, PX_SERR, 0xFFFFFFFF);
    }
    if (!link_up(p)) {
        kprintf("ahci: %s: no device\n", nm);
        port_stop(p);
        port_free(p);
        return;
    }
    port_clear_errors(p);

    u32 ssts = px_rd(p, PX_SSTS);
    static const char *const speeds[] = { "?", "1.5", "3.0", "6.0" };
    const char *spd = SSTS_SPD(ssts) < 4 ? speeds[SSTS_SPD(ssts)] : "?";

    if (wait_ready(p, 5000)) {
        /* a device that never finishes its reset: try one COMRESET */
        if (!port_comreset(p) || wait_ready(p, 5000)) {
            kprintf("ahci: %s: link up (%s Gbps) but device not ready (TFD %08x), skipped\n",
                    nm, spd, px_rd(p, PX_TFD));
            port_stop(p);
            port_free(p);
            return;
        }
    }

    u32 sig = px_rd(p, PX_SIG);
    if (sig != SIG_ATA) {
        kprintf("ahci: %s: %s (signature %08x, %s Gbps) unsupported, skipped\n",
                nm, sig_name(sig), sig, spd);
        port_stop(p);
        port_free(p);
        return;
    }
    kprintf("ahci: %s: ATA disk, link %s Gbps\n", nm, spd);

    port_clear_errors(p);
    port_start_cmd(p);

    if (port_identify(p)) {
        port_stop(p);
        port_free(p);
        return;
    }

    blockdev_t *bd = &p->bd;
    snprintf(bd->name, sizeof bd->name, "sata%d", g_disk_count);
    bd->type = BDEV_DISK_SATA;
    bd->sector_size = p->sector_size;
    bd->sector_count = p->sectors;
    bd->removable = 0;
    bd->readonly = 0;
    bd->present = 1;
    bd->read = ahci_read;
    bd->write = ahci_write;
    bd->priv = p;
    g_disk_count++;
    kprintf("ahci: %s: disk %s\n", nm, bd->name);
    if (blockdev_register) {
        int rc = blockdev_register(bd);
        if (rc) kprintf("ahci: %s: blockdev_register failed (%d)\n", bd->name, rc);
    } else {
        kprintf("ahci: %s: no block layer linked\n", bd->name);
    }
}

/* ---------------- controller bring-up ---------------- */
static void hba_bios_handoff(ahci_hba_t *h) {
    if (!(h->cap2 & CAP2_BOH)) return;
    u32 bohc = hba_rd(h, HBA_BOHC);
    if (!(bohc & BOHC_BOS) && (bohc & BOHC_OOS)) return;    /* already ours */
    hba_wr(h, HBA_BOHC, bohc | BOHC_OOS);
    /* BIOS must clear BOS within 25 ms; if it sets BB it may take up to 2 s */
    u64 start = time_ms();
    for (;;) {
        bohc = hba_rd(h, HBA_BOHC);
        if (!(bohc & BOHC_BOS)) break;
        u64 el = time_ms() - start;
        if (el > 25 && !(bohc & BOHC_BB)) break;
        if (el > 2025) break;
        udelay(100);
    }
    if (bohc & BOHC_BOS) kprintf("ahci: BIOS did not release the controller (BOHC %08x), taking it\n", bohc);
    else kprintf("ahci: BIOS/OS handoff done\n");
    /* clear the OS ownership change status bit (write-1-to-clear) */
    hba_wr(h, HBA_BOHC, (hba_rd(h, HBA_BOHC) & ~BOHC_BOS) | BOHC_OOS | (1u << 3));
}

static int hba_reset(ahci_hba_t *h) {
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_AE);
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_HR);
    if (wait_reg(h->abar + HBA_GHC, GHC_HR, 0, 1000)) return E_TIMEOUT;
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_AE);
    return 0;
}

/* INTx acknowledge: clear PxIS of every interrupting port (remembering the
 * bits for port_exec), then IS */
#define AHCI_PORT_IE ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26) | (1u << 24) | \
                      (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1) | (1u << 0))
static int ahci_ack(void *ctx) {
    ahci_hba_t *h = ctx;
    u32 is = hba_rd(h, HBA_IS);
    if (!is || is == 0xFFFFFFFF) return 0;
    for (int i = 0; i < 32; i++) {
        if (!(is & (1u << i))) continue;
        volatile u8 *regs = h->abar + PORT_BASE(i);
        u32 pis = mmio_read32(regs + PX_IS);
        if (h->ports[i]) h->ports[i]->is_acc |= pis;
        mmio_write32(regs + PX_IS, pis);
    }
    hba_wr(h, HBA_IS, is);
    return 1;
}

static void hba_irq_setup(ahci_hba_t *h) {
    pci_dev_t *d = h->pci;
    snprintf(h->irq_name, sizeof h->irq_name, "AHCI %02x:%02x.%x", d->bus, d->dev, d->func);
    h->irq.name = h->irq_name;
    h->irq.pci = d;
    h->irq.ack = ahci_ack;
    h->irq.ctx = h;
    if (irq_attach(&h->irq) == IRQ_NONE) return;
    for (int i = 0; i < 32; i++)
        if (h->ports[i]) px_wr(h->ports[i], PX_IE, AHCI_PORT_IE);
    hba_wr(h, HBA_IS, 0xFFFFFFFF);
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_IE);
    /* one harmless non-data command (CHECK POWER MODE) produces the first
     * interrupt, which proves MSI or teaches the INTx route */
    for (int i = 0; i < 32; i++) {
        if (!h->ports[i]) continue;
        port_exec(h->ports[i], 0xE5, 0, 0, NULL, 0, 0, 1000);
        break;
    }
    u64 end = time_ms() + 20;
    while (!irq_live(&h->irq) && time_ms() < end) irq_service();
    if (!irq_live(&h->irq)) kprintf("ahci: %s: no interrupt seen yet, polling\n", h->irq_name);
}

static void hba_init(pci_dev_t *d) {
    pci_enable(d);
    volatile u8 *abar = pci_map_bar(d, 5);
    if (!abar) {
        kprintf("ahci: %02x:%02x.%x: no memory BAR5 (ABAR), skipped\n", d->bus, d->dev, d->func);
        return;
    }
    ahci_hba_t *h = kzalloc(sizeof *h);
    if (!h) return;
    h->pci = d;
    h->abar = abar;
    d->driver = "ahci";

    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_AE);
    h->cap = hba_rd(h, HBA_CAP);
    h->cap2 = hba_rd(h, HBA_CAP2);
    h->vs = hba_rd(h, HBA_VS);
    hba_bios_handoff(h);
    hba_wr(h, HBA_GHC, (hba_rd(h, HBA_GHC) | GHC_AE) & ~GHC_IE);

    h->pi = hba_rd(h, HBA_PI);
    h->nports = (h->cap & CAP_NP_MASK) + 1;
    if (!h->pi) h->pi = (h->nports >= 32) ? 0xFFFFFFFFu : ((1u << h->nports) - 1);

    char ver[16];
    u32 maj = h->vs >> 16, min = (h->vs >> 8) & 0xFF, sub = h->vs & 0xFF;
    if (maj == 0 && min == 9 && sub == 5) snprintf(ver, sizeof ver, "0.95");
    else if (sub) snprintf(ver, sizeof ver, "%u.%u.%u", maj, min, sub);
    else snprintf(ver, sizeof ver, "%u.%u", maj, min);
    static const char *const gen[] = { "?", "1.5", "3", "6" };
    kprintf("ahci: controller %02x:%02x.%x [%04x:%04x] %s, AHCI %s at %lx, %d ports (PI %08x), "
            "%d slots, %s Gbps%s%s%s\n",
            d->bus, d->dev, d->func, d->vendor, d->device, pci_vendor_name(d->vendor), ver,
            (u64)(uptr)abar, h->nports, h->pi, CAP_NCS(h->cap),
            CAP_ISS(h->cap) < 4 ? gen[CAP_ISS(h->cap)] : "?",
            (h->cap & CAP_S64A) ? ", 64-bit" : ", 32-bit",
            (h->cap & CAP_SSS) ? ", staggered spin-up" : "",
            (h->cap & CAP_SCLO) ? ", CLO" : "");

    /* stop all ports first; reset the HBA if one refuses */
    int stuck = 0;
    for (int i = 0; i < 32; i++) {
        if (!(h->pi & (1u << i))) continue;
        ahci_port_t tmp = { .hba = h, .index = i, .regs = abar + PORT_BASE(i) };
        if (port_stop(&tmp)) stuck = 1;
    }
    if (stuck) {
        kprintf("ahci: a port engine did not stop, resetting HBA\n");
        if (hba_reset(h)) {
            kprintf("ahci: HBA reset timed out, controller skipped\n");
            return;
        }
        hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) & ~GHC_IE);
        h->pi = hba_rd(h, HBA_PI);
        if (!h->pi) h->pi = (h->nports >= 32) ? 0xFFFFFFFFu : ((1u << h->nports) - 1);
    }
    hba_wr(h, HBA_IS, 0xFFFFFFFF);

    ahci_port_t *ports[32];
    int resetting = 0;
    for (int i = 0; i < 32; i++) {
        ports[i] = (h->pi & (1u << i)) ? port_prepare(h, i) : NULL;
        if (ports[i] && ports[i]->in_reset) resetting = 1;
    }
    if (resetting) {
        mdelay(2);                              /* COMRESET: DET=1 for >= 1 ms */
        for (int i = 0; i < 32; i++)
            if (ports[i] && ports[i]->in_reset) comreset_release(ports[i]);
        /* wait up to 1 s for links; give up early on ports where nothing
         * at all (DET == 0) was detected after 100 ms */
        u64 start = time_ms();
        for (;;) {
            int pending = 0;
            u64 el = time_ms() - start;
            for (int i = 0; i < 32; i++) {
                if (!ports[i] || !ports[i]->in_reset) continue;
                u32 det = SSTS_DET(px_rd(ports[i], PX_SSTS));
                if (det != 3 && !(det == 0 && el > 100)) pending = 1;
            }
            if (!pending || el > 1000) break;
            udelay(100);
        }
    }
    for (int i = 0; i < 32; i++) h->ports[i] = ports[i];
    for (int i = 0; i < 32; i++)
        if (ports[i]) port_finish(ports[i]);       /* frees (and forgets) ports without a disk */
    hba_irq_setup(h);
}

void ahci_init(void) {
    int found = 0;
    u64 t0 = time_ms();
    for (int idx = 0;; idx++) {
        pci_dev_t *d = pci_find_class(0x01, 0x06, -1, idx);
        if (!d) break;
        if (d->prog_if != 0x01) {
            kprintf("ahci: %02x:%02x.%x: SATA controller with prog-if %02x is not AHCI, skipped\n",
                    d->bus, d->dev, d->func, d->prog_if);
            continue;
        }
        found++;
        hba_init(d);
    }
    kprintf("ahci: %d controller(s), %d disk(s), init took %lu ms\n", found, g_disk_count,
            time_ms() - t0);
}
