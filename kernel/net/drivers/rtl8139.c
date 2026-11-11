/* Halcyon OS - Realtek RTL8139 family Fast Ethernet (QEMU -device rtl8139,
 * the RTL8139A/B/C/C+/D, RTL8100/8130 and the many cards built on them).
 *
 * Reference: Realtek RTL8139(A/B/C/D) data sheets and the "RTL8139(A/B)
 * Programming Guide" (register set, receive ring, transmit descriptors).
 *
 * Receive: one 32 KB ring the chip fills with [status, length, frame, CRC]
 * records aligned to 4 bytes; WRAP mode, so a record that crosses the end is
 * written linearly past it into a 2 KB overflow area. The record headers come
 * from the wire side of the chip: a length outside 18..1522 bytes or a record
 * without "receive OK" means the ring is out of step, and the receiver is
 * reset. Transmit: the chip's four transmit descriptors, used round robin.
 * The RTL8139 has INTx only; the interrupt just wakes the net thread, and
 * poll() works from the chip's state (BUFE, TSD, media status), never from
 * the interrupt status. Tested in QEMU (INTx through the I/O APIC and the
 * PIC, and polled); the C+ mode descriptor interface is not used. */
#include "net/netdev.h"
#include "net/drivers/nic.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

#define R_IDR0    0x00
#define R_MAR0    0x08
#define R_TSD0    0x10
#define R_TSAD0   0x20
#define R_RBSTART 0x30
#define R_CR      0x37
#define R_CAPR    0x38
#define R_CBR     0x3A
#define R_IMR     0x3C
#define R_ISR     0x3E
#define R_TCR     0x40
#define R_RCR     0x44
#define R_MPC     0x4C
#define R_9346CR  0x50
#define R_CONFIG1 0x52
#define R_MSR     0x58
#define R_BMCR    0x62
#define R_ANAR    0x66
#define R_ANLPAR  0x68

#define CR_BUFE 0x01
#define CR_TE   0x04
#define CR_RE   0x08
#define CR_RST  0x10

#define TSD_SIZE 0x1FFF
#define TSD_OWN  (1u << 13)
#define TSD_TUN  (1u << 14)
#define TSD_TOK  (1u << 15)
#define TSD_TABT (1u << 30)

#define RCR_APM  (1u << 1)
#define RCR_AM   (1u << 2)
#define RCR_AB   (1u << 3)
#define RCR_WRAP (1u << 7)
#define TCR_CLRABT 1u

#define RX_ROK   0x0001

#define MSR_LINKB  0x04
#define MSR_SPEED10 0x08

#define INTR_MASK 0xE07F       /* ROK RER TOK TER RXOVW LinkChg FOVW LenChg TimeOut SERR */

#define RING   32768           /* RCR.RBLEN = 10: 32 KB + 16 */
#define RXALLOC (RING + 16 + 2048)
#define NTX    4
#define TXBUF  2048

typedef struct rtl {
    netdev_t nd;
    pci_dev_t *pci;
    volatile u8 *mmio;
    u16 io;
    u8 *rx;
    u8 *tx;
    u32 rx_off;
    u32 tx_next, tx_clean, tx_busy;
    u32 tx_len[NTX];
    u32 tcr, rcr;
    u8 mar[8];
    irq_handle_t irq;
    char irq_name[24];
} rtl_t;

static u8  r8(rtl_t *r, u32 o)  { return r->mmio ? mmio_read8(r->mmio + o) : inb((u16)(r->io + o)); }
static u16 r16(rtl_t *r, u32 o) { return r->mmio ? mmio_read16(r->mmio + o) : inw((u16)(r->io + o)); }
static u32 r32(rtl_t *r, u32 o) { return r->mmio ? mmio_read32(r->mmio + o) : inl((u16)(r->io + o)); }
static void w8(rtl_t *r, u32 o, u8 v)   { if (r->mmio) mmio_write8(r->mmio + o, v); else outb((u16)(r->io + o), v); }
static void w16(rtl_t *r, u32 o, u16 v) { if (r->mmio) mmio_write16(r->mmio + o, v); else outw((u16)(r->io + o), v); }
static void w32(rtl_t *r, u32 o, u32 v) { if (r->mmio) mmio_write32(r->mmio + o, v); else outl((u16)(r->io + o), v); }

static int rtl_ack(void *ctx) {
    rtl_t *r = ctx;
    u16 isr = r16(r, R_ISR);
    if (isr == 0xFFFF) return 0;                 /* gone */
    if (isr) w16(r, R_ISR, isr);                 /* write 1 to clear */
    return isr != 0;
}

static void rx_start(rtl_t *r) {
    w32(r, R_RBSTART, (u32)virt_to_phys(r->rx));
    w8(r, R_CR, CR_RE | CR_TE);
    w32(r, R_RCR, r->rcr);
    r->rx_off = 0;
    w16(r, R_CAPR, 0xFFF0);                      /* read pointer - 16, as the chip counts */
}

