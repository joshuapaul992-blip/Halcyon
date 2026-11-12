/* PAINT.EXE - Paint in user mode: a simple bitmap editor for Windows BMP
 * files. Looks and works like the kernel's Paint (kernel/apps/paint.c),
 * which stays as the fallback when programs cannot run.
 *
 * Tools: pencil, brush, eraser, line, rectangle, filled rectangle, ellipse,
 * flood fill, colour picker. Left button draws with the primary colour,
 * right button with the secondary colour. One level of undo (Ctrl+Z
 * toggles between the last two states).
 *
 * The picture lives in the program's memory. The window's pixels are mapped
 * into the program (win_canvas), so a brush stroke copies only the changed
 * part of the picture into the window: no system call per pixel. The tool
 * box and palette are drawn with the drawing calls; scroll bars and the
 * status bar are controls.
 *
 * BMP files are read with bounded reads (header, palette, then the pixel
 * rows in chunks): every size, offset and bit depth is checked before
 * anything is allocated or read. */
#include "hal.h"

#define TOOL_W 62
#define PAL_H 46
#define STATUS_H 22
#define SB 17
#define PAD 6
#define DEF_W 480
#define DEF_H 360
#define MAX_BYTES (16u * 1024 * 1024)       /* largest picture we accept (32 bpp) */
#define PATH_MAX 520                        /* as file_dialog */
#define CHUNK (256u * 1024)                 /* file I/O block for pixel rows */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define OPAQUE 0xFF000000u                  /* picture pixels carry it, as the kernel's surfaces */

enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_FRECT, T_ELLIPSE, T_FILL, T_PICK, NTOOLS };

static const char *tool_names[NTOOLS] = {
    "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Filled rectangle", "Ellipse", "Fill with colour", "Pick colour",
};
static const char *tool_hints[NTOOLS] = {
    "Draws a free-form line one pixel wide.",
    "Paints with a round brush of the selected size.",
    "Erases to the secondary colour.",
    "Draws a straight line. Hold Shift for 45 degree steps.",
    "Draws a rectangle outline. Hold Shift for a square.",
    "Draws a filled rectangle. Hold Shift for a square.",
    "Draws an ellipse. Hold Shift for a circle.",
    "Fills an area with the current colour.",
    "Picks up a colour from the picture.",
};

/* tool option sizes */
static const int brush_sizes[4] = { 2, 4, 7, 11 };
static const int eraser_sizes[4] = { 4, 8, 12, 18 };
static const int line_widths[4] = { 1, 2, 3, 5 };

static const unsigned default_palette[28] = {
    0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080,
    0x800080, 0x808040, 0x004040, 0x0080FF, 0x004080, 0x8000FF, 0x804000,
    0xFFFFFF, 0xC0C0C0, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF,
    0xFF00FF, 0xFFFF80, 0x00FF80, 0x80FFFF, 0x8080FF, 0xFF0080, 0xFF8040,
};

/* menu items and controls */
enum { PM_NEW = 1, PM_OPEN, PM_SAVE, PM_SAVEAS, PM_EXIT, PM_UNDO, PM_CLEAR, PM_SWAP, PM_ABOUT };
enum { C_VSCROLL = 100, C_HSCROLL, C_STATUS };

