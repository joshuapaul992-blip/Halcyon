/* Halcyon screen savers: the common part (one header, included once by each
 * saver; see docs/DESIGN-M8-SCREENSAVER-SETTINGS.md).
 *
 * A screen saver is an ordinary program named NAME.SCR in C:\PROGRAMS,
 * started the way Windows starts them:
 *   /s            full screen until input (the window manager starts it after
 *                 the wait, ends it on input and hides the pointer)
 *   /p <handle>   draw in the preview box of Display Properties
 *   /c            its settings dialog (no options: a message says so)
 *   (nothing)     as /c
 * Its display name is an ELF note (owner "Halcyon", type 1), written by
 * SAVER_NAME("Starfield"), which Display Properties lists.
 *
 * A saver defines saver_start (the window exists or changed size; draw the
 * first frame), saver_frame (draw the next one: dt is the milliseconds since
 * the last) and saver_config (/c), and draws straight into s->px (0x00RRGGBB,
 * s->stride pixels per row); the frame is then shown. Frames come every
 * SAVER_MS (default 33: about 30 per second), from a window timer, so a
 * saver uses no CPU between them. */
#ifndef SAVER_H
#define SAVER_H
#include "hal.h"

#ifndef SAVER_MS
#define SAVER_MS 33
#endif

typedef struct saver {
    int win;
    int w, h, stride;
    unsigned *px;
    int preview;                /* in the Display Properties box (small) */
    int fullscreen;             /* /s */
    uint64_t t0;                /* time_ms() when it started */
    unsigned frame;
    int ndirty;                 /* sv_dirty() rectangles of this frame */
    int dirty[8][4];
} saver_t;

static void saver_start(saver_t *s);
static int  saver_frame(saver_t *s, int dt);     /* 1: the frame changed */
static void saver_config(void);

/* the name Display Properties shows */
#define SAVER_NAME(n) \
    __attribute__((section(".note.halcyon"), used, aligned(4))) \
    static const struct { unsigned namesz, descsz, type; char name[8]; char desc[(sizeof(n) + 3) & ~3u]; } \
    saver_note = { 8, sizeof(n), 1, "Halcyon", n }

/* ---------------- small helpers for the savers ---------------- */
static unsigned sv_seed = 1;
static __attribute__((unused)) unsigned sv_rand(void) { sv_seed = sv_seed * 1664525u + 1013904223u; return sv_seed >> 8; }
static __attribute__((unused)) int sv_range(int lo, int hi) { return hi <= lo ? lo : lo + (int)(sv_rand() % (unsigned)(hi - lo + 1)); }

