/* Halcyon OS - generic sockets (see socket.h and docs/DESIGN-M9.md):
 * the socket system calls (100-139), H_SOCKET handles, waiting, poll,
 * socket options, limits and memory accounting, socket buffers. */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/thread.h"
#include "core/proc.h"
#include "net/socket.h"

/* the program's window events, for poll (gui/usergui.c) */
int usergui_poll_ready(proc_t *p, u64 *next_timer_ms) __attribute__((weak));

/* the internet stack's system calls (116-127, net/inet.c) */
long inet_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) __attribute__((weak));

/* address families (AF_INET6 later) */
static const net_family_t *const families[] = { &local_family, &inet_family };

/* ---------------- accounting and limits ---------------- */

/* per program: sockets (including connections waiting in its backlogs) and
 * bytes in their receive buffers */
struct netacct { u32 pid; int used; int sockets; u64 mem; };
static netacct_t accts[MAX_PROCS + 16];
static int nsockets;
static u64 net_mem;
static sock_t *all_socks;

static netacct_t *acct_of(proc_t *p, int create) {
    if (!p) return NULL;
    for (u32 i = 0; i < ARRAY_SIZE(accts); i++) if (accts[i].used && accts[i].pid == p->pid) return &accts[i];
    if (!create) return NULL;
    for (u32 i = 0; i < ARRAY_SIZE(accts); i++) {
        if (accts[i].used) continue;
        memset(&accts[i], 0, sizeof accts[i]);
        accts[i].used = 1;
        accts[i].pid = p->pid;
        return &accts[i];
    }
    return NULL;
}

u32 net_acct_pid(const netacct_t *a) { return a ? a->pid : 0; }

int net_charge(netacct_t *a, u32 n) {
    if (net_mem + n > NET_MAX_MEM) return E_NOMEM;
    if (a && a->mem + n > NET_PROC_MEM) return E_NOMEM;
    if (mm_heap_free() < NET_HEAP_RESERVE + n) return E_NOMEM;
    net_mem += n;
    if (a) a->mem += n;
    return 0;
}

void net_uncharge(netacct_t *a, u32 n) {
    net_mem -= MIN(net_mem, (u64)n);
    if (a) a->mem -= MIN(a->mem, (u64)n);
}

/* ---------------- user / kernel buffers ---------------- */

int uio_in(uio_t *u, void *dst, u64 n) {
    if (n > uio_left(u)) return E_INVAL;
    if (u->kernel) memcpy(dst, (const void *)(uptr)(u->addr + u->done), n);
    else if (copyin(dst, u->addr + u->done, n)) return E_INVAL;
    u->done += n;
    return 0;
}

int uio_out(uio_t *u, const void *src, u64 n) {
    if (n > uio_left(u)) return E_INVAL;
    if (u->kernel) memcpy((void *)(uptr)(u->addr + u->done), src, n);
    else if (copyout(u->addr + u->done, src, n)) return E_INVAL;
    u->done += n;
    return 0;
}

int uio_check_out(const uio_t *u, u64 n) {
    if (n > uio_left(u)) return 0;
    return u->kernel || user_writable(u->addr + u->done, n);
}

/* ---------------- socket buffers ---------------- */
/* Streams are runs of NB_CHUNK-byte buffers, datagrams one buffer each
 * (the sender's address first). Memory is charged to the receiving
 * socket's program when it is allocated; a few free chunks are kept. */
#define NB_CHUNK 4000
#define NB_CACHE 32
static nbuf_t *nb_cache;
static int nb_ncache;

static netacct_t *q_acct(nqueue_t *q) { return q->owner ? q->owner->acct : NULL; }

static nbuf_t *nb_alloc(nqueue_t *q, u32 cap) {
    u32 sz = (u32)sizeof(nbuf_t) + cap;
    if (net_charge(q_acct(q), sz)) return NULL;
    nbuf_t *b;
    if (cap == NB_CHUNK && nb_cache) { b = nb_cache; nb_cache = b->next; nb_ncache--; }
    else b = kmalloc(sz);
    if (!b) { net_uncharge(q_acct(q), sz); return NULL; }
    b->next = NULL;
    b->cap = cap;
    b->len = b->off = 0;
    b->alen = b->msg = 0;
    q->mem += sz;
    return b;
}

static void nb_free(nqueue_t *q, nbuf_t *b) {
    u32 sz = (u32)sizeof(nbuf_t) + b->cap;
    net_uncharge(q_acct(q), sz);
    q->mem -= MIN(q->mem, sz);
    if (b->cap == NB_CHUNK && nb_ncache < NB_CACHE) { b->next = nb_cache; nb_cache = b; nb_ncache++; }
    else kfree(b);
}

