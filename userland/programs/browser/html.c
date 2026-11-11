/* BROWSER.EXE: the HTML tokenizer and tree builder, character sets and
 * entities.
 *
 * Tolerant in the ways HTML is in practice, after the outline of the HTML
 * standard's parser but much simpler: implied end tags (p, li, dt/dd,
 * option, table parts, headings), void elements, raw text elements (script
 * and style are dropped, title and textarea keep their text), comments,
 * doctypes, character references with and without the semicolon. The input
 * is untrusted: the tree is bounded in nodes (DOC_MAX_NODES), depth
 * (DOC_MAX_DEPTH) and attributes, and all memory comes from the document's
 * arena (with its own limit). */
#include "browser.h"

/* ---------------- arena ---------------- */
#define ARENA_BLOCK (256 * 1024)
void *arena_alloc(arena_t *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (a->failed) return 0;
    if (!a->head || a->head->size - a->head->used < n) {
        size_t sz = n + sizeof(arena_blk_t) + 16 > ARENA_BLOCK ? n + sizeof(arena_blk_t) + 16 : ARENA_BLOCK;
        if (a->limit && a->total + sz > a->limit) { a->failed = 1; return 0; }
        arena_blk_t *b = malloc(sz);
        if (!b) { a->failed = 1; return 0; }
        b->next = a->head;
        b->size = sz - ((sizeof(arena_blk_t) + 15) & ~(size_t)15);
        b->used = 0;
        a->head = b;
        a->total += sz;
    }
    char *p = (char *)a->head + ((sizeof(arena_blk_t) + 15) & ~(size_t)15) + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}
char *arena_strndup(arena_t *a, const char *s, size_t n) {
    char *d = arena_alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
void arena_free(arena_t *a) {
    while (a->head) { arena_blk_t *b = a->head; a->head = b->next; free(b); }
    a->total = 0;
    a->failed = 0;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha_c(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int ci_eq(const char *a, size_t al, const char *b) {
    size_t bl = strlen(b);
    if (al != bl) return 0;
    for (size_t i = 0; i < al; i++) if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
    return 1;
}
static const char *ci_find(const char *h, size_t hn, const char *n) {
    size_t nl = strlen(n);
    for (size_t i = 0; i + nl <= hn; i++) {
        size_t k = 0;
        while (k < nl && lc((unsigned char)h[i + k]) == lc((unsigned char)n[k])) k++;
        if (k == nl) return h + i;
    }
    return 0;
}

/* ---------------- UTF-8 and character sets ---------------- */
static int put_utf8(unsigned cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* one UTF-8 sequence at s (n bytes left): code point and length, or -1 if invalid */
static int utf8_seq(const unsigned char *s, size_t n, unsigned *cp) {
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
    if (!len || (size_t)len > n) return -1;
    unsigned v = c & (0x3F >> (len - 1));
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        v = v << 6 | (s[i] & 0x3F);
    }
    if ((len == 3 && (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))) || (len == 4 && (v < 0x10000 || v > 0x10FFFF))) return -1;
    *cp = v;
    return len;
}

int utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned cp;
        int l = utf8_seq(p + i, n - i, &cp);
        if (l < 0) return 0;
        i += (size_t)l;
    }
    return 1;
}

static const uint16_t cp1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

static void norm_charset(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) if (*in != '"' && *in != '\'' && !is_ws((unsigned char)*in)) out[o++] = (char)lc((unsigned char)*in);
    out[o] = 0;
}

char *to_utf8(const char *s, size_t n, const char *charset, size_t *outlen) {
    char cs[40];
    norm_charset(charset ? charset : "", cs, sizeof cs);
    const unsigned char *p = (const unsigned char *)s;
    int kind;               /* 0 utf-8, 1 windows-1252, 2 utf-16le, 3 utf-16be */
    if (!strcmp(cs, "utf-8") || !strcmp(cs, "utf8") || !strcmp(cs, "unicode-1-1-utf-8")) kind = 0;
    else if (!strcmp(cs, "utf-16le") || !strcmp(cs, "utf-16")) kind = 2;
    else if (!strcmp(cs, "utf-16be")) kind = 3;
    else if (!strncmp(cs, "iso-8859-1", 10) || !strcmp(cs, "latin1") || !strcmp(cs, "iso8859-1") || !strncmp(cs, "windows-125", 11) ||
             !strcmp(cs, "us-ascii") || !strcmp(cs, "ascii") || !strcmp(cs, "cp1252") || !strcmp(cs, "l1")) kind = 1;
    else kind = utf8_valid(s, n) ? 0 : 1;   /* unknown: UTF-8 if it is valid, else Windows-1252 */
    if (kind == 1 && strcmp(cs, "windows-1252") && strncmp(cs, "iso-8859-1", 10) && strcmp(cs, "latin1") && strcmp(cs, "cp1252") &&
        strcmp(cs, "us-ascii") && strcmp(cs, "ascii") && strcmp(cs, "l1") && strcmp(cs, "iso8859-1") && cs[0] && utf8_valid(s, n))
        kind = 0;             /* windows-1250/1251... we cannot map: valid UTF-8 is more likely right */
    /* a BOM wins */
    size_t i = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { kind = 0; i = 3; }
    else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { kind = 2; i = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { kind = 3; i = 2; }
    if (n > (SIZE_MAX - 8) / 4) return 0;
    char *o = malloc(n * 3 + 8);
    if (!o) return 0;
    size_t k = 0;
    while (i < n) {
        unsigned cp;
        if (kind == 0) {
            int l = utf8_seq(p + i, n - i, &cp);
            if (l < 0) { cp = 0xFFFD; l = 1; }
            i += (size_t)l;
        } else if (kind == 1) {
            cp = p[i++];
            if (cp >= 0x80 && cp < 0xA0) cp = cp1252[cp - 0x80];
        } else {
            if (i + 1 >= n) break;
            cp = kind == 2 ? (unsigned)(p[i] | p[i + 1] << 8) : (unsigned)(p[i] << 8 | p[i + 1]);
            i += 2;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) {
                unsigned lo = kind == 2 ? (unsigned)(p[i] | p[i + 1] << 8) : (unsigned)(p[i] << 8 | p[i + 1]);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
                else cp = 0xFFFD;
            } else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
        }
        if (cp == 0) cp = 0xFFFD;
        k += (size_t)put_utf8(cp, o + k);
    }
    o[k] = 0;
    *outlen = k;
    return o;
}

