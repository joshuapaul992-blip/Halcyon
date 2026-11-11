/* Halcyon user-mode sockets and poll: thin wrappers over the system calls
 * (kernel/net/, numbers in kernel/net/sockabi.h) */
#include "hal.h"

int  socket(int d, int t, int p) { return (int)SC3(SYS_SOCKET, d, t, p); }
int  bind(int fd, const struct sockaddr *a, socklen_t l) { return (int)SC3(SYS_BIND, fd, a, l); }
int  listen(int fd, int backlog) { return (int)SC2(SYS_LISTEN, fd, backlog); }
int  accept(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC4(SYS_ACCEPT, fd, a, l, 0); }
int  accept4(int fd, struct sockaddr *a, socklen_t *l, int flags) { return (int)SC4(SYS_ACCEPT, fd, a, l, flags); }
int  connect(int fd, const struct sockaddr *a, socklen_t l) { return (int)SC3(SYS_CONNECT, fd, a, l); }
long send(int fd, const void *b, size_t n, int flags) { return SC6(SYS_SENDTO, fd, b, n, flags, 0, 0); }
long recv(int fd, void *b, size_t n, int flags) { return SC6(SYS_RECVFROM, fd, b, n, flags, 0, 0); }
long sendto(int fd, const void *b, size_t n, int flags, const struct sockaddr *to, socklen_t tl) {
    return SC6(SYS_SENDTO, fd, b, n, flags, to, tl);
}
long recvfrom(int fd, void *b, size_t n, int flags, struct sockaddr *from, socklen_t *fl) {
    return SC6(SYS_RECVFROM, fd, b, n, flags, from, fl);
}
int  shutdown(int fd, int how) { return (int)SC2(SYS_SHUTDOWN, fd, how); }
int  getsockname(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC3(SYS_GETSOCKNAME, fd, a, l); }
int  getpeername(int fd, struct sockaddr *a, socklen_t *l) { return (int)SC3(SYS_GETPEERNAME, fd, a, l); }
int  socketpair(int d, int t, int p, int sv[2]) { return (int)SC4(SYS_SOCKETPAIR, d, t, p, sv); }
int  setsockopt(int fd, int level, int name, const void *v, socklen_t l) { return (int)SC5(SYS_SETSOCKOPT, fd, level, name, v, l); }
int  getsockopt(int fd, int level, int name, void *v, socklen_t *l) { return (int)SC5(SYS_GETSOCKOPT, fd, level, name, v, l); }
int  ioctlsocket(int fd, long cmd, unsigned long *arg) { return (int)SC3(SYS_IOCTLSOCKET, fd, cmd, arg); }
int  poll(struct pollfd *fds, unsigned nfds, int timeout_ms) { return (int)SC3(SYS_POLL, fds, nfds, timeout_ms); }
int  sockstat(hal_sockstat_t *st) { return (int)SC1(SYS_SOCKSTAT, st); }

socklen_t local_addr(struct sockaddr_local *a, const char *name) {
    memset(a, 0, sizeof *a);
    a->sl_family = AF_LOCAL;
    strlcpy(a->sl_name, name ? name : "", sizeof a->sl_name);
    return (socklen_t)(sizeof a->sl_family + strlen(a->sl_name) + 1);
}

int sock_nonblock(int fd, int on) {
    unsigned long v = on != 0;
    return ioctlsocket(fd, FIONBIO, &v);
}

int sock_timeouts(int fd, int recv_ms, int send_ms) {
    int r = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_ms, sizeof recv_ms);
    return r ? r : setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_ms, sizeof send_ms);
}

int local_listen(const char *name, int backlog) {
    struct sockaddr_local a;
    socklen_t l = local_addr(&a, name);
    int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    int r = bind(fd, (struct sockaddr *)&a, l);
    if (!r) r = listen(fd, backlog);
    if (r) { close(fd); return r; }
    return fd;
}

/* ---------------- internet ---------------- */

int resolve(const char *host, unsigned int *out, int max, int timeout_ms) {
    return (int)SC4(SYS_RESOLVE, host, out, max, timeout_ms);
}
int net_ifinfo(int index, hal_ifinfo_t *out) { return (int)SC2(SYS_NET_IFINFO, index, out); }
int net_ifconfig(const hal_ifconfig_t *cfg) { return (int)SC1(SYS_NET_IFCONFIG, cfg); }
int net_ctl(int op, const char *ifname) { return (int)SC2(SYS_NET_CTL, op, ifname); }
int net_stats(hal_netstats_t *st) { return (int)SC1(SYS_NET_STATS, st); }
int net_conns(hal_netconn_t *out, int max) { return (int)SC2(SYS_NET_CONNS, out, max); }

int inet_aton(const char *s, struct in_addr *out) {
    unsigned v[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned x = 0;
        int d = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (unsigned)(*s++ - '0'); if (++d > 3 || x > 255) return 0; }
        v[i] = x;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    if (out) out->s_addr = v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24;
    return 1;
}

unsigned int inet_addr(const char *s) {
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa_r(unsigned int addr, char *buf) {
    const unsigned char *p = (const unsigned char *)&addr;
    snprintf(buf, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

const char *inet_ntoa(struct in_addr a) {
    static char buf[16];
    return inet_ntoa_r(a.s_addr, buf);
}

socklen_t inet_sockaddr(struct sockaddr_in *a, unsigned int addr, int port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((unsigned short)port);
    a->sin_addr.s_addr = addr;
    return sizeof *a;
}

int inet_connect(const char *host, int port, int timeout_ms) {
    unsigned int addrs[8];
    int n = resolve(host, addrs, 8, timeout_ms);
    if (n < 0) return n;
    int r = E_CONNREFUSED;
    for (int i = 0; i < n; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return fd;
        if (timeout_ms > 0) sock_timeouts(fd, 0, timeout_ms);
        struct sockaddr_in a;
        r = connect(fd, (struct sockaddr *)&a, inet_sockaddr(&a, addrs[i], port));
        if (!r) { sock_timeouts(fd, 0, 0); return fd; }
        close(fd);
        if (r == E_AGAIN) r = E_TIMEOUT;
    }
    return r;
}

int local_connect(const char *name) {
    struct sockaddr_local a;
    socklen_t l = local_addr(&a, name);
    int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    int r = connect(fd, (struct sockaddr *)&a, l);
    if (r) { close(fd); return r; }
    return fd;
}
