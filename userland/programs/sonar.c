/* SONAR.EXE - Sonar in user mode: chart the hidden sea mines in a bay.
 * Looks and plays like the kernel's Sonar (kernel/apps/mines.c).
 *
 * A hidden-mines logic puzzle: every revealed patch of sea floor tells how
 * many of its eight neighbours hide a mine. Right click drops a buoy on a
 * suspected mine; clicking a number whose mines are all marked (or a middle
 * click) sweeps its other neighbours.
 *
 * The user API has no menus, so the kernel version's Game and Help menus
 * become a command strip of flat buttons where its menu bar was: the three
 * levels (the current one shown checked) and "?" (How to Play and the
 * best times; also F1). New Game is the submarine button or F2, as before;
 * Exit is the window's close button.
 *
 * Drawing: everything except text is drawn into a private pixel buffer with
 * the same primitives the kernel uses, copied into the window with gfx_blit
 * (only the part that changed), then the text is drawn on top. */
#include "hal.h"

#define TS 20            /* tile size */
#define MARGIN 10
#define PANEL_H 46
#define FRAME 3
#define BAR_H 20         /* command strip (the kernel version's menu bar) */
#define MAX_COLS 30
#define MAX_ROWS 16

#define C_MINE  0x01
#define C_OPEN  0x02
#define C_FLAG  0x04
#define C_BOOM  0x08

/* "Halcyon Blue" theme colours (kernel/gui/wm.c) */
#define TH_FACE        0xEEEBE0
#define TH_SELECT      0x316AC5
#define TH_SELECT_TEXT 0xFFFFFF
#define TH_HOVER       0xF5B547

#define ICON_GAME 34     /* the kernel's ICON_GAME (build/gen/icons.h) */

#define KEY_F1 0x3A
#define KEY_F2 0x3B
#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/* ================= drawing into a private pixel buffer ================= */
typedef struct { int x, y, w, h; } rc_t;
static rc_t R(int x, int y, int w, int h) { rc_t r = { x, y, w, h }; return r; }
static int rc_has(rc_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int rc_empty(rc_t r) { return r.w <= 0 || r.h <= 0; }
static rc_t rc_and(rc_t a, rc_t b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return R(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}
static rc_t rc_or(rc_t a, rc_t b) {
    if (rc_empty(a)) return b;
    if (rc_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return R(x0, y0, x1 - x0, y1 - y0);
}
static rc_t rc_inset(rc_t r, int d) { return R(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }

static unsigned *px;         /* the client area, 0xRRGGBB */
static unsigned *tmp;        /* a part of it, for gfx_blit */
static int pw, ph;
static rc_t clip;

static unsigned mix(unsigned a, unsigned b, int t) {
    unsigned ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    unsigned br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    unsigned r = (ar * (255 - t) + br * t) / 255, g = (ag * (255 - t) + bg * t) / 255, l = (ab * (255 - t) + bb * t) / 255;
    return (r << 16) | (g << 8) | l;
}
static unsigned lighten(unsigned c, int amt) { return amt >= 0 ? mix(c, 0xFFFFFF, amt) : mix(c, 0, -amt); }
static unsigned blend(unsigned d, unsigned s, unsigned a) {
    unsigned rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a)) >> 8;
    unsigned g = ((s & 0x00FF00) * a + (d & 0x00FF00) * (255 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}
static void pixel(int x, int y, unsigned c) { if (rc_has(clip, x, y)) px[y * pw + x] = c; }
static void pixel_blend(int x, int y, unsigned c, int a) {
    if (rc_has(clip, x, y)) px[y * pw + x] = blend(px[y * pw + x], c, (unsigned)a);
}
static void pixel_a(int x, int y, unsigned c, int a) {
    if (a >= 255) pixel(x, y, c);
    else if (a > 0) pixel_blend(x, y, c, a);
}
static void fill(rc_t r, unsigned c) {
    r = rc_and(r, clip);
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) px[(r.y + y) * pw + r.x + x] = c;
}
static void hline(int x, int y, int w, unsigned c) { fill(R(x, y, w, 1), c); }
static void vline(int x, int y, int h, unsigned c) { fill(R(x, y, 1, h), c); }
static void rect(rc_t r, unsigned c) {
    hline(r.x, r.y, r.w, c);
    hline(r.x, r.y + r.h - 1, r.w, c);
    vline(r.x, r.y, r.h, c);
    vline(r.x + r.w - 1, r.y, r.h, c);
}
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void grad_v(rc_t r, unsigned top, unsigned bot) {
    for (int y = 0; y < r.h; y++) hline(r.x, r.y + y, r.w, mix(top, bot, r.h > 1 ? y * 255 / (r.h - 1) : 0));
}
/* coverage (0..255) of a pixel at a rounded corner (4x4 samples) */
static int corner_cov(int dx2, int dy2, int r) {
    int cov = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int x = dx2 * 4 + sx * 2 + 1, y = dy2 * 4 + sy * 2 + 1, rr = r * 8;
            if (x * x + y * y <= rr * rr) cov++;
        }
    return cov * 255 / 16;
}
/* rounded rectangle with a vertical gradient; corners: 1 top-left, 2 top-right,
 * 4 bottom-right, 8 bottom-left */
static void rounded(rc_t r, int rad, int corners, unsigned top, unsigned bot) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    for (int y = 0; y < r.h; y++) {
        unsigned col = mix(top, bot, r.h > 1 ? y * 255 / (r.h - 1) : 0);
        int x0 = 0, x1 = r.w;
        int in_top = y < rad, in_bot = y >= r.h - rad;
        if ((in_top && (corners & 3)) || (in_bot && (corners & 12))) {
            int dy = in_top ? rad - y - 1 : y - (r.h - rad);
            int left = in_top ? (corners & 1) : (corners & 8), right = in_top ? (corners & 2) : (corners & 4);
            for (int x = 0; x < rad; x++) {
                int cov = corner_cov((rad - x - 1) * 2, dy * 2, rad);
                if (left) pixel_a(r.x + x, r.y + y, col, cov);
                if (right) pixel_a(r.x + r.w - 1 - x, r.y + y, col, cov);
            }
            if (left) x0 = rad;
            if (right) x1 = r.w - rad;
        }
        hline(r.x + x0, r.y + y, x1 - x0, col);
    }
}
static void rounded_outline(rc_t r, int rad, int corners, unsigned c) {
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    int tl = corners & 1 ? rad : 0, tr = corners & 2 ? rad : 0, br = corners & 4 ? rad : 0, bl = corners & 8 ? rad : 0;
    hline(r.x + tl, r.y, r.w - tl - tr, c);
    hline(r.x + bl, r.y + r.h - 1, r.w - bl - br, c);
    vline(r.x, r.y + tl, r.h - tl - bl, c);
    vline(r.x + r.w - 1, r.y + tr, r.h - tr - br, c);
    for (int q = 0; q < 4; q++) {
        int rr = q == 0 ? tl : q == 1 ? tr : q == 2 ? br : bl;
        if (!rr) continue;
        int left = q == 0 || q == 3, upper = q == 0 || q == 1;
        int cx = left ? r.x + rr : r.x + r.w - 1 - rr, cy = upper ? r.y + rr : r.y + r.h - 1 - rr;
        for (int dy = 0; dy <= rr; dy++)
            for (int dx = 0; dx <= rr; dx++) {
                int diff = dx * dx + dy * dy - rr * rr, band = 2 * rr;
                if (diff < -band || diff > band) continue;
                int a = 255 - (diff < 0 ? -diff : diff) * 255 / band;
                if (a > 0) pixel_blend(cx + (left ? -dx : dx), cy + (upper ? -dy : dy), c, a);
            }
    }
}
/* anti-aliased filled circle (the kernel's gfx_circle) */
static void disc(int cx, int cy, int rad, unsigned c) {
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++) {
            int d = dx * dx + dy * dy;
            if (d > rad * rad + rad) continue;
            pixel_blend(cx + dx, cy + dy, c, d <= rad * rad - rad ? 255 : 140);
        }
}

