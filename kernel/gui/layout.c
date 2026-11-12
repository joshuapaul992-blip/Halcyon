/* Halcyon OS - monitor arrangement math (see layout.h) */
#include "layout.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int overlaps(rect_t a, rect_t b) { return !rect_empty(rect_intersect(a, b)); }

int layout_adjacent(rect_t a, rect_t b) {
    if (rect_empty(a) || rect_empty(b)) return 0;
    /* vertical edge shared: left/right neighbours overlapping in y */
    if (a.x + a.w == b.x || b.x + b.w == a.x)
        return imin(a.y + a.h, b.y + b.h) - imax(a.y, b.y) > 0;
    if (a.y + a.h == b.y || b.y + b.h == a.y)
        return imin(a.x + a.w, b.x + b.w) - imax(a.x, b.x) > 0;
    return 0;
}

int layout_at(const rect_t *m, int n, int x, int y) {
    for (int i = 0; i < n; i++) if (rect_contains(m[i], x, y)) return i;
    return -1;
}

int layout_clamp_point(const rect_t *m, int n, int *x, int *y) {
    int best = -1, bx = *x, by = *y;
    long long bd = 0;
    for (int i = 0; i < n; i++) {
        if (rect_empty(m[i])) continue;
        int cx = iclamp(*x, m[i].x, m[i].x + m[i].w - 1), cy = iclamp(*y, m[i].y, m[i].y + m[i].h - 1);
        long long d = (long long)(cx - *x) * (cx - *x) + (long long)(cy - *y) * (cy - *y);
        if (best < 0 || d < bd) { best = i; bd = d; bx = cx; by = cy; }
        if (!d) break;
    }
    *x = bx; *y = by;
    return best;
}

/* squared distance from a point to a rectangle */
static long long point_dist(rect_t r, int x, int y) {
    int cx = iclamp(x, r.x, r.x + r.w - 1), cy = iclamp(y, r.y, r.y + r.h - 1);
    return (long long)(cx - x) * (cx - x) + (long long)(cy - y) * (cy - y);
}

static int best_overlap(const rect_t *m, int n, rect_t r, long *area) {
    int best = -1;
    long ba = 0;
    for (int i = 0; i < n; i++) {
        long a = rect_area(rect_intersect(m[i], r));
        if (a > ba) { ba = a; best = i; }
    }
    *area = ba;
    return best;
}

