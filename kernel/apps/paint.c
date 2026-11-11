/* Halcyon OS - Paint: a simple bitmap editor (Windows BMP files).
 *
 * Tools: pencil, brush, eraser, line, rectangle, filled rectangle, ellipse,
 * flood fill, colour picker. Left button draws with the primary colour,
 * right button with the secondary colour. One level of undo (Ctrl+Z
 * toggles between the last two states). */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/kmath.h"
#include "fs/vfs.h"

#define TOOL_W 62
#define PAL_H 46
#define STATUS_H 22
#define SB 17
#define PAD 6
#define DEF_W 480
#define DEF_H 360
#define MAX_BYTES (16u * 1024 * 1024)       /* largest picture we accept (32 bpp) */
#define MAX_FILE (96u * 1024 * 1024)

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

static const u32 default_palette[28] = {
    0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080,
    0x800080, 0x808040, 0x004040, 0x0080FF, 0x004080, 0x8000FF, 0x804000,
    0xFFFFFF, 0xC0C0C0, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF,
    0xFF00FF, 0xFFFF80, 0x00FF80, 0x80FFFF, 0x8080FF, 0xFF0080, 0xFF8040,
};

typedef struct paint {
    surface_t *img;
    surface_t *undo;
    int has_undo;
    char path[VFS_MAX_PATH];
    int dirty;
    int tool, prev_tool;
    int opt[NTOOLS];          /* option index per tool */
    color_t fg, bg;
    color_t pal[28];
    int sx, sy;               /* scroll position */
    /* drawing in progress */
    int drawing, dbtn;
    color_t dcol;
    int x0, y0, lx, ly;
    int pv_valid, pvx0, pvy0, pvx1, pvy1;   /* area touched by the shape preview */
    /* pointer */
    int mx, my, over;
    /* scroll bar dragging: 1 = vertical, 2 = horizontal */
    int sb_drag, sb_grab_pos, sb_grab_mouse;
} paint_t;

/* ---------------- layout ---------------- */
typedef struct layout {
    rect_t tools, view, canvas, vbar, hbar, palette, status;
    int need_v, need_h;
    int ox, oy;               /* canvas origin in client coordinates */
} layout_t;

static void get_layout(window_t *w, paint_t *p, layout_t *L) {
    int cw = w->client->w, ch = w->client->h, mb = menubar_height(w);
    int body_h = ch - mb - PAL_H - STATUS_H;
    L->tools = mkrect(0, mb, TOOL_W, body_h);
    L->palette = mkrect(0, ch - STATUS_H - PAL_H, cw, PAL_H);
    L->status = mkrect(0, ch - STATUS_H, cw, STATUS_H);
    L->view = mkrect(TOOL_W, mb, cw - TOOL_W, body_h);
    int tw = p->img->w + 2 * PAD, th = p->img->h + 2 * PAD;
    int vw = L->view.w, vh = L->view.h;
    L->need_h = tw > vw;
    L->need_v = th > vh - (L->need_h ? SB : 0);
    if (L->need_v && !L->need_h) L->need_h = tw > vw - SB;
    if (L->need_v) vw -= SB;
    if (L->need_h) vh -= SB;
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;
    L->canvas = mkrect(L->view.x, L->view.y, vw, vh);
    L->vbar = mkrect(L->view.x + vw, L->view.y, SB, vh);
    L->hbar = mkrect(L->view.x, L->view.y + vh, vw, SB);
    int maxx = MAX(0, tw - vw), maxy = MAX(0, th - vh);
    p->sx = CLAMP(p->sx, 0, maxx);
    p->sy = CLAMP(p->sy, 0, maxy);
    L->ox = L->canvas.x + PAD - p->sx;
    L->oy = L->canvas.y + PAD - p->sy;
    /* center small pictures */
    if (!L->need_h && tw < vw) L->ox = L->canvas.x + PAD;
    if (!L->need_v && th < vh) L->oy = L->canvas.y + PAD;
}

static rect_t tool_rect(layout_t *L, int i) {
    return mkrect(L->tools.x + 6 + (i % 2) * 26, L->tools.y + 8 + (i / 2) * 26, 25, 25);
}
static rect_t opt_box(layout_t *L) { return mkrect(L->tools.x + 6, L->tools.y + 8 + 5 * 26 + 6, 51, 74); }
static rect_t opt_rect(layout_t *L, int i) { rect_t b = opt_box(L); return mkrect(b.x + 3, b.y + 3 + i * 17, b.w - 6, 17); }
static rect_t pal_cell(layout_t *L, int i) { return mkrect(L->palette.x + 52 + (i % 14) * 18, L->palette.y + 6 + (i / 14) * 18, 16, 16); }
static rect_t swatch_rect(layout_t *L) { return mkrect(L->palette.x + 8, L->palette.y + 5, 36, 36); }

static int tool_has_options(int t) { return t == T_BRUSH || t == T_ERASER || t == T_LINE || t == T_RECT || t == T_ELLIPSE; }

