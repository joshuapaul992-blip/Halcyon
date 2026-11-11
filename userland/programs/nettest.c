/* NETTEST.EXE - checks the internet stack against the outside world: the
 * DHCP configuration, ping (ICMP socket), DNS lookups, TCP downloads and
 * uploads with checksums, many parallel connections, a TCP server reached
 * from outside, UDP echo, the loopback, error cases. Run by the automated
 * QEMU tests (the `net` variant: QEMU's user network, servers on the host)
 * with arguments naming the host's ports:
 *
 *   nettest http=P sink=P echo=P [server=P] [loss] [big=MiB]
 *
 * "loss" is the run with frames dropped by the stack's test hook: fewer,
 * shorter checks, and retransmissions must have happened. Prints
 * "nettest: all N checks passed" or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("nettest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SA(a) ((struct sockaddr *)(a))
#define GW IP(10, 0, 2, 2)
#define IP(a, b, c, d) ((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24)

static int http_port, sink_port, echo_port, server_port = 8080, loss, big_mib = 32;
static char ipbuf[8][16];
static int ipslot;
static const char *ip(unsigned a) { return inet_ntoa_r(a, ipbuf[ipslot++ & 7]); }

/* ---------------- CRC-32 (IEEE) ---------------- */
static unsigned crctab[256];
static void crc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crctab[i] = c;
    }
}
static unsigned crc_update(unsigned c, const unsigned char *p, long n) {
    c = ~c;
    while (n--) c = crctab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static unsigned char pat(long i) { return (unsigned char)((i * 7) ^ (i >> 8) ^ (i >> 16) ^ 0x5A); }

static unsigned long long ms(void) { return time_ms(); }

/* ---------------- helpers ---------------- */

static int tcp_to(unsigned addr, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    sock_timeouts(fd, timeout_ms, timeout_ms);
    struct sockaddr_in a;
    int r = connect(fd, SA(&a), inet_sockaddr(&a, addr, port));
    if (r) { close(fd); return r; }
    return fd;
}

static long send_all(int fd, const void *b, long n) {
    long done = 0;
    while (done < n) {
        long r = send(fd, (const char *)b + done, (size_t)(n - done), 0);
        if (r <= 0) return r ? r : E_IO;
        done += r;
    }
    return done;
}

/* read an HTTP response header; returns its length in buf (up to the blank line) */
static int read_header(int fd, char *buf, int max, int *body_start, int *have) {
    int n = 0;
    while (n < max - 1) {
        long r = recv(fd, buf + n, (size_t)(max - 1 - n), 0);
        if (r <= 0) return r ? (int)r : E_IO;
        n += (int)r;
        buf[n] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) { *body_start = (int)(e - buf) + 4; *have = n; return 0; }
    }
    return E_MSGSIZE;
}

static long header_num(const char *h, const char *name, int base) {
    const char *p = strstr(h, name);
    if (!p) return -1;
    p += strlen(name);
    while (*p == ' ') p++;
    return strtol(p, 0, base);
}

/* ---------------- configuration ---------------- */

static hal_ifinfo_t eth;

