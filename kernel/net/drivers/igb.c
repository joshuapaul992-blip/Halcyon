/* Halcyon OS - Intel gigabit Ethernet with advanced descriptors ("igb"):
 * 82576 (QEMU -device igb), I350, and the I210/I211 common on desktop and
 * embedded boards.
 *
 * References: Intel 82576EB GbE Controller Datasheet, Intel Ethernet
 * Controller I350 Datasheet, Intel Ethernet Controller I210 Datasheet
 * (sections 4.5/4.6 initialization, 7.1/7.2 descriptors, 7.3 interrupts,
 * 8 registers).
 *
 * One receive and one transmit queue (queue 0), advanced descriptors: the
 * receive queue in "advanced, one buffer" mode (SRRCTL) with 2 KB buffers,
 * advanced transmit data descriptors. Interrupts: with MSI-X the chip runs in
 * MSI-X mode (GPIE.MSIX_MODE): receive queue 0, transmit queue 0 and the
 * "other" causes (link) all on vector 0 through IVAR0 / IVAR_MISC, EICR read
 * and cleared by poll(); with MSI or INTx the legacy ICR causes. They only
 * wake the net thread (DESIGN-M2). Tested in QEMU with the 82576 (MSI-X,
 * INTx through the I/O APIC and the PIC, polled); the I350, I210 and I211
 * paths (same registers, the PHY semaphore through SW_FW_SYNC) are written to
 * the datasheets and untested. Copper ports only: fiber, SerDes and SGMII
 * need their own link setup. The MAC address must be in RAL0/RAH0 (loaded
 * from the NVM / iNVM at reset); a blank I210/I211 iNVM gives no address and
 * the port is not used. */
#include "net/netdev.h"
#include "net/drivers/nic.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

#define REG_CTRL      0x0000
#define REG_STATUS    0x0008
#define REG_EEC       0x0010
#define REG_EERD      0x0014
#define REG_CTRL_EXT  0x0018
#define REG_MDIC      0x0020
#define REG_ICR       0x00C0
#define REG_IMS       0x00D0
#define REG_IMC       0x00D8
#define REG_RCTL      0x0100
#define REG_TCTL      0x0400
#define REG_GPIE      0x1514
#define REG_EIMS      0x1524
#define REG_EIMC      0x1528
#define REG_EIAC      0x152C
#define REG_EIAM      0x1530
#define REG_EICR      0x1580
#define REG_IVAR0     0x1700
#define REG_IVAR_MISC 0x1740
#define REG_MPC       0x4010
#define REG_RXCSUM    0x5000
#define REG_MTA       0x5200
#define REG_RAL0      0x5400
#define REG_RAH0      0x5404
#define REG_SWSM      0x5B50
#define REG_SW_FW_SYNC 0x5B5C
#define REG_RDBAL     0xC000
#define REG_RDBAH     0xC004
#define REG_RDLEN     0xC008
#define REG_SRRCTL    0xC00C
#define REG_RDH       0xC010
#define REG_RDT       0xC018
#define REG_RXDCTL    0xC028
#define REG_TDBAL     0xE000
#define REG_TDBAH     0xE004
#define REG_TDLEN     0xE008
#define REG_TDH       0xE010
#define REG_TDT       0xE018
#define REG_TXDCTL    0xE028

#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_LRST     (1u << 3)
#define CTRL_SLU      (1u << 6)
#define CTRL_ILOS     (1u << 7)
#define CTRL_FRCSPD   (1u << 11)
#define CTRL_FRCDPX   (1u << 12)
#define CTRL_RST      (1u << 26)
#define CTRL_PHY_RST  (1u << 31)
#define STATUS_FD     (1u << 0)
#define STATUS_LU     (1u << 1)
#define STATUS_GIO_MASTER_EN (1u << 19)
#define EEC_AUTO_RD   (1u << 9)
#define CTRL_EXT_DRV_LOAD (1u << 28)
#define SWSM_SMBI     (1u << 0)
#define SWSM_SWESMBI  (1u << 1)
#define MDIC_WRITE    (1u << 26)
#define MDIC_READ     (2u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)
#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15)
#define RCTL_SECRC    (1u << 26)
#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)
#define TCTL_RTLC     (1u << 24)
#define GPIE_NSICR    (1u << 0)
#define GPIE_MSIX_MODE (1u << 4)
#define GPIE_PBA      (1u << 31)
#define SRRCTL_ONEBUF (1u << 25)
#define SRRCTL_DROP_EN (1u << 31)
#define QUEUE_ENABLE  (1u << 25)
#define ICR_TXDW      (1u << 0)
#define ICR_LSC       (1u << 2)
#define ICR_RXDMT0    (1u << 4)
#define ICR_RXO       (1u << 6)
#define ICR_RXDW      (1u << 7)