/* ---------------- canvas primitives ---------------- */
static inline void put(surface_t *s, int x, int y, color_t c) {
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->stride + x] = c;
}
static void box(surface_t *s, int x0, int y0, int x1, int y1, color_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    x0 = MAX(x0, 0); y0 = MAX(y0, 0); x1 = MIN(x1, s->w - 1); y1 = MIN(y1, s->h - 1);
    for (int y = y0; y <= y1; y++) {
        u32 *row = s->px + y * s->stride;
        for (int x = x0; x <= x1; x++) row[x] = c;
    }
}
static void disc(surface_t *s, int cx, int cy, int d, color_t c) {
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
static void stamp(surface_t *s, int x, int y, int kind, int size, color_t c) {
    if (kind == 1) box(s, x - size / 2, y - size / 2, x - size / 2 + size - 1, y - size / 2 + size - 1, c);
    else disc(s, x, y, size, c);
}
static void stroke(surface_t *s, int x0, int y0, int x1, int y1, int kind, int size, color_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 200000; guard++) {
        stamp(s, x0, y0, kind, size, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void rect_outline(surface_t *s, int x0, int y0, int x1, int y1, int wd, color_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 + 1 <= 2 * wd || y1 - y0 + 1 <= 2 * wd) { box(s, x0, y0, x1, y1, c); return; }
    box(s, x0, y0, x1, y0 + wd - 1, c);
    box(s, x0, y1 - wd + 1, x1, y1, c);
    box(s, x0, y0, x0 + wd - 1, y1, c);
    box(s, x1 - wd + 1, y0, x1, y1, c);
}
static void ellipse(surface_t *s, int x0, int y0, int x1, int y1, int wd, int filled, color_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    double a = (x1 - x0 + 1) / 2.0, b = (y1 - y0 + 1) / 2.0;
    double cx = x0 + a, cy = y0 + b;
    double ai = a - wd, bi = b - wd;
    for (int y = y0; y <= y1; y++) {
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

static void flood(surface_t *s, int x, int y, color_t c) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) return;
    u32 target = s->px[y * s->stride + x];
    if (target == c) return;
    int cap = 4096, sp = 0;
    int *st = kmalloc(sizeof(int) * 2 * cap);
    if (!st) return;
    st[sp++] = x; st[sp++] = y;
    while (sp) {
        int cy = st[--sp], cx = st[--sp];
        u32 *row = s->px + cy * s->stride;
        if (row[cx] != target) continue;
        int xl = cx, xr = cx;
        while (xl > 0 && row[xl - 1] == target) xl--;
        while (xr < s->w - 1 && row[xr + 1] == target) xr++;
        for (int i = xl; i <= xr; i++) row[i] = c;
        for (int d = -1; d <= 1; d += 2) {
            int ny = cy + d;
            if (ny < 0 || ny >= s->h) continue;
            u32 *nr = s->px + ny * s->stride;
            int in = 0;
            for (int i = xl; i <= xr; i++) {
                if (nr[i] == target) {
                    if (!in) {
                        if (sp + 2 > 2 * cap) {
                            int *ns = krealloc(st, sizeof(int) * 4 * cap);
                            if (!ns) { kfree(st); return; }
                            st = ns; cap *= 2;
                        }
                        st[sp++] = i; st[sp++] = ny;
                        in = 1;
                    }
                } else in = 0;
            }
        }
    }
    kfree(st);
}

/* ---------------- BMP files ---------------- */
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

typedef struct chan { int shift, bits; } chan_t;
static chan_t mask_info(u32 m) {
    chan_t c = { 0, 0 };
    if (!m) return c;
    while (!(m & 1)) { m >>= 1; c.shift++; }
    while (m & 1) { m >>= 1; c.bits++; }
    return c;
}
static u32 chan_get(u32 v, chan_t c) {
    if (!c.bits) return 0;
    u32 x = (v >> c.shift) & ((c.bits >= 32) ? 0xFFFFFFFFu : ((1u << c.bits) - 1));
    if (c.bits >= 8) return x >> (c.bits - 8);
    /* scale up, e.g. 5 bits -> 8 bits */
    return (x * 255 + ((1u << c.bits) - 1) / 2) / ((1u << c.bits) - 1);
}

static int read_whole(const char *path, u8 **out, u32 *size, char *err, int errsz) {
    vfs_file_t *f;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r < 0) { snprintf(err, errsz, "Cannot open the file (%s).", strerror(r)); return -1; }
    u64 sz = vfs_size(f);
    if (sz > MAX_FILE) { vfs_close(f); snprintf(err, errsz, "The file is too large to open (%u MB).", (unsigned)(sz >> 20)); return -1; }
    u8 *buf = kmalloc(sz ? sz : 1);
    if (!buf) { vfs_close(f); snprintf(err, errsz, "Not enough memory to open the file."); return -1; }
    u32 got = 0;
    while (got < sz) {
        r = vfs_read(f, buf + got, (u32)(sz - got));
        if (r <= 0) break;
        got += (u32)r;
    }
    vfs_close(f);
    if (r < 0) { kfree(buf); snprintf(err, errsz, "Error reading the file (%s).", strerror(r)); return -1; }
    *out = buf;
    *size = got;
    return 0;
}

static surface_t *bmp_decode(const u8 *d, u32 size, char *err, int errsz) {
    const char *bad = "This is not a valid bitmap file.";
    if (size < 26 || d[0] != 'B' || d[1] != 'M') { strlcpy(err, bad, errsz); return NULL; }
    u32 off = rd32(d + 10), hdr = rd32(d + 14);
    i32 w, h;
    u32 bpp, comp = 0, nclr = 0, entsz = 4;
    u32 masks[3] = { 0, 0, 0 }, amask = 0;
    if (hdr == 12) {
        w = rd16(d + 18); h = (i16)rd16(d + 20); bpp = rd16(d + 24); entsz = 3;
    } else if (hdr >= 40 && hdr <= 4096 && size >= 14 + 40) {
        w = (i32)rd32(d + 18); h = (i32)rd32(d + 22); bpp = rd16(d + 28); comp = rd32(d + 30); nclr = rd32(d + 46);
        if (comp == 3 || comp == 6) {
            if (size < 54 + 12) { strlcpy(err, bad, errsz); return NULL; }
            masks[0] = rd32(d + 54); masks[1] = rd32(d + 58); masks[2] = rd32(d + 62);
            if (hdr >= 56 && size >= 70) amask = rd32(d + 66);
        }
    } else {
        strlcpy(err, bad, errsz);
        return NULL;
    }
    (void)amask;
    int topdown = h < 0;
    i64 hh = h < 0 ? -(i64)h : h;
    if (w <= 0 || hh <= 0 || w > 65535 || hh > 65535) { strlcpy(err, bad, errsz); return NULL; }
    if ((u64)w * (u64)hh * 4 > MAX_BYTES) {
        snprintf(err, errsz, "The picture is too large (%d x %d pixels). Paint can open pictures of up to 16 MB, "
                 "for example 2048 x 2048 pixels.", (int)w, (int)hh);
        return NULL;
    }
    int ok_fmt = 0;
    if ((bpp == 1 || bpp == 4 || bpp == 8) && comp == 0) ok_fmt = 1;
    if ((bpp == 16 || bpp == 32) && (comp == 0 || comp == 3 || comp == 6)) ok_fmt = 1;
    if (bpp == 24 && comp == 0) ok_fmt = 1;
    if (!ok_fmt) {
        snprintf(err, errsz, "This bitmap format is not supported (%u bits per pixel%s).", (unsigned)bpp,
                 comp == 1 || comp == 2 ? ", RLE compressed" : comp == 4 || comp == 5 ? ", JPEG/PNG inside" : comp ? ", compressed" : "");
        return NULL;
    }
    if (comp == 0 && bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; }
    if (comp == 0 && bpp == 32) { masks[0] = 0xFF0000; masks[1] = 0xFF00; masks[2] = 0xFF; }
    u64 rowsz = (((u64)w * bpp + 31) / 32) * 4;
    if ((u64)off + rowsz * (u64)hh > size || off < 14 + hdr) { strlcpy(err, "The bitmap file is truncated or damaged.", errsz); return NULL; }

    u32 pal[256];
    memset(pal, 0, sizeof pal);
    if (bpp <= 8) {
        u32 maxc = 1u << bpp;
        if (!nclr || nclr > maxc) nclr = maxc;
        u32 pofs = 14 + hdr;
        for (u32 i = 0; i < nclr; i++) {
            u32 o = pofs + i * entsz;
            if (o + 3 > size || o + 3 > off) break;
            pal[i] = RGB(d[o + 2], d[o + 1], d[o]);
        }
        for (u32 i = nclr; i < 256; i++) pal[i] = RGB(0, 0, 0);
    }
    chan_t ch[3];
    for (int i = 0; i < 3; i++) ch[i] = mask_info(masks[i]);

    surface_t *s = surface_new(w, (int)hh);
    if (!s) { strlcpy(err, "Not enough memory to open the picture.", errsz); return NULL; }
    for (i64 y = 0; y < hh; y++) {
        const u8 *row = d + off + rowsz * (u64)y;
        int dy = topdown ? (int)y : (int)(hh - 1 - y);
        u32 *dst = s->px + dy * s->stride;
        for (int x = 0; x < w; x++) {
            u32 c;
            switch (bpp) {
            case 1: c = pal[(row[x >> 3] >> (7 - (x & 7))) & 1]; break;
            case 4: c = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
            case 8: c = pal[row[x]]; break;
            case 24: c = RGB(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]); break;
            case 16: { u32 v = rd16(row + x * 2); c = RGB(chan_get(v, ch[0]), chan_get(v, ch[1]), chan_get(v, ch[2])); break; }
            default: { u32 v = rd32(row + x * 4); c = RGB(chan_get(v, ch[0]), chan_get(v, ch[1]), chan_get(v, ch[2])); break; }
            }
            dst[x] = c;
        }
    }
    return s;
}

