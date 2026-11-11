/* Halcyon OS - UDP (RFC 768) sockets, ping sockets (socket(AF_INET,
 * SOCK_DGRAM, IPPROTO_ICMP) as on Linux: echo requests out, echo replies
 * in, the socket's port is the echo identifier), and kernel endpoints for
 * the resolver and the DHCP client. */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"

#define UDP_HDR 8
#define UDP_MAX (IP_MAX_PAYLOAD - UDP_HDR)
#define UDP_QLEN 128                /* datagrams queued per socket */

struct upcb {
    upcb_t *next;
    sock_t *s;                      /* NULL: a kernel endpoint */
    ukin_t kin;
    void *kctx;
    int ping;
    u32 laddr, raddr;               /* bound address (0 = any), peer */
    u32 saddr;                      /* source chosen at connect() */
    u16 lport, rport;
    u8 bound, connected, bcast_ok, rd_shut, wr_shut;
    int ttl, tos;
    nqueue_t rq;
};

static upcb_t *pcbs;
static int npcbs;

int udp_count(void) { return npcbs; }
#define U(s) ((upcb_t *)(s)->priv)

static void link_pcb(upcb_t *u) { u->next = pcbs; pcbs = u; npcbs++; }
static void unlink_pcb(upcb_t *u) {
    for (upcb_t **pp = &pcbs; *pp; pp = &(*pp)->next) if (*pp == u) { *pp = u->next; npcbs--; return; }
}

/* is (addr, port) taken for this kind of socket? */
static int port_busy(int ping, u32 addr, u16 port, const upcb_t *self, int reuse) {
    for (upcb_t *u = pcbs; u; u = u->next) {
        if (u == self || !u->bound || u->ping != ping || u->lport != port) continue;
        if (u->laddr && addr && u->laddr != addr) continue;
        if (reuse && u->s && u->s->reuseaddr) continue;     /* both asked for it */
        return 1;
    }
    return 0;
}

static int autobind(upcb_t *u, int reuse) {
    u16 p = port_random();
    for (int i = 0; i <= PORT_EPH_HI - PORT_EPH_LO; i++) {
        if (!port_busy(u->ping, u->laddr, p, u, 0)) { u->lport = p; u->bound = 1; return 0; }
        p = p == PORT_EPH_HI ? PORT_EPH_LO : p + 1;
    }
    (void)reuse;
    return E_ADDRINUSE;
}

/* ---------------- socket operations ---------------- */

int udp_create(sock_t *s, int ping) {
    upcb_t *u = kzalloc(sizeof *u);
    if (!u) return E_NOMEM;
    u->s = s;
    u->ping = ping;
    u->rq.owner = s;
    s->priv = u;
    s->ops = &udp_ops;
    link_pcb(u);
    return 0;
}

static void u_release(sock_t *s) {
    upcb_t *u = U(s);
    unlink_pcb(u);
    nq_free(&u->rq);
    kfree(u);
    s->priv = NULL;
}

static int u_bind(sock_t *s, const struct sockaddr *a, u32 len) {
    upcb_t *u = U(s);
    u32 addr; u16 port;
    int r = sin_parse(a, len, &addr, &port);
    if (r) return r;
    if (u->bound) return E_INVAL;
    if (addr && !ip_is_local(addr) && addr != 0xFFFFFFFFu) return E_ADDRNOTAVAIL;
    u->laddr = addr;
    if (!port) return autobind(u, 0);
    if (port_busy(u->ping, addr, port, u, s->reuseaddr)) return E_ADDRINUSE;
    u->lport = port;
    u->bound = 1;
    return 0;
}

static int u_connect(sock_t *s, const struct sockaddr *a, u32 len) {
    upcb_t *u = U(s);
    if (len >= sizeof(sa_family_t) && a->sa_family == AF_UNSPEC) {
        u->connected = 0; u->raddr = 0; u->rport = 0; u->saddr = 0;
        sock_wake(s);
        return 0;
    }
    u32 addr; u16 port;
    int r = sin_parse(a, len, &addr, &port);
    if (r) return r;
    if (!addr) addr = IP4(127, 0, 0, 1);
    if (!port && !u->ping) return E_INVAL;
    if (ip_is_bcast(NULL, addr) && !u->bcast_ok) return E_PERM;
    route_res_t rt;
    if (route_lookup(addr, &rt)) return E_NETUNREACH;
    if (!u->bound && (r = autobind(u, 0))) return r;
    u->raddr = addr;
    u->rport = port;
    u->saddr = u->laddr ? u->laddr : rt.src;
    u->connected = 1;
    s->error = 0;
    sock_wake(s);
    return 0;
}