/* text goes on top of the blitted pixels: queued while drawing (if its box
 * is in the part being redrawn), drawn by present() */
typedef struct { short x, y; unsigned char font; unsigned color; char s[16]; } text_t;
static text_t tq[MAX_COLS * MAX_ROWS + 16];
static int ntq;
static int font_h[8];
static int font_height(int f) { if (!font_h[f]) font_h[f] = gfx_font_height(f); return font_h[f]; }
static void text_at(rc_t box, int x, int y, const char *s, unsigned c, int f) {
    if (rc_empty(rc_and(box, clip)) || ntq >= (int)(sizeof tq / sizeof tq[0])) return;
    text_t *t = &tq[ntq++];
    t->x = (short)x; t->y = (short)y; t->font = (unsigned char)f; t->color = c;
    strlcpy(t->s, s, sizeof t->s);
}
/* centred in r */
static void text_center(rc_t r, const char *s, unsigned c, int f, int w) {
    if (w < 0) w = gfx_text_width(s, f);
    text_at(r, r.x + (r.w - w) / 2, r.y + (r.h - font_height(f)) / 2, s, c, f);
}

/* ================= the game ================= */
enum { ST_READY, ST_PLAYING, ST_WON, ST_LOST };

typedef struct level { const char *name; int cols, rows, mines; } level_t;
static const level_t levels[3] = {
    { "Beginner", 9, 9, 10 },
    { "Intermediate", 16, 16, 40 },
    { "Expert", 30, 16, 99 },
};
static int best_time[3] = { 999, 999, 999 };

typedef struct sonar {
    int level, cols, rows, nmines;
    uint8_t cell[MAX_COLS * MAX_ROWS];
    uint8_t adj[MAX_COLS * MAX_ROWS];
    int stack[MAX_COLS * MAX_ROWS];
    int state, flags, opened;
    uint64_t start_ms;
    int seconds;
    int press_cell;      /* cell under the held left button, or -1 */
    int press_chord;     /* pressing a revealed number: show its neighbours pressed */
    int btn_pressed, btn_hover;
    int hover_cell;      /* water tile under the pointer (drawn lighter), or -1 */
    int mx, my, active;  /* pointer position, window active */
    int bar_hover, bar_pressed;
    uint32_t rng;
} sonar_t;

