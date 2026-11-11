/* Halcyon OS - popup menus and menu bars.
 *
 * Keyboard: F10 or a tap of the left Alt key opens a window's first menu;
 * Alt+letter opens the menu whose title has that mnemonic ("&File"). In an
 * open menu the arrows move (Left/Right switch between the bar's menus),
 * Enter chooses, Esc closes, and a letter chooses the item with that
 * mnemonic. */
#include "wm.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/mm.h"

#define ITEM_H 22
#define SEP_H 7

typedef struct popup {
    menu_t *menu;
    window_t *owner;
    int hover;
    window_t *bar_owner;    /* opened from this window's menu bar */
} popup_t;

static window_t *open_popup;
static window_t *bar_press;     /* a press on this window's menu bar: its mouse-up is the bar's */

/* ---------------- labels with mnemonics ---------------- */

/* the label without '&' markers; *ul = byte offset of the mnemonic or -1 */
static void label_strip(const char *label, char *out, int n, int *ul) {
    int o = 0;
    *ul = -1;
    for (int i = 0; label[i] && o < n - 1; i++) {
        if (label[i] == '&' && label[i + 1]) {
            i++;
            if (label[i] != '&' && *ul < 0) *ul = o;
        }
        out[o++] = label[i];
    }
    out[o] = 0;
}

/* the mnemonic as an upper-case code point, 0 if none */
static u32 label_key(const char *label) {
    char buf[128];
    int ul;
    label_strip(label, buf, sizeof buf, &ul);
    if (ul < 0) return 0;
    int pos = ul;
    return uc_upper(utf8_get(buf, (int)strlen(buf), &pos));
}

int menu_label_width(const font_t *f, const char *label) {
    char buf[128];
    int ul;
    label_strip(label, buf, sizeof buf, &ul);
    return gfx_text_width(f, buf);
}

static void label_draw(gc_t *g, const font_t *f, rect_t r, const char *label, color_t c, int align) {
    char buf[128];
    int ul;
    label_strip(label, buf, sizeof buf, &ul);
    gfx_text_box(g, f, r, buf, c, align);
    int w = gfx_text_width(f, buf);
    if (ul < 0 || w > r.w) return;
    int x = align == 1 ? r.x + (r.w - w) / 2 : align == 2 ? r.x + r.w - w : r.x;
    int pos = ul;
    u32 cp = utf8_get(buf, (int)strlen(buf), &pos);
    int ux = x + gfx_text_width_n(f, buf, ul);
    gfx_hline(g, ux, r.y + (r.h - f->height) / 2 + f->ascent + 1, gfx_char_width(f, cp), c);
}

/* the letter a key event stands for (layout letter or typed character), upper case */
static u32 event_letter(const input_event_t *k) {
    if (k->vk) return k->vk;
    return k->ch >= 32 ? uc_upper(k->ch) : 0;
}

/* ---------------- popups ---------------- */
static int item_h(menu_item_t *it) { return (it->flags & MI_SEPARATOR) ? SEP_H : ITEM_H; }

static int item_at(popup_t *p, int y) {
    int yy = 2;
    for (int i = 0; i < p->menu->count; i++) {
        int h = item_h(&p->menu->items[i]);
        if (y >= yy && y < yy + h) return (p->menu->items[i].flags & (MI_SEPARATOR | MI_DISABLED)) ? -1 : i;
        yy += h;
    }
    return -1;
}

static void popup_paint(window_t *w, gc_t *g) {
    popup_t *p = w->data;
    rect_t r = mkrect(0, 0, w->client->w, w->client->h);
    gfx_fill(g, r, HEX(0xFFFFFF));
    gfx_fill(g, mkrect(1, 1, 26, r.h - 2), color_lighten(g_theme->face, -8));
    gfx_rect(g, r, HEX(0x8A8A80));
    int y = 2;
    for (int i = 0; i < p->menu->count; i++) {
        menu_item_t *it = &p->menu->items[i];
        int h = item_h(it);
        if (it->flags & MI_SEPARATOR) {
            gfx_hline(g, 30, y + 3, r.w - 34, HEX(0xC5C2B8));
        } else {
            int hov = i == p->hover;
            if (hov) gfx_fill(g, mkrect(2, y, r.w - 4, h), g_theme->select);
            color_t tc = (it->flags & MI_DISABLED) ? HEX(0xA19F94) : hov ? g_theme->select_text : HEX(0x000000);
            if (it->flags & MI_CHECKED) {
                for (int k = 0; k < 2; k++) {
                    gfx_line(g, 9, y + 10 + k, 12, y + 13 + k, tc);
                    gfx_line(g, 12, y + 13 + k, 18, y + 7 + k, tc);
                }
            } else if (it->icon > 0 && it->icon < ICON_COUNT) {
                if (it->flags & MI_DISABLED) gfx_icon_alpha(g, 5, y + 3, &icons16[it->icon], 90);
                else gfx_icon(g, 5, y + 3, &icons16[it->icon]);
            }
            label_draw(g, (it->flags & MI_BOLD) ? &font_ui_bold : &font_ui, mkrect(32, y, r.w - 40, h), it->label, tc, 0);
            if (it->shortcut) gfx_text_box(g, &font_ui, mkrect(32, y, r.w - 42, h), it->shortcut, tc, 2);
        }
        y += h;
    }
}

