/* libweb: HTTPS through BearSSL (userland/third_party/bearssl).
 *
 * - TLS 1.2 only (BearSSL has no TLS 1.3; TLS 1.0 / 1.1 are deprecated),
 *   forward-secret ECDHE suites first, no 3DES, no static ECDH.
 * - Server name: sent as SNI and checked against the certificate (BearSSL's
 *   minimal X.509 engine: chain to a trust anchor, signatures, names
 *   including a leading wildcard, validity dates, basic constraints, key
 *   usage). A rejected certificate fails the request with WEB_E_CERT and a
 *   reason; there is no way to "continue anyway".
 * - Entropy: 32 bytes from getrandom() per connection.
 * - Time: wp_utc_now(). When the clock's time zone is unknown (Halcyon's
 *   real-time clock keeps local time), two validations run side by side at
 *   the two ends of the window the true time lies in, and the chain is
 *   accepted if it is valid at either end. See docs/DESIGN-M6-WEB.md.
 * - Session resumption per host and port (TLS_CACHE_MAX entries). */
#include "webint.h"

#define EE_MAX 16384                /* the end-entity certificate is kept for error messages up to this size */

typedef struct {
    const br_x509_class *vtable;
    br_x509_minimal_context a, b;
    int two, which;
    unsigned err_a, err_b;
    int ncert;
    unsigned char *ee;
    size_t eelen, eecap;
} x509_dual_t;

struct tls_state {
    br_ssl_client_context cc;
    x509_dual_t x;
    br_sslio_context io;
    long long now;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* ---------------- the two-time X.509 wrapper ---------------- */
static void xd_start_chain(const br_x509_class **ctx, const char *name) {
    x509_dual_t *d = (x509_dual_t *)(void *)ctx;
    d->a.vtable->start_chain(&d->a.vtable, name);
    if (d->two) d->b.vtable->start_chain(&d->b.vtable, name);
    d->ncert = 0;
    d->eelen = 0;
    d->which = 0;
}
static void xd_start_cert(const br_x509_class **ctx, uint32_t len) {
    x509_dual_t *d = (x509_dual_t *)(void *)ctx;
    d->a.vtable->start_cert(&d->a.vtable, len);
    if (d->two) d->b.vtable->start_cert(&d->b.vtable, len);
    if (++d->ncert == 1 && len <= EE_MAX) {
        free(d->ee);
        d->ee = malloc(len ? len : 1);
        d->eecap = d->ee ? len : 0;
        d->eelen = 0;
    }
}
static void xd_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    x509_dual_t *d = (x509_dual_t *)(void *)ctx;
    d->a.vtable->append(&d->a.vtable, buf, len);
    if (d->two) d->b.vtable->append(&d->b.vtable, buf, len);
    if (d->ncert == 1 && d->ee && d->eelen + len <= d->eecap) {
        memcpy(d->ee + d->eelen, buf, len);
        d->eelen += len;
    }
}
static void xd_end_cert(const br_x509_class **ctx) {
    x509_dual_t *d = (x509_dual_t *)(void *)ctx;
    d->a.vtable->end_cert(&d->a.vtable);
    if (d->two) d->b.vtable->end_cert(&d->b.vtable);
}
static unsigned xd_end_chain(const br_x509_class **ctx) {
    x509_dual_t *d = (x509_dual_t *)(void *)ctx;
    d->err_a = d->a.vtable->end_chain(&d->a.vtable);
    d->err_b = d->two ? d->b.vtable->end_chain(&d->b.vtable) : d->err_a;
    if (!d->err_a) { d->which = 0; return 0; }
    if (!d->err_b) { d->which = 1; return 0; }
    /* both failed: a reason other than the dates is the real one */
    if (d->err_a == BR_ERR_X509_EXPIRED && d->err_b != BR_ERR_X509_EXPIRED) return d->err_b;
    return d->err_a;
}
static const br_x509_pkey *xd_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    const x509_dual_t *d = (const x509_dual_t *)(const void *)ctx;
    if (d->which) return d->b.vtable->get_pkey(&d->b.vtable, usages);
    return d->a.vtable->get_pkey(&d->a.vtable, usages);
}
static const br_x509_class xd_vtable = {
    sizeof(x509_dual_t), xd_start_chain, xd_start_cert, xd_append, xd_end_cert, xd_end_chain, xd_get_pkey
};

