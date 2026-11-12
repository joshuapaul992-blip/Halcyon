/* MARQUEE.SCR - Scrolling Text: a line of text glides across the screen
 * from right to left, at a new height each time round. Its settings dialog
 * (/c) sets the text, the font, the size (1x to 4x, smoothed), the colour
 * (plain or a gradient, or a moving rainbow) and the speed, and shows the
 * result as it is set. Saved as marquee.* in SETTINGS.INI.
 *
 * The text is drawn once with the system font into a scratch band of the
 * window, read back as a coverage mask, and from then on scaled and coloured
 * here; a frame clears the text's old place and draws its new one. */
#include "saver.h"

SAVER_NAME("Scrolling Text");

static const struct { const char *name; int font; } fonts[] = {
    { "Normal", FONT_UI }, { "Bold", FONT_UI_BOLD }, { "Title", FONT_TITLE }, { "Large", FONT_LARGE }, { "Huge", FONT_HUGE },
};
#define NFONTS 5
static const struct { const char *name; unsigned top, bottom; } colors[] = {
    { "White", 0xFFFFFF, 0xFFFFFF }, { "Halcyon Blue", 0xA8E8FF, 0x2E6FDB }, { "Sunset", 0xFFE08A, 0xF0602A },
    { "Spring Green", 0xD2FFA0, 0x2BAA4B }, { "Gold", 0xFFF4B8, 0xC8901A }, { "Rainbow", 0, 0 },
};
#define NCOLORS 6
#define RAINBOW 5
#define DEFAULT_TEXT "Halcyon"

typedef struct {
    char text[200];
    int font, color, size, speed;     /* indexes; size 1..4; speed 1..10 */
} conf_t;

static void conf_load(conf_t *c) {
    if (setting_get("marquee.text", c->text, sizeof c->text) <= 0) strlcpy(c->text, DEFAULT_TEXT, sizeof c->text);
    c->font = setting_get_int("marquee.font", 4);
    c->color = setting_get_int("marquee.color", 1);
    c->size = setting_get_int("marquee.size", 3);
    c->speed = setting_get_int("marquee.speed", 4);
    if (c->font < 0 || c->font >= NFONTS) c->font = 4;
    if (c->color < 0 || c->color >= NCOLORS) c->color = 1;
    if (c->size < 1 || c->size > 4) c->size = 3;
    if (c->speed < 1 || c->speed > 10) c->speed = 4;
}

/* ---------------- the moving text ---------------- */
typedef struct {
    conf_t c;
    unsigned char *mask;              /* coverage, mw x mh */
    int mw, mh, scale;
    int x, y;                         /* 1/256 pixel */
    int dx, dy, dw, dh, drawn;        /* the last frame's rectangle */
    int org_x, org_y;                 /* where s->px starts in the window (the dialog's strip) */
    int phase;                        /* rainbow */
} mq_t;

static mq_t mq;

/* draw the text with the system font into the top of s (cleared again
 * afterwards) and keep what it covered */
static void mask_build(saver_t *s, mq_t *m) {
    free(m->mask);
    m->mask = NULL;
    int font = fonts[m->c.font].font;
    const char *t = m->c.text[0] ? m->c.text : " ";
    m->mw = gfx_text_width(t, font) + 2;
    m->mh = gfx_font_height(font);
    if (m->mw < 1 || m->mh < 1 || m->mh > s->h) { m->mw = m->mh = 0; return; }
    m->mask = calloc((size_t)m->mw * m->mh, 1);
    if (!m->mask) { m->mw = m->mh = 0; return; }
    int band = s->w;
    for (int o = 0; o < m->mw; o += band) {
        sv_fill(s, 0, 0, band, m->mh, 0x000000);
        gfx_clip(s->win, m->org_x, m->org_y, band, m->mh);
        gfx_text(s->win, m->org_x - o, m->org_y, t, 0xFFFFFF, font);
        gfx_clip_reset(s->win);
        for (int y = 0; y < m->mh; y++)
            for (int x = 0; x < band && o + x < m->mw; x++) {
                unsigned p = s->px[(size_t)y * s->stride + x];
                unsigned r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                m->mask[(size_t)y * m->mw + o + x] = (unsigned char)(r > g ? (r > b ? r : b) : (g > b ? g : b));
            }
    }
    sv_fill(s, 0, 0, band, m->mh, 0x000000);
}

static void mq_place(saver_t *s, mq_t *m, int first) {
    int th = m->mh * m->scale;
    int room = s->h - th;
    m->y = (room > 0 ? (first && s->preview ? room / 2 : sv_range(0, room)) : 0) << 8;
    m->x = (first && s->preview ? s->w / 3 : s->w) << 8;
}

