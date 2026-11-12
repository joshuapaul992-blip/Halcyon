/* libweb: HTTP/1.1 client (RFC 9110 / 9112): GET, HEAD and POST, Host and
 * User-Agent, redirects (at most max_redirects), Content-Length, chunked
 * transfer coding, gzip / deflate content coding, persistent connections
 * (a small pool per session), cookies (a simple jar per session), and
 * timeouts, size limits and cancellation on every wait. */
#include "webint.h"

#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_HEADER_LINES 256
#define MAX_LINE 8192
#define MAX_1XX 8
#define POOL_IDLE_MS 30000
#define DEF_MAX_BODY (16u << 20)
#define DEF_REDIRECTS 10

const char *web_strerror(int e) {
    switch (e) {
    case 0: return "Success";
    case WEB_E_URL: return "The address is not valid";
    case WEB_E_DNS: return "The server's name was not found";
    case WEB_E_CONNECT: return "Could not connect to the server";
    case WEB_E_TIMEOUT: return "The server did not answer in time";
    case WEB_E_NONET: return "This computer is not connected to a network";
    case WEB_E_TLS: return "The secure connection failed";
    case WEB_E_CERT: return "The server's certificate is not valid";
    case WEB_E_PROTOCOL: return "The server's answer is not valid HTTP";
    case WEB_E_TOOBIG: return "The answer is too large";
    case WEB_E_REDIRECTS: return "Too many redirects";
    case WEB_E_CANCELED: return "Stopped";
    case WEB_E_TRUNCATED: return "The connection closed before the whole answer arrived";
    case WEB_E_DECODE: return "The compressed answer is corrupt";
    case WEB_E_HEADERS: return "The answer's headers are too large";
    case WEB_E_CLOSED: return "The server closed the connection without answering";
    case WEB_E_SCHEME: return "This kind of address cannot be fetched";
    }
    return hal_strerror(e);
}

int conn_fail(wconn_t *c, int err, const char *why) {
    if (!c->err) {
        c->err = err;
        w_strlcpy(c->why, why ? why : web_strerror(err), sizeof c->why);
    }
    return c->err;
}

static int canceled(wconn_t *c) { return c->cancel && *c->cancel; }

/* wait until fd is ready for ev, at most ms, in slices so a cancel is seen */
static int wait_ready(wconn_t *c, int ev, int ms, const char *timeout_why) {
    uint64_t end = wp_ms() + (uint64_t)ms;
    for (;;) {
        if (canceled(c)) return conn_fail(c, WEB_E_CANCELED, "Stopped");
        uint64_t now = wp_ms();
        if (now >= end) return conn_fail(c, WEB_E_TIMEOUT, timeout_why);
        int slice = (int)(end - now < 100 ? end - now : 100);
        int r = wp_poll(c->fd, ev, slice);
        if (r < 0) return conn_fail(c, r, hal_strerror(r));
        if (r) return r;
    }
}

static int sock_err(wconn_t *c, long r) {
    if (r == E_CONNRESET) return conn_fail(c, E_CONNRESET, "The server reset the connection");
    if (r == E_PIPE) return conn_fail(c, E_PIPE, "The server closed the connection");
    return conn_fail(c, (int)r, hal_strerror((int)r));
}

long conn_raw_read(wconn_t *c, void *b, size_t n) {
    for (;;) {
        if (canceled(c)) return conn_fail(c, WEB_E_CANCELED, "Stopped");
        long r = wp_recv(c->fd, b, n);
        if (r > 0) return r;
        if (r == 0) { c->eof = 1; return 0; }
        if (r != E_AGAIN) {
            if (r == E_CONNRESET || r == E_PIPE) c->eof = 1;
            return sock_err(c, r);
        }
        int w = wait_ready(c, WP_IN, c->idle_ms, "The server stopped sending data");
        if (w < 0) return w;
    }
}

int conn_raw_write(wconn_t *c, const void *b, size_t n) {
    const uint8_t *p = b;
    while (n) {
        if (canceled(c)) return conn_fail(c, WEB_E_CANCELED, "Stopped");
        long r = wp_send(c->fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r == 0) continue;
        if (r != E_AGAIN) return sock_err(c, r);
        int w = wait_ready(c, WP_OUT, c->idle_ms, "The server does not take the request");
        if (w < 0) return w;
    }
    return 0;
}

static long conn_read(wconn_t *c, void *b, size_t n) {
    return c->tls ? tls_read(c, b, n) : conn_raw_read(c, b, n);
}
static int conn_write(wconn_t *c, const void *b, size_t n) {
    return c->tls ? tls_write(c, b, n) : conn_raw_write(c, b, n);
}

static void conn_free(wconn_t *c, int graceful) {
    if (!c) return;
    if (c->tls) tls_close(c, graceful);
    if (c->fd >= 0) wp_close(c->fd);
    free(c);
}

/* more bytes into the buffer: > 0, 0 at the end, < 0 error */
static long fill(wconn_t *c) {
    if (c->bpos < c->blen) return (long)(c->blen - c->bpos);
    c->bpos = c->blen = 0;
    long r = conn_read(c, c->buf, sizeof c->buf);
    if (r > 0) c->blen = (size_t)r;
    return r;
}

/* a line (CR LF or LF) without its end; WEB_E_HEADERS if longer than max - 1,
 * 1 at the end of the stream before any byte */
