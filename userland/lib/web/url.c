/* libweb: URL parsing, formatting and resolution (RFC 3986, with the
 * leniencies browsers have: backslashes as slashes in http/https/file,
 * spaces and non-ASCII bytes percent-encoded, surrounding white space and
 * embedded tabs / newlines removed). */
#include "webplat.h"

static int is_alpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* special schemes have an authority and a path; backslash counts as slash */
static int special(const char *scheme) {
    return !strcmp(scheme, "http") || !strcmp(scheme, "https") || !strcmp(scheme, "file");
}
static int default_port(const char *scheme) {
    if (!strcmp(scheme, "http")) return 80;
    if (!strcmp(scheme, "https")) return 443;
    return 0;
}

/* copy [s, s+len) to out percent-encoding what must be; returns 0 or -1 */
static int enc_copy(const char *s, size_t len, char *out, size_t n, int bs_to_slash, int keep_hash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '\\' && bs_to_slash) c = '/';
        int esc = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' ||
                  c == '}' || c == '|' || c == '^' || (c == '#' && !keep_hash) || c == '\\';
        if (c == '%' && !(i + 2 < len && w_hexval((unsigned char)s[i + 1]) >= 0 && w_hexval((unsigned char)s[i + 2]) >= 0))
            esc = 1;                     /* a lone '%' */
        if (esc) {
            if (o + 3 >= n) return -1;
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
        } else {
            if (o + 1 >= n) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return 0;
}