static int bmp_save(const char *path, surface_t *s, char *err, int errsz) {
    u32 rowsz = ((u32)s->w * 3 + 3) & ~3u;
    u64 total = 54 + (u64)rowsz * s->h;
    if (total > 0xFFFFFFFFu) { strlcpy(err, "The picture is too large to save.", errsz); return -1; }
    u8 *buf = kzalloc(total);
    if (!buf) { strlcpy(err, "Not enough memory to save the picture.", errsz); return -1; }
    buf[0] = 'B'; buf[1] = 'M';
    wr32(buf + 2, (u32)total);
    wr32(buf + 10, 54);
    wr32(buf + 14, 40);
    wr32(buf + 18, (u32)s->w);
    wr32(buf + 22, (u32)s->h);
    wr16(buf + 26, 1);
    wr16(buf + 28, 24);
    wr32(buf + 34, rowsz * (u32)s->h);
    wr32(buf + 38, 2835);       /* 72 dpi */
    wr32(buf + 42, 2835);
    for (int y = 0; y < s->h; y++) {
        u8 *row = buf + 54 + (u64)rowsz * (s->h - 1 - y);
        const u32 *src = s->px + y * s->stride;
        for (int x = 0; x < s->w; x++) {
            row[x * 3] = (u8)src[x];
            row[x * 3 + 1] = (u8)(src[x] >> 8);
            row[x * 3 + 2] = (u8)(src[x] >> 16);
        }
    }
    int r = vfs_write_file(path, buf, (u32)total);
    kfree(buf);
    if (r < 0) { snprintf(err, errsz, "Cannot save the picture (%s).", strerror(r)); return -1; }
    return 0;
}

/* ---------------- document management ---------------- */
static void update_title(window_t *w) {
    paint_t *p = w->data;
    char t[128];
    snprintf(t, sizeof t, "%s - Paint", p->path[0] ? vfs_basename(p->path) : "Untitled");
    wm_set_title(w, t);
}

static void set_image(window_t *w, surface_t *img) {
    paint_t *p = w->data;
    surface_free(p->img);
    surface_free(p->undo);
    p->img = img;
    p->undo = NULL;
    p->has_undo = 0;
    p->sx = p->sy = 0;
    p->drawing = 0;
    p->dirty = 0;
    wm_invalidate(w);
}

static surface_t *blank(int wd, int ht, color_t c) {
    surface_t *s = surface_new(wd, ht);
    if (s) for (int i = 0; i < s->stride * s->h; i++) s->px[i] = c;
    return s;
}

static void start_dir(paint_t *p, char *out, int outsz) {
    if (p->path[0]) vfs_dirname(p->path, out, outsz);
    else strlcpy(out, "C:/Documents", outsz);
}

static int save_as(window_t *w);
static int save(window_t *w) {
    paint_t *p = w->data;
    if (!p->path[0]) return save_as(w);
    char err[160];
    wm_set_busy(1);
    int r = bmp_save(p->path, p->img, err, sizeof err);
    wm_set_busy(0);
    if (r < 0) { msgbox(w, "Paint", err, ICON_ERROR, MB_OK); return 0; }
    p->dirty = 0;
    update_title(w);
    return 1;
}

static int save_as(window_t *w) {
    paint_t *p = w->data;
    char dir[VFS_MAX_PATH], out[VFS_MAX_PATH];
    start_dir(p, dir, sizeof dir);
    if (!file_dialog(w, 1, "Save As", dir, "bmp", out, sizeof out)) return 0;
    if (!strrchr(vfs_basename(out), '.') && strlen(out) + 4 < sizeof out) strlcat(out, ".bmp", sizeof out);
    vfs_dirent_t st;
    if (vfs_stat(out, &st) == 0) {
        char q[VFS_MAX_PATH + 64];
        if (st.is_dir) { msgbox(w, "Save As", "A folder with that name already exists.", ICON_ERROR, MB_OK); return 0; }
        snprintf(q, sizeof q, "%s already exists.\nDo you want to replace it?", vfs_basename(out));
        if (msgbox(w, "Save As", q, ICON_WARNING, MB_YESNO) != ID_YES) return 0;
    }
    char old[VFS_MAX_PATH];
    strlcpy(old, p->path, sizeof old);
    strlcpy(p->path, out, sizeof p->path);
    if (!save(w)) { strlcpy(p->path, old, sizeof p->path); update_title(w); return 0; }
    return 1;
}

