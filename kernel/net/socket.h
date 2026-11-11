#ifndef NET_SOCKET_H
#define NET_SOCKET_H
/* Halcyon OS - the socket layer (see docs/DESIGN-M9.md, "Sockets and poll").
 *
 * Two levels:
 *   socket.c   generic sockets: handles, the system calls, waiting (blocking,
 *              non-blocking, timeouts, a process being ended), poll, socket
 *              options, limits and memory accounting, socket buffers.
 *   families   one per address family: local.c (AF_LOCAL), inet.c (AF_INET:
 *              TCP, UDP and ping sockets, docs/DESIGN-M6.md); AF_INET6 later. A family only implements
 *              non-blocking primitives (sock_ops_t) and says what changed by
 *              calling sock_wake(); the generic layer does every wait.
 *
 * What a family provides (an internet family in milestone 6 included):
 *   - a net_family_t entry in the table in socket.c (family number, create,
 *     optional pair for socketpair);
 *   - create(): check type / protocol, allocate s->priv, set s->ops;
 *   - the sock_ops_t below. Every operation is non-blocking: return E_AGAIN
 *     when the caller would have to wait, after making sure that whatever
 *     ends the wait calls sock_wake() on s, or on the socket stored in
 *     s->watch (e.g. the listener of a full backlog, the target of a full
 *     datagram queue). A TCP connect returns E_INPROGRESS, then reports
 *     POLLOUT (connected) or POLLERR with s->error set; the generic layer
 *     waits for that when the socket is blocking;
 *   - data copies through uio_t (user or kernel buffers), memory through
 *     the nq_* socket buffers or net_charge() so that the per-program and
 *     system-wide limits hold; ops->poll() returning the POLL* mask now;
 *   - release(): tear down completely (peers told, names / ports freed,
 *     buffers freed); no other operation is called after it;
 *   - work done outside system calls (packets arriving from a network
 *     card, timers) runs in a kernel thread and ends with sock_wake().
 *
 * Locking: the implicit big kernel lock. Nothing here blocks except
 * sock_wait(), which only the generic layer calls, so a family's state does
 * not change while one of its operations runs. */
#include "core/types.h"
#include "core/sysabi.h"

typedef struct sock sock_t;
typedef struct netacct netacct_t;

/* a caller's buffer: user memory of the current process, or kernel memory */
typedef struct uio {
    u64 addr;
    u64 len;
    u64 done;                   /* bytes copied so far */
    int kernel;
} uio_t;
static inline u64 uio_left(const uio_t *u) { return u->len - u->done; }
int uio_in(uio_t *u, void *dst, u64 n);         /* from the buffer (advances); E_INVAL on a bad pointer */
int uio_out(uio_t *u, const void *src, u64 n);  /* into the buffer (advances) */
int uio_check_out(const uio_t *u, u64 n);       /* the next n bytes are writable */

/* ---- socket buffers: a queue of byte runs (streams) or messages ---- */
typedef struct nbuf {
    struct nbuf *next;
    u32 cap, len, off;          /* data[off..len) not read yet; datagrams: data[0..alen) is the sender's address */
    u16 alen, msg;              /* msg: one datagram */
    u8 data[];
} nbuf_t;

typedef struct nqueue {
    nbuf_t *head, *tail;
    u32 bytes;                  /* payload not read yet */
    u32 count;                  /* datagrams queued */
    u32 mem;                    /* memory charged */
    sock_t *owner;              /* the receiving socket: charged to its program (s->acct) */
} nqueue_t;

/* append up to max bytes of u as stream data; bytes or E_NOMEM / E_INVAL */
long nq_put_stream(nqueue_t *q, uio_t *u, u32 max);
/* append one datagram of uio_left(u) bytes from the sender address a */
int  nq_put_msg(nqueue_t *q, uio_t *u, const void *a, u32 alen);
/* stream read up to uio_left(u) bytes; bytes or E_INVAL */
long nq_get_stream(nqueue_t *q, uio_t *u, int peek);
/* read the next datagram: copies what fits, returns its full length;
 * *alen in: room at a, out: the address length */
long nq_get_msg(nqueue_t *q, uio_t *u, int peek, void *a, u32 *alen);
u32  nq_next_msg_len(const nqueue_t *q);
/* streams: copy n bytes starting off bytes into the unread data (TCP
 * retransmissions; the caller checks the bounds), drop n bytes from the front */
