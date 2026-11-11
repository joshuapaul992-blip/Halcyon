/* WEBTEST.EXE - checks the web library and BearSSL as built for Halcyon
 * (the QEMU "web" boot test variant runs it): TLS 1.2 handshakes between a
 * BearSSL client and server inside this program, over memory, with RSA and
 * EC certificates of a test CA and several cipher suites (key exchange,
 * signatures, AES-GCM, AES-CBC and ChaCha20-Poly1305 all computed here),
 * certificate rejections (wrong name, expired), inflate, URLs, the clock,
 * and what a fetch does on this system's network (today: no network). The
 * host tests (tests/web) cover the rest against real servers.
 * Prints "webtest: all N checks passed". */
#include "hal.h"
#include "web.h"
#include "bearssl.h"
#include "webtest_certs.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { char m_[300]; failed++; snprintf(m_, sizeof m_, __VA_ARGS__); \
                                              printf("webtest: FAIL line %d: %s\n", __LINE__, m_); } } while (0)

/* libweb's clock (plat.c) */
void wp_utc_now(long long *unix_s, long *uncertain_s);

/* ---------------- a TLS handshake over memory ---------------- */
typedef struct { unsigned char *p; size_t n, cap; } acc_t;
static void acc(void *ctx, const void *b, size_t n) {
    acc_t *a = ctx;
    if (a->n + n > a->cap) return;
    memcpy(a->p + a->n, b, n);
    a->n += n;
}

static br_x509_trust_anchor ta;
static unsigned char ta_dn[512], ta_key[600];

static int make_anchor(void) {
    br_x509_decoder_context dc;
    acc_t dn = { ta_dn, 0, sizeof ta_dn };
    br_x509_decoder_init(&dc, acc, &dn);
    br_x509_decoder_push(&dc, ca_der, sizeof ca_der);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || pk->key_type != BR_KEYTYPE_RSA || pk->key.rsa.nlen + pk->key.rsa.elen > sizeof ta_key) return -1;
    memcpy(ta_key, pk->key.rsa.n, pk->key.rsa.nlen);
    memcpy(ta_key + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
    ta.dn.data = ta_dn;
    ta.dn.len = dn.n;
    ta.flags = BR_X509_TA_CA;
    ta.pkey.key_type = BR_KEYTYPE_RSA;
    ta.pkey.key.rsa.n = ta_key;
    ta.pkey.key.rsa.nlen = pk->key.rsa.nlen;
    ta.pkey.key.rsa.e = ta_key + pk->key.rsa.nlen;
    ta.pkey.key.rsa.elen = pk->key.rsa.elen;
    return 0;
}

/* move records from one engine to the other; 1 if anything moved */
static int pump(br_ssl_engine_context *from, br_ssl_engine_context *to) {
    size_t sl, rl;
    unsigned char *s = br_ssl_engine_sendrec_buf(from, &sl);
    if (!sl) return 0;
    unsigned char *r = br_ssl_engine_recvrec_buf(to, &rl);
    if (!rl) return 0;
    size_t n = sl < rl ? sl : rl;
    memcpy(r, s, n);
    br_ssl_engine_sendrec_ack(from, n);
    br_ssl_engine_recvrec_ack(to, n);
    return 1;
}

static void run_engines(br_ssl_engine_context *c, br_ssl_engine_context *s) {
    for (int i = 0; i < 10000; i++) {
        int m = pump(c, s) | pump(s, c);
        if (!m) break;
    }
}

/* send text from a to b and read it there */
static int app_data(br_ssl_engine_context *a, br_ssl_engine_context *b, const char *text) {
    size_t n, len = strlen(text);
    unsigned char *p = br_ssl_engine_sendapp_buf(a, &n);
    if (!p || n < len) return -1;
    memcpy(p, text, len);
    br_ssl_engine_sendapp_ack(a, len);
    br_ssl_engine_flush(a, 0);
    run_engines(a, b);
    unsigned char *q = br_ssl_engine_recvapp_buf(b, &n);
    if (!q || n != len || memcmp(q, text, len)) return -1;
    br_ssl_engine_recvapp_ack(b, n);
    return 0;
}

static int set_time(br_x509_minimal_context *x, long long t) {
    long long days = t / 86400, secs = t % 86400;
    br_x509_minimal_set_time(x, (uint32_t)(days + 719528), (uint32_t)secs);
    return 0;
}