/* ---------------- a little DER, for error messages only ---------------- */
typedef struct { const unsigned char *p; size_t n; } der_t;

/* next element of d: tag and contents; 0 or -1 */
static int der_next(der_t *d, int *tag, der_t *val) {
    if (d->n < 2) return -1;
    int t = d->p[0];
    size_t len = d->p[1], hl = 2;
    if (len & 0x80) {
        size_t k = len & 0x7F;
        if (k == 0 || k > 3 || d->n < 2 + k) return -1;
        len = 0;
        for (size_t i = 0; i < k; i++) len = len << 8 | d->p[2 + i];
        hl = 2 + k;
    }
    if (len > d->n - hl) return -1;
    *tag = t;
    val->p = d->p + hl;
    val->n = len;
    d->p += hl + len;
    d->n -= hl + len;
    return 0;
}

static const unsigned char OID_CN[] = { 0x55, 0x04, 0x03 };
static const unsigned char OID_SAN[] = { 0x55, 0x1D, 0x11 };

static void der_text(der_t v, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < v.n && o + 1 < n; i++) {
        unsigned char c = v.p[i];
        out[o++] = (char)(c >= 0x20 && c != 0x7F ? c : '?');
    }
    if (n) out[o] = 0;
}

/* the common name of a Name */
static void name_cn(der_t name, char *out, size_t n) {
    int t;
    der_t rdn, atv;
    out[0] = 0;
    while (!der_next(&name, &t, &rdn)) {
        while (!der_next(&rdn, &t, &atv)) {
            der_t oid, val;
            int ot, vt;
            if (der_next(&atv, &ot, &oid) || der_next(&atv, &vt, &val)) break;
            if (ot == 6 && oid.n == 3 && !memcmp(oid.p, OID_CN, 3)) der_text(val, out, n);
        }
    }
}