/* returns 0 if the user cancelled */
static int confirm_discard(window_t *w) {
    paint_t *p = w->data;
    if (!p->dirty) return 1;
    char q[VFS_MAX_PATH + 48];
    snprintf(q, sizeof q, "Save changes to %s?", p->path[0] ? vfs_basename(p->path) : "Untitled");
    int r = msgbox(w, "Paint", q, ICON_QUESTION, MB_YESNOCANCEL);
    if (r == ID_YES) return save(w);
    return r == ID_NO;
}

static int load(window_t *w, const char *path) {
    paint_t *p = w->data;
    char err[200];
    u8 *buf;
    u32 size;
    wm_set_busy(1);
    surface_t *s = NULL;
    if (read_whole(path, &buf, &size, err, sizeof err) == 0) {
        s = bmp_decode(buf, size, err, sizeof err);
        kfree(buf);
    }
    wm_set_busy(0);
    if (!s) {
        char msg[VFS_MAX_PATH + 220];
        snprintf(msg, sizeof msg, "Paint cannot open %s.\n\n%s", vfs_basename(path), err);
        msgbox(w, "Paint", msg, ICON_ERROR, MB_OK);
        return 0;
    }
    set_image(w, s);
    strlcpy(p->path, path, sizeof p->path);
    update_title(w);
    return 1;
}

static void cmd_new(window_t *w) {
    paint_t *p = w->data;
    if (!confirm_discard(w)) return;
    surface_t *s = blank(DEF_W, DEF_H, HEX(0xFFFFFF));
    if (!s) { msgbox(w, "Paint", "Not enough memory for a new picture.", ICON_ERROR, MB_OK); return; }
    set_image(w, s);
    p->path[0] = 0;
    update_title(w);
}

static void cmd_open(window_t *w) {
    paint_t *p = w->data;
    if (!confirm_discard(w)) return;
    char dir[VFS_MAX_PATH], out[VFS_MAX_PATH];
    start_dir(p, dir, sizeof dir);
    if (file_dialog(w, 0, "Open", dir, "bmp", out, sizeof out)) load(w, out);
}

/* ---------------- undo ---------------- */
static void snapshot(paint_t *p) {
    if (!p->undo || p->undo->w != p->img->w || p->undo->h != p->img->h) {
        surface_free(p->undo);
        p->undo = surface_new(p->img->w, p->img->h);
    }
    if (!p->undo) { p->has_undo = 0; return; }
    memcpy(p->undo->px, p->img->px, (size_t)p->img->stride * p->img->h * 4);
    p->has_undo = 1;
}

static void cmd_undo(window_t *w) {
    paint_t *p = w->data;
    if (!p->has_undo || p->drawing) return;
    surface_t *t = p->img;
    p->img = p->undo;
    p->undo = t;
    p->dirty = 1;
    wm_invalidate(w);
}

static void restore_area(paint_t *p, int x0, int y0, int x1, int y1) {
    if (!p->has_undo) return;
    x0 = MAX(x0, 0); y0 = MAX(y0, 0); x1 = MIN(x1, p->img->w - 1); y1 = MIN(y1, p->img->h - 1);
    for (int y = y0; y <= y1; y++)
        if (x1 >= x0) memcpy(p->img->px + y * p->img->stride + x0, p->undo->px + y * p->undo->stride + x0, (size_t)(x1 - x0 + 1) * 4);
}

/* ---------------- drawing operations ---------------- */
static void constrain(paint_t *p, int *x, int *y) {
    if (!(input_mods() & MOD_SHIFT)) return;
    int dx = *x - p->x0, dy = *y - p->y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (p->tool == T_LINE) {
        if (adx > 2 * ady) *y = p->y0;
        else if (ady > 2 * adx) *x = p->x0;
        else { int m = MAX(adx, ady); *x = p->x0 + (dx < 0 ? -m : m); *y = p->y0 + (dy < 0 ? -m : m); }
    } else {
        int m = MAX(adx, ady);
        *x = p->x0 + (dx < 0 ? -m : m);
        *y = p->y0 + (dy < 0 ? -m : m);
    }
}

static void draw_shape(paint_t *p, int x, int y) {
    int wd = line_widths[p->opt[p->tool] & 3];
    constrain(p, &x, &y);
    if (p->pv_valid) restore_area(p, p->pvx0, p->pvy0, p->pvx1, p->pvy1);
    switch (p->tool) {
    case T_LINE: stroke(p->img, p->x0, p->y0, x, y, 0, wd, p->dcol); break;
    case T_RECT: rect_outline(p->img, p->x0, p->y0, x, y, wd, p->dcol); break;
    case T_FRECT: box(p->img, p->x0, p->y0, x, y, p->dcol); break;
    case T_ELLIPSE: ellipse(p->img, p->x0, p->y0, x, y, wd, 0, p->dcol); break;
    }
    p->pv_valid = 1;
    p->pvx0 = MIN(p->x0, x) - wd - 1; p->pvy0 = MIN(p->y0, y) - wd - 1;
    p->pvx1 = MAX(p->x0, x) + wd + 1; p->pvy1 = MAX(p->y0, y) + wd + 1;
}

