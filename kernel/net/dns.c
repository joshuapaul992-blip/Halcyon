/* Halcyon OS - the DNS resolver (RFC 1035; RFC 2308 negative caching;
 * RFC 5452 defences against forged answers).
 *
 * A lookup asks the configured servers for the A records of a name: one
 * server at a time (1 s, then 2 s, then 4 s per attempt, going round the
 * servers) until the caller's deadline. Every attempt has a new random
 * query id and a new random source port; an answer counts only if it comes
 * from the server and port the query went to, to that source port, with
 * that id and the same question. CNAME chains are followed (within the
 * answer, and with new queries for a target the answer did not resolve, at
 * most 8 links). Answers are cached for their TTL (the smallest along the
 * chain, at most a day); "no such name" is cached for the SOA's negative
 * TTL (at most 5 minutes). "localhost" and dotted quads never reach the
 * network. Concurrent lookups of one name share one query. */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"

#define DNS_PORT     53
#define DNS_ATT_OPEN 4              /* attempts whose answers are still accepted */
#define DNS_CHAIN    8
#define DNS_TTL_MAX  86400
#define DNS_NEG_MAX  300
#define NAME_MAX_LEN 253

typedef struct dnsc {
    char name[NAME_MAX_LEN + 2];
    u32 addrs[DNS_MAX_ADDRS];
    int n;                          /* 0: negative entry (no such name) */
    u64 expire, used;
} dnsc_t;

typedef struct datt {               /* one attempt: one query packet, one port */
    dnsq_t *q;
    upcb_t *u;
    u16 id;
    u32 server;
    u16 sport;
} datt_t;

struct dnsq {
    dnsq_t *next;
    int refs;
    char name[NAME_MAX_LEN + 2];    /* what was asked (lower case) */
    char cur[NAME_MAX_LEN + 2];     /* what is being asked now (a CNAME target) */
    int chain;
    u64 deadline, next_try;
    int done, result;
    u32 addrs[DNS_MAX_ADDRS];
    int n;
    u32 ttl;
    int attempt;                    /* attempts sent for cur */
    int errors;                     /* servers that answered with an error */
    datt_t att[DNS_ATT_OPEN];
};

static dnsc_t cache[DNS_CACHE_MAX];
static dnsq_t *pending;
static int npending;
static u64 st_queries, st_hits, st_rejected;

/* ---------------- names ---------------- */

static void lower(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) d[i] = (char)tolower((u8)s[i]);
    d[i] = 0;
    if (i && d[i - 1] == '.') d[i - 1] = 0;        /* "example.com." is "example.com" */
}

int dns_name_ok(const char *name) {
    size_t len = strlen(name);
    if (len && name[len - 1] == '.') len--;
    if (!len || len > NAME_MAX_LEN) return 0;
    size_t lab = 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '.') { if (!lab) return 0; lab = 0; continue; }
        if (!isalnum((u8)c) && c != '-' && c != '_') return 0;
        if (++lab > 63) return 0;
    }
    return lab > 0;
}

