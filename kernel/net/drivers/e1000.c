/* Halcyon OS - Intel 8254x gigabit Ethernet ("e1000"): 82540EM (QEMU's
 * default -device e1000), 82545EM and relatives with the classic register
 * set and legacy descriptors.
 *
 * Reference: Intel PCI/PCI-X Family of Gigabit Ethernet Controllers Software
 * Developer's Manual (8254x), sections 13 (registers) and 3 (descriptors).
 *
 * Polled design (DESIGN-M2): the interrupt only wakes the net thread; poll()
 * reads ICR (which acknowledges it), harvests the receive ring, reaps the
 * transmit ring and reads the link state from STATUS. */
#include "net/netdev.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define CTRL_RST   (1u << 26)
#define STATUS_FD  (1u << 0)
#define STATUS_LU  (1u << 1)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define ICR_TXDW   (1u << 0)
#define ICR_LSC    (1u << 2)
#define ICR_RXDMT0 (1u << 4)
#define ICR_RXO    (1u << 6)
#define ICR_RXT0   (1u << 7)

#define NRX 64
#define NTX 64
#define BUFSZ 2048

typedef struct rxd { u64 addr; u16 len, csum; u8 status, errors; u16 special; } __attribute__((packed)) rxd_t;
typedef struct txd { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) txd_t;

#define RXS_DD  0x01
#define RXS_EOP 0x02
#define TXC_EOP 0x01
#define TXC_IFCS 0x02
#define TXC_RS  0x08
#define TXS_DD  0x01

typedef struct e1000 {
    netdev_t nd;
    pci_dev_t *pci;
    volatile u8 *regs;
    rxd_t *rx;
    txd_t *tx;
    u8 *rxbuf, *txbuf;
    u32 rx_next;              /* next descriptor the device will fill */
    u32 tx_next, tx_clean;    /* next free / oldest in flight */
    irq_handle_t irq;
    char irq_name[24];
} e1000_t;

static u32 rd(e1000_t *e, u32 r) { return mmio_read32(e->regs + r); }
static void wr(e1000_t *e, u32 r, u32 v) { mmio_write32(e->regs + r, v); }

static int e1000_ack(void *ctx) {
    e1000_t *e = ctx;
    return rd(e, REG_ICR) != 0;              /* reading ICR clears the causes */
}

static void link_update(e1000_t *e) {
    u32 st = rd(e, REG_STATUS);
    e->nd.link_up = !!(st & STATUS_LU);
    e->nd.full_duplex = !!(st & STATUS_FD);
    u32 sp = (st >> 6) & 3;
    e->nd.speed_mbps = sp == 0 ? 10 : sp == 1 ? 100 : 1000;
}

static int e1000_xmit(netdev_t *nd, const void *frame, u32 len) {
    e1000_t *e = nd->priv;
    if (len > ETH_MAX_FRAME) return E_INVAL;
    u32 n = (e->tx_next + 1) % NTX;
    if (n == e->tx_clean) return E_BUSY;     /* ring full */
    txd_t *d = &e->tx[e->tx_next];
    u8 *buf = e->txbuf + e->tx_next * BUFSZ;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }
    d->addr = (u64)(uptr)buf;
    d->len = (u16)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = TXC_EOP | TXC_IFCS | TXC_RS;
    e->tx_next = n;
    __asm__ volatile("sfence" ::: "memory");
    wr(e, REG_TDT, e->tx_next);
    nd->tx_packets++;
    nd->tx_bytes += len;
    return 0;
}

static void e1000_poll(netdev_t *nd) {
    e1000_t *e = nd->priv;
    u32 icr = rd(e, REG_ICR);
    if (icr & ICR_RXO) nd->rx_errors++;
    int work = 0;
    /* receive: every descriptor the device has finished */
    for (int budget = 0; budget < NRX; budget++) {
        rxd_t *d = &e->rx[e->rx_next];
        if (!(d->status & RXS_DD)) break;
        work = 1;
        if ((d->status & RXS_EOP) && !d->errors && d->len >= ETH_HDR) {
            nd->rx_packets++;
            nd->rx_bytes += d->len;
            netdev_rx(nd, e->rxbuf + e->rx_next * BUFSZ, d->len);
        } else {
            nd->rx_errors++;                 /* errors, or a frame larger than one buffer */
        }
        d->status = 0;
        u32 done = e->rx_next;
        e->rx_next = (e->rx_next + 1) % NRX;
        wr(e, REG_RDT, done);                /* give the descriptor back */
    }
    /* transmit: reclaim finished descriptors */
    while (e->tx_clean != e->tx_next && (e->tx[e->tx_clean].status & TXS_DD)) {
        e->tx_clean = (e->tx_clean + 1) % NTX;
        work = 1;
    }
    if (work) irq_note_work(&e->irq);
    if ((icr & ICR_LSC) || !nd->link_up) link_update(e);
    nd->irq_live = irq_live(&e->irq);
}

static u16 eeprom_read(e1000_t *e, u8 addr) {
    wr(e, REG_EERD, 1u | ((u32)addr << 8));
    for (int i = 0; i < 10000; i++) {
        u32 v = rd(e, REG_EERD);
        if (v & (1u << 4)) return (u16)(v >> 16);
        udelay(5);
    }
    return 0xFFFF;
}

