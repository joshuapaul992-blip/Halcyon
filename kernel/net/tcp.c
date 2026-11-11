/* Halcyon OS - TCP (RFC 9293), as the SOCK_STREAM half of AF_INET.
 *
 *   RFC 6298  retransmission timer (Karn's rule: no samples from retransmitted
 *             segments); minimum RTO 200 ms as Linux, not RFC 6298's 1 s
 *   RFC 5681  slow start, congestion avoidance (byte counting), fast
 *             retransmit; RFC 6582 NewReno recovery; RFC 6928 initial window
 *   RFC 7323  window scaling (no timestamps: RTT is timed one segment at a
 *             time, PAWS is not done)
 *   RFC 1122  delayed ACKs (every second full segment, else 40 ms), Nagle,
 *             sender / receiver silly-window avoidance, zero-window probes,
 *             keepalive
 *   RFC 5961  RST and SYN in the window: challenge ACKs; ACK range check
 *   RFC 6528  initial sequence numbers: clock + keyed hash of the addresses
 *   RFC 1337  RST in TIME_WAIT ignored; RFC 5927 ICMP errors checked against
 *             the window
 *
 * A connection is a tcb. A socket's tcb lives in s->priv; when the program
 * closes the socket, the tcb lives on by itself (an "orphan": FIN_WAIT,
 * LAST_ACK, TIME_WAIT) and its send queue is charged to the system instead.
 * A listener keeps half-open connections (SYN_RCVD, no socket yet) in its
 * SYN queue; when the handshake completes they become sockets (charged to
 * the listener's program) in the accept queue. */
#include "net/inet.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20

#define TCP_HDR             20
#define TCP_MSL_MS          30000
#define TCP_TIMEWAIT_MS     (2 * TCP_MSL_MS)
#define TCP_FIN_TIMEOUT_MS  60000       /* orphans in FIN_WAIT_2 */
#define TCP_RTO_INIT        1000
#define TCP_RTO_MIN         200
#define TCP_RTO_MAX         60000
#define TCP_SYN_RETRIES     5           /* 1 + 2 + 4 + 8 + 16 s, then E_TIMEOUT after 63 s */
#define TCP_SYNACK_RETRIES  5
#define TCP_RETRIES         12          /* data retransmissions before the connection is dropped */
#define TCP_PROBES          10          /* unanswered zero-window probes */
#define TCP_DELACK_MS       40
#define TCP_IW_SEGS         10
#define TCP_WSCALE          3           /* 65535 << 3 covers NET_BUF_MAX */
#define TCP_MIN_MSS         64
#define TCP_DEFAULT_MSS     536
#define TCP_SYNQ_MAX        64          /* per listener */
#define TCP_EMBRYONIC_MAX   256         /* half-open connections, all listeners */
#define TCP_ORPHAN_MAX      256         /* closed by their program, still finishing */
#define TCP_TW_MAX          1024        /* in TIME_WAIT */
#define TCP_OOO_SEGS        64          /* out-of-order segments kept per connection */
#define TCP_HASH            256
#define TCP_CHALLENGE_PER_S 5
#define KEEPIDLE_DEF        7200
#define KEEPINTVL_DEF       75
#define KEEPCNT_DEF         9

typedef struct ooo { struct ooo *next; u32 seq, len, mem; u8 data[]; } ooo_t;

typedef struct tcb tcb_t;
struct tcb {
    tcb_t *next, *hnext;
    sock_t *s;                      /* NULL: half-open, or orphaned */
    tcb_t *listener;                /* in this listener's SYN queue (inq 1) or accept queue (inq 2) */
    tcb_t *qnext;
    u8 state, inq, hashed, bound;
    u8 orphan, unaccepted;
    u32 laddr, raddr;
    u16 lport, rport;
    /* options */
    u8 nodelay, keepalive, linger0;
    u8 rd_shut, wr_shut, fin_rcvd, fin_sent;
    u8 ws_ok, snd_ws, rcv_ws, peer_ws;
    int ttl, tos;
    u32 keepidle, keepintvl, keepcnt, keep_sent;
    u64 keep_last;
    u16 mss, peer_mss, adv_mss;
    /* send side */
    u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, max_sndwnd;
    u32 sq_seq;                     /* sequence number of the send queue's first byte */
    nqueue_t sq;
    /* receive side */
    u32 irs, rcv_nxt, rcv_adv;      /* rcv_adv: right edge of the window we offered */
    u32 rcvbuf_hint;                /* half-open: the listener's SO_RCVBUF */
    nqueue_t rq;
    ooo_t *ooo;
    u32 ooo_bytes, ooo_n;
    u8 ooo_fin;
    u32 ooo_fin_seq;
    /* timing */
    u32 srtt, rttvar, rto;          /* srtt x8, rttvar x4, ms */
    u32 rtt_seq;
    u64 rtt_start;
    u8 rtt_on;
    u8 nrexmt, dupacks, in_rec, persist_n, ack_now, delack_segs, chal_n, force;
    u32 recover, cwnd, ssthresh, cwnd_acc;
    u64 t_rexmt, t_persist, t_delack, t_keep, t_close;
    u64 last_rx, last_tx, chal_t, oow_t;
    int soft_err;                   /* last ICMP error (reported if the connection times out) */
    int err;                        /* why it ended (E_CONNRESET, E_TIMEOUT...) */
    u8 err_told;                    /* err was reported (by recv or send): later calls give 0 / E_PIPE */
    /* listening */
    int backlog, synq_n, acq_n;
    tcb_t *synq, *acq, *acq_tail;
};

static tcb_t *all;
static tcb_t *hash[TCP_HASH];
static int ntcb, nembryonic, norphan, ntw;
static u64 tcp_next = ~0ull;
static tcb_t *adopting;            /* tcp_create() takes this tcb instead of a new one */
static u8 segbuf[HDRROOM + 60 + LO_MTU];

static void tcp_output(tcb_t *t);
static void tcp_free(tcb_t *t);
static int tcp_close_conn(tcb_t *t, int err);

#define T(s) ((tcb_t *)(s)->priv)

static void tset(u64 *f, u64 when) {
    *f = when;
    if (when && when < tcp_next) tcp_next = when;
}

static u32 hkey(u32 la, u16 lp, u32 ra, u16 rp) { return (la ^ ra ^ ((u32)lp << 16 | rp) ^ (ra >> 13)) * 2654435761u >> 24; }

static void hash_add(tcb_t *t) {
    if (t->hashed) return;
    u32 k = hkey(t->laddr, t->lport, t->raddr, t->rport) % TCP_HASH;
    t->hnext = hash[k];
    hash[k] = t;
    t->hashed = 1;
}

static void hash_del(tcb_t *t) {
    if (!t->hashed) return;
    u32 k = hkey(t->laddr, t->lport, t->raddr, t->rport) % TCP_HASH;
    for (tcb_t **pp = &hash[k]; *pp; pp = &(*pp)->hnext) if (*pp == t) { *pp = t->hnext; break; }
    t->hashed = 0;
}

static tcb_t *lookup(u32 la, u16 lp, u32 ra, u16 rp) {
    for (tcb_t *t = hash[hkey(la, lp, ra, rp) % TCP_HASH]; t; t = t->hnext)
        if (t->lport == lp && t->rport == rp && t->laddr == la && t->raddr == ra) return t;
    return NULL;
}

static tcb_t *lookup_listener(u32 la, u16 lp) {
    tcb_t *any = NULL;
    for (tcb_t *t = all; t; t = t->next) {
        if (t->state != TCPS_LISTEN || t->lport != lp) continue;
        if (t->laddr == la) return t;
        if (!t->laddr) any = t;
    }
    return any;
}

static void set_state(tcb_t *t, int st) {
    if (t->state == TCPS_TIME_WAIT) ntw--;
    if (st == TCPS_TIME_WAIT) ntw++;
    t->state = (u8)st;
}

static tcb_t *tcb_alloc(void) {
    tcb_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->rto = TCP_RTO_INIT;
    t->mss = TCP_DEFAULT_MSS;
    t->keepidle = KEEPIDLE_DEF; t->keepintvl = KEEPINTVL_DEF; t->keepcnt = KEEPCNT_DEF;
    t->ssthresh = 0x7FFFFFFF;
    t->next = all;
    all = t;
    ntcb++;
    return t;
}

static u32 rcvbuf_of(tcb_t *t) { return t->s ? t->s->rcvbuf : t->rcvbuf_hint ? t->rcvbuf_hint : NET_RCVBUF_DEFAULT; }

