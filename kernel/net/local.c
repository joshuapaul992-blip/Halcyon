/* Halcyon OS - AF_LOCAL sockets: programs on this computer talking by name
 * (see docs/DESIGN-M9.md, "Sockets and poll").
 *
 * Names live in one flat namespace in memory (no files): 1-63 characters
 * from [A-Za-z0-9._-], starting with a letter or digit, compared without
 * regard to case. A name belongs to the socket bound to it until that socket
 * is closed (or its program ends). bind() with an empty name, and the first
 * send of an unnamed datagram socket, take an automatic name "~xxxxxx" that
 * programs cannot choose. Stream and datagram sockets share the namespace.
 *
 * Streams: connect() puts a new socket in the listener's backlog at once
 * (the client may send before it is accepted; a full backlog makes connect
 * wait, or E_AGAIN). Data goes straight into the peer's receive buffer,
 * whose size (the peer's SO_RCVBUF) is the back-pressure: a sender waits
 * until the reader makes room. When a socket closes, its peer reads the
 * remaining data and then end of stream (0); E_CONNRESET first if the
 * closing side had unread data or was never accepted. Sending to a closed
 * peer is E_PIPE.
 *
 * Datagrams: each message is queued whole at the receiver (at most
 * LOCAL_DGRAM_QLEN messages and SO_RCVBUF bytes; the sender waits, or
 * E_AGAIN), with the sender's name. connect() sets the default destination
 * and, as on BSD, a socket connected to a peer takes messages from that peer
 * only. When a connected peer closes, recv() returns 0 once the queue is
 * empty and send() fails with E_CONNREFUSED. */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "net/socket.h"

#define LOCAL_DGRAM_MAX  65536      /* largest datagram */
#define LOCAL_DGRAM_QLEN 128        /* datagrams queued at one socket */

typedef struct lsock {
    sock_t *s;
    char name[LOCAL_NAME_MAX + 1];      /* own name: bound, automatic, or the listener's (accepted) */
    char peername[LOCAL_NAME_MAX + 1];
    u8 named, registered, has_peername;
    u8 connected;           /* streams: connected (also once the peer is gone); datagrams: has a default peer */
    u8 peer_gone;           /* the (connected) peer closed */
    u8 eof;                 /* streams: nothing more will arrive */
    u8 epipe;               /* streams: the peer takes no more data */
    u8 rd_shut, wr_shut;
    u8 pending;             /* in a listener's backlog, not accepted yet */
    sock_t *peer;           /* streams: the other end while it exists; datagrams: default destination */
    nqueue_t rx;
    /* listening */
    int backlog_max, backlog_n;
    sock_t *bl_head, *bl_tail;
    sock_t *listener;       /* pending: whose backlog */
    sock_t *bl_next;
    struct lsock *reg_next; /* registered names */
} lsock_t;

#define L(s) ((lsock_t *)(s)->priv)

static lsock_t *registry;
static int nnames;
static u32 autoseq;

int local_name_count(void) { return nnames; }

/* ---------------- names ---------------- */

static int name_ok(const char *n) {
    size_t l = strlen(n);
    if (!l || l > LOCAL_NAME_MAX || !isalnum((u8)n[0])) return 0;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!isalnum((u8)c) && c != '.' && c != '-' && c != '_') return 0;
    }
    return 1;
}

static lsock_t *name_find(const char *n) {
    for (lsock_t *l = registry; l; l = l->reg_next) if (!strcasecmp(l->name, n)) return l;
    return NULL;
}

static void name_register(lsock_t *l, const char *n) {
    strlcpy(l->name, n, sizeof l->name);
    l->named = l->registered = 1;
    l->reg_next = registry;
    registry = l;
    nnames++;
}

static void name_unregister(lsock_t *l) {
    for (lsock_t **pp = &registry; *pp; pp = &(*pp)->reg_next)
        if (*pp == l) { *pp = l->reg_next; nnames--; break; }
    l->registered = 0;
}