int layout_best(const rect_t *m, int n, rect_t r) {
    long a;
    int best = best_overlap(m, n, r, &a);
    if (best >= 0) return best;
    long long bd = 0;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < n; i++) {
        if (rect_empty(m[i])) continue;
        long long d = point_dist(m[i], cx, cy);
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

rect_t layout_bounds(const rect_t *m, int n) {
    rect_t b = mkrect(0, 0, 0, 0);
    for (int i = 0; i < n; i++) b = rect_union(b, m[i]);
    return b;
}

/* can monitor i be at c, given the monitors in mask? (touching one, overlapping none) */
static int place_ok(const rect_t *m, int n, int i, unsigned mask, rect_t c) {
    int touch = 0;
    for (int k = 0; k < n; k++) {
        if (k == i || !(mask & (1u << k)) || rect_empty(m[k])) continue;
        if (overlaps(c, m[k])) return 0;
        if (layout_adjacent(c, m[k])) touch = 1;
    }
    return touch;
}

static void try_place(const rect_t *m, int n, int i, unsigned mask, rect_t c, long long *bd, rect_t *best) {
    if (!place_ok(m, n, i, mask, c)) return;
    long long dx = c.x - m[i].x, dy = c.y - m[i].y, d = dx * dx + dy * dy;
    if (*bd < 0 || d < *bd) { *bd = d; *best = c; }
}

/* snap m[i] against the monitors in mask */
static void snap_mask(rect_t *m, int n, int i, unsigned mask) {
    rect_t me = m[i];
    int w = me.w, h = me.h, any = 0;
    for (int k = 0; k < n; k++) if (k != i && (mask & (1u << k)) && !rect_empty(m[k])) any = 1;
    if (!any || rect_empty(me)) return;
    if (place_ok(m, n, i, mask, me)) return;
    long long bd = -1;
    rect_t best = me;
    for (int j = 0; j < n; j++) {
        if (j == i || !(mask & (1u << j)) || rect_empty(m[j])) continue;
        rect_t o = m[j];
        /* along each side of j: the nearest position to where i is, and
         * positions lined up with the other monitors' edges (to slide past them) */
        int ylo = o.y - h + 1, yhi = o.y + o.h - 1, xlo = o.x - w + 1, xhi = o.x + o.w - 1;
        int ys[2 + 4 * LAYOUT_MAX], xs[2 + 4 * LAYOUT_MAX], ny = 0, nx = 0;
        ys[ny++] = iclamp(me.y, ylo, yhi); ys[ny++] = o.y;
        xs[nx++] = iclamp(me.x, xlo, xhi); xs[nx++] = o.x;
        for (int k = 0; k < n && k < LAYOUT_MAX; k++) {
            if (k == i || !(mask & (1u << k)) || rect_empty(m[k])) continue;
            ys[ny++] = iclamp(m[k].y + m[k].h, ylo, yhi); ys[ny++] = iclamp(m[k].y - h, ylo, yhi);
            xs[nx++] = iclamp(m[k].x + m[k].w, xlo, xhi); xs[nx++] = iclamp(m[k].x - w, xlo, xhi);
        }
        for (int a = 0; a < ny; a++) {
            try_place(m, n, i, mask, mkrect(o.x + o.w, ys[a], w, h), &bd, &best);
            try_place(m, n, i, mask, mkrect(o.x - w, ys[a], w, h), &bd, &best);
        }
        for (int a = 0; a < nx; a++) {
            try_place(m, n, i, mask, mkrect(xs[a], o.y + o.h, w, h), &bd, &best);
            try_place(m, n, i, mask, mkrect(xs[a], o.y - h, w, h), &bd, &best);
        }
    }
    if (bd < 0) {
        /* boxed in (cannot happen with the candidates above, but be safe):
         * right of the rightmost monitor, top aligned */
        int r = -1;
        for (int k = 0; k < n; k++)
            if (k != i && (mask & (1u << k)) && !rect_empty(m[k]) && (r < 0 || m[k].x + m[k].w > m[r].x + m[r].w)) r = k;
        best = mkrect(m[r].x + m[r].w, m[r].y, w, h);
    }
    m[i] = best;
}

void layout_snap(rect_t *m, int n, int i) {
    if (n > LAYOUT_MAX) n = LAYOUT_MAX;
    snap_mask(m, n, i, ((1u << n) - 1) & ~(1u << i));
}

void layout_normalize(rect_t *m, int n, int first, int *dx, int *dy) {
    if (n > LAYOUT_MAX) n = LAYOUT_MAX;
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (n <= 0) return;
    if (first < 0 || first >= n || rect_empty(m[first])) {
        first = -1;
        for (int i = 0; i < n && first < 0; i++) if (!rect_empty(m[i])) first = i;
        if (first < 0) return;
    }
    unsigned placed = 1u << first;
    rect_t f = m[first];
    for (;;) {
        /* next: a monitor already touching the placed ones (it stays: a valid
         * arrangement is left as it is), else the nearest unplaced one */
        int next = -1, fits = 0;
        long long nd = 0;
        for (int i = 0; i < n; i++) {
            if ((placed & (1u << i)) || rect_empty(m[i])) continue;
            long long ddx = (m[i].x + m[i].w / 2) - (f.x + f.w / 2), ddy = (m[i].y + m[i].h / 2) - (f.y + f.h / 2);
            long long d = ddx * ddx + ddy * ddy;
            int ok = place_ok(m, n, i, placed, m[i]);
            if (next < 0 || (ok && !fits) || (ok == fits && d < nd)) { next = i; nd = d; fits = ok; }
        }
        if (next < 0) break;
        snap_mask(m, n, next, placed);
        placed |= 1u << next;
    }
    rect_t b = layout_bounds(m, n);
    for (int i = 0; i < n; i++) if (!rect_empty(m[i])) { m[i].x -= b.x; m[i].y -= b.y; }
    if (dx) *dx = -b.x;
    if (dy) *dy = -b.y;
}

rect_t layout_fit(const rect_t *wa, int n, rect_t f, int prefer, int force) {
    if (n <= 0) return f;
    if (prefer >= n) prefer = -1;
    long area;
    int t = best_overlap(wa, n, f, &area);
    if (force && prefer >= 0) t = prefer;
    else if (t < 0) t = prefer >= 0 ? prefer : layout_best(wa, n, f);
    if (t < 0) return f;
    rect_t a = wa[t];
    f.w = imin(f.w, a.w);
    f.h = imin(f.h, a.h);
    f.x = iclamp(f.x, a.x, a.x + a.w - f.w);
    f.y = iclamp(f.y, a.y, a.y + a.h - f.h);
    return f;
}

int layout_grabbable(const rect_t *m, int n, rect_t f, int th, int min_w) {
    rect_t strip = mkrect(f.x, f.y, f.w, th);
    for (int i = 0; i < n; i++) {
        rect_t r = rect_intersect(strip, m[i]);
        if (r.w >= min_w && r.h >= (th < 8 ? th : 8)) return 1;
    }
    return 0;
}
