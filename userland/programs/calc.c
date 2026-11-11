/* CALC.EXE - Calculator in user mode: a standard immediate-execution
 * calculator with memory, keyboard input and its own number formatting.
 * Looks and works like the kernel's Calculator (kernel/apps/calc.c).
 *
 * The user API has no menus, so the kernel version's menu bar becomes a
 * command strip of flat buttons in the same place: Copy, Paste (also
 * Ctrl+C / Ctrl+V) and Help (the About box, also F1).
 *
 * Drawing: everything except text is drawn into a private pixel buffer with
 * the same primitives the kernel uses (gradients, anti-aliased rounded
 * rectangles), copied into the window with gfx_blit, then the text is drawn
 * on top with gfx_text. */
#include "hal.h"

#define COLS 5
#define ROWS 6
#define BW 46
#define BH 32
#define GAP 6
#define MARGIN 12
#define DISP_H 64
#define BAR_H 20             /* command strip (the kernel version's menu bar) */
#define MAX_DIGITS 16

/* "Halcyon Blue" theme colours (kernel/gui/wm.c) */
#define TH_FACE          0xEEEBE0
#define TH_SELECT        0x316AC5
#define TH_SELECT_TEXT   0xFFFFFF
#define TH_SIDEBAR_TOP   0x7AA2E6
#define TH_MENU_HDR_BOT  0x1B4FB2
#define TH_BUTTON_BORDER 0x2F4E87
#define TH_HOVER         0xF5B547

#define ICON_CALC 16         /* the kernel's ICON_CALC (build/gen/icons.h) */

/* key codes (USB HID usages, kernel/drivers/input.h) */
#define KEY_A 0x04
#define KEY_ENTER 0x28
#define KEY_ESC 0x29
#define KEY_BACKSPACE 0x2A
#define KEY_F1 0x3A
#define KEY_F9 0x42
#define KEY_DELETE 0x4C
#define KEY_KP_ENTER 0x58
#define MOD_CTRL 0x01
#define MOUSE_LEFT 0x01