static int quad(const char *s, u32 *out) {
    u32 v[4];
    for (int i = 0; i < 4; i++) {
        if (!isdigit((u8)*s)) return 0;
        u32 x = 0;
        int d = 0;
        while (isdigit((u8)*s)) { x = x * 10 + (u32)(*s++ - '0'); if (++d > 3 || x > 255) return 0; }
        v[i] = x;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    *out = IP4(v[0], v[1], v[2], v[3]);
    return 1;
}

int dns_literal(const char *name, u32 *out) {
    char n[NAME_MAX_LEN + 2];
    lower(n, name, sizeof n);
    size_t l = strlen(n);
    if (!strcmp(n, "localhost") || (l > 10 && !strcmp(n + l - 10, ".localhost"))) { *out = IP4(127, 0, 0, 1); return 1; }
    return quad(n, out);
}

/* a (possibly compressed) name at off; *end = offset after it in place */
static int read_name(const u8 *m, u32 len, u32 off, char *out, u32 *end) {
    u32 o = off, outl = 0;
    int jumped = 0, hops = 0;
    for (;;) {
        if (o >= len) return -1;
        u8 l = m[o];
        if ((l & 0xC0) == 0xC0) {
            if (o + 1 >= len || ++hops > 32) return -1;
            u32 ptr = (u32)(l & 0x3F) << 8 | m[o + 1];
            if (ptr >= o) return -1;                /* pointers go back: no loops */
            if (!jumped) *end = o + 2;
            jumped = 1;
            o = ptr;
            continue;
        }
        if (l & 0xC0) return -1;
        if (!l) { if (!jumped) *end = o + 1; break; }
        if (o + 1 + l > len || outl + l + 1 > NAME_MAX_LEN) return -1;
        if (outl) out[outl++] = '.';
        for (u32 i = 0; i < l; i++) {
            u8 c = m[o + 1 + i];
            if (c == '.' || c < 0x21 || c > 0x7E) return -1;
            out[outl++] = (char)tolower(c);
        }
        o += 1 + l;
    }
    out[outl] = 0;
    return 0;
}

/* ---------------- cache ---------------- */

static dnsc_t *cache_find(const char *n) {
    u64 now = time_ms();
    for (int i = 0; i < DNS_CACHE_MAX; i++) {
        if (!cache[i].name[0]) continue;
        if (now >= cache[i].expire) { cache[i].name[0] = 0; continue; }
        if (!strcmp(cache[i].name, n)) return &cache[i];
    }
    return NULL;
}

static void cache_put(const char *n, const u32 *a, int cnt, u32 ttl) {
    if (!ttl) return;
    dnsc_t *e = cache_find(n);
    if (!e) {
        u64 now = time_ms();
        for (int i = 0; i < DNS_CACHE_MAX; i++) {
            dnsc_t *c = &cache[i];
            if (!c->name[0] || now >= c->expire) { e = c; break; }
            if (!e || c->used < e->used) e = c;     /* least recently used */
        }
    }
    strlcpy(e->name, n, sizeof e->name);
    e->n = MIN(cnt, DNS_MAX_ADDRS);
    for (int i = 0; i < e->n; i++) e->addrs[i] = a[i];
    e->expire = time_ms() + (u64)ttl * 1000;
    e->used = time_ms();
}

int dns_cached(const char *name, u32 *out, int max) {
    char n[NAME_MAX_LEN + 2];
    lower(n, name, sizeof n);
    dnsc_t *e = cache_find(n);
    if (!e) return 0;
    st_hits++;
    e->used = time_ms();
    if (!e->n) return E_NOENT;
    int k = MIN(e->n, max);
    for (int i = 0; i < k; i++) out[i] = e->addrs[i];
    return k;
}

void dns_flush(void) { memset(cache, 0, sizeof cache); }

void dns_counts(u32 *cached, u64 *queries, u64 *hits, u64 *rejected) {
    u32 n = 0;
    u64 now = time_ms();
    for (int i = 0; i < DNS_CACHE_MAX; i++) if (cache[i].name[0] && now < cache[i].expire) n++;
    *cached = n; *queries = st_queries; *hits = st_hits; *rejected = st_rejected;
}

/* ---------------- queries ---------------- */

static void att_close(datt_t *a) {
    if (a->u) udp_kclose(a->u);
    memset(a, 0, sizeof *a);
}

static void q_finish(dnsq_t *q, int result) {
    q->done = 1;
    q->result = result;
    for (int i = 0; i < DNS_ATT_OPEN; i++) att_close(&q->att[i]);
}

static void q_unlink_free(dnsq_t *q) {
    for (dnsq_t **pp = &pending; *pp; pp = &(*pp)->next) if (*pp == q) { *pp = q->next; npending--; break; }
    for (int i = 0; i < DNS_ATT_OPEN; i++) att_close(&q->att[i]);
    kfree(q);
}

static void dns_answer(void *ctx, const ip_meta_t *m, u16 sport, const u8 *p, u32 len);

static u32 build_query(u8 *b, u16 id, const char *name) {
    memset(b, 0, 12);
    wr16(b, id);
    wr16(b + 2, 0x0100);                            /* recursion desired */
    wr16(b + 4, 1);
    u32 o = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        u32 l = dot ? (u32)(dot - s) : (u32)strlen(s);
        b[o++] = (u8)l;
        memcpy(b + o, s, l);
        o += l;
        s += l;
        if (*s == '.') s++;
    }
    b[o++] = 0;
    wr16(b + o, 1); wr16(b + o + 2, 1);             /* type A, class IN */
    return o + 4;
}

/* send the next attempt for q->cur */
static void q_send(dnsq_t *q, u64 now) {
    u32 srv[DNS_SERVERS]; u16 port[DNS_SERVERS];
    int ns = net_dns_servers(srv, port, DNS_SERVERS);
    if (!ns) { q_finish(q, E_NODEV); return; }
    int k = q->attempt++;
    int si = k % ns;
    u32 tmo = 1000u << MIN(k / ns, 2);
    /* the oldest attempt's port closes when a fifth one opens */
    datt_t *a = &q->att[k % DNS_ATT_OPEN];
    att_close(a);
    a->q = q;
    a->server = srv[si];
    a->sport = port[si];
    a->id = (u16)random_u32();
    a->u = udp_kopen(0, dns_answer, a);
    q->next_try = now + tmo;
    if (q->next_try > q->deadline) q->next_try = q->deadline;
    if (!a->u) return;
    u8 b[NAME_MAX_LEN + 2 + 16 + 12];
    u32 l = build_query(b, a->id, q->cur);
    st_queries++;
    udp_ksend(a->u, a->server, a->sport, b, l);
}