#define BMCR_ANRESTART (1u << 9)
#define BMCR_ISOLATE  (1u << 10)
#define BMCR_PDOWN    (1u << 11)
#define BMCR_ANENABLE (1u << 12)

/* advanced descriptors */
typedef union rxd {
    struct { u64 pkt_addr, hdr_addr; } read;
    struct { u32 info, rss; u32 staterr; u16 len, vlan; } wb;
} __attribute__((packed)) rxd_t;
typedef struct txd { u64 addr; u32 cmd_type_len, olinfo_status; } __attribute__((packed)) txd_t;
#define RXS_DD        0x01
#define RXS_EOP       0x02
#define RXE_FRAME_ERR 0x97000000u      /* CE, SE, SEQ, CXE, RXE: errors on the wire */
#define TXD_DTYP_DATA 0x00300000u
#define TXD_EOP       0x01000000u
#define TXD_IFCS      0x02000000u
#define TXD_RS        0x08000000u
#define TXD_DEXT      0x20000000u
#define TXS_DD        0x01

#define NRX 128
#define NTX 128
#define BUFSZ 2048

enum { MT_82576, MT_I350, MT_I210, MT_I211 };

typedef struct igb {
    netdev_t nd;
    pci_dev_t *pci;
    volatile u8 *regs;
    int type;
    volatile rxd_t *rx;
    volatile txd_t *tx;
    u8 *rxbuf, *txbuf;
    u32 tx_len[NTX];
    u32 rx_next, tx_next, tx_clean;
    int msix;
    u32 phy_sm;                 /* this port's PHY bit in SW_FW_SYNC */
    irq_handle_t irq;
    char irq_name[24];
} igb_t;

static u32 rd(igb_t *g, u32 r) { return mmio_read32(g->regs + r); }
static void wr(igb_t *g, u32 r, u32 v) { mmio_write32(g->regs + r, v); }

/* ---------------- PHY: SW_FW_SYNC under the SWSM hardware semaphore ---------------- */
static int swsm_get(igb_t *g) {
    for (int i = 0; i < 2000; i++) { if (!(rd(g, REG_SWSM) & SWSM_SMBI)) goto fw; udelay(50); }
    return -1;
fw:
    for (int i = 0; i < 2000; i++) {
        wr(g, REG_SWSM, rd(g, REG_SWSM) | SWSM_SWESMBI);
        if (rd(g, REG_SWSM) & SWSM_SWESMBI) return 0;
        udelay(50);
    }
    wr(g, REG_SWSM, rd(g, REG_SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI));
    return -1;
}
static void swsm_put(igb_t *g) { wr(g, REG_SWSM, rd(g, REG_SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI)); }

static int phy_get(igb_t *g) {
    for (int i = 0; i < 200; i++) {
        if (swsm_get(g)) return -1;
        u32 s = rd(g, REG_SW_FW_SYNC);
        if (!(s & (g->phy_sm | g->phy_sm << 16))) {      /* neither software nor firmware holds it */
            wr(g, REG_SW_FW_SYNC, s | g->phy_sm);
            swsm_put(g);
            return 0;
        }
        swsm_put(g);
        mdelay(5);
    }
    return -1;
}
static void phy_put(igb_t *g) {
    if (swsm_get(g)) return;
    wr(g, REG_SW_FW_SYNC, rd(g, REG_SW_FW_SYNC) & ~g->phy_sm);
    swsm_put(g);
}

static int mdic(igb_t *g, u32 reg, u32 op, u16 data, u16 *out) {
    wr(g, REG_MDIC, (u32)data | (reg & 31) << 16 | 1u << 21 | op);    /* the internal PHY: address 1 */
    for (int i = 0; i < 2000; i++) {
        udelay(5);
        u32 v = rd(g, REG_MDIC);
        if (v & MDIC_READY) {
            if (v & MDIC_ERROR) return E_IO;
            if (out) *out = (u16)v;
            return 0;
        }
    }
    return E_IO;
}