static void mq_start(saver_t *s, mq_t *m) {
    sv_clear(s, 0x000000);
    mask_build(s, m);
    m->scale = s->preview ? 1 : m->c.size;
    while (m->scale > 1 && m->mh * m->scale > s->h) m->scale--;
    m->drawn = 0;
    mq_place(s, m, 1);
}

/* the colour of a text pixel at (u, v): v 0..255 down the text */
static unsigned shade(mq_t *m, int u, int v) {
    if (m->c.color == RAINBOW) return sv_hue(u * 3 + m->phase);
    return sv_mix(colors[m->c.color].top, colors[m->c.color].bottom, v);
}

static int mq_frame(saver_t *s, mq_t *m, int dt) {
    if (m->drawn) { sv_fill(s, m->dx, m->dy, m->dw, m->dh, 0x000000); sv_dirty(s, m->dx, m->dy, m->dw, m->dh); }
    int pps = (30 + m->c.speed * 34) * (s->preview ? 1 : (s->w + 512) / 1024 + 1);   /* pixels per second */
    m->x -= pps * 256 / 1000 * dt;
    m->phase += dt;
    int S = m->scale, tw = m->mw * S, th = m->mh * S;
    if ((m->x >> 8) + tw < 0) mq_place(s, m, 0);
    int px = m->x >> 8, py = m->y >> 8;
    if (!m->mask) return 1;
    int x0 = px < 0 ? 0 : px, x1 = px + tw > s->w ? s->w : px + tw;
    int y0 = py < 0 ? 0 : py, y1 = py + th > s->h ? s->h : py + th;
    for (int y = y0; y < y1; y++) {
        unsigned *row = s->px + (size_t)y * s->stride;
        /* source row in 1/256 (smooth scaling: bilinear) */
        int sy = ((y - py) * 256 + 128) / S - 128;
        if (sy < 0) sy = 0;
        int ry = sy >> 8, fy = sy & 255;
        int ry2 = ry + 1 < m->mh ? ry + 1 : ry;
        const unsigned char *m1 = m->mask + (size_t)ry * m->mw, *m2 = m->mask + (size_t)ry2 * m->mw;
        int v = (y - py) * 256 / (th ? th : 1);
        for (int x = x0; x < x1; x++) {
            int sx = ((x - px) * 256 + 128) / S - 128;
            if (sx < 0) sx = 0;
            int rx = sx >> 8, fx = sx & 255;
            int rx2 = rx + 1 < m->mw ? rx + 1 : rx;
            int a = (m1[rx] * (256 - fx) + m1[rx2] * fx) * (256 - fy) + (m2[rx] * (256 - fx) + m2[rx2] * fx) * fy;
            a >>= 16;
            if (a <= 0) continue;
            row[x] = sv_mix(0x000000, shade(m, x, v), a * 256 / 255);
        }
    }
    m->dx = px; m->dy = py; m->dw = tw; m->dh = th; m->drawn = 1;
    sv_dirty(s, px, py, tw, th);
    return 1;
}

static void saver_start(saver_t *s) {
    conf_load(&mq.c);
    mq_start(s, &mq);
}
static int saver_frame(saver_t *s, int dt) { return mq_frame(s, &mq, dt); }

/* ---------------- the settings dialog (/c) ---------------- */
enum { C_TEXT = 10, C_FONT, C_COLOR, C_SIZE, C_SPEED, C_SIZE_VAL, C_SPEED_VAL, C_OK, C_CANCEL };
#define DW 420
#define DH 356
#define STRIP_X 14
#define STRIP_Y 226
#define STRIP_W (DW - 28)
#define STRIP_H 78

static void show_values(int win, conf_t *c) {
    char b[16];
    snprintf(b, sizeof b, "%dx", c->size);
    ctl_set_text(win, C_SIZE_VAL, b);
    static const char *const sp[11] = { "", "Very slow", "Slow", "Slow", "Medium", "Medium", "Medium", "Fast", "Fast", "Very fast", "Fastest" };
    ctl_set_text(win, C_SPEED_VAL, sp[c->speed]);
}

