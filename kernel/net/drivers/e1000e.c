/* Halcyon OS - Intel PCI Express gigabit Ethernet with the 8254x-style
 * register set ("e1000e"): the 82571/82572 server adapters, the 82573,
 * 82574 (QEMU -device e1000e) and 82583 client/embedded chips, and the
 * PCH-attached I217, I218 and I219 ("Lynx Point" to "Raptor Lake"
 * chipsets: the on-board Ethernet of most Intel desktops and laptops from
 * 2013 on).
 *
 * References: Intel 82574 GbE Controller Family Datasheet; 82571/82572 and
 * 82573 Family Software Developer's Manuals; 82583V datasheet; Intel
 * Ethernet Connection I217 / I218 / I219 datasheets (PHY registers and the
 * MAC/PHY interconnect); and the register descriptions in Intel's
 * BSD-licensed shared code (FreeBSD sys/dev/e1000) for the chipset-side
 * MAC registers that the PCH datasheets do not describe publicly. The same
 * legacy descriptors as e1000.c are used: 16-byte receive descriptors with
 * a 2 KB buffer each, 16-byte transmit descriptors.
 *
 * Tested in QEMU: the 82574L (MSI-X, INTx through the I/O APIC and the PIC,
 * polled). Written to the documentation only, untested:
 *   - 82571/82572/82573/82583: the reset sequence, the software/firmware
 *     semaphores (SWSM, EXTCNF_CTRL.MDIO_SW_OWNERSHIP), the "must be set"
 *     register bits from the specification updates (TARC0/1, GCR, CTRL_EXT).
 *     Copper ports only (fiber and SerDes need their own link setup).
 *   - I217/I218/I219: the PHY is reached over MDIC at PHY address 2 (pages
 *     768 and up at address 1, page select in register 31) under the
 *     EXTCNF_CTRL.SWFLAG semaphore shared with the Management Engine; at
 *     start the PHY is woken from Ultra Low Power (ULP) and the MAC/PHY
 *     interconnect taken out of SMBus mode: through the ME (H2ME, waiting for
 *     FWSM.ULP_CFG_DONE) when ME firmware is present, else by toggling
 *     LANPHYPC and clearing the ULP and SMBus settings in the PHY. A PHY
 *     reset blocked by the ME (FWSM.RSPCIPHY clear) is respected.
 *     Not done: EEE/LPI and K1 power settings (the PHY stays out of K1 after
 *     a ULP exit: a little more power, no loss of function), the I219
 *     descriptor ring flush that a reset after another driver used the rings
 *     can need (logged when the chip asks for it), Wake-on-LAN, and the
 *     82577/82578/82579 (older PCH PHYs with many workarounds) and ICH8-10
 *     chips. Meteor Lake and newer I219s (they need a K1 exit timeout
 *     change) are not claimed. The MAC address must be in RAL0/RAH0, where
 *     the hardware loads it from the NVM at reset (the NVM itself, in the
 *     platform flash, is not read).
 *
 * Interrupts: MSI-X (82574: every cause mapped to vector 0 through IVAR),
 * MSI or INTx; only wake-ups, poll() reads ICR (and clears it: in MSI-X mode
 * reading does not) and works from the descriptor rings. */
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
#define REG_STRAP     0x000C
#define REG_EECD      0x0010
#define REG_EERD      0x0014
#define REG_CTRL_EXT  0x0018
#define REG_MDIC      0x0020
#define REG_FEXTNVM3  0x003C
#define REG_ICR       0x00C0
#define REG_IMS       0x00D0
#define REG_IMC       0x00D8
#define REG_EIAC_82574 0x00DC
#define REG_IVAR      0x00E4
#define REG_RCTL      0x0100
#define REG_TCTL      0x0400
#define REG_TIPG      0x0410
#define REG_EXTCNF_CTRL 0x0F00
#define REG_KABGTXD   0x3004
#define REG_RDBAL     0x2800
#define REG_RDBAH     0x2804
#define REG_RDLEN     0x2808
#define REG_RDH       0x2810
#define REG_RDT       0x2818
#define REG_RXDCTL    0x2828
#define REG_TDBAL     0x3800
#define REG_TDBAH     0x3804
#define REG_TDLEN     0x3808
#define REG_TDH       0x3810
#define REG_TDT       0x3818
#define REG_TXDCTL0   0x3828
#define REG_TARC0     0x3840
#define REG_TXDCTL1   0x3928
#define REG_TARC1     0x3940
#define REG_MPC       0x4010
#define REG_RFCTL     0x5008
#define REG_MTA       0x5200
#define REG_RAL0      0x5400
#define REG_RAH0      0x5404
#define REG_GCR       0x5B00
#define REG_SWSM      0x5B50
#define REG_H2ME      0x5B50     /* PCH: host to ME (same offset as SWSM) */
#define REG_FWSM      0x5B54
#define REG_FFLT_DBG  0x5F04
#define REG_PBECCSTS  0x100C