static void nq_link(nqueue_t *q, nbuf_t *b) {
    if (q->tail) q->tail->next = b; else q->head = b;
    q->tail = b;
}

long nq_put_stream(nqueue_t *q, uio_t *u, u32 max) {
    u64 want = MIN(uio_left(u), (u64)max);
    long done = 0;
    while ((u64)done < want) {
        nbuf_t *t = q->tail;
        int fresh = 0;
        if (!t || t->msg || t->len == t->cap) {
            t = nb_alloc(q, NB_CHUNK);
            if (!t) return done ? done : E_NOMEM;
            fresh = 1;
        }
        u32 n = (u32)MIN(want - (u64)done, (u64)(t->cap - t->len));
        if (uio_in(u, t->data + t->len, n)) {       /* nothing of this piece is queued */
            if (fresh) nb_free(q, t);
            return done ? done : E_INVAL;
        }
        if (fresh) nq_link(q, t);
        t->len += n;
        q->bytes += n;
        done += n;
    }
    return done;
}

int nq_put_msg(nqueue_t *q, uio_t *u, const void *a, u32 alen) {
    u64 len = uio_left(u);
    if (len > 0x7FFFFFFF || alen > 0xFFFF) return E_MSGSIZE;
    nbuf_t *b = nb_alloc(q, alen + (u32)len);
    if (!b) return E_NOMEM;
    if (alen) memcpy(b->data, a, alen);
    if (uio_in(u, b->data + alen, len)) { nb_free(q, b); return E_INVAL; }
    b->alen = (u16)alen;
    b->off = alen;
    b->len = alen + (u32)len;
    b->msg = 1;
    nq_link(q, b);
    q->bytes += (u32)len;
    q->count++;
    return 0;
}

long nq_get_stream(nqueue_t *q, uio_t *u, int peek) {
    u64 want = MIN(uio_left(u), (u64)q->bytes);
    if (!uio_check_out(u, want)) return E_INVAL;     /* before taking anything */
    long done = 0;
    nbuf_t *b = q->head;
    u32 off = b ? b->off : 0;
    while ((u64)done < want && b) {
        u32 n = (u32)MIN(want - (u64)done, (u64)(b->len - off));
        if (uio_out(u, b->data + off, n)) break;
        done += n;
        off += n;
        if (!peek) { b->off = off; q->bytes -= n; }
        if (off == b->len) {
            nbuf_t *nx = b->next;
            if (!peek) {
                q->head = nx;
                if (!nx) q->tail = NULL;
                nb_free(q, b);
            }
            b = nx;
            off = b ? b->off : 0;
        }
    }
    return done;
}

long nq_get_msg(nqueue_t *q, uio_t *u, int peek, void *a, u32 *alen) {
    nbuf_t *b = q->head;
    if (!b) return E_AGAIN;
    u32 len = b->len - b->off;
    u64 n = MIN(uio_left(u), (u64)len);
    if (!uio_check_out(u, n)) return E_INVAL;
    if (uio_out(u, b->data + b->off, n)) return E_INVAL;
    if (alen) {
        if (a) memcpy(a, b->data, MIN(*alen, (u32)b->alen));
        *alen = b->alen;
    }
    if (!peek) {
        q->head = b->next;
        if (!q->head) q->tail = NULL;
        q->bytes -= len;
        q->count--;
        nb_free(q, b);
    }
    return len;
}

void nq_copy(const nqueue_t *q, u32 off, void *dst, u32 n) {
    u8 *d = dst;
    for (nbuf_t *b = q->head; b && n; b = b->next) {
        u32 have = b->len - b->off;
        if (off >= have) { off -= have; continue; }
        u32 k = MIN(n, have - off);
        memcpy(d, b->data + b->off + off, k);
        d += k; n -= k; off = 0;
    }
}

void nq_drop(nqueue_t *q, u32 n) {
    while (n && q->head) {
        nbuf_t *b = q->head;
        u32 k = MIN(n, b->len - b->off);
        b->off += k; q->bytes -= k; n -= k;
        if (b->off == b->len) {
            q->head = b->next;
            if (!q->head) q->tail = NULL;
            nb_free(q, b);
        }
    }
}

u32 nq_next_msg_len(const nqueue_t *q) { return q->head ? q->head->len - q->head->off : 0; }

