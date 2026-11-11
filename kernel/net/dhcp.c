/* Halcyon OS - the DHCP client (RFC 2131 / 2132) and static configuration
 * from the settings:
 *
 *   net.eth0.mode     dhcp (default) or static
 *   net.eth0.ip       static address, e.g. 192.168.1.20
 *   net.eth0.mask     static netmask (default: by address class)
 *   net.eth0.gateway  static default gateway (optional)
 *   net.dns           DNS servers for the whole system, comma-separated
 *                     (an address may carry ":port", for tests); empty:
 *                     the servers DHCP (or nothing) provides
 *
 * DHCP: INIT -> DISCOVER -> SELECTING -> REQUEST -> REQUESTING -> ACK ->
 * BOUND; RENEWING at T1 (unicast to the server), REBINDING at T2
 * (broadcast), the address is dropped when the lease expires. A link that
 * comes back starts INIT-REBOOT (asks for the address it had). An ARP
 * conflict right after binding sends DECLINE and starts over. */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/settings.h"

#define BOOTP_LEN    236
#define DHCP_MAGIC   0x63825363u
#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define DECLINE  4
#define ACK      5
#define NAK      6
#define RELEASE  7

typedef struct dhcp {
    iface_t *ifc;
    int state;                  /* NET_DHCP_* */
    u32 xid;
    u64 next;                   /* next action (ms), 0 = none */
    int tries;
    u32 backoff;
    u64 started;                /* this exchange (the "secs" field) */
    u32 offer, server;          /* chosen offer and its server */
    u32 addr, mask, gw;         /* the lease */
    u32 dns[DNS_SERVERS];
    int ndns;
    u32 lease_s;
    u64 lease_start, t1, t2, expire;
    u64 req_time;               /* when the REQUEST that got the lease went out */
    u64 bound_at;
} dhcp_t;

static dhcp_t clients[IF_MAX];
static u32 dns_ovr[DNS_SERVERS];
static u16 dns_ovr_port[DNS_SERVERS];
static int dns_novr;

/* ---------------- DNS server list ---------------- */

void net_dns_override(const u32 *ip, const u16 *port, int n) {
    dns_novr = MIN(n, DNS_SERVERS);
    for (int i = 0; i < dns_novr; i++) { dns_ovr[i] = ip[i]; dns_ovr_port[i] = port[i] ? port[i] : 53; }
}

int net_dns_servers(u32 *ip, u16 *port, int max) {
    int n = 0;
    if (dns_novr) {
        for (int i = 0; i < dns_novr && n < max; i++, n++) { ip[n] = dns_ovr[i]; port[n] = dns_ovr_port[i]; }
        return n;
    }
    for (int k = 0; k < iface_count(); k++) {
        iface_t *f = iface_get(k);
        if (!f->dev || !f->addr) continue;
        for (int i = 0; i < f->ndns && n < max; i++) {
            int dup = 0;
            for (int j = 0; j < n; j++) if (ip[j] == f->dns[i]) dup = 1;
            if (!dup) { ip[n] = f->dns[i]; port[n] = f->dns_port[i] ? f->dns_port[i] : 53; n++; }
        }
    }
    return n;
}

/* "a.b.c.d" -> address (network order); 0 on error */
static int parse_ip(const char *s, u32 *out) {
    u32 v[4];
    for (int i = 0; i < 4; i++) {
        if (!isdigit((u8)*s)) return 0;
        u32 x = 0;
        int digits = 0;
        while (isdigit((u8)*s)) { x = x * 10 + (u32)(*s++ - '0'); if (++digits > 3 || x > 255) return 0; }
        v[i] = x;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s && *s != ':' && *s != ',' && *s != ' ') return 0;
    *out = IP4(v[0], v[1], v[2], v[3]);
    return 1;
}

static void load_dns_setting(void) {
    const char *s = settings_get("net.dns", "");
    u32 ip[DNS_SERVERS]; u16 port[DNS_SERVERS];
    int n = 0;
    while (*s && n < DNS_SERVERS) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        u32 a;
        if (!parse_ip(s, &a)) { kprintf("net: net.dns: cannot read '%s'\n", s); break; }
        while (*s && *s != ':' && *s != ',' && *s != ' ') s++;
        u16 p = 53;
        if (*s == ':') {
            s++;
            long v = strtol(s, NULL, 10);
            if (v > 0 && v < 65536) p = (u16)v;
            while (isdigit((u8)*s)) s++;
        }
        ip[n] = a; port[n] = p; n++;
    }
    net_dns_override(ip, port, n);
}

