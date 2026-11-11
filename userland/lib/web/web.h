/* Halcyon web library (libweb): URLs, HTTP/1.1 and HTTPS (BearSSL), gzip /
 * deflate, cookies, and a worker thread to run a request in. Linked only
 * into the programs that use it (BROWSER.EXE, WGET.EXE); it also compiles
 * for Linux (-DWEB_HOST) for the host tests in tests/web/.
 *
 * See docs/DESIGN-M6-WEB.md. Results are >= 0 or a negative error: the
 * system's E_* codes or the WEB_E_* codes below; web_strerror() names
 * them. */
#ifndef WEB_H
#define WEB_H
#include <stddef.h>
#include <stdint.h>

/* ---- errors (besides E_*) ---- */
#define WEB_E_URL        (-1001)   /* not a URL this library can fetch */
#define WEB_E_DNS        (-1002)   /* the host name was not found */
#define WEB_E_CONNECT    (-1003)   /* no connection (refused, unreachable) */
#define WEB_E_TIMEOUT    (-1004)   /* the server did not answer in time */
#define WEB_E_NONET      (-1005)   /* this computer has no network (no AF_INET / name lookup) */
#define WEB_E_TLS        (-1006)   /* the secure connection failed (see error text) */
#define WEB_E_CERT       (-1007)   /* the server's certificate was rejected (see error text) */
#define WEB_E_PROTOCOL   (-1008)   /* the server's answer is not valid HTTP */
#define WEB_E_TOOBIG     (-1009)   /* the answer is larger than the request's limit */
#define WEB_E_REDIRECTS  (-1010)   /* too many redirects (or a loop) */
#define WEB_E_CANCELED   (-1011)   /* web_cancel() / the job was stopped */
#define WEB_E_TRUNCATED  (-1012)   /* the connection closed before the whole answer arrived */
#define WEB_E_DECODE     (-1013)   /* bad gzip / deflate data */
#define WEB_E_HEADERS    (-1014)   /* the headers are too large */
#define WEB_E_CLOSED     (-1015)   /* the server closed the connection without an answer */
#define WEB_E_SCHEME     (-1016)   /* a URL scheme this library does not fetch (ftp:, ...) */
const char *web_strerror(int err);

/* ---- URLs (RFC 3986) ---- */
#define URL_MAX 2048
typedef struct web_url {
    char scheme[16];              /* lower case: "http", "https", "file", "about", "data", ... */
    char user[64];                /* userinfo before '@' (kept, never sent) */
    char host[256];               /* lower case; an IPv6 literal without its brackets */
    int  port;                    /* explicit or the scheme's default (80, 443; 0 = none) */
    int  explicit_port;
    char path[URL_MAX];           /* starts with '/' for http(s) ("/" when empty); percent-encoded */
    char query[URL_MAX];          /* without '?'; has_query tells "?" from nothing */
    char fragment[512];           /* without '#' */
    int  has_query, has_fragment;
} web_url_t;
/* parse an absolute URL; 0 or WEB_E_URL. Spaces and other bytes that need it
 * are percent-encoded; a host is lower-cased and checked */
int  url_parse(const char *s, web_url_t *u);
/* rebuild (with the fragment if frag); returns the length or WEB_E_URL if it does not fit */
int  url_format(const web_url_t *u, int frag, char *out, size_t n);
/* resolve a reference (href) against a base URL into out: RFC 3986 section 5,
 * including "../" removal; 0 or WEB_E_URL */
int  url_resolve(const char *base, const char *ref, char *out, size_t n);
/* "%41" decoding (plus '+' as space if form); returns the length, out NUL-terminated */
int  url_decode(const char *s, size_t len, char *out, size_t n, int form);
/* application/x-www-form-urlencoded encoding of one name or value; appends to out (NUL-terminated);
 * returns the new length or -1 if it does not fit */
int  url_form_encode(const char *s, size_t len, char *out, size_t n, size_t pos);
/* what was typed in an address bar into a URL: "example.com" -> "http://example.com/",
 * "C:\\dir\\a.htm" -> "file:///C:/dir/a.htm"; 0 or WEB_E_URL */
int  url_from_user(const char *typed, char *out, size_t n);
/* a URL string's scheme ("https" for "https://x"), lower case, "" if none */
void url_scheme_of(const char *s, char *out, size_t n);

/* ---- inflate (RFC 1950 / 1951 / 1952); our own decoder, every length bounded ---- */
#define INF_E_DATA   (-1)        /* corrupt stream */
#define INF_E_TRUNC  (-2)        /* the input ended early */
#define INF_E_LIMIT  (-3)        /* the output would exceed max_out */
#define INF_E_NOMEM  (-4)
#define INF_E_CHECK  (-5)        /* CRC-32 / Adler-32 / length mismatch */
/* raw deflate: inflate in[0..inlen) into a malloc()ed *out (*outlen bytes, at
 * most max_out); *used = input bytes consumed. 0 or INF_E_* */