static int autobind(lsock_t *l) {
    char n[16];
    for (int tries = 0; tries < 4 * NET_MAX_SOCKETS; tries++) {
        snprintf(n, sizeof n, "~%06x", (autoseq++) & 0xFFFFFF);
        if (!name_find(n)) { name_register(l, n); return 0; }
    }
    return E_ADDRINUSE;
}

/* the name in an AF_LOCAL address (may be empty) */
static int addr_name(const struct sockaddr *a, u32 len, char *out) {
    if (len < sizeof(sa_family_t)) return E_INVAL;
    if (a->sa_family != AF_LOCAL) return E_AFNOSUPPORT;
    const struct sockaddr_local *la = (const struct sockaddr_local *)a;
    u32 room = MIN(len - (u32)sizeof(sa_family_t), (u32)sizeof la->sl_name);
    size_t l = strnlen(la->sl_name, room);
    if (l > LOCAL_NAME_MAX) return E_NAMETOOLONG;
    memcpy(out, la->sl_name, l);
    out[l] = 0;
    return 0;
}

static u32 make_addr(struct sockaddr *a, u32 room, const char *name) {
    struct sockaddr_local k;
    memset(&k, 0, sizeof k);
    k.sl_family = AF_LOCAL;
    u32 kl = sizeof(sa_family_t);
    if (name && name[0]) {
        strlcpy(k.sl_name, name, sizeof k.sl_name);
        kl += (u32)strlen(name) + 1;
    }
    if (a) memcpy(a, &k, MIN(room, kl));
    return kl;
}

/* ---------------- common ---------------- */

static int l_create(sock_t *s, int type, int protocol);

static void l_release(sock_t *s) {
    lsock_t *l = L(s);
    if (l->registered) name_unregister(l);
    /* connections nobody accepted: their clients are reset */
    while (l->bl_head) {
        sock_t *n = l->bl_head;
        l->bl_head = L(n)->bl_next;
        L(n)->listener = NULL;
        sock_close(n);
    }
    l->bl_tail = NULL;
    l->backlog_n = 0;
    if (l->listener) {                  /* (pending and closed on its own) */
        lsock_t *ll = L(l->listener);
        for (sock_t **pp = &ll->bl_head; *pp; pp = &L(*pp)->bl_next)
            if (*pp == s) { *pp = l->bl_next; break; }
        ll->bl_tail = NULL;
        for (sock_t *t = ll->bl_head; t; t = L(t)->bl_next) ll->bl_tail = t;
        ll->backlog_n--;
        sock_wake(l->listener);
        l->listener = NULL;
    }
    if (s->type == SOCK_STREAM && l->peer) {
        sock_t *ps = l->peer;
        lsock_t *pl = L(ps);
        pl->peer = NULL;
        pl->peer_gone = pl->eof = pl->epipe = 1;
        if (l->rx.bytes || l->pending) ps->error = E_CONNRESET;   /* data was lost */
        sock_wake(ps);
    } else if (s->type == SOCK_DGRAM) {
        for (sock_t *t = sock_first(); t; t = t->all_next) {
            if (t->closed || t->fam != &local_family || t->type != SOCK_DGRAM || L(t)->peer != s) continue;
            L(t)->peer = NULL;
            L(t)->peer_gone = 1;
            sock_wake(t);
        }
    }
    l->peer = NULL;
    nq_free(&l->rx);
    kfree(l);
    s->priv = NULL;
}

static int l_bind(sock_t *s, const struct sockaddr *a, u32 len) {
    lsock_t *l = L(s);
    char name[LOCAL_NAME_MAX + 1];
    int r = addr_name(a, len, name);
    if (r) return r;
    if (l->named) return E_INVAL;       /* bound already (or accepted) */
    if (!name[0]) return autobind(l);
    if (!name_ok(name)) return E_INVAL;
    if (name_find(name)) return E_ADDRINUSE;
    name_register(l, name);
    return 0;
}