static const hal_menu_item_t menubar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&New", "Ctrl+N", PM_NEW, 0, 0, 0 },
    { "&Open...", "Ctrl+O", PM_OPEN, 0, HICON_FOLDER_OPEN, 0 },
    { "&Save", "Ctrl+S", PM_SAVE, 0, 0, 0 },
    { "Save &As...", 0, PM_SAVEAS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "E&xit", 0, PM_EXIT, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "&Undo", "Ctrl+Z", PM_UNDO, MENU_DISABLED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Swap colours", 0, PM_SWAP, 0, 0, 0 },
    { "&Clear image", 0, PM_CLEAR, 0, 0, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About Paint", 0, PM_ABOUT, 0, HICON_PAINT, 0 },
};

/* ---------------- small helpers ---------------- */
typedef struct { int x, y, w, h; } rect_t;
static rect_t mk(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static int has(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static inline void copy32(uint32_t *d, const uint32_t *s, size_t n) {
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline void fill32(uint32_t *d, uint32_t v, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

/* floating point helpers (no libm), as kernel/core/kmath.h */
#define K_PI 3.14159265358979323846
static inline double k_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline double k_floor(double x) {
    long long i = (long long)x;
    return (double)(x < (double)i ? i - 1 : i);
}
static double k_sin(double x) {
    x = x + K_PI;
    x = x - k_floor(x / (2 * K_PI)) * (2 * K_PI) - K_PI;
    if (x > K_PI / 2) x = K_PI - x;
    else if (x < -K_PI / 2) x = -K_PI - x;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static double k_cos(double x) { return k_sin(x + K_PI / 2); }

/* colours as the kernel's color_mix / color_lighten (0xRRGGBB) */
static unsigned mix(unsigned a, unsigned b, int t) {
    unsigned ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    unsigned br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (((ar * (255 - t) + br * t) / 255) << 16) | (((ag * (255 - t) + bg * t) / 255) << 8) | ((ab * (255 - t) + bb * t) / 255);
}
static unsigned lighten(unsigned c, int amt) { return amt >= 0 ? mix(c, 0xFFFFFF, amt) : mix(c, 0, -amt); }

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* ---------------- state ---------------- */
typedef struct image { uint32_t *px; int w, h; } image_t;     /* stride = w; pixels OPAQUE | 0xRRGGBB */

static int win = -1;
static hal_canvas_t cv;            /* the client area's pixels, mapped */
static int cw, ch;                 /* client size (the canvas) */
static hal_theme_t th;

static image_t img, undo;
static int has_undo;
static char path[PATH_MAX];
static int dirty;
static int tool = T_PENCIL, prev_tool = T_PENCIL;
static int opt[NTOOLS];            /* option index per tool */
static unsigned fg, bg;            /* OPAQUE | RGB */
static unsigned pal[28];
static int sx, sy;                 /* scroll position */
/* drawing in progress */
static int drawing, dbtn, dmods;
static unsigned dcol;
static int x0_, y0_, lx, ly;
static int pv_valid, pvx0, pvy0, pvx1, pvy1;   /* area touched by the shape preview */
/* pointer */
static int mx, my, over, hover = -1, cursor = -1;

/* what must be drawn again before the window is shown (flush) */
enum { N_TOOLS = 1, N_PAL = 2, N_VIEW = 4, N_ALL = 7 };
static int need;
static int dd, dx0, dy0, dx1, dy1;  /* changed picture area (inclusive), copied into the window */

static void mark(int x0, int y0, int x1, int y1) {
    if (!dd) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; dd = 1; return; }
    dx0 = MIN(dx0, x0); dy0 = MIN(dy0, y0); dx1 = MAX(dx1, x1); dy1 = MAX(dy1, y1);
}
static void mark_all(void) { mark(0, 0, img.w - 1, img.h - 1); }

/* ---------------- layout ---------------- */
typedef struct layout {
    rect_t tools, view, canvas, vbar, hbar, palette, status;
    int need_v, need_h;
    int ox, oy;               /* picture origin in client coordinates */
} layout_t;

static void get_layout(layout_t *L) {
    int body_h = MAX(0, ch - PAL_H - STATUS_H);
    L->tools = mk(0, 0, TOOL_W, body_h);
    L->palette = mk(0, ch - STATUS_H - PAL_H, cw, PAL_H);
    L->status = mk(0, ch - STATUS_H, cw, STATUS_H);
    L->view = mk(TOOL_W, 0, MAX(0, cw - TOOL_W), body_h);
    int tw = img.w + 2 * PAD, tht = img.h + 2 * PAD;
    int vw = L->view.w, vh = L->view.h;
    L->need_h = tw > vw;
    L->need_v = tht > vh - (L->need_h ? SB : 0);
    if (L->need_v && !L->need_h) L->need_h = tw > vw - SB;
    if (L->need_v) vw -= SB;
    if (L->need_h) vh -= SB;
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;
    L->canvas = mk(L->view.x, L->view.y, vw, vh);
    L->vbar = mk(L->view.x + vw, L->view.y, SB, vh);
    L->hbar = mk(L->view.x, L->view.y + vh, vw, SB);
    int maxx = MAX(0, tw - vw), maxy = MAX(0, tht - vh);
    sx = CLAMP(sx, 0, maxx);
    sy = CLAMP(sy, 0, maxy);
    L->ox = L->canvas.x + PAD - sx;
    L->oy = L->canvas.y + PAD - sy;
}

static rect_t tool_rect(layout_t *L, int i) {
    return mk(L->tools.x + 6 + (i % 2) * 26, L->tools.y + 8 + (i / 2) * 26, 25, 25);
}
static rect_t opt_box(layout_t *L) { return mk(L->tools.x + 6, L->tools.y + 8 + 5 * 26 + 6, 51, 74); }
static rect_t opt_rect(layout_t *L, int i) { rect_t b = opt_box(L); return mk(b.x + 3, b.y + 3 + i * 17, b.w - 6, 17); }
static rect_t pal_cell(layout_t *L, int i) { return mk(L->palette.x + 52 + (i % 14) * 18, L->palette.y + 6 + (i / 14) * 18, 16, 16); }
static rect_t swatch_rect(layout_t *L) { return mk(L->palette.x + 8, L->palette.y + 5, 36, 36); }

static int tool_has_options(int t) { return t == T_BRUSH || t == T_ERASER || t == T_LINE || t == T_RECT || t == T_ELLIPSE; }

/* ---------------- picture primitives ---------------- */
static inline void put(image_t *s, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) { s->px[(size_t)y * s->w + x] = c; mark(x, y, x, y); }
}
static void box(image_t *s, int x0, int y0, int x1, int y1, uint32_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    x0 = MAX(x0, 0); y0 = MAX(y0, 0); x1 = MIN(x1, s->w - 1); y1 = MIN(y1, s->h - 1);
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) fill32(s->px + (size_t)y * s->w + x0, c, (size_t)(x1 - x0 + 1));
    mark(x0, y0, x1, y1);
}
static void disc(image_t *s, int cx, int cy, int d, uint32_t c) {
    if (d <= 1) { put(s, cx, cy, c); return; }
    /* d x d pixel disc whose top-left pixel is at (cx - d/2, cy - d/2) */
    double r = d / 2.0, ox = cx - d / 2 + r, oy = cy - d / 2 + r;
    for (int j = 0; j < d; j++) {
        double dy = (cy - d / 2 + j + 0.5) - oy;
        double hw = r * r - dy * dy;
        if (hw <= 0) continue;
        hw = k_sqrt(hw);
        int xl = (int)k_floor(ox - hw + 0.5), xr = (int)k_floor(ox + hw - 0.5);
        if (xr < xl) xr = xl;
        box(s, xl, cy - d / 2 + j, xr, cy - d / 2 + j, c);
    }
}
static void stamp(image_t *s, int x, int y, int kind, int size, uint32_t c) {
    if (kind == 1) box(s, x - size / 2, y - size / 2, x - size / 2 + size - 1, y - size / 2 + size - 1, c);
    else disc(s, x, y, size, c);
}
static void stroke(image_t *s, int x0, int y0, int x1, int y1, int kind, int size, uint32_t c) {
    /* the pointer can be far outside the window while the mouse is captured:
     * keep the end points within reach of the picture (bounded work) */
    x0 = CLAMP(x0, -70000, 70000); y0 = CLAMP(y0, -70000, 70000);
    x1 = CLAMP(x1, -70000, 70000); y1 = CLAMP(y1, -70000, 70000);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx_ = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy_ = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 400000; guard++) {
        /* stamps entirely outside the picture change nothing */
        if (x0 + size >= 0 && y0 + size >= 0 && x0 - size < s->w && y0 - size < s->h) stamp(s, x0, y0, kind, size, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx_; }
        if (e2 <= dx) { err += dx; y0 += sy_; }
    }
}
static void rect_outline(image_t *s, int x0, int y0, int x1, int y1, int wd, uint32_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 + 1 <= 2 * wd || y1 - y0 + 1 <= 2 * wd) { box(s, x0, y0, x1, y1, c); return; }
    box(s, x0, y0, x1, y0 + wd - 1, c);
    box(s, x0, y1 - wd + 1, x1, y1, c);
    box(s, x0, y0, x0 + wd - 1, y1, c);
    box(s, x1 - wd + 1, y0, x1, y1, c);
}
static void ellipse(image_t *s, int x0, int y0, int x1, int y1, int wd, int filled, uint32_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    double a = (x1 - x0 + 1) / 2.0, b = (y1 - y0 + 1) / 2.0;
    double cx = x0 + a, cy = y0 + b;
    double ai = a - wd, bi = b - wd;
    /* only the rows that can touch the picture */
    int ya = MAX(y0, 0), yb = MIN(y1, s->h - 1);
    for (int y = ya; y <= yb; y++) {
        double dy = y + 0.5 - cy;
        double t = 1 - dy * dy / (b * b);
        double xo = t > 0 ? a * k_sqrt(t) : 0;
        int xl = (int)k_floor(cx - xo + 0.5), xr = (int)k_floor(cx + xo - 0.5);
        if (xl > xr) { xl = (int)k_floor(cx - 0.5); xr = (int)k_floor(cx); }
        if (filled || ai <= 0 || bi <= 0 || dy * dy >= bi * bi) { box(s, xl, y, xr, y, c); continue; }
        double xi = ai * k_sqrt(1 - dy * dy / (bi * bi));
        int il = (int)k_floor(cx - xi + 0.5), ir = (int)k_floor(cx + xi - 0.5);
        if (il <= xl) il = xl + 1;
        if (ir >= xr) ir = xr - 1;
        box(s, xl, y, il - 1, y, c);
        box(s, ir + 1, y, xr, y, c);
    }
}

static void flood(image_t *s, int x, int y, uint32_t c) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    uint32_t target = s->px[(size_t)y * s->w + x];
    if (target == c) return;
    size_t cap = 4096, sp = 0;
    int *st = malloc(sizeof(int) * 2 * cap);
    if (!st) return;
    st[sp++] = x; st[sp++] = y;
    while (sp) {
        int cy = st[--sp], cx = st[--sp];
        uint32_t *row = s->px + (size_t)cy * s->w;
        if (row[cx] != target) continue;
        int xl = cx, xr = cx;
        while (xl > 0 && row[xl - 1] == target) xl--;
        while (xr < s->w - 1 && row[xr + 1] == target) xr++;
        for (int i = xl; i <= xr; i++) row[i] = c;
        for (int d = -1; d <= 1; d += 2) {
            int ny = cy + d;
            if (ny < 0 || ny >= s->h) continue;
            uint32_t *nr = s->px + (size_t)ny * s->w;
            int in = 0;
            for (int i = xl; i <= xr; i++) {
                if (nr[i] == target) {
                    if (!in) {
                        if (sp + 2 > 2 * cap) {
                            int *ns = realloc(st, sizeof(int) * 4 * cap);
                            if (!ns) { free(st); mark_all(); return; }
                            st = ns; cap *= 2;
                        }
                        st[sp++] = i; st[sp++] = ny;
                        in = 1;
                    }
                } else in = 0;
            }
        }
    }
    free(st);
    mark_all();
}

/* ---------------- BMP files ---------------- */
static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

typedef struct chan { int shift, bits; } chan_t;
static chan_t mask_info(uint32_t m) {
    chan_t c = { 0, 0 };
    if (!m) return c;
    while (!(m & 1)) { m >>= 1; c.shift++; }
    while (m & 1) { m >>= 1; c.bits++; }
    return c;
}
static uint32_t chan_get(uint32_t v, chan_t c) {
    if (!c.bits) return 0;
    uint32_t x = (v >> c.shift) & ((c.bits >= 32) ? 0xFFFFFFFFu : ((1u << c.bits) - 1));
    if (c.bits >= 8) return x >> (c.bits - 8);
    /* scale up, e.g. 5 bits -> 8 bits */
    return (x * 255 + ((1u << c.bits) - 1) / 2) / ((1u << c.bits) - 1);
}

/* read exactly n bytes (short reads are retried); bytes read or an error */
static long read_full(int fd, void *buf, long n) {
    long got = 0;
    while (got < n) {
        long r = read(fd, (char *)buf + got, (size_t)(n - got));
        if (r < 0) return r;
        if (r == 0) break;
        got += r;
    }
    return got;
}

/* the header, info header (up to 4096 bytes), bit masks and a palette of
 * up to 256 entries all lie within the first HEAD_MAX bytes */