/* ================= drawing into a private pixel buffer ================= */
typedef struct { int x, y, w, h; } rc_t;
static rc_t R(int x, int y, int w, int h) { rc_t r = { x, y, w, h }; return r; }
static int rc_has(rc_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static rc_t rc_and(rc_t a, rc_t b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return R(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

static unsigned *px;         /* the client area, 0xRRGGBB */
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
/* rounded rectangle with a vertical gradient (all four corners) */
static void rounded(rc_t r, int rad, unsigned top, unsigned bot) {
    if (r.w <= 0 || r.h <= 0) return;
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    for (int y = 0; y < r.h; y++) {
        unsigned col = mix(top, bot, r.h > 1 ? y * 255 / (r.h - 1) : 0);
        int x0 = 0, x1 = r.w;
        int in_top = y < rad, in_bot = y >= r.h - rad;
        if (in_top || in_bot) {
            int dy = in_top ? rad - y - 1 : y - (r.h - rad);
            for (int x = 0; x < rad; x++) {
                int cov = corner_cov((rad - x - 1) * 2, dy * 2, rad);
                pixel_a(r.x + x, r.y + y, col, cov);
                pixel_a(r.x + r.w - 1 - x, r.y + y, col, cov);
            }
            x0 = rad; x1 = r.w - rad;
        }
        hline(r.x + x0, r.y + y, x1 - x0, col);
    }
}
static void rounded_outline(rc_t r, int rad, unsigned c) {
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    hline(r.x + rad, r.y, r.w - 2 * rad, c);
    hline(r.x + rad, r.y + r.h - 1, r.w - 2 * rad, c);
    vline(r.x, r.y + rad, r.h - 2 * rad, c);
    vline(r.x + r.w - 1, r.y + rad, r.h - 2 * rad, c);
    if (!rad) return;
    for (int q = 0; q < 4; q++) {
        int left = q == 0 || q == 3, upper = q == 0 || q == 1;
        int cx = left ? r.x + rad : r.x + r.w - 1 - rad, cy = upper ? r.y + rad : r.y + r.h - 1 - rad;
        for (int dy = 0; dy <= rad; dy++)
            for (int dx = 0; dx <= rad; dx++) {
                int diff = dx * dx + dy * dy - rad * rad, band = 2 * rad;
                if (diff < -band || diff > band) continue;
                int a = 255 - (diff < 0 ? -diff : diff) * 255 / band;
                if (a > 0) pixel_blend(cx + (left ? -dx : dx), cy + (upper ? -dy : dy), c, a);
            }
    }
}

/* text goes on top of the blitted pixels: queued while drawing, drawn by present() */
typedef struct { short x, y; unsigned char font; unsigned color; char s[48]; } text_t;
static text_t tq[64];
static int ntq;
static int font_h[8];
static int font_height(int f) { if (!font_h[f]) font_h[f] = gfx_font_height(f); return font_h[f]; }
static void text_at(int x, int y, const char *s, unsigned c, int f) {
    if (ntq >= (int)(sizeof tq / sizeof tq[0])) return;
    text_t *t = &tq[ntq++];
    t->x = (short)x; t->y = (short)y; t->font = (unsigned char)f; t->color = c;
    strlcpy(t->s, s, sizeof t->s);
}
/* align 0 left, 1 center, 2 right; vertically centred; "..." when too long */
static void text_box(rc_t r, const char *s, unsigned c, int f, int align) {
    int w = gfx_text_width(s, f), y = r.y + (r.h - font_height(f)) / 2;
    if (w <= r.w) { text_at(align == 1 ? r.x + (r.w - w) / 2 : align == 2 ? r.x + r.w - w : r.x, y, s, c, f); return; }
    char t[52];
    int room = r.w - gfx_text_width("...", f), n = 0;
    for (int i = 1; s[i - 1] && i < 44; i++) {
        if ((s[i] & 0xC0) == 0x80) continue;           /* whole UTF-8 characters */
        memcpy(t, s, (size_t)i); t[i] = 0;
        if (gfx_text_width(t, f) > room) break;
        n = i;
    }
    memcpy(t, s, (size_t)n);
    strlcpy(t + n, "...", sizeof t - (size_t)n);
    text_at(r.x, y, t, c, f);
}
static void present(int win) {
    gfx_blit(win, 0, 0, pw, ph, px);
    for (int i = 0; i < ntq; i++) gfx_text(win, tq[i].x, tq[i].y, tq[i].s, tq[i].color, tq[i].font);
    ntq = 0;
    win_invalidate(win);
}

/* ================= calculator ================= */
enum { K_DIGIT, K_OP, K_FUNC, K_MEM, K_CLEAR, K_EQUALS };

/* actions */
enum {
    A_MC = 200, A_MR, A_MS, A_MPLUS, A_MMINUS,
    A_BACK, A_CE, A_C, A_NEG, A_SQRT, A_PCT, A_RECIP,
};

typedef struct cbtn { const char *label; int action; int col, row, cs, rs, kind; } cbtn_t;

static const cbtn_t buttons[] = {
    { "MC", A_MC, 0, 0, 1, 1, K_MEM }, { "MR", A_MR, 1, 0, 1, 1, K_MEM }, { "MS", A_MS, 2, 0, 1, 1, K_MEM },
    { "M+", A_MPLUS, 3, 0, 1, 1, K_MEM }, { "M-", A_MMINUS, 4, 0, 1, 1, K_MEM },
    { "", A_BACK, 0, 1, 1, 1, K_CLEAR }, { "CE", A_CE, 1, 1, 1, 1, K_CLEAR }, { "C", A_C, 2, 1, 1, 1, K_CLEAR },
    { "±", A_NEG, 3, 1, 1, 1, K_FUNC }, { "", A_SQRT, 4, 1, 1, 1, K_FUNC },
    { "7", '7', 0, 2, 1, 1, K_DIGIT }, { "8", '8', 1, 2, 1, 1, K_DIGIT }, { "9", '9', 2, 2, 1, 1, K_DIGIT },
    { "÷", '/', 3, 2, 1, 1, K_OP }, { "%", A_PCT, 4, 2, 1, 1, K_FUNC },
    { "4", '4', 0, 3, 1, 1, K_DIGIT }, { "5", '5', 1, 3, 1, 1, K_DIGIT }, { "6", '6', 2, 3, 1, 1, K_DIGIT },
    { "×", '*', 3, 3, 1, 1, K_OP }, { "1/x", A_RECIP, 4, 3, 1, 1, K_FUNC },
    { "1", '1', 0, 4, 1, 1, K_DIGIT }, { "2", '2', 1, 4, 1, 1, K_DIGIT }, { "3", '3', 2, 4, 1, 1, K_DIGIT },
    { "-", '-', 3, 4, 1, 1, K_OP }, { "=", '=', 4, 4, 1, 2, K_EQUALS },
    { "0", '0', 0, 5, 2, 1, K_DIGIT }, { ".", '.', 2, 5, 1, 1, K_DIGIT }, { "+", '+', 3, 5, 1, 1, K_OP },
};
#define NBTN ((int)(sizeof buttons / sizeof buttons[0]))

/* command strip */
enum { CM_COPY = 1, CM_PASTE, CM_HELP };
static const struct { const char *label; int id, right; } bar[] = {
    { "Copy", CM_COPY, 0 }, { "Paste", CM_PASTE, 0 }, { "Help", CM_HELP, 1 },
};
#define NBAR ((int)(sizeof bar / sizeof bar[0]))

typedef struct calc {
    double acc;          /* left operand */
    int op;              /* pending operator or 0 */
    double cur;          /* value shown when not entering */
    char entry[40];      /* digits being typed */
    int entering;
    int error;
    char errmsg[40];
    double mem;
    int last_op;         /* for repeated '=' */
    double last_val;
    char expr[96];
    int hover, pressed;  /* button indices or -1 */
    int flash;           /* button highlighted by a key press */
    int bar_hover, bar_pressed;
} calc_t;

static calc_t calc;
static int win;

/* ---------------- maths ---------------- */
static double c_fabs(double v) { return v < 0 ? -v : v; }
static double c_sqrt(double v) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(v)); return r; }
static double c_floor(double v) { double t = (double)(long long)v; return t > v ? t - 1 : t; }   /* |v| < 2^63 */

/* ---------------- number formatting ---------------- */
static const double p10tab[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static double scale10(double v, int k) {
    while (k > 22) { v *= 1e22; k -= 22; }
    while (k < -22) { v /= 1e22; k += 22; }
    return k >= 0 ? v * p10tab[k] : v / p10tab[-k];
}

static int is_bad(double v) { return v != v || v - v != 0; }   /* NaN or infinity */

/* format with up to 15 significant digits, fixed or scientific */
static void fmt_num(double v, char *out, int outsz) {
    char buf[48];
    int p = 0;
    if (is_bad(v)) { strlcpy(out, "Overflow", (size_t)outsz); return; }
    if (v == 0) { strlcpy(out, "0", (size_t)outsz); return; }
    if (v < 0) { buf[p++] = '-'; v = -v; }
    union { double d; uint64_t u; } bits = { .d = v };
    int be = (int)((bits.u >> 52) & 0x7FF) - 1023;
    int e = (int)c_floor(be * 0.30102999566398);
    double s = scale10(v, 14 - e);
    for (int i = 0; i < 40 && (s >= 1e15 || s < 1e14); i++) {
        if (s >= 1e15) e++; else e--;
        s = scale10(v, 14 - e);
    }
    uint64_t n = (uint64_t)(s + 0.5);
    if (n >= 1000000000000000ull) { n = 100000000000000ull; e++; }
    char d[16];
    for (int i = 14; i >= 0; i--) { d[i] = (char)('0' + n % 10); n /= 10; }
    int nd = 15;
    while (nd > 1 && d[nd - 1] == '0') nd--;
    if (e >= 16 || e < -7) {
        buf[p++] = d[0];
        if (nd > 1) { buf[p++] = '.'; for (int i = 1; i < nd; i++) buf[p++] = d[i]; }
        buf[p] = 0;
        snprintf(buf + p, sizeof buf - (size_t)p, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e >= 0) {
        for (int i = 0; i <= e; i++) buf[p++] = i < nd ? d[i] : '0';
        if (nd > e + 1) { buf[p++] = '.'; for (int i = e + 1; i < nd; i++) buf[p++] = d[i]; }
        buf[p] = 0;
    } else {
        buf[p++] = '0'; buf[p++] = '.';
        for (int i = 0; i < -e - 1; i++) buf[p++] = '0';
        for (int i = 0; i < nd; i++) buf[p++] = d[i];
        buf[p] = 0;
    }
    strlcpy(out, buf, (size_t)outsz);
}

static double parse_entry(const char *s) {
    int neg = 0, frac = -1;
    double m = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.') { frac = 0; continue; }
        if (*s < '0' || *s > '9') break;
        m = m * 10 + (*s - '0');
        if (frac >= 0) frac++;
    }
    if (frac > 0) m = scale10(m, -frac);
    return neg ? -m : m;
}

/* ---------------- logic ---------------- */
static double cur_value(calc_t *c) { return c->entering ? parse_entry(c->entry) : c->cur; }

static void set_error(calc_t *c, const char *msg) {
    c->error = 1;
    strlcpy(c->errmsg, msg, sizeof c->errmsg);
    c->op = 0; c->entering = 0; c->expr[0] = 0; c->last_op = 0;
}

static void set_result(calc_t *c, double v) {
    if (is_bad(v) || c_fabs(v) > 1e300) { set_error(c, "Overflow"); return; }
    if (c_fabs(v) < 1e-300) v = 0;
    c->cur = v;
    c->entering = 0;
}

static const char *op_text(int op) {
    switch (op) { case '+': return "+"; case '-': return "-"; case '*': return "×"; case '/': return "÷"; }
    return "";
}

static void update_expr(calc_t *c) {
    if (!c->op) { c->expr[0] = 0; return; }
    char n[48];
    fmt_num(c->acc, n, sizeof n);
    snprintf(c->expr, sizeof c->expr, "%s %s", n, op_text(c->op));
}

/* returns 0 on error (already reported) */
static int apply_op(calc_t *c, double a, int op, double b, double *out) {
    switch (op) {
    case '+': *out = a + b; break;
    case '-': *out = a - b; break;
    case '*': *out = a * b; break;
    case '/':
        if (b == 0) { set_error(c, a == 0 ? "Result is undefined" : "Cannot divide by zero"); return 0; }
        *out = a / b; break;
    default: *out = b; break;
    }
    if (is_bad(*out) || c_fabs(*out) > 1e300) { set_error(c, "Overflow"); return 0; }
    return 1;
}

static void clear_all(calc_t *c) {
    c->acc = 0; c->op = 0; c->cur = 0; c->entering = 0; c->error = 0;
    c->entry[0] = 0; c->expr[0] = 0; c->last_op = 0; c->last_val = 0;
}

static int entry_digits(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s >= '0' && *s <= '9') n++;
    return n;
}

static void calc_action(calc_t *c, int a) {
    if (c->error) {
        /* only digits, the decimal point and clearing work after an error */
        if ((a >= '0' && a <= '9') || a == '.' || a == A_C || a == A_CE || a == A_BACK) clear_all(c);
        else if (a == A_MC) c->mem = 0;
        else return;
        if (a == A_C || a == A_CE || a == A_BACK) return;
    }
    if (a >= '0' && a <= '9') {
        if (!c->entering) { strlcpy(c->entry, "0", sizeof c->entry); c->entering = 1; if (!c->op) c->last_op = 0; }
        if (entry_digits(c->entry) >= MAX_DIGITS) return;
        if (!strcmp(c->entry, "0")) c->entry[0] = 0;
        else if (!strcmp(c->entry, "-0")) c->entry[1] = 0;
        size_t l = strlen(c->entry);
        c->entry[l] = (char)a; c->entry[l + 1] = 0;
        return;
    }
    switch (a) {
    case '.':
        if (!c->entering) { strlcpy(c->entry, "0", sizeof c->entry); c->entering = 1; if (!c->op) c->last_op = 0; }
        if (!strchr(c->entry, '.') && entry_digits(c->entry) < MAX_DIGITS) strlcat(c->entry, ".", sizeof c->entry);
        break;
    case A_BACK:
        if (c->entering) {
            size_t l = strlen(c->entry);
            if (l) c->entry[l - 1] = 0;
            if (!c->entry[0] || !strcmp(c->entry, "-")) strlcpy(c->entry, "0", sizeof c->entry);
        }
        break;
    case A_CE:
        c->entering = 0; c->cur = 0;
        break;
    case A_C:
        clear_all(c);
        break;
    case A_NEG:
        if (c->entering) {
            if (c->entry[0] == '-') memmove(c->entry, c->entry + 1, strlen(c->entry));
            else if (strcmp(c->entry, "0")) { memmove(c->entry + 1, c->entry, strlen(c->entry) + 1); c->entry[0] = '-'; }
        } else {
            c->cur = -c->cur;
        }
        break;
    case '+': case '-': case '*': case '/': {
        double v = cur_value(c);
        if (c->op && c->entering) {
            double r;
            if (!apply_op(c, c->acc, c->op, v, &r)) return;
            c->acc = r;
        } else if (!c->op) {
            c->acc = v;
        }
        /* an operator right after another one just replaces it */
        c->op = a;
        c->entering = 0;
        c->cur = c->acc;
        c->last_op = 0;
        update_expr(c);
        break;
    }
    case '=': {
        double v = cur_value(c), r;
        if (c->op) {
            if (!apply_op(c, c->acc, c->op, v, &r)) return;
            c->last_op = c->op; c->last_val = v;
            c->op = 0;
            set_result(c, r);
        } else if (c->last_op) {
            if (!apply_op(c, v, c->last_op, c->last_val, &r)) return;
            set_result(c, r);
        } else {
            set_result(c, v);
        }
        c->expr[0] = 0;
        break;
    }
    case A_SQRT: {
        double v = cur_value(c);
        if (v < 0) { set_error(c, "Invalid input"); return; }
        set_result(c, c_sqrt(v));
        break;
    }
    case A_RECIP: {
        double v = cur_value(c);
        if (v == 0) { set_error(c, "Cannot divide by zero"); return; }
        set_result(c, 1.0 / v);
        break;
    }
    case A_PCT: {
        double v = cur_value(c);
        if (c->op == '+' || c->op == '-') set_result(c, c->acc * v / 100.0);
        else set_result(c, v / 100.0);
        break;
    }
    case A_MC: c->mem = 0; break;
    case A_MR: set_result(c, c->mem); break;
    case A_MS: c->mem = cur_value(c); c->entering = 0; c->cur = c->mem; break;
    case A_MPLUS: c->mem += cur_value(c); if (c->entering) set_result(c, cur_value(c)); break;
    case A_MMINUS: c->mem -= cur_value(c); if (c->entering) set_result(c, cur_value(c)); break;
    }
    if (is_bad(c->mem) || c_fabs(c->mem) > 1e300) c->mem = 0;
}

static void display_text(calc_t *c, char *out, int outsz) {
    if (c->error) strlcpy(out, c->errmsg, (size_t)outsz);
    else if (c->entering) strlcpy(out, c->entry, (size_t)outsz);
    else fmt_num(c->cur, out, outsz);
}

/* ---------------- drawing ---------------- */
static int content_top(void) { return BAR_H + 10; }

static rc_t btn_rect(int i) {
    const cbtn_t *b = &buttons[i];
    int y0 = content_top() + DISP_H + 10;
    return R(MARGIN + b->col * (BW + GAP), y0 + b->row * (BH + GAP),
             b->cs * BW + (b->cs - 1) * GAP, b->rs * BH + (b->rs - 1) * GAP);
}

static rc_t bar_rect(int i) {
    int x = 2, xr = pw - 2;
    for (int j = 0; j < NBAR; j++) {
        int iw = gfx_text_width(bar[j].label, FONT_UI) + 16;
        if (bar[j].right) xr -= iw;
        if (j == i) return R(bar[j].right ? xr : x, 1, iw, BAR_H - 3);
        if (!bar[j].right) x += iw;
    }
    return R(0, 0, 0, 0);
}

static void draw_glyph(rc_t r, int action, unsigned c) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (action == A_BACK) {
        /* left arrow */
        for (int i = 0; i < 2; i++) hline(cx - 6, cy + i - 1, 13, c);
        for (int i = 0; i < 5; i++) vline(cx - 6 + i, cy - i - 1, 2 * i + 2, c);
    } else if (action == A_SQRT) {
        /* radical sign followed by x */
        int x = cx - 11;
        line(x, cy, x + 2, cy - 1, c);
        line(x + 2, cy - 1, x + 5, cy + 6, c);
        line(x + 3, cy - 1, x + 6, cy + 6, c);
        line(x + 6, cy + 6, x + 10, cy - 7, c);
        hline(x + 10, cy - 7, 13, c);
        text_at(x + 12, cy - 6, "x", c, FONT_UI_BOLD);
    }
}

