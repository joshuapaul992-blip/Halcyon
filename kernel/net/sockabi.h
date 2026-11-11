/* Halcyon OS - sockets and poll: the part of the system call interface
 * (sysabi.h) owned by kernel/net/, system calls 100-139. See
 * docs/DESIGN-M9.md ("Sockets and poll").
 *
 * Names, structures and constant values follow BSD sockets as Linux lays
 * them out (a 16-bit family first in every address, Linux's AF_ / SOCK_ /
 * SO_ / MSG_ / POLL* numbers), so the Linux compatibility layer can pass
 * most of them through. Errors are Halcyon's negative E_* codes. */
#ifndef SOCKABI_H
#define SOCKABI_H

enum {
    SYS_SOCKET = 100,   /* (domain, type | SOCK_NONBLOCK, protocol) -> handle */
    SYS_BIND,           /* (fd, addr, addrlen) */
    SYS_LISTEN,         /* (fd, backlog) */
    SYS_ACCEPT,         /* (fd, addr or 0, socklen_t *addrlen or 0, flags: SOCK_NONBLOCK) -> handle */
    SYS_CONNECT,        /* (fd, addr, addrlen) */
    SYS_SENDTO,         /* (fd, buf, len, flags, addr or 0, addrlen) -> bytes */
    SYS_RECVFROM,       /* (fd, buf, len, flags, addr or 0, socklen_t *addrlen or 0) -> bytes, 0 = end */
    SYS_SHUTDOWN,       /* (fd, SHUT_RD / SHUT_WR / SHUT_RDWR) */
    SYS_GETSOCKNAME,    /* (fd, addr, socklen_t *addrlen) */
    SYS_GETPEERNAME,    /* (fd, addr, socklen_t *addrlen) */
    SYS_SOCKETPAIR,     /* (domain, type, protocol, int sv[2]) */
    SYS_SETSOCKOPT,     /* (fd, level, name, value, len) */
    SYS_GETSOCKOPT,     /* (fd, level, name, value, socklen_t *len) */
    SYS_IOCTLSOCKET,    /* (fd, FIONBIO / FIONREAD, unsigned long *arg) */
    SYS_POLL,           /* (struct pollfd *fds, nfds, timeout_ms; < 0 = forever) -> entries ready */
    SYS_SOCKSTAT,       /* (hal_sockstat_t *) */
    /* 116-127: the internet stack (kernel/net/inet.c, docs/DESIGN-M6.md) */
    SYS_RESOLVE = 116,  /* (host, unsigned *ipv4_out, max, timeout_ms) -> addresses found (>= 1) */
    SYS_NET_IFINFO,     /* (index, hal_ifinfo_t *) -> 0, E_NOENT past the last interface */
    SYS_NET_IFCONFIG,   /* (const hal_ifconfig_t *) -> 0: change (and save) an interface's configuration */
    SYS_NET_CTL,        /* (op NETCTL_*, const char *ifname or 0) */
    SYS_NET_STATS,      /* (hal_netstats_t *) */
    SYS_NET_CONNS,      /* (hal_netconn_t *, max) -> entries (TCP and UDP endpoints) */
    /* 122-127: free; 128 SYS_GETRANDOM; 129-139 free */
};

/* address families (socket()'s domain) */
#define AF_UNSPEC   0
#define AF_LOCAL    1           /* local sockets: programs on this computer, by name */
#define AF_UNIX     AF_LOCAL
#define AF_INET     2           /* IPv4: TCP, UDP, ICMP echo (milestone 6) */
#define AF_INET6    10          /* not yet */
#define PF_LOCAL    AF_LOCAL
#define PF_UNIX     AF_LOCAL
#define PF_INET     AF_INET
#define PF_INET6    AF_INET6

/* socket types; flags may be or-ed into the type */
#define SOCK_STREAM   1         /* reliable byte stream, connected */
#define SOCK_DGRAM    2         /* messages (boundaries kept) */
#define SOCK_NONBLOCK 0x800     /* calls return E_AGAIN instead of waiting */
#define SOCK_CLOEXEC  0x80000   /* accepted and ignored */

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

/* big enough for any address */
struct sockaddr_storage {
    sa_family_t ss_family;
    char ss_data[126];
} __attribute__((aligned(8)));

/* AF_LOCAL address: a name in one flat namespace, e.g. "halcyon.clipboard".
 * 1 to 63 characters: letters, digits, '.', '-' and '_', starting with a
 * letter or a digit; compared without regard to case. bind() with an empty
 * name picks a unique automatic name ("~" and hex digits). addrlen is
 * offsetof(sl_name) + the name's length (+ 1 for the NUL, optional). */
#define LOCAL_NAME_MAX 63
struct sockaddr_local {
    sa_family_t sl_family;              /* AF_LOCAL */
    char sl_name[LOCAL_NAME_MAX + 1];
};

