/* WGET.EXE - fetch a URL to a file or the console (HTTP and HTTPS, through
 * libweb). Prints the status, the type and the size.
 *
 *   wget [options] URL
 *     -O FILE         save as FILE ("-" = the console); default: the name
 *                     from the server or the URL, in the current folder
 *     -q              quiet: only errors
 *     -S              show the answer's headers
 *     --head          HEAD request (headers only, nothing saved)
 *     --post-data D   POST D (application/x-www-form-urlencoded)
 *     --max-redirect N, --timeout SECONDS, --no-compression
 *
 * Exit code: 0 done, 1 error (no answer), 8 the server answered with an error. */
#include "hal.h"
#include "web.h"

typedef struct {
    web_response_t *rs;
    const char *out;            /* -O, or NULL */
    char path[300];
    int fd;                     /* -1 not open yet, 1 console */
    int quiet, err;
    long long written, next_report;
    uint64_t t0;
} dl_t;

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* a file name from the server's or the URL's name */
static void pick_name(dl_t *d) {
    char name[160] = "";
    if (d->rs->filename[0]) strlcpy(name, d->rs->filename, sizeof name);
    else {
        web_url_t u;
        if (!url_parse(d->rs->url, &u)) {
            char dec[300];
            url_decode(u.path, strlen(u.path), dec, sizeof dec, 0);
            strlcpy(name, base_name(dec), sizeof name);
        }
    }
    /* nothing that leaves the folder or is not a valid name */
    for (char *p = name; *p; p++) if (*p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|' || (unsigned char)*p < 32) *p = '_';
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) strlcpy(name, "index.html", sizeof name);
    strlcpy(d->path, name, sizeof d->path);
}

static int on_data(void *ctx, const void *data, size_t n) {
    dl_t *d = ctx;
    if (d->rs->status < 200 || d->rs->status > 299) return 0;     /* an error page: not saved */
    if (d->fd < 0) {
        if (d->out && !strcmp(d->out, "-")) d->fd = 1;
        else {
            if (d->out) strlcpy(d->path, d->out, sizeof d->path);
            else pick_name(d);
            d->fd = open(d->path, O_WRITE | O_CREATE | O_TRUNC);
            if (d->fd < 0) { d->err = d->fd; return d->fd; }
            if (!d->quiet) printf("Saving to: %s\n", d->path);
        }
    }
    long w = write(d->fd, data, n);
    if (w != (long)n) { d->err = w < 0 ? (int)w : E_NOSPC; return d->err; }
    d->written += (long long)n;
    if (!d->quiet && d->fd != 1 && d->written >= d->next_report) {
        long long t = d->rs->content_length;
        if (t > 0) printf("  %lld KB of %lld KB (%d%%)\n", d->written >> 10, t >> 10, (int)(d->written * 100 / t));
        else printf("  %lld KB\n", d->written >> 10);
        d->next_report = d->written + (t > 0 ? (t / 4 > 65536 ? t / 4 : 65536) : (1 << 20));
    }
    return 0;
}

static void on_progress(void *ctx, const char *phase, long long got, long long total) {
    dl_t *d = ctx;
    if (!d->quiet && strcmp(phase, "Receiving") && strcmp(phase, "Sending the request")) printf("%s...\n", phase);
}

int main(int argc, char **argv) {
    web_request_t rq;
    memset(&rq, 0, sizeof rq);
    dl_t d;
    memset(&d, 0, sizeof d);
    d.fd = -1;
    int headers = 0;
    const char *url = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-O") && i + 1 < argc) d.out = argv[++i];
        else if (!strcmp(a, "-q")) d.quiet = 1;
        else if (!strcmp(a, "-S")) headers = 1;
        else if (!strcmp(a, "--head")) rq.method = "HEAD";
        else if (!strcmp(a, "--post-data") && i + 1 < argc) { rq.method = "POST"; rq.body = argv[++i]; rq.body_len = strlen(argv[i]); }
        else if (!strcmp(a, "--max-redirect") && i + 1 < argc) { int n = atoi(argv[++i]); rq.max_redirects = n > 0 ? n : -1; }
        else if (!strcmp(a, "--timeout") && i + 1 < argc) rq.connect_timeout_ms = rq.idle_timeout_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(a, "--no-compression")) rq.flags |= WEB_REQ_NO_GZIP;
        else if (a[0] == '-' && a[1]) { printf("wget: unknown option %s\n", a); return 1; }
        else url = a;
    }
    if (!url) {
        printf("Usage: wget [-O file|-] [-q] [-S] [--head] [--post-data data] [--max-redirect n] [--timeout s] URL\n");
        return 1;
    }
    char full[URL_MAX];
    if (url_from_user(url, full, sizeof full)) { printf("wget: %s is not a valid address\n", url); return 1; }
    web_response_t rs;
    d.rs = &rs;
    d.next_report = 0;
    d.t0 = time_ms();
    rq.url = full;
    rq.max_body = 1u << 30;               /* files up to 1 GiB (bodies with a content coding are decoded in memory) */
    rq.on_data = on_data;
    rq.on_progress = on_progress;
    rq.ctx = &d;
    if (!d.quiet) printf("--> %s\n", full);
    web_session_t *s = web_session_new();
    if (!s) return 1;
    int r = web_fetch(s, &rq, &rs);
    if (headers && rs.headers) {
        printf("  HTTP/1.1 %d %s\n", rs.status, rs.reason);
        for (char *p = rs.headers; *p; ) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            printf("  %s\n", p);
            if (!e) break;
            p = e + 1;
        }
    }
    int code = 0;
    if (r) {
        printf("wget: %s\n", rs.error_text[0] ? rs.error_text : web_strerror(r));
        if (d.err) printf("wget: cannot write %s: %s\n", d.path, hal_strerror(d.err));
        code = 1;
    } else {
        if (!d.quiet) {
            if (rs.redirects) printf("Redirected %d time%s to %s\n", rs.redirects, rs.redirects == 1 ? "" : "s", rs.url);
            if (rs.tls) printf("Secure connection: %s\n", rs.tls_info);
            printf("HTTP %d %s, %s\n", rs.status, rs.reason, rs.content_type[0] ? rs.content_type : "no type");
        }
        if (rs.status < 200 || rs.status > 299) {
            printf("wget: the server answered %d %s\n", rs.status, rs.reason);
            code = 8;
        } else if (rq.method && !strcmp(rq.method, "HEAD")) {
            if (!d.quiet) printf("Length: %lld\n", rs.content_length);
        } else {
            if (d.fd < 0 && !(d.out && !strcmp(d.out, "-"))) {
                /* an empty body: still make the file */
                on_data(&d, "", 0);
            }
            uint64_t ms = time_ms() - d.t0;
            if (!d.quiet && d.fd != 1)
                printf("Saved %s: %lld bytes in %u.%u s\n", d.path, d.written, (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
        }
    }
    if (d.fd > 1) close(d.fd);
    web_response_free(&rs);
    web_session_free(s);
    return code;
}