static int read_line(wconn_t *c, char *line, size_t max, size_t *len) {
    size_t o = 0;
    for (;;) {
        long r = fill(c);
        if (r < 0) return (int)r;
        if (r == 0) {
            if (!o) return 1;
            return WEB_E_TRUNCATED;
        }
        uint8_t *p = c->buf + c->bpos, *e = c->buf + c->blen;
        uint8_t *nl = w_memchr(p, '\n', (size_t)(e - p));
        size_t take = nl ? (size_t)(nl - p) : (size_t)(e - p);
        if (o + take >= max) return WEB_E_HEADERS;
        memcpy(line + o, p, take);
        o += take;
        c->bpos += take + (nl ? 1 : 0);
        if (nl) break;
    }
    if (o && line[o - 1] == '\r') o--;
    line[o] = 0;
    *len = o;
    return 0;
}

/* ---------------- cookies ---------------- */
struct cookie {
    cookie_t *next;
    char *name, *value, *domain, *path;
    int host_only, secure;
    long long expires;              /* 0: for this session */
};
#define MAX_COOKIES 300
#define MAX_COOKIE_BYTES 4096

static void cookie_free(cookie_t *k) {
    free(k->name); free(k->value); free(k->domain); free(k->path); free(k);
}

static int month_of(const char *s) {
    static const char m[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    for (int i = 0; i < 12; i++)
        if (w_tolower(s[0]) == m[i * 3] && w_tolower(s[1]) == m[i * 3 + 1] && w_tolower(s[2]) == m[i * 3 + 2]) return i + 1;
    return 0;
}

/* an HTTP date (RFC 1123, RFC 850 or asctime) -> seconds since 1970, or -1 */
static long long http_date(const char *s) {
    int day = -1, mon = 0, year = -1, hh = -1, mm = 0, ss = 0;
    while (*s) {
        while (*s && !(w_tolower(*s) >= 'a' && w_tolower(*s) <= 'z') && !(*s >= '0' && *s <= '9')) s++;
        const char *t = s;
        while ((*s >= '0' && *s <= '9') || *s == ':' || (w_tolower(*s) >= 'a' && w_tolower(*s) <= 'z')) s++;
        size_t l = (size_t)(s - t);
        if (!l) break;
        if (t[0] >= '0' && t[0] <= '9') {
            if (w_memchr(t, ':', l)) {
                hh = (int)strtol(t, 0, 10);
                const char *c1 = w_memchr(t, ':', l);
                mm = (int)strtol(c1 + 1, 0, 10);
                const char *c2 = w_memchr(c1 + 1, ':', l - (size_t)(c1 + 1 - t));
                ss = c2 ? (int)strtol(c2 + 1, 0, 10) : 0;
            } else if (l <= 2 && day < 0) day = (int)strtol(t, 0, 10);
            else year = (int)strtol(t, 0, 10);
        } else if (l >= 3 && !mon) {
            mon = month_of(t);
        }
    }
    if (year >= 0 && year < 70) year += 2000;
    else if (year >= 70 && year < 100) year += 1900;
    if (day < 1 || day > 31 || !mon || year < 1601 || hh < 0 || hh > 23 || mm > 59 || ss > 60) return -1;
    int y = year - (mon <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

static int domain_match(const char *host, const char *dom) {
    size_t hl = strlen(host), dl = strlen(dom);
    if (hl == dl) return !strcmp(host, dom);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, dom);
}

static int path_match(const char *rpath, const char *cpath) {
    size_t cl = strlen(cpath);
    if (strncmp(rpath, cpath, cl)) return 0;
    return rpath[cl] == 0 || cpath[cl - 1] == '/' || rpath[cl] == '/' || rpath[cl] == '?';
}

static int is_ipv4(const char *h, uint32_t *ip) {
    unsigned v[4];
    int n = 0;
    const char *p = h;
    for (; n < 4; n++) {
        if (!(*p >= '0' && *p <= '9')) return 0;
        unsigned x = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (unsigned)(*p++ - '0'); if (++digits > 3) return 0; }
        if (x > 255) return 0;
        v[n] = x;
        if (n < 3) { if (*p != '.') return 0; p++; }
    }
    if (*p) return 0;
    if (ip) {
        uint8_t b[4] = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3] };
        memcpy(ip, b, 4);
    }
    return 1;
}

static char *trim_dup(const char *s, size_t n) {
    while (n && w_isspace((unsigned char)*s)) s++, n--;
    while (n && w_isspace((unsigned char)s[n - 1])) n--;
    return w_strndup(s, n);
}

