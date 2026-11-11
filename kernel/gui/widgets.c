/* Halcyon OS - widgets: buttons, labels, text boxes, check boxes, list boxes */
#include "wm.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/mm.h"
#include "core/kernel.h"

#define LIST_ROW 18

widget_t *widget_add(window_t *w, int type, rect_t r, const char *text, int id) {
    widget_t *wd = kzalloc(sizeof *wd);
    wd->type = type;
    wd->r = r;
    wd->id = id;
    strlcpy(wd->text, text ? text : "", sizeof wd->text);
    if (type == W_TEXTBOX) { wd->cursor = wd->sel_anchor = (int)strlen(wd->text); }
    if (type == W_LISTBOX) wd->value = -1;
    wd->icon = -1;
    if (type == W_VSCROLL || type == W_HSCROLL) wd->line = 1;
    widget_t **pp = &w->widgets;
    while (*pp) pp = &(*pp)->next;
    *pp = wd;
    if (!w->focus && (type == W_TEXTBOX || type == W_LISTBOX)) w->focus = wd;
    wm_invalidate(w);
    return wd;
}

widget_t *widget_find(window_t *w, int id) {
    for (widget_t *wd = w->widgets; wd; wd = wd->next) if (wd->id == id) return wd;
    return NULL;
}

void widget_set_text(window_t *w, widget_t *wd, const char *t) {
    strlcpy(wd->text, t, sizeof wd->text);
    if (wd->type == W_TEXTBOX) { wd->cursor = wd->sel_anchor = (int)strlen(wd->text); wd->scroll = 0; }
    wm_invalidate(w);
}

void widget_list_set(window_t *w, widget_t *wd, char **items, int n) {
    for (int i = 0; i < wd->nitems; i++) kfree(wd->items[i]);
    kfree(wd->items);
    wd->items = n ? kzalloc(sizeof(char *) * n) : NULL;
    for (int i = 0; i < n; i++) wd->items[i] = strdup(items[i]);
    wd->nitems = n;
    wd->top = 0;
    wd->value = n ? 0 : -1;
    wm_invalidate(w);
}

void widget_list_add(window_t *w, widget_t *wd, const char *item) {
    char **ni = kzalloc(sizeof(char *) * (wd->nitems + 1));
    for (int i = 0; i < wd->nitems; i++) ni[i] = wd->items[i];
    ni[wd->nitems] = strdup(item);
    kfree(wd->items);
    wd->items = ni;
    wd->nitems++;
    wm_invalidate(w);
}

/* ---------------- drawing ---------------- */
void draw_button(gc_t *g, rect_t r, const char *text, int state, int is_default) {
    color_t top = HEX(0xFFFFFF), bot = HEX(0xE3E1D8);
    if (state == 2) { top = HEX(0xE0DED4); bot = HEX(0xF4F3EE); }
    if (state == 3) { top = HEX(0xF4F3EE); bot = HEX(0xF4F3EE); }
    gfx_rounded(g, r, 3, 15, top, bot);
    color_t border = state == 3 ? HEX(0xC9C7BA) : g_theme->button_border;
    gfx_rounded_outline(g, r, 3, 15, border);
    if (state == 1) {
        /* warm hover glow */
        gfx_hline(g, r.x + 2, r.y + 1, r.w - 4, color_lighten(g_theme->hover, 60));
        gfx_hline(g, r.x + 1, r.y + 2, r.w - 2, g_theme->hover);
        gfx_hline(g, r.x + 2, r.y + r.h - 2, r.w - 4, color_lighten(g_theme->hover, -30));
        gfx_vline(g, r.x + 1, r.y + 2, r.h - 4, color_lighten(g_theme->hover, 20));
        gfx_vline(g, r.x + r.w - 2, r.y + 2, r.h - 4, color_lighten(g_theme->hover, 20));
    } else if (is_default && state != 3) {
        color_t d = HEX(0x8DB2E3);
        gfx_hline(g, r.x + 2, r.y + 1, r.w - 4, HEX(0xCEE7FF));
        gfx_hline(g, r.x + 2, r.y + r.h - 2, r.w - 4, HEX(0x6982EE));
        gfx_vline(g, r.x + 1, r.y + 2, r.h - 4, d);
        gfx_vline(g, r.x + r.w - 2, r.y + 2, r.h - 4, d);
    }
    if (text) {
        rect_t tr = r;
        if (state == 2) { tr.x++; tr.y++; }
        gfx_text_box(g, &font_ui, tr, text, state == 3 ? HEX(0xA19F94) : HEX(0x000000), 1);
    }
}