/* powered up, auto-negotiation (re)started after the reset */
static void phy_setup(igb_t *g) {
    int held = !phy_get(g);
    u16 bmcr;
    if (!held) kprintf("igb: the firmware holds the PHY semaphore, going ahead without it\n");
    if (!mdic(g, 0, MDIC_READ, 0, &bmcr) && bmcr != 0xFFFF)
        mdic(g, 0, MDIC_WRITE, (u16)((bmcr & ~(BMCR_PDOWN | BMCR_ISOLATE)) | BMCR_ANENABLE | BMCR_ANRESTART), NULL);
    else
        kprintf("igb: no answer from the PHY\n");
    if (held) phy_put(g);
}

/* ---------------- run time ---------------- */
static int igb_ack(void *ctx) {
    igb_t *g = ctx;
    return rd(g, REG_ICR) != 0;                  /* GPIE.NSICR: the read clears */
}

static void link_update(igb_t *g) {
    u32 st = rd(g, REG_STATUS);
    g->nd.link_up = !!(st & STATUS_LU);
    g->nd.full_duplex = !!(st & STATUS_FD);
    u32 sp = (st >> 6) & 3;
    g->nd.speed_mbps = sp == 0 ? 10 : sp == 1 ? 100 : 1000;
}

static void rx_post(igb_t *g, u32 i) {
    g->rx[i].read.pkt_addr = virt_to_phys(g->rxbuf + i * BUFSZ);
    g->rx[i].read.hdr_addr = 0;                  /* its bit 0 is DD once written back */
}

static int tx_reap(igb_t *g) {
    int n = 0;
    while (g->tx_clean != g->tx_next && (g->tx[g->tx_clean].olinfo_status & TXS_DD)) {
        g->nd.tx_packets++;
        g->nd.tx_bytes += g->tx_len[g->tx_clean];
        g->tx_clean = (g->tx_clean + 1) % NTX;
        n++;
    }
    return n;
}

static int igb_xmit(netdev_t *nd, const void *frame, u32 len) {
    igb_t *g = nd->priv;
    if (len < ETH_HDR || len > ETH_MAX_FRAME) return E_INVAL;
    u32 n = (g->tx_next + 1) % NTX;
    if (n == g->tx_clean) tx_reap(g);
    if (n == g->tx_clean) return E_BUSY;         /* ring full */
    u8 *buf = g->txbuf + g->tx_next * BUFSZ;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }
    volatile txd_t *d = &g->tx[g->tx_next];
    d->addr = virt_to_phys(buf);
    d->cmd_type_len = len | TXD_DTYP_DATA | TXD_EOP | TXD_IFCS | TXD_RS | TXD_DEXT;
    d->olinfo_status = len << 14;                /* PAYLEN: the whole frame (no offloads) */
    g->tx_len[g->tx_next] = len;
    g->tx_next = n;
    wmb();
    wr(g, REG_TDT, g->tx_next);
    return 0;
}

static void igb_poll(netdev_t *nd) {
    igb_t *g = nd->priv;
    u32 icr = rd(g, REG_ICR);
    if (g->msix) { u32 eicr = rd(g, REG_EICR); if (eicr) wr(g, REG_EICR, eicr); }
    int work = 0;
    u32 posted = NRX;
    for (int budget = 0; budget < NRX; budget++) {
        volatile rxd_t *d = &g->rx[g->rx_next];
        u32 st = d->wb.staterr;
        if (!(st & RXS_DD)) break;
        rmb();
        work = 1;
        u32 len = d->wb.len;
        if ((st & RXS_EOP) && !(st & RXE_FRAME_ERR) && len >= ETH_HDR && len <= ETH_MAX_FRAME + 4) {
            nd->rx_packets++;
            nd->rx_bytes += len;
            netdev_rx(nd, g->rxbuf + g->rx_next * BUFSZ, len);
        } else {
            nd->rx_errors++;                     /* errors, or a frame larger than one buffer */
        }
        rx_post(g, g->rx_next);
        posted = g->rx_next;
        g->rx_next = (g->rx_next + 1) % NRX;
    }
    if (posted != NRX) {                         /* give the descriptors back, all at once */
        wmb();
        wr(g, REG_RDT, posted);
    }
    if (tx_reap(g)) work = 1;
    nd->rx_dropped += rd(g, REG_MPC);
    if (work) irq_note_work(&g->irq);
    if ((icr & ICR_LSC) || !nd->link_up) link_update(g);
    nd->irq_live = irq_live(&g->irq);
}

