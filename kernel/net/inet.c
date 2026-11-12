/* Halcyon OS - the AF_INET family and the protocol stack's frame:
 * socket(AF_INET, ...), addresses and ports, the hooks the net thread
 * calls (net_stack_init / input / poll / link, netdev.h), the loopback
 * interface, the packet-loss test hook, and the internet system calls
 * 116-127 (name lookup, interface information and configuration). */
#include "net/inet.h"
#include "net/netcfg.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/proc.h"
#include "core/settings.h"

netstat_k_t nstat;
int net_loss_pct;                   /* test hook: fw_cfg opt/halcyon/netloss */
static int stack_up, configured, settings_ready;
static u64 init_ms;

/* ---------------- SipHash-2-4 (ISNs, ports) ---------------- */

static u64 sip_k0, sip_k1;
#define ROTL(x, b) (u64)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
                      v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)

u64 net_hash(const void *data, u32 len) {
    const u8 *p = data;
    u64 v0 = 0x736f6d6570736575ull ^ sip_k0, v1 = 0x646f72616e646f6dull ^ sip_k1;
    u64 v2 = 0x6c7967656e657261ull ^ sip_k0, v3 = 0x7465646279746573ull ^ sip_k1;
    u64 b = (u64)len << 56;
    u32 i = 0;
    for (; i + 8 <= len; i += 8) {
        u64 m;
        __builtin_memcpy(&m, p + i, 8);
        v3 ^= m; SIPROUND; SIPROUND; v0 ^= m;
    }
    for (u32 k = 0; i + k < len; k++) b |= (u64)p[i + k] << (8 * k);
    v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
    v2 ^= 0xFF;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/* ---------------- addresses, ports, options ---------------- */

u16 port_random(void) { return (u16)(PORT_EPH_LO + random_u32() % (PORT_EPH_HI - PORT_EPH_LO + 1)); }

int sin_parse(const struct sockaddr *a, u32 len, u32 *addr, u16 *port) {
    if (len < sizeof(sa_family_t)) return E_INVAL;
    if (a->sa_family != AF_INET) return E_AFNOSUPPORT;
    if (len < 8) return E_INVAL;
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    *addr = in->sin_addr.s_addr;
    *port = ntoh16(in->sin_port);
    return 0;
}

u32 sin_make(struct sockaddr *a, u32 room, u32 addr, u16 port) {
    struct sockaddr_in k;
    memset(&k, 0, sizeof k);
    k.sin_family = AF_INET;
    k.sin_port = hton16(port);
    k.sin_addr.s_addr = addr;
    if (a && room) memcpy(a, &k, MIN(room, (u32)sizeof k));
    return sizeof k;
}

int inet_ipopt_set(int *ttl, int *tos, int name, const void *v, u32 len) {
    if (len < sizeof(int)) return E_INVAL;
    int iv = *(const int *)v;
    switch (name) {
    case IP_TTL:
        if (iv == -1) iv = 0;
        if (iv < 0 || iv > 255) return E_INVAL;
        *ttl = iv;
        return 0;
    case IP_TOS: *tos = iv & 0xFC; return 0;
    }
    return E_NOTSUP;
}

int inet_ipopt_get(int ttl, int tos, int name, void *v, u32 *len) {
    *len = sizeof(int);
    switch (name) {
    case IP_TTL: *(int *)v = ttl ? ttl : IP_DEFAULT_TTL; return 0;
    case IP_TOS: *(int *)v = tos; return 0;
    }
    return E_NOTSUP;
}

/* ---------------- the family ---------------- */

static int inet_create(sock_t *s, int type, int protocol) {
    if (!stack_up) return E_AFNOSUPPORT;
    if (type == SOCK_STREAM) {
        if (protocol && protocol != IPPROTO_TCP) return E_NOTSUP;
        s->protocol = IPPROTO_TCP;
        return tcp_create(s);
    }
    if (type == SOCK_DGRAM) {
        if (protocol == IPPROTO_ICMP) return udp_create(s, 1);
        if (protocol && protocol != IPPROTO_UDP) return E_NOTSUP;
        s->protocol = IPPROTO_UDP;
        return udp_create(s, 0);
    }
    return E_NOTSUP;
}

const net_family_t inet_family = { AF_INET, "inet", inet_create, NULL };

/* ---------------- loopback and the loss hook ---------------- */

#define LO_QMAX_BYTES (4u << 20)
static qpkt_t *loq, *loq_tail;
static u32 loq_bytes;

void net_poke(void) { netdev_kick(); }

void lo_output(const u8 *pkt, u32 len) {
    iface_t *lo = iface_lo();
    if (loq_bytes + len > LO_QMAX_BYTES) { if (lo) lo->tx_drop_stack++; return; }
    qpkt_t *q = kmalloc(sizeof *q + len);
    if (!q) return;
    q->next = NULL;
    q->len = len;
    memcpy(q->data, pkt, len);
    if (loq_tail) loq_tail->next = q; else loq = q;
    loq_tail = q;
    loq_bytes += len;
    if (lo) { lo->lo_packets++; lo->lo_bytes += len; }
    net_poke();
}

static void lo_drain(int budget) {
    iface_t *lo = iface_lo();
    while (loq && budget-- > 0) {
        qpkt_t *q = loq;
        loq = q->next;
        if (!loq) loq_tail = NULL;
        loq_bytes -= q->len;
        ip_input(lo, q->data, q->len, 0);
        kfree(q);
    }
}

int net_loss(void) {
    if (!net_loss_pct) return 0;
    if ((int)(random_u32() % 1000) >= net_loss_pct * 10) return 0;
    nstat.loss_dropped++;
    return 1;
}

/* ---------------- the hooks of the net thread (netdev.h) ---------------- */

void net_settings_loaded(void) { settings_ready = 1; }

void net_stack_init(void) {
    random_bytes(&sip_k0, sizeof sip_k0);
    random_bytes(&sip_k1, sizeof sip_k1);
    iface_init();
    tcp_init();
    char buf[16];
    if (qemu_fw_file("opt/halcyon/netloss", buf, sizeof buf)) {
        net_loss_pct = CLAMP(atoi(buf), 0, 50);
        if (net_loss_pct) kprintf("net: test hook: %d%% of frames lost (receive and transmit)\n", net_loss_pct);
    }
    init_ms = time_ms();
    stack_up = 1;
}

void net_stack_input(netdev_t *d, const u8 *frame, u32 len) {
    iface_t *ifc = iface_of_dev(d);
    if (!ifc) { d->rx_dropped++; return; }
    if (net_loss()) return;
    eth_input(ifc, frame, len);
}

u64 net_stack_poll(u64 now) {
    if (!configured && (settings_ready || now - init_ms > 4000)) {
        configured = 1;
        net_config_init();
    }
    for (int i = 0; i < iface_count(); i++) eth_flush(iface_get(i));
    lo_drain(256);
    u64 next = arp_timer(now);
    u64 t = ip_timer(now); if (t < next) next = t;
    t = tcp_timer(now); if (t < next) next = t;
    t = dhcp_timer(now); if (t < next) next = t;
    t = dns_timer(now); if (t < next) next = t;
    if (!configured) next = MIN(next, init_ms + 4000);
    if (loq) next = now;
    for (int i = 0; i < iface_count(); i++) if (iface_get(i)->ntxq) next = MIN(next, now + 1);
    return next;
}

void net_stack_link(netdev_t *d) {
    iface_t *ifc = iface_of_dev(d);
    if (!ifc) return;
    ifc->up = d->link_up;
    if (!ifc->up) { arp_flush(ifc); eth_flush(ifc); }
    route_rebuild();
    if (configured) dhcp_link(ifc);
}

/* ---------------- system calls 116-127 ---------------- */

static int resolve_cond(void *arg) { return !dns_pending(arg); }

static long sys_resolve(u64 uhost, u64 uout, long max, long timeout_ms) {
    char name[256];
    if (!uhost || !uout || max <= 0) return E_INVAL;
    int r = copyin_str(name, uhost, sizeof name);
    if (r) return r == E_NAMETOOLONG ? E_INVAL : r;
    max = MIN(max, (long)DNS_MAX_ADDRS);
    if (!user_writable(uout, (u64)max * 4)) return E_INVAL;
    u32 out[DNS_MAX_ADDRS];
    if (dns_literal(name, out)) return copyout(uout, out, 4) ? E_INVAL : 1;
    if (!dns_name_ok(name)) return E_INVAL;
    int n = dns_cached(name, out, (int)max);
    if (n) return n < 0 ? n : copyout(uout, out, (u64)n * 4) ? E_INVAL : n;
    if (timeout_ms <= 0) timeout_ms = 10000;
    timeout_ms = MIN(timeout_ms, 60000L);
    u64 deadline = time_ms() + (u64)timeout_ms;
    int err = 0;
    dnsq_t *q = dns_start(name, deadline, &err);
    if (!q) return err ? err : E_NOMEM;
    net_poke();
    int w = 1;
    while (dns_pending(q)) {
        w = proc_wait(resolve_cond, q, time_ms_to_tsc(deadline + 50));
        if (w <= 0) break;
    }
    if (w < 0) { dns_put(q); return w; }            /* the program is being ended */
    n = dns_result(q, out, (int)max);
    dns_put(q);
    if (n == E_AGAIN) return E_TIMEOUT;
    if (n < 0) return n;
    return copyout(uout, out, (u64)n * 4) ? E_INVAL : n;
}

static void fill_ifinfo(iface_t *f, hal_ifinfo_t *o) {
    memset(o, 0, sizeof *o);
    strlcpy(o->name, f->name, sizeof o->name);
    o->index = f->index;
    o->mtu = f->mtu;
    o->mode = f->mode;
    o->addr = f->addr; o->mask = f->mask; o->gateway = f->gw;
    o->conflict = f->conflict;
    u32 ds[DNS_SERVERS]; u16 dp[DNS_SERVERS];
    int nd = net_dns_servers(ds, dp, DNS_SERVERS);
    for (int i = 0; i < nd && i < 3; i++) o->dns[i] = ds[i];
    o->lease_s = o->lease_left_s = -1;
    if (f->dev) {
        netdev_t *d = f->dev;
        strlcpy(o->desc, d->desc, sizeof o->desc);
        strlcpy(o->driver, d->driver ? d->driver : "", sizeof o->driver);
        memcpy(o->mac, d->mac, 6);
        o->link_up = d->link_up; o->speed_mbps = d->speed_mbps; o->full_duplex = d->full_duplex;
        o->rx_packets = d->rx_packets; o->rx_bytes = d->rx_bytes; o->rx_errors = d->rx_errors;
        o->rx_dropped = d->rx_dropped + f->rx_drop_stack;
        o->tx_packets = d->tx_packets; o->tx_bytes = d->tx_bytes; o->tx_errors = d->tx_errors;
        o->tx_dropped = d->tx_dropped;
        i64 left, lease;
        u32 server;
        o->dhcp_state = dhcp_state(f, &server, &left, &lease);
        o->dhcp_server = server;
        o->lease_s = lease; o->lease_left_s = left;
    } else {
        strlcpy(o->desc, "Loopback", sizeof o->desc);
        o->loopback = 1;
        o->link_up = 1;
        o->rx_packets = o->tx_packets = f->lo_packets;
        o->rx_bytes = o->tx_bytes = f->lo_bytes;
        o->tx_dropped = f->tx_drop_stack;
    }
}

static int mask_ok(u32 m) {
    u32 h = ntoh32(m);
    return h && (~h & (~h + 1)) == 0;               /* contiguous ones */
}

static void save_config(iface_t *f) {
    char key[40];
    snprintf(key, sizeof key, "net.%s.mode", f->name);
    settings_set(key, f->mode == IFMODE_STATIC ? "static" : "dhcp");
    const char *names[3] = { "ip", "mask", "gateway" };
    u32 vals[3] = { f->addr, f->mask, f->gw };
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof key, "net.%s.%s", f->name, names[i]);
        settings_set(key, f->mode == IFMODE_STATIC && vals[i] ? ipstr(vals[i]) : NULL);
    }
}