/* the ring is out of step: restart the receiver */
static void rx_reset(rtl_t *r) {
    w8(r, R_CR, CR_TE);
    for (int i = 0; i < 100 && (r8(r, R_CR) & CR_RE); i++) udelay(10);
    rx_start(r);
}

static void tx_reap(rtl_t *r) {
    while (r->tx_busy) {
        u32 tsd = r32(r, R_TSD0 + 4 * r->tx_clean);
        if (!(tsd & (TSD_TOK | TSD_TUN | TSD_TABT))) break;
        if (tsd & TSD_TOK) { r->nd.tx_packets++; r->nd.tx_bytes += r->tx_len[r->tx_clean]; }
        else r->nd.tx_errors++;
        if (tsd & TSD_TABT) w32(r, R_TCR, r->tcr | TCR_CLRABT);     /* aborted (collisions): clear the abort */
        r->tx_clean = (r->tx_clean + 1) % NTX;
        r->tx_busy--;
    }
}

static int rtl_xmit(netdev_t *nd, const void *frame, u32 len) {
    rtl_t *r = nd->priv;
    if (len < ETH_HDR || len > ETH_MAX_FRAME) return E_INVAL;
    if (r->tx_busy == NTX) tx_reap(r);
    if (r->tx_busy == NTX) return E_BUSY;
    u32 s = r->tx_next;
    u8 *buf = r->tx + s * TXBUF;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }
    r->tx_len[s] = len;
    wmb();
    /* size, early transmit threshold 48 x 32 bytes (the whole frame is in the FIFO first); OWN = 0 starts it */
    w32(r, R_TSD0 + 4 * s, len | (48u << 16));
    r->tx_next = (s + 1) % NTX;
    r->tx_busy++;
    return 0;
}

static void link_update(rtl_t *r) {
    u8 msr = r8(r, R_MSR);
    u16 bmcr = r16(r, R_BMCR);
    r->nd.link_up = !(msr & MSR_LINKB);
    r->nd.speed_mbps = (msr & MSR_SPEED10) ? 10 : 100;
    if (bmcr & 0x1000) {                         /* auto-negotiation: the best common mode */
        u16 common = r16(r, R_ANAR) & r16(r, R_ANLPAR);
        r->nd.full_duplex = r->nd.speed_mbps == 100 ? !!(common & 0x0100) : !!(common & 0x0040);
    } else {
        r->nd.full_duplex = !!(bmcr & 0x0100);
    }
}

static void rtl_poll(netdev_t *nd) {
    rtl_t *r = nd->priv;
    int work = 0;
    for (int budget = 0; budget < 64 && !(r8(r, R_CR) & CR_BUFE); budget++) {
        rmb();
        u8 *p = r->rx + r->rx_off;
        u32 status = (u32)p[0] | (u32)p[1] << 8, len = (u32)p[2] | (u32)p[3] << 8;
        if (len == 0xFFF0) break;                /* the chip is still copying this frame */
        work = 1;
        /* len counts the CRC */
        if (!(status & RX_ROK) || len < ETH_HDR + 4 || len > ETH_MAX_FRAME + 8) {
            nd->rx_errors++;
            rx_reset(r);
            break;
        }
        nd->rx_packets++;
        nd->rx_bytes += len - 4;
        netdev_rx(nd, p + 4, len - 4);          /* within RXALLOC: rx_off < RING, 4 + len <= 1530 */
        r->rx_off = ((r->rx_off + len + 4 + 3) & ~3u) % RING;
        w16(r, R_CAPR, (u16)(r->rx_off - 16));
    }
    u32 before = r->tx_busy;
    tx_reap(r);
    if (r->tx_busy != before) work = 1;
    u32 missed = r32(r, R_MPC) & 0xFFFFFF;
    if (missed) { nd->rx_dropped += missed; w32(r, R_MPC, 0); }
    if (work) irq_note_work(&r->irq);
    link_update(r);
    nd->irq_live = irq_live(&r->irq);
}

static void rtl_set_multicast(netdev_t *nd, const u8 (*macs)[6], int n) {
    rtl_t *r = nd->priv;
    memset(r->mar, 0, sizeof r->mar);
    for (int i = 0; i < n; i++) {
        u32 bit = nic_crc32_be(macs[i], 6) >> 26;
        r->mar[bit >> 3] |= (u8)(1u << (bit & 7));
    }
    for (int i = 0; i < 8; i++) w8(r, R_MAR0 + (u32)i, r->mar[i]);
}

static const char *chip_name(u32 tcr) {
    static const struct { u32 id; const char *name; } v[] = {
        { 0x60000000, "RTL8139" }, { 0x70000000, "RTL8139A" }, { 0x70800000, "RTL8139A-G" },
        { 0x78000000, "RTL8139B" }, { 0x7C000000, "RTL8130" }, { 0x74000000, "RTL8139C" },
        { 0x74400000, "RTL8100B/8139D" }, { 0x74800000, "RTL8139C+" }, { 0x78800000, "RTL8100" },
        { 0x74C00000, "RTL8101" },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(v); i++) if ((tcr & 0x7CC00000) == v[i].id) return v[i].name;
    return "RTL8139 (unknown version)";
}