/* YYMMDDhhmmssZ / YYYYMMDDhhmmssZ -> "YYYY-MM-DD" and seconds since 1970 */
static long long der_time(int tag, der_t v, char *out, size_t n) {
    int y, i = 0;
    if (out && n) out[0] = 0;
    if (tag == 0x17 && v.n >= 12) { y = (v.p[0] - '0') * 10 + (v.p[1] - '0'); y += y < 50 ? 2000 : 1900; i = 2; }
    else if (tag == 0x18 && v.n >= 14) { y = 0; for (; i < 4; i++) y = y * 10 + (v.p[i] - '0'); }
    else return 0;
    int f[5];
    for (int k = 0; k < 5; k++, i += 2) f[k] = (v.p[i] - '0') * 10 + (v.p[i + 1] - '0');
    if (out) w_snprintf(out, n, "%04d-%02d-%02d", y, f[0], f[1]);
    /* days from civil */
    int m = f[0], d = f[1];
    int yy = y - (m <= 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + f[2] * 3600 + f[3] * 60 + f[4];
}

typedef struct {
    char subject[96], issuer[96];
    int self_signed;
    char not_before[16], not_after[16];
    long long nb, na;
    char names[160];            /* subjectAltName DNS names */
} certinfo_t;

static int cert_info(const unsigned char *p, size_t n, certinfo_t *ci) {
    memset(ci, 0, sizeof *ci);
    der_t d = { p, n }, cert, tbs, x;
    int t;
    if (der_next(&d, &t, &cert) || t != 0x30 || der_next(&cert, &t, &tbs) || t != 0x30) return -1;
    if (der_next(&tbs, &t, &x)) return -1;
    if (t == 0xA0 && der_next(&tbs, &t, &x)) return -1;      /* version, then serial */
    der_t sig, issuer, validity, subject;
    if (der_next(&tbs, &t, &sig) || der_next(&tbs, &t, &issuer) || der_next(&tbs, &t, &validity) ||
        der_next(&tbs, &t, &subject)) return -1;
    name_cn(issuer, ci->issuer, sizeof ci->issuer);
    name_cn(subject, ci->subject, sizeof ci->subject);
    ci->self_signed = issuer.n == subject.n && !memcmp(issuer.p, subject.p, issuer.n);
    der_t tv;
    int tt;
    if (!der_next(&validity, &tt, &tv)) ci->nb = der_time(tt, tv, ci->not_before, sizeof ci->not_before);
    if (!der_next(&validity, &tt, &tv)) ci->na = der_time(tt, tv, ci->not_after, sizeof ci->not_after);
    /* extensions: [3] { SEQUENCE OF Extension } */
    der_t e;
    while (!der_next(&tbs, &t, &e)) {
        if (t != 0xA3) continue;
        der_t exts, ext;
        if (der_next(&e, &t, &exts)) break;
        while (!der_next(&exts, &t, &ext)) {
            der_t oid, v;
            int vt;
            if (der_next(&ext, &t, &oid) || t != 6) break;
            if (der_next(&ext, &vt, &v)) break;
            if (vt == 1 && der_next(&ext, &vt, &v)) break;   /* critical */
            if (oid.n != 3 || memcmp(oid.p, OID_SAN, 3) || vt != 4) continue;
            der_t names, gn;
            if (der_next(&v, &t, &names)) break;
            while (!der_next(&names, &t, &gn)) {
                if (t != 0x82) continue;
                char nm[80];
                der_text(gn, nm, sizeof nm);
                if (strlen(ci->names) + strlen(nm) + 3 >= sizeof ci->names) { w_strlcat(ci->names, ", ...", sizeof ci->names); break; }
                if (ci->names[0]) w_strlcat(ci->names, ", ", sizeof ci->names);
                w_strlcat(ci->names, nm, sizeof ci->names);
            }
        }
    }
    return 0;
}

static void cert_reason(tls_state_t *t, unsigned err, const char *host, char *out, size_t n) {
    certinfo_t ci;
    int have = t->x.ee && t->x.eelen && !cert_info(t->x.ee, t->x.eelen, &ci);
    switch (err) {
    case BR_ERR_X509_EXPIRED:
        if (have && ci.na && t->now > ci.na)
            w_snprintf(out, n, "The certificate of %s expired on %s.", host, ci.not_after);
        else if (have && ci.nb && t->now < ci.nb)
            w_snprintf(out, n, "The certificate of %s is not valid before %s.", host, ci.not_before);
        else
            w_snprintf(out, n, "A certificate of %s has expired or is not yet valid (check the computer's clock).", host);
        break;
    case BR_ERR_X509_BAD_SERVER_NAME:
        if (have) w_snprintf(out, n, "The certificate is not for %s: it is for %s.", host, ci.names[0] ? ci.names : ci.subject);
        else w_snprintf(out, n, "The certificate is not for %s.", host);
        break;
    case BR_ERR_X509_NOT_TRUSTED:
        if (have && ci.self_signed) w_snprintf(out, n, "The certificate of %s is self-signed: nobody this browser trusts vouches for it.", host);
        else if (have) w_snprintf(out, n, "The certificate of %s was issued by \"%s\", which is not a trusted certificate authority.", host, ci.issuer);
        else w_snprintf(out, n, "The certificate of %s is not from a trusted certificate authority.", host);
        break;
    case BR_ERR_X509_NOT_CA: w_snprintf(out, n, "A certificate in the chain of %s that signs another is not a certificate authority.", host); break;
    case BR_ERR_X509_BAD_SIGNATURE: w_snprintf(out, n, "A signature in the certificate chain of %s is not valid.", host); break;
    case BR_ERR_X509_WEAK_PUBLIC_KEY: w_snprintf(out, n, "The certificate of %s has a key that is too weak.", host); break;
    case BR_ERR_X509_DN_MISMATCH: w_snprintf(out, n, "The certificate chain of %s is incomplete or out of order.", host); break;
    case BR_ERR_X509_EMPTY_CHAIN: w_snprintf(out, n, "%s sent no certificate.", host); break;
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: w_snprintf(out, n, "The certificate of %s may not be used for this.", host); break;
    case BR_ERR_X509_CRITICAL_EXTENSION:
    case BR_ERR_X509_UNSUPPORTED: w_snprintf(out, n, "The certificate of %s uses a feature this browser does not support.", host); break;
    case BR_ERR_X509_TIME_UNKNOWN: w_snprintf(out, n, "The time is not known, so the certificate cannot be checked."); break;
    default: w_snprintf(out, n, "The certificate of %s is not valid (BearSSL error %u).", host, err); break;
    }
}

static void tls_reason(unsigned err, char *out, size_t n) {
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT) {
        unsigned a = err - BR_ERR_RECV_FATAL_ALERT;
        if (a == 70)
            w_snprintf(out, n, "The server does not speak TLS 1.2 (it probably requires TLS 1.3, which this browser does not have).");
        else if (a == 40)
            w_snprintf(out, n, "The server and this browser have no TLS version or cipher in common (the server may require TLS 1.3).");
        else if (a == 112)
            w_snprintf(out, n, "The server does not know the requested host name (TLS alert 112).");
        else
            w_snprintf(out, n, "The server ended the secure connection (TLS alert %u).", a);
        return;
    }
    if (err >= BR_ERR_SEND_FATAL_ALERT) { w_snprintf(out, n, "The secure connection failed (sent TLS alert %u).", err - BR_ERR_SEND_FATAL_ALERT); return; }
    switch (err) {
    case BR_ERR_BAD_VERSION: w_snprintf(out, n, "The server chose a TLS version other than 1.2."); break;
    case BR_ERR_BAD_CIPHER_SUITE: w_snprintf(out, n, "The server chose a cipher this browser did not offer."); break;
    case BR_ERR_UNSUPPORTED_VERSION: w_snprintf(out, n, "The server speaks an unsupported TLS version."); break;
    case BR_ERR_BAD_MAC: w_snprintf(out, n, "Data on the secure connection was corrupted."); break;
    case BR_ERR_IO: w_snprintf(out, n, "The connection failed during the secure handshake."); break;
    default: w_snprintf(out, n, "The secure connection failed (BearSSL error %u).", err); break;
    }
}