static int u_getname(sock_t *s, int peer, struct sockaddr *a, u32 *len) {
    upcb_t *u = U(s);
    if (peer) {
        if (!u->connected) return E_NOTCONN;
        *len = sin_make(a, *len, u->raddr, u->rport);
    } else {
        *len = sin_make(a, *len, u->laddr ? u->laddr : u->connected ? u->saddr : 0, u->lport);
    }
    return 0;
}

static int u_shutdown(sock_t *s, int how) {
    upcb_t *u = U(s);
    if (!u->connected) return E_NOTCONN;
    if (how != SHUT_WR) { u->rd_shut = 1; nq_free(&u->rq); }
    if (how != SHUT_RD) u->wr_shut = 1;
    sock_wake(s);
    return 0;
}

/* build and send one datagram: data from a uio (sockets) or memory */
static int u_output(upcb_t *u, iface_t *force, u32 src, u32 dst, u16 dport, uio_t *uio, const void *data, u32 len) {
    u32 hl = u->ping ? 0 : UDP_HDR;
    u8 *buf = kmalloc(HDRROOM + hl + len);
    if (!buf) return E_NOMEM;
    u8 *h = buf + HDRROOM;
    if (uio) {
        if (uio_in(uio, h + hl, len)) { kfree(buf); return E_INVAL; }
    } else {
        memcpy(h + hl, data, len);
    }
    if (!src) {
        if (force) src = force->addr;
        else {
            route_res_t rt;
            if (route_lookup(dst, &rt)) { kfree(buf); nstat.ip_noroute++; return E_NETUNREACH; }
            src = rt.src;
        }
    }
    int r;
    if (u->ping) {
        wr16(h + 4, u->lport);                      /* our identifier */
        h[2] = h[3] = 0;
        u16 c = csum_fold(csum_add(0, h, len));
        __builtin_memcpy(h + 2, &c, 2);
        nstat.icmp_out++;
        r = ip_output(buf, HDRROOM, len, src, dst, 1, u->ttl, u->tos, 0, force);
    } else {
        wr16(h, u->lport);
        wr16(h + 2, dport);
        wr16(h + 4, (u16)(hl + len));
        h[6] = h[7] = 0;
        u16 c = csum_fold(csum_add(csum_pseudo(src, dst, 17, hl + len), h, hl + len));
        if (!c) c = 0xFFFF;
        __builtin_memcpy(h + 6, &c, 2);
        nstat.udp_out++;
        r = ip_output(buf, HDRROOM, hl + len, src, dst, 17, u->ttl, u->tos, 0, force);
    }
    kfree(buf);
    return r;
}

static long u_send(sock_t *s, uio_t *uio, int flags, const struct sockaddr *to, u32 tolen) {
    upcb_t *u = U(s);
    if (u->wr_shut) return E_PIPE;
    u32 dst; u16 dport;
    if (to) {
        int r = sin_parse(to, tolen, &dst, &dport);
        if (r) return r;
        if (!dst) dst = IP4(127, 0, 0, 1);
        if (!dport && !u->ping) return E_INVAL;
    } else {
        if (!u->connected) return E_NOTCONN;
        dst = u->raddr; dport = u->rport;
    }
    if (s->error && u->connected && !to) { int e = s->error; s->error = 0; return e; }
    u64 len = uio_left(uio);
    if (len > UDP_MAX || (u->ping && len + IP_HDR > IP_MAX_PAYLOAD)) return E_MSGSIZE;
    if (u->ping) {                                  /* an echo request: type 8, code 0, at least the header */
        u8 h[2];
        if (len < 8) return E_INVAL;
        uio_t peek = *uio;
        if (uio_in(&peek, h, 2)) return E_INVAL;
        if (h[0] != 8 || h[1] != 0) return E_INVAL;
    }
    int bc = dst == 0xFFFFFFFFu;
    route_res_t rt;
    if (!route_lookup(dst, &rt) && rt.bcast) bc = 1;
    if (bc && !u->bcast_ok) return E_PERM;
    if (ip_is_mcast(dst)) return E_NETUNREACH;
    if (!u->bound) {
        int r = autobind(u, 0);
        if (r) return r;
    }
    u32 src = u->laddr && u->laddr != 0xFFFFFFFFu ? u->laddr : (u->connected && !to ? u->saddr : 0);
    int r = u_output(u, NULL, src, dst, dport, uio, NULL, (u32)len);
    if (r == E_NOMEM) return E_NOMEM;
    if (r && r != E_NOMEM) return r;
    return (long)len;
}