void nq_copy(const nqueue_t *q, u32 off, void *dst, u32 n);
void nq_drop(nqueue_t *q, u32 n);
void nq_free(nqueue_t *q);

/* ---- sockets ---- */
typedef struct sock_ops {
    void (*release)(sock_t *s);
    int  (*bind)(sock_t *s, const struct sockaddr *a, u32 len);
    int  (*listen)(sock_t *s, int backlog);
    int  (*accept)(sock_t *s, sock_t **out);                            /* E_AGAIN: nothing waiting */
    int  (*connect)(sock_t *s, const struct sockaddr *a, u32 len);      /* 0, E_AGAIN, E_INPROGRESS */
    /* bytes taken (streams may take part), E_AGAIN when nothing could be */
    long (*send)(sock_t *s, uio_t *u, int flags, const struct sockaddr *to, u32 tolen);
    /* bytes, 0 = end of stream, E_AGAIN; *fromlen in: room at from (may be 0) */
    long (*recv)(sock_t *s, uio_t *u, int flags, struct sockaddr *from, u32 *fromlen);
    int  (*shutdown)(sock_t *s, int how);
    int  (*getname)(sock_t *s, int peer, struct sockaddr *a, u32 *len);  /* *len in: room */
    int  (*poll)(sock_t *s);                                            /* POLL* mask */
    int  (*ioctl)(sock_t *s, long req, u64 *arg);                       /* FIONREAD ...; may be NULL */
    int  (*setopt)(sock_t *s, int level, int name, const void *v, u32 len);   /* options the generic layer leaves; may be NULL */
    int  (*getopt)(sock_t *s, int level, int name, void *v, u32 *len);
} sock_ops_t;

typedef struct net_family {
    int family;                 /* AF_* */
    const char *name;
    int (*create)(sock_t *s, int type, int protocol);   /* E_NOTSUP for unknown types / protocols */
    int (*pair)(sock_t *a, sock_t *b);                   /* connect two new sockets (socketpair); NULL = E_NOTSUP */
} net_family_t;

struct sock {
    const net_family_t *fam;
    const sock_ops_t *ops;
    void *priv;                 /* family state */
    int family, type, protocol;
    int refs;                   /* handle (or owner such as a listener's backlog) + calls in progress */
    int closed;                 /* released: no more ops calls */
    int nonblock, listening, reuseaddr;
    int error;                  /* pending error (SO_ERROR), reported once */
    u32 gen;                    /* bumped by sock_wake() on every change */
    sock_t *watch;              /* another socket whose changes matter to a wait / poll of this one */
    u32 rcvbuf, sndbuf;
    u64 rcvtimeo_ms, sndtimeo_ms;   /* 0 = forever */
    netacct_t *acct;            /* owning program (NULL: kernel or released) */
    sock_t *all_next;
};

/* limits */
#define NET_MAX_SOCKETS     512             /* system-wide */
#define NET_PROC_SOCKETS    128             /* per program, including connections in its backlogs */
#define NET_MAX_MEM         (32u << 20)     /* bytes in socket buffers, system-wide */
#define NET_PROC_MEM        (4u << 20)      /* ... in the sockets of one program */
#define NET_HEAP_RESERVE    (32ull << 20)   /* kernel heap sockets never touch */
#define NET_MAX_BACKLOG     16
#define NET_RCVBUF_DEFAULT  (64u * 1024)
#define NET_BUF_MIN         2048
#define NET_BUF_MAX         (256u * 1024)
#define NET_POLL_MAX        128             /* poll() entries */

void sock_wake(sock_t *s);                  /* something changed: waiters and pollers look again */
/* a new socket with one reference (its owner's); E_NOMEM at a limit */
int  sock_new(const net_family_t *f, int type, int protocol, netacct_t *acct, sock_t **out);
void sock_get(sock_t *s);
void sock_put(sock_t *s);
void sock_close(sock_t *s);                 /* release + drop the owner's reference */
sock_t *sock_first(void);                   /* every socket (including released ones still referenced) */

/* memory accounting for buffers families allocate themselves */
int  net_charge(netacct_t *a, u32 bytes);   /* E_NOMEM over a limit */
void net_uncharge(netacct_t *a, u32 bytes);
u32  net_acct_pid(const netacct_t *a);      /* the program it belongs to (0: none) */

extern const net_family_t local_family;     /* local.c */
extern const net_family_t inet_family;      /* inet.c (AF_INET: TCP, UDP, ping) */
int local_name_count(void);

#endif
