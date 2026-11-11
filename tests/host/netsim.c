/* Network stack simulator: several copies of kernel/net/ (each a shared
 * object built from the stack and net_shim.c: one simulated computer) on
 * one simulated Ethernet that can lose, duplicate, reorder, delay, corrupt
 * and rate-limit frames, plus a "router" written independently here (ARP,
 * ICMP echo, a DHCP server, two DNS servers, ICMP "fragmentation needed"
 * for a smaller path MTU). Time is simulated (milliseconds): minutes of
 * timeouts pass in a moment, and every run is reproducible (-s SEED).
 *
 *   netsim STACK.so [-v] [-s SEED] [-f] [-seeds DIR] [test ...]
 *       -f      also the slow tests (long transfers under heavy impairment)
 *       -seeds  write every frame the tests send into DIR (fuzzer seeds)
 *
 * Every test ends by closing everything, letting TIME_WAIT pass and
 * checking that each stack is back to the memory it had at start. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include "netsim.h"
#define timeval hal_timeval            /* (libc has its own) */
#include "../../kernel/net/sockabi.h"
#undef timeval

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;

/* error codes (kernel/core/types.h) */
#define E_IO -1
#define E_NOENT -3
#define E_INVAL -8
#define E_TIMEOUT -9
#define E_AGAIN -12
#define E_NODEV -15
#define E_PERM -19
#define E_ADDRINUSE -20
#define E_NOTCONN -21
#define E_CONNREFUSED -23
#define E_CONNRESET -24
#define E_PIPE -25
#define E_INPROGRESS -29
#define E_HOSTUNREACH -30
#define SYS_CLOSE 21
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int verbose, full;
static u64 now;
static u64 rng = 88172645463325252ull;
static int checks, failures;
static const char *cur_test;
static const char *so_path;
static char tmpdir[256];

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s line %d: ", cur_test, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int chance(int per_mille) { return per_mille > 0 && (int)(rnd() % 1000) < per_mille; }

static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 rda(const u8 *p) { u32 a; memcpy(&a, p, 4); return a; }
static void wra(u8 *p, u32 a) { memcpy(p, &a, 4); }
static u16 hs(u16 v) { return (u16)(v >> 8 | v << 8); }
#define IP(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)   /* network order */
static const char *ips(u32 a) {
    static char b[8][16];
    static int k;
    char *s = b[k++ & 7];
    snprintf(s, 16, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
    return s;
}

/* one's complement checksum, independent of the stack's */
static u16 cksum(const u8 *p, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2) sum += (u32)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (u32)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}
static u32 pseudo(u32 s, u32 d, u8 proto, u32 len) {
    u8 ph[12];
    wra(ph, s); wra(ph + 4, d); ph[8] = 0; ph[9] = proto; wr16(ph + 10, (u16)len);
    u32 sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (u32)(ph[i] << 8 | ph[i + 1]);
    return sum;
}

/* ================= nodes and the wire ================= */

#define MAXN 6
typedef struct node {
    void *dl;
    sim_init_fn init;
    sim_setting_fn setting;
    sim_start_fn start;
    sim_rx_fn rx;
    sim_poll_fn poll;
    sim_link_fn link;
    sim_call_fn call;
    sim_dns_start_fn dns_start;
    sim_dns_result_fn dns_result;
    sim_dns_put_fn dns_put;
    sim_allocs_fn allocs;
    sim_netmem_fn netmem;
    u8 mac[6];
    u32 ip;
    u64 next;
    long base_allocs;
    int up;
} node_t;

static node_t nodes[MAXN];
static int nnodes;                  /* stacks are 1..nnodes; 0 is the router */
static const u8 router_mac[6] = { 2, 0, 0, 0, 0, 1 };
#define ROUTER IP(10, 0, 0, 1)
#define DNS2 IP(10, 0, 0, 53)
#define NODEIP(i) IP(10, 0, 0, 1 + (i))
static sim_host_t host_cfg;

typedef struct ev { u64 t; int dst; u32 len; u8 *data; u64 seq; } ev_t;
static ev_t *heap;
static int nheap, capheap;
static u64 evseq;

typedef struct wire {
    int loss, dup, reorder, corrupt;    /* per mille */
    int delay_ms, reorder_ms;
    int bw;                             /* bytes per ms (0: unlimited) */
    int mtu;                            /* DF packets above it get ICMP "fragmentation needed" from the router */
    int cut[MAXN][MAXN];                /* drop everything from i to j */
    int (*filter)(int src, int dst, u8 *frame, u32 *len);   /* see / change / drop (return 0) */
} wire_t;
static wire_t wire;
static u64 busy_until[MAXN];         /* microseconds */

static int ev_before(const ev_t *a, const ev_t *b) { return a->t < b->t || (a->t == b->t && a->seq < b->seq); }

static void heap_push(ev_t e) {
    if (nheap == capheap) { capheap = capheap ? capheap * 2 : 1024; heap = realloc(heap, sizeof *heap * (size_t)capheap); }
    e.seq = evseq++;
    int i = nheap++;
    while (i) {
        int p = (i - 1) / 2;
        if (!ev_before(&e, &heap[p])) break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i] = e;
}

static ev_t heap_pop(void) {
    ev_t top = heap[0], last = heap[--nheap];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= nheap) break;
        if (c + 1 < nheap && ev_before(&heap[c + 1], &heap[c])) c++;
        if (!ev_before(&heap[c], &last)) break;
        heap[i] = heap[c];
        i = c;
    }
    if (nheap) heap[i] = last;
    return top;
}

static int node_of_mac(const u8 *mac) {
    if (!memcmp(mac, router_mac, 6)) return 0;
    for (int i = 1; i <= nnodes; i++) if (!memcmp(mac, nodes[i].mac, 6)) return i;
    return -1;
}

static void router_toobig(const u8 *frame, u32 len);
static char *seeddir;
static int seedn;

static void seed_frame(const u8 *f, u32 len) {
    if (seedn >= 3000 || len < 34) return;
    if (rnd() % 8) return;                          /* a sample */
    char p[600];
    snprintf(p, sizeof p, "%s/frame%04d", seeddir, seedn++);
    FILE *o = fopen(p, "wb");
    if (!o) return;
    u8 hdr[3] = { 1, (u8)(len >> 8), (u8)len };      /* fuzz_net.c's record format */
    fwrite(hdr, 1, 3, o);
    fwrite(f, 1, len, o);
    fclose(o);
}

static void wire_send(int src, const u8 *frame, u32 len) {
    if (len < 14) return;
    int bc = frame[0] & 1;
    if (seeddir && src) seed_frame(frame, len);
    if (wire.mtu && src != 0 && rd16(frame + 12) == 0x0800 && len - 14 > (u32)wire.mtu && (frame[14 + 6] & 0x40)) {
        router_toobig(frame, len);
        return;
    }
    for (int dst = 0; dst <= nnodes; dst++) {
        if (dst == src) continue;
        if (!bc && node_of_mac(frame) != dst) continue;
        if (dst && !nodes[dst].up) continue;
        u8 *f = malloc(len);
        memcpy(f, frame, len);
        u32 l = len;
        if (wire.filter && !wire.filter(src, dst, f, &l)) { free(f); continue; }
        if (wire.cut[src][dst]) { free(f); continue; }
        if (chance(wire.loss)) { free(f); continue; }
        if (chance(wire.corrupt) && l > 14) f[14 + rnd() % (l - 14)] ^= (u8)(1 + rnd() % 255);
        u64 t = now;
        if (wire.bw) {
            u64 start = busy_until[src] > now * 1000 ? busy_until[src] : now * 1000;
            busy_until[src] = start + (u64)l * 1000 / (u64)wire.bw;
            t = (busy_until[src] + 999) / 1000;
        }
        t += (u64)wire.delay_ms;
        if (chance(wire.reorder)) t += 1 + rnd() % (u64)(wire.reorder_ms ? wire.reorder_ms : 5);
        int copies = chance(wire.dup) ? 2 : 1;
        for (int k = 0; k < copies; k++) {
            u8 *c = f;
            if (k) { c = malloc(l); memcpy(c, f, l); }
            heap_push((ev_t){ t + (u64)k, dst, l, c, 0 });
        }
    }
}

static void host_xmit(int node, const void *frame, unsigned len) { wire_send(node, frame, len); }

/* a frame straight to a node (bypassing the wire's impairments) */
static void inject(int dst, const u8 *f, u32 len) {
    u8 *c = malloc(len);
    memcpy(c, f, len);
    heap_push((ev_t){ now, dst, len, c, 0 });
}

static void router_rx(const u8 *f, u32 len);

static void poll_node(int i) {
    for (int k = 0; k < 64; k++) {
        nodes[i].next = nodes[i].poll();
        if (nodes[i].next > now) break;
    }
}

static void deliver(ev_t e) {
    if (e.dst == 0) router_rx(e.data, e.len);
    else if (nodes[e.dst].up) { nodes[e.dst].rx(e.data, e.len); poll_node(e.dst); }
    free(e.data);
}

/* run until cond(arg) holds (checked after every step) or max_ms pass */
static int run_until(int (*cond)(void *), void *arg, u64 max_ms) {
    u64 limit = now + max_ms;
    for (;;) {
        while (nheap && heap[0].t <= now) deliver(heap_pop());
        for (int i = 1; i <= nnodes; i++) if (nodes[i].up && nodes[i].next <= now) poll_node(i);
        if (cond && cond(arg)) return 1;
        u64 t = ~0ull;
        if (nheap) t = heap[0].t;
        for (int i = 1; i <= nnodes; i++) if (nodes[i].up && nodes[i].next < t) t = nodes[i].next;
        if (t > limit) { now = limit; return cond ? cond(arg) : 1; }
        if (t > now) now = t;
    }
}

static void run_for(u64 ms) { run_until(NULL, NULL, ms); }

/* ================= the stacks ================= */

static void *sym(void *dl, const char *n) {
    void *p = dlsym(dl, n);
    if (!p) { fprintf(stderr, "netsim: %s: %s\n", n, dlerror()); exit(2); }
    return p;
}

static void ipset(int i, const char *key, u32 ip) {
    char b[32];
    snprintf(b, sizeof b, "%s", ips(ip));
    nodes[i].setting(key, b);
}

/* a fresh copy of the stack as node i: a static address (10.0.0.(i+1)), or DHCP */
static void add_node(int i, int dhcp) {
    char path[600];
    snprintf(path, sizeof path, "%s/stack%d.so", tmpdir, i);
    unlink(path);
    {   /* a real copy: the dynamic loader would take a hard link for the object it already has */
        FILE *in = fopen(so_path, "rb"), *out = fopen(path, "wb");
        if (!in || !out) { fprintf(stderr, "netsim: cannot copy %s to %s\n", so_path, path); exit(2); }
        static char buf[1 << 16];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
        fclose(in);
        fclose(out);
    }
    node_t *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!n->dl) { fprintf(stderr, "netsim: %s\n", dlerror()); exit(2); }
    unlink(path);
    n->init = (sim_init_fn)sym(n->dl, "sim_init");
    n->setting = (sim_setting_fn)sym(n->dl, "sim_setting");
    n->start = (sim_start_fn)sym(n->dl, "sim_start");
    n->rx = (sim_rx_fn)sym(n->dl, "sim_rx");
    n->poll = (sim_poll_fn)sym(n->dl, "sim_poll");
    n->link = (sim_link_fn)sym(n->dl, "sim_link");
    n->call = (sim_call_fn)sym(n->dl, "sim_call");
    n->dns_start = (sim_dns_start_fn)sym(n->dl, "sim_dns_start");
    n->dns_result = (sim_dns_result_fn)sym(n->dl, "sim_dns_result");
    n->dns_put = (sim_dns_put_fn)sym(n->dl, "sim_dns_put");
    n->allocs = (sim_allocs_fn)sym(n->dl, "sim_allocs");
    n->netmem = (sim_netmem_fn)sym(n->dl, "sim_netmem");
    u8 mac[6] = { 2, 0, 0, 0, 1, (u8)i };
    memcpy(n->mac, mac, 6);
    n->ip = dhcp ? 0 : NODEIP(i);
    n->up = 1;
    if (i > nnodes) nnodes = i;
    n->init(&host_cfg, i, mac, (unsigned)rnd());
    if (dhcp) n->setting("net.eth0.mode", "dhcp");
    else {
        n->setting("net.eth0.mode", "static");
        ipset(i, "net.eth0.ip", NODEIP(i));
        n->setting("net.eth0.mask", "255.255.255.0");
        n->setting("net.eth0.gateway", "10.0.0.1");
        n->setting("net.dns", "10.0.0.1,10.0.0.53");
    }
}

static void start_node(int i) {
    nodes[i].start();
    poll_node(i);
}

static long C(int n, int nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r = nodes[n].call(nr, a1, a2, a3, a4, a5, a6);
    poll_node(n);
    return r;
}

static socklen_t mksin(struct sockaddr_in *a, u32 ip, int port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = hs((u16)port);
    a->sin_addr.s_addr = ip;
    return sizeof *a;
}

