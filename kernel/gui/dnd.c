/* Halcyon OS - drag and drop (see dnd.h). Runs on the GUI thread; the window
 * manager hands it the pointer and the keys while a drag is in progress.
 * Programs reach it through gui/usergui.c (SYS_DND_*, EVT_DRAG). */
#include "dnd.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

#define MAX_PATHS_BYTES (256 * 1024)
enum { T_NONE = 0, T_WINDOW, T_DESKTOP };

static struct {
    int active;
    u32 serial;
    window_t *src;
    char *paths;                 /* NUL-separated; kept until the next drag */
    int len, count;
    int allowed, icon, button;
    int x, y, mods;
    /* the target under the pointer */
    int tkind;
    window_t *twin;
    int effect, possible;        /* its last answer */
    u64 over_task_ms;            /* hovering over a task button since */
    window_t *over_task;
    rect_t drawn;                /* where the image was drawn */
    /* a right-button drop waiting for its menu */
    int menu;
    int mtkind;
    window_t *mtwin;
    int mx, my, mmods, mpossible, meffect;
} D;

static window_t menu_owner;     /* owner of the right-drag menu (not a real window) */

int dnd_active(void) { return D.active; }

const char *dnd_paths(u32 serial, int *len, int *count) {
    if (!D.paths || serial != D.serial) return NULL;
    if (len) *len = D.len;
    if (count) *count = D.count;
    return D.paths;
}

static rect_t image_rect(int x, int y) { return mkrect(x - 2, y + 4, 76, 60); }

static void redraw(void) {
    wm_invalidate_rect(D.drawn);
    D.drawn = image_rect(D.x, D.y);
    wm_invalidate_rect(D.drawn);
}

static int filter(int effect, int possible) {
    possible &= D.allowed;
    if (!(effect & possible)) effect = (possible & DND_MOVE) ? DND_MOVE : (possible & DND_COPY) ? DND_COPY : DND_NONE;
    if (effect & DND_MOVE) return DND_MOVE;
    return effect & DND_COPY;
}

/* deliver an event to a target; returns its answer (or DND_LATER) */
static int send_to(int kind, window_t *w, int type, int x, int y, int mods, int effect) {
    dnd_event_t e = { .type = type, .mods = mods, .button = D.button, .serial = D.serial, .count = D.count,
                      .allowed = D.allowed, .effect = effect };
    if (kind == T_WINDOW && w && wm_alive(w) && w->on_dnd) {
        rect_t c = wm_client_rect(w);
        e.x = x - c.x;
        e.y = y - c.y;
        return w->on_dnd(w, &e);
    }
    if (kind == T_DESKTOP) {
        e.x = x;
        e.y = y;
        return desktop_dnd(&e);
    }
    return DND_ANSWER(DND_NONE, 0);
}

static void take_answer(int r) {
    if (r == DND_LATER) return;
    D.effect = r & 0xFF;
    D.possible = (r >> 8) & 0xFF;
    D.effect = filter(D.effect, D.possible | D.effect);
}

static void end_source(int effect) {
    if (D.src && wm_alive(D.src) && D.src->on_dnd) {
        dnd_event_t e = { .type = DND_END, .x = -1, .y = -1, .mods = input_mods(), .button = D.button,
                          .serial = D.serial, .count = D.count, .allowed = D.allowed, .effect = effect };
        D.src->on_dnd(D.src, &e);
    }
    D.src = NULL;
}

/* the target under the pointer */
static void find_target(int *kind, window_t **win) {
    *kind = T_NONE;
    *win = NULL;
    window_t *w = wm_window_at(D.x, D.y);
    if (w) {
        if ((w->flags & WF_POPUP) || (w->modal && wm_alive(w->modal))) return;
        if (!w->on_dnd || !rect_contains(wm_client_rect(w), D.x, D.y)) { *kind = T_WINDOW; return; }   /* (frames: no drop) */
        *kind = T_WINDOW;
        *win = w;
        return;
    }
    if (rect_contains(taskbar_rect(), D.x, D.y)) return;
    *kind = T_DESKTOP;
}

static void update_target(int force_over) {
    int kind;
    window_t *w;
    find_target(&kind, &w);
    if (kind != D.tkind || w != D.twin) {
        send_to(D.tkind, D.twin, DND_LEAVE, D.x, D.y, D.mods, 0);
        D.tkind = kind;
        D.twin = w;
        D.effect = D.possible = 0;
        take_answer(send_to(kind, w, DND_ENTER, D.x, D.y, D.mods, 0));
    } else if (kind != T_NONE || force_over) {
        take_answer(send_to(kind, w, DND_OVER, D.x, D.y, D.mods, 0));
    }
    redraw();
}