void sniff_charset(const char *s, size_t n, char *out, size_t outn) {
    const unsigned char *p = (const unsigned char *)s;
    out[0] = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { strlcpy(out, "utf-8", outn); return; }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { strlcpy(out, "utf-16le", outn); return; }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { strlcpy(out, "utf-16be", outn); return; }
    size_t lim = n < 4096 ? n : 4096;
    const char *m = s;
    while ((m = ci_find(m, lim - (size_t)(m - s), "<meta"))) {
        const char *end = memchr(m, '>', lim - (size_t)(m - s));
        if (!end) break;
        const char *c = ci_find(m, (size_t)(end - m), "charset");
        if (c) {
            c += 7;
            while (c < end && (is_ws((unsigned char)*c) || *c == '=')) c++;
            while (c < end && (*c == '"' || *c == '\'')) c++;
            size_t o = 0;
            while (c < end && *c != '"' && *c != '\'' && *c != ';' && *c != '/' && !is_ws((unsigned char)*c) && o + 1 < outn)
                out[o++] = (char)lc((unsigned char)*c++);
            out[o] = 0;
            if (o) return;
        }
        m = end;
    }
}

/* ---------------- entities ---------------- */
static const char *const latin1_names[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};
static const struct { const char *name; unsigned short cp; } entities[] = {
    { "quot", 34 }, { "amp", 38 }, { "apos", 39 }, { "lt", 60 }, { "gt", 62 }, { "OElig", 338 }, { "oelig", 339 },
    { "Scaron", 352 }, { "scaron", 353 }, { "Yuml", 376 }, { "fnof", 402 }, { "circ", 710 }, { "tilde", 732 },
    { "Alpha", 913 }, { "Beta", 914 }, { "Gamma", 915 }, { "Delta", 916 }, { "Epsilon", 917 }, { "Zeta", 918 }, { "Eta", 919 },
    { "Theta", 920 }, { "Iota", 921 }, { "Kappa", 922 }, { "Lambda", 923 }, { "Mu", 924 }, { "Nu", 925 }, { "Xi", 926 },
    { "Omicron", 927 }, { "Pi", 928 }, { "Rho", 929 }, { "Sigma", 931 }, { "Tau", 932 }, { "Upsilon", 933 }, { "Phi", 934 },
    { "Chi", 935 }, { "Psi", 936 }, { "Omega", 937 }, { "alpha", 945 }, { "beta", 946 }, { "gamma", 947 }, { "delta", 948 },
    { "epsilon", 949 }, { "zeta", 950 }, { "eta", 951 }, { "theta", 952 }, { "iota", 953 }, { "kappa", 954 }, { "lambda", 955 },
    { "mu", 956 }, { "nu", 957 }, { "xi", 958 }, { "omicron", 959 }, { "pi", 960 }, { "rho", 961 }, { "sigmaf", 962 },
    { "sigma", 963 }, { "tau", 964 }, { "upsilon", 965 }, { "phi", 966 }, { "chi", 967 }, { "psi", 968 }, { "omega", 969 },
    { "ensp", 8194 }, { "emsp", 8195 }, { "thinsp", 8201 }, { "zwnj", 8204 }, { "zwj", 8205 }, { "lrm", 8206 }, { "rlm", 8207 },
    { "ndash", 8211 }, { "mdash", 8212 }, { "lsquo", 8216 }, { "rsquo", 8217 }, { "sbquo", 8218 }, { "ldquo", 8220 },
    { "rdquo", 8221 }, { "bdquo", 8222 }, { "dagger", 8224 }, { "Dagger", 8225 }, { "bull", 8226 }, { "hellip", 8230 },
    { "permil", 8240 }, { "prime", 8242 }, { "Prime", 8243 }, { "lsaquo", 8249 }, { "rsaquo", 8250 }, { "oline", 8254 },
    { "frasl", 8260 }, { "euro", 8364 }, { "trade", 8482 }, { "larr", 8592 }, { "uarr", 8593 }, { "rarr", 8594 },
    { "darr", 8595 }, { "harr", 8596 }, { "crarr", 8629 }, { "lArr", 8656 }, { "uArr", 8657 }, { "rArr", 8658 },
    { "dArr", 8659 }, { "hArr", 8660 }, { "forall", 8704 }, { "part", 8706 }, { "exist", 8707 }, { "empty", 8709 },
    { "nabla", 8711 }, { "isin", 8712 }, { "notin", 8713 }, { "ni", 8715 }, { "prod", 8719 }, { "sum", 8721 },
    { "minus", 8722 }, { "lowast", 8727 }, { "radic", 8730 }, { "prop", 8733 }, { "infin", 8734 }, { "ang", 8736 },
    { "and", 8743 }, { "or", 8744 }, { "cap", 8745 }, { "cup", 8746 }, { "int", 8747 }, { "there4", 8756 }, { "sim", 8764 },
    { "cong", 8773 }, { "asymp", 8776 }, { "ne", 8800 }, { "equiv", 8801 }, { "le", 8804 }, { "ge", 8805 }, { "sub", 8834 },
    { "sup", 8835 }, { "nsub", 8836 }, { "sube", 8838 }, { "supe", 8839 }, { "oplus", 8853 }, { "otimes", 8855 },
    { "perp", 8869 }, { "sdot", 8901 }, { "lceil", 8968 }, { "rceil", 8969 }, { "lfloor", 8970 }, { "rfloor", 8971 },
    { "lang", 9001 }, { "rang", 9002 }, { "loz", 9674 }, { "spades", 9824 }, { "clubs", 9827 }, { "hearts", 9829 },
    { "diams", 9830 }, { "check", 10003 }, { "star", 9734 }, { "starf", 9733 }, { "nbhy", 8209 }, { "colon", 58 },
    { "comma", 44 }, { "period", 46 }, { "excl", 33 }, { "quest", 63 }, { "num", 35 }, { "dollar", 36 }, { "percnt", 37 },
    { "lpar", 40 }, { "rpar", 41 }, { "ast", 42 }, { "plus", 43 }, { "sol", 47 }, { "semi", 59 }, { "equals", 61 },
    { "lsqb", 91 }, { "bsol", 92 }, { "rsqb", 93 }, { "lowbar", 95 }, { "grave", 96 }, { "lcub", 123 }, { "verbar", 124 },
    { "rcub", 125 }, { "Tab", 9 }, { "NewLine", 10 }, { "hyphen", 8208 }, { "dash", 8208 }, { "half", 189 },
};