static void begin_draw(window_t *w, int x, int y, int btn) {
    paint_t *p = w->data;
    color_t col = btn == MOUSE_RIGHT ? p->bg : p->fg;
    if (p->tool == T_PICK) {
        if ((unsigned)x < (unsigned)p->img->w && (unsigned)y < (unsigned)p->img->h) {
            color_t c = p->img->px[y * p->img->stride + x] | 0xFF000000u;
            if (btn == MOUSE_RIGHT) p->bg = c; else p->fg = c;
            p->tool = p->prev_tool;
        }
        wm_invalidate(w);
        return;
    }
    snapshot(p);
    if (p->tool == T_FILL) {
        flood(p->img, x, y, col);
        p->dirty = 1;
        wm_invalidate(w);
        return;
    }
    p->drawing = 1;
    p->dbtn = btn;
    p->dcol = p->tool == T_ERASER ? p->bg : col;
    p->x0 = p->lx = x; p->y0 = p->ly = y;
    p->pv_valid = 0;
    switch (p->tool) {
    case T_PENCIL: put(p->img, x, y, p->dcol); break;
    case T_BRUSH: stamp(p->img, x, y, 0, brush_sizes[p->opt[T_BRUSH] & 3], p->dcol); break;
    case T_ERASER: stamp(p->img, x, y, 1, eraser_sizes[p->opt[T_ERASER] & 3], p->dcol); break;
    default: if (p->has_undo) draw_shape(p, x, y); break;
    }
    p->dirty = 1;
    wm_invalidate(w);
}

static void continue_draw(window_t *w, int x, int y) {
    paint_t *p = w->data;
    if (!p->drawing) return;
    switch (p->tool) {
    case T_PENCIL: stroke(p->img, p->lx, p->ly, x, y, 0, 1, p->dcol); break;
    case T_BRUSH: stroke(p->img, p->lx, p->ly, x, y, 0, brush_sizes[p->opt[T_BRUSH] & 3], p->dcol); break;
    case T_ERASER: stroke(p->img, p->lx, p->ly, x, y, 1, eraser_sizes[p->opt[T_ERASER] & 3], p->dcol); break;
    default: if (p->has_undo) draw_shape(p, x, y); break;
    }
    p->lx = x; p->ly = y;
    wm_invalidate(w);
}

static void end_draw(window_t *w, int x, int y) {
    paint_t *p = w->data;
    if (!p->drawing) return;
    if (p->tool >= T_LINE && p->tool <= T_ELLIPSE) {
        if (!p->has_undo) p->pv_valid = 0;
        draw_shape(p, x, y);
    } else {
        continue_draw(w, x, y);
    }
    p->drawing = 0;
    p->pv_valid = 0;
    p->dirty = 1;
    wm_invalidate(w);
}

static void cancel_draw(window_t *w) {
    paint_t *p = w->data;
    if (!p->drawing) return;
    if (p->has_undo) restore_area(p, 0, 0, p->img->w - 1, p->img->h - 1);
    p->drawing = 0;
    p->pv_valid = 0;
    wm_invalidate(w);
}

/* ---------------- painting the window ---------------- */
static void draw_tool_glyph(gc_t *g, rect_t r, int t) {
    int x = r.x + (r.w - 16) / 2, y = r.y + (r.h - 16) / 2;
    color_t ink = HEX(0x1C2230);
    switch (t) {
    case T_PENCIL:
        for (int i = 0; i < 10; i++) {
            gfx_fill(g, mkrect(x + 4 + i, y + 10 - i, 3, 2), i < 2 ? HEX(0xF2C89A) : i > 8 ? HEX(0xE88AA0) : HEX(0xF5B547));
        }
        gfx_line(g, x + 3, y + 13, x + 13, y + 3, HEX(0x9A6A10));
        gfx_line(g, x + 5, y + 14, x + 15, y + 4, HEX(0x9A6A10));
        gfx_pixel(g, x + 3, y + 14, ink); gfx_pixel(g, x + 2, y + 15, ink);
        break;
    case T_BRUSH:
        gfx_line(g, x + 8, y + 8, x + 14, y + 2, HEX(0x8A5A2A));
        gfx_line(g, x + 9, y + 8, x + 15, y + 2, HEX(0xA8743A));
        gfx_line(g, x + 9, y + 9, x + 15, y + 3, HEX(0x8A5A2A));
        gfx_fill(g, mkrect(x + 6, y + 8, 4, 3), HEX(0xB0B6C0));
        gfx_circle(g, x + 5, y + 12, 3, HEX(0x2E6FDB));
        gfx_pixel(g, x + 1, y + 15, HEX(0x2E6FDB)); gfx_pixel(g, x + 2, y + 15, HEX(0x2E6FDB));
        break;
    case T_ERASER:
        for (int i = 0; i < 7; i++) {
            gfx_hline(g, x + 6 - i / 2 + i, y + 4 + i, 8, i < 3 ? HEX(0xF4F4F4) : HEX(0xF08AA0));
        }
        gfx_line(g, x + 6, y + 4, x + 13, y + 4, ink);
        gfx_line(g, x + 13, y + 4, x + 17, y + 10, ink);
        gfx_line(g, x + 2, y + 10, x + 6, y + 4, ink);
        gfx_line(g, x + 2, y + 10, x + 9, y + 10, ink);
        gfx_line(g, x + 2, y + 10, x + 4, y + 13, ink);
        gfx_line(g, x + 4, y + 13, x + 11, y + 13, ink);
        gfx_line(g, x + 11, y + 13, x + 17, y + 10, ink);
        break;
    case T_LINE:
        gfx_line(g, x + 2, y + 13, x + 13, y + 2, ink);
        gfx_line(g, x + 3, y + 13, x + 14, y + 2, ink);
        break;
    case T_RECT:
        gfx_rect(g, mkrect(x + 1, y + 3, 14, 10), ink);
        gfx_rect(g, mkrect(x + 2, y + 4, 12, 8), ink);
        break;
    case T_FRECT:
        gfx_fill(g, mkrect(x + 1, y + 3, 14, 10), HEX(0x6FA2F5));
        gfx_rect(g, mkrect(x + 1, y + 3, 14, 10), ink);
        break;
    case T_ELLIPSE: {
        for (int a = 0; a < 64; a++) {
            double ang = a * 2 * K_PI / 64;
            int px = x + 8 + (int)(k_cos(ang) * 7.0), py = y + 8 + (int)(k_sin(ang) * 5.0);
            gfx_fill(g, mkrect(px, py, 2, 1), ink);
        }
        break;
    }
    case T_FILL:
        /* tilted bucket pouring a drop */
        for (int i = 0; i < 8; i++) gfx_hline(g, x + 4 + i / 2, y + 4 + i, 9 - i / 2 * 1, HEX(0xC8CCD4));
        gfx_line(g, x + 4, y + 4, x + 8, y + 12, ink);
        gfx_line(g, x + 8, y + 12, x + 13, y + 11, ink);
        gfx_line(g, x + 13, y + 11, x + 13, y + 3, ink);
        gfx_line(g, x + 4, y + 4, x + 13, y + 3, ink);
        gfx_line(g, x + 7, y + 2, x + 12, y + 1, ink);
        gfx_fill(g, mkrect(x + 2, y + 5, 2, 6), HEX(0x2E6FDB));
        gfx_fill(g, mkrect(x + 1, y + 10, 4, 3), HEX(0x2E6FDB));
        break;
    case T_PICK:
        gfx_line(g, x + 3, y + 13, x + 9, y + 7, ink);
        gfx_line(g, x + 4, y + 13, x + 10, y + 7, ink);
        gfx_line(g, x + 2, y + 14, x + 3, y + 13, ink);
        gfx_fill(g, mkrect(x + 9, y + 4, 4, 4), HEX(0x5A6470));
        gfx_circle(g, x + 12, y + 4, 3, HEX(0x2A3240));
        gfx_line(g, x + 8, y + 5, x + 11, y + 8, ink);
        break;
    }
}

