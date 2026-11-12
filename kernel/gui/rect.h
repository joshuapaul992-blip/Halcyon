/* Halcyon OS - rectangles (screen and surface coordinates). Free of any
 * other kernel header, so the monitor layout code can be tested on the host. */
#ifndef RECT_H
#define RECT_H

typedef struct rect { int x, y, w, h; } rect_t;

static inline rect_t mkrect(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static inline int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline int rect_contains(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static inline rect_t rect_inset(rect_t r, int d) { return mkrect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }
static inline rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return mkrect(0, 0, 0, 0);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}
static inline rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}
static inline int rect_equal(rect_t a, rect_t b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
static inline long rect_area(rect_t r) { return rect_empty(r) ? 0 : (long)r.w * r.h; }

#endif