/* a character reference at s ('&' already seen; n bytes after it): the code
 * point and the bytes used, or 0 if it is not one */
static int char_ref(const char *s, size_t n, unsigned *cp, int in_attr) {
    if (!n) return 0;
    if (s[0] == '#') {
        size_t i = 1;
        int hex = i < n && (s[i] == 'x' || s[i] == 'X');
        if (hex) i++;
        unsigned long v = 0;
        size_t start = i;
        for (; i < n; i++) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && (c | 32) >= 'a' && (c | 32) <= 'f') d = (c | 32) - 'a' + 10;
            else break;
            if (v < 0x110000) v = v * (hex ? 16 : 10) + (unsigned)d;
        }
        if (i == start) return 0;
        if (i < n && s[i] == ';') i++;
        if (v >= 0x80 && v < 0xA0) v = cp1252[v - 0x80];
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        *cp = (unsigned)v;
        return (int)i;
    }
    size_t l = 0;
    while (l < n && l < 32 && (is_alpha_c((unsigned char)s[l]) || (s[l] >= '0' && s[l] <= '9'))) l++;
    if (!l) return 0;
    int semi = l < n && s[l] == ';';
    /* without ';' only the old Latin-1 names count, and not before '=' or a letter in attributes */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t k = l; k >= 2; k--) {
            unsigned found = 0;
            for (int i = 0; i < 96 && !found; i++) if (strlen(latin1_names[i]) == k && !memcmp(latin1_names[i], s, k)) found = 160 + (unsigned)i;
            for (size_t i = 0; i < sizeof entities / sizeof entities[0] && !found; i++)
                if (strlen(entities[i].name) == k && !memcmp(entities[i].name, s, k)) found = entities[i].cp;
            if (!found) { if (pass == 0) break; continue; }
            int exact = k == l && semi;
            if (pass == 0 && !exact) break;
            if (pass == 1) {
                if (found > 255 && !(found == 38 || found == 60 || found == 62 || found == 34)) continue;
                if (in_attr && k < n && (s[k] == '=' || is_alpha_c((unsigned char)s[k]) || (s[k] >= '0' && s[k] <= '9'))) return 0;
            }
            *cp = found;
            return (int)(k + (exact ? 1 : 0));
        }
    }
    return 0;
}

/* decode character references in s[0..n) into the arena */
static char *decode_text(arena_t *a, const char *s, size_t n, int in_attr, int *outlen) {
    if (!memchr(s, '&', n) && !memchr(s, '\r', n)) {
        if (outlen) *outlen = (int)n;
        return arena_strndup(a, s, n);
    }
    char *o = arena_alloc(a, n + 1);
    if (!o) return 0;
    size_t k = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            unsigned cp;
            int used = char_ref(s + i + 1, n - i - 1, &cp, in_attr);
            /* the UTF-8 of a reference is never longer than the reference (>= 2 bytes for 1-byte, ...) */
            if (used) {
                char tmp[4];
                int l = put_utf8(cp, tmp);
                if ((size_t)l <= (size_t)used + 1) { memcpy(o + k, tmp, (size_t)l); k += (size_t)l; i += (size_t)used + 1; continue; }
            }
        }
        if (s[i] == '\r') { o[k++] = '\n'; i += i + 1 < n && s[i + 1] == '\n' ? 2 : 1; continue; }
        o[k++] = s[i++];
    }
    o[k] = 0;
    if (outlen) *outlen = (int)k;
    return o;
}

