/* SOCKTEST.EXE - checks sockets and poll: local stream and datagram sockets
 * between this program and copies of itself (started with an argument),
 * back-pressure, non-blocking calls and timeouts, poll on sockets and window
 * events, backlogs, peers that crash or are ended, names, bad pointers and
 * limits. Run by the automated QEMU tests (the `sock` variant) and by hand
 * ("socktest" in the Command Prompt). Prints "socktest: all N checks passed"
 * or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("socktest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define EXE "C:/PROGRAMS/SOCKTEST.EXE"
#define SA(a) ((struct sockaddr *)(a))
#define MINL(a, b) ((a) < (b) ? (a) : (b))

static unsigned char pat(long i) { return (unsigned char)((i * 7) ^ (i >> 8) ^ (i >> 16)); }

static int start(const char *mode, const char *name, long n) {
    char args[96];
    snprintf(args, sizeof args, "%s %s %ld", mode, name, n);
    return spawn(EXE, args);
}

static int connect_retry(const char *name, int ms) {
    uint64_t t0 = time_ms();
    for (;;) {
        int fd = local_connect(name);
        if (fd >= 0 || fd != E_CONNREFUSED || time_ms() - t0 > (uint64_t)ms) return fd;
        sleep_ms(10);
    }
}

static int accept_within(int lfd, int ms) {
    struct pollfd p = { lfd, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1) return E_AGAIN;
    return accept(lfd, 0, 0);
}

static socklen_t name_of(int fd, int peer, char *out) {
    struct sockaddr_local a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    int r = peer ? getpeername(fd, SA(&a), &l) : getsockname(fd, SA(&a), &l);
    if (r) { strcpy(out, "(error)"); return 0; }
    strlcpy(out, l > 2 ? a.sl_name : "", 64);
    return l;
}

/* ================= child programs ================= */

/* a child's failure: say what went wrong (the exit code identifies the step) */
static int cfail(int code, long r) {
    printf("socktest: child %d: step %d failed: %ld (%s)\n", getpid(), code, r, r < 0 ? hal_strerror((int)r) : "");
    return code;
}

/* echo one connection until end of stream */
static int child_server(const char *name) {
    int l = local_listen(name, 4);
    if (l < 0) return 1;
    int c = accept_within(l, 10000);
    if (c < 0) return 2;
    static char buf[4096];
    long r;
    while ((r = recv(c, buf, sizeof buf, 0)) > 0)
        if (send(c, buf, (size_t)r, 0) != r) return 3;
    close(c);
    close(l);
    return r == 0 ? 0 : 4;
}

/* send n bytes to an echo server and check what comes back, non-blocking
 * with poll (a blocking client would deadlock with the echo server once
 * both buffers are full) */
static int child_client(const char *name, long n) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 10;
    if (sock_nonblock(fd, 1)) return 11;
    static unsigned char out[8192], in[8192];
    long sent = 0, got = 0;
    uint64_t t0 = time_ms();
    while (got < n) {
        if (time_ms() - t0 > 30000) return 12;
        struct pollfd p = { fd, (short)(POLLIN | (sent < n ? POLLOUT : 0)), 0 };
        if (poll(&p, 1, 5000) != 1) return 13;
        if ((p.revents & POLLOUT) && sent < n) {
            long k = (long)(1 + rand() % sizeof out);
            k = MINL(n - sent, k);
            for (long i = 0; i < k; i++) out[i] = pat(sent + i);
            long r = send(fd, out, (size_t)k, 0);
            if (r > 0) sent += r;
            else if (r != E_AGAIN) return 14;
            if (sent == n) shutdown(fd, SHUT_WR);
        }
        if (p.revents & (POLLIN | POLLHUP)) {
            long r = recv(fd, in, sizeof in, 0);
            if (r == 0) break;
            if (r < 0 && r != E_AGAIN) return 15;
            for (long i = 0; i < r; i++) if (in[i] != pat(got + i)) return 16;
            if (r > 0) got += r;
        }
    }
    struct pollfd p = { fd, POLLIN, 0 };
    long r = poll(&p, 1, 5000);
    if (r != 1) return cfail(17, r);
    if ((r = recv(fd, in, sizeof in, 0)) != 0) return cfail(19, r);       /* end of stream */
    close(fd);
    return got == n ? 0 : 18;
}

/* read n bytes slowly (the sender must wait), then answer 'K' */
static int child_sink(const char *name, long n) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 20;
    static unsigned char buf[3000];
    long got = 0, next_pause = 300000;
    while (got < n) {
        long r = recv(fd, buf, 1 + rand() % sizeof buf, 0);
        if (r <= 0) return 21;
        for (long i = 0; i < r; i++) if (buf[i] != pat(got + i)) return 22;
        got += r;
        if (got >= next_pause) { sleep_ms(20); next_pause += 300000; }
    }
    if (send(fd, "K", 1, 0) != 1) return 23;
    while (recv(fd, buf, sizeof buf, 0) > 0) ;
    return 0;
}

/* connect and stay: not reading (sleep) or blocked in recv */
static int child_hold(const char *name, long mode) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 30;
    if (mode == 0) sleep_ms(60000);
    else { char b[16]; while (recv(fd, b, sizeof b, MSG_PEEK) >= 0) sleep_ms(60000); }
    return 31;
}

/* connect, say hello, then crash */
static int child_crash(const char *name, long delay) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 40;
    send(fd, "hello", 5, 0);
    sleep_ms((unsigned)delay);
    *(volatile int *)0 = 1;
    return 41;
}

/* connect, send n bytes, exit without closing */
static int child_bye(const char *name, long n) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 50;
    static unsigned char b[4096];
    for (long i = 0; i < n && i < (long)sizeof b; i++) b[i] = pat(i);
    return send(fd, b, (size_t)n, 0) == n ? 0 : 51;
}

/* connect, wait, then send "late!" */
static int child_late(const char *name, long ms) {
    int fd = connect_retry(name, 5000);
    if (fd < 0) return 60;
    sleep_ms((unsigned)ms);
    if (send(fd, "late!", 5, 0) != 5) return 61;
    char b[16];
    while (recv(fd, b, sizeof b, 0) > 0) ;
    return 0;
}