#define CTRL_FD       (1u << 0)
#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_LRST     (1u << 3)
#define CTRL_ASDE     (1u << 5)
#define CTRL_SLU      (1u << 6)
#define CTRL_ILOS     (1u << 7)
#define CTRL_FRCSPD   (1u << 11)
#define CTRL_FRCDPX   (1u << 12)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRL_MEHE     (1u << 19)
#define CTRL_RST      (1u << 26)
#define CTRL_PHY_RST  (1u << 31)
#define STATUS_FD     (1u << 0)
#define STATUS_LU     (1u << 1)
#define STATUS_FUNC1  (1u << 2)
#define STATUS_LAN_INIT_DONE (1u << 9)
#define STATUS_GIO_MASTER_EN (1u << 19)
#define EECD_REQ      (1u << 6)
#define EECD_GNT      (1u << 7)
#define EECD_AUTO_RD  (1u << 9)
#define CTRL_EXT_LPCD       (1u << 2)
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define CTRL_EXT_RO_DIS     (1u << 17)
#define CTRL_EXT_DMA_DYN_CLK (1u << 19)
#define CTRL_EXT_PHYPDEN    (1u << 20)
#define CTRL_EXT_DRV_LOAD   (1u << 28)
#define CTRL_EXT_PBA_CLR    (1u << 31)
#define EXTCNF_SWFLAG (1u << 5)     /* PCH: SWFLAG; 82573/4/83: MDIO_SW_OWNERSHIP */
#define SWSM_SMBI     (1u << 0)
#define SWSM_SWESMBI  (1u << 1)
#define FWSM_RSPCIPHY (1u << 6)
#define FWSM_ULP_CFG_DONE (1u << 10)
#define FWSM_FW_VALID (1u << 15)
#define H2ME_ULP      (1u << 11)
#define H2ME_ENFORCE  (1u << 12)
#define MDIC_WRITE    (1u << 26)
#define MDIC_READ     (2u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)
#define RCTL_EN       (1u << 1)
#define RCTL_MPE      (1u << 4)
#define RCTL_BAM      (1u << 15)
#define RCTL_SECRC    (1u << 26)
#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)
#define TCTL_MULR     (1u << 28)
#define ICR_TXDW      (1u << 0)
#define ICR_LSC       (1u << 2)
#define ICR_RXDMT0    (1u << 4)
#define ICR_RXO       (1u << 6)
#define ICR_RXT0      (1u << 7)
#define ICR_RXQ0      (1u << 20)
#define ICR_TXQ0      (1u << 22)
#define ICR_OTHER     (1u << 24)

/* PHY registers (page << 5 | register for the PCH PHYs) */
#define PHY_BMCR      0
#define PHY_ID1       2
#define PHY_ID2       3
#define BMCR_ANRESTART (1u << 9)
#define BMCR_ISOLATE  (1u << 10)
#define BMCR_PDOWN    (1u << 11)
#define BMCR_ANENABLE (1u << 12)
#define PHYREG(page, reg) (((page) << 5) | ((reg) & 31))
#define CV_SMB_CTRL      PHYREG(769, 23)
#define CV_SMB_FORCE     0x0001
#define HV_PM_CTRL       PHYREG(770, 17)
#define HV_PM_K1_ENABLE  0x4000
#define ULP_CONFIG1      PHYREG(779, 16)
#define ULP_START        0x0001
#define ULP_CLEAR        (0x0004 | 0x0010 | 0x0020 | 0x0040 | 0x0100 | 0x0400 | 0x0800 | 0x1000)

#define NRX 128
#define NTX 128
#define BUFSZ 2048

enum { MT_82571, MT_82572, MT_82573, MT_82574, MT_82583, MT_LPT, MT_SPT, MT_CNP, MT_TGP, MT_ADP };
#define IS_PCH(t) ((t) >= MT_LPT)

typedef struct rxd { u64 addr; u16 len, csum; u8 status, errors; u16 special; } __attribute__((packed)) rxd_t;
typedef struct txd { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) txd_t;
#define RXS_DD  0x01
#define RXS_EOP 0x02
#define TXC_EOP 0x01
#define TXC_IFCS 0x02
#define TXC_RS  0x08
#define TXS_DD  0x01

typedef struct e1e {
    netdev_t nd;
    pci_dev_t *pci;
    volatile u8 *regs;
    int type;
    u8 ulp;                     /* this PCH PHY has Ultra Low Power mode */
    volatile rxd_t *rx;
    volatile txd_t *tx;
    u8 *rxbuf, *txbuf;
    u32 rx_next, tx_next, tx_clean;
    u32 ims;
    int msix;
    int sema_warned;
    irq_handle_t irq;
    char irq_name[24];
} e1e_t;