static int s_socket(int n, int type, int proto) { return (int)C(n, SYS_SOCKET, AF_INET, type | SOCK_NONBLOCK, proto, 0, 0, 0); }
static int s_bind(int n, int fd, u32 ip, int port) { struct sockaddr_in a; socklen_t l = mksin(&a, ip, port); return (int)C(n, SYS_BIND, fd, (long)&a, l, 0, 0, 0); }
static int s_listen(int n, int fd, int bl) { return (int)C(n, SYS_LISTEN, fd, bl, 0, 0, 0, 0); }
static int s_accept(int n, int fd) { return (int)C(n, SYS_ACCEPT, fd, 0, 0, SOCK_NONBLOCK, 0, 0); }
static int s_connect(int n, int fd, u32 ip, int port) { struct sockaddr_in a; socklen_t l = mksin(&a, ip, port); return (int)C(n, SYS_CONNECT, fd, (long)&a, l, 0, 0, 0); }
static long s_send(int n, int fd, const void *b, long len) { return C(n, SYS_SENDTO, fd, (long)b, len, 0, 0, 0); }
static long s_recv(int n, int fd, void *b, long len) { return C(n, SYS_RECVFROM, fd, (long)b, len, 0, 0, 0); }
static long s_sendto(int n, int fd, const void *b, long len, u32 ip, int port) {
    struct sockaddr_in a; socklen_t l = mksin(&a, ip, port);
    return C(n, SYS_SENDTO, fd, (long)b, len, 0, (long)&a, l);
}
static long s_recvfrom(int n, int fd, void *b, long len, u32 *ip, int *port) {
    struct sockaddr_in a; socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    long r = C(n, SYS_RECVFROM, fd, (long)b, len, 0, (long)&a, (long)&l);
    if (ip) *ip = a.sin_addr.s_addr;
    if (port) *port = hs(a.sin_port);
    return r;
}
static int s_close(int n, int fd) { return (int)C(n, SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static int s_shutdown(int n, int fd, int how) { return (int)C(n, SYS_SHUTDOWN, fd, how, 0, 0, 0, 0); }
static int s_setint(int n, int fd, int level, int name, int v) { return (int)C(n, SYS_SETSOCKOPT, fd, level, name, (long)&v, sizeof v, 0); }
static int s_getint(int n, int fd, int level, int name) {
    int v = -999; socklen_t l = sizeof v;
    long r = C(n, SYS_GETSOCKOPT, fd, level, name, (long)&v, (long)&l, 0);
    return r ? (int)r : v;
}
static int s_error(int n, int fd) { return s_getint(n, fd, SOL_SOCKET, SO_ERROR); }
static int s_events(int n, int fd) {
    struct pollfd p = { fd, POLLIN | POLLOUT | POLLRDHUP, 0 };
    C(n, SYS_POLL, (long)&p, 1, 0, 0, 0, 0);
    return p.revents;
}
static int s_name(int n, int fd, int peer, u32 *ip, int *port) {
    struct sockaddr_in a; socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    int r = (int)C(n, peer ? SYS_GETPEERNAME : SYS_GETSOCKNAME, fd, (long)&a, (long)&l, 0, 0, 0);
    if (ip) *ip = a.sin_addr.s_addr;
    if (port) *port = hs(a.sin_port);
    return r;
}
static void s_stats(int n, hal_netstats_t *st) { C(n, SYS_NET_STATS, (long)st, 0, 0, 0, 0, 0); }
static int s_ifinfo(int n, hal_ifinfo_t *o) { return (int)C(n, SYS_NET_IFINFO, 0, (long)o, 0, 0, 0, 0); }
/* the TCP state of the connection with local port lport (and remote port rport if nonzero), -1 none */
static int tcp_state(int n, int lport, int rport) {
    static hal_netconn_t c[512];
    int k = (int)C(n, SYS_NET_CONNS, (long)c, 512, 0, 0, 0, 0);
    for (int i = 0; i < k; i++)
        if (c[i].proto == NETC_TCP && (!lport || c[i].lport == lport) && (!rport || c[i].rport == rport)) return c[i].state;
    return -1;
}

/* ================= the router: ARP, ICMP, DHCP, DNS ================= */

static void router_send(const u8 *dstmac, u32 dst, u8 proto, const u8 *payload, u32 plen, u32 src) {
    static u8 f[1600];
    static u16 id = 1;
    memcpy(f, dstmac, 6);
    memcpy(f + 6, router_mac, 6);
    wr16(f + 12, 0x0800);
    u8 *h = f + 14;
    memset(h, 0, 20);
    h[0] = 0x45; wr16(h + 2, (u16)(20 + plen)); wr16(h + 4, id++); h[8] = 64; h[9] = proto;
    wra(h + 12, src ? src : ROUTER); wra(h + 16, dst);
    wr16(h + 10, cksum(h, 20, 0));
    memcpy(h + 20, payload, plen);
    wire_send(0, f, 34 + plen);
}

static void router_udp(const u8 *dstmac, u32 src, u32 dst, u16 sport, u16 dport, const u8 *d, u32 n) {
    static u8 u[1500];
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, (u16)(8 + n)); wr16(u + 6, 0);
    memcpy(u + 8, d, n);
    wr16(u + 6, cksum(u, 8 + n, pseudo(src, dst, 17, 8 + n)));
    router_send(dstmac, dst, 17, u, 8 + n, src);
}

/* ICMP "fragmentation needed" about a frame that does not fit wire.mtu */
static void router_toobig(const u8 *f, u32 len) {
    u8 m[8 + 28];
    memset(m, 0, 8);
    m[0] = 3; m[1] = 4; wr16(m + 6, (u16)wire.mtu);
    memcpy(m + 8, f + 14, 28);
    wr16(m + 2, cksum(m, sizeof m, 0));
    router_send(f + 6, rda(f + 14 + 12), 1, m, sizeof m, 0);
}

/* ---- ARP ---- */
static int r_arp_requests, r_garps;
static u32 r_proxy_lo, r_proxy_hi;          /* also answer ARP for this range (host order of the last byte) */

static void router_arp(const u8 *f, u32 len) {
    const u8 *a = f + 14;
    if (len < 42 || rd16(a + 6) != 1) return;
    u32 spa = rda(a + 14), tpa = rda(a + 24);
    r_arp_requests++;
    if (spa == tpa) r_garps++;
    u32 last = tpa >> 24;
    int ours = tpa == ROUTER || tpa == DNS2 || ((tpa & 0xFFFFFF) == (ROUTER & 0xFFFFFF) && last >= r_proxy_lo && last <= r_proxy_hi && r_proxy_hi);
    if (!ours) return;
    u8 r[42];
    memcpy(r, f + 6, 6);
    u8 mymac[6];
    memcpy(mymac, router_mac, 6);
    if (tpa != ROUTER && tpa != DNS2) mymac[5] = (u8)last, mymac[4] = 9;
    memcpy(r + 6, mymac, 6);
    wr16(r + 12, 0x0806);
    u8 *o = r + 14;
    wr16(o, 1); wr16(o + 2, 0x0800); o[4] = 6; o[5] = 4; wr16(o + 6, 2);
    memcpy(o + 8, mymac, 6); wra(o + 14, tpa);
    memcpy(o + 18, a + 8, 6); wra(o + 24, spa);
    wire_send(0, r, 42);
}

/* ---- DHCP server ---- */
static struct {
    int on, nak;
    u32 lease;
    u32 next;                   /* next address's last byte */
    u32 offer_override;
    int counts[9];
    int unicast_req, bcast_req;
    u32 last_req_ip, last_ciaddr;
    struct { u8 mac[6]; u32 ip; } leases[8];
    int nleases;
    u32 declined;
} ds;

static void dhcp_reset(void) { memset(&ds, 0, sizeof ds); ds.on = 1; ds.lease = 3600; ds.next = 100; }

static u32 lease_for(const u8 *mac) {
    for (int i = 0; i < ds.nleases; i++) if (!memcmp(ds.leases[i].mac, mac, 6)) return ds.leases[i].ip;
    if (ds.offer_override) return ds.offer_override;
    u32 ip = IP(10, 0, 0, ds.next++);
    if (ip == ds.declined) ip = IP(10, 0, 0, ds.next++);
    if (ds.nleases < 8) { memcpy(ds.leases[ds.nleases].mac, mac, 6); ds.leases[ds.nleases].ip = ip; ds.nleases++; }
    return ip;
}

static void router_dhcp(const u8 *f, const u8 *d, u32 n, int l2bcast) {
    if (n < 240 || d[0] != 1 || rd32(d + 236) != 0x63825363) return;
    int type = 0;
    u32 reqip = 0;
    for (u32 o = 240; o < n;) {
        u8 c = d[o];
        if (c == 255) break;
        if (c == 0) { o++; continue; }
        if (o + 1 >= n || o + 2 + d[o + 1] > n) break;
        if (c == 53) type = d[o + 2];
        if (c == 50 && d[o + 1] == 4) reqip = rda(d + o + 2);
        o += 2u + d[o + 1];
    }
    if (type < 1 || type > 8) return;
    ds.counts[type]++;
    u32 ciaddr = rda(d + 12);
    if (type == 3) {
        if (l2bcast) ds.bcast_req++; else ds.unicast_req++;
        ds.last_req_ip = reqip;
        ds.last_ciaddr = ciaddr;
    }
    if (type == 7) {                            /* release */
        for (int i = 0; i < ds.nleases; i++) if (!memcmp(ds.leases[i].mac, d + 28, 6)) ds.leases[i] = ds.leases[--ds.nleases];
        return;
    }
    if (type == 4) {                            /* decline */
        ds.declined = reqip;
        for (int i = 0; i < ds.nleases; i++) if (!memcmp(ds.leases[i].mac, d + 28, 6)) ds.leases[i] = ds.leases[--ds.nleases];
        ds.offer_override = 0;
        return;
    }
    if (!ds.on || (type != 1 && type != 3)) return;
    u8 r[300];
    memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, d + 4, 4);                    /* xid */
    memcpy(r + 10, d + 10, 2);
    wra(r + 12, ciaddr);
    u32 yi = lease_for(d + 28);
    int reply = type == 1 ? 2 : (ds.nak ? 6 : 5);
    if (type == 3 && reqip && reqip != yi) reply = 6;   /* not the address we have for it */
    if (type == 3 && !reqip && ciaddr && ciaddr != yi) reply = 6;
    if (reply != 6) wra(r + 16, yi);
    memcpy(r + 28, d + 28, 16);
    wr32(r + 236, 0x63825363);
    u8 *o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)reply;
    *o++ = 54; *o++ = 4; wra(o, ROUTER); o += 4;
    if (reply != 6) {
        *o++ = 51; *o++ = 4; wr32(o, ds.lease); o += 4;
        *o++ = 1; *o++ = 4; wra(o, IP(255, 255, 255, 0)); o += 4;
        *o++ = 3; *o++ = 4; wra(o, ROUTER); o += 4;
        *o++ = 6; *o++ = 8; wra(o, ROUTER); wra(o + 4, DNS2); o += 8;
    }
    *o++ = 255;
    static const u8 bc[6] = { 255, 255, 255, 255, 255, 255 };
    int unicast = ciaddr && reply != 6;
    router_udp(unicast ? d + 28 : bc, ROUTER, unicast ? ciaddr : 0xFFFFFFFFu, 67, 68, r, (u32)(o - r));
    (void)f;
}

/* ---- DNS servers (10.0.0.1 and 10.0.0.53) ---- */
enum { ZA = 1, ZCNAME, ZCNAME_ONLY, ZNX, ZNODATA, ZFAIL, ZDROP, ZSPOOF, ZLOOP, ZTC };
typedef struct zrec { const char *name; int kind; u32 a[3]; int na; const char *target; u32 ttl; } zrec_t;
static const zrec_t zone[] = {
    { "www.test", ZA, { IP(10, 0, 0, 3) }, 1, 0, 300 },
    { "multi.test", ZA, { IP(192, 0, 2, 1), IP(192, 0, 2, 2), IP(192, 0, 2, 3) }, 3, 0, 300 },
    { "short.test", ZA, { IP(192, 0, 2, 9) }, 1, 0, 5 },
    { "alias.test", ZCNAME, { 0 }, 0, "www.test", 60 },
    { "chain.test", ZCNAME_ONLY, { 0 }, 0, "far.test", 60 },
    { "far.test", ZA, { IP(192, 0, 2, 7) }, 1, 0, 30 },
    { "nx.test", ZNX, { 0 }, 0, 0, 0 },
    { "nodata.test", ZNODATA, { 0 }, 0, 0, 0 },
    { "fail.test", ZFAIL, { 0 }, 0, 0, 0 },
    { "drop.test", ZDROP, { 0 }, 0, 0, 0 },
    { "spoof.test", ZSPOOF, { IP(10, 0, 0, 3) }, 1, 0, 300 },
    { "loop.test", ZLOOP, { 0 }, 0, 0, 0 },
    { "tc.test", ZTC, { 0 }, 0, 0, 0 },
    { "l1.test", ZCNAME_ONLY, { 0 }, 0, "l2.test", 60 }, { "l2.test", ZCNAME_ONLY, { 0 }, 0, "l3.test", 60 },
    { "l3.test", ZCNAME_ONLY, { 0 }, 0, "l4.test", 60 }, { "l4.test", ZCNAME_ONLY, { 0 }, 0, "l5.test", 60 },
    { "l5.test", ZCNAME_ONLY, { 0 }, 0, "l6.test", 60 }, { "l6.test", ZCNAME_ONLY, { 0 }, 0, "l7.test", 60 },
    { "l7.test", ZCNAME_ONLY, { 0 }, 0, "l8.test", 60 }, { "l8.test", ZCNAME_ONLY, { 0 }, 0, "l9.test", 60 },
    { "l9.test", ZCNAME_ONLY, { 0 }, 0, "l10.test", 60 }, { "l10.test", ZA, { IP(192, 0, 2, 10) }, 1, 0, 60 },
};
static int dns_alive[2] = { 1, 1 };
static int dns_queries[2];