/* AF_INET address (Linux layout): port and address in network byte order */
struct in_addr {
    unsigned int s_addr;
};
struct sockaddr_in {
    sa_family_t sin_family;             /* AF_INET */
    unsigned short sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};
#define INADDR_ANY       0x00000000u    /* (host byte order values, as in BSD) */
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_LOOPBACK  0x7F000001u
#define INADDR_NONE      0xFFFFFFFFu

/* socket()'s protocol, and the level of IP / TCP options */
#define IPPROTO_IP    0
#define IPPROTO_ICMP  1                 /* SOCK_DGRAM: "ping socket" (echo requests / replies) */
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
/* level IPPROTO_IP */
#define IP_TOS        1
#define IP_TTL        2
/* level IPPROTO_TCP */
#define TCP_NODELAY   1                 /* send small segments at once (no Nagle) */
#define TCP_MAXSEG    2                 /* get: the segment size in use */
#define TCP_KEEPIDLE  4                 /* seconds of idle before keepalive probes (default 7200) */
#define TCP_KEEPINTVL 5                 /* seconds between probes (75) */
#define TCP_KEEPCNT   6                 /* unanswered probes before the connection is dropped (9) */

/* send / recv flags */
#define MSG_PEEK      0x02      /* recv: leave the data queued */
#define MSG_TRUNC     0x20      /* recv (datagrams): return the real length even if longer than the buffer */
#define MSG_DONTWAIT  0x40      /* this call does not wait */
#define MSG_WAITALL   0x100     /* recv (streams): wait for the whole length (or end / error / timeout) */
#define MSG_NOSIGNAL  0x4000    /* accepted and ignored (there are no signals) */

/* shutdown() */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* socket options: level SOL_SOCKET */
#define SOL_SOCKET    1
#define SO_REUSEADDR  2         /* internet sockets: bind to a port that a closed connection still holds */
#define SO_TYPE       3         /* get: SOCK_STREAM / SOCK_DGRAM */
#define SO_ERROR      4         /* get: pending error (an E_* code or 0), then cleared */
#define SO_BROADCAST  6         /* UDP: may send to broadcast addresses */
#define SO_SNDBUF     7         /* send buffer size (internet sockets) */
#define SO_RCVBUF     8         /* receive buffer size: what peers can queue to this socket (clamped) */
#define SO_KEEPALIVE  9         /* TCP: probe an idle connection (see TCP_KEEP*) */
#define SO_LINGER     13        /* TCP: struct linger; { 1, 0 } = close() resets the connection */
#define SO_RCVTIMEO   20        /* recv / accept wait at most this long, then E_AGAIN */
#define SO_SNDTIMEO   21        /* send / connect wait at most this long, then E_AGAIN */
#define SO_ACCEPTCONN 30        /* get: 1 if listening */
#define SO_PROTOCOL   38        /* get */
#define SO_DOMAIN     39        /* get: AF_* */
/* SO_RCVTIMEO / SO_SNDTIMEO take a struct timeval, or an int of
 * milliseconds (by the option length); 0 = wait forever */
struct timeval {
    long tv_sec;
    long tv_usec;
};

struct linger {
    int l_onoff;
    int l_linger;               /* seconds; only 0 (abortive close) is acted on */
};

/* ioctlsocket() requests (Linux numbers); the argument is an unsigned long */
#define FIONREAD 0x541B         /* get: bytes that can be read now (datagrams: size of the next one) */
#define FIONBIO  0x5421         /* set: nonzero = non-blocking */

/* poll() */
struct pollfd {
    int fd;                     /* handle, FD_WINDOWS, or < 0 (ignored) */
    short events;               /* what to wait for */
    short revents;              /* what happened */
};
#define POLLIN     0x001        /* data to read (or end of stream), a connection to accept, window events */
#define POLLPRI    0x002
#define POLLOUT    0x004        /* a send would not wait */
#define POLLERR    0x008        /* a pending error (always reported) */
#define POLLHUP    0x010        /* the peer is gone / both directions shut (always reported) */
#define POLLNVAL   0x020        /* not a handle poll understands (always reported) */
#define POLLRDHUP  0x2000       /* nothing more will arrive */
/* the program's window events as a poll descriptor: POLLIN when
 * win_get_event() has something (an event or a timer that is due) */
#define FD_WINDOWS 0x7FFF0000

/* SYS_SOCKSTAT: socket use, system-wide and of the calling program */
typedef struct hal_sockstat {
    int sockets, max_sockets;           /* system-wide */
    int proc_sockets, max_proc_sockets; /* this program (including connections waiting in its backlogs) */
    unsigned long long mem, max_mem;    /* bytes queued in socket buffers, system-wide */
    unsigned long long proc_mem, max_proc_mem;  /* ... in this program's sockets */
    int names;                          /* bound local names */
    int max_backlog;                    /* listen() backlog limit */
    int rcvbuf_default, rcvbuf_max;     /* SO_RCVBUF */
} hal_sockstat_t;