static u32 rd(e1e_t *e, u32 r) { return mmio_read32(e->regs + r); }
static void wr(e1e_t *e, u32 r, u32 v) { mmio_write32(e->regs + r, v); }
static void flush(e1e_t *e) { (void)rd(e, REG_STATUS); }

/* ---------------- semaphores shared with firmware ---------------- */
static int sema_get(e1e_t *e) {
    if (e->type >= MT_82573) {
        /* PCH: wait until nobody holds SWFLAG; then claim it and see it stick */
        for (int i = 0; IS_PCH(e->type) && i < 100 && (rd(e, REG_EXTCNF_CTRL) & EXTCNF_SWFLAG); i++) mdelay(1);
        for (int i = 0; i < (IS_PCH(e->type) ? 1000 : 100); i++) {
            wr(e, REG_EXTCNF_CTRL, rd(e, REG_EXTCNF_CTRL) | EXTCNF_SWFLAG);
            if (rd(e, REG_EXTCNF_CTRL) & EXTCNF_SWFLAG) return 0;
            mdelay(1);
        }
        wr(e, REG_EXTCNF_CTRL, rd(e, REG_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
    } else {
        /* 82571/82572: SMBI (set by the read that finds it clear), then SWESMBI */
        int ok = 0;
        for (int i = 0; i < 2000; i++) { if (!(rd(e, REG_SWSM) & SWSM_SMBI)) { ok = 1; break; } udelay(50); }
        for (int i = 0; ok && i < 2000; i++) {
            wr(e, REG_SWSM, rd(e, REG_SWSM) | SWSM_SWESMBI);
            if (rd(e, REG_SWSM) & SWSM_SWESMBI) return 0;
            udelay(50);
        }
        wr(e, REG_SWSM, rd(e, REG_SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI));
    }
    if (!e->sema_warned++) kprintf("e1000e: the firmware holds the PHY semaphore, going ahead without it\n");
    return -1;
}

static void sema_put(e1e_t *e) {
    if (e->type >= MT_82573) wr(e, REG_EXTCNF_CTRL, rd(e, REG_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
    else wr(e, REG_SWSM, rd(e, REG_SWSM) & ~(SWSM_SMBI | SWSM_SWESMBI));
}

/* ---------------- PHY (MDIC); the caller holds the semaphore ---------------- */
static int mdic(e1e_t *e, u32 phy, u32 reg, u32 op, u16 data, u16 *out) {
    wr(e, REG_MDIC, (u32)data | (reg & 31) << 16 | (phy & 31) << 21 | op);
    for (int i = 0; i < 2000; i++) {
        udelay(5);
        u32 v = rd(e, REG_MDIC);
        if (v & MDIC_READY) {
            if (IS_PCH(e->type)) udelay(100);        /* let the PHY catch up (PCH) */
            if (v & MDIC_ERROR) return E_IO;
            if (out) *out = (u16)v;
            return 0;
        }
    }
    return E_IO;
}

/* reg = PHYREG(page, register); 8257x PHYs: page 0 only, at address 1 */
static int phy_access(e1e_t *e, u32 reg, int write, u16 *val) {
    u32 page = reg >> 5, r = reg & 31;
    if (!IS_PCH(e->type)) return mdic(e, 1, r, write ? MDIC_WRITE : MDIC_READ, write ? *val : 0, val);
    u32 addr = page >= 768 ? 1 : 2;              /* the PCH PHYs answer pages 768+ at address 1 */
    if (page == 768) page = 0;
    if (r > 15 && mdic(e, 1, 31, MDIC_WRITE, (u16)(page << 5), NULL)) return E_IO;
    return mdic(e, addr, r, write ? MDIC_WRITE : MDIC_READ, write ? *val : 0, val);
}
static int phy_read(e1e_t *e, u32 reg, u16 *v) { return phy_access(e, reg, 0, v); }
static int phy_write(e1e_t *e, u32 reg, u16 v) { return phy_access(e, reg, 1, &v); }

static int phy_accessible(e1e_t *e) {
    u16 a = 0xFFFF, b = 0xFFFF;
    for (int i = 0; i < 2; i++)
        if (!phy_read(e, PHY_ID1, &a) && a != 0xFFFF && !phy_read(e, PHY_ID2, &b) && b != 0xFFFF) return 1;
    return 0;
}

/* ---------------- PCH: wake the PHY (ULP, SMBus mode) ---------------- */
static void lanphypc_toggle(e1e_t *e) {
    u32 v = rd(e, REG_FEXTNVM3);
    wr(e, REG_FEXTNVM3, (v & ~0x0C000000u) | 0x08000000u);     /* PHY config counter: 50 ms */
    v = rd(e, REG_CTRL);
    wr(e, REG_CTRL, (v | CTRL_LANPHYPC_OVERRIDE) & ~CTRL_LANPHYPC_VALUE);
    flush(e);
    mdelay(1);
    wr(e, REG_CTRL, v & ~(CTRL_LANPHYPC_OVERRIDE | CTRL_LANPHYPC_VALUE));
    flush(e);
    for (int i = 0; i < 20 && !(rd(e, REG_CTRL_EXT) & CTRL_EXT_LPCD); i++) mdelay(5);
    mdelay(30);
}

static void force_smbus(e1e_t *e, int on) {
    u32 v = rd(e, REG_CTRL_EXT);
    wr(e, REG_CTRL_EXT, on ? v | CTRL_EXT_FORCE_SMBUS : v & ~CTRL_EXT_FORCE_SMBUS);
}

static const char *pch_phy_wake(e1e_t *e) {
    int me = !!(rd(e, REG_FWSM) & FWSM_FW_VALID);
    int may_reset = !!(rd(e, REG_FWSM) & FWSM_RSPCIPHY);
    /* ULP exit through the ME: ask it to undo ULP and wait until it has */
    if (e->ulp && me) {
        wr(e, REG_H2ME, (rd(e, REG_H2ME) & ~H2ME_ULP) | H2ME_ENFORCE);
        int i = 0;
        while ((rd(e, REG_FWSM) & FWSM_ULP_CFG_DONE) && i++ < 250) mdelay(10);
        wr(e, REG_H2ME, rd(e, REG_H2ME) & ~H2ME_ENFORCE);
        if (i > 250) kprintf("e1000e: the ME did not leave ULP mode\n");
    }
    int held = !sema_get(e);
    const char *how = NULL;
    if (phy_accessible(e)) how = "accessible";
    else {
        force_smbus(e, 1);                       /* maybe the interconnect is in SMBus mode */
        mdelay(50);
        if (phy_accessible(e)) how = "accessible in SMBus mode";
        else if (!may_reset) how = NULL;
        else {
            lanphypc_toggle(e);                  /* full PHY power cycle */
            if (phy_accessible(e)) how = "woken by LANPHYPC";
            else { force_smbus(e, 0); if (phy_accessible(e)) how = "woken by LANPHYPC"; }
        }
    }
    if (how && !me) {
        /* no ME: take the interconnect out of SMBus mode and clear ULP ourselves */
        u16 v;
        if (!phy_read(e, CV_SMB_CTRL, &v)) phy_write(e, CV_SMB_CTRL, v & ~CV_SMB_FORCE);
        force_smbus(e, 0);
        if (e->ulp) {
            if (!phy_read(e, HV_PM_CTRL, &v)) phy_write(e, HV_PM_CTRL, v | HV_PM_K1_ENABLE);
            if (!phy_read(e, ULP_CONFIG1, &v)) {
                v &= ~ULP_CLEAR;
                phy_write(e, ULP_CONFIG1, v);
                phy_write(e, ULP_CONFIG1, v | ULP_START);
            }
        }
    }
    if (held) sema_put(e);
    if (!how) return may_reset ? "PHY not accessible" : "PHY not accessible (reset blocked by the ME)";
    return NULL;
}

/* ---------------- reset and setup ---------------- */
static void wait_master_disable(e1e_t *e) {
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_GIO_MASTER_DISABLE);
    for (int i = 0; i < 800 && (rd(e, REG_STATUS) & STATUS_GIO_MASTER_EN); i++) udelay(100);
}

static void hw_reset(e1e_t *e) {
    wait_master_disable(e);
    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_RCTL, 0);
    wr(e, REG_TCTL, TCTL_PSP);
    flush(e);
    mdelay(10);
    u32 ctrl = rd(e, REG_CTRL);
    if (IS_PCH(e->type)) {
        if (e->type >= MT_SPT && (pci_read32(e->pci, 0xE4) & 0x100))
            kprintf("e1000e: the chip asks for a descriptor ring flush before reset (not done)\n");
        if (rd(e, REG_FWSM) & FWSM_RSPCIPHY) ctrl |= CTRL_PHY_RST;     /* MAC and PHY together, unless the ME objects */
        int held = !sema_get(e);
        (void)pci_read16(e->pci, 0);             /* a configuration cycle before the reset (PCH errata) */
        wr(e, REG_CTRL, ctrl | CTRL_RST);
        mdelay(20);                              /* no register access meanwhile */
        (void)pci_read16(e->pci, 0);
        if (ctrl & CTRL_PHY_RST) {
            for (int i = 0; i < 1500 && !(rd(e, REG_STATUS) & STATUS_LAN_INIT_DONE); i++) udelay(100);
            wr(e, REG_STATUS, rd(e, REG_STATUS) & ~STATUS_LAN_INIT_DONE);
            mdelay(10);
        }
        if (held && (rd(e, REG_EXTCNF_CTRL) & EXTCNF_SWFLAG)) sema_put(e);
        wr(e, REG_KABGTXD, rd(e, REG_KABGTXD) | 0x00050000);
    } else {
        int held = e->type >= MT_82573 ? !sema_get(e) : 0;
        wr(e, REG_CTRL, ctrl | CTRL_RST);
        mdelay(2);
        for (int i = 0; i < 1000 && (rd(e, REG_CTRL) & CTRL_RST); i++) udelay(10);
        if (held) sema_put(e);
        for (int i = 0; i < 100 && !(rd(e, REG_EECD) & EECD_AUTO_RD); i++) udelay(100);
        if (e->type <= MT_82572) wr(e, REG_EECD, rd(e, REG_EECD) & ~(EECD_REQ | EECD_GNT));
        else mdelay(25);
    }
    wr(e, REG_IMC, 0xFFFFFFFF);
    (void)rd(e, REG_ICR);
}

/* register bits the specification updates require after a reset */
static void hw_bits(e1e_t *e) {
    u32 v;
    wr(e, REG_TXDCTL0, rd(e, REG_TXDCTL0) | (1u << 22));
    wr(e, REG_TXDCTL1, rd(e, REG_TXDCTL1) | (1u << 22));
    if (IS_PCH(e->type)) {
        wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) | (1u << 22) | CTRL_EXT_PHYPDEN);
        wr(e, REG_TARC0, rd(e, REG_TARC0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
        v = rd(e, REG_TARC1) | (1u << 24) | (1u << 26) | (1u << 30);
        v = (rd(e, REG_TCTL) & TCTL_MULR) ? v & ~(1u << 28) : v | (1u << 28);
        wr(e, REG_TARC1, v);
        wr(e, REG_RFCTL, rd(e, REG_RFCTL) | 0x40 | 0x80);           /* NFS write / read filtering off */
        wr(e, REG_PBECCSTS, rd(e, REG_PBECCSTS) | 0x00010000);      /* packet buffer ECC */
        wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_MEHE);
        if (e->type >= MT_TGP) wr(e, REG_FFLT_DBG, rd(e, REG_FFLT_DBG) | (1u << 12));   /* DMA clock ungated */
        wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) | CTRL_EXT_RO_DIS);
        return;
    }
    v = rd(e, REG_TARC0) & ~(0xFu << 27);
    if (e->type <= MT_82572) v |= (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
    else if (e->type != MT_82573) v |= 1u << 26;
    wr(e, REG_TARC0, v);
    if (e->type <= MT_82572) {
        v = (rd(e, REG_TARC1) & ~((1u << 29) | (1u << 30))) | (1u << 22) | (1u << 24) | (1u << 25) | (1u << 26);
        v = (rd(e, REG_TCTL) & TCTL_MULR) ? v & ~(1u << 28) : v | (1u << 28);
        wr(e, REG_TARC1, v);
        wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) & ~CTRL_EXT_DMA_DYN_CLK);
    } else {
        wr(e, REG_CTRL, rd(e, REG_CTRL) & ~(1u << 29));
        wr(e, REG_CTRL_EXT, (rd(e, REG_CTRL_EXT) & ~(1u << 23)) | (1u << 22));
    }
    if (e->type <= MT_82573) wr(e, REG_RFCTL, rd(e, REG_RFCTL) | 0x00010000 | 0x00020000);   /* IPv6 extension header parsing off */
    if (e->type == MT_82574 || e->type == MT_82583)
        wr(e, REG_GCR, (rd(e, REG_GCR) | (1u << 22)) & ~0x08000000u);  /* 82574 errata 25, 82583 errata 12 */
}