void nq_free(nqueue_t *q) {
    while (q->head) {
        nbuf_t *b = q->head;
        q->head = b->next;
        nb_free(q, b);
    }
    q->tail = NULL;
    q->bytes = q->count = 0;
}

/* ---------------- socket objects ---------------- */

void sock_wake(sock_t *s) { if (s) s->gen++; }
void sock_get(sock_t *s) { s->refs++; }
sock_t *sock_first(void) { return all_socks; }

int sock_new(const net_family_t *f, int type, int protocol, netacct_t *a, sock_t **out) {
    *out = NULL;
    if (nsockets >= NET_MAX_SOCKETS || (a && a->sockets >= NET_PROC_SOCKETS)) return E_NOMEM;
    if (mm_heap_free() < NET_HEAP_RESERVE) return E_NOMEM;
    sock_t *s = kzalloc(sizeof *s);
    if (!s) return E_NOMEM;
    s->fam = f;
    s->family = f->family;
    s->type = type;
    s->protocol = protocol;
    s->refs = 1;
    s->rcvbuf = s->sndbuf = NET_RCVBUF_DEFAULT;
    s->acct = a;
    int r = f->create(s, type, protocol);
    if (r) { kfree(s); return r; }
    s->all_next = all_socks;
    all_socks = s;
    nsockets++;
    if (a) a->sockets++;
    *out = s;
    return 0;
}

void sock_put(sock_t *s) {
    if (--s->refs > 0) return;
    if (!s->closed) { s->refs = 1; sock_close(s); return; }   /* (not reached: the owner closes) */
    for (sock_t **pp = &all_socks; *pp; pp = &(*pp)->all_next)
        if (*pp == s) { *pp = s->all_next; break; }
    nsockets--;
    kfree(s);
}

/* the owner (a handle, a listener's backlog) lets go: tear down, tell
 * everyone who waits on it, then drop the owner's reference */
void sock_close(sock_t *s) {
    if (!s->closed) {
        s->closed = 1;
        s->ops->release(s);
        for (sock_t *t = all_socks; t; t = t->all_next)
            if (t->watch == s) { t->watch = NULL; sock_wake(t); }
        s->watch = NULL;
        if (s->acct) { s->acct->sockets--; s->acct = NULL; }
        sock_wake(s);
    }
    sock_put(s);
}

/* ---------------- waiting ---------------- */

typedef struct { sock_t *s, *w; u32 g, wg; } snap_t;

static int snap_cond(void *arg) {
    snap_t *n = arg;
    return n->s->gen != n->g || (n->w && n->w->gen != n->wg);
}

/* wait until s (or what it watches) changes: 0, E_AGAIN when the deadline
 * passed, E_CANCELED when the process is being ended */
static int sock_wait(sock_t *s, u64 deadline) {
    snap_t n = { s, s->watch, s->gen, 0 };
    if (n.w) { sock_get(n.w); n.wg = n.w->gen; }
    int r = proc_wait(snap_cond, &n, deadline);
    if (n.w) sock_put(n.w);
    if (r < 0) return r;
    if (!r) return E_AGAIN;               /* timed out */
    return 0;
}

static u64 deadline_after(u64 ms) { return ms ? time_ms_to_tsc(time_ms() + ms) : ~0ull; }

static long sock_send(sock_t *s, uio_t *u, int flags, const struct sockaddr *to, u32 tolen) {
    int nb = s->nonblock || (flags & MSG_DONTWAIT);
    u64 dl = deadline_after(s->sndtimeo_ms);
    long total = 0, r;
    for (;;) {
        if (s->closed) { r = E_CANCELED; break; }
        r = s->ops->send(s, u, flags, to, tolen);
        if (r > 0) {
            total += r;
            if (s->type != SOCK_STREAM || !uio_left(u)) break;
            continue;                     /* blocking streams send everything */
        }
        if (r != E_AGAIN || nb) break;
        r = sock_wait(s, dl);
        if (r) break;
    }
    return total ? total : r;
}

static long sock_recv(sock_t *s, uio_t *u, int flags, struct sockaddr *from, u32 *fromlen) {
    int nb = s->nonblock || (flags & MSG_DONTWAIT);
    int all = (flags & MSG_WAITALL) && !(flags & MSG_PEEK) && s->type == SOCK_STREAM;
    u64 dl = deadline_after(s->rcvtimeo_ms);
    u32 room = fromlen ? *fromlen : 0;
    long total = 0, r;
    for (;;) {
        if (s->closed) { r = E_CANCELED; break; }
        if (fromlen) *fromlen = room;     /* (an attempt that found nothing may have changed it) */
        r = s->ops->recv(s, u, flags, from, fromlen);
        if (r > 0) {
            total += r;
            if (!all || !uio_left(u)) break;
            continue;
        }
        if (r != E_AGAIN || nb) break;
        r = sock_wait(s, dl);
        if (r) break;
    }
    if (total && (r == E_CONNRESET || r == E_PIPE) && !s->error) s->error = (int)r;   /* report it next time */
    return total ? total : r;
}

