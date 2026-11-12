/* Host tests of NOTEPAD.EXE's text model and undo engine
 * (userland/programs/notepad/doc.c, undo.c), built with ASan/UBSan:
 *   - row breaking: words wrap at spaces, long words are broken, tabs,
 *     spaces hang, the LINE_BYTES limit, UTF-8 and invalid bytes;
 *   - random edits on random text with and without word wrap, with the
 *     wrap width changed in between and the stale rows rebuilt a slice at a
 *     time while editing goes on: after every step the rows must be valid
 *     (sorted, a hard row at every line start, soft flags and counts right,
 *     line numbers right) and, once nothing is stale, equal to rows made
 *     from scratch; the text must equal a plain copy;
 *   - undo and redo: random edits, undos and redos checked against copies of
 *     the text at each step; how typing, Backspace and Delete are grouped;
 *     the selection restored; the saved state; the memory limit;
 *   - a 10 MB file: wrapping it in slices, edits in the middle of it.
 *   notepadtest [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "doc.h"
#include "undo.h"

static int fails, checks, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 30) { printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)rng; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* made-up metrics: ASCII 7 px, 'W' and 'M' 11, other characters 9 */
static int cw(unsigned cp) { return cp == 'W' || cp == 'M' ? 11 : cp < 128 ? 7 : 9; }

/* ---------------- a plain copy of the text ---------------- */
static char *ref;
static int reflen, refcap;
static void ref_replace(int a, int b, const char *s, int n) {
    if (reflen - (b - a) + n + 1 > refcap) { refcap = (reflen - (b - a) + n + 1) * 2; ref = realloc(ref, (size_t)refcap); }
    memmove(ref + a + n, ref + b, (size_t)(reflen - b));
    if (n) memcpy(ref + a, s, (size_t)n);
    reflen += n - (b - a);
}
static int text_equal(void) {
    if (tlen() != reflen) return 0;
    static char *buf;
    static int cap;
    if (reflen > cap) { cap = reflen * 2 + 16; buf = realloc(buf, (size_t)cap); }
    text_get(0, reflen, buf);
    return !memcmp(buf, ref, (size_t)reflen);
}

/* ---------------- row checks ---------------- */
/* the rows of the whole text made from scratch */
static int *rows, nrows, rowcap;
static void push_row(int v) { if (nrows == rowcap) { rowcap = rowcap * 2 + 1024; rows = realloc(rows, (size_t)rowcap * sizeof *rows); } rows[nrows++] = v; }
static void scratch_rows(void) {
    nrows = 0;
    int pos = 0, soft = 0;
    for (;;) {
        push_row(pos | (soft ? SOFT : 0));
        int e = pos;
        while (e < reflen && ref[e] != '\n') e++;
        int take = e > pos ? row_break(ref + pos, e - pos, D.wrap) : 0;
        if (pos + take < e) { pos += take; soft = 1; }
        else if (e < reflen) { pos = e + 1; soft = 0; }
        else break;
    }
}
/* structure (always), and equal to the scratch rows when nothing is stale */
static int check_rows(const char *what) {
    int n = nlines(), bad = 0, ns = 0, ln = 0;
    if (n < 1 || ls(0) != 0 || lsoft(0)) { CHECK(0, "%s: row 0 is %d", what, n ? ls(0) : -1); return 0; }
    for (int i = 0; i < n && !bad; i++) {
        int s = ls(i), hard = s == 0 || ref[s - 1] == '\n';
        if (i && s <= ls(i - 1)) { CHECK(0, "%s: row %d at %d after %d", what, i, s, ls(i - 1)); bad = 1; }
        if (s > reflen) { CHECK(0, "%s: row %d at %d past the end %d", what, i, s, reflen); bad = 1; }
        if (hard == !!lsoft(i)) { CHECK(0, "%s: row %d at %d: soft flag %d, line start %d", what, i, s, !!lsoft(i), hard); bad = 1; }
        if (lsoft(i)) ns++;
        else if (i) ln++;
        /* every line start has a row: no newline inside a row */
        int e = i + 1 < n ? ls(i + 1) : reflen;
        for (int k = s; k < e - 1 && !bad; k++) if (ref[k] == '\n') { CHECK(0, "%s: row %d [%d, %d) holds a newline at %d", what, i, s, e, k); bad = 1; }
        if (!bad && (rnd() % 64 == 0 || i == n - 1)) {
            int got = logical_line(i);
            if (got != ln) { CHECK(0, "%s: row %d is in line %d, logical_line says %d", what, i, ln, got); bad = 1; }
        }
    }
    if (!bad && ns != D.nsoft) { CHECK(0, "%s: %d soft rows, nsoft %d", what, ns, D.nsoft); bad = 1; }
    if (!bad && doc_stale_at() < 0) {
        scratch_rows();
        if (nrows != n) { CHECK(0, "%s: %d rows, %d from scratch (wrap %d)", what, n, nrows, D.wrap); bad = 1; }
        for (int i = 0; i < n && !bad; i++)
            if ((rows[i] & OFFM) != ls(i)) { CHECK(0, "%s: row %d at %d, from scratch %d (wrap %d)", what, i, ls(i), rows[i] & OFFM, D.wrap); bad = 1; }
    }
    checks++;
    return !bad;
}