/* the PHY: powered up and auto-negotiating */
static void phy_setup(e1e_t *e) {
    int held = !sema_get(e);
    u16 bmcr;
    if (!phy_read(e, PHY_BMCR, &bmcr) && ((bmcr & (BMCR_PDOWN | BMCR_ISOLATE)) || !(bmcr & BMCR_ANENABLE)))
        phy_write(e, PHY_BMCR, (u16)((bmcr & ~(BMCR_PDOWN | BMCR_ISOLATE)) | BMCR_ANENABLE | BMCR_ANRESTART));
    if (held) sema_put(e);
}

static u16 eeprom_read(e1e_t *e, u32 addr) {
    wr(e, REG_EERD, 1u | addr << 2);
    for (int i = 0; i < 10000; i++) {
        u32 v = rd(e, REG_EERD);
        if (v & 2) return (u16)(v >> 16);
        udelay(5);
    }
    return 0xFFFF;
}

static int read_mac(e1e_t *e) {
    u32 ral = rd(e, REG_RAL0), rah = rd(e, REG_RAH0);
    if ((rah & (1u << 31)) && (ral || (rah & 0xFFFF)) && ral != 0xFFFFFFFF) {
        for (int i = 0; i < 4; i++) e->nd.mac[i] = (u8)(ral >> (8 * i));
        e->nd.mac[4] = (u8)rah; e->nd.mac[5] = (u8)(rah >> 8);
        return 0;
    }
    if (IS_PCH(e->type)) return E_IO;            /* the NVM is in the platform flash: not read */
    for (int i = 0; i < 3; i++) {
        u16 w = eeprom_read(e, (u32)i);
        e->nd.mac[2 * i] = (u8)w; e->nd.mac[2 * i + 1] = (u8)(w >> 8);
    }
    if (rd(e, REG_STATUS) & STATUS_FUNC1) e->nd.mac[5] ^= 1;   /* second port of a dual-port 82571 */
    if ((e->nd.mac[0] & 1) || !(e->nd.mac[0] | e->nd.mac[1] | e->nd.mac[2] | e->nd.mac[3] | e->nd.mac[4] | e->nd.mac[5])) return E_IO;
    wr(e, REG_RAL0, (u32)e->nd.mac[0] | (u32)e->nd.mac[1] << 8 | (u32)e->nd.mac[2] << 16 | (u32)e->nd.mac[3] << 24);
    wr(e, REG_RAH0, (u32)e->nd.mac[4] | (u32)e->nd.mac[5] << 8 | (1u << 31));
    return 0;
}

