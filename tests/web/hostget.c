/* Host test driver for libweb: fetch a URL and print one RESULT line that
 * tests/web/run.sh checks.
 *   hostget [options] URL
 *     --ca FILE        extra trust anchor(s) (the test CA)
 *     --method M       GET / HEAD / POST     --data TEXT   POST body
 *     --max N          body limit            --timeout MS  idle timeout
 *     --no-gzip        no Accept-Encoding    --out FILE    write the body
 *     --stream         take the body through on_data
 *     --repeat N       fetch N times in one session (keep-alive, cookies, TLS resumption)
 *   Several URLs are fetched one after the other in the same session.
 *     --job MS         run in a web_job and cancel it after MS milliseconds
 *     --no-redirect */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

static const char *errname(int e) {
    switch (e) {
    case 0: return "OK";
    case WEB_E_URL: return "URL"; case WEB_E_DNS: return "DNS"; case WEB_E_CONNECT: return "CONNECT";
    case WEB_E_TIMEOUT: return "TIMEOUT"; case WEB_E_NONET: return "NONET"; case WEB_E_TLS: return "TLS";
    case WEB_E_CERT: return "CERT"; case WEB_E_PROTOCOL: return "PROTOCOL"; case WEB_E_TOOBIG: return "TOOBIG";
    case WEB_E_REDIRECTS: return "REDIRECTS"; case WEB_E_CANCELED: return "CANCELED"; case WEB_E_TRUNCATED: return "TRUNCATED";
    case WEB_E_DECODE: return "DECODE"; case WEB_E_HEADERS: return "HEADERS"; case WEB_E_CLOSED: return "CLOSED";
    case WEB_E_SCHEME: return "SCHEME";
    }
    return "OTHER";
}

static FILE *streamf;
static size_t streamed;
static int on_data(void *ctx, const void *d, size_t n) {
    streamed += n;
    if (streamf) fwrite(d, 1, n, streamf);
    return 0;
}

int main(int argc, char **argv) {
    web_request_t rq;
    memset(&rq, 0, sizeof rq);
    const char *ca = 0, *out = 0, *urls[16];
    int nurls = 0;
    int repeat = 1, job_ms = -1, stream = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--ca") && i + 1 < argc) ca = argv[++i];
        else if (!strcmp(a, "--method") && i + 1 < argc) rq.method = argv[++i];
        else if (!strcmp(a, "--data") && i + 1 < argc) { rq.body = argv[++i]; rq.body_len = strlen(argv[i]); }
        else if (!strcmp(a, "--max") && i + 1 < argc) rq.max_body = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "--timeout") && i + 1 < argc) rq.idle_timeout_ms = rq.connect_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--no-gzip")) rq.flags |= WEB_REQ_NO_GZIP;
        else if (!strcmp(a, "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--stream")) stream = 1;
        else if (!strcmp(a, "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--job") && i + 1 < argc) job_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--no-redirect")) rq.max_redirects = -1;
        else if (nurls < 16) urls[nurls++] = a;
    }
    if (!nurls) { fprintf(stderr, "usage: hostget [options] URL...\n"); return 2; }
    web_session_t *s = web_session_new();
    if (ca) {
        FILE *f = fopen(ca, "rb");
        static char pem[1 << 20];
        size_t n = f ? fread(pem, 1, sizeof pem, f) : 0;
        if (f) fclose(f);
        if (web_session_add_ca_pem(s, pem, n) < 1) { fprintf(stderr, "no CA in %s\n", ca); return 2; }
    }
    if (stream) {
        rq.on_data = on_data;
        if (out) streamf = fopen(out, "wb");
    }
    int last = 0;
    for (int it = 0; it < repeat * nurls; it++) {
        rq.url = urls[it % nurls];
        web_response_t rs, *rp = &rs;
        web_job_t *j = 0;
        streamed = 0;
        if (job_ms >= 0) {
            j = web_job_start(s, &rq);
            if (!j) return 3;
            if (!web_job_wait(j, job_ms)) web_job_cancel(j);
            web_job_wait(j, -1);
            rp = web_job_response(j);
        } else {
            web_fetch(s, &rq, &rs);
        }
        last = rp->error;
        size_t len = stream ? streamed : rp->body_len;
        printf("RESULT err=%s status=%d len=%zu redirects=%d ctype=%s charset=%s tls=%s | %s\n", errname(rp->error), rp->status,
               len, rp->redirects, rp->content_type[0] ? rp->content_type : "-", rp->charset[0] ? rp->charset : "-",
               rp->tls_info[0] ? "yes" : "no", rp->error ? rp->error_text : rp->url);
        if (out && !stream && rp->body) {
            FILE *f = fopen(out, "wb");
            if (f) { fwrite(rp->body, 1, rp->body_len, f); fclose(f); }
        }
        if (j) web_job_free(j);
        else web_response_free(&rs);
    }
    if (streamf) fclose(streamf);
    web_session_free(s);
    return last ? 1 : 0;
}
