/* Fuzz harness for the network stack's receive path (kernel/net/: Ethernet,
 * ARP, IPv4 and reassembly, ICMP, UDP, TCP, the DHCP client, the resolver).
 *
 * One stack (net_shim.c) is the computer 10.0.0.3/24 (MAC 02:00:00:00:01:02,
 * gateway 10.0.0.1, DNS 10.0.0.1), as node 2 of the simulator (netsim.c),
 * so frames the simulator recorded are valid inputs. Every input starts
 * from the same situation: a TCP listener on port 80 and a TCP connection
 * to 10.0.0.1:8080 in progress, UDP sockets on 53 and 5353, a ping socket,
 * a lookup of "fuzz.test" in flight, and (if the first byte says so) the
 * DHCP client in place of the static address. The input is a list of records:
 *
 *   1 LEN16 FRAME      a frame, as it is
 *   2 LEN16 FRAME      a frame with its IP / ICMP / UDP / TCP checksums fixed
 *   3 LEN16 FRAME      as 2, and made to fit what the stack sent last: TCP
 *                      seq / ack in sync with the connection the frame is
 *                      for, DHCP xid and client address, DNS id / port / server
 *   4 MS16             let time pass
 *   5 OP               the program acts: send, read, shutdown, close, accept
 *
 * Bugs: ASan / UBSan reports, a stack that never returns, and at the end of
 * each input the memory check: once everything is closed and enough time
 * has passed, no socket memory may stay charged.
 *
 * Built two ways (tests/host/Makefile): clang -fsanitize=fuzzer (fuzz_net),
 * and gcc with the replay main() below (fuzz_net_replay FILE|DIR...). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "netsim.h"
#define timeval hal_timeval
#include "../../kernel/net/sockabi.h"
#undef timeval

int sim_init(const sim_host_t *h, int node, const unsigned char mac[6], unsigned seed);
void sim_setting(const char *key, const char *value);
void sim_start(void);
void sim_rx(const void *frame, unsigned len);
unsigned long long sim_poll(void);
long sim_call(int nr, long a1, long a2, long a3, long a4, long a5, long a6);
void *sim_dns_start(const char *name, unsigned timeout_ms, int *err);
void sim_dns_put(void *q);
unsigned long long sim_netmem(void);

static unsigned long long now = 1000;
static const uint8_t my_mac[6] = { 2, 0, 0, 0, 1, 2 }, gw_mac[6] = { 2, 0, 0, 0, 0, 1 };
#define MYIP  0x0300000Au   /* 10.0.0.3 */
#define GWIP  0x0100000Au   /* 10.0.0.1 */

/* what the stack sent last, for record type 3 */
static struct { uint32_t seq, ack; int valid; } tcpst[4];   /* per (our port & 3) */
static uint32_t dhcp_xid;
static uint16_t dns_id, dns_port;
static uint32_t dns_server;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void host_xmit(int node, const void *frame, unsigned len) {
    (void)node;
    const uint8_t *f = frame;
    if (len < 34 || rd16(f + 12) != 0x0800) return;
    const uint8_t *h = f + 14;
    unsigned hl = (h[0] & 15u) * 4, tot = rd16(h + 2);
    if (tot > len - 14 || hl < 20 || tot < hl) return;
    const uint8_t *p = h + hl;
    unsigned pl = tot - hl;
    if (h[9] == 6 && pl >= 20) {
        unsigned thl = (p[12] >> 4) * 4u;
        if (thl > pl) return;
        uint32_t seq = rd32(p + 4) + (pl - thl) + !!(p[13] & 2) + !!(p[13] & 1);
        int k = rd16(p) & 3;
        tcpst[k].seq = seq;                        /* what the peer should ack */
        tcpst[k].ack = rd32(p + 8);                /* what the peer should send next */
        tcpst[k].valid = 1;
    } else if (h[9] == 17 && pl >= 8) {
        uint16_t dport = rd16(p + 2);
        if (dport == 67 && pl >= 8 + 240) dhcp_xid = rd32(p + 8 + 4);
        if (dport == 53 && pl >= 8 + 12) { dns_id = rd16(p + 8); dns_port = rd16(p); memcpy(&dns_server, h + 16, 4); }
    }
}