static void save_dns(const u32 *dns, int n) {
    char val[64] = "";
    for (int i = 0; i < n; i++) {
        size_t l = strlen(val);
        snprintf(val + l, sizeof val - l, "%s%s", i ? "," : "", ipstr(dns[i]));
    }
    settings_set("net.dns", n ? val : NULL);
}

int net_if_config(const hal_ifconfig_t *cfg) {
    if (!stack_up) return E_NODEV;
    hal_ifconfig_t c = *cfg;
    c.name[sizeof c.name - 1] = 0;
    iface_t *f = iface_by_name(c.name);
    if (!f) return E_NOENT;
    if (!f->dev && (c.flags & NETCFG_ADDR)) return E_PERM;     /* the loopback stays as it is */
    if (c.flags & NETCFG_ADDR) {
        if (c.mode == NET_MODE_STATIC) {
            if (ip_bad_unicast(c.addr) || ip_is_loopnet(c.addr) || !mask_ok(c.mask)) return E_INVAL;
            if (c.gateway && ((c.gateway & c.mask) != (c.addr & c.mask) || c.gateway == c.addr || ip_bad_unicast(c.gateway))) return E_INVAL;
            dhcp_stop(f);
            f->mode = IFMODE_STATIC;
            iface_set_addr(f, c.addr, c.mask, c.gateway, "static");
        } else if (c.mode == NET_MODE_DHCP) {
            f->mode = IFMODE_DHCP;
            dhcp_start(f);
        } else return E_INVAL;
    }
    int nd = 0;
    u32 dns[3]; u16 dp[3];
    if (c.flags & NETCFG_DNS) {
        for (int i = 0; i < 3; i++) {
            if (!c.dns[i]) continue;
            if (ip_bad_unicast(c.dns[i])) return E_INVAL;
            dns[nd] = c.dns[i]; dp[nd] = 53; nd++;
        }
        net_dns_override(dns, dp, nd);
    }
    net_poke();
    if (c.flags & NETCFG_SAVE) {
        if (c.flags & NETCFG_ADDR) save_config(f);
        if (c.flags & NETCFG_DNS) save_dns(dns, nd);
        int r = settings_save();
        if (r) return r;
    }
    return 0;
}