/* ---------------- messages ---------------- */

static u32 jitter(u32 ms) { return ms + random_u32() % 1000; }

static int dhcp_send(dhcp_t *d, int type, u32 ciaddr, u32 reqip, u32 server, u32 dst) {
    iface_t *ifc = d->ifc;
    u8 m[BOOTP_LEN + 4 + 64];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;
    wr32(m + 4, d->xid);
    u64 secs = (time_ms() - d->started) / 1000;
    wr16(m + 8, (u16)MIN(secs, 65535ull));
    if (!ciaddr) wr16(m + 10, 0x8000);             /* answer by broadcast: we have no address */
    wraddr(m + 12, ciaddr);
    memcpy(m + 28, ifc->mac, 6);
    wr32(m + BOOTP_LEN, DHCP_MAGIC);
    u8 *o = m + BOOTP_LEN + 4;
    *o++ = 53; *o++ = 1; *o++ = (u8)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, ifc->mac, 6); o += 6;      /* client id */
    if (reqip) { *o++ = 50; *o++ = 4; wraddr(o, reqip); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; wraddr(o, server); o += 4; }
    if (type == DISCOVER || type == REQUEST) {
        *o++ = 57; *o++ = 2; wr16(o, 1500); o += 2;                     /* max message size */
        *o++ = 12; *o++ = 7; memcpy(o, "halcyon", 7); o += 7;           /* host name */
        static const u8 want[] = { 1, 3, 6, 15, 26, 51, 54, 58, 59 };
        *o++ = 55; *o++ = sizeof want; memcpy(o, want, sizeof want); o += sizeof want;
    }
    *o++ = 255;
    u32 len = (u32)(o - m);
    if (len < 300) len = 300;                       /* BOOTP minimum (some servers insist) */
    return udp_send_if(dst == 0xFFFFFFFFu ? ifc : NULL, ciaddr, 68, dst, 67, m, len);
}

static void new_exchange(dhcp_t *d) {
    d->xid = random_u32();
    d->started = time_ms();
    d->tries = 0;
    d->backoff = 2000;
}

static void schedule_retry(dhcp_t *d, u64 now) {
    d->next = now + jitter(d->backoff);
    if (d->backoff < 64000) d->backoff *= 2;
}

static void go_init(dhcp_t *d, u64 delay) {
    d->state = NET_DHCP_INIT;
    d->offer = d->server = 0;
    d->next = time_ms() + delay;
    if (!d->next) d->next = 1;
}

static void unconfigure(dhcp_t *d, const char *why) {
    if (d->ifc->addr) iface_set_addr(d->ifc, 0, 0, 0, why);
    d->addr = 0;
    d->ifc->ndns = 0;
}

static void bind_lease(dhcp_t *d, u32 addr, u32 mask, u32 gw, u32 server, u32 lease, u32 t1, u32 t2,
                       const u32 *dns, int ndns) {
    u64 base = d->req_time ? d->req_time : time_ms();   /* the lease runs from the REQUEST (RFC 2131 4.4.1) */
    if (lease == 0xFFFFFFFFu) { t1 = t2 = 0; }
    else {
        if (lease < 10) lease = 10;
        if (!t1 || t1 >= lease) t1 = lease / 2;
        if (!t2 || t2 >= lease || t2 <= t1) t2 = (u32)((u64)lease * 7 / 8);
        if (t2 <= t1) t2 = t1 + 1;
    }
    int fresh = d->addr != addr;
    d->state = NET_DHCP_BOUND;
    d->addr = addr; d->mask = mask; d->gw = gw; d->server = server;
    d->lease_s = lease;
    d->lease_start = base;
    d->t1 = lease == 0xFFFFFFFFu ? 0 : base + (u64)t1 * 1000;
    d->t2 = lease == 0xFFFFFFFFu ? 0 : base + (u64)t2 * 1000;
    d->expire = lease == 0xFFFFFFFFu ? 0 : base + (u64)lease * 1000;
    d->next = d->t1;
    if (fresh) d->bound_at = time_ms();
    iface_t *ifc = d->ifc;
    ifc->ndns = 0;
    for (int i = 0; i < ndns && i < DNS_SERVERS; i++) { ifc->dns[i] = dns[i]; ifc->dns_port[i] = 53; ifc->ndns++; }
    iface_set_addr(ifc, addr, mask, gw, "DHCP");
}