static void igb_set_multicast(netdev_t *nd, const u8 (*macs)[6], int n) {
    igb_t *g = nd->priv;
    u32 mta[128];
    memset(mta, 0, sizeof mta);
    for (int i = 0; i < n; i++) { u32 h = nic_intel_mta_hash(macs[i]); mta[h >> 5] |= 1u << (h & 31); }
    for (int i = 0; i < 128; i++) wr(g, REG_MTA + 4u * (u32)i, mta[i]);
}

static int wait_bit(igb_t *g, u32 reg, u32 bit) {
    for (int i = 0; i < 1000; i++) { if (rd(g, reg) & bit) return 0; udelay(10); }
    return -1;
}

static void igb_attach(pci_dev_t *pd, int type, const char *model) {
    igb_t *g = kzalloc(sizeof *g);
    if (!g) return;
    g->pci = pd;
    g->type = type;
    pci_enable(pd);
    g->regs = pci_map_bar(pd, 0);
    if (!g->regs || pd->bar[0].size < 0x20000) {
        kprintf("igb: %02x:%02x.%x: cannot map BAR0\n", pd->bus, pd->dev, pd->func);
        kfree(g);
        return;
    }
    /* stop DMA, reset */
    wr(g, REG_CTRL, rd(g, REG_CTRL) | CTRL_GIO_MASTER_DISABLE);
    for (int i = 0; i < 800 && (rd(g, REG_STATUS) & STATUS_GIO_MASTER_EN); i++) udelay(100);
    wr(g, REG_IMC, 0xFFFFFFFF);
    wr(g, REG_EIMC, 0xFFFFFFFF);
    wr(g, REG_RCTL, 0);
    wr(g, REG_TCTL, TCTL_PSP);
    (void)rd(g, REG_STATUS);
    mdelay(10);
    wr(g, REG_CTRL, rd(g, REG_CTRL) | CTRL_RST);
    mdelay(2);
    for (int i = 0; i < 1000 && (rd(g, REG_CTRL) & CTRL_RST); i++) udelay(10);
    for (int i = 0; i < 1000 && !(rd(g, REG_EEC) & EEC_AUTO_RD); i++) udelay(100);
    wr(g, REG_IMC, 0xFFFFFFFF);
    wr(g, REG_EIMC, 0xFFFFFFFF);
    (void)rd(g, REG_ICR);
    (void)rd(g, REG_EICR);
    u32 func = (rd(g, REG_STATUS) >> 2) & 3;
    g->phy_sm = func == 0 ? 0x02 : func == 1 ? 0x04 : func == 2 ? 0x20 : 0x40;
    /* link: auto-negotiated by the PHY */
    wr(g, REG_CTRL, (rd(g, REG_CTRL) | CTRL_SLU) & ~(CTRL_LRST | CTRL_ILOS | CTRL_FRCSPD | CTRL_FRCDPX | CTRL_PHY_RST));
    wr(g, REG_CTRL_EXT, rd(g, REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    phy_setup(g);
    u32 ral = rd(g, REG_RAL0), rah = rd(g, REG_RAH0);
    if (!(rah & (1u << 31)) || (!ral && !(rah & 0xFFFF)) || (ral & 1)) {
        kprintf("igb: %02x:%02x.%x: no MAC address (blank NVM?)\n", pd->bus, pd->dev, pd->func);
        wr(g, REG_CTRL_EXT, rd(g, REG_CTRL_EXT) & ~CTRL_EXT_DRV_LOAD);
        kfree(g);
        return;
    }
    for (int i = 0; i < 4; i++) g->nd.mac[i] = (u8)(ral >> (8 * i));
    g->nd.mac[4] = (u8)rah; g->nd.mac[5] = (u8)(rah >> 8);
    for (int i = 0; i < 128; i++) wr(g, REG_MTA + i * 4, 0);
    g->rx = dma_alloc(sizeof(rxd_t) * NRX, 128);
    g->tx = dma_alloc(sizeof(txd_t) * NTX, 128);
    g->rxbuf = dma_alloc(NRX * BUFSZ, 4096);
    g->txbuf = dma_alloc(NTX * BUFSZ, 4096);
    if (!g->rx || !g->tx || !g->rxbuf || !g->txbuf) {
        kprintf("igb: out of memory\n");
        dma_free((void *)g->rx); dma_free((void *)g->tx); dma_free(g->rxbuf); dma_free(g->txbuf); kfree(g);
        return;
    }
    for (u32 i = 0; i < NRX; i++) rx_post(g, i);
    /* receive queue 0: advanced one-buffer descriptors, 2 KB buffers, drop when no descriptor is free */
    wr(g, REG_RXCSUM, 0);
    wr(g, REG_RDBAL, (u32)virt_to_phys((void *)g->rx));
    wr(g, REG_RDBAH, (u32)(virt_to_phys((void *)g->rx) >> 32));
    wr(g, REG_RDLEN, sizeof(rxd_t) * NRX);
    wr(g, REG_SRRCTL, (BUFSZ >> 10) | SRRCTL_ONEBUF | SRRCTL_DROP_EN);
    wr(g, REG_RDH, 0);
    wr(g, REG_RDT, 0);
    wr(g, REG_RXDCTL, QUEUE_ENABLE | (8u << 0) | (8u << 8) | (1u << 16));
    if (wait_bit(g, REG_RXDCTL, QUEUE_ENABLE)) kprintf("igb: %02x:%02x.%x: receive queue not enabled\n", pd->bus, pd->dev, pd->func);
    wr(g, REG_RDT, NRX - 1);
    wr(g, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    /* transmit queue 0 */
    wr(g, REG_TDBAL, (u32)virt_to_phys((void *)g->tx));
    wr(g, REG_TDBAH, (u32)(virt_to_phys((void *)g->tx) >> 32));
    wr(g, REG_TDLEN, sizeof(txd_t) * NTX);
    wr(g, REG_TDH, 0);
    wr(g, REG_TDT, 0);
    wr(g, REG_TXDCTL, QUEUE_ENABLE | (8u << 0) | (1u << 8) | (1u << 16));
    if (wait_bit(g, REG_TXDCTL, QUEUE_ENABLE)) kprintf("igb: %02x:%02x.%x: transmit queue not enabled\n", pd->bus, pd->dev, pd->func);
    wr(g, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_RTLC | (0x0Fu << 4) | (0x3Fu << 12));
    (void)rd(g, REG_MPC);
    /* interrupts: wake-ups only */
    snprintf(g->irq_name, sizeof g->irq_name, "igb %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    g->irq.name = g->irq_name;
    g->irq.pci = pd;
    g->irq.ack = igb_ack;
    g->irq.ctx = g;
    g->msix = irq_attach(&g->irq) == IRQ_MSIX;
    if (g->msix) {
        /* MSI-X mode: rx queue 0 (byte 0), tx queue 0 (byte 1) and the other causes all to vector 0 */
        wr(g, REG_GPIE, GPIE_NSICR | GPIE_MSIX_MODE | GPIE_PBA);
        wr(g, REG_IVAR0, 0x00008080);
        wr(g, REG_IVAR_MISC, 0x00008000);
        wr(g, REG_EIAC, 0);                      /* no auto-clear: poll() clears EICR */
        wr(g, REG_EIAM, 0);
        wr(g, REG_EIMS, 1);
    } else {
        wr(g, REG_GPIE, GPIE_NSICR);
    }
    wr(g, REG_IMS, ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXDW);
    link_update(g);
    g->nd.driver = "igb";
    snprintf(g->nd.desc, sizeof g->nd.desc, "Intel %s Gigabit Ethernet", model);
    g->nd.xmit = igb_xmit;
    g->nd.poll = igb_poll;
    g->nd.set_multicast = igb_set_multicast;
    g->nd.priv = g;
    g->nd.pci = pd;
    g->nd.irq = &g->irq;
    pd->driver = "igb";
    if (netdev_register(&g->nd)) kprintf("igb: too many network adapters\n");
}

/* copper ports */
static const struct { u16 id; u8 type; const char *model; } models[] = {
    { 0x10C9, MT_82576, "82576" }, { 0x10E8, MT_82576, "82576 (quad)" }, { 0x1526, MT_82576, "82576 (quad)" },
    { 0x150A, MT_82576, "82576NS" }, { 0x1521, MT_I350, "I350" }, { 0x1533, MT_I210, "I210" },
    { 0x1534, MT_I210, "I210" }, { 0x1535, MT_I210, "I210-IT" }, { 0x157B, MT_I210, "I210 (flashless)" },
    { 0x1539, MT_I211, "I211" },
};

void igb_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->vendor != 0x8086 || pd->driver) continue;
        for (unsigned m = 0; m < ARRAY_SIZE(models); m++)
            if (pd->device == models[m].id) { igb_attach(pd, models[m].type, models[m].model); break; }
    }
}