static long sys_ifconfig(u64 ucfg) {
    hal_ifconfig_t c;
    if (copyin(&c, ucfg, sizeof c)) return E_INVAL;
    return net_if_config(&c);
}

int net_if_ctl(int op, const char *name) {
    if (!stack_up) return E_NODEV;
    iface_t *f = NULL;
    if (name) {
        f = iface_by_name(name);
        if (!f) return E_NOENT;
    }
    switch (op) {
    case NETCTL_RENEW:
    case NETCTL_RELEASE:
        if (!f) f = iface_get(0);
        if (!f || !f->dev) return E_NODEV;
        if (f->mode != IFMODE_DHCP) return E_INVAL;
        net_poke();
        return op == NETCTL_RENEW ? dhcp_renew(f) : dhcp_release(f);
    case NETCTL_DNS_FLUSH: dns_flush(); return 0;
    case NETCTL_ARP_FLUSH:
        if (f) arp_flush(f);
        else for (int i = 0; i < iface_count(); i++) arp_flush(iface_get(i));
        return 0;
    }
    return E_INVAL;
}

static long sys_netctl(long op, u64 uname) {
    char name[16];
    if (uname && copyin_str(name, uname, sizeof name)) return E_INVAL;
    return net_if_ctl((int)op, uname ? name : NULL);
}