static void draw_options(gc_t *g, layout_t *L, paint_t *p) {
    rect_t b = opt_box(L);
    gfx_sunken_field(g, b, color_lighten(g_theme->face, 30));
    int t = p->tool;
    if (!tool_has_options(t)) return;
    for (int i = 0; i < 4; i++) {
        rect_t r = opt_rect(L, i);
        int sel = (p->opt[t] & 3) == i;
        if (sel) gfx_fill(g, r, g_theme->select);
        color_t c = sel ? g_theme->select_text : HEX(0x1C2230);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (t == T_BRUSH) { int d = brush_sizes[i]; gfx_circle(g, cx, cy, d / 2, c); if (d <= 2) gfx_fill(g, mkrect(cx - 1, cy - 1, 2, 2), c); }
        else if (t == T_ERASER) { int d = MIN(eraser_sizes[i], 14); gfx_fill(g, mkrect(cx - d / 2, cy - d / 2 + (d > 13 ? 1 : 0), d, MIN(d, 14)), c); }
        else gfx_fill(g, mkrect(r.x + 6, cy - line_widths[i] / 2, r.w - 12, line_widths[i]), c);
    }
}

static void paint_paint(window_t *w, gc_t *g) {
    paint_t *p = w->data;
    const theme_t *th = g_theme;
    layout_t L;
    get_layout(w, p, &L);

    /* tool box */
    gfx_gradient_h(g, L.tools, color_lighten(th->face, 40), th->face);
    gfx_vline(g, L.tools.x + L.tools.w - 1, L.tools.y, L.tools.h, color_lighten(th->face, -30));
    for (int i = 0; i < NTOOLS; i++) {
        rect_t r = tool_rect(&L, i);
        int sel = p->tool == i;
        int hov = rect_contains(r, p->mx, p->my) && !p->over;
        if (sel) {
            gfx_rounded(g, r, 3, 15, color_lighten(th->select, 120), color_lighten(th->select, 80));
            gfx_rounded_outline(g, r, 3, 15, th->select);
        } else if (hov) {
            gfx_rounded(g, r, 3, 15, HEX(0xFFFFFF), color_lighten(th->hover, 70));
            gfx_rounded_outline(g, r, 3, 15, th->hover);
        } else {
            gfx_rounded(g, r, 3, 15, HEX(0xFFFFFF), HEX(0xE6E4DA));
            gfx_rounded_outline(g, r, 3, 15, color_lighten(th->button_border, 50));
        }
        draw_tool_glyph(g, r, i);
    }
    draw_options(g, &L, p);

    /* canvas area */
    gfx_fill(g, L.view, HEX(0x8E98AC));
    gc_t cg = gc_sub(g, L.canvas);
    int ix = L.ox - L.canvas.x, iy = L.oy - L.canvas.y;
    gfx_shadow(&cg, mkrect(ix, iy, p->img->w, p->img->h), 5, 90);
    gfx_blit(&cg, ix, iy, p->img, mkrect(0, 0, p->img->w, p->img->h));
    if (L.need_v) draw_scrollbar(g, L.vbar, p->img->h + 2 * PAD, L.canvas.h, p->sy, 1);
    if (L.need_h) draw_scrollbar(g, L.hbar, p->img->w + 2 * PAD, L.canvas.w, p->sx, 0);
    if (L.need_v && L.need_h) gfx_fill(g, mkrect(L.vbar.x, L.hbar.y, SB, SB), th->face);

    /* palette */
    rect_t pr = L.palette;
    gfx_gradient_v(g, pr, color_lighten(th->face, 30), th->face);
    gfx_hline(g, pr.x, pr.y, pr.w, color_lighten(th->face, -30));
    gfx_hline(g, pr.x, pr.y + 1, pr.w, HEX(0xFFFFFF));
    rect_t sw = swatch_rect(&L);
    gfx_sunken_field(g, sw, color_lighten(th->face, 40));
    rect_t bgr = mkrect(sw.x + 14, sw.y + 14, 18, 18), fgr = mkrect(sw.x + 4, sw.y + 4, 18, 18);
    gfx_fill(g, bgr, p->bg); gfx_rect(g, bgr, HEX(0x404040)); gfx_rect(g, rect_inset(bgr, 1), HEX(0xFFFFFF));
    gfx_fill(g, fgr, p->fg); gfx_rect(g, fgr, HEX(0x404040)); gfx_rect(g, rect_inset(fgr, 1), HEX(0xFFFFFF));
    for (int i = 0; i < 28; i++) {
        rect_t c = pal_cell(&L, i);
        gfx_fill(g, c, p->pal[i]);
        gfx_rect(g, c, HEX(0x6A6A6A));
        gfx_hline(g, c.x + 1, c.y + 1, c.w - 2, color_lighten(p->pal[i], 50));
    }

    /* status bar */
    rect_t st = L.status;
    gfx_gradient_v(g, st, color_lighten(th->face, 20), color_lighten(th->face, -10));
    gfx_hline(g, st.x, st.y, st.w, color_lighten(th->face, -35));
    char left[160], pos[48], dim[48];
    snprintf(left, sizeof left, "%s: %s", tool_names[p->tool], tool_hints[p->tool]);
    int rw = 210;
    gfx_text_box(g, &font_ui, mkrect(st.x + 8, st.y + 1, st.w - rw - 16, st.h - 1), left, HEX(0x303440), 0);
    if (p->over) snprintf(pos, sizeof pos, "%d, %d px", p->mx, p->my);
    else pos[0] = 0;
    snprintf(dim, sizeof dim, "%d x %d px", p->img->w, p->img->h);
    int sx = st.x + st.w - rw;
    if (sx > st.x + 60) {
        gfx_vline(g, sx, st.y + 3, st.h - 6, color_lighten(th->face, -40));
        gfx_vline(g, sx + 1, st.y + 3, st.h - 6, HEX(0xFFFFFF));
        gfx_text_box(g, &font_ui, mkrect(sx + 8, st.y + 1, 95, st.h - 1), pos, HEX(0x303440), 0);
        gfx_vline(g, sx + 105, st.y + 3, st.h - 6, color_lighten(th->face, -40));
        gfx_vline(g, sx + 106, st.y + 3, st.h - 6, HEX(0xFFFFFF));
        gfx_text_box(g, &font_ui, mkrect(sx + 113, st.y + 1, 95, st.h - 1), dim, HEX(0x303440), 0);
    }
}