static int edit(int a, int b, const char *s, int n) {
    int r = doc_replace(a, b, s, n);
    if (r == 0) ref_replace(a, b, s, n);
    return r;
}

/* ---------------- row breaking ---------------- */
static int brk(const char *s, int width) { return row_break(s, (int)strlen(s), width); }
static void test_break(void) {
    D.tabw = 28;
    /* "hello world": 11 chars * 7 = 77 px */
    CHECK(brk("hello world", 0) == 11, "no wrap");
    CHECK(brk("hello world", 77) == 11, "fits exactly");
    CHECK(brk("hello world", 76) == 6, "wraps after the space: %d", brk("hello world", 76));
    CHECK(brk("hello world", 42) == 6, "the space may hang: %d", brk("hello world", 42));
    CHECK(brk("hello world", 35) == 6, "a word as wide as the row, its space hangs: %d", brk("hello world", 35));
    CHECK(brk("hello     world", 40) == 10, "spaces hang: %d", brk("hello     world", 40));
    CHECK(brk("abcdefghij", 30) == 4, "long word broken: %d", brk("abcdefghij", 30));
    CHECK(brk("abcdefghij", 1) == 1, "at least one character: %d", brk("abcdefghij", 1));
    CHECK(brk("ab cdefghijkl", 40) == 3, "word longer than the row starts a row: %d", brk("ab cdefghijkl", 40));
    CHECK(brk("a\tb", 30) == 2, "tab to 28, b at 28..35 wraps: %d", brk("a\tb", 30));
    CHECK(brk("a\tb", 35) == 3, "tab fits: %d", brk("a\tb", 35));
    CHECK(brk("WWW", 30) == 2, "wide letters: %d", brk("WWW", 30));
    CHECK(brk("\xc3\xa9\xc3\xa9\xc3\xa9", 20) == 4, "UTF-8 not split: %d", brk("\xc3\xa9\xc3\xa9\xc3\xa9", 20));
    CHECK(brk("\xc3\xc3\xc3", 20) == 2, "invalid bytes are characters: %d", brk("\xc3\xc3\xc3", 20));
    CHECK(row_width("a\tb", 3) == 35, "row width %d", row_width("a\tb", 3));
    /* the byte limit */
    static char big[LINE_BYTES * 3];
    memset(big, 'x', sizeof big);
    CHECK(row_break(big, sizeof big, 0) == LINE_BYTES, "limit %d", row_break(big, sizeof big, 0));
    CHECK(row_break(big, sizeof big, 1 << 30) == LINE_BYTES, "limit with wrap %d", row_break(big, sizeof big, 1 << 30));
    big[100] = ' ';
    CHECK(row_break(big, sizeof big, 1 << 30) == 101, "limit at a space %d", row_break(big, sizeof big, 1 << 30));
    big[100] = 'x';
    memcpy(big + LINE_BYTES - 1, "\xe2\x82\xac", 3);
    CHECK(row_break(big, sizeof big, 0) == LINE_BYTES - 1, "limit not inside a character %d", row_break(big, sizeof big, 0));
}

