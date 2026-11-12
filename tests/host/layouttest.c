/* Host test: the monitor arrangement math (kernel/gui/layout.c): adjacency,
 * the pointer crossing between monitors of different sizes, snapping a
 * dragged monitor, normalizing saved arrangements (overlaps, gaps,
 * negative positions), window places across monitors (a monitor that went
 * away, windows larger than a monitor), and random arrangements checked for
 * the invariants: no overlaps, every monitor reachable, the origin at
 * (0, 0), the first monitor where it was (shifted), idempotence. */
#include <stdio.h>
#include <stdlib.h>
#include "gui/layout.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("layouttest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int overlap_any(const rect_t *m, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (!rect_empty(rect_intersect(m[i], m[j]))) return 1;
    return 0;
}
static int connected(const rect_t *m, int n) {
    int seen[LAYOUT_MAX] = { 1 }, grew = 1;
    while (grew) {
        grew = 0;
        for (int i = 0; i < n; i++)
            if (seen[i])
                for (int j = 0; j < n; j++)
                    if (!seen[j] && layout_adjacent(m[i], m[j])) { seen[j] = 1; grew = 1; }
    }
    for (int i = 0; i < n; i++) if (!seen[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 20000;

    /* adjacency */
    CHECK(layout_adjacent(mkrect(0, 0, 100, 100), mkrect(100, 50, 50, 100)), "right neighbour");
    CHECK(!layout_adjacent(mkrect(0, 0, 100, 100), mkrect(100, 100, 50, 50)), "corners only touch");
    CHECK(layout_adjacent(mkrect(0, 0, 100, 100), mkrect(-20, 100, 30, 30)), "below, overlapping in x");
    CHECK(!layout_adjacent(mkrect(0, 0, 100, 100), mkrect(101, 0, 50, 50)), "a gap");

    /* the pointer: 1920x1080 primary, a 1024x768 monitor right of it, top aligned */
    rect_t m[LAYOUT_MAX] = { { 0, 0, 1920, 1080 }, { 1920, 0, 1024, 768 } };
    int x = 1925, y = 1000;
    int on = layout_clamp_point(m, 2, &x, &y);
    CHECK(on == 0 && x == 1919 && y == 1000, "below the small monitor: stays at the edge (%d: %d,%d)", on, x, y);
    x = 1930; y = 500;
    CHECK(layout_at(m, 2, x, y) == 1, "crosses where they touch");
    x = 5000; y = -40;
    on = layout_clamp_point(m, 2, &x, &y);
    CHECK(on == 1 && x == 2943 && y == 0, "far away: the nearest corner (%d,%d)", x, y);
    x = 2500; y = 900;
    on = layout_clamp_point(m, 2, &x, &y);
    CHECK(on == 1 && x == 2500 && y == 767, "under monitor 2 -> its bottom edge (%d: %d,%d)", on, x, y);

    /* best monitor and window places */
    CHECK(layout_best(m, 2, mkrect(1800, 100, 400, 300)) == 1, "mostly on monitor 2");
    CHECK(layout_best(m, 2, mkrect(5000, 5000, 10, 10)) == 1, "nowhere: the nearest");
    rect_t wa[2] = { { 0, 0, 1920, 1050 }, { 1920, 0, 1024, 768 } };
    rect_t f = layout_fit(wa, 2, mkrect(2800, 600, 400, 300), 0, 0);
    CHECK(f.x == 2944 - 400 && f.y == 768 - 300 && f.w == 400, "fits inside monitor 2 (%d,%d)", f.x, f.y);
    f = layout_fit(wa, 1, mkrect(2800, 600, 400, 300), 0, 0);
    CHECK(f.x == 1920 - 400 && f.y == 600, "monitor 2 gone: onto the primary (%d,%d)", f.x, f.y);
    f = layout_fit(wa, 2, mkrect(3500, 100, 400, 300), 0, 0);
    CHECK(f.x == 1920 - 400 && f.y == 100, "off every monitor: the primary (%d,%d)", f.x, f.y);
    f = layout_fit(wa, 2, mkrect(1930, 10, 1500, 1000), 0, 0);
    CHECK(f.x == 1920 && f.y == 0 && f.w == 1024 && f.h == 768, "larger than its monitor: shrunk (%dx%d)", f.w, f.h);
    f = layout_fit(wa, 2, mkrect(100, 100, 300, 200), 1, 1);
    CHECK(f.x >= 1920, "forced onto monitor 2");
    CHECK(layout_grabbable(m, 2, mkrect(1900, -10, 300, 200), 26, 60), "title bar partly on screen");
    CHECK(!layout_grabbable(m, 2, mkrect(2000, 900, 300, 200), 26, 60), "title bar below monitor 2");

    /* snapping a dragged monitor */
    m[1] = mkrect(1500, 1200, 1024, 768);            /* dropped below with a gap */
    layout_snap(m, 2, 1);
    CHECK(m[1].y == 1080 && m[1].x == 1500, "snapped up under the primary (%d,%d)", m[1].x, m[1].y);
    m[1] = mkrect(800, 300, 1024, 768);              /* dropped onto the primary */
    layout_snap(m, 2, 1);
    CHECK(!overlap_any(m, 2) && layout_adjacent(m[0], m[1]), "dropped on top: moved to an edge (%d,%d)", m[1].x, m[1].y);
    m[1] = mkrect(2100, 1200, 1024, 768);            /* corner only */
    layout_snap(m, 2, 1);
    CHECK(!overlap_any(m, 2) && layout_adjacent(m[0], m[1]), "diagonal: moved to share an edge (%d,%d)", m[1].x, m[1].y);

    /* normalizing saved arrangements */
    rect_t a[3] = { { 0, 0, 1280, 1024 }, { 0, 0, 1024, 768 }, { 0, 0, 800, 600 } };   /* every one at 0,0 */
    int dx, dy;
    layout_normalize(a, 3, 0, &dx, &dy);
    CHECK(!overlap_any(a, 3) && connected(a, 3), "three at the origin: spread out");
    rect_t b = layout_bounds(a, 3);
    CHECK(b.x == 0 && b.y == 0, "origin at 0,0 (%d,%d)", b.x, b.y);
    rect_t c[2] = { { 0, 0, 1280, 1024 }, { -1024, 100, 1024, 768 } };     /* left of the primary */
    layout_normalize(c, 2, 0, &dx, &dy);
    CHECK(c[0].x == 1024 && c[0].y == 0 && c[1].x == 0 && c[1].y == 100 && dx == 1024 && dy == 0,
          "monitor left of the primary: everything shifts (%d,%d %d,%d)", c[0].x, c[0].y, c[1].x, c[1].y);
    rect_t d[2] = { { 0, 0, 1280, 1024 }, { 1300, 0, 800, 600 } };         /* a 20 pixel gap */
    layout_normalize(d, 2, 0, NULL, NULL);
    CHECK(d[1].x == 1280 && d[1].y == 0, "gap closed (%d,%d)", d[1].x, d[1].y);

    /* random arrangements */
    srand(42);
    for (int it = 0; it < iters; it++) {
        int n = 1 + rand() % 6;
        rect_t r[LAYOUT_MAX];
        for (int i = 0; i < n; i++)
            r[i] = mkrect(rand() % 6000 - 3000, rand() % 4000 - 2000, 640 + rand() % 3200, 480 + rand() % 1700);
        int first = rand() % n;
        rect_t p0 = r[first];
        layout_normalize(r, n, first, &dx, &dy);
        rect_t bb = layout_bounds(r, n);
        int ok = !overlap_any(r, n) && connected(r, n) && bb.x == 0 && bb.y == 0 &&
                 r[first].x == p0.x + dx && r[first].y == p0.y + dy;
        if (!ok && getenv("V")) {
            printf("normalize: overlap %d connected %d bounds %d,%d first moved %d\n", overlap_any(r, n), connected(r, n), bb.x, bb.y,
                   r[first].x != p0.x + dx || r[first].y != p0.y + dy);
            for (int q = 0; q < n; q++) printf("  %d: %d,%d %dx%d\n", q, r[q].x, r[q].y, r[q].w, r[q].h);
        }
        for (int i = 0; i < n; i++) if (r[i].w < 640 || r[i].h < 480) ok = 0;
        rect_t again[LAYOUT_MAX];
        for (int i = 0; i < n; i++) again[i] = r[i];
        layout_normalize(again, n, first, NULL, NULL);
        for (int i = 0; i < n; i++) if (!rect_equal(again[i], r[i])) { ok = 0; if (getenv("V")) printf("not idempotent: %d\n", i); }
        for (int k = 0; k < 20 && ok; k++) {
            int px = rand() % 12000 - 6000, py = rand() % 8000 - 4000;
            int mi = layout_clamp_point(r, n, &px, &py);
            if (mi < 0 || layout_at(r, n, px, py) < 0) ok = 0;
        }
        /* drag one monitor anywhere and drop it */
        int i = rand() % n;
        r[i].x = rand() % 8000 - 2000;
        r[i].y = rand() % 6000 - 2000;
        layout_snap(r, n, i);
        if (n > 1) {
            int touch = 0;
            for (int j = 0; j < n; j++) if (j != i && layout_adjacent(r[i], r[j])) touch = 1;
            if (overlap_any(r, n) || !touch) ok = 0;
        }
        /* (it may have been the bridge between two others: Display
         * Properties normalizes after a drop) */
        layout_normalize(r, n, first, NULL, NULL);
        if (overlap_any(r, n) || !connected(r, n)) ok = 0;
        if (!ok) { CHECK(0, "random arrangement %d (%d monitors) breaks an invariant", it, n); break; }
    }

    if (failed) printf("layouttest: %d of %d checks FAILED\n", failed, checks);
    else printf("layouttest: all %d checks passed (%d random arrangements)\n", checks, iters);
    return failed != 0;
}
