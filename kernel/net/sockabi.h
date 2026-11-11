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
    /* 116-139: free (internet families: name resolution, interfaces...) */
};

/* address families (socket()'s domain) */
#define AF_UNSPEC   0
#define AF_LOCAL    1           /* local sockets: programs on this computer, by name */
#define AF_UNIX     AF_LOCAL
#define AF_INET     2           /* not yet (milestone 6) */
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
#define SO_REUSEADDR  2         /* stored (for internet sockets) */
#define SO_TYPE       3         /* get: SOCK_STREAM / SOCK_DGRAM */
#define SO_ERROR      4         /* get: pending error (an E_* code or 0), then cleared */
#define SO_SNDBUF     7         /* send buffer size (internet sockets) */
#define SO_RCVBUF     8         /* receive buffer size: what peers can queue to this socket (clamped) */
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

#endif