static void cookie_set(web_session_t *s, const web_url_t *u, const char *hdr) {
    if (s->flags & WEB_SESS_NO_COOKIES) return;
    if (strlen(hdr) > MAX_COOKIE_BYTES) return;
    const char *semi = strchr(hdr, ';');
    size_t nvl = semi ? (size_t)(semi - hdr) : strlen(hdr);
    const char *eq = w_memchr(hdr, '=', nvl);
    if (!eq) return;
    cookie_t *k = calloc(1, sizeof *k);
    if (!k) return;
    k->name = trim_dup(hdr, (size_t)(eq - hdr));
    k->value = trim_dup(eq + 1, nvl - (size_t)(eq + 1 - hdr));
    long long now;
    long unc;
    wp_utc_now(&now, &unc);
    int del = 0;
    long long maxage = -1, expires = 0;
    char *domain = 0, *path = 0;
    while (semi) {
        const char *a = semi + 1;
        semi = strchr(a, ';');
        size_t al = semi ? (size_t)(semi - a) : strlen(a);
        const char *ae = w_memchr(a, '=', al);
        char *an = trim_dup(a, ae ? (size_t)(ae - a) : al);
        char *av = ae ? trim_dup(ae + 1, al - (size_t)(ae + 1 - a)) : w_strndup("", 0);
        if (an && av) {
            if (!w_strncasecmp(an, "domain", 7) && av[0]) { free(domain); domain = av; av = 0; }
            else if (!w_strncasecmp(an, "path", 5) && av[0] == '/') { free(path); path = av; av = 0; }
            else if (!w_strncasecmp(an, "secure", 7)) k->secure = 1;
            else if (!w_strncasecmp(an, "max-age", 8)) maxage = strtol(av, 0, 10);
            else if (!w_strncasecmp(an, "expires", 8)) { long long t = http_date(av); if (t >= 0) expires = t; }
        }
        free(an);
        free(av);
    }
    if (!k->name || !k->value || !k->name[0]) goto drop;
    if (maxage >= 0) { if (maxage == 0) del = 1; else expires = now + maxage; }
    else if (expires && expires <= now) del = 1;
    k->expires = expires;
    if (domain) {
        char *d = domain[0] == '.' ? domain + 1 : domain;
        for (char *q = d; *q; q++) *q = (char)w_tolower((unsigned char)*q);
        /* the host must be in the domain; no top-level domains, no other IPs */
        if (!domain_match(u->host, d) || !strchr(d, '.') || (is_ipv4(u->host, 0) && strcmp(u->host, d))) goto drop;
        k->domain = w_strndup(d, strlen(d));
    } else {
        k->domain = w_strndup(u->host, strlen(u->host));
        k->host_only = 1;
    }
    if (path) k->path = path, path = 0;
    else {
        const char *sl = strrchr(u->path, '/');
        size_t pl = sl && sl != u->path ? (size_t)(sl - u->path) : 1;
        k->path = w_strndup(u->path, pl);
    }
    if (!k->domain || !k->path) goto drop;
    wp_lock(&s->lock);
    cookie_t **pp = &s->cookies;
    while (*pp) {
        cookie_t *o = *pp;
        if (!strcmp(o->name, k->name) && !strcmp(o->domain, k->domain) && !strcmp(o->path, k->path)) {
            *pp = o->next;
            cookie_free(o);
            s->ncookies--;
            continue;
        }
        pp = &o->next;
    }
    if (!del && s->ncookies < MAX_COOKIES) {
        k->next = s->cookies;
        s->cookies = k;
        s->ncookies++;
        k = 0;
    }
    wp_unlock(&s->lock);
drop:
    free(domain);
    free(path);
    if (k) cookie_free(k);
}

/* "Cookie: ..." for a request, appended to out at pos; returns the new pos */
static size_t cookie_header(web_session_t *s, const web_url_t *u, char *out, size_t n, size_t pos) {
    if (s->flags & WEB_SESS_NO_COOKIES) return pos;
    long long now;
    long unc;
    wp_utc_now(&now, &unc);
    int https = !strcmp(u->scheme, "https"), first = 1;
    wp_lock(&s->lock);
    size_t start = pos;
    for (cookie_t *k = s->cookies; k; k = k->next) {
        if (k->expires && k->expires <= now) continue;
        if (k->secure && !https) continue;
        if (k->host_only ? strcmp(u->host, k->domain) : !domain_match(u->host, k->domain)) continue;
        if (!path_match(u->path, k->path)) continue;
        int w = w_snprintf(out + pos, n - pos, "%s%s=%s", first ? "Cookie: " : "; ", k->name, k->value);
        if (w < 0 || (size_t)w >= n - pos - 3) break;
        pos += (size_t)w;
        first = 0;
    }
    wp_unlock(&s->lock);
    if (pos > start) { memcpy(out + pos, "\r\n", 3); pos += 2; }
    return pos;
}

void web_session_clear_cookies(web_session_t *s) {
    wp_lock(&s->lock);
    while (s->cookies) { cookie_t *k = s->cookies; s->cookies = k->next; cookie_free(k); }
    s->ncookies = 0;
    wp_unlock(&s->lock);
}

/* ---------------- sessions ---------------- */
web_session_t *web_session_new(void) {
    return calloc(1, sizeof(web_session_t));
}

void web_session_set_flags(web_session_t *s, int flags) { s->flags = flags; }

void web_session_free(web_session_t *s) {
    if (!s) return;
    web_session_clear_cookies(s);
    for (int i = 0; i < POOL_MAX; i++) conn_free(s->pool[i], 1);
    tls_free_session(s);
    free(s);
}

static wconn_t *pool_take(web_session_t *s, const web_url_t *u, int tls) {
    wconn_t *found = 0;
    wp_lock(&s->lock);
    for (int i = 0; i < POOL_MAX; i++) {
        wconn_t *c = s->pool[i];
        if (!c) continue;
        if (wp_ms() - c->idle_since > POOL_IDLE_MS) { s->pool[i] = 0; conn_free(c, 0); continue; }
        if (!found && c->tls == tls && c->port == u->port && !strcmp(c->host, u->host)) { found = c; s->pool[i] = 0; }
    }
    wp_unlock(&s->lock);
    /* the server may have closed it meanwhile (then it is readable) */
    if (found && (found->bpos < found->blen || wp_poll(found->fd, WP_IN, 0) != 0)) { conn_free(found, 0); found = 0; }
    return found;
}

static void pool_put(web_session_t *s, wconn_t *c) {
    c->idle_since = wp_ms();
    c->cancel = 0;
    wp_lock(&s->lock);
    int slot = -1;
    for (int i = 0; i < POOL_MAX; i++) if (!s->pool[i]) { slot = i; break; }
    if (slot < 0) {               /* drop the oldest */
        slot = 0;
        for (int i = 1; i < POOL_MAX; i++) if (s->pool[i]->idle_since < s->pool[slot]->idle_since) slot = i;
        conn_free(s->pool[slot], 0);
    }
    s->pool[slot] = c;
    wp_unlock(&s->lock);
}

