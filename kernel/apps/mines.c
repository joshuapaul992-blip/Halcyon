/* Halcyon OS - Sonar: chart the hidden sea mines in a bay.
 *
 * A hidden-mines logic puzzle: every revealed patch of sea floor tells how
 * many of its eight neighbours hide a mine. Right click drops a buoy on a
 * suspected mine. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

#define TS 20            /* tile size */
#define MARGIN 10
#define PANEL_H 46
#define FRAME 3
#define MAX_COLS 30
#define MAX_ROWS 16

#define C_MINE  0x01
#define C_OPEN  0x02
#define C_FLAG  0x04
#define C_BOOM  0x08

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
    u8 cell[MAX_COLS * MAX_ROWS];
    u8 adj[MAX_COLS * MAX_ROWS];
    int stack[MAX_COLS * MAX_ROWS];
    int state, flags, opened;
    u64 start_ms;
    int seconds;
    int press_cell;      /* cell under the held left button, or -1 */
    int press_chord;     /* pressing a revealed number: show its neighbours pressed */
    int btn_pressed, btn_hover;
    u32 rng;
} sonar_t;

static window_t *sonar_win;

/* ---------------- geometry ---------------- */
static int grid_x(void) { return MARGIN + FRAME; }
static int grid_y(window_t *w) { return menubar_height(w) + MARGIN + PANEL_H + 8 + FRAME; }
static void client_size(sonar_t *s, int *cw, int *ch) {
    *cw = s->cols * TS + 2 * (MARGIN + FRAME);
    *ch = MENUBAR_H + MARGIN + PANEL_H + 8 + s->rows * TS + 2 * FRAME + MARGIN;
}
static rect_t panel_rect(window_t *w) { return mkrect(MARGIN, menubar_height(w) + MARGIN, w->client->w - 2 * MARGIN, PANEL_H); }
static rect_t button_rect(window_t *w) {
    rect_t p = panel_rect(w);
    return mkrect(p.x + (p.w - 36) / 2, p.y + (p.h - 36) / 2, 36, 36);
}
static int cell_at(window_t *w, sonar_t *s, int x, int y) {
    x -= grid_x(); y -= grid_y(w);
    if (x < 0 || y < 0) return -1;
    int cx = x / TS, cy = y / TS;
    if (cx >= s->cols || cy >= s->rows) return -1;
    return cy * s->cols + cx;
}

/* ---------------- game logic ---------------- */
static u32 rnd(sonar_t *s) {
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return s->rng;
}

static void new_game(window_t *w, sonar_t *s) {
    const level_t *l = &levels[s->level];
    s->cols = l->cols; s->rows = l->rows; s->nmines = l->mines;
    memset(s->cell, 0, sizeof s->cell);
    memset(s->adj, 0, sizeof s->adj);
    s->state = ST_READY;
    s->flags = s->opened = 0;
    s->seconds = 0;
    s->press_cell = -1; s->press_chord = 0;
    if (w) wm_invalidate(w);
}

static int near(sonar_t *s, int a, int b) {
    int ax = a % s->cols, ay = a / s->cols, bx = b % s->cols, by = b / s->cols;
    return ax - bx <= 1 && bx - ax <= 1 && ay - by <= 1 && by - ay <= 1;
}

static void lay_mines(sonar_t *s, int safe) {
    int n = s->cols * s->rows;
    /* keep the first click and (if there is room) its neighbours clear */
    int keep_ring = n - 9 >= s->nmines;
    s->rng ^= (u32)time_us() * 2654435761u;
    if (!s->rng) s->rng = 0x1234567;
    int placed = 0;
    while (placed < s->nmines) {
        int c = (int)(rnd(s) % (u32)n);
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
        s->adj[i] = (u8)k;
    }
    s->state = ST_PLAYING;
    s->start_ms = time_ms();
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
    s->seconds = (int)MIN(999, (time_ms() - s->start_ms) / 1000);
    for (int i = 0; i < s->cols * s->rows; i++) if (s->cell[i] & C_MINE) s->cell[i] |= C_FLAG;
    s->flags = s->nmines;
}