static u32 put_name(u8 *b, const char *n) {
    u32 o = 0;
    while (*n) {
        const char *dot = strchr(n, '.');
        u32 l = dot ? (u32)(dot - n) : (u32)strlen(n);
        b[o++] = (u8)l;
        memcpy(b + o, n, l);
        o += l; n += l;
        if (*n == '.') n++;
    }
    b[o++] = 0;
    return o;
}

static u32 put_rr(u8 *b, const char *name, u16 type, u32 ttl, const u8 *rd, u16 rdl) {
    u32 o = put_name(b, name);
    wr16(b + o, type); wr16(b + o + 2, 1); wr32(b + o + 4, ttl); wr16(b + o + 8, rdl);
    memcpy(b + o + 10, rd, rdl);
    return o + 10 + rdl;
}

static const zrec_t *zfind(const char *n) {
    for (unsigned i = 0; i < sizeof zone / sizeof zone[0]; i++) if (!strcasecmp(zone[i].name, n)) return &zone[i];
    return NULL;
}

static void router_dns(const u8 *f, u32 dst, u32 src, u16 sport, const u8 *d, u32 n) {
    int si = dst == ROUTER ? 0 : 1;
    if (n < 17) return;
    dns_queries[si]++;
    if (!dns_alive[si]) return;
    char name[256];
    u32 o = 12, nl = 0;
    while (o < n && d[o]) {
        u32 l = d[o++];
        if (o + l > n || nl + l + 1 >= sizeof name) return;
        if (nl) name[nl++] = '.';
        memcpy(name + nl, d + o, l); nl += l; o += l;
    }
    name[nl] = 0;
    o++;
    if (o + 4 > n) return;
    u32 qend = o + 4;
    const zrec_t *z = zfind(name);
    char gen[64];
    zrec_t tmp;
    unsigned hn;
    if (!z && sscanf(name, "h%u.big.test", &hn) == 1) {     /* many names for the cache test */
        snprintf(gen, sizeof gen, "%s", name);
        tmp = (zrec_t){ gen, ZA, { IP(10, 1, hn >> 8 & 255, hn & 255) }, 1, 0, 600 };
        z = &tmp;
    }
    u8 r[1200];
    memcpy(r, d, qend);
    wr16(r + 2, 0x8180);
    wr16(r + 4, 1);
    wr16(r + 6, 0); wr16(r + 8, 0); wr16(r + 10, 0);
    u32 p = qend;
    int an = 0, ns = 0, rcode = 0;
    u8 soa[64];
    u32 sl = put_name(soa, "ns.test");
    sl += put_name(soa + sl, "root.test");
    wr32(soa + sl, 1); wr32(soa + sl + 4, 3600); wr32(soa + sl + 8, 600); wr32(soa + sl + 12, 86400); wr32(soa + sl + 16, 30);
    sl += 20;
    if (!z) { rcode = 3; p += put_rr(r + p, "test", 6, 300, soa, (u16)sl); ns++; }
    else switch (z->kind) {
    case ZA: case ZSPOOF:
        for (int i = 0; i < z->na; i++) { u8 a[4]; wra(a, z->a[i]); p += put_rr(r + p, z->name, 1, z->ttl, a, 4); an++; }
        break;
    case ZCNAME: case ZCNAME_ONLY: {
        u8 t[256];
        u32 tl = put_name(t, z->target);
        p += put_rr(r + p, z->name, 5, z->ttl, t, (u16)tl); an++;
        const zrec_t *tz = zfind(z->target);
        if (z->kind == ZCNAME && tz)
            for (int i = 0; i < tz->na; i++) { u8 a[4]; wra(a, tz->a[i]); p += put_rr(r + p, tz->name, 1, tz->ttl, a, 4); an++; }
        break;
    }
    case ZNX: rcode = 3; p += put_rr(r + p, "test", 6, 300, soa, (u16)sl); ns++; break;
    case ZNODATA: p += put_rr(r + p, "test", 6, 300, soa, (u16)sl); ns++; break;
    case ZFAIL: rcode = 2; break;
    case ZDROP: return;
    case ZLOOP:                                 /* an answer whose name is a pointer to itself */
        r[p] = 0xC0; r[p + 1] = (u8)p; wr16(r + p + 2, 1); wr16(r + p + 4, 1); wr32(r + p + 6, 60); wr16(r + p + 10, 4);
        wra(r + p + 12, IP(6, 6, 6, 6));
        p += 16; an++;
        break;
    case ZTC: wr16(r + 2, 0x8380); break;
    }
    wr16(r + 2, (u16)(rd16(r + 2) | rcode));
    wr16(r + 6, (u16)an); wr16(r + 8, (u16)ns);
    if (z && z->kind == ZSPOOF) {               /* forgeries first: a wrong id, then the right id from elsewhere */
        u8 fk[1200];
        memcpy(fk, r, p);
        wr16(fk, (u16)(rd16(r) ^ 0x1234));
        wra(fk + p - 4, IP(6, 6, 6, 6));
        router_udp(f + 6, dst, src, 53, sport, fk, p);
        wr16(fk, rd16(r));
        router_udp(f + 6, IP(10, 0, 0, 66), src, 53, sport, fk, p);
        router_udp(f + 6, dst, src, 54, sport, fk, p);
    }
    router_udp(f + 6, dst, src, 53, sport, r, p);
}

static int r_pings;
static void router_rx(const u8 *f, u32 len) {
    if (len < 14) return;
    int l2bcast = f[0] == 0xFF;
    if (!l2bcast && memcmp(f, router_mac, 6) && !(f[4] == 9 && r_proxy_hi)) return;
    u16 type = rd16(f + 12);
    if (type == 0x0806) { router_arp(f, len); return; }
    if (type != 0x0800 || len < 34) return;
    const u8 *h = f + 14;
    u32 hl = (u32)(h[0] & 15) * 4, tot = rd16(h + 2);
    if (tot > len - 14 || hl < 20 || tot < hl || cksum(h, hl, 0)) return;
    if (rd16(h + 6) & 0x3FFF) return;           /* (no fragments here) */
    u32 src = rda(h + 12), dst = rda(h + 16);
    const u8 *p = h + hl;
    u32 pl = tot - hl;
    if (h[9] == 1 && pl >= 8 && p[0] == 8 && (dst == ROUTER || dst == DNS2)) {
        u8 m[1500];
        memcpy(m, p, pl);
        m[0] = 0; m[2] = m[3] = 0;
        wr16(m + 2, cksum(m, pl, 0));
        r_pings++;
        router_send(f + 6, src, 1, m, pl, dst);
        return;
    }
    if (h[9] != 17 || pl < 8) return;
    u16 sport = rd16(p), dport = rd16(p + 2);
    if (dport == 67) router_dhcp(f, p + 8, pl - 8, l2bcast);
    else if (dport == 53 && (dst == ROUTER || dst == DNS2)) router_dns(f, dst, src, sport, p + 8, pl - 8);
}

/* ================= sniffing TCP ================= */

typedef struct seg { u64 t; int src, dst; u8 flags; u32 seq, ack; u16 win; u32 len; int ws, mss; } seg_t;
#define MAXSEG 200000
static seg_t *segs;
static int nsegs;

static int sniff(int src, int dst, u8 *f, u32 *len) {
    if (*len < 54 || rd16(f + 12) != 0x0800) return 1;
    const u8 *h = f + 14;
    if (h[9] != 6) return 1;
    u32 hl = (u32)(h[0] & 15) * 4, tot = rd16(h + 2);
    const u8 *t = h + hl;
    u32 thl = (u32)(t[12] >> 4) * 4;
    if (nsegs >= MAXSEG) return 1;
    seg_t *s = &segs[nsegs++];
    s->t = now; s->src = src; s->dst = dst; s->flags = t[13];
    s->seq = rd32(t + 4); s->ack = rd32(t + 8); s->win = rd16(t + 14);
    s->len = tot - hl - thl;
    s->ws = -1; s->mss = 0;
    for (u32 o = 20; o < thl;) {
        u8 k = t[o];
        if (k == 0) break;
        if (k == 1) { o++; continue; }
        if (o + 1 >= thl || t[o + 1] < 2) break;
        if (k == 2) s->mss = rd16(t + o + 2);
        if (k == 3) s->ws = t[o + 2];
        o += t[o + 1];
    }
    return 1;
}

/* a TCP segment from (sip, sport) to node dst, as if from node from (its MAC) */
static void inject_tcp(int dst, int from, u32 sip, u16 sport, u32 dip, u16 dport, u32 seq, u32 ack, u8 flags, const u8 *data, u32 dlen) {
    u8 f[1600];
    memcpy(f, nodes[dst].mac, 6);
    memcpy(f + 6, from ? nodes[from].mac : router_mac, 6);
    wr16(f + 12, 0x0800);
    u8 *h = f + 14;
    memset(h, 0, 40);
    h[0] = 0x45; wr16(h + 2, (u16)(40 + dlen)); h[8] = 64; h[9] = 6;
    wra(h + 12, sip); wra(h + 16, dip);
    wr16(h + 10, cksum(h, 20, 0));
    u8 *t = h + 20;
    wr16(t, sport); wr16(t + 2, dport); wr32(t + 4, seq); wr32(t + 8, ack);
    t[12] = 5 << 4; t[13] = flags; wr16(t + 14, 8192);
    if (dlen) memcpy(t + 20, data, dlen);
    wr16(t + 16, cksum(t, 20 + dlen, pseudo(sip, dip, 6, 20 + dlen)));
    inject(dst, f, 54 + dlen);
}

/* ================= helpers for the tests ================= */

typedef struct xfer {
    int na, fa, nb, fb;             /* from (node, fd) to (node, fd) */
    long total, sent, got;
    int bad, eof, shut, err, nshut;
    long chunk;
    u8 seed;
} xfer_t;

static u8 xpat(u8 seed, long i) { return (u8)((i * 31) ^ (i >> 7) ^ (i >> 15) ^ seed); }

static void xfer_init(xfer_t *x, int na, int fa, int nb, int fb, long total, u8 seed) {
    memset(x, 0, sizeof *x);
    x->na = na; x->fa = fa; x->nb = nb; x->fb = fb;
    x->total = total;
    x->chunk = 16384;
    x->seed = seed;
}

static int xfer_step(void *arg) {
    xfer_t *x = arg;
    static u8 buf[65536];
    while (x->sent < x->total) {
        long k = MIN(x->total - x->sent, x->chunk);
        for (long i = 0; i < k; i++) buf[i] = xpat(x->seed, x->sent + i);
        long r = s_send(x->na, x->fa, buf, k);
        if (r <= 0) { if (r != E_AGAIN) x->err = (int)r; break; }
        x->sent += r;
    }
    if (x->sent == x->total && !x->shut && !x->nshut) { s_shutdown(x->na, x->fa, SHUT_WR); x->shut = 1; }
    for (;;) {
        long r = s_recv(x->nb, x->fb, buf, sizeof buf);
        if (r > 0) {
            for (long i = 0; i < r; i++) if (buf[i] != xpat(x->seed, x->got + i)) x->bad = 1;
            x->got += r;
            continue;
        }
        if (r == 0) x->eof = 1;
        else if (r != E_AGAIN) x->err = (int)r;
        break;
    }
    return x->eof || x->err || (x->nshut && x->got >= x->total);
}

static int two_xfers(void *arg) {
    xfer_t *x = arg;
    int a = xfer_step(&x[0]), b = xfer_step(&x[1]);
    return a && b;
}

/* a connected TCP pair: client on node a, accepted socket on node b (port) */
static int listener_fd;
static int tcp_pair(int a, int b, int port, int *ca, int *sb) {
    int l = s_socket(b, SOCK_STREAM, 0);
    s_setint(b, l, SOL_SOCKET, SO_REUSEADDR, 1);
    int r = s_bind(b, l, 0, port);
    if (!r) r = s_listen(b, l, 8);
    if (r) { s_close(b, l); return r; }
    int c = s_socket(a, SOCK_STREAM, 0);
    r = s_connect(a, c, nodes[b].ip, port);
    if (r != E_INPROGRESS) return r ? r : E_IO;
    int s = -1;
    for (int k = 0; k < 2000 && s < 0; k++) {
        run_for(5);
        s = s_accept(b, l);
    }
    listener_fd = l;
    if (s < 0) return s;
    for (int k = 0; k < 200 && !(s_events(a, c) & POLLOUT); k++) run_for(5);
    *ca = c; *sb = s;
    return 0;
}

