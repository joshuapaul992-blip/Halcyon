/* LINES.SCR - Drifting Lines: two closed ribbons of four corners each drift
 * across the screen, bouncing off the edges, and leave a fading trail of
 * their last positions. The colours slide slowly round the colour wheel,
 * the two ribbons half a wheel apart. Each frame erases the previous trail
 * (drawn again in black) and draws the new one: no full-screen clear. */
#include "saver.h"

SAVER_NAME("Drifting Lines");

#define SHAPES 2
#define CORNERS 4
#define TRAIL 14
typedef struct { int x, y; } pt_t;
typedef struct {
    pt_t p[CORNERS];               /* fixed point: 1/256 pixel */
    pt_t v[CORNERS];               /* per second, 1/256 pixel */
    pt_t hist[TRAIL][CORNERS];     /* screen pixels, newest at head */
    int head, count;
} shape_t;
static shape_t sh[SHAPES];
static int hue;
static int bx0, by0, bx1, by1, have_box;    /* what the last frame drew */

static void saver_start(saver_t *s) {
    sv_clear(s, 0x000000);
    int scale = s->preview ? 1 : 3;
    for (int k = 0; k < SHAPES; k++) {
        shape_t *a = &sh[k];
        memset(a, 0, sizeof *a);
        for (int i = 0; i < CORNERS; i++) {
            a->p[i].x = sv_range(0, s->w - 1) << 8;
            a->p[i].y = sv_range(0, s->h - 1) << 8;
            int vx = sv_range(40, 110) * scale, vy = sv_range(40, 110) * scale;
            a->v[i].x = (sv_rand() & 1 ? vx : -vx) << 8;
            a->v[i].y = (sv_rand() & 1 ? vy : -vy) << 8;
        }
    }
    hue = sv_range(0, 1535);
    have_box = 0;
}

static void ribbon(saver_t *s, const pt_t *q, unsigned c) {
    for (int i = 0; i < CORNERS; i++) {
        const pt_t *a = &q[i], *b = &q[(i + 1) % CORNERS];
        sv_line(s, a->x, a->y, b->x, b->y, c);
    }
}

static void draw_trail(saver_t *s, shape_t *a, int base_hue, int erase) {
    for (int n = a->count - 1; n >= 0; n--) {            /* oldest first, so the newest is on top */
        int idx = (a->head - n + TRAIL) % TRAIL;
        unsigned c = 0;
        if (!erase) {
            int fade = 256 - n * 230 / TRAIL;
            c = sv_mix(0x000000, sv_hue(base_hue + n * 12), fade);
        }
        ribbon(s, a->hist[idx], c);
    }
}

static int saver_frame(saver_t *s, int dt) {
    for (int k = 0; k < SHAPES; k++) draw_trail(s, &sh[k], hue + k * 768, 1);
    hue += dt / 10 + 1;
    int W = s->w << 8, H = s->h << 8;
    for (int k = 0; k < SHAPES; k++) {
        shape_t *a = &sh[k];
        for (int i = 0; i < CORNERS; i++) {
            pt_t *p = &a->p[i], *v = &a->v[i];
            p->x += v->x / 1000 * dt;
            p->y += v->y / 1000 * dt;
            if (p->x < 0) { p->x = -p->x; v->x = -v->x; }
            if (p->y < 0) { p->y = -p->y; v->y = -v->y; }
            if (p->x >= W) { p->x = 2 * (W - 1) - p->x; v->x = -v->x; }
            if (p->y >= H) { p->y = 2 * (H - 1) - p->y; v->y = -v->y; }
        }
        a->head = (a->head + 1) % TRAIL;
        for (int i = 0; i < CORNERS; i++) { a->hist[a->head][i].x = a->p[i].x >> 8; a->hist[a->head][i].y = a->p[i].y >> 8; }
        if (a->count < TRAIL) a->count++;
    }
    for (int k = 0; k < SHAPES; k++) draw_trail(s, &sh[k], hue + k * 768, 0);
    /* the part of the screen that changed: the old trails and the new */
    int x0 = s->w, y0 = s->h, x1 = 0, y1 = 0;
    for (int k = 0; k < SHAPES; k++)
        for (int n = 0; n < sh[k].count; n++)
            for (int i = 0; i < CORNERS; i++) {
                pt_t *p = &sh[k].hist[(sh[k].head - n + TRAIL) % TRAIL][i];
                if (p->x < x0) x0 = p->x;
                if (p->y < y0) y0 = p->y;
                if (p->x > x1) x1 = p->x;
                if (p->y > y1) y1 = p->y;
            }
    int ux0 = x0, uy0 = y0, ux1 = x1, uy1 = y1;
    if (have_box) {
        if (bx0 < ux0) ux0 = bx0;
        if (by0 < uy0) uy0 = by0;
        if (bx1 > ux1) ux1 = bx1;
        if (by1 > uy1) uy1 = by1;
    }
    sv_dirty(s, ux0, uy0, ux1 - ux0 + 1, uy1 - uy0 + 1);
    bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; have_box = 1;
    return 1;
}

static void saver_config(void) { sv_no_options("Drifting Lines"); }