/* ---------------- one request ---------------- */
typedef struct {
    const web_request_t *rq;
    web_response_t *rs;
    int coding;                 /* 0 identity, 1 gzip, 2 deflate */
    int discard;                /* a redirect's body */
    uint8_t *buf;
    size_t len, cap, limit;
    long long got, total;
    int err;
} sink_t;

static void progress(const web_request_t *rq, const char *phase, long long got, long long total) {
    if (rq->on_progress) rq->on_progress(rq->ctx, phase, got, total);
}

static int sink_put(sink_t *k, const uint8_t *p, size_t n) {
    k->got += (long long)n;
    if (k->discard) return 0;
    if (!k->coding && k->rq->on_data) {
        if (k->got > (long long)k->limit) return WEB_E_TOOBIG;
        int r = k->rq->on_data(k->rq->ctx, p, n);
        return r < 0 ? r : 0;
    }
    if (n > k->limit || k->len > k->limit - n) return WEB_E_TOOBIG;
    if (k->len + n + 1 > k->cap) {
        size_t nc = k->cap ? k->cap : 16384;
        while (nc < k->len + n + 1) nc *= 2;
        uint8_t *q = realloc(k->buf, nc);
        if (!q) return E_NOMEM;
        k->buf = q;
        k->cap = nc;
    }
    memcpy(k->buf + k->len, p, n);
    k->len += n;
    k->buf[k->len] = 0;
    return 0;
}

static int sink_end(sink_t *k) {
    if (k->discard) return 0;
    uint8_t *out = k->buf;
    size_t outn = k->len;
    if (k->coding && k->len) {
        int r;
        if (k->coding == 1) r = inflate_gzip(k->buf, k->len, &out, &outn, k->limit);
        else if ((k->buf[0] & 0x0F) == 8 && k->len >= 2 && ((k->buf[0] << 8) | k->buf[1]) % 31 == 0)
            r = inflate_zlib(k->buf, k->len, &out, &outn, k->limit);
        else r = inflate_raw(k->buf, k->len, 0, &out, &outn, k->limit);  /* "deflate" without the zlib wrapper */
        free(k->buf);
        k->buf = 0;
        if (r == INF_E_LIMIT) return WEB_E_TOOBIG;
        if (r == INF_E_NOMEM) return E_NOMEM;
        if (r) return WEB_E_DECODE;
        if (k->rq->on_data) {
            int e = outn ? k->rq->on_data(k->rq->ctx, out, outn) : 0;
            free(out);
            return e < 0 ? e : 0;
        }
    }
    if (!out) { out = malloc(1); if (!out) return E_NOMEM; out[0] = 0; }
    k->rs->body = out;
    k->rs->body_len = outn;
    k->buf = 0;
    return 0;
}

/* header value helpers */
static int has_token(const char *v, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = v; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
        const char *semi = w_memchr(p, ';', (size_t)(t - p));
        if (semi) { t = semi; while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--; }
        if ((size_t)(t - p) == tl && !w_strncasecmp(p, tok, tl)) return 1;
        p = e;
    }
    return 0;
}

/* the last token of a list ("gzip, chunked" -> "chunked") */
static int last_token_is(const char *v, const char *tok) {
    const char *p = v + strlen(v);
    while (p > v && (p[-1] == ' ' || p[-1] == '\t')) p--;
    const char *e = p;
    while (p > v && p[-1] != ',') p--;
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    return (size_t)(e - p) == strlen(tok) && !w_strncasecmp(p, tok, strlen(tok));
}

int web_header(const web_response_t *rs, const char *name, char *out, size_t n) {
    if (!rs->headers) return -1;
    size_t nl = strlen(name);
    for (const char *p = rs->headers; *p; ) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > nl && p[nl] == ':' && !w_strncasecmp(p, name, nl)) {
            const char *v = p + nl + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            size_t l = (size_t)(e - v);
            if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(out, v, c); out[c] = 0; }
            return (int)l;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

static void content_type(web_response_t *rs, const char *v) {
    size_t i = 0;
    while (v[i] && v[i] != ';' && i + 1 < sizeof rs->content_type) {
        if (v[i] != ' ' && v[i] != '\t') rs->content_type[i] = (char)w_tolower((unsigned char)v[i]);
        i++;
    }
    rs->content_type[i] = 0;
    /* strip blanks that were skipped */
    char *d = rs->content_type, *q = rs->content_type;
    for (size_t k = 0; k < i; k++) if (q[k]) *d++ = q[k];
    *d = 0;
    const char *cs = 0;
    for (const char *p = v; *p; p++) if (!w_strncasecmp(p, "charset=", 8)) { cs = p + 8; break; }
    rs->charset[0] = 0;
    if (cs) {
        if (*cs == '"') cs++;
        size_t o = 0;
        while (cs[o] && cs[o] != '"' && cs[o] != ';' && cs[o] != ' ' && o + 1 < sizeof rs->charset) {
            rs->charset[o] = (char)w_tolower((unsigned char)cs[o]);
            o++;
        }
        rs->charset[o] = 0;
    }
}

static void disposition(web_response_t *rs, const char *v) {
    const char *f = 0;
    for (const char *p = v; *p; p++) if (!w_strncasecmp(p, "filename=", 9)) { f = p + 9; break; }
    if (!f) return;
    int q = *f == '"';
    if (q) f++;
    size_t o = 0;
    while (f[o] && (q ? f[o] != '"' : (f[o] != ';' && f[o] != ' ')) && o + 1 < sizeof rs->filename) {
        char c = f[o];
        rs->filename[o] = (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) ? '_' : c;
        o++;
    }
    rs->filename[o] = 0;
}