static const sim_host_t host = { &now, host_xmit, 0 };

static uint16_t cksum(const uint8_t *p, unsigned n, uint32_t sum) {
    for (unsigned i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void fix_frame(uint8_t *f, unsigned len, int sync) {
    if (len < 34 || rd16(f + 12) != 0x0800) return;
    uint8_t *h = f + 14;
    unsigned hl = (h[0] & 15u) * 4;
    if (hl < 20 || 14 + hl > len) return;
    unsigned tot = rd16(h + 2);
    if (tot < hl || tot > len - 14) { tot = len - 14; wr16(h + 2, (uint16_t)tot); }
    if (sync) { memcpy(f, my_mac, 6); memcpy(h + 16, &(uint32_t){ MYIP }, 4); }
    h[10] = h[11] = 0;
    wr16(h + 10, cksum(h, hl, 0));
    if (rd16(h + 6) & 0x3FFF) return;              /* fragments: the payload sum spans them */
    uint8_t *p = h + hl;
    unsigned pl = tot - hl;
    uint32_t ps = 0;
    for (int i = 12; i < 20; i += 2) ps += (uint32_t)(h[i] << 8 | h[i + 1]);
    ps += h[9] + pl;
    if (h[9] == 1 && pl >= 4) { p[2] = p[3] = 0; wr16(p + 2, cksum(p, pl, 0)); }
    else if (h[9] == 17 && pl >= 8) {
        if (sync) {
            uint16_t sport = rd16(p);
            uint8_t *d = p + 8;
            unsigned dl = pl - 8;
            if (sport == 67 && dl >= 240) { wr32(d + 4, dhcp_xid); memcpy(d + 28, my_mac, 6); }
            if (sport == 53 && dl >= 12) { wr16(p + 2, dns_port); wr16(d, dns_id); memcpy(h + 12, &dns_server, 4); h[10] = h[11] = 0; wr16(h + 10, cksum(h, hl, 0)); }
            ps = 0;
            for (int i = 12; i < 20; i += 2) ps += (uint32_t)(h[i] << 8 | h[i + 1]);
            ps += h[9] + pl;
        }
        wr16(p + 4, (uint16_t)pl);
        p[6] = p[7] = 0;
        uint16_t c = cksum(p, pl, ps);
        wr16(p + 6, c ? c : 0xFFFF);
    } else if (h[9] == 6 && pl >= 20) {
        if (sync) {
            int k = rd16(p + 2) & 3;
            if (tcpst[k].valid) {
                wr32(p + 4, tcpst[k].ack);
                if (p[13] & 0x10) wr32(p + 8, tcpst[k].seq);
            }
        }
        p[16] = p[17] = 0;
        wr16(p + 16, cksum(p, pl, ps));
    }
}

static int initialized;
static int fd_listen, fd_conn, fd_udp53, fd_udp5353, fd_ping;
static void *dnsq;

static long call(int nr, long a, long b, long c, long d, long e, long f) {
    long r = sim_call(nr, a, b, c, d, e, f);
    sim_poll();
    return r;
}

static socklen_t mksin(struct sockaddr_in *a, uint32_t ip, int port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = (uint16_t)((port >> 8) | (port << 8));
    a->sin_addr.s_addr = ip;
    return sizeof *a;
}

static void advance(unsigned long long ms) {
    unsigned long long end = now + ms;
    while (now < end) {
        unsigned long long next = sim_poll();
        if (next <= now) next = now + 1;
        now = next < end ? next : end;
    }
    sim_poll();
}

static void rx_raw(const uint8_t *f, unsigned len) { sim_rx(f, len); sim_poll(); }

static void begin(int dhcp) {
    if (!initialized) {
        sim_init(&host, 2, my_mac, 7);
        sim_setting("net.eth0.mode", "static");
        sim_setting("net.eth0.ip", "10.0.0.3");
        sim_setting("net.eth0.mask", "255.255.255.0");
        sim_setting("net.eth0.gateway", "10.0.0.1");
        sim_setting("net.dns", "10.0.0.1");
        sim_start();
        initialized = 1;
    }
    memset(tcpst, 0, sizeof tcpst);
    /* the gateway's MAC, so nothing waits for ARP */
    uint8_t arp[42];
    memcpy(arp, my_mac, 6); memcpy(arp + 6, gw_mac, 6); wr16(arp + 12, 0x0806);
    uint8_t *a = arp + 14;
    wr16(a, 1); wr16(a + 2, 0x0800); a[4] = 6; a[5] = 4; wr16(a + 6, 2);
    memcpy(a + 8, gw_mac, 6); memcpy(a + 14, &(uint32_t){ GWIP }, 4);
    memcpy(a + 18, my_mac, 6); memcpy(a + 24, &(uint32_t){ MYIP }, 4);
    rx_raw(arp, 42);
    struct sockaddr_in sa;
    fd_listen = (int)call(SYS_SOCKET, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    call(SYS_BIND, fd_listen, (long)&sa, mksin(&sa, 0, 80), 0, 0, 0);
    call(SYS_LISTEN, fd_listen, 4, 0, 0, 0, 0);
    fd_conn = (int)call(SYS_SOCKET, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    call(SYS_CONNECT, fd_conn, (long)&sa, mksin(&sa, GWIP, 8080), 0, 0, 0);
    fd_udp53 = (int)call(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    call(SYS_BIND, fd_udp53, (long)&sa, mksin(&sa, 0, 53), 0, 0, 0);
    fd_udp5353 = (int)call(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    call(SYS_BIND, fd_udp5353, (long)&sa, mksin(&sa, 0, 5353), 0, 0, 0);
    fd_ping = (int)call(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_ICMP, 0, 0, 0);
    uint8_t echo[16] = { 8 };
    call(SYS_SENDTO, fd_ping, (long)echo, sizeof echo, 0, (long)&sa, mksin(&sa, GWIP, 0));
    int err;
    dnsq = sim_dns_start("fuzz.test", 5000, &err);
    if (dhcp) {
        hal_ifconfig_t c;
        memset(&c, 0, sizeof c);
        strcpy(c.name, "eth0");
        c.mode = NET_MODE_DHCP;
        c.flags = NETCFG_ADDR;
        call(SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0);
    }
}

static void end(int dhcp) {
    if (dnsq) { sim_dns_put(dnsq); dnsq = NULL; }
    for (int fd = 3; fd < 64; fd++) sim_call(21, fd, 0, 0, 0, 0, 0);
    if (dhcp) {
        hal_ifconfig_t c;
        memset(&c, 0, sizeof c);
        strcpy(c.name, "eth0");
        c.mode = NET_MODE_STATIC;
        c.addr = MYIP; c.mask = 0x00FFFFFFu; c.gateway = GWIP;
        c.flags = NETCFG_ADDR;
        call(SYS_NET_IFCONFIG, (long)&c, 0, 0, 0, 0, 0);
    }
    advance(400000);                    /* orphans, TIME_WAIT, reassembly, ARP, the DNS query */
    if (sim_netmem()) { fprintf(stderr, "fuzz_net: %llu bytes still charged after everything closed\n", sim_netmem()); abort(); }
}

static void app_op(uint8_t op) {
    static uint8_t buf[70000];
    int fds[5] = { fd_listen, fd_conn, fd_udp53, fd_udp5353, fd_ping };
    int fd = fds[(op >> 4) % 5];
    struct sockaddr_in sa;
    switch (op & 15) {
    case 0: call(SYS_RECVFROM, fd, (long)buf, sizeof buf, 0, 0, 0); break;
    case 1: memset(buf, 'a', 3000); call(SYS_SENDTO, fd, (long)buf, 3000, 0, 0, 0); break;
    case 2: call(SYS_SHUTDOWN, fd, SHUT_WR, 0, 0, 0, 0); break;
    case 3: call(SYS_SHUTDOWN, fd, SHUT_RD, 0, 0, 0, 0); break;
    case 4: { long a = call(SYS_ACCEPT, fd_listen, 0, 0, SOCK_NONBLOCK, 0, 0); if (a >= 0 && a < 64) fd_conn = (int)a; break; }
    case 5: call(21, fd, 0, 0, 0, 0, 0); break;
    case 6: buf[0] = 8; call(SYS_SENDTO, fd_ping, (long)buf, 1200, 0, (long)&sa, mksin(&sa, GWIP, 0)); break;
    case 7: call(SYS_SENDTO, fd_udp5353, (long)buf, 9000, 0, (long)&sa, mksin(&sa, GWIP, 5353)); break;
    case 8: { int v = 1; call(SYS_SETSOCKOPT, fd, IPPROTO_TCP, TCP_NODELAY, (long)&v, sizeof v, 0); break; }
    case 9: { int v = 1; call(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_KEEPALIVE, (long)&v, sizeof v, 0);
              v = 1; call(SYS_SETSOCKOPT, fd, IPPROTO_TCP, TCP_KEEPIDLE, (long)&v, sizeof v, 0); break; }
    case 10: { struct linger l = { 1, 0 }; call(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_LINGER, (long)&l, sizeof l, 0); break; }
    case 11: { int v = 2048; call(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_RCVBUF, (long)&v, sizeof v, 0); break; }
    case 12: call(SYS_NET_CTL, NETCTL_RENEW, 0, 0, 0, 0, 0); break;
    case 13: { int err; void *q = sim_dns_start(op & 0x80 ? "www.fuzz.test" : "other.test", 3000, &err); if (q) sim_dns_put(q); break; }
    default: { struct pollfd p = { fd, POLLIN | POLLOUT, 0 }; call(SYS_POLL, (long)&p, 1, 0, 0, 0, 0); break; }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int dhcp = size && (data[0] & 0x80) && data[0] != 1;      /* (a raw seed record starts with 1) */
    begin(dhcp);
    size_t i = 0;
    static uint8_t frame[2048];
    while (i < size) {
        uint8_t t = data[i++];
        if (t == 0x80 || t == 0x81) continue;       /* (the DHCP flag byte) */
        unsigned kind = t & 7;
        if (kind >= 1 && kind <= 3) {
            if (i + 2 > size) break;
            unsigned len = rd16(data + i);
            i += 2;
            if (len > size - i) len = (unsigned)(size - i);
            if (len > 1600) len = 1600;
            memcpy(frame, data + i, len);
            i += len;
            if (kind >= 2) fix_frame(frame, len, kind == 3);
            rx_raw(frame, len);
        } else if (kind == 4) {
            if (i + 2 > size) break;
            advance(rd16(data + i));
            i += 2;
        } else if (kind == 5) {
            if (i + 1 > size) break;
            app_op(data[i++]);
        }
    }
    end(dhcp);
    return 0;
}

#ifndef FUZZ_LIBFUZZER
/* replay: every file (directories one level deep) through the harness */
static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    static uint8_t b[1 << 20];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    LLVMFuzzerTestOneInput(b, n);
}

int main(int argc, char **argv) {
    int runs = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st)) { fprintf(stderr, "cannot stat %s\n", argv[i]); return 2; }
        if (!S_ISDIR(st.st_mode)) { run_file(argv[i]); runs++; continue; }
        DIR *d = opendir(argv[i]);
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", argv[i], de->d_name);
            run_file(p);
            runs++;
        }
        closedir(d);
    }
    printf("fuzz_net_replay: %d input(s), no bugs\n", runs);
    return 0;
}
#endif