static sonar_t S;
static int win = -1;
static rc_t dirty;       /* part of the window to redraw */
static int digit_w[10];  /* FONT_TITLE digit widths */

/* command strip: the three levels, then "?" at the right */
enum { CM_BEGINNER, CM_INTER, CM_EXPERT, CM_HELP, NBAR };
static const char *const bar_label[NBAR] = { "Beginner", "Intermediate", "Expert", "?" };

/* ---------------- geometry ---------------- */
static int grid_x(void) { return MARGIN + FRAME; }
static int grid_y(void) { return BAR_H + MARGIN + PANEL_H + 8 + FRAME; }
static void client_size(sonar_t *s, int *cw, int *ch) {
    *cw = s->cols * TS + 2 * (MARGIN + FRAME);
    *ch = BAR_H + MARGIN + PANEL_H + 8 + s->rows * TS + 2 * FRAME + MARGIN;
}
static rc_t panel_rect(void) { return R(MARGIN, BAR_H + MARGIN, pw - 2 * MARGIN, PANEL_H); }
static rc_t button_rect(void) {
    rc_t p = panel_rect();
    return R(p.x + (p.w - 36) / 2, p.y + (p.h - 36) / 2, 36, 36);
}
static rc_t lcd_rect(int right) {
    rc_t p = panel_rect();
    return right ? R(p.x + p.w - 64, p.y + 8, 56, 30) : R(p.x + 8, p.y + 8, 56, 30);
}
static rc_t cell_rect(sonar_t *s, int i) { return R(grid_x() + (i % s->cols) * TS, grid_y() + (i / s->cols) * TS, TS, TS); }
static int cell_at(sonar_t *s, int x, int y) {
    x -= grid_x(); y -= grid_y();
    if (x < 0 || y < 0) return -1;
    int cx = x / TS, cy = y / TS;
    if (cx >= s->cols || cy >= s->rows) return -1;
    return cy * s->cols + cx;
}
static rc_t bar_rect(int i) {
    if (i == CM_HELP) {
        int w = gfx_text_width(bar_label[i], FONT_UI) + 8;
        return R(pw - 2 - w, 1, w, BAR_H - 3);
    }
    int x = 2;
    for (int j = 0; j < i; j++) x += gfx_text_width(bar_label[j], FONT_UI) + 8;
    return R(x, 1, gfx_text_width(bar_label[i], FONT_UI) + 8, BAR_H - 3);
}
static int bar_at(int x, int y) {
    for (int i = 0; i < NBAR; i++) if (rc_has(bar_rect(i), x, y)) return i;
    return -1;
}

/* ---------------- redrawing ---------------- */
static void mark(rc_t r) { dirty = rc_or(dirty, r); }
static void mark_all(void) { dirty = R(0, 0, pw, ph); }
static void mark_cell(sonar_t *s, int c, int chord) {
    if (c < 0) return;
    rc_t r = cell_rect(s, c);
    mark(chord ? rc_and(R(r.x - TS, r.y - TS, 3 * TS, 3 * TS), R(grid_x(), grid_y(), s->cols * TS, s->rows * TS)) : r);
}

/* ---------------- game logic ---------------- */
static uint32_t rnd(sonar_t *s) {
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return s->rng;
}
static uint32_t entropy(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi ^ (uint32_t)time_ms();
}

static void new_game(sonar_t *s) {
    const level_t *l = &levels[s->level];
    s->cols = l->cols; s->rows = l->rows; s->nmines = l->mines;
    memset(s->cell, 0, sizeof s->cell);
    memset(s->adj, 0, sizeof s->adj);
    s->state = ST_READY;
    s->flags = s->opened = 0;
    s->seconds = 0;
    s->press_cell = -1; s->press_chord = 0;
    if (win >= 0) win_set_timer(win, 0);
    mark_all();
}

static int near(sonar_t *s, int a, int b) {
    int ax = a % s->cols, ay = a / s->cols, bx = b % s->cols, by = b / s->cols;
    return ax - bx <= 1 && bx - ax <= 1 && ay - by <= 1 && by - ay <= 1;
}

static void lay_mines(sonar_t *s, int safe) {
    int n = s->cols * s->rows;
    /* keep the first click and (if there is room) its neighbours clear */
    int keep_ring = n - 9 >= s->nmines;
    s->rng ^= entropy() * 2654435761u;
    if (!s->rng) s->rng = 0x1234567;
    int placed = 0;
    while (placed < s->nmines) {
        int c = (int)(rnd(s) % (uint32_t)n);
        if (s->cell[c] & C_MINE) continue;
        if (c == safe || (keep_ring && near(s, c, safe))) continue;
        s->cell[c] |= C_MINE;
        placed++;
    }
    for (int i = 0; i < n; i++) {
        int x = i % s->cols, y = i / s->cols, k = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < s->cols && ny < s->rows && (s->cell[ny * s->cols + nx] & C_MINE)) k++;
            }
        s->adj[i] = (uint8_t)k;
    }
    s->state = ST_PLAYING;
    s->start_ms = time_ms();
    win_set_timer(win, 200);
}