static void q_restart(dnsq_t *q, const char *name) {
    for (int i = 0; i < DNS_ATT_OPEN; i++) att_close(&q->att[i]);
    strlcpy(q->cur, name, sizeof q->cur);
    q->attempt = 0;
    q->errors = 0;
    q_send(q, time_ms());
}

static void q_result(dnsq_t *q, const u32 *addrs, int n, u32 ttl) {
    q->n = MIN(n, DNS_MAX_ADDRS);
    for (int i = 0; i < q->n; i++) q->addrs[i] = addrs[i];
    cache_put(q->name, addrs, q->n, MIN(ttl, (u32)DNS_TTL_MAX));
    q_finish(q, q->n);
}

/* the negative TTL from an SOA in the authority section */
static u32 neg_ttl(const u8 *p, u32 len, u32 off, int an, int ns) {
    char nm[NAME_MAX_LEN + 2];
    for (int i = 0; i < an + ns; i++) {
        u32 e;
        if (read_name(p, len, off, nm, &e) || e + 10 > len) return 0;
        u16 type = rd16(p + e);
        u32 ttl = rd32(p + e + 4);
        u16 rdl = rd16(p + e + 8);
        u32 rd = e + 10;
        if (rd + rdl > len) return 0;
        if (i >= an && type == 6) {                 /* SOA: mname, rname, 5 numbers */
            u32 x;
            if (read_name(p, len, rd, nm, &x) || read_name(p, len, x, nm, &x) || x + 20 > rd + rdl) return 0;
            u32 minimum = rd32(p + x + 16);
            return MIN(MIN(ttl, minimum), (u32)DNS_NEG_MAX);
        }
        off = rd + rdl;
    }
    return 0;
}