/* datagram echo: answer every message to its sender until "quit" */
static int child_dgecho(const char *name) {
    int fd = socket(AF_LOCAL, SOCK_DGRAM, 0);
    struct sockaddr_local a;
    if (fd < 0 || bind(fd, SA(&a), local_addr(&a, name))) return 70;
    sock_timeouts(fd, 10000, 10000);
    static char buf[2048];
    for (;;) {
        struct sockaddr_local from;
        socklen_t fl = sizeof from;
        long r = recvfrom(fd, buf, sizeof buf, 0, SA(&from), &fl);
        if (r < 0) return 71;
        if (r == 4 && !memcmp(buf, "quit", 4)) return 0;
        long w = sendto(fd, buf, (size_t)r, 0, SA(&from), fl);
        if (w != r) return cfail(72, w);
    }
}

/* take as many sockets and as much buffer memory as the limits allow,
 * report, and wait to be ended. Nothing here may need socket memory once
 * the others may have taken it all: the parent's "go" is end of stream
 * (shutdown), and the report goes into the buffer that already holds the
 * "r" sent before. */
typedef struct { long pairs, bytes; int sock_err, send_err; } hog_report_t;
static int child_hog(const char *name) {
    int c = connect_retry(name, 5000);
    if (c < 0) return cfail(80, c);
    if (send(c, "r", 1, 0) != 1) return 83;
    char go;
    long w = recv(c, &go, 1, 0);
    if (w != 0) return cfail(81, w);
    hog_report_t rep = { 0, 0, 0, 0 };
    static unsigned char chunk[16384];
    for (;;) {
        int sv[2];
        int r = socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
        if (r) { rep.sock_err = r; break; }
        rep.pairs++;
        int big = 256 * 1024;
        setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        for (;;) {
            w = send(sv[0], chunk, sizeof chunk, 0);
            if (w > 0) { rep.bytes += w; continue; }
            if (w != E_AGAIN && !rep.send_err) rep.send_err = (int)w;
            break;
        }
    }
    w = send(c, &rep, sizeof rep, 0);
    if (w != (long)sizeof rep) return cfail(82, w);
    sleep_ms(60000);
    return 84;
}

/* ================= checks ================= */

static void test_basics(void) {
    CHECK(socket(AF_INET, SOCK_STREAM, 0) == E_AFNOSUPPORT, "AF_INET not yet");
    CHECK(socket(77, SOCK_STREAM, 0) == E_AFNOSUPPORT, "unknown family");
    CHECK(socket(AF_LOCAL, 3, 0) == E_NOTSUP, "raw sockets");
    CHECK(socket(AF_LOCAL, SOCK_STREAM, 6) == E_NOTSUP, "protocol");
    int s = socket(AF_LOCAL, SOCK_STREAM, 0);
    CHECK(s >= 3, "socket %d", s);
    int v = 0;
    socklen_t l = sizeof v;
    CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, &v, &l) == 0 && v == SOCK_STREAM && l == sizeof v, "SO_TYPE");
    CHECK(getsockopt(s, SOL_SOCKET, SO_DOMAIN, &v, &l) == 0 && v == AF_LOCAL, "SO_DOMAIN");
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &l) == 0 && v == 65536, "default SO_RCVBUF %d", v);
    v = 100 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &l) == 0 && v == 256 * 1024, "SO_RCVBUF clamped to %d", v);
    v = 1;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
    CHECK(getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &l) == 0 && v == 2048, "SO_RCVBUF minimum %d", v);
    CHECK(setsockopt(s, SOL_SOCKET, SO_TYPE, &v, sizeof v) == E_INVAL, "SO_TYPE read only");
    CHECK(setsockopt(s, SOL_SOCKET, 999, &v, sizeof v) == E_NOTSUP, "unknown option");
    char b[8];
    CHECK(recv(s, b, sizeof b, 0) == E_NOTCONN, "recv unconnected");
    CHECK(send(s, "x", 1, 0) == E_NOTCONN, "send unconnected");
    CHECK(listen(s, 1) == E_INVAL, "listen without a name");
    CHECK(shutdown(s, SHUT_WR) == E_NOTCONN, "shutdown unconnected");
    CHECK(close(s) == 0, "close");
    CHECK(send(s, "x", 1, 0) == E_INVAL, "closed handle");
    CHECK(listen(99, 1) == E_INVAL && listen(-1, 1) == E_INVAL, "bad handles");
    int f = open("C:/PROGRAMS/SOCKTEST.EXE", O_READ);
    CHECK(f >= 0 && listen(f, 1) == E_NOTSOCK, "file handle is not a socket");
    if (f >= 0) close(f);

    /* socketpair, read / write / close work on sockets */
    int sv[2];
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == 0, "socketpair");
    CHECK(write(sv[0], "abcdef", 6) == 6, "write()");
    unsigned long n = 0;
    CHECK(ioctlsocket(sv[1], FIONREAD, &n) == 0 && n == 6, "FIONREAD %lu", n);
    CHECK(recv(sv[1], b, 3, MSG_PEEK) == 3 && !memcmp(b, "abc", 3), "MSG_PEEK");
    CHECK(read(sv[1], b, 8) == 6 && !memcmp(b, "abcdef", 6), "read() after peek");
    CHECK(send(sv[1], "xyz", 3, 0) == 3 && recv(sv[0], b, 8, 0) == 3 && !memcmp(b, "xyz", 3), "other direction");
    CHECK(send(sv[0], "", 0, 0) == 0, "zero-length send");
    CHECK(shutdown(sv[0], SHUT_WR) == 0 && recv(sv[1], b, 8, 0) == 0, "shutdown(SHUT_WR): end of stream");
    CHECK(send(sv[0], "x", 1, 0) == E_PIPE, "send after shutdown");
    CHECK(send(sv[1], "q", 1, 0) == 1 && recv(sv[0], b, 8, 0) == 1, "half-closed: other direction still works");
    CHECK(shutdown(sv[0], SHUT_RD) == 0 && send(sv[1], "q", 1, 0) == E_PIPE, "peer shut reading: E_PIPE");
    CHECK(shutdown(sv[0], 7) == E_INVAL, "bad shutdown");
    close(sv[0]);
    close(sv[1]);
}