typedef struct {
    int type;
    u32 server, mask, router, lease, t1, t2;
    u32 dns[DNS_SERVERS];
    int ndns;
} dopts_t;

static int parse_options(const u8 *o, u32 n, dopts_t *r) {
    memset(r, 0, sizeof *r);
    while (n) {
        u8 code = o[0];
        if (code == 255) break;
        if (code == 0) { o++; n--; continue; }
        if (n < 2 || o[1] > n - 2) return -1;       /* truncated */
        u8 l = o[1];
        const u8 *v = o + 2;
        switch (code) {
        case 53: if (l == 1) r->type = v[0]; break;
        case 54: if (l == 4) r->server = rdaddr(v); break;
        case 1:  if (l == 4) r->mask = rdaddr(v); break;
        case 3:  if (l >= 4) r->router = rdaddr(v); break;
        case 51: if (l == 4) r->lease = rd32(v); break;
        case 58: if (l == 4) r->t1 = rd32(v); break;
        case 59: if (l == 4) r->t2 = rd32(v); break;
        case 6:
            for (u32 i = 0; i + 4 <= l && r->ndns < DNS_SERVERS; i += 4) {
                u32 a = rdaddr(v + i);
                if (!ip_bad_unicast(a)) r->dns[r->ndns++] = a;
            }
            break;
        }
        o += 2 + l; n -= 2u + l;
    }
    return 0;
}

static int mask_contiguous(u32 m) {
    u32 h = ntoh32(m);
    return h && (~h & (~h + 1)) == 0;
}

void dhcp_input(iface_t *ifc, const ip_meta_t *m, const u8 *p, u32 len) {
    dhcp_t *d = ifc->dhcp;
    if (!d || d->state == NET_DHCP_OFF || d->state == NET_DHCP_BOUND) return;
    if (len < BOOTP_LEN + 4 || p[0] != 2 || p[1] != 1 || p[2] != 6) return;
    if (rd32(p + 4) != d->xid || memcmp(p + 28, ifc->mac, 6) || rd32(p + BOOTP_LEN) != DHCP_MAGIC) return;
    dopts_t o;
    if (parse_options(p + BOOTP_LEN + 4, len - BOOTP_LEN - 4, &o)) return;
    u32 yi = rdaddr(p + 16);
    if (!o.server) o.server = m->src;
    u64 now = time_ms();
    if (o.type == OFFER && d->state == NET_DHCP_SELECTING) {
        if (ip_bad_unicast(yi) || ip_is_loopnet(yi) || ip_bad_unicast(o.server)) return;
        d->offer = yi;
        d->server = o.server;
        d->state = NET_DHCP_REQUESTING;
        d->tries = 0;
        d->backoff = 2000;
        d->req_time = now;
        dhcp_send(d, REQUEST, 0, yi, o.server, 0xFFFFFFFFu);
        schedule_retry(d, now);
        return;
    }
    if (o.type == ACK && (d->state == NET_DHCP_REQUESTING || d->state == NET_DHCP_RENEWING ||
                          d->state == NET_DHCP_REBINDING || d->state == NET_DHCP_REBOOTING)) {
        if (ip_bad_unicast(yi) || ip_is_loopnet(yi)) return;
        if (d->state == NET_DHCP_REQUESTING && (yi != d->offer || o.server != d->server)) return;
        if ((d->state == NET_DHCP_RENEWING || d->state == NET_DHCP_REBINDING) && yi != d->addr) return;
        u32 mask = o.mask && mask_contiguous(o.mask) ? o.mask : 0;
        u32 gw = o.router;
        if (gw && (ip_bad_unicast(gw) || gw == yi)) gw = 0;
        bind_lease(d, yi, mask, gw, o.server, o.lease ? o.lease : 3600, o.t1, o.t2, o.dns, o.ndns);
        return;
    }
    if (o.type == NAK) {
        if (d->state == NET_DHCP_SELECTING) return;
        if (d->state == NET_DHCP_REQUESTING && o.server != d->server) return;
        kprintf("net: %s: DHCP server %s refused (NAK)\n", ifc->name, ipstr(o.server));
        unconfigure(d, "(DHCP NAK)");
        go_init(d, 1000);
    }
}

/* ---------------- timers ---------------- */