#define HEAD_MAX (14 + 4096 + 12 + 256 * 4)

static int bmp_load(const char *file, image_t *out, char *err, int errsz) {
    const char *bad = "This is not a valid bitmap file.";
    const char *trunc = "The bitmap file is truncated or damaged.";
    int fd = open(file, O_READ);
    if (fd < 0) { snprintf(err, (size_t)errsz, "Cannot open the file (%s).", hal_strerror(fd)); return -1; }
    long fsz = fsize(fd);
    uint8_t *d = malloc(HEAD_MAX);
    uint8_t *buf = NULL;
    uint32_t *px = NULL;
    if (fsz < 0) { snprintf(err, (size_t)errsz, "Error reading the file (%s).", hal_strerror((int)fsz)); goto fail; }
    if (!d) { strlcpy(err, "Not enough memory to open the file.", (size_t)errsz); goto fail; }
    uint64_t size = (uint64_t)fsz;
    long hn = fsz < HEAD_MAX ? fsz : HEAD_MAX;
    long r = read_full(fd, d, hn);
    if (r < 0) { snprintf(err, (size_t)errsz, "Error reading the file (%s).", hal_strerror((int)r)); goto fail; }
    if (r < hn) { strlcpy(err, trunc, (size_t)errsz); goto fail; }
    if (size < 26 || d[0] != 'B' || d[1] != 'M') { strlcpy(err, bad, (size_t)errsz); goto fail; }
    uint32_t off = rd32(d + 10), hdr = rd32(d + 14);
    int32_t w, h;
    uint32_t bpp, comp = 0, nclr = 0, entsz = 4;
    uint32_t masks[3] = { 0, 0, 0 };
    if (hdr == 12) {
        w = (int32_t)rd16(d + 18); h = (int16_t)rd16(d + 20); bpp = rd16(d + 24); entsz = 3;
    } else if (hdr >= 40 && hdr <= 4096 && size >= 14 + 40) {
        w = (int32_t)rd32(d + 18); h = (int32_t)rd32(d + 22); bpp = rd16(d + 28); comp = rd32(d + 30); nclr = rd32(d + 46);
        if (comp == 3 || comp == 6) {
            if (size < 54 + 12) { strlcpy(err, bad, (size_t)errsz); goto fail; }
            masks[0] = rd32(d + 54); masks[1] = rd32(d + 58); masks[2] = rd32(d + 62);
        }
    } else {
        strlcpy(err, bad, (size_t)errsz);
        goto fail;
    }
    int topdown = h < 0;
    int64_t hh = h < 0 ? -(int64_t)h : h;
    if (w <= 0 || hh <= 0 || w > 65535 || hh > 65535) { strlcpy(err, bad, (size_t)errsz); goto fail; }
    if ((uint64_t)w * (uint64_t)hh * 4 > MAX_BYTES) {
        snprintf(err, (size_t)errsz, "The picture is too large (%d x %d pixels). Paint can open pictures of up to 16 MB, "
                 "for example 2048 x 2048 pixels.", (int)w, (int)hh);
        goto fail;
    }
    int ok_fmt = 0;
    if ((bpp == 1 || bpp == 4 || bpp == 8) && comp == 0) ok_fmt = 1;
    if ((bpp == 16 || bpp == 32) && (comp == 0 || comp == 3 || comp == 6)) ok_fmt = 1;
    if (bpp == 24 && comp == 0) ok_fmt = 1;
    if (!ok_fmt) {
        snprintf(err, (size_t)errsz, "This bitmap format is not supported (%u bits per pixel%s).", (unsigned)bpp,
                 comp == 1 || comp == 2 ? ", RLE compressed" : comp == 4 || comp == 5 ? ", JPEG/PNG inside" : comp ? ", compressed" : "");
        goto fail;
    }
    if (comp == 0 && bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; }
    if (comp == 0 && bpp == 32) { masks[0] = 0xFF0000; masks[1] = 0xFF00; masks[2] = 0xFF; }
    uint64_t rowsz = (((uint64_t)w * bpp + 31) / 32) * 4;
    if ((uint64_t)off + rowsz * (uint64_t)hh > size || off < 14 + hdr) { strlcpy(err, trunc, (size_t)errsz); goto fail; }

    uint32_t pal_[256];
    memset(pal_, 0, sizeof pal_);
    if (bpp <= 8) {
        uint32_t maxc = 1u << bpp;
        if (!nclr || nclr > maxc) nclr = maxc;
        uint32_t pofs = 14 + hdr;
        for (uint32_t i = 0; i < nclr; i++) {
            uint64_t o = (uint64_t)pofs + (uint64_t)i * entsz;
            if (o + 3 > size || o + 3 > off || o + 3 > (uint64_t)hn) break;
            pal_[i] = OPAQUE | RGB(d[o + 2], d[o + 1], d[o]);
        }
        for (uint32_t i = 0; i < 256; i++) pal_[i] |= OPAQUE;
    }
    chan_t chn[3];
    for (int i = 0; i < 3; i++) chn[i] = mask_info(masks[i]);

    /* rows in chunks: rowsz <= 65535 * 32 / 8 bytes, so a chunk holds at least one */
    long rows_per = (long)MAX(1, CHUNK / rowsz);
    if (rows_per > hh) rows_per = (long)hh;
    px = malloc((size_t)w * (size_t)hh * 4);
    buf = malloc((size_t)(rowsz * (uint64_t)rows_per));
    if (!px || !buf) { strlcpy(err, "Not enough memory to open the picture.", (size_t)errsz); goto fail; }
    if (seek(fd, (long)off, 0) != (long)off) { strlcpy(err, trunc, (size_t)errsz); goto fail; }
    for (int64_t y = 0; y < hh; y += rows_per) {
        long n = (long)MIN((int64_t)rows_per, hh - y);
        long want = (long)rowsz * n;
        r = read_full(fd, buf, want);
        if (r < 0) { snprintf(err, (size_t)errsz, "Error reading the file (%s).", hal_strerror((int)r)); goto fail; }
        if (r < want) { strlcpy(err, trunc, (size_t)errsz); goto fail; }   /* the file changed meanwhile */
        for (long k = 0; k < n; k++) {
            const uint8_t *row = buf + (uint64_t)k * rowsz;
            int64_t yy = y + k;
            int64_t dy = topdown ? yy : hh - 1 - yy;
            uint32_t *dst = px + (size_t)dy * (size_t)w;
            for (int x = 0; x < w; x++) {
                uint32_t c;
                switch (bpp) {
                case 1: c = pal_[(row[x >> 3] >> (7 - (x & 7))) & 1]; break;
                case 4: c = pal_[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
                case 8: c = pal_[row[x]]; break;
                case 24: c = OPAQUE | RGB(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]); break;
                case 16: { uint32_t v = rd16(row + x * 2); c = OPAQUE | RGB(chan_get(v, chn[0]), chan_get(v, chn[1]), chan_get(v, chn[2])); break; }
                default: { uint32_t v = rd32(row + (size_t)x * 4); c = OPAQUE | RGB(chan_get(v, chn[0]), chan_get(v, chn[1]), chan_get(v, chn[2])); break; }
                }
                dst[x] = c;
            }
        }
    }
    close(fd);
    free(d);
    free(buf);
    out->px = px; out->w = w; out->h = (int)hh;
    return 0;
fail:
    close(fd);
    free(d);
    free(buf);
    free(px);
    return -1;
}