/* ---------------- run time ---------------- */
static int e1e_ack(void *ctx) {
    e1e_t *e = ctx;
    return rd(e, REG_ICR) != 0;                  /* INTx: reading ICR clears the causes */
}

static void link_update(e1e_t *e) {
    u32 st = rd(e, REG_STATUS);
    e->nd.link_up = !!(st & STATUS_LU);
    e->nd.full_duplex = !!(st & STATUS_FD);
    u32 sp = (st >> 6) & 3;
    e->nd.speed_mbps = sp == 0 ? 10 : sp == 1 ? 100 : 1000;
}

static int tx_reap(e1e_t *e) {
    int n = 0;
    while (e->tx_clean != e->tx_next && (e->tx[e->tx_clean].status & TXS_DD)) {
        volatile txd_t *t = &e->tx[e->tx_clean];
        if (t->status & 0x06) e->nd.tx_errors++;   /* excess / late collisions */
        else { e->nd.tx_packets++; e->nd.tx_bytes += t->len; }
        e->tx_clean = (e->tx_clean + 1) % NTX;
        n++;
    }
    return n;
}

static int e1e_xmit(netdev_t *nd, const void *frame, u32 len) {
    e1e_t *e = nd->priv;
    if (len < ETH_HDR || len > ETH_MAX_FRAME) return E_INVAL;
    u32 n = (e->tx_next + 1) % NTX;
    if (n == e->tx_clean) tx_reap(e);
    if (n == e->tx_clean) return E_BUSY;         /* ring full */
    volatile txd_t *d = &e->tx[e->tx_next];
    u8 *buf = e->txbuf + e->tx_next * BUFSZ;
    memcpy(buf, frame, len);
    if (len < 60) { memset(buf + len, 0, 60 - len); len = 60; }
    d->addr = virt_to_phys(buf);
    d->len = (u16)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = TXC_EOP | TXC_IFCS | TXC_RS;
    e->tx_next = n;
    wmb();
    wr(e, REG_TDT, e->tx_next);
    return 0;
}