/* ---------------- handles ---------------- */

static int handle_room(proc_t *p) {
    int n = 0;
    for (int i = 3; i < PROC_HANDLES; i++) if (p->handles[i].type == H_NONE) n++;
    return n;
}

/* the socket behind a handle, with a reference for the call */
static sock_t *fd_sock(proc_t *p, int fd, long *err) {
    sock_t *s = handle_get(p, fd, H_SOCKET);
    if (!s) {
        *err = (fd >= 0 && fd < PROC_HANDLES && p->handles[fd].type != H_NONE) ? E_NOTSOCK : E_INVAL;
        return NULL;
    }
    sock_get(s);
    return s;
}

int socket_handle_close(proc_t *p, int h) {
    sock_t *s = handle_get(p, h, H_SOCKET);
    if (!s) return E_INVAL;
    handle_free(p, h);
    sock_close(s);
    return 0;
}

long socket_handle_io(proc_t *p, int h, u64 buf, u64 len, int write) {
    long r;
    sock_t *s = fd_sock(p, h, &r);
    if (!s) return r;
    uio_t u = { buf, MIN(len, 0x7FFFF000ull), 0, 0 };
    r = write ? sock_send(s, &u, 0, NULL, 0) : sock_recv(s, &u, 0, NULL, NULL);
    sock_put(s);
    return r;
}

void socket_release_all(proc_t *p) {
    for (int i = 0; i < PROC_HANDLES; i++) {
        if (p->handles[i].type != H_SOCKET) continue;
        sock_t *s = p->handles[i].obj;
        handle_free(p, i);
        sock_close(s);
    }
    netacct_t *a = acct_of(p, 0);
    if (!a) return;
    if (a->sockets || a->mem) {
        kprintf("net: %u %s: %d sockets / %lu bytes still charged at exit\n", p->pid, p->name, a->sockets, a->mem);
        for (sock_t *t = all_socks; t; t = t->all_next) if (t->acct == a) t->acct = NULL;
    }
    a->used = 0;
}

/* ---------------- addresses from / to the program ---------------- */

static int addr_in(u64 ua, u64 len, struct sockaddr_storage *k) {
    if (len < sizeof(sa_family_t) || len > sizeof *k) return E_INVAL;
    memset(k, 0, sizeof *k);
    return copyin(k, ua, len) ? E_INVAL : 0;
}

/* check an (addr, socklen_t *len) result pair before anything is taken;
 * *room = bytes that may be written at addr */
static int addr_out_check(u64 ua, u64 ulenp, u32 *room) {
    *room = 0;
    if (!ulenp) return ua ? E_INVAL : 0;
    u32 l;
    if (copyin(&l, ulenp, sizeof l) || !user_writable(ulenp, sizeof l)) return E_INVAL;
    if (ua && l) {
        l = MIN(l, (u32)sizeof(struct sockaddr_storage));
        if (!user_writable(ua, l)) return E_INVAL;
        *room = l;
    }
    return 0;
}

static void addr_out(u64 ua, u64 ulenp, u32 room, const void *k, u32 klen) {
    if (!ulenp) return;
    if (ua && room) copyout(ua, k, MIN(room, klen));
    copyout(ulenp, &klen, sizeof klen);
}

/* ---------------- system calls ---------------- */

static const net_family_t *family_of(int af) {
    for (u32 i = 0; i < ARRAY_SIZE(families); i++) if (families[i]->family == af) return families[i];
    return NULL;
}

static long sys_socket(proc_t *p, int domain, int type, int protocol) {
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~flags;
    const net_family_t *f = family_of(domain);
    if (!f) return E_AFNOSUPPORT;
    netacct_t *a = acct_of(p, 1);
    if (!a || !handle_room(p)) return E_NOMEM;
    sock_t *s;
    int r = sock_new(f, type, protocol, a, &s);
    if (r) return r;
    s->nonblock = !!(flags & SOCK_NONBLOCK);
    int h = handle_alloc(p, H_SOCKET, s);
    if (h < 0) sock_close(s);
    return h;
}