static void run(dhcp_t *d, u64 now) {
    iface_t *ifc = d->ifc;
    if (!ifc->up) { d->next = now + 1000; return; }     /* (the link change restarts it) */
    switch (d->state) {
    case NET_DHCP_INIT:
        new_exchange(d);
        d->req_time = 0;
        d->state = NET_DHCP_SELECTING;
        /* fall through */
    case NET_DHCP_SELECTING:
        d->tries++;
        dhcp_send(d, DISCOVER, 0, 0, 0, 0xFFFFFFFFu);
        schedule_retry(d, now);
        break;
    case NET_DHCP_REQUESTING:
        if (++d->tries > 4) { go_init(d, 0); break; }
        dhcp_send(d, REQUEST, 0, d->offer, d->server, 0xFFFFFFFFu);
        schedule_retry(d, now);
        break;
    case NET_DHCP_REBOOTING:
        if (++d->tries > 3) {                       /* nobody answers: keep the lease while it lasts */
            if (d->expire && now < d->expire && ifc->addr) {
                d->state = NET_DHCP_BOUND;
                d->next = d->t1 > now ? d->t1 : now + 1000;
            } else {
                unconfigure(d, "(lease expired)");
                go_init(d, 0);
            }
            break;
        }
        d->req_time = now;
        dhcp_send(d, REQUEST, 0, d->addr, 0, 0xFFFFFFFFu);
        schedule_retry(d, now);
        break;
    case NET_DHCP_BOUND:
        if (!d->t1) { d->next = 0; break; }         /* infinite lease */
        new_exchange(d);
        d->state = NET_DHCP_RENEWING;
        /* fall through */
    case NET_DHCP_RENEWING:
    case NET_DHCP_REBINDING:
        if (d->expire && now >= d->expire) {
            kprintf("net: %s: DHCP lease expired\n", ifc->name);
            unconfigure(d, "(lease expired)");
            go_init(d, 0);
            break;
        }
        if (d->state == NET_DHCP_RENEWING && d->t2 && now >= d->t2) { d->state = NET_DHCP_REBINDING; new_exchange(d); }
        d->req_time = now;
        if (d->state == NET_DHCP_RENEWING) dhcp_send(d, REQUEST, d->addr, 0, 0, d->server);
        else dhcp_send(d, REQUEST, d->addr, 0, 0, 0xFFFFFFFFu);
        /* next try: half the time to the next deadline (RFC 2131 4.4.5), at least 2 s */
        u64 until = d->state == NET_DHCP_RENEWING ? d->t2 : d->expire;
        u64 wait = until > now ? (until - now) / 2 : 1000;
        if (wait < 2000) wait = MIN(2000ull, until > now ? until - now : 1000ull);
        if (!wait) wait = 1;
        d->next = now + wait;
        break;
    }
}

u64 dhcp_timer(u64 now) {
    u64 next = ~0ull;
    for (int i = 0; i < IF_MAX; i++) {
        dhcp_t *d = &clients[i];
        if (!d->ifc || d->state == NET_DHCP_OFF) continue;
        if (d->next && now >= d->next) run(d, now);
        if (d->next && d->next < next) next = d->next;
    }
    return next;
}

/* ---------------- control ---------------- */

void dhcp_start(iface_t *ifc) {
    dhcp_t *d = &clients[ifc->index];
    if (!ifc->dhcp) {
        memset(d, 0, sizeof *d);
        d->ifc = ifc;
        ifc->dhcp = d;
    }
    if (ifc->addr && d->addr != ifc->addr) iface_set_addr(ifc, 0, 0, 0, "(switching to DHCP)");
    d->addr = ifc->addr;
    go_init(d, 0);
    if (d->addr && d->expire && time_ms() < d->expire) { d->state = NET_DHCP_REBOOTING; new_exchange(d); }
}

void dhcp_stop(iface_t *ifc) {
    dhcp_t *d = ifc->dhcp;
    if (!d) return;
    if (d->state == NET_DHCP_BOUND || d->state == NET_DHCP_RENEWING || d->state == NET_DHCP_REBINDING)
        dhcp_send(d, RELEASE, d->addr, 0, d->server, d->server);
    if (ifc->addr && d->addr == ifc->addr) iface_set_addr(ifc, 0, 0, 0, NULL);
    d->state = NET_DHCP_OFF;
    d->addr = 0;
    d->expire = 0;
    d->next = 0;
}