static int l_getname(sock_t *s, int peer, struct sockaddr *a, u32 *len) {
    lsock_t *l = L(s);
    if (peer && !l->connected) return E_NOTCONN;
    const char *n = peer ? (l->has_peername ? l->peername : NULL) : (l->named ? l->name : NULL);
    *len = make_addr(a, *len, n);
    return 0;
}

static int l_shutdown(sock_t *s, int how) {
    lsock_t *l = L(s);
    if (s->type == SOCK_STREAM && !l->connected) return E_NOTCONN;
    if (how != SHUT_WR) {
        l->rd_shut = 1;
        nq_free(&l->rx);                /* unread data is discarded */
        if (s->type == SOCK_STREAM && l->peer) L(l->peer)->epipe = 1;
    }
    if (how != SHUT_RD) {
        l->wr_shut = 1;
        if (s->type == SOCK_STREAM && l->peer) L(l->peer)->eof = 1;
    }
    sock_wake(s);
    sock_wake(l->peer);
    return 0;
}

static int l_ioctl(sock_t *s, long req, u64 *arg) {
    lsock_t *l = L(s);
    if (req != FIONREAD) return E_NOTSUP;
    *arg = s->type == SOCK_STREAM ? l->rx.bytes : nq_next_msg_len(&l->rx);
    return 0;
}

/* ---------------- streams ---------------- */

static int st_listen(sock_t *s, int backlog) {
    lsock_t *l = L(s);
    if (!l->registered || l->connected) return E_INVAL;
    l->backlog_max = backlog;
    return 0;
}

static int st_accept(sock_t *s, sock_t **out) {
    lsock_t *l = L(s);
    sock_t *n = l->bl_head;
    if (!n) return E_AGAIN;
    l->bl_head = L(n)->bl_next;
    if (!l->bl_head) l->bl_tail = NULL;
    l->backlog_n--;
    L(n)->bl_next = NULL;
    L(n)->listener = NULL;
    L(n)->pending = 0;
    sock_wake(s);                       /* room in the backlog */
    *out = n;
    return 0;
}

static void st_link(sock_t *a, sock_t *b) {
    L(a)->peer = b; L(b)->peer = a;
    L(a)->connected = L(b)->connected = 1;
    a->watch = b; b->watch = a;
}

static int st_connect(sock_t *s, const struct sockaddr *a, u32 len) {
    lsock_t *l = L(s);
    char name[LOCAL_NAME_MAX + 1];
    int r = addr_name(a, len, name);
    if (r) return r;
    if (l->connected) return E_ISCONN;
    if (!name[0]) return E_INVAL;
    lsock_t *t = name_find(name);
    if (!t || t->s->type != SOCK_STREAM || !t->s->listening) return E_CONNREFUSED;
    if (t->backlog_n >= t->backlog_max) { s->watch = t->s; return E_AGAIN; }
    sock_t *n;
    if (sock_new(&local_family, SOCK_STREAM, 0, t->s->acct, &n)) return E_CONNREFUSED;   /* the server is at a limit */
    lsock_t *nl = L(n);
    n->rcvbuf = t->s->rcvbuf;
    n->sndbuf = t->s->sndbuf;
    n->rcvtimeo_ms = t->s->rcvtimeo_ms;
    n->sndtimeo_ms = t->s->sndtimeo_ms;
    strlcpy(nl->name, t->name, sizeof nl->name);
    nl->named = 1;
    if (l->named) { strlcpy(nl->peername, l->name, sizeof nl->peername); nl->has_peername = 1; }
    strlcpy(l->peername, t->name, sizeof l->peername);
    l->has_peername = 1;
    st_link(s, n);
    nl->pending = 1;
    nl->listener = t->s;
    if (t->bl_tail) L(t->bl_tail)->bl_next = n; else t->bl_head = n;
    t->bl_tail = n;
    t->backlog_n++;
    sock_wake(t->s);
    sock_wake(s);
    return 0;
}