static void lose(sonar_t *s, int boom) {
    s->state = ST_LOST;
    s->cell[boom] |= C_BOOM;
    for (int i = 0; i < s->cols * s->rows; i++)
        if ((s->cell[i] & C_MINE) && !(s->cell[i] & C_FLAG)) s->cell[i] |= C_OPEN;
}

/* open one cell with flood fill of empty water; returns 0 if a mine was hit */
static int reveal(sonar_t *s, int c) {
    if (s->cell[c] & (C_OPEN | C_FLAG)) return 1;
    if (s->cell[c] & C_MINE) { lose(s, c); return 0; }
    int sp = 0;
    s->cell[c] |= C_OPEN; s->opened++;
    s->stack[sp++] = c;
    while (sp) {
        int i = s->stack[--sp];
        if (s->adj[i]) continue;
        int x = i % s->cols, y = i / s->cols;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= s->cols || ny >= s->rows) continue;
                int j = ny * s->cols + nx;
                if (s->cell[j] & (C_OPEN | C_FLAG | C_MINE)) continue;
                s->cell[j] |= C_OPEN; s->opened++;
                s->stack[sp++] = j;
            }
    }
    return 1;
}

static int count_flags_around(sonar_t *s, int c) {
    int x = c % s->cols, y = c / s->cols, k = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if ((dx || dy) && nx >= 0 && ny >= 0 && nx < s->cols && ny < s->rows && (s->cell[ny * s->cols + nx] & C_FLAG)) k++;
        }
    return k;
}

static void chord(sonar_t *s, int c) {
    if (!(s->cell[c] & C_OPEN) || !s->adj[c] || count_flags_around(s, c) != s->adj[c]) return;
    int x = c % s->cols, y = c / s->cols;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= s->cols || ny >= s->rows) continue;
            if (!reveal(s, ny * s->cols + nx)) return;
        }
}

static void check_win(sonar_t *s) {
    if (s->state != ST_PLAYING) return;
    if (s->opened != s->cols * s->rows - s->nmines) return;
    s->state = ST_WON;
    uint64_t t = (time_ms() - s->start_ms) / 1000;
    s->seconds = t > 999 ? 999 : (int)t;
    for (int i = 0; i < s->cols * s->rows; i++) if (s->cell[i] & C_MINE) s->cell[i] |= C_FLAG;
    s->flags = s->nmines;
}

/* ---------------- drawing ---------------- */
static uint32_t hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void draw_water(int x, int y, int cx, int cy, int hover) {
    unsigned top = hover ? 0x9AD8F8 : 0x78C2EE, bot = hover ? 0x5AA8E2 : 0x3A8CD0;
    grad_v(R(x, y, TS, TS), top, bot);
    hline(x, y, TS - 1, 0xD2F0FF);
    vline(x, y, TS - 1, 0xB8E4FC);
    hline(x + 1, y + TS - 1, TS - 1, 0x205890);
    vline(x + TS - 1, y + 1, TS - 1, 0x205890);
    hline(x + 1, y + TS - 2, TS - 2, 0x2E72B0);
    vline(x + TS - 2, y + 1, TS - 2, 0x2E72B0);
    uint32_t h = hash(cx, cy);
    if (h % 3 == 0) {
        /* a little wave crest */
        int wx = x + 4 + (int)(h >> 8) % 6, wy = y + 6 + (int)(h >> 16) % 7;
        unsigned c = lighten(top, 55);
        pixel(wx, wy + 1, c);
        hline(wx + 1, wy, 2, c);
        pixel(wx + 3, wy + 1, c);
        hline(wx + 4, wy + 2, 2, c);
        pixel(wx + 6, wy + 1, c);
    }
}

static void draw_sand(int x, int y, int cx, int cy, unsigned base) {
    fill(R(x, y, TS, TS), base);
    hline(x, y + TS - 1, TS, lighten(base, -28));
    vline(x + TS - 1, y, TS, lighten(base, -28));
    uint32_t h = hash(cx + 100, cy + 7);
    for (int i = 0; i < 4; i++) {
        int x1 = x + 2 + (int)((h >> (i * 8)) % 15), y1 = y + 2 + (int)((h >> (i * 8 + 4)) % 15);
        pixel(x1, y1, lighten(base, i & 1 ? -18 : 30));
    }
}

static void draw_mine(int cx, int cy) {
    unsigned body = 0x1F2B36;
    static const int sp[8][2] = { { 0, -7 }, { 5, -5 }, { 7, 0 }, { 5, 5 }, { 0, 7 }, { -5, 5 }, { -7, 0 }, { -5, -5 } };
    for (int i = 0; i < 8; i++) {
        line(cx, cy, cx + sp[i][0], cy + sp[i][1], body);
        fill(R(cx + sp[i][0] - (sp[i][0] > 0), cy + sp[i][1] - (sp[i][1] > 0), 2, 2), body);
    }
    disc(cx, cy, 5, body);
    fill(R(cx - 3, cy - 3, 2, 2), 0xE8F0F8);
    pixel(cx - 1, cy - 3, 0x8090A0);
}

