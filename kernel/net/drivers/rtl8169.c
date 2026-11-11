/* Halcyon OS - Realtek RTL8169/8110 (PCI) and RTL8111/8168 (PCI Express)
 * gigabit Ethernet: the most common Ethernet chip on PC mainboards since
 * about 2006, and the RTL8169-based add-in cards.
 *
 * UNTESTED: QEMU has no model of these chips, and no real one has been tried
 * yet. Written from the Realtek RTL8169S/8110S, RTL8169SC and RTL8111B/C
 * data sheets (registers, descriptor format, PHY access through PHYAR) and
 * the per-version register notes in FreeBSD's BSD-licensed re(4) driver;
 * no GPL code was used. The adapter's line in BOOT.LOG names the chip
 * version (the TCR XID bits): please report what happens.
 *
 * Model: one ring of 128 receive descriptors with 2 KB buffers and one ring
 * of 64 transmit descriptors (16 bytes each: opts1 with OWN/EOR/FS/LS and
 * the length, opts2, a 64-bit buffer address). The receive status and length
 * come from the wire side: anything but a single-descriptor frame (FS and LS)
 * of 18..1522 bytes (the 8169 family keeps the CRC) without the error summary
 * bit is dropped. Link state and speed come from the PHYstatus register.
 * Interrupts: MSI-X, MSI or INTx; they only wake the net thread and poll()
 * clears ISR (an MSI is only sent when ISR & IMR goes from 0 to non-zero).
 *
 * Version differences handled (TCR bits 30:26 and 23:22 identify the chip):
 *   - RTL8169/8169S/8110S: transmit and receive are enabled before TCR/RCR
 *     are written, register 0x82 is set after a reset;
 *   - RTL8169SB/SC and later: the PHY is woken by writing PHY registers 0x1F
 *     and 0x0E; RTL8168D/E and others also need PMCH bit 7 (PHY power);
 *   - RTL8169SC/8110SC: the vendor's register 0x7C value, no interrupt
 *     mitigation;
 *   - RTL8168B and later (PCIe): C+ mode with MACSTAT disabled;
 *   - RTL8168C (some): wake from MAC sleep through GPIO;
 *   - RTL8168E-VL/F: "early receive off" in RCR;
 *   - RTL8168G/GU/H/EP/FP, RTL8411B: the RXDV gate in MISC must be opened or
 *     nothing is received; early receive off (v2); TCR/RCR before enabling;
 *   - RTL8168DP: wait for the previous transmit poll to be taken.
 * Not done: the vendor's per-version PHY parameter tables (undocumented
 * magic register writes: some boards may link at lower speed or not at all
 * without them), the 8168G+ MCU/OOB hand-over used with DASH management,
 * EEE, jumbo frames, offloads, Wake-on-LAN, and the RTL8101/8102/8105/8106
 * Fast Ethernet relatives and the 2.5 Gbit/s RTL8125 (not claimed). */
#include "net/netdev.h"
#include "net/drivers/nic.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

#define R_IDR0     0x00
#define R_MAR0     0x08
#define R_TNPDS    0x20     /* transmit normal priority descriptors, 64-bit, 256-byte aligned */
#define R_CR       0x37
#define R_TPPOLL   0x38
#define R_IMR      0x3C
#define R_ISR      0x3E
#define R_TCR      0x40
#define R_RCR      0x44
#define R_9346CR   0x50
#define R_PHYAR    0x60
#define R_PHYSTATUS 0x6C
#define R_MACDBG   0x6D
#define R_GPIO     0x6E
#define R_PMCH     0x6F
#define R_7C       0x7C
#define R_82       0x82
#define R_RMS      0xDA
#define R_CPLUS    0xE0
#define R_INTRMIT  0xE2
#define R_RDSAR    0xE4     /* receive descriptors, 64-bit, 256-byte aligned */
#define R_MTPS     0xEC
#define R_MISC     0xF0