static void test_names(void) {
    hal_sockstat_t st0, st;
    sockstat(&st0);
    struct sockaddr_local a;
    int s1 = socket(AF_LOCAL, SOCK_STREAM, 0), s2 = socket(AF_LOCAL, SOCK_STREAM, 0);
    int d = socket(AF_LOCAL, SOCK_DGRAM, 0);
    CHECK(bind(s1, SA(&a), local_addr(&a, "socktest.name")) == 0, "bind");
    CHECK(bind(s1, SA(&a), local_addr(&a, "socktest.other")) == E_INVAL, "bind twice");
    CHECK(bind(s2, SA(&a), local_addr(&a, "socktest.name")) == E_ADDRINUSE, "name taken");
    CHECK(bind(s2, SA(&a), local_addr(&a, "SockTest.NAME")) == E_ADDRINUSE, "names ignore case");
    CHECK(bind(d, SA(&a), local_addr(&a, "socktest.name")) == E_ADDRINUSE, "one namespace for all types");
    sockstat(&st);
    CHECK(st.names == st0.names + 1, "sockstat names %d", st.names);
    const char *bad[] = { "has space", ".dot", "-dash", "~auto", "a/b", "caf\xc3\xa9", "x:y" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(bind(s2, SA(&a), local_addr(&a, bad[i])) == E_INVAL, "bad name '%s'", bad[i]);
    char n63[64];
    memset(n63, 'n', 63); n63[63] = 0;
    memset(&a, 0, sizeof a);
    a.sl_family = AF_LOCAL;
    memset(a.sl_name, 'n', 64);                       /* 64 characters, no NUL */
    CHECK(bind(s2, SA(&a), sizeof a) == E_NAMETOOLONG, "name too long");
    CHECK(bind(s2, SA(&a), 1) == E_INVAL, "address too short");
    CHECK(bind(s2, SA(&a), 1000) == E_INVAL, "address too long");
    struct sockaddr_storage in4;
    memset(&in4, 0, sizeof in4);
    in4.ss_family = AF_INET;
    CHECK(bind(s2, SA(&in4), 16) == E_AFNOSUPPORT, "wrong family");
    char got[64];
    CHECK(bind(s2, SA(&a), local_addr(&a, n63)) == 0 && name_of(s2, 0, got) && !strcmp(got, n63), "63-character name");
    CHECK(name_of(s1, 0, got) == 2 + 14 && !strcmp(got, "socktest.name"), "getsockname '%s'", got);
    int au = socket(AF_LOCAL, SOCK_DGRAM, 0);
    CHECK(bind(au, SA(&a), local_addr(&a, "")) == 0 && name_of(au, 0, got) && got[0] == '~', "automatic name '%s'", got);
    char autoname[64];
    strcpy(autoname, got);
    int unb = socket(AF_LOCAL, SOCK_STREAM, 0);
    CHECK(name_of(unb, 0, got) == 2 && !got[0], "unnamed socket");
    CHECK(getpeername(unb, SA(&a), &(socklen_t){ sizeof a }) == E_NOTCONN, "getpeername unconnected");

    /* connecting */
    CHECK(connect(unb, SA(&a), local_addr(&a, "socktest.nobody")) == E_CONNREFUSED, "nobody there");
    CHECK(connect(unb, SA(&a), local_addr(&a, "socktest.name")) == E_CONNREFUSED, "bound but not listening");
    CHECK(connect(unb, SA(&a), local_addr(&a, autoname)) == E_CONNREFUSED, "stream to a datagram name");
    CHECK(connect(unb, SA(&a), local_addr(&a, "")) == E_INVAL, "empty name");
    CHECK(listen(d, 1) == E_NOTSUP, "listen on datagrams");

    close(s1);
    CHECK(bind(s2, SA(&a), local_addr(&a, "socktest.again")) == E_INVAL, "still bound to the long name");
    CHECK(bind(d, SA(&a), local_addr(&a, "SOCKTEST.NAME")) == 0, "name free after close");
    close(s2); close(d); close(au); close(unb);
    sockstat(&st);
    CHECK(st.names == st0.names && st.sockets == st0.sockets, "names and sockets released (%d, %d)", st.names, st.sockets);
}

static void test_stream_local(void) {
    int l = local_listen("socktest.local", 4);
    CHECK(l >= 0, "listen %d", l);
    int c = local_connect("socktest.local");
    CHECK(c >= 0, "connect %d", c);
    CHECK(send(c, "early", 5, 0) == 5, "send before accept");
    struct sockaddr_local a;
    socklen_t al = sizeof a;
    memset(&a, 0x55, sizeof a);
    int s = accept(l, SA(&a), &al);
    CHECK(s >= 0 && al == 2 && a.sl_family == AF_LOCAL, "accept: unnamed client (len %u)", al);
    char b[64];
    CHECK(recv(s, b, sizeof b, 0) == 5 && !memcmp(b, "early", 5), "data sent before accept");
    CHECK(name_of(c, 1, b) && !strcmp(b, "socktest.local"), "client's peer name '%s'", b);
    CHECK(name_of(s, 0, b) && !strcmp(b, "socktest.local"), "accepted socket's name '%s'", b);
    CHECK(connect(c, SA(&a), local_addr(&a, "socktest.local")) == E_ISCONN, "connect twice");
    CHECK(connect(l, SA(&a), local_addr(&a, "socktest.local")) == E_INVAL, "connect a listener");
    int v = 0;
    socklen_t vl = sizeof v;
    CHECK(getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &v, &vl) == 0 && v == 1, "SO_ACCEPTCONN");

    /* a named client */
    int c2 = socket(AF_LOCAL, SOCK_STREAM, 0);
    bind(c2, SA(&a), local_addr(&a, "socktest.caller"));
    CHECK(connect(c2, SA(&a), local_addr(&a, "SOCKTEST.LOCAL")) == 0, "connect (name in other case)");
    al = sizeof a;
    int s2 = accept(l, SA(&a), &al);
    CHECK(s2 >= 0 && !strcmp(a.sl_name, "socktest.caller") && al == 2 + 16, "accept: client name '%s'", a.sl_name);
    al = 4;                                     /* too small: truncated, real length returned */
    memset(&a, 0, sizeof a);
    CHECK(getpeername(s2, SA(&a), &al) == 0 && al == 2 + 16 && !memcmp(a.sl_name, "so", 2) && !a.sl_name[2], "truncated address");

    /* a client that closes before it is accepted: data, then end of stream */
    int c3 = local_connect("socktest.local");
    send(c3, "bye", 3, 0);
    close(c3);
    int s3 = accept(l, 0, 0);
    CHECK(s3 >= 0 && recv(s3, b, sizeof b, 0) == 3 && recv(s3, b, sizeof b, 0) == 0, "closed before accept");

    /* closing with unread data resets the peer: data first, then E_CONNRESET, then end */
    send(s2, "unread", 6, 0);
    send(c2, "xy", 2, 0);
    close(c2);
    CHECK(recv(s2, b, sizeof b, 0) == 2, "data before the reset");
    CHECK(recv(s2, b, sizeof b, 0) == E_CONNRESET, "E_CONNRESET");
    CHECK(recv(s2, b, sizeof b, 0) == 0, "then end of stream");
    CHECK(send(s2, "x", 1, 0) == E_PIPE, "send to a closed peer");
    close(s); close(s2); close(s3); close(c);

    /* closing a listener resets the connections in its backlog */
    int c4 = local_connect("socktest.local");
    close(l);
    CHECK(recv(c4, b, sizeof b, 0) == E_CONNRESET && recv(c4, b, sizeof b, 0) == 0, "listener closed: reset");
    CHECK(local_connect("socktest.local") == E_CONNREFUSED, "name gone with the listener");
    close(c4);
}

