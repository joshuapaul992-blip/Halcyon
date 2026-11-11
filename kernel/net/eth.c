/* Halcyon OS - interfaces, Ethernet, ARP (RFC 826, 5227 defence) and the
 * routing table (connected subnets, a default gateway, the loopback). */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"

#define ETH_IP  0x0800
#define ETH_ARP 0x0806
#define ROUTE_MAX 24
#define ARP_QUEUED_MAX 64          /* packets waiting for ARP, all interfaces */

static iface_t ifs[IF_MAX];
static int nifs;
static iface_t *lo;
static int arp_queued;
static const u8 bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

int iface_count(void) { return nifs; }
iface_t *iface_get(int i) { return i >= 0 && i < nifs ? &ifs[i] : NULL; }
iface_t *iface_lo(void) { return lo; }

iface_t *iface_by_name(const char *name) {
    for (int i = 0; i < nifs; i++) if (!strcasecmp(ifs[i].name, name)) return &ifs[i];
    return NULL;
}

iface_t *iface_of_dev(netdev_t *d) { return d ? d->stack : NULL; }

void iface_init(void) {
    nifs = 0;
    for (int i = 0; i < netdev_count() && nifs < IF_MAX - 1; i++) {
        netdev_t *d = netdev_get(i);
        iface_t *f = &ifs[nifs];
        memset(f, 0, sizeof *f);
        f->index = nifs++;
        f->dev = d;
        strlcpy(f->name, d->name, sizeof f->name);
        memcpy(f->mac, d->mac, 6);
        f->mtu = d->mtu ? d->mtu : 1500;
        if (f->mtu > ETH_MAX_FRAME - ETH_HDR) f->mtu = ETH_MAX_FRAME - ETH_HDR;
        f->up = d->link_up;
        d->stack = f;
    }
    lo = &ifs[nifs];
    memset(lo, 0, sizeof *lo);
    lo->index = nifs++;
    strlcpy(lo->name, "lo", sizeof lo->name);
    lo->mtu = LO_MTU;
    lo->up = 1;
    lo->mode = IFMODE_STATIC;
    lo->addr = IP4(127, 0, 0, 1);
    lo->mask = IP4(255, 0, 0, 0);
    route_rebuild();
}

int ip_is_local(u32 a) {
    if (ip_is_loopnet(a)) return 1;
    for (int i = 0; i < nifs; i++) if (ifs[i].addr && ifs[i].addr == a) return 1;
    return 0;
}

int ip_is_bcast(iface_t *ifc, u32 a) {
    if (a == 0xFFFFFFFFu) return 1;
    return ifc && ifc->addr && ifc->mask != 0xFFFFFFFFu && a == (ifc->addr | ~ifc->mask);
}

/* ---------------- routing ---------------- */

typedef struct route { u32 dst, mask, gw; iface_t *ifc; int metric; } route_t;
static route_t routes[ROUTE_MAX];
static int nroutes;

void route_rebuild(void) {
    nroutes = 0;
    for (int i = 0; i < nifs && nroutes < ROUTE_MAX - 1; i++) {
        iface_t *f = &ifs[i];
        if (!f->addr) continue;
        routes[nroutes++] = (route_t){ f->addr & f->mask, f->mask, 0, f, f->dev ? 10 + i : 0 };
        if (f->gw && nroutes < ROUTE_MAX)
            routes[nroutes++] = (route_t){ 0, 0, f->gw, f, 100 + i };
    }
}