static long u_recv(sock_t *s, uio_t *uio, int flags, struct sockaddr *from, u32 *fromlen) {
    upcb_t *u = U(s);
    if (u->rq.count) {
        u64 before = uio->done;
        long r = nq_get_msg(&u->rq, uio, flags & MSG_PEEK, from, fromlen);
        if (r < 0) return r;
        return (flags & MSG_TRUNC) ? r : (long)(uio->done - before);
    }
    if (fromlen) *fromlen = 0;
    if (s->error) { int e = s->error; s->error = 0; return e; }
    if (u->rd_shut) return 0;
    return E_AGAIN;
}

static int u_poll(sock_t *s) {
    upcb_t *u = U(s);
    int m = POLLOUT;
    if (s->error) m |= POLLERR;
    if (u->rq.count || u->rd_shut) m |= POLLIN;
    if (u->rd_shut) m |= POLLRDHUP;
    if (u->wr_shut) m &= ~POLLOUT;
    return m;
}

static int u_ioctl(sock_t *s, long req, u64 *arg) {
    if (req != FIONREAD) return E_NOTSUP;
    *arg = nq_next_msg_len(&U(s)->rq);
    return 0;
}

static int u_setopt(sock_t *s, int level, int name, const void *v, u32 len) {
    upcb_t *u = U(s);
    if (level == SOL_SOCKET && name == SO_BROADCAST) {
        if (len < sizeof(int)) return E_INVAL;
        u->bcast_ok = !!*(const int *)v;
        return 0;
    }
    if (level == IPPROTO_IP) return inet_ipopt_set(&u->ttl, &u->tos, name, v, len);
    return E_NOTSUP;
}

static int u_getopt(sock_t *s, int level, int name, void *v, u32 *len) {
    upcb_t *u = U(s);
    if (level == SOL_SOCKET && name == SO_BROADCAST) { *(int *)v = u->bcast_ok; *len = sizeof(int); return 0; }
    if (level == IPPROTO_IP) return inet_ipopt_get(u->ttl, u->tos, name, v, len);
    return E_NOTSUP;
}

static int u_listen(sock_t *s, int backlog) { return E_NOTSUP; }
static int u_accept(sock_t *s, sock_t **out) { return E_NOTSUP; }

const sock_ops_t udp_ops = {
    .release = u_release, .bind = u_bind, .listen = u_listen, .accept = u_accept, .connect = u_connect,
    .send = u_send, .recv = u_recv, .shutdown = u_shutdown, .getname = u_getname, .poll = u_poll,
    .ioctl = u_ioctl, .setopt = u_setopt, .getopt = u_getopt,
};

/* ---------------- receiving ---------------- */

/* queue a datagram at a socket (message limits as for local sockets) */
static void u_queue(upcb_t *u, const ip_meta_t *m, u16 sport, const u8 *data, u32 len) {
    sock_t *s = u->s;
    if (u->rd_shut || u->rq.count >= UDP_QLEN || u->rq.bytes + len > s->rcvbuf) { nstat.udp_drop++; return; }
    struct sockaddr_in from;
    u32 alen = sin_make((struct sockaddr *)&from, sizeof from, m->src, sport);
    uio_t k = { (u64)(uptr)data, len, 0, 1 };
    if (nq_put_msg(&u->rq, &k, &from, alen)) { nstat.udp_drop++; return; }
    sock_wake(s);
}

/* the best match: connected > bound to the address > bound to any */
static upcb_t *u_lookup(int ping, u32 laddr, u16 lport, u32 raddr, u16 rport) {
    upcb_t *best = NULL;
    int bestscore = -1;
    for (upcb_t *u = pcbs; u; u = u->next) {
        if (u->ping != ping || !u->bound || u->lport != lport) continue;
        int score = 0;
        if (u->laddr) { if (u->laddr != laddr) continue; score += 1; }
        if (u->connected) {
            if (u->raddr != raddr || (!ping && u->rport != rport)) continue;
            score += 2;
        }
        if (score > bestscore) { best = u; bestscore = score; }
    }
    return best;
}

