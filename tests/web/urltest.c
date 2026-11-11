/* Host test of libweb's URL code: RFC 3986 section 5.4's reference
 * resolution examples, parsing, form encoding, and address bar input. */
#include <stdio.h>
#include <string.h>
#include "web.h"

static int fails, checks;

static void resolve(const char *base, const char *ref, const char *want) {
    char out[URL_MAX];
    int r = url_resolve(base, ref, out, sizeof out);
    checks++;
    if (want ? (r || strcmp(out, want)) : !r) {
        printf("FAIL resolve(%s, %s) = %d \"%s\", want \"%s\"\n", base, ref, r, out, want ? want : "(error)");
        fails++;
    }
}

static void user(const char *in, const char *want) {
    char out[URL_MAX];
    int r = url_from_user(in, out, sizeof out);
    checks++;
    if (want ? (r || strcmp(out, want)) : !r) {
        printf("FAIL from_user(%s) = %d \"%s\", want \"%s\"\n", in, r, out, want ? want : "(error)");
        fails++;
    }
}

static void parse(const char *in, const char *host, int port, const char *path, const char *query) {
    web_url_t u;
    int r = url_parse(in, &u);
    checks++;
    if (!host) {
        if (!r) { printf("FAIL parse(%s) accepted\n", in); fails++; }
        return;
    }
    if (r || strcmp(u.host, host) || u.port != port || strcmp(u.path, path) || strcmp(u.query, query)) {
        printf("FAIL parse(%s): %d host=%s port=%d path=%s query=%s\n", in, r, u.host, u.port, u.path, u.query);
        fails++;
    }
}

int main(void) {
    const char *b = "http://a/b/c/d;p?q";
    /* RFC 3986 5.4.1 normal examples */
    resolve(b, "g:h", "g:h");
    resolve(b, "g", "http://a/b/c/g");
    resolve(b, "./g", "http://a/b/c/g");
    resolve(b, "g/", "http://a/b/c/g/");
    resolve(b, "/g", "http://a/g");
    resolve(b, "//g", "http://g/");
    resolve(b, "?y", "http://a/b/c/d;p?y");
    resolve(b, "g?y", "http://a/b/c/g?y");
    resolve(b, "#s", "http://a/b/c/d;p?q#s");
    resolve(b, "g#s", "http://a/b/c/g#s");
    resolve(b, "g?y#s", "http://a/b/c/g?y#s");
    resolve(b, ";x", "http://a/b/c/;x");
    resolve(b, "g;x", "http://a/b/c/g;x");
    resolve(b, "g;x?y#s", "http://a/b/c/g;x?y#s");
    resolve(b, "", "http://a/b/c/d;p?q");
    resolve(b, ".", "http://a/b/c/");
    resolve(b, "./", "http://a/b/c/");
    resolve(b, "..", "http://a/b/");
    resolve(b, "../", "http://a/b/");
    resolve(b, "../g", "http://a/b/g");
    resolve(b, "../..", "http://a/");
    resolve(b, "../../", "http://a/");
    resolve(b, "../../g", "http://a/g");
    /* 5.4.2 abnormal examples */
    resolve(b, "../../../g", "http://a/g");
    resolve(b, "../../../../g", "http://a/g");
    resolve(b, "/./g", "http://a/g");
    resolve(b, "/../g", "http://a/g");
    resolve(b, "g.", "http://a/b/c/g.");
    resolve(b, ".g", "http://a/b/c/.g");
    resolve(b, "g..", "http://a/b/c/g..");
    resolve(b, "..g", "http://a/b/c/..g");
    resolve(b, "./../g", "http://a/b/g");
    resolve(b, "./g/.", "http://a/b/c/g/");
    resolve(b, "g/./h", "http://a/b/c/g/h");
    resolve(b, "g/../h", "http://a/b/c/h");
    resolve(b, "g;x=1/./y", "http://a/b/c/g;x=1/y");
    resolve(b, "g;x=1/../y", "http://a/b/c/y");
    resolve(b, "g?y/./x", "http://a/b/c/g?y/./x");
    resolve(b, "g?y/../x", "http://a/b/c/g?y/../x");
    resolve(b, "g#s/./x", "http://a/b/c/g#s/./x");
    resolve(b, "g#s/../x", "http://a/b/c/g#s/../x");
    /* browsers' leniencies */
    resolve("http://Example.COM:80/a/b.html", "c d.html", "http://example.com/a/c%20d.html");
    resolve("http://example.com/a/", "  /x\\y  ", "http://example.com/x/y");
    resolve("https://example.com:8443/a", "//other.org/p", "https://other.org/p");
    resolve("https://example.com/", "HTTP://X.ORG", "http://x.org/");
    resolve("http://example.com/", "caf\xc3\xa9", "http://example.com/caf%C3%A9");
    resolve("file:///C:/Documents/Web/index.htm", "page2.htm", "file:///C:/Documents/Web/page2.htm");
    resolve("file:///C:/Documents/Web/index.htm", "../Pictures/a.png", "file:///C:/Documents/Pictures/a.png");
    resolve("file:///C:/Documents/Web/index.htm", "C:\\Other\\x.htm", "file:///C:/Other/x.htm");
    resolve("http://example.com/a", "mailto:someone@example.com", "mailto:someone@example.com");
    resolve("about:blank", "x.html", 0);
    resolve("http://example.com/", "http://bad host/", 0);
    resolve("http://example.com/", "http://x.org:99999/", 0);
    /* parsing */
    parse("http://example.com", "example.com", 80, "/", "");
    parse("https://user:pw@Example.com:8443/p/q?x=1&y=2#frag", "example.com", 8443, "/p/q", "x=1&y=2");
    parse("http://127.0.0.1:8000/a%20b", "127.0.0.1", 8000, "/a%20b", "");
    parse("http://[::1]:81/", "::1", 81, "/", "");
    parse("http:///nohost", 0, 0, 0, 0);
    parse("http://exa mple.com/", 0, 0, 0, 0);
    parse("http://example.com:0/", 0, 0, 0, 0);
    parse("http://example.com:8x/", 0, 0, 0, 0);
    parse("nothing", 0, 0, 0, 0);
    parse("file:///C:/dir/x.htm", "", 0, "/C:/dir/x.htm", "");
    parse("file:C:/dir/x.htm", "", 0, "/C:/dir/x.htm", "");
    /* the address bar */
    user("example.com", "http://example.com/");
    user("  example.com/a b  ", "http://example.com/a%20b");
    user("localhost:8000", "http://localhost:8000/");
    user("https://example.com", "https://example.com/");
    user("C:\\Documents\\Web\\index.htm", "file:///C:/Documents/Web/index.htm");
    user("c:/x.htm", "file:///c:/x.htm");
    user("about:home", "about:home");
    /* form encoding */
    char f[64];
    int n = url_form_encode("a b&c=d/é", strlen("a b&c=d/é"), f, sizeof f, 0);
    checks++;
    if (n < 0 || strcmp(f, "a+b%26c%3Dd%2F%C3%A9")) { printf("FAIL form_encode: %s\n", f); fails++; }
    n = url_form_encode("abcdefgh", 8, f, 5, 0);
    checks++;
    if (n != -1) { printf("FAIL form_encode overflow: %d\n", n); fails++; }
    char d[64];
    url_decode("a+b%26%zz%4", 11, d, sizeof d, 1);
    checks++;
    if (strcmp(d, "a b&%zz%4")) { printf("FAIL decode: %s\n", d); fails++; }
    printf("url: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