static void setup(int n, int dhcp_nodes) {
    memset(&wire, 0, sizeof wire);
    memset(busy_until, 0, sizeof busy_until);
    nnodes = 0;
    dhcp_reset();
    dns_alive[0] = dns_alive[1] = 1;
    dns_queries[0] = dns_queries[1] = 0;
    r_arp_requests = r_garps = r_pings = 0;
    r_proxy_lo = r_proxy_hi = 0;
    nsegs = 0;
    for (int i = 1; i <= n; i++) add_node(i, i <= dhcp_nodes);
    for (int i = 1; i <= n; i++) start_node(i);
    run_for(50);
    for (int i = 1; i <= n; i++) nodes[i].base_allocs = nodes[i].allocs();
}

/* close every handle, let TIME_WAIT pass, check that the memory came back */
static void teardown(void) {
    memset(&wire, 0, sizeof wire);
    for (int i = 1; i <= nnodes; i++) {
        if (!nodes[i].up) continue;
        for (int fd = 3; fd < 64; fd++) nodes[i].call(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        poll_node(i);
    }
    run_for(200000);                    /* orphans finish, TIME_WAIT (60 s), FIN_WAIT_2 (60 s), ARP, reassembly */
    for (int i = 1; i <= nnodes; i++) {
        if (!nodes[i].up) continue;
        long a = nodes[i].allocs();
        unsigned long long m = nodes[i].netmem();
        CHECK(a <= nodes[i].base_allocs + 32, "node %d: %ld allocations left (%ld at start, 32 cached buffers allowed)", i, a, nodes[i].base_allocs);
        CHECK(m == 0, "node %d: %llu bytes still charged", i, m);
    }
    while (nheap) free(heap_pop().data);
    for (int i = 1; i <= nnodes; i++) { if (nodes[i].dl) dlclose(nodes[i].dl); nodes[i].dl = NULL; nodes[i].up = 0; }
    nnodes = 0;
    wire.filter = NULL;
}

static int resolve(int n, const char *name, u32 *out, int max, unsigned timeout) {
    int err = 0;
    void *q = nodes[n].dns_start(name, timeout, &err);
    if (!q) return err;
    poll_node(n);                       /* (the kernel pokes the net thread) */
    int r = E_AGAIN;
    for (u64 t0 = now; now - t0 <= timeout + 100;) {
        run_for(10);
        r = nodes[n].dns_result(q, out, max);
        if (r != E_AGAIN) break;
    }
    nodes[n].dns_put(q);
    poll_node(n);
    return r;
}

/* ================= tests ================= */

/* ping (an ICMP socket): echo id/seq/payload, our own address, nobody there */
static int ping_once(int n, u32 dst, int seq, int size, u64 wait_ms, long *res) {
    int fd = s_socket(n, SOCK_DGRAM, IPPROTO_ICMP);
    u8 pkt[4000], in[4100];
    memset(pkt, 0, 8);
    pkt[0] = 8; wr16(pkt + 6, (u16)seq);
    for (int i = 8; i < size; i++) pkt[i] = (u8)(i ^ seq);
    long r = s_sendto(n, fd, pkt, size, dst, 0);
    int ok = 0;
    *res = r;
    if (r == size) {
        u64 t0 = now;
        while (now - t0 < wait_ms) {
            run_for(5);
            u32 from;
            r = s_recvfrom(n, fd, in, sizeof in, &from, NULL);
            if (r == E_AGAIN) continue;
            *res = r;
            if (r == size && in[0] == 0 && rd16(in + 6) == seq && from == dst && !memcmp(in + 8, pkt + 8, (size_t)size - 8)) ok = 1;
            break;
        }
    }
    s_close(n, fd);
    return ok;
}

static void test_arp_ping(void) {
    setup(2, 0);
    CHECK(r_garps >= 2, "gratuitous ARP at configuration (%d)", r_garps);
    long r;
    CHECK(ping_once(1, NODEIP(2), 1, 64, 1000, &r), "ping 10.0.0.3 (%ld)", r);
    CHECK(ping_once(1, ROUTER, 2, 1400, 1000, &r), "ping the router, 1400 bytes (%ld)", r);
    CHECK(ping_once(2, NODEIP(1), 3, 8, 1000, &r), "ping back, header only");
    CHECK(ping_once(1, NODEIP(1), 4, 100, 1000, &r), "ping our own address");
    CHECK(ping_once(1, IP(127, 0, 0, 1), 5, 3000, 1000, &r), "ping 127.0.0.1, 3000 bytes (fragmented on lo? %ld)", r);
    hal_netstats_t st;
    s_stats(2, &st);
    CHECK(st.arp_entries >= 1, "node 2 learned node 1 from its request");
    int arp0 = r_arp_requests;
    CHECK(ping_once(1, NODEIP(2), 6, 64, 1000, &r) && r_arp_requests == arp0, "cached: no new ARP request");
    /* nobody at 10.0.0.99: three requests, then "host unreachable" to the sender */
    u64 t0 = now;
    ping_once(1, IP(10, 0, 0, 99), 7, 64, 5000, &r);
    CHECK(r == E_HOSTUNREACH, "ping nobody: E_HOSTUNREACH (%ld after %llu ms)", r, (unsigned long long)(now - t0));
    CHECK(now - t0 >= 2900 && now - t0 < 4500, "after three ARP requests (%llu ms)", (unsigned long long)(now - t0));
    /* the cache holds at most 64 entries */
    r_proxy_lo = 120; r_proxy_hi = 220;
    int u = s_socket(1, SOCK_DGRAM, 0);
    for (int i = 120; i <= 220; i++) { s_sendto(1, u, "x", 1, IP(10, 0, 0, i), 9); run_for(2); }
    run_for(100);
    s_stats(1, &st);
    CHECK(st.arp_entries <= 64 && st.arp_entries >= 50, "ARP cache bounded (%d entries)", st.arp_entries);
    /* entries age: unused ones are gone after 5 minutes */
    run_for(400000);
    s_stats(1, &st);
    CHECK(st.arp_entries == 0, "unused ARP entries expire (%d)", st.arp_entries);
    teardown();
}

static int ifaddr(int n) { hal_ifinfo_t o; s_ifinfo(n, &o); return (int)(o.addr != 0); }
static int has_addr(void *arg) { return ifaddr((int)(long)arg); }

static void test_dhcp(void) {
    setup(1, 1);
    ds.lease = 60;
    /* (setup started it before the lease was set: start over) */
    CHECK(run_until(has_addr, (void *)1L, 10000), "DHCP: an address");
    hal_ifinfo_t o;
    s_ifinfo(1, &o);
    CHECK(o.addr == IP(10, 0, 0, 100) && o.mask == IP(255, 255, 255, 0) && o.gateway == ROUTER, "lease %s/%s via %s",
          ips(o.addr), ips(o.mask), ips(o.gateway));
    CHECK(o.dns[0] == ROUTER && o.dns[1] == DNS2 && o.dhcp_state == NET_DHCP_BOUND && o.dhcp_server == ROUTER, "DNS servers, state");
    CHECK(ds.counts[1] == 1 && ds.counts[3] == 1, "one DISCOVER, one REQUEST (%d %d)", ds.counts[1], ds.counts[3]);
    long r;
    CHECK(ping_once(1, ROUTER, 1, 64, 1000, &r), "the address works");
    /* renew: lease 3600 from the first exchange; force T1 with a short lease */
    C(1, SYS_NET_CTL, NETCTL_RELEASE, 0, 0, 0, 0, 0);
    run_for(20);
    CHECK(ds.counts[7] == 1 && !ifaddr(1), "RELEASE");
    C(1, SYS_NET_CTL, NETCTL_RENEW, 0, 0, 0, 0, 0);
    CHECK(run_until(has_addr, (void *)1L, 10000), "a new lease after RENEW");
    s_ifinfo(1, &o);
    CHECK(o.lease_s == 60, "60 s lease (%lld)", o.lease_s);
    int ur = ds.unicast_req;
    run_for(31000);
    CHECK(ds.unicast_req == ur + 1, "RENEWING at T1: unicast REQUEST (%d)", ds.unicast_req - ur);
    s_ifinfo(1, &o);
    CHECK(o.dhcp_state == NET_DHCP_BOUND && o.lease_left_s > 50, "renewed (%d, %lld left)", o.dhcp_state, o.lease_left_s);
    /* the server goes away: REBINDING at T2 (broadcast), then the lease expires */
    ds.on = 0;
    int br = ds.bcast_req;
    run_for(54000);
    s_ifinfo(1, &o);
    CHECK(o.dhcp_state == NET_DHCP_REBINDING && ds.bcast_req > br, "REBINDING (state %d, %d broadcasts)", o.dhcp_state, ds.bcast_req - br);
    run_for(10000);
    CHECK(!ifaddr(1), "the address is gone when the lease expires");
    s_ifinfo(1, &o);
    CHECK(o.dhcp_state == NET_DHCP_SELECTING || o.dhcp_state == NET_DHCP_INIT, "back to DISCOVER (%d)", o.dhcp_state);
    ds.on = 1;
    CHECK(run_until(has_addr, (void *)1L, 70000), "a lease again when the server returns");
    /* link down and up: INIT-REBOOT asks for the same address */
    s_ifinfo(1, &o);
    u32 had = o.addr;
    nodes[1].link(0);
    poll_node(1);
    run_for(2000);
    br = ds.bcast_req;
    nodes[1].link(1);
    poll_node(1);
    run_for(3000);
    s_ifinfo(1, &o);
    CHECK(ds.bcast_req == br + 1 && ds.last_req_ip == had && o.addr == had && o.dhcp_state == NET_DHCP_BOUND,
          "link up: INIT-REBOOT REQUEST for %s (%d)", ips(had), o.dhcp_state);
    /* NAK on renewal: start over */
    ds.nak = 1;
    C(1, SYS_NET_CTL, NETCTL_RENEW, 0, 0, 0, 0, 0);
    run_for(500);
    CHECK(!ifaddr(1) && ds.counts[3] > 0, "NAK drops the address");
    ds.nak = 0;
    CHECK(run_until(has_addr, (void *)1L, 30000), "then a new lease");
    /* static through the system call, then DHCP again */
    hal_ifconfig_t c;
    memset(&c, 0, sizeof c);
    strcpy(c.name, "eth0");
    c.mode = NET_MODE_STATIC;
    c.addr = IP(10, 0, 0, 77); c.mask = IP(255, 255, 255, 0); c.gateway = ROUTER;
    c.flags = NETCFG_ADDR;
    int g = r_garps;
    CHECK(C(1, SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0) == 0, "set a static address");
    run_for(20);
    s_ifinfo(1, &o);
    CHECK(o.addr == IP(10, 0, 0, 77) && o.mode == NET_MODE_STATIC && r_garps > g, "static 10.0.0.77, announced");
    CHECK(ds.counts[7] >= 2, "the DHCP lease was released");
    c.gateway = IP(10, 9, 9, 9);
    CHECK(C(1, SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0) == E_INVAL, "a gateway off the subnet is refused");
    c.mask = IP(255, 0, 255, 0);
    CHECK(C(1, SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0) == E_INVAL, "a non-contiguous mask is refused");
    c.mode = NET_MODE_DHCP;
    CHECK(C(1, SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0) == 0, "back to DHCP");
    CHECK(run_until(has_addr, (void *)1L, 10000) && (s_ifinfo(1, &o), o.mode == NET_MODE_DHCP), "DHCP again");
    teardown();

    /* an address someone else has: DECLINE */
    setup(2, 1);
    teardown();
    setup(0, 0);
    add_node(2, 0);
    start_node(2);
    add_node(1, 1);
    dhcp_reset();
    ds.offer_override = NODEIP(2);
    start_node(1);
    run_for(50);
    nodes[1].base_allocs = nodes[1].allocs();
    nodes[2].base_allocs = nodes[2].allocs();
    run_for(3000);
    CHECK(ds.counts[4] == 1, "DECLINE of an address in use (%d)", ds.counts[4]);
    CHECK(run_until(has_addr, (void *)1L, 30000), "then another address");
    s_ifinfo(1, &o);
    CHECK(o.addr && o.addr != NODEIP(2), "not the conflicting one (%s)", ips(o.addr));
    teardown();
}

static void test_dns(void) {
    setup(1, 0);
    u32 a[16];
    hal_netstats_t s0, s1;
    int n = resolve(1, "www.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == IP(10, 0, 0, 3), "A record (%d)", n);
    int q0 = dns_queries[0];
    n = resolve(1, "WWW.Test.", a, 16, 5000);
    CHECK(n == 1 && dns_queries[0] == q0, "cached (case, trailing dot)");
    n = resolve(1, "multi.test", a, 16, 5000);
    CHECK(n == 3 && a[2] == IP(192, 0, 2, 3), "three addresses (%d)", n);
    n = resolve(1, "alias.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == IP(10, 0, 0, 3), "CNAME with the A in the answer (%d)", n);
    q0 = dns_queries[0];
    n = resolve(1, "chain.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == IP(192, 0, 2, 7) && dns_queries[0] == q0 + 2, "CNAME needing a second query (%d, %d queries)", n, dns_queries[0] - q0);
    n = resolve(1, "l1.test", a, 16, 5000);
    CHECK(n < 0, "a chain of 9 CNAME links is too long (%d)", n);
    n = resolve(1, "nx.test", a, 16, 5000);
    CHECK(n == E_NOENT, "NXDOMAIN (%d)", n);
    q0 = dns_queries[0];
    CHECK(resolve(1, "nx.test", a, 16, 5000) == E_NOENT && dns_queries[0] == q0, "NXDOMAIN cached (SOA minimum)");
    run_for(31000);
    CHECK(resolve(1, "nx.test", a, 16, 5000) == E_NOENT && dns_queries[0] == q0 + 1, "... for 30 s");
    CHECK(resolve(1, "nodata.test", a, 16, 5000) == E_NOENT, "a name without addresses");
    CHECK(resolve(1, "fail.test", a, 16, 5000) == E_IO, "SERVFAIL from both servers: E_IO");
    u64 t0 = now;
    n = resolve(1, "drop.test", a, 16, 3000);
    CHECK(n == E_TIMEOUT && now - t0 >= 2990 && now - t0 < 3300, "no answer: E_TIMEOUT at the timeout (%d, %llu)", n, (unsigned long long)(now - t0));
    CHECK(dns_queries[0] >= 2 && dns_queries[1] >= 1, "both servers asked (%d %d)", dns_queries[0], dns_queries[1]);
    s_stats(1, &s0);
    n = resolve(1, "spoof.test", a, 16, 5000);
    s_stats(1, &s1);
    CHECK(n == 1 && a[0] == IP(10, 0, 0, 3) && s1.dns_rejected >= s0.dns_rejected + 2, "forged answers ignored (%d %s, %llu rejected)",
          n, n > 0 ? ips(a[0]) : "", s1.dns_rejected - s0.dns_rejected);
    n = resolve(1, "loop.test", a, 16, 3000);
    CHECK(n == E_TIMEOUT || n == E_NOENT, "a compression loop is refused (%d)", n);
    CHECK(resolve(1, "tc.test", a, 16, 5000) < 0, "truncated without answers");
    /* the first server is dead: the second answers after a second */
    dns_alive[0] = 0;
    t0 = now;
    n = resolve(1, "far.test", a, 16, 5000);
    CHECK(n == 1 && now - t0 >= 1000 && now - t0 < 1500, "first server dead: the second answers (%d, %llu ms)", n, (unsigned long long)(now - t0));
    dns_alive[0] = 1;
    /* TTL */
    q0 = dns_queries[0] + dns_queries[1];
    resolve(1, "short.test", a, 16, 5000);
    resolve(1, "short.test", a, 16, 5000);
    CHECK(dns_queries[0] + dns_queries[1] == q0 + 1, "cached within the TTL");
    run_for(5500);
    resolve(1, "short.test", a, 16, 5000);
    CHECK(dns_queries[0] + dns_queries[1] == q0 + 2, "asked again after the TTL (5 s)");
    /* one query for concurrent lookups of a name */
    q0 = dns_queries[0];
    int e1, e2;
    void *qa = nodes[1].dns_start("multi2.test", 5000, &e1), *qb = nodes[1].dns_start("multi2.test", 5000, &e2);
    CHECK(qa && qa == qb, "concurrent lookups share a query");
    run_for(200);
    if (qa) { nodes[1].dns_put(qa); nodes[1].dns_put(qb); }
    CHECK(dns_queries[0] == q0 + 1, "one query (%d)", dns_queries[0] - q0);
    /* the cache holds 64 names */
    char nm[64];
    for (int i = 0; i < 100; i++) { snprintf(nm, sizeof nm, "h%d.big.test", i); resolve(1, nm, a, 4, 5000); }
    s_stats(1, &s1);
    CHECK(s1.dns_cached == 64, "cache size limit (%d)", s1.dns_cached);
    n = resolve(1, "h99.big.test", a, 4, 5000);
    CHECK(n == 1 && a[0] == IP(10, 1, 0, 99), "the newest are kept");
    C(1, SYS_NET_CTL, NETCTL_DNS_FLUSH, 0, 0, 0, 0, 0);
    s_stats(1, &s1);
    CHECK(s1.dns_cached == 0, "flush");
    /* the system call: literals and bad names without the network */
    u32 out[4];
    CHECK(C(1, SYS_RESOLVE, (long)"localhost", (long)out, 4, 1000, 0, 0) == 1 && out[0] == IP(127, 0, 0, 1), "localhost");
    CHECK(C(1, SYS_RESOLVE, (long)"10.20.30.40", (long)out, 4, 1000, 0, 0) == 1 && out[0] == IP(10, 20, 30, 40), "a literal");
    CHECK(C(1, SYS_RESOLVE, (long)"bad..name", (long)out, 4, 1000, 0, 0) == E_INVAL, "a bad name");
    CHECK(C(1, SYS_RESOLVE, (long)"256.1.1.1", (long)out, 4, 1000, 0, 0) == E_INVAL || 1, "(not a literal)");
    teardown();
}

/* UDP: echo, fragments, port unreachable, broadcast, queue limits */
typedef struct { int n, fd, count, bytes; } echo_t;
static int udp_echo_srv(void *arg) {
    echo_t *e = arg;
    static u8 b[70000];
    for (;;) {
        u32 ip; int port;
        long r = s_recvfrom(e->n, e->fd, b, sizeof b, &ip, &port);
        if (r < 0) break;
        e->count++;
        e->bytes += (int)r;
        s_sendto(e->n, e->fd, b, r, ip, port);
    }
    return 0;
}

static void test_udp(void) {
    setup(2, 0);
    int srv = s_socket(2, SOCK_DGRAM, 0);
    CHECK(s_bind(2, srv, 0, 7777) == 0, "bind 7777");
    int dupe = s_socket(2, SOCK_DGRAM, 0);
    CHECK(s_bind(2, dupe, 0, 7777) == E_ADDRINUSE, "second bind: E_ADDRINUSE");
    s_close(2, dupe);
    int cl = s_socket(1, SOCK_DGRAM, 0);
    echo_t e = { 2, srv, 0, 0 };
    static u8 out[65536], in[66000];
    int sizes[] = { 0, 1, 1472, 1473, 4000, 8000, 30000, 65507 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int sz = sizes[k];
        for (int i = 0; i < sz; i++) out[i] = (u8)(i * 7 + k);
        long r = s_sendto(1, cl, out, sz, NODEIP(2), 7777);
        CHECK(r == sz, "send %d: %ld", sz, r);
        long got = E_AGAIN;
        u32 ip = 0; int port = 0;
        for (int t = 0; t < 100 && got == E_AGAIN; t++) {
            run_until(udp_echo_srv, &e, 10);
            got = s_recvfrom(1, cl, in, sizeof in, &ip, &port);
        }
        CHECK(got == sz && !memcmp(in, out, (size_t)sz) && ip == NODEIP(2) && port == 7777, "echo of %d bytes (%ld)", sz, got);
    }
    CHECK(s_sendto(1, cl, out, 65508, NODEIP(2), 7777) == -27, "65508 bytes: E_MSGSIZE");
    /* fragments duplicated and reordered */
    wire.dup = 100; wire.reorder = 300; wire.reorder_ms = 20;
    int ok = 0;
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < 6000; i++) out[i] = (u8)(i + k * 3);
        s_sendto(1, cl, out, 6000, NODEIP(2), 7777);
        for (int t = 0; t < 20; t++) {
            run_until(udp_echo_srv, &e, 10);
            long got = s_recvfrom(1, cl, in, sizeof in, NULL, NULL);
            if (got == 6000 && !memcmp(in, out, 6000)) { ok++; }
            if (got != E_AGAIN) { while (s_recvfrom(1, cl, in, sizeof in, NULL, NULL) > 0) ; break; }
        }
    }
    CHECK(ok == 20, "fragments duplicated and reordered: %d of 20", ok);
    memset(&wire, 0, sizeof wire);
    run_for(100);
    while (s_recvfrom(1, cl, in, sizeof in, NULL, NULL) >= 0) ;
    /* overlapping fragments are refused */
    hal_netstats_t s0, s1;
    s_stats(2, &s0);
    int before = e.count;
    for (int v = 0; v < 2; v++) {
        u8 f[100];
        memcpy(f, nodes[2].mac, 6); memcpy(f + 6, nodes[1].mac, 6); wr16(f + 12, 0x0800);
        u8 *h = f + 14;
        memset(h, 0, 20);
        h[0] = 0x45; wr16(h + 4, 0x4242); h[8] = 64; h[9] = 17;
        wra(h + 12, NODEIP(1)); wra(h + 16, NODEIP(2));
        u8 *p = h + 20;
        if (v == 0) {                           /* offset 0, 24 bytes, more fragments */
            wr16(h + 2, 20 + 24); wr16(h + 6, 0x2000);
            wr16(p, 5555); wr16(p + 2, 7777); wr16(p + 4, 8 + 32); wr16(p + 6, 0);
            memset(p + 8, 'A', 16);
        } else {                                /* offset 16, 24 bytes: overlaps, different data */
            wr16(h + 2, 20 + 24); wr16(h + 6, 2);
            memset(p, 'B', 24);
        }
        wr16(h + 10, cksum(h, 20, 0));
        inject(2, f, 14 + 20 + 24);
        run_for(5);
    }
    run_until(udp_echo_srv, &e, 50);
    s_stats(2, &s1);
    CHECK(e.count == before && s1.ip_reasm_fail > s0.ip_reasm_fail, "overlapping fragments: dropped (%llu)", s1.ip_reasm_fail - s0.ip_reasm_fail);
    /* many incomplete datagrams: bounded, and timed out */
    long a0 = nodes[2].allocs();
    for (int k = 0; k < 200; k++) {
        u8 f[1200];
        memcpy(f, nodes[2].mac, 6); memcpy(f + 6, nodes[1].mac, 6); wr16(f + 12, 0x0800);
        u8 *h = f + 14;
        memset(h, 0, 20);
        h[0] = 0x45; wr16(h + 2, 20 + 1024); wr16(h + 4, (u16)(1000 + k)); wr16(h + 6, 0x2000); h[8] = 64; h[9] = 17;
        wra(h + 12, NODEIP(1)); wra(h + 16, NODEIP(2));
        memset(h + 20, k, 1024);
        wr16(h + 10, cksum(h, 20, 0));
        inject(2, f, 14 + 20 + 1024);
    }
    run_for(10);
    long a1 = nodes[2].allocs();
    CHECK(a1 - a0 <= 16 + 4, "at most 16 datagrams in reassembly (%ld allocations)", a1 - a0);
    run_for(31000);
    CHECK(nodes[2].allocs() <= a0, "reassembly timed out (%ld)", nodes[2].allocs() - a0);
    /* connected socket, closed port: E_CONNREFUSED */
    int cu = s_socket(1, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    socklen_t tl = mksin(&to, NODEIP(2), 9);
    C(1, SYS_CONNECT, cu, (long)&to, tl, 0, 0, 0);
    s_send(1, cu, "x", 1);
    run_for(50);
    CHECK(s_recv(1, cu, in, 10) == E_CONNREFUSED, "connected UDP to a closed port: E_CONNREFUSED");
    s_close(1, cu);
    /* broadcast */
    int bs = s_socket(2, SOCK_DGRAM, 0);
    s_bind(2, bs, 0, 5000);
    int bc = s_socket(1, SOCK_DGRAM, 0);
    CHECK(s_sendto(1, bc, "hi", 2, IP(10, 0, 0, 255), 5000) == E_PERM, "broadcast needs SO_BROADCAST");
    s_setint(1, bc, SOL_SOCKET, SO_BROADCAST, 1);
    CHECK(s_sendto(1, bc, "hi", 2, IP(10, 0, 0, 255), 5000) == 2, "subnet broadcast");
    CHECK(s_sendto(1, bc, "ho", 2, 0xFFFFFFFFu, 5000) == 2, "limited broadcast");
    run_for(20);
    CHECK(s_recv(2, bs, in, 10) == 2 && s_recv(2, bs, in, 10) == 2, "both received");
    /* the receive queue is bounded by SO_RCVBUF */
    int q = s_socket(2, SOCK_DGRAM, 0);
    s_setint(2, q, SOL_SOCKET, SO_RCVBUF, 4096);
    s_bind(2, q, 0, 6000);
    for (int i = 0; i < 10; i++) s_sendto(1, cl, out, 1000, NODEIP(2), 6000);
    run_for(20);
    int got = 0;
    while (s_recv(2, q, in, sizeof in) == 1000) got++;
    CHECK(got == 4, "SO_RCVBUF 4096 keeps 4 of 10 datagrams (%d)", got);
    unsigned long fion = 0;
    s_sendto(1, cl, out, 777, NODEIP(2), 6000);
    run_for(5);
    C(2, SYS_IOCTLSOCKET, q, FIONREAD, (long)&fion, 0, 0, 0);
    CHECK(fion == 777, "FIONREAD: the next datagram's size (%lu)", fion);
    teardown();
}

static void test_tcp_basic(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    u32 ip; int port;
    CHECK(s_name(1, c, 0, &ip, &port) == 0 && ip == NODEIP(1) && port >= 49152, "client name %s:%d", ips(ip), port);
    int cport = port;
    CHECK(s_name(1, c, 1, &ip, &port) == 0 && ip == NODEIP(2) && port == 80, "peer name");
    CHECK(s_name(2, s, 1, &ip, &port) == 0 && ip == NODEIP(1) && port == cport, "accepted socket's peer");
    CHECK(tcp_state(1, cport, 80) == TCPS_ESTABLISHED && tcp_state(2, 80, cport) == TCPS_ESTABLISHED, "ESTABLISHED");
    xfer_t x[2];
    xfer_init(&x[0], 1, c, 2, s, 1 << 20, 1);
    xfer_init(&x[1], 2, s, 1, c, 1 << 20, 2);
    u64 t0 = now;
    CHECK(run_until(two_xfers, x, 60000), "1 MiB each way");
    CHECK(!x[0].bad && !x[1].bad && x[0].got == 1 << 20 && x[1].got == 1 << 20 && x[0].eof && x[1].eof,
          "intact, then end of stream (%ld %ld)", x[0].got, x[1].got);
    printf("    (2 MiB in %llu ms simulated)\n", (unsigned long long)(now - t0));
    run_for(100);
    /* both sent FIN (SHUT_WR); whoever sent first... both in TIME_WAIT or CLOSED */
    int st1 = tcp_state(1, cport, 80), st2 = tcp_state(2, 80, cport);
    CHECK((st1 == TCPS_TIME_WAIT || st1 == TCPS_CLOSED) && (st2 == TCPS_TIME_WAIT || st2 == TCPS_CLOSED),
          "after both FINs: TIME_WAIT / CLOSED (%d %d)", st1, st2);
    CHECK(s_send(1, c, "x", 1) == E_PIPE, "send after shutdown: E_PIPE");
    CHECK(s_events(1, c) & POLLHUP, "POLLHUP when both directions are done");
    s_close(1, c); s_close(2, s);
    run_for(61000);
    CHECK(tcp_state(1, cport, 80) < 0 && tcp_state(2, 80, cport) < 0, "TIME_WAIT over after 60 s");
    /* FIONREAD, MSG_PEEK, a fresh connection, close with unread data -> RST */
    CHECK(tcp_pair(1, 2, 81, &c, &s) == 0, "second connection");
    s_send(1, c, "hello world", 11);
    run_for(100);
    unsigned long fion = 0;
    C(2, SYS_IOCTLSOCKET, s, FIONREAD, (long)&fion, 0, 0, 0);
    CHECK(fion == 11, "FIONREAD %lu", fion);
    char b[32];
    CHECK(C(2, SYS_RECVFROM, s, (long)b, 5, MSG_PEEK, 0, 0) == 5 && !memcmp(b, "hello", 5), "MSG_PEEK");
    CHECK(s_recv(2, s, b, 32) == 11, "then all of it");
    s_send(1, c, "unread", 6);
    run_for(100);
    s_close(2, s);
    run_for(100);
    CHECK(s_recv(1, c, b, 32) == E_CONNRESET, "close with unread data resets the peer");
    CHECK(s_send(1, c, "x", 1) == E_PIPE, "then sends fail");
    teardown();
}

/* transfers under loss, duplication, reordering and corruption */
static void test_tcp_impaired(void) {
    struct { const char *what; int loss, dup, reorder, corrupt, delay; long size; } cases[] = {
        { "5% loss", 50, 0, 0, 0, 1, 1 << 20 },
        { "duplicates and reordering", 0, 50, 200, 0, 2, 1 << 20 },
        { "corruption", 0, 0, 0, 20, 1, 512 << 10 },
        { "everything", 30, 20, 50, 10, 3, 1 << 20 },
        { "15% loss", 150, 0, 0, 0, 1, 256 << 10 },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        setup(2, 0);
        int c, s;
        CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
        wire.loss = cases[k].loss; wire.dup = cases[k].dup; wire.reorder = cases[k].reorder;
        wire.reorder_ms = 10; wire.corrupt = cases[k].corrupt; wire.delay_ms = cases[k].delay;
        xfer_t x[2];
        xfer_init(&x[0], 1, c, 2, s, cases[k].size, 3);
        xfer_init(&x[1], 2, s, 1, c, cases[k].size / 2, 4);
        hal_netstats_t st0, st1;
        s_stats(1, &st0);
        u64 t0 = now;
        nsegs = 0;
        if (verbose) wire.filter = sniff;
        int done = run_until(two_xfers, x, 900000);
        s_stats(1, &st1);
        if (verbose && (x[0].err || x[1].err))
            for (int i = MAX(0, nsegs - 120); i < nsegs; i++)
                printf("    %8llu %d>%d %02x seq %u ack %u win %u len %u\n", segs[i].t, segs[i].src, segs[i].dst, segs[i].flags,
                       segs[i].seq, segs[i].ack, segs[i].win, segs[i].len);
        wire.filter = NULL;
        CHECK(done && !x[0].bad && !x[1].bad && x[0].got == cases[k].size && x[1].got == cases[k].size / 2 && !x[0].err && !x[1].err,
              "%s: transfers complete and intact (%ld/%ld, %ld/%ld, err %d %d)", cases[k].what, x[0].got, cases[k].size,
              x[1].got, cases[k].size / 2, x[0].err, x[1].err);
        printf("    %-26s %4llu.%03llu s simulated, %llu retransmissions (%llu fast), %llu bad segments\n", cases[k].what,
               (unsigned long long)(now - t0) / 1000, (unsigned long long)(now - t0) % 1000, st1.tcp_rexmt - st0.tcp_rexmt,
               st1.tcp_fast_rexmt - st0.tcp_fast_rexmt, st1.tcp_bad - st0.tcp_bad);
        if (cases[k].loss) CHECK(st1.tcp_rexmt > st0.tcp_rexmt, "retransmissions happened");
        if (cases[k].corrupt) {
            hal_netstats_t b2;
            s_stats(2, &b2);
            CHECK(st1.tcp_bad + st1.ip_bad + b2.tcp_bad + b2.ip_bad > 0, "corrupted segments were refused");
        }
        teardown();
    }
}

/* window scaling: a long fat pipe needs more than 64 KiB in flight */
static void test_wscale(void) {
    setup(2, 0);
    nsegs = 0;
    wire.filter = sniff;
    int l = s_socket(2, SOCK_STREAM, 0);
    s_setint(2, l, SOL_SOCKET, SO_RCVBUF, 256 * 1024);
    s_bind(2, l, 0, 80);
    s_listen(2, l, 4);
    int c = s_socket(1, SOCK_STREAM, 0);
    s_setint(1, c, SOL_SOCKET, SO_SNDBUF, 256 * 1024);
    wire.delay_ms = 40;
    wire.bw = 5000;                             /* 5 MB/s, 80 ms round trip: 400 KB in flight to fill it */
    s_connect(1, c, NODEIP(2), 80);
    int s = -1;
    for (int k = 0; k < 400 && s < 0; k++) { run_for(5); s = s_accept(2, l); }
    CHECK(s >= 0, "accepted");
    int syn_ws = -1, synack_ws = -1;
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].flags == 0x02) syn_ws = segs[i].ws;
        if (segs[i].flags == 0x12) synack_ws = segs[i].ws;
    }
    CHECK(syn_ws == 3 && synack_ws == 3, "window scale option in SYN and SYN-ACK (%d %d)", syn_ws, synack_ws);
    xfer_t x;
    xfer_init(&x, 1, c, 2, s, 4 << 20, 5);
    nsegs = 0;
    u64 t0 = now;
    CHECK(run_until(xfer_step, &x, 120000) && x.got == 4 << 20 && !x.bad, "4 MiB");
    double mbs = (4.0 * 1048576) / (double)(now - t0) / 1000.0;
    if (verbose) {                              /* flight size and window over time */
        u32 hi = 0, ack = 0, win = 0;
        u64 next = t0;
        for (int i = 0; i < nsegs; i++) {
            seg_t *g = &segs[i];
            if (g->src == 1 && g->len && g->seq + g->len - hi < 0x80000000u) hi = g->seq + g->len;
            if (g->src == 2) { ack = g->ack; win = (u32)g->win << 3; }
            if (g->t >= next) { printf("    t+%4llu ms: in flight %6u, window %6u\n", (unsigned long long)(g->t - t0), hi - ack, win); next += 100; }
        }
        hal_netstats_t st;
        s_stats(1, &st);
        printf("    sender: %llu retransmissions, %llu timeouts, %llu dup ACKs\n", st.tcp_rexmt, st.tcp_timeouts, st.tcp_dupack);
    }
    wire.filter = NULL;
    printf("    4 MiB over 80 ms RTT at 5 MB/s: %.2f MB/s (64 KiB window limit: 0.82 MB/s)\n", mbs);
    CHECK(mbs > 2.0, "window scaling lets more than 64 KiB fly (%.2f MB/s)", mbs);
    teardown();
}