#define CR_TE  0x04
#define CR_RE  0x08
#define CR_RST 0x10
#define TPPOLL_NPQ 0x40
#define PHYAR_FLAG (1u << 31)
#define PS_FDX   0x01
#define PS_LINK  0x02
#define PS_10    0x04
#define PS_100   0x08
#define PS_1000  0x10
#define RCR_APM  0x02
#define RCR_AM   0x04
#define RCR_AB   0x08
#define RCR_DMA_UNLIMITED 0x0700
#define RCR_NO_FIFO_THRESH 0xE000
#define RCR_EARLYOFF   0x3800
#define RCR_EARLYOFFV2 0x0800
#define TCR_IFG  0x03000000
#define TCR_DMA_2048 0x0700
#define CPLUS_TXENB 0x0001
#define CPLUS_RXENB 0x0002
#define CPLUS_PCI_MRW 0x0008
#define CPLUS_MACSTAT_DIS 0x0080
#define MISC_RXDV_GATE (1u << 19)
#define INTR_MASK 0x807F   /* ROK RER TOK TER RDU LinkChg FOVW SYSErr */

#define D_OWN (1u << 31)
#define D_EOR (1u << 30)
#define D_FS  (1u << 29)
#define D_LS  (1u << 28)
#define RX_RES (1u << 21)  /* receive error summary (8169 layout) */
#define TX_ES  (1u << 23)  /* transmit error summary */

#define NRX 128
#define NTX 64
#define BUFSZ 2048

/* version flags */
#define V_OLD8169   0x0001  /* 8169, 8169S, 8110S */
#define V_PHYWAKE   0x0002
#define V_PHYWAKE_PM 0x0004
#define V_MACSTAT   0x0008
#define V_EARLYOFF  0x0010
#define V_8168G     0x0020
#define V_WAITPOLL  0x0040
#define V_8110SC    0x0080
#define V_MACSLEEP  0x0100

typedef struct desc { u32 opts1, opts2; u64 addr; } __attribute__((packed)) desc_t;

typedef struct r8169 {
    netdev_t nd;
    pci_dev_t *pci;
    volatile u8 *mmio;
    u16 io;
    u32 flags;
    int pcie;
    volatile desc_t *rx, *tx;
    u8 *rxbuf, *txbuf;
    u32 tx_len[NTX];
    u32 rx_next, tx_next, tx_clean;
    irq_handle_t irq;
    char irq_name[24];
} r8169_t;

static u8  r8(r8169_t *r, u32 o)  { return r->mmio ? mmio_read8(r->mmio + o) : inb((u16)(r->io + o)); }
static u16 r16(r8169_t *r, u32 o) { return r->mmio ? mmio_read16(r->mmio + o) : inw((u16)(r->io + o)); }
static u32 r32(r8169_t *r, u32 o) { return r->mmio ? mmio_read32(r->mmio + o) : inl((u16)(r->io + o)); }
static void w8(r8169_t *r, u32 o, u8 v)   { if (r->mmio) mmio_write8(r->mmio + o, v); else outb((u16)(r->io + o), v); }
static void w16(r8169_t *r, u32 o, u16 v) { if (r->mmio) mmio_write16(r->mmio + o, v); else outw((u16)(r->io + o), v); }
static void w32(r8169_t *r, u32 o, u32 v) { if (r->mmio) mmio_write32(r->mmio + o, v); else outl((u16)(r->io + o), v); }

/* ---------------- PHY through PHYAR (the chip's own PHY) ---------------- */
static int phy_write(r8169_t *r, u32 reg, u16 v) {
    w32(r, R_PHYAR, PHYAR_FLAG | (reg & 31) << 16 | v);
    for (int i = 0; i < 1000; i++) {
        udelay(25);
        if (!(r32(r, R_PHYAR) & PHYAR_FLAG)) { udelay(20); return 0; }
    }
    return E_IO;
}