/* ---- the internet stack's information and configuration calls ---- */

/* SYS_NET_IFINFO: one interface (index 0..n-1; the loopback "lo" is last) */
enum { NET_MODE_DHCP = 0, NET_MODE_STATIC = 1 };
enum { NET_DHCP_OFF = 0, NET_DHCP_INIT, NET_DHCP_SELECTING, NET_DHCP_REQUESTING, NET_DHCP_BOUND,
       NET_DHCP_RENEWING, NET_DHCP_REBINDING, NET_DHCP_REBOOTING };
typedef struct hal_ifinfo {
    char name[8];                       /* "eth0", "lo" */
    char desc[64];                      /* "Intel 82540EM Gigabit Ethernet" */
    char driver[16];
    unsigned char mac[6];
    unsigned short mtu;
    int index, loopback;
    int link_up, speed_mbps, full_duplex;
    int mode;                           /* NET_MODE_* */
    int dhcp_state;                     /* NET_DHCP_* */
    int conflict;                       /* another computer uses our address */
    unsigned int addr, mask, gateway;   /* network byte order; 0 = none */
    unsigned int dns[3];                /* DNS servers in use (system-wide) */
    unsigned int dhcp_server;
    long long lease_s;                  /* DHCP lease granted, seconds (-1 none) */
    long long lease_left_s;             /* ... left */
    unsigned long long rx_packets, rx_bytes, rx_errors, rx_dropped;
    unsigned long long tx_packets, tx_bytes, tx_errors, tx_dropped;
} hal_ifinfo_t;

/* SYS_NET_IFCONFIG: change an interface's configuration (applied at once) */
#define NETCFG_SAVE 0x01                /* also save it in SETTINGS.INI (net.<if>.*, net.dns) */
#define NETCFG_DNS  0x02                /* set the system-wide DNS servers from dns[] (all 0: use DHCP's) */
#define NETCFG_ADDR 0x04                /* set mode / addresses (otherwise only the DNS part) */
typedef struct hal_ifconfig {
    char name[8];                       /* "eth0" */
    int mode;                           /* NET_MODE_DHCP or NET_MODE_STATIC */
    unsigned int addr, mask, gateway;   /* static: network byte order (gateway 0 = none) */
    unsigned int dns[3];
    int flags;                          /* NETCFG_* */
} hal_ifconfig_t;

/* SYS_NET_CTL */
enum { NETCTL_RENEW = 1,                /* DHCP: renew the lease now (or start over) */
       NETCTL_RELEASE,                  /* DHCP: give the address back */
       NETCTL_DNS_FLUSH,                /* empty the DNS cache */
       NETCTL_ARP_FLUSH };              /* forget the interface's ARP cache */

/* SYS_NET_STATS: stack counters since boot */
typedef struct hal_netstats {
    unsigned long long ip_in, ip_out, ip_bad, ip_frag_in, ip_reasm_ok, ip_reasm_fail, ip_frag_out, ip_noroute;
    unsigned long long icmp_in, icmp_out, icmp_err_in, icmp_err_out, icmp_ratelimited;
    unsigned long long arp_in, arp_out, arp_fail;
    unsigned long long udp_in, udp_out, udp_noport, udp_drop;
    unsigned long long tcp_in, tcp_out, tcp_bad, tcp_rexmt, tcp_fast_rexmt, tcp_timeouts;
    unsigned long long tcp_rst_in, tcp_rst_out, tcp_active, tcp_passive, tcp_established;
    unsigned long long tcp_dupack, tcp_ooo, tcp_challenge, tcp_synq_drop;
    unsigned long long dns_queries, dns_hits, dns_rejected;
    unsigned long long loss_dropped;    /* test hook (fw_cfg opt/halcyon/netloss) */
    int dns_cached, tcp_conns, udp_socks;
    int arp_entries;
} hal_netstats_t;

/* SYS_NET_CONNS: TCP connections and listeners, UDP and ping sockets */
enum { NETC_TCP = 6, NETC_UDP = 17, NETC_ICMP = 1 };
enum { TCPS_CLOSED = 0, TCPS_LISTEN, TCPS_SYN_SENT, TCPS_SYN_RCVD, TCPS_ESTABLISHED, TCPS_FIN_WAIT_1,
       TCPS_FIN_WAIT_2, TCPS_CLOSE_WAIT, TCPS_CLOSING, TCPS_LAST_ACK, TCPS_TIME_WAIT };
typedef struct hal_netconn {
    int proto;                          /* NETC_* */
    int state;                          /* TCP: TCPS_*; UDP: 0 */
    unsigned int laddr, raddr;          /* network byte order */
    unsigned short lport, rport;        /* host byte order */
    unsigned int rxq, txq;              /* bytes queued */
    unsigned int pid;                   /* owning program, 0 = none (kernel, or closed) */
} hal_netconn_t;

#endif