/* open a connection to u (resolve, connect, TLS) */
static wconn_t *conn_open(web_session_t *s, const web_url_t *u, const web_request_t *rq, int *err, char *why, size_t whyn) {
    wconn_t *c = calloc(1, sizeof *c);
    if (!c) { *err = E_NOMEM; return 0; }
    c->fd = -1;
    c->tls = !strcmp(u->scheme, "https");
    w_strlcpy(c->host, u->host, sizeof c->host);
    c->port = u->port;
    c->cancel = rq->cancel;
    c->idle_ms = rq->idle_timeout_ms > 0 ? rq->idle_timeout_ms : 30000;
    uint32_t ips[8];
    int nip;
    char phase[300];
    if (strchr(u->host, ':')) { *err = WEB_E_CONNECT; w_snprintf(why, whyn, "IPv6 addresses are not supported yet."); goto fail; }
    if (is_ipv4(u->host, &ips[0])) nip = 1;
    else {
        w_snprintf(phase, sizeof phase, "Looking up %s", u->host);
        progress(rq, phase, 0, -1);
        char dwhy[120] = "";
        nip = wp_resolve(u->host, ips, 8, 10000, dwhy, sizeof dwhy);
        if (canceled(c)) { *err = WEB_E_CANCELED; goto fail; }
        if (nip <= 0) {
            *err = nip < 0 ? nip : WEB_E_DNS;
            if (*err == WEB_E_NONET) w_snprintf(why, whyn, "There is no network connection: no DNS server is set up (the network cable may be unplugged, or no address was obtained).");
            else if (*err == WEB_E_TIMEOUT) w_snprintf(why, whyn, "Looking up %s took too long%s%s.", u->host, dwhy[0] ? ": " : "", dwhy);
            else w_snprintf(why, whyn, "The server name %s was not found%s%s.", u->host, dwhy[0] ? ": " : "", dwhy);
            goto fail;
        }
    }
    int last = WEB_E_CONNECT;
    char lastwhy[200] = "";
    int ct = rq->connect_timeout_ms > 0 ? rq->connect_timeout_ms : 15000;
    for (int i = 0; i < nip; i++) {
        if (canceled(c)) { *err = WEB_E_CANCELED; goto fail; }
        const uint8_t *b = (const uint8_t *)&ips[i];
        w_snprintf(phase, sizeof phase, "Connecting to %s (%u.%u.%u.%u)", u->host, b[0], b[1], b[2], b[3]);
        progress(rq, phase, 0, -1);
        int fd = wp_tcp_open();
        if (fd < 0) {
            if (fd == E_AFNOSUPPORT || fd == E_NOTSUP) {
                *err = WEB_E_NONET;
                w_snprintf(why, whyn, "There is no network connection (this computer has no internet sockets).");
                goto fail;
            }
            *err = fd;
            w_snprintf(why, whyn, "%s", hal_strerror(fd));
            goto fail;
        }
        c->fd = fd;
        c->err = 0;
        int r = wp_tcp_connect(fd, ips[i], u->port);
        if (r == E_INPROGRESS || r == E_AGAIN) {
            r = wait_ready(c, WP_OUT, ct, "The server did not answer");
            if (r >= 0) r = wp_so_error(fd);
            else r = c->err;
        }
        if (r == 0) { last = 0; break; }
        if (r == WEB_E_CANCELED) { *err = r; goto fail; }
        wp_close(fd);
        c->fd = -1;
        if (r == WEB_E_TIMEOUT || r == E_TIMEOUT) { last = WEB_E_TIMEOUT; w_snprintf(lastwhy, sizeof lastwhy, "%s:%d did not answer.", u->host, u->port); }
        else if (r == E_CONNREFUSED) { last = WEB_E_CONNECT; w_snprintf(lastwhy, sizeof lastwhy, "%s:%d refused the connection.", u->host, u->port); }
        else if (r == E_AFNOSUPPORT || r == E_NETUNREACH) { last = WEB_E_NONET; w_snprintf(lastwhy, sizeof lastwhy, "There is no network connection (no route to %s).", u->host); }
        else if (r == E_HOSTUNREACH) { last = WEB_E_CONNECT; w_snprintf(lastwhy, sizeof lastwhy, "%s could not be reached.", u->host); }
        else { last = WEB_E_CONNECT; w_snprintf(lastwhy, sizeof lastwhy, "%s:%d: %s.", u->host, u->port, hal_strerror(r)); }
    }
    if (last) { *err = last; w_strlcpy(why, lastwhy, whyn); goto fail; }
    c->err = 0;
    if (c->tls) {
        w_snprintf(phase, sizeof phase, "Securing the connection to %s", u->host);
        progress(rq, phase, 0, -1);
        int r = tls_start(s, c);
        if (r) { *err = r; w_strlcpy(why, c->why, whyn); goto fail; }
    }
    return c;
fail:
    if (c->tls && c->t) tls_close(c, 0);
    if (c->fd >= 0) wp_close(c->fd);
    free(c);
    return 0;
}