/* ---------------- low-level I/O for BearSSL ---------------- */
static int low_read(void *ctx, unsigned char *buf, size_t len) {
    wconn_t *c = ctx;
    long r = conn_raw_read(c, buf, len);
    if (r > 0) return (int)r;
    return -1;                       /* end (c->eof) or error (c->err) */
}
static int low_write(void *ctx, const unsigned char *buf, size_t len) {
    wconn_t *c = ctx;
    if (conn_raw_write(c, buf, len)) return -1;
    return (int)len;
}

/* trust anchors: built-in, plus the session's extra ones */
static const br_x509_trust_anchor *anchors(web_session_t *s, size_t *n) {
    if (!s->nextra) { *n = web_trust_anchors_count; return web_trust_anchors; }
    if (!s->all) {
        size_t m = web_trust_anchors_count + (size_t)s->nextra;
        s->all = malloc(m * sizeof *s->all);
        if (!s->all) { *n = web_trust_anchors_count; return web_trust_anchors; }
        memcpy(s->all, web_trust_anchors, web_trust_anchors_count * sizeof *s->all);
        memcpy(s->all + web_trust_anchors_count, s->extra, (size_t)s->nextra * sizeof *s->all);
        s->nall = (int)m;
    }
    *n = (size_t)s->nall;
    return s->all;
}

static const uint16_t suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
    BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_RSA_WITH_AES_256_CBC_SHA256,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_RSA_WITH_AES_256_CBC_SHA,
};

static void set_time(br_x509_minimal_context *x, long long t) {
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    br_x509_minimal_set_time(x, (uint32_t)(days + 719528), (uint32_t)secs);
}