static int begin(window_t *src, const char *paths, int len, int count, int allowed, int icon, int button) {
    if (D.active || D.menu) return E_BUSY;
    if (len <= 0 || len > MAX_PATHS_BYTES || count <= 0) return E_INVAL;
    if (!button) return E_AGAIN;
    char *copy = kmalloc((size_t)len + 1);
    if (!copy) return E_NOMEM;
    memcpy(copy, paths, (size_t)len);
    copy[len] = 0;
    kfree(D.paths);
    memset(&D, 0, sizeof D);
    static u32 serials;
    D.serial = ++serials;
    D.paths = copy;
    D.len = len;
    D.count = count;
    D.src = src;
    D.allowed = allowed & (DND_COPY | DND_MOVE);
    D.icon = icon >= 0 && icon < ICON_COUNT ? icon : ICON_FILE;
    D.button = button;
    D.x = wm_mouse_x();
    D.y = wm_mouse_y();
    D.mods = input_mods();
    D.active = 1;
    menu_close_all();
    wm_capture_release();
    kprintf("dnd: drag %u of %d item%s (%s button)\n", D.serial, count, count == 1 ? "" : "s", button == MOUSE_LEFT ? "left" : "right");
    update_target(1);
    return (int)D.serial;
}

int dnd_begin(window_t *src, const char *paths, int len, int count, int allowed, int icon) {
    int buttons = wm_mouse_buttons();
    return begin(src, paths, len, count, allowed, icon,
                 (buttons & MOUSE_LEFT) ? MOUSE_LEFT : (buttons & MOUSE_RIGHT) ? MOUSE_RIGHT : 0);
}

int dnd_test_begin(const char *paths, int len, int count, int icon) {
    return begin(NULL, paths, len, count, DND_COPY | DND_MOVE, icon, MOUSE_LEFT);
}

int dnd_effect(void) { return D.active ? D.effect : -1; }

void dnd_answer(window_t *w, u32 serial, int effect, int possible) {
    if (!D.active || serial != D.serial || w != D.twin || D.tkind != T_WINDOW) return;
    int old = D.effect;
    D.possible = possible & 0xFF;
    D.effect = filter(effect & 0xFF, D.possible | (effect & 0xFF));
    if (D.effect != old) redraw();
}

static void finish(void) {
    D.active = 0;
    D.tkind = T_NONE;
    D.twin = NULL;
    wm_invalidate_rect(D.drawn);
    wm_cursor_changed();
}

static void cancel(void) {
    send_to(D.tkind, D.twin, DND_LEAVE, D.x, D.y, D.mods, 0);
    kprintf("dnd: drag %u canceled\n", D.serial);
    end_source(0);
    finish();
}

static void deliver_drop(int kind, window_t *w, int x, int y, int mods, int effect) {
    kprintf("dnd: drop %u (%s) on %s\n", D.serial, effect == DND_COPY ? "copy" : "move",
            kind == T_DESKTOP ? "the desktop" : w ? w->title : "?");
    send_to(kind, w, DND_DROP, x, y, mods, effect);
    end_source(effect);
}