void url_scheme_of(const char *s, char *out, size_t n) {
    while (*s && w_isspace((unsigned char)*s)) s++;
    size_t i = 0;
    out[0] = 0;
    if (!is_alpha((unsigned char)s[0])) return;
    while (s[i] && (is_alpha((unsigned char)s[i]) || is_digit((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    if (s[i] != ':' || i + 1 > n) return;
    for (size_t k = 0; k < i; k++) out[k] = (char)w_tolower((unsigned char)s[k]);
    out[i] = 0;
}

/* trim white space at both ends into a temporary copy */
static char *trimmed(const char *s) {
    while (*s && (unsigned char)*s <= 0x20) s++;
    size_t l = strlen(s);
    while (l && (unsigned char)s[l - 1] <= 0x20) l--;
    return w_strndup(s, l);
}

static int parse_host(const char *s, size_t len, web_url_t *u) {
    if (len >= sizeof u->host) return WEB_E_URL;
    if (len && s[0] == '[') {                       /* IPv6 literal */
        if (s[len - 1] != ']') return WEB_E_URL;
        for (size_t i = 1; i + 1 < len; i++) {
            int c = (unsigned char)s[i];
            if (!(w_hexval(c) >= 0 || c == ':' || c == '.')) return WEB_E_URL;
        }
        memcpy(u->host, s + 1, len - 2);
        u->host[len - 2] = 0;
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)s[i];
        if (c == '%' && i + 2 < len && w_hexval((unsigned char)s[i + 1]) >= 0 && w_hexval((unsigned char)s[i + 2]) >= 0) {
            c = w_hexval((unsigned char)s[i + 1]) * 16 + w_hexval((unsigned char)s[i + 2]);
            i += 2;
        }
        c = w_tolower(c);
        /* letters, digits, '-', '.', '_' (no IDN: non-ASCII host names are not supported) */
        if (!(is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_')) return WEB_E_URL;
        u->host[o++] = (char)c;
    }
    u->host[o] = 0;
    return 0;
}

int url_parse(const char *in, web_url_t *u) {
    memset(u, 0, sizeof *u);
    char *s = trimmed(in);
    if (!s) return E_NOMEM;
    int r = WEB_E_URL;
    url_scheme_of(s, u->scheme, sizeof u->scheme);
    if (!u->scheme[0]) goto out;
    const char *p = s + strlen(u->scheme) + 1;
    if (!special(u->scheme)) {
        /* opaque (about:, data:, mailto:, javascript:...): everything after the colon */
        const char *h = strchr(p, '#');
        size_t pl = h ? (size_t)(h - p) : strlen(p);
        if (strcmp(u->scheme, "data") && enc_copy(p, pl, u->path, sizeof u->path, 0, 0)) goto out;
        if (!strcmp(u->scheme, "data")) {
            if (pl >= sizeof u->path) goto out;
            memcpy(u->path, p, pl); u->path[pl] = 0;
        }
        if (h) { u->has_fragment = 1; if (enc_copy(h + 1, strlen(h + 1), u->fragment, sizeof u->fragment, 0, 1)) goto out; }
        r = 0;
        goto out;
    }
    /* authority: "//" (backslashes too); file: also "file:/C:/x" and "file:C:/x" */
    int slashes = 0;
    while (p[slashes] == '/' || p[slashes] == '\\') slashes++;
    const char *auth = p + (slashes >= 2 ? 2 : slashes);
    const char *ae = auth;
    if (!strcmp(u->scheme, "file")) {
        if (slashes < 2 || (is_alpha((unsigned char)auth[0]) && (auth[1] == ':' || auth[1] == '|'))) ae = auth;   /* no host */
        else while (*ae && *ae != '/' && *ae != '\\' && *ae != '?' && *ae != '#') ae++;
    } else {
        if (slashes < 1) goto out;
        while (*ae && *ae != '/' && *ae != '\\' && *ae != '?' && *ae != '#') ae++;
        if (ae == auth) goto out;                           /* no host */
    }
    /* userinfo@host:port */
    const char *at = 0;
    for (const char *q = auth; q < ae; q++) if (*q == '@') at = q;
    const char *hs = auth;
    if (at) {
        size_t ul = (size_t)(at - auth);
        if (ul >= sizeof u->user) goto out;
        memcpy(u->user, auth, ul); u->user[ul] = 0;
        hs = at + 1;
    }
    const char *colon = 0;
    if (hs < ae && *hs == '[') {
        const char *rb = hs;
        while (rb < ae && *rb != ']') rb++;
        if (rb >= ae) goto out;
        if (rb + 1 < ae) { if (rb[1] != ':') goto out; colon = rb + 1; }
    } else {
        for (const char *q = hs; q < ae; q++) if (*q == ':') { colon = q; break; }
    }
    const char *he = colon ? colon : ae;
    if (parse_host(hs, (size_t)(he - hs), u)) goto out;
    if (strcmp(u->scheme, "file") && !u->host[0]) goto out;
    u->port = default_port(u->scheme);
    if (colon && colon + 1 < ae) {
        long port = 0;
        for (const char *q = colon + 1; q < ae; q++) {
            if (!is_digit((unsigned char)*q)) goto out;
            port = port * 10 + (*q - '0');
            if (port > 65535) goto out;
        }
        if (port < 1) goto out;
        u->port = (int)port;
        u->explicit_port = port != default_port(u->scheme);
    }
    /* path, query, fragment */
    const char *ps = ae;
    const char *qm = ps, *hm;
    while (*qm && *qm != '?' && *qm != '#') qm++;
    hm = qm;
    while (*hm && *hm != '#') hm++;
    char *pathbuf = u->path;
    size_t pathn = sizeof u->path;
    if (*ps != '/' && *ps != '\\') { pathbuf[0] = '/'; pathbuf++; pathn--; }
    if (enc_copy(ps, (size_t)(qm - ps), pathbuf, pathn, 1, 0)) goto out;
    /* file:///C|/x -> C:/x */
    if (!strcmp(u->scheme, "file") && is_alpha((unsigned char)u->path[1]) && u->path[2] == '|') u->path[2] = ':';
    if (*qm == '?') {
        u->has_query = 1;
        if (enc_copy(qm + 1, (size_t)(hm - qm - 1), u->query, sizeof u->query, 0, 0)) goto out;
    }
    if (*hm == '#') {
        u->has_fragment = 1;
        if (enc_copy(hm + 1, strlen(hm + 1), u->fragment, sizeof u->fragment, 0, 1)) goto out;
    }
    r = 0;
out:
    free(s);
    if (r) memset(u, 0, sizeof *u);
    return r;
}

int url_format(const web_url_t *u, int frag, char *out, size_t n) {
    int len;
    if (!special(u->scheme)) {
        len = w_snprintf(out, n, "%s:%s%s%s", u->scheme, u->path, frag && u->has_fragment ? "#" : "",
                         frag && u->has_fragment ? u->fragment : "");
    } else {
        char port[12] = "";
        if (u->explicit_port) w_snprintf(port, sizeof port, ":%d", u->port);
        int v6 = strchr(u->host, ':') != 0;
        len = w_snprintf(out, n, "%s://%s%s%s%s%s%s%s%s%s", u->scheme, v6 ? "[" : "", u->host, v6 ? "]" : "", port,
                         u->path, u->has_query ? "?" : "", u->has_query ? u->query : "",
                         frag && u->has_fragment ? "#" : "", frag && u->has_fragment ? u->fragment : "");
    }
    if (len < 0 || (size_t)len >= n) { if (n) out[0] = 0; return WEB_E_URL; }
    return len;
}

/* RFC 3986 5.2.4, in place */
static void remove_dots(char *path) {
    char *out = path, *in = path;
    /* the path is absolute ("/..."): work segment by segment */
    while (*in) {
        if (!strncmp(in, "/./", 3)) { in += 2; continue; }
        if (!strcmp(in, "/.")) { in[1] = 0; continue; }
        if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
            int last = in[3] == 0;
            in += 3;
            while (out > path && *--out != '/') ;
            *out = 0;
            if (last) { *out++ = '/'; *out = 0; break; }
            continue;
        }
        if (!strncmp(in, "/%2e/", 5) || !strncmp(in, "/%2E/", 5)) { in += 4; continue; }
        do *out++ = *in++; while (*in && *in != '/');
    }
    *out = 0;
    if (!path[0]) { path[0] = '/'; path[1] = 0; }
}

int url_resolve(const char *base, const char *ref, char *out, size_t n) {
    char sch[16];
    url_scheme_of(ref, sch, sizeof sch);
    web_url_t *u = malloc(sizeof *u);
    if (!u) return E_NOMEM;
    int r = WEB_E_URL;
    char *t = trimmed(ref);
    if (!t) { free(u); return E_NOMEM; }
    /* a reference with a scheme is absolute; a one-letter "scheme" is a drive (C:\...) */
    int drive = is_alpha((unsigned char)t[0]) && t[1] == ':' && (t[2] == '\\' || t[2] == '/');
    if (sch[0] && !drive) {
        if (!url_parse(t, u)) { r = url_format(u, 1, out, n); r = r < 0 ? r : 0; }
        goto done;
    }
    if (url_parse(base, u)) goto done;
    if (!special(u->scheme) && !(t[0] == '#')) goto done;     /* nothing is relative to about: or data: */
    char *tmp = malloc(URL_MAX * 2 + 16);
    if (!tmp) { r = E_NOMEM; goto done; }
    const char *p = t;
    if (drive) {
        /* C:\dir\x.htm */
        w_snprintf(tmp, URL_MAX * 2, "file:///%s", p);
        if (!url_parse(tmp, u)) { r = url_format(u, 1, out, n); r = r < 0 ? r : 0; }
        free(tmp);
        goto done;
    }
    if ((p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\')) {
        w_snprintf(tmp, URL_MAX * 2, "%s:%s", u->scheme, p);
    } else if (p[0] == '/' || p[0] == '\\') {
        web_url_t v = *u;
        char pq[URL_MAX];
        if (enc_copy(p, w_strcspn(p, "?#"), pq, sizeof pq, 1, 0)) { free(tmp); goto done; }
        w_strlcpy(v.path, pq, sizeof v.path);
        remove_dots(v.path);
        const char *q = p + w_strcspn(p, "?#");
        v.has_query = 0; v.query[0] = 0; v.has_fragment = 0;
        url_format(&v, 0, tmp, URL_MAX * 2);
        w_strlcat(tmp, q, URL_MAX * 2);
    } else if (p[0] == '?') {
        u->has_query = 0; u->has_fragment = 0;
        url_format(u, 0, tmp, URL_MAX * 2);
        w_strlcat(tmp, p, URL_MAX * 2);
    } else if (p[0] == '#' || p[0] == 0) {
        u->has_fragment = 0;
        url_format(u, 0, tmp, URL_MAX * 2);
        w_strlcat(tmp, p, URL_MAX * 2);
    } else {
        /* merge: the base's directory + the reference */
        web_url_t v = *u;
        char seg[URL_MAX];
        size_t pl = w_strcspn(p, "?#");
        if (enc_copy(p, pl, seg, sizeof seg, 1, 0)) { free(tmp); goto done; }
        char *slash = strrchr(v.path, '/');
        if (slash) slash[1] = 0; else w_strlcpy(v.path, "/", sizeof v.path);
        if (w_strlcat(v.path, seg, sizeof v.path) >= sizeof v.path) { free(tmp); goto done; }
        remove_dots(v.path);
        v.has_query = 0; v.query[0] = 0; v.has_fragment = 0;
        url_format(&v, 0, tmp, URL_MAX * 2);
        w_strlcat(tmp, p + pl, URL_MAX * 2);
    }
    if (!url_parse(tmp, u)) {
        if (special(u->scheme)) remove_dots(u->path);
        r = url_format(u, 1, out, n);
        r = r < 0 ? r : 0;
    }
    free(tmp);
done:
    free(t);
    free(u);
    if (r && n) out[0] = 0;
    return r;
}

int url_decode(const char *s, size_t len, char *out, size_t n, int form) {
    size_t o = 0;
    if (!n) return 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '%' && i + 2 < len) {
            int a = w_hexval((unsigned char)s[i + 1]), b = w_hexval((unsigned char)s[i + 2]);
            if (a >= 0 && b >= 0) { c = a * 16 + b; i += 2; }
        } else if (c == '+' && form) c = ' ';
        out[o++] = (char)c;
    }
    out[o] = 0;
    return (int)o;
}

int url_form_encode(const char *s, size_t len, char *out, size_t n, size_t pos) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_alpha(c) || is_digit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            if (pos + 1 >= n) return -1;
            out[pos++] = (char)c;
        } else if (c == ' ') {
            if (pos + 1 >= n) return -1;
            out[pos++] = '+';
        } else {
            if (pos + 3 >= n) return -1;
            out[pos++] = '%'; out[pos++] = hex[c >> 4]; out[pos++] = hex[c & 15];
        }
    }
    if (pos < n) out[pos] = 0;
    return (int)pos;
}