static int popcount32(u32 v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

int route_lookup(u32 dst, route_res_t *r) {
    memset(r, 0, sizeof *r);
    if (ip_is_local(dst)) {                 /* to ourselves: the loopback */
        r->ifc = lo;
        r->nexthop = dst;
        r->src = ip_is_loopnet(dst) ? IP4(127, 0, 0, 1) : dst;
        return 0;
    }
    if (dst == 0xFFFFFFFFu) {               /* limited broadcast: the first configured interface that is up */
        for (int i = 0; i < nifs; i++) {
            if (!ifs[i].dev || !ifs[i].up || !ifs[i].addr) continue;
            r->ifc = &ifs[i]; r->nexthop = dst; r->src = ifs[i].addr; r->bcast = 1;
            return 0;
        }
        return E_NETUNREACH;
    }
    if (ip_is_mcast(dst) || !ntoh32(dst)) return E_NETUNREACH;     /* no multicast */
    route_t *best = NULL;
    for (int i = 0; i < nroutes; i++) {
        route_t *e = &routes[i];
        if ((dst & e->mask) != e->dst || !e->ifc->up) continue;
        if (!best || popcount32(e->mask) > popcount32(best->mask) ||
            (e->mask == best->mask && e->metric < best->metric)) best = e;
    }
    if (!best || best->ifc == lo) return E_NETUNREACH;      /* (127/8 other than local: no) */
    r->ifc = best->ifc;
    r->nexthop = best->gw ? best->gw : dst;
    r->src = best->ifc->addr;
    r->bcast = !best->gw && ip_is_bcast(best->ifc, dst);
    return 0;
}

int route_list(u32 *dst, u32 *mask, u32 *gw, int *ifindex, int max) {
    int n = 0;
    for (int i = 0; i < nroutes && n < max; i++, n++) {
        dst[n] = routes[i].dst; mask[n] = routes[i].mask; gw[n] = routes[i].gw; ifindex[n] = routes[i].ifc->index;
    }
    return n;
}

/* ---------------- interface addresses ---------------- */

static int mask_bits(u32 m) { return popcount32(m); }

void iface_set_addr(iface_t *ifc, u32 addr, u32 mask, u32 gw, const char *how) {
    if (ifc == lo) return;
    int changed = ifc->addr != addr || ifc->mask != mask || ifc->gw != gw;
    if (addr && !mask) {                    /* classful default */
        u32 h = ntoh32(addr) >> 24;
        mask = h < 128 ? IP4(255, 0, 0, 0) : h < 192 ? IP4(255, 255, 0, 0) : IP4(255, 255, 255, 0);
    }
    if (gw && (gw & mask) != (addr & mask)) gw = 0;      /* a gateway must be on the subnet */
    int newaddr = ifc->addr != addr;
    ifc->addr = addr;
    ifc->mask = addr ? mask : 0;
    ifc->gw = addr ? gw : 0;
    ifc->conflict = 0;
    if (!addr) ifc->ndns = 0;
    route_rebuild();
    if (addr && newaddr) arp_announce(ifc);
    if (!changed && addr) return;
    if (!addr) { kprintf("net: %s: no address%s%s\n", ifc->name, how ? " " : "", how ? how : ""); return; }
    char dns[64] = "";
    u32 ds[DNS_SERVERS]; u16 dp[DNS_SERVERS];
    int nd = net_dns_servers(ds, dp, DNS_SERVERS);
    for (int i = 0; i < nd; i++) {
        size_t l = strlen(dns);
        snprintf(dns + l, sizeof dns - l, "%s%s", i ? " " : "", ipstr(ds[i]));
        if (dp[i] != 53) { l = strlen(dns); snprintf(dns + l, sizeof dns - l, ":%u", dp[i]); }
    }
    char via[32] = "";
    if (gw) snprintf(via, sizeof via, " via %s", ipstr(gw));
    kprintf("net: %s: %s/%d%s, DNS %s (%s)\n", ifc->name, ipstr(addr), mask_bits(ifc->mask), via,
            nd ? dns : "none", how ? how : "static");
}

/* ---------------- Ethernet ---------------- */

static void txq_free_all(iface_t *ifc) {
    while (ifc->txq) { qpkt_t *q = ifc->txq; ifc->txq = q->next; kfree(q); }
    ifc->txq_tail = NULL;
    ifc->ntxq = 0;
}

/* hand a complete frame to the driver, or queue it behind earlier ones */
static int eth_send_frame(iface_t *ifc, const u8 *frame, u32 len) {
    netdev_t *d = ifc->dev;
    if (!ifc->up) { d->tx_dropped++; return E_NETUNREACH; }
    if (net_loss()) return 0;               /* test hook: "lost on the wire" */
    if (!ifc->txq) {
        int r = d->xmit(d, frame, len);
        if (r != E_BUSY) { if (r) d->tx_errors++; return r; }
    }
    if (ifc->ntxq >= IF_TXQ) { d->tx_dropped++; ifc->tx_drop_stack++; return E_NOMEM; }
    qpkt_t *q = kmalloc(sizeof *q + len);
    if (!q) { d->tx_dropped++; return E_NOMEM; }
    q->next = NULL;
    q->len = len;
    memcpy(q->data, frame, len);
    if (ifc->txq_tail) ifc->txq_tail->next = q; else ifc->txq = q;
    ifc->txq_tail = q;
    ifc->ntxq++;
    return 0;
}

void eth_flush(iface_t *ifc) {
    if (!ifc->dev) return;
    if (!ifc->up) { txq_free_all(ifc); return; }
    while (ifc->txq) {
        qpkt_t *q = ifc->txq;
        int r = ifc->dev->xmit(ifc->dev, q->data, q->len);
        if (r == E_BUSY) break;
        if (r) ifc->dev->tx_errors++;
        ifc->txq = q->next;
        if (!ifc->txq) ifc->txq_tail = NULL;
        ifc->ntxq--;
        kfree(q);
    }
}

static void eth_header(u8 *f, const u8 *dst, const u8 *src, u16 type) {
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    wr16(f + 12, type);
}

/* ---------------- ARP ---------------- */

static void arp_send(iface_t *ifc, u16 op, const u8 *tha, u32 tpa, u32 spa, const u8 *dstmac) {
    u8 f[ETH_HDR + 28];
    eth_header(f, dstmac, ifc->mac, ETH_ARP);
    u8 *a = f + ETH_HDR;
    wr16(a, 1); wr16(a + 2, ETH_IP); a[4] = 6; a[5] = 4; wr16(a + 6, op);
    memcpy(a + 8, ifc->mac, 6); wraddr(a + 14, spa);
    memcpy(a + 18, tha, 6); wraddr(a + 24, tpa);
    nstat.arp_out++;
    eth_send_frame(ifc, f, sizeof f);
}

static void arp_request(iface_t *ifc, u32 ip, const u8 *unicast) {
    static const u8 zero[6];
    arp_send(ifc, 1, zero, ip, ifc->addr, unicast ? unicast : bcast_mac);
}

void arp_announce(iface_t *ifc) {
    static const u8 zero[6];
    if (!ifc->dev || !ifc->addr || !ifc->up) return;
    arp_send(ifc, 1, zero, ifc->addr, ifc->addr, bcast_mac);   /* gratuitous: sender = target = us */
    ifc->garp_at = time_ms() + 1000;                         /* once more in a second */
}

int arp_probe_conflict(iface_t *ifc, u32 addr) {
    static const u8 zero[6];
    if (!ifc->dev || !ifc->up) return E_NETUNREACH;
    arp_send(ifc, 1, zero, addr, 0, bcast_mac);              /* RFC 5227 probe: sender address 0 */
    return 0;
}

/* free an entry; with report, its waiting packets come back to their
 * senders as "host unreachable" (after the entry is gone: they may send) */
static void ent_free(iface_t *ifc, arp_ent_t *e, int report) {
    qpkt_t *list = e->q;
    arp_queued -= e->nq;
    memset(e, 0, sizeof *e);
    while (list) {
        qpkt_t *q = list;
        list = q->next;
        if (report) ip_deliver_error(q->data + ETH_HDR, q->len - ETH_HDR, E_HOSTUNREACH, 3, 1, 0, 1);
        kfree(q);
    }
}

void arp_flush(iface_t *ifc) {
    for (int i = 0; i < ARP_MAX; i++) if (ifc->arp[i].state) ent_free(ifc, &ifc->arp[i], 0);
}

static arp_ent_t *arp_find(iface_t *ifc, u32 ip) {
    for (int i = 0; i < ARP_MAX; i++) if (ifc->arp[i].state && ifc->arp[i].ip == ip) return &ifc->arp[i];
    return NULL;
}

/* a free slot, else the least recently used resolved entry (never one
 * that is being resolved: those time out by themselves) */
static arp_ent_t *arp_alloc(iface_t *ifc, u32 ip) {
    arp_ent_t *victim = NULL;
    for (int i = 0; i < ARP_MAX; i++) {
        arp_ent_t *e = &ifc->arp[i];
        if (!e->state) { victim = e; break; }
        if (e->state == ARP_REACHABLE && (!victim || e->used < victim->used)) victim = e;
    }
    if (!victim) return NULL;
    if (victim->state) ent_free(ifc, victim, 0);
    victim->ip = ip;
    victim->used = time_ms();
    return victim;
}

static void ent_resolved(iface_t *ifc, arp_ent_t *e, const u8 *mac) {
    memcpy(e->mac, mac, 6);
    e->state = ARP_REACHABLE;
    e->confirmed = time_ms();
    e->tries = 0;
    while (e->q) {                          /* send what waited */
        qpkt_t *q = e->q;
        e->q = q->next;
        eth_header(q->data, e->mac, ifc->mac, ETH_IP);
        eth_send_frame(ifc, q->data, q->len);
        kfree(q);
        arp_queued--;
    }
    e->nq = 0;
}

int arp_list(iface_t *ifc, u32 *ip, u8 (*mac)[6], int max) {
    int n = 0;
    for (int i = 0; i < ARP_MAX && n < max; i++) {
        if (ifc->arp[i].state != ARP_REACHABLE) continue;
        ip[n] = ifc->arp[i].ip;
        memcpy(mac[n], ifc->arp[i].mac, 6);
        n++;
    }
    return n;
}

static void arp_input(iface_t *ifc, const u8 *a, u32 len) {
    nstat.arp_in++;
    if (len < 28 || rd16(a) != 1 || rd16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4) return;
    u16 op = rd16(a + 6);
    if (op != 1 && op != 2) return;
    const u8 *sha = a + 8, *tha = a + 18;
    u32 spa = rdaddr(a + 14), tpa = rdaddr(a + 24);
    (void)tha;
    if (sha[0] & 1) return;                         /* a multicast / broadcast sender: bogus */
    if (!memcmp(sha, ifc->mac, 6)) return;          /* our own (looped back) */
    if (ifc->addr && spa == ifc->addr) {            /* someone else uses our address */
        nstat.arp_conflict++;
        if (!ifc->conflict) kprintf("net: %s: address %s is also used by %02x:%02x:%02x:%02x:%02x:%02x\n", ifc->name,
                                    ipstr(spa), sha[0], sha[1], sha[2], sha[3], sha[4], sha[5]);
        ifc->conflict = 1;
        u64 now = time_ms();
        if (!ifc->defend_at || now - ifc->defend_at > 10000) {   /* defend once per 10 s */
            ifc->defend_at = now;
            arp_announce(ifc);
        }
        dhcp_conflict(ifc);
        return;
    }
    if (ifc->addr && !spa && tpa == ifc->addr && op == 1) {      /* a probe for our address: defend */
        static const u8 zero[6];
        (void)zero;
        arp_send(ifc, 2, sha, 0, ifc->addr, bcast_mac);
        return;
    }
    if (!spa || ip_bad_unicast(spa) || ip_is_loopnet(spa)) return;
    /* RFC 826 merge: update an entry we have; add one only when the
     * packet is for us */
    arp_ent_t *e = arp_find(ifc, spa);
    int for_us = ifc->addr && tpa == ifc->addr;
    if (e) {
        if (e->state == ARP_INCOMPLETE || memcmp(e->mac, sha, 6) || for_us) ent_resolved(ifc, e, sha);
    } else if (for_us && (spa & ifc->mask) == (ifc->addr & ifc->mask)) {
        e = arp_alloc(ifc, spa);
        if (e) ent_resolved(ifc, e, sha);
    }
    if (for_us && op == 1) arp_send(ifc, 2, sha, spa, ifc->addr, sha);
}

/* ---------------- IP over Ethernet ---------------- */

int eth_output(iface_t *ifc, u32 nexthop, int bcast, u8 *pkt, u32 len) {
    if (ifc == lo || !ifc->dev) { lo_output(pkt, len); return 0; }
    u8 *f = pkt - ETH_HDR;
    u32 flen = len + ETH_HDR;
    if (bcast || nexthop == 0xFFFFFFFFu) {
        eth_header(f, bcast_mac, ifc->mac, ETH_IP);
        return eth_send_frame(ifc, f, flen);
    }
    u64 now = time_ms();
    arp_ent_t *e = arp_find(ifc, nexthop);
    if (e && e->state == ARP_REACHABLE) {
        e->used = now;
        u64 age = now - e->confirmed;
        if (age <= ARP_REACHABLE_MS + ARP_STALE_MS) {
            if (age > ARP_REACHABLE_MS && now >= e->next_try) {     /* stale: refresh it (unicast), keep using it */
                e->next_try = now + 1000;
                arp_request(ifc, nexthop, e->mac);
            }
            eth_header(f, e->mac, ifc->mac, ETH_IP);
            return eth_send_frame(ifc, f, flen);
        }
        ent_free(ifc, e, 0);                /* no answer to the refresh: resolve again */
        e = NULL;
    }
    if (!e) {
        if (!ifc->addr) return E_NETUNREACH;
        e = arp_alloc(ifc, nexthop);
        if (!e) { nstat.arp_fail++; return E_NOMEM; }
        e->state = ARP_INCOMPLETE;
        e->tries = 1;
        e->next_try = now + 1000;
        arp_request(ifc, nexthop, NULL);
    }
    /* keep the packet until the answer comes (the oldest goes first) */
    if (e->nq >= ARP_QUEUE || arp_queued >= ARP_QUEUED_MAX) {
        if (!e->q) return E_NOMEM;
        qpkt_t *old = e->q;
        e->q = old->next;
        kfree(old);
        e->nq--;
        arp_queued--;
    }
    qpkt_t *q = kmalloc(sizeof *q + flen);
    if (!q) return E_NOMEM;
    q->next = NULL;
    q->len = flen;
    memcpy(q->data, f, flen);
    qpkt_t **pp = &e->q;
    while (*pp) pp = &(*pp)->next;
    *pp = q;
    e->nq++;
    arp_queued++;
    return 0;
}

void eth_input(iface_t *ifc, const u8 *f, u32 len) {
    if (len < ETH_HDR) return;
    int l2bcast = !memcmp(f, bcast_mac, 6);
    if (!l2bcast && memcmp(f, ifc->mac, 6)) return;     /* (multicast too: none joined) */
    u16 type = rd16(f + 12);
    if (type == ETH_IP) ip_input(ifc, f + ETH_HDR, len - ETH_HDR, l2bcast);
    else if (type == ETH_ARP) arp_input(ifc, f + ETH_HDR, len - ETH_HDR);
}

u64 arp_timer(u64 now) {
    u64 next = ~0ull;
    for (int i = 0; i < nifs; i++) {
        iface_t *ifc = &ifs[i];
        if (!ifc->dev) continue;
        if (ifc->garp_at && now >= ifc->garp_at) {
            static const u8 zero[6];
            ifc->garp_at = 0;
            if (ifc->addr && ifc->up) arp_send(ifc, 1, zero, ifc->addr, ifc->addr, bcast_mac);
        }
        if (ifc->garp_at && ifc->garp_at < next) next = ifc->garp_at;
        for (int k = 0; k < ARP_MAX; k++) {
            arp_ent_t *e = &ifc->arp[k];
            if (e->state == ARP_INCOMPLETE) {
                if (now >= e->next_try) {
                    if (e->tries >= ARP_TRIES || !ifc->up || !ifc->addr) {
                        nstat.arp_fail++;
                        ent_free(ifc, e, 1);        /* "host unreachable" to the senders */
                        continue;
                    }
                    e->tries++;
                    e->next_try = now + 1000;
                    arp_request(ifc, e->ip, NULL);
                }
                if (e->next_try < next) next = e->next_try;
            } else if (e->state == ARP_REACHABLE) {
                u64 gone = MAX(e->used + ARP_UNUSED_MS, e->confirmed + ARP_REACHABLE_MS) + 1;
                if (now >= gone) { ent_free(ifc, e, 0); continue; }
                if (gone < next) next = gone;
            }
        }
    }
    return next;
}
