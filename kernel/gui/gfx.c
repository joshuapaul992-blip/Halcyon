/* Halcyon OS - 2D graphics primitives */
#include "gfx.h"
#include "core/unicode.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

surface_t *g_screen;

rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return mkrect(0, 0, 0, 0);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}
rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}

surface_t *surface_new(int w, int h) {
    surface_t *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->w = w; s->h = h; s->stride = w;
    s->px = kmalloc_aligned((size_t)w * h * 4 + 64, 64);
    if (!s->px) { kfree(s); return NULL; }
    return s;
}
void surface_free(surface_t *s) {
    if (!s) return;
    kfree(s->px);
    kfree(s);
}

gc_t gc_for(surface_t *s) {
    gc_t g = { s, 0, 0, { 0, 0, s->w, s->h } };
    return g;
}
gc_t gc_sub(const gc_t *g, rect_t r) {
    gc_t n = *g;
    n.ox = g->ox + r.x;
    n.oy = g->oy + r.y;
    n.clip = rect_intersect(g->clip, mkrect(n.ox, n.oy, r.w, r.h));
    return n;
}

color_t color_mix(color_t a, color_t b, int t) {
    u32 ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    u32 br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    u32 r = (ar * (255 - t) + br * t) / 255;
    u32 gg = (ag * (255 - t) + bg * t) / 255;
    u32 bl = (ab * (255 - t) + bb * t) / 255;
    return 0xFF000000u | (r << 16) | (gg << 8) | bl;
}
color_t color_lighten(color_t c, int amt) {
    return amt >= 0 ? color_mix(c, 0xFFFFFFFF, amt) : color_mix(c, 0xFF000000, -amt);
}

static inline u32 blend(u32 dst, u32 src, u32 a) {
    /* a in 0..255; fast per-channel blend */
    u32 rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a)) >> 8;
    u32 g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a)) >> 8;
    return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}

static inline int clip_rect(gc_t *g, rect_t *r) {
    r->x += g->ox; r->y += g->oy;
    *r = rect_intersect(*r, g->clip);
    return !rect_empty(*r);
}

void gfx_pixel(gc_t *g, int x, int y, color_t c) {
    x += g->ox; y += g->oy;
    if (!rect_contains(g->clip, x, y)) return;
    g->s->px[y * g->s->stride + x] = c;
}

void gfx_fill(gc_t *g, rect_t r, color_t c) {
    if (!clip_rect(g, &r)) return;
    for (int y = 0; y < r.h; y++) {
        u32 *p = g->s->px + (r.y + y) * g->s->stride + r.x;
        int n = r.w;
        __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    }
}

void gfx_fill_alpha(gc_t *g, rect_t r, color_t c, int alpha) {
    if (alpha >= 255) { gfx_fill(g, r, c); return; }
    if (alpha <= 0 || !clip_rect(g, &r)) return;
    for (int y = 0; y < r.h; y++) {
        u32 *p = g->s->px + (r.y + y) * g->s->stride + r.x;
        for (int x = 0; x < r.w; x++) p[x] = blend(p[x], c, (u32)alpha);
    }
}

void gfx_hline(gc_t *g, int x, int y, int w, color_t c) { gfx_fill(g, mkrect(x, y, w, 1), c); }
void gfx_vline(gc_t *g, int x, int y, int h, color_t c) { gfx_fill(g, mkrect(x, y, 1, h), c); }
void gfx_rect(gc_t *g, rect_t r, color_t c) {
    gfx_hline(g, r.x, r.y, r.w, c);
    gfx_hline(g, r.x, r.y + r.h - 1, r.w, c);
    gfx_vline(g, r.x, r.y, r.h, c);
    gfx_vline(g, r.x + r.w - 1, r.y, r.h, c);
}

void gfx_line(gc_t *g, int x0, int y0, int x1, int y1, color_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_gradient_v(gc_t *g, rect_t r, color_t top, color_t bottom) {
    if (r.h <= 0) return;
    int oy = r.y, oh = r.h;
    rect_t c = r;
    if (!clip_rect(g, &c)) return;
    for (int y = 0; y < c.h; y++) {
        int t = oh > 1 ? (c.y - g->oy - oy + y) * 255 / (oh - 1) : 0;
        color_t col = color_mix(top, bottom, t);
        u32 *p = g->s->px + (c.y + y) * g->s->stride + c.x;
        int n = c.w;
        __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(col) : "memory");
    }
}