void draw_scrollbar(gc_t *g, rect_t r, int total, int visible, int pos, int vertical) {
    gfx_fill(g, r, HEX(0xF3F1EA));
    int len = vertical ? r.h : r.w, th = vertical ? r.w : r.h;
    rect_t a1 = vertical ? mkrect(r.x, r.y, r.w, th) : mkrect(r.x, r.y, th, r.h);
    rect_t a2 = vertical ? mkrect(r.x, r.y + r.h - th, r.w, th) : mkrect(r.x + r.w - th, r.y, th, r.h);
    color_t bt = color_lighten(g_theme->sidebar_top, 140), bb = color_lighten(g_theme->sidebar_bottom, 60);
    gfx_rounded(g, rect_inset(a1, 1), 3, 15, bt, bb);
    gfx_rounded_outline(g, rect_inset(a1, 1), 3, 15, color_lighten(g_theme->sidebar_bottom, 20));
    gfx_rounded(g, rect_inset(a2, 1), 3, 15, bt, bb);
    gfx_rounded_outline(g, rect_inset(a2, 1), 3, 15, color_lighten(g_theme->sidebar_bottom, 20));
    color_t ac = HEX(0x4D6185);
    int cx1 = a1.x + a1.w / 2, cy1 = a1.y + a1.h / 2, cx2 = a2.x + a2.w / 2, cy2 = a2.y + a2.h / 2;
    for (int i = 0; i < 4; i++) {
        if (vertical) {
            gfx_hline(g, cx1 - i, cy1 - 2 + i, 2 * i + 1, ac);
            gfx_hline(g, cx2 - i, cy2 + 2 - i, 2 * i + 1, ac);
        } else {
            gfx_vline(g, cx1 - 2 + i, cy1 - i, 2 * i + 1, ac);
            gfx_vline(g, cx2 + 2 - i, cy2 - i, 2 * i + 1, ac);
        }
    }
    int track = len - 2 * th;
    if (total <= visible || track < 8) return;
    int tlen = (int)MAX(16, (i64)track * visible / total);
    int tpos = (int)((i64)(track - tlen) * pos / MAX(1, total - visible));
    rect_t t = vertical ? mkrect(r.x + 1, r.y + th + tpos, r.w - 2, tlen) : mkrect(r.x + th + tpos, r.y + 1, tlen, r.h - 2);
    if (vertical) gfx_rounded(g, t, 3, 15, bt, bb);
    else gfx_rounded(g, t, 3, 15, bt, bb);
    gfx_rounded_outline(g, t, 3, 15, color_lighten(g_theme->sidebar_bottom, 20));
    /* grip */
    for (int i = -2; i <= 2; i += 2) {
        if (vertical) gfx_hline(g, t.x + t.w / 2 - 3, t.y + t.h / 2 + i, 7, color_lighten(g_theme->sidebar_bottom, 60));
        else gfx_vline(g, t.x + t.w / 2 + i, t.y + t.h / 2 - 3, 7, color_lighten(g_theme->sidebar_bottom, 60));
    }
}

int scrollbar_hit(rect_t r, int total, int visible, int pos, int x, int y, int vertical, int *part) {
    int len = vertical ? r.h : r.w, th = vertical ? r.w : r.h;
    int p = vertical ? y - r.y : x - r.x;
    int maxpos = MAX(0, total - visible);
    if (p < th) { *part = 1; return MAX(0, pos - 3); }
    if (p >= len - th) { *part = 2; return MIN(maxpos, pos + 3); }
    int track = len - 2 * th;
    int tlen = (int)MAX(16, (i64)track * visible / MAX(1, total));
    int tpos = (int)((i64)(track - tlen) * pos / MAX(1, maxpos));
    int rel = p - th;
    if (rel < tpos) { *part = 3; return MAX(0, pos - visible); }
    if (rel > tpos + tlen) { *part = 4; return MIN(maxpos, pos + visible); }
    *part = 5;   /* thumb */
    return pos;
}

void draw_group(gc_t *g, rect_t r, const char *title) {
    color_t c = HEX(0xD0D0BF);
    gfx_rounded_outline(g, mkrect(r.x, r.y + 7, r.w, r.h - 7), 4, 15, c);
    int tw = gfx_text_width(&font_ui, title);
    gfx_fill(g, mkrect(r.x + 8, r.y, tw + 6, 14), g_theme->face);
    gfx_text(g, &font_ui, r.x + 11, r.y + 1, title, HEX(0x0046D5));
}