/* ---------------- tags ---------------- */
static const char *const tag_names[T_COUNT] = {
    "", "html", "head", "body", "title", "meta", "link", "base", "style", "script", "noscript",
    "template", "p", "div", "span", "a", "b", "strong", "i", "em", "u", "s", "strike", "del", "ins",
    "small", "big", "sub", "sup", "code", "tt", "kbd", "samp", "var", "pre", "listing", "xmp", "plaintext",
    "br", "wbr", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "menu", "dir",
    "blockquote", "center", "font", "img", "table", "caption", "thead", "tbody", "tfoot", "tr", "td", "th",
    "col", "colgroup", "form", "input", "button", "select", "option", "optgroup", "textarea", "label",
    "fieldset", "legend", "address", "article", "aside", "footer", "header", "main", "nav", "section",
    "figure", "figcaption", "iframe", "object", "embed", "svg", "math", "canvas", "video", "audio", "map",
    "area", "abbr", "cite", "q", "mark", "nobr", "details", "summary", "picture", "source", "noframes",
    "frameset", "frame", "param", "track", "dialog", "hgroup", "dfn", "time", "data", "output", "progress",
    "meter", "selectedcontent", "search",
};

int tag_of(const char *name, size_t len) {
    if (len == 5 && !memcmp(name, "image", 5)) return T_IMG;
    for (int i = 1; i < T_COUNT; i++) if (strlen(tag_names[i]) == len && !memcmp(tag_names[i], name, len)) return i;
    return T_UNKNOWN;
}

static int is_void(int t) {
    return t == T_BR || t == T_HR || t == T_IMG || t == T_INPUT || t == T_META || t == T_LINK || t == T_BASE ||
           t == T_AREA || t == T_COL || t == T_EMBED || t == T_SOURCE || t == T_TRACK || t == T_WBR || t == T_PARAM ||
           t == T_FRAME;
}
/* start tags that close an open <p> */
static int closes_p(int t) {
    switch (t) {
    case T_P: case T_DIV: case T_UL: case T_OL: case T_DL: case T_TABLE: case T_H1: case T_H2: case T_H3: case T_H4:
    case T_H5: case T_H6: case T_PRE: case T_LISTING: case T_BLOCKQUOTE: case T_FORM: case T_HR: case T_ADDRESS:
    case T_ARTICLE: case T_ASIDE: case T_FOOTER: case T_HEADER: case T_MAIN: case T_NAV: case T_SECTION: case T_FIGURE:
    case T_FIELDSET: case T_CENTER: case T_MENU: case T_DIR: case T_DETAILS: case T_SUMMARY: case T_XMP:
    case T_PLAINTEXT: case T_DIALOG: case T_HGROUP: case T_FIGCAPTION: case T_SEARCH:
        return 1;
    }
    return 0;
}
static int is_heading(int t) { return t >= T_H1 && t <= T_H6; }
/* elements an implied end does not reach through */
static int scope_boundary(int t) {
    return t == T_TABLE || t == T_TD || t == T_TH || t == T_HTML || t == T_BUTTON || t == T_OBJECT || t == T_CAPTION ||
           t == T_TEMPLATE;
}

const char *node_attr(const node_t *n, const char *name) {
    if (!n) return 0;
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return 0;
}

/* ---------------- the tree builder ---------------- */
typedef struct {
    doc_t *d;
    node_t *stack[DOC_MAX_DEPTH + 1];
    int depth;                  /* stack[0] is the root */
    node_t *form;               /* the open <form> */
    int in_pre;                 /* a newline right after <pre> is dropped */
} tb_t;

static node_t *new_node(tb_t *t) {
    if (t->d->nnodes >= DOC_MAX_NODES) return 0;
    node_t *n = arena_alloc(&t->d->ar, sizeof *n);
    if (!n) return 0;
    n->min_w = n->max_w = -1;
    t->d->nnodes++;
    return n;
}
static void append(node_t *parent, node_t *n) {
    n->parent = parent;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
}
static node_t *cur(tb_t *t) { return t->stack[t->depth]; }
static int cur_tag(tb_t *t) { return cur(t)->tag; }
static void pop(tb_t *t) { if (t->depth > 0) t->depth--; }
/* pop up to and including the nearest open tag (not through a boundary); 1 if found */
static int close_tag(tb_t *t, int tag, int respect_scope) {
    for (int i = t->depth; i > 0; i--) {
        int st = t->stack[i]->tag;
        if (st == tag) { t->depth = i - 1; return 1; }
        if (respect_scope && scope_boundary(st)) return 0;
    }
    return 0;
}
static int open_in_scope(tb_t *t, int tag) {
    for (int i = t->depth; i > 0; i--) {
        if (t->stack[i]->tag == tag) return 1;
        if (scope_boundary(t->stack[i]->tag)) return 0;
    }
    return 0;
}