static int phy_read(r8169_t *r, u32 reg, u16 *v) {
    w32(r, R_PHYAR, (reg & 31) << 16);
    for (int i = 0; i < 1000; i++) {
        udelay(25);
        u32 x = r32(r, R_PHYAR);
        if (x & PHYAR_FLAG) { udelay(20); *v = (u16)x; return 0; }
    }
    return E_IO;
}

/* ---------------- versions ---------------- */
static const struct { u32 id; const char *name; u32 flags; } versions[] = {
    { 0x00000000, "RTL8169",        V_OLD8169 },
    { 0x00800000, "RTL8169S",       V_OLD8169 },
    { 0x04000000, "RTL8110S",       V_OLD8169 },
    { 0x10000000, "RTL8169SB/8110SB", V_OLD8169 | V_PHYWAKE },
    { 0x7CC00000, "RTL8169SBL",     V_OLD8169 | V_PHYWAKE },
    { 0x18000000, "RTL8169SC/8110SC", V_OLD8169 | V_PHYWAKE | V_8110SC },
    { 0x98000000, "RTL8169SC/8110SC", V_OLD8169 | V_PHYWAKE | V_8110SC },
    { 0x30000000, "RTL8168B/8111B", V_PHYWAKE | V_MACSTAT },
    { 0x38000000, "RTL8168B/8111B", V_PHYWAKE | V_MACSTAT },
    { 0x38400000, "RTL8168B/8111B", V_PHYWAKE | V_MACSTAT },
    { 0x3C000000, "RTL8168C/8111C", V_PHYWAKE | V_MACSTAT },
    { 0x3C400000, "RTL8168C/8111C", V_PHYWAKE | V_MACSTAT | V_MACSLEEP },
    { 0x3C800000, "RTL8168CP/8111CP", V_PHYWAKE | V_MACSTAT },
    { 0x28000000, "RTL8168D/8111D", V_PHYWAKE | V_PHYWAKE_PM | V_MACSTAT },
    { 0x28800000, "RTL8168DP/8111DP", V_PHYWAKE | V_MACSTAT | V_WAITPOLL },
    { 0x2C000000, "RTL8168E/8111E", V_PHYWAKE | V_PHYWAKE_PM | V_MACSTAT },
    { 0x2C800000, "RTL8168E-VL/8111E-VL", V_PHYWAKE | V_MACSTAT | V_EARLYOFF },
    { 0x48000000, "RTL8168F/8111F", V_PHYWAKE | V_MACSTAT | V_EARLYOFF },
    { 0x48800000, "RTL8411",        V_PHYWAKE | V_MACSTAT },
    { 0x4C000000, "RTL8168G/8111G", V_PHYWAKE | V_MACSTAT | V_8168G },
    { 0x50800000, "RTL8168GU/8111GU", V_PHYWAKE | V_MACSTAT | V_8168G },
    { 0x50000000, "RTL8168EP/8111EP", V_PHYWAKE | V_MACSTAT | V_8168G },
    { 0x54000000, "RTL8168H/8111H", V_PHYWAKE | V_MACSTAT | V_8168G },
    { 0x54800000, "RTL8168FP/8111FP", V_PHYWAKE | V_MACSTAT | V_8168G },
    { 0x5C800000, "RTL8411B",       V_PHYWAKE | V_MACSTAT | V_8168G },
};

/* ---------------- run time ---------------- */
static int r8169_ack(void *ctx) {
    r8169_t *r = ctx;
    u16 isr = r16(r, R_ISR);
    if (isr == 0xFFFF) return 0;
    if (isr) w16(r, R_ISR, isr);
    return isr != 0;
}

static void link_update(r8169_t *r) {
    u8 ps = r8(r, R_PHYSTATUS);
    r->nd.link_up = !!(ps & PS_LINK);
    r->nd.full_duplex = !!(ps & PS_FDX);
    r->nd.speed_mbps = (ps & PS_1000) ? 1000 : (ps & PS_100) ? 100 : (ps & PS_10) ? 10 : 0;
}