static void textbox_paint(window_t *w, gc_t *g, widget_t *wd) {
    int dis = wd->style & WS_DISABLED;
    gfx_sunken_field(g, wd->r, dis || (wd->style & WS_READONLY) ? HEX(0xF4F3EE) : HEX(0xFFFFFF));
    rect_t in = rect_inset(wd->r, 3);
    gc_t sub = gc_sub(g, in);
    /* shown: the text, or one '*' per character of a password; map[] turns a
     * byte offset in the text into one in shown */
    char shown[sizeof wd->text];
    static int map[sizeof wd->text];
    int n = (int)strlen(wd->text);
    int pw = (wd->style & WS_PASSWORD) != 0, sn = 0;
    for (int i = 0; i < n;) {
        int nx = utf8_after(wd->text, n, i);
        for (int j = i; j < nx; j++) map[j] = sn;
        if (pw) shown[sn++] = '*';
        else while (i < nx) shown[sn++] = wd->text[i++];
        i = nx;
    }
    map[n] = sn;
    shown[sn] = 0;
    int cur = map[CLAMP(wd->cursor, 0, n)], anc = map[CLAMP(wd->sel_anchor, 0, n)];
    /* keep cursor visible */
    int cx = gfx_text_width_n(&font_ui, shown, cur);
    if (cx - wd->scroll > in.w - 2) wd->scroll = cx - in.w + 2;
    if (cx - wd->scroll < 0) wd->scroll = cx;
    int ty = (in.h - font_ui.height) / 2;
    int focused = w->focus == wd && wm_active() == w;
    int s0 = MIN(cur, anc), s1 = MAX(cur, anc);
    if (focused && s1 > s0) {
        int x0 = gfx_text_width_n(&font_ui, shown, s0) - wd->scroll;
        int x1 = gfx_text_width_n(&font_ui, shown, s1) - wd->scroll;
        gfx_fill(&sub, mkrect(x0, ty - 1, x1 - x0, font_ui.height + 2), g_theme->select);
    }
    int x = -wd->scroll;
    for (int i = 0; i < sn;) {
        int sel = focused && i >= s0 && i < s1;
        int nx = utf8_after(shown, sn, i);
        x = gfx_text_n(&sub, &font_ui, x, ty, shown + i, nx - i, dis ? HEX(0xA19F94) : sel ? g_theme->select_text : HEX(0x000000));
        i = nx;
    }
    if (focused) gfx_vline(&sub, cx - wd->scroll, ty - 1, font_ui.height + 2, HEX(0x000000));
}

static void listbox_paint(window_t *w, gc_t *g, widget_t *wd) {
    gfx_sunken_field(g, wd->r, HEX(0xFFFFFF));
    rect_t in = rect_inset(wd->r, 1);
    int rows = in.h / LIST_ROW;
    int need_sb = wd->nitems > rows;
    rect_t area = in;
    if (need_sb) area.w -= 17;
    gc_t sub = gc_sub(g, area);
    for (int i = 0; i < rows + 1 && wd->top + i < wd->nitems; i++) {
        int idx = wd->top + i;
        rect_t row = mkrect(0, i * LIST_ROW, area.w, LIST_ROW);
        int sel = idx == wd->value;
        if (sel) gfx_fill(&sub, row, w->focus == wd ? g_theme->select : HEX(0xD8D6CC));
        gfx_text_box(&sub, &font_ui, mkrect(4, row.y, area.w - 8, LIST_ROW), wd->items[idx], sel && w->focus == wd ? g_theme->select_text : HEX(0x000000), 0);
    }
    if (need_sb) draw_scrollbar(g, mkrect(in.x + in.w - 17, in.y, 17, in.h), wd->nitems, rows, wd->top, 1);
}

/* ---------------- scroll bars, tool buttons, status bars, tooltips ---------------- */
static int sb_maxpos(widget_t *wd) { return MAX(0, wd->total - wd->page); }

void widget_scroll_set(window_t *w, widget_t *wd, int total, int page, int pos, int line) {
    wd->total = MAX(0, total);
    wd->page = MAX(0, page);
    wd->line = MAX(1, line);
    wd->value = CLAMP(pos, 0, sb_maxpos(wd));
    wm_invalidate(w);
}

static void toolbutton_paint(gc_t *g, widget_t *wd) {
    rect_t r = wd->r;
    const theme_t *th = g_theme;
    int dis = (wd->style & WS_DISABLED) != 0;
    if (wd->value && !dis) {
        gfx_rounded(g, r, 3, 15, color_lighten(th->select, 120), color_lighten(th->select, 80));
        gfx_rounded_outline(g, r, 3, 15, th->select);
    } else if (wd->hover && !dis) {
        if (wd->pressed) gfx_rounded(g, r, 3, 15, color_lighten(th->face, -25), color_lighten(th->face, 10));
        else gfx_rounded(g, r, 3, 15, HEX(0xFFFFFF), color_lighten(th->face, -15));
        gfx_rounded_outline(g, r, 3, 15, color_lighten(th->face, -60));
    }
    int off = wd->pressed && wd->hover && !dis ? 1 : 0;
    int tw = wd->text[0] ? gfx_text_width(&font_ui, wd->text) : 0;
    int iw = wd->icon >= 0 ? 16 : 0;
    int cw = iw + (iw && tw ? 4 : 0) + tw;
    int x = r.x + MAX(4, (r.w - cw) / 2) + off;
    if (iw) {
        if (dis) gfx_icon_alpha(g, x, r.y + (r.h - 16) / 2 + off, &icons16[wd->icon], 90);
        else gfx_icon(g, x, r.y + (r.h - 16) / 2 + off, &icons16[wd->icon]);
        x += 20;
    }
    if (tw) gfx_text_box(g, &font_ui, mkrect(x, r.y + off, r.x + r.w - x - 2, r.h), wd->text, dis ? HEX(0xA0A0A0) : HEX(0x000000), 0);
}