static int read_chunked(wconn_t *c, sink_t *k, char *line) {
    for (;;) {
        size_t ll;
        int r = read_line(c, line, 1024, &ll);
        if (r == 1) return WEB_E_TRUNCATED;
        if (r == WEB_E_HEADERS) return conn_fail(c, WEB_E_PROTOCOL, "Bad chunked encoding (chunk size line too long)");
        if (r) return r;
        unsigned long long size = 0;
        size_t i = 0, digits = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        for (; w_hexval((unsigned char)line[i]) >= 0; i++) {
            if (++digits > 15) return conn_fail(c, WEB_E_PROTOCOL, "Bad chunked encoding (chunk too large)");
            size = size * 16 + (unsigned)w_hexval((unsigned char)line[i]);
        }
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!digits || (line[i] && line[i] != ';')) return conn_fail(c, WEB_E_PROTOCOL, "Bad chunked encoding (not a chunk size)");
        if (size == 0) break;
        if (size > k->limit + 1) return WEB_E_TOOBIG;
        while (size) {
            long f = fill(c);
            if (f < 0) return (int)f;
            if (f == 0) return WEB_E_TRUNCATED;
            size_t take = (size_t)f < size ? (size_t)f : (size_t)size;
            r = sink_put(k, c->buf + c->bpos, take);
            if (r) return r;
            c->bpos += take;
            size -= take;
            progress(k->rq, "Receiving", k->got, k->total);
        }
        r = read_line(c, line, 1024, &ll);
        if (r == 1) return WEB_E_TRUNCATED;
        if (r) return r == WEB_E_HEADERS ? conn_fail(c, WEB_E_PROTOCOL, "Bad chunked encoding") : r;
        if (ll) return conn_fail(c, WEB_E_PROTOCOL, "Bad chunked encoding (no line end after a chunk)");
    }
    /* trailers, up to an empty line */
    for (int n = 0;; n++) {
        size_t ll;
        int r = read_line(c, line, MAX_LINE, &ll);
        if (r == 1) return 0;                       /* end of stream right after the last chunk: accept */
        if (r) return r;
        if (!ll) return 0;
        if (n > MAX_HEADER_LINES) return conn_fail(c, WEB_E_HEADERS, "Too many trailer lines");
    }
}