static void saver_config(void) {
    conf_t c;
    conf_load(&c);
    int win = win_create("Scrolling Text Settings", DW, DH, WIN_DIALOG);
    if (win < 0) return;
    win_set_icon(win, HICON_DISPLAY);
    ctl_create(win, CTL_LABEL, 14, 16, 60, 18, "Text:", 0, 0);
    ctl_create(win, CTL_TEXTBOX, 80, 13, DW - 94, 23, c.text, C_TEXT, 0);
    ctl_create(win, CTL_GROUP, 12, 46, 190, 118, "Font", 0, 0);
    ctl_create(win, CTL_LISTBOX, 24, 66, 166, 5 * 18 + 2, "", C_FONT, 0);
    const char *fn[NFONTS], *cn[NCOLORS];
    for (int i = 0; i < NFONTS; i++) fn[i] = fonts[i].name;
    for (int i = 0; i < NCOLORS; i++) cn[i] = colors[i].name;
    ctl_list_set(win, C_FONT, fn, NFONTS);
    ctl_set_value(win, C_FONT, c.font);
    ctl_create(win, CTL_GROUP, 216, 46, DW - 228, 118, "Colour", 0, 0);
    ctl_create(win, CTL_LISTBOX, 228, 66, DW - 252, 5 * 18 + 2, "", C_COLOR, 0);
    ctl_list_set(win, C_COLOR, cn, NCOLORS);
    ctl_set_value(win, C_COLOR, c.color);
    ctl_create(win, CTL_LABEL, 14, 176, 60, 18, "Size:", 0, 0);
    ctl_create(win, CTL_HSCROLL, 80, 176, 200, 17, "", C_SIZE, 0);
    ctl_scroll_set(win, C_SIZE, 4, 1, c.size - 1, 1);
    ctl_create(win, CTL_LABEL, 290, 176, 100, 18, "", C_SIZE_VAL, 0);
    ctl_create(win, CTL_LABEL, 14, 202, 60, 18, "Speed:", 0, 0);
    ctl_create(win, CTL_HSCROLL, 80, 202, 200, 17, "", C_SPEED, 0);
    ctl_scroll_set(win, C_SPEED, 10, 1, c.speed - 1, 1);
    ctl_create(win, CTL_LABEL, 290, 202, 110, 18, "", C_SPEED_VAL, 0);
    ctl_create(win, CTL_BUTTON, DW - 2 * 83 - 4, DH - 35, 75, 23, "OK", C_OK, CTL_DEFAULT);
    ctl_create(win, CTL_BUTTON, DW - 83 - 4, DH - 35, 75, 23, "Cancel", C_CANCEL, CTL_CANCEL);
    show_values(win, &c);
    /* the strip shows the text as it will scroll */
    hal_canvas_t cv;
    if (win_canvas(win, &cv)) { win_destroy(win); return; }
    saver_t s;
    memset(&s, 0, sizeof s);
    s.win = win;
    s.preview = 0;
    s.w = STRIP_W - 4; s.h = STRIP_H - 4;
    s.stride = cv.stride;
    s.px = cv.pixels + (size_t)(STRIP_Y + 2) * cv.stride + STRIP_X + 2;
    gfx_shade(win, STRIP_X, STRIP_Y, STRIP_W, STRIP_H, 0x000000, 0, SHADE_SUNKEN, 0);
    mq.c = c;
    mq.org_x = STRIP_X + 2; mq.org_y = STRIP_Y + 2;
    mq_start(&s, &mq);
    mq.scale = 1;                      /* the strip is small: the size shows in the number */
    win_set_timer(win, 33);
    win_invalidate(win);
    uint64_t last = time_ms();
    char text[200];
    strlcpy(text, c.text, sizeof text);
    for (;;) {
        hal_event_t ev;
        if (win_get_event(&ev, -1) <= 0) break;
        int rebuild = 0;
        if (ev.type == EVT_CLOSE) break;
        if (ev.type == EVT_COMMAND && ev.id == C_CANCEL) break;
        if (ev.type == EVT_COMMAND && ev.id == C_OK) {
            ctl_get_text(win, C_TEXT, c.text, sizeof c.text);
            setting_set("marquee.text", c.text);
            setting_set_int("marquee.font", c.font);
            setting_set_int("marquee.color", c.color);
            setting_set_int("marquee.size", c.size);
            setting_set_int("marquee.speed", c.speed);
            break;
        }
        if (ev.type == EVT_COMMAND && ev.id == C_FONT) { int v = ctl_get_value(win, C_FONT); if (v >= 0) c.font = v; rebuild = 1; }
        if (ev.type == EVT_COMMAND && ev.id == C_COLOR) { int v = ctl_get_value(win, C_COLOR); if (v >= 0) c.color = v; mq.c.color = c.color; }
        if (ev.type == EVT_SCROLL && ev.id == C_SIZE) { c.size = ev.x + 1; show_values(win, &c); }
        if (ev.type == EVT_SCROLL && ev.id == C_SPEED) { c.speed = ev.x + 1; mq.c.speed = c.speed; show_values(win, &c); }
        if (ev.type == EVT_TIMER) {
            ctl_get_text(win, C_TEXT, c.text, sizeof c.text);
            if (strcmp(c.text, text)) { strlcpy(text, c.text, sizeof text); rebuild = 1; }
            uint64_t now = time_ms();
            int dt = (int)(now - last);
            last = now;
            if (dt > 200) dt = 200;
            mq_frame(&s, &mq, dt);
            win_invalidate(win);
        }
        if (rebuild) {
            mq.c = c;
            int keep = mq.x;
            mq_start(&s, &mq);
            mq.scale = 1;
            mq.x = keep;
        }
    }
    win_destroy(win);
}