/* parts separated by tabs: the first takes what the others (value px each, default 150) leave */
static void statusbar_paint(gc_t *g, widget_t *wd) {
    rect_t st = wd->r;
    const theme_t *th = g_theme;
    gfx_gradient_v(g, st, color_lighten(th->face, 20), color_lighten(th->face, -10));
    gfx_hline(g, st.x, st.y, st.w, color_lighten(th->face, -35));
    const char *parts[8];
    int lens[8], n = 0;
    const char *s = wd->text;
    while (n < 8) {
        const char *t = strchr(s, '\t');
        parts[n] = s;
        lens[n] = t ? (int)(t - s) : (int)strlen(s);
        n++;
        if (!t) break;
        s = t + 1;
    }
    int pw = wd->value > 0 ? wd->value : 150;
    int right = st.x + st.w;
    char buf[sizeof wd->text];
    for (int i = n - 1; i >= 0; i--) {
        int x = i ? right - pw : st.x;
        if (i && x < st.x + 60) continue;
        memcpy(buf, parts[i], (size_t)lens[i]);
        buf[lens[i]] = 0;
        gfx_text_box(g, &font_ui, mkrect(x + 8, st.y + 1, right - x - 12, st.h - 1), buf, HEX(0x303440), 0);
        if (i) {
            gfx_vline(g, x, st.y + 3, st.h - 6, color_lighten(th->face, -40));
            gfx_vline(g, x + 1, st.y + 3, st.h - 6, HEX(0xFFFFFF));
        }
        right = x;
    }
}

static widget_t *tip_wd;
static window_t *tip_win;
static u64 tip_since;
static int tip_shown;

static int widget_of(window_t *w, widget_t *x) {
    for (widget_t *wd = w->widgets; wd; wd = wd->next) if (wd == x) return 1;
    return 0;
}

static void tip_hide(void) {
    if (tip_shown && tip_win && wm_alive(tip_win)) wm_invalidate(tip_win);
    tip_wd = NULL; tip_win = NULL; tip_shown = 0;
}

static void tip_paint(window_t *w, gc_t *g) {
    if (!tip_shown || tip_win != w || !widget_of(w, tip_wd) || !w->client) return;
    widget_t *wd = tip_wd;
    int tw = MIN(gfx_text_width(&font_ui, wd->tip) + 10, w->client->w - 4);
    int h = font_ui.height + 6;
    int x = CLAMP(wd->r.x + 4, 2, MAX(2, w->client->w - tw - 2));
    int y = wd->r.y + wd->r.h + 3;
    if (y + h > w->client->h) y = MAX(0, wd->r.y - h - 3);
    rect_t r = mkrect(x, y, tw, h);
    gfx_fill(g, r, HEX(0xFFFFE1));
    gfx_rect(g, r, HEX(0x6E6E5A));
    gfx_text_box(g, &font_ui, mkrect(x + 5, y, tw - 8, h), wd->tip, HEX(0x000000), 0);
}

void widgets_paint(window_t *w, gc_t *g) {
    for (widget_t *wd = w->widgets; wd; wd = wd->next) {
        if (wd->style & WS_HIDDEN) continue;
        switch (wd->type) {
        case W_BUTTON: {
            int st = (wd->style & WS_DISABLED) ? 3 : (wd->pressed && wd->hover) ? 2 : wd->hover ? 1 : 0;
            draw_button(g, wd->r, wd->text, st, (wd->style & WS_DEFAULT) != 0);
            if (w->focus == wd) gfx_focus_rect(g, rect_inset(wd->r, 3));
            break;
        }
        case W_LABEL:
            if (wd->style & WS_WRAP) gfx_text_wrap(g, (wd->style & WS_BOLD) ? &font_ui_bold : &font_ui, wd->r, wd->text, HEX(0x000000));
            else gfx_text_box(g, (wd->style & WS_BOLD) ? &font_ui_bold : &font_ui, wd->r, wd->text, (wd->style & WS_DISABLED) ? HEX(0xA19F94) : HEX(0x000000), wd->value);
            break;
        case W_TEXTBOX: textbox_paint(w, g, wd); break;
        case W_CHECKBOX: {
            rect_t b = mkrect(wd->r.x, wd->r.y + (wd->r.h - 13) / 2, 13, 13);
            gfx_gradient_v(g, b, HEX(0xDCDCD7), HEX(0xFFFFFF));
            gfx_rect(g, b, HEX(0x1C5180));
            if (wd->hover) gfx_rect(g, rect_inset(b, 1), g_theme->hover);
            if (wd->value) {
                color_t c = HEX(0x21A121);
                for (int i = 0; i < 3; i++) {
                    gfx_line(g, b.x + 3, b.y + 5 + i, b.x + 5, b.y + 7 + i, c);
                    gfx_line(g, b.x + 5, b.y + 7 + i, b.x + 9, b.y + 3 + i, c);
                }
            }
            gfx_text_box(g, &font_ui, mkrect(wd->r.x + 19, wd->r.y, wd->r.w - 19, wd->r.h), wd->text, HEX(0x000000), 0);
            break;
        }
        case W_LISTBOX: listbox_paint(w, g, wd); break;
        case W_GROUP: draw_group(g, wd->r, wd->text); break;
        case W_PROGRESS: {
            gfx_sunken_field(g, wd->r, HEX(0xFFFFFF));
            rect_t in = rect_inset(wd->r, 2);
            int filled = in.w * CLAMP(wd->value, 0, 1000) / 1000;
            for (int x = 0; x + 8 <= filled; x += 10)
                gfx_gradient_v(g, mkrect(in.x + x, in.y, 8, in.h), HEX(0x8EE68A), HEX(0x2BAA2B));
            break;
        }
        case W_ICON:
            gfx_icon(g, wd->r.x, wd->r.y, wd->r.w >= 48 ? &icons48[wd->value] : &icons32[wd->value]);
            break;
        case W_VSCROLL: case W_HSCROLL:
            /* disabled: no thumb, like a bar with nothing to scroll */
            draw_scrollbar(g, wd->r, (wd->style & WS_DISABLED) ? 0 : wd->total, wd->page, wd->value, wd->type == W_VSCROLL);
            break;
        case W_TOOLBUTTON: toolbutton_paint(g, wd); break;
        case W_STATUSBAR: statusbar_paint(g, wd); break;
        }
    }
    tip_paint(w, g);
}