static u32 make_iss(tcb_t *t) {
    u8 k[12];
    wraddr(k, t->laddr); wraddr(k + 4, t->raddr); wr16(k + 8, t->lport); wr16(k + 10, t->rport);
    return (u32)(time_us() / 4) + (u32)net_hash(k, sizeof k);
}

static void wake(tcb_t *t) { if (t->s) sock_wake(t->s); }

/* ---------------- ports ---------------- */

static int port_busy(u32 addr, u16 port, tcb_t *self, int reuse) {
    for (tcb_t *t = all; t; t = t->next) {
        if (t == self || !t->bound || t->lport != port) continue;
        if (t->laddr && addr && t->laddr != addr) continue;
        if (!reuse) return 1;
        if (t->state == TCPS_LISTEN) return 1;
        if (t->state == TCPS_CLOSED && t->s && !t->s->reuseaddr && !t->hashed) return 1;   /* bound, idle, unwilling */
    }
    return 0;
}

static int autobind(tcb_t *t) {
    u16 p = port_random();
    for (int i = 0; i <= PORT_EPH_HI - PORT_EPH_LO; i++) {
        if (!port_busy(t->laddr, p, t, 0)) { t->lport = p; t->bound = 1; return 0; }
        p = p == PORT_EPH_HI ? PORT_EPH_LO : p + 1;
    }
    return E_ADDRINUSE;
}

/* ---------------- memory of a connection ---------------- */

static netacct_t *acct_of(tcb_t *t) { return t->s ? t->s->acct : NULL; }

static void ooo_free_all(tcb_t *t) {
    while (t->ooo) {
        ooo_t *o = t->ooo;
        t->ooo = o->next;
        net_uncharge(acct_of(t), o->mem);
        kfree(o);
    }
    t->ooo_bytes = t->ooo_n = 0;
    t->ooo_fin = 0;
}

/* the program let go of the socket: the tcb lives on without it */
static int detach(tcb_t *t) {
    sock_t *s = t->s;
    if (!s) return 0;
    nq_free(&t->rq);
    ooo_free_all(t);
    u32 m = t->sq.mem;
    if (mm_heap_free() < NET_HEAP_RESERVE + m) return E_NOMEM;   /* (then the caller resets it) */
    t->rq.owner = NULL;
    net_uncharge(s->acct, m);
    t->sq.owner = NULL;
    net_charge(NULL, m);                /* (cannot fail: the same bytes, the heap checked) */
    t->s = NULL;
    s->priv = NULL;
    t->orphan = 1;
    norphan++;
    return 0;
}