static void e1e_poll(netdev_t *nd) {
    e1e_t *e = nd->priv;
    u32 icr = rd(e, REG_ICR);
    if (icr) wr(e, REG_ICR, icr);                /* MSI-X mode: ICR is write-1-to-clear only */
    int work = 0;
    u32 posted = NRX;
    for (int budget = 0; budget < NRX; budget++) {
        volatile rxd_t *d = &e->rx[e->rx_next];
        u8 status = d->status;
        if (!(status & RXS_DD)) break;
        rmb();
        work = 1;
        u32 len = d->len;
        if ((status & RXS_EOP) && !d->errors && len >= ETH_HDR && len <= ETH_MAX_FRAME + 4) {
            nd->rx_packets++;
            nd->rx_bytes += len;
            netdev_rx(nd, e->rxbuf + e->rx_next * BUFSZ, len);
        } else {
            nd->rx_errors++;                     /* errors, or a frame larger than one buffer */
        }
        d->status = 0;
        posted = e->rx_next;
        e->rx_next = (e->rx_next + 1) % NRX;
    }
    if (posted != NRX) {                         /* give the descriptors back, all at once */
        wmb();
        wr(e, REG_RDT, posted);
    }
    if (tx_reap(e)) work = 1;
    nd->rx_dropped += rd(e, REG_MPC);            /* missed: no free descriptor (clear on read) */
    if (work) irq_note_work(&e->irq);
    if ((icr & (ICR_LSC | ICR_OTHER)) || !nd->link_up) link_update(e);
    nd->irq_live = irq_live(&e->irq);
}