static void test_children_echo(void) {
    int srv = start("server", "socktest.echo", 0);
    int cli = start("client", "socktest.echo", 300000);
    CHECK(srv > 0 && cli > 0, "spawn echo pair");
    int rc = wait(cli), rs = wait(srv);
    CHECK(rc == 0, "echo client (child) exit %d", rc);
    CHECK(rs == 0, "echo server (child) exit %d", rs);
}

static void test_backpressure(void) {
    /* a buffer fills to exactly its size */
    int sv[2];
    socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    static unsigned char chunk[10000];
    long total = 0, r;
    while ((r = send(sv[0], chunk, sizeof chunk, 0)) > 0) total += r;
    CHECK(r == E_AGAIN && total == 65536, "buffer full at %ld (%ld)", total, r);
    struct pollfd p = { sv[0], POLLOUT, 0 };
    CHECK(poll(&p, 1, 0) == 0, "full: not writable");
    char b[100];
    CHECK(recv(sv[1], b, 100, 0) == 100 && poll(&p, 1, 0) == 1 && (p.revents & POLLOUT), "writable after a read");
    CHECK(send(sv[0], chunk, sizeof chunk, 0) == 100, "room for what was read");
    close(sv[0]); close(sv[1]);

    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
    int small = 8192;
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    sock_timeouts(sv[0], 0, 150);
    uint64_t t0 = time_ms();
    r = send(sv[0], chunk, sizeof chunk, 0);
    uint64_t dt = time_ms() - t0;
    CHECK(r == 8192 && dt >= 140 && dt < 2000, "blocking send with timeout: partial %ld after %llu ms", r, (unsigned long long)dt);
    t0 = time_ms();
    r = send(sv[0], chunk, 1, 0);
    dt = time_ms() - t0;
    CHECK(r == E_AGAIN && dt >= 140 && dt < 2000, "send timeout %ld after %llu ms", r, (unsigned long long)dt);
    close(sv[0]); close(sv[1]);

    /* 4 MiB to a slow reader in another program: send() blocks until all is taken */
    int l = local_listen("socktest.sink", 1);
    long n = 4L << 20;
    unsigned char *big = malloc((size_t)n);
    CHECK(big != 0, "malloc");
    if (!big || l < 0) return;
    for (long i = 0; i < n; i++) big[i] = pat(i);
    int ch = start("sink", "socktest.sink", n);
    int s = accept_within(l, 5000);
    CHECK(s >= 0, "sink connected");
    t0 = time_ms();
    r = send(s, big, (size_t)n, 0);
    CHECK(r == n, "large blocking send %ld", r);
    sock_timeouts(s, 10000, 0);
    CHECK(recv(s, b, 1, 0) == 1 && b[0] == 'K', "sink got everything intact");
    close(s);
    CHECK(wait(ch) == 0, "sink exit");
    free(big);
    close(l);
}