/* ---------------- events ---------------- */
static void edit_palette(window_t *w, int i, int secondary) {
    paint_t *p = w->data;
    char buf[16];
    snprintf(buf, sizeof buf, "%06x", p->pal[i] & 0xFFFFFF);
    if (!inputbox(w, "Edit Colours", "Enter the colour as a hex RRGGBB value:", buf, sizeof buf)) return;
    const char *s = buf;
    while (*s == ' ' || *s == '#') s++;
    char *end;
    long v = strtol(s, &end, 16);
    if (end == s || v < 0 || v > 0xFFFFFF) { msgbox(w, "Edit Colours", "Please enter six hex digits, e.g. FF8000.", ICON_WARNING, MB_OK); return; }
    p->pal[i] = HEX((u32)v);
    if (secondary) p->bg = p->pal[i]; else p->fg = p->pal[i];
    wm_invalidate(w);
}

static void select_tool(paint_t *p, int t) {
    if (t == T_PICK && p->tool != T_PICK) p->prev_tool = p->tool;
    p->tool = t;
}

static void paint_event(window_t *w, gui_event_t *e) {
    paint_t *p = w->data;
    layout_t L;
    get_layout(w, p, &L);
    int cx = e->x - L.ox, cy = e->y - L.oy;

    switch (e->type) {
    case GE_MOUSE_DOWN: case GE_DBLCLICK: {
        if (p->drawing) {
            /* the other button cancels a drag */
            if (e->button != p->dbtn) cancel_draw(w);
            break;
        }
        if (e->button != MOUSE_LEFT && e->button != MOUSE_RIGHT) break;
        for (int i = 0; i < NTOOLS; i++)
            if (rect_contains(tool_rect(&L, i), e->x, e->y)) { select_tool(p, i); wm_invalidate(w); return; }
        if (tool_has_options(p->tool))
            for (int i = 0; i < 4; i++)
                if (rect_contains(opt_rect(&L, i), e->x, e->y)) { p->opt[p->tool] = i; wm_invalidate(w); return; }
        for (int i = 0; i < 28; i++)
            if (rect_contains(pal_cell(&L, i), e->x, e->y)) {
                if (e->type == GE_DBLCLICK) { edit_palette(w, i, e->button == MOUSE_RIGHT); return; }
                if (e->button == MOUSE_RIGHT) p->bg = p->pal[i]; else p->fg = p->pal[i];
                wm_invalidate(w);
                return;
            }
        if (rect_contains(swatch_rect(&L), e->x, e->y) && e->type == GE_DBLCLICK) {
            color_t t = p->fg; p->fg = p->bg; p->bg = t;   /* double click swaps the colours */
            wm_invalidate(w);
            return;
        }
        if (L.need_v && rect_contains(L.vbar, e->x, e->y) && e->button == MOUSE_LEFT) {
            int part, total = p->img->h + 2 * PAD;
            int np = scrollbar_hit(L.vbar, total, L.canvas.h, p->sy, e->x, e->y, 1, &part);
            if (part == 1 || part == 2) np = CLAMP(p->sy + (part == 1 ? -24 : 24), 0, total - L.canvas.h);
            if (part == 5) { p->sb_drag = 1; p->sb_grab_pos = p->sy; p->sb_grab_mouse = e->y; }
            p->sy = np;
            wm_invalidate(w);
            return;
        }
        if (L.need_h && rect_contains(L.hbar, e->x, e->y) && e->button == MOUSE_LEFT) {
            int part, total = p->img->w + 2 * PAD;
            int np = scrollbar_hit(L.hbar, total, L.canvas.w, p->sx, e->x, e->y, 0, &part);
            if (part == 1 || part == 2) np = CLAMP(p->sx + (part == 1 ? -24 : 24), 0, total - L.canvas.w);
            if (part == 5) { p->sb_drag = 2; p->sb_grab_pos = p->sx; p->sb_grab_mouse = e->x; }
            p->sx = np;
            wm_invalidate(w);
            return;
        }
        if (rect_contains(L.canvas, e->x, e->y)) begin_draw(w, cx, cy, e->button);
        break;
    }
    case GE_MOUSE_MOVE: case GE_MOUSE_LEAVE: {
        if (p->sb_drag && (e->buttons & MOUSE_LEFT)) {
            int vertical = p->sb_drag == 1;
            rect_t bar = vertical ? L.vbar : L.hbar;
            int total = vertical ? p->img->h + 2 * PAD : p->img->w + 2 * PAD;
            int visible = vertical ? L.canvas.h : L.canvas.w;
            int track = (vertical ? bar.h : bar.w) - 2 * SB;
            int tlen = MAX(16, track * visible / MAX(1, total));
            int delta = (vertical ? e->y : e->x) - p->sb_grab_mouse;
            int range = MAX(1, track - tlen);
            int np = CLAMP(p->sb_grab_pos + delta * (total - visible) / range, 0, MAX(0, total - visible));
            if (vertical) p->sy = np; else p->sx = np;
            wm_invalidate(w);
            break;
        }
        int over = e->type == GE_MOUSE_MOVE && rect_contains(L.canvas, e->x, e->y);
        w->cursor = over ? CURSOR_CROSS : CURSOR_ARROW;
        if (p->drawing) {
            continue_draw(w, cx, cy);
            p->over = 1; p->mx = cx; p->my = cy;
            break;
        }
        int inimg = over && cx >= 0 && cy >= 0 && cx < p->img->w && cy < p->img->h;
        int nmx = inimg ? cx : e->x, nmy = inimg ? cy : e->y;
        if (inimg != p->over || nmx != p->mx || nmy != p->my) {
            p->over = inimg; p->mx = nmx; p->my = nmy;
            wm_invalidate(w);
        }
        break;
    }
    case GE_MOUSE_UP:
        if (p->sb_drag) { p->sb_drag = 0; break; }
        if (p->drawing && e->button == p->dbtn) end_draw(w, cx, cy);
        break;
    case GE_WHEEL: {
        int step = 48 * e->dz;
        if (input_mods() & MOD_SHIFT) p->sx -= step; else p->sy -= step;
        wm_invalidate(w);   /* get_layout clamps */
        break;
    }
    case GE_KEY: {
        input_event_t *k = &e->key;
        if (!k->pressed) break;
        if (k->key == KEY_ESC) { cancel_draw(w); break; }
        if (!(k->mods & MOD_CTRL) || p->drawing) break;
        if (key_shortcut(k, 'N')) cmd_new(w);
        else if (key_shortcut(k, 'O')) cmd_open(w);
        else if (key_shortcut(k, 'S')) save(w);
        else if (key_shortcut(k, 'Z')) cmd_undo(w);
        break;
    }
    case GE_BLUR:
        if (p->drawing) end_draw(w, p->lx, p->ly);
        break;
    }
}

