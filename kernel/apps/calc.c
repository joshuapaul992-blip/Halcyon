/* Halcyon OS - Calculator: standard immediate-execution calculator with
 * memory, keyboard input and its own number formatting (no %g in the
 * kernel's snprintf). */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/kmath.h"
#include "version.h"

#define COLS 5
#define ROWS 6
#define BW 46
#define BH 32
#define GAP 6
#define MARGIN 12
#define DISP_H 64
#define MAX_DIGITS 16

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
#define NBTN ((int)ARRAY_SIZE(buttons))

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
    u64 flash_until;
} calc_t;

static window_t *calc_win;

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
    if (is_bad(v)) { strlcpy(out, "Overflow", outsz); return; }
    if (v == 0) { strlcpy(out, "0", outsz); return; }
    if (v < 0) { buf[p++] = '-'; v = -v; }
    union { double d; u64 u; } bits = { .d = v };
    int be = (int)((bits.u >> 52) & 0x7FF) - 1023;
    int e = (int)k_floor(be * 0.30102999566398);
    double s = scale10(v, 14 - e);
    for (int i = 0; i < 40 && (s >= 1e15 || s < 1e14); i++) {
        if (s >= 1e15) e++; else e--;
        s = scale10(v, 14 - e);
    }
    u64 n = (u64)(s + 0.5);
    if (n >= 1000000000000000ull) { n = 100000000000000ull; e++; }
    char d[16];
    for (int i = 14; i >= 0; i--) { d[i] = (char)('0' + n % 10); n /= 10; }
    int nd = 15;
    while (nd > 1 && d[nd - 1] == '0') nd--;
    if (e >= 16 || e < -7) {
        buf[p++] = d[0];
        if (nd > 1) { buf[p++] = '.'; for (int i = 1; i < nd; i++) buf[p++] = d[i]; }
        buf[p] = 0;
        snprintf(buf + p, sizeof buf - p, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
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
    strlcpy(out, buf, outsz);
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
    if (is_bad(v) || k_fabs(v) > 1e300) { set_error(c, "Overflow"); return; }
    if (k_fabs(v) < 1e-300) v = 0;
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
    if (is_bad(*out) || k_fabs(*out) > 1e300) { set_error(c, "Overflow"); return 0; }
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
        set_result(c, k_sqrt(v));
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
    if (is_bad(c->mem) || k_fabs(c->mem) > 1e300) c->mem = 0;
}

static void display_text(calc_t *c, char *out, int outsz) {
    if (c->error) strlcpy(out, c->errmsg, outsz);
    else if (c->entering) strlcpy(out, c->entry, outsz);
    else fmt_num(c->cur, out, outsz);
}

/* ---------------- drawing ---------------- */
static int content_top(window_t *w) { return menubar_height(w) + 10; }

static rect_t btn_rect(window_t *w, int i) {
    const cbtn_t *b = &buttons[i];
    int y0 = content_top(w) + DISP_H + 10;
    return mkrect(MARGIN + b->col * (BW + GAP), y0 + b->row * (BH + GAP),
                  b->cs * BW + (b->cs - 1) * GAP, b->rs * BH + (b->rs - 1) * GAP);
}

static void draw_glyph(gc_t *g, rect_t r, int action, color_t c) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (action == A_BACK) {
        /* left arrow */
        for (int i = 0; i < 2; i++) gfx_hline(g, cx - 6, cy + i - 1, 13, c);
        for (int i = 0; i < 5; i++) {
            gfx_vline(g, cx - 6 + i, cy - i - 1, 2 * i + 2, c);
        }
    } else if (action == A_SQRT) {
        /* radical sign followed by x */
        int x = cx - 11;
        gfx_line(g, x, cy, x + 2, cy - 1, c);
        gfx_line(g, x + 2, cy - 1, x + 5, cy + 6, c);
        gfx_line(g, x + 3, cy - 1, x + 6, cy + 6, c);
        gfx_line(g, x + 6, cy + 6, x + 10, cy - 7, c);
        gfx_hline(g, x + 10, cy - 7, 13, c);
        gfx_text(g, &font_ui_bold, x + 12, cy - 6, "x", c);
    }
}