/* ---------------- random editing with wrap changes ---------------- */
static const char *words[] = { "a", "the", "Halcyon", "notepad", "wrap", "extraordinarily", "x", "WWWWWWWWWWWWWWWW",
                               "\xc3\xa9t\xc3\xa9", "\t", "  ", "\n", "\n\n", "supercalifragilisticexpialidocious" };
static int rand_text(char *out, int max) {
    int n = 0;
    while (n < max - 40) {
        const char *w = words[rnd() % (sizeof words / sizeof *words)];
        int l = (int)strlen(w);
        memcpy(out + n, w, (size_t)l);
        n += l;
        if (rnd() % 3) out[n++] = ' ';
        if (rnd() % 200 == 0) { int k = (int)(rnd() % 3000); while (k-- && n < max - 2) out[n++] = (char)('a' + rnd() % 26); }
    }
    return n;
}

static void test_random(int rounds) {
    static char buf[20000];
    int widths[] = { 0, 1, 7, 50, 120, 400, 2000, 1 << 20 };
    for (int round = 0; round < rounds; round++) {
        doc_reset();
        reflen = 0;
        doc_set_wrap(widths[rnd() % 8], 0);
        doc_rewrap(1 << 30);
        int n = rand_text(buf, (int)(rnd() % 6000) + 50);
        edit(0, 0, buf, n);
        char what[64];
        snprintf(what, sizeof what, "round %d load", round);
        check_rows(what);
        for (int step = 0; step < 300; step++) {
            int len = tlen(), op = (int)(rnd() % 10);
            if (op < 5) {
                int a = len ? (int)(rnd() % (len + 1)) : 0, b = a + (int)(rnd() % 8 == 0 ? rnd() % (len - a + 1) : rnd() % 4);
                if (b > len) b = len;
                int k = rnd() % 4 ? (int)(rnd() % 3) : rand_text(buf, (int)(rnd() % 400) + 41);
                if (k <= 2) for (int i = 0; i < k; i++) buf[i] = " \nab\t"[rnd() % 5];
                CHECK(edit(a, b, buf, k) == 0, "edit failed");
            } else if (op < 7) {
                doc_set_wrap(widths[rnd() % 8], len ? (int)(rnd() % (len + 1)) : 0);
            } else {
                if (rnd() % 2) doc_rewrap_prefer(len ? (int)(rnd() % (len + 1)) : 0);
                doc_rewrap((int)(rnd() % 3000) + 1);
            }
            snprintf(what, sizeof what, "round %d step %d op %d", round, step, op);
            if (!text_equal()) { CHECK(0, "%s: text differs", what); break; }
            if (!check_rows(what)) break;
        }
        while (doc_rewrap(1 << 30)) {}
        snprintf(what, sizeof what, "round %d end", round);
        check_rows(what);
    }
}

/* ---------------- undo ---------------- */
static undo_t U;
static int cur, anchor;
static int apply_fn(void *ctx, int a, int b, const char *s, int n) { (void)ctx; return edit(a, b, s, n); }

/* an edit with its history entry */
static void uedit(int kind, int a, int b, const char *s, int n, uint64_t t) {
    static char old[4 << 20];
    text_get(a, b - a, old);
    int c0 = cur, a0 = anchor;
    cur = anchor = a + n;
    CHECK(undo_record(&U, kind, a, old, b - a, s, n, c0, a0, cur, anchor, t) == 0, "record");
    edit(a, b, s, n);
}
static void type(const char *s, uint64_t *t, int gap) {
    for (; *s; s++) { uedit(UNDO_TYPE, cur, cur, s, 1, *t); *t += gap; }
}
static int text_is(const char *want) { return reflen == (int)strlen(want) && !memcmp(ref, want, (size_t)reflen); }
static void show(const char *what) { if (verbose) printf("%s: \"%.*s\"\n", what, reflen, ref); }