static void rx_post(r8169_t *r, u32 i) {
    r->rx[i].addr = virt_to_phys(r->rxbuf + i * BUFSZ);
    r->rx[i].opts2 = 0;
    wmb();
    r->rx[i].opts1 = D_OWN | (i == NRX - 1 ? D_EOR : 0) | BUFSZ;
}

static int tx_reap(r8169_t *r) {
    int n = 0;
    while (r->tx_clean != r->tx_next) {
        u32 o = r->tx[r->tx_clean].opts1;
        if (o & D_OWN) break;
        if (o & TX_ES) r->nd.tx_errors++;
        else { r->nd.tx_packets++; r->nd.tx_bytes += r->tx_len[r->tx_clean]; }
        r->tx_clean = (r->tx_clean + 1) % NTX;
        n++;
    }
    return n;
}

static int r8169_xmit(netdev_t *nd, const void *frame, u32 len) {
    r8169_t *r = nd->priv;
    if (len < ETH_HDR || len > ETH_MAX_FRAME) return E_INVAL;
    u32 n = (r->tx_next + 1) % NTX;
    if (n == r->tx_clean) tx_reap(r);
    if (n == r->tx_clean) return E_BUSY;
    u32 i = r->tx_next;
    u8 *buf = r->txbuf + i * BUFSZ;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }     /* not every version pads */
    r->tx_len[i] = len;
    volatile desc_t *d = &r->tx[i];
    d->addr = virt_to_phys(buf);
    d->opts2 = 0;
    wmb();
    d->opts1 = D_OWN | D_FS | D_LS | (i == NTX - 1 ? D_EOR : 0) | len;
    r->tx_next = n;
    wmb();
    if (r->flags & V_WAITPOLL)
        for (int k = 0; k < 100 && (r8(r, R_TPPOLL) & TPPOLL_NPQ); k++) udelay(5);
    w8(r, R_TPPOLL, TPPOLL_NPQ);
    return 0;
}

static void r8169_poll(netdev_t *nd) {
    r8169_t *r = nd->priv;
    u16 isr = r16(r, R_ISR);
    if (isr && isr != 0xFFFF) w16(r, R_ISR, isr);    /* re-arms MSI */
    int work = 0;
    for (int budget = 0; budget < NRX; budget++) {
        volatile desc_t *d = &r->rx[r->rx_next];
        u32 o = d->opts1;
        if (o & D_OWN) break;
        rmb();
        work = 1;
        u32 len = o & 0x3FFF;                    /* counts the CRC */
        if ((o & (D_FS | D_LS)) == (D_FS | D_LS) && !(o & RX_RES) && len >= ETH_HDR + 4 && len <= ETH_MAX_FRAME + 8) {
            nd->rx_packets++;
            nd->rx_bytes += len - 4;
            netdev_rx(nd, r->rxbuf + r->rx_next * BUFSZ, len - 4);
        } else {
            nd->rx_errors++;                     /* errors, or a frame spread over several buffers */
        }
        rx_post(r, r->rx_next);
        r->rx_next = (r->rx_next + 1) % NRX;
    }
    if (tx_reap(r)) work = 1;
    /* frames still queued and no poll pending: the chip may have missed one */
    if (r->tx_clean != r->tx_next && !(r8(r, R_TPPOLL) & TPPOLL_NPQ)) w8(r, R_TPPOLL, TPPOLL_NPQ);
    if (work) irq_note_work(&r->irq);
    link_update(r);
    nd->irq_live = irq_live(&r->irq);
}

static void r8169_set_multicast(netdev_t *nd, const u8 (*macs)[6], int n) {
    r8169_t *r = nd->priv;
    u8 mar[8];
    memset(mar, 0, sizeof mar);
    for (int i = 0; i < n; i++) {
        u32 bit = nic_crc32_be(macs[i], 6) >> 26;
        mar[bit >> 3] |= (u8)(1u << (bit & 7));
    }
    /* the PCI Express versions take the hash in the opposite register order */
    for (int i = 0; i < 8; i++) w8(r, R_MAR0 + (u32)i, r->pcie ? mar[7 - i] : mar[i]);
}