/* the command strip, drawn like the kernel's menu bar */
static void draw_bar(calc_t *c) {
    fill(R(0, 0, pw, BAR_H), TH_FACE);
    hline(0, BAR_H - 1, pw, lighten(TH_FACE, -25));
    for (int i = 0; i < NBAR; i++) {
        rc_t r = bar_rect(i);
        int act = c->bar_pressed == i && c->bar_hover == i, hov = c->bar_hover == i || c->bar_pressed == i;
        if (act) fill(r, TH_SELECT);
        else if (hov) {
            hline(r.x, r.y, r.w, lighten(TH_SELECT, 60)); hline(r.x, r.y + r.h - 1, r.w, lighten(TH_SELECT, 60));
            vline(r.x, r.y, r.h, lighten(TH_SELECT, 60)); vline(r.x + r.w - 1, r.y, r.h, lighten(TH_SELECT, 60));
        }
        text_box(r, bar[i].label, act ? TH_SELECT_TEXT : 0x000000, FONT_UI, 1);
    }
}

static void paint(void) {
    calc_t *c = &calc;
    int cw = pw;
    int top = content_top();
    clip = R(0, 0, pw, ph);
    draw_bar(c);
    /* subtle background gradient */
    grad_v(R(0, BAR_H, cw, ph - BAR_H), lighten(TH_FACE, 50), TH_FACE);

    /* display */
    rc_t d = R(MARGIN, top, cw - 2 * MARGIN, DISP_H);
    rounded(d, 5, 0xFFFFFF, lighten(TH_SIDEBAR_TOP, 170));
    rounded_outline(d, 5, lighten(TH_BUTTON_BORDER, 20));
    hline(d.x + 4, d.y + 1, d.w - 8, lighten(TH_SIDEBAR_TOP, 120));
    if (c->expr[0]) text_box(R(d.x + 24, d.y + 6, d.w - 34, 16), c->expr, 0x6D7380, FONT_UI, 2);
    if (c->mem != 0) {
        rounded(R(d.x + 7, d.y + 7, 16, 15), 3, lighten(TH_SELECT, 40), TH_SELECT);
        text_box(R(d.x + 7, d.y + 7, 16, 15), "M", 0xFFFFFF, FONT_SMALL, 1);
    }
    char txt[64];
    display_text(c, txt, sizeof txt);
    int f = FONT_LARGE, avail = d.w - 18;
    if (gfx_text_width(txt, f) > avail) f = FONT_TITLE;
    if (gfx_text_width(txt, f) > avail) f = FONT_UI;
    text_box(R(d.x + 8, d.y + 26, d.w - 18, 32), txt, c->error ? 0xB0301C : 0x101828, f, 2);

    /* buttons */
    for (int i = 0; i < NBTN; i++) {
        const cbtn_t *b = &buttons[i];
        rc_t r = btn_rect(i);
        int hov = c->hover == i, prs = (c->pressed == i && c->hover == i) || c->flash == i;
        unsigned t, bt, tc = 0x101828;
        switch (b->kind) {
        case K_DIGIT: t = 0xFFFFFF; bt = 0xE6E8EE; break;
        case K_OP: t = lighten(TH_SIDEBAR_TOP, 150); bt = lighten(TH_SIDEBAR_TOP, 85); tc = lighten(TH_MENU_HDR_BOT, -60); break;
        case K_FUNC: t = lighten(TH_SIDEBAR_TOP, 165); bt = lighten(TH_SIDEBAR_TOP, 115); tc = lighten(TH_MENU_HDR_BOT, -60); break;
        case K_MEM: t = 0xF8F7F2; bt = 0xE4E2D8; tc = 0x4A4F5C; break;
        case K_CLEAR: t = 0xFFF4EE; bt = 0xF4DCD0; tc = 0xB0301C; break;
        default: t = lighten(TH_HOVER, 45); bt = lighten(TH_HOVER, -15); tc = 0xFFFFFF; break;
        }
        if (hov && !prs) { t = lighten(t, 40); bt = lighten(bt, 30); }
        if (prs) { unsigned x = lighten(bt, -18); bt = lighten(t, -8); t = x; }
        rounded(r, 4, t, bt);
        rounded_outline(r, 4, hov ? lighten(TH_HOVER, -20) : lighten(TH_BUTTON_BORDER, 25));
        if (!prs) hline(r.x + 3, r.y + 1, r.w - 6, lighten(t, 70));
        rc_t lr = r;
        if (prs) { lr.x++; lr.y++; }
        if (b->action == A_BACK || b->action == A_SQRT) draw_glyph(lr, b->action, tc);
        else {
            int bf = b->kind == K_MEM ? FONT_UI : (b->kind == K_DIGIT || b->kind == K_EQUALS || b->kind == K_OP) ? FONT_TITLE : FONT_UI_BOLD;
            if (b->kind == K_EQUALS) text_box(R(lr.x + 1, lr.y + 1, lr.w, lr.h), b->label, lighten(TH_HOVER, -60), FONT_LARGE, 1);
            text_box(lr, b->label, tc, b->kind == K_EQUALS ? FONT_LARGE : bf, 1);
        }
    }
    present(win);
}