int  inflate_raw(const uint8_t *in, size_t inlen, size_t *used, uint8_t **out, size_t *outlen, size_t max_out);
int  inflate_zlib(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t max_out);
/* gzip (concatenated members too) */
int  inflate_gzip(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t max_out);
/* the same into a caller's buffer of exactly expect bytes (PNG); 0 or INF_E_* */
int  inflate_zlib_into(const uint8_t *in, size_t inlen, uint8_t *out, size_t expect);
uint32_t web_crc32(uint32_t crc, const uint8_t *p, size_t n);
uint32_t web_adler32(uint32_t a, const uint8_t *p, size_t n);

/* ---- HTTP ---- */
typedef struct web_session web_session_t;   /* cookies, kept-alive connections, TLS sessions */
web_session_t *web_session_new(void);
void web_session_free(web_session_t *s);
/* Add trust anchors from PEM certificates (for tests: a private test CA).
 * Returns how many were added. The built-in ones are Mozilla's (tools/mktrust.py). */
int  web_session_add_ca_pem(web_session_t *s, const char *pem, size_t len);
/* "flags" of a session */
#define WEB_SESS_NO_COOKIES 0x01
void web_session_set_flags(web_session_t *s, int flags);
void web_session_clear_cookies(web_session_t *s);

typedef struct web_request {
    const char *method;           /* "GET" (default), "HEAD", "POST" */
    const char *url;
    const char *content_type;     /* POST: e.g. "application/x-www-form-urlencoded" */
    const void *body;             /* POST body */
    size_t body_len;
    const char *referer;          /* or NULL */
    const char *accept;           /* Accept: header (default "*" "/" "*") */
    int max_redirects;            /* 0 = default (10); < 0 = do not follow */
    size_t max_body;              /* decoded body limit (0 = 16 MiB) */
    int connect_timeout_ms;       /* per address (0 = 15 s) */
    int idle_timeout_ms;          /* longest wait for the next bytes (0 = 30 s) */
    int flags;                    /* WEB_REQ_* */
    /* called with the body as it arrives (decoded only when not compressed; a
     * compressed body arrives once, decoded, at the end). Returning < 0
     * stops the request with that error. NULL: the body is kept in the
     * response. */
    int (*on_data)(void *ctx, const void *data, size_t n);
    /* progress: bytes of the body so far and the expected total (-1 unknown);
     * phase is a short text ("Looking up example.com", "Connecting", ...) */
    void (*on_progress)(void *ctx, const char *phase, long long got, long long total);
    void *ctx;
    volatile int *cancel;         /* nonzero: stop as soon as possible (WEB_E_CANCELED) */
} web_request_t;
#define WEB_REQ_NO_GZIP     0x01  /* do not send Accept-Encoding */
#define WEB_REQ_NO_KEEPALIVE 0x02

typedef struct web_response {
    int status;                   /* 200, 404... (0 when there was no answer) */
    char reason[64];
    char url[URL_MAX];            /* the final URL, after redirects */
    char content_type[128];       /* lower case, without parameters ("text/html") */
    char charset[32];             /* lower case, from Content-Type ("" if none) */
    char filename[128];           /* from Content-Disposition, "" if none */
    long long content_length;     /* -1 if not given (the encoded length) */
    int redirects;
    int tls;                      /* 1: came over HTTPS */
    char tls_info[96];            /* "TLS 1.2, ECDHE_RSA_WITH_AES_128_GCM_SHA256" */
    char *headers;                /* the final response's header lines, "Name: value\n"..., malloc()ed */
    uint8_t *body;                /* malloc()ed, NUL-terminated (not counted in body_len) */
    size_t body_len;
    int error;                    /* 0 or the error (also the result of web_fetch) */
    char error_text[256];         /* details: "the certificate has expired", "connection refused"... */
} web_response_t;

/* Run a request (blocking; use web_job_* for a worker thread). The response
 * is filled also on errors (error, error_text, whatever arrived); call
 * web_response_free afterwards. Returns 0 or the error. HTTP errors (404...)
 * are not errors here: see status. */
int  web_fetch(web_session_t *s, const web_request_t *rq, web_response_t *rs);
void web_response_free(web_response_t *rs);
/* a header of the response (case-insensitive name); copies the value, returns its length or -1 */
int  web_header(const web_response_t *rs, const char *name, char *out, size_t n);

/* ---- a request in a worker thread ---- */
typedef struct web_job web_job_t;
/* copies the request (and its strings and body); on_data / on_progress are
 * called in the worker thread. NULL on failure. */
web_job_t *web_job_start(web_session_t *s, const web_request_t *rq);
int  web_job_done(web_job_t *j);                /* 1 when finished */
void web_job_cancel(web_job_t *j);             /* asks the worker to stop */
/* wait for the end (timeout_ms < 0: no limit); 1 if done */
int  web_job_wait(web_job_t *j, int timeout_ms);
web_response_t *web_job_response(web_job_t *j); /* valid once done */
/* free the job: when it is still running, it is canceled and freed by the worker when it ends */
void web_job_free(web_job_t *j);

/* the User-Agent sent */
#define WEB_USER_AGENT "Halcyon/1.0"

#endif