static void test_config(void) {
    unsigned long long t0 = ms();
    int r;
    do {
        r = net_ifinfo(0, &eth);
        if (!r && eth.addr && (eth.dhcp_state == NET_DHCP_BOUND || eth.mode == NET_MODE_STATIC)) break;
        sleep_ms(200);
    } while (ms() - t0 < 20000);
    CHECK(!r && !eth.loopback, "interface 0 is an Ethernet adapter (%d)", r);
    CHECK(eth.addr != 0, "eth0 has an address");
    printf("nettest: %s %s (%s) MAC %02x:%02x:%02x:%02x:%02x:%02x link %s %d Mbit/s\n", eth.name, eth.desc, eth.driver,
           eth.mac[0], eth.mac[1], eth.mac[2], eth.mac[3], eth.mac[4], eth.mac[5], eth.link_up ? "up" : "down", eth.speed_mbps);
    printf("nettest: %s/%s gateway %s DNS %s, %s, lease %lld s (%lld left), server %s\n", ip(eth.addr), ip(eth.mask),
           ip(eth.gateway), ip(eth.dns[0]), eth.mode == NET_MODE_DHCP ? "DHCP" : "static", eth.lease_s, eth.lease_left_s,
           ip(eth.dhcp_server));
    CHECK(eth.mode == NET_MODE_DHCP && eth.dhcp_state == NET_DHCP_BOUND, "configured by DHCP (state %d)", eth.dhcp_state);
    CHECK(eth.addr == IP(10, 0, 2, 15) && eth.mask == IP(255, 255, 255, 0) && eth.gateway == GW,
          "10.0.2.15/24 via 10.0.2.2 (%s/%s via %s)", ip(eth.addr), ip(eth.mask), ip(eth.gateway));
    CHECK(eth.lease_s > 0 && eth.lease_left_s > 0 && eth.lease_left_s <= eth.lease_s, "lease time");
    CHECK(eth.dns[0] == GW, "DNS server from net.dns (%s)", ip(eth.dns[0]));
    hal_ifinfo_t lo;
    int i = 0;
    while (!net_ifinfo(i, &lo) && !lo.loopback) i++;
    CHECK(lo.loopback && !strcmp(lo.name, "lo") && lo.addr == IP(127, 0, 0, 1), "loopback interface");
    CHECK(net_ifinfo(i + 1, &lo) == E_NOENT, "E_NOENT past the last interface");
}

/* ---------------- ping ---------------- */

/* n echo requests to addr; returns the replies; *rtt_us the average */
static int ping(unsigned addr, int n, int size, int timeout_ms, int *avg_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) { printf("nettest: ping socket: %d\n", fd); return fd; }
    sock_timeouts(fd, timeout_ms, 0);
    unsigned char pkt[1500], in[1600];
    int got = 0;
    long total = 0;
    for (int seq = 1; seq <= n; seq++) {
        memset(pkt, 0, 8);
        pkt[0] = 8;
        pkt[6] = (unsigned char)(seq >> 8); pkt[7] = (unsigned char)seq;
        for (int i = 8; i < size; i++) pkt[i] = (unsigned char)(i + seq);
        struct sockaddr_in to;
        unsigned long long t0 = ms();
        long r = sendto(fd, pkt, (size_t)size, 0, SA(&to), inet_sockaddr(&to, addr, 0));
        if (r != size) { printf("nettest: ping send: %ld\n", r); continue; }
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            r = recvfrom(fd, in, sizeof in, 0, SA(&from), &fl);
            if (r < 0) break;
            if (r >= 8 && in[0] == 0 && in[6] == pkt[6] && in[7] == pkt[7] && from.sin_addr.s_addr == addr) {
                if (r == size && !memcmp(in + 8, pkt + 8, (size_t)(size - 8))) { got++; total += (long)(ms() - t0); }
                break;
            }
        }
    }
    close(fd);
    if (avg_ms) *avg_ms = got ? (int)(total / got) : -1;
    return got;
}

static void test_ping(void) {
    int avg;
    int got = ping(GW, 4, 64, 2000, &avg);
    CHECK(got == 4, "ping 10.0.2.2: %d of 4 replies", got);
    printf("nettest: ping 10.0.2.2: %d/4 replies, average %d ms\n", got, avg);
    got = ping(IP(127, 0, 0, 1), 3, 1000, 1000, &avg);
    CHECK(got == 3, "ping 127.0.0.1: %d of 3 replies", got);
    got = ping(eth.addr, 2, 56, 1000, &avg);
    CHECK(got == 2, "ping our own address: %d of 2", got);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    unsigned char bad[8] = { 0 };
    struct sockaddr_in to;
    CHECK(sendto(fd, bad, 8, 0, SA(&to), inet_sockaddr(&to, GW, 0)) == E_INVAL, "ping socket sends echo requests only");
    CHECK(sendto(fd, bad, 4, 0, SA(&to), inet_sockaddr(&to, GW, 0)) == E_INVAL, "ping socket: the header at least");
    close(fd);
}

/* ---------------- DNS ---------------- */