static void test_dgram(void) {
    struct sockaddr_local a, from;
    socklen_t fl;
    int x = socket(AF_LOCAL, SOCK_DGRAM, 0), y = socket(AF_LOCAL, SOCK_DGRAM, 0), z = socket(AF_LOCAL, SOCK_DGRAM, 0);
    bind(x, SA(&a), local_addr(&a, "socktest.dgx"));
    socklen_t yl = local_addr(&a, "socktest.dgy");
    bind(y, SA(&a), yl);
    struct sockaddr_local ya = a;
    static char buf[70000];
    memset(buf, 'm', sizeof buf);
    CHECK(sendto(x, buf, 10, 0, SA(&ya), yl) == 10, "sendto");
    CHECK(sendto(x, buf, 0, 0, SA(&ya), yl) == 0, "empty datagram");
    CHECK(sendto(x, buf, 100, 0, SA(&ya), yl) == 100, "sendto 100");
    unsigned long nx = 0;
    CHECK(ioctlsocket(y, FIONREAD, &nx) == 0 && nx == 10, "FIONREAD: next datagram %lu", nx);
    fl = sizeof from;
    CHECK(recvfrom(y, buf, sizeof buf, 0, SA(&from), &fl) == 10 && !strcmp(from.sl_name, "socktest.dgx"), "boundary 1, from '%s'", from.sl_name);
    CHECK(recv(y, buf, sizeof buf, 0) == 0, "boundary 2 (empty)");
    CHECK(recv(y, buf, 30, MSG_PEEK) == 30, "peek");
    CHECK(recv(y, buf, 30, MSG_PEEK | MSG_TRUNC) == 100, "MSG_TRUNC: real length");
    CHECK(recv(y, buf, 30, 0) == 30, "truncated read");
    CHECK(recv(y, buf, sizeof buf, MSG_DONTWAIT) == E_AGAIN, "rest of a truncated datagram is gone");
    CHECK(sendto(x, buf, 65537, 0, SA(&ya), yl) == E_MSGSIZE, "too long");
    CHECK(sendto(x, buf, 65536, 0, SA(&ya), yl) == 65536 && recv(y, buf, sizeof buf, 0) == 65536, "64 KiB datagram");
    CHECK(send(x, buf, 1, 0) == E_NOTCONN, "send without a destination");
    CHECK(sendto(x, buf, 1, 0, SA(&a), local_addr(&a, "socktest.none")) == E_CONNREFUSED, "no such name");

    /* queue limits */
    sock_nonblock(x, 1);
    int k = 0;
    long r;
    while ((r = sendto(x, buf, 1, 0, SA(&ya), yl)) == 1 && k < 1000) k++;
    CHECK(r == E_AGAIN && k == 128, "datagram queue full after %d (%ld)", k, r);
    struct pollfd p = { y, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLIN, "datagram readable");
    sock_nonblock(x, 0);
    sock_timeouts(x, 0, 100);
    uint64_t t0 = time_ms();
    r = sendto(x, buf, 1, 0, SA(&ya), yl);
    CHECK(r == E_AGAIN && time_ms() - t0 >= 90, "blocking sendto to a full queue times out (%ld)", r);
    sock_timeouts(x, 0, 0);
    while (recv(y, buf, sizeof buf, MSG_DONTWAIT) >= 0) ;

    /* an unnamed sender gets an automatic name, so it can be answered */
    CHECK(sendto(z, "hi", 2, 0, SA(&ya), yl) == 2, "unnamed sender");
    fl = sizeof from;
    CHECK(recvfrom(y, buf, sizeof buf, 0, SA(&from), &fl) == 2 && from.sl_name[0] == '~', "automatic sender name '%s'", from.sl_name);
    CHECK(sendto(y, "re", 2, 0, SA(&from), fl) == 2 && recv(z, buf, sizeof buf, 0) == 2 && !memcmp(buf, "re", 2), "reply to it");

    /* a connected socket takes messages from its peer only */
    CHECK(connect(y, SA(&a), local_addr(&a, "socktest.dgx")) == 0, "datagram connect");
    CHECK(sendto(z, "no", 2, 0, SA(&ya), yl) == E_PERM, "others are refused");
    CHECK(sendto(x, "ok", 2, 0, SA(&ya), yl) == 2 && recv(y, buf, sizeof buf, 0) == 2, "the peer gets through");
    CHECK(send(y, "to x", 4, 0) == 4 && recv(x, buf, sizeof buf, 0) == 4, "send to the connected peer");
    CHECK(name_of(y, 1, buf) && !strcmp(buf, "socktest.dgx"), "datagram peer name");
    close(x);
    CHECK(send(y, "x", 1, 0) == E_CONNREFUSED, "peer gone: E_CONNREFUSED");
    CHECK(recv(y, buf, sizeof buf, 0) == 0, "peer gone: end");
    struct sockaddr un = { AF_UNSPEC, { 0 } };
    CHECK(connect(y, &un, sizeof un) == 0 && send(y, "x", 1, 0) == E_NOTCONN, "AF_UNSPEC dissolves the association");
    close(y); close(z);

    /* datagram socketpair: peer closes -> end, POLLHUP */
    int sv[2];
    CHECK(socketpair(AF_LOCAL, SOCK_DGRAM, 0, sv) == 0, "datagram socketpair");
    send(sv[0], "one", 3, 0);
    send(sv[0], "two!", 4, 0);
    CHECK(recv(sv[1], buf, sizeof buf, 0) == 3 && recv(sv[1], buf, sizeof buf, 0) == 4, "pair boundaries");
    close(sv[0]);
    p.fd = sv[1]; p.events = POLLIN;
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP) && recv(sv[1], buf, sizeof buf, 0) == 0, "pair peer gone");
    close(sv[1]);

    /* with another program */
    int ch = start("dgecho", "socktest.dgecho", 0);
    int s = socket(AF_LOCAL, SOCK_DGRAM, 0);
    socklen_t el = local_addr(&a, "socktest.dgecho");
    t0 = time_ms();
    while ((r = sendto(s, "ping 0", 6, 0, SA(&a), el)) == E_CONNREFUSED && time_ms() - t0 < 5000) sleep_ms(10);
    sock_timeouts(s, 5000, 5000);
    int ok = r == 6 && recv(s, buf, sizeof buf, 0) == 6 && !memcmp(buf, "ping 0", 6);
    for (int i = 1; ok && i < 50; i++) {
        int len = snprintf(buf, 64, "ping %d ", i);
        memset(buf + len, '.', (size_t)i * 20);
        len += i * 20;
        char back[2048];
        ok = sendto(s, buf, (size_t)len, 0, SA(&a), el) == len && recv(s, back, sizeof back, 0) == len && !memcmp(back, buf, (size_t)len);
    }
    CHECK(ok, "datagram echo with another program");
    sendto(s, "quit", 4, 0, SA(&a), el);
    CHECK(wait(ch) == 0, "dgecho exit");
    close(s);
}

static void test_nonblock_timeouts(void) {
    int sv[2];
    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
    char b[16];
    CHECK(recv(sv[0], b, sizeof b, MSG_DONTWAIT) == E_AGAIN, "MSG_DONTWAIT");
    CHECK(sock_nonblock(sv[0], 1) == 0 && recv(sv[0], b, sizeof b, 0) == E_AGAIN, "FIONBIO");
    sock_nonblock(sv[0], 0);
    int ms = 150;
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms) == 0, "SO_RCVTIMEO (ms)");
    uint64_t t0 = time_ms();
    long r = recv(sv[0], b, sizeof b, 0);
    uint64_t dt = time_ms() - t0;
    CHECK(r == E_AGAIN && dt >= 140 && dt < 2000, "recv timeout %ld after %llu ms", r, (unsigned long long)dt);
    struct timeval tv = { 0, 0 };
    socklen_t tl = sizeof tv;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &tl) == 0 && tl == sizeof tv && tv.tv_sec == 0 && tv.tv_usec == 150000, "timeout as timeval");
    tv.tv_sec = 0; tv.tv_usec = 80000;
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "SO_RCVTIMEO (timeval)");
    tl = sizeof ms;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &ms, &tl) == 0 && ms == 80, "timeout as int %d", ms);
    tv.tv_usec = -1;
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == E_INVAL, "bad timeval");
    send(sv[1], "data", 4, 0);
    CHECK(recv(sv[0], b, sizeof b, 0) == 4, "data within the timeout");
    /* MSG_WAITALL waits for the whole length */
    int ch_ms = 0;
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &ch_ms, sizeof ch_ms);
    send(sv[1], "12345", 5, 0);
    sock_timeouts(sv[0], 300, 0);
    t0 = time_ms();
    r = recv(sv[0], b, 8, MSG_WAITALL);
    CHECK(r == 5 && time_ms() - t0 >= 250, "MSG_WAITALL: partial after the timeout (%ld)", r);
    close(sv[0]); close(sv[1]);

    int l = local_listen("socktest.nb", 2);
    sock_nonblock(l, 1);
    CHECK(accept(l, 0, 0) == E_AGAIN, "non-blocking accept");
    sock_nonblock(l, 0);
    sock_timeouts(l, 100, 0);
    t0 = time_ms();
    r = accept(l, 0, 0);
    CHECK(r == E_AGAIN && time_ms() - t0 >= 90, "accept timeout");
    close(l);
}