int tls_start(web_session_t *s, wconn_t *c) {
    tls_state_t *t = calloc(1, sizeof *t);
    if (!t) return conn_fail(c, E_NOMEM, "Out of memory");
    c->t = t;
    size_t nta;
    wp_lock(&s->lock);
    const br_x509_trust_anchor *ta = anchors(s, &nta);
    wp_unlock(&s->lock);
    br_ssl_client_init_full(&t->cc, &t->x.a, ta, nta);
    br_ssl_engine_set_versions(&t->cc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&t->cc.eng, suites, sizeof suites / sizeof suites[0]);
    long uncertain;
    wp_utc_now(&t->now, &uncertain);
    t->x.vtable = &xd_vtable;
    if (uncertain) {
        br_x509_minimal_init_full(&t->x.b, ta, nta);
        set_time(&t->x.a, t->now - uncertain);
        set_time(&t->x.b, t->now + uncertain);
        t->x.two = 1;
    } else {
        set_time(&t->x.a, t->now);
    }
    br_ssl_engine_set_x509(&t->cc.eng, &t->x.vtable);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof t->iobuf, 1);
    unsigned char seed[32];
    if (wp_random(seed, sizeof seed)) return conn_fail(c, WEB_E_TLS, "No random numbers for the secure connection");
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    /* resume a session with this host if there is one */
    int resume = 0;
    wp_lock(&s->lock);
    for (int i = 0; i < TLS_CACHE_MAX; i++) {
        tls_cache_t *e = &s->cache[i];
        if (e->valid && e->port == c->port && !strcmp(e->host, c->host)) {
            br_ssl_engine_set_session_parameters(&t->cc.eng, &e->p);
            resume = 1;
            break;
        }
    }
    wp_unlock(&s->lock);
    /* the name is always checked; an IP address goes in as the name too
     * (it then has to match the certificate, which it rarely does) */
    if (!br_ssl_client_reset(&t->cc, c->host, resume)) return conn_fail(c, WEB_E_TLS, "The secure connection could not be set up");
    br_sslio_init(&t->io, &t->cc.eng, low_read, c, low_write, c);
    if (br_sslio_flush(&t->io) < 0 || br_ssl_engine_current_state(&t->cc.eng) == BR_SSL_CLOSED) {
        unsigned err = (unsigned)br_ssl_engine_last_error(&t->cc.eng);
        char why[200];
        if (err >= 32 && err < 64) {
            cert_reason(t, err, c->host, why, sizeof why);
            return conn_fail(c, WEB_E_CERT, why);
        }
        if (err) { tls_reason(err, why, sizeof why); return conn_fail(c, WEB_E_TLS, why); }
        if (c->err) return c->err;
        return conn_fail(c, WEB_E_TLS, "The server closed the connection during the secure handshake (it may require TLS 1.3).");
    }
    /* keep the session for next time */
    wp_lock(&s->lock);
    int slot = -1;
    for (int i = 0; i < TLS_CACHE_MAX; i++)
        if (s->cache[i].valid && s->cache[i].port == c->port && !strcmp(s->cache[i].host, c->host)) slot = i;
    if (slot < 0) { slot = s->cache_next; s->cache_next = (s->cache_next + 1) % TLS_CACHE_MAX; }
    tls_cache_t *e = &s->cache[slot];
    w_strlcpy(e->host, c->host, sizeof e->host);
    e->port = c->port;
    br_ssl_engine_get_session_parameters(&t->cc.eng, &e->p);
    e->valid = 1;
    wp_unlock(&s->lock);
    free(t->x.ee);
    t->x.ee = 0;
    return 0;
}

long tls_read(wconn_t *c, void *b, size_t n) {
    tls_state_t *t = c->t;
    int r = br_sslio_read(&t->io, b, n);
    if (r > 0) return r;
    unsigned err = (unsigned)br_ssl_engine_last_error(&t->cc.eng);
    /* close_notify, or the end of the TCP stream without one (many servers
     * do that; the HTTP framing tells a truncated answer apart) */
    if (!err && br_ssl_engine_current_state(&t->cc.eng) == BR_SSL_CLOSED) return 0;
    if ((!err || err == BR_ERR_IO) && c->eof && !c->err) return 0;
    if (c->err) return c->err;
    char why[200];
    tls_reason(err, why, sizeof why);
    return conn_fail(c, WEB_E_TLS, why);
}

int tls_write(wconn_t *c, const void *b, size_t n) {
    tls_state_t *t = c->t;
    if (br_sslio_write_all(&t->io, b, n) < 0 || br_sslio_flush(&t->io) < 0) {
        if (c->err) return c->err;
        unsigned err = (unsigned)br_ssl_engine_last_error(&t->cc.eng);
        char why[200];
        tls_reason(err, why, sizeof why);
        return conn_fail(c, WEB_E_TLS, why);
    }
    return 0;
}

void tls_close(wconn_t *c, int notify) {
    tls_state_t *t = c->t;
    if (!t) return;
    if (notify && !c->err && !c->eof) {
        /* a close_notify, without waiting long for the answer */
        int old = c->idle_ms;
        c->idle_ms = 200;
        br_ssl_engine_close(&t->cc.eng);
        br_sslio_flush(&t->io);
        c->idle_ms = old;
    }
    free(t->x.ee);
    memset(t, 0, sizeof *t);
    free(t);
    c->t = 0;
}