static void tcp_free(tcb_t *t) {
    hash_del(t);
    for (tcb_t **pp = &all; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    ntcb--;
    if (t->state == TCPS_TIME_WAIT) ntw--;
    if (t->orphan) norphan--;
    if (t->inq == 1) nembryonic--;
    nq_free(&t->sq);
    nq_free(&t->rq);
    ooo_free_all(t);
    if (t->s) t->s->priv = NULL;
    kfree(t);
}

/* ---------------- sending segments ---------------- */

/* the window to offer (unscaled bytes), never shrinking what we offered,
 * moving the right edge only in steps worth it (receiver SWS avoidance) */
static u32 rcv_window(tcb_t *t) {
    u32 buf = rcvbuf_of(t);
    u32 used = t->rq.bytes + t->ooo_bytes;
    u32 win = used >= buf ? 0 : buf - used;
    u32 max = (u32)65535 << t->rcv_ws;
    if (win > max) win = max;
    u32 promised = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
    /* while there is a hole, every ACK must look the same: senders count
     * duplicate ACKs only when the window does not change (RFC 5681 2) */
    if (win < promised || t->ooo) return promised;
    if (win - promised < MIN(buf / 2, (u32)t->mss) && win < buf / 2) win = promised;
    return win;
}

static int seg_send(tcb_t *t, u32 seq, u32 len, u8 flags) {
    u8 *h = segbuf + HDRROOM;
    u32 hl = TCP_HDR;
    if (flags & TH_SYN) {
        u8 *o = h + TCP_HDR;
        o[0] = 2; o[1] = 4; wr16(o + 2, t->adv_mss);
        hl += 4;
        if (t->state == TCPS_SYN_SENT || t->peer_ws) {          /* offer scaling; answer only if offered */
            o[4] = 1; o[5] = 3; o[6] = 3; o[7] = TCP_WSCALE;
            hl += 4;
        }
    }
    wr16(h, t->lport);
    wr16(h + 2, t->rport);
    wr32(h + 4, seq);
    wr32(h + 8, (flags & TH_ACK) ? t->rcv_nxt : 0);
    h[12] = (u8)((hl / 4) << 4);
    h[13] = flags;
    u32 win = rcv_window(t);
    u32 field;
    if (flags & TH_SYN) field = MIN(win, 65535u);
    else {
        field = win >> t->rcv_ws;
        u32 promised = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
        if ((field << t->rcv_ws) < promised)                    /* rounding must not shrink it */
            field = (promised + (1u << t->rcv_ws) - 1) >> t->rcv_ws;
        if (field > 65535) field = 65535;
    }
    wr16(h + 14, (u16)field);
    h[16] = h[17] = 0;
    h[18] = h[19] = 0;
    if (len) nq_copy(&t->sq, seq - t->sq_seq, h + hl, len);
    u16 c = csum_fold(csum_add(csum_pseudo(t->laddr, t->raddr, 6, hl + len), h, hl + len));
    __builtin_memcpy(h + 16, &c, 2);
    if (flags & TH_ACK) {
        u32 edge = t->rcv_nxt + ((flags & TH_SYN) ? field : field << t->rcv_ws);
        if (SEQ_GT(edge, t->rcv_adv)) t->rcv_adv = edge;
        t->ack_now = 0;
        t->delack_segs = 0;
        t->t_delack = 0;
    }
    t->last_tx = time_ms();
    nstat.tcp_out++;
    if (flags & TH_RST) nstat.tcp_rst_out++;
    return ip_output(segbuf, HDRROOM, hl + len, t->laddr, t->raddr, 6, t->ttl, t->tos, IPF_DF, NULL);
}

/* a RST answering a segment that has no connection (RFC 9293 3.10.7.1) */
static void send_rst_for(const ip_meta_t *m, const u8 *th, u32 dlen) {
    u8 flags = th[13];
    if (flags & TH_RST || m->bcast) return;
    u8 buf[HDRROOM + TCP_HDR];
    u8 *h = buf + HDRROOM;
    memset(h, 0, TCP_HDR);
    wr16(h, rd16(th + 2));
    wr16(h + 2, rd16(th));
    if (flags & TH_ACK) {
        wr32(h + 4, rd32(th + 8));
        h[13] = TH_RST;
    } else {
        wr32(h + 8, rd32(th + 4) + dlen + !!(flags & TH_SYN) + !!(flags & TH_FIN));
        h[13] = TH_RST | TH_ACK;
    }
    h[12] = 5 << 4;
    u16 c = csum_fold(csum_add(csum_pseudo(m->dst, m->src, 6, TCP_HDR), h, TCP_HDR));
    __builtin_memcpy(h + 16, &c, 2);
    nstat.tcp_out++;
    nstat.tcp_rst_out++;
    ip_output(buf, HDRROOM, TCP_HDR, m->dst, m->src, 6, 0, 0, IPF_DF, NULL);
}

/* a pure ACK (or RST) carries snd_max, not a snd_nxt that a timeout pulled
 * back below what the peer has (it would find it out of the window) */
static void send_ack(tcb_t *t) { seg_send(t, t->snd_max, 0, TH_ACK); }

/* RFC 5961: at most a few challenge ACKs per second per connection */
static void challenge_ack(tcb_t *t) {
    u64 now = time_ms();
    if (now - t->chal_t >= 1000) { t->chal_t = now; t->chal_n = 0; }
    if (t->chal_n >= TCP_CHALLENGE_PER_S) return;
    t->chal_n++;
    nstat.tcp_challenge++;
    send_ack(t);
}

static void start_rexmt(tcb_t *t) { tset(&t->t_rexmt, time_ms() + t->rto); }

static void tcp_output(tcb_t *t) {
    if (t->state == TCPS_CLOSED || t->state == TCPS_LISTEN) return;
    if (t->state == TCPS_TIME_WAIT) { if (t->ack_now) send_ack(t); return; }
    u64 now = time_ms();
    int synstate = t->state == TCPS_SYN_SENT || t->state == TCPS_SYN_RCVD;
    /* restart after idle (RFC 5681 4.1) */
    if (t->snd_una == t->snd_max && now - t->last_tx > t->rto) {
        u32 iw = TCP_IW_SEGS * (u32)t->mss;
        if (t->cwnd > iw) t->cwnd = iw;
    }
    for (int iter = 0; iter < 512; iter++) {
        u8 flags = TH_ACK;
        int syn = synstate && t->snd_nxt == t->iss;
        if (syn) flags = t->state == TCPS_SYN_SENT ? TH_SYN : TH_SYN | TH_ACK;
        else if (t->state == TCPS_SYN_SENT) return;           /* waiting for the answer */
        u32 data_end = t->sq_seq + t->sq.bytes;
        u32 avail = (!synstate && SEQ_GEQ(t->snd_nxt, t->sq_seq) && SEQ_LT(t->snd_nxt, data_end)) ? data_end - t->snd_nxt : 0;
        u32 inflight = t->snd_nxt - t->snd_una;
        u32 wnd = MIN(t->snd_wnd, t->cwnd);
        u32 usable = wnd > inflight ? wnd - inflight : 0;
        u32 len = MIN(MIN(avail, usable), (u32)t->mss);
        int fin = !synstate && t->wr_shut && t->snd_nxt + len == data_end && !SEQ_GT(t->snd_nxt, data_end) &&
                  (t->state == TCPS_FIN_WAIT_1 || t->state == TCPS_CLOSING || t->state == TCPS_LAST_ACK);
        if (fin && len < avail) fin = 0;
        int send = syn || fin || t->ack_now;
        if (len) {
            if (len == t->mss) send = 1;
            else if (len == avail && (t->nodelay || inflight == 0)) send = 1;  /* Nagle */
            else if (t->max_sndwnd && len >= t->max_sndwnd / 2) send = 1;
            else if (SEQ_LT(t->snd_nxt, t->snd_max)) send = 1;              /* retransmitting */
            else if (t->force) send = 1;                                     /* the persist timer says so */
        }
        if (!send) {
            /* data waits (no window, or too small a one, with nothing in
             * flight): the persist timer probes, or sends what fits */
            if (avail && inflight == 0 && !t->t_persist && !t->t_rexmt) {
                t->persist_n = 0;
                tset(&t->t_persist, now + CLAMP(t->rto, (u32)TCP_RTO_MIN, (u32)TCP_RTO_MAX));
            }
            t->force = 0;
            return;
        }
        t->force = 0;
        if (fin) flags |= TH_FIN;
        if (len && len == avail) flags |= TH_PSH;
        u32 seq = t->snd_nxt;
        if (!len && !(flags & (TH_SYN | TH_FIN))) seq = t->snd_max;     /* (a pure ACK: see send_ack) */
        seg_send(t, seq, len, flags);
        u32 adv = len + !!(flags & TH_SYN) + !!(flags & TH_FIN);
        if (flags & TH_FIN) t->fin_sent = 1;
        t->snd_nxt += adv;
        if (adv) {
            if (SEQ_GT(t->snd_nxt, t->snd_max)) {
                if (!t->rtt_on && seq == t->snd_max) { t->rtt_on = 1; t->rtt_seq = seq; t->rtt_start = now; }
                t->snd_max = t->snd_nxt;
            }
            if (!t->t_rexmt) start_rexmt(t);
            t->t_persist = 0;
        }
        if (!adv) return;
    }
}

/* retransmit one segment at snd_una (fast retransmit, NewReno partial ACK) */
static void rexmt_first(tcb_t *t) {
    u32 data_end = t->sq_seq + t->sq.bytes;
    u32 len = SEQ_LT(t->snd_una, data_end) ? MIN(data_end - t->snd_una, (u32)t->mss) : 0;
    u8 flags = TH_ACK;
    if (t->fin_sent && t->snd_una + len == data_end) flags |= TH_FIN;
    if (!len && !(flags & TH_FIN)) return;
    nstat.tcp_rexmt++;
    t->rtt_on = 0;                      /* Karn */
    seg_send(t, t->snd_una, len, flags);
}

/* ---------------- ending ---------------- */

/* the connection is over: tell the socket (err 0 = an orderly end).
 * Returns 1 when the tcb was freed (it had no socket). */
static int tcp_close_conn(tcb_t *t, int err) {
    hash_del(t);
    set_state(t, TCPS_CLOSED);
    t->t_rexmt = t->t_persist = t->t_delack = t->t_keep = t->t_close = 0;
    if (t->inq) {                       /* half-open, or not accepted yet: the listener forgets it */
        tcb_t *l = t->listener;
        if (l) {
            tcb_t **head = t->inq == 1 ? &l->synq : &l->acq;
            tcb_t *prev = NULL;
            for (tcb_t **pp = head; *pp; prev = *pp, pp = &(*pp)->qnext) if (*pp == t) { *pp = t->qnext; break; }
            if (t->inq == 1) l->synq_n--;
            else { l->acq_n--; if (l->acq_tail == t) l->acq_tail = prev; }
        }
        if (t->inq == 1) nembryonic--;
        int was_acq = t->inq == 2;
        t->inq = 0;
        t->listener = NULL;
        if (was_acq && t->s) {          /* a socket nobody accepted: close it ourselves */
            sock_t *s = t->s;
            t->err = err;
            sock_close(s);
            return 1;
        }
    }
    if (err) {
        t->err = err;
        if (t->s) t->s->error = err;
    }
    if (!t->s) { tcp_free(t); return 1; }
    wake(t);
    return 0;
}

static int tcp_reset(tcb_t *t, int err) {
    if (t->state != TCPS_SYN_SENT && t->state != TCPS_CLOSED && t->state != TCPS_LISTEN && t->state != TCPS_TIME_WAIT)
        seg_send(t, t->snd_max, 0, TH_RST | TH_ACK);
    return tcp_close_conn(t, err);
}

static void enter_time_wait(tcb_t *t) {
    if (ntw >= TCP_TW_MAX) {            /* recycle the oldest TIME_WAIT */
        tcb_t *old = NULL;
        for (tcb_t *x = all; x; x = x->next)
            if (x->state == TCPS_TIME_WAIT && !x->s && (!old || x->t_close < old->t_close)) old = x;
        if (old) tcp_free(old);
    }
    set_state(t, TCPS_TIME_WAIT);
    t->t_rexmt = t->t_persist = t->t_keep = 0;
    tset(&t->t_close, time_ms() + TCP_TIMEWAIT_MS);
    wake(t);
}

/* ---------------- receiving ---------------- */

static void parse_opts(const u8 *o, u32 n, u16 *mss, int *ws) {
    while (n) {
        u8 k = o[0];
        if (k == 0) break;
        if (k == 1) { o++; n--; continue; }
        if (n < 2) break;
        u8 l = o[1];
        if (l < 2 || l > n) break;
        if (k == 2 && l == 4) *mss = rd16(o + 2);
        if (k == 3 && l == 3) *ws = MIN(o[2], 14);
        o += l; n -= l;
    }
}

static u16 mss_for(u32 raddr) {
    u32 m = ip_route_mtu(raddr);
    if (!m) m = 576;
    return (u16)CLAMP(m - IP_HDR - TCP_HDR, (u32)TCP_MIN_MSS, 65535u);
}

static void set_peer_opts(tcb_t *t, u16 pmss, int pws) {
    t->peer_mss = pmss ? (u16)MAX(pmss, TCP_MIN_MSS) : TCP_DEFAULT_MSS;
    t->mss = MIN(t->peer_mss, mss_for(t->raddr));
    if (pws >= 0) { t->peer_ws = 1; }
}

static void ws_settle(tcb_t *t, int pws) {
    if (pws >= 0 && t->peer_ws) { t->ws_ok = 1; t->snd_ws = (u8)pws; t->rcv_ws = TCP_WSCALE; }
    else { t->ws_ok = 0; t->snd_ws = 0; t->rcv_ws = 0; }
}

static void init_cc(tcb_t *t) {
    t->cwnd = TCP_IW_SEGS * (u32)t->mss;
    t->ssthresh = 0x7FFFFFFF;
    t->recover = t->iss;
}

static void rtt_sample(tcb_t *t, u32 r) {
    if (!r) r = 1;
    if (!t->srtt) {
        t->srtt = r << 3;
        t->rttvar = r << 1;
    } else {
        i32 delta = (i32)r - (i32)(t->srtt >> 3);
        t->srtt = (u32)((i32)t->srtt + delta);
        if (delta < 0) delta = -delta;
        t->rttvar = (u32)((i32)t->rttvar + delta - (i32)(t->rttvar >> 2));
    }
    u32 rto = (t->srtt >> 3) + MAX(t->rttvar, 1u);
    t->rto = CLAMP(rto, (u32)TCP_RTO_MIN, (u32)TCP_RTO_MAX);
}

/* insert out-of-order data (already trimmed to the window) */
static void ooo_insert(tcb_t *t, u32 seq, const u8 *d, u32 len) {
    ooo_t **pp = &t->ooo;
    while (*pp && SEQ_LEQ((*pp)->seq + (*pp)->len, seq)) pp = &(*pp)->next;
    /* *pp is the first segment that ends after seq */
    if (*pp && SEQ_LEQ((*pp)->seq, seq)) {          /* overlaps its front */
        u32 end = (*pp)->seq + (*pp)->len;
        if (SEQ_GEQ(end, seq + len)) return;       /* nothing new */
        u32 k = end - seq;
        d += k; seq += k; len -= k;
        pp = &(*pp)->next;
    }
    while (*pp && SEQ_LT((*pp)->seq, seq + len)) {
        ooo_t *e = *pp;
        if (SEQ_LEQ(e->seq + e->len, seq + len)) {  /* covered by the new one */
            *pp = e->next;
            t->ooo_bytes -= e->len;
            t->ooo_n--;
            net_uncharge(acct_of(t), e->mem);
            kfree(e);
            continue;
        }
        len = e->seq - seq;                         /* keep the old one's data */
        break;
    }
    if (!len || t->ooo_n >= TCP_OOO_SEGS) return;
    u32 mem = (u32)sizeof(ooo_t) + len;
    if (net_charge(acct_of(t), mem)) return;
    ooo_t *o = kmalloc(mem);
    if (!o) { net_uncharge(acct_of(t), mem); return; }
    o->seq = seq; o->len = len; o->mem = mem;
    memcpy(o->data, d, len);
    o->next = *pp;
    *pp = o;
    t->ooo_bytes += len;
    t->ooo_n++;
    nstat.tcp_ooo++;
}

/* in-order data: into the receive queue (or discarded after SHUT_RD);
 * returns the bytes taken */
static u32 take_data(tcb_t *t, const u8 *d, u32 len) {
    if (t->rd_shut || !t->s) return len;
    uio_t k = { (u64)(uptr)d, len, 0, 1 };
    long n = nq_put_stream(&t->rq, &k, len);
    return n > 0 ? (u32)n : 0;
}

static void got_fin(tcb_t *t) {
    t->fin_rcvd = 1;
    t->rcv_nxt++;
    t->ack_now = 1;
    switch (t->state) {
    case TCPS_SYN_RCVD:
    case TCPS_ESTABLISHED: set_state(t, TCPS_CLOSE_WAIT); break;
    case TCPS_FIN_WAIT_1:
        if (t->fin_sent && t->snd_una == t->snd_max) enter_time_wait(t);
        else set_state(t, TCPS_CLOSING);
        break;
    case TCPS_FIN_WAIT_2: enter_time_wait(t); break;
    }
    wake(t);
}

static void ooo_pull(tcb_t *t) {
    while (t->ooo && SEQ_LEQ(t->ooo->seq, t->rcv_nxt)) {
        ooo_t *o = t->ooo;
        t->ooo = o->next;
        t->ooo_bytes -= o->len;
        t->ooo_n--;
        if (SEQ_GT(o->seq + o->len, t->rcv_nxt)) {
            u32 off = t->rcv_nxt - o->seq;
            t->rcv_nxt += take_data(t, o->data + off, o->len - off);
        }
        net_uncharge(acct_of(t), o->mem);
        kfree(o);
    }
    if (t->ooo_fin && t->ooo_fin_seq == t->rcv_nxt && !t->fin_rcvd) { t->ooo_fin = 0; got_fin(t); }
}

/* a new connection request to a listener */
static void listen_input(tcb_t *l, const ip_meta_t *m, const u8 *th, u32 hl, u32 dlen) {
    u8 flags = th[13];
    if (flags & TH_RST) return;
    if (flags & TH_ACK) { send_rst_for(m, th, dlen); return; }
    if (!(flags & TH_SYN) || (flags & TH_FIN)) return;
    if (ip_bad_unicast(m->src) || m->bcast) return;
    if (l->synq_n >= MIN(TCP_SYNQ_MAX, MAX(8, 2 * l->backlog)) || nembryonic >= TCP_EMBRYONIC_MAX || l->acq_n >= l->backlog) {
        nstat.tcp_synq_drop++;
        return;
    }
    tcb_t *t = tcb_alloc();
    if (!t) return;
    t->bound = 1;
    t->laddr = m->dst; t->lport = rd16(th + 2);
    t->raddr = m->src; t->rport = rd16(th);
    t->listener = l;
    t->inq = 1;
    t->qnext = l->synq;
    l->synq = t;
    l->synq_n++;
    nembryonic++;
    t->rcvbuf_hint = l->s ? l->s->rcvbuf : NET_RCVBUF_DEFAULT;
    t->nodelay = l->nodelay; t->keepalive = l->keepalive;
    t->keepidle = l->keepidle; t->keepintvl = l->keepintvl; t->keepcnt = l->keepcnt;
    t->ttl = l->ttl; t->tos = l->tos;
    u16 pmss = 0; int pws = -1;
    parse_opts(th + TCP_HDR, hl - TCP_HDR, &pmss, &pws);
    set_peer_opts(t, pmss, pws);
    ws_settle(t, pws);
    t->adv_mss = mss_for(t->raddr);
    t->irs = rd32(th + 4);
    t->rcv_nxt = t->irs + 1;
    t->rcv_adv = t->rcv_nxt;
    t->iss = make_iss(t);
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    t->sq_seq = t->iss + 1;
    t->snd_wnd = rd16(th + 14);                     /* (never scaled in a SYN) */
    t->snd_wl1 = t->irs; t->snd_wl2 = t->iss;
    t->max_sndwnd = t->snd_wnd;
    init_cc(t);
    set_state(t, TCPS_SYN_RCVD);
    t->last_rx = time_ms();
    hash_add(t);
    nstat.tcp_passive++;
    tcp_output(t);                                  /* SYN-ACK */
}

/* the handshake of a half-open connection completed: it becomes a socket */
static int promote(tcb_t *t) {
    tcb_t *l = t->listener;
    if (!l || !l->s || l->acq_n >= l->backlog) return E_AGAIN;
    sock_t *ns;
    adopting = t;
    int r = sock_new(&inet_family, SOCK_STREAM, IPPROTO_TCP, l->s->acct, &ns);
    adopting = NULL;
    if (r) return r;
    ns->rcvbuf = l->s->rcvbuf;
    ns->sndbuf = l->s->sndbuf;
    ns->reuseaddr = l->s->reuseaddr;
    /* out of the SYN queue, into the accept queue */
    for (tcb_t **pp = &l->synq; *pp; pp = &(*pp)->qnext) if (*pp == t) { *pp = t->qnext; break; }
    l->synq_n--;
    nembryonic--;
    t->inq = 2;
    t->qnext = NULL;
    if (l->acq_tail) l->acq_tail->qnext = t; else l->acq = t;
    l->acq_tail = t;
    l->acq_n++;
    sock_wake(l->s);
    return 0;
}

static void established(tcb_t *t) {
    set_state(t, TCPS_ESTABLISHED);
    nstat.tcp_established++;
    t->t_rexmt = 0;
    t->nrexmt = 0;
    t->last_rx = time_ms();
    if (t->keepalive) tset(&t->t_keep, t->last_rx + (u64)t->keepidle * 1000);
    wake(t);
}

void tcp_input(const ip_meta_t *m, const u8 *p, u32 len) {
    nstat.tcp_in++;
    if (len < TCP_HDR) { nstat.tcp_bad++; return; }
    u32 hl = (u32)(p[12] >> 4) * 4;
    if (hl < TCP_HDR || hl > len) { nstat.tcp_bad++; return; }
    if (m->ifc->dev && csum_fold(csum_add(csum_pseudo(m->src, m->dst, 6, len), p, len))) { nstat.tcp_bad++; return; }
    if (ip_bad_unicast(m->src)) return;
    u16 sport = rd16(p), dport = rd16(p + 2);
    u32 seq = rd32(p + 4), ack = rd32(p + 8);
    u8 flags = p[13];
    u32 win = rd16(p + 14);
    const u8 *data = p + hl;
    u32 dlen = len - hl;
    if (flags & TH_RST) nstat.tcp_rst_in++;
    tcb_t *t = lookup(m->dst, dport, m->src, sport);
    u64 now = time_ms();

    /* TIME_WAIT and a new SYN with a higher sequence number: a new connection */
    if (t && t->state == TCPS_TIME_WAIT && (flags & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN && SEQ_GT(seq, t->rcv_nxt)) {
        tcb_t *l = lookup_listener(m->dst, dport);
        if (l) {
            if (t->s) tcp_close_conn(t, 0); else tcp_free(t);
            listen_input(l, m, p, hl, dlen);
            return;
        }
    }
    if (!t || t->state == TCPS_CLOSED) {
        tcb_t *l = lookup_listener(m->dst, dport);
        if (l) { listen_input(l, m, p, hl, dlen); return; }
        send_rst_for(m, p, dlen);
        return;
    }
    t->last_rx = now;
    t->keep_sent = 0;

    if (t->state == TCPS_SYN_SENT) {
        if (flags & TH_ACK) {
            if (SEQ_LEQ(ack, t->iss) || SEQ_GT(ack, t->snd_max)) {
                if (!(flags & TH_RST)) send_rst_for(m, p, dlen);
                return;
            }
        }
        if (flags & TH_RST) {
            if (flags & TH_ACK) tcp_close_conn(t, E_CONNREFUSED);
            return;
        }
        if (!(flags & TH_SYN)) return;
        u16 pmss = 0; int pws = -1;
        parse_opts(p + TCP_HDR, hl - TCP_HDR, &pmss, &pws);
        set_peer_opts(t, pmss, pws);
        ws_settle(t, pws);
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->rcv_adv = t->rcv_nxt;
        init_cc(t);
        if (flags & TH_ACK) {
            t->snd_una = ack;
            t->sq_seq = t->iss + 1;
            if (SEQ_LT(t->snd_nxt, t->snd_una)) t->snd_nxt = t->snd_una;
            t->snd_wnd = win;                       /* SYN: not scaled */
            t->snd_wl1 = seq; t->snd_wl2 = ack;
            t->max_sndwnd = t->snd_wnd;
            if (t->rtt_on) { rtt_sample(t, (u32)(now - t->rtt_start)); t->rtt_on = 0; }
            established(t);
            t->ack_now = 1;
            /* data in the SYN-ACK: dropped (it will come again) */
            tcp_output(t);
            return;
        }
        /* simultaneous open */
        set_state(t, TCPS_SYN_RCVD);
        t->snd_wnd = win;
        t->snd_wl1 = seq; t->snd_wl2 = t->iss;
        t->snd_nxt = t->iss;                        /* SYN-ACK from our ISS */
        tcp_output(t);
        return;
    }

    /* 1. sequence number acceptable? (RFC 9293 3.10.7.4) */
    u32 rwnd = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
    u32 slen = dlen + !!(flags & TH_SYN) + !!(flags & TH_FIN);
    int ok;
    /* (an empty segment may sit at the right edge, as BSD and Linux accept it) */
    if (!slen) ok = SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LEQ(seq, t->rcv_nxt + rwnd);
    else if (!rwnd) ok = 0;
    else ok = (SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + rwnd)) ||
              (SEQ_GEQ(seq + slen - 1, t->rcv_nxt) && SEQ_LT(seq + slen - 1, t->rcv_nxt + rwnd));
    if (!ok) {
        if (flags & TH_RST) return;
        if (t->state == TCPS_TIME_WAIT && (flags & TH_FIN)) tset(&t->t_close, now + TCP_TIMEWAIT_MS);
        if (dlen) nstat.tcp_dupack++;
        else if (now - t->oow_t < 200) return;     /* empty and outside the window: answer at most 5 a second */
        if (!dlen) t->oow_t = now;
        send_ack(t);
        return;
    }
    /* 2. RST: exactly at rcv_nxt resets, elsewhere in the window gets a challenge */
    if (flags & TH_RST) {
        if (seq != t->rcv_nxt) { challenge_ack(t); return; }
        if (t->state == TCPS_TIME_WAIT) return;                 /* RFC 1337 */
        if (t->state == TCPS_SYN_RCVD && t->listener) { tcp_close_conn(t, 0); return; }
        int err = (t->state == TCPS_SYN_RCVD) ? E_CONNREFUSED :
                  (t->state == TCPS_CLOSING || t->state == TCPS_LAST_ACK) ? 0 : E_CONNRESET;
        tcp_close_conn(t, err);
        return;
    }
    /* 3. SYN in a synchronized state: challenge (never reset) */
    if (flags & TH_SYN) {
        if (t->state == TCPS_SYN_RCVD && seq == t->irs) { t->snd_nxt = t->iss; tcp_output(t); return; }  /* (retransmitted SYN) */
        challenge_ack(t);
        return;
    }
    /* 4. ACK */
    if (!(flags & TH_ACK)) return;
    if (t->state == TCPS_SYN_RCVD) {
        if (SEQ_LEQ(ack, t->snd_una) || SEQ_GT(ack, t->snd_max)) { send_rst_for(m, p, dlen); return; }
        if (t->listener && promote(t)) return;      /* accept queue full: the peer will retry */
        t->snd_wnd = win << t->snd_ws;
        t->snd_wl1 = seq; t->snd_wl2 = ack;
        t->max_sndwnd = MAX(t->max_sndwnd, t->snd_wnd);
        if (t->rtt_on) { rtt_sample(t, (u32)(now - t->rtt_start)); t->rtt_on = 0; }
        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, t->snd_una)) t->snd_nxt = t->snd_una;
        established(t);
        if (t->wr_shut) set_state(t, TCPS_FIN_WAIT_1);
    }
    if (SEQ_GT(ack, t->snd_max)) { challenge_ack(t); return; }  /* acks what we never sent */
    if (SEQ_LT(ack, t->snd_una - MAX(t->max_sndwnd, 65535u))) { challenge_ack(t); return; }
    u32 swnd = win << t->snd_ws;
    if (SEQ_GT(ack, t->snd_una)) {
        u32 acked = ack - t->snd_una;
        u32 flight = t->snd_max - t->snd_una;
        if (t->rtt_on && SEQ_GT(ack, t->rtt_seq)) { rtt_sample(t, (u32)(now - t->rtt_start)); t->rtt_on = 0; }
        /* data that may go */
        u32 data_end = t->sq_seq + t->sq.bytes;
        if (SEQ_GT(ack, t->sq_seq)) {
            u32 n = (SEQ_LT(ack, data_end) ? ack : data_end) - t->sq_seq;
            nq_drop(&t->sq, n);
            t->sq_seq += n;
            wake(t);
        }
        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, t->snd_una)) t->snd_nxt = t->snd_una;
        t->nrexmt = 0;
        t->persist_n = 0;
        /* congestion control */
        if (t->in_rec) {
            if (SEQ_GEQ(ack, t->recover)) {         /* full ACK: recovery is over */
                u32 fl = t->snd_max - t->snd_una;
                t->cwnd = MIN(t->ssthresh, fl + t->mss);
                t->in_rec = 0;
            } else {                                /* partial ACK: the next hole */
                rexmt_first(t);
                t->cwnd = (t->cwnd > acked ? t->cwnd - acked : 0) + t->mss;
                start_rexmt(t);
            }
        } else if (t->cwnd < t->ssthresh) {
            t->cwnd += MIN(acked, 2u * t->mss);         /* RFC 3465, L = 2 (ACKs come every second segment) */
        } else {
            t->cwnd_acc += acked;
            if (t->cwnd_acc >= t->cwnd) { t->cwnd_acc -= t->cwnd; t->cwnd += t->mss; }
        }
        if (t->cwnd > (1u << 30)) t->cwnd = 1u << 30;
        t->dupacks = 0;
        (void)flight;
        if (t->snd_una == t->snd_max) t->t_rexmt = 0;
        else if (!t->in_rec) start_rexmt(t);
        t->soft_err = 0;
    } else if (ack == t->snd_una && !dlen && !(flags & (TH_SYN | TH_FIN)) && swnd == t->snd_wnd && t->snd_max != t->snd_una) {
        /* duplicate ACK */
        nstat.tcp_dupack++;
        if (++t->dupacks == 3 && !t->in_rec && SEQ_GT(t->snd_una, t->recover - 1)) {
            u32 flight = t->snd_max - t->snd_una;
            t->ssthresh = MAX(flight / 2, 2u * t->mss);
            t->recover = t->snd_max;
            t->in_rec = 1;
            nstat.tcp_fast_rexmt++;
            rexmt_first(t);
            t->cwnd = t->ssthresh + 3u * t->mss;
            start_rexmt(t);
        } else if (t->in_rec) {
            t->cwnd += t->mss;
        }
    }
    /* send window update */
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
        int opened = swnd > t->snd_wnd;
        t->snd_wnd = swnd;
        t->snd_wl1 = seq; t->snd_wl2 = ack;
        if (swnd > t->max_sndwnd) t->max_sndwnd = swnd;
        if (opened && swnd >= (u32)t->mss) t->t_persist = 0;
    }
    /* our FIN acknowledged? */
    int fin_acked = t->fin_sent && t->snd_una == t->snd_max;
    switch (t->state) {
    case TCPS_FIN_WAIT_1:
        if (fin_acked) {
            set_state(t, TCPS_FIN_WAIT_2);
            if (!t->s) tset(&t->t_close, now + TCP_FIN_TIMEOUT_MS);
            wake(t);
        }
        break;
    case TCPS_CLOSING:
        if (fin_acked) enter_time_wait(t);
        break;
    case TCPS_LAST_ACK:
        if (fin_acked) { tcp_close_conn(t, 0); return; }
        break;
    case TCPS_TIME_WAIT:
        if (flags & TH_FIN) { t->ack_now = 1; tset(&t->t_close, now + TCP_TIMEWAIT_MS); }
        tcp_output(t);
        return;
    }
    /* 5. data (URG is ignored: urgent data is ordinary data) */
    int can_data = t->state == TCPS_ESTABLISHED || t->state == TCPS_FIN_WAIT_1 || t->state == TCPS_FIN_WAIT_2;
    if (dlen && can_data && !t->s) {                /* closed by its program: nobody will read it */
        tcp_reset(t, 0);
        return;
    }
    int fin = !!(flags & TH_FIN);
    if (dlen && can_data) {
        /* trim what we have already, and what is beyond the window */
        if (SEQ_LT(seq, t->rcv_nxt)) {
            u32 k = t->rcv_nxt - seq;
            if (k >= dlen) { dlen = 0; } else { data += k; dlen -= k; seq += k; }
            t->ack_now = 1;
        }
        u32 edge = t->rcv_nxt + rwnd;
        if (dlen && SEQ_GT(seq + dlen, edge)) { dlen = SEQ_GT(edge, seq) ? edge - seq : 0; fin = 0; }
        if (dlen) {
            if (seq == t->rcv_nxt) {
                int had_hole = t->ooo != NULL;
                u32 took = take_data(t, data, dlen);
                t->rcv_nxt += took;
                if (took < dlen) fin = 0;
                if (had_hole) { ooo_pull(t); t->ack_now = 1; }
                if (++t->delack_segs >= 2) t->ack_now = 1;     /* every second segment */
                else if (!t->t_delack) tset(&t->t_delack, now + TCP_DELACK_MS);
                if (took) wake(t);
            } else {
                ooo_insert(t, seq, data, dlen);
                if (fin) { t->ooo_fin = 1; t->ooo_fin_seq = seq + dlen; fin = 0; }
                t->ack_now = 1;                     /* a duplicate ACK says where the hole is */
            }
        }
    }
    /* 6. FIN */
    if (fin && !t->fin_rcvd && seq + dlen == t->rcv_nxt &&
        (t->state == TCPS_ESTABLISHED || t->state == TCPS_FIN_WAIT_1 || t->state == TCPS_FIN_WAIT_2 || t->state == TCPS_SYN_RCVD))
        got_fin(t);
    else if (fin && t->fin_rcvd) t->ack_now = 1;    /* (retransmitted FIN) */
    tcp_output(t);
}