static void add_text(tb_t *t, const char *s, size_t n) {
    if (!n) return;
    node_t *p = cur(t);
    int pt = p->tag;
    /* text directly in a table or a row is only white space in sane pages: dropped */
    if (pt == T_TABLE || pt == T_TBODY || pt == T_THEAD || pt == T_TFOOT || pt == T_TR || pt == T_SELECT || pt == T_COLGROUP) {
        size_t i = 0;
        while (i < n && is_ws((unsigned char)s[i])) i++;
        if (i == n) return;
    }
    if (t->in_pre) {
        t->in_pre = 0;
        if (s[0] == '\n') { s++; n--; }
        else if (s[0] == '\r' && n > 1 && s[1] == '\n') { s += 2; n -= 2; }
        if (!n) return;
    }
    int len;
    char *txt = decode_text(&t->d->ar, s, n, 0, &len);
    if (!txt) return;
    node_t *last = p->last;
    if (last && last->is_text) {
        /* merge with the text before it */
        char *m = arena_alloc(&t->d->ar, (size_t)last->textlen + (size_t)len + 1);
        if (!m) return;
        memcpy(m, last->text, (size_t)last->textlen);
        memcpy(m + last->textlen, txt, (size_t)len);
        last->text = m;
        last->textlen += len;
        return;
    }
    node_t *x = new_node(t);
    if (!x) return;
    x->is_text = 1;
    x->text = txt;
    x->textlen = len;
    append(p, x);
}

static void body_attrs(tb_t *t, attr_t *attrs, int na) {
    doc_t *d = t->d;
    for (int i = 0; i < na; i++) {
        uint32_t c;
        if (!strcmp(attrs[i].name, "bgcolor") && parse_color(attrs[i].value, &c)) { d->bg = c; d->has_bg = 1; }
        else if (!strcmp(attrs[i].name, "text") && parse_color(attrs[i].value, &c)) d->fg = c;
        else if (!strcmp(attrs[i].name, "link") && parse_color(attrs[i].value, &c)) d->link_color = c;
        else if (!strcmp(attrs[i].name, "vlink") && parse_color(attrs[i].value, &c)) d->vlink_color = c;
    }
}

static void start_tag(tb_t *t, int tag, const char *name, size_t namelen, attr_t *attrs, int na, int selfclose) {
    doc_t *d = t->d;
    if (tag == T_HTML || tag == T_HEAD) return;
    if (tag == T_BODY) { body_attrs(t, attrs, na); return; }
    if (tag == T_BASE) {
        for (int i = 0; i < na; i++) if (!strcmp(attrs[i].name, "href") && attrs[i].value[0]) {
            /* resolved by the caller against the document URL (html_parse) */
            strlcpy(d->base, attrs[i].value, sizeof d->base);
            d->base[sizeof d->base - 1] = 0;
        }
        return;
    }
    /* implied end tags */
    int ct = cur_tag(t);
    if (closes_p(tag) && open_in_scope(t, T_P)) close_tag(t, T_P, 1);
    if (is_heading(tag) && is_heading(cur_tag(t))) pop(t);
    if (tag == T_LI) {
        for (int i = t->depth; i > 0; i--) {
            int st = t->stack[i]->tag;
            if (st == T_LI) { t->depth = i - 1; break; }
            if (st == T_UL || st == T_OL || st == T_MENU || st == T_DIR || scope_boundary(st)) break;
        }
    } else if (tag == T_DT || tag == T_DD) {
        for (int i = t->depth; i > 0; i--) {
            int st = t->stack[i]->tag;
            if (st == T_DT || st == T_DD) { t->depth = i - 1; break; }
            if (st == T_DL || scope_boundary(st)) break;
        }
    } else if (tag == T_OPTION || tag == T_OPTGROUP) {
        if (cur_tag(t) == T_OPTION) pop(t);
        if (tag == T_OPTGROUP && cur_tag(t) == T_OPTGROUP) pop(t);
    } else if (tag == T_TR) {
        for (int i = t->depth; i > 0; i--) {
            int st = t->stack[i]->tag;
            if (st == T_TR) { t->depth = i - 1; break; }
            if (st == T_TABLE || st == T_TBODY || st == T_THEAD || st == T_TFOOT) break;
        }
    } else if (tag == T_TD || tag == T_TH) {
        for (int i = t->depth; i > 0; i--) {
            int st = t->stack[i]->tag;
            if (st == T_TD || st == T_TH) { t->depth = i - 1; break; }
            if (st == T_TR || st == T_TABLE) break;
        }
    } else if (tag == T_THEAD || tag == T_TBODY || tag == T_TFOOT || tag == T_CAPTION || tag == T_COLGROUP) {
        for (int i = t->depth; i > 0; i--) {
            int st = t->stack[i]->tag;
            if (st == T_TABLE) { t->depth = i; break; }
        }
    } else if (tag == T_A) {
        close_tag(t, T_A, 1);
    } else if (tag == T_BUTTON && open_in_scope(t, T_BUTTON)) {
        close_tag(t, T_BUTTON, 0);
    } else if (tag == T_SELECT && open_in_scope(t, T_SELECT)) {
        close_tag(t, T_SELECT, 0);
        return;
    } else if (tag == T_FORM && t->form) {
        return;                                   /* no forms in forms */
    } else if (tag == T_NOBR) {
        close_tag(t, T_NOBR, 1);
    }
    /* a table cell or row outside a table: implied parents are not built; it still shows */
    (void)ct;
    node_t *n = new_node(t);
    if (!n) return;
    n->tag = (uint16_t)tag;
    if (tag == T_UNKNOWN) {
        /* custom elements: keep the name as an attribute-less inline element */
        (void)name; (void)namelen;
    }
    if (na) {
        n->attrs = arena_alloc(&d->ar, sizeof(attr_t) * (size_t)na);
        if (n->attrs) { memcpy(n->attrs, attrs, sizeof(attr_t) * (size_t)na); n->nattrs = na; }
    }
    append(cur(t), n);
    if (tag == T_FORM) t->form = n;
    if (tag == T_META) {
        const char *he = node_attr(n, "http-equiv"), *ct2 = node_attr(n, "content");
        if (he && ct2 && !strcmp(he, "refresh")) {
            const char *u = ci_find(ct2, strlen(ct2), "url=");
            if (u) {
                u += 4;
                while (*u == '\'' || *u == '"' || *u == ' ') u++;
                size_t l = strlen(u);
                while (l && (u[l - 1] == '\'' || u[l - 1] == '"' || u[l - 1] == ' ')) l--;
                d->refresh_url = arena_strndup(&d->ar, u, l);
            }
        }
    }
    if (is_void(tag) || selfclose) return;
    if (t->depth < DOC_MAX_DEPTH) t->stack[++t->depth] = n;
    if (tag == T_PRE || tag == T_LISTING || tag == T_TEXTAREA) t->in_pre = 1;
}