static void draw_buoy(int x, int y) {
    /* pole with a pennant, red and white float, a ripple */
    vline(x + 10, y + 3, 10, 0x3A3A3A);
    for (int i = 0; i < 5; i++) hline(x + 11, y + 3 + i, 6 - (i > 2 ? (i - 2) * 2 : (2 - i) * 2) + 1, 0xF5B547);
    rounded(R(x + 5, y + 11, 11, 6), 3, 15, 0xF2563E, 0xB8261A);
    hline(x + 5, y + 13, 11, 0xFFFFFF);
    hline(x + 3, y + 17, 15, 0x2E72B0);
}

static void draw_cross(int x, int y) {
    for (int d = 0; d < 2; d++) {
        line(x + 3 + d, y + 3, x + 15 + d, y + 15, 0xD02818);
        line(x + 15 + d, y + 3, x + 3 + d, y + 15, 0xD02818);
    }
}

static const unsigned num_colors[9] = { 0, 0x1E5BD8, 0x23863A, 0xD0362A, 0x1A2E8C, 0x8C1C1C, 0x138A8A, 0x202020, 0x707070 };

static void draw_cell(sonar_t *s, int i) {
    int cx = i % s->cols, cy = i / s->cols;
    int x = grid_x() + cx * TS, y = grid_y() + cy * TS;
    if (rc_empty(rc_and(R(x, y, TS, TS), clip))) return;
    uint8_t c = s->cell[i];
    int over = s->state == ST_WON || s->state == ST_LOST;
    if (c & C_OPEN) {
        draw_sand(x, y, cx, cy, (c & C_BOOM) ? 0xF0604A : 0xEEDFB2);
        if (c & C_MINE) draw_mine(x + TS / 2, y + TS / 2);
        else if (s->adj[i]) {
            char t[2] = { (char)('0' + s->adj[i]), 0 };
            text_center(R(x, y, TS - 1, TS - 1), t, num_colors[s->adj[i]], FONT_TITLE, digit_w[s->adj[i]]);
        }
        return;
    }
    int pressed = 0;
    if (!over && s->press_cell >= 0 && !(c & C_FLAG)) {
        if (s->press_chord) pressed = s->press_cell != i && near(s, s->press_cell, i);
        else pressed = s->press_cell == i;
    }
    if (pressed) { draw_sand(x, y, cx, cy, 0xE2D2A2); return; }
    draw_water(x, y, cx, cy, !over && s->hover_cell == i);
    if (c & C_FLAG) {
        if (s->state == ST_LOST && !(c & C_MINE)) { draw_mine(x + TS / 2, y + TS / 2); draw_cross(x, y); }
        else draw_buoy(x, y);
    }
}

static void draw_lcd(rc_t r, int value) {
    rounded(r, 4, 15, 0x03141C, 0x072530);
    rounded_outline(r, 4, 15, 0x5C7E96);
    /* faint sonar sweep line */
    hline(r.x + 4, r.y + r.h / 2, r.w - 8, 0x0B3340);
    char t[8];
    if (value < 0) snprintf(t, sizeof t, "-%02d", -value > 99 ? 99 : -value);
    else snprintf(t, sizeof t, "%03d", value > 999 ? 999 : value);
    text_center(r, "888", 0x0C3A38, FONT_LARGE, -1);
    text_center(r, t, 0x5CF2B4, FONT_LARGE, -1);
}

static void draw_sub(rc_t b, int mood, int pressed) {
    /* mood: 0 normal, 1 holding breath (tile pressed), 2 won, 3 lost */
    int x = b.x + (b.w - 32) / 2 + pressed, y = b.y + (b.h - 32) / 2 + pressed;
    unsigned top = 0xFFE680, bot = 0xE8A820, ln = 0x8A5E10;
    if (mood == 3) { top = 0xC8C8C8; bot = 0x8A8A8A; ln = 0x505050; }
    /* water */
    grad_v(R(b.x + 3, y + 24, b.w - 6, b.y + b.h - 3 - (y + 24)), 0x8CCDF2, 0x3A8CD0);
    /* periscope */
    int py = mood == 1 ? y + 1 : y + 4;
    fill(R(x + 17, py, 2, y + 12 - py), 0x5A6470);
    fill(R(x + 17, py, 6, 3), 0x5A6470);
    fill(R(x + 22, py, 2, 3), mood == 2 ? 0x40E080 : 0x9FD8FF);
    /* tower and hull */
    rounded(R(x + 11, y + 10, 10, 8), 3, 3, top, bot);
    rounded_outline(R(x + 11, y + 10, 10, 8), 3, 3, ln);
    rounded(R(x + 3, y + 16, 26, 11), 5, 15, top, bot);
    rounded_outline(R(x + 3, y + 16, 26, 11), 5, 15, ln);
    hline(x + 7, y + 17, 18, lighten(top, 60));
    /* propeller */
    vline(x + 1, y + 18, 7, ln);
    hline(x + 1, y + 21, 3, ln);
    /* portholes */
    for (int i = 0; i < 3; i++) {
        int hx = x + 10 + i * 6, hy = y + 21;
        if (mood == 3) {
            line(hx - 1, hy - 1, hx + 1, hy + 1, 0x202020);
            line(hx + 1, hy - 1, hx - 1, hy + 1, 0x202020);
        } else {
            fill(R(hx - 1, hy - 1, 3, 3), 0x2E72B0);
            pixel(hx - 1, hy - 1, 0xD2F0FF);
        }
    }
    if (mood == 2) {
        /* sparkles */
        hline(x + 3, y + 6, 5, 0xF5B547); vline(x + 5, y + 4, 5, 0xF5B547);
        hline(x + 25, y + 9, 3, 0xF5B547); vline(x + 26, y + 8, 3, 0xF5B547);
    }
    if (mood == 3) {
        /* bubbles */
        rect(R(x + 24, y + 6, 3, 3), 0xFFFFFF);
        rect(R(x + 27, y + 1, 4, 4), 0xFFFFFF);
    }
}

