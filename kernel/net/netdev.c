/* Halcyon OS - network devices and the net thread */
#include "netdev.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/irq.h"
#include "core/thread.h"

static netdev_t *devs[NETDEV_MAX];
static int ndevs;
static volatile int kicked;
static thread_t *net_thread;
static u32 wait_gen;

int netdev_count(void) { return ndevs; }
netdev_t *netdev_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }

int netdev_register(netdev_t *d) {
    if (ndevs >= NETDEV_MAX) return E_BUSY;
    snprintf(d->name, sizeof d->name, "eth%d", ndevs);
    if (!d->mtu) d->mtu = 1500;
    devs[ndevs++] = d;
    kprintf("net: %s: %s (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", d->name, d->desc, d->driver,
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->link_up ? "up" : "down");
    return 0;
}

void netdev_rx(netdev_t *d, const void *frame, u32 len) {
    if (len < ETH_HDR || len > ETH_MAX_FRAME + 4) { d->rx_errors++; return; }
    if (net_stack_input) net_stack_input(d, frame, len);
    else d->rx_dropped++;
}

void netdev_kick(void) { kicked = 1; }

static int net_wanted(void *arg) { (void)arg; return kicked || irq_generation() != wait_gen; }

static void net_main(void *arg) {
    (void)arg;
    int last_link[NETDEV_MAX];
    for (int i = 0; i < ndevs; i++) last_link[i] = devs[i]->link_up;
    if (net_stack_init) net_stack_init();
    for (;;) {
        wait_gen = irq_generation();
        kicked = 0;
        int polled_only = 0;
        for (int i = 0; i < ndevs; i++) {
            netdev_t *d = devs[i];
            d->poll(d);
            if (!d->irq_live) polled_only = 1;
            if (d->link_up != last_link[i]) {
                last_link[i] = d->link_up;
                kprintf("net: %s: link %s%s\n", d->name, d->link_up ? "up" : "down",
                        d->link_up && d->speed_mbps ? (d->speed_mbps >= 1000 ? ", 1 Gbit/s" : d->speed_mbps >= 100 ? ", 100 Mbit/s" : ", 10 Mbit/s") : "");
                if (net_stack_link) net_stack_link(d);
            }
        }
        u64 now = time_ms();
        u64 next = net_stack_poll ? net_stack_poll(now) : ~0ull;
        /* without working interrupts poll often; link changes are noticed within a second */
        u64 cap = now + (polled_only ? 10 : 1000);
        if (next > cap) next = cap;
        if (kicked) continue;
        thread_wait(net_wanted, NULL, time_ms_to_tsc(next));
    }
}

void net_init(void) {
    if (virtio_net_init) virtio_net_init();
    if (e1000_init) e1000_init();
    if (rtl8139_init) rtl8139_init();
    if (rtl8169_init) rtl8169_init();
    if (!ndevs) { kprintf("net: no network adapter\n"); return; }
    net_thread = thread_create("net", net_main, NULL);
    if (!net_thread) kprintf("net: cannot start the net thread\n");
}