static int bmp_save(const char *file, const image_t *s, char *err, int errsz) {
    uint32_t rowsz = ((uint32_t)s->w * 3 + 3) & ~3u;
    uint64_t total = 54 + (uint64_t)rowsz * (uint64_t)s->h;
    if (total > 0xFFFFFFFFu) { strlcpy(err, "The picture is too large to save.", (size_t)errsz); return -1; }
    int rows_per = (int)MAX(1, CHUNK / rowsz);
    uint8_t *buf = malloc((size_t)rowsz * (size_t)rows_per);
    if (!buf) { strlcpy(err, "Not enough memory to save the picture.", (size_t)errsz); return -1; }
    uint8_t hd[54];
    memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M';
    wr32(hd + 2, (uint32_t)total);
    wr32(hd + 10, 54);
    wr32(hd + 14, 40);
    wr32(hd + 18, (uint32_t)s->w);
    wr32(hd + 22, (uint32_t)s->h);
    wr16(hd + 26, 1);
    wr16(hd + 28, 24);
    wr32(hd + 34, rowsz * (uint32_t)s->h);
    wr32(hd + 38, 2835);       /* 72 dpi */
    wr32(hd + 42, 2835);
    int fd = open(file, O_WRITE | O_CREATE | O_TRUNC);
    long r = fd;
    if (fd >= 0) {
        r = write(fd, hd, sizeof hd);
        if (r == (long)sizeof hd) r = 0; else if (r >= 0) r = E_NOSPC;
        /* bottom-up rows */
        for (int y = s->h - 1; y >= 0 && !r; y -= rows_per) {
            int n = MIN(rows_per, y + 1);
            memset(buf, 0, (size_t)rowsz * (size_t)n);
            for (int k = 0; k < n; k++) {
                uint8_t *row = buf + (size_t)rowsz * (size_t)k;
                const uint32_t *src = s->px + (size_t)(y - k) * s->w;
                for (int x = 0; x < s->w; x++) {
                    row[x * 3] = (uint8_t)src[x];
                    row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
                    row[x * 3 + 2] = (uint8_t)(src[x] >> 16);
                }
            }
            long want = (long)rowsz * n;
            r = write(fd, buf, (size_t)want);
            if (r == want) r = 0; else if (r >= 0) r = E_NOSPC;
        }
        int c = close(fd);
        if (!r && c < 0) r = c;
    }
    free(buf);
    if (r < 0) { snprintf(err, (size_t)errsz, "Cannot save the picture (%s).", hal_strerror((int)r)); return -1; }
    return 0;
}

/* ---------------- events that wait while a dialog of ours is open ---------------- */
#define DEFER_MAX 64
static hal_event_t deferred[DEFER_MAX];
static int ndeferred;

static void defer(const hal_event_t *ev) {
    if (ev->type == EVT_MOUSE_MOVE || ev->type == EVT_TIMER) return;
    if (ev->type == EVT_PAINT || ev->type == EVT_RESIZE)
        for (int i = 0; i < ndeferred; i++)
            if (deferred[i].type == ev->type && deferred[i].win == ev->win) { deferred[i] = *ev; return; }
    if (ndeferred < DEFER_MAX) deferred[ndeferred++] = *ev;
}
static int get_event(hal_event_t *ev, int timeout) {
    if (ndeferred) {
        *ev = deferred[0];
        memmove(deferred, deferred + 1, sizeof deferred[0] * (size_t)--ndeferred);
        return 1;
    }
    return win_get_event(ev, timeout);
}

/* ---------------- message boxes: modal over Paint's window, like the kernel's ---------------- */
/* wrapped text as the kernel's labels (gfx_text_wrap); returns its height */
static int wrap_text(int w, int x, int y, int width, const char *s, int draw) {
    int lh = text_height(FONT_UI) + 1, y0 = y;
    const char *p = s;
    while (*p) {
        int len = (int)strlen(p), n = 0, wsum = 0, last_space = -1;
        while (p[n] && p[n] != '\n') {
            int n2 = n;
            int adv = text_char_width(utf8_get(p, len, &n2), FONT_UI);
            if (wsum + adv > width && n > 0) break;
            if (p[n] == ' ') last_space = n;
            wsum += adv; n = n2;
        }
        int brk = n;
        if (p[n] && p[n] != '\n' && last_space > 0) brk = last_space;
        if (draw) gfx_text_n(w, x, y, p, MIN(brk, 2048), 0x000000, FONT_UI);
        y += lh;
        p += brk;
        if (*p == ' ' || *p == '\n') p++;
    }
    return y - y0;
}

static void flush(void);

/* icon: HICON_*; buttons: MBX_*; returns IDR_* */
static int pmsg(const char *title, const char *text, int icon, int buttons) {
    flush();
    int tw = text_width(text, FONT_UI);
    int textw = CLAMP(tw + 10, 200, 420);
    int texth = wrap_text(-1, 0, 0, textw, text, 0);
    int nb = buttons == MBX_OK ? 1 : buttons == MBX_YESNOCANCEL ? 3 : 2;
    int dw = MAX(textw + 76, nb * 82 + 30), dh = MAX(texth, 36) + 72;
    int d = win_create(title, dw, dh, WIN_DIALOG);
    if (d < 0) {
        static const int kinds[] = { MBX_ICON_ERROR, MBX_ICON_WARNING, MBX_ICON_QUESTION };
        int k = icon == HICON_ERROR ? kinds[0] : icon == HICON_WARNING ? kinds[1] : icon == HICON_QUESTION ? kinds[2] : MBX_ICON_INFO;
        return msgbox(win, title, text, k, buttons);
    }
    win_set_icon(d, icon);
    win_set_owner(d, win, 1);
    static const char *names[] = { "OK", "Cancel", "Yes", "No" };
    int ids[3], n = 0;
    const char *lbl[3];
    if (buttons == MBX_OK) { ids[n] = IDR_OK; lbl[n++] = names[0]; }
    if (buttons == MBX_OKCANCEL) { ids[n] = IDR_OK; lbl[n++] = names[0]; ids[n] = IDR_CANCEL; lbl[n++] = names[1]; }
    if (buttons == MBX_YESNO || buttons == MBX_YESNOCANCEL) { ids[n] = IDR_YES; lbl[n++] = names[2]; ids[n] = IDR_NO; lbl[n++] = names[3]; }
    if (buttons == MBX_YESNOCANCEL) { ids[n] = IDR_CANCEL; lbl[n++] = names[1]; }
    int bx = (dw - (n * 82 - 7)) / 2;
    for (int i = 0; i < n; i++) {
        int style = (i == 0 ? CTL_DEFAULT : 0) | (ids[i] == IDR_CANCEL || n == 1 ? CTL_CANCEL : 0);
        ctl_create(d, CTL_BUTTON, bx + i * 82, dh - 36, 75, 23, lbl[i], ids[i], style);
    }
    ctl_focus(d, ids[0]);
    int result = 0;
    while (!result) {
        hal_event_t ev;
        if ((int)SC2(SYS_WIN_GET_EVENT, &ev, -1) != 1) { result = IDR_CANCEL; break; }
        if (ev.win != d) { defer(&ev); continue; }
        if (ev.type == EVT_PAINT) {
            gfx_theme(&th);
            gfx_fill(d, 0, 0, dw, dh, th.face);
            gfx_icon(d, 16, 16, icon);
            wrap_text(d, 62, 16 + (texth < 32 ? (32 - texth) / 2 : 0), textw, text, 1);
            win_invalidate(d);
        } else if (ev.type == EVT_CLOSE) result = IDR_CANCEL;
        else if (ev.type == EVT_COMMAND) result = ev.id;
    }
    win_destroy(d);
    return result;
}

/* ---------------- document management ---------------- */
static void update_title(void) {
    char t[96];
    snprintf(t, sizeof t, "%s - Paint", path[0] ? basename_of(path) : "Untitled");
    win_set_title(win, t);
}

static void set_undo(int on) {
    if (on == has_undo) return;
    has_undo = on;
    menu_enable(win, PM_UNDO, on);
}

/* a new picture (the old one and its undo copy are freed) */
static void set_image(image_t s) {
    free(img.px);
    free(undo.px);
    img = s;
    undo.px = NULL; undo.w = undo.h = 0;
    set_undo(0);
    sx = sy = 0;
    drawing = 0;
    dirty = 0;
    need |= N_VIEW;
}

static int blank(image_t *s, int wd, int ht, uint32_t c) {
    s->px = malloc((size_t)wd * (size_t)ht * 4);
    if (!s->px) return -1;
    s->w = wd; s->h = ht;
    fill32(s->px, c, (size_t)wd * (size_t)ht);
    return 0;
}

static void start_dir(char *out, int outsz) {
    if (path[0]) {
        /* the folder of the current file ("C:/" for a file in the root) */
        strlcpy(out, path, (size_t)outsz);
        char *b = (char *)basename_of(out);
        if (b - out == 3 && out[1] == ':') *b = 0;
        else if (b > out) b[-1] = 0;
    } else {
        strlcpy(out, "C:/Documents", (size_t)outsz);
    }
}

