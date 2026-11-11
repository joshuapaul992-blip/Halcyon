/* Halcyon OS - network devices (milestone 6).
 *
 * A network card driver fills in a netdev_t and registers it. The "net"
 * kernel thread owns all network work: it calls every device's poll() (the
 * driver harvests received frames and hands each to netdev_rx(), reaps sent
 * ones, notes link changes), then the protocol stack's net_stack_poll() (timers,
 * queued output), then sleeps until a device interrupt, a netdev_kick(), or
 * the stack's next deadline. Interrupts are only wake-ups, as everywhere in
 * Halcyon (DESIGN-M2): drivers attach their PCI interrupt with irq_attach()
 * and must work polled too. Everything runs under the big kernel lock.
 *
 * Frames are whole Ethernet frames without the FCS: 14-byte header + payload,
 * at most 1514 bytes (MTU 1500). Drivers copy on both sides: the buffer given
 * to xmit() and the one given to netdev_rx() belong to the caller. */
#ifndef NET_NETDEV_H
#define NET_NETDEV_H
#include "core/types.h"

struct pci_dev;
struct irq_handle;

#define ETH_HDR 14
#define ETH_MAX_FRAME 1514
#define NETDEV_MAX 8

typedef struct netdev netdev_t;
struct netdev {
    char name[8];                   /* "eth0" (set by netdev_register) */
    char desc[64];                  /* "Intel 82540EM Gigabit Ethernet" */
    const char *driver;             /* "e1000" */
    u8 mac[6];
    u16 mtu;                        /* 1500 */
    int link_up;                    /* the driver keeps these current */
    int speed_mbps;                 /* 10 / 100 / 1000, 0 = unknown */
    int full_duplex;
    /* send one frame (len <= ETH_MAX_FRAME; the driver pads short frames to
     * 60 bytes). 0, or E_BUSY when the transmit ring is full (the stack
     * keeps the frame and tries again after the next poll), or E_IO. */
    int  (*xmit)(netdev_t *d, const void *frame, u32 len);
    /* harvest received frames (netdev_rx each), reap transmitted ones,
     * update link_up; called by the net thread only */
    void (*poll)(netdev_t *d);
    /* accept frames for these multicast MACs too (IPv6 later); may be NULL */
    void (*set_multicast)(netdev_t *d, const u8 (*macs)[6], int n);
    void *priv;                     /* driver */
    void *stack;                    /* protocol stack's interface state */
    int irq_live;                   /* the driver's interrupt works (else the thread polls often) */
    struct pci_dev *pci;            /* the adapter's PCI function (System Information), may be NULL */
    struct irq_handle *irq;         /* its interrupt (System Information), may be NULL */
    /* statistics (drivers count rx/tx; the stack counts drops it makes).
     * tx_packets / tx_bytes count frames the adapter reports sent. */
    u64 rx_packets, rx_bytes, rx_errors, rx_dropped;
    u64 tx_packets, tx_bytes, tx_errors, tx_dropped;
};

int  netdev_register(netdev_t *d);          /* names it ethN; 0 or E_BUSY */
int  netdev_count(void);
netdev_t *netdev_get(int i);
void netdev_rx(netdev_t *d, const void *frame, u32 len);   /* driver -> stack */
void netdev_kick(void);                     /* wake the net thread (output queued, config changed) */
void net_init(void);                        /* kmain: probe drivers, start the thread */

/* the protocol stack (weak: the drivers build and run without one) */
void net_stack_init(void) __attribute__((weak));
void net_stack_input(netdev_t *d, const u8 *frame, u32 len) __attribute__((weak));
/* timers and queued output; returns the next deadline (time_ms) or ~0 */
u64  net_stack_poll(u64 now_ms) __attribute__((weak));
void net_stack_link(netdev_t *d) __attribute__((weak));    /* link_up changed */
void net_settings_loaded(void) __attribute__((weak));      /* kmain: SETTINGS.INI has been read */

/* drivers (weak: probe the PCI bus, register what they find) */
void virtio_net_init(void) __attribute__((weak));
void e1000_init(void) __attribute__((weak));
void e1000e_init(void) __attribute__((weak));
void igb_init(void) __attribute__((weak));
void rtl8139_init(void) __attribute__((weak));
void rtl8169_init(void) __attribute__((weak));

#endif