static void calc_paint(window_t *w, gc_t *g) {
    calc_t *c = w->data;
    const theme_t *th = g_theme;
    int cw = w->client->w;
    int top = content_top(w);
    /* subtle background gradient */
    gfx_gradient_v(g, mkrect(0, menubar_height(w), cw, w->client->h - menubar_height(w)),
                   color_lighten(th->face, 50), th->face);

    /* display */
    rect_t d = mkrect(MARGIN, top, cw - 2 * MARGIN, DISP_H);
    gfx_rounded(g, d, 5, 15, HEX(0xFFFFFF), color_lighten(th->sidebar_top, 170));
    gfx_rounded_outline(g, d, 5, 15, color_lighten(th->button_border, 20));
    gfx_hline(g, d.x + 4, d.y + 1, d.w - 8, color_lighten(th->sidebar_top, 120));
    gfx_text_box(g, &font_ui, mkrect(d.x + 24, d.y + 6, d.w - 34, 16), c->expr, HEX(0x6D7380), 2);
    if (c->mem != 0) {
        gfx_rounded(g, mkrect(d.x + 7, d.y + 7, 16, 15), 3, 15, color_lighten(th->select, 40), th->select);
        gfx_text_box(g, &font_small, mkrect(d.x + 7, d.y + 7, 16, 15), "M", HEX(0xFFFFFF), 1);
    }
    char txt[64];
    display_text(c, txt, sizeof txt);
    const font_t *f = &font_large;
    int avail = d.w - 18;
    if (gfx_text_width(f, txt) > avail) f = &font_title;
    if (gfx_text_width(f, txt) > avail) f = &font_ui;
    rect_t vr = mkrect(d.x + 8, d.y + 26, d.w - 18, 32);
    gfx_text_box(g, f, vr, txt, c->error ? HEX(0xB0301C) : HEX(0x101828), 2);

    /* buttons */
    for (int i = 0; i < NBTN; i++) {
        const cbtn_t *b = &buttons[i];
        rect_t r = btn_rect(w, i);
        int hov = c->hover == i, prs = (c->pressed == i && c->hover == i) || c->flash == i;
        color_t t, bt, tc = HEX(0x101828);
        switch (b->kind) {
        case K_DIGIT: t = HEX(0xFFFFFF); bt = HEX(0xE6E8EE); break;
        case K_OP: t = color_lighten(th->sidebar_top, 150); bt = color_lighten(th->sidebar_top, 85); tc = color_lighten(th->menu_header_bottom, -60); break;
        case K_FUNC: t = color_lighten(th->sidebar_top, 165); bt = color_lighten(th->sidebar_top, 115); tc = color_lighten(th->menu_header_bottom, -60); break;
        case K_MEM: t = HEX(0xF8F7F2); bt = HEX(0xE4E2D8); tc = HEX(0x4A4F5C); break;
        case K_CLEAR: t = HEX(0xFFF4EE); bt = HEX(0xF4DCD0); tc = HEX(0xB0301C); break;
        default: t = color_lighten(th->hover, 45); bt = color_lighten(th->hover, -15); tc = HEX(0xFFFFFF); break;
        }
        if (hov && !prs) { t = color_lighten(t, 40); bt = color_lighten(bt, 30); }
        if (prs) { color_t x = color_lighten(bt, -18); bt = color_lighten(t, -8); t = x; }
        gfx_rounded(g, r, 4, 15, t, bt);
        gfx_rounded_outline(g, r, 4, 15, hov ? color_lighten(th->hover, -20) : color_lighten(th->button_border, 25));
        if (!prs) gfx_hline(g, r.x + 3, r.y + 1, r.w - 6, color_lighten(t, 70));
        rect_t lr = r;
        if (prs) { lr.x++; lr.y++; }
        if (b->action == A_BACK || b->action == A_SQRT) draw_glyph(g, lr, b->action, tc);
        else {
            const font_t *bf = b->kind == K_MEM ? &font_ui : (b->kind == K_DIGIT || b->kind == K_EQUALS || b->kind == K_OP) ? &font_title : &font_ui_bold;
            if (b->kind == K_EQUALS) gfx_text_box(g, &font_large, mkrect(lr.x + 1, lr.y + 1, lr.w, lr.h), b->label, color_lighten(th->hover, -60), 1);
            gfx_text_box(g, b->kind == K_EQUALS ? &font_large : bf, lr, b->label, tc, 1);
        }
    }
}

/* ---------------- input ---------------- */
static int btn_at(window_t *w, int x, int y) {
    for (int i = 0; i < NBTN; i++) if (rect_contains(btn_rect(w, i), x, y)) return i;
    return -1;
}

static void flash_action(window_t *w, int a) {
    calc_t *c = w->data;
    for (int i = 0; i < NBTN; i++) if (buttons[i].action == a) { c->flash = i; c->flash_until = time_ms() + 120; }
}

static void do_action(window_t *w, int a) {
    calc_action(w->data, a);
    wm_invalidate(w);
}

static void calc_copy(window_t *w) {
    calc_t *c = w->data;
    char t[64];
    display_text(c, t, sizeof t);
    clipboard_set(t, (int)strlen(t));
}

static int char_action(char ch) {
    if (ch >= '0' && ch <= '9') return ch;
    switch (ch) {
    case '.': case ',': return '.';
    case '+': case '-': case '*': case '/': case '=': return ch;
    case 'x': case 'X': return '*';
    case '%': return A_PCT;
    case '@': return A_SQRT;
    case 'r': case 'R': return A_RECIP;
    case '\n': return '=';
    }
    return 0;
}