/* ---------------- input ---------------- */
static int btn_at(int x, int y) {
    for (int i = 0; i < NBTN; i++) if (rc_has(btn_rect(i), x, y)) return i;
    return -1;
}
static int bar_at(int x, int y) {
    for (int i = 0; i < NBAR; i++) if (rc_has(bar_rect(i), x, y)) return i;
    return -1;
}

static void flash_action(int a) {
    for (int i = 0; i < NBTN; i++) if (buttons[i].action == a) { calc.flash = i; win_set_timer(win, 120); }
}

static void calc_copy(void) {
    char t[64];
    display_text(&calc, t, sizeof t);
    clipboard_set(t);
}

static int char_action(unsigned ch) {
    if (ch >= '0' && ch <= '9') return (int)ch;
    switch (ch) {
    case '.': case ',': return '.';
    case '+': case '-': case '*': case '/': case '=': return (int)ch;
    case 'x': case 'X': return '*';
    case '%': return A_PCT;
    case '@': return A_SQRT;
    case 'r': case 'R': return A_RECIP;
    case '\n': return '=';
    }
    return 0;
}

static void calc_paste(void) {
    char t[130];
    int len = clipboard_get(t, sizeof t);
    for (int i = 0; i < len && i < 128; i++) {
        int a = char_action((unsigned char)t[i]);
        if (t[i] == 'e' || t[i] == 'E') break;
        if (a) calc_action(&calc, a);
    }
}