static long sys_socketpair(proc_t *p, int domain, int type, int protocol, u64 usv) {
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~flags;
    if (!user_writable(usv, 2 * sizeof(int))) return E_INVAL;
    const net_family_t *f = family_of(domain);
    if (!f) return E_AFNOSUPPORT;
    if (!f->pair) return E_NOTSUP;
    netacct_t *a = acct_of(p, 1);
    if (!a || handle_room(p) < 2) return E_NOMEM;
    sock_t *x, *y = NULL;
    int r = sock_new(f, type, protocol, a, &x);
    if (!r) r = sock_new(f, type, protocol, a, &y);
    if (!r) r = f->pair(x, y);
    if (r) {
        if (x) sock_close(x);
        if (y) sock_close(y);
        return r;
    }
    x->nonblock = y->nonblock = !!(flags & SOCK_NONBLOCK);
    int sv[2] = { handle_alloc(p, H_SOCKET, x), handle_alloc(p, H_SOCKET, y) };
    copyout(usv, sv, sizeof sv);       /* checked above; handles are there */
    return 0;
}

static long sys_accept(proc_t *p, sock_t *s, u64 ua, u64 ulenp, int flags) {
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return E_INVAL;
    u32 room;
    int r = addr_out_check(ua, ulenp, &room);
    if (r) return r;
    if (!s->listening) return E_INVAL;
    int nb = s->nonblock;
    u64 dl = deadline_after(s->rcvtimeo_ms);
    sock_t *n = NULL;
    for (;;) {
        if (s->closed) return E_CANCELED;
        if (!handle_room(p)) return E_NOMEM;           /* leave the connection waiting */
        r = s->ops->accept(s, &n);
        if (!r) break;
        if (r != E_AGAIN || nb) return r;
        r = sock_wait(s, dl);
        if (r) return r;
    }
    n->nonblock = !!(flags & SOCK_NONBLOCK);
    int h = handle_alloc(p, H_SOCKET, n);
    if (h < 0) { sock_close(n); return h; }
    if (ulenp) {
        struct sockaddr_storage k;
        u32 kl = sizeof k;
        if (n->ops->getname(n, 1, (struct sockaddr *)&k, &kl)) { k.ss_family = (sa_family_t)n->family; kl = sizeof(sa_family_t); }
        addr_out(ua, ulenp, room, &k, kl);
    }
    return h;
}

static long sys_connect(sock_t *s, u64 ua, u64 len) {
    struct sockaddr_storage k;
    int r = addr_in(ua, len, &k);
    if (r) return r;
    int nb = s->nonblock;
    u64 dl = deadline_after(s->sndtimeo_ms);
    for (;;) {
        if (s->closed) return E_CANCELED;
        r = s->ops->connect(s, (struct sockaddr *)&k, (u32)len);
        if (r != E_AGAIN || nb) break;
        r = sock_wait(s, dl);
        if (r) break;
    }
    /* internet streams: wait for the handshake (POLLOUT) or an error */
    while (r == E_INPROGRESS && !nb) {
        if (s->closed) return E_CANCELED;
        int m = s->ops->poll(s);
        if (m & (POLLOUT | POLLERR | POLLHUP)) {
            r = s->error;
            s->error = 0;
            if (!r && !(m & POLLOUT)) r = E_CONNREFUSED;
            break;
        }
        int w = sock_wait(s, dl);
        if (w) r = w;
    }
    return r;
}

static long sys_sendto(sock_t *s, u64 buf, u64 len, int flags, u64 ua, u64 alen) {
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) return E_NOTSUP;
    struct sockaddr_storage k;
    if (ua) {
        int r = addr_in(ua, alen, &k);
        if (r) return r;
    }
    uio_t u = { buf, MIN(len, 0x7FFFF000ull), 0, 0 };
    return sock_send(s, &u, flags, ua ? (struct sockaddr *)&k : NULL, ua ? (u32)alen : 0);
}

static long sys_recvfrom(sock_t *s, u64 buf, u64 len, int flags, u64 ua, u64 ulenp) {
    if (flags & ~(MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL)) return E_NOTSUP;
    u32 room;
    int r = addr_out_check(ua, ulenp, &room);
    if (r) return r;
    struct sockaddr_storage k;
    memset(&k, 0, sizeof k);
    u32 kl = sizeof k;
    uio_t u = { buf, MIN(len, 0x7FFFF000ull), 0, 0 };
    long n = sock_recv(s, &u, flags, ulenp ? (struct sockaddr *)&k : NULL, ulenp ? &kl : NULL);
    if (n >= 0 && ulenp) addr_out(ua, ulenp, room, &k, kl);
    return n;
}