/* one exchange on c; *keep = the connection can be reused */
static int exchange(web_session_t *s, wconn_t *c, const web_url_t *u, const char *method, const web_request_t *rq,
                    web_response_t *rs, int *keep, int *got_any, int redirecting_ok) {
    *keep = 0;
    *got_any = 0;
    size_t hcap = 16384 + (rq->referer ? strlen(rq->referer) : 0);
    char *req = malloc(hcap);
    char *line = malloc(MAX_LINE);
    if (!req || !line) { free(req); free(line); return E_NOMEM; }
    int r = 0;
    char hostport[300];
    if (u->explicit_port) w_snprintf(hostport, sizeof hostport, "%s:%d", u->host, u->port);
    else w_strlcpy(hostport, u->host, sizeof hostport);
    int post = rq->body && !strcmp(method, "POST");
    size_t pos = (size_t)w_snprintf(req, hcap, "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " WEB_USER_AGENT "\r\n"
                                    "Accept: %s\r\nAccept-Language: en\r\n%s%s",
                                    method, u->path, u->has_query ? "?" : "", u->has_query ? u->query : "", hostport,
                                    rq->accept ? rq->accept : "*/*",
                                    rq->flags & WEB_REQ_NO_GZIP ? "" : "Accept-Encoding: gzip, deflate\r\n",
                                    rq->flags & WEB_REQ_NO_KEEPALIVE ? "Connection: close\r\n" : "");
    if (pos >= hcap) { r = WEB_E_URL; goto out; }
    if (rq->referer && rq->referer[0]) {
        int w = w_snprintf(req + pos, hcap - pos, "Referer: %s\r\n", rq->referer);
        if (w > 0 && (size_t)w < hcap - pos) pos += (size_t)w;
    }
    pos = cookie_header(s, u, req, hcap - 64, pos);
    if (post) {
        int w = w_snprintf(req + pos, hcap - pos, "Content-Type: %s\r\nContent-Length: %lu\r\n",
                           rq->content_type ? rq->content_type : "application/x-www-form-urlencoded", (unsigned long)rq->body_len);
        if (w < 0 || (size_t)w >= hcap - pos) { r = WEB_E_URL; goto out; }
        pos += (size_t)w;
    }
    memcpy(req + pos, "\r\n", 2);
    pos += 2;
    progress(rq, "Sending the request", 0, -1);
    r = conn_write(c, req, pos);
    if (!r && post && rq->body_len) r = conn_write(c, rq->body, rq->body_len);
    if (r) goto out;

    /* status line (skipping 1xx answers) and headers */
    char wait_phase[300];
    w_snprintf(wait_phase, sizeof wait_phase, "Waiting for %s", u->host);
    progress(rq, wait_phase, 0, -1);
    int minor = 1;
    for (int n1 = 0;; n1++) {
        size_t ll;
        r = read_line(c, line, MAX_LINE, &ll);
        if (r == 1) { r = c->err ? c->err : WEB_E_CLOSED; goto out; }
        if (r == WEB_E_HEADERS) { r = conn_fail(c, WEB_E_PROTOCOL, "The status line is too long"); goto out; }
        if (r) goto out;
        *got_any = 1;
        if (!ll && n1 < 4) { n1--; continue; }      /* stray empty lines before the status line */
        if (ll < 12 || strncmp(line, "HTTP/1.", 7) || !(line[7] >= '0' && line[7] <= '9') || line[8] != ' ' ||
            !(line[9] >= '1' && line[9] <= '5') || !(line[10] >= '0' && line[10] <= '9') || !(line[11] >= '0' && line[11] <= '9') ||
            (line[12] && line[12] != ' ')) {
            r = conn_fail(c, WEB_E_PROTOCOL, "The server's answer does not start with an HTTP status line");
            goto out;
        }
        minor = line[7] - '0';
        rs->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        w_strlcpy(rs->reason, line[12] ? line + 13 : "", sizeof rs->reason);
        /* headers */
        free(rs->headers);
        rs->headers = malloc(MAX_HEADER_BYTES + 2);
        if (!rs->headers) { r = E_NOMEM; goto out; }
        size_t hl = 0;
        int nlines = 0;
        rs->headers[0] = 0;
        for (;;) {
            r = read_line(c, line, MAX_LINE, &ll);
            if (r == 1) { r = WEB_E_TRUNCATED; goto out; }
            if (r == WEB_E_HEADERS) { r = conn_fail(c, WEB_E_HEADERS, "A header line is too long"); goto out; }
            if (r) goto out;
            if (!ll) break;
            if (++nlines > MAX_HEADER_LINES) { r = conn_fail(c, WEB_E_HEADERS, "Too many header lines"); goto out; }
            if (line[0] == ' ' || line[0] == '\t') {           /* obsolete line folding */
                if (!hl) continue;
                size_t i = 0;
                while (line[i] == ' ' || line[i] == '\t') i++;
                if (hl + 1 + (ll - i) + 1 > MAX_HEADER_BYTES) { r = conn_fail(c, WEB_E_HEADERS, "The headers are too large"); goto out; }
                hl--;                                          /* replace the '\n' */
                rs->headers[hl++] = ' ';
                memcpy(rs->headers + hl, line + i, ll - i);
                hl += ll - i;
                rs->headers[hl++] = '\n';
                rs->headers[hl] = 0;
                continue;
            }
            char *colon = w_memchr(line, ':', ll);
            if (!colon || colon == line) continue;             /* not a header: ignored */
            int bad = 0;
            for (char *q = line; q < colon; q++) if (*q <= ' ' || *q >= 127) bad = 1;
            if (bad) continue;
            if (hl + ll + 2 > MAX_HEADER_BYTES) { r = conn_fail(c, WEB_E_HEADERS, "The headers are too large"); goto out; }
            for (size_t i = 0; i < ll; i++) if (line[i] == 0) line[i] = ' ';
            memcpy(rs->headers + hl, line, ll);
            hl += ll;
            rs->headers[hl++] = '\n';
            rs->headers[hl] = 0;
        }
        if (rs->status >= 200 || n1 >= MAX_1XX) break;
        if (rs->status == 101) { r = conn_fail(c, WEB_E_PROTOCOL, "The server switched protocols"); goto out; }
    }
    if (rs->status < 200) { r = conn_fail(c, WEB_E_PROTOCOL, "Too many interim (1xx) answers"); goto out; }

    /* the headers that matter */
    char *v = malloc(MAX_LINE);
    if (!v) { r = E_NOMEM; goto out; }
    long long clen = -1;
    int chunked = 0, close_after = minor == 0, bad_len = 0, coding = 0;
    rs->content_type[0] = rs->charset[0] = 0;
    for (const char *p = rs->headers; *p; ) {
        const char *e = strchr(p, '\n');
        const char *col = w_memchr(p, ':', (size_t)(e - p));
        size_t nl = (size_t)(col - p), vl;
        const char *vs = col + 1;
        while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
        vl = (size_t)(e - vs);
        while (vl && (vs[vl - 1] == ' ' || vs[vl - 1] == '\t')) vl--;
        if (vl >= MAX_LINE) vl = MAX_LINE - 1;
        memcpy(v, vs, vl);
        v[vl] = 0;
#define IS(name) (nl == sizeof(name) - 1 && !w_strncasecmp(p, name, nl))
        if (IS("content-length")) {
            long long x = 0;
            size_t i = 0;
            for (; v[i] >= '0' && v[i] <= '9'; i++) { x = x * 10 + (v[i] - '0'); if (x > (1LL << 50)) { bad_len = 1; break; } }
            if (!i || v[i]) bad_len = 1;
            else if (clen >= 0 && clen != x) bad_len = 1;
            else clen = x;
        } else if (IS("transfer-encoding")) {
            if (last_token_is(v, "chunked")) chunked = 1;
            else if (!has_token(v, "identity")) close_after = 1;   /* unknown: read until the end */
        } else if (IS("connection")) {
            if (has_token(v, "close")) close_after = 1;
            else if (minor == 0 && has_token(v, "keep-alive")) close_after = 0;
        } else if (IS("content-encoding")) {
            if (has_token(v, "gzip") || has_token(v, "x-gzip")) coding = 1;
            else if (has_token(v, "deflate")) coding = 2;
        } else if (IS("content-type")) {
            content_type(rs, v);
        } else if (IS("content-disposition")) {
            disposition(rs, v);
        } else if (IS("set-cookie")) {
            cookie_set(s, u, v);
        }
#undef IS
        p = e + 1;
    }
    free(v);
    if (chunked) clen = -1;
    if (bad_len && !chunked) { r = conn_fail(c, WEB_E_PROTOCOL, "The answer has a bad Content-Length"); goto out; }
    rs->content_length = clen;

    int nobody = !strcmp(method, "HEAD") || rs->status == 204 || rs->status == 304;
    sink_t k;
    memset(&k, 0, sizeof k);
    k.rq = rq;
    k.rs = rs;
    k.coding = nobody ? 0 : coding;
    k.limit = rq->max_body ? rq->max_body : DEF_MAX_BODY;
    k.total = clen;
    int is_redirect = (rs->status == 301 || rs->status == 302 || rs->status == 303 || rs->status == 307 || rs->status == 308) &&
                      redirecting_ok && web_header(rs, "location", 0, 0) > 0;
    if (is_redirect) { k.discard = 1; k.coding = 0; }
    if (is_redirect && (chunked || clen < 0 || clen > 65536)) {
        /* a redirect's body is not wanted: rather than read a long one, close */
        r = 0;
        close_after = 1;
    } else if (nobody) {
        r = 0;
    } else if (chunked) {
        r = read_chunked(c, &k, line);
    } else if (clen >= 0) {
        if (!k.discard && clen > (long long)k.limit) { r = WEB_E_TOOBIG; goto out_sink; }
        long long left = clen;
        while (left > 0) {
            long f = fill(c);
            if (f < 0) { r = (int)f; break; }
            if (f == 0) { r = WEB_E_TRUNCATED; break; }
            size_t take = (long long)f < left ? (size_t)f : (size_t)left;
            r = sink_put(&k, c->buf + c->bpos, take);
            c->bpos += take;
            left -= (long long)take;
            if (r) break;
            progress(rq, "Receiving", k.got, k.total);
        }
    } else {
        close_after = 1;
        for (;;) {
            long f = fill(c);
            if (f < 0) { r = (int)f; break; }
            if (f == 0) break;
            r = sink_put(&k, c->buf + c->bpos, (size_t)f);
            c->bpos += (size_t)f;
            if (r) break;
            progress(rq, "Receiving", k.got, k.total);
        }
    }
    if (!r) r = sink_end(&k);
    if (!r && !close_after && !(rq->flags & WEB_REQ_NO_KEEPALIVE) && c->bpos == c->blen) *keep = 1;
out_sink:
    free(k.buf);
out:
    free(req);
    free(line);
    return r;
}