static __attribute__((unused)) void sv_clear(saver_t *s, unsigned c) {
    for (int y = 0; y < s->h; y++) {
        unsigned *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = c;
    }
}
static __attribute__((unused)) inline void sv_pixel(saver_t *s, int x, int y, unsigned c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[(size_t)y * s->stride + x] = c;
}
static __attribute__((unused)) void sv_fill(saver_t *s, int x, int y, int w, int h, unsigned c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->w ? s->w : x + w, y1 = y + h > s->h ? s->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        unsigned *row = s->px + (size_t)yy * s->stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}
static __attribute__((unused)) void sv_line(saver_t *s, int x0, int y0, int x1, int y1, unsigned c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int n = 0; n < 20000; n++) {
        sv_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static __attribute__((unused)) unsigned sv_mix(unsigned a, unsigned b, int t) {      /* t 0..256: a .. b */
    int r = (int)((a >> 16) & 255) + ((((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t) >> 8);
    int g = (int)((a >> 8) & 255) + ((((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t) >> 8);
    int bl = (int)(a & 255) + ((((int)(b & 255) - (int)(a & 255)) * t) >> 8);
    return (unsigned)(r << 16 | g << 8 | bl);
}
/* a colour on a hue wheel: h 0..1535 */
static __attribute__((unused)) unsigned sv_hue(int h) {
    h %= 1536;
    if (h < 0) h += 1536;
    int x = h % 256, r, g, b;
    switch (h / 256) {
    case 0: r = 255; g = x; b = 0; break;
    case 1: r = 255 - x; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = x; break;
    case 3: r = 0; g = 255 - x; b = 255; break;
    case 4: r = x; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - x; break;
    }
    return (unsigned)(r << 16 | g << 8 | b);
}

/* only these parts changed this frame (else saver_frame's 1 means all of
 * it): far less for the window manager to copy. More than 8 become one. */
static __attribute__((unused)) void sv_dirty(saver_t *s, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    if (s->ndirty == 8) {
        int *u = s->dirty[0];
        for (int i = 1; i < 8; i++) {
            int *d = s->dirty[i];
            int x1 = u[0] + u[2] > d[0] + d[2] ? u[0] + u[2] : d[0] + d[2], y1 = u[1] + u[3] > d[1] + d[3] ? u[1] + u[3] : d[1] + d[3];
            u[0] = u[0] < d[0] ? u[0] : d[0]; u[1] = u[1] < d[1] ? u[1] : d[1];
            u[2] = x1 - u[0]; u[3] = y1 - u[1];
        }
        s->ndirty = 1;
    }
    int *d = s->dirty[s->ndirty++];
    d[0] = x; d[1] = y; d[2] = w; d[3] = h;
}

static __attribute__((unused)) void sv_map(saver_t *s) {
    hal_canvas_t c;
    if (win_canvas(s->win, &c) == 0) { s->px = c.pixels; s->w = c.w; s->h = c.h; s->stride = c.stride; }
}

/* a saver with no settings */
static __attribute__((unused)) void sv_no_options(const char *name) {
    char t[160];
    snprintf(t, sizeof t, "This screen saver has no options that you can set.");
    msgbox(-1, name, t, MBX_ICON_INFO, MBX_OK);
}

int main(int argc, char **argv) {
    saver_t s;
    memset(&s, 0, sizeof s);
    int mode = 'c', handle = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && a[1]) {
            int m = tolower((unsigned char)a[1]);
            if (m == 's' || m == 'p' || m == 'c') mode = m;
            if (m == 'p') {
                if (a[2] == ':' || a[2] == ' ') handle = atoi(a + 3);
                else if (i + 1 < argc) handle = atoi(argv[++i]);
            }
        }
    }
    if (mode == 'c') { saver_config(); return 0; }
    unsigned seed;
    getrandom(&seed, sizeof seed);
    sv_seed = seed | 1;
    if (mode == 'p') {
        s.win = win_create_preview("Screen saver preview", handle);
        s.preview = 1;
    } else {
        s.win = win_create("Screen saver", 640, 480, WIN_FULLSCREEN);
        s.fullscreen = 1;
    }
    if (s.win < 0) return 1;
    sv_map(&s);
    if (!s.px) return 1;
    s.t0 = time_ms();
    saver_start(&s);
    win_invalidate(s.win);
    win_set_timer(s.win, SAVER_MS);
    uint64_t last = time_ms();
    int ax = -1, ay = -1;
    for (;;) {
        hal_event_t ev;
        int r = win_get_event(&ev, -1);
        if (r < 0) return 0;                     /* the window went away (the preview box closed) */
        if (r == 0) continue;
        switch (ev.type) {
        case EVT_TIMER: {
            uint64_t now = time_ms();
            int dt = (int)(now - last);
            last = now;
            if (dt > 200) dt = 200;              /* after a stall, no huge jumps */
            s.frame++;
            s.ndirty = 0;
            int changed = saver_frame(&s, dt);
            if (s.ndirty)
                for (int i = 0; i < s.ndirty; i++) win_invalidate_rect(s.win, s.dirty[i][0], s.dirty[i][1], s.dirty[i][2], s.dirty[i][3]);
            else if (changed) win_invalidate(s.win);
            /* a preview whose box has gone: stop */
            hal_rect_t wr;
            if (s.frame % 16 == 0 && win_get_rect(s.win, &wr) < 0) return 0;
            break;
        }
        case EVT_RESIZE:
            sv_map(&s);
            saver_start(&s);
            win_invalidate(s.win);
            break;
        case EVT_CLOSE:
            return 0;
        /* started by hand (not by the window manager, which ends a saver
         * itself): any input ends it */
        case EVT_KEY: case EVT_MOUSE_DOWN: case EVT_WHEEL:
            if (!s.preview) return 0;
            break;
        case EVT_MOUSE_MOVE:
            if (s.preview) break;
            if (ax < 0) { ax = ev.x; ay = ev.y; }
            else if (abs(ev.x - ax) > 6 || abs(ev.y - ay) > 6) return 0;
            break;
        }
    }
}

#endif