static long sys_getname(sock_t *s, int peer, u64 ua, u64 ulenp) {
    u32 room;
    if (!ulenp || !ua) return E_INVAL;
    int r = addr_out_check(ua, ulenp, &room);
    if (r) return r;
    struct sockaddr_storage k;
    memset(&k, 0, sizeof k);
    u32 kl = sizeof k;
    r = s->ops->getname(s, peer, (struct sockaddr *)&k, &kl);
    if (r) return r;
    addr_out(ua, ulenp, room, &k, kl);
    return 0;
}

static u64 opt_ms(const u8 *v, u32 len, int *err) {
    *err = 0;
    if (len >= sizeof(struct timeval)) {
        const struct timeval *tv = (const void *)v;
        if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) { *err = E_INVAL; return 0; }
        u64 ms = (u64)MIN(tv->tv_sec, 100000000L) * 1000 + (u64)tv->tv_usec / 1000;
        return ms ? ms : tv->tv_usec ? 1 : 0;
    }
    if (len >= sizeof(int)) {
        int ms = *(const int *)v;
        if (ms < 0) { *err = E_INVAL; return 0; }
        return (u64)ms;
    }
    *err = E_INVAL;
    return 0;
}

static long sys_setsockopt(sock_t *s, int level, int name, u64 uv, u64 len) {
    u64 kv[32];
    if (len > sizeof kv) return E_INVAL;
    memset(kv, 0, sizeof kv);
    if (len && copyin(kv, uv, len)) return E_INVAL;
    const u8 *v = (const u8 *)kv;
    int iv = len >= sizeof(int) ? *(const int *)v : 0;
    if (level == SOL_SOCKET) {
        int err;
        switch (name) {
        case SO_RCVBUF:
        case SO_SNDBUF: {
            if (len < sizeof(int)) return E_INVAL;
            u32 b = (u32)CLAMP((i64)iv, (i64)NET_BUF_MIN, (i64)NET_BUF_MAX);
            if (name == SO_RCVBUF) s->rcvbuf = b; else s->sndbuf = b;
            sock_wake(s);                 /* writers to it look again */
            return 0;
        }
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            u64 ms = opt_ms(v, (u32)len, &err);
            if (err) return err;
            if (name == SO_RCVTIMEO) s->rcvtimeo_ms = ms; else s->sndtimeo_ms = ms;
            return 0;
        }
        case SO_REUSEADDR:
            if (len < sizeof(int)) return E_INVAL;
            s->reuseaddr = !!iv;
            return 0;
        case SO_TYPE: case SO_ERROR: case SO_ACCEPTCONN: case SO_DOMAIN: case SO_PROTOCOL:
            return E_INVAL;               /* read only */
        }
    }
    return s->ops->setopt ? s->ops->setopt(s, level, name, v, (u32)len) : E_NOTSUP;
}

static long sys_getsockopt(sock_t *s, int level, int name, u64 uv, u64 ulenp) {
    u32 room;
    if (!ulenp || !uv) return E_INVAL;
    if (copyin(&room, ulenp, sizeof room) || !user_writable(ulenp, sizeof room)) return E_INVAL;
    room = MIN(room, 256u);
    if (!user_writable(uv, room)) return E_INVAL;
    u64 kv[32];
    memset(kv, 0, sizeof kv);
    u32 kl = sizeof(int);
    int *iv = (int *)kv;
    if (level == SOL_SOCKET && name != SO_RCVTIMEO && name != SO_SNDTIMEO) {
        switch (name) {
        case SO_TYPE: *iv = s->type; break;
        case SO_ERROR: *iv = s->error; s->error = 0; break;
        case SO_RCVBUF: *iv = (int)s->rcvbuf; break;
        case SO_SNDBUF: *iv = (int)s->sndbuf; break;
        case SO_REUSEADDR: *iv = s->reuseaddr; break;
        case SO_ACCEPTCONN: *iv = s->listening; break;
        case SO_DOMAIN: *iv = s->family; break;
        case SO_PROTOCOL: *iv = s->protocol; break;
        default: goto family;
        }
    } else if (level == SOL_SOCKET) {
        u64 ms = name == SO_RCVTIMEO ? s->rcvtimeo_ms : s->sndtimeo_ms;
        if (room >= sizeof(struct timeval)) {
            struct timeval *tv = (struct timeval *)kv;
            tv->tv_sec = (long)(ms / 1000);
            tv->tv_usec = (long)(ms % 1000) * 1000;
            kl = sizeof *tv;
        } else {
            *iv = (int)MIN(ms, 0x7FFFFFFFull);
        }
    } else {
    family:
        if (!s->ops->getopt) return E_NOTSUP;
        kl = room;
        int r = s->ops->getopt(s, level, name, kv, &kl);
        if (r) return r;
        kl = MIN(kl, (u32)sizeof kv);
    }
    copyout(uv, kv, MIN(room, kl));
    copyout(ulenp, &kl, sizeof kl);
    return 0;
}

