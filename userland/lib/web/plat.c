/* libweb's platform layer (webplat.h): sockets, name lookup, time, random
 * bytes and threads on Halcyon, or on Linux for the host tests. */
#include "webplat.h"

#ifdef WEB_HOST                 /* (compiled with -D_GNU_SOURCE) */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

static int map_errno(int e) {
    switch (e) {
    case EAGAIN: return E_AGAIN;
    case EINPROGRESS: return E_INPROGRESS;
    case ECONNREFUSED: return E_CONNREFUSED;
    case ECONNRESET: return E_CONNRESET;
    case EPIPE: return E_PIPE;
    case ETIMEDOUT: return E_TIMEOUT;
    case ENOTCONN: return E_NOTCONN;
    case ENOMEM: return E_NOMEM;
    case EAFNOSUPPORT: return E_AFNOSUPPORT;
    case EHOSTUNREACH: return E_HOSTUNREACH;
    case ENETUNREACH: return E_NETUNREACH;
    case EINVAL: return E_INVAL;
    }
    return E_IO;
}

const char *hal_strerror(int e) {
    switch (e) {
    case E_IO: return "I/O error";
    case E_NOMEM: return "Out of memory";
    case E_TIMEOUT: return "Timed out";
    case E_CONNREFUSED: return "Connection refused";
    case E_CONNRESET: return "Connection reset";
    case E_PIPE: return "Connection closed";
    case E_NOTCONN: return "Not connected";
    case E_AFNOSUPPORT: return "Address family not supported";
    case E_HOSTUNREACH: return "Host unreachable";
    case E_NETUNREACH: return "Network unreachable";
    }
    return "Error";
}

uint64_t wp_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
void wp_sleep(int ms) { usleep((useconds_t)ms * 1000); }

int wp_random(void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r <= 0) return E_IO;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int wp_resolve(const char *host, uint32_t *ip, int max, int timeout_ms, char *why, size_t whyn) {
    (void)timeout_ms;
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, 0, &hints, &res);
    if (r) {
        if (why) w_snprintf(why, whyn, "%s", gai_strerror(r));
        return r == EAI_AGAIN ? WEB_E_TIMEOUT : WEB_E_DNS;
    }
    int n = 0;
    for (struct addrinfo *a = res; a && n < max; a = a->ai_next)
        if (a->ai_family == AF_INET) ip[n++] = ((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return n ? n : WEB_E_DNS;
}

int wp_tcp_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd < 0 ? map_errno(errno) : fd;
}

int wp_tcp_connect(int fd, uint32_t ip, int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = ip;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return 0;
    return map_errno(errno);
}

int wp_poll(int fd, int events, int ms) {
    struct pollfd p = { fd, (short)((events & WP_IN ? POLLIN : 0) | (events & WP_OUT ? POLLOUT : 0)), 0 };
    int r = poll(&p, 1, ms);
    if (r < 0) return errno == EINTR ? 0 : map_errno(errno);
    if (!r) return 0;
    return (p.revents & POLLIN ? WP_IN : 0) | (p.revents & POLLOUT ? WP_OUT : 0) |
           (p.revents & (POLLERR | POLLNVAL) ? WP_ERR : 0) | (p.revents & POLLHUP ? WP_HUP : 0);
}

int wp_so_error(int fd) {
    int e = 0;
    socklen_t l = sizeof e;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0) return map_errno(errno);
    return e ? map_errno(e) : 0;
}

long wp_send(int fd, const void *b, size_t n) {
    ssize_t r = send(fd, b, n, MSG_NOSIGNAL);
    return r < 0 ? map_errno(errno) : (long)r;
}
long wp_recv(int fd, void *b, size_t n) {
    ssize_t r = recv(fd, b, n, 0);
    return r < 0 ? map_errno(errno) : (long)r;
}
void wp_close(int fd) { close(fd); }

void wp_utc_now(long long *unix_s, long *uncertain_s) {
    const char *fake = getenv("WEB_TEST_TIME");     /* tests: pretend another time (seconds since 1970) */
    const char *unc = getenv("WEB_TEST_UNCERTAIN");  /* tests: an unknown time zone, as on Halcyon (seconds) */
    *unix_s = fake ? atoll(fake) : (long long)time(0);
    *uncertain_s = unc ? atol(unc) : 0;
}

typedef struct { int (*fn)(void *); void *arg; } tstart_t;
static void *tstart(void *p) {
    tstart_t t = *(tstart_t *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
int wp_thread(int (*fn)(void *), void *arg) {
    tstart_t *t = malloc(sizeof *t);
    if (!t) return E_NOMEM;
    t->fn = fn; t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, 0, tstart, t)) { free(t); return E_NOMEM; }
    pthread_detach(th);
    return 0;
}
void wp_lock(wp_mutex_t *m) { while (__sync_lock_test_and_set(&m->v, 1)) sched_yield(); }
void wp_unlock(wp_mutex_t *m) { __sync_lock_release(&m->v); }

#else  /* ---------------- Halcyon ---------------- */

uint64_t wp_ms(void) { return time_ms(); }
void wp_sleep(int ms) { sleep_ms((unsigned)ms); }