static void test_dns(void) {
    unsigned a[16];
    hal_netstats_t s0, s1;
    net_ctl(NETCTL_DNS_FLUSH, 0);
    int n = resolve("files.halcyon.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == GW, "files.halcyon.test -> 10.0.2.2 (%d, %s)", n, n > 0 ? ip(a[0]) : "");
    net_stats(&s0);
    unsigned long long t0 = ms();
    n = resolve("FILES.halcyon.test.", a, 16, 5000);
    net_stats(&s1);
    CHECK(n == 1 && a[0] == GW && s1.dns_queries == s0.dns_queries && s1.dns_hits > s0.dns_hits,
          "second lookup from the cache, case and trailing dot ignored (%d, %llu queries)", n, s1.dns_queries - s0.dns_queries);
    CHECK(ms() - t0 < 50, "cache hit is immediate");
    n = resolve("alias.halcyon.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == GW, "CNAME in the answer (%d)", n);
    n = resolve("chain.halcyon.test", a, 16, 5000);
    CHECK(n == 1 && a[0] == IP(192, 0, 2, 7), "CNAME chain needing a second query (%d %s)", n, n > 0 ? ip(a[0]) : "");
    n = resolve("multi.halcyon.test", a, 16, 5000);
    CHECK(n == 3 && a[0] == IP(192, 0, 2, 1) && a[2] == IP(192, 0, 2, 3), "three addresses (%d)", n);
    n = resolve("multi.halcyon.test", a, 2, 5000);
    CHECK(n == 2, "max respected (%d)", n);
    n = resolve("nx.halcyon.test", a, 16, 5000);
    CHECK(n == E_NOENT, "no such name: E_NOENT (%d)", n);
    net_stats(&s0);
    n = resolve("nx.halcyon.test", a, 16, 5000);
    net_stats(&s1);
    CHECK(n == E_NOENT && s1.dns_queries == s0.dns_queries, "negative answer cached");
    n = resolve("localhost", a, 16, 1000);
    CHECK(n == 1 && a[0] == IP(127, 0, 0, 1), "localhost");
    n = resolve("192.168.7.9", a, 16, 1000);
    CHECK(n == 1 && a[0] == IP(192, 168, 7, 9), "dotted quad literal");
    CHECK(resolve("bad..name", a, 16, 1000) == E_INVAL, "invalid name");
    CHECK(resolve("x", a, 0, 1000) == E_INVAL, "max 0");
    CHECK(resolve((const char *)16, a, 4, 1000) == E_INVAL, "bad pointer");
    t0 = ms();
    n = resolve("slow.halcyon.test", a, 16, 1500);
    unsigned long long el = ms() - t0;
    CHECK(n == E_TIMEOUT && el >= 1400 && el < 3000, "server that never answers: E_TIMEOUT after the timeout (%d, %llu ms)", n, el);
    n = resolve("fail.halcyon.test", a, 16, 5000);
    CHECK(n == E_IO, "SERVFAIL: E_IO (%d)", n);
    net_stats(&s0);
    n = resolve("spoof.halcyon.test", a, 16, 5000);
    net_stats(&s1);
    CHECK(n == 1 && a[0] == GW && s1.dns_rejected > s0.dns_rejected, "a forged answer (wrong id) is ignored (%d %s)",
          n, n > 0 ? ip(a[0]) : "");
    net_stats(&s0);
    net_ctl(NETCTL_DNS_FLUSH, 0);
    n = resolve("files.halcyon.test", a, 16, 5000);
    net_stats(&s1);
    CHECK(n == 1 && s1.dns_queries > s0.dns_queries, "after a flush the server is asked again");
    printf("nettest: DNS: %llu queries, %llu cache hits, %llu answers rejected, %d cached\n",
           s1.dns_queries, s1.dns_hits, s1.dns_rejected, s1.dns_cached);
}

/* ---------------- TCP against the host ---------------- */

static unsigned char buf[65536];

/* GET path from the host's HTTP server; checks length and CRC; returns bytes or < 0 */
static long http_get(const char *path, unsigned long long *elapsed) {
    int fd = tcp_to(GW, http_port, loss ? 60000 : 10000);
    if (fd < 0) return fd;
    char req[128];
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n", path);
    unsigned long long t0 = ms();
    if (send_all(fd, req, (long)strlen(req)) < 0) { close(fd); return E_IO; }
    char hdr[2048];
    int body = 0, have = 0;
    int r = read_header(fd, hdr, sizeof hdr, &body, &have);
    if (r) { close(fd); return r; }
    unsigned crc = crc_update(0, (unsigned char *)hdr + body, have - body);
    hdr[body] = 0;                      /* (after the CRC: this is the body's first byte) */
    long len = header_num(hdr, "Content-Length:", 10);
    unsigned crc_want = (unsigned)header_num(hdr, "X-CRC32:", 16);
    long got = have - body;
    for (;;) {
        long k = recv(fd, buf, sizeof buf, 0);
        if (k < 0) { printf("nettest: GET %s: recv %ld after %ld bytes\n", path, k, got); close(fd); return k; }
        if (!k) break;
        crc = crc_update(crc, buf, k);
        got += k;
    }
    if (elapsed) *elapsed = ms() - t0;
    close(fd);
    if (got != len || crc != crc_want) {
        printf("nettest: GET %s: %ld of %ld bytes, crc %08x want %08x\n", path, got, len, crc, crc_want);
        return E_IO;
    }
    return got;
}

/* bytes in ms as "12.3 MB/s" or "456 KB/s" (decimal units) */
static const char *rate(long bytes, unsigned long long el) {
    static char r[32];
    unsigned long long bps = (unsigned long long)bytes * 1000 / el;
    if (bps >= 1000000) snprintf(r, sizeof r, "%llu.%llu MB/s", bps / 1000000, bps / 100000 % 10);
    else snprintf(r, sizeof r, "%llu KB/s", bps / 1000);
    return r;
}

static void test_download(int mib) {
    char path[32];
    snprintf(path, sizeof path, "/big/%d", mib);
    unsigned long long el = 0;
    long n = http_get(path, &el);
    CHECK(n == (long)mib << 20, "download %d MiB with the right CRC (%ld)", mib, n);
    if (n > 0 && el) printf("nettest: download %d MiB in %llu ms: %s\n", mib, el, rate(n, el));
}

static void test_upload(int mib) {
    int fd = tcp_to(GW, sink_port, loss ? 60000 : 20000);
    CHECK(fd >= 0, "connect to the sink (%d)", fd);
    if (fd < 0) return;
    long total = (long)mib << 20;
    unsigned crc = 0;
    unsigned long long t0 = ms();
    for (long off = 0; off < total; off += (long)sizeof buf) {
        long k = total - off < (long)sizeof buf ? total - off : (long)sizeof buf;
        for (long i = 0; i < k; i++) buf[i] = pat(off + i);
        crc = crc_update(crc, buf, k);
        long r = send_all(fd, buf, k);
        if (r < 0) { CHECK(0, "upload send at %ld: %ld", off, r); close(fd); return; }
    }
    CHECK(shutdown(fd, SHUT_WR) == 0, "half-close after the upload");
    char reply[64];
    long r = recv(fd, reply, sizeof reply - 1, MSG_WAITALL);
    unsigned long long el = ms() - t0;
    reply[r > 0 ? r : 0] = 0;
    long len = strtol(reply, 0, 10);
    char *sp = strchr(reply, ' ');
    unsigned c = sp ? (unsigned)strtol(sp + 1, 0, 16) : 0;
    CHECK(len == total && c == crc, "the host received %d MiB intact (%s)", mib, reply);
    CHECK(recv(fd, reply, 1, 0) == 0, "then end of stream");
    close(fd);
    if (el) printf("nettest: upload %d MiB in %llu ms: %s\n", mib, el, rate(total, el));
}

/* many connections at once, non-blocking, with poll */
#define PAR 40
static void test_parallel(void) {
    int fd[PAR], state[PAR];
    long got[PAR];
    unsigned crc[PAR];
    char hdr[PAR][256];
    int hl[PAR];
    int ok = 0;
    for (int i = 0; i < PAR; i++) {
        fd[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        struct sockaddr_in a;
        int r = connect(fd[i], SA(&a), inet_sockaddr(&a, GW, http_port));
        state[i] = r == E_INPROGRESS || r == 0 ? 0 : -1;
        got[i] = 0; crc[i] = 0; hl[i] = 0;
    }
    unsigned long long t0 = ms();
    int active = PAR;
    while (active && ms() - t0 < 30000) {
        struct pollfd p[PAR];
        int map[PAR], np = 0;
        for (int i = 0; i < PAR; i++) {
            if (state[i] < 0 || state[i] == 3) continue;
            p[np].fd = fd[i];
            p[np].events = (short)(state[i] == 0 ? POLLOUT : POLLIN);
            p[np].revents = 0;
            map[np++] = i;
        }
        if (poll(p, (unsigned)np, 5000) <= 0) break;
        for (int k = 0; k < np; k++) {
            int i = map[k];
            if (!p[k].revents) continue;
            if (state[i] == 0) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd[i], SOL_SOCKET, SO_ERROR, &err, &el);
                if (err || (p[k].revents & POLLERR)) { state[i] = -1; active--; continue; }
                char req[64];
                snprintf(req, sizeof req, "GET /small/%d HTTP/1.0\r\n\r\n", i);
                if (send(fd[i], req, strlen(req), 0) != (long)strlen(req)) { state[i] = -1; active--; continue; }
                state[i] = 1;
                continue;
            }
            long n = recv(fd[i], buf, sizeof buf, 0);
            if (n == E_AGAIN) continue;
            if (n < 0) { state[i] = -1; active--; continue; }
            if (n == 0) {
                state[i] = 3; active--;
                long len = header_num(hdr[i], "Content-Length:", 10);
                unsigned want = (unsigned)header_num(hdr[i], "X-CRC32:", 16);
                if (len == got[i] && want == crc[i]) ok++;
                continue;
            }
            long off = 0;
            if (state[i] == 1) {                    /* still in the header */
                while (off < n && hl[i] < 255) {
                    hdr[i][hl[i]++] = (char)buf[off++];
                    hdr[i][hl[i]] = 0;
                    if (hl[i] >= 4 && !memcmp(hdr[i] + hl[i] - 4, "\r\n\r\n", 4)) { state[i] = 2; break; }
                }
            }
            if (state[i] == 2 && off < n) { crc[i] = crc_update(crc[i], buf + off, n - off); got[i] += n - off; }
        }
    }
    for (int i = 0; i < PAR; i++) if (fd[i] >= 0) close(fd[i]);
    CHECK(ok == PAR, "%d parallel connections: %d complete and intact", PAR, ok);
    printf("nettest: %d parallel downloads in %llu ms\n", PAR, ms() - t0);
}

/* a server the host connects to through QEMU's port forwarding */
static void test_server(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    CHECK(bind(l, SA(&a), inet_sockaddr(&a, 0, server_port)) == 0, "bind :%d", server_port);
    CHECK(listen(l, 4) == 0, "listen");
    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(bind(l2, SA(&a), inet_sockaddr(&a, 0, server_port)) == E_ADDRINUSE, "a second bind: E_ADDRINUSE");
    close(l2);
    printf("nettest: server ready on port %d\n", server_port);
    struct pollfd p = { l, POLLIN, 0 };
    int r = poll(&p, 1, 30000);
    CHECK(r == 1, "the host connected (%d)", r);
    if (r != 1) { close(l); return; }
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int c = accept(l, SA(&peer), &pl);
    CHECK(c >= 0 && pl == sizeof peer && peer.sin_family == AF_INET, "accept (%d)", c);
    if (c < 0) { close(l); return; }
    sock_timeouts(c, 20000, 20000);
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    getsockname(c, SA(&me), &ml);
    CHECK(me.sin_addr.s_addr == eth.addr && ntohs(me.sin_port) == server_port, "accepted socket's name");
    long total = 0;
    unsigned crc = 0;
    for (;;) {
        long k = recv(c, buf, sizeof buf, 0);
        if (k <= 0) { CHECK(k == 0, "server recv: %ld", k); break; }
        crc = crc_update(crc, buf, k);
        total += k;
    }
    char reply[64];
    snprintf(reply, sizeof reply, "OK %ld %08x\n", total, crc);
    CHECK(send_all(c, reply, (long)strlen(reply)) > 0, "server reply");
    printf("nettest: server: received %ld bytes, crc %08x from %s:%d\n", total, crc, ip(peer.sin_addr.s_addr), ntohs(peer.sin_port));
    CHECK(total == 1 << 20, "server received 1 MiB (%ld)", total);
    close(c);
    close(l);
}

/* ---------------- UDP ---------------- */

static void test_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sock_timeouts(fd, 2000, 0);
    static unsigned char out[9000], in[9000];
    int sizes[] = { 1, 100, 512, 1472, 1473, 3000, 8000, 0 };
    int ok = 0, n = 0;
    struct sockaddr_in to;
    inet_sockaddr(&to, GW, echo_port);
    for (int k = 0; k < 8; k++) {
        int sz = sizes[k];
        for (int i = 0; i < sz; i++) out[i] = (unsigned char)(i * 13 + k);
        long r = sendto(fd, out, (size_t)sz, 0, SA(&to), sizeof to);
        if (r != sz) { printf("nettest: udp send %d: %ld\n", sz, r); continue; }
        n++;
        for (int tries = 0; tries < 3; tries++) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            r = recvfrom(fd, in, sizeof in, 0, SA(&from), &fl);
            if (r < 0) break;
            if (r == sz && !memcmp(in, out, (size_t)sz) && from.sin_addr.s_addr == GW && ntohs(from.sin_port) == echo_port) { ok++; break; }
        }
    }
    CHECK(ok == n && n == 8, "UDP echo, 0..8000 bytes (fragmented above 1472): %d of %d", ok, n);
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    getsockname(fd, SA(&me), &ml);
    CHECK(ntohs(me.sin_port) >= 49152, "ephemeral port %d", ntohs(me.sin_port));
    unsigned long v = 0;
    sendto(fd, "x", 1, 0, SA(&to), sizeof to);
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, 2000);
    CHECK(ioctlsocket(fd, FIONREAD, &v) == 0 && v == 1, "FIONREAD on UDP (%lu)", v);
    close(fd);
    /* broadcast needs SO_BROADCAST */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in b;
    CHECK(sendto(fd, "x", 1, 0, SA(&b), inet_sockaddr(&b, 0xFFFFFFFFu, 9)) == E_PERM, "broadcast without SO_BROADCAST");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    CHECK(sendto(fd, "x", 1, 0, SA(&b), inet_sockaddr(&b, 0xFFFFFFFFu, 9)) == 1, "broadcast with SO_BROADCAST");
    close(fd);
}