static const char *suite_name(unsigned s) {
    switch (s) {
    case BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-RSA-CHACHA20-POLY1305";
    case BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: return "ECDHE-RSA-AES128-GCM-SHA256";
    case BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: return "ECDHE-RSA-AES256-GCM-SHA384";
    case BR_TLS_RSA_WITH_AES_128_GCM_SHA256: return "RSA-AES128-GCM-SHA256";
    case BR_TLS_RSA_WITH_AES_256_GCM_SHA384: return "RSA-AES256-GCM-SHA384";
    }
    return "CBC suite";
}

void tls_info(wconn_t *c, char *out, size_t n) {
    if (!c->t) { if (n) out[0] = 0; return; }
    br_ssl_session_parameters p;
    br_ssl_engine_get_session_parameters(&c->t->cc.eng, &p);
    w_snprintf(out, n, "TLS 1.2, %s", suite_name(p.cipher_suite));
}

void tls_free_session(web_session_t *s) {
    for (int i = 0; i < s->nextra; i++) free(s->extra[i].dn.data);   /* one block per anchor (der_to_ta) */
    free(s->extra);
    free(s->all);
    s->extra = 0;
    s->all = 0;
    s->nextra = s->nall = 0;
}

/* ---------------- extra trust anchors from PEM (tests) ---------------- */
typedef struct { unsigned char *p; size_t n, cap; int over; } bufacc_t;
static void acc_append(void *ctx, const void *buf, size_t len) {
    bufacc_t *a = ctx;
    if (a->over) return;
    if (a->n + len > a->cap) {
        size_t nc = (a->cap ? a->cap * 2 : 4096);
        while (nc < a->n + len) nc *= 2;
        if (nc > (1 << 20)) { a->over = 1; return; }
        unsigned char *q = realloc(a->p, nc);
        if (!q) { a->over = 1; return; }
        a->p = q;
        a->cap = nc;
    }
    memcpy(a->p + a->n, buf, len);
    a->n += len;
}

/* one DER certificate -> trust anchor (data in one malloc()ed block hung off dn.data) */
static int der_to_ta(const unsigned char *der, size_t len, br_x509_trust_anchor *ta) {
    br_x509_decoder_context dc;
    bufacc_t dn = { 0, 0, 0, 0 };
    br_x509_decoder_init(&dc, acc_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.over || br_x509_decoder_last_error(&dc)) { free(dn.p); return -1; }
    size_t klen = pk->key_type == BR_KEYTYPE_RSA ? pk->key.rsa.nlen + pk->key.rsa.elen : pk->key.ec.qlen;
    unsigned char *blk = malloc(dn.n + klen + 1);
    if (!blk) { free(dn.p); return -1; }
    memcpy(blk, dn.p, dn.n);
    free(dn.p);
    memset(ta, 0, sizeof *ta);
    ta->dn.data = blk;
    ta->dn.len = dn.n;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    unsigned char *k = blk + dn.n;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        memcpy(k, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(k + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.n = k;
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = k + pk->key.rsa.nlen;
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else {
        memcpy(k, pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = k;
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    }
    return 0;
}

int web_session_add_ca_pem(web_session_t *s, const char *pem, size_t len) {
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    bufacc_t der = { 0, 0, 0, 0 };
    int added = 0, in_cert = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t k = br_pem_decoder_push(&pc, pem + pos, len - pos);
        pos += k;
        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE");
            der.n = 0;
            der.over = 0;
            br_pem_decoder_setdest(&pc, in_cert ? acc_append : 0, &der);
        } else if (ev == BR_PEM_END_OBJ) {
            if (in_cert && !der.over) {
                br_x509_trust_anchor ta;
                if (!der_to_ta(der.p, der.n, &ta)) {
                    wp_lock(&s->lock);
                    br_x509_trust_anchor *n = realloc(s->extra, (size_t)(s->nextra + 1) * sizeof *n);
                    if (n) { s->extra = n; s->extra[s->nextra++] = ta; added++; free(s->all); s->all = 0; }
                    else free(ta.dn.data);
                    wp_unlock(&s->lock);
                }
            }
            in_cert = 0;
        } else if (ev == BR_PEM_ERROR || !k) {
            break;
        }
    }
    free(der.p);
    return added;
}