int url_from_user(const char *typed, char *out, size_t n) {
    char *t = trimmed(typed);
    if (!t) return E_NOMEM;
    char *tmp = malloc(URL_MAX + 16);
    if (!tmp) { free(t); return E_NOMEM; }
    char sch[16];
    url_scheme_of(t, sch, sizeof sch);
    if (is_alpha((unsigned char)t[0]) && t[1] == ':' && (t[2] == '\\' || t[2] == '/' || !t[2]))
        w_snprintf(tmp, URL_MAX + 16, "file:///%s", t);                 /* C:\dir\a.htm */
    else if (t[0] == '\\' || t[0] == '/')
        w_snprintf(tmp, URL_MAX + 16, "file:///C:%s", t);               /* \dir\a.htm on C: */
    else if (sch[0] && (strstr(t, "://") || !strcmp(sch, "about") || !strcmp(sch, "data") || !strcmp(sch, "file") ||
                        !strcmp(sch, "http") || !strcmp(sch, "https") || !strcmp(sch, "mailto") || !strcmp(sch, "javascript")))
        w_strlcpy(tmp, t, URL_MAX + 16);                                 /* has a scheme */
    else
        w_snprintf(tmp, URL_MAX + 16, "http://%s", t);                  /* example.com, localhost:8000 */
    web_url_t *u = malloc(sizeof *u);
    int r = E_NOMEM;
    if (u) {
        r = url_parse(tmp, u);
        if (!r) { r = url_format(u, 1, out, n); r = r < 0 ? r : 0; }
        free(u);
    }
    free(tmp);
    free(t);
    if (r && n) out[0] = 0;
    return r;
}