/* ---------------- ICMP errors ---------------- */

void tcp_err(const u8 *orig, u32 len, int err, u8 type, u8 code, u32 mtu, int local) {
    u32 hl = (u32)(orig[0] & 15) * 4;
    const u8 *th = orig + hl;
    tcb_t *t = lookup(rdaddr(orig + 12), rd16(th), rdaddr(orig + 16), rd16(th + 2));
    if (!t || t->state == TCPS_LISTEN || t->state == TCPS_CLOSED || t->state == TCPS_TIME_WAIT) return;
    u32 seq = rd32(th + 4);
    if (!local) {                                   /* RFC 5927: it must be about something in flight */
        if (t->state == TCPS_SYN_SENT ? seq != t->iss : !(SEQ_GEQ(seq, t->snd_una) && SEQ_LT(seq, t->snd_max))) return;
    }
    if (type == 3 && code == 4) {                   /* fragmentation needed: smaller segments */
        u16 nm = mss_for(t->raddr);
        if (nm < t->mss) {
            t->mss = MAX(nm, (u16)TCP_MIN_MSS);
            t->snd_nxt = t->snd_una;                /* resend in the new size (not a congestion signal) */
            tcp_output(t);
        }
        return;
    }
    if (!err) return;
    if (t->state == TCPS_SYN_SENT) { tcp_close_conn(t, err); return; }
    if (t->state == TCPS_SYN_RCVD && t->listener) {
        if (err == E_CONNREFUSED) tcp_close_conn(t, 0);
        return;
    }
    t->soft_err = err;
}

