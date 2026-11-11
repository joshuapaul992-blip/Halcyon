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

int local_connect(const char *name) {
    struct sockaddr_local a;
    socklen_t l = local_addr(&a, name);
    int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) return fd;
    int r = connect(fd, (struct sockaddr *)&a, l);
    if (r) { close(fd); return r; }
    return fd;
}
