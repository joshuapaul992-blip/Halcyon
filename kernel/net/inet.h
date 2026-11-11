/* Halcyon OS - the IPv4 protocol stack (milestone 6): shared definitions.
 * See docs/DESIGN-M6.md.
 *
 *   inet.c   the AF_INET family (socket(), ports), the stack hooks the net
 *            thread calls (init / input / poll / link), loopback, the
 *            interface and configuration system calls (116-127)
 *   eth.c    interfaces, Ethernet, ARP, the routing table
 *   ip.c     IPv4 input (checks, reassembly) and output (routing,
 *            fragmentation, path MTU), ICMP
 *   udp.c    UDP sockets, ping sockets (IPPROTO_ICMP), kernel endpoints
 *   tcp.c    TCP (RFC 9293 with 5681, 6298, 6582, 7323 window scaling, 5961)
 *   dhcp.c   the DHCP client and static configuration
 *   dns.c    the resolver (SYS_RESOLVE)
 *
 * Everything runs under the big kernel lock: in the net thread (frames,
 * timers) or in a program's system call (sending, reading, configuration).
 * Nothing here blocks except the waits in the system calls of inet.c.
 *
 * Byte order: IPv4 addresses are u32 in network byte order (as they are in
 * packets and in sockaddr_in) everywhere; ports are u16 in host order inside
 * the stack and converted at the sockaddr / packet boundary. */
#ifndef NET_INET_H
#define NET_INET_H
#include "core/types.h"
#include "net/netdev.h"
#include "net/socket.h"

/* ---------------- byte order, checksums ---------------- */
static inline u16 rd16(const void *p) { const u8 *b = p; return (u16)(b[0] << 8 | b[1]); }
static inline u32 rd32(const void *p) { const u8 *b = p; return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]; }
static inline void wr16(void *p, u16 v) { u8 *b = p; b[0] = (u8)(v >> 8); b[1] = (u8)v; }
static inline void wr32(void *p, u32 v) { u8 *b = p; b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16); b[2] = (u8)(v >> 8); b[3] = (u8)v; }
/* an address as stored (network order) from / to packet bytes */
static inline u32 rdaddr(const void *p) { u32 a; __builtin_memcpy(&a, p, 4); return a; }
static inline void wraddr(void *p, u32 a) { __builtin_memcpy(p, &a, 4); }
static inline u32 hton32(u32 v) { return __builtin_bswap32(v); }
static inline u16 hton16(u16 v) { return __builtin_bswap16(v); }
#define ntoh32 hton32
#define ntoh16 hton16
#define IP4(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)   /* network order */

u32 csum_add(u32 sum, const void *data, u32 len);   /* one's complement sum (not folded) */
u16 csum_fold(u32 sum);                             /* final checksum (complemented) */
u32 csum_pseudo(u32 src, u32 dst, u8 proto, u32 len);
/* "a.b.c.d" (static buffer per call slot, 4 slots) */
const char *ipstr(u32 a);
u64 net_hash(const void *data, u32 len);            /* SipHash-2-4, secret key */

/* sequence-number arithmetic (TCP) */
#define SEQ_LT(a, b)  ((i32)((u32)(a) - (u32)(b)) < 0)
#define SEQ_LEQ(a, b) ((i32)((u32)(a) - (u32)(b)) <= 0)
#define SEQ_GT(a, b)  ((i32)((u32)(a) - (u32)(b)) > 0)
#define SEQ_GEQ(a, b) ((i32)((u32)(a) - (u32)(b)) >= 0)

/* ---------------- interfaces (eth.c) ---------------- */
#define IF_MAX          (NETDEV_MAX + 1)
#define LO_MTU          16384
#define ARP_MAX         64      /* cache entries per interface */
#define ARP_QUEUE       8       /* packets waiting for one resolution */
#define ARP_TRIES       3       /* requests, one second apart */
#define ARP_REACHABLE_MS 60000  /* an answer is trusted this long, then refreshed */
#define ARP_STALE_MS    20000   /* ... and dropped when the refresh gets no answer */
#define ARP_UNUSED_MS   300000  /* unused entries are dropped */
#define IF_TXQ          256     /* frames waiting for a full transmit ring */
#define DNS_SERVERS     3