static void calc_paste(window_t *w) {
    int len;
    const char *t = clipboard_get(&len);
    if (!t) return;
    for (int i = 0; i < len && i < 128; i++) {
        int a = char_action(t[i]);
        if (t[i] == 'e' || t[i] == 'E') break;
        if (a) calc_action(w->data, a);
    }
    wm_invalidate(w);
}

static void calc_event(window_t *w, gui_event_t *e) {
    calc_t *c = w->data;
    switch (e->type) {
    case GE_MOUSE_MOVE: case GE_MOUSE_LEAVE: {
        int h = e->type == GE_MOUSE_LEAVE ? -1 : btn_at(w, e->x, e->y);
        if (h != c->hover) { c->hover = h; wm_invalidate(w); }
        break;
    }
    case GE_MOUSE_DOWN: case GE_DBLCLICK:
        if (e->button == MOUSE_LEFT) { c->pressed = btn_at(w, e->x, e->y); c->hover = c->pressed; wm_invalidate(w); }
        break;
    case GE_MOUSE_UP:
        if (e->button == MOUSE_LEFT) {
            int p = c->pressed;
            c->pressed = -1;
            if (p >= 0 && btn_at(w, e->x, e->y) == p) calc_action(c, buttons[p].action);
            wm_invalidate(w);
        }
        break;
    case GE_KEY: {
        input_event_t *k = &e->key;
        if (!k->pressed) break;
        int a = 0;
        if ((k->mods & MOD_CTRL) && !key_is_text(k)) {
            /* shortcuts by the letter on the keyboard layout */
            if (key_shortcut(k, 'C')) { calc_copy(w); return; }
            if (key_shortcut(k, 'V')) { calc_paste(w); return; }
            if (key_shortcut(k, 'L')) a = A_MC;
            else if (key_shortcut(k, 'R')) a = A_MR;
            else if (key_shortcut(k, 'M')) a = A_MS;
            else if (key_shortcut(k, 'P')) a = A_MPLUS;
            else if (key_shortcut(k, 'Q')) a = A_MMINUS;
            else return;
        } else if (k->key == KEY_ENTER || k->key == KEY_KP_ENTER) a = '=';
        else if (k->key == KEY_ESC) a = A_C;
        else if (k->key == KEY_DELETE) a = A_CE;
        else if (k->key == KEY_BACKSPACE) a = A_BACK;
        else if (k->key == KEY_F9) a = A_NEG;
        else a = k->ch < 128 ? char_action((char)k->ch) : 0;
        if (a) { flash_action(w, a); do_action(w, a); }
        break;
    }
    }
}

static void calc_tick(window_t *w) {
    calc_t *c = w->data;
    if (c->flash >= 0 && time_ms() >= c->flash_until) { c->flash = -1; wm_invalidate(w); }
}

enum { CM_COPY = 1, CM_PASTE, CM_ABOUT };
static menu_item_t calc_edit_items[] = {
    { "Copy", "Ctrl+C", CM_COPY, 0, 0 },
    { "Paste", "Ctrl+V", CM_PASTE, 0, 0 },
};
static menu_item_t calc_help_items[] = {
    { "About Calculator", NULL, CM_ABOUT, 0, ICON_CALC },
};
static menu_t calc_menus[] = {
    { "Edit", calc_edit_items, ARRAY_SIZE(calc_edit_items) },
    { "Help", calc_help_items, ARRAY_SIZE(calc_help_items) },
};

static void calc_menu(window_t *w, int id) {
    if (id == CM_COPY) calc_copy(w);
    else if (id == CM_PASTE) calc_paste(w);
    else if (id == CM_ABOUT)
        msgbox(w, "About Calculator", "Halcyon Calculator " OS_VERSION "\n\nKeyboard: digits, + - * /, Enter (=), "
               "Esc (C), Del (CE), Backspace, % , @ (square root), R (1/x), F9 (sign).", ICON_CALC, MB_OK);
}

static void calc_destroy(window_t *w) {
    kfree(w->data);
    w->data = NULL;
    if (calc_win == w) calc_win = NULL;
}

void app_calc(void) {
    if (calc_win && wm_alive(calc_win)) { wm_show(calc_win); return; }
    int cw = 2 * MARGIN + COLS * BW + (COLS - 1) * GAP;
    int ch = MENUBAR_H + 10 + DISP_H + 10 + ROWS * BH + (ROWS - 1) * GAP + MARGIN;
    calc_t *c = kzalloc(sizeof *c);
    if (!c) return;
    c->hover = c->pressed = c->flash = -1;
    clear_all(c);
    window_t *w = wm_create("Calculator", ICON_CALC, -1, -1, cw, ch, WF_NO_MAXIMIZE);
    if (w) w->help = "calc";              /* F1 */
    w->data = c;
    w->on_paint = calc_paint;
    w->on_event = calc_event;
    w->on_tick = calc_tick;
    w->on_menu = calc_menu;
    w->on_destroy = calc_destroy;
    menubar_attach(w, calc_menus, ARRAY_SIZE(calc_menus));
    calc_win = w;
}