void gfx_gradient_h(gc_t *g, rect_t r, color_t left, color_t right) {
    if (r.w <= 0) return;
    int ox = r.x, ow = r.w;
    rect_t c = r;
    if (!clip_rect(g, &c)) return;
    for (int x = 0; x < c.w; x++) {
        int t = ow > 1 ? (c.x - g->ox - ox + x) * 255 / (ow - 1) : 0;
        color_t col = color_mix(left, right, t);
        for (int y = 0; y < c.h; y++) g->s->px[(c.y + y) * g->s->stride + c.x + x] = col;
    }
}

/* coverage (0..255) of a pixel at distance from a corner circle */
static int corner_cov(int dx2, int dy2, int r) {
    /* sample 4x4 sub-pixels */
    int cov = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int px = dx2 * 4 + sx * 2 + 1, py = dy2 * 4 + sy * 2 + 1;   /* in 1/8 px units: dx2 is half-pixel units */
            int rr = r * 8;
            if (px * px + py * py <= rr * rr) cov++;
        }
    return cov * 255 / 16;
}

void gfx_rounded(gc_t *g, rect_t r, int rad, int corners, color_t top, color_t bottom) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    for (int y = 0; y < r.h; y++) {
        int t = r.h > 1 ? y * 255 / (r.h - 1) : 0;
        color_t col = color_mix(top, bottom, t);
        int x0 = 0, x1 = r.w;
        int in_top = y < rad, in_bot = y >= r.h - rad;
        if ((in_top && (corners & 3)) || (in_bot && (corners & 12))) {
            int dy = in_top ? rad - y - 1 : y - (r.h - rad);
            for (int x = 0; x < rad; x++) {
                int dx = rad - x - 1;
                int cov = corner_cov(dx * 2, dy * 2, rad) ;
                int left = in_top ? (corners & 1) : (corners & 8);
                int right = in_top ? (corners & 2) : (corners & 4);
                if (left) {
                    gc_t *gg = g;
                    int px = r.x + x, py = r.y + y;
                    if (cov >= 255) gfx_pixel(gg, px, py, col);
                    else if (cov > 0) {
                        int ax = px + g->ox, ay = py + g->oy;
                        if (rect_contains(g->clip, ax, ay)) { u32 *p = &g->s->px[ay * g->s->stride + ax]; *p = blend(*p, col, (u32)cov); }
                    }
                }
                if (right) {
                    int px = r.x + r.w - 1 - x, py = r.y + y;
                    if (cov >= 255) gfx_pixel(g, px, py, col);
                    else if (cov > 0) {
                        int ax = px + g->ox, ay = py + g->oy;
                        if (rect_contains(g->clip, ax, ay)) { u32 *p = &g->s->px[ay * g->s->stride + ax]; *p = blend(*p, col, (u32)cov); }
                    }
                }
            }
            if (in_top ? (corners & 1) : (corners & 8)) x0 = rad;
            if (in_top ? (corners & 2) : (corners & 4)) x1 = r.w - rad;
        }
        gfx_fill(g, mkrect(r.x + x0, r.y + y, x1 - x0, 1), col);
    }
}

void gfx_rounded_outline(gc_t *g, rect_t r, int rad, int corners, color_t c) {
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    int tl = corners & 1 ? rad : 0, tr = corners & 2 ? rad : 0, br = corners & 4 ? rad : 0, bl = corners & 8 ? rad : 0;
    gfx_hline(g, r.x + tl, r.y, r.w - tl - tr, c);
    gfx_hline(g, r.x + bl, r.y + r.h - 1, r.w - bl - br, c);
    gfx_vline(g, r.x, r.y + tl, r.h - tl - bl, c);
    gfx_vline(g, r.x + r.w - 1, r.y + tr, r.h - tr - br, c);
    /* anti-aliased quarter circles */
    for (int q = 0; q < 4; q++) {
        int rr = (q == 0) ? tl : (q == 1) ? tr : (q == 2) ? br : bl;
        if (!rr) continue;
        int cx = (q == 0 || q == 3) ? r.x + rr : r.x + r.w - 1 - rr;
        int cy = (q == 0 || q == 1) ? r.y + rr : r.y + r.h - 1 - rr;
        for (int dy = 0; dy <= rr; dy++)
            for (int dx = 0; dx <= rr; dx++) {
                int d2 = dx * dx + dy * dy;
                /* distance to circle radius rr (approx) */
                int target = rr * rr;
                int diff = d2 - target;
                int band = 2 * rr;
                if (diff < -band || diff > band) continue;
                int a = 255 - (diff < 0 ? -diff : diff) * 255 / band;
                if (a <= 0) continue;
                int px = cx + ((q == 0 || q == 3) ? -dx : dx);
                int py = cy + ((q == 0 || q == 1) ? -dy : dy);
                int ax = px + g->ox, ay = py + g->oy;
                if (!rect_contains(g->clip, ax, ay)) continue;
                u32 *p = &g->s->px[ay * g->s->stride + ax];
                *p = blend(*p, c, (u32)a);
            }
    }
}