/* one handshake; returns the client's error (0 = success) */
static int handshake(int ec, uint16_t suite, const char *name, long long when, unsigned *server_err, uint64_t *ms) {
    static br_ssl_client_context cc;
    static br_ssl_server_context sc;
    static br_x509_minimal_context xc;
    static unsigned char cbuf[BR_SSL_BUFSIZE_BIDI], sbuf[BR_SSL_BUFSIZE_BIDI];
    static br_skey_decoder_context kd;
    br_x509_certificate chain = { ec ? (unsigned char *)ec_der : (unsigned char *)rsa_der, ec ? sizeof ec_der : sizeof rsa_der };
    br_skey_decoder_init(&kd);
    br_skey_decoder_push(&kd, ec ? ec_key : rsa_key, ec ? sizeof ec_key : sizeof rsa_key);
    if (br_skey_decoder_last_error(&kd)) return -100;
    if (ec) br_ssl_server_init_full_ec(&sc, &chain, 1, BR_KEYTYPE_RSA, br_skey_decoder_get_ec(&kd));
    else br_ssl_server_init_full_rsa(&sc, &chain, 1, br_skey_decoder_get_rsa(&kd));
    br_ssl_client_init_full(&cc, &xc, &ta, 1);
    br_ssl_engine_set_versions(&cc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&cc.eng, &suite, 1);
    set_time(&xc, when);
    br_ssl_engine_set_buffer(&cc.eng, cbuf, sizeof cbuf, 1);
    br_ssl_engine_set_buffer(&sc.eng, sbuf, sizeof sbuf, 1);
    unsigned char seed[64];
    if (getrandom(seed, sizeof seed) != sizeof seed) return -101;
    br_ssl_engine_inject_entropy(&cc.eng, seed, 32);
    br_ssl_engine_inject_entropy(&sc.eng, seed + 32, 32);
    uint64_t t0 = time_ms();
    if (!br_ssl_client_reset(&cc, name, 0) || !br_ssl_server_reset(&sc)) return -102;
    run_engines(&cc.eng, &sc.eng);
    *ms = time_ms() - t0;
    *server_err = (unsigned)br_ssl_engine_last_error(&sc.eng);
    int err = br_ssl_engine_last_error(&cc.eng);
    if (err) return err;
    if (!(br_ssl_engine_current_state(&cc.eng) & BR_SSL_SENDAPP)) return -103;
    br_ssl_session_parameters p;
    br_ssl_engine_get_session_parameters(&cc.eng, &p);
    if (p.cipher_suite != suite || br_ssl_engine_get_version(&cc.eng) != BR_TLS12) return -104;
    if (app_data(&cc.eng, &sc.eng, "GET / HTTP/1.1\r\nHost: webtest.halcyon\r\n\r\n")) return -105;
    if (app_data(&sc.eng, &cc.eng, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")) return -106;
    return 0;
}

static const unsigned char gz[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xFF, 0xF3, 0x48, 0xCC, 0x49, 0xAE, 0xCC, 0xCF, 0x53, 0x28, 0x4F,
    0x4D, 0x52, 0x28, 0x49, 0x2D, 0x2E, 0xB1, 0x52, 0xC8, 0xCC, 0x4B, 0xCB, 0x49, 0x2C, 0x49, 0x55, 0x00, 0x0A, 0x7A, 0x40,
    0x24, 0xF5, 0x60, 0x8C, 0x51, 0x55, 0xA3, 0xAA, 0x46, 0x55, 0x8D, 0xAA, 0x1A, 0x55, 0x35, 0x64, 0x55, 0x01, 0x00, 0x1E,
    0x9E, 0xE6, 0x65, 0xF0, 0x05, 0x00, 0x00 };

int main(int argc, char **argv) {
    printf("webtest: starting\n");
    /* the clock */
    long long now;
    long unc;
    wp_utc_now(&now, &unc);
    hal_datetime_t dt;
    datetime(&dt);
    CHECK(now > 1700000000LL && now < 4102444800LL, "the time (%lld) is not between 2023 and 2100", now);
    printf("webtest: clock %04d-%02d-%02d %02d:%02d, uncertainty %ld s\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, unc);

    /* TLS */
    CHECK(make_anchor() == 0, "test CA");
    static const struct { int ec; uint16_t suite; const char *name; } cases[] = {
        { 0, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, "ECDHE-RSA-AES128-GCM" },
        { 0, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, "ECDHE-RSA-CHACHA20-POLY1305" },
        { 0, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, "ECDHE-RSA-AES256-GCM" },
        { 0, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, "ECDHE-RSA-AES128-CBC-SHA256" },
        { 0, BR_TLS_RSA_WITH_AES_128_GCM_SHA256, "RSA-AES128-GCM" },
        { 1, BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, "ECDHE-ECDSA-AES128-GCM" },
        { 1, BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, "ECDHE-ECDSA-CHACHA20-POLY1305" },
    };
    long long t = now + unc + 3600;   /* inside the certificates' 100 years whatever the time zone */
    for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        unsigned serr;
        uint64_t ms;
        int r = handshake(cases[i].ec, cases[i].suite, "webtest.halcyon", t, &serr, &ms);
        CHECK(r == 0, "%s: client error %d, server error %u", cases[i].name, r, serr);
        if (!r) printf("webtest: TLS 1.2 %s: handshake and data in %u ms\n", cases[i].name, (unsigned)ms);
    }
    unsigned serr;
    uint64_t ms;
    int r = handshake(0, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, "other.halcyon", t, &serr, &ms);
    CHECK(r == BR_ERR_X509_BAD_SERVER_NAME, "wrong name: %d", r);
    r = handshake(0, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, "webtest.halcyon", 7258118400LL, &serr, &ms);   /* 2200 */
    CHECK(r == BR_ERR_X509_EXPIRED, "expired: %d", r);
    r = handshake(1, BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, "webtest.halcyon", 1577836800LL, &serr, &ms); /* 2020 */
    CHECK(r == BR_ERR_X509_EXPIRED, "not yet valid: %d", r);

    /* inflate */
    uint8_t *out;
    size_t outn;
    r = inflate_gzip(gz, sizeof gz, &out, &outn, 1 << 20);
    CHECK(r == 0 && outn == 1520 && !memcmp(out, "Halcyon web test: inflate on Halcyon. Halcyon", 45), "gzip: %d, %u bytes", r, (unsigned)outn);
    if (!r) free(out);
    r = inflate_gzip(gz, sizeof gz - 1, &out, &outn, 1 << 20);
    CHECK(r == INF_E_TRUNC, "truncated gzip: %d", r);
    r = inflate_gzip(gz, sizeof gz, &out, &outn, 1000);
    CHECK(r == INF_E_LIMIT, "gzip limit: %d", r);

    /* URLs */
    char u[URL_MAX];
    CHECK(!url_resolve("http://a/b/c/d;p?q", "../g", u, sizeof u) && !strcmp(u, "http://a/b/g"), "resolve: %s", u);
    CHECK(!url_from_user("C:\\Documents\\Web\\index.htm", u, sizeof u) && !strcmp(u, "file:///C:/Documents/Web/index.htm"), "from_user: %s", u);

    /* a fetch on this system's network: whatever it is, it must end in time and cleanly */
    web_session_t *s = web_session_new();
    web_request_t rq;
    memset(&rq, 0, sizeof rq);
    rq.url = "http://example.com/";
    rq.connect_timeout_ms = rq.idle_timeout_ms = 5000;
    web_response_t rs;
    uint64_t t0 = time_ms();
    r = web_fetch(s, &rq, &rs);
    uint64_t took = time_ms() - t0;
    printf("webtest: fetch http://example.com/: %s (%s) in %u ms\n", r ? web_strerror(r) : "ok", r ? rs.error_text : rs.reason, (unsigned)took);
    CHECK(r == 0 || r == WEB_E_NONET || r == WEB_E_DNS || r == WEB_E_CONNECT || r == WEB_E_TIMEOUT, "fetch: %d", r);
    CHECK(took < 30000, "fetch took %u ms", (unsigned)took);
    web_response_free(&rs);
    /* the same in a worker thread, stopped */
    web_job_t *j = web_job_start(s, &rq);
    CHECK(j != 0, "web_job_start");
    if (j) {
        web_job_cancel(j);
        CHECK(web_job_wait(j, 20000), "a canceled job ends");
        web_job_free(j);
    }
    web_session_free(s);

    if (failed) printf("webtest: %d of %d checks FAILED\n", failed, checks);
    else printf("webtest: all %d checks passed\n", checks);
    return failed != 0;
}
