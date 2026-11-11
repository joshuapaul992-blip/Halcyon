/* Halcyon OS - widgets: buttons, labels, text boxes, check boxes, list boxes */
#include "wm.h"
#include "core/lib.h"
#include "core/mm.h"

#define LIST_ROW 18

widget_t *widget_add(window_t *w, int type, rect_t r, const char *text, int id) {
    widget_t *wd = kzalloc(sizeof *wd);
    wd->type = type;
    wd->r = r;
    wd->id = id;
    strlcpy(wd->text, text ? text : "", sizeof wd->text);
    if (type == W_TEXTBOX) { wd->cursor = wd->sel_anchor = (int)strlen(wd->text); }
    if (type == W_LISTBOX) wd->value = -1;
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
    int tlen = MAX(16, track * visible / total);
    int tpos = (track - tlen) * pos / MAX(1, total - visible);
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
    int tlen = MAX(16, track * visible / MAX(1, total));
    int tpos = (track - tlen) * pos / MAX(1, maxpos);
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
    char shown[256];
    int n = (int)strlen(wd->text);
    for (int i = 0; i < n; i++) shown[i] = (wd->style & WS_PASSWORD) ? '*' : wd->text[i];
    shown[n] = 0;
    /* keep cursor visible */
    int cx = gfx_text_width_n(&font_ui, shown, wd->cursor);
    if (cx - wd->scroll > in.w - 2) wd->scroll = cx - in.w + 2;
    if (cx - wd->scroll < 0) wd->scroll = cx;
    int ty = (in.h - font_ui.height) / 2;
    int focused = w->focus == wd && wm_active() == w;
    int s0 = MIN(wd->cursor, wd->sel_anchor), s1 = MAX(wd->cursor, wd->sel_anchor);
    if (focused && s1 > s0) {
        int x0 = gfx_text_width_n(&font_ui, shown, s0) - wd->scroll;
        int x1 = gfx_text_width_n(&font_ui, shown, s1) - wd->scroll;
        gfx_fill(&sub, mkrect(x0, ty - 1, x1 - x0, font_ui.height + 2), g_theme->select);
    }
    int x = -wd->scroll;
    for (int i = 0; i < n; i++) {
        int sel = focused && i >= s0 && i < s1;
        char c[2] = { shown[i], 0 };
        x = gfx_text(&sub, &font_ui, x, ty, c, dis ? HEX(0xA19F94) : sel ? g_theme->select_text : HEX(0x000000));
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
        }
    }
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
    case KEY_LEFT: if (wd->cursor > 0) wd->cursor--; if (!shift) wd->sel_anchor = wd->cursor; break;
    case KEY_RIGHT: if (wd->cursor < len) wd->cursor++; if (!shift) wd->sel_anchor = wd->cursor; break;
    case KEY_HOME: wd->cursor = 0; if (!shift) wd->sel_anchor = 0; break;
    case KEY_END: wd->cursor = len; if (!shift) wd->sel_anchor = len; break;
    case KEY_BACKSPACE:
        if (ro) break;
        if (wd->cursor != wd->sel_anchor) textbox_delete_sel(wd);
        else if (wd->cursor > 0) { memmove(wd->text + wd->cursor - 1, wd->text + wd->cursor, len - wd->cursor + 1); wd->cursor--; wd->sel_anchor = wd->cursor; }
        if (wd->on_change) wd->on_change(w, wd);
        break;
    case KEY_DELETE:
        if (ro) break;
        if (wd->cursor != wd->sel_anchor) textbox_delete_sel(wd);
        else if (wd->cursor < len) memmove(wd->text + wd->cursor, wd->text + wd->cursor + 1, len - wd->cursor);
        if (wd->on_change) wd->on_change(w, wd);
        break;
    default:
        if ((k->mods & MOD_CTRL) && k->key == KEY_A) { wd->sel_anchor = 0; wd->cursor = len; break; }
        if ((k->mods & MOD_CTRL) && (k->key == KEY_A + 2 || k->key == KEY_A + 23)) {   /* ^C / ^X */
            int s0 = MIN(wd->cursor, wd->sel_anchor), s1 = MAX(wd->cursor, wd->sel_anchor);
            if (s1 > s0 && !(wd->style & WS_PASSWORD)) clipboard_set(wd->text + s0, s1 - s0);
            if (k->key == KEY_A + 23 && !ro) { textbox_delete_sel(wd); if (wd->on_change) wd->on_change(w, wd); }
            break;
        }
        if ((k->mods & MOD_CTRL) && k->key == KEY_A + 21) {   /* ^V */
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
        if (k->ch >= 32 && k->ch != 127 && !ro) {
            textbox_delete_sel(wd);
            len = (int)strlen(wd->text);
            if (len >= 255) break;
            memmove(wd->text + wd->cursor + 1, wd->text + wd->cursor, len - wd->cursor + 1);
            wd->text[wd->cursor++] = k->ch;
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
    for (int i = 0; i < n; i++) {
        int cw = gfx_char_width(&font_ui, (wd->style & WS_PASSWORD) ? '*' : wd->text[i]);
        if (rel < acc + cw / 2) return i;
        acc += cw;
    }
    return n;
}

static widget_t *pressed_widget;
static int sb_drag;

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
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            int h = !(wd->style & (WS_DISABLED | WS_HIDDEN)) && rect_contains(wd->r, e->x, e->y);
            if (h != wd->hover && (wd->type == W_BUTTON || wd->type == W_CHECKBOX)) { wd->hover = h; changed = 1; }
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
            if (wd->type == W_LISTBOX && rect_contains(wd->r, e->x, e->y)) {
                int rows = (wd->r.h - 2) / LIST_ROW;
                wd->top = CLAMP(wd->top - e->dz * 3, 0, MAX(0, wd->nitems - rows));
                wm_invalidate(w);
                return 1;
            }
        return 0;
    }

    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        if (e->button != MOUSE_LEFT) return 0;
        for (widget_t *wd = w->widgets; wd; wd = wd->next) {
            if ((wd->style & (WS_DISABLED | WS_HIDDEN)) || !rect_contains(wd->r, e->x, e->y)) continue;
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
        if (!wd) return 0;
        wd->pressed = 0;
        int inside = rect_contains(wd->r, e->x, e->y);
        wm_invalidate(w);
        if (inside && wd->type == W_CHECKBOX) {
            wd->value = !wd->value;
            if (wd->on_change) wd->on_change(w, wd);
            if (wd->on_click) wd->on_click(w, wd);
        } else if (inside && wd->type == W_BUTTON && wd->on_click) {
            wd->on_click(w, wd);
        }
        return 1;
    }
    return 0;
}