/* ---------------- loopback ---------------- */

static void test_loopback(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    CHECK(bind(l, SA(&a), inet_sockaddr(&a, IP(127, 0, 0, 1), 0)) == 0 && listen(l, 2) == 0, "loopback listener");
    socklen_t al = sizeof a;
    getsockname(l, SA(&a), &al);
    int port = ntohs(a.sin_port);
    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int r = connect(c, SA(&a), inet_sockaddr(&a, IP(127, 0, 0, 1), port));
    CHECK(r == E_INPROGRESS || r == 0, "non-blocking connect: E_INPROGRESS (%d)", r);
    struct pollfd p = { l, POLLIN, 0 };
    CHECK(poll(&p, 1, 2000) == 1, "connection to accept");
    int s = accept4(l, 0, 0, SOCK_NONBLOCK);
    CHECK(s >= 0, "accept");
    long total = 4 << 20, sent = 0, got = 0;
    int bad = 0;
    static unsigned char o[16384], in[16384];
    unsigned long long t0 = ms();
    while (got < total && ms() - t0 < 20000) {
        struct pollfd q[2] = { { c, (short)(sent < total ? POLLOUT : 0), 0 }, { s, POLLIN, 0 } };
        poll(q, 2, 1000);
        if (sent < total && (q[0].revents & POLLOUT)) {
            long k = total - sent < (long)sizeof o ? total - sent : (long)sizeof o;
            for (long i = 0; i < k; i++) o[i] = pat(sent + i);
            long w = send(c, o, (size_t)k, 0);
            if (w > 0) sent += w;
            if (sent == total) shutdown(c, SHUT_WR);
        }
        if (q[1].revents & POLLIN) {
            long k = recv(s, in, sizeof in, 0);
            if (k == 0) break;
            for (long i = 0; i < k; i++) if (in[i] != pat(got + i)) bad = 1;
            if (k > 0) got += k;
        }
    }
    unsigned long long el = ms() - t0;
    CHECK(got == total && !bad, "4 MiB over the loopback intact (%ld)", got);
    if (el) printf("nettest: loopback 4 MiB in %llu ms\n", el);
    unsigned char x;
    sock_nonblock(s, 0);
    sock_timeouts(s, 2000, 0);
    CHECK(recv(s, &x, 1, 0) == 0, "end of stream after the half-close");
    CHECK(send(s, "bye", 3, 0) == 3, "the other direction still works");
    sock_nonblock(c, 0);
    sock_timeouts(c, 2000, 0);
    char bb[4];
    CHECK(recv(c, bb, 3, MSG_WAITALL) == 3 && !memcmp(bb, "bye", 3), "half-closed side reads");
    close(s);
    CHECK(recv(c, bb, 1, 0) == 0, "then end of stream");
    close(c);
    /* connect to a closed port: RST */
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, SA(&a), inet_sockaddr(&a, IP(127, 0, 0, 1), port + 1 > 65535 ? 1 : port + 1)) == E_CONNREFUSED, "closed port: E_CONNREFUSED");
    close(c);
    /* abortive close: the peer sees a reset */
    c = tcp_to(IP(127, 0, 0, 1), port, 2000);
    s = accept(l, 0, 0);
    struct linger lg = { 1, 0 };
    setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(c);
    sock_timeouts(s, 2000, 0);
    long rr = recv(s, bb, 1, 0);
    CHECK(rr == E_CONNRESET, "SO_LINGER 0 close resets the peer (%ld)", rr);
    close(s);
    close(l);
    /* UDP over the loopback, connected, and to a closed port */
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    bind(u1, SA(&a), inet_sockaddr(&a, IP(127, 0, 0, 1), 0));
    al = sizeof a;
    getsockname(u1, SA(&a), &al);
    CHECK(connect(u2, SA(&a), sizeof a) == 0 && send(u2, "hello", 5, 0) == 5, "connected UDP send");
    sock_timeouts(u1, 1000, 0);
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    CHECK(recvfrom(u1, bb, 4, MSG_TRUNC, SA(&from), &fl) == 5 && from.sin_addr.s_addr == IP(127, 0, 0, 1), "MSG_TRUNC gives the length");
    close(u1);
    sock_timeouts(u2, 1000, 0);
    send(u2, "again", 5, 0);
    long e = recv(u2, bb, 4, 0);
    CHECK(e == E_CONNREFUSED, "connected UDP to a closed port: E_CONNREFUSED (%ld)", e);
    close(u2);
}