static void e1e_set_multicast(netdev_t *nd, const u8 (*macs)[6], int n) {
    e1e_t *e = nd->priv;
    u32 mta[128];
    memset(mta, 0, sizeof mta);
    for (int i = 0; i < n; i++) { u32 h = nic_intel_mta_hash(macs[i]); mta[h >> 5] |= 1u << (h & 31); }
    for (int i = 0; i < 128; i++) wr(e, REG_MTA + 4u * (u32)i, mta[i]);
}

static void e1e_attach(pci_dev_t *pd, int type, const char *model, int ulp) {
    e1e_t *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pci = pd;
    e->type = type;
    e->ulp = (u8)ulp;
    pci_enable(pd);
    e->regs = pci_map_bar(pd, 0);
    if (!e->regs || pd->bar[0].size < 0x10000) {
        kprintf("e1000e: %02x:%02x.%x: cannot map BAR0\n", pd->bus, pd->dev, pd->func);
        kfree(e);
        return;
    }
    const char *err = NULL;
    if (IS_PCH(type)) err = pch_phy_wake(e);
    hw_reset(e);
    if (IS_PCH(type) && !err) {
        int held = !sema_get(e), ok = phy_accessible(e);
        if (held) sema_put(e);
        if (!ok) err = pch_phy_wake(e);          /* the reset put it back to sleep */
    }
    if (err) kprintf("e1000e: %02x:%02x.%x: %s; the link may not come up\n", pd->bus, pd->dev, pd->func, err);
    hw_bits(e);
    wr(e, REG_CTRL, (rd(e, REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~(CTRL_LRST | CTRL_ILOS | CTRL_FRCSPD | CTRL_FRCDPX | CTRL_PHY_RST));
    wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);   /* tell the firmware a driver runs */
    phy_setup(e);
    if (read_mac(e)) {
        kprintf("e1000e: %02x:%02x.%x: no MAC address\n", pd->bus, pd->dev, pd->func);
        wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) & ~CTRL_EXT_DRV_LOAD);
        kfree(e);
        return;
    }
    for (int i = 0; i < 128; i++) wr(e, REG_MTA + i * 4, 0);
    e->rx = dma_alloc(sizeof(rxd_t) * NRX, 128);
    e->tx = dma_alloc(sizeof(txd_t) * NTX, 128);
    e->rxbuf = dma_alloc(NRX * BUFSZ, 4096);
    e->txbuf = dma_alloc(NTX * BUFSZ, 4096);
    if (!e->rx || !e->tx || !e->rxbuf || !e->txbuf) {
        kprintf("e1000e: out of memory\n");
        dma_free((void *)e->rx); dma_free((void *)e->tx); dma_free(e->rxbuf); dma_free(e->txbuf); kfree(e);
        return;
    }
    for (int i = 0; i < NRX; i++) e->rx[i].addr = virt_to_phys(e->rxbuf + i * BUFSZ);
    wr(e, REG_RDBAL, (u32)virt_to_phys((void *)e->rx));
    wr(e, REG_RDBAH, (u32)(virt_to_phys((void *)e->rx) >> 32));
    wr(e, REG_RDLEN, sizeof(rxd_t) * NRX);
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, NRX - 1);
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);        /* 2048-byte buffers, legacy descriptors */
    wr(e, REG_TDBAL, (u32)virt_to_phys((void *)e->tx));
    wr(e, REG_TDBAH, (u32)(virt_to_phys((void *)e->tx) >> 32));
    wr(e, REG_TDLEN, sizeof(txd_t) * NTX);
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    /* descriptors written back one by one (WTHRESH 1, GRAN 1), plus the "count" bit 22 */
    wr(e, REG_TXDCTL0, (rd(e, REG_TXDCTL0) & ~0x003F0000u) | 0x01010000u | (1u << 22));
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x3Fu << 12) | (rd(e, REG_TCTL) & TCTL_MULR));
    wr(e, REG_TIPG, 8u | (8u << 10) | (6u << 20));
    (void)rd(e, REG_MPC);
    /* interrupts: wake-ups only */
    snprintf(e->irq_name, sizeof e->irq_name, "e1000e %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    e->irq.name = e->irq_name;
    e->irq.pci = pd;
    e->irq.ack = e1e_ack;
    e->irq.ctx = e;
    e->ims = ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0;
    e->msix = irq_attach(&e->irq) == IRQ_MSIX;
    if (e->msix) {
        /* 82574: every cause (RxQ0/1, TxQ0/1, other) to MSI-X vector 0; Tx interrupts on every write-back */
        wr(e, REG_IVAR, 0x80088888u);
        wr(e, REG_EIAC_82574, 0);                /* no auto-clear: poll() clears ICR */
        wr(e, REG_CTRL_EXT, rd(e, REG_CTRL_EXT) | CTRL_EXT_PBA_CLR);
        e->ims |= ICR_RXQ0 | ICR_TXQ0 | ICR_OTHER;
    }
    wr(e, REG_IMS, e->ims);
    link_update(e);
    e->nd.driver = "e1000e";
    snprintf(e->nd.desc, sizeof e->nd.desc, "Intel %s Gigabit Ethernet", model);
    e->nd.xmit = e1e_xmit;
    e->nd.poll = e1e_poll;
    e->nd.set_multicast = e1e_set_multicast;
    e->nd.priv = e;
    e->nd.pci = pd;
    e->nd.irq = &e->irq;
    pd->driver = "e1000e";
    if (netdev_register(&e->nd)) kprintf("e1000e: too many network adapters\n");
}