static void test_undo_groups(void) {
    doc_reset(); reflen = 0; doc_set_wrap(0, 0);
    undo_init(&U, 4 << 20);
    cur = anchor = 0;
    uint64_t t = 1000;
    type("hello world", &t, 100);
    CHECK(text_is("hello world"), "typed");
    int c, an;
    CHECK(undo_undo(&U, apply_fn, 0, &c, &an) == 1 && text_is("hello "), "undo the second word");
    show("undo 1");
    CHECK(c == 6 && an == 6, "caret after undo %d %d", c, an);
    CHECK(undo_undo(&U, apply_fn, 0, &c, &an) == 1 && text_is(""), "undo the first word");
    CHECK(undo_undo(&U, apply_fn, 0, &c, &an) == 0, "nothing more to undo");
    CHECK(undo_redo(&U, apply_fn, 0, &c, &an) == 1 && text_is("hello "), "redo");
    CHECK(undo_redo(&U, apply_fn, 0, &c, &an) == 1 && text_is("hello world") && c == 11, "redo 2");
    CHECK(!undo_can_redo(&U), "nothing to redo");
    /* a pause splits a word, a newline starts a step */
    undo_clear(&U); doc_reset(); reflen = 0; cur = anchor = 0;
    type("abc", &t, 100); t += 5000; type("def", &t, 100);
    type("\n", &t, 100); type("ghi", &t, 100);
    CHECK(text_is("abcdef\nghi"), "typed 2");
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("abcdef\n"), "undo after newline: \"%.*s\"", reflen, ref);
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("abcdef"), "undo newline");
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("abc"), "undo after the pause");
    /* a caret move (undo_break) splits */
    undo_clear(&U); doc_reset(); reflen = 0; cur = anchor = 0;
    type("abc", &t, 100); undo_break(&U); type("def", &t, 100);
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("abc"), "undo after a move");
    /* Backspace runs, Delete runs, and typing over a selection */
    undo_clear(&U); doc_reset(); reflen = 0; cur = anchor = 0;
    edit(0, 0, "one two three", 13);
    undo_clear(&U);
    cur = anchor = 13;
    for (int i = 0; i < 5; i++) uedit(UNDO_BACKSPACE, cur - 1, cur, "", 0, t++);
    CHECK(text_is("one two "), "backspaced");
    cur = anchor = 0;
    undo_break(&U);
    for (int i = 0; i < 2; i++) uedit(UNDO_DELETE, 0, 1, "", 0, t++);
    CHECK(text_is("e two "), "deleted");
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("one two ") && c == 0, "undo deletes");
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("one two three") && c == 13, "undo backspaces");
    undo_redo(&U, apply_fn, 0, &c, &an); CHECK(text_is("one two "), "redo backspaces");
    /* typing over a selection: one step, the selection comes back */
    cur = 4; anchor = 7;
    uedit(UNDO_TYPE, 4, 7, "2", 1, t++);
    type("22", &t, 10);
    CHECK(text_is("one 222 "), "typed over: \"%.*s\"", reflen, ref);
    undo_undo(&U, apply_fn, 0, &c, &an);
    CHECK(text_is("one two ") && c == 4 && an == 7, "selection back: %d %d", c, an);
    /* other edits are steps of their own; UNDO_JOIN joins */
    uedit(UNDO_OTHER, 0, 0, "X", 1, t);
    uedit(UNDO_OTHER, 1, 1, "Y", 1, t);
    uedit(UNDO_OTHER | UNDO_JOIN, 2, 2, "Z", 1, t);
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("Xone two "), "join: \"%.*s\"", reflen, ref);
    undo_undo(&U, apply_fn, 0, &c, &an); CHECK(text_is("one two "), "other");
    undo_clear(&U);
}