static long st_send(sock_t *s, uio_t *u, int flags, const struct sockaddr *to, u32 tolen) {
    lsock_t *l = L(s);
    if (l->wr_shut || l->epipe) return E_PIPE;
    if (!l->connected) return E_NOTCONN;
    if (!l->peer) return E_PIPE;
    if (!uio_left(u)) return 0;
    sock_t *ps = l->peer;
    lsock_t *pl = L(ps);
    u32 space = ps->rcvbuf > pl->rx.bytes ? ps->rcvbuf - pl->rx.bytes : 0;
    if (!space) { s->watch = ps; return E_AGAIN; }
    long r = nq_put_stream(&pl->rx, u, space);
    if (r > 0) sock_wake(ps);
    return r;
}

static long st_recv(sock_t *s, uio_t *u, int flags, struct sockaddr *from, u32 *fromlen) {
    lsock_t *l = L(s);
    if (fromlen) *fromlen = 0;
    if (s->listening) return E_INVAL;
    if (l->rx.bytes) {
        if (!uio_left(u)) return 0;
        long r = nq_get_stream(&l->rx, u, flags & MSG_PEEK);
        if (r > 0 && !(flags & MSG_PEEK)) { sock_wake(s); sock_wake(l->peer); }   /* room for the writer */
        return r;
    }
    if (s->error) { int e = s->error; s->error = 0; return e; }
    if (l->eof || l->rd_shut) return 0;
    if (!l->connected) return E_NOTCONN;
    return E_AGAIN;
}

static int st_poll(sock_t *s) {
    lsock_t *l = L(s);
    if (s->listening) return l->bl_head ? POLLIN : 0;
    int m = s->error ? POLLERR : 0;
    if (l->rx.bytes || l->eof || l->rd_shut) m |= POLLIN;
    if (l->eof || l->rd_shut) m |= POLLRDHUP;
    if (!l->connected) return m | POLLHUP;
    if (l->peer_gone || (l->eof && l->wr_shut)) m |= POLLHUP;
    if (l->wr_shut || l->epipe || !l->peer) m |= POLLOUT;        /* a send fails at once */
    else {
        s->watch = l->peer;
        if (l->peer->rcvbuf > L(l->peer)->rx.bytes) m |= POLLOUT;
    }
    return m;
}

/* ---------------- datagrams ---------------- */

static int dg_listen(sock_t *s, int backlog) { return E_NOTSUP; }
static int dg_accept(sock_t *s, sock_t **out) { return E_NOTSUP; }

/* may `from` send to t? (a connected socket takes messages from its peer only) */
static int dg_may_send(sock_t *from, sock_t *t) {
    lsock_t *tl = L(t);
    if (tl->peer_gone || (tl->peer && tl->peer != from)) return E_PERM;
    if (tl->rd_shut) return E_PIPE;
    return 0;
}

static int dg_connect(sock_t *s, const struct sockaddr *a, u32 len) {
    lsock_t *l = L(s);
    if (len >= sizeof(sa_family_t) && a->sa_family == AF_UNSPEC) {     /* dissolve the association */
        l->peer = NULL;
        l->connected = l->peer_gone = l->has_peername = 0;
        s->watch = NULL;
        sock_wake(s);
        return 0;
    }
    char name[LOCAL_NAME_MAX + 1];
    int r = addr_name(a, len, name);
    if (r) return r;
    if (!name[0]) return E_INVAL;
    lsock_t *t = name_find(name);
    if (!t || t->s->type != SOCK_DGRAM) return E_CONNREFUSED;
    r = dg_may_send(s, t->s);
    if (r == E_PIPE) r = 0;
    if (r) return r;
    l->peer = t->s;
    l->connected = 1;
    l->peer_gone = 0;
    strlcpy(l->peername, t->name, sizeof l->peername);
    l->has_peername = 1;
    s->watch = t->s;
    sock_wake(s);
    return 0;
}