static void calc_help(void) {
    msgbox(win, "About Calculator", "Halcyon Calculator 1.0\n\nKeyboard: digits, + - * /, Enter (=), "
           "Esc (C), Del (CE), Backspace, % , @ (square root), R (1/x), F9 (sign).\n\n"
           "Ctrl+C copies the display and Ctrl+V pastes a number.\n"
           "Memory keys: Ctrl+L, Ctrl+R, Ctrl+M, Ctrl+P and Ctrl+Q.", MBX_ICON_INFO, MBX_OK);
}

static void command(int id) {
    if (id == CM_COPY) calc_copy();
    else if (id == CM_PASTE) calc_paste();
    else if (id == CM_HELP) calc_help();
}

/* returns 1 if the window needs repainting */
static int handle(hal_event_t *e) {
    calc_t *c = &calc;
    switch (e->type) {
    case EVT_PAINT: case EVT_RESIZE: return 1;
    case EVT_MOUSE_MOVE: case EVT_MOUSE_LEAVE: {
        int out = e->type == EVT_MOUSE_LEAVE;
        int h = out ? -1 : btn_at(e->x, e->y), bh = out ? -1 : bar_at(e->x, e->y);
        if (h == c->hover && bh == c->bar_hover) return 0;
        c->hover = h; c->bar_hover = bh;
        return 1;
    }
    case EVT_MOUSE_DOWN: case EVT_DBLCLICK:
        if (e->button != MOUSE_LEFT) return 0;
        c->bar_pressed = c->bar_hover = bar_at(e->x, e->y);
        c->pressed = btn_at(e->x, e->y);
        c->hover = c->pressed;
        return 1;
    case EVT_MOUSE_UP:
        if (e->button != MOUSE_LEFT) return 0;
        if (c->bar_pressed >= 0) {
            int p = c->bar_pressed;
            c->bar_pressed = -1;
            paint();
            if (bar_at(e->x, e->y) == p) command(bar[p].id);
            return 1;
        }
        {
            int p = c->pressed;
            c->pressed = -1;
            if (p >= 0 && btn_at(e->x, e->y) == p) calc_action(c, buttons[p].action);
        }
        return 1;
    case EVT_KEY: {
        if (!e->pressed) return 0;
        int a = 0;
        if (e->mods & MOD_CTRL) {
            switch (e->id) {
            case KEY_A + 2: calc_copy(); return 0;        /* ^C */
            case KEY_A + 21: calc_paste(); return 1;      /* ^V */
            case KEY_A + 11: a = A_MC; break;             /* ^L */
            case KEY_A + 17: a = A_MR; break;             /* ^R */
            case KEY_A + 12: a = A_MS; break;             /* ^M */
            case KEY_A + 15: a = A_MPLUS; break;          /* ^P */
            case KEY_A + 16: a = A_MMINUS; break;         /* ^Q */
            default: return 0;
            }
        } else if (e->id == KEY_ENTER || e->id == KEY_KP_ENTER) a = '=';
        else if (e->id == KEY_ESC) a = A_C;
        else if (e->id == KEY_DELETE) a = A_CE;
        else if (e->id == KEY_BACKSPACE) a = A_BACK;
        else if (e->id == KEY_F9) a = A_NEG;
        else if (e->id == KEY_F1) { calc_help(); return 0; }
        else a = char_action(e->ch);
        if (!a) return 0;
        flash_action(a);
        calc_action(c, a);
        return 1;
    }
    case EVT_TIMER:
        win_set_timer(win, 0);
        if (c->flash < 0) return 0;
        c->flash = -1;
        return 1;
    case EVT_FOCUS:
        if (!e->pressed && (c->hover >= 0 || c->bar_hover >= 0) && c->pressed < 0 && c->bar_pressed < 0) {
            c->hover = c->bar_hover = -1;
            return 1;
        }
        return 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    pw = 2 * MARGIN + COLS * BW + (COLS - 1) * GAP;
    ph = BAR_H + 10 + DISP_H + 10 + ROWS * BH + (ROWS - 1) * GAP + MARGIN;
    px = malloc((size_t)pw * (size_t)ph * 4);
    if (!px) return 1;
    calc.hover = calc.pressed = calc.flash = calc.bar_hover = calc.bar_pressed = -1;
    clear_all(&calc);
    win = win_create("Calculator", pw, ph, WIN_NO_MAX);
    if (win < 0) { printf("Calculator needs the desktop (%d)\n", win); return 1; }
    win_set_icon(win, ICON_CALC);
    int dirty = 0;
    for (;;) {
        hal_event_t ev;
        /* handle everything queued, then repaint once */
        if (!win_get_event(&ev, dirty ? 0 : -1)) {
            if (dirty) { paint(); dirty = 0; }
            continue;
        }
        if (ev.type == EVT_CLOSE) break;
        dirty |= handle(&ev);
    }
    win_destroy(win);
    return 0;
}