static int save_as(void);
static int save(void) {
    if (!path[0]) return save_as();
    char err[160];
    flush();
    win_set_cursor(win, CUR_WAIT);
    int r = bmp_save(path, &img, err, sizeof err);
    win_set_cursor(win, cursor < 0 ? CUR_ARROW : cursor);
    if (r < 0) { pmsg("Paint", err, HICON_ERROR, MBX_OK); return 0; }
    dirty = 0;
    update_title();
    return 1;
}

static int save_as(void) {
    char dir[PATH_MAX], out[PATH_MAX];
    start_dir(dir, sizeof dir);
    flush();
    if (!file_dialog(win, 1, "Save As", dir, "bmp", out, sizeof out)) return 0;
    if (!strrchr(basename_of(out), '.') && strlen(out) + 4 < sizeof out) strlcat(out, ".bmp", sizeof out);
    hal_dirent_t st;
    if (stat(out, &st) == 0) {
        char q[PATH_MAX + 64];
        if (st.is_dir) { pmsg("Save As", "A folder with that name already exists.", HICON_ERROR, MBX_OK); return 0; }
        snprintf(q, sizeof q, "%s already exists.\nDo you want to replace it?", basename_of(out));
        if (pmsg("Save As", q, HICON_WARNING, MBX_YESNO) != IDR_YES) return 0;
    }
    char old[PATH_MAX];
    strlcpy(old, path, sizeof old);
    strlcpy(path, out, sizeof path);
    if (!save()) { strlcpy(path, old, sizeof path); update_title(); return 0; }
    return 1;
}

/* returns 0 if the user cancelled */
static int confirm_discard(void) {
    if (!dirty) return 1;
    char q[PATH_MAX + 48];
    snprintf(q, sizeof q, "Save changes to %s?", path[0] ? basename_of(path) : "Untitled");
    int r = pmsg("Paint", q, HICON_QUESTION, MBX_YESNOCANCEL);
    if (r == IDR_YES) return save();
    return r == IDR_NO;
}

static int load(const char *file) {
    char err[200];
    image_t s;
    flush();
    win_set_cursor(win, CUR_WAIT);
    int r = bmp_load(file, &s, err, sizeof err);
    win_set_cursor(win, cursor < 0 ? CUR_ARROW : cursor);
    if (r < 0) {
        char msg[PATH_MAX + 220];
        snprintf(msg, sizeof msg, "Paint cannot open %s.\n\n%s", basename_of(file), err);
        pmsg("Paint", msg, HICON_ERROR, MBX_OK);
        return 0;
    }
    set_image(s);
    strlcpy(path, file, sizeof path);
    update_title();
    return 1;
}

static void cmd_new(void) {
    if (!confirm_discard()) return;
    image_t s;
    if (blank(&s, DEF_W, DEF_H, OPAQUE | 0xFFFFFF) < 0) { pmsg("Paint", "Not enough memory for a new picture.", HICON_ERROR, MBX_OK); return; }
    set_image(s);
    path[0] = 0;
    update_title();
}

static void cmd_open(void) {
    if (!confirm_discard()) return;
    char dir[PATH_MAX], out[PATH_MAX];
    start_dir(dir, sizeof dir);
    flush();
    if (file_dialog(win, 0, "Open", dir, "bmp", out, sizeof out)) load(out);
}

/* ---------------- undo ---------------- */
static void snapshot(void) {
    if (!undo.px || undo.w != img.w || undo.h != img.h) {
        free(undo.px);
        undo.px = malloc((size_t)img.w * (size_t)img.h * 4);
        undo.w = img.w; undo.h = img.h;
    }
    if (!undo.px) { set_undo(0); return; }
    copy32(undo.px, img.px, (size_t)img.w * (size_t)img.h);
    set_undo(1);
}

static void cmd_undo(void) {
    if (!has_undo || drawing) return;
    image_t t = img;
    img = undo;
    undo = t;
    dirty = 1;
    mark_all();
}

static void restore_area(int x0, int y0, int x1, int y1) {
    if (!has_undo) return;
    x0 = MAX(x0, 0); y0 = MAX(y0, 0); x1 = MIN(x1, img.w - 1); y1 = MIN(y1, img.h - 1);
    if (x1 < x0 || y1 < y0) return;
    for (int y = y0; y <= y1; y++) copy32(img.px + (size_t)y * img.w + x0, undo.px + (size_t)y * undo.w + x0, (size_t)(x1 - x0 + 1));
    mark(x0, y0, x1, y1);
}

/* ---------------- drawing operations ---------------- */
static void constrain(int *x, int *y) {
    if (!(dmods & MOD_SHIFT)) return;
    int dx = *x - x0_, dy = *y - y0_;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (tool == T_LINE) {
        if (adx > 2 * ady) *y = y0_;
        else if (ady > 2 * adx) *x = x0_;
        else { int m = MAX(adx, ady); *x = x0_ + (dx < 0 ? -m : m); *y = y0_ + (dy < 0 ? -m : m); }
    } else {
        int m = MAX(adx, ady);
        *x = x0_ + (dx < 0 ? -m : m);
        *y = y0_ + (dy < 0 ? -m : m);
    }
}

static void draw_shape(int x, int y) {
    int wd = line_widths[opt[tool] & 3];
    /* far outside the picture (captured pointer): the shape is cut there anyway */
    x = CLAMP(x, -70000, 70000); y = CLAMP(y, -70000, 70000);
    constrain(&x, &y);
    if (pv_valid) restore_area(pvx0, pvy0, pvx1, pvy1);
    switch (tool) {
    case T_LINE: stroke(&img, x0_, y0_, x, y, 0, wd, dcol); break;
    case T_RECT: rect_outline(&img, x0_, y0_, x, y, wd, dcol); break;
    case T_FRECT: box(&img, x0_, y0_, x, y, dcol); break;
    case T_ELLIPSE: ellipse(&img, x0_, y0_, x, y, wd, 0, dcol); break;
    }
    pv_valid = 1;
    pvx0 = MIN(x0_, x) - wd - 1; pvy0 = MIN(y0_, y) - wd - 1;
    pvx1 = MAX(x0_, x) + wd + 1; pvy1 = MAX(y0_, y) + wd + 1;
}

static void begin_draw(int x, int y, int btn) {
    uint32_t col = btn == MOUSE_RIGHT ? bg : fg;
    if (tool == T_PICK) {
        if ((unsigned)x < (unsigned)img.w && (unsigned)y < (unsigned)img.h) {
            uint32_t c = img.px[(size_t)y * img.w + x] | OPAQUE;
            if (btn == MOUSE_RIGHT) bg = c; else fg = c;
            tool = prev_tool;
        }
        need |= N_TOOLS | N_PAL;
        return;
    }
    snapshot();
    if (tool == T_FILL) {
        flood(&img, x, y, col);
        dirty = 1;
        return;
    }
    drawing = 1;
    dbtn = btn;
    dcol = tool == T_ERASER ? bg : col;
    x0_ = lx = x; y0_ = ly = y;
    pv_valid = 0;
    switch (tool) {
    case T_PENCIL: put(&img, x, y, dcol); break;
    case T_BRUSH: stamp(&img, x, y, 0, brush_sizes[opt[T_BRUSH] & 3], dcol); break;
    case T_ERASER: stamp(&img, x, y, 1, eraser_sizes[opt[T_ERASER] & 3], dcol); break;
    default: if (has_undo) draw_shape(x, y); break;
    }
    dirty = 1;
}

static void continue_draw(int x, int y) {
    if (!drawing) return;
    switch (tool) {
    case T_PENCIL: stroke(&img, lx, ly, x, y, 0, 1, dcol); break;
    case T_BRUSH: stroke(&img, lx, ly, x, y, 0, brush_sizes[opt[T_BRUSH] & 3], dcol); break;
    case T_ERASER: stroke(&img, lx, ly, x, y, 1, eraser_sizes[opt[T_ERASER] & 3], dcol); break;
    default: if (has_undo) draw_shape(x, y); break;
    }
    lx = x; ly = y;
}

static void end_draw(int x, int y) {
    if (!drawing) return;
    if (tool >= T_LINE && tool <= T_ELLIPSE) {
        if (!has_undo) pv_valid = 0;
        draw_shape(x, y);
    } else {
        continue_draw(x, y);
    }
    drawing = 0;
    pv_valid = 0;
    dirty = 1;
}

static void cancel_draw(void) {
    if (!drawing) return;
    if (has_undo) restore_area(0, 0, img.w - 1, img.h - 1);
    drawing = 0;
    pv_valid = 0;
}