/* ---------------- timers ---------------- */

static int rexmt_timeout(tcb_t *t, u64 now) {
    t->t_rexmt = 0;
    int syn = t->state == TCPS_SYN_SENT || t->state == TCPS_SYN_RCVD;
    int limit = syn ? (t->state == TCPS_SYN_SENT ? TCP_SYN_RETRIES : TCP_SYNACK_RETRIES) : TCP_RETRIES;
    if (t->snd_una == t->snd_max) return 0;
    if (t->nrexmt >= limit) {
        nstat.tcp_timeouts++;
        int err = t->soft_err ? t->soft_err : E_TIMEOUT;
        if (t->state == TCPS_SYN_RCVD && t->listener) return tcp_close_conn(t, 0);
        return tcp_reset(t, err);
    }
    t->nrexmt++;
    nstat.tcp_rexmt++;
    t->rto = MIN(t->rto * 2, (u32)TCP_RTO_MAX);
    if (!syn) {
        u32 flight = t->snd_max - t->snd_una;
        t->ssthresh = MAX(flight / 2, 2u * t->mss);
        t->cwnd = t->mss;
        t->cwnd_acc = 0;
        t->recover = t->snd_max;
        t->in_rec = 0;
        t->dupacks = 0;
    }
    t->rtt_on = 0;                                  /* Karn */
    if (t->nrexmt > 3) t->srtt = 0;                 /* RFC 6298 5.7: start the estimate over */
    t->snd_nxt = t->snd_una;
    if (syn) t->snd_nxt = t->iss;
    tcp_output(t);
    if (!t->t_rexmt && t->snd_una != t->snd_max) tset(&t->t_rexmt, now + t->rto);
    return 0;
}