static void end_tag(tb_t *t, int tag) {
    t->in_pre = 0;
    switch (tag) {
    case T_HTML: case T_HEAD: case T_BODY: return;
    case T_BR: { attr_t none; start_tag(t, T_BR, "br", 2, &none, 0, 1); return; }
    case T_P:
        if (!close_tag(t, T_P, 1)) { attr_t none; start_tag(t, T_P, "p", 1, &none, 0, 1); }
        return;
    case T_FORM:
        t->form = 0;
        close_tag(t, T_FORM, 1);
        return;
    case T_UNKNOWN:
        return;
    }
    if (is_heading(tag)) {
        for (int i = t->depth; i > 0; i--) {
            if (is_heading(t->stack[i]->tag)) { t->depth = i - 1; return; }
            if (scope_boundary(t->stack[i]->tag)) return;
        }
        return;
    }
    /* table structure ends close what is inside them */
    int scoped = !(tag == T_TABLE || tag == T_TD || tag == T_TH || tag == T_BUTTON || tag == T_CAPTION || tag == T_OBJECT);
    close_tag(t, tag, scoped);
}

/* the end of a raw text element (</script...) from s; returns its position or len */
static size_t raw_end(const char *s, size_t len, size_t i, const char *name) {
    size_t nl = strlen(name);
    for (; i + 2 + nl <= len; i++) {
        if (s[i] == '<' && s[i + 1] == '/' && ci_eq(s + i + 2, nl, name)) {
            int c = i + 2 + nl < len ? (unsigned char)s[i + 2 + nl] : '>';
            if (c == '>' || is_ws(c) || c == '/') return i;
        }
    }
    return len;
}