static void e1000_attach(pci_dev_t *pd, const char *model) {
    e1000_t *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pci = pd;
    pci_enable(pd);
    e->regs = pci_map_bar(pd, 0);
    if (!e->regs) { kprintf("e1000: %02x:%02x.%x: cannot map BAR0\n", pd->bus, pd->dev, pd->func); kfree(e); return; }
    /* reset, then link up with auto speed detection */
    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    mdelay(2);
    for (int i = 0; i < 1000 && (rd(e, REG_CTRL) & CTRL_RST); i++) udelay(10);
    wr(e, REG_IMC, 0xFFFFFFFF);
    (void)rd(e, REG_ICR);
    wr(e, REG_CTRL, (rd(e, REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~(1u << 3) & ~(1u << 31) & ~(1u << 7));
    /* MAC: the receive address register the firmware / EEPROM load set, else the EEPROM */
    u32 ral = rd(e, REG_RAL0), rah = rd(e, REG_RAH0);
    if ((rah & (1u << 31)) && (ral || (rah & 0xFFFF))) {
        for (int i = 0; i < 4; i++) e->nd.mac[i] = (u8)(ral >> (8 * i));
        e->nd.mac[4] = (u8)rah; e->nd.mac[5] = (u8)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            u16 w = eeprom_read(e, (u8)i);
            e->nd.mac[2 * i] = (u8)w; e->nd.mac[2 * i + 1] = (u8)(w >> 8);
        }
        wr(e, REG_RAL0, (u32)e->nd.mac[0] | (u32)e->nd.mac[1] << 8 | (u32)e->nd.mac[2] << 16 | (u32)e->nd.mac[3] << 24);
        wr(e, REG_RAH0, (u32)e->nd.mac[4] | (u32)e->nd.mac[5] << 8 | (1u << 31));
    }
    for (int i = 0; i < 128; i++) wr(e, REG_MTA + i * 4, 0);
    /* rings (the kernel heap is identity mapped: virtual = physical) */
    e->rx = kmalloc_aligned(sizeof(rxd_t) * NRX, 128);
    e->tx = kmalloc_aligned(sizeof(txd_t) * NTX, 128);
    e->rxbuf = kmalloc_aligned(NRX * BUFSZ, 4096);
    e->txbuf = kmalloc_aligned(NTX * BUFSZ, 4096);
    if (!e->rx || !e->tx || !e->rxbuf || !e->txbuf) {
        kprintf("e1000: out of memory\n");
        kfree(e->rx); kfree(e->tx); kfree(e->rxbuf); kfree(e->txbuf); kfree(e);
        return;
    }
    memset(e->rx, 0, sizeof(rxd_t) * NRX);
    memset(e->tx, 0, sizeof(txd_t) * NTX);
    for (int i = 0; i < NRX; i++) e->rx[i].addr = (u64)(uptr)(e->rxbuf + i * BUFSZ);
    wr(e, REG_RDBAL, (u32)(uptr)e->rx);
    wr(e, REG_RDBAH, (u32)((u64)(uptr)e->rx >> 32));
    wr(e, REG_RDLEN, sizeof(rxd_t) * NRX);
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, NRX - 1);
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);         /* 2048-byte buffers (BSIZE 00) */
    wr(e, REG_TDBAL, (u32)(uptr)e->tx);
    wr(e, REG_TDBAH, (u32)((u64)(uptr)e->tx >> 32));
    wr(e, REG_TDLEN, sizeof(txd_t) * NTX);
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    wr(e, REG_TIPG, 10u | (8u << 10) | (6u << 20));
    /* interrupts: wake-ups only */
    snprintf(e->irq_name, sizeof e->irq_name, "e1000 %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    e->irq.name = e->irq_name;
    e->irq.pci = pd;
    e->irq.ack = e1000_ack;
    e->irq.ctx = e;
    irq_attach(&e->irq);
    wr(e, REG_IMS, ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0);
    link_update(e);
    e->nd.driver = "e1000";
    snprintf(e->nd.desc, sizeof e->nd.desc, "Intel %s Gigabit Ethernet", model);
    e->nd.xmit = e1000_xmit;
    e->nd.poll = e1000_poll;
    e->nd.priv = e;
    pd->driver = "e1000";
    if (netdev_register(&e->nd)) kprintf("e1000: too many network adapters\n");
}

static const struct { u16 id; const char *model; } models[] = {
    { 0x100E, "82540EM" }, { 0x1015, "82540EM (LOM)" }, { 0x1016, "82540EP (LOM)" }, { 0x1017, "82540EP" },
    { 0x100F, "82545EM" }, { 0x1011, "82545EM (fiber)" }, { 0x1026, "82545GM" }, { 0x1027, "82545GM (fiber)" },
    { 0x1028, "82545GM (SerDes)" }, { 0x1010, "82546EB" }, { 0x1079, "82546GB" }, { 0x1076, "82541GI" },
    { 0x1019, "82547EI" }, { 0x101A, "82547EI (mobile)" },
};

void e1000_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->vendor != 0x8086 || pd->driver) continue;
        for (unsigned m = 0; m < ARRAY_SIZE(models); m++)
            if (pd->device == models[m].id) { e1000_attach(pd, models[m].model); break; }
    }
}