static void clear_response(web_response_t *rs) {
    free(rs->headers);
    free(rs->body);
    rs->headers = 0;
    rs->body = 0;
    rs->body_len = 0;
    rs->status = 0;
    rs->reason[0] = 0;
    rs->content_type[0] = rs->charset[0] = rs->filename[0] = 0;
    rs->content_length = -1;
    rs->tls_info[0] = 0;
}

int web_fetch(web_session_t *s, const web_request_t *rq, web_response_t *rs) {
    memset(rs, 0, sizeof *rs);
    rs->content_length = -1;
    web_url_t *u = malloc(sizeof *u);
    char *cur = malloc(URL_MAX);
    if (!u || !cur) { free(u); free(cur); return rs->error = E_NOMEM; }
    w_strlcpy(cur, rq->url ? rq->url : "", URL_MAX);
    const char *method = rq->method && rq->method[0] ? rq->method : "GET";
    int maxr = rq->max_redirects == 0 ? DEF_REDIRECTS : rq->max_redirects;
    web_request_t r2 = *rq;           /* the body is dropped when a redirect turns POST into GET */
    int err = 0;
    for (;;) {
        clear_response(rs);
        if (url_parse(cur, u)) { err = WEB_E_URL; w_snprintf(rs->error_text, sizeof rs->error_text, "\"%s\" is not a valid address.", cur); break; }
        url_format(u, 0, rs->url, sizeof rs->url);
        if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) {
            err = WEB_E_SCHEME;
            w_snprintf(rs->error_text, sizeof rs->error_text, "Addresses starting with \"%s:\" cannot be fetched.", u->scheme);
            break;
        }
        int tls = !strcmp(u->scheme, "https");
        int post = !strcmp(method, "POST");
        int keep = 0, got_any = 0;
        int attempt;
        for (attempt = 0; attempt < 2; attempt++) {
            wconn_t *c = post || attempt ? 0 : pool_take(s, u, tls);
            int pooled = c != 0;
            if (!c) {
                c = conn_open(s, u, &r2, &err, rs->error_text, sizeof rs->error_text);
                if (!c) break;
            } else {
                c->cancel = rq->cancel;
                c->idle_ms = rq->idle_timeout_ms > 0 ? rq->idle_timeout_ms : 30000;
                c->err = 0;
                c->why[0] = 0;
            }
            rs->tls = tls;
            if (tls) tls_info(c, rs->tls_info, sizeof rs->tls_info);
            err = exchange(s, c, u, method, &r2, rs, &keep, &got_any, maxr > 0);
            if (err && c->why[0] && !rs->error_text[0]) w_strlcpy(rs->error_text, c->why, sizeof rs->error_text);
            if (!err && keep) pool_put(s, c);
            else conn_free(c, !err);
            /* a kept-alive connection the server had closed: once more on a new one */
            if (err && pooled && !got_any && err != WEB_E_CANCELED) { err = 0; rs->error_text[0] = 0; clear_response(rs); continue; }
            break;
        }
        if (err) break;
        int st = rs->status;
        char loc[URL_MAX];
        if (maxr > 0 && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) &&
            web_header(rs, "location", loc, sizeof loc) > 0) {
            if (rs->redirects >= maxr) {
                err = WEB_E_REDIRECTS;
                w_snprintf(rs->error_text, sizeof rs->error_text, "The server redirected more than %d times (a redirect loop?).", maxr);
                break;
            }
            char *next = malloc(URL_MAX);
            if (!next) { err = E_NOMEM; break; }
            if (url_resolve(cur, loc, next, URL_MAX)) {
                free(next);
                err = WEB_E_PROTOCOL;
                w_snprintf(rs->error_text, sizeof rs->error_text, "The server redirected to an invalid address.");
                break;
            }
            w_strlcpy(cur, next, URL_MAX);
            free(next);
            int redirects = rs->redirects + 1;
            rs->redirects = redirects;
            if (st == 303 || ((st == 301 || st == 302) && post)) {
                method = "GET";
                r2.body = 0;
                r2.body_len = 0;
            }
            continue;
        }
        break;
    }
    rs->error = err;
    if (err && !rs->error_text[0]) w_strlcpy(rs->error_text, web_strerror(err), sizeof rs->error_text);
    free(u);
    free(cur);
    return err;
}

void web_response_free(web_response_t *rs) {
    free(rs->headers);
    free(rs->body);
    rs->headers = 0;
    rs->body = 0;
}