static void test_backlog(void) {
    struct sockaddr_local a;
    socklen_t al = local_addr(&a, "socktest.backlog");
    int l = local_listen("socktest.backlog", 2);
    int c[20], nc = 0;
    for (int i = 0; i < 3; i++) {
        c[nc] = socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0);
        int r = connect(c[nc], SA(&a), al);
        if (i < 2) CHECK(r == 0, "connect %d into the backlog: %d", i, r);
        else CHECK(r == E_AGAIN, "backlog full: %d", r);
        nc++;
    }
    sock_nonblock(c[2], 0);
    sock_timeouts(c[2], 0, 100);
    uint64_t t0 = time_ms();
    int r = connect(c[2], SA(&a), al);
    CHECK(r == E_AGAIN && time_ms() - t0 >= 90, "blocking connect gives up after SO_SNDTIMEO (%d)", r);
    struct pollfd p = { l, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLIN, "listener readable");
    int s = accept(l, 0, 0);
    CHECK(s >= 0 && connect(c[2], SA(&a), al) == 0, "room again after accept");
    close(s);
    for (int i = 0; i < nc; i++) close(c[i]);
    close(l);

    /* backlogs are capped */
    l = local_listen("socktest.backlog", 1000);
    hal_sockstat_t st;
    sockstat(&st);
    nc = 0;
    for (int i = 0; i < 20; i++) {
        c[nc] = socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (connect(c[nc++], SA(&a), al)) break;
    }
    CHECK(nc == st.max_backlog + 1, "backlog capped at %d (%d)", st.max_backlog, nc - 1);
    for (int i = 0; i < nc; i++) close(c[i]);
    close(l);
}

static void test_poll(void) {
    int sv[3][2];
    for (int i = 0; i < 3; i++) socketpair(AF_LOCAL, SOCK_STREAM, 0, sv[i]);
    struct pollfd p[6];
    for (int i = 0; i < 3; i++) { p[i].fd = sv[i][1]; p[i].events = POLLIN; p[i].revents = -1; }
    uint64_t t0 = time_ms();
    CHECK(poll(p, 3, 100) == 0 && time_ms() - t0 >= 90 && !p[0].revents, "poll timeout");
    send(sv[1][0], "x", 1, 0);
    CHECK(poll(p, 3, 1000) == 1 && !p[0].revents && p[1].revents == POLLIN && !p[2].revents, "one of three ready");
    p[0].events = POLLIN | POLLOUT;
    CHECK(poll(p, 3, 0) == 2 && p[0].revents == POLLOUT, "writable");
    p[3].fd = -1; p[3].events = POLLIN;
    p[4].fd = 50; p[4].events = POLLIN;
    p[5].fd = 1; p[5].events = POLLIN | POLLOUT;
    CHECK(poll(p, 6, 0) == 4 && !p[3].revents && p[4].revents == POLLNVAL && p[5].revents == POLLOUT, "ignored, invalid, console");
    int f = open("C:/PROGRAMS/SOCKTEST.EXE", O_READ);
    struct pollfd pf = { f, POLLIN | POLLOUT, 0 };
    CHECK(poll(&pf, 1, 0) == 1 && pf.revents == (POLLIN | POLLOUT), "files are always ready");
    close(f);
    CHECK(poll(p, 129, 0) == E_INVAL, "too many entries");
    CHECK(poll((struct pollfd *)0x1000, 1, 0) == E_INVAL, "bad pointer");
    CHECK(poll(0, 0, 50) == 0, "poll as a sleep");

    /* woken by another program */
    int l = local_listen("socktest.late", 1);
    int ch = start("late", "socktest.late", 200);
    struct pollfd pl = { l, POLLIN, 0 };
    CHECK(poll(&pl, 1, 5000) == 1, "connection arrives");
    int s = accept(l, 0, 0);
    p[0].fd = s; p[0].events = POLLIN;
    p[1].fd = sv[0][1]; p[1].events = POLLIN;
    t0 = time_ms();
    int r = poll(p, 2, 5000);
    uint64_t dt = time_ms() - t0;
    CHECK(r == 1 && p[0].revents == POLLIN && dt >= 100 && dt < 3000, "woken by the other program's send (%d, %llu ms)", r, (unsigned long long)dt);
    char b[16];
    CHECK(recv(s, b, sizeof b, 0) == 5 && !memcmp(b, "late!", 5), "its data");
    close(s);
    CHECK(wait(ch) == 0, "late child exit");
    close(l);

    /* hang-up */
    close(sv[2][0]);
    p[0].fd = sv[2][1]; p[0].events = POLLIN;
    CHECK(poll(p, 1, 0) == 1 && (p[0].revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP), "POLLHUP %x", p[0].revents);
    for (int i = 0; i < 3; i++) { close(sv[i][0]); close(sv[i][1]); }

    /* window events and sockets in one call */
    int w = win_create("Socket Test", 160, 80, 0);
    if (w < 0) { printf("socktest: no desktop, window poll checks skipped\n"); return; }
    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv[0]);
    struct pollfd pw[2] = { { FD_WINDOWS, POLLIN, 0 }, { sv[0][1], POLLIN, 0 } };
    CHECK(poll(pw, 2, 2000) == 1 && pw[0].revents == POLLIN, "window events (first paint)");
    hal_event_t ev;
    int painted = 0;
    while (win_get_event(&ev, 0) == 1) if (ev.type == EVT_PAINT) painted = 1;
    CHECK(painted, "the paint event");
    t0 = time_ms();
    CHECK(poll(pw, 2, 100) == 0 && time_ms() - t0 >= 90, "no window events");
    send(sv[0][0], "s", 1, 0);
    CHECK(poll(pw, 2, 1000) == 1 && !pw[0].revents && pw[1].revents == POLLIN, "socket ready, windows not");
    recv(sv[0][1], b, sizeof b, 0);
    win_set_timer(w, 60);
    t0 = time_ms();
    r = poll(pw, 2, 3000);
    dt = time_ms() - t0;
    CHECK(r == 1 && pw[0].revents == POLLIN && dt < 1000, "window timer wakes poll (%d, %llu ms)", r, (unsigned long long)dt);
    CHECK(win_get_event(&ev, 0) == 1 && ev.type == EVT_TIMER, "the timer event");
    win_destroy(w);
    close(sv[0][0]); close(sv[0][1]);
}