void gfx_circle(gc_t *g, int cx, int cy, int rad, color_t c) {
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++) {
            int d = dx * dx + dy * dy;
            if (d > rad * rad + rad) continue;
            int a = d <= rad * rad - rad ? 255 : 140;
            int ax = cx + dx + g->ox, ay = cy + dy + g->oy;
            if (!rect_contains(g->clip, ax, ay)) continue;
            u32 *p = &g->s->px[ay * g->s->stride + ax];
            *p = blend(*p, c, (u32)a);
        }
}

void gfx_bevel(gc_t *g, rect_t r, int raised) {
    color_t lt = raised ? HEX(0xFFFFFF) : HEX(0x808080);
    color_t dk = raised ? HEX(0x808080) : HEX(0xFFFFFF);
    gfx_hline(g, r.x, r.y, r.w, lt);
    gfx_vline(g, r.x, r.y, r.h, lt);
    gfx_hline(g, r.x, r.y + r.h - 1, r.w, dk);
    gfx_vline(g, r.x + r.w - 1, r.y, r.h, dk);
}

void gfx_sunken_field(gc_t *g, rect_t r, color_t bg) {
    gfx_fill(g, rect_inset(r, 1), bg);
    gfx_rect(g, r, HEX(0x7F9DB9));
}

void gfx_focus_rect(gc_t *g, rect_t r) {
    for (int x = 0; x < r.w; x += 2) { gfx_pixel(g, r.x + x, r.y, HEX(0x404040)); gfx_pixel(g, r.x + x, r.y + r.h - 1, HEX(0x404040)); }
    for (int y = 0; y < r.h; y += 2) { gfx_pixel(g, r.x, r.y + y, HEX(0x404040)); gfx_pixel(g, r.x + r.w - 1, r.y + y, HEX(0x404040)); }
}

void gfx_shadow(gc_t *g, rect_t r, int size, int strength) {
    for (int i = 0; i < size; i++) {
        int a = strength * (size - i) / size / 2;
        rect_t right = mkrect(r.x + r.w + i, r.y + size / 2 + i, 1, r.h - size / 2);
        rect_t bottom = mkrect(r.x + size / 2 + i, r.y + r.h + i, r.w - size / 2, 1);
        gfx_fill_alpha(g, right, 0xFF000000, a);
        gfx_fill_alpha(g, bottom, 0xFF000000, a);
    }
}

void gfx_blit(gc_t *g, int x, int y, const surface_t *src, rect_t sr) {
    rect_t d = mkrect(x, y, sr.w, sr.h);
    rect_t c = d;
    if (!clip_rect(g, &c)) return;
    int sx = sr.x + (c.x - (x + g->ox)), sy = sr.y + (c.y - (y + g->oy));
    for (int j = 0; j < c.h; j++) {
        if (sy + j < 0 || sy + j >= src->h) continue;
        memcpy(g->s->px + (c.y + j) * g->s->stride + c.x, src->px + (sy + j) * src->stride + sx, (size_t)c.w * 4);
    }
}

