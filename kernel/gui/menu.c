/* Halcyon OS - popup menus and menu bars */
#include "wm.h"
#include "core/lib.h"
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
            } else if (it->icon > 0) {
                gfx_icon(g, 5, y + 3, &icons16[it->icon]);
            }
            gfx_text_box(g, (it->flags & MI_BOLD) ? &font_ui_bold : &font_ui, mkrect(32, y, r.w - 40, h), it->label, tc, 0);
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
            for (int k = 0; k < n; k++) {
                i = (i + d + n) % n;
                if (!(p->menu->items[i].flags & (MI_SEPARATOR | MI_DISABLED))) break;
            }
            p->hover = i;
            wm_invalidate(w);
        } else if (e->key.key == KEY_ENTER && p->hover >= 0) {
            popup_activate(w, p->hover);
        } else if ((e->key.key == KEY_LEFT || e->key.key == KEY_RIGHT) && p->bar_owner && p->bar_owner->menubar) {
            menubar_t *mb = p->bar_owner->menubar;
            int nx = (mb->open + (e->key.key == KEY_RIGHT ? 1 : mb->count - 1)) % mb->count;
            window_t *bo = p->bar_owner;
            menu_close_all();
            mb->open = nx;
            /* reopen at the new position */
            rect_t c = wm_client_rect(bo);
            int x = 2;
            for (int i = 0; i < nx; i++) x += gfx_text_width(&font_ui, mb->menus[i].title) + 16;
            menu_popup(bo, &mb->menus[nx], c.x + x, c.y + MENUBAR_H);
            if (open_popup) ((popup_t *)open_popup->data)->bar_owner = bo;
            mb->open = nx;
            wm_invalidate(bo);
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

void menu_popup(window_t *owner, menu_t *m, int x, int y) {
    menu_close_all();
    int wdt = 120, h = 4;
    for (int i = 0; i < m->count; i++) {
        menu_item_t *it = &m->items[i];
        h += item_h(it);
        if (!(it->flags & MI_SEPARATOR)) {
            int tw = gfx_text_width(&font_ui_bold, it->label) + 52 + (it->shortcut ? gfx_text_width(&font_ui, it->shortcut) + 24 : 0);
            if (tw > wdt) wdt = tw;
        }
    }
    if (x + wdt > screen_w()) x = screen_w() - wdt;
    if (y + h > screen_h()) y = MAX(0, y - h);
    window_t *w = wm_create("", -1, x, y, wdt, h, WF_POPUP | WF_TOPMOST);
    popup_t *p = kzalloc(sizeof *p);
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

static int bar_item_at(window_t *w, int x, int *ix) {
    menubar_t *mb = w->menubar;
    int xx = 2;
    for (int i = 0; i < mb->count; i++) {
        int iw = gfx_text_width(&font_ui, mb->menus[i].title) + 16;
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
        int iw = gfx_text_width(&font_ui, mb->menus[i].title) + 16;
        rect_t r = mkrect(x, 1, iw, MENUBAR_H - 3);
        int act = i == mb->open, hov = i == mb->hover;
        if (act) gfx_fill(g, r, g_theme->select);
        else if (hov) gfx_rect(g, r, color_lighten(g_theme->select, 60));
        gfx_text_box(g, &font_ui, r, mb->menus[i].title, act ? g_theme->select_text : HEX(0x000000), 1);
        x += iw;
    }
}

int menubar_event(window_t *w, gui_event_t *e) {
    menubar_t *mb = w->menubar;
    if (e->type == GE_KEY) return 0;
    int in = e->y >= 0 && e->y < MENUBAR_H && e->x >= 0;
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        int ix, h = in ? bar_item_at(w, e->x, &ix) : -1;
        if (h != mb->hover) { mb->hover = h; wm_invalidate(w); }
        return in;
    }
    if (!in) return 0;
    if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_LEFT) {
        int ix, h = bar_item_at(w, e->x, &ix);
        if (h < 0) return 1;
        if (mb->open == h) { menu_close_all(); mb->open = -1; wm_invalidate(w); return 1; }
        rect_t c = wm_client_rect(w);
        menu_popup(w, &mb->menus[h], c.x + ix, c.y + MENUBAR_H - 1);
        if (open_popup) ((popup_t *)open_popup->data)->bar_owner = w;
        mb->open = h;
        wm_invalidate(w);
        return 1;
    }
    return 1;
}