/* ---------------- drawing ---------------- */
static u32 hash(int x, int y) {
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void draw_water(gc_t *g, int x, int y, int cx, int cy, int hover) {
    color_t top = hover ? HEX(0x9AD8F8) : HEX(0x78C2EE), bot = hover ? HEX(0x5AA8E2) : HEX(0x3A8CD0);
    gfx_gradient_v(g, mkrect(x, y, TS, TS), top, bot);
    gfx_hline(g, x, y, TS - 1, HEX(0xD2F0FF));
    gfx_vline(g, x, y, TS - 1, HEX(0xB8E4FC));
    gfx_hline(g, x + 1, y + TS - 1, TS - 1, HEX(0x205890));
    gfx_vline(g, x + TS - 1, y + 1, TS - 1, HEX(0x205890));
    gfx_hline(g, x + 1, y + TS - 2, TS - 2, HEX(0x2E72B0));
    gfx_vline(g, x + TS - 2, y + 1, TS - 2, HEX(0x2E72B0));
    u32 h = hash(cx, cy);
    if (h % 3 == 0) {
        /* a little wave crest */
        int wx = x + 4 + (int)(h >> 8) % 6, wy = y + 6 + (int)(h >> 16) % 7;
        color_t c = color_lighten(top, 55);
        gfx_pixel(g, wx, wy + 1, c);
        gfx_hline(g, wx + 1, wy, 2, c);
        gfx_pixel(g, wx + 3, wy + 1, c);
        gfx_hline(g, wx + 4, wy + 2, 2, c);
        gfx_pixel(g, wx + 6, wy + 1, c);
    }
}

static void draw_sand(gc_t *g, int x, int y, int cx, int cy, color_t base) {
    gfx_fill(g, mkrect(x, y, TS, TS), base);
    gfx_hline(g, x, y + TS - 1, TS, color_lighten(base, -28));
    gfx_vline(g, x + TS - 1, y, TS, color_lighten(base, -28));
    u32 h = hash(cx + 100, cy + 7);
    for (int i = 0; i < 4; i++) {
        int px = x + 2 + (int)((h >> (i * 8)) % 15), py = y + 2 + (int)((h >> (i * 8 + 4)) % 15);
        gfx_pixel(g, px, py, color_lighten(base, i & 1 ? -18 : 30));
    }
}

static void draw_mine(gc_t *g, int cx, int cy) {
    color_t body = HEX(0x1F2B36);
    static const int sp[8][2] = { { 0, -7 }, { 5, -5 }, { 7, 0 }, { 5, 5 }, { 0, 7 }, { -5, 5 }, { -7, 0 }, { -5, -5 } };
    for (int i = 0; i < 8; i++) {
        gfx_line(g, cx, cy, cx + sp[i][0], cy + sp[i][1], body);
        gfx_fill(g, mkrect(cx + sp[i][0] - (sp[i][0] > 0), cy + sp[i][1] - (sp[i][1] > 0), 2, 2), body);
    }
    gfx_circle(g, cx, cy, 5, body);
    gfx_fill(g, mkrect(cx - 3, cy - 3, 2, 2), HEX(0xE8F0F8));
    gfx_pixel(g, cx - 1, cy - 3, HEX(0x8090A0));
}

static void draw_buoy(gc_t *g, int x, int y) {
    /* pole with a pennant, red and white float, a ripple */
    gfx_vline(g, x + 10, y + 3, 10, HEX(0x3A3A3A));
    for (int i = 0; i < 5; i++) gfx_hline(g, x + 11, y + 3 + i, 6 - (i > 2 ? (i - 2) * 2 : (2 - i) * 2) + 1, HEX(0xF5B547));
    gfx_rounded(g, mkrect(x + 5, y + 11, 11, 6), 3, 15, HEX(0xF2563E), HEX(0xB8261A));
    gfx_hline(g, x + 5, y + 13, 11, HEX(0xFFFFFF));
    gfx_hline(g, x + 3, y + 17, 15, HEX(0x2E72B0));
}

static void draw_cross(gc_t *g, int x, int y) {
    for (int d = 0; d < 2; d++) {
        gfx_line(g, x + 3 + d, y + 3, x + 15 + d, y + 15, HEX(0xD02818));
        gfx_line(g, x + 15 + d, y + 3, x + 3 + d, y + 15, HEX(0xD02818));
    }
}

static const u32 num_colors[9] = { 0, 0x1E5BD8, 0x23863A, 0xD0362A, 0x1A2E8C, 0x8C1C1C, 0x138A8A, 0x202020, 0x707070 };

static void draw_cell(window_t *w, sonar_t *s, gc_t *g, int i) {
    int cx = i % s->cols, cy = i / s->cols;
    int x = grid_x() + cx * TS, y = grid_y(w) + cy * TS;
    u8 c = s->cell[i];
    int over = s->state == ST_WON || s->state == ST_LOST;
    if (c & C_OPEN) {
        draw_sand(g, x, y, cx, cy, (c & C_BOOM) ? HEX(0xF0604A) : HEX(0xEEDFB2));
        if (c & C_MINE) draw_mine(g, x + TS / 2, y + TS / 2);
        else if (s->adj[i]) {
            char t[2] = { (char)('0' + s->adj[i]), 0 };
            gfx_text_box(g, &font_title, mkrect(x, y, TS - 1, TS - 1), t, HEX(num_colors[s->adj[i]]), 1);
        }
        return;
    }
    int pressed = 0;
    if (!over && s->press_cell >= 0 && !(c & C_FLAG)) {
        if (s->press_chord) pressed = s->press_cell != i && near(s, s->press_cell, i);
        else pressed = s->press_cell == i;
    }
    if (pressed) { draw_sand(g, x, y, cx, cy, HEX(0xE2D2A2)); return; }
    int hov = 0;
    if (!over && wm_active() == w) {
        rect_t cr = wm_client_rect(w);
        hov = cell_at(w, s, wm_mouse_x() - cr.x, wm_mouse_y() - cr.y) == i;
    }
    draw_water(g, x, y, cx, cy, hov);
    if (c & C_FLAG) {
        if (s->state == ST_LOST && !(c & C_MINE)) { draw_mine(g, x + TS / 2, y + TS / 2); draw_cross(g, x, y); }
        else draw_buoy(g, x, y);
    }
}

static void draw_lcd(gc_t *g, rect_t r, int value) {
    gfx_rounded(g, r, 4, 15, HEX(0x03141C), HEX(0x072530));
    gfx_rounded_outline(g, r, 4, 15, HEX(0x5C7E96));
    /* faint sonar sweep rings */
    gfx_hline(g, r.x + 4, r.y + r.h / 2, r.w - 8, HEX(0x0B3340));
    char t[8], ghost[] = "888";
    if (value < 0) snprintf(t, sizeof t, "-%02d", MIN(-value, 99));
    else snprintf(t, sizeof t, "%03d", MIN(value, 999));
    int tw = gfx_text_width(&font_large, ghost);
    int tx = r.x + (r.w - tw) / 2, ty = r.y + (r.h - font_large.height) / 2;
    gfx_text(g, &font_large, tx, ty, ghost, HEX(0x0C3A38));
    int vw = gfx_text_width(&font_large, t);
    tx = r.x + (r.w - vw) / 2;
    gfx_text(g, &font_large, tx, ty, t, HEX(0x5CF2B4));
}

static void draw_sub(gc_t *g, rect_t b, int mood, int pressed) {
    /* mood: 0 normal, 1 holding breath (tile pressed), 2 won, 3 lost */
    int x = b.x + (b.w - 32) / 2 + pressed, y = b.y + (b.h - 32) / 2 + pressed;
    color_t top = HEX(0xFFE680), bot = HEX(0xE8A820), line = HEX(0x8A5E10);
    if (mood == 3) { top = HEX(0xC8C8C8); bot = HEX(0x8A8A8A); line = HEX(0x505050); }
    /* water */
    gfx_gradient_v(g, mkrect(b.x + 3, y + 24, b.w - 6, b.y + b.h - 3 - (y + 24)), HEX(0x8CCDF2), HEX(0x3A8CD0));
    /* periscope */
    int py = mood == 1 ? y + 1 : y + 4;
    gfx_fill(g, mkrect(x + 17, py, 2, y + 12 - py), HEX(0x5A6470));
    gfx_fill(g, mkrect(x + 17, py, 6, 3), HEX(0x5A6470));
    gfx_fill(g, mkrect(x + 22, py, 2, 3), mood == 2 ? HEX(0x40E080) : HEX(0x9FD8FF));
    /* tower and hull */
    gfx_rounded(g, mkrect(x + 11, y + 10, 10, 8), 3, 3, top, bot);
    gfx_rounded_outline(g, mkrect(x + 11, y + 10, 10, 8), 3, 3, line);
    gfx_rounded(g, mkrect(x + 3, y + 16, 26, 11), 5, 15, top, bot);
    gfx_rounded_outline(g, mkrect(x + 3, y + 16, 26, 11), 5, 15, line);
    gfx_hline(g, x + 7, y + 17, 18, color_lighten(top, 60));
    /* propeller */
    gfx_vline(g, x + 1, y + 18, 7, line);
    gfx_hline(g, x + 1, y + 21, 3, line);
    /* portholes */
    for (int i = 0; i < 3; i++) {
        int hx = x + 10 + i * 6, hy = y + 21;
        if (mood == 3) {
            gfx_line(g, hx - 1, hy - 1, hx + 1, hy + 1, HEX(0x202020));
            gfx_line(g, hx + 1, hy - 1, hx - 1, hy + 1, HEX(0x202020));
        } else {
            gfx_fill(g, mkrect(hx - 1, hy - 1, 3, 3), HEX(0x2E72B0));
            gfx_pixel(g, hx - 1, hy - 1, HEX(0xD2F0FF));
        }
    }
    if (mood == 2) {
        /* sparkles */
        gfx_hline(g, x + 3, y + 6, 5, HEX(0xF5B547)); gfx_vline(g, x + 5, y + 4, 5, HEX(0xF5B547));
        gfx_hline(g, x + 25, y + 9, 3, HEX(0xF5B547)); gfx_vline(g, x + 26, y + 8, 3, HEX(0xF5B547));
    }
    if (mood == 3) {
        /* bubbles */
        gfx_rect(g, mkrect(x + 24, y + 6, 3, 3), HEX(0xFFFFFF));
        gfx_rect(g, mkrect(x + 27, y + 1, 4, 4), HEX(0xFFFFFF));
    }
}

static void sonar_paint(window_t *w, gc_t *g) {
    sonar_t *s = w->data;
    const theme_t *th = g_theme;
    int cw = w->client->w, ch = w->client->h, mb = menubar_height(w);
    gfx_gradient_v(g, mkrect(0, mb, cw, ch - mb), color_lighten(th->face, 40), th->face);

    /* status panel */
    rect_t p = panel_rect(w);
    gfx_rounded(g, p, 6, 15, HEX(0x24476E), HEX(0x0F2742));
    gfx_rounded_outline(g, p, 6, 15, HEX(0x0A1A2E));
    gfx_hline(g, p.x + 5, p.y + 1, p.w - 10, HEX(0x4A73A0));
    rect_t l1 = mkrect(p.x + 8, p.y + 8, 56, 30), l2 = mkrect(p.x + p.w - 64, p.y + 8, 56, 30);
    draw_lcd(g, l1, s->nmines - s->flags);
    draw_lcd(g, l2, s->seconds);
    rect_t b = button_rect(w);
    int bp = s->btn_pressed && s->btn_hover;
    color_t bt = bp ? HEX(0x9FC0DA) : s->btn_hover ? HEX(0xF2FAFF) : HEX(0xDCEBF6);
    color_t bb = bp ? HEX(0xDCEBF6) : s->btn_hover ? HEX(0xB2D2EA) : HEX(0x9FC0DA);
    gfx_rounded(g, b, 5, 15, bt, bb);
    gfx_rounded_outline(g, b, 5, 15, s->btn_hover ? th->hover : HEX(0x0A1A2E));
    int mood = s->state == ST_WON ? 2 : s->state == ST_LOST ? 3 : s->press_cell >= 0 ? 1 : 0;
    gc_t bg = gc_sub(g, rect_inset(b, 2));
    draw_sub(&bg, mkrect(-2, -2, b.w, b.h), mood, bp);

    /* grid frame */
    rect_t gr = mkrect(grid_x() - FRAME, grid_y(w) - FRAME, s->cols * TS + 2 * FRAME, s->rows * TS + 2 * FRAME);
    gfx_rounded(g, gr, 3, 15, HEX(0x1A3E66), HEX(0x0E2440));
    for (int i = 0; i < s->cols * s->rows; i++) draw_cell(w, s, g, i);
}

/* ---------------- input ---------------- */
static void finish_move(window_t *w, sonar_t *s) {
    check_win(s);
    wm_invalidate(w);
    if (s->state == ST_LOST) {
        msgbox(w, "Sonar", "Boom! You steered into a sea mine.\n\nThe bay has been surveyed: every remaining mine is now shown.", ICON_WARNING, MB_OK);
    } else if (s->state == ST_WON) {
        char msg[200];
        int lvl = s->level, rec = s->seconds < best_time[lvl];
        if (rec) best_time[lvl] = s->seconds;
        snprintf(msg, sizeof msg, "Bay cleared! You charted all %d sea mines in %d second%s.%s", s->nmines, s->seconds,
                 s->seconds == 1 ? "" : "s", rec ? "\n\nThat is a new best time for this level." : "");
        msgbox(w, "Sonar", msg, ICON_INFO, MB_OK);
    }
}

static void sonar_event(window_t *w, gui_event_t *e) {
    sonar_t *s = w->data;
    int over = s->state == ST_WON || s->state == ST_LOST;
    rect_t br = button_rect(w);
    switch (e->type) {
    case GE_MOUSE_MOVE: case GE_MOUSE_LEAVE: {
        int bh = e->type != GE_MOUSE_LEAVE && rect_contains(br, e->x, e->y);
        if (bh != s->btn_hover) s->btn_hover = bh;
        if (s->press_cell >= 0 && (e->buttons & MOUSE_LEFT)) {
            int c = cell_at(w, s, e->x, e->y);
            s->press_cell = c;
            s->press_chord = c >= 0 && (s->cell[c] & C_OPEN);
        }
        wm_invalidate(w);   /* hover highlight follows the pointer */
        break;
    }
    case GE_MOUSE_DOWN: case GE_DBLCLICK: {
        if (rect_contains(br, e->x, e->y) && e->button == MOUSE_LEFT) { s->btn_pressed = 1; s->btn_hover = 1; wm_invalidate(w); break; }
        int c = cell_at(w, s, e->x, e->y);
        if (c < 0 || over) break;
        if (e->button == MOUSE_RIGHT) {
            if (!(s->cell[c] & C_OPEN)) {
                s->cell[c] ^= C_FLAG;
                s->flags += (s->cell[c] & C_FLAG) ? 1 : -1;
                wm_invalidate(w);
            }
        } else if (e->button == MOUSE_LEFT || e->button == MOUSE_MIDDLE) {
            s->press_cell = c;
            s->press_chord = (s->cell[c] & C_OPEN) || e->button == MOUSE_MIDDLE;
            wm_invalidate(w);
        }
        break;
    }
    case GE_MOUSE_UP: {
        if (s->btn_pressed && e->button == MOUSE_LEFT) {
            s->btn_pressed = 0;
            if (rect_contains(br, e->x, e->y)) new_game(w, s);
            wm_invalidate(w);
            break;
        }
        if (e->button != MOUSE_LEFT && e->button != MOUSE_MIDDLE) break;
        int c = s->press_cell, ch = s->press_chord;
        s->press_cell = -1; s->press_chord = 0;
        wm_invalidate(w);
        if (c < 0 || over || cell_at(w, s, e->x, e->y) != c) break;
        if (ch) chord(s, c);
        else if (!(s->cell[c] & (C_FLAG | C_OPEN))) {
            if (s->state == ST_READY) lay_mines(s, c);
            reveal(s, c);
        }
        finish_move(w, s);
        break;
    }
    case GE_KEY:
        if (e->key.pressed && e->key.key == KEY_F2) new_game(w, s);
        break;
    }
}

static void sonar_tick(window_t *w) {
    sonar_t *s = w->data;
    if (s->state != ST_PLAYING) return;
    int sec = (int)MIN(999, (time_ms() - s->start_ms) / 1000);
    if (sec != s->seconds) { s->seconds = sec; wm_invalidate(w); }
}

/* ---------------- menus ---------------- */
enum { SM_NEW = 1, SM_BEGINNER, SM_INTER, SM_EXPERT, SM_BEST, SM_EXIT, SM_HOWTO, SM_ABOUT };
static menu_item_t game_items[] = {
    { "New Game", "F2", SM_NEW, MI_BOLD, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Beginner", NULL, SM_BEGINNER, 0, 0 },
    { "Intermediate", NULL, SM_INTER, 0, 0 },
    { "Expert", NULL, SM_EXPERT, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Best Times...", NULL, SM_BEST, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Exit", NULL, SM_EXIT, 0, 0 },
};
static menu_item_t help_items[] = {
    { "How to Play", NULL, SM_HOWTO, 0, ICON_HELP },
    { "About Sonar", NULL, SM_ABOUT, 0, ICON_GAME },
};
static menu_t sonar_menus[] = {
    { "Game", game_items, ARRAY_SIZE(game_items) },
    { "Help", help_items, ARRAY_SIZE(help_items) },
};

static void set_level(window_t *w, sonar_t *s, int lvl) {
    s->level = lvl;
    for (int i = 0; i < 3; i++) {
        if (i == lvl) game_items[2 + i].flags |= MI_CHECKED;
        else game_items[2 + i].flags &= ~MI_CHECKED;
    }
    new_game(NULL, s);
    int cw, ch;
    client_size(s, &cw, &ch);
    if (w->client && (w->client->w != cw || w->client->h != ch)) {
        wm_resize_client(w, cw, ch);
        /* keep the window on screen */
        rect_t f = w->frame;
        int nx = MIN(f.x, screen_w() - f.w), ny = MIN(f.y, screen_h() - TASKBAR_H - f.h);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx != f.x || ny != f.y) {
            wm_invalidate_rect(mkrect(f.x, f.y, f.w + 8, f.h + 8));
            w->frame.x = nx; w->frame.y = ny;
            wm_invalidate_rect(mkrect(nx, ny, f.w + 8, f.h + 8));
        }
    }
    wm_invalidate(w);
}

static void sonar_menu(window_t *w, int id) {
    sonar_t *s = w->data;
    switch (id) {
    case SM_NEW: new_game(w, s); break;
    case SM_BEGINNER: set_level(w, s, 0); break;
    case SM_INTER: set_level(w, s, 1); break;
    case SM_EXPERT: set_level(w, s, 2); break;
    case SM_BEST: {
        char msg[200], t[3][24];
        for (int i = 0; i < 3; i++) {
            if (best_time[i] >= 999) strlcpy(t[i], "not yet", sizeof t[i]);
            else snprintf(t[i], sizeof t[i], "%d seconds", best_time[i]);
        }
        snprintf(msg, sizeof msg, "Fastest surveys this session:\n\nBeginner: %s\nIntermediate: %s\nExpert: %s", t[0], t[1], t[2]);
        msgbox(w, "Best Times", msg, ICON_GAME, MB_OK);
        break;
    }
    case SM_EXIT: wm_close(w); break;
    case SM_HOWTO:
        msgbox(w, "How to Play Sonar",
               "Sea mines are hidden under the water tiles. Left click a tile to sweep it with sonar: "
               "the number on the sand tells how many mines touch that tile.\n\n"
               "Right click to drop a buoy where you think a mine lies. Click a number whose mines are all "
               "marked to sweep the rest of its neighbours.\n\nChart every safe tile to clear the bay. "
               "Your first sweep is always safe.", ICON_HELP, MB_OK);
        break;
    case SM_ABOUT:
        msgbox(w, "About Sonar", "Sonar " OS_VERSION " for Halcyon\n\nA mine-charting puzzle for the open sea.", ICON_GAME, MB_OK);
        break;
    }
}

static void sonar_destroy(window_t *w) {
    kfree(w->data);
    w->data = NULL;
    if (sonar_win == w) sonar_win = NULL;
}

void app_mines(void) {
    if (sonar_win && wm_alive(sonar_win)) { wm_show(sonar_win); return; }
    sonar_t *s = kzalloc(sizeof *s);
    if (!s) return;
    s->rng = (u32)time_us() | 1;
    int lvl = 0;
    for (int i = 0; i < 3; i++) if (game_items[2 + i].flags & MI_CHECKED) lvl = i;
    s->level = lvl;
    new_game(NULL, s);
    int cw, ch;
    client_size(s, &cw, &ch);
    window_t *w = wm_create("Sonar", ICON_GAME, -1, -1, cw, ch, WF_NO_MAXIMIZE);
    w->data = s;
    w->on_paint = sonar_paint;
    w->on_event = sonar_event;
    w->on_tick = sonar_tick;
    w->on_menu = sonar_menu;
    w->on_destroy = sonar_destroy;
    menubar_attach(w, sonar_menus, ARRAY_SIZE(sonar_menus));
    set_level(w, s, lvl);
    sonar_win = w;
}