static void popup_activate(window_t *w, int idx) {
    popup_t *p = w->data;
    window_t *owner = p->owner;
    int id = p->menu->items[idx].id;
    menu_close_all();
    if (owner && owner->on_menu && !owner->closed && (wm_alive(owner) || !owner->client)) owner->on_menu(owner, id);
}

static int first_enabled(menu_t *m) {
    for (int i = 0; i < m->count; i++) if (!(m->items[i].flags & (MI_SEPARATOR | MI_DISABLED))) return i;
    return -1;
}

static void popup_event(window_t *w, gui_event_t *e) {
    popup_t *p = w->data;
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int h = (e->x >= 0 && e->x < w->client->w) ? item_at(p, e->y) : -1;
        if (h != p->hover) { p->hover = h; wm_invalidate(w); }
    } else if (e->type == GE_MOUSE_UP) {
        int h = item_at(p, e->y);
        if (h >= 0 && e->x >= 0 && e->x < w->client->w) popup_activate(w, h);
    } else if (e->type == GE_KEY && e->key.pressed) {
        int n = p->menu->count;
        if (e->key.key == KEY_ESC) { menu_close_all(); return; }
        if (e->key.key == KEY_DOWN || e->key.key == KEY_UP) {
            int d = e->key.key == KEY_DOWN ? 1 : -1;
            int i = p->hover;
            if (i < 0 && d < 0) i = 0;
            for (int k = 0; k < n; k++) {
                i = (i + d + n) % n;
                if (!(p->menu->items[i].flags & (MI_SEPARATOR | MI_DISABLED))) break;
            }
            if (n && !(p->menu->items[i].flags & (MI_SEPARATOR | MI_DISABLED))) p->hover = i;
            wm_invalidate(w);
        } else if ((e->key.key == KEY_ENTER || e->key.key == KEY_KP_ENTER) && p->hover >= 0) {
            popup_activate(w, p->hover);
        } else if ((e->key.key == KEY_LEFT || e->key.key == KEY_RIGHT) && p->bar_owner && p->bar_owner->menubar) {
            menubar_t *mb = p->bar_owner->menubar;
            int nx = (mb->open + (e->key.key == KEY_RIGHT ? 1 : mb->count - 1)) % mb->count;
            menubar_open(p->bar_owner, nx, 1);
        } else {
            /* a mnemonic chooses its item */
            u32 c = event_letter(&e->key);
            if (!c || (e->key.mods & MOD_CTRL)) return;
            for (int i = 0; i < n; i++) {
                menu_item_t *it = &p->menu->items[i];
                if (!(it->flags & (MI_SEPARATOR | MI_DISABLED)) && label_key(it->label) == c) { popup_activate(w, i); return; }
            }
        }
    }
}

static void popup_destroy(window_t *w) {
    popup_t *p = w->data;
    if (p->bar_owner && wm_alive(p->bar_owner) && p->bar_owner->menubar) {
        p->bar_owner->menubar->open = -1;
        wm_invalidate(p->bar_owner);
    }
    kfree(p);
    if (open_popup == w) open_popup = NULL;
}

void menu_close_all(void) {
    for (;;) {
        window_t *pop = NULL;
        for (window_t *w = wm_first(); w; w = w->next) if (w->flags & WF_POPUP) pop = w;
        if (!pop) break;
        wm_destroy(pop);
    }
    open_popup = NULL;
}

void menu_close_owned(window_t *owner) {
    if (bar_press == owner) bar_press = NULL;
    for (window_t *w = wm_first(); w; w = w->next) {
        if (!(w->flags & WF_POPUP) || w->on_paint != popup_paint) continue;
        popup_t *p = w->data;
        if (p && (p->owner == owner || p->bar_owner == owner)) { menu_close_all(); return; }
    }
}

void menu_refresh(void) {
    for (window_t *w = wm_first(); w; w = w->next)
        if ((w->flags & WF_POPUP) && w->on_paint == popup_paint) wm_invalidate(w);
}

