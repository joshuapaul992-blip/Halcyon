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

/* ---------------- test hook: QEMU fw_cfg "opt/halcyon/netprobe" ----------------
 * The value is a frame count N (empty: 1). Once an adapter's link is up, the
 * net thread sends N ARP requests for QEMU's user-mode gateway 10.0.2.2 from
 * 10.0.2.15 on it and watches the received frames for the replies without
 * consuming them. At most 32 requests are outstanding, so the replies never
 * overrun a receive ring; with a few thousand frames every ring wraps many
 * times. It logs the first reply and a summary (frames sent, replies, frames
 * the adapter reported transmitted, the interrupt). With N > 1 a burst of N/4
 * requests follows as fast as the transmit ring takes them: the ring fills
 * (E_BUSY, retried after the next poll) and the replies may overrun the
 * receive ring (the adapter drops them; nothing else may go wrong). Never
 * active on real hardware (fw_cfg exists only under QEMU). */
enum { PR_IDLE, PR_PING, PR_PING_WAIT, PR_BURST, PR_BURST_WAIT, PR_DONE };
typedef struct probe { u32 sent, replies, errors, busy, burst_sent, burst_replies; u64 tx0, t0, last; u8 state; } probe_t;
static probe_t probes[NETDEV_MAX];
static u32 probe_want;              /* frames per adapter; 0 = no probe */

static int probe_index(netdev_t *d) {
    for (int i = 0; i < ndevs; i++) if (devs[i] == d) return i;
    return -1;
}

static void probe_rx(netdev_t *d, const u8 *f, u32 len) {
    static const u8 gw[4] = { 10, 0, 2, 2 }, me[4] = { 10, 0, 2, 15 };
    int i = probe_index(d);
    if (i < 0 || len < 42 || f[12] != 0x08 || f[13] != 0x06) return;
    if (f[20] != 0 || f[21] != 2 || memcmp(f + 28, gw, 4) || memcmp(f + 38, me, 4)) return;
    probe_t *p = &probes[i];
    if (p->state == PR_IDLE || p->state == PR_DONE) return;
    if (p->state >= PR_BURST) { p->burst_replies++; p->last = time_ms(); return; }
    if (!p->replies++)
        kprintf("net: %s: probe reply from %02x:%02x:%02x:%02x:%02x:%02x\n", d->name, f[22], f[23], f[24], f[25], f[26], f[27]);
    p->last = time_ms();
}

static void probe_frame(netdev_t *d, u8 *f) {
    static const u8 tail[] = { 0x08, 0x06, 0, 1, 0x08, 0x00, 6, 4, 0, 1 };
    memset(f, 0xFF, 6);
    memcpy(f + 6, d->mac, 6);
    memcpy(f + 12, tail, sizeof tail);
    memcpy(f + 22, d->mac, 6);
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;
    memset(f + 32, 0, 6);
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;
}

/* send up to *count frames while the window allows; 1 when all are sent */
static int probe_send(netdev_t *d, probe_t *p, u32 *count, u32 want, int window, u64 now) {
    u8 f[42];
    probe_frame(d, f);
    while (*count < want && (!window || p->sent - p->replies < 32)) {
        int r = d->xmit(d, f, sizeof f);
        if (r == E_BUSY) { p->busy++; break; }  /* ring full: again after the next poll */
        if (r) p->errors++;
        (*count)++;
        p->last = now;
    }
    return *count >= want;
}

/* returns the next deadline */
static u64 probe_poll(u64 now) {
    u64 next = ~0ull;
    for (int i = 0; i < ndevs; i++) {
        netdev_t *d = devs[i];
        probe_t *p = &probes[i];
        u64 done = d->tx_packets - p->tx0;
        switch (p->state) {
        case PR_DONE: continue;
        case PR_IDLE:
            if (!d->link_up) continue;
            p->state = PR_PING;
            p->t0 = p->last = now;
            p->tx0 = d->tx_packets;
            done = 0;
            /* fall through */
        case PR_PING:
            if (probe_send(d, p, &p->sent, probe_want, 1, now)) p->state = PR_PING_WAIT;
            break;
        case PR_PING_WAIT:
            if ((p->replies >= p->sent && done >= p->sent) || now - p->last > 3000) {
                char irq[48] = "polled";
                if (d->irq) irq_describe(d->irq, irq, sizeof irq);
                kprintf("net: %s: probe: %u sent, %u replies, %lu transmitted in %lu ms; errors: %u send, %lu tx, %lu rx; interrupt %s\n",
                        d->name, p->sent, p->replies, done, p->last - p->t0, p->errors, d->tx_errors, d->rx_errors, irq);
                p->state = probe_want > 1 ? PR_BURST : PR_DONE;
                p->t0 = now;
                p->busy = 0;
            }
            break;
        case PR_BURST:
            if (probe_send(d, p, &p->burst_sent, probe_want / 4, 0, now)) p->state = PR_BURST_WAIT;
            break;
        case PR_BURST_WAIT:
            if (done >= (u64)p->sent + p->burst_sent || now - p->last > 3000) {
                kprintf("net: %s: probe burst: %u sent (transmit ring full %u times), %lu transmitted, %u replies in %lu ms; errors: %lu tx, %lu rx\n",
                        d->name, p->burst_sent, p->busy, done - p->sent, p->burst_replies, now - p->t0, d->tx_errors, d->rx_errors);
                p->state = PR_DONE;
                continue;
            }
            break;
        }
        u64 t = now + (p->state == PR_PING || p->state == PR_BURST ? 1 : 20);
        if (t < next) next = t;
    }
    return next;
}

void netdev_rx(netdev_t *d, const void *frame, u32 len) {
    if (len < ETH_HDR || len > ETH_MAX_FRAME + 4) { d->rx_errors++; return; }
    if (probe_want) probe_rx(d, frame, len);
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
    char pb[16];
    if (qemu_fw_file("opt/halcyon/netprobe", pb, sizeof pb)) {
        probe_want = pb[0] ? (u32)strtol(pb, NULL, 0) : 1;
        if (probe_want) kprintf("net: probe: %u ARP requests for 10.0.2.2 per adapter\n", probe_want);
    }
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
        if (probe_want) { u64 t = probe_poll(now); if (t < next) next = t; }
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
    if (e1000e_init) e1000e_init();
    if (igb_init) igb_init();
    if (rtl8139_init) rtl8139_init();
    if (rtl8169_init) rtl8169_init();
    if (!ndevs) kprintf("net: no network adapter\n");
    if (!ndevs && !net_stack_init) return;      /* (the stack still runs the loopback) */
    net_thread = thread_create("net", net_main, NULL);
    if (!net_thread) kprintf("net: cannot start the net thread\n");
}