static void icon_draw(gc_t *g, int x, int y, const icon_t *ic, color_t tint, int tamt, int galpha) {
    if (!ic) return;
    rect_t c = mkrect(x, y, ic->w, ic->h);
    if (!clip_rect(g, &c)) return;
    int sx = c.x - (x + g->ox), sy = c.y - (y + g->oy);
    for (int j = 0; j < c.h; j++) {
        const u32 *sp = ic->px + (sy + j) * ic->w + sx;
        u32 *dp = g->s->px + (c.y + j) * g->s->stride + c.x;
        for (int i = 0; i < c.w; i++) {
            u32 s = sp[i];
            u32 a = s >> 24;
            if (galpha < 255) a = a * (u32)galpha / 255;
            if (!a) continue;
            if (tamt) s = color_mix(s, tint, tamt);
            dp[i] = a == 255 ? (s | 0xFF000000u) : blend(dp[i], s, a);
        }
    }
}
void gfx_icon(gc_t *g, int x, int y, const icon_t *ic) { icon_draw(g, x, y, ic, 0, 0, 255); }
void gfx_icon_tinted(gc_t *g, int x, int y, const icon_t *ic, color_t tint, int amount) { icon_draw(g, x, y, ic, tint, amount, 255); }
void gfx_icon_alpha(gc_t *g, int x, int y, const icon_t *ic, int alpha) { icon_draw(g, x, y, ic, 0, 0, alpha); }

/* ---- text ---- */
/* Text is UTF-8 (see core/unicode.h). Glyphs cover ranges of code points;
 * anything else is drawn as U+FFFD. Lengths (n) are in bytes. */
static const glyph_t *glyph_cp(const font_t *f, u32 cp) {
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < f->nranges; i++) {
            const font_range_t *r = &f->ranges[i];
            if (cp >= r->first && cp <= r->last) return &f->glyphs[r->index + cp - r->first];
        }
        cp = pass ? '?' : UC_REPLACEMENT;
    }
    return &f->glyphs[0];
}
int gfx_char_width(const font_t *f, u32 cp) { return glyph_cp(f, cp)->adv; }
int gfx_text_width_n(const font_t *f, const char *s, int n) {
    int w = 0, pos = 0;
    while (pos < n && s[pos]) w += glyph_cp(f, utf8_get(s, n, &pos))->adv;
    return w;
}
int gfx_text_width(const font_t *f, const char *s) { return gfx_text_width_n(f, s, 1 << 30); }

int gfx_text_n(gc_t *g, const font_t *f, int x, int y, const char *s, int n, color_t c) {
    int pos = 0;
    while (pos < n && s[pos]) {
        const glyph_t *gl = glyph_cp(f, utf8_get(s, n, &pos));
        if (gl->w && gl->h) {
            int gx = x + gl->xoff + g->ox, gy = y + gl->yoff + g->oy;
            rect_t r = rect_intersect(mkrect(gx, gy, gl->w, gl->h), g->clip);
            if (!rect_empty(r)) {
                const u8 *bm = f->bitmap + gl->off;
                for (int j = r.y; j < r.y + r.h; j++) {
                    const u8 *row = bm + (j - gy) * gl->w;
                    u32 *dp = g->s->px + j * g->s->stride;
                    for (int i = r.x; i < r.x + r.w; i++) {
                        u32 a = row[i - gx];
                        if (!a) continue;
                        dp[i] = a == 255 ? c : blend(dp[i], c, a);
                    }
                }
            }
        }
        x += gl->adv;
    }
    return x;
}
int gfx_text(gc_t *g, const font_t *f, int x, int y, const char *s, color_t c) { return gfx_text_n(g, f, x, y, s, 1 << 30, c); }

/* bytes of s that fit in width w (whole characters) */
int gfx_text_fit(const font_t *f, const char *s, int w) {
    int pos = 0, acc = 0;
    while (s[pos]) {
        int p2 = pos;
        int cw = glyph_cp(f, utf8_get(s, 1 << 30, &p2))->adv;
        if (acc + cw > w) break;
        acc += cw;
        pos = p2;
    }
    return pos;
}

void gfx_text_box(gc_t *g, const font_t *f, rect_t r, const char *s, color_t c, int align) {
    int w = gfx_text_width(f, s);
    int y = r.y + (r.h - f->height) / 2;
    gc_t sub = *g;
    sub.clip = rect_intersect(g->clip, mkrect(r.x + g->ox, r.y + g->oy, r.w, r.h));
    if (w <= r.w) {
        int x = align == 1 ? r.x + (r.w - w) / 2 : align == 2 ? r.x + r.w - w : r.x;
        gfx_text(&sub, f, x, y, s, c);
        return;
    }
    int ell = gfx_text_width(f, "...");
    int n = gfx_text_fit(f, s, r.w - ell);
    int x = gfx_text_n(&sub, f, r.x, y, s, n, c);
    gfx_text(&sub, f, x, y, "...", c);
}