static int persist_timeout(tcb_t *t, u64 now) {
    t->t_persist = 0;
    u32 data_end = t->sq_seq + t->sq.bytes;
    if (t->snd_nxt == data_end || t->snd_nxt != t->snd_una) return 0;
    if (MIN(t->snd_wnd, t->cwnd)) { t->force = 1; tcp_output(t); return 0; }   /* a small window: use it */
    if (t->persist_n >= TCP_PROBES) { nstat.tcp_timeouts++; return tcp_reset(t, E_TIMEOUT); }
    t->persist_n++;
    nstat.tcp_persist++;
    seg_send(t, t->snd_una - 1, 0, TH_ACK);        /* old byte: the peer answers with its window */
    u32 iv = MIN(t->rto << MIN(t->persist_n, (u8)7), (u32)TCP_RTO_MAX);
    tset(&t->t_persist, now + MAX(iv, (u32)TCP_RTO_MIN));
    return 0;
}

static int keep_timeout(tcb_t *t, u64 now) {
    t->t_keep = 0;
    if (!t->keepalive || (t->state != TCPS_ESTABLISHED && t->state != TCPS_CLOSE_WAIT)) return 0;
    u64 due = t->keep_sent ? t->keep_last + (u64)t->keepintvl * 1000 : t->last_rx + (u64)t->keepidle * 1000;
    if (now < due) { tset(&t->t_keep, due); return 0; }
    if (t->keep_sent >= t->keepcnt) { nstat.tcp_keep_drop++; return tcp_reset(t, E_TIMEOUT); }
    t->keep_sent++;
    t->keep_last = now;
    seg_send(t, t->snd_una - 1, 0, TH_ACK);
    tset(&t->t_keep, now + (u64)t->keepintvl * 1000);
    return 0;
}

u64 tcp_timer(u64 now) {
    if (now < tcp_next) return tcp_next;
    tcp_next = ~0ull;
    tcb_t *nx;
    for (tcb_t *t = all; t; t = nx) {
        nx = t->next;
        if (t->t_close && now >= t->t_close) {
            t->t_close = 0;
            if (t->state == TCPS_TIME_WAIT || t->state == TCPS_FIN_WAIT_2) {
                if (t->s) { tcp_close_conn(t, 0); } else { tcp_free(t); }
                continue;
            }
        }
        if (t->t_delack && now >= t->t_delack) { t->t_delack = 0; t->ack_now = 1; tcp_output(t); }
        if (t->t_rexmt && now >= t->t_rexmt && rexmt_timeout(t, now)) continue;     /* (freed) */
        if (t->t_persist && now >= t->t_persist && persist_timeout(t, now)) continue;
        if (t->t_keep && now >= t->t_keep && keep_timeout(t, now)) continue;
    }
    for (tcb_t *t = all; t; t = t->next) {
        u64 ts[5] = { t->t_close, t->t_delack, t->t_rexmt, t->t_persist, t->t_keep };
        for (int i = 0; i < 5; i++) if (ts[i] && ts[i] < tcp_next) tcp_next = ts[i];
    }
    return tcp_next;
}