static void tokenize(tb_t *t, const char *s, size_t len) {
    doc_t *d = t->d;
    size_t i = 0, text_start = 0;
    attr_t attrs[DOC_MAX_ATTRS];
    while (i < len) {
        if (s[i] != '<') { i++; continue; }
        /* "<" starts markup only before a letter, '/', '!' or '?' */
        int c1 = i + 1 < len ? (unsigned char)s[i + 1] : 0;
        if (!(is_alpha_c(c1) || c1 == '/' || c1 == '!' || c1 == '?')) { i++; continue; }
        add_text(t, s + text_start, i - text_start);
        if (c1 == '!') {
            if (i + 3 < len && s[i + 2] == '-' && s[i + 3] == '-') {
                const char *e = 0;
                for (size_t k = i + 4; k + 2 < len + 0 && !e; k++) if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') e = s + k;
                if (i + 4 < len && s[i + 4] == '>') e = s + i + 2;          /* <!--> */
                i = e ? (size_t)(e - s) + 3 : len;
            } else {
                const char *e = memchr(s + i, '>', len - i);
                i = e ? (size_t)(e - s) + 1 : len;
            }
            text_start = i;
            continue;
        }
        if (c1 == '?') {
            const char *e = memchr(s + i, '>', len - i);
            i = e ? (size_t)(e - s) + 1 : len;
            text_start = i;
            continue;
        }
        int end = c1 == '/';
        size_t p = i + 1 + (size_t)end;
        if (end && (p >= len || !is_alpha_c((unsigned char)s[p]))) {
            /* "</>" or "</ x>": a bogus comment */
            const char *e = memchr(s + p, '>', len - p);
            i = e ? (size_t)(e - s) + 1 : len;
            text_start = i;
            continue;
        }
        size_t ns = p;
        while (p < len && !is_ws((unsigned char)s[p]) && s[p] != '/' && s[p] != '>') p++;
        char name[24];
        size_t nl = p - ns;
        int tag = T_UNKNOWN;
        if (nl < sizeof name) {
            for (size_t k = 0; k < nl; k++) name[k] = (char)lc((unsigned char)s[ns + k]);
            name[nl] = 0;
            tag = tag_of(name, nl);
        } else name[0] = 0;
        /* attributes */
        int na = 0, selfclose = 0;
        for (;;) {
            while (p < len && (is_ws((unsigned char)s[p]) || (s[p] == '/' && !(p + 1 < len && s[p + 1] == '>')))) p++;
            if (p >= len) break;
            if (s[p] == '>') { p++; break; }
            if (s[p] == '/' ) { selfclose = 1; p += 2; break; }
            size_t as = p;
            while (p < len && !is_ws((unsigned char)s[p]) && s[p] != '/' && s[p] != '>' && (s[p] != '=' || p == as)) p++;
            size_t al = p - as;
            while (p < len && is_ws((unsigned char)s[p])) p++;
            const char *vs = 0;
            size_t vl = 0;
            if (p < len && s[p] == '=') {
                p++;
                while (p < len && is_ws((unsigned char)s[p])) p++;
                if (p < len && (s[p] == '"' || s[p] == '\'')) {
                    char q = s[p++];
                    vs = s + p;
                    const char *e = memchr(s + p, q, len - p);
                    vl = e ? (size_t)(e - vs) : len - p;
                    p += vl + (e ? 1 : 0);
                } else {
                    vs = s + p;
                    while (p < len && !is_ws((unsigned char)s[p]) && s[p] != '>') p++;
                    vl = (size_t)(s + p - vs);
                }
            }
            if (end || na >= DOC_MAX_ATTRS || al == 0 || al > 64) continue;
            char an[65];
            for (size_t k = 0; k < al; k++) an[k] = (char)lc((unsigned char)s[as + k]);
            an[al] = 0;
            int dup = 0;
            for (int k = 0; k < na; k++) if (!strcmp(attrs[k].name, an)) dup = 1;
            if (dup) continue;
            attrs[na].name = arena_strndup(&d->ar, an, al);
            attrs[na].value = vs ? decode_text(&d->ar, vs, vl, 1, 0) : arena_strndup(&d->ar, "", 0);
            if (attrs[na].name && attrs[na].value) na++;
        }
        i = p;
        text_start = i;
        if (end) { end_tag(t, tag); continue; }
        if (!name[0]) continue;
        start_tag(t, tag, name, nl, attrs, na, selfclose && tag != T_SCRIPT && tag != T_STYLE && tag != T_TITLE && tag != T_TEXTAREA);
        /* raw text elements */
        if (selfclose && tag != T_SCRIPT && tag != T_TITLE && tag != T_TEXTAREA && tag != T_STYLE) continue;
        if (tag == T_SCRIPT || tag == T_STYLE || tag == T_XMP || tag == T_IFRAME || tag == T_TITLE || tag == T_TEXTAREA ||
            tag == T_PLAINTEXT || tag == T_SVG || tag == T_MATH || tag == T_TEMPLATE) {
            size_t e = tag == T_PLAINTEXT ? len : raw_end(s, len, i, name);
            if (tag == T_TITLE) {
                if (!d->title) {
                    int tl;
                    d->title = decode_text(&d->ar, s + i, e - i, 0, &tl);
                }
            } else if (tag == T_TEXTAREA || tag == T_XMP || tag == T_PLAINTEXT) {
                if (tag == T_TEXTAREA) add_text(t, s + i, e - i);
                else {
                    /* shown as it is (no references) */
                    node_t *x = new_node(t);
                    if (x) { x->is_text = 1; x->text = arena_strndup(&d->ar, s + i, e - i); x->textlen = x->text ? (int)(e - i) : 0; append(cur(t), x); }
                }
            }
            /* the element's end */
            close_tag(t, tag, 0);
            if (e < len) {
                const char *gt = memchr(s + e, '>', len - e);
                i = gt ? (size_t)(gt - s) + 1 : len;
            } else i = len;
            text_start = i;
        }
    }
    add_text(t, s + text_start, len - text_start);
}

static doc_t *doc_new(const char *url) {
    doc_t *d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->ar.limit = 64u << 20;
    d->fg = 0x000000;
    d->bg = 0xFFFFFF;
    d->link_color = 0x0000EE;
    d->vlink_color = 0x551A8B;
    if (url) strlcpy(d->base, url, sizeof d->base);
    d->root = arena_alloc(&d->ar, sizeof(node_t));
    if (!d->root) { free(d); return 0; }
    d->root->tag = T_BODY;
    d->root->min_w = d->root->max_w = -1;
    return d;
}