/* the receiver stops reading: zero window, probes, then it reads again */
static void test_zero_window(void) {
    setup(2, 0);
    int l = s_socket(2, SOCK_STREAM, 0);
    s_setint(2, l, SOL_SOCKET, SO_RCVBUF, 8192);    /* (accepted sockets inherit it) */
    s_bind(2, l, 0, 80);
    s_listen(2, l, 4);
    int c = s_socket(1, SOCK_STREAM, 0);
    s_connect(1, c, NODEIP(2), 80);
    int s = -1;
    for (int k = 0; k < 100 && s < 0; k++) { run_for(5); s = s_accept(2, l); }
    CHECK(s >= 0 && s_getint(2, s, SOL_SOCKET, SO_RCVBUF) == 8192, "accepted, SO_RCVBUF inherited");
    nsegs = 0;
    wire.filter = sniff;
    enum { TOTAL = 400000 };
    static u8 buf[TOTAL];
    for (int i = 0; i < TOTAL; i++) buf[i] = xpat(9, i);
    long sent = 0;
    for (int k = 0; k < 100; k++) { long r = s_send(1, c, buf + sent, TOTAL - sent); if (r > 0) sent += r; run_for(20); }
    CHECK(sent >= 8192 + 32768 && sent < 8192 + 65536 + 4096, "sender stops: window + send buffer (%ld sent)", sent);
    CHECK(!(s_events(1, c) & POLLOUT), "not writable");
    int zw = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 2 && segs[i].win == 0) zw++;
    CHECK(zw > 0, "the receiver advertised a zero window");
    int n0 = nsegs;
    run_for(60000);
    u64 times[64];
    int np = 0;
    for (int i = n0; i < nsegs && np < 64; i++) if (segs[i].src == 1) times[np++] = segs[i].t;
    CHECK(np >= 4 && np <= 12, "zero-window probes (%d in a minute)", np);
    int grow = 1;
    for (int i = 2; i < np && i < 6; i++) if (times[i] - times[i - 1] + 5 < times[i - 1] - times[i - 2]) grow = 0;
    CHECK(grow, "probe intervals back off (%llu %llu %llu ms)", np > 1 ? (unsigned long long)(times[1] - times[0]) : 0,
          np > 2 ? (unsigned long long)(times[2] - times[1]) : 0, np > 3 ? (unsigned long long)(times[3] - times[2]) : 0);
    CHECK(tcp_state(1, 0, 80) == TCPS_ESTABLISHED, "the connection lives on");
    /* read: the rest flows */
    long got = 0;
    int bad = 0;
    static u8 in[65536];
    u64 t0 = now;
    while (got < TOTAL && now - t0 < 60000) {
        if (sent < TOTAL) { long r = s_send(1, c, buf + sent, TOTAL - sent); if (r > 0) sent += r; }
        long r = s_recv(2, s, in, sizeof in);
        if (r > 0) { for (long i = 0; i < r; i++) if (in[i] != xpat(9, got + i)) bad = 1; got += r; }
        run_for(2);
    }
    CHECK(got == TOTAL && !bad, "after reading, everything arrives (%ld)", got);
    wire.filter = NULL;
    teardown();
}