static long sys_ioctlsocket(sock_t *s, long req, u64 uarg) {
    u64 v = 0;
    if (!uarg || copyin(&v, uarg, sizeof v) || !user_writable(uarg, sizeof v)) return E_INVAL;
    if (req == FIONBIO) { s->nonblock = v != 0; return 0; }
    if (!s->ops->ioctl) return E_NOTSUP;
    int r = s->ops->ioctl(s, req, &v);
    if (r) return r;
    copyout(uarg, &v, sizeof v);
    return 0;
}

/* ---------------- poll ---------------- */

typedef struct { sock_t *s, *w; u32 g, wg; } pent_t;
typedef struct {
    proc_t *p;
    int n, windows;
    int sound;                      /* a sound stream is polled: wake when streams play on */
    u32 sgen;
    pent_t e[NET_POLL_MAX];
} pwait_t;

static int poll_cond(void *arg) {
    pwait_t *w = arg;
    for (int i = 0; i < w->n; i++) {
        if (w->e[i].s->gen != w->e[i].g) return 1;
        if (w->e[i].w && w->e[i].w->gen != w->e[i].wg) return 1;
    }
    if (w->sound && sound_wait_gen() != w->sgen) return 1;
    u64 next;
    return w->windows && usergui_poll_ready && usergui_poll_ready(w->p, &next);
}

static void pw_drop(pwait_t *w) {
    for (int i = 0; i < w->n; i++) {
        sock_put(w->e[i].s);
        if (w->e[i].w) sock_put(w->e[i].w);
    }
    w->n = 0;
}

static short poll_one(proc_t *p, const struct pollfd *f, pwait_t *w, u64 *next_ms) {
    int fd = f->fd;
    int ev = (u16)f->events | POLLERR | POLLHUP | POLLNVAL;
    if (fd < 0) return 0;
    if (fd == FD_WINDOWS) {
        w->windows = 1;
        if (!usergui_poll_ready) return 0;
        u64 next = ~0ull;
        int ready = usergui_poll_ready(p, &next);
        if (next < *next_ms) *next_ms = next;
        return ready ? (short)(POLLIN & ev) : 0;
    }
    if (fd >= PROC_HANDLES) return POLLNVAL;
    if (fd <= 2 && p->handles[fd].type == H_NONE)   /* console: output never waits; input is not pollable */
        return fd == 0 ? POLLNVAL : (short)(POLLOUT & ev);
    switch (p->handles[fd].type) {
    case H_SOCKET: {
        sock_t *s = p->handles[fd].obj;
        if (s->closed) return POLLNVAL;
        int m = s->ops->poll(s);
        pent_t *e = &w->e[w->n++];
        e->s = s; e->g = s->gen; sock_get(s);
        e->w = s->watch; e->wg = 0;
        if (e->w) { sock_get(e->w); e->wg = e->w->gen; }
        return (short)(m & ev);
    }
    case H_SOUND:                                  /* sound streams (sound/sndsys.c): POLLOUT when there is room */
        if (!sound_handle_poll || !sound_wait_gen) return POLLNVAL;
        w->sound = 1;
        return (short)(sound_handle_poll(p, fd) & ev);
    case H_FILE:
        return (short)((POLLIN | POLLOUT) & ev);   /* files never wait */
    default:
        return POLLNVAL;
    }
}