/* ---------------- menus ---------------- */
enum { PM_NEW = 1, PM_OPEN, PM_SAVE, PM_SAVEAS, PM_EXIT, PM_UNDO, PM_CLEAR, PM_SWAP, PM_ABOUT };
static menu_item_t file_items[] = {
    { "New", "Ctrl+N", PM_NEW, 0, 0 },
    { "Open...", "Ctrl+O", PM_OPEN, 0, ICON_FOLDER_OPEN },
    { "Save", "Ctrl+S", PM_SAVE, 0, 0 },
    { "Save As...", NULL, PM_SAVEAS, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Exit", NULL, PM_EXIT, 0, 0 },
};
static menu_item_t edit_items[] = {
    { "Undo", "Ctrl+Z", PM_UNDO, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Swap colours", NULL, PM_SWAP, 0, 0 },
    { "Clear image", NULL, PM_CLEAR, 0, 0 },
};
static menu_item_t help_items[] = {
    { "About Paint", NULL, PM_ABOUT, 0, ICON_PAINT },
};
static menu_t paint_menus[] = {
    { "File", file_items, ARRAY_SIZE(file_items) },
    { "Edit", edit_items, ARRAY_SIZE(edit_items) },
    { "Help", help_items, ARRAY_SIZE(help_items) },
};

static void paint_menu(window_t *w, int id) {
    paint_t *p = w->data;
    if (p->drawing) return;
    switch (id) {
    case PM_NEW: cmd_new(w); break;
    case PM_OPEN: cmd_open(w); break;
    case PM_SAVE: save(w); break;
    case PM_SAVEAS: save_as(w); break;
    case PM_EXIT: wm_close(w); break;
    case PM_UNDO: cmd_undo(w); break;
    case PM_SWAP: { color_t t = p->fg; p->fg = p->bg; p->bg = t; wm_invalidate(w); break; }
    case PM_CLEAR:
        snapshot(p);
        box(p->img, 0, 0, p->img->w - 1, p->img->h - 1, p->bg);
        p->dirty = 1;
        wm_invalidate(w);
        break;
    case PM_ABOUT:
        msgbox(w, "About Paint", "Halcyon Paint " OS_VERSION "\n\nOpens 1, 4, 8, 16, 24 and 32-bit Windows bitmaps "
               "and saves 24-bit bitmaps.\n\nLeft button: primary colour. Right button: secondary colour. "
               "Double click a palette colour to change it.", ICON_PAINT, MB_OK);
        break;
    }
}

static int paint_close(window_t *w) {
    paint_t *p = w->data;
    if (p->drawing) cancel_draw(w);
    return confirm_discard(w);
}

static void paint_destroy(window_t *w) {
    paint_t *p = w->data;
    if (p) {
        surface_free(p->img);
        surface_free(p->undo);
        kfree(p);
    }
    w->data = NULL;
}

void app_paint(const char *path) {
    paint_t *p = kzalloc(sizeof *p);
    if (!p) return;
    p->img = blank(DEF_W, DEF_H, HEX(0xFFFFFF));
    if (!p->img) { kfree(p); msgbox(NULL, "Paint", "Not enough memory to start Paint.", ICON_ERROR, MB_OK); return; }
    p->tool = p->prev_tool = T_PENCIL;
    p->opt[T_BRUSH] = 1;
    p->opt[T_ERASER] = 1;
    p->fg = HEX(0x000000);
    p->bg = HEX(0xFFFFFF);
    for (int i = 0; i < 28; i++) p->pal[i] = HEX(default_palette[i]);
    int cw = MIN(TOOL_W + DEF_W + 2 * PAD + SB + 4, screen_w() - 40);
    int ch = MIN(MENUBAR_H + DEF_H + 2 * PAD + SB + PAL_H + STATUS_H + 4, screen_h() - TASKBAR_H - 60);
    window_t *w = wm_create("Untitled - Paint", ICON_PAINT, -1, -1, cw, ch, WF_RESIZABLE);
    w->data = p;
    w->min_w = 380;
    w->min_h = 320;
    w->on_paint = paint_paint;
    w->on_event = paint_event;
    w->on_menu = paint_menu;
    w->on_close = paint_close;
    w->on_destroy = paint_destroy;
    menubar_attach(w, paint_menus, ARRAY_SIZE(paint_menus));
    if (path && path[0]) load(w, path);
}

void app_imageview(const char *path) { app_paint(path); }