/* ---------------- events ---------------- */
static int focusable(widget_t *wd) {
    return !(wd->style & (WS_DISABLED | WS_HIDDEN)) && (wd->type == W_BUTTON || wd->type == W_TEXTBOX || wd->type == W_CHECKBOX || wd->type == W_LISTBOX);
}

static void focus_next(window_t *w, int back) {
    widget_t *list[64]; int n = 0, cur = -1;
    for (widget_t *wd = w->widgets; wd && n < 64; wd = wd->next)
        if (focusable(wd)) { if (wd == w->focus) cur = n; list[n++] = wd; }
    if (!n) return;
    int nx = cur < 0 ? 0 : (cur + (back ? n - 1 : 1)) % n;
    w->focus = list[nx];
    if (w->focus->type == W_TEXTBOX) { w->focus->sel_anchor = 0; w->focus->cursor = (int)strlen(w->focus->text); }
    wm_invalidate(w);
}

static void textbox_delete_sel(widget_t *wd) {
    int s0 = MIN(wd->cursor, wd->sel_anchor), s1 = MAX(wd->cursor, wd->sel_anchor);
    if (s1 > s0) {
        memmove(wd->text + s0, wd->text + s1, strlen(wd->text + s1) + 1);
        wd->cursor = wd->sel_anchor = s0;
    }
}

static int textbox_key(window_t *w, widget_t *wd, input_event_t *k) {
    int len = (int)strlen(wd->text);
    int shift = k->mods & MOD_SHIFT;
    int ro = wd->style & WS_READONLY;
    switch (k->key) {
    /* the text is UTF-8: move and delete whole characters */
    case KEY_LEFT: if (wd->cursor > 0) wd->cursor = utf8_prev(wd->text, wd->cursor); if (!shift) wd->sel_anchor = wd->cursor; break;
    case KEY_RIGHT: if (wd->cursor < len) wd->cursor = utf8_after(wd->text, len, wd->cursor); if (!shift) wd->sel_anchor = wd->cursor; break;
    case KEY_HOME: wd->cursor = 0; if (!shift) wd->sel_anchor = 0; break;
    case KEY_END: wd->cursor = len; if (!shift) wd->sel_anchor = len; break;
    case KEY_BACKSPACE:
        if (ro) break;
        if (wd->cursor != wd->sel_anchor) textbox_delete_sel(wd);
        else if (wd->cursor > 0) {
            int p = utf8_prev(wd->text, wd->cursor);
            memmove(wd->text + p, wd->text + wd->cursor, len - wd->cursor + 1);
            wd->cursor = wd->sel_anchor = p;
        }
        if (wd->on_change) wd->on_change(w, wd);
        break;
    case KEY_DELETE:
        if (ro) break;
        if (wd->cursor != wd->sel_anchor) textbox_delete_sel(wd);
        else if (wd->cursor < len) {
            int nx = utf8_after(wd->text, len, wd->cursor);
            memmove(wd->text + wd->cursor, wd->text + nx, len - nx + 1);
        }
        if (wd->on_change) wd->on_change(w, wd);
        break;
    default:
        if (key_shortcut(k, 'A')) { wd->sel_anchor = 0; wd->cursor = len; break; }
        if (key_shortcut(k, 'C') || key_shortcut(k, 'X')) {
            int s0 = MIN(wd->cursor, wd->sel_anchor), s1 = MAX(wd->cursor, wd->sel_anchor);
            if (s1 > s0 && !(wd->style & WS_PASSWORD)) clipboard_set(wd->text + s0, s1 - s0);
            if (key_shortcut(k, 'X') && !ro) { textbox_delete_sel(wd); if (wd->on_change) wd->on_change(w, wd); }
            break;
        }
        if (key_shortcut(k, 'V')) {
            if (ro) break;
            int cl; const char *c = clipboard_get(&cl);
            textbox_delete_sel(wd);
            len = (int)strlen(wd->text);
            for (int i = 0; c && i < cl && len < 255; i++) {
                if (c[i] == '\n' || c[i] == '\r') continue;
                memmove(wd->text + wd->cursor + 1, wd->text + wd->cursor, len - wd->cursor + 1);
                wd->text[wd->cursor++] = c[i]; len++;
            }
            wd->sel_anchor = wd->cursor;
            if (wd->on_change) wd->on_change(w, wd);
            break;
        }
        if (key_is_text(k) && !ro) {
            char u[4];
            int n = utf8_put(k->ch, u);
            textbox_delete_sel(wd);
            len = (int)strlen(wd->text);
            if (len + n > 255) break;
            memmove(wd->text + wd->cursor + n, wd->text + wd->cursor, len - wd->cursor + 1);
            memcpy(wd->text + wd->cursor, u, n);
            wd->cursor += n;
            wd->sel_anchor = wd->cursor;
            if (wd->on_change) wd->on_change(w, wd);
            break;
        }
        return 0;
    }
    wm_invalidate(w);
    return 1;
}