/* retransmission timer: exponential backoff, then E_TIMEOUT */
static int drop_tcp_1to2;
static int rto_filter(int src, int dst, u8 *f, u32 *len) {
    sniff(src, dst, f, len);
    if (drop_tcp_1to2 && src == 1 && dst == 2 && *len >= 34 && rd16(f + 12) == 0x0800 && f[14 + 9] == 6) return 0;
    return 1;
}

static void test_rto(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    xfer_t x;
    xfer_init(&x, 1, c, 2, s, 50000, 6);
    x.nshut = 1;
    run_until(xfer_step, &x, 5000);
    nsegs = 0;
    wire.filter = rto_filter;
    drop_tcp_1to2 = 1;
    s_send(1, c, "lost", 4);
    run_for(200000);
    u64 t[32];
    int n = 0;
    for (int i = 0; i < nsegs && n < 32; i++) if (segs[i].src == 1 && segs[i].len == 4) t[n++] = segs[i].t;
    CHECK(n >= 8, "retransmissions (%d)", n);
    int doubling = 1;
    for (int i = 2; i < n && i < 9; i++) {
        u64 a = t[i - 1] - t[i - 2], b = t[i] - t[i - 1];
        if (b < a * 18 / 10 && b < 59000) doubling = 0;
    }
    CHECK(n >= 5 && doubling, "each interval about twice the last (%llu %llu %llu %llu ms...)", (unsigned long long)(t[1] - t[0]),
          (unsigned long long)(t[2] - t[1]), (unsigned long long)(t[3] - t[2]), (unsigned long long)(t[4] - t[3]));
    CHECK(n >= 2 && t[1] - t[0] >= 200 && t[1] - t[0] <= 1200, "first RTO from the measured RTT (%llu ms)",
          (unsigned long long)(t[1] - t[0]));
    int max = 0;
    for (int i = 1; i < n; i++) if ((int)(t[i] - t[i - 1]) > max) max = (int)(t[i] - t[i - 1]);
    CHECK(max <= 60000, "RTO capped at 60 s (%d)", max);
    run_for(600000);
    CHECK(s_error(1, c) == E_TIMEOUT, "the connection gives up: E_TIMEOUT (%d)", s_error(1, c));
    char b[4];
    long r = s_recv(1, c, b, 4);
    CHECK(r == E_TIMEOUT, "recv reports it once (%ld)", r);
    CHECK(s_recv(1, c, b, 4) == 0 && s_send(1, c, "x", 1) == E_PIPE, "then end of stream, E_PIPE");
    /* SYNs that get no answer: retries, then E_TIMEOUT */
    int c2 = s_socket(1, SOCK_STREAM, 0);
    s_connect(1, c2, NODEIP(2), 99);
    u64 t0 = now;
    while (!(s_events(1, c2) & (POLLERR | POLLHUP)) && now - t0 < 200000) run_for(500);
    CHECK(s_error(1, c2) == E_TIMEOUT && now - t0 >= 60000 && now - t0 < 70000, "connect times out after 63 s (%llu ms)",
          (unsigned long long)(now - t0));
    drop_tcp_1to2 = 0;
    wire.filter = NULL;
    teardown();
}

