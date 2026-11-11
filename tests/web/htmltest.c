/* Host test of the browser's HTML parser and layout (browser/html.c,
 * layout.c) with made-up font metrics: what the tree and the display list
 * of some pages must contain, character sets and entities, and random tag
 * soup and mutations of the sample pages (run under ASan and UBSan).
 *   htmltest PAGE...   (the sample pages in rootfs/Documents/Web) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int u8len(const char *s, int n) { int k = 0; for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) k++; return k; }
static int tw(const char *s, int n, int font) { return u8len(s, n) * (font == 5 ? 18 : font == 4 ? 11 : 6); }
static int fh(int font) { return font == 5 ? 40 : font == 4 ? 24 : 14; }
static int fa(int font) { return fh(font) * 4 / 5; }
static int fit(const char *s, int n, int font, int w) {
    int per = font == 5 ? 18 : font == 4 ? 11 : 6, k = 0, i = 0;
    while (i < n && (k + 1) * per <= w) { i++; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++; k++; }
    return i;
}

/* all text of the display list, words joined by spaces */
static void items_text(doc_t *d, char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < d->nitems && o + 2 < n; i++) {
        if (d->items[i].kind != IT_TEXT) continue;
        int l = d->items[i].len;
        if (o + (size_t)l + 2 >= n) break;
        memcpy(out + o, d->items[i].text, (size_t)l);
        o += (size_t)l;
        out[o++] = ' ';
        out[o] = 0;
    }
}

static void check_items(doc_t *d, const char *what) {
    for (int i = 0; i < d->nitems; i++) {
        item_t *it = &d->items[i];
        if (it->y < 0 || it->y > d->height + 50 || it->w < 0 || it->h < 0) {
            CHECK(0, "%s: item %d (kind %d) at y=%d h=%d, page height %d", what, i, it->kind, it->y, it->h, d->height);
            return;
        }
    }
}

static doc_t *parse_layout(const char *html, int width) {
    doc_t *d = html_parse(html, strlen(html), 0, "http://example.com/dir/page.html");
    if (d) layout_doc(d, width);
    return d;
}

static unsigned long long rng = 12345;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }

int main(int argc, char **argv) {
    g_metrics.text_width = tw;
    g_metrics.font_height = fh;
    g_metrics.font_ascent = fa;
    g_metrics.text_fit = fit;
    static char buf[1 << 20];

    /* structure, entities, hidden content */
    doc_t *d = parse_layout("<html><head><title>T &amp; t</title><script>var x = '<p>no</p>';</script><style>p{}</style></head>"
                            "<body><p>One<p>Two &lt;b&gt; &eacute;&#233;&#xE9; &copy &zzzz; &notit;<div style='display:none'>hidden</div>"
                            "<ul><li>a<li>b</ul><table><tr><td>c1<td>c2</table><!-- comment --><noscript>ns</noscript></body></html>", 500);
    CHECK(d && d->title && !strcmp(d->title, "T & t"), "title: %s", d && d->title ? d->title : "(none)");
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "One") && strstr(buf, "Two"), "paragraphs: %s", buf);
    CHECK(strstr(buf, "<b>") && strstr(buf, "\xc3\xa9\xc3\xa9\xc3\xa9") && strstr(buf, "\xc2\xa9") && strstr(buf, "&zzzz;") && strstr(buf, "\xc2\xacit;"), "entities: %s", buf);
    CHECK(!strstr(buf, "hidden") && !strstr(buf, "var x") && !strstr(buf, "no ") && !strstr(buf, "comment"), "hidden text shown: %s", buf);
    CHECK(strstr(buf, "c1") && strstr(buf, "c2") && strstr(buf, "ns"), "table / noscript: %s", buf);
    /* <p>One<p>Two: two sibling paragraphs */
    int np = 0;
    for (node_t *k = d->root->first; k; k = k->next) if (!k->is_text && k->tag == T_P) np++;
    CHECK(np == 2, "implied </p>: %d paragraphs at the top", np);
    /* the two list items are siblings */
    int nli = 0;
    for (node_t *k = d->root->first; k; k = k->next)
        if (!k->is_text && k->tag == T_P)
            for (node_t *q = k->first; q; q = q->next)
                if (!q->is_text && q->tag == T_UL) for (node_t *li = q->first; li; li = li->next) nli += !li->is_text && li->tag == T_LI;
    CHECK(nli == 0, "a <ul> inside <p>");
    for (node_t *k = d->root->first; k; k = k->next)
        if (!k->is_text && k->tag == T_UL) for (node_t *li = k->first; li; li = li->next) nli += !li->is_text && li->tag == T_LI;
    CHECK(nli == 2, "implied </li>: %d items", nli);
    doc_free(d);

    /* character sets: <meta> Latin-1, HTTP charset, BOM, invalid UTF-8 */
    const char l1[] = "<meta charset=\"iso-8859-1\"><p>caf\xe9 \x80";
    d = html_parse(l1, sizeof l1 - 1, 0, "file:///x");
    layout_doc(d, 400);
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "caf\xc3\xa9") && strstr(buf, "\xe2\x82\xac"), "latin-1: %s", buf);
    doc_free(d);
    const char u8[] = "<p>caf\xc3\xa9 \xff";
    d = html_parse(u8, sizeof u8 - 1, "utf-8", "file:///x");
    layout_doc(d, 400);
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "caf\xc3\xa9") && strstr(buf, "\xef\xbf\xbd"), "utf-8 with a bad byte: %s", buf);
    doc_free(d);
    d = html_parse(u8, sizeof u8 - 1, "windows-1252", "file:///x");
    layout_doc(d, 400);
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "caf\xc3\x83\xc2\xa9"), "an HTTP charset beats sniffing: %s", buf);
    doc_free(d);
    const char bom[] = "\xef\xbb\xbf<p>x\xc3\xa9";
    d = html_parse(bom, sizeof bom - 1, "iso-8859-1", "file:///x");
    layout_doc(d, 400);
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "x\xc3\xa9") != 0, "a BOM beats the HTTP charset: %s", buf);
    doc_free(d);

    /* line breaking: a long paragraph at width 200 has several lines, none wider than the box */
    d = parse_layout("<p>aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo ppp qqq rrr sss ttt uuu vvv www xxx</p>"
                     "<p>averyveryveryveryveryveryveryveryverylongwordthatmustbesplit</p><pre>  two  spaces\nline</pre>", 200);
    int maxx = 0, lines = 0, lasty = -1;
    for (int i = 0; i < d->nitems; i++) {
        item_t *it = &d->items[i];
        if (it->kind != IT_TEXT) continue;
        if (it->x + it->w > maxx) maxx = it->x + it->w;
        if (it->y != lasty) { lines++; lasty = it->y; }
    }
    CHECK(maxx <= 200 - 8, "a line overflows: right edge %d", maxx);
    CHECK(lines >= 6, "only %d lines", lines);
    items_text(d, buf, sizeof buf);
    CHECK(strstr(buf, "  two  spaces") != 0, "pre keeps spaces: %s", buf);
    doc_free(d);

    /* tables: cells side by side, a 100%-wide table fills the width */
    d = parse_layout("<table width=100% border=1><tr><td>left</td><td>right</td></tr></table>", 600);
    int xl = -1, xr = -1, yl = -1, yr = -2;
    for (int i = 0; i < d->nitems; i++) {
        item_t *it = &d->items[i];
        if (it->kind == IT_TEXT && it->len == 4 && !memcmp(it->text, "left", 4)) { xl = it->x; yl = it->y; }
        if (it->kind == IT_TEXT && it->len == 5 && !memcmp(it->text, "right", 5)) { xr = it->x; yr = it->y; }
    }
    CHECK(xl >= 0 && xr > xl + 100 && yl == yr, "table cells: left %d,%d right %d,%d", xl, yl, xr, yr);
    doc_free(d);

    /* links and anchors */
    d = parse_layout("<p><a href='x.html'>link</a> <a name='here'>anchor</a></p><p id='p2'>second</p>", 400);
    int linked = 0;
    for (int i = 0; i < d->nitems; i++) if (d->items[i].kind == IT_TEXT && d->items[i].link) linked++;
    CHECK(linked == 1, "%d linked runs", linked);
    CHECK(anchor_y(d, "here") >= 0 && anchor_y(d, "p2") > anchor_y(d, "here"), "anchors %d %d", anchor_y(d, "here"), anchor_y(d, "p2"));
    doc_free(d);

    /* forms: controls are made with their values */
    d = parse_layout("<form><input name=a value=1><input type=checkbox name=c checked><select name=s><option>x<option selected value=v>y</select>"
                     "<textarea name=t>hi &amp; bye</textarea><input type=hidden name=h value=z><button>Go</button></form>", 600);
    CHECK(d->nctls == 6, "%d controls", d->nctls);
    if (d->nctls == 6) {
        CHECK(!strcmp(d->ctls[0]->value, "1") && d->ctls[1]->checked && d->ctls[2]->selected == 1 && !strcmp(d->ctls[2]->opt_value[1], "v") &&
              !strcmp(d->ctls[3]->value, "hi & bye") && d->ctls[4]->kind == FC_HIDDEN && !strcmp(d->ctls[5]->value, "Go") &&
              d->ctls[0]->form != 0, "control values");
    }
    doc_free(d);

    /* deep nesting and huge input stay bounded */
    char *deep = malloc(500000);
    size_t o = 0;
    for (int i = 0; i < 20000; i++) o += (size_t)sprintf(deep + o, "<div><table><tr><td>");
    deep[o] = 0;
    d = parse_layout(deep, 800);
    CHECK(d != 0, "deep nesting");
    if (d) { check_items(d, "deep"); doc_free(d); }
    free(deep);

    /* the sample pages at many widths */
    char *pages[16];
    size_t plen[16];
    int npages = 0;
    for (int i = 1; i < argc && npages < 16; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long l = ftell(f);
        fseek(f, 0, SEEK_SET);
        pages[npages] = malloc((size_t)l + 1);
        plen[npages] = fread(pages[npages], 1, (size_t)l, f);
        fclose(f);
        npages++;
    }
    for (int p = 0; p < npages; p++) {
        for (int w = 60; w <= 1400; w += 67) {
            d = html_parse(pages[p], plen[p], 0, "file:///C:/Documents/Web/x.htm");
            CHECK(d != 0, "page %s", argv[p + 1]);
            if (!d) continue;
            layout_doc(d, w);
            check_items(d, argv[p + 1]);
            doc_free(d);
        }
    }
    /* mutations of the pages and random tag soup */
    static const char *frag[] = { "<p>", "</p>", "<table>", "<tr>", "<td>", "</td>", "</table>", "<ul>", "<li>", "<ol>", "</ul>",
                                  "<b>", "</i>", "<a href=x>", "</a>", "<img src=a.png alt=x width=99999 height=-5>", "<br>",
                                  "<pre>", "\n", " text ", "&amp;", "&#x110000;", "&#0;", "<!--", "-->", "<script>", "</script>",
                                  "<input>", "<select><option>", "<textarea>", "<td colspan=999>", "<font size=+9>", "<h1>", "<hr width=150%>",
                                  "<div style='display:none'>", "<style>", "</style>", "<", ">", "\"", "'", "&", "<li value=-2147483648>",
                                  "<ol start=2147483647><li><li>", "<table cellspacing=9999 cellpadding=9999 border=99>",
                                  "<td width=100000>", "<p align=center>", "\xff\xfe", "\xc3", "<base href='//evil/'>", "<meta charset=utf-16>" };
    for (int it = 0; it < 3000; it++) {
        size_t cap = 20000, n = 0;
        char *s = malloc(cap);
        if (npages && it % 2) {
            int p = (int)(rnd() % (unsigned)npages);
            n = plen[p] < cap ? plen[p] : cap;
            memcpy(s, pages[p], n);
            for (int k = 0; k < 10; k++) {
                size_t pos = rnd() % (n ? n : 1);
                const char *f = frag[rnd() % (sizeof frag / sizeof frag[0])];
                size_t fl = strlen(f);
                if (n + fl < cap) { memmove(s + pos + fl, s + pos, n - pos); memcpy(s + pos, f, fl); n += fl; }
                if (n) s[rnd() % n] = (char)rnd();
            }
        } else {
            while (n < cap - 200) {
                const char *f = frag[rnd() % (sizeof frag / sizeof frag[0])];
                size_t fl = strlen(f);
                memcpy(s + n, f, fl);
                n += fl;
                if (rnd() % 50 == 0) break;
            }
        }
        d = html_parse(s, n, rnd() % 2 ? "utf-8" : 0, "http://example.com/a/");
        if (d) {
            layout_doc(d, 50 + (int)(rnd() % 1500));
            check_items(d, "mutation");
            doc_free(d);
        }
        free(s);
        checks++;
    }
    for (int p = 0; p < npages; p++) free(pages[p]);
    printf("html: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