static int textbox_pos(widget_t *wd, int x) {
    int rel = x - wd->r.x - 3 + wd->scroll;
    int n = (int)strlen(wd->text), acc = 0;
    for (int i = 0; i < n;) {
        int i2 = i;
        u32 cp = utf8_get(wd->text, n, &i2);
        int cw = gfx_char_width(&font_ui, (wd->style & WS_PASSWORD) ? '*' : cp);
        if (rel < acc + cw / 2) return i;
        acc += cw;
        i = i2;
    }
    return n;
}

static widget_t *pressed_widget;
static int sb_drag;
/* scroll bar widget being pressed: part, thumb grab, auto-repeat */
static int sbw_part, sbw_grab_pos, sbw_grab_mouse, sbw_mx, sbw_my;
static u64 sbw_next;

void widget_gone(widget_t *wd) {
    if (pressed_widget == wd) { pressed_widget = NULL; sb_drag = 0; sbw_part = 0; }
    if (tip_wd == wd) { tip_wd = NULL; tip_win = NULL; tip_shown = 0; }
}

static int sb_vertical(widget_t *wd) { return wd->type == W_VSCROLL; }
static int sb_track(widget_t *wd, int *tlen) {
    int len = sb_vertical(wd) ? wd->r.h : wd->r.w, th = sb_vertical(wd) ? wd->r.w : wd->r.h;
    int track = len - 2 * th;
    *tlen = (int)MAX(16, (i64)track * wd->page / MAX(1, wd->total));
    return track;
}

/* move a scroll bar and tell its owner */
static void sb_move(window_t *w, widget_t *wd, int pos, int part) {
    pos = CLAMP(pos, 0, sb_maxpos(wd));
    if (pos == wd->value && part != SBP_THUMB_END) return;
    wd->value = pos;
    wm_invalidate(w);
    if (wd->on_scroll) wd->on_scroll(w, wd, part);
}

/* one step for an arrow or page part; 0 when the pointer left that part */
static int sb_step(widget_t *wd, int part, int x, int y) {
    int p;
    if (!rect_contains(wd->r, x, y)) return 0;
    scrollbar_hit(wd->r, wd->total, wd->page, wd->value, x, y, sb_vertical(wd), &p);
    if (p != part) return 0;
    switch (part) {
    case SBP_LINE_UP: return -wd->line;
    case SBP_LINE_DOWN: return wd->line;
    case SBP_PAGE_UP: return -MAX(1, wd->page);
    case SBP_PAGE_DOWN: return MAX(1, wd->page);
    }
    return 0;
}

void widgets_tick(window_t *w) {
    u64 now = time_ms();
    if (tip_wd && tip_win == w && !tip_shown && now - tip_since >= 600 && widget_of(w, tip_wd)) {
        tip_shown = 1;
        wm_invalidate(w);
    }
    /* holding an arrow or the track repeats */
    if (pressed_widget && sbw_part >= SBP_LINE_UP && sbw_part <= SBP_PAGE_DOWN && now >= sbw_next && widget_of(w, pressed_widget)) {
        widget_t *wd = pressed_widget;
        int d = sb_step(wd, sbw_part, sbw_mx, sbw_my);
        if (d) sb_move(w, wd, wd->value + d, sbw_part);
        sbw_next = now + 50;
    }
}