void udp_input(const ip_meta_t *m, const u8 *p, u32 len) {
    nstat.udp_in++;
    if (len < UDP_HDR) { nstat.udp_bad++; return; }
    u32 ulen = rd16(p + 4);
    if (ulen < UDP_HDR || ulen > len) { nstat.udp_bad++; return; }
    if ((p[6] | p[7]) && m->ifc->dev && csum_fold(csum_add(csum_pseudo(m->src, m->dst, 17, ulen), p, ulen))) {
        nstat.udp_bad++;
        return;
    }
    u16 sport = rd16(p), dport = rd16(p + 2);
    const u8 *data = p + UDP_HDR;
    u32 dlen = ulen - UDP_HDR;
    if (dport == 68 && sport == 67 && m->ifc->dev) { dhcp_input(m->ifc, m, data, dlen); return; }
    if (!m->ifc->addr && m->ifc->dev) return;           /* (unconfigured: DHCP only) */
    if (m->bcast) {                                     /* every socket bound to the port */
        int any = 0;
        for (upcb_t *u = pcbs; u; u = u->next) {    /* (kernel endpoints take no broadcasts) */
            if (u->ping || !u->bound || !u->s || u->lport != dport) continue;
            if (u->laddr && u->laddr != m->dst && u->laddr != 0xFFFFFFFFu) continue;
            if (u->connected && (u->raddr != m->src || u->rport != sport)) continue;
            any = 1;
            u_queue(u, m, sport, data, dlen);
        }
        if (!any) nstat.udp_noport++;
        return;
    }
    upcb_t *u = u_lookup(0, m->dst, dport, m->src, sport);
    if (!u) {
        nstat.udp_noport++;
        icmp_error(m, 3, 3, 0);                         /* port unreachable */
        return;
    }
    if (u->s) u_queue(u, m, sport, data, dlen);
    else u->kin(u->kctx, m, sport, data, dlen);     /* (may close u: nothing touches it after this) */
}

/* an ICMP error about a datagram we sent: connected sockets learn of it */
void udp_err(const u8 *orig, u32 len, int err, u8 type, u8 code) {
    u32 hl = (u32)(orig[0] & 15) * 4;
    const u8 *uh = orig + hl;
    u32 src = rdaddr(orig + 12), dst = rdaddr(orig + 16);
    upcb_t *u = u_lookup(0, src, rd16(uh), dst, rd16(uh + 2));
    if (!u || !u->connected || !u->s || !err || err == E_MSGSIZE) return;
    if (u->raddr != dst || u->rport != rd16(uh + 2)) return;
    u->s->error = err;
    sock_wake(u->s);
}

void ping_input(const ip_meta_t *m, const u8 *p, u32 len) {
    if (m->bcast) return;
    upcb_t *u = u_lookup(1, m->dst, rd16(p + 4), m->src, 0);
    if (!u || !u->s) return;
    u_queue(u, m, 0, p, len);
}

void ping_err(const u8 *orig, u32 len, int err) {
    u32 hl = (u32)(orig[0] & 15) * 4;
    upcb_t *u = u_lookup(1, rdaddr(orig + 12), rd16(orig + hl + 4), rdaddr(orig + 16), 0);
    if (!u || !u->s || !err) return;
    u->s->error = err;
    sock_wake(u->s);
}

/* ---------------- kernel endpoints ---------------- */

upcb_t *udp_kopen(u16 port, ukin_t fn, void *ctx) {
    upcb_t *u = kzalloc(sizeof *u);
    if (!u) return NULL;
    u->kin = fn;
    u->kctx = ctx;
    if (port) {
        if (port_busy(0, 0, port, u, 0)) { kfree(u); return NULL; }
        u->lport = port; u->bound = 1;
    } else if (autobind(u, 0)) { kfree(u); return NULL; }
    link_pcb(u);
    return u;
}

u16 udp_kport(upcb_t *u) { return u->lport; }

int udp_ksend(upcb_t *u, u32 dst, u16 dport, const void *data, u32 len) {
    if (len > UDP_MAX) return E_MSGSIZE;
    return u_output(u, NULL, 0, dst, dport, NULL, data, len);
}

void udp_kclose(upcb_t *u) {
    if (!u) return;
    unlink_pcb(u);
    kfree(u);
}

int udp_send_if(iface_t *ifc, u32 src, u16 sport, u32 dst, u16 dport, const void *data, u32 len) {
    upcb_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.lport = sport;
    return u_output(&tmp, ifc, src ? src : 0, dst, dport, NULL, data, len) ;
}

/* SYS_NET_CONNS */
int udp_conns(hal_netconn_t *out, int max) {
    int n = 0;
    for (upcb_t *u = pcbs; u && n < max; u = u->next) {
        if (!u->s) continue;
        hal_netconn_t *c = &out[n++];
        memset(c, 0, sizeof *c);
        c->proto = u->ping ? NETC_ICMP : NETC_UDP;
        c->laddr = u->laddr ? u->laddr : u->connected ? u->saddr : 0;
        c->lport = u->lport;
        c->raddr = u->connected ? u->raddr : 0;
        c->rport = u->connected ? u->rport : 0;
        c->rxq = u->rq.bytes;
        c->pid = net_acct_pid(u->s->acct);
    }
    return n;
}