enum { IFMODE_DHCP = 0, IFMODE_STATIC };
enum { ARP_FREE = 0, ARP_INCOMPLETE, ARP_REACHABLE };

typedef struct qpkt {           /* a queued frame / packet */
    struct qpkt *next;
    u32 len;
    u64 time;
    u8 data[];
} qpkt_t;

typedef struct arp_ent {
    u32 ip;
    u8 mac[6];
    u8 state, tries;
    u64 confirmed, used, next_try;
    qpkt_t *q;                  /* IP packets (14 bytes of room in front) waiting */
    int nq;
} arp_ent_t;

struct dhcp;
typedef struct iface {
    int index;
    char name[8];
    netdev_t *dev;              /* NULL: loopback */
    u8 mac[6];
    u16 mtu;
    int up;
    int mode;                   /* IFMODE_* (the loopback is static) */
    u32 addr, mask, gw;         /* network order; addr 0 = not configured */
    u32 dns[DNS_SERVERS];       /* learned from DHCP (or static) */
    u16 dns_port[DNS_SERVERS];  /* 53 unless the setting says otherwise (tests) */
    int ndns;
    int conflict;               /* another host answered for our address */
    u64 garp_at;                /* second gratuitous ARP due */
    u64 defend_at;              /* last ARP defence (RFC 5227) */
    arp_ent_t arp[ARP_MAX];
    qpkt_t *txq, *txq_tail;
    int ntxq;
    struct dhcp *dhcp;
    u64 rx_drop_stack, tx_drop_stack;   /* (loopback: its own packet counters) */
    u64 lo_packets, lo_bytes;
} iface_t;

int iface_count(void);
iface_t *iface_get(int i);
iface_t *iface_by_name(const char *name);
iface_t *iface_lo(void);
iface_t *iface_of_dev(netdev_t *d);
void iface_init(void);
/* set (or with addr 0 clear) an interface's address; how = "DHCP" / "static" */
void iface_set_addr(iface_t *ifc, u32 addr, u32 mask, u32 gw, const char *how);
int  ip_is_local(u32 a);                    /* one of our addresses (any interface, 127/8) */
int  ip_is_bcast(iface_t *ifc, u32 a);      /* 255.255.255.255 or the subnet broadcast */
static inline int ip_is_mcast(u32 a) { return (ntoh32(a) >> 28) == 14; }
static inline int ip_is_loopnet(u32 a) { return (ntoh32(a) >> 24) == 127; }
static inline int ip_bad_unicast(u32 a) {   /* never a valid unicast address */
    u32 h = ntoh32(a);
    return !h || h == 0xFFFFFFFF || (h >> 28) >= 14 || (h >> 24) == 0;
}

typedef struct route_res {
    iface_t *ifc;
    u32 nexthop;                /* on-link address to resolve */
    u32 src;                    /* preferred source address */
    int bcast;                  /* link-layer broadcast */
} route_res_t;
int  route_lookup(u32 dst, route_res_t *r);   /* 0 or E_NETUNREACH */
void route_rebuild(void);
int  route_list(u32 *dst, u32 *mask, u32 *gw, int *ifindex, int max);

/* send an IP packet whose bytes start at pkt (with ETH_HDR bytes of room
 * in front of it) on ifc to the on-link nexthop; 0 or E_* */
int  eth_output(iface_t *ifc, u32 nexthop, int bcast, u8 *pkt, u32 len);
void eth_input(iface_t *ifc, const u8 *frame, u32 len);
void eth_flush(iface_t *ifc);               /* retry frames the ring had no room for */
u64  arp_timer(u64 now);                    /* aging, retries; next deadline */
void arp_announce(iface_t *ifc);            /* gratuitous ARP */
void arp_flush(iface_t *ifc);               /* forget everything (link down, new address) */
int  arp_probe_conflict(iface_t *ifc, u32 addr);
int  arp_list(iface_t *ifc, u32 *ip, u8 (*mac)[6], int max);