static void test_undo_saved(void) {
    doc_reset(); reflen = 0;
    undo_init(&U, 4 << 20);
    cur = anchor = 0;
    uint64_t t = 0;
    type("abc ", &t, 10);
    undo_mark_saved(&U);
    CHECK(undo_is_saved(&U), "saved");
    type("d", &t, 10);
    CHECK(!undo_is_saved(&U), "typed after saving (not merged into the saved step)");
    undo_undo(&U, apply_fn, 0, &cur, &anchor);
    CHECK(undo_is_saved(&U) && text_is("abc "), "back to the saved state");
    undo_undo(&U, apply_fn, 0, &cur, &anchor);
    CHECK(!undo_is_saved(&U), "before the saved state");
    undo_redo(&U, apply_fn, 0, &cur, &anchor);
    CHECK(undo_is_saved(&U), "redo to the saved state");
    undo_undo(&U, apply_fn, 0, &cur, &anchor);
    type("x", &t, 10);
    CHECK(!undo_is_saved(&U) && !undo_can_redo(&U), "a new edit drops redo");
    undo_undo(&U, apply_fn, 0, &cur, &anchor);
    CHECK(!undo_is_saved(&U), "the saved state is gone");
    /* a new file: clear */
    undo_clear(&U);
    CHECK(undo_is_saved(&U) && !undo_can_undo(&U), "cleared");
}

static void test_undo_limit(void) {
    doc_reset(); reflen = 0;
    undo_init(&U, 1 << 20);
    cur = anchor = 0;
    static char blk[100000];
    memset(blk, 'q', sizeof blk);
    for (int i = 0; i < 40; i++) uedit(UNDO_OTHER, cur, cur, blk, sizeof blk, 0);
    CHECK(U.bytes <= U.limit, "history %zu bytes, limit %zu", U.bytes, U.limit);
    CHECK(U.n >= 5 && U.n <= 11, "%d steps kept", U.n);
    int c, an, k = 0;
    while (undo_undo(&U, apply_fn, 0, &c, &an) == 1) k++;
    CHECK(reflen == (40 - k) * (int)sizeof blk, "undid %d steps, %d bytes left", k, reflen);
    /* the newest step is kept even when it is larger than the limit */
    undo_clear(&U); doc_reset(); reflen = 0; cur = anchor = 0;
    static char huge[3 << 20];
    memset(huge, 'h', sizeof huge);
    uedit(UNDO_OTHER, 0, 0, huge, sizeof huge, 0);
    uedit(UNDO_OTHER, 0, sizeof huge, "", 0, 0);
    CHECK(U.n == 1 && reflen == 0, "one huge step kept: %d", U.n);
    CHECK(undo_undo(&U, apply_fn, 0, &c, &an) == 1 && reflen == (int)sizeof huge, "huge undo");
    undo_clear(&U);
}