/* ---------------- errors and options ---------------- */

static void test_errors(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    unsigned long long t0 = ms();
    sock_timeouts(fd, 0, 10000);
    int r = connect(fd, SA(&a), inet_sockaddr(&a, IP(10, 0, 2, 77), 80));
    CHECK(r == E_HOSTUNREACH, "connect to an address nobody has: E_HOSTUNREACH (%d after %llu ms)", r, ms() - t0);
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(fd, SA(&a), inet_sockaddr(&a, GW, 1));
    CHECK(r == E_CONNREFUSED, "host port with no server: E_CONNREFUSED (%d)", r);
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(bind(fd, SA(&a), inet_sockaddr(&a, IP(10, 9, 9, 9), 0)) == E_ADDRNOTAVAIL, "bind to a foreign address");
    CHECK(send(fd, "x", 1, 0) == E_NOTCONN, "send before connect: E_NOTCONN");
    int v = 1;
    socklen_t vl = sizeof v;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v) == 0 &&
          getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &vl) == 0 && v == 1, "TCP_NODELAY");
    v = 30;
    CHECK(setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof v) == 0, "TCP_KEEPIDLE");
    v = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof v) == 0, "SO_KEEPALIVE");
    v = 200000;
    vl = sizeof v;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0 && getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &vl) == 0 &&
          v == 200000, "SO_RCVBUF");
    close(fd);
    CHECK(socket(AF_INET, SOCK_DGRAM, 99) == E_NOTSUP, "unknown protocol");
    CHECK(socket(AF_INET, 5, 0) == E_NOTSUP, "unknown type");
    /* a connection with a large receive window (scaling) to the host */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    v = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
    sock_timeouts(fd, 10000, 10000);
    r = connect(fd, SA(&a), inet_sockaddr(&a, GW, http_port));
    CHECK(r == 0, "connect with a 256 KiB window (%d)", r);
    socklen_t pl = sizeof a;
    CHECK(getpeername(fd, SA(&a), &pl) == 0 && a.sin_addr.s_addr == GW && ntohs(a.sin_port) == http_port, "getpeername");
    vl = sizeof v;
    CHECK(getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &v, &vl) == 0 && v > 500 && v <= 1460, "TCP_MAXSEG %d", v);
    close(fd);
    hal_netconn_t cs[64];
    int nc = net_conns(cs, 64);
    CHECK(nc >= 0, "net_conns (%d)", nc);
}