static void rtl_attach(pci_dev_t *pd) {
    rtl_t *r = kzalloc(sizeof *r);
    if (!r) return;
    r->pci = pd;
    pci_enable(pd);
    if (!pd->bar[1].is_io && pd->bar[1].addr && pd->bar[1].size >= 0x80) r->mmio = pci_map_bar(pd, 1);
    if (!r->mmio) {
        if (!pd->bar[0].is_io || !pd->bar[0].addr) {
            kprintf("rtl8139: %02x:%02x.%x: no usable BAR\n", pd->bus, pd->dev, pd->func);
            kfree(r);
            return;
        }
        r->io = (u16)pd->bar[0].addr;
    }
    r->rx = dma_alloc(RXALLOC, 16);
    r->tx = dma_alloc(NTX * TXBUF, 16);
    if (!r->rx || !r->tx || virt_to_phys(r->rx) + RXALLOC > 0x100000000ull || virt_to_phys(r->tx) + NTX * TXBUF > 0x100000000ull) {
        kprintf("rtl8139: out of memory\n");
        dma_free(r->rx); dma_free(r->tx); kfree(r);
        return;
    }
    /* power on (CONFIG1 needs the configuration registers unlocked), then reset */
    w8(r, R_9346CR, 0xC0);
    w8(r, R_CONFIG1, 0x00);
    w8(r, R_9346CR, 0x00);
    w16(r, R_IMR, 0);
    w8(r, R_CR, CR_RST);
    for (int i = 0; i < 1000 && (r8(r, R_CR) & CR_RST); i++) udelay(10);
    if (r8(r, R_CR) & CR_RST) kprintf("rtl8139: %02x:%02x.%x: reset did not complete\n", pd->bus, pd->dev, pd->func);
    for (int i = 0; i < 6; i++) r->nd.mac[i] = r8(r, R_IDR0 + (u32)i);
    u32 tcr0 = r32(r, R_TCR);
    /* receive: broadcast, multicast (hash filter), our address; unlimited DMA bursts, no FIFO threshold */
    r->rcr = RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | (7u << 8) | (2u << 11) | (7u << 13);
    /* transmit: standard inter-frame gap, 2 KB DMA bursts */
    r->tcr = (3u << 24) | (7u << 8);
    for (int i = 0; i < 8; i++) w8(r, R_MAR0 + (u32)i, 0);
    for (int i = 0; i < NTX; i++) w32(r, R_TSAD0 + 4u * (u32)i, (u32)virt_to_phys(r->tx + i * TXBUF));
    w32(r, R_RCR, r->rcr);
    w32(r, R_TCR, r->tcr);
    rx_start(r);
    w32(r, R_TCR, r->tcr);                       /* again with the transmitter enabled (some revisions want it so) */
    w32(r, R_MPC, 0);
    w16(r, R_ISR, 0xFFFF);
    snprintf(r->irq_name, sizeof r->irq_name, "rtl8139 %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    r->irq.name = r->irq_name;
    r->irq.pci = pd;
    r->irq.ack = rtl_ack;
    r->irq.ctx = r;
    if (irq_attach(&r->irq) != IRQ_NONE) w16(r, R_IMR, INTR_MASK);
    link_update(r);
    r->nd.driver = "rtl8139";
    snprintf(r->nd.desc, sizeof r->nd.desc, "Realtek %s Fast Ethernet", chip_name(tcr0));
    r->nd.xmit = rtl_xmit;
    r->nd.poll = rtl_poll;
    r->nd.set_multicast = rtl_set_multicast;
    r->nd.priv = r;
    r->nd.pci = pd;
    r->nd.irq = &r->irq;
    pd->driver = "rtl8139";
    if (netdev_register(&r->nd)) kprintf("rtl8139: too many network adapters\n");
}

static const struct { u16 vendor, device; } ids[] = {
    { 0x10EC, 0x8139 }, { 0x10EC, 0x8138 }, { 0x1113, 0x1211 }, { 0x1186, 0x1300 }, { 0x1186, 0x1340 },
    { 0x13D1, 0xAB06 }, { 0x1259, 0xA117 }, { 0x1259, 0xA11E }, { 0x14EA, 0xAB06 }, { 0x14EA, 0xAB07 },
    { 0x11DB, 0x1234 }, { 0x1432, 0x9130 }, { 0x02AC, 0x1012 }, { 0x018A, 0x0106 }, { 0x126C, 0x1211 },
    { 0x1743, 0x8139 }, { 0x021B, 0x8139 }, { 0x16EC, 0xAB06 },
};

void rtl8139_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->driver) continue;
        for (unsigned k = 0; k < ARRAY_SIZE(ids); k++)
            if (pd->vendor == ids[k].vendor && pd->device == ids[k].device) { rtl_attach(pd); break; }
    }
}