#define DNS_RR_MAX 64
static void dns_answer(void *ctx, const ip_meta_t *m, u16 sport, const u8 *p, u32 len) {
    datt_t *a = ctx;
    dnsq_t *q = a->q;
    /* is it the answer to this attempt? */
    if (!q || q->done || m->src != a->server || sport != a->sport || len < 12 || rd16(p) != a->id) { st_rejected++; return; }
    u16 fl = rd16(p + 2);
    if (!(fl & 0x8000) || ((fl >> 11) & 15) || rd16(p + 4) != 1) { st_rejected++; return; }
    char nm[NAME_MAX_LEN + 2];
    u32 off;
    if (read_name(p, len, 12, nm, &off) || off + 4 > len || strcmp(nm, q->cur) || rd16(p + off) != 1 || rd16(p + off + 2) != 1) {
        st_rejected++;
        return;
    }
    off += 4;
    int rcode = fl & 15;
    int an = rd16(p + 6), ns = rd16(p + 8);
    att_close(a);                                   /* (a is done; q stays) */
    if (rcode == 3) {                               /* no such name */
        cache_put(q->name, NULL, 0, neg_ttl(p, len, off, an, ns));
        q_finish(q, E_NOENT);
        return;
    }
    if (rcode != 0) {                               /* server failure, refused...: the next server */
        u32 srv[DNS_SERVERS]; u16 port[DNS_SERVERS];
        int nsrv = net_dns_servers(srv, port, DNS_SERVERS);
        if (++q->errors >= MAX(nsrv, 1)) { q_finish(q, E_IO); return; }
        q->next_try = time_ms();
        return;
    }
    /* collect the answer records (static: the net thread only, no recursion) */
    static char owners[DNS_RR_MAX][NAME_MAX_LEN + 2], targets[DNS_RR_MAX][NAME_MAX_LEN + 2];
    static u16 types[DNS_RR_MAX];
    static u32 ttls[DNS_RR_MAX], addr[DNS_RR_MAX];
    int nrr = 0;
    u32 o = off;
    for (int i = 0; i < an && nrr < DNS_RR_MAX; i++) {
        u32 e;
        if (read_name(p, len, o, owners[nrr], &e) || e + 10 > len) break;
        u16 type = rd16(p + e), cls = rd16(p + e + 2);
        u32 ttl = rd32(p + e + 4) & 0x7FFFFFFF;
        u16 rdl = rd16(p + e + 8);
        u32 rd = e + 10;
        if (rd + rdl > len) break;
        o = rd + rdl;
        if (cls != 1) continue;
        if (type == 1 && rdl == 4) {
            addr[nrr] = rdaddr(p + rd);
            if (ip_bad_unicast(addr[nrr])) continue;
        } else if (type == 5) {
            u32 x;
            if (read_name(p, len, rd, targets[nrr], &x)) continue;
        } else continue;
        types[nrr] = type;
        ttls[nrr] = ttl;
        nrr++;
    }
    /* follow the chain from cur */
    char want[NAME_MAX_LEN + 2];
    strlcpy(want, q->cur, sizeof want);
    u32 ttl = q->ttl ? q->ttl : 0xFFFFFFFFu;
    u32 out[DNS_MAX_ADDRS];
    int n = 0, links = 0, moved = 0;
    for (;;) {
        for (int i = 0; i < nrr; i++) {
            if (types[i] != 1 || strcmp(owners[i], want)) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) if (out[k] == addr[i]) dup = 1;
            if (dup || n >= DNS_MAX_ADDRS) continue;
            out[n++] = addr[i];
            ttl = MIN(ttl, ttls[i]);
        }
        if (n) break;
        int found = -1;
        for (int i = 0; i < nrr; i++) if (types[i] == 5 && !strcmp(owners[i], want)) { found = i; break; }
        if (found < 0 || q->chain + ++links > DNS_CHAIN) break;
        strlcpy(want, targets[found], sizeof want);
        ttl = MIN(ttl, ttls[found]);
        moved = 1;
    }
    if (n) { q_result(q, out, n, ttl); return; }
    if (moved && q->chain + links <= DNS_CHAIN && (fl & 0x0200) == 0) {   /* a CNAME to resolve on its own */
        q->chain += links;
        q->ttl = ttl;
        q_restart(q, want);
        return;
    }
    if (fl & 0x0200) {                              /* truncated, no usable answer (no TCP fallback) */
        q->next_try = time_ms();
        if (++q->errors >= 2) q_finish(q, E_IO);
        return;
    }
    cache_put(q->name, NULL, 0, neg_ttl(p, len, off, an, ns));   /* the name exists, but has no address */
    q_finish(q, E_NOENT);
}

dnsq_t *dns_start(const char *name, u64 deadline_ms, int *err) {
    char n[NAME_MAX_LEN + 2];
    lower(n, name, sizeof n);
    *err = 0;
    for (dnsq_t *q = pending; q; q = q->next) {
        if (q->done || strcmp(q->name, n)) continue;
        q->refs++;
        if (deadline_ms > q->deadline) q->deadline = deadline_ms;
        return q;
    }
    u32 srv[DNS_SERVERS]; u16 port[DNS_SERVERS];
    if (!net_dns_servers(srv, port, DNS_SERVERS)) { *err = E_NODEV; return NULL; }
    if (npending >= DNS_PENDING_MAX) { *err = E_BUSY; return NULL; }
    dnsq_t *q = kzalloc(sizeof *q);
    if (!q) { *err = E_NOMEM; return NULL; }
    q->refs = 1;
    strlcpy(q->name, n, sizeof q->name);
    strlcpy(q->cur, n, sizeof q->cur);
    q->deadline = deadline_ms;
    q->next = pending;
    pending = q;
    npending++;
    q_send(q, time_ms());
    return q;
}

int dns_pending(dnsq_t *q) { return !q->done; }

int dns_result(dnsq_t *q, u32 *out, int max) {
    if (!q->done) return E_AGAIN;
    if (q->result <= 0) return q->result ? q->result : E_NOENT;
    int k = MIN(q->n, max);
    for (int i = 0; i < k; i++) out[i] = q->addrs[i];
    return k;
}

void dns_put(dnsq_t *q) {
    if (--q->refs <= 0 && q->done) q_unlink_free(q);
}

u64 dns_timer(u64 now) {
    u64 next = ~0ull;
    dnsq_t *nx;
    for (dnsq_t *q = pending; q; q = nx) {
        nx = q->next;
        if (q->done) {
            if (q->refs <= 0) q_unlink_free(q);
            continue;
        }
        if (now >= q->deadline) { q_finish(q, q->errors ? E_IO : E_TIMEOUT); if (q->refs <= 0) q_unlink_free(q); continue; }
        if (now >= q->next_try) q_send(q, now);
        if (q->done) continue;
        if (q->next_try < next) next = q->next_try;
    }
    return next;
}