/* ---------------- IP (ip.c) ---------------- */
#define IP_HDR          20
#define HDRROOM         (ETH_HDR + IP_HDR + 30)     /* room in front of a transport header */
#define IP_DEFAULT_TTL  64
#define REASM_MAX       16          /* datagrams being reassembled */
#define REASM_MEM       (512u << 10)
#define REASM_FRAGS     64          /* fragments per datagram */
#define REASM_TIMEOUT_MS 30000
#define PMTU_MAX        32
#define PMTU_EXPIRE_MS  600000
#define IP_MAX_PAYLOAD  (65535 - IP_HDR)

enum { IPF_DF = 1, IPF_NOFRAG_LOCAL = 2 };
typedef struct ip_meta {        /* a received datagram */
    iface_t *ifc;
    u32 src, dst;
    const u8 *hdr;              /* its IP header (for ICMP errors) */
    u32 hlen, totlen;
    int bcast;                  /* sent to a broadcast address */
    u8 ttl, tos;
} ip_meta_t;

/* the transport packet is buf[off .. off+len); off >= HDRROOM - 30 + 20... at
 * least ETH_HDR + IP_HDR bytes are free in front of it. src 0 = choose.
 * force: send on this interface (DHCP), nexthop = dst. */
int  ip_output(u8 *buf, u32 off, u32 len, u32 src, u32 dst, u8 proto, int ttl, int tos, int flags, iface_t *force);
void ip_input(iface_t *ifc, const u8 *pkt, u32 len, int l2bcast);
u64  ip_timer(u64 now);
u32  pmtu_get(u32 dst);                     /* path MTU (0 = interface MTU) */
u32  ip_route_mtu(u32 dst);                 /* min(path MTU, interface MTU); 0 = no route */
/* ICMP */
void icmp_input(const ip_meta_t *m, const u8 *p, u32 len);
void icmp_error(const ip_meta_t *m, u8 type, u8 code, u32 extra);
/* an error about a packet we sent (from ICMP, or ARP failing): hand it to
 * the socket it came from. orig = our IP header + at least 8 bytes */
void ip_deliver_error(const u8 *orig, u32 len, int err, u8 type, u8 code, u32 mtu, int local);
int  icmp_err_to_errno(u8 type, u8 code);

/* ---------------- UDP and ping sockets (udp.c) ---------------- */
typedef struct upcb upcb_t;
typedef void (*ukin_t)(void *ctx, const ip_meta_t *m, u16 sport, const u8 *data, u32 len);
void udp_input(const ip_meta_t *m, const u8 *p, u32 len);
void udp_err(const u8 *orig, u32 len, int err, u8 type, u8 code);
void ping_input(const ip_meta_t *m, const u8 *p, u32 len);   /* an echo reply */
void ping_err(const u8 *orig, u32 len, int err);
/* kernel endpoints (DNS): a local port (0 = random ephemeral) with a callback */
upcb_t *udp_kopen(u16 port, ukin_t fn, void *ctx);
int  udp_ksend(upcb_t *u, u32 dst, u16 dport, const void *data, u32 len);
void udp_kclose(upcb_t *u);
u16  udp_kport(upcb_t *u);
/* DHCP: from src (may be 0) on ifc, nexthop = dst */
int  udp_send_if(iface_t *ifc, u32 src, u16 sport, u32 dst, u16 dport, const void *data, u32 len);
extern const sock_ops_t udp_ops;
int  udp_create(sock_t *s, int ping);
int  udp_count(void);

/* ---------------- TCP (tcp.c) ---------------- */
void tcp_input(const ip_meta_t *m, const u8 *p, u32 len);
void tcp_err(const u8 *orig, u32 len, int err, u8 type, u8 code, u32 mtu, int local);
u64  tcp_timer(u64 now);
extern const sock_ops_t tcp_ops;
int  tcp_create(sock_t *s);
void tcp_init(void);
int  tcp_count(void);
int  tcp_conns(hal_netconn_t *out, int max);
int  udp_conns(hal_netconn_t *out, int max);