static long dg_send(sock_t *s, uio_t *u, int flags, const struct sockaddr *to, u32 tolen) {
    lsock_t *l = L(s);
    if (l->wr_shut) return E_PIPE;
    sock_t *ts;
    if (to) {
        char name[LOCAL_NAME_MAX + 1];
        int r = addr_name(to, tolen, name);
        if (r) return r;
        lsock_t *t = name[0] ? name_find(name) : NULL;
        if (!t || t->s->type != SOCK_DGRAM) return E_CONNREFUSED;
        ts = t->s;
    } else {
        if (!l->peer) return l->peer_gone ? E_CONNREFUSED : E_NOTCONN;
        ts = l->peer;
    }
    int r = dg_may_send(s, ts);
    if (r) return r;
    lsock_t *tl = L(ts);
    u64 len = uio_left(u);
    if (len > LOCAL_DGRAM_MAX || len > ts->rcvbuf) return E_MSGSIZE;
    if (tl->rx.bytes + len > ts->rcvbuf || tl->rx.count >= LOCAL_DGRAM_QLEN) { s->watch = ts; return E_AGAIN; }
    if (!l->named) autobind(l);         /* so the receiver can answer */
    struct sockaddr_local from;
    u32 alen = make_addr((struct sockaddr *)&from, sizeof from, l->named ? l->name : NULL);
    r = nq_put_msg(&tl->rx, u, &from, alen);
    if (r) return r;
    sock_wake(ts);
    return (long)len;
}

static long dg_recv(sock_t *s, uio_t *u, int flags, struct sockaddr *from, u32 *fromlen) {
    lsock_t *l = L(s);
    if (l->rx.count) {
        u64 before = u->done;
        long r = nq_get_msg(&l->rx, u, flags & MSG_PEEK, from, fromlen);
        if (r < 0) return r;
        if (!(flags & MSG_PEEK)) sock_wake(s);      /* room for senders */
        return (flags & MSG_TRUNC) ? r : (long)(u->done - before);
    }
    if (fromlen) *fromlen = 0;
    if (s->error) { int e = s->error; s->error = 0; return e; }
    if (l->rd_shut || l->peer_gone) return 0;
    return E_AGAIN;
}

static int dg_poll(sock_t *s) {
    lsock_t *l = L(s);
    int m = s->error ? POLLERR : 0;
    if (l->rx.count || l->rd_shut || l->peer_gone) m |= POLLIN;
    if (l->rd_shut || l->peer_gone) m |= POLLRDHUP;
    if (l->peer_gone) m |= POLLHUP;
    if (l->wr_shut || !l->peer) m |= POLLOUT;
    else {
        lsock_t *tl = L(l->peer);
        s->watch = l->peer;
        if (tl->rx.bytes < l->peer->rcvbuf && tl->rx.count < LOCAL_DGRAM_QLEN) m |= POLLOUT;
    }
    return m;
}

/* ---------------- family ---------------- */

static const sock_ops_t stream_ops = {
    .release = l_release, .bind = l_bind, .listen = st_listen, .accept = st_accept, .connect = st_connect,
    .send = st_send, .recv = st_recv, .shutdown = l_shutdown, .getname = l_getname, .poll = st_poll,
    .ioctl = l_ioctl,
};
static const sock_ops_t dgram_ops = {
    .release = l_release, .bind = l_bind, .listen = dg_listen, .accept = dg_accept, .connect = dg_connect,
    .send = dg_send, .recv = dg_recv, .shutdown = l_shutdown, .getname = l_getname, .poll = dg_poll,
    .ioctl = l_ioctl,
};

static int l_create(sock_t *s, int type, int protocol) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return E_NOTSUP;
    if (protocol != 0) return E_NOTSUP;
    lsock_t *l = kzalloc(sizeof *l);
    if (!l) return E_NOMEM;
    l->s = s;
    l->rx.owner = s;
    s->priv = l;
    s->ops = type == SOCK_STREAM ? &stream_ops : &dgram_ops;
    return 0;
}

static int l_pair(sock_t *a, sock_t *b) {
    if (a->type == SOCK_STREAM) {
        st_link(a, b);
    } else {
        L(a)->peer = b; L(b)->peer = a;
        L(a)->connected = L(b)->connected = 1;
        a->watch = b; b->watch = a;
    }
    return 0;
}

const net_family_t local_family = { AF_LOCAL, "local", l_create, l_pair };