/* copper ports only; ulp = the PHY has Ultra Low Power mode (I217 and the
 * first I218 steppings do not) */
static const struct { u16 id; u8 type, ulp; const char *model; } models[] = {
    { 0x105E, MT_82571, 0, "82571EB" }, { 0x10A4, MT_82571, 0, "82571EB (quad)" }, { 0x10BC, MT_82571, 0, "82571EB (quad)" },
    { 0x10D5, MT_82571, 0, "82571PT (quad)" }, { 0x107D, MT_82572, 0, "82572EI" }, { 0x10B9, MT_82572, 0, "82572EI" },
    { 0x108B, MT_82573, 0, "82573V" }, { 0x108C, MT_82573, 0, "82573E" }, { 0x109A, MT_82573, 0, "82573L" },
    { 0x10D3, MT_82574, 0, "82574L" }, { 0x10F6, MT_82574, 0, "82574LA" }, { 0x150C, MT_82583, 0, "82583V" },
    { 0x153A, MT_LPT, 0, "I217-LM" }, { 0x153B, MT_LPT, 0, "I217-V" }, { 0x155A, MT_LPT, 1, "I218-LM" },
    { 0x1559, MT_LPT, 1, "I218-V" }, { 0x15A0, MT_LPT, 0, "I218-LM" }, { 0x15A1, MT_LPT, 0, "I218-V" },
    { 0x15A2, MT_LPT, 1, "I218-LM" }, { 0x15A3, MT_LPT, 1, "I218-V" },
    { 0x156F, MT_SPT, 1, "I219-LM" }, { 0x1570, MT_SPT, 1, "I219-V" }, { 0x15B7, MT_SPT, 1, "I219-LM" },
    { 0x15B8, MT_SPT, 1, "I219-V" }, { 0x15B9, MT_SPT, 1, "I219-LM" }, { 0x15D7, MT_SPT, 1, "I219-LM" },
    { 0x15D8, MT_SPT, 1, "I219-V" }, { 0x15E3, MT_SPT, 1, "I219-LM" }, { 0x15D6, MT_SPT, 1, "I219-V" },
    { 0x0D53, MT_SPT, 1, "I219-LM" }, { 0x0D55, MT_SPT, 1, "I219-V" },
    { 0x15BD, MT_CNP, 1, "I219-LM" }, { 0x15BE, MT_CNP, 1, "I219-V" }, { 0x15BB, MT_CNP, 1, "I219-LM" },
    { 0x15BC, MT_CNP, 1, "I219-V" }, { 0x15DF, MT_CNP, 1, "I219-LM" }, { 0x15E0, MT_CNP, 1, "I219-V" },
    { 0x15E1, MT_CNP, 1, "I219-LM" }, { 0x15E2, MT_CNP, 1, "I219-V" }, { 0x0D4E, MT_CNP, 1, "I219-LM" },
    { 0x0D4F, MT_CNP, 1, "I219-V" }, { 0x0D4C, MT_CNP, 1, "I219-LM" }, { 0x0D4D, MT_CNP, 1, "I219-V" },
    { 0x15FB, MT_TGP, 1, "I219-LM" }, { 0x15FC, MT_TGP, 1, "I219-V" }, { 0x15F9, MT_TGP, 1, "I219-LM" },
    { 0x15FA, MT_TGP, 1, "I219-V" }, { 0x15F4, MT_TGP, 1, "I219-LM" }, { 0x15F5, MT_TGP, 1, "I219-V" },
    { 0x1A1E, MT_ADP, 1, "I219-LM" }, { 0x1A1F, MT_ADP, 1, "I219-V" }, { 0x1A1C, MT_ADP, 1, "I219-LM" },
    { 0x1A1D, MT_ADP, 1, "I219-V" }, { 0x550C, MT_ADP, 1, "I219-LM" }, { 0x550D, MT_ADP, 1, "I219-V" },
    { 0x0DC7, MT_ADP, 1, "I219-LM" }, { 0x0DC8, MT_ADP, 1, "I219-V" }, { 0x0DC5, MT_ADP, 1, "I219-LM" },
    { 0x0DC6, MT_ADP, 1, "I219-V" },
};

void e1000e_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->vendor != 0x8086 || pd->driver) continue;
        for (unsigned m = 0; m < ARRAY_SIZE(models); m++)
            if (pd->device == models[m].id) { e1e_attach(pd, models[m].type, models[m].model, models[m].ulp); break; }
    }
}