static void test_peer_death(void) {
    int l = local_listen("socktest.death", 4);
    char b[64];
    /* a peer that is ended (not reading, with data we sent unread) */
    int ch = start("hold", "socktest.death", 0);
    int s = accept_within(l, 5000);
    CHECK(s >= 0, "hold connected");
    send(s, "unread", 6, 0);
    sleep_ms(50);
    CHECK(kill(ch) == 0, "kill");
    sock_timeouts(s, 5000, 5000);
    uint64_t t0 = time_ms();
    long r = recv(s, b, sizeof b, 0);
    CHECK((r == 0 || r == E_CONNRESET) && time_ms() - t0 < 2000, "killed peer: %ld", r);
    CHECK(recv(s, b, sizeof b, 0) == 0, "then end of stream");
    CHECK(send(s, "x", 1, 0) == E_PIPE, "send: E_PIPE");
    struct pollfd p = { s, POLLIN | POLLOUT, 0 };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP), "POLLHUP after peer death");
    CHECK(wait(ch) == -1, "killed child's exit code");
    close(s);

    /* a peer ended while it waits in recv */
    ch = start("hold", "socktest.death", 1);
    s = accept_within(l, 5000);
    sleep_ms(100);
    t0 = time_ms();
    CHECK(s >= 0 && kill(ch) == 0 && wait(ch) == -1 && time_ms() - t0 < 1000, "end a program waiting in recv");
    sock_timeouts(s, 2000, 0);
    CHECK(recv(s, b, sizeof b, 0) == 0, "its socket ends");
    close(s);

    /* a peer that crashes while we wait in recv */
    ch = start("crash", "socktest.death", 200);
    s = accept_within(l, 5000);
    sock_timeouts(s, 5000, 0);
    CHECK(s >= 0 && recv(s, b, 5, MSG_WAITALL) == 5 && !memcmp(b, "hello", 5), "data from the crashing peer");
    t0 = time_ms();
    r = recv(s, b, sizeof b, 0);
    CHECK(r == 0 && time_ms() - t0 < 3000, "crash ends the stream (%ld)", r);
    CHECK(wait(ch) == -1014, "crash exit code");
    close(s);

    /* a peer that exits without closing: its data, then end of stream */
    ch = start("bye", "socktest.death", 1000);
    s = accept_within(l, 5000);
    CHECK(wait(ch) == 0, "bye exit");
    static unsigned char d[2000];
    sock_timeouts(s, 2000, 0);
    r = recv(s, d, sizeof d, MSG_WAITALL);
    int ok = r == 1000;
    for (int i = 0; ok && i < 1000; i++) ok = d[i] == pat(i);
    CHECK(ok, "data of an exited peer (%ld)", r);
    CHECK(recv(s, d, sizeof d, 0) == 0, "end after exit");
    close(s);
    close(l);
}

static void test_bad_pointers(void) {
    hal_sockstat_t st0, st;
    sockstat(&st0);
    int s = socket(AF_LOCAL, SOCK_STREAM, 0);
    CHECK(bind(s, (struct sockaddr *)0x1000, 10) == E_INVAL, "bind(null page)");
    CHECK(bind(s, (struct sockaddr *)0x100000, 10) == E_INVAL, "bind(kernel)");
    CHECK(connect(s, (struct sockaddr *)0xFFFF800000000000ull, 10) == E_INVAL, "connect(kernel half)");
    CHECK(getsockname(s, (struct sockaddr *)0x100000, &(socklen_t){ 16 }) == E_INVAL, "getsockname(kernel)");
    CHECK(getsockname(s, (struct sockaddr *)0, (socklen_t *)0) == E_INVAL, "getsockname(null)");
    int v = 0;
    CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, (void *)0x100000, &(socklen_t){ 4 }) == E_INVAL, "getsockopt(kernel)");
    CHECK(getsockopt(s, SOL_SOCKET, SO_TYPE, &v, (socklen_t *)0x1000) == E_INVAL, "getsockopt(bad length)");
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVBUF, (void *)0x1000, 4) == E_INVAL, "setsockopt(bad)");
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, 100000) == E_INVAL, "setsockopt(huge)");
    CHECK(ioctlsocket(s, FIONBIO, (unsigned long *)0x100000) == E_INVAL, "ioctlsocket(kernel)");
    CHECK(sockstat((hal_sockstat_t *)0x100000) == E_INVAL, "sockstat(kernel)");
    close(s);
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, (int *)0x1000) == E_INVAL, "socketpair(bad)");

    int sv[2];
    socketpair(AF_LOCAL, SOCK_STREAM, 0, sv);
    send(sv[0], "keep", 4, 0);
    CHECK(send(sv[0], (void *)0x1000, 10, 0) == E_INVAL, "send(null page)");
    CHECK(recv(sv[1], (void *)0x100000, 10, 0) == E_INVAL, "recv(kernel)");
    char b[16];
    CHECK(recv(sv[1], b, (size_t)1 << 40, 0) == 4 && !memcmp(b, "keep", 4), "data kept after bad recvs; huge length");
    close(sv[0]); close(sv[1]);

    socketpair(AF_LOCAL, SOCK_DGRAM, 0, sv);
    send(sv[0], "msg", 3, 0);
    socklen_t fl = 32;
    CHECK(recvfrom(sv[1], b, sizeof b, 0, (struct sockaddr *)0x100000, &fl) == E_INVAL, "recvfrom(bad address)");
    CHECK(recvfrom(sv[1], b, sizeof b, 0, (struct sockaddr *)b, (socklen_t *)0x1000) == E_INVAL, "recvfrom(bad length)");
    CHECK(recv(sv[1], b, sizeof b, 0) == 3, "datagram kept");
    close(sv[0]); close(sv[1]);

    int l = local_listen("socktest.badptr", 2);
    int c = local_connect("socktest.badptr");
    CHECK(accept(l, (struct sockaddr *)b, (socklen_t *)0x100000) == E_INVAL, "accept(bad length)");
    int a = accept(l, 0, 0);
    CHECK(a >= 0, "connection still waiting");
    close(a); close(c); close(l);
    sockstat(&st);
    CHECK(st.sockets == st0.sockets && st.proc_sockets == st0.proc_sockets, "nothing leaked (%d)", st.sockets);
}