/* ---------------- addresses and ports (inet.c) ---------------- */
#define PORT_EPH_LO 49152
#define PORT_EPH_HI 65535
u16  port_random(void);                     /* a random ephemeral port */
/* sockaddr_in -> address (network order) and port (host order) */
int  sin_parse(const struct sockaddr *a, u32 len, u32 *addr, u16 *port);
u32  sin_make(struct sockaddr *a, u32 room, u32 addr, u16 port);
/* IP-level and common options of inet sockets (IP_TTL, IP_TOS) */
int  inet_ipopt_set(int *ttl, int *tos, int name, const void *v, u32 len);
int  inet_ipopt_get(int ttl, int tos, int name, void *v, u32 *len);

/* ---------------- DHCP and configuration (dhcp.c) ---------------- */
void net_config_init(void);                 /* settings -> DHCP or static, per interface */
void dhcp_input(iface_t *ifc, const ip_meta_t *m, const u8 *p, u32 len);
u64  dhcp_timer(u64 now);
void dhcp_link(iface_t *ifc);
void dhcp_conflict(iface_t *ifc);           /* ARP saw another host with our address */
int  dhcp_renew(iface_t *ifc);
int  dhcp_release(iface_t *ifc);
int  dhcp_state(iface_t *ifc, u32 *server, i64 *left_s, i64 *lease_s);
void dhcp_stop(iface_t *ifc);
void dhcp_start(iface_t *ifc);
/* the system-wide DNS servers: net.dns if set, else the interfaces' */
int  net_dns_servers(u32 *ip, u16 *port, int max);
void net_dns_override(const u32 *ip, const u16 *port, int n);   /* n = 0: use DHCP's */

/* ---------------- DNS (dns.c) ---------------- */
#define DNS_CACHE_MAX   64
#define DNS_PENDING_MAX 16
#define DNS_MAX_ADDRS   16
typedef struct dnsq dnsq_t;
int  dns_literal(const char *name, u32 *out);           /* 1 for "localhost" / a.b.c.d, 0 otherwise */
int  dns_name_ok(const char *name);
int  dns_cached(const char *name, u32 *out, int max);   /* n, 0 = not cached, E_NOENT */
dnsq_t *dns_start(const char *name, u64 deadline_ms, int *err);
int  dns_result(dnsq_t *q, u32 *out, int max);          /* n, E_AGAIN while pending, E_* */
int  dns_pending(dnsq_t *q);
void dns_put(dnsq_t *q);
u64  dns_timer(u64 now);
void dns_flush(void);
void dns_counts(u32 *cached, u64 *queries, u64 *hits, u64 *answers_rejected);

/* ---------------- stack-wide ---------------- */
void net_poke(void);                        /* output queued from a system call: wake the net thread */
void lo_output(const u8 *pkt, u32 len);     /* a packet for ourselves */
extern int net_loss_pct;                    /* test hook: drop this % of frames */
int  net_loss(void);                        /* 1 = drop this frame (test hook) */

/* counters for SYS_NET_STATS */
typedef struct netstat_k {
    u64 ip_in, ip_out, ip_bad, ip_noproto, ip_notours, ip_frag_in, ip_reasm_ok, ip_reasm_fail, ip_frag_out, ip_noroute;
    u64 icmp_in, icmp_out, icmp_err_in, icmp_err_out, icmp_ratelimited, icmp_bad;
    u64 arp_in, arp_out, arp_fail, arp_conflict;
    u64 udp_in, udp_out, udp_noport, udp_bad, udp_drop;
    u64 tcp_in, tcp_out, tcp_bad, tcp_rexmt, tcp_fast_rexmt, tcp_timeouts, tcp_rst_in, tcp_rst_out;
    u64 tcp_active, tcp_passive, tcp_established, tcp_dupack, tcp_ooo, tcp_challenge, tcp_synq_drop, tcp_persist;
    u64 tcp_keep_drop, tcp_drop;
    u64 loss_dropped;
} netstat_k_t;
extern netstat_k_t nstat;

#endif
