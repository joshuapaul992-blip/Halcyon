/* Halcyon OS - IPv4 (RFC 791, 1122 host requirements, 815 reassembly,
 * 1191 path MTU) and ICMP (RFC 792, 1812 4.3.2.8 rate limiting). */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"

#define P_ICMP 1
#define P_TCP  6
#define P_UDP  17

/* ---------------- checksums and helpers ---------------- */

u32 csum_add(u32 sum, const void *data, u32 len) {
    const u8 *p = data;
    u64 s = sum;
    while (len >= 8) {                      /* 32-bit words, big-endian pairs */
        u32 a, b;
        __builtin_memcpy(&a, p, 4);
        __builtin_memcpy(&b, p + 4, 4);
        s += (u64)a + b;
        p += 8; len -= 8;
    }
    while (len >= 2) { u16 w; __builtin_memcpy(&w, p, 2); s += w; p += 2; len -= 2; }
    if (len) { u16 w = 0; *(u8 *)&w = *p; s += w; }
    while (s >> 32) s = (s & 0xFFFFFFFFu) + (s >> 32);
    return (u32)s;
}

/* the sum is kept in memory order; folding gives the 16 bits to store as
 * they are (little-endian host: the bytes come out right) */
u16 csum_fold(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

u32 csum_pseudo(u32 src, u32 dst, u8 proto, u32 len) {
    u8 ph[12];
    wraddr(ph, src); wraddr(ph + 4, dst);
    ph[8] = 0; ph[9] = proto; wr16(ph + 10, (u16)len);
    return csum_add(0, ph, 12);
}

const char *ipstr(u32 a) {
    static char buf[4][16];
    static int slot;
    char *b = buf[slot++ & 3];
    const u8 *p = (const u8 *)&a;
    snprintf(b, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

/* ---------------- path MTU (RFC 1191) ---------------- */

typedef struct { u32 dst; u32 mtu; u64 expire; } pmtu_t;
static pmtu_t pmtus[PMTU_MAX];

u32 pmtu_get(u32 dst) {
    u64 now = time_ms();
    for (int i = 0; i < PMTU_MAX; i++)
        if (pmtus[i].mtu && pmtus[i].dst == dst) {
            if (now >= pmtus[i].expire) { pmtus[i].mtu = 0; return 0; }
            return pmtus[i].mtu;
        }
    return 0;
}

static void pmtu_set(u32 dst, u32 mtu) {
    pmtu_t *slot = NULL;
    for (int i = 0; i < PMTU_MAX; i++) {
        if (pmtus[i].mtu && pmtus[i].dst == dst) { slot = &pmtus[i]; break; }
        if (!slot || !pmtus[i].mtu || (slot->mtu && pmtus[i].expire < slot->expire)) slot = &pmtus[i];
    }
    slot->dst = dst;
    slot->mtu = mtu;
    slot->expire = time_ms() + PMTU_EXPIRE_MS;
}

u32 ip_route_mtu(u32 dst) {
    route_res_t r;
    if (route_lookup(dst, &r)) return 0;
    u32 m = r.ifc->mtu, p = pmtu_get(dst);
    return p && p < m ? p : m;
}

/* ---------------- output ---------------- */

static u16 ip_id;

static void ip_header(u8 *h, u32 totlen, u16 id, u16 frag, int ttl, int tos, u8 proto, u32 src, u32 dst) {
    h[0] = 0x45; h[1] = (u8)tos;
    wr16(h + 2, (u16)totlen);
    wr16(h + 4, id);
    wr16(h + 6, frag);
    h[8] = (u8)ttl; h[9] = proto;
    h[10] = h[11] = 0;
    wraddr(h + 12, src); wraddr(h + 16, dst);
    u16 c = csum_fold(csum_add(0, h, IP_HDR));
    __builtin_memcpy(h + 10, &c, 2);
}

int ip_output(u8 *buf, u32 off, u32 len, u32 src, u32 dst, u8 proto, int ttl, int tos, int flags, iface_t *force) {
    if (off < ETH_HDR + IP_HDR || len > IP_MAX_PAYLOAD) return E_MSGSIZE;
    route_res_t r;
    if (force) {
        memset(&r, 0, sizeof r);
        r.ifc = force; r.nexthop = dst; r.src = force->addr;
        r.bcast = ip_is_bcast(force, dst);
    } else if (route_lookup(dst, &r)) {
        nstat.ip_noroute++;
        return E_NETUNREACH;
    }
    if (!src) src = r.src;
    if (ttl <= 0) ttl = IP_DEFAULT_TTL;
    u32 mtu = r.ifc->mtu;
    if (r.ifc->dev) { u32 p = pmtu_get(dst); if (p && p < mtu) mtu = p; }
    u8 *h = buf + off - IP_HDR;
    nstat.ip_out++;
    net_poke();                         /* (from a system call: the net thread looks at its timers again) */
    if (len + IP_HDR <= mtu) {
        ip_header(h, len + IP_HDR, ip_id++, (flags & IPF_DF) ? 0x4000 : 0, ttl, tos, proto, src, dst);
        return eth_output(r.ifc, r.nexthop, r.bcast, h, len + IP_HDR);
    }
    if (flags & IPF_DF) return E_MSGSIZE;
    /* fragments of (mtu - 20) & ~7 bytes, each built in its own buffer */
    u32 chunk = (mtu - IP_HDR) & ~7u;
    if (chunk < 8) return E_MSGSIZE;
    u8 *fb = kmalloc(ETH_HDR + IP_HDR + chunk);
    if (!fb) return E_NOMEM;
    u16 id = ip_id++;
    int res = 0;
    for (u32 o = 0; o < len; o += chunk) {
        u32 n = MIN(chunk, len - o);
        u8 *fh = fb + ETH_HDR;
        memcpy(fh + IP_HDR, buf + off + o, n);
        ip_header(fh, n + IP_HDR, id, (u16)((o >> 3) | (o + n < len ? 0x2000 : 0)), ttl, tos, proto, src, dst);
        nstat.ip_frag_out++;
        int e = eth_output(r.ifc, r.nexthop, r.bcast, fh, n + IP_HDR);
        if (e && !res) res = e;
    }
    kfree(fb);
    return res;
}

/* ---------------- reassembly (RFC 815, overlaps refused) ---------------- */

typedef struct frag { struct frag *next; u32 off, len; u8 data[]; } frag_t;
typedef struct reasm {
    int used;
    u32 src, dst;
    u16 id;
    u8 proto;
    u64 expire;
    frag_t *frags;              /* sorted by offset */
    int nfrags;
    u32 total;                  /* payload length once the last fragment came (0: not yet) */
    u32 have;
    u32 mem;
    u8 hdr[60];                 /* the first fragment's header */
    u32 hlen;
    int have_first;
    iface_t *ifc;
} reasm_t;
static reasm_t rs[REASM_MAX];
static u32 reasm_mem;

static void reasm_free(reasm_t *r) {
    while (r->frags) { frag_t *f = r->frags; r->frags = f->next; kfree(f); }
    reasm_mem -= r->mem;
    memset(r, 0, sizeof *r);
}

static void reasm_fail(reasm_t *r) { nstat.ip_reasm_fail++; reasm_free(r); }

/* add a fragment; returns the whole datagram (header + payload, kmalloc'ed)
 * when it is complete */
static u8 *reasm_add(iface_t *ifc, const u8 *h, u32 hl, const u8 *payload, u32 plen, u32 off, int more, u32 *outlen) {
    u32 src = rdaddr(h + 12), dst = rdaddr(h + 16);
    u16 id = rd16(h + 4);
    u8 proto = h[9];
    u64 now = time_ms();
    nstat.ip_frag_in++;
    if (off + plen > IP_MAX_PAYLOAD || (more && (plen & 7)) || (!plen && more)) { nstat.ip_reasm_fail++; return NULL; }
    reasm_t *r = NULL, *freeslot = NULL, *oldest = NULL;
    for (int i = 0; i < REASM_MAX; i++) {
        if (!rs[i].used) { if (!freeslot) freeslot = &rs[i]; continue; }
        if (rs[i].src == src && rs[i].dst == dst && rs[i].id == id && rs[i].proto == proto) { r = &rs[i]; break; }
        if (!oldest || rs[i].expire < oldest->expire) oldest = &rs[i];
    }
    if (!r) {
        if (!freeslot) { reasm_fail(oldest); freeslot = oldest; }
        r = freeslot;
        r->used = 1;
        r->src = src; r->dst = dst; r->id = id; r->proto = proto;
        r->expire = now + REASM_TIMEOUT_MS;
        r->ifc = ifc;
    }
    if (!more) {
        u32 t = off + plen;
        if ((r->total && r->total != t) || r->have > t) { reasm_fail(r); return NULL; }
        for (frag_t *f = r->frags; f; f = f->next) if (f->off + f->len > t) { reasm_fail(r); return NULL; }
        r->total = t;
    } else if (r->total && off + plen > r->total) { reasm_fail(r); return NULL; }
    /* find the place; an exact duplicate is ignored, any other overlap
     * ends the datagram (RFC 5722's rule, applied to IPv4 too) */
    frag_t **pp = &r->frags;
    while (*pp && (*pp)->off + (*pp)->len <= off) {
        if ((*pp)->off == off && (*pp)->len == plen) break;
        pp = &(*pp)->next;
    }
    if (*pp && (*pp)->off == off && (*pp)->len == plen) {
        if (memcmp((*pp)->data, payload, plen)) reasm_fail(r);
        return NULL;
    }
    if (*pp && (*pp)->off < off + plen) { reasm_fail(r); return NULL; }
    if (!plen) return NULL;
    u32 sz = (u32)sizeof(frag_t) + plen;
    if (r->nfrags >= REASM_FRAGS || reasm_mem + sz > REASM_MEM) { reasm_fail(r); return NULL; }
    frag_t *f = kmalloc(sz);
    if (!f) { reasm_fail(r); return NULL; }
    f->off = off; f->len = plen;
    memcpy(f->data, payload, plen);
    f->next = *pp;
    *pp = f;
    r->nfrags++;
    r->mem += sz;
    reasm_mem += sz;
    r->have += plen;
    if (off == 0) { memcpy(r->hdr, h, hl); r->hlen = hl; r->have_first = 1; }
    if (!r->total || r->have != r->total || !r->have_first) return NULL;
    /* complete: header + payload in one buffer */
    u32 tot = r->hlen + r->total;
    u8 *d = kmalloc(tot + ETH_HDR);
    if (!d) { reasm_fail(r); return NULL; }
    u8 *ip = d + ETH_HDR;
    memcpy(ip, r->hdr, r->hlen);
    for (frag_t *q = r->frags; q; q = q->next) memcpy(ip + r->hlen + q->off, q->data, q->len);
    wr16(ip + 2, (u16)MIN(tot, 65535u));
    wr16(ip + 6, 0);
    *outlen = tot;
    nstat.ip_reasm_ok++;
    reasm_free(r);
    return d;
}

u64 ip_timer(u64 now) {
    u64 next = ~0ull;
    for (int i = 0; i < REASM_MAX; i++) {
        reasm_t *r = &rs[i];
        if (!r->used) continue;
        if (now >= r->expire) {
            if (r->have_first) {            /* "fragment reassembly time exceeded" */
                ip_meta_t m = { r->ifc, r->src, r->dst, r->hdr, r->hlen, r->hlen, 0, 0, 0 };
                u8 tmp[68];
                u32 n = r->hlen;
                memcpy(tmp, r->hdr, n);
                if (r->frags && r->frags->off == 0) { u32 k = MIN(8u, r->frags->len); memcpy(tmp + n, r->frags->data, k); n += k; }
                m.hdr = tmp; m.totlen = n;
                icmp_error(&m, 11, 1, 0);
            }
            reasm_fail(r);
            continue;
        }
        if (r->expire < next) next = r->expire;
    }
    return next;
}

/* ---------------- input ---------------- */

static void ip_deliver(ip_meta_t *m, const u8 *p, u32 len) {
    switch (m->hdr[9]) {
    case P_ICMP: icmp_input(m, p, len); break;
    case P_UDP: udp_input(m, p, len); break;
    case P_TCP: if (!m->bcast) tcp_input(m, p, len); break;
    default:
        nstat.ip_noproto++;
        icmp_error(m, 3, 2, 0);             /* protocol unreachable */
        break;
    }
}

void ip_input(iface_t *ifc, const u8 *p, u32 len, int l2bcast) {
    nstat.ip_in++;
    if (len < IP_HDR || (p[0] >> 4) != 4) { nstat.ip_bad++; return; }
    u32 hl = (u32)(p[0] & 15) * 4;
    u32 tot = rd16(p + 2);
    if (hl < IP_HDR || tot < hl || tot > len) { nstat.ip_bad++; return; }
    if (ifc->dev && csum_fold(csum_add(0, p, hl))) { nstat.ip_bad++; return; }
    u32 src = rdaddr(p + 12), dst = rdaddr(p + 16);
    /* martians: a source that cannot send, 127/8 from the wire */
    if (src == 0xFFFFFFFFu || ip_is_mcast(src) || (ifc->dev && (ip_is_loopnet(src) || ip_is_loopnet(dst)))) { nstat.ip_bad++; return; }
    if (ifc->dev && ifc->addr && src == ifc->addr) { nstat.ip_bad++; return; }   /* our own address from outside */
    int bcast = ip_is_bcast(ifc, dst);
    int ours = bcast || (ifc->dev ? (ifc->addr && dst == ifc->addr) || (dst != 0 && ip_is_local(dst) && !ip_is_loopnet(dst)) : 1);
    /* not configured yet: DHCP answers may come to the offered address */
    if (!ours && ifc->dev && !ifc->addr && p[9] == P_UDP) ours = 1;
    if (!ours || ip_is_mcast(dst)) { nstat.ip_notours++; return; }
    if (!src && !(p[9] == P_UDP && bcast)) { nstat.ip_bad++; return; }
    ip_meta_t m = { ifc, src, dst, p, hl, tot, bcast || l2bcast, p[8], p[1] };
    u16 frag = rd16(p + 6);
    if (frag & 0x8000) { nstat.ip_bad++; return; }        /* reserved bit */
    if (frag & 0x3FFF) {
        u32 outlen = 0;
        u8 *d = reasm_add(ifc, p, hl, p + hl, tot - hl, (u32)(frag & 0x1FFF) * 8, !!(frag & 0x2000), &outlen);
        if (!d) return;
        const u8 *ip = d + ETH_HDR;
        m.hdr = ip; m.totlen = outlen;
        ip_deliver(&m, ip + hl, outlen - hl);
        kfree(d);
        return;
    }
    ip_deliver(&m, p + hl, tot - hl);
}

/* ---------------- ICMP ---------------- */

/* errors we send: a token bucket, 10 per second, bursts of 20 */
static u64 icmp_tokens_ms;
static int icmp_tokens = 20;

static int icmp_allow(void) {
    u64 now = time_ms();
    if (now - icmp_tokens_ms >= 100) {
        int add = (int)MIN((now - icmp_tokens_ms) / 100, 20ull);
        icmp_tokens = MIN(20, icmp_tokens + add);
        icmp_tokens_ms = now;
    }
    if (icmp_tokens <= 0) { nstat.icmp_ratelimited++; return 0; }
    icmp_tokens--;
    return 1;
}

static void icmp_send(u32 src, u32 dst, u8 *buf, u32 len, int tos) {
    /* buf + HDRROOM holds the message; the checksum field is filled here */
    u8 *msg = buf + HDRROOM;
    msg[2] = msg[3] = 0;
    u16 c = csum_fold(csum_add(0, msg, len));
    __builtin_memcpy(msg + 2, &c, 2);
    nstat.icmp_out++;
    ip_output(buf, HDRROOM, len, src, dst, P_ICMP, 0, tos, 0, NULL);
}

void icmp_error(const ip_meta_t *m, u8 type, u8 code, u32 extra) {
    const u8 *h = m->hdr;
    if (m->bcast || ip_bad_unicast(m->src) || (ip_is_local(m->src) && m->ifc->dev)) return;
    if (rd16(h + 6) & 0x1FFF) return;                    /* only about first fragments */
    if (h[9] == P_ICMP) {                                /* never about an error (only about queries) */
        if (m->totlen < m->hlen + 1) return;
        u8 t = h[m->hlen];
        if (t != 8 && t != 13 && t != 15 && t != 17) return;
    }
    if (!icmp_allow()) return;
    u32 n = MIN(m->totlen, 548u);                        /* RFC 1812: as much as fits in 576 */
    u8 buf[HDRROOM + 8 + 548];
    u8 *msg = buf + HDRROOM;
    msg[0] = type; msg[1] = code;
    wr32(msg + 4, extra);
    memcpy(msg + 8, h, n);
    nstat.icmp_err_out++;
    icmp_send(ip_is_local(m->dst) && !ip_is_bcast(m->ifc, m->dst) ? m->dst : 0, m->src, buf, 8 + n, 0);
}

int icmp_err_to_errno(u8 type, u8 code) {
    if (type == 11) return E_HOSTUNREACH;               /* time exceeded */
    if (type == 12) return E_INVAL;                     /* parameter problem */
    if (type != 3) return 0;
    switch (code) {
    case 0: case 6: case 11: return E_NETUNREACH;
    case 2: case 3: return E_CONNREFUSED;
    case 4: return E_MSGSIZE;
    default: return E_HOSTUNREACH;
    }
}

void ip_deliver_error(const u8 *orig, u32 len, int err, u8 type, u8 code, u32 mtu, int local) {
    if (len < IP_HDR || (orig[0] >> 4) != 4) return;
    u32 hl = (u32)(orig[0] & 15) * 4;
    if (hl < IP_HDR || len < hl + 8) return;
    if (!ip_is_local(rdaddr(orig + 12))) return;        /* it must be from us */
    if (rd16(orig + 6) & 0x1FFF) return;
    switch (orig[9]) {
    case P_TCP: tcp_err(orig, len, err, type, code, mtu, local); break;
    case P_UDP: udp_err(orig, len, err, type, code); break;
    case P_ICMP: if (orig[hl] == 8) ping_err(orig, len, err); break;
    }
}

void icmp_input(const ip_meta_t *m, const u8 *p, u32 len) {
    nstat.icmp_in++;
    if (len < 8 || csum_fold(csum_add(0, p, len))) { nstat.icmp_bad++; return; }
    u8 type = p[0], code = p[1];
    switch (type) {
    case 8: {                                           /* echo request -> reply */
        if (m->bcast || code) return;                   /* no "smurf" amplification */
        if (len > IP_MAX_PAYLOAD) return;
        u8 *buf = kmalloc(HDRROOM + len);
        if (!buf) return;
        memcpy(buf + HDRROOM, p, len);
        buf[HDRROOM] = 0;
        icmp_send(m->dst, m->src, buf, len, m->tos);
        kfree(buf);
        return;
    }
    case 0:
        ping_input(m, p, len);
        return;
    case 3: case 11: case 12: {                         /* errors about something we sent */
        nstat.icmp_err_in++;
        if (type == 3 && code > 15) return;
        u32 mtu = 0;
        if (type == 3 && code == 4) {
            mtu = rd16(p + 6);
            const u8 *o = p + 8;
            if (len < 8 + IP_HDR + 8) return;
            u32 omtu = rd16(o + 2);
            if (mtu < 68 || mtu >= omtu) {              /* old routers send 0: plateaus (RFC 1191) */
                static const u16 plateau[] = { 32000, 17914, 8166, 4352, 2002, 1492, 1006, 508, 296, 68 };
                mtu = 68;
                for (u32 i = 0; i < ARRAY_SIZE(plateau); i++) if (plateau[i] < omtu) { mtu = plateau[i]; break; }
            }
            if (mtu < 576) mtu = 576;                   /* (refuse to go below: RFC 1191's caution) */
            u32 odst = rdaddr(o + 16);
            u32 cur = pmtu_get(odst);
            route_res_t r;
            if (!route_lookup(odst, &r) && mtu < r.ifc->mtu && (!cur || mtu < cur)) pmtu_set(odst, mtu);
        }
        ip_deliver_error(p + 8, len - 8, icmp_err_to_errno(type, code), type, code, mtu, 0);
        return;
    }
    default:                                            /* redirects, source quench, timestamps: ignored */
        return;
    }
}