/* ---------------- painting the window ---------------- */
static void fillr(rect_t r, unsigned c) { gfx_fill(win, r.x, r.y, r.w, r.h, c); }
static void pixel(int x, int y, unsigned c) { gfx_fill(win, x, y, 1, 1, c); }
static void hline(int x, int y, int w, unsigned c) { gfx_fill(win, x, y, w, 1, c); }
static void vline(int x, int y, int h, unsigned c) { gfx_fill(win, x, y, 1, h, c); }
static void rect(rect_t r, unsigned c) { gfx_rect(win, r.x, r.y, r.w, r.h, c); }
static void disc_aa(int cx, int cy, int r, unsigned c) { gfx_circle(win, cx, cy, r, c, 0); }   /* the kernel's gfx_circle */
static void rounded(rect_t r, unsigned top, unsigned bottom) { gfx_rounded(win, r.x, r.y, r.w, r.h, 3, top, bottom); }
static void rounded_outline(rect_t r, unsigned c) { gfx_rounded_outline(win, r.x, r.y, r.w, r.h, 3, c); }
static void sunken(rect_t r, unsigned c) { gfx_shade(win, r.x, r.y, r.w, r.h, c, 0, SHADE_SUNKEN, 0); }
static rect_t inset(rect_t r, int d) { return mk(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }

static void draw_tool_glyph(rect_t r, int t) {
    int x = r.x + (r.w - 16) / 2, y = r.y + (r.h - 16) / 2;
    unsigned ink = 0x1C2230;
    switch (t) {
    case T_PENCIL:
        for (int i = 0; i < 10; i++)
            gfx_fill(win, x + 4 + i, y + 10 - i, 3, 2, i < 2 ? 0xF2C89A : i > 8 ? 0xE88AA0 : 0xF5B547);
        gfx_line(win, x + 3, y + 13, x + 13, y + 3, 0x9A6A10);
        gfx_line(win, x + 5, y + 14, x + 15, y + 4, 0x9A6A10);
        pixel(x + 3, y + 14, ink); pixel(x + 2, y + 15, ink);
        break;
    case T_BRUSH:
        gfx_line(win, x + 8, y + 8, x + 14, y + 2, 0x8A5A2A);
        gfx_line(win, x + 9, y + 8, x + 15, y + 2, 0xA8743A);
        gfx_line(win, x + 9, y + 9, x + 15, y + 3, 0x8A5A2A);
        gfx_fill(win, x + 6, y + 8, 4, 3, 0xB0B6C0);
        disc_aa(x + 5, y + 12, 3, 0x2E6FDB);
        pixel(x + 1, y + 15, 0x2E6FDB); pixel(x + 2, y + 15, 0x2E6FDB);
        break;
    case T_ERASER:
        for (int i = 0; i < 7; i++) hline(x + 6 - i / 2 + i, y + 4 + i, 8, i < 3 ? 0xF4F4F4 : 0xF08AA0);
        gfx_line(win, x + 6, y + 4, x + 13, y + 4, ink);
        gfx_line(win, x + 13, y + 4, x + 17, y + 10, ink);
        gfx_line(win, x + 2, y + 10, x + 6, y + 4, ink);
        gfx_line(win, x + 2, y + 10, x + 9, y + 10, ink);
        gfx_line(win, x + 2, y + 10, x + 4, y + 13, ink);
        gfx_line(win, x + 4, y + 13, x + 11, y + 13, ink);
        gfx_line(win, x + 11, y + 13, x + 17, y + 10, ink);
        break;
    case T_LINE:
        gfx_line(win, x + 2, y + 13, x + 13, y + 2, ink);
        gfx_line(win, x + 3, y + 13, x + 14, y + 2, ink);
        break;
    case T_RECT:
        gfx_rect(win, x + 1, y + 3, 14, 10, ink);
        gfx_rect(win, x + 2, y + 4, 12, 8, ink);
        break;
    case T_FRECT:
        gfx_fill(win, x + 1, y + 3, 14, 10, 0x6FA2F5);
        gfx_rect(win, x + 1, y + 3, 14, 10, ink);
        break;
    case T_ELLIPSE:
        for (int a = 0; a < 64; a++) {
            double ang = a * 2 * K_PI / 64;
            int px = x + 8 + (int)(k_cos(ang) * 7.0), py = y + 8 + (int)(k_sin(ang) * 5.0);
            gfx_fill(win, px, py, 2, 1, ink);
        }
        break;
    case T_FILL:
        /* tilted bucket pouring a drop */
        for (int i = 0; i < 8; i++) hline(x + 4 + i / 2, y + 4 + i, 9 - i / 2, 0xC8CCD4);
        gfx_line(win, x + 4, y + 4, x + 8, y + 12, ink);
        gfx_line(win, x + 8, y + 12, x + 13, y + 11, ink);
        gfx_line(win, x + 13, y + 11, x + 13, y + 3, ink);
        gfx_line(win, x + 4, y + 4, x + 13, y + 3, ink);
        gfx_line(win, x + 7, y + 2, x + 12, y + 1, ink);
        gfx_fill(win, x + 2, y + 5, 2, 6, 0x2E6FDB);
        gfx_fill(win, x + 1, y + 10, 4, 3, 0x2E6FDB);
        break;
    case T_PICK:
        gfx_line(win, x + 3, y + 13, x + 9, y + 7, ink);
        gfx_line(win, x + 4, y + 13, x + 10, y + 7, ink);
        gfx_line(win, x + 2, y + 14, x + 3, y + 13, ink);
        gfx_fill(win, x + 9, y + 4, 4, 4, 0x5A6470);
        disc_aa(x + 12, y + 4, 3, 0x2A3240);
        gfx_line(win, x + 8, y + 5, x + 11, y + 8, ink);
        break;
    }
}

static void draw_options(layout_t *L) {
    rect_t b = opt_box(L);
    sunken(b, lighten(th.face, 30));
    int t = tool;
    if (!tool_has_options(t)) return;
    for (int i = 0; i < 4; i++) {
        rect_t r = opt_rect(L, i);
        int sel = (opt[t] & 3) == i;
        if (sel) fillr(r, th.select);
        unsigned c = sel ? th.select_text : 0x1C2230;
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (t == T_BRUSH) { int d = brush_sizes[i]; disc_aa(cx, cy, d / 2, c); if (d <= 2) gfx_fill(win, cx - 1, cy - 1, 2, 2, c); }
        else if (t == T_ERASER) { int d = MIN(eraser_sizes[i], 14); gfx_fill(win, cx - d / 2, cy - d / 2 + (d > 13 ? 1 : 0), d, MIN(d, 14), c); }
        else gfx_fill(win, r.x + 6, cy - line_widths[i] / 2, r.w - 12, line_widths[i], c);
    }
}

static void draw_tools(layout_t *L) {
    gfx_shade(win, L->tools.x, L->tools.y, L->tools.w, L->tools.h, lighten(th.face, 40), th.face, SHADE_GRADIENT_H, 0);
    vline(L->tools.x + L->tools.w - 1, L->tools.y, L->tools.h, lighten(th.face, -30));
    for (int i = 0; i < NTOOLS; i++) {
        rect_t r = tool_rect(L, i);
        if (tool == i) {
            rounded(r, lighten(th.select, 120), lighten(th.select, 80));
            rounded_outline(r, th.select);
        } else if (hover == i) {
            rounded(r, 0xFFFFFF, lighten(th.hover, 70));
            rounded_outline(r, th.hover);
        } else {
            rounded(r, 0xFFFFFF, 0xE6E4DA);
            rounded_outline(r, lighten(th.button_border, 50));
        }
        draw_tool_glyph(r, i);
    }
    draw_options(L);
}

static void draw_palette(layout_t *L) {
    rect_t pr = L->palette;
    gfx_gradient(win, pr.x, pr.y, pr.w, pr.h, lighten(th.face, 30), th.face);
    hline(pr.x, pr.y, pr.w, lighten(th.face, -30));
    hline(pr.x, pr.y + 1, pr.w, 0xFFFFFF);
    rect_t sw = swatch_rect(L);
    sunken(sw, lighten(th.face, 40));
    rect_t bgr = mk(sw.x + 14, sw.y + 14, 18, 18), fgr = mk(sw.x + 4, sw.y + 4, 18, 18);
    fillr(bgr, bg); rect(bgr, 0x404040); rect(inset(bgr, 1), 0xFFFFFF);
    fillr(fgr, fg); rect(fgr, 0x404040); rect(inset(fgr, 1), 0xFFFFFF);
    for (int i = 0; i < 28; i++) {
        rect_t c = pal_cell(L, i);
        fillr(c, pal[i]);
        rect(c, 0x6A6A6A);
        hline(c.x + 1, c.y + 1, c.w - 2, lighten(pal[i], 50));
    }
}

/* copy part of the picture (picture coordinates, inclusive) into the window */
static void present(layout_t *L, int x0, int y0, int x1, int y1) {
    if (!cv.pixels) return;
    /* the window area: the picture's place, within the visible canvas and the mapping */
    int cx0 = MAX(L->ox + MAX(x0, 0), MAX(L->canvas.x, 0));
    int cy0 = MAX(L->oy + MAX(y0, 0), MAX(L->canvas.y, 0));
    int cx1 = MIN(L->ox + MIN(x1, img.w - 1), MIN(L->canvas.x + L->canvas.w, cv.w) - 1);
    int cy1 = MIN(L->oy + MIN(y1, img.h - 1), MIN(L->canvas.y + L->canvas.h, cv.h) - 1);
    if (cx1 < cx0 || cy1 < cy0) return;
    for (int y = cy0; y <= cy1; y++)
        copy32(cv.pixels + (size_t)y * cv.stride + cx0, img.px + (size_t)(y - L->oy) * img.w + (cx0 - L->ox), (size_t)(cx1 - cx0 + 1));
}

static int last_hidden_v = -1, last_hidden_h = -1;
static void draw_view(layout_t *L) {
    fillr(L->view, 0x8E98AC);
    /* the picture's shadow, clipped to the canvas; the rectangle is cut a
     * little outside the canvas (16-bit geometry, and nothing else shows) */
    rect_t c = L->canvas;
    int x0 = MAX(L->ox, c.x - 16), y0 = MAX(L->oy, c.y - 16);
    int x1 = MIN(L->ox + img.w, c.x + c.w + 16), y1 = MIN(L->oy + img.h, c.y + c.h + 16);
    if (x1 > x0 && y1 > y0 && c.w > 0 && c.h > 0) {
        gfx_clip(win, c.x, c.y, c.w, c.h);
        gfx_shade(win, x0, y0, x1 - x0, y1 - y0, 0, 0, SHADE_SHADOW, 5 | 90 << 8);
        gfx_clip_reset(win);
    }
    present(L, 0, 0, img.w - 1, img.h - 1);
    /* scroll bars */
    if (L->need_v) {
        ctl_move(win, C_VSCROLL, L->vbar.x, L->vbar.y, L->vbar.w, L->vbar.h);
        ctl_scroll_set(win, C_VSCROLL, img.h + 2 * PAD, L->canvas.h, sy, 24);
    }
    if (L->need_h) {
        ctl_move(win, C_HSCROLL, L->hbar.x, L->hbar.y, L->hbar.w, L->hbar.h);
        ctl_scroll_set(win, C_HSCROLL, img.w + 2 * PAD, L->canvas.w, sx, 24);
    }
    if (last_hidden_v != !L->need_v) { last_hidden_v = !L->need_v; ctl_set_style(win, C_VSCROLL, L->need_v ? 0 : CTL_HIDDEN); }
    if (last_hidden_h != !L->need_h) { last_hidden_h = !L->need_h; ctl_set_style(win, C_HSCROLL, L->need_h ? 0 : CTL_HIDDEN); }
    if (L->need_v && L->need_h) gfx_fill(win, L->vbar.x, L->hbar.y, SB, SB, th.face);
}

static char last_status[256];
static void update_status(void) {
    char s[256], pos[48];
    if (over) snprintf(pos, sizeof pos, "%d, %d px", mx, my);
    else pos[0] = 0;
    snprintf(s, sizeof s, "%s: %s\t%s\t%d x %d px", tool_names[tool], tool_hints[tool], pos, img.w, img.h);
    if (strcmp(s, last_status)) {
        strlcpy(last_status, s, sizeof last_status);
        ctl_set_text(win, C_STATUS, s);
    }
}

/* draw what changed since the last time, then show it */
static void flush(void) {
    if (win < 0) return;
    layout_t L;
    get_layout(&L);
    int shown = 0;
    if (need) gfx_theme(&th);
    if (need & N_TOOLS) draw_tools(&L);
    if (need & N_PAL) draw_palette(&L);
    if (need & N_VIEW) draw_view(&L);
    else if (dd) present(&L, dx0, dy0, dx1, dy1);
    shown = need || dd;
    need = 0;
    dd = 0;
    update_status();
    if (shown) win_invalidate(win);
}

/* ---------------- events ---------------- */
static void edit_palette(int i, int secondary) {
    char buf[16];
    snprintf(buf, sizeof buf, "%06x", pal[i] & 0xFFFFFF);
    flush();
    if (!inputbox(win, "Edit Colours", "Enter the colour as a hex RRGGBB value:", buf, sizeof buf)) return;
    const char *s = buf;
    while (*s == ' ' || *s == '#') s++;
    char *end;
    long v = strtol(s, &end, 16);
    if (end == s || v < 0 || v > 0xFFFFFF) { pmsg("Edit Colours", "Please enter six hex digits, e.g. FF8000.", HICON_WARNING, MBX_OK); return; }
    pal[i] = OPAQUE | (unsigned)v;
    if (secondary) bg = pal[i]; else fg = pal[i];
    need |= N_PAL;
}

static void select_tool(int t) {
    if (t == T_PICK && tool != T_PICK) prev_tool = tool;
    tool = t;
    need |= N_TOOLS;
}

static void set_cursor(int c) {
    if (c == cursor) return;
    cursor = c;
    win_set_cursor(win, c);
}

static void swap_colours(void) { unsigned t = fg; fg = bg; bg = t; need |= N_PAL; }

static void menu_command(int id) {
    if (drawing) return;
    switch (id) {
    case PM_NEW: cmd_new(); break;
    case PM_OPEN: cmd_open(); break;
    case PM_SAVE: save(); break;
    case PM_SAVEAS: save_as(); break;
    case PM_EXIT: if (confirm_discard()) { win_destroy(win); exit(0); } break;
    case PM_UNDO: cmd_undo(); break;
    case PM_SWAP: swap_colours(); break;
    case PM_CLEAR:
        snapshot();
        box(&img, 0, 0, img.w - 1, img.h - 1, bg);
        dirty = 1;
        break;
    case PM_ABOUT: {
        hal_sysinfo_t si;
        char text[400];
        if (sysinfo(&si) < 0 || !si.version[0]) strcpy(si.version, "1.0");
        snprintf(text, sizeof text, "Halcyon Paint %s\n\nOpens 1, 4, 8, 16, 24 and 32-bit Windows bitmaps "
                 "and saves 24-bit bitmaps.\n\nLeft button: primary colour. Right button: secondary colour. "
                 "Double click a palette colour to change it.", si.version);
        pmsg("About Paint", text, HICON_PAINT, MBX_OK);
        break;
    }
    }
}

static void mouse_down(const hal_event_t *e, layout_t *L) {
    int cx = e->x - L->ox, cy = e->y - L->oy;
    if (drawing) {
        /* the other button cancels a drag */
        if (e->button != dbtn) cancel_draw();
        return;
    }
    if (e->button != MOUSE_LEFT && e->button != MOUSE_RIGHT) return;
    for (int i = 0; i < NTOOLS; i++)
        if (has(tool_rect(L, i), e->x, e->y)) { select_tool(i); return; }
    if (tool_has_options(tool))
        for (int i = 0; i < 4; i++)
            if (has(opt_rect(L, i), e->x, e->y)) { opt[tool] = i; need |= N_TOOLS; return; }
    for (int i = 0; i < 28; i++)
        if (has(pal_cell(L, i), e->x, e->y)) {
            if (e->type == EVT_DBLCLICK) { edit_palette(i, e->button == MOUSE_RIGHT); return; }
            if (e->button == MOUSE_RIGHT) bg = pal[i]; else fg = pal[i];
            need |= N_PAL;
            return;
        }
    if (has(swatch_rect(L), e->x, e->y) && e->type == EVT_DBLCLICK) { swap_colours(); return; }   /* double click swaps */
    if (has(L->canvas, e->x, e->y)) {
        dmods = e->mods;
        begin_draw(cx, cy, e->button);
    }
}

static void mouse_move(const hal_event_t *e, layout_t *L) {
    int cx = e->x - L->ox, cy = e->y - L->oy;
    int on_canvas = has(L->canvas, e->x, e->y);
    set_cursor(on_canvas ? CUR_CROSS : CUR_ARROW);
    if (drawing) {
        dmods = e->mods;
        continue_draw(cx, cy);
        over = 1; mx = cx; my = cy;
        return;
    }
    int inimg = on_canvas && cx >= 0 && cy >= 0 && cx < img.w && cy < img.h;
    over = inimg;
    mx = cx; my = cy;
    int h = -1;
    for (int i = 0; i < NTOOLS; i++) if (has(tool_rect(L, i), e->x, e->y)) h = i;
    if (h != hover) { hover = h; need |= N_TOOLS; }
}

/* ---- the colours, palette, tool and tool options survive a restart:
 * paint.fg / paint.bg / paint.palette (RRGGBB, the 28 cells), paint.tool,
 * paint.options (one digit per tool), kept in SETTINGS.INI ---- */
static char prefs_last[400];
static void prefs_text(char *out, int n) {
    int o = snprintf(out, (size_t)n, "%06x|%06x|%d|", fg & 0xFFFFFF, bg & 0xFFFFFF, tool == T_PICK ? prev_tool : tool);
    for (int i = 0; i < NTOOLS && o < n - 1; i++) out[o++] = (char)('0' + (opt[i] & 3));
    for (int i = 0; i < 28 && o < n - 8; i++) o += snprintf(out + o, (size_t)(n - o), "%c%06x", i ? ',' : '|', pal[i] & 0xFFFFFF);
    out[o < n ? o : n - 1] = 0;
}
static unsigned hex_at(const char *s) { return (unsigned)strtol(s, 0, 16) & 0xFFFFFF; }
static void prefs_load(void) {
    char b[300];
    if (setting_get("paint.fg", b, sizeof b) == 6) fg = OPAQUE | hex_at(b);
    if (setting_get("paint.bg", b, sizeof b) == 6) bg = OPAQUE | hex_at(b);
    int t = setting_get_int("paint.tool", T_PENCIL);
    if (t >= 0 && t < NTOOLS && t != T_PICK) tool = prev_tool = t;
    if (setting_get("paint.options", b, sizeof b) == NTOOLS)
        for (int i = 0; i < NTOOLS; i++) if (b[i] >= '0' && b[i] <= '3') opt[i] = b[i] - '0';
    if (setting_get("paint.palette", b, sizeof b) == 28 * 7 - 1)
        for (int i = 0; i < 28; i++) pal[i] = OPAQUE | hex_at(b + i * 7);
    prefs_text(prefs_last, sizeof prefs_last);
}
/* after each batch of events: write what changed (the kernel coalesces) */
static void prefs_sync(void) {
    char now[400], b[300];
    prefs_text(now, sizeof now);
    if (!strcmp(now, prefs_last)) return;
    strlcpy(prefs_last, now, sizeof prefs_last);
    snprintf(b, sizeof b, "%06x", fg & 0xFFFFFF); setting_set("paint.fg", b);
    snprintf(b, sizeof b, "%06x", bg & 0xFFFFFF); setting_set("paint.bg", b);
    setting_set_int("paint.tool", tool == T_PICK ? prev_tool : tool);
    for (int i = 0; i < NTOOLS; i++) b[i] = (char)('0' + (opt[i] & 3));
    b[NTOOLS] = 0;
    setting_set("paint.options", b);
    int o = 0, custom = 0;
    for (int i = 0; i < 28; i++) {
        o += snprintf(b + o, sizeof b - (size_t)o, "%s%06x", i ? "," : "", pal[i] & 0xFFFFFF);
        if ((pal[i] & 0xFFFFFF) != default_palette[i]) custom = 1;
    }
    setting_set("paint.palette", custom ? b : NULL);
}

static void handle(const hal_event_t *e) {
    if (e->win != win) return;
    layout_t L;
    get_layout(&L);
    switch (e->type) {
    case EVT_PAINT: need |= N_ALL; break;
    case EVT_RESIZE:
        if (win_canvas(win, &cv) == 0) { cw = cv.w; ch = cv.h; }
        need |= N_ALL;
        break;
    case EVT_CLOSE:
        cancel_draw();
        flush();
        if (confirm_discard()) { win_destroy(win); exit(0); }
        break;
    case EVT_MENU: menu_command(e->id); break;
    case EVT_MOUSE_DOWN: case EVT_DBLCLICK: mouse_down(e, &L); break;
    case EVT_MOUSE_MOVE: mouse_move(e, &L); break;
    case EVT_MOUSE_LEAVE:
        if (drawing) break;                 /* the mouse is captured: moves keep coming */
        set_cursor(CUR_ARROW);
        over = 0;
        if (hover >= 0) { hover = -1; need |= N_TOOLS; }
        break;
    case EVT_MOUSE_UP:
        if (drawing && e->button == dbtn) { dmods = e->mods; end_draw(e->x - L.ox, e->y - L.oy); }
        break;
    case EVT_WHEEL: {
        int step = 48 * e->dz;
        if (e->mods & MOD_SHIFT) sx -= step; else sy -= step;
        need |= N_VIEW;                     /* get_layout clamps */
        break;
    }
    case EVT_SCROLL:
        if (e->id == C_VSCROLL) sy = e->x; else if (e->id == C_HSCROLL) sx = e->x;
        need |= N_VIEW;
        break;
    case EVT_KEY:
        if (!e->pressed) break;
        if (e->id == KEY_ESC) { cancel_draw(); break; }
        if (!(e->mods & MOD_CTRL) || drawing) break;
        if (key_shortcut(e, 'N')) cmd_new();
        else if (key_shortcut(e, 'O')) cmd_open();
        else if (key_shortcut(e, 'S')) save();
        else if (key_shortcut(e, 'Z')) cmd_undo();
        break;
    case EVT_FOCUS:
        if (!e->pressed && drawing) end_draw(lx, ly);
        break;
    }
}

int main(int argc, char **argv) {
    if (blank(&img, DEF_W, DEF_H, OPAQUE | 0xFFFFFF) < 0) { msgbox(-1, "Paint", "Not enough memory to start Paint.", MBX_ICON_ERROR, MBX_OK); return 1; }
    opt[T_BRUSH] = 1;
    opt[T_ERASER] = 1;
    fg = OPAQUE | 0x000000;
    bg = OPAQUE | 0xFFFFFF;
    for (int i = 0; i < 28; i++) pal[i] = OPAQUE | default_palette[i];
    prefs_load();

    win = win_create("Untitled - Paint", TOOL_W + DEF_W + 2 * PAD + SB + 4, DEF_H + 2 * PAD + SB + PAL_H + STATUS_H + 4, WIN_RESIZABLE);
    if (win < 0) { printf("paint: no window (%d)\n", win); return 1; }
    win_set_icon(win, HICON_PAINT);
    win_set_min_size(win, 372, 270);        /* the kernel's 380 x 320 window frame */
    menu_set(win, menubar, N(menubar));
    ctl_create(win, CTL_VSCROLL, 0, 0, SB, 10, 0, C_VSCROLL, CTL_HIDDEN);
    ctl_create(win, CTL_HSCROLL, 0, 0, 10, SB, 0, C_HSCROLL, CTL_HIDDEN);
    last_hidden_v = last_hidden_h = 1;
    ctl_create(win, CTL_STATUSBAR, 0, 0, 0, STATUS_H, "", C_STATUS, 0);
    ctl_set_value(win, C_STATUS, 105);
    if (win_canvas(win, &cv) < 0) { printf("paint: no canvas\n"); return 1; }
    cw = cv.w; ch = cv.h;
    need = N_ALL;
    flush();
    if (argc > 1 && argv[1][0]) load(argv[1]);

    for (;;) {
        hal_event_t ev;
        if (!get_event(&ev, -1)) continue;
        handle(&ev);
        /* everything that is already queued, then draw once */
        for (int i = 0; i < 64 && get_event(&ev, 0); i++) handle(&ev);
        flush();
        prefs_sync();
    }
}