/* ---------------- socket operations ---------------- */

int tcp_create(sock_t *s) {
    tcb_t *t = adopting;
    if (!t) {
        t = tcb_alloc();
        if (!t) return E_NOMEM;
    }
    adopting = NULL;
    t->s = s;
    t->sq.owner = s;
    t->rq.owner = s;
    s->priv = t;
    s->ops = &tcp_ops;
    return 0;
}

static void t_release(sock_t *s) {
    tcb_t *t = T(s);
    if (!t) return;
    if (t->state == TCPS_LISTEN) {
        while (t->synq) { tcb_t *e = t->synq; seg_send(e, e->snd_max, 0, TH_RST | TH_ACK); tcp_close_conn(e, 0); }
        while (t->acq) {                            /* never accepted: reset */
            tcb_t *e = t->acq;
            t->acq = e->qnext;
            if (!t->acq) t->acq_tail = NULL;
            t->acq_n--;
            e->inq = 0;
            e->listener = NULL;
            e->unaccepted = 1;
            sock_close(e->s);
        }
        t->s = NULL;
        s->priv = NULL;
        tcp_free(t);
        return;
    }
    if (t->state == TCPS_CLOSED || t->state == TCPS_SYN_SENT) {
        t->s = NULL; s->priv = NULL;
        tcp_free(t);
        return;
    }
    if (t->state == TCPS_TIME_WAIT) {
        if (detach(t)) { t->s = NULL; s->priv = NULL; tcp_free(t); }
        return;
    }
    int abort = t->rq.bytes || t->linger0 || t->unaccepted || norphan >= TCP_ORPHAN_MAX || t->inq == 2;
    if (abort) {
        seg_send(t, t->snd_max, 0, TH_RST | TH_ACK);
        t->s = NULL; s->priv = NULL;
        if (t->inq) { tcp_close_conn(t, 0); return; }   /* (frees it: no socket) */
        tcp_free(t);
        return;
    }
    if (detach(t)) {
        seg_send(t, t->snd_max, 0, TH_RST | TH_ACK);
        t->s = NULL; s->priv = NULL;
        tcp_free(t);
        return;
    }
    if (!t->wr_shut) {
        t->wr_shut = 1;
        if (t->state == TCPS_ESTABLISHED) set_state(t, TCPS_FIN_WAIT_1);
        else if (t->state == TCPS_CLOSE_WAIT) set_state(t, TCPS_LAST_ACK);
        /* (SYN_RCVD: the FIN follows once our SYN is acknowledged) */
    }
    if (t->state == TCPS_FIN_WAIT_2) tset(&t->t_close, time_ms() + TCP_FIN_TIMEOUT_MS);
    tcp_output(t);
}

static int t_bind(sock_t *s, const struct sockaddr *a, u32 len) {
    tcb_t *t = T(s);
    u32 addr; u16 port;
    int r = sin_parse(a, len, &addr, &port);
    if (r) return r;
    if (t->bound || t->state != TCPS_CLOSED) return E_INVAL;
    if (addr && !ip_is_local(addr)) return E_ADDRNOTAVAIL;
    t->laddr = addr;
    if (!port) return autobind(t);
    if (port_busy(addr, port, t, s->reuseaddr)) return E_ADDRINUSE;
    t->lport = port;
    t->bound = 1;
    return 0;
}

static int t_listen(sock_t *s, int backlog) {
    tcb_t *t = T(s);
    if (t->state == TCPS_LISTEN) { t->backlog = backlog; return 0; }
    if (t->state != TCPS_CLOSED || t->hashed) return E_INVAL;
    if (!t->bound) { int r = autobind(t); if (r) return r; }
    for (tcb_t *x = all; x; x = x->next)
        if (x != t && x->state == TCPS_LISTEN && x->lport == t->lport && (!x->laddr || !t->laddr || x->laddr == t->laddr))
            return E_ADDRINUSE;
    t->backlog = backlog;
    set_state(t, TCPS_LISTEN);
    return 0;
}

static int t_accept(sock_t *s, sock_t **out) {
    tcb_t *l = T(s);
    if (l->state != TCPS_LISTEN) return E_INVAL;
    tcb_t *t = l->acq;
    if (!t) return E_AGAIN;
    l->acq = t->qnext;
    if (!l->acq) l->acq_tail = NULL;
    l->acq_n--;
    t->inq = 0;
    t->qnext = NULL;
    t->listener = NULL;
    *out = t->s;
    sock_wake(s);
    return 0;
}

static int t_connect(sock_t *s, const struct sockaddr *a, u32 len) {
    tcb_t *t = T(s);
    switch (t->state) {
    case TCPS_SYN_SENT: case TCPS_SYN_RCVD: return E_INPROGRESS;
    case TCPS_CLOSED: break;
    case TCPS_LISTEN: return E_INVAL;
    default: return E_ISCONN;
    }
    if (t->hashed) return E_ISCONN;
    u32 addr; u16 port;
    int r = sin_parse(a, len, &addr, &port);
    if (r) return r;
    if (!addr) addr = IP4(127, 0, 0, 1);
    if (!port || ip_bad_unicast(addr) || ip_is_bcast(NULL, addr)) return E_INVAL;
    route_res_t rt;
    if (route_lookup(addr, &rt)) return E_NETUNREACH;
    if (rt.bcast) return E_INVAL;
    u32 laddr = t->laddr ? t->laddr : rt.src;
    if (!t->bound) {
        t->laddr = 0;
        if ((r = autobind(t))) return r;
    }
    if (lookup(laddr, t->lport, addr, port)) return E_ADDRINUSE;
    /* (a fresh start: an earlier attempt may have failed) */
    nq_free(&t->sq);
    nq_free(&t->rq);
    ooo_free_all(t);
    t->laddr = laddr;
    t->raddr = addr;
    t->rport = port;
    t->err = 0; t->err_told = 0; t->soft_err = 0;
    t->fin_rcvd = t->fin_sent = t->rd_shut = t->wr_shut = 0;
    t->peer_ws = 0;
    t->adv_mss = mss_for(addr);
    t->mss = t->adv_mss;
    t->iss = make_iss(t);
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    t->sq_seq = t->iss + 1;
    t->snd_wnd = 0;
    t->rcv_nxt = t->rcv_adv = 0;
    t->rcv_ws = 0;                                  /* (until the SYN-ACK says the peer scales) */
    t->rto = TCP_RTO_INIT; t->srtt = t->rttvar = 0;
    t->nrexmt = 0;
    t->cwnd = TCP_IW_SEGS * (u32)t->mss;
    s->error = 0;
    set_state(t, TCPS_SYN_SENT);
    hash_add(t);
    nstat.tcp_active++;
    tcp_output(t);
    net_poke();
    return E_INPROGRESS;
}

static long t_send(sock_t *s, uio_t *u, int flags, const struct sockaddr *to, u32 tolen) {
    tcb_t *t = T(s);
    if (t->err) {
        if (!t->err_told) { t->err_told = 1; return t->err; }
        return E_PIPE;
    }
    switch (t->state) {
    case TCPS_SYN_SENT: case TCPS_SYN_RCVD: return E_AGAIN;     /* (the generic layer waits for the connection) */
    case TCPS_ESTABLISHED: case TCPS_CLOSE_WAIT: break;
    case TCPS_CLOSED: case TCPS_LISTEN: return t->hashed || t->fin_rcvd ? E_PIPE : E_NOTCONN;
    default: return E_PIPE;
    }
    if (t->wr_shut) return E_PIPE;
    u32 room = s->sndbuf > t->sq.bytes ? s->sndbuf - t->sq.bytes : 0;
    if (!room) return E_AGAIN;
    long n = nq_put_stream(&t->sq, u, room);
    if (n <= 0) return n ? n : E_AGAIN;
    tcp_output(t);
    net_poke();
    return n;
}