static void test_limits(void) {
    hal_sockstat_t st0, st;
    sockstat(&st0);
    /* handles */
    int fds[80], n = 0, r = 0;
    while (n < 80 && (r = socket(AF_LOCAL, SOCK_DGRAM, 0)) >= 0) fds[n++] = r;
    CHECK(r == E_NOMEM && n >= 50 && n < 80, "%d sockets, then %d", n, r);
    for (int i = 0; i < n; i++) close(fds[i]);

    /* buffer memory per program */
    int pairs[40][2], np = 0;
    long total = 0;
    int err = 0;
    static unsigned char chunk[16384];
    while (np < 25 && !err) {
        if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK, 0, pairs[np])) break;
        int big = 256 * 1024;
        setsockopt(pairs[np][1], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        long w;
        while ((w = send(pairs[np][0], chunk, sizeof chunk, 0)) > 0) total += w;
        if (w != E_AGAIN) err = (int)w;
        np++;
    }
    sockstat(&st);
    CHECK(err == E_NOMEM && total <= (long)st.max_proc_mem && total > (long)st.max_proc_mem - 300000,
          "program memory limit: %ld bytes, then %d", total, err);
    CHECK(st.proc_mem <= st.max_proc_mem && st.mem <= st.max_mem, "accounting %llu", st.proc_mem);
    for (int i = 0; i < np; i++) { close(pairs[i][0]); close(pairs[i][1]); }
    sockstat(&st);
    CHECK(st.proc_mem == st0.proc_mem && st.mem == st0.mem, "memory back (%llu)", st.mem);

    /* system-wide: ten programs take everything they can */
    int l = local_listen("socktest.hogs", 16);
    int kids[10], conn[10];
    for (int i = 0; i < 10; i++) kids[i] = start("hog", "socktest.hogs", 0);
    int all = 1;
    for (int i = 0; i < 10; i++) { conn[i] = accept_within(l, 10000); if (conn[i] < 0) all = 0; }
    CHECK(all, "hogs connected");
    for (int i = 0; i < 10; i++) if (conn[i] >= 0) shutdown(conn[i], SHUT_WR);     /* go */
    long pairs_min = 1 << 30, pairs_sum = 0, bytes = 0;
    int got = 0;
    for (int i = 0; i < 10; i++) {
        struct { char r; hog_report_t rep; } __attribute__((packed)) m;
        hog_report_t rep;
        if (conn[i] < 0) continue;
        /* read only once the whole report is there (a partial read would
         * free the buffer the rest is meant to go into) */
        unsigned long avail = 0;
        uint64_t t0 = time_ms();
        while (!ioctlsocket(conn[i], FIONREAD, &avail) && avail < sizeof m && time_ms() - t0 < 20000) sleep_ms(10);
        if (recv(conn[i], &m, sizeof m, MSG_DONTWAIT) != (long)sizeof m || m.r != 'r') continue;
        memcpy(&rep, &m.rep, sizeof rep);
        got++;
        if (rep.pairs < pairs_min) pairs_min = rep.pairs;
        pairs_sum += rep.pairs;
        bytes += rep.bytes;
    }
    sockstat(&st);
    CHECK(got == 10, "hog reports %d", got);
    CHECK(st.sockets <= st.max_sockets && pairs_min < 30, "system-wide socket limit: %d sockets, fewest pairs %ld", st.sockets, pairs_min);
    /* (a socketpair needs two: one may be left) */
    int extra = socket(AF_LOCAL, SOCK_STREAM, 0), extra2 = socket(AF_LOCAL, SOCK_STREAM, 0);
    CHECK(st.sockets >= st.max_sockets - 1 && (extra == E_NOMEM || extra2 == E_NOMEM), "no more sockets for anyone (%d, %d)", extra, extra2);
    if (extra >= 0) close(extra);
    if (extra2 >= 0) close(extra2);
    CHECK(st.mem <= st.max_mem && bytes <= (long)st.max_mem && bytes > (long)st.max_mem / 2,
          "system-wide memory limit: %llu in buffers (%ld sent)", st.mem, bytes);
    /* ending them gives everything back, and tells their peers */
    int codes = 0;
    for (int i = 0; i < 10; i++) if (kill(kids[i]) == 0 && wait(kids[i]) == -1) codes++;
    CHECK(codes == 10, "hogs ended %d", codes);
    char b;
    int ends = 0;
    for (int i = 0; i < 10; i++) if (conn[i] >= 0 && recv(conn[i], &b, 1, 0) == 0) ends++;
    CHECK(ends == 10, "end of stream from the ended hogs %d", ends);
    for (int i = 0; i < 10; i++) if (conn[i] >= 0) close(conn[i]);
    close(l);
    sockstat(&st);
    CHECK(st.sockets == st0.sockets && st.mem == st0.mem && st.names == st0.names,
          "everything released: %d sockets, %llu bytes, %d names", st.sockets, st.mem, st.names);
}

int main(int argc, char **argv) {
    if (argc >= 3) {
        long n = argc >= 4 ? strtol(argv[3], 0, 10) : 0;
        const char *m = argv[1], *name = argv[2];
        if (!strcmp(m, "server")) return child_server(name);
        if (!strcmp(m, "client")) return child_client(name, n);
        if (!strcmp(m, "sink")) return child_sink(name, n);
        if (!strcmp(m, "hold")) return child_hold(name, n);
        if (!strcmp(m, "crash")) return child_crash(name, n);
        if (!strcmp(m, "bye")) return child_bye(name, n);
        if (!strcmp(m, "late")) return child_late(name, n);
        if (!strcmp(m, "dgecho")) return child_dgecho(name);
        if (!strcmp(m, "hog")) return child_hog(name);
        return 99;
    }
    printf("socktest: starting (pid %d)\n", getpid());
    hal_sockstat_t st0, st;
    sockstat(&st0);
    srand((unsigned)time_ms());
    test_basics();
    test_names();
    test_stream_local();
    test_children_echo();
    test_backpressure();
    test_dgram();
    test_nonblock_timeouts();
    test_backlog();
    test_poll();
    test_peer_death();
    test_bad_pointers();
    test_limits();
    sockstat(&st);
    CHECK(st.sockets == st0.sockets && st.mem == st0.mem && st.names == st0.names && st.proc_sockets == 0,
          "all released at the end (%d sockets, %llu bytes, %d names)", st.sockets, st.mem, st.names);
    if (failed) printf("socktest: %d of %d checks FAILED\n", failed, checks);
    else printf("socktest: all %d checks passed\n", checks);
    return failed;
}