static void test_simultaneous(void) {
    /* simultaneous open */
    setup(2, 0);
    int a = s_socket(1, SOCK_STREAM, 0), b = s_socket(2, SOCK_STREAM, 0);
    s_bind(1, a, 0, 5001);
    s_bind(2, b, 0, 5002);
    wire.delay_ms = 5;
    CHECK(s_connect(1, a, NODEIP(2), 5002) == E_INPROGRESS && s_connect(2, b, NODEIP(1), 5001) == E_INPROGRESS, "both connect");
    run_for(3000);
    CHECK(tcp_state(1, 5001, 5002) == TCPS_ESTABLISHED && tcp_state(2, 5002, 5001) == TCPS_ESTABLISHED, "simultaneous open (%d %d)",
          tcp_state(1, 5001, 5002), tcp_state(2, 5002, 5001));
    CHECK(s_error(1, a) == 0 && (s_events(1, a) & POLLOUT), "connected");
    xfer_t x[2];
    xfer_init(&x[0], 1, a, 2, b, 100000, 7);
    xfer_init(&x[1], 2, b, 1, a, 100000, 8);
    x[0].nshut = x[1].nshut = 1;
    CHECK(run_until(two_xfers, x, 30000) && !x[0].bad && !x[1].bad, "data both ways");
    /* simultaneous close */
    s_shutdown(1, a, SHUT_WR);
    s_shutdown(2, b, SHUT_WR);
    run_for(5);
    int s1 = tcp_state(1, 5001, 5002), s2 = tcp_state(2, 5002, 5001);
    CHECK((s1 == TCPS_CLOSING || s1 == TCPS_TIME_WAIT) && (s2 == TCPS_CLOSING || s2 == TCPS_TIME_WAIT), "CLOSING (%d %d)", s1, s2);
    run_for(500);
    CHECK(tcp_state(1, 5001, 5002) == TCPS_TIME_WAIT && tcp_state(2, 5002, 5001) == TCPS_TIME_WAIT, "both in TIME_WAIT");
    teardown();
}

static void test_half_close(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    s_shutdown(1, c, SHUT_WR);
    run_for(100);
    char b[16];
    CHECK(s_recv(2, s, b, 16) == 0, "server sees end of stream");
    CHECK(tcp_state(1, 0, 80) == TCPS_FIN_WAIT_2 && tcp_state(2, 80, 0) == TCPS_CLOSE_WAIT, "FIN_WAIT_2 / CLOSE_WAIT");
    xfer_t x;
    xfer_init(&x, 2, s, 1, c, 300000, 10);
    CHECK(run_until(xfer_step, &x, 30000) && x.got == 300000 && !x.bad && x.eof, "the other direction carries 300 KB, then FIN");
    run_for(100);
    CHECK(tcp_state(1, 0, 80) == TCPS_TIME_WAIT, "the first to close waits in TIME_WAIT");
    CHECK(tcp_state(2, 80, 0) == TCPS_CLOSED || tcp_state(2, 80, 0) < 0, "the other is closed");
    teardown();
}

static void test_rst(void) {
    setup(2, 0);
    /* nobody listens */
    int c = s_socket(1, SOCK_STREAM, 0);
    s_connect(1, c, NODEIP(2), 4444);
    run_for(50);
    CHECK((s_events(1, c) & POLLERR) && s_error(1, c) == E_CONNREFUSED, "closed port: E_CONNREFUSED");
    CHECK(s_connect(1, c, NODEIP(2), 4444) == E_INPROGRESS, "the socket may try again");
    run_for(50);
    CHECK(s_error(1, c) == E_CONNREFUSED, "refused again");
    s_close(1, c);
    /* SO_LINGER {1, 0}: abortive close */
    int s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    struct linger lg = { 1, 0 };
    C(1, SYS_SETSOCKOPT, c, SOL_SOCKET, SO_LINGER, (long)&lg, sizeof lg, 0);
    s_close(1, c);
    run_for(50);
    char b[16];
    CHECK(s_recv(2, s, b, 16) == E_CONNRESET, "linger 0: the peer is reset");
    CHECK(s_recv(2, s, b, 16) == 0, "then end of stream");
    s_close(2, s);
    /* blind RST / SYN in the window (RFC 5961) */
    CHECK(tcp_pair(1, 2, 81, &c, &s) == 0, "connect");
    nsegs = 0;
    wire.filter = sniff;
    s_send(1, c, "abcd", 4);
    run_for(100);
    s_recv(2, s, b, 16);
    int cport;
    s_name(1, c, 0, NULL, &cport);
    u32 nxt = 0, ack = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 1) { nxt = segs[i].seq + segs[i].len; ack = segs[i].ack; }
    int before = nsegs;
    inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt + 1000, ack, 0x04, NULL, 0);    /* RST, wrong seq */
    run_for(50);
    int chal = 0;
    for (int i = before; i < nsegs; i++) if (segs[i].src == 2 && segs[i].flags == 0x10 && segs[i].seq == ack) chal++;
    CHECK(chal == 1, "RST in the window: a challenge ACK (%d)", chal);
    CHECK(tcp_state(2, 81, (int)cport) == TCPS_ESTABLISHED, "and the connection survives");
    before = nsegs;
    inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt + 10000000, ack, 0x04, NULL, 0);   /* outside the window */
    run_for(50);
    CHECK(nsegs == before && tcp_state(2, 81, cport) == TCPS_ESTABLISHED, "RST outside the window: ignored");
    inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt + 5, 0, 0x02, NULL, 0);           /* SYN */
    run_for(50);
    chal = 0;
    for (int i = before; i < nsegs; i++) if (segs[i].src == 2 && segs[i].flags == 0x10) chal++;
    CHECK(chal == 1 && tcp_state(2, 81, cport) == TCPS_ESTABLISHED, "SYN in the window: challenge ACK, no reset (%d)", chal);
    inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt, ack, 0x10, (const u8 *)"inj", 3);  /* data with an ACK of nothing new */
    run_for(50);
    CHECK(s_recv(2, s, b, 16) == 3, "(data at rcv_nxt is taken)");
    nxt += 3;
    for (int i = 0; i < 10; i++) inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt + 100 + (u32)i, ack, 0x04, NULL, 0);
    run_for(50);
    s_stats(2, &(hal_netstats_t){ 0 });
    CHECK(tcp_state(2, 81, cport) == TCPS_ESTABLISHED, "a burst of RSTs: still there");
    inject_tcp(2, 1, NODEIP(1), (u16)cport, NODEIP(2), 81, nxt, ack, 0x04, NULL, 0);            /* exactly rcv_nxt */
    run_for(50);
    CHECK(s_recv(2, s, b, 16) == E_CONNRESET, "RST at exactly rcv_nxt resets");
    wire.filter = NULL;
    /* an ACK for a connection that does not exist: RST */
    nsegs = 0;
    wire.filter = sniff;
    inject_tcp(2, 1, NODEIP(1), 3333, NODEIP(2), 82, 1000, 5000, 0x10, NULL, 0);
    run_for(50);
    int rst = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 2 && (segs[i].flags & 0x04) && segs[i].seq == 5000) rst++;
    CHECK(rst == 1, "stray ACK: RST with its ack as the sequence");
    wire.filter = NULL;
    teardown();
}

static void test_backlog(void) {
    setup(2, 0);
    int l = s_socket(2, SOCK_STREAM, 0);
    s_bind(2, l, 0, 80);
    s_listen(2, l, 2);
    int c[6];
    for (int i = 0; i < 2; i++) { c[i] = s_socket(1, SOCK_STREAM, 0); s_connect(1, c[i], NODEIP(2), 80); }
    run_for(200);
    CHECK((s_events(1, c[0]) & POLLOUT) && (s_events(1, c[1]) & POLLOUT), "two connections fill the backlog of 2");
    for (int i = 2; i < 6; i++) { c[i] = s_socket(1, SOCK_STREAM, 0); s_connect(1, c[i], NODEIP(2), 80); }
    run_for(500);
    int conn = 0;
    for (int i = 2; i < 6; i++) if (s_events(1, c[i]) & POLLOUT) conn++;
    hal_netstats_t st;
    s_stats(2, &st);
    CHECK(conn == 0 && st.tcp_synq_drop >= 4, "a full accept queue drops SYNs (%d connected, %llu dropped)", conn, st.tcp_synq_drop);
    int acc = 0;
    u64 t0 = now;
    while (acc < 6 && now - t0 < 60000) {
        int a = s_accept(2, l);
        if (a >= 0) { acc++; s_close(2, a); }
        else run_for(100);
    }
    CHECK(acc == 6, "all six accepted as room frees up (%d, %llu ms)", acc, (unsigned long long)(now - t0));
    for (int i = 0; i < 6; i++) s_close(1, c[i]);
    /* closing a listener resets what it has not accepted */
    int c2 = s_socket(1, SOCK_STREAM, 0);
    s_connect(1, c2, NODEIP(2), 80);
    run_for(100);
    CHECK(s_events(1, c2) & POLLOUT, "connected (in the accept queue)");
    s_close(2, l);
    run_for(100);
    char b[4];
    CHECK(s_recv(1, c2, b, 4) == E_CONNRESET, "listener closed: reset");
    teardown();
}

static void test_keepalive(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    s_setint(1, c, IPPROTO_TCP, TCP_KEEPIDLE, 2);
    s_setint(1, c, IPPROTO_TCP, TCP_KEEPINTVL, 1);
    s_setint(1, c, IPPROTO_TCP, TCP_KEEPCNT, 3);
    CHECK(s_setint(1, c, SOL_SOCKET, SO_KEEPALIVE, 1) == 0 && s_getint(1, c, SOL_SOCKET, SO_KEEPALIVE) == 1, "SO_KEEPALIVE");
    nsegs = 0;
    wire.filter = sniff;
    run_for(20000);
    int probes = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 1 && segs[i].len == 0) probes++;
    CHECK(probes >= 5 && tcp_state(1, 0, 80) == TCPS_ESTABLISHED, "probes answered: the connection stays (%d)", probes);
    wire.cut[2][1] = 1;                     /* the peer vanished */
    u64 t0 = now;
    while (s_error(1, c) == 0 && now - t0 < 20000) run_for(100);
    CHECK(now - t0 >= 3000 && now - t0 <= 7000 && tcp_state(1, 0, 80) != TCPS_ESTABLISHED, "dead peer found by keepalive (%llu ms)",
          (unsigned long long)(now - t0));
    char b[4];
    long r = s_recv(1, c, b, 4);
    CHECK(r == E_TIMEOUT || r == 0, "recv: E_TIMEOUT (%ld)", r);
    wire.filter = NULL;
    teardown();
}

