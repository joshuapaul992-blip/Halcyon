/* LOGO.SCR - the Halcyon logo bouncing round the screen. The logo (the
 * round badge with the kingfisher-like wing of the Start button) is drawn
 * here from its outline, at any size, smoothed with 4 x 4 samples per pixel;
 * each time it touches an edge the badge takes the next colour. Only the
 * logo's old and new places are drawn. */
#include "saver.h"

SAVER_NAME("Halcyon Logo");

/* the logo in a 32 x 32 design space (as tools/mkicons.py ic_logo) */
typedef struct { float x, y; } fpt_t;
static const fpt_t wing[] = { { 5, 20 }, { 12, 13 }, { 19, 10.5f }, { 27, 11 }, { 20, 14 }, { 14, 18.5f }, { 9, 23 } };
static const fpt_t swoosh[] = { { 9, 23 }, { 15, 19.5f }, { 22, 18 }, { 26, 19.5f }, { 19, 22 }, { 13, 25 } };
/* badge colours: top, bottom, rim (the first is the logo's own) */
static const unsigned palettes[][3] = {
    { 0x5FD3E8, 0x1546A8, 0x0B2A6A }, { 0x8BE07A, 0x1E7A3A, 0x0E4020 }, { 0xC8A0FF, 0x5A2AB0, 0x2E1260 },
    { 0xFFC070, 0xC0501A, 0x6A2408 }, { 0xFF9AC8, 0xB0306E, 0x5E0E36 }, { 0x9EF0E0, 0x168A84, 0x08484A },
};
#define NPAL ((int)(sizeof palettes / sizeof palettes[0]))

static unsigned *spr;            /* colour << 8 | alpha, L x L */
static int L, pal;
static int x, y, vx, vy;         /* 1/256 pixel; per second */
static int ox, oy, drawn;        /* where the last frame put it */
static int text_w, text_h, font;
static const char *title = "Halcyon";

static int inside(const fpt_t *p, int n, float px, float py) {
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].y > py) != (p[j].y > py) && px < (p[j].x - p[i].x) * (py - p[i].y) / (p[j].y - p[i].y) + p[i].x) in = !in;
    return in;
}

static void build(void) {
    const unsigned *c = palettes[pal];
    float k = 32.0f / (float)L;
    for (int j = 0; j < L; j++) {
        for (int i = 0; i < L; i++) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = ((float)i + (sx + 0.5f) / 4) * k, v = ((float)j + (sy + 0.5f) / 4) * k;
                    float du = u - 16, dv = v - 16, d2 = du * du + dv * dv;
                    unsigned col;
                    if (d2 > 14.5f * 14.5f) continue;
                    int t = (int)(v / 32 * 256);
                    if (inside(swoosh, 6, u, v)) col = sv_mix(0xFFB040, 0xF07010, (int)((v - 18) / 7 * 256));
                    else if (inside(wing, 7, u, v)) col = sv_mix(0xFFFFFF, 0xFFE7B0, (int)((v - 10.5f) / 12.5f * 256));
                    else if (d2 > 13.4f * 13.4f) col = c[2];                      /* the rim */
                    else {
                        col = sv_mix(c[0], c[1], t);
                        /* gloss: a soft light on the upper half */
                        float gy = (v - 3) / 12, gx = du / 12;
                        if (v < 16 && gx * gx + gy * gy < 1.0f) col = sv_mix(col, 0xFFFFFF, (int)(110 * (1 - gy)));
                    }
                    r += (int)(col >> 16); g += (int)((col >> 8) & 255); b += (int)(col & 255); a++;
                }
            unsigned p = 0;
            if (a) p = (unsigned)((r / a) << 24 | (g / a) << 16 | (b / a) << 8 | (a * 255 / 16));
            spr[j * L + i] = p;
        }
    }
}

static void saver_start(saver_t *s) {
    sv_clear(s, 0x000000);
    int m = s->w < s->h ? s->w : s->h;
    L = m * 28 / 100;
    if (L < 16) L = 16;
    free(spr);
    spr = malloc((size_t)L * L * sizeof *spr);
    if (!spr) exit(1);
    pal = 0;
    build();
    font = s->preview ? FONT_SMALL : m >= 700 ? FONT_HUGE : FONT_LARGE;
    text_w = gfx_text_width(title, font);
    text_h = gfx_font_height(font);
    int bw = L > text_w ? L : text_w, bh = L + text_h + L / 12;
    x = sv_range(0, s->w - bw > 0 ? s->w - bw : 0) << 8;
    y = sv_range(0, s->h - bh > 0 ? s->h - bh : 0) << 8;
    int sp = s->preview ? 30 : s->w / 8 + 40;
    vx = (sv_rand() & 1 ? sp : -sp) << 8;
    vy = (sv_rand() & 1 ? sp * 3 / 4 : -sp * 3 / 4) << 8;
    drawn = 0;
}

static int saver_frame(saver_t *s, int dt) {
    int bw = L > text_w ? L : text_w, bh = L + text_h + L / 12;
    if (drawn) sv_fill(s, ox, oy, bw, bh, 0x000000);
    x += vx / 1000 * dt;
    y += vy / 1000 * dt;
    int maxx = (s->w - bw) << 8, maxy = (s->h - bh) << 8, bounced = 0;
    if (x < 0) { x = 0; vx = -vx; bounced = 1; }
    if (y < 0) { y = 0; vy = -vy; bounced = 1; }
    if (x > maxx) { x = maxx > 0 ? maxx : 0; vx = -vx; bounced = 1; }
    if (y > maxy) { y = maxy > 0 ? maxy : 0; vy = -vy; bounced = 1; }
    if (bounced) { pal = (pal + 1) % NPAL; build(); }
    int px = x >> 8, py = y >> 8, lx = px + (bw - L) / 2;
    for (int j = 0; j < L; j++) {
        if ((unsigned)(py + j) >= (unsigned)s->h) continue;
        unsigned *row = s->px + (size_t)(py + j) * s->stride;
        for (int i = 0; i < L; i++) {
            unsigned p = spr[j * L + i];
            int a = (int)(p & 255);
            if (!a || (unsigned)(lx + i) >= (unsigned)s->w) continue;
            row[lx + i] = sv_mix(0x000000, p >> 8, a * 256 / 255);
        }
    }
    gfx_text(s->win, px + (bw - text_w) / 2, py + L + L / 12, title, 0xF2F6FF, font);
    ox = px; oy = py; drawn = 1;
    return 1;
}

static void saver_config(void) { sv_no_options("Halcyon Logo"); }
