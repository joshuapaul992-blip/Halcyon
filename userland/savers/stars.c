/* STARS.SCR - Starfield: flying through stars. Each star is a point in 3D
 * (x, y in -4096..4096, depth z 1..4096, fixed point) moving towards the
 * viewer and projected onto the screen; near stars are bigger, brighter and
 * leave a short streak. Only the pixels drawn in the last frame are cleared,
 * so a frame costs a few thousand pixels whatever the resolution. */
#define SAVER_MS 40                      /* 25 frames a second: every frame is the whole screen */
#include "saver.h"

SAVER_NAME("Starfield");

#define MAXSTARS 2400
#define ZFAR 4096
typedef struct { int x, y, z; int px, py, pz, size; unsigned tint; } star_t;
static star_t st[MAXSTARS];
static int nstars, speed;

static void star_spawn(saver_t *s, star_t *t, int anyz) {
    t->x = sv_range(-4096, 4096);
    t->y = sv_range(-4096, 4096) * s->h / (s->w ? s->w : 1);
    t->z = anyz ? sv_range(64, ZFAR) : ZFAR - sv_range(0, 256);
    t->size = 0;
    static const unsigned tints[4] = { 0xFFFFFF, 0xCFE0FF, 0xFFF2D8, 0xD8FFF4 };
    t->tint = tints[sv_rand() & 3];
}

static void saver_start(saver_t *s) {
    sv_clear(s, 0x000000);
    long area = (long)s->w * s->h;
    nstars = (int)(area / 600);
    if (nstars > MAXSTARS) nstars = MAXSTARS;
    if (nstars < 40) nstars = 40;
    speed = s->preview ? 900 : 1300;           /* depth units per second */
    for (int i = 0; i < nstars; i++) star_spawn(s, &st[i], 1);
}

/* erase or draw a star's last image */
static void plot(saver_t *s, star_t *t, unsigned c, int erase) {
    int n = t->size;
    for (int dy = 0; dy < n; dy++)
        for (int dx = 0; dx < n; dx++) sv_pixel(s, t->px + dx, t->py + dy, c);
    if (n >= 2) {   /* the streak back towards where it came from */
        int sx = t->px + (t->px - s->w / 2) / 24 * -1, sy = t->py + (t->py - s->h / 2) / 24 * -1;
        sv_line(s, t->px, t->py, sx, sy, erase ? 0 : sv_mix(0, c, 110));
    }
}

static int saver_frame(saver_t *s, int dt) {
    int cx = s->w / 2, cy = s->h / 2, f = s->w / 2;
    for (int i = 0; i < nstars; i++) {
        star_t *t = &st[i];
        if (t->size) plot(s, t, 0x000000, 1);
        t->z -= speed * dt / 1000 + 1;
        if (t->z < 16) { star_spawn(s, t, 0); continue; }
        int sx = cx + t->x * f / t->z / 4, sy = cy + t->y * f / t->z / 4;
        if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) { star_spawn(s, t, 0); continue; }
        int near = ZFAR - t->z;                       /* 0 far .. ZFAR near */
        int b = 70 + near * 185 / ZFAR;
        t->px = sx; t->py = sy;
        t->size = s->preview ? 1 : near > ZFAR * 85 / 100 ? 3 : near > ZFAR * 55 / 100 ? 2 : 1;
        plot(s, t, sv_mix(0, t->tint, b), 0);
    }
    return 1;
}

static void saver_config(void) { sv_no_options("Starfield"); }