int wp_random(void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        size_t k = n > 65536 ? 65536 : n;
        long r = getrandom(p, k);
        if (r <= 0) return r < 0 ? (int)r : E_IO;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int wp_resolve(const char *host, uint32_t *ip, int max, int timeout_ms, char *why, size_t whyn) {
    unsigned int a[16];
    if (max > 16) max = 16;
    long r = resolve(host, a, max, timeout_ms);
    if (r > 0) {
        for (long i = 0; i < r && i < max; i++) ip[i] = a[i];
        return (int)(r < max ? r : max);
    }
    if (r == 0 || r == E_NOENT) return WEB_E_DNS;
    if (r == E_TIMEOUT || r == E_AGAIN) {
        if (why) w_snprintf(why, whyn, "the DNS server did not answer");
        return WEB_E_TIMEOUT;
    }
    if (r == E_NODEV || r == E_NETUNREACH || r == E_NOTSUP || r == E_AFNOSUPPORT) return WEB_E_NONET;
    if (r == E_INVAL) return WEB_E_DNS;
    if (why) w_snprintf(why, whyn, "%s", hal_strerror((int)r));
    return r == E_NOTCONN ? WEB_E_NONET : WEB_E_DNS;
}

int wp_tcp_open(void) { return socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); }

int wp_tcp_connect(int fd, uint32_t ip, int port) {
    struct sockaddr_in a;
    socklen_t len = inet_sockaddr(&a, ip, port);
    return connect(fd, (const struct sockaddr *)&a, len);
}

int wp_poll(int fd, int events, int ms) {
    struct pollfd p = { fd, (short)((events & WP_IN ? POLLIN : 0) | (events & WP_OUT ? POLLOUT : 0)), 0 };
    int r = poll(&p, 1, ms);
    if (r <= 0) return r;
    return (p.revents & POLLIN ? WP_IN : 0) | (p.revents & POLLOUT ? WP_OUT : 0) |
           (p.revents & (POLLERR | POLLNVAL) ? WP_ERR : 0) | (p.revents & POLLHUP ? WP_HUP : 0);
}

int wp_so_error(int fd) {
    int e = 0;
    socklen_t l = sizeof e;
    int r = getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
    return r < 0 ? r : e;
}

long wp_send(int fd, const void *b, size_t n) { return send(fd, b, n, MSG_DONTWAIT | MSG_NOSIGNAL); }
long wp_recv(int fd, void *b, size_t n) { return recv(fd, b, n, MSG_DONTWAIT); }
void wp_close(int fd) { close(fd); }

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The real-time clock keeps local time (as Windows does) and Halcyon has no
 * time zone setting yet. If C:\HALCYON\SETTINGS.INI has
 * "clock.utc_offset = <minutes east of UTC>", the time is exact; otherwise
 * it may be off by up to 14 hours either way, and the certificate check
 * accepts a certificate valid at some moment within that window. */
static int utc_offset_min(int *known) {
    *known = 0;
    char *data = 0;
    long size = 0;
    if (read_file("C:/HALCYON/SETTINGS.INI", &data, &size) < 0) return 0;
    int off = 0;
    const char *key = "clock.utc_offset";
    for (char *s = data; s && *s; ) {
        char *nl = strchr(s, '\n');
        while (*s == ' ' || *s == '\t') s++;
        if (!w_strncasecmp(s, key, strlen(key))) {
            char *p = s + strlen(key);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                char *end;
                long v = strtol(p, &end, 10);
                if (end != p && v >= -16 * 60 && v <= 16 * 60) { off = (int)v; *known = 1; }
            }
        }
        s = nl ? nl + 1 : 0;
    }
    free(data);
    return off;
}

void wp_utc_now(long long *unix_s, long *uncertain_s) {
    hal_datetime_t d;
    memset(&d, 0, sizeof d);
    datetime(&d);
    long long t = days_from_civil(d.year, d.month, d.day) * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
    int known;
    int off = utc_offset_min(&known);
    *unix_s = t - off * 60LL;
    *uncertain_s = known ? 0 : 14 * 3600;
}

typedef struct { int (*fn)(void *); void *arg; } tstart_t;
static int tstart(void *p) {
    tstart_t t = *(tstart_t *)p;
    free(p);
    return t.fn(t.arg);
}
int wp_thread(int (*fn)(void *), void *arg) {
    tstart_t *t = malloc(sizeof *t);
    if (!t) return E_NOMEM;
    t->fn = fn; t->arg = arg;
    int tid = thread_create(tstart, t);
    if (tid < 0) { free(t); return tid; }
    thread_detach(tid);
    return 0;
}
void wp_lock(wp_mutex_t *m) { mutex_lock((hal_mutex_t *)m); }
void wp_unlock(wp_mutex_t *m) { mutex_unlock((hal_mutex_t *)m); }
#endif

/* ---------------- string helpers ---------------- */
int w_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int w_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int w_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
int w_strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = w_tolower((unsigned char)*a), y = w_tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
const void *w_memmem(const void *h, size_t hn, const void *n, size_t nn) {
    const unsigned char *a = h;
    if (!nn) return h;
    for (size_t i = 0; i + nn <= hn; i++)
        if (a[i] == *(const unsigned char *)n && !memcmp(a + i, n, nn)) return a + i;
    return 0;
}
void *w_memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
size_t w_strcspn(const char *s, const char *reject) {
    size_t i = 0;
    for (; s[i]; i++) if (strchr(reject, s[i])) break;
    return i;
}
size_t w_strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t w_strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + w_strlcpy(d + dl, s, n - dl);
}
int w_snprintf(char *b, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}
char *w_strndup(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