int widgets_event(window_t *w, gui_event_t *e) {
    if (e->type == GE_KEY) {
        input_event_t *k = &e->key;
        if (!k->pressed) return 0;
        if (k->key == KEY_TAB && w->widgets && !(k->mods & (MOD_CTRL | MOD_ALT))) {
            for (widget_t *wd = w->widgets; wd; wd = wd->next) if (focusable(wd)) { focus_next(w, k->mods & MOD_SHIFT); return 1; }
            return 0;
        }
        widget_t *f = w->focus;
        if (f && f->type == W_TEXTBOX && !(f->style & WS_DISABLED)) {
            if (k->key == KEY_ENTER && f->on_activate) { f->on_activate(w, f); return 1; }
            if (k->key != KEY_ENTER && k->key != KEY_ESC && textbox_key(w, f, k)) return 1;
        }
        if (f && f->type == W_LISTBOX && f->nitems) {
            int rows = (f->r.h - 2) / LIST_ROW, old = f->value;
            if (k->key == KEY_UP) f->value = MAX(0, f->value - 1);
            else if (k->key == KEY_DOWN) f->value = MIN(f->nitems - 1, f->value + 1);
            else if (k->key == KEY_HOME) f->value = 0;
            else if (k->key == KEY_END) f->value = f->nitems - 1;
            else if (k->key == KEY_ENTER && f->on_activate) { f->on_activate(w, f); return 1; }
            if (f->value != old) {
                if (f->value < f->top) f->top = f->value;
                if (f->value >= f->top + rows) f->top = f->value - rows + 1;
                if (f->on_change) f->on_change(w, f);
                wm_invalidate(w);
                return 1;
            }
        }
        if ((k->key == KEY_SPACE) && f && (f->type == W_BUTTON || f->type == W_CHECKBOX)) {
            if (f->type == W_CHECKBOX) { f->value = !f->value; if (f->on_change) f->on_change(w, f); }
            if (f->on_click) f->on_click(w, f);
            wm_invalidate(w);
            return 1;
        }
        if (k->key == KEY_ENTER || k->key == KEY_KP_ENTER) {
            if (f && f->type == W_BUTTON && f->on_click) { f->on_click(w, f); return 1; }
            for (widget_t *wd = w->widgets; wd; wd = wd->next)
                if ((wd->style & WS_DEFAULT) && !(wd->style & WS_DISABLED) && wd->on_click) { wd->on_click(w, wd); return 1; }
        }
        if (k->key == KEY_ESC) {
            for (widget_t *wd = w->widgets; wd; wd = wd->next)
                if ((wd->style & WS_CANCEL) && wd->on_click) { wd->on_click(w, wd); return 1; }
        }
        return 0;
    }

    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int changed = 0;
        widget_t *tip = NULL;
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            int h = !(wd->style & (WS_DISABLED | WS_HIDDEN)) && rect_contains(wd->r, e->x, e->y);
            if (h != wd->hover && (wd->type == W_BUTTON || wd->type == W_CHECKBOX || wd->type == W_TOOLBUTTON)) { wd->hover = h; changed = 1; }
            if (e->type == GE_MOUSE_MOVE && wd->tip[0] && !(wd->style & WS_HIDDEN) && rect_contains(wd->r, e->x, e->y)) tip = wd;
        }
        if (e->buttons) tip = NULL;
        if (tip != tip_wd || (tip && tip_win != w)) {
            if (tip_wd || tip) tip_hide();
            if (tip) { tip_wd = tip; tip_win = w; tip_since = time_ms(); }
        }
        if (pressed_widget && (pressed_widget->type == W_VSCROLL || pressed_widget->type == W_HSCROLL) && widget_of(w, pressed_widget)) {
            widget_t *wd = pressed_widget;
            sbw_mx = e->x; sbw_my = e->y;
            if (sbw_part == SBP_THUMB_TRACK && (e->buttons & MOUSE_LEFT)) {
                int tlen, track = sb_track(wd, &tlen);
                int delta = (sb_vertical(wd) ? e->y : e->x) - sbw_grab_mouse;
                sb_move(w, wd, (int)CLAMP(sbw_grab_pos + (i64)delta * sb_maxpos(wd) / MAX(1, track - tlen), 0, sb_maxpos(wd)), SBP_THUMB_TRACK);
            }
            if (changed) wm_invalidate(w);
            return 1;
        }
        if (pressed_widget && pressed_widget->type == W_TEXTBOX && (e->buttons & MOUSE_LEFT)) {
            pressed_widget->cursor = textbox_pos(pressed_widget, e->x);
            changed = 1;
        }
        if (pressed_widget && pressed_widget->type == W_LISTBOX && sb_drag && (e->buttons & MOUSE_LEFT)) {
            widget_t *wd = pressed_widget;
            int rows = (wd->r.h - 2) / LIST_ROW;
            int track = wd->r.h - 2 - 34;
            int maxtop = MAX(0, wd->nitems - rows);
            wd->top = CLAMP((e->y - wd->r.y - 17) * maxtop / MAX(1, track), 0, maxtop);
            changed = 1;
        }
        if (changed) wm_invalidate(w);
        return pressed_widget != NULL;
    }

    if (e->type == GE_WHEEL) {
        for (widget_t *wd = w->widgets; wd; wd = wd->next)
            if ((wd->type == W_VSCROLL || wd->type == W_HSCROLL) && !(wd->style & (WS_DISABLED | WS_HIDDEN)) && rect_contains(wd->r, e->x, e->y)) {
                sb_move(w, wd, wd->value - e->dz * 3 * wd->line, SBP_WHEEL);
                return 1;
            }
        for (widget_t *wd = w->widgets; wd; wd = wd->next)
            if (wd->type == W_LISTBOX && rect_contains(wd->r, e->x, e->y)) {
                int rows = (wd->r.h - 2) / LIST_ROW;
                wd->top = CLAMP(wd->top - e->dz * 3, 0, MAX(0, wd->nitems - rows));
                wm_invalidate(w);
                return 1;
            }
        return 0;
    }

    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        if (tip_wd) tip_hide();
        if (e->button != MOUSE_LEFT) return 0;
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            if ((wd->style & (WS_DISABLED | WS_HIDDEN)) || !rect_contains(wd->r, e->x, e->y)) continue;
            if (wd->type == W_VSCROLL || wd->type == W_HSCROLL) {
                /* not a tab stop: the keyboard focus stays where it was */
                pressed_widget = wd;
                sbw_mx = e->x; sbw_my = e->y;
                int part;
                scrollbar_hit(wd->r, wd->total, wd->page, wd->value, e->x, e->y, sb_vertical(wd), &part);
                sbw_part = 0;
                if (wd->total <= wd->page) return 1;
                if (part == 5) {
                    sbw_part = SBP_THUMB_TRACK;
                    sbw_grab_pos = wd->value;
                    sbw_grab_mouse = sb_vertical(wd) ? e->y : e->x;
                } else {
                    sbw_part = part;
                    sb_move(w, wd, wd->value + sb_step(wd, part, e->x, e->y), part);
                    sbw_next = time_ms() + 400;
                }
                return 1;
            }
            if (wd->type == W_TOOLBUTTON) {
                pressed_widget = wd;
                wd->pressed = 1;
                wm_invalidate(w);
                return 1;
            }
            if (!focusable(wd)) continue;
            w->focus = wd;
            pressed_widget = wd;
            wd->pressed = 1;
            if (wd->type == W_TEXTBOX) {
                wd->cursor = textbox_pos(wd, e->x);
                if (e->type == GE_DBLCLICK) { wd->sel_anchor = 0; wd->cursor = (int)strlen(wd->text); }
                else if (!(input_mods() & MOD_SHIFT)) wd->sel_anchor = wd->cursor;
            } else if (wd->type == W_LISTBOX) {
                int rows = (wd->r.h - 2) / LIST_ROW;
                sb_drag = 0;
                if (wd->nitems > rows && e->x >= wd->r.x + wd->r.w - 18) {
                    int part;
                    wd->top = scrollbar_hit(mkrect(wd->r.x + wd->r.w - 18, wd->r.y + 1, 17, wd->r.h - 2), wd->nitems, rows, wd->top, e->x, e->y, 1, &part);
                    if (part == 5) sb_drag = 1;
                } else {
                    int idx = wd->top + (e->y - wd->r.y - 1) / LIST_ROW;
                    if (idx >= 0 && idx < wd->nitems) {
                        int ch = idx != wd->value;
                        wd->value = idx;
                        if (ch && wd->on_change) wd->on_change(w, wd);
                        if (e->type == GE_DBLCLICK && wd->on_activate) { wm_invalidate(w); pressed_widget = NULL; wd->on_activate(w, wd); return 1; }
                    }
                }
            }
            wm_invalidate(w);
            return 1;
        }
        return 0;
    }

    if (e->type == GE_MOUSE_UP) {
        widget_t *wd = pressed_widget;
        pressed_widget = NULL;
        sb_drag = 0;
        if (!wd || !widget_of(w, wd)) { sbw_part = 0; return 0; }
        wd->pressed = 0;
        if (wd->type == W_VSCROLL || wd->type == W_HSCROLL) {
            int part = sbw_part;
            sbw_part = 0;
            if (part == SBP_THUMB_TRACK) sb_move(w, wd, wd->value, SBP_THUMB_END);
            return 1;
        }
        int inside = rect_contains(wd->r, e->x, e->y);
        wm_invalidate(w);
        if (inside && wd->type == W_CHECKBOX) {
            wd->value = !wd->value;
            if (wd->on_change) wd->on_change(w, wd);
            if (wd->on_click) wd->on_click(w, wd);
        } else if (inside && (wd->type == W_BUTTON || wd->type == W_TOOLBUTTON) && wd->on_click) {
            wd->on_click(w, wd);
        }
        return 1;
    }
    return 0;
}