int dhcp_renew(iface_t *ifc) {
    dhcp_t *d = ifc->dhcp;
    if (!d) { dhcp_start(ifc); return 0; }
    if (d->state == NET_DHCP_BOUND || d->state == NET_DHCP_RENEWING || d->state == NET_DHCP_REBINDING) {
        new_exchange(d);
        d->state = NET_DHCP_RENEWING;
        d->next = time_ms();
        if (!d->next) d->next = 1;
        if (d->t2 && d->t2 <= time_ms()) d->t2 = time_ms() + 1;
        return 0;
    }
    if (d->state == NET_DHCP_OFF || d->state == NET_DHCP_INIT) go_init(d, 0);
    return 0;
}

int dhcp_release(iface_t *ifc) {
    dhcp_t *d = ifc->dhcp;
    if (!d || !d->addr) return E_NOTCONN;
    dhcp_send(d, RELEASE, d->addr, 0, d->server, d->server);
    kprintf("net: %s: DHCP lease released\n", ifc->name);
    unconfigure(d, "(released)");
    d->state = NET_DHCP_OFF;
    d->expire = 0;
    d->next = 0;
    return 0;
}

int dhcp_state(iface_t *ifc, u32 *server, i64 *left_s, i64 *lease_s) {
    dhcp_t *d = ifc->dhcp;
    *server = 0; *left_s = -1; *lease_s = -1;
    if (!d || ifc->mode != IFMODE_DHCP) return NET_DHCP_OFF;
    if (d->addr) {
        *server = d->server;
        *lease_s = d->lease_s == 0xFFFFFFFFu ? -1 : (i64)d->lease_s;
        u64 now = time_ms();
        *left_s = !d->expire ? -1 : d->expire > now ? (i64)((d->expire - now) / 1000) : 0;
    }
    return d->state;
}

void dhcp_link(iface_t *ifc) {
    dhcp_t *d = ifc->dhcp;
    if (!d || ifc->mode != IFMODE_DHCP || d->state == NET_DHCP_OFF) {
        if (ifc->up && ifc->addr) arp_announce(ifc);
        return;
    }
    if (!ifc->up) return;
    if (d->addr && d->expire && time_ms() < d->expire) {
        d->state = NET_DHCP_REBOOTING;              /* is the address still ours on this network? */
        new_exchange(d);
        d->next = time_ms() + 1;
    } else {
        if (d->addr) unconfigure(d, "(lease expired)");
        go_init(d, 0);
    }
}

void dhcp_conflict(iface_t *ifc) {
    dhcp_t *d = ifc->dhcp;
    if (!d || d->state != NET_DHCP_BOUND || !d->addr || time_ms() - d->bound_at > 10000) return;
    kprintf("net: %s: %s is in use: DHCP DECLINE\n", ifc->name, ipstr(d->addr));
    u32 a = d->addr;
    unconfigure(d, "(address conflict)");
    dhcp_send(d, DECLINE, 0, a, d->server, 0xFFFFFFFFu);
    d->expire = 0;
    go_init(d, 10000);                              /* RFC 2131 3.1.5: wait at least 10 s */
}

/* ---------------- configuration from the settings ---------------- */

void net_config_init(void) {
    load_dns_setting();
    for (int i = 0; i < iface_count(); i++) {
        iface_t *f = iface_get(i);
        if (!f->dev) continue;
        char key[40];
        snprintf(key, sizeof key, "net.%s.mode", f->name);
        const char *mode = settings_get(key, "dhcp");
        if (!strcasecmp(mode, "static")) {
            u32 ip = 0, mask = 0, gw = 0;
            snprintf(key, sizeof key, "net.%s.ip", f->name);
            int ok = parse_ip(settings_get(key, ""), &ip) && !ip_bad_unicast(ip) && !ip_is_loopnet(ip);
            snprintf(key, sizeof key, "net.%s.mask", f->name);
            const char *ms = settings_get(key, "");
            if (*ms && (!parse_ip(ms, &mask) || !mask_contiguous(mask))) ok = 0;
            snprintf(key, sizeof key, "net.%s.gateway", f->name);
            const char *gs = settings_get(key, "");
            if (*gs && !parse_ip(gs, &gw)) ok = 0;
            if (ok) {
                f->mode = IFMODE_STATIC;
                iface_set_addr(f, ip, mask, gw, "static");
                continue;
            }
            kprintf("net: %s: the static configuration in the settings is not valid: using DHCP\n", f->name);
        } else if (strcasecmp(mode, "dhcp")) {
            kprintf("net: %s: unknown mode '%s': using DHCP\n", f->name, mode);
        }
        f->mode = IFMODE_DHCP;
        dhcp_start(f);
    }
}