/* ---------------- main ---------------- */

static void args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        int v = eq ? atoi(eq + 1) : 0;
        if (!strncmp(argv[i], "http=", 5)) http_port = v;
        else if (!strncmp(argv[i], "sink=", 5)) sink_port = v;
        else if (!strncmp(argv[i], "echo=", 5)) echo_port = v;
        else if (!strncmp(argv[i], "server=", 7)) server_port = v;
        else if (!strncmp(argv[i], "big=", 4)) big_mib = v;
        else if (!strcmp(argv[i], "loss")) loss = 1;
    }
}

int main(int argc, char **argv) {
    args(argc, argv);
    crc_init();
    printf("nettest: starting (pid %d)%s\n", getpid(), loss ? ", with frame loss" : "");
    hal_sockstat_t st0, st;
    sockstat(&st0);
    hal_netstats_t n0, n1;
    net_stats(&n0);
    test_config();
    if (loss) {
        int avg;
        int got = ping(GW, 20, 64, 1000, &avg);
        CHECK(got >= 10, "ping with loss: %d of 20", got);
        unsigned a[4];
        int n = resolve("files.halcyon.test", a, 4, 15000);
        CHECK(n == 1 && a[0] == GW, "DNS with loss (%d)", n);
        if (http_port) test_download(big_mib);
        if (sink_port) test_upload(2);
        test_loopback();
    } else {
        test_ping();
        test_dns();
        if (http_port) test_download(big_mib);
        if (sink_port) test_upload(8);
        if (http_port) test_parallel();
        if (echo_port) test_udp();
        test_loopback();
        test_errors();
        test_server();
    }
    net_stats(&n1);
    printf("nettest: tcp %llu segments in, %llu out, %llu retransmitted (%llu fast), %llu timeouts; "
           "frames lost by the test hook: %llu\n", n1.tcp_in - n0.tcp_in, n1.tcp_out - n0.tcp_out, n1.tcp_rexmt - n0.tcp_rexmt,
           n1.tcp_fast_rexmt - n0.tcp_fast_rexmt, n1.tcp_timeouts - n0.tcp_timeouts, n1.loss_dropped - n0.loss_dropped);
    if (loss) CHECK(n1.tcp_rexmt > n0.tcp_rexmt && n1.loss_dropped > n0.loss_dropped, "retransmissions happened");
    sleep_ms(100);
    sockstat(&st);
    CHECK(st.proc_sockets == 0 && st.proc_mem == 0, "all released at the end (%d sockets, %llu bytes)", st.proc_sockets, st.proc_mem);
    if (failed) printf("nettest: %d of %d checks FAILED\n", failed, checks);
    else printf("nettest: all %d checks passed\n", checks);
    return failed;
}