/* the command strip, drawn like the kernel's menu bar; the current level
 * is shown as a checked toolbar button */
static void draw_bar(sonar_t *s) {
    fill(R(0, 0, pw, BAR_H), TH_FACE);
    hline(0, BAR_H - 1, pw, lighten(TH_FACE, -25));
    for (int i = 0; i < NBAR; i++) {
        rc_t r = bar_rect(i);
        int act = s->bar_pressed == i && s->bar_hover == i, hov = s->bar_hover == i || s->bar_pressed == i;
        int checked = i == s->level;
        unsigned tc = 0x000000;
        if (act) { fill(r, TH_SELECT); tc = TH_SELECT_TEXT; }
        else if (checked) {
            fill(r, lighten(TH_SELECT, hov ? 190 : 170));
            rect(r, lighten(TH_SELECT, 20));
            tc = lighten(TH_SELECT, -60);
        } else if (hov) rect(r, lighten(TH_SELECT, 60));
        text_center(r, bar_label[i], tc, FONT_UI, -1);
    }
}

static void render(sonar_t *s) {
    int cw = pw, ch = ph, mb = BAR_H;
    draw_bar(s);
    grad_v(R(0, mb, cw, ch - mb), lighten(TH_FACE, 40), TH_FACE);

    /* status panel */
    rc_t p = panel_rect();
    if (!rc_empty(rc_and(p, clip))) {
        rounded(p, 6, 15, 0x24476E, 0x0F2742);
        rounded_outline(p, 6, 15, 0x0A1A2E);
        hline(p.x + 5, p.y + 1, p.w - 10, 0x4A73A0);
        draw_lcd(lcd_rect(0), s->nmines - s->flags);
        draw_lcd(lcd_rect(1), s->seconds);
        rc_t b = button_rect();
        int bp = s->btn_pressed && s->btn_hover;
        unsigned bt = bp ? 0x9FC0DA : s->btn_hover ? 0xF2FAFF : 0xDCEBF6;
        unsigned bb = bp ? 0xDCEBF6 : s->btn_hover ? 0xB2D2EA : 0x9FC0DA;
        rounded(b, 5, 15, bt, bb);
        rounded_outline(b, 5, 15, s->btn_hover ? TH_HOVER : 0x0A1A2E);
        int mood = s->state == ST_WON ? 2 : s->state == ST_LOST ? 3 : s->press_cell >= 0 ? 1 : 0;
        rc_t saved = clip;
        clip = rc_and(clip, rc_inset(b, 2));
        draw_sub(b, mood, bp);
        clip = saved;
    }

    /* grid frame */
    rc_t gr = R(grid_x() - FRAME, grid_y() - FRAME, s->cols * TS + 2 * FRAME, s->rows * TS + 2 * FRAME);
    rounded(gr, 3, 15, 0x1A3E66, 0x0E2440);
    for (int i = 0; i < s->cols * s->rows; i++) draw_cell(s, i);
}

/* grow a part to be redrawn so that it covers every text it touches
 * completely (text is drawn over the blitted pixels, so drawing one over
 * pixels that were not redrawn would blend it twice) */
static rc_t snap(sonar_t *s, rc_t r) {
    rc_t g = R(grid_x(), grid_y(), s->cols * TS, s->rows * TS);
    for (int pass = 0; pass < 4; pass++) {
        rc_t old = r, in = rc_and(r, g);
        if (!rc_empty(in)) {
            int x0 = (in.x - g.x) / TS, y0 = (in.y - g.y) / TS;
            int x1 = (in.x + in.w - g.x + TS - 1) / TS, y1 = (in.y + in.h - g.y + TS - 1) / TS;
            r = rc_or(r, R(g.x + x0 * TS, g.y + y0 * TS, (x1 - x0) * TS, (y1 - y0) * TS));
        }
        for (int i = 0; i < 2; i++) if (!rc_empty(rc_and(r, lcd_rect(i)))) r = rc_or(r, lcd_rect(i));
        if (r.y < BAR_H) r = rc_or(r, R(0, 0, pw, BAR_H));
        if (old.x == r.x && old.y == r.y && old.w == r.w && old.h == r.h) break;
    }
    return r;
}