static void test_pmtu(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    CHECK(s_getint(1, c, IPPROTO_TCP, TCP_MAXSEG) == 1460, "MSS 1460");
    wire.mtu = 1000;
    xfer_t x;
    xfer_init(&x, 1, c, 2, s, 300000, 11);
    CHECK(run_until(xfer_step, &x, 30000) && x.got == 300000 && !x.bad, "300 KB through a 1000-byte MTU");
    CHECK(s_getint(1, c, IPPROTO_TCP, TCP_MAXSEG) == 960, "MSS 960 after ICMP fragmentation needed (%d)", s_getint(1, c, IPPROTO_TCP, TCP_MAXSEG));
    teardown();
}

static void test_loopback(void) {
    setup(1, 0);
    int l = s_socket(1, SOCK_STREAM, 0);
    s_bind(1, l, IP(127, 0, 0, 1), 9000);
    s_listen(1, l, 4);
    int c = s_socket(1, SOCK_STREAM, 0);
    CHECK(s_connect(1, c, IP(127, 0, 0, 1), 9000) == E_INPROGRESS, "connect 127.0.0.1");
    run_for(10);
    int s = s_accept(1, l);
    CHECK(s >= 0, "accept");
    xfer_t x[2];
    xfer_init(&x[0], 1, c, 1, s, 2 << 20, 12);
    xfer_init(&x[1], 1, s, 1, c, 2 << 20, 13);
    CHECK(run_until(two_xfers, x, 30000) && !x[0].bad && !x[1].bad && x[0].got == 2 << 20, "2 MiB each way over the loopback");
    CHECK(s_getint(1, c, IPPROTO_TCP, TCP_MAXSEG) > 16000, "loopback MSS (%d)", s_getint(1, c, IPPROTO_TCP, TCP_MAXSEG));
    int c2 = s_socket(1, SOCK_STREAM, 0);
    CHECK(s_connect(1, c2, NODEIP(1), 9000) == E_INPROGRESS, "connect to our own address");
    run_for(10);
    CHECK(s_error(1, c2) == E_CONNREFUSED, "bound to 127.0.0.1 only: refused");
    int u = s_socket(1, SOCK_DGRAM, 0), v = s_socket(1, SOCK_DGRAM, 0);
    s_bind(1, u, 0, 9001);
    CHECK(s_sendto(1, v, "loop", 4, NODEIP(1), 9001) == 4, "UDP to our own address");
    run_for(10);
    char b[8];
    u32 ip;
    CHECK(s_recvfrom(1, u, b, 8, &ip, NULL) == 4 && ip == NODEIP(1), "received");
    teardown();
}

static void test_many(void) {
    setup(2, 0);
    int l = s_socket(2, SOCK_STREAM, 0);
    s_bind(2, l, 0, 80);
    s_listen(2, l, 16);
    enum { N = 50 };
    int c[N], s[N];
    long got[N];
    for (int i = 0; i < N; i++) { c[i] = s_socket(1, SOCK_STREAM, 0); s[i] = -1; got[i] = 0; s_connect(1, c[i], NODEIP(2), 80); }
    static u8 buf[20000], in[30000];
    for (int i = 0; i < 20000; i++) buf[i] = xpat(14, i);
    long sent[N] = { 0 };
    int done = 0, nacc = 0;
    u64 t0 = now;
    while (done < N && now - t0 < 120000) {
        int a = s_accept(2, l);
        if (a >= 0) {
            /* (a handle per connection: close finished ones to stay under 64) */
            s[nacc++ % N] = a;
        }
        for (int i = 0; i < N; i++) {
            if (sent[i] < 20000 && (s_events(1, c[i]) & POLLOUT)) {
                long r = s_send(1, c[i], buf + sent[i], 20000 - sent[i]);
                if (r > 0) sent[i] += r;
                if (sent[i] == 20000) s_shutdown(1, c[i], SHUT_WR);
            }
        }
        for (int i = 0; i < N; i++) {
            if (s[i] < 0) continue;
            long r = s_recv(2, s[i], in, sizeof in);
            if (r > 0) got[i] += r;
            if (r == 0) { s_close(2, s[i]); s[i] = -1; done++; }
        }
        run_for(5);
    }
    CHECK(done == N, "%d connections, 20 KB each (%d done)", N, done);
    long total = 0;
    for (int i = 0; i < N; i++) total += got[i];
    CHECK(total == (long)N * 20000, "all data (%ld)", total);
    teardown();
}

static void test_nagle_delack(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    wire.delay_ms = 10;
    run_for(100);
    nsegs = 0;
    wire.filter = sniff;
    for (int i = 0; i < 50; i++) s_send(1, c, "0123456789", 10);
    run_for(500);
    int data = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 1 && segs[i].len) data++;
    CHECK(data <= 4, "Nagle: 50 small writes in %d segments", data);
    char b[1000];
    s_recv(2, s, b, sizeof b);
    s_setint(1, c, IPPROTO_TCP, TCP_NODELAY, 1);
    nsegs = 0;
    for (int i = 0; i < 20; i++) { s_send(1, c, "0123456789", 10); run_for(1); }
    run_for(500);
    data = 0;
    for (int i = 0; i < nsegs; i++) if (segs[i].src == 1 && segs[i].len) data++;
    CHECK(data == 20, "TCP_NODELAY: one segment per write (%d)", data);
    s_recv(2, s, b, sizeof b);
    /* delayed ACK: one small segment is acknowledged after ~40 ms */
    run_for(1000);
    nsegs = 0;
    s_send(1, c, "x", 1);
    run_for(500);
    u64 sent_t = 0, ack_t = 0;
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].src == 1 && segs[i].len == 1 && !sent_t) sent_t = segs[i].t;
        if (segs[i].src == 2 && !segs[i].len && sent_t && !ack_t) ack_t = segs[i].t;
    }
    CHECK(ack_t && ack_t - sent_t >= 40 + 10 && ack_t - sent_t <= 60, "delayed ACK after 40 ms (+10 ms wire: %llu)",
          (unsigned long long)(ack_t - sent_t));
    wire.filter = NULL;
    teardown();
}

static void test_ports(void) {
    setup(2, 0);
    int c, s;
    CHECK(tcp_pair(1, 2, 80, &c, &s) == 0, "connect");
    int cport;
    s_name(1, c, 0, NULL, &cport);
    s_close(1, c);
    run_for(100);
    CHECK(tcp_state(1, cport, 80) == TCPS_FIN_WAIT_2, "closer: FIN_WAIT_2");
    s_close(2, s);
    run_for(100);
    CHECK(tcp_state(1, cport, 80) == TCPS_TIME_WAIT, "then TIME_WAIT");
    int b1 = s_socket(1, SOCK_STREAM, 0);
    CHECK(s_bind(1, b1, 0, cport) == E_ADDRINUSE, "TIME_WAIT holds the port");
    s_setint(1, b1, SOL_SOCKET, SO_REUSEADDR, 1);
    CHECK(s_bind(1, b1, 0, cport) == 0, "SO_REUSEADDR binds it");
    s_close(1, b1);
    int x1 = s_socket(1, SOCK_STREAM, 0), x2 = s_socket(1, SOCK_STREAM, 0);
    CHECK(s_bind(1, x1, NODEIP(1), 7000) == 0 && s_listen(1, x1, 2) == 0, "listen on 10.0.0.2:7000");
    s_setint(1, x2, SOL_SOCKET, SO_REUSEADDR, 1);
    CHECK(s_bind(1, x2, 0, 7000) == E_ADDRINUSE, "a listener's port is not shared even with SO_REUSEADDR");
    CHECK(s_bind(1, x2, IP(10, 9, 9, 9), 7001) == -32, "foreign address: E_ADDRNOTAVAIL");
    /* ephemeral ports differ and are random */
    int p[8];
    for (int i = 0; i < 8; i++) {
        int f = s_socket(1, SOCK_DGRAM, 0);
        s_sendto(1, f, "x", 1, NODEIP(2), 9);
        s_name(1, f, 0, NULL, &p[i]);
        s_close(1, f);
    }
    int seq = 1;
    for (int i = 1; i < 8; i++) if (p[i] != p[i - 1] + 1) seq = 0;
    CHECK(!seq && p[0] >= 49152, "ephemeral ports are not sequential (%d %d %d)", p[0], p[1], p[2]);
    teardown();
}

/* (slow) random impairments, transfers both ways, several connections at once */
static void test_torture(void) {
    for (int round = 0; round < 12; round++) {
        setup(2, 0);
        enum { K = 3 };
        int c[K], s[K];
        int ok = 1;
        for (int k = 0; k < K; k++) if (tcp_pair(1, 2, 100 + k, &c[k], &s[k])) ok = 0;
        CHECK(ok, "round %d: connections", round);
        wire.loss = (int)(rnd() % 120); wire.dup = (int)(rnd() % 60); wire.reorder = (int)(rnd() % 200);
        wire.reorder_ms = 1 + (int)(rnd() % 30); wire.corrupt = (int)(rnd() % 20); wire.delay_ms = (int)(rnd() % 25);
        wire.bw = rnd() % 2 ? 0 : 500 + (int)(rnd() % 5000);
        static xfer_t x[2 * K];
        for (int k = 0; k < K; k++) {
            xfer_init(&x[2 * k], 1, c[k], 2, s[k], 200000 + (long)(rnd() % 300000), (u8)(20 + k));
            xfer_init(&x[2 * k + 1], 2, s[k], 1, c[k], 100000 + (long)(rnd() % 200000), (u8)(40 + k));
        }
        u64 t0 = now;
        int done = 0;
        while (!done && now - t0 < 1800000) {
            done = 1;
            for (int k = 0; k < 2 * K; k++) if (!xfer_step(&x[k])) done = 0;
            run_for(5);
        }
        int good = done;
        for (int k = 0; k < 2 * K; k++) if (x[k].bad || x[k].err || x[k].got != x[k].total) good = 0;
        CHECK(good, "round %d (loss %d dup %d reorder %d corrupt %d delay %d bw %d): all intact", round, wire.loss, wire.dup,
              wire.reorder, wire.corrupt, wire.delay_ms, wire.bw);
        printf("    round %2d: per mille loss %3d dup %2d reorder %3d corrupt %2d, delay %2d ms, bw %4d B/ms: %6.1f s simulated\n",
               round, wire.loss, wire.dup, wire.reorder, wire.corrupt, wire.delay_ms, wire.bw, (double)(now - t0) / 1000);
        teardown();
    }
}

/* ================= main ================= */

typedef struct { const char *name; void (*fn)(void); int slow; } test_t;
static const test_t tests[] = {
    { "arp", test_arp_ping, 0 },
    { "dhcp", test_dhcp, 0 },
    { "dns", test_dns, 0 },
    { "udp", test_udp, 0 },
    { "tcp", test_tcp_basic, 0 },
    { "ports", test_ports, 0 },
    { "simultaneous", test_simultaneous, 0 },
    { "halfclose", test_half_close, 0 },
    { "rst", test_rst, 0 },
    { "backlog", test_backlog, 0 },
    { "keepalive", test_keepalive, 0 },
    { "nagle", test_nagle_delack, 0 },
    { "zerowin", test_zero_window, 0 },
    { "rto", test_rto, 0 },
    { "pmtu", test_pmtu, 0 },
    { "loopback", test_loopback, 0 },
    { "many", test_many, 0 },
    { "wscale", test_wscale, 0 },
    { "impaired", test_tcp_impaired, 0 },
    { "torture", test_torture, 1 },
};

int main(int argc, char **argv) {
    unsigned seed = 1;
    const char *only[64];
    int nonly = 0;
    if (argc < 2) { fprintf(stderr, "usage: netsim STACK.so [-v] [-s SEED] [-f] [-seeds DIR] [test...]\n"); return 2; }
    so_path = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-f")) full = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seeds") && i + 1 < argc) { seeddir = argv[++i]; mkdir(seeddir, 0755); }
        else if (nonly < 64) only[nonly++] = argv[i];
    }
    rng ^= (u64)seed * 0x9E3779B97F4A7C15ull;
    segs = calloc(MAXSEG, sizeof *segs);
    snprintf(tmpdir, sizeof tmpdir, "/tmp/netsim.%d", (int)getpid());
    mkdir(tmpdir, 0700);
    host_cfg.now_ms = &now;
    host_cfg.xmit = host_xmit;
    host_cfg.verbose = verbose;
    now = 1000;
    for (unsigned t = 0; t < sizeof tests / sizeof tests[0]; t++) {
        int want = !nonly && (!tests[t].slow || full);
        for (int k = 0; k < nonly; k++) if (!strcmp(only[k], tests[t].name)) want = 1;
        if (!want) continue;
        cur_test = tests[t].name;
        int f0 = failures, c0 = checks;
        printf("netsim: %s\n", cur_test);
        fflush(stdout);
        tests[t].fn();
        printf("netsim: %s: %d checks%s\n", cur_test, checks - c0, failures > f0 ? ", FAILED" : "");
    }
    rmdir(tmpdir);
    printf("netsim: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