/* the right-drag menu */
enum { MID_COPY = 1, MID_MOVE, MID_CANCEL };
static menu_item_t menu_items[] = {
    { "&Copy Here", NULL, MID_COPY, 0, 0 },
    { "&Move Here", NULL, MID_MOVE, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Cancel", NULL, MID_CANCEL, 0, 0 },
};
static menu_t drop_menu = { "", menu_items, 4 };

static void menu_chosen(window_t *w, int id) {
    (void)w;
    if (!D.menu) return;
    D.menu = 0;
    int effect = id == MID_COPY ? DND_COPY : id == MID_MOVE ? DND_MOVE : DND_NONE;
    if (effect && (D.mtkind != T_WINDOW || (D.mtwin && wm_alive(D.mtwin)))) {
        deliver_drop(D.mtkind, D.mtwin, D.mx, D.my, D.mmods, effect);
    } else {
        send_to(D.mtkind, D.mtwin, DND_LEAVE, D.mx, D.my, D.mmods, 0);
        end_source(0);
    }
    D.mtwin = NULL;
}

static void drop(void) {
    int effect = D.effect;
    if (!effect) { cancel(); return; }
    int kind = D.tkind;
    window_t *w = D.twin;
    if (D.button == MOUSE_RIGHT) {
        /* ask: Copy Here / Move Here / Cancel, the target's choice in bold */
        int possible = (D.possible | D.effect) & D.allowed;
        menu_items[0].flags = (possible & DND_COPY ? 0 : MI_DISABLED) | (effect == DND_COPY ? MI_BOLD : 0);
        menu_items[1].flags = (possible & DND_MOVE ? 0 : MI_DISABLED) | (effect == DND_MOVE ? MI_BOLD : 0);
        D.menu = 1;
        D.mtkind = kind;
        D.mtwin = w;
        D.mx = D.x; D.my = D.y; D.mmods = D.mods;
        finish();
        menu_owner.on_menu = menu_chosen;
        menu_popup(&menu_owner, &drop_menu, D.mx, D.my);
        return;
    }
    finish();
    deliver_drop(kind, w, D.x, D.y, D.mods, effect);
}

void dnd_pointer(int x, int y, int old_buttons, int buttons) {
    if (!D.active) return;
    D.x = x;
    D.y = y;
    int other = (D.button == MOUSE_LEFT ? MOUSE_RIGHT : MOUSE_LEFT) | MOUSE_MIDDLE;
    if ((buttons & ~old_buttons) & other) { cancel(); return; }     /* another button: cancel */
    if (!(buttons & D.button)) {
        update_target(0);
        drop();
        return;
    }
    update_target(0);
}

int dnd_key(const input_event_t *k) {
    if (!D.active) return 0;
    if (k->pressed && k->key == KEY_ESC) { cancel(); return 1; }
    int m = input_mods();
    if (m != D.mods) {          /* Ctrl / Shift change the effect */
        D.mods = m;
        update_target(1);
    }
    return 1;
}

void dnd_window_gone(window_t *w) {
    if (D.src == w) D.src = NULL;
    if (D.twin == w) { D.twin = NULL; D.tkind = T_NONE; D.effect = D.possible = 0; }
    if (D.mtwin == w) D.mtwin = NULL;
    if (D.over_task == w) D.over_task = NULL;
}

void dnd_tick(void) {
    if (D.menu) {
        /* the menu went away without a choice */
        int open = 0;
        for (window_t *w = wm_first(); w; w = w->next) if (w->flags & WF_POPUP) open = 1;
        if (!open) {
            D.menu = 0;
            send_to(D.mtkind, D.mtwin, DND_LEAVE, D.mx, D.my, D.mmods, 0);
            end_source(0);
            D.mtwin = NULL;
        }
        return;
    }
    if (!D.active) return;
    /* resting on a task button brings its window up, as on Windows */
    window_t *t = rect_contains(taskbar_rect(), D.x, D.y) ? taskbar_window_at(D.x, D.y) : NULL;
    if (t != D.over_task) { D.over_task = t; D.over_task_ms = time_ms(); }
    else if (t && time_ms() - D.over_task_ms > 800) {
        D.over_task = NULL;
        if (t->state == WS_MINIMIZED || t != wm_active()) wm_show(t);
    }
}

void dnd_test_move(int x, int y) {
    if (!D.active) return;
    D.x = CLAMP(x, 0, screen_w() - 1);
    D.y = CLAMP(y, 0, screen_h() - 1);
    update_target(0);
}

void dnd_test_drop(void) {
    if (D.active) drop();
}

/* ---------------- the image ---------------- */

static void badge_copy(gc_t *g, int x, int y) {
    gfx_fill(g, mkrect(x, y, 11, 11), HEX(0xFFFFFF));
    gfx_rect(g, mkrect(x, y, 11, 11), HEX(0x000000));
    gfx_fill(g, mkrect(x + 5, y + 2, 1, 7), HEX(0x000000));
    gfx_fill(g, mkrect(x + 2, y + 5, 7, 1), HEX(0x000000));
}

/* a no-entry sign: a red ring with a bar */
static void badge_none(gc_t *g, int cx, int cy) {
    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > 56) continue;
            color_t c = d2 >= 26 ? HEX(0xD02010) : HEX(0xFFFFFF);
            if (d2 < 26 && (dx + dy >= -1 && dx + dy <= 1)) c = HEX(0xD02010);
            if (d2 >= 49) c = HEX(0x701008);
            gfx_pixel(g, cx + dx, cy + dy, c);
        }
}

void dnd_paint(gc_t *g, rect_t clip) {
    if (!D.active || rect_empty(rect_intersect(clip, D.drawn))) return;
    int x = D.x, y = D.y;
    /* the first item's icon, half transparent, below and right of the pointer */
    gfx_icon_alpha(g, x + 14, y + 22, &icons32[D.icon], D.effect ? 180 : 110);
    if (D.count > 1) {
        char n[12];
        snprintf(n, sizeof n, "%d", D.count);
        int tw = gfx_text_width(&font_ui_bold, n);
        rect_t b = mkrect(x + 38, y + 16, MAX(18, tw + 10), 16);
        gfx_rounded(g, b, 7, 15, HEX(0x5C9AF0), HEX(0x1E58C0));
        gfx_rounded_outline(g, b, 7, 15, HEX(0xFFFFFF));
        gfx_text_box(g, &font_ui_bold, b, n, HEX(0xFFFFFF), 1);
    }
    if (!D.effect) badge_none(g, x + 16, y + 20);
    else if (D.effect == DND_COPY) badge_copy(g, x + 11, y + 14);
}
