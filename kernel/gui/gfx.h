/* Halcyon OS - 2D graphics: surfaces, clipping, primitives, text, icons */
#ifndef GFX_H
#define GFX_H
#include "core/types.h"
#include "fonts.h"
#include "icons.h"

typedef u32 color_t;             /* 0xAARRGGBB (alpha only used where noted) */
#define RGB(r, g, b) ((color_t)(0xFF000000u | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b)))
#define HEX(h) ((color_t)(0xFF000000u | (u32)(h)))

typedef struct rect { int x, y, w, h; } rect_t;

static inline rect_t mkrect(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static inline int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline int rect_contains(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
rect_t rect_intersect(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
static inline rect_t rect_inset(rect_t r, int d) { return mkrect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }

typedef struct surface {
    u32 *px;
    int w, h, stride;          /* stride in pixels */
} surface_t;

surface_t *surface_new(int w, int h);
void surface_free(surface_t *s);

/* drawing context: target surface + origin offset + clip (in surface coords) */
typedef struct gc {
    surface_t *s;
    int ox, oy;
    rect_t clip;
} gc_t;

gc_t gc_for(surface_t *s);
/* sub-context: origin moved to r (relative to current origin), clip narrowed to r */
gc_t gc_sub(const gc_t *g, rect_t r);

color_t color_mix(color_t a, color_t b, int t);   /* t: 0..255 (0 = a) */
color_t color_lighten(color_t c, int amount);     /* amount -255..255 */

void gfx_pixel(gc_t *g, int x, int y, color_t c);
void gfx_fill(gc_t *g, rect_t r, color_t c);
void gfx_fill_alpha(gc_t *g, rect_t r, color_t c, int alpha);
void gfx_hline(gc_t *g, int x, int y, int w, color_t c);
void gfx_vline(gc_t *g, int x, int y, int h, color_t c);
void gfx_rect(gc_t *g, rect_t r, color_t c);
void gfx_line(gc_t *g, int x0, int y0, int x1, int y1, color_t c);
void gfx_gradient_v(gc_t *g, rect_t r, color_t top, color_t bottom);
void gfx_gradient_h(gc_t *g, rect_t r, color_t left, color_t right);
/* rounded rectangle filled with a vertical gradient; radius per corner set
 * by mask (1 = top-left, 2 = top-right, 4 = bottom-right, 8 = bottom-left) */
void gfx_rounded(gc_t *g, rect_t r, int radius, int corners, color_t top, color_t bottom);
void gfx_rounded_outline(gc_t *g, rect_t r, int radius, int corners, color_t c);
void gfx_circle(gc_t *g, int cx, int cy, int radius, color_t c);
/* classic 3D bevels */
void gfx_bevel(gc_t *g, rect_t r, int raised);
void gfx_sunken_field(gc_t *g, rect_t r, color_t bg);
void gfx_focus_rect(gc_t *g, rect_t r);
void gfx_shadow(gc_t *g, rect_t r, int size, int strength);

void gfx_blit(gc_t *g, int x, int y, const surface_t *src, rect_t srect);
void gfx_icon(gc_t *g, int x, int y, const icon_t *ic);
void gfx_icon_tinted(gc_t *g, int x, int y, const icon_t *ic, color_t tint, int amount);
void gfx_icon_alpha(gc_t *g, int x, int y, const icon_t *ic, int alpha);

/* text */
int  gfx_text(gc_t *g, const font_t *f, int x, int y, const char *s, color_t c);       /* returns end x */
int  gfx_text_n(gc_t *g, const font_t *f, int x, int y, const char *s, int n, color_t c);   /* text is UTF-8, n in bytes */
int  gfx_text_width(const font_t *f, const char *s);
int  gfx_text_width_n(const font_t *f, const char *s, int n);
int  gfx_char_width(const font_t *f, u32 codepoint);
int  gfx_text_fit(const font_t *f, const char *s, int width);   /* bytes (whole UTF-8 characters) that fit */
/* draw text in a box: align 0 left, 1 center, 2 right; vertically centered;
 * truncated with "..." when it doesn't fit */
void gfx_text_box(gc_t *g, const font_t *f, rect_t r, const char *s, color_t c, int align);
/* word-wrapped text; returns height used */
int  gfx_text_wrap(gc_t *g, const font_t *f, rect_t r, const char *s, color_t c);
int  gfx_text_wrap_height(const font_t *f, int width, const char *s);

/* ---- screen ---- */
extern surface_t *g_screen;      /* back buffer */
void screen_init(void);
void screen_present(rect_t r);   /* copy part of the back buffer to the framebuffer */
int  screen_w(void);
int  screen_h(void);

#endif