static long sys_poll(proc_t *p, u64 ufds, u64 nfds, long timeout) {
    if (nfds > NET_POLL_MAX) return E_INVAL;
    struct pollfd f[NET_POLL_MAX];
    u64 size = nfds * sizeof(struct pollfd);
    if (nfds && (copyin(f, ufds, size) || !user_writable(ufds, size))) return E_INVAL;
    pwait_t *w = kmalloc(sizeof *w);
    if (!w) return E_NOMEM;
    w->p = p;
    w->n = 0;
    u64 dl = timeout < 0 ? ~0ull : time_ms_to_tsc(time_ms() + (u64)MIN(timeout, 86400000L * 365));
    long r;
    for (;;) {
        int ready = 0;
        u64 next_ms = ~0ull;
        w->windows = 0;
        w->sound = 0;
        for (u64 i = 0; i < nfds; i++) {
            f[i].revents = poll_one(p, &f[i], w, &next_ms);
            if (f[i].revents) ready++;
        }
        if (ready || timeout == 0 || rdtsc() >= dl) { r = ready; break; }
        u64 d = dl;
        if (next_ms != ~0ull && time_ms_to_tsc(next_ms) < d) d = time_ms_to_tsc(next_ms);   /* a window timer */
        if (w->sound) w->sgen = sound_wait_gen();
        r = proc_wait(poll_cond, w, d);
        pw_drop(w);
        if (r < 0) break;
    }
    pw_drop(w);
    kfree(w);
    if (r >= 0 && nfds) copyout(ufds, f, size);   /* checked writable above */
    return r;
}

/* ---------------- statistics ---------------- */

static long sys_sockstat(proc_t *p, u64 ust) {
    hal_sockstat_t st;
    memset(&st, 0, sizeof st);
    st.sockets = nsockets;
    st.max_sockets = NET_MAX_SOCKETS;
    netacct_t *a = acct_of(p, 0);
    st.proc_sockets = a ? a->sockets : 0;
    st.max_proc_sockets = NET_PROC_SOCKETS;
    st.mem = net_mem;
    st.max_mem = NET_MAX_MEM;
    st.proc_mem = a ? a->mem : 0;
    st.max_proc_mem = NET_PROC_MEM;
    st.names = local_name_count();
    st.max_backlog = NET_MAX_BACKLOG;
    st.rcvbuf_default = NET_RCVBUF_DEFAULT;
    st.rcvbuf_max = NET_BUF_MAX;
    return copyout(ust, &st, sizeof st) ? E_INVAL : 0;
}

/* ---------------- dispatcher (core/syscall.c) ---------------- */

long socket_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    proc_t *p = proc_current();
    if (!p) return E_INVAL;
    switch (nr) {
    case SYS_SOCKET: return sys_socket(p, (int)a1, (int)a2, (int)a3);
    case SYS_SOCKETPAIR: return sys_socketpair(p, (int)a1, (int)a2, (int)a3, a4);
    case SYS_POLL: return sys_poll(p, a1, a2, (long)(int)a3);
    case SYS_SOCKSTAT: return sys_sockstat(p, a1);
    }
    if (nr >= SYS_RESOLVE && nr <= SYS_RESOLVE + 11) return inet_syscall ? inet_syscall(nr, a1, a2, a3, a4, a5, a6) : E_NOTSUP;
    if (nr < SYS_SOCKET || nr > SYS_IOCTLSOCKET) return E_NOTSUP;
    long r;
    sock_t *s = fd_sock(p, (int)a1, &r);
    if (!s) return r;
    switch (nr) {
    case SYS_BIND: {
        struct sockaddr_storage k;
        r = addr_in(a2, a3, &k);
        if (!r) r = s->listening ? E_INVAL : s->ops->bind(s, (struct sockaddr *)&k, (u32)a3);
        break;
    }
    case SYS_LISTEN:
        r = s->ops->listen(s, (int)CLAMP((i64)(int)a2, 1, NET_MAX_BACKLOG));
        if (!r) s->listening = 1;
        break;
    case SYS_ACCEPT: r = sys_accept(p, s, a2, a3, (int)a4); break;
    case SYS_CONNECT: r = s->listening ? E_INVAL : sys_connect(s, a2, a3); break;
    case SYS_SENDTO: r = sys_sendto(s, a2, a3, (int)a4, a5, a6); break;
    case SYS_RECVFROM: r = sys_recvfrom(s, a2, a3, (int)a4, a5, a6); break;
    case SYS_SHUTDOWN: r = a2 > SHUT_RDWR ? E_INVAL : s->ops->shutdown(s, (int)a2); break;
    case SYS_GETSOCKNAME: r = sys_getname(s, 0, a2, a3); break;
    case SYS_GETPEERNAME: r = sys_getname(s, 1, a2, a3); break;
    case SYS_SETSOCKOPT: r = sys_setsockopt(s, (int)a2, (int)a3, a4, a5); break;
    case SYS_GETSOCKOPT: r = sys_getsockopt(s, (int)a2, (int)a3, a4, a5); break;
    case SYS_IOCTLSOCKET: r = sys_ioctlsocket(s, (long)a2, a3); break;
    default: r = E_NOTSUP; break;
    }
    sock_put(s);
    return r;
}