static int wrap_impl(gc_t *g, const font_t *f, rect_t r, const char *s, color_t c, int draw) {
    int y = r.y, lh = f->height + 1;
    const char *p = s;
    while (*p) {
        /* find how many bytes fit on this line (whole characters) */
        int n = 0, w = 0, last_space = -1;
        while (p[n] && p[n] != '\n') {
            int n2 = n;
            int cw = glyph_cp(f, utf8_get(p, 1 << 30, &n2))->adv;
            if (w + cw > r.w && n > 0) break;
            if (p[n] == ' ') last_space = n;
            w += cw; n = n2;
        }
        int brk = n;
        if (p[n] && p[n] != '\n' && last_space > 0) brk = last_space;
        if (draw) gfx_text_n(g, f, r.x, y, p, brk, c);
        y += lh;
        p += brk;
        if (*p == ' ' || *p == '\n') p++;
    }
    return y - r.y;
}
int gfx_text_wrap(gc_t *g, const font_t *f, rect_t r, const char *s, color_t c) { return wrap_impl(g, f, r, s, c, 1); }
int gfx_text_wrap_height(const font_t *f, int width, const char *s) {
    return wrap_impl(NULL, f, mkrect(0, 0, width, 0), s, 0, 0);
}

/* ---- screen ---- */
int screen_w(void) { return (int)g_bootinfo.fb_width; }
int screen_h(void) { return (int)g_bootinfo.fb_height; }

static int fb_native;   /* 32bpp with R at 16, G at 8, B at 0 */

void screen_init(void) {
    if (g_screen) return;
    g_screen = surface_new((int)g_bootinfo.fb_width, (int)g_bootinfo.fb_height);
    if (!g_screen) panic("cannot allocate the screen back buffer");
    fb_native = g_bootinfo.fb_bpp == 32 && g_bootinfo.fb_red_pos == 16 && g_bootinfo.fb_green_pos == 8 && g_bootinfo.fb_blue_pos == 0;
}

/* the mode changed (g_bootinfo.fb_*): a new back buffer of the new size */
int screen_resize(void) {
    int w = (int)g_bootinfo.fb_width, h = (int)g_bootinfo.fb_height;
    if (g_screen && g_screen->w == w && g_screen->h == h) return 0;
    surface_t *n = surface_new(w, h);
    if (!n) return E_NOMEM;
    surface_free(g_screen);
    g_screen = n;
    fb_native = g_bootinfo.fb_bpp == 32 && g_bootinfo.fb_red_pos == 16 && g_bootinfo.fb_green_pos == 8 && g_bootinfo.fb_blue_pos == 0;
    return 0;
}

void screen_present(rect_t r) {
    r = rect_intersect(r, mkrect(0, 0, g_screen->w, g_screen->h));
    if (rect_empty(r)) return;
    u8 *fb = (u8 *)(uptr)g_bootinfo.fb_addr;
    u32 pitch = g_bootinfo.fb_pitch;
    for (int y = r.y; y < r.y + r.h; y++) {
        u32 *src = g_screen->px + y * g_screen->stride + r.x;
        if (fb_native) {
            memcpy(fb + (u64)y * pitch + (u64)r.x * 4, src, (size_t)r.w * 4);
        } else if (g_bootinfo.fb_bpp == 32) {
            u32 *dst = (u32 *)(fb + (u64)y * pitch) + r.x;
            for (int x = 0; x < r.w; x++) {
                u32 c = src[x];
                dst[x] = (((c >> 16) & 255) << g_bootinfo.fb_red_pos) | (((c >> 8) & 255) << g_bootinfo.fb_green_pos) |
                         ((c & 255) << g_bootinfo.fb_blue_pos);
            }
        } else {
            u8 *dst = fb + (u64)y * pitch + (u64)r.x * 3;
            for (int x = 0; x < r.w; x++) {
                u32 c = src[x];
                u32 v = (((c >> 16) & 255) << g_bootinfo.fb_red_pos) | (((c >> 8) & 255) << g_bootinfo.fb_green_pos) |
                        ((c & 255) << g_bootinfo.fb_blue_pos);
                dst[x * 3] = (u8)v; dst[x * 3 + 1] = (u8)(v >> 8); dst[x * 3 + 2] = (u8)(v >> 16);
            }
        }
    }
}