doc_t *html_parse(const char *data, size_t len, const char *http_charset, const char *url) {
    char cs[40] = "";
    if (http_charset && http_charset[0]) strlcpy(cs, http_charset, sizeof cs);
    char meta[40];
    sniff_charset(data, len, meta, sizeof meta);
    /* a BOM beats the HTTP header, the header beats <meta> */
    if (meta[0] && (!cs[0] || (len >= 2 && ((unsigned char)data[0] == 0xEF || (unsigned char)data[0] == 0xFF || (unsigned char)data[0] == 0xFE))))
        strlcpy(cs, meta, sizeof cs);
    size_t ul;
    char *u8 = to_utf8(data, len, cs, &ul);
    if (!u8) return 0;
    doc_t *d = doc_new(url);
    if (!d) { free(u8); return 0; }
    tb_t *t = calloc(1, sizeof *t);
    if (!t) { free(u8); doc_free(d); return 0; }
    t->d = d;
    t->stack[0] = d->root;
    tokenize(t, u8, ul);
    free(t);
    free(u8);
    return d;
}

doc_t *text_doc(const char *data, size_t len, const char *charset, const char *url) {
    size_t ul;
    char *u8 = to_utf8(data, len, charset, &ul);
    if (!u8) return 0;
    doc_t *d = doc_new(url);
    if (d) {
        node_t *pre = arena_alloc(&d->ar, sizeof(node_t)), *tx = arena_alloc(&d->ar, sizeof(node_t));
        char *s = arena_strndup(&d->ar, u8, ul);
        if (pre && tx && s) {
            pre->tag = T_PRE;
            pre->min_w = pre->max_w = tx->min_w = tx->max_w = -1;
            tx->is_text = 1;
            /* CR LF -> LF */
            size_t k = 0;
            for (size_t i = 0; i < ul; i++) if (!(s[i] == '\r' && i + 1 < ul && s[i + 1] == '\n')) s[k++] = s[i] == '\r' ? '\n' : s[i];
            s[k] = 0;
            tx->text = s;
            tx->textlen = (int)k;
            append(d->root, pre);
            append(pre, tx);
            d->nnodes = 2;
        }
    }
    free(u8);
    return d;
}

void doc_free(doc_t *d) {
    if (!d) return;
    for (int i = 0; i < d->nimages; i++) free(d->images[i].px);
    for (int i = 0; i < d->nctls; i++) {
        fctl_t *c = d->ctls[i];
        free(c->value);
        for (int k = 0; k < c->noptions; k++) {
            if (c->opt_label) free(c->opt_label[k]);
            if (c->opt_value) free(c->opt_value[k]);
        }
        free(c->opt_label);
        free(c->opt_value);
        free(c);
    }
    free(d->ctls);
    free(d->items);
    free(d->anchors);
    arena_free(&d->ar);
    free(d);
}

/* ---------------- colors ---------------- */
int parse_color(const char *s, uint32_t *out) {
    static const struct { const char *n; uint32_t c; } names[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
        { "navy", 0x000080 }, { "purple", 0x800080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
        { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "orange", 0xFFA500 },
        { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "gold", 0xFFD700 }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
        { "darkgreen", 0x006400 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 },
        { "lightblue", 0xADD8E6 }, { "lightyellow", 0xFFFFE0 }, { "whitesmoke", 0xF5F5F5 }, { "beige", 0xF5F5DC },
        { "ivory", 0xFFFFF0 }, { "lavender", 0xE6E6FA }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 },
        { "crimson", 0xDC143C }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "tan", 0xD2B48C }, { "salmon", 0xFA8072 },
        { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 }, { "khaki", 0xF0E68C }, { "orchid", 0xDA70D6 }, { "skyblue", 0x87CEEB },
        { "gainsboro", 0xDCDCDC }, { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "dimgray", 0x696969 }, { "slategray", 0x708090 },
    };
    while (*s == ' ') s++;
    int bare = strlen(s) == 6;
    for (int i = 0; i < 6 && bare; i++) bare = (s[i] >= '0' && s[i] <= '9') || ((s[i] | 32) >= 'a' && (s[i] | 32) <= 'f');
    if (*s == '#' || bare) {
        if (*s == '#') s++;
        size_t l = 0;
        while (s[l] && s[l] != ' ' && s[l] != ';') l++;
        unsigned v = 0;
        for (size_t i = 0; i < l; i++) {
            int c = (unsigned char)s[i], dgt = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (dgt < 0) return 0;
            v = v << 4 | (unsigned)dgt;
        }
        if (l == 3) v = ((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11;
        else if (l == 4) { v >>= 4; v = ((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11; }
        else if (l == 8) v >>= 8;
        else if (l != 6) return 0;
        *out = v & 0xFFFFFF;
        return 1;
    }
    if (!strncmp(s, "rgb", 3)) {
        const char *p = strchr(s, '(');
        if (!p) return 0;
        int c[3] = { 0, 0, 0 };
        p++;
        for (int k = 0; k < 3; k++) {
            while (*p == ' ' || *p == ',') p++;
            int v = 0, digits = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); digits++; if (v > 1000) v = 1000; }
            if (!digits) return 0;
            if (*p == '%') { v = v * 255 / 100; p++; }
            while (*p == '.' || (*p >= '0' && *p <= '9')) p++;
            c[k] = v > 255 ? 255 : v;
        }
        *out = (uint32_t)(c[0] << 16 | c[1] << 8 | c[2]);
        return 1;
    }
    char n[24];
    size_t l = 0;
    while (s[l] && s[l] != ' ' && s[l] != ';' && s[l] != '!' && l + 1 < sizeof n) { n[l] = (char)lc((unsigned char)s[l]); l++; }
    n[l] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(names[i].n, n)) { *out = names[i].c; return 1; }
    return 0;
}