/* after the program read: open the window if it grew enough */
static void window_update(tcb_t *t) {
    if (t->state != TCPS_ESTABLISHED && t->state != TCPS_FIN_WAIT_1 && t->state != TCPS_FIN_WAIT_2) return;
    u32 promised = SEQ_GT(t->rcv_adv, t->rcv_nxt) ? t->rcv_adv - t->rcv_nxt : 0;
    u32 w = rcv_window(t);
    u32 buf = rcvbuf_of(t);
    if (w > promised && (w - promised >= 2u * t->mss || w - promised >= buf / 2 || promised < t->mss)) {
        t->ack_now = 1;
        tcp_output(t);
        net_poke();
    }
}

static long t_recv(sock_t *s, uio_t *u, int flags, struct sockaddr *from, u32 *fromlen) {
    tcb_t *t = T(s);
    if (fromlen) {
        if (t->state != TCPS_CLOSED || t->hashed) *fromlen = sin_make(from, *fromlen, t->raddr, t->rport);
        else *fromlen = 0;
    }
    if (t->rq.bytes) {
        long n = nq_get_stream(&t->rq, u, flags & MSG_PEEK);
        if (n > 0 && !(flags & MSG_PEEK)) window_update(t);
        return n;
    }
    if (t->fin_rcvd || t->rd_shut) return 0;
    if (t->err) {
        if (!t->err_told) { t->err_told = 1; return t->err; }
        return 0;
    }
    switch (t->state) {
    case TCPS_SYN_SENT: case TCPS_SYN_RCVD: case TCPS_ESTABLISHED: case TCPS_FIN_WAIT_1: case TCPS_FIN_WAIT_2:
        return E_AGAIN;
    case TCPS_CLOSED: return t->hashed ? 0 : E_NOTCONN;
    case TCPS_LISTEN: return E_NOTCONN;
    default: return 0;
    }
}

static int t_shutdown(sock_t *s, int how) {
    tcb_t *t = T(s);
    if (t->state == TCPS_CLOSED || t->state == TCPS_LISTEN || t->state == TCPS_SYN_SENT) return E_NOTCONN;
    if (how != SHUT_WR) {
        t->rd_shut = 1;
        nq_free(&t->rq);
        ooo_free_all(t);
    }
    if (how != SHUT_RD && !t->wr_shut) {
        t->wr_shut = 1;
        if (t->state == TCPS_ESTABLISHED) set_state(t, TCPS_FIN_WAIT_1);
        else if (t->state == TCPS_CLOSE_WAIT) set_state(t, TCPS_LAST_ACK);
        tcp_output(t);
        net_poke();
    }
    sock_wake(s);
    return 0;
}

static int t_getname(sock_t *s, int peer, struct sockaddr *a, u32 *len) {
    tcb_t *t = T(s);
    if (peer) {
        if (t->state == TCPS_CLOSED || t->state == TCPS_LISTEN || t->state == TCPS_SYN_SENT) return E_NOTCONN;
        *len = sin_make(a, *len, t->raddr, t->rport);
    } else {
        *len = sin_make(a, *len, t->laddr, t->bound ? t->lport : 0);
    }
    return 0;
}

static int t_poll(sock_t *s) {
    tcb_t *t = T(s);
    int m = s->error ? POLLERR : 0;
    switch (t->state) {
    case TCPS_LISTEN:
        if (t->acq) m |= POLLIN;
        return m;
    case TCPS_SYN_SENT: case TCPS_SYN_RCVD:
        return m;
    case TCPS_CLOSED:
        if (t->hashed || t->err || t->fin_rcvd || t->fin_sent) m |= POLLIN | POLLOUT | POLLHUP | POLLRDHUP;
        else m |= POLLOUT | POLLHUP;                /* never connected */
        return m;
    default: break;
    }
    if (t->rq.bytes || t->fin_rcvd || t->rd_shut) m |= POLLIN;
    if (t->fin_rcvd || t->rd_shut) m |= POLLRDHUP;
    if (t->wr_shut) m |= POLLOUT;                   /* (a send fails at once) */
    else if (t->state == TCPS_ESTABLISHED || t->state == TCPS_CLOSE_WAIT) {
        u32 room = s->sndbuf > t->sq.bytes ? s->sndbuf - t->sq.bytes : 0;
        if (room >= s->sndbuf / 4 || room >= 16384) m |= POLLOUT;
    }
    if (t->state == TCPS_TIME_WAIT || (t->fin_rcvd && t->wr_shut)) m |= POLLHUP;
    return m;
}

static int t_ioctl(sock_t *s, long req, u64 *arg) {
    if (req != FIONREAD) return E_NOTSUP;
    *arg = T(s)->rq.bytes;
    return 0;
}

static int t_setopt(sock_t *s, int level, int name, const void *v, u32 len) {
    tcb_t *t = T(s);
    int iv = len >= sizeof(int) ? *(const int *)v : 0;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_KEEPALIVE:
            if (len < sizeof(int)) return E_INVAL;
            t->keepalive = !!iv;
            t->keep_sent = 0;
            if (t->keepalive && (t->state == TCPS_ESTABLISHED || t->state == TCPS_CLOSE_WAIT))
                tset(&t->t_keep, t->last_rx + (u64)t->keepidle * 1000);
            net_poke();
            return 0;
        case SO_LINGER: {
            if (len < sizeof(struct linger)) return E_INVAL;
            const struct linger *l = v;
            t->linger0 = l->l_onoff && l->l_linger == 0;
            return 0;
        }
        }
        return E_NOTSUP;
    }
    if (level == IPPROTO_TCP) {
        if (len < sizeof(int)) return E_INVAL;
        switch (name) {
        case TCP_NODELAY:
            t->nodelay = !!iv;
            if (t->nodelay) { tcp_output(t); net_poke(); }
            return 0;
        case TCP_MAXSEG: return 0;                  /* (accepted; the path decides) */
        case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT:
            if (iv < 1 || iv > (name == TCP_KEEPCNT ? 127 : 32767)) return E_INVAL;
            if (name == TCP_KEEPIDLE) t->keepidle = (u32)iv;
            else if (name == TCP_KEEPINTVL) t->keepintvl = (u32)iv;
            else t->keepcnt = (u32)iv;
            if (t->keepalive && (t->state == TCPS_ESTABLISHED || t->state == TCPS_CLOSE_WAIT)) {
                tset(&t->t_keep, time_ms());
                net_poke();
            }
            return 0;
        }
        return E_NOTSUP;
    }
    if (level == IPPROTO_IP) return inet_ipopt_set(&t->ttl, &t->tos, name, v, len);
    return E_NOTSUP;
}

static int t_getopt(sock_t *s, int level, int name, void *v, u32 *len) {
    tcb_t *t = T(s);
    int *iv = v;
    *len = sizeof(int);
    if (level == SOL_SOCKET) {
        if (name == SO_KEEPALIVE) { *iv = t->keepalive; return 0; }
        if (name == SO_LINGER) {
            struct linger *l = v;
            l->l_onoff = t->linger0; l->l_linger = 0;
            *len = sizeof *l;
            return 0;
        }
        return E_NOTSUP;
    }
    if (level == IPPROTO_TCP) {
        switch (name) {
        case TCP_NODELAY: *iv = t->nodelay; return 0;
        case TCP_MAXSEG: *iv = t->mss; return 0;
        case TCP_KEEPIDLE: *iv = (int)t->keepidle; return 0;
        case TCP_KEEPINTVL: *iv = (int)t->keepintvl; return 0;
        case TCP_KEEPCNT: *iv = (int)t->keepcnt; return 0;
        }
        return E_NOTSUP;
    }
    if (level == IPPROTO_IP) return inet_ipopt_get(t->ttl, t->tos, name, v, len);
    return E_NOTSUP;
}

const sock_ops_t tcp_ops = {
    .release = t_release, .bind = t_bind, .listen = t_listen, .accept = t_accept, .connect = t_connect,
    .send = t_send, .recv = t_recv, .shutdown = t_shutdown, .getname = t_getname, .poll = t_poll,
    .ioctl = t_ioctl, .setopt = t_setopt, .getopt = t_getopt,
};

void tcp_init(void) {}

int tcp_count(void) { return ntcb; }

int tcp_conns(hal_netconn_t *out, int max) {
    int n = 0;
    for (tcb_t *t = all; t && n < max; t = t->next) {
        if (t->state == TCPS_CLOSED && !t->bound) continue;
        hal_netconn_t *c = &out[n++];
        memset(c, 0, sizeof *c);
        c->proto = NETC_TCP;
        c->state = t->state;
        c->laddr = t->laddr; c->lport = t->lport;
        c->raddr = t->raddr; c->rport = t->rport;
        c->rxq = t->rq.bytes;
        c->txq = t->sq.bytes;
        c->pid = t->s ? net_acct_pid(t->s->acct) : 0;
    }
    return n;
}