void menu_popup(window_t *owner, menu_t *m, int x, int y) {
    menu_close_all();
    int wdt = 120, h = 4;
    for (int i = 0; i < m->count; i++) {
        menu_item_t *it = &m->items[i];
        h += item_h(it);
        if (!(it->flags & MI_SEPARATOR)) {
            int tw = menu_label_width(&font_ui_bold, it->label) + 52 + (it->shortcut ? gfx_text_width(&font_ui, it->shortcut) + 24 : 0);
            if (tw > wdt) wdt = tw;
        }
    }
    wdt = MIN(wdt, screen_w());
    if (x + wdt > screen_w()) x = screen_w() - wdt;
    if (y + h > screen_h()) y = MAX(0, y - h);
    x = MAX(0, x);
    window_t *w = wm_create("", -1, x, y, wdt, h, WF_POPUP | WF_TOPMOST);
    popup_t *p = kzalloc(sizeof *p);
    if (!w || !p) { if (w) wm_destroy(w); kfree(p); return; }
    p->menu = m; p->owner = owner; p->hover = -1;
    w->data = p;
    w->on_paint = popup_paint;
    w->on_event = popup_event;
    w->on_destroy = popup_destroy;
    open_popup = w;
}

/* ---------------- menu bar ---------------- */
void menubar_attach(window_t *w, menu_t *menus, int count) {
    menubar_t *mb = kzalloc(sizeof *mb);
    mb->menus = menus; mb->count = count; mb->open = -1; mb->hover = -1;
    w->menubar = mb;
    wm_invalidate(w);
}

int menubar_height(window_t *w) { return w->menubar ? MENUBAR_H : 0; }

static int bar_item_w(menubar_t *mb, int i) { return menu_label_width(&font_ui, mb->menus[i].title) + 16; }

static int bar_item_at(window_t *w, int x, int *ix) {
    menubar_t *mb = w->menubar;
    int xx = 2;
    for (int i = 0; i < mb->count; i++) {
        int iw = bar_item_w(mb, i);
        if (x >= xx && x < xx + iw) { *ix = xx; return i; }
        xx += iw;
    }
    return -1;
}

void menubar_paint(window_t *w, gc_t *g) {
    menubar_t *mb = w->menubar;
    int wd = w->client->w;
    gfx_fill(g, mkrect(0, 0, wd, MENUBAR_H), g_theme->face);
    gfx_hline(g, 0, MENUBAR_H - 1, wd, color_lighten(g_theme->face, -25));
    int x = 2;
    for (int i = 0; i < mb->count; i++) {
        int iw = bar_item_w(mb, i);
        rect_t r = mkrect(x, 1, iw, MENUBAR_H - 3);
        int act = i == mb->open, hov = i == mb->hover;
        if (act) gfx_fill(g, r, g_theme->select);
        else if (hov) gfx_rect(g, r, color_lighten(g_theme->select, 60));
        label_draw(g, &font_ui, r, mb->menus[i].title, act ? g_theme->select_text : HEX(0x000000), 1);
        x += iw;
    }
}

void menubar_open(window_t *w, int index, int keyboard) {
    menubar_t *mb = w->menubar;
    if (!mb || index < 0 || index >= mb->count) return;
    menu_close_all();
    rect_t c = wm_client_rect(w);
    int x = 2;
    for (int i = 0; i < index; i++) x += bar_item_w(mb, i);
    menu_popup(w, &mb->menus[index], c.x + x, c.y + MENUBAR_H - 1);
    if (open_popup) {
        popup_t *p = open_popup->data;
        p->bar_owner = w;
        if (keyboard) p->hover = first_enabled(&mb->menus[index]);
    }
    mb->open = open_popup ? index : -1;
    wm_invalidate(w);
}

int menubar_event(window_t *w, gui_event_t *e) {
    menubar_t *mb = w->menubar;
    if (e->type == GE_KEY) {
        input_event_t *k = &e->key;
        if (!k->pressed || !mb->count) return 0;
        if (k->key == KEY_F10 && !(k->mods & (MOD_CTRL | MOD_ALT | MOD_SHIFT))) { menubar_open(w, 0, 1); return 1; }
        if ((k->mods & (MOD_CTRL | MOD_ALT)) == MOD_ALT) {
            u32 c = event_letter(k);
            for (int i = 0; c && i < mb->count; i++)
                if (label_key(mb->menus[i].title) == c) { menubar_open(w, i, 1); return 1; }
        }
        return 0;
    }
    int in = e->y >= 0 && e->y < MENUBAR_H && e->x >= 0;
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int ix, h = in && !e->buttons ? bar_item_at(w, e->x, &ix) : -1;
        if (h != mb->hover) { mb->hover = h; wm_invalidate(w); }
        /* a drag that started in the client area keeps going to it over the bar */
        return in && (!e->buttons || bar_press == w);
    }
    if (e->type == GE_MOUSE_UP) {
        if (bar_press != w) return 0;
        if (!e->buttons) bar_press = NULL;
        return 1;
    }
    if (!in) return 0;
    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) bar_press = w;
    if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_LEFT) {
        int ix, h = bar_item_at(w, e->x, &ix);
        if (h < 0) return 1;
        if (mb->open == h) { menu_close_all(); mb->open = -1; wm_invalidate(w); return 1; }
        menubar_open(w, h, 0);
        return 1;
    }
    return 1;
}