int net_if_info(int index, hal_ifinfo_t *out) {
    if (!stack_up) return E_NODEV;
    iface_t *f = index >= 0 ? iface_get(index) : NULL;
    if (!f) return E_NOENT;
    fill_ifinfo(f, out);
    return 0;
}

static long sys_netstats(u64 uout) {
    hal_netstats_t o;
    memset(&o, 0, sizeof o);
    o.ip_in = nstat.ip_in; o.ip_out = nstat.ip_out; o.ip_bad = nstat.ip_bad; o.ip_frag_in = nstat.ip_frag_in;
    o.ip_reasm_ok = nstat.ip_reasm_ok; o.ip_reasm_fail = nstat.ip_reasm_fail; o.ip_frag_out = nstat.ip_frag_out;
    o.ip_noroute = nstat.ip_noroute;
    o.icmp_in = nstat.icmp_in; o.icmp_out = nstat.icmp_out; o.icmp_err_in = nstat.icmp_err_in;
    o.icmp_err_out = nstat.icmp_err_out; o.icmp_ratelimited = nstat.icmp_ratelimited;
    o.arp_in = nstat.arp_in; o.arp_out = nstat.arp_out; o.arp_fail = nstat.arp_fail;
    o.udp_in = nstat.udp_in; o.udp_out = nstat.udp_out; o.udp_noport = nstat.udp_noport; o.udp_drop = nstat.udp_drop;
    o.tcp_in = nstat.tcp_in; o.tcp_out = nstat.tcp_out; o.tcp_bad = nstat.tcp_bad; o.tcp_rexmt = nstat.tcp_rexmt;
    o.tcp_fast_rexmt = nstat.tcp_fast_rexmt; o.tcp_timeouts = nstat.tcp_timeouts; o.tcp_rst_in = nstat.tcp_rst_in;
    o.tcp_rst_out = nstat.tcp_rst_out; o.tcp_active = nstat.tcp_active; o.tcp_passive = nstat.tcp_passive;
    o.tcp_established = nstat.tcp_established; o.tcp_dupack = nstat.tcp_dupack; o.tcp_ooo = nstat.tcp_ooo;
    o.tcp_challenge = nstat.tcp_challenge; o.tcp_synq_drop = nstat.tcp_synq_drop;
    u32 cached; u64 q, h, rej;
    dns_counts(&cached, &q, &h, &rej);
    o.dns_cached = (int)cached; o.dns_queries = q; o.dns_hits = h; o.dns_rejected = rej;
    o.loss_dropped = nstat.loss_dropped;
    o.tcp_conns = tcp_count();
    o.udp_socks = udp_count();
    for (int i = 0; i < iface_count(); i++) {
        iface_t *f = iface_get(i);
        for (int k = 0; k < ARP_MAX; k++) if (f->arp[k].state == ARP_REACHABLE) o.arp_entries++;
    }
    return copyout(uout, &o, sizeof o) ? E_INVAL : 0;
}