/* random edits, undos and redos against copies of the text at each step */
static void test_undo_random(void) {
    enum { MAXH = 400 };
    static char *snap[MAXH + 1];
    static int snaplen[MAXH + 1];
    for (int round = 0; round < 40; round++) {
        doc_reset(); reflen = 0;
        doc_set_wrap(round % 2 ? 90 : 0, 0);
        undo_init(&U, 1 << 30);
        cur = anchor = 0;
        int depth = 0, top = 0;           /* steps applied, steps in the history (as counted here) */
        snap[0] = malloc(1); snaplen[0] = 0;
        uint64_t t = 0;
        for (int step = 0; step < 300; step++) {
            int op = (int)(rnd() % 10), len = tlen();
            if (op < 6 && top < MAXH) {
                int a = len ? (int)(rnd() % (len + 1)) : 0, b = a + (int)(rnd() % 5);
                if (b > len) b = len;
                char s[64];
                int n = (int)(rnd() % 6);
                for (int i = 0; i < n; i++) s[i] = "ab \n\t\xc3"[rnd() % 6];
                uedit(UNDO_OTHER, a, b, s, n, t++);
                for (int i = depth + 1; i <= top; i++) free(snap[i]);
                depth = top = depth + 1;
                snap[depth] = malloc((size_t)reflen + 1);
                memcpy(snap[depth], ref, (size_t)reflen);
                snaplen[depth] = reflen;
            } else if (op < 8) {
                int c, an, r = undo_undo(&U, apply_fn, 0, &c, &an);
                CHECK(r == (depth > 0), "undo result %d at depth %d", r, depth);
                if (r == 1) depth--;
            } else {
                int c, an, r = undo_redo(&U, apply_fn, 0, &c, &an);
                CHECK(r == (depth < top), "redo result %d at %d of %d", r, depth, top);
                if (r == 1) depth++;
            }
            if (reflen != snaplen[depth] || memcmp(ref, snap[depth], (size_t)reflen)) { CHECK(0, "round %d step %d: text differs at depth %d", round, step, depth); break; }
            if (!text_equal()) { CHECK(0, "doc differs"); break; }
            if (step % 10 == 0 && !check_rows("undo random")) break;
        }
        for (int i = 0; i <= top; i++) free(snap[i]);
        undo_clear(&U);
    }
}

/* ---------------- a 10 MB file ---------------- */
static void test_big(void) {
    int n = 10 << 20;
    char *big = malloc((size_t)n);
    int k = 0;
    while (k < n) {
        int l = rand_text(big + k, n - k < 4000 ? n - k : 4000);
        if (l <= 0) break;
        k += l;
    }
    while (k < n) big[k++] = 'z';
    doc_reset(); reflen = 0; doc_set_wrap(0, 0);
    double t0 = now_s();
    edit(0, 0, big, n);
    double t1 = now_s();
    doc_set_wrap(600, n / 2);
    int slices = 0;
    double worst = 0;
    while (1) {
        double s0 = now_s();
        int more = doc_rewrap(256 << 10);
        double s1 = now_s();
        if (s1 - s0 > worst) worst = s1 - s0;
        slices++;
        if (!more) break;
    }
    double t2 = now_s();
    check_rows("10 MB wrapped");
    /* edits in the middle */
    double e_total = 0, e_worst = 0;
    for (int i = 0; i < 2000; i++) {
        int a = n / 2 + i * 3;
        double e0 = now_s();
        int r = doc_replace(a, a + (i % 3 == 0), "w ", 2);
        double e1 = now_s();
        e_total += e1 - e0;
        if (e1 - e0 > e_worst) e_worst = e1 - e0;
        if (r == 0) ref_replace(a, a + (i % 3 == 0), "w ", 2);       /* (the plain copy is slow: not timed) */
    }
    check_rows("10 MB edited");
    int ln = logical_line(nlines() - 1);
    int want = 0;
    for (int i = 0; i < reflen; i++) if (ref[i] == '\n') want++;
    CHECK(ln == want, "last row in line %d, want %d", ln, want);
    printf("notepad: 10 MB: loaded in %.0f ms, wrapped in %d slices (%.0f ms, at most %.1f ms each, %d rows), "
           "2000 edits in %.1f ms (at most %.2f ms)\n", (t1 - t0) * 1e3, slices, (t2 - t1) * 1e3, worst * 1e3, nlines(),
           e_total * 1e3, e_worst * 1e3);
    CHECK(worst < 0.25, "a slice took %.0f ms", worst * 1e3);
    free(big);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    D.cwidth = cw;
    D.tabw = 28;
    if (doc_init()) { printf("no memory\n"); return 1; }
    test_break();
    test_random(120);
    test_undo_groups();
    test_undo_saved();
    test_undo_limit();
    test_undo_random();
    test_big();
    if (fails) printf("notepad: %d of %d checks FAILED\n", fails, checks);
    else printf("notepad: all %d checks passed\n", checks);
    return fails != 0;
}