static void r8169_attach(pci_dev_t *pd) {
    r8169_t *r = kzalloc(sizeof *r);
    if (!r) return;
    r->pci = pd;
    pci_enable(pd);
    /* the first memory BAR with the registers (BAR1 on the 8169, BAR2 on the 8168); else I/O BAR0,
     * always for the 8169SC, whose memory mapping is reported not to work */
    r->pcie = pci_find_cap(pd, 0x10) != 0;
    for (int b = 1; b < 6 && !r->mmio && pd->device != 0x8167; b++)
        if (!pd->bar[b].is_io && pd->bar[b].addr && pd->bar[b].size >= 0x100) r->mmio = pci_map_bar(pd, b);
    if (!r->mmio) {
        if (!pd->bar[0].is_io || !pd->bar[0].addr) {
            kprintf("r8169: %02x:%02x.%x: no usable BAR\n", pd->bus, pd->dev, pd->func);
            kfree(r);
            return;
        }
        r->io = (u16)pd->bar[0].addr;
    }
    u32 tcr = r32(r, R_TCR);
    if (tcr == 0xFFFFFFFF) {
        kprintf("r8169: %02x:%02x.%x: no answer from the chip\n", pd->bus, pd->dev, pd->func);
        kfree(r);
        return;
    }
    /* the version ID: bits 30:26 and 23:22 of TCR; for the PCI 8169 family bits 31:26 and 23 */
    u32 ver = (tcr & 0x60000000) == 0 ? tcr & 0xFC800000 : tcr & 0x7CC00000;
    const char *name = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(versions); i++)
        if (ver == versions[i].id) {
            name = versions[i].name; r->flags = versions[i].flags; break;
        }
    if (!name) {
        /* unknown: newer than this table, treated like the 8168G family */
        name = "RTL8168 (unknown)";
        r->flags = V_PHYWAKE | V_MACSTAT | V_8168G;
    }
    r->rx = dma_alloc(sizeof(desc_t) * NRX, 256);
    r->tx = dma_alloc(sizeof(desc_t) * NTX, 256);
    r->rxbuf = dma_alloc(NRX * BUFSZ, 4096);
    r->txbuf = dma_alloc(NTX * BUFSZ, 4096);
    if (!r->rx || !r->tx || !r->rxbuf || !r->txbuf) {
        kprintf("r8169: out of memory\n");
        dma_free((void *)r->rx); dma_free((void *)r->tx); dma_free(r->rxbuf); dma_free(r->txbuf); kfree(r);
        return;
    }
    /* wake the MAC and the PHY */
    if (r->flags & V_MACSLEEP) {
        if (r8(r, R_MACDBG) & 0x80) w8(r, R_GPIO, r8(r, R_GPIO) | 0x01);
        else w8(r, R_GPIO, r8(r, R_GPIO) & ~0x01);
    }
    if (r->flags & V_PHYWAKE_PM) w8(r, R_PMCH, r8(r, R_PMCH) | 0x80);
    if (r->flags & V_PHYWAKE) { phy_write(r, 0x1F, 0); phy_write(r, 0x0E, 0); }
    /* reset */
    w16(r, R_IMR, 0);
    w8(r, R_CR, CR_RST);
    for (int i = 0; i < 1000 && (r8(r, R_CR) & CR_RST); i++) udelay(10);
    if (r8(r, R_CR) & CR_RST) kprintf("r8169: %02x:%02x.%x: reset did not complete\n", pd->bus, pd->dev, pd->func);
    if (r->flags & V_OLD8169) w8(r, R_82, 1);
    for (int i = 0; i < 6; i++) r->nd.mac[i] = r8(r, R_IDR0 + (u32)i);
    /* PHY: powered up, auto-negotiating */
    u16 bmcr;
    if (!phy_read(r, 0, &bmcr) && ((bmcr & 0x0C00) || !(bmcr & 0x1000)))
        phy_write(r, 0, (u16)((bmcr & ~0x0C00) | 0x1000 | 0x0200));
    for (u32 i = 0; i < NRX; i++) rx_post(r, i);
    w8(r, R_9346CR, 0xC0);                      /* configuration registers writable */
    u16 cplus = CPLUS_PCI_MRW;
    cplus |= (r->flags & V_MACSTAT) ? (CPLUS_MACSTAT_DIS | 0x0001) : (CPLUS_RXENB | CPLUS_TXENB);
    w16(r, R_CPLUS, cplus);                      /* C+ mode first: before the other registers */
    if (r->flags & V_8110SC) {
        w32(r, R_7C, 0x000FFF00 | ((r8(r, 0x53) & 0x08) ? 0xFF : 0));   /* vendor value; low byte with 66 MHz PCI */
        w16(r, R_INTRMIT, 0);
    }
    w32(r, R_RDSAR + 4, (u32)(virt_to_phys((void *)r->rx) >> 32));
    w32(r, R_RDSAR, (u32)virt_to_phys((void *)r->rx));
    w32(r, R_TNPDS + 4, (u32)(virt_to_phys((void *)r->tx) >> 32));
    w32(r, R_TNPDS, (u32)virt_to_phys((void *)r->tx));
    if (r->flags & V_8168G) w32(r, R_MISC, r32(r, R_MISC) & ~MISC_RXDV_GATE);
    if (!(r->flags & V_8168G)) w8(r, R_CR, CR_TE | CR_RE);   /* before TCR/RCR, except 8168G+ */
    w32(r, R_TCR, TCR_IFG | TCR_DMA_2048);
    w8(r, R_MTPS, 0x3F);
    u32 rcr = RCR_AB | RCR_APM | RCR_AM | RCR_DMA_UNLIMITED | RCR_NO_FIFO_THRESH;
    if (r->flags & V_EARLYOFF) rcr |= RCR_EARLYOFF;
    if (r->flags & V_8168G) rcr |= RCR_EARLYOFFV2;
    w32(r, R_RCR, rcr);
    w16(r, R_RMS, BUFSZ);
    for (int i = 0; i < 8; i++) w8(r, R_MAR0 + (u32)i, 0);
    if (r->flags & V_8168G) w8(r, R_CR, CR_TE | CR_RE);
    w8(r, R_9346CR, 0x00);
    w16(r, R_ISR, 0xFFFF);
    snprintf(r->irq_name, sizeof r->irq_name, "r8169 %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    r->irq.name = r->irq_name;
    r->irq.pci = pd;
    r->irq.ack = r8169_ack;
    r->irq.ctx = r;
    if (irq_attach(&r->irq) != IRQ_NONE) w16(r, R_IMR, INTR_MASK);
    link_update(r);
    r->nd.driver = "r8169";
    snprintf(r->nd.desc, sizeof r->nd.desc, "Realtek %s Gigabit Ethernet (XID %08x)", name, ver);
    r->nd.xmit = r8169_xmit;
    r->nd.poll = r8169_poll;
    r->nd.set_multicast = r8169_set_multicast;
    r->nd.priv = r;
    r->nd.pci = pd;
    r->nd.irq = &r->irq;
    pd->driver = "r8169";
    if (netdev_register(&r->nd)) kprintf("r8169: too many network adapters\n");
}

static const struct { u16 vendor, device; } ids[] = {
    { 0x10EC, 0x8161 }, { 0x10EC, 0x8167 }, { 0x10EC, 0x8168 }, { 0x10EC, 0x8169 },
    { 0x1186, 0x4300 }, { 0x1186, 0x4302 }, { 0x1259, 0xC107 }, { 0x1737, 0x1032 }, { 0x16EC, 0x0116 },
};

void rtl8169_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->driver) continue;
        for (unsigned k = 0; k < ARRAY_SIZE(ids); k++)
            if (pd->vendor == ids[k].vendor && pd->device == ids[k].device) { r8169_attach(pd); break; }
    }
}