static long sys_netconns(u64 uout, long max) {
    if (max <= 0) return 0;
    max = MIN(max, 2048L);
    if (!user_writable(uout, (u64)max * sizeof(hal_netconn_t))) return E_INVAL;
    hal_netconn_t *c = kmalloc((u64)max * sizeof *c);
    if (!c) return E_NOMEM;
    int n = tcp_conns(c, (int)max);
    n += udp_conns(c + n, (int)max - n);
    long r = copyout(uout, c, (u64)n * sizeof *c) ? E_INVAL : n;
    kfree(c);
    return r;
}

long inet_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    if (!stack_up) return E_NODEV;
    switch (nr) {
    case SYS_RESOLVE: return sys_resolve(a1, a2, (long)(int)a3, (long)(int)a4);
    case SYS_NET_IFINFO: {
        iface_t *f = iface_get((int)a1);
        if ((int)a1 < 0 || !f) return E_NOENT;
        hal_ifinfo_t o;
        fill_ifinfo(f, &o);
        return copyout(a2, &o, sizeof o) ? E_INVAL : 0;
    }
    case SYS_NET_IFCONFIG: return sys_ifconfig(a1);
    case SYS_NET_CTL: return sys_netctl((long)(int)a1, a2);
    case SYS_NET_STATS: return sys_netstats(a1);
    case SYS_NET_CONNS: return sys_netconns(a1, (long)(int)a2);
    }
    return E_NOTSUP;
}