/* redraw the dirty part of the window */
static void paint(void) {
    rc_t r = rc_and(snap(&S, dirty), R(0, 0, pw, ph));
    dirty = R(0, 0, 0, 0);
    if (rc_empty(r) || !px) return;
    clip = r;
    render(&S);
    if (r.w == pw) gfx_blit(win, 0, r.y, pw, r.h, px + r.y * pw);
    else {
        for (int y = 0; y < r.h; y++) memcpy(tmp + y * r.w, px + (r.y + y) * pw + r.x, (size_t)r.w * 4);
        gfx_blit(win, r.x, r.y, r.w, r.h, tmp);
    }
    for (int i = 0; i < ntq; i++) gfx_text(win, tq[i].x, tq[i].y, tq[i].s, tq[i].color, tq[i].font);
    ntq = 0;
    win_invalidate(win);
}

/* the pixel buffers for the current window size */
static int alloc_buffers(void) {
    int w, h;
    win_size(win, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    if (w == pw && h == ph && px) return 1;
    free(px); free(tmp);
    px = malloc((size_t)w * (size_t)h * 4);
    tmp = malloc((size_t)w * (size_t)h * 4);
    if (!px || !tmp) { free(px); free(tmp); px = tmp = NULL; pw = ph = 0; return 0; }
    pw = w; ph = h;
    mark_all();
    return 1;
}

/* ---------------- commands ---------------- */
static void finish_move(sonar_t *s) {
    check_win(s);
    if (s->state != ST_PLAYING) win_set_timer(win, 0);
    mark_all();
    paint();
    if (s->state == ST_LOST) {
        msgbox(win, "Sonar", "Boom! You steered into a sea mine.\n\nThe bay has been surveyed: every remaining mine is now shown.", MBX_ICON_WARNING, MBX_OK);
    } else if (s->state == ST_WON) {
        char msg[200];
        int lvl = s->level, rec = s->seconds < best_time[lvl];
        if (rec) best_time[lvl] = s->seconds;
        snprintf(msg, sizeof msg, "Bay cleared! You charted all %d sea mines in %d second%s.%s", s->nmines, s->seconds,
                 s->seconds == 1 ? "" : "s", rec ? "\n\nThat is a new best time for this level." : "");
        msgbox(win, "Sonar", msg, MBX_ICON_INFO, MBX_OK);
    }
}

static void set_level(sonar_t *s, int lvl) {
    s->level = lvl;
    new_game(s);
    s->hover_cell = -1;
    int cw, ch;
    client_size(s, &cw, &ch);
    if (cw != pw || ch != ph) {
        win_resize(win, cw, ch);           /* EVT_RESIZE + EVT_PAINT follow */
        alloc_buffers();
    }
    mark_all();
}

/* How to Play and the best times (a message box shows at most 255 bytes) */
static void show_help(void) {
    char msg[256], t[3][8];
    for (int i = 0; i < 3; i++) {
        if (best_time[i] >= 999) strlcpy(t[i], "-", sizeof t[i]);
        else snprintf(t[i], sizeof t[i], "%d", best_time[i]);
    }
    snprintf(msg, sizeof msg,
             "Click a water tile to sweep it; its number counts the mines around it. Right click drops a buoy. "
             "Click a number with all its mines marked to sweep the rest. New game: F2.\n\n"
             "Best times (s): Beginner %s, Intermediate %s, Expert %s.", t[0], t[1], t[2]);
    msgbox(win, "How to Play Sonar", msg, MBX_ICON_INFO, MBX_OK);
}

static void command(sonar_t *s, int id) {
    if (id == CM_HELP) show_help();
    else set_level(s, id);
}

/* ---------------- input ---------------- */
static void update_hover(sonar_t *s) {
    int over = s->state == ST_WON || s->state == ST_LOST;
    int h = !over && s->active ? cell_at(s, s->mx, s->my) : -1;
    if (h >= 0 && (s->cell[h] & C_OPEN)) h = -1;
    if (h != s->hover_cell) {
        mark_cell(s, s->hover_cell, 0);
        mark_cell(s, h, 0);
        s->hover_cell = h;
    }
}

static void handle(sonar_t *s, hal_event_t *e) {
    int over = s->state == ST_WON || s->state == ST_LOST;
    rc_t br = button_rect();
    switch (e->type) {
    case EVT_PAINT: mark_all(); break;
    case EVT_RESIZE: alloc_buffers(); break;
    case EVT_FOCUS:
        s->active = e->pressed;
        update_hover(s);
        break;
    case EVT_MOUSE_MOVE: case EVT_MOUSE_LEAVE: {
        int out = e->type == EVT_MOUSE_LEAVE;
        s->mx = out ? -10000 : e->x; s->my = out ? -10000 : e->y;
        int bh = !out && rc_has(br, e->x, e->y);
        if (bh != s->btn_hover) { s->btn_hover = bh; mark(br); }
        int bar = out ? -1 : bar_at(e->x, e->y);
        if (bar != s->bar_hover) { s->bar_hover = bar; mark(R(0, 0, pw, BAR_H)); }
        if (s->press_cell >= 0 && (e->buttons & MOUSE_LEFT)) {
            int c = cell_at(s, e->x, e->y);
            int chord = c >= 0 && (s->cell[c] & C_OPEN);
            if (c != s->press_cell || chord != s->press_chord) {
                mark_cell(s, s->press_cell, s->press_chord);
                mark_cell(s, c, chord);
                if (c < 0) mark(br);       /* the submarine stops holding its breath */
                s->press_cell = c;
                s->press_chord = chord;
            }
        }
        update_hover(s);
        break;
    }
    case EVT_MOUSE_DOWN: case EVT_DBLCLICK: {
        s->active = 1;
        if (e->button == MOUSE_LEFT && e->y < BAR_H) {
            s->bar_pressed = s->bar_hover = bar_at(e->x, e->y);
            mark(R(0, 0, pw, BAR_H));
            break;
        }
        if (rc_has(br, e->x, e->y) && e->button == MOUSE_LEFT) { s->btn_pressed = 1; s->btn_hover = 1; mark(br); break; }
        int c = cell_at(s, e->x, e->y);
        if (c < 0 || over) break;
        if (e->button == MOUSE_RIGHT) {
            if (!(s->cell[c] & C_OPEN)) {
                s->cell[c] ^= C_FLAG;
                s->flags += (s->cell[c] & C_FLAG) ? 1 : -1;
                mark_cell(s, c, 0);
                mark(lcd_rect(0));
            }
        } else if (e->button == MOUSE_LEFT || e->button == MOUSE_MIDDLE) {
            s->press_cell = c;
            s->press_chord = (s->cell[c] & C_OPEN) || e->button == MOUSE_MIDDLE;
            mark_cell(s, c, s->press_chord);
            mark(br);
        }
        break;
    }
    case EVT_MOUSE_UP: {
        if (s->bar_pressed >= 0 && e->button == MOUSE_LEFT) {
            int p = s->bar_pressed;
            s->bar_pressed = -1;
            mark(R(0, 0, pw, BAR_H));
            paint();
            if (bar_at(e->x, e->y) == p) command(s, p);
            break;
        }
        if (s->btn_pressed && e->button == MOUSE_LEFT) {
            s->btn_pressed = 0;
            if (rc_has(br, e->x, e->y)) new_game(s);
            mark(br);
            break;
        }
        if (e->button != MOUSE_LEFT && e->button != MOUSE_MIDDLE) break;
        int c = s->press_cell, ch = s->press_chord;
        if (c >= 0) { mark_cell(s, c, ch); mark(br); }
        s->press_cell = -1; s->press_chord = 0;
        if (c < 0 || over || cell_at(s, e->x, e->y) != c) break;
        if (ch) chord(s, c);
        else if (!(s->cell[c] & (C_FLAG | C_OPEN))) {
            if (s->state == ST_READY) lay_mines(s, c);
            reveal(s, c);
        }
        finish_move(s);
        update_hover(s);
        break;
    }
    case EVT_KEY:
        if (e->pressed && e->id == KEY_F2) new_game(s);
        else if (e->pressed && e->id == KEY_F1) show_help();
        break;
    case EVT_TIMER: {
        if (s->state != ST_PLAYING) { win_set_timer(win, 0); break; }
        uint64_t t = (time_ms() - s->start_ms) / 1000;
        int sec = t > 999 ? 999 : (int)t;
        if (sec != s->seconds) { s->seconds = sec; mark(lcd_rect(1)); }
        break;
    }
    }
}

int main(int argc, char **argv) {
    sonar_t *s = &S;
    s->rng = entropy() | 1;
    s->level = 0;
    s->hover_cell = s->bar_hover = s->bar_pressed = -1;
    s->active = 1;
    s->mx = s->my = -10000;
    new_game(s);
    for (int i = 1; i <= 8; i++) { char t[2] = { (char)('0' + i), 0 }; digit_w[i] = gfx_text_width(t, FONT_TITLE); }
    int cw, ch;
    client_size(s, &cw, &ch);
    win = win_create("Sonar", cw, ch, WIN_NO_MAX);
    if (win < 0) { printf("Sonar needs the desktop (%d)\n", win); return 1; }
    win_set_icon(win, ICON_GAME);
    if (!alloc_buffers()) return 1;
    for (;;) {
        hal_event_t ev;
        /* handle everything queued, then redraw what changed */
        if (!win_get_event(&ev, rc_empty(dirty) ? -1 : 0)) { paint(); continue; }
        if (ev.type == EVT_CLOSE) break;
        handle(s, &ev);
    }
    win_destroy(win);
    return 0;
}
