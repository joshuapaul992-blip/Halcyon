/* Halcyon OS - windows and drawing for user programs (SYS_WIN_*, SYS_GFX_*,
 * SYS_CTL_*, SYS_MENU_*, message boxes, clipboard).
 *
 * Each program window has a canvas in the kernel: drawing calls paint into it
 * and the window manager copies it into the window whenever the window is
 * drawn (retained, like a GDI window with a backing bitmap). Input arrives in
 * a per-process event queue. Window-manager changes run on the GUI thread
 * (gui_call), the drawing and the queue are private to the program.
 *
 * A menu bar is outside the program's client area, as in Win32: the canvas
 * starts below it (u->top), events and control rectangles are moved by it,
 * and adding one grows the window instead of shrinking the canvas. */
#include "wm.h"
#include "dnd.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/io.h"
#include "core/unicode.h"
#include "core/thread.h"
#include "core/proc.h"
#include "core/sysabi.h"
#include "fs/vfs.h"
#include "fonts.h"
#include "core/settings.h"

#define MAX_WINDOWS 16
#define QLEN 128
#define MAX_CONTROLS 256                 /* per window */
#define MAX_PIXELS (8u << 20)            /* all canvases of one process (32 MiB) */
#define GUI_RESERVE (32ull << 20)        /* kernel heap the GUI calls never touch */
#define MAX_MSGBOXES 4
#define MENU_LABEL 64                    /* bytes per menu label / shortcut, with the NUL */
#define MENU_SHORTCUT 32
#define MAX_CLIP (1u << 20)              /* SYS_CLIPBOARD_SET_N */
#define STATUS_H_DEFAULT 22

/* a program's menu bar or popup menu, copied into one block */
typedef struct umenu {
    menubar_t bar;          /* bar.menus = menus */
    menu_t *menus;
    menu_item_t *items;
    int nmenus, nitems;
    char *pool;
} umenu_t;

typedef struct uwin {
    window_t *w;            /* NULL once the window is gone */
    proc_t *p;
    int handle;
    surface_t *canvas;
    int timer_ms;
    u64 next_timer;
    u64 close_asked;        /* EVT_CLOSE sent at (ms), to spot a program that ignores it */
    surface_t *mapped;      /* the canvas mapped into the program (SYS_WIN_CANVAS), maybe an old one */
    u64 map_va;
    int ncontrols;
    int top;                /* menu bar height above the canvas */
    umenu_t *bar, *pop;     /* menu bar and the last popup menu (both may be shown) */
    int clip_on, ox, oy;    /* drawing clip rectangle and origin (canvas coordinates) */
    rect_t clip;
    int min_cw, min_ch;     /* minimum client size (SYS_WIN_SET_MIN_SIZE) */
} uwin_t;

/* a program's message box: not modal for the GUI thread, the program waits */
typedef struct umb {
    window_t *w;
    volatile int result;    /* 0 while open */
} umb_t;

typedef struct ustate {
    hal_event_t q[QLEN];
    int qh, qt;
    uwin_t *wins[MAX_WINDOWS];
    umb_t *mbs[MAX_MSGBOXES];
    u64 pixels;             /* canvas pixels in use */
} ustate_t;

static ustate_t *state_of(proc_t *p) {
    if (!p->gui) p->gui = kzalloc(sizeof(ustate_t));
    return p->gui;
}

static void push(proc_t *p, const hal_event_t *ev) {
    ustate_t *s = p ? p->gui : NULL;
    if (!s) return;
    /* coalesce pointer motion, scroll bar thumb tracking and drags moving over a window */
    if ((ev->type == EVT_MOUSE_MOVE || (ev->type == EVT_SCROLL && ev->button == SCROLL_THUMB_TRACK) ||
         (ev->type == EVT_DRAG && ev->button == DND_OVER)) && s->qh != s->qt) {
        hal_event_t *last = &s->q[(s->qh + QLEN - 1) % QLEN];
        if (last->type == ev->type && last->win == ev->win && last->id == ev->id && last->button == ev->button) { *last = *ev; return; }
    }
    int n = (s->qh + 1) % QLEN;
    if (n == s->qt) return;                 /* full: the program is not keeping up */
    s->q[s->qh] = *ev;
    s->qh = n;
}

static const font_t *font_of(int f) {
    static const font_t *const fonts[] = { &font_ui, &font_ui_bold, &font_title, &font_mono, &font_large, &font_huge, &font_small };
    return (f >= 0 && f < (int)ARRAY_SIZE(fonts)) ? fonts[f] : &font_ui;
}

/* ---- window callbacks (GUI thread) ---- */

static void uw_paint(window_t *w, gc_t *g) {
    uwin_t *u = w->data;
    if (u && u->canvas) gfx_blit(g, 0, u->top, u->canvas, mkrect(0, 0, u->canvas->w, u->canvas->h));
}

static void uw_event(window_t *w, gui_event_t *e) {
    uwin_t *u = w->data;
    if (!u || !u->p) return;
    hal_event_t ev = { 0 };
    ev.win = u->handle;
    ev.x = e->x; ev.y = (e->type == GE_KEY || e->type == GE_FOCUS || e->type == GE_BLUR) ? e->y : e->y - u->top;
    ev.button = e->button; ev.buttons = e->buttons; ev.dz = e->dz;
    ev.mods = input_mods();
    switch (e->type) {
    case GE_MOUSE_DOWN: ev.type = EVT_MOUSE_DOWN; break;
    case GE_MOUSE_UP: ev.type = EVT_MOUSE_UP; break;
    case GE_MOUSE_MOVE: ev.type = EVT_MOUSE_MOVE; break;
    case GE_DBLCLICK: ev.type = EVT_DBLCLICK; break;
    case GE_WHEEL: ev.type = EVT_WHEEL; break;
    case GE_FOCUS: ev.type = EVT_FOCUS; ev.pressed = 1; break;
    case GE_BLUR: ev.type = EVT_FOCUS; ev.pressed = 0; break;
    case GE_MOUSE_LEAVE: ev.type = EVT_MOUSE_LEAVE; break;
    case GE_KEY:
        ev.type = EVT_KEY;
        ev.id = e->key.key;
        ev.pressed = e->key.pressed;
        ev.mods = e->key.mods;
        ev.ch = e->key.ch;
        ev.vk = e->key.vk;
        break;
    default: return;
    }
    push(u->p, &ev);
}

static void uw_notify(window_t *w, widget_t *wd, int type, int code) {
    uwin_t *u = w->data;
    if (!u || !u->p) return;
    hal_event_t ev = { 0 };
    ev.type = type;
    ev.win = u->handle;
    ev.id = wd->id;
    ev.button = code;
    if (type == EVT_SCROLL) ev.x = wd->value;
    push(u->p, &ev);
}
static void uw_click(window_t *w, widget_t *wd) { uw_notify(w, wd, EVT_COMMAND, CMD_CLICK); }
static void uw_activate(window_t *w, widget_t *wd) { uw_notify(w, wd, EVT_COMMAND, CMD_ACTIVATE); }
static void uw_changed(window_t *w, widget_t *wd) { uw_notify(w, wd, EVT_COMMAND, CMD_CHANGE); }
static void uw_scroll(window_t *w, widget_t *wd, int part) { uw_notify(w, wd, EVT_SCROLL, part); }

static void uw_menu(window_t *w, int id) {
    uwin_t *u = w->data;
    if (!u || !u->p) return;
    hal_event_t ev = { 0 };
    ev.type = EVT_MENU;
    ev.win = u->handle;
    ev.id = id;
    push(u->p, &ev);
}

static void uw_tick(window_t *w) { widgets_tick(w); }

/* drag and drop: the program answers DND_ENTER / DND_OVER with SYS_DND_ACCEPT */
static int uw_dnd(window_t *w, dnd_event_t *e) {
    uwin_t *u = w->data;
    if (!u || !u->p) return DND_ANSWER(DND_NONE, 0);
    hal_event_t ev = { 0 };
    ev.type = EVT_DRAG;
    ev.win = u->handle;
    ev.button = e->type;
    ev.x = e->x;
    ev.y = e->y - u->top;
    ev.buttons = e->button;
    ev.mods = e->mods;
    ev.id = (int)e->serial;
    ev.dz = e->count;
    ev.vk = e->allowed;
    ev.pressed = e->effect;
    push(u->p, &ev);
    return e->type == DND_ENTER || e->type == DND_OVER ? DND_LATER : 0;
}

/* status bars sit at the bottom of the client area */
static void dock(window_t *w) {
    for (widget_t *wd = w->widgets; wd; wd = wd->next)
        if (wd->type == W_STATUSBAR) wd->r = mkrect(0, w->client->h - wd->r.h, w->client->w, wd->r.h);
}

/* the frame's minimum size from the program's minimum client size */
static void set_min(uwin_t *u) {
    window_t *w = u->w;
    w->min_w = MAX(160, u->min_cw + 2 * BORDER);
    w->min_h = MAX(80, u->min_ch + u->top + TITLE_H + BORDER);
}

/* after a size change: keep the window on screen (like Sonar's level change) */
static void keep_on_screen(window_t *w) {
    rect_t f = w->frame, g = wm_keep_on_monitor(f);
    int nx = g.x, ny = g.y;
    if (nx != f.x || ny != f.y) {
        wm_invalidate_rect(mkrect(f.x, f.y, f.w + 8, f.h + 8));
        w->frame.x = nx; w->frame.y = ny;
        wm_invalidate_rect(mkrect(nx, ny, f.w + 8, f.h + 8));
    }
}

static int uw_close(window_t *w) {
    uwin_t *u = w->data;
    if (!u || !u->p) return 1;
    u64 now = time_ms();
    if (u->close_asked && now - u->close_asked > 5000) {
        /* asked before and still open: the program is not responding. The
         * box runs a nested loop in which the program may end and u go away,
         * so only the pid is used afterwards. */
        u32 pid = u->p->pid;
        char msg[200];
        snprintf(msg, sizeof msg, "%s is not responding.\n\nIf you end it now, unsaved information will be lost. End it now?", u->p->name);
        if (msgbox(w, w->title, msg, ICON_WARNING, MB_YESNO) == ID_YES) proc_kill(pid, NULL);
        return 0;
    }
    if (!u->close_asked) u->close_asked = now;
    hal_event_t ev = { 0 };
    ev.type = EVT_CLOSE;
    ev.win = u->handle;
    push(u->p, &ev);
    return 0;                               /* the program closes it (win_destroy) */
}

static void uw_resize(window_t *w) {
    uwin_t *u = w->data;
    if (!u || !w->client || !u->p || !u->p->gui) return;
    dock(w);
    ustate_t *s = u->p->gui;
    int cw = w->client->w, ch = MAX(1, w->client->h - u->top);
    if (u->canvas && u->canvas->w == cw && u->canvas->h == ch) return;
    u64 oldpx = u->canvas ? (u64)u->canvas->w * u->canvas->h : 0, newpx = (u64)cw * ch;
    if (s->pixels - oldpx + newpx > MAX_PIXELS || mm_heap_free() < GUI_RESERVE + newpx * 4) return;
    surface_t *n = surface_new_paged(cw, ch);
    if (!n) return;
    s->pixels = s->pixels - oldpx + newpx;
    gc_t g = gc_for(n);
    gfx_fill(&g, mkrect(0, 0, n->w, n->h), g_theme->face);
    if (u->canvas) {
        gfx_blit(&g, 0, 0, u->canvas, mkrect(0, 0, u->canvas->w, u->canvas->h));
        /* still mapped into the program: freed when it maps the new one */
        if (u->canvas != u->mapped) surface_free(u->canvas);
    }
    u->canvas = n;
    if (w->direct) w->direct = n;
    hal_event_t ev = { 0 };
    ev.win = u->handle;
    ev.x = n->w; ev.y = n->h;
    ev.type = EVT_RESIZE;
    push(u->p, &ev);
    ev.type = EVT_PAINT;
    push(u->p, &ev);
}

static void uw_destroyed(window_t *w) {
    uwin_t *u = w->data;
    if (u) u->w = NULL;
}

/* ---- jobs run on the GUI thread ---- */

typedef struct { uwin_t *u; char title[96]; int cw, ch, flags; int ok; window_t *host; rect_t box; } create_job_t;
static void job_create(void *a) {
    create_job_t *j = a;
    u32 f = 0;
    if (j->flags & WIN_RESIZABLE) f |= WF_RESIZABLE;
    if (j->flags & WIN_DIALOG) f |= WF_DIALOG | WF_CENTER;
    if (j->flags & WIN_NO_MAX) f |= WF_NO_MAXIMIZE;
    /* full screen (screen savers, video): above the taskbar; the running
     * screen saver's above everything (gui/saver.c) */
    if (j->flags & WIN_FULLSCREEN) {
        f = WF_FULLSCREEN | WF_TOPMOST | WF_NOSHADOW;
        if (saver_is_saver_pid(j->u->p->pid)) f |= WF_SAVER;
    }
    if (j->host) {                          /* a screen saver's preview, inside Display Properties */
        if (!wm_alive(j->host)) return;
        f = WF_EMBED | WF_DIALOG | WF_NOSHADOW;
    }
    window_t *w = wm_create(j->title, ICON_FILE_APP, -1, -1, j->cw, j->ch, f);
    if (!w) return;
    if (j->host) { w->owner = j->host; w->restore = j->box; }
    if (f & WF_FULLSCREEN) w->direct = j->u->canvas;   /* drawn straight from the canvas */
    j->u->w = w;
    w->data = j->u;
    w->on_paint = uw_paint;
    w->on_event = uw_event;
    w->on_close = uw_close;
    w->on_resize = uw_resize;
    w->on_destroy = uw_destroyed;
    w->on_menu = uw_menu;
    w->on_dnd = uw_dnd;
    j->ok = 1;
}

static void job_destroy(void *a) {
    uwin_t *u = a;
    if (u->w) {
        menu_close_owned(u->w);
        u->w->menubar = NULL;
        u->w->on_close = NULL; u->w->data = NULL; wm_destroy(u->w);
    }
    u->w = NULL;
}

/* the place and size the program's window had last time */
static void job_place(void *a) {
    uwin_t *u = a;
    if (!u->w || !u->p) return;
    char key[40];
    settings_prog_prefix(u->p->name, key, sizeof key);
    wm_place_restore(u->w, key);
}

/* new client size; keep the window on screen (like Sonar's level change) */
typedef struct { uwin_t *u; int cw, ch; } resize_job_t;
static void job_resize(void *a) {
    resize_job_t *j = a;
    window_t *w = j->u->w;
    if (!w || !w->client || (w->client->w == j->cw && w->client->h == j->ch + j->u->top)) return;
    wm_resize_client(w, j->cw, j->ch + j->u->top);
    keep_on_screen(w);
}

/* CTL_NOTIFY: Enter / double click report CMD_ACTIVATE (otherwise Enter presses the default button) */
static void set_notify(widget_t *wd) {
    if (wd->type == W_TEXTBOX || wd->type == W_LISTBOX) wd->on_activate = (wd->style & CTL_NOTIFY) ? uw_activate : NULL;
}

typedef struct { uwin_t *u; int type; rect_t r; char text[256]; int id, style; } ctl_job_t;
static void job_ctl(void *a) {
    ctl_job_t *j = a;
    window_t *w = j->u->w;
    if (!w) return;
    static const int types[] = { 0, W_BUTTON, W_LABEL, W_TEXTBOX, W_CHECKBOX, W_LISTBOX, W_GROUP, W_PROGRESS,
                                 W_VSCROLL, W_HSCROLL, W_TOOLBUTTON, W_STATUSBAR };
    if (j->u->ncontrols >= MAX_CONTROLS || mm_heap_free() < GUI_RESERVE) return;
    rect_t r = j->r;
    r.y += j->u->top;
    if (j->type == CTL_STATUSBAR) r.h = r.h > 0 ? CLAMP(r.h, 16, 48) : STATUS_H_DEFAULT;
    widget_t *wd = widget_add(w, types[j->type], r, j->text, j->id);
    if (!wd) return;
    j->u->ncontrols++;
    wd->style |= (u32)j->style;
    if (j->type == CTL_BUTTON || j->type == CTL_TOOLBUTTON) wd->on_click = uw_click;
    if (j->type == CTL_CHECKBOX || j->type == CTL_LISTBOX) wd->on_change = uw_click;
    if (j->type == CTL_TEXTBOX && (j->style & CTL_CHANGES)) wd->on_change = uw_changed;
    if (j->type == CTL_TEXTBOX && !w->focus) w->focus = wd;
    if (j->type == CTL_VSCROLL || j->type == CTL_HSCROLL) { wd->on_scroll = uw_scroll; w->on_tick = uw_tick; }
    if (j->type == CTL_STATUSBAR) dock(w);
    set_notify(wd);
}

/* ---- menus ---- */

static void umenu_free(umenu_t *m) { kfree(m); }

/* copy a program's flat item array; bar: MENU_TITLE entries start menus */
static long umenu_build(u64 uitems, u64 count, int bar, umenu_t **out) {
    if (count == 0 || count > MENU_MAX_ITEMS) return E_INVAL;
    if (mm_heap_free() < GUI_RESERVE + (64u << 10)) return E_NOMEM;
    hal_menu_item_t *in = kmalloc(sizeof *in * count);
    if (!in) return E_NOMEM;
    if (copyin(in, uitems, sizeof *in * count)) { kfree(in); return E_INVAL; }
    int nmenus = 0;
    for (u64 i = 0; i < count; i++) if (in[i].flags & MENU_TITLE) nmenus++;
    if (bar ? (nmenus > MENU_MAX_TITLES || !(in[0].flags & MENU_TITLE)) : nmenus != 0) { kfree(in); return E_INVAL; }
    if (!bar) nmenus = 1;
    int nitems = (int)count - (bar ? nmenus : 0);
    u64 size = sizeof(umenu_t) + sizeof(menu_t) * (u64)nmenus + sizeof(menu_item_t) * (u64)(nitems + 1) +
               (u64)count * (MENU_LABEL + MENU_SHORTCUT);
    umenu_t *m = kzalloc(size);
    if (!m) { kfree(in); return E_NOMEM; }
    m->menus = (menu_t *)(m + 1);
    m->items = (menu_item_t *)(m->menus + nmenus);
    m->pool = (char *)(m->items + nitems + 1);
    m->nmenus = nmenus; m->nitems = nitems;
    char *pool = m->pool;
    int mi = bar ? -1 : 0, ii = 0;
    if (!bar) { m->menus[0].title = ""; m->menus[0].items = m->items; }
    long r = 0;
    for (u64 i = 0; i < count && !r; i++) {
        hal_menu_item_t *it = &in[i];
        char *label = pool;
        pool += MENU_LABEL;
        if (!(it->flags & MENU_SEPARATOR) && it->label && copyin_str(label, (u64)it->label, MENU_LABEL) == E_INVAL) r = E_INVAL;
        if (it->flags & MENU_TITLE) {
            mi++;
            m->menus[mi].title = label;
            m->menus[mi].items = m->items + ii;
            m->menus[mi].count = 0;
            pool += MENU_SHORTCUT;
            continue;
        }
        menu_item_t *k = &m->items[ii++];
        k->label = label;
        k->id = it->id;
        k->flags = (u32)it->flags & (MI_SEPARATOR | MI_DISABLED | MI_CHECKED | MI_BOLD);
        k->icon = it->icon > 0 && it->icon < ICON_COUNT ? it->icon : 0;
        if (it->shortcut && !(it->flags & MENU_SEPARATOR)) {
            if (copyin_str(pool, (u64)it->shortcut, MENU_SHORTCUT) == E_INVAL) r = E_INVAL;
            if (pool[0]) k->shortcut = pool;
        }
        pool += MENU_SHORTCUT;
        m->menus[mi].count++;
    }
    kfree(in);
    if (r) { kfree(m); return r; }
    m->bar.menus = m->menus;
    m->bar.count = nmenus;
    m->bar.open = m->bar.hover = -1;
    *out = m;
    return 0;
}

typedef struct { uwin_t *u; umenu_t *m, *old; int x, y, ok; } menu_job_t;

/* set, replace or remove (m NULL) the menu bar */
static void job_menubar(void *a) {
    menu_job_t *j = a;
    uwin_t *u = j->u;
    window_t *w = u->w;
    if (!w || !w->client) return;
    menu_close_owned(w);             /* an open menu may show the old items */
    j->old = u->bar;
    u->bar = j->m;
    w->menubar = j->m ? &j->m->bar : NULL;
    j->ok = 1;
    int top = j->m ? MENUBAR_H : 0, d = top - u->top;
    if (d) {
        for (widget_t *wd = w->widgets; wd; wd = wd->next) wd->r.y += d;
        u->top = top;
        w->place_top = top;              /* the saved size is the canvas's */
        set_min(u);
        /* grow or shrink the window so the canvas keeps its size (not when maximized) */
        if (w->state == WS_NORMAL) {
            wm_resize_client(w, w->client->w, w->client->h + d);
            keep_on_screen(w);
        }
        uw_resize(w);
    }
    wm_invalidate(w);
}

static void job_popup(void *a) {
    menu_job_t *j = a;
    uwin_t *u = j->u;
    window_t *w = u->w;
    if (!w || wm_active() != w) return;          /* only over the window the user is working in */
    rect_t c = wm_client_rect(w);
    menu_popup(w, &j->m->menus[0], c.x + j->x, c.y + u->top + j->y);   /* closes the previous one */
    j->old = u->pop;
    u->pop = j->m;
    j->ok = 1;
}

/* a modal or owned dialog: owner == NULL makes it free again */
typedef struct { uwin_t *u, *owner; int modal, ok; } owner_job_t;
static void job_owner(void *a) {
    owner_job_t *j = a;
    window_t *w = j->u->w, *o = j->owner ? j->owner->w : NULL;
    if (!w || (j->owner && !o)) return;
    if (w->flags & WF_EMBED) return;         /* a preview stays in its box */
    /* no cycles: the owner chain must not lead back to w */
    for (window_t *x = o; x; x = x->owner) if (x == w) return;
    if (w->owner && wm_alive(w->owner) && w->owner->modal == w) w->owner->modal = NULL;
    w->owner = o;
    if (o && j->modal) o->modal = w;
    j->ok = 1;
}

typedef struct { umb_t *mb; char title[96]; char text[1024]; int icon, buttons; } mb_job_t;
/* the program waiting in SYS_MSGBOX wakes and destroys the box (job_mb_destroy) */
static void mb_click(window_t *w, widget_t *wd) {
    umb_t *mb = w->data;
    if (mb && !mb->result) mb->result = wd->id;
}
static int mb_close(window_t *w) {
    umb_t *mb = w->data;
    if (mb && !mb->result) mb->result = ID_CANCEL;
    return 0;
}
static void job_msgbox(void *a) {
    mb_job_t *j = a;
    window_t *w = msgbox_window(j->title, j->text, j->icon, j->buttons, mb_click);
    if (!w) { j->mb->result = ID_CANCEL; return; }
    w->data = j->mb;
    w->on_close = mb_close;
    j->mb->w = w;
}
static void job_mb_destroy(void *a) {
    umb_t *mb = a;
    if (mb->w && wm_alive(mb->w)) { mb->w->data = NULL; mb->w->on_close = NULL; wm_destroy(mb->w); }
    mb->w = NULL;
}
static int mb_answered(void *a) { return ((umb_t *)a)->result != 0; }

typedef struct { window_t *owner; int save; char title[96], dir[VFS_MAX_PATH], ext[16], out[VFS_MAX_PATH]; int ok; } fd_job_t;
static int filedlg_open;            /* one program file dialog at a time (it is modal) */
static void job_filedlg(void *a) {
    fd_job_t *j = a;
    j->ok = file_dialog(j->owner, j->save, j->title, j->dir[0] ? j->dir : NULL, j->ext[0] ? j->ext : NULL, j->out, sizeof j->out);
}

/* ---- helpers for the system calls ---- */

static uwin_t *uwin(proc_t *p, int h) {
    uwin_t *u = handle_get(p, h, H_WINDOW);
    return u && u->w ? u : NULL;
}

/* drop the program's view of the canvas (and an old canvas kept for it) */
static void canvas_unmap(proc_t *p, uwin_t *u) {
    if (u->map_va) vm_unmap_shared(p, u->map_va);
    if (u->mapped && u->mapped != u->canvas) surface_free(u->mapped);
    u->mapped = NULL;
    u->map_va = 0;
}

static void uwin_free(proc_t *p, uwin_t *u) {
    gui_call(job_destroy, u);
    canvas_unmap(p, u);
    umenu_free(u->bar);
    umenu_free(u->pop);
    ustate_t *s = p->gui;
    if (s) for (int i = 0; i < MAX_WINDOWS; i++) if (s->wins[i] == u) s->wins[i] = NULL;
    handle_free(p, u->handle);
    if (u->canvas) {
        if (s) s->pixels -= (u64)u->canvas->w * u->canvas->h;
        surface_free(u->canvas);
    }
    kfree(u);
}

static int queue_ready(void *arg) {
    proc_t *p = arg;
    ustate_t *s = p->gui;
    if (!s) return 0;
    if (s->qh != s->qt) return 1;
    u64 now = time_ms();
    for (int i = 0; i < MAX_WINDOWS; i++) if (s->wins[i] && s->wins[i]->timer_ms && now >= s->wins[i]->next_timer) return 1;
    return 0;
}

/* poll() on window events (net/socket.c): ready now; *next_ms = the next window timer (time_ms) */
int usergui_poll_ready(proc_t *p, u64 *next_ms) {
    ustate_t *s = p->gui;
    *next_ms = ~0ull;
    for (int i = 0; s && i < MAX_WINDOWS; i++)
        if (s->wins[i] && s->wins[i]->timer_ms && s->wins[i]->next_timer < *next_ms) *next_ms = s->wins[i]->next_timer;
    return queue_ready(p);
}

static long get_event(proc_t *p, u64 uev, long timeout) {
    ustate_t *s = state_of(p);
    if (!s) return E_NOMEM;
    if (!user_writable(uev, sizeof(hal_event_t))) return E_INVAL;
    u64 deadline = timeout < 0 ? ~0ull : time_ms_to_tsc(time_ms() + (u64)timeout);
    for (;;) {
        hal_event_t ev;
        if (s->qh != s->qt) {
            ev = s->q[s->qt];
            s->qt = (s->qt + 1) % QLEN;
            if (ev.type == EVT_CLOSE) {
                uwin_t *u = uwin(p, ev.win);
                if (u) u->close_asked = 0;      /* the program has it: it may decline */
            }
            return copyout(uev, &ev, sizeof ev) ? E_INVAL : 1;
        }
        u64 now = time_ms(), next = ~0ull;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            uwin_t *u = s->wins[i];
            if (!u || !u->timer_ms) continue;
            if (now >= u->next_timer) {
                u->next_timer = now + (u64)u->timer_ms;
                memset(&ev, 0, sizeof ev);
                ev.type = EVT_TIMER;
                ev.win = u->handle;
                return copyout(uev, &ev, sizeof ev) ? E_INVAL : 1;
            }
            if (u->next_timer < next) next = u->next_timer;
        }
        u64 dl = deadline;
        if (next != ~0ull && time_ms_to_tsc(next) < dl) dl = time_ms_to_tsc(next);
        if (rdtsc() >= deadline) return 0;
        int r = proc_wait(queue_ready, p, dl);
        if (r < 0) return r;
    }
}

/* the user's canvas as a graphics context, with the program's clip rectangle and origin */
static int canvas_gc(proc_t *p, int h, gc_t *g, uwin_t **out) {
    uwin_t *u = uwin(p, h);
    if (!u || !u->canvas) return E_INVAL;
    *g = gc_for(u->canvas);
    if (u->clip_on) {
        g->clip = rect_intersect(g->clip, u->clip);
        g->ox = u->ox; g->oy = u->oy;
    }
    if (out) *out = u;
    return 0;
}

static rect_t unpack_rect(u64 geom) { return mkrect((i16)(geom >> 48), (i16)(geom >> 32), (i16)(geom >> 16), (i16)geom); }

static color_t col(u64 c) { return HEX((u32)c & 0xFFFFFF); }

static long usergui_syscall_more(proc_t *p, int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

long usergui_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    proc_t *p = proc_current();
    if (!p) return E_INVAL;
    gc_t g;
    uwin_t *u;
    char text[1024];
    switch (nr) {
    case SYS_WIN_CREATE: {
        ustate_t *s = state_of(p);
        if (!s) return E_NOMEM;
        int slot = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) if (!s->wins[i]) { slot = i; break; }
        if (slot < 0) return E_NOMEM;
        create_job_t *j = kzalloc(sizeof *j);
        u = kzalloc(sizeof *u);
        int cw = (int)CLAMP((i64)a2, 32, 2048), ch = (int)CLAMP((i64)a3, 16, 1536);
        if (!j || !u || copyin_str(j->title, a1, sizeof j->title) == E_INVAL) { kfree(j); kfree(u); return E_INVAL; }
        if (a4 & WIN_FULLSCREEN) {          /* the primary monitor; a screen saver every monitor */
            rect_t fr = wm_fullscreen_rect(saver_is_saver_pid(p->pid));
            cw = fr.w; ch = fr.h;
        }
        if (a4 & WIN_PREVIEW) {
            /* only the program Display Properties started for that box */
            j->host = saver_preview_host((int)a5, p->pid, &j->box);
            if (!j->host) { kfree(j); kfree(u); return E_INVAL; }
            cw = j->box.w; ch = j->box.h;
        }
        if (s->pixels + (u64)cw * ch > MAX_PIXELS || mm_heap_free() < GUI_RESERVE + (u64)cw * ch * 8) { kfree(j); kfree(u); return E_NOMEM; }
        u->p = p;
        u->canvas = surface_new_paged(cw, ch);
        if (!u->canvas) { kfree(j); kfree(u); return E_NOMEM; }
        gc_t cg = gc_for(u->canvas);
        gfx_fill(&cg, mkrect(0, 0, cw, ch), g_theme->face);
        u->handle = handle_alloc(p, H_WINDOW, u);
        if (u->handle < 0) { surface_free(u->canvas); kfree(j); kfree(u); return E_NOMEM; }
        s->pixels += (u64)cw * ch;
        j->u = u; j->cw = cw; j->ch = ch; j->flags = (int)a4;
        gui_call(job_create, j);
        int ok = j->ok;
        kfree(j);
        if (!ok) { handle_free(p, u->handle); s->pixels -= (u64)cw * ch; surface_free(u->canvas); kfree(u); return E_NOMEM; }
        s->wins[slot] = u;
        /* a program's main window comes back where it was (window.<program>) */
        if (!(a4 & (WIN_DIALOG | WIN_FULLSCREEN | WIN_PREVIEW))) gui_call(job_place, u);
        hal_event_t ev = { 0 };
        ev.type = EVT_PAINT;
        ev.win = u->handle;
        push(p, &ev);
        return u->handle;
    }
    case SYS_WIN_DESTROY:
        u = handle_get(p, (int)a1, H_WINDOW);
        if (!u) return E_INVAL;
        uwin_free(p, u);
        return 0;
    case SYS_WIN_SET_TITLE:
        if (!(u = uwin(p, (int)a1)) || copyin_str(text, a2, 96) == E_INVAL) return E_INVAL;
        wm_set_title(u->w, text);
        return 0;
    case SYS_WIN_INVALIDATE:
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        wm_invalidate(u->w);
        return 0;
    case SYS_WIN_GET_EVENT: return get_event(p, a1, (long)a2);
    case SYS_WIN_CANVAS: {
        /* map the canvas into the program; drawing calls and direct pixel
         * writes then go to the same memory */
        if (!(u = uwin(p, (int)a1)) || !u->canvas || !user_writable(a2, sizeof(hal_canvas_t))) return E_INVAL;
        if (u->mapped != u->canvas || !u->map_va) {
            canvas_unmap(p, u);
            u64 size = ((u64)u->canvas->w * u->canvas->h * 4 + 4095) & ~4095ull;
            u64 va = vm_map_shared(p, u->canvas->px, size, 1);
            if (!va) return E_NOMEM;
            u->mapped = u->canvas;
            u->map_va = va;
        }
        hal_canvas_t c = { (unsigned int *)(uptr)u->map_va, u->canvas->w, u->canvas->h, u->canvas->stride };
        return copyout(a2, &c, sizeof c) ? E_INVAL : 0;
    }
    case SYS_WIN_SIZE:
        if (!(u = uwin(p, (int)a1)) || !u->canvas) return E_INVAL;
        return ((long)u->canvas->w << 16) | u->canvas->h;
    case SYS_WIN_SET_TIMER:
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        u->timer_ms = (int)MIN(a2, 3600000ull);
        if (u->timer_ms && u->timer_ms < 10) u->timer_ms = 10;
        u->next_timer = time_ms() + (u64)u->timer_ms;
        return 0;
    case SYS_WIN_SHOW:
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        wm_show(u->w);
        return 0;
    case SYS_WIN_RESIZE: {
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        if (u->w->flags & (WF_FULLSCREEN | WF_EMBED)) return E_INVAL;   /* their size is the screen's / the box's */
        resize_job_t j = { u, (int)CLAMP((i64)a2, 32, 2048), (int)CLAMP((i64)a3, 16, 1536) };
        ustate_t *s = p->gui;
        u64 oldpx = u->canvas ? (u64)u->canvas->w * u->canvas->h : 0, newpx = (u64)j.cw * j.ch;
        if (!s || s->pixels - oldpx + newpx > MAX_PIXELS) return E_NOMEM;
        gui_call(job_resize, &j);
        return 0;
    }
    case SYS_WIN_SET_ICON:
        if (!(u = uwin(p, (int)a1)) || (i64)a2 < 0 || (i64)a2 >= ICON_COUNT) return E_INVAL;
        u->w->icon = (int)a2;
        wm_invalidate_rect(mkrect(u->w->frame.x, u->w->frame.y, u->w->frame.w, TITLE_H));
        taskbar_invalidate();
        return 0;

    case SYS_GFX_FILL: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_fill(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), col(a6)); return 0;
    case SYS_GFX_RECT: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_rect(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), col(a6)); return 0;
    case SYS_GFX_LINE: {
        /* bounded work: coordinates far outside any canvas are clamped */
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        int x0 = (int)CLAMP((i64)(int)a2, -8192, 8192), y0 = (int)CLAMP((i64)(int)a3, -8192, 8192);
        int x1 = (int)CLAMP((i64)(int)a4, -8192, 8192), y1 = (int)CLAMP((i64)(int)a5, -8192, 8192);
        gfx_line(&g, x0, y0, x1, y1, col(a6));
        return 0;
    }
    case SYS_GFX_BEVEL: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_bevel(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), (int)a6); return 0;
    case SYS_GFX_CIRCLE: {
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        int cx = (int)CLAMP((i64)(int)a2, -8192, 8192), cy = (int)CLAMP((i64)(int)a3, -8192, 8192);
        int rad = (int)CLAMP((i64)(int)a4, 0, 2048);
        if (a6) {
            /* filled: one clipped span per scan line */
            int dx = rad;
            for (int dy = 0; dy <= rad; dy++) {
                while (dx > 0 && dx * dx + dy * dy > rad * rad) dx--;
                gfx_fill(&g, mkrect(cx - dx, cy - dy, 2 * dx + 1, 1), col(a5));
                if (dy) gfx_fill(&g, mkrect(cx - dx, cy + dy, 2 * dx + 1, 1), col(a5));
            }
        } else {
            gfx_circle(&g, cx, cy, rad, col(a5));
        }
        return 0;
    }
    case SYS_GFX_TEXT:
        if (canvas_gc(p, (int)a1, &g, NULL) || copyin_str(text, a4, sizeof text) == E_INVAL) return E_INVAL;
        return gfx_text(&g, font_of((int)a6), (int)a2, (int)a3, text, col(a5));
    case SYS_GFX_TEXT_WIDTH:
        if (copyin_str(text, a1, sizeof text) == E_INVAL) return E_INVAL;
        return gfx_text_width(font_of((int)a2), text);
    case SYS_GFX_FONT_HEIGHT: return font_of((int)a1)->height;
    case SYS_GFX_ICON: {
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        int id = (int)(a4 & 0xFFFF), sz = (int)(a4 >> 16);
        if (id < 0 || id >= ICON_COUNT) return E_INVAL;
        gfx_icon(&g, (int)a2, (int)a3, sz == 1 ? &icons16[id] : sz == 2 ? &icons48[id] : &icons32[id]);
        return 0;
    }
    case SYS_GFX_BLIT: {
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        int w = (int)a4, h = (int)a5;
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (u64)w * (u64)h > 4ull << 20) return E_INVAL;
        surface_t *s = surface_new(w, h);
        if (!s) return E_NOMEM;
        int r = 0;
        for (int y = 0; y < h && !r; y++) {
            r = copyin(s->px + y * s->stride, a6 + (u64)y * (u64)w * 4, (u64)w * 4);
            for (int x = 0; x < w; x++) s->px[y * s->stride + x] |= 0xFF000000u;
        }
        if (!r) gfx_blit(&g, (int)a2, (int)a3, s, mkrect(0, 0, w, h));
        surface_free(s);
        return r;
    }

    case SYS_CTL_CREATE: {
        if (!(u = uwin(p, (int)a1)) || a2 < CTL_BUTTON || a2 > CTL_STATUSBAR) return E_INVAL;
        if (u->ncontrols >= MAX_CONTROLS) return E_NOMEM;
        ctl_job_t *j = kzalloc(sizeof *j);
        if (!j) return E_NOMEM;
        if (a4 && copyin_str(j->text, a4, sizeof j->text) == E_INVAL) { kfree(j); return E_INVAL; }
        j->u = u; j->type = (int)a2;
        j->r = unpack_rect(a3);
        j->id = (int)a5; j->style = (int)(a6 & 0x3FF);
        gui_call(job_ctl, j);
        kfree(j);
        wm_invalidate(u->w);
        return 0;
    }
    case SYS_CTL_SET_TEXT: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2)) || copyin_str(text, a3, 256) == E_INVAL) return E_INVAL;
        widget_set_text(u->w, wd, text);
        return 0;
    }
    case SYS_CTL_GET_TEXT: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2)) || (i64)a4 <= 0) return E_INVAL;
        u64 n = MIN(strlen(wd->text) + 1, a4);
        char tmp[sizeof wd->text];
        memcpy(tmp, wd->text, n);
        tmp[n - 1] = 0;
        return copyout(a3, tmp, n) ? E_INVAL : (long)n - 1;
    }
    case SYS_CTL_SET_VALUE: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2))) return E_INVAL;
        if (wd->type == W_VSCROLL || wd->type == W_HSCROLL) widget_scroll_set(u->w, wd, wd->total, wd->page, (int)a3, wd->line);
        else wd->value = (int)a3;
        wm_invalidate(u->w);
        return 0;
    }
    case SYS_CTL_GET_VALUE: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2))) return E_INVAL;
        return wd->value;
    }
    case SYS_CTL_SET_STYLE: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2))) return E_INVAL;
        wd->style = (wd->style & ~0x1FFu) | (u32)(a3 & 0x1FF);
        set_notify(wd);
        wm_invalidate(u->w);
        return 0;
    }
    case SYS_CTL_LIST_SET: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2)) || a4 > 4096) return E_INVAL;
        int n = (int)a4;
        char **items = kzalloc(sizeof(char *) * (size_t)(n + 1));
        int r = items ? 0 : E_NOMEM;
        for (int i = 0; i < n && !r; i++) {
            u64 ptr;
            items[i] = kmalloc(256);
            if (!items[i] || copyin(&ptr, a3 + (u64)i * 8, 8) || copyin_str(items[i], ptr, 256) == E_INVAL) r = E_INVAL;
        }
        if (!r) widget_list_set(u->w, wd, items, n);
        for (int i = 0; i < n && items; i++) kfree(items[i]);
        kfree(items);
        return r;
    }

    case SYS_MSGBOX: {
        /* the box is an ordinary window; this thread waits for the answer,
         * so the GUI never nests a loop for a program and ending the
         * program ends the wait */
        ustate_t *s = state_of(p);
        if (!s) return E_NOMEM;
        int slot = -1;
        for (int i = 0; i < MAX_MSGBOXES; i++) if (!s->mbs[i]) { slot = i; break; }
        if (slot < 0) return E_BUSY;
        mb_job_t *j = kzalloc(sizeof *j);
        umb_t *mb = kzalloc(sizeof *mb);
        if (!j || !mb) { kfree(j); kfree(mb); return E_NOMEM; }
        if (copyin_str(j->title, a2, sizeof j->title) == E_INVAL || copyin_str(j->text, a3, sizeof j->text) == E_INVAL) {
            kfree(j); kfree(mb); return E_INVAL;
        }
        static const int icons[] = { ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_QUESTION };
        j->icon = icons[a4 < 4 ? a4 : 0];
        j->buttons = (int)CLAMP((i64)a5, MB_OK, MB_YESNOCANCEL);
        j->mb = mb;
        s->mbs[slot] = mb;
        gui_call(job_msgbox, j);
        kfree(j);
        int r = proc_wait(mb_answered, mb, ~0ull);
        int result = mb->result;
        if (p->gui == s && s->mbs[slot] == mb) {          /* not already released */
            gui_call(job_mb_destroy, mb);
            s->mbs[slot] = NULL;
            kfree(mb);
        }
        return r < 0 ? r : result;
    }
    case SYS_FILE_DIALOG: {
        fd_job_t *j = kzalloc(sizeof *j);
        if (!j) return E_NOMEM;
        u = uwin(p, (int)a1);
        j->owner = u ? u->w : NULL;
        j->save = (int)a2;
        int bad = copyin_str(j->title, a3, sizeof j->title) == E_INVAL ||
                  (a4 && copyin_str(j->dir, a4, sizeof j->dir) == E_INVAL) ||
                  (a5 && copyin_str(j->ext, a5, sizeof j->ext) == E_INVAL) || !user_writable(a6, VFS_MAX_PATH);
        if (bad) { kfree(j); return E_INVAL; }
        if (filedlg_open) { kfree(j); return E_BUSY; }
        /* the folder the program's last Open / Save dialog ended in, kept as
         * dialog.<program>.folder: used when the program asks for no folder
         * or the default one (My Documents) */
        char key[64], pre[40];
        settings_prog_prefix(p->name, pre, sizeof pre);
        snprintf(key, sizeof key, "dialog.%s.folder", pre);
        const char *last = settings_get(key, NULL);
        vfs_dirent_t st;
        if (last && (!j->dir[0] || !strcasecmp(j->dir, "C:/Documents")) && !vfs_stat(last, &st) && st.is_dir)
            strlcpy(j->dir, last, sizeof j->dir);
        filedlg_open = 1;
        gui_call(job_filedlg, j);
        filedlg_open = 0;
        if (j->ok) {
            char d[VFS_MAX_PATH];
            vfs_dirname(j->out, d, sizeof d);
            if (d[0] && strlen(d) < SETTINGS_VAL_MAX) { settings_set(key, d); settings_save_later(); }
        }
        int r = j->ok ? (copyout(a6, j->out, strlen(j->out) + 1) ? E_INVAL : 1) : 0;
        kfree(j);
        return r;
    }
    case SYS_CLIPBOARD_SET:
        if (copyin_str(text, a1, sizeof text) == E_INVAL) return E_INVAL;
        clipboard_set(text, (int)strlen(text));
        return 0;
    case SYS_CLIPBOARD_SET_N: {
        if (a2 > MAX_CLIP) return E_INVAL;
        if (mm_heap_free() < GUI_RESERVE + 2 * a2) return E_NOMEM;
        char *buf = kmalloc(a2 + 1);
        if (!buf) return E_NOMEM;
        if (a2 && copyin(buf, a1, a2)) { kfree(buf); return E_INVAL; }
        clipboard_set(buf, (int)a2);
        kfree(buf);
        return 0;
    }
    case SYS_CLIPBOARD_GET: {
        int len = 0;
        const char *c = clipboard_get(&len);
        if ((i64)a2 <= 0) return len;
        u64 n = MIN((u64)len, a2 - 1);
        if (c && copyout(a1, c, n)) return E_INVAL;
        char z = 0;
        if (copyout(a1 + n, &z, 1)) return E_INVAL;
        return (long)n;
    }
    }
    return usergui_syscall_more(p, nr, a1, a2, a3, a4, a5, a6);
}

/* ---- menus, cursors, geometry, clipping, font metrics, theme, more controls ---- */

_Static_assert(MENU_SEPARATOR == MI_SEPARATOR && MENU_DISABLED == MI_DISABLED && MENU_CHECKED == MI_CHECKED &&
               MENU_BOLD == MI_BOLD && !(MENU_TITLE & (MI_SEPARATOR | MI_DISABLED | MI_CHECKED | MI_BOLD)), "menu flags");
_Static_assert((int)CUR_ARROW == (int)CURSOR_ARROW && (int)CUR_IBEAM == (int)CURSOR_IBEAM && (int)CUR_WAIT == (int)CURSOR_BUSY &&
               (int)CUR_SIZE_NWSE == (int)CURSOR_RESIZE_NWSE && (int)CUR_SIZE_WE == (int)CURSOR_RESIZE_H && (int)CUR_SIZE_NS == (int)CURSOR_RESIZE_V &&
               (int)CUR_HAND == (int)CURSOR_HAND && (int)CUR_CROSS == (int)CURSOR_CROSS, "cursor shapes");
_Static_assert((int)HICON_COUNT == (int)ICON_COUNT && (int)HICON_FOLDER == (int)ICON_FOLDER && (int)HICON_NOTEPAD == (int)ICON_NOTEPAD &&
               (int)HICON_PAINT == (int)ICON_PAINT && (int)HICON_CALC == (int)ICON_CALC && (int)HICON_GAME == (int)ICON_GAME &&
               (int)HICON_RECYCLE == (int)ICON_RECYCLE && (int)HICON_HELP == (int)ICON_HELP && (int)HICON_FILE_TEXT == (int)ICON_FILE_TEXT, "icon ids");
_Static_assert((int)SCROLL_LINE_UP == (int)SBP_LINE_UP && (int)SCROLL_THUMB_TRACK == (int)SBP_THUMB_TRACK && (int)SCROLL_WHEEL == (int)SBP_WHEEL, "scroll parts");
_Static_assert(CTL_NOTIFY > 0xFF && !(CTL_NOTIFY & (WS_DEFAULT | WS_CANCEL | WS_DISABLED | WS_HIDDEN | WS_PASSWORD | WS_BOLD |
                                                  WS_READONLY | WS_WRAP)), "control styles");

static int menu_set_flags(umenu_t *m, int id, u32 mask, u32 val) {
    int n = 0;
    for (int i = 0; m && i < m->nitems; i++) {
        menu_item_t *it = &m->items[i];
        if (it->id != id || (it->flags & MI_SEPARATOR)) continue;
        it->flags = (it->flags & ~mask) | (val & mask);
        n++;
    }
    return n;
}

static color_t shade_col(u64 c) { return HEX((u32)c & 0xFFFFFF); }

typedef struct { uwin_t *u; char *paths; int len, count, effects, icon, result; } dnd_job_t;
static void job_dnd_begin(void *a) {
    dnd_job_t *j = a;
    window_t *w = j->u->w;
    /* only the window the held button was pressed in can start a drag */
    j->result = !w ? E_INVAL : wm_mouse_buttons() && wm_capture() == w ? dnd_begin(w, j->paths, j->len, j->count, j->effects, j->icon) : E_AGAIN;
}

static long usergui_syscall_more(proc_t *p, int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    uwin_t *u;
    gc_t g;
    switch (nr) {
    case SYS_WIN_INVALIDATE_RECT: {
        /* only that part of the canvas changed: it is copied into the
         * window as it is and only it is drawn again (screen savers) */
        if (!(u = uwin(p, (int)a1)) || !u->canvas) return E_INVAL;
        window_t *w = u->w;
        rect_t r = rect_intersect(unpack_rect(a2), mkrect(0, 0, u->canvas->w, u->canvas->h));
        if (rect_empty(r)) return 0;
        if (w->needs_paint || !w->client || w->widgets || w->menubar || w->client->w != u->canvas->w ||
            w->client->h != u->canvas->h + u->top) {
            wm_invalidate(w);                 /* controls are drawn over the canvas: all of it */
            return 0;
        }
        if (w->direct != u->canvas) {          /* (a full-screen window is drawn from the canvas itself) */
            g = gc_for(w->client);
            gfx_blit(&g, r.x, r.y + u->top, u->canvas, r);
        }
        rect_t c = wm_client_rect(w);
        wm_invalidate_rect(mkrect(c.x + r.x, c.y + u->top + r.y, r.w, r.h));
        return 0;
    }
    case SYS_MENU_SET: {
        /* items, count; count 0 removes the bar */
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        menu_job_t j = { u, NULL, NULL, 0, 0, 0 };
        if (a3) {
            long r = umenu_build(a2, a3, 1, &j.m);
            if (r) return r;
        }
        gui_call(job_menubar, &j);
        if (!j.ok) { umenu_free(j.m); return E_INVAL; }
        umenu_free(j.old);
        return 0;
    }
    case SYS_MENU_POPUP: {
        /* items, count, client x, y; the choice arrives as EVT_MENU */
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        menu_job_t j = { u, NULL, NULL, (int)CLAMP((i64)(int)a4, -8192, 8192), (int)CLAMP((i64)(int)a5, -8192, 8192), 0 };
        long r = umenu_build(a2, a3, 0, &j.m);
        if (r) return r;
        gui_call(job_popup, &j);
        if (!j.ok) { umenu_free(j.m); return E_INVAL; }
        umenu_free(j.old);
        return 0;
    }
    case SYS_MENU_STATE: {
        /* id, mask, flags (MENU_DISABLED, MENU_CHECKED, MENU_BOLD): items changed */
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        u32 mask = (u32)a3 & (MI_DISABLED | MI_CHECKED | MI_BOLD);
        int n = menu_set_flags(u->bar, (int)a2, mask, (u32)a4) + menu_set_flags(u->pop, (int)a2, mask, (u32)a4);
        if (n) menu_refresh();
        return n;
    }
    case SYS_WIN_SET_CURSOR:
        if (!(u = uwin(p, (int)a1)) || a2 > CUR_CROSS) return E_INVAL;
        if (u->w->cursor != (int)a2) {
            u->w->cursor = (int)a2;
            wm_cursor_changed();
        }
        return 0;
    case SYS_WIN_GET_RECT: {
        /* the client area (below the menu bar) on screen; returns 0 normal, 1 minimized, 2 maximized */
        if (!(u = uwin(p, (int)a1)) || !u->canvas) return E_INVAL;
        rect_t c = wm_client_rect(u->w);
        rect_t r = mkrect(c.x, c.y + u->top, u->canvas->w, u->canvas->h);
        if (copyout(a2, &r, sizeof r)) return E_INVAL;
        return u->w->state == WS_MINIMIZED ? 1 : u->w->state == WS_MAXIMIZED ? 2 : 0;
    }
    case SYS_WIN_SET_MIN_SIZE:
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        u->min_cw = (int)CLAMP((i64)a2, 0, 2048);
        u->min_ch = (int)CLAMP((i64)a3, 0, 1536);
        set_min(u);
        return 0;
    case SYS_WIN_SET_OWNER: {
        /* owner window (or -1), modal: the owner takes no input while this window is open */
        owner_job_t j = { uwin(p, (int)a1), NULL, (int)(a3 != 0), 0 };
        if (!j.u) return E_INVAL;
        if ((int)a2 >= 0 && (!(j.owner = uwin(p, (int)a2)) || j.owner == j.u)) return E_INVAL;
        gui_call(job_owner, &j);
        return j.ok ? 0 : E_INVAL;
    }
    case SYS_GFX_SET_CLIP: {
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        if (a6 & CLIP_RESET) { u->clip_on = 0; u->ox = u->oy = 0; return 0; }
        int x = (int)CLAMP((i64)(int)a2, -8192, 8192), y = (int)CLAMP((i64)(int)a3, -8192, 8192);
        int w = (int)CLAMP((i64)(int)a4, 0, 8192), h = (int)CLAMP((i64)(int)a5, 0, 8192);
        u->clip = mkrect(x, y, w, h);
        u->clip_on = 1;
        if (a6 & CLIP_ORIGIN) { u->ox = x; u->oy = y; } else { u->ox = u->oy = 0; }
        return 0;
    }
    case SYS_GFX_FONT_METRICS: {
        /* font, first code point, count, u8 advances[count] (or NULL), hal_fontinfo_t * (or NULL) */
        const font_t *f = font_of((int)a1);
        if (a3 > FONT_METRICS_MAX || a2 > 0x10FFFF || a2 + a3 > 0x110000) return E_INVAL;
        if (a5) {
            hal_fontinfo_t fi = { f->height, f->ascent, f->descent, 0 };
            if (copyout(a5, &fi, sizeof fi)) return E_INVAL;
        }
        if (a4 && a3) {
            u8 adv[256];
            for (u64 i = 0; i < a3; i += sizeof adv) {
                u64 n = MIN(sizeof adv, a3 - i);
                for (u64 k = 0; k < n; k++) adv[k] = (u8)MIN(gfx_char_width(f, (u32)(a2 + i + k)), 255);
                if (copyout(a4 + i, adv, n)) return E_INVAL;
            }
        }
        return 0;
    }
    case SYS_GFX_TEXT_N: {
        /* x, y, text, bytes, color | font << 24: text that need not end in NUL; returns end x */
        char buf[2048];
        if (canvas_gc(p, (int)a1, &g, NULL) || a5 > sizeof buf) return E_INVAL;
        if (a5 && copyin(buf, a4, a5)) return E_INVAL;
        return gfx_text_n(&g, font_of((int)((a6 >> 24) & 0xFF)), (int)CLAMP((i64)(int)a2, -8192, 8192),
                          (int)CLAMP((i64)(int)a3, -8192, 8192), buf, (int)a5, col(a6));
    }
    case SYS_GFX_SHADE: {
        /* geometry (packed as SYS_CTL_CREATE), c1, c2, kind, param */
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        rect_t r = unpack_rect(a2);
        int param = (int)a6;
        int rad = CLAMP(param & 0xFF, 0, 32), corners = (param >> 8) & 15;
        if (!corners) corners = 15;
        switch ((int)a5) {
        case SHADE_GRADIENT_V: gfx_gradient_v(&g, r, shade_col(a3), shade_col(a4)); return 0;
        case SHADE_GRADIENT_H: gfx_gradient_h(&g, r, shade_col(a3), shade_col(a4)); return 0;
        case SHADE_ROUNDED: gfx_rounded(&g, r, rad, corners, shade_col(a3), shade_col(a4)); return 0;
        case SHADE_ROUNDED_OUTLINE: gfx_rounded_outline(&g, r, rad, corners, shade_col(a3)); return 0;
        case SHADE_SUNKEN: gfx_sunken_field(&g, r, shade_col(a3)); return 0;
        case SHADE_FOCUS: gfx_focus_rect(&g, r); return 0;
        case SHADE_SHADOW: gfx_shadow(&g, r, CLAMP(param & 0xFF, 1, 16), CLAMP((param >> 8) & 0xFF, 0, 255)); return 0;
        case SHADE_ICON_TINTED: case SHADE_ICON_ALPHA: {
            int id = (int)(a4 & 0xFFFF), sz = (int)((a4 >> 16) & 0xFF);
            if (id < 0 || id >= ICON_COUNT) return E_INVAL;
            const icon_t *ic = sz == 1 ? &icons16[id] : sz == 2 ? &icons48[id] : &icons32[id];
            if ((int)a5 == SHADE_ICON_TINTED) gfx_icon_tinted(&g, r.x, r.y, ic, shade_col(a3), CLAMP(param, 0, 255));
            else gfx_icon_alpha(&g, r.x, r.y, ic, CLAMP(param, 0, 255));
            return 0;
        }
        }
        return E_INVAL;
    }
    case SYS_GFX_THEME: {
        const theme_t *t = g_theme;
        hal_theme_t ht = {
            t->title_top, t->title_mid, t->title_bottom, t->title_in_top, t->title_in_bottom,
            t->frame, t->frame_in, t->title_text, t->title_in_text, t->face,
            t->taskbar_top, t->taskbar_bottom, t->taskbar_line, t->start_top, t->start_bottom,
            t->select, t->select_text, t->sidebar_top, t->sidebar_bottom,
            t->menu_header_top, t->menu_header_bottom, t->button_border, t->hover, theme_index() };
        for (unsigned *c = &ht.title_top; c <= &ht.hover; c++) *c &= 0xFFFFFF;
        return copyout(a1, &ht, sizeof ht) ? E_INVAL : 0;
    }
    case SYS_CTL_MOVE: {
        /* id, geometry (packed as SYS_CTL_CREATE); status bars stay docked */
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2))) return E_INVAL;
        if (wd->type != W_STATUSBAR) {
            rect_t r = unpack_rect(a3);
            r.y += u->top;
            wd->r = r;
        }
        wm_invalidate(u->w);
        return 0;
    }
    case SYS_CTL_FOCUS: {
        /* id: the control that gets the keyboard, 0 = the program's own drawing; -1 only asks.
         * Returns the id that had the focus (0 = none) */
        widget_t *wd = NULL;
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        int old = u->w->focus ? u->w->focus->id : 0;
        if ((int)a2 == -1) return old;
        if ((int)a2 != 0) {
            wd = widget_find(u->w, (int)a2);
            if (!wd || (wd->style & (WS_DISABLED | WS_HIDDEN)) ||
                !(wd->type == W_BUTTON || wd->type == W_TEXTBOX || wd->type == W_CHECKBOX || wd->type == W_LISTBOX)) return E_INVAL;
        }
        u->w->focus = wd;
        if (wd && wd->type == W_TEXTBOX) { wd->sel_anchor = 0; wd->cursor = (int)strlen(wd->text); }   /* as Tab: all selected */
        wm_invalidate(u->w);
        return old;
    }
    case SYS_CTL_SCROLL_SET: {
        /* id, total, page, position, line step */
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2)) || (wd->type != W_VSCROLL && wd->type != W_HSCROLL)) return E_INVAL;
        widget_scroll_set(u->w, wd, (int)CLAMP((i64)a3, 0, 1 << 30), (int)CLAMP((i64)a4, 0, 1 << 30),
                          (int)CLAMP((i64)a5, 0, 1 << 30), (int)CLAMP((i64)a6, 1, 1 << 30));
        return 0;
    }
    case SYS_DND_BEGIN: {
        /* win, paths (NUL-separated), len, effects | icon << 8, count */
        if (!(u = uwin(p, (int)a1)) || (i64)a3 <= 0 || a3 > 256 * 1024 || (i64)a5 <= 0 || a5 > 65536) return E_INVAL;
        if (mm_heap_free() < GUI_RESERVE + 2 * a3) return E_NOMEM;
        dnd_job_t j = { u, kmalloc(a3), (int)a3, (int)a5, (int)(a4 & 0xFF), (int)((a4 >> 8) & 0xFFFF), 0 };
        if (!j.paths) return E_NOMEM;
        if (copyin(j.paths, a2, a3)) { kfree(j.paths); return E_INVAL; }
        gui_call(job_dnd_begin, &j);
        kfree(j.paths);
        return j.result;
    }
    case SYS_DND_ACCEPT:
        if (!(u = uwin(p, (int)a1))) return E_INVAL;
        dnd_answer(u->w, (u32)a2, (int)a3, (int)a4);
        return 0;
    case SYS_DND_DATA: {
        int len = 0, count = 0;
        const char *d = dnd_paths((u32)a1, &len, &count);
        if (!d) return E_NOENT;
        u64 n = MIN((u64)len, a3);
        if (n && copyout(a2, d, n)) return E_INVAL;
        return len;
    }
    case SYS_CTL_SET_TOOL: {
        /* id, icon (HICON_*, -1 none), tooltip (NULL none; any control) */
        widget_t *wd;
        char tip[sizeof wd->tip];
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2)) || (i64)a3 < -1 || (i64)a3 >= ICON_COUNT) return E_INVAL;
        tip[0] = 0;
        if (a4 && copyin_str(tip, a4, sizeof tip) == E_INVAL) return E_INVAL;
        wd->icon = (int)a3;
        memcpy(wd->tip, tip, sizeof tip);
        if (tip[0]) u->w->on_tick = uw_tick;
        wm_invalidate(u->w);
        return 0;
    }
    }
    return E_NOTSUP;
}

/* a process ended: close its windows */
static void release(proc_t *p) {
    ustate_t *s = p->gui;
    if (!s) return;
    for (int i = 0; i < MAX_WINDOWS; i++) if (s->wins[i]) uwin_free(p, s->wins[i]);
    for (int i = 0; i < MAX_MSGBOXES; i++) if (s->mbs[i]) { gui_call(job_mb_destroy, s->mbs[i]); kfree(s->mbs[i]); }
    kfree(s);
    p->gui = NULL;
}

/* "X has encountered a problem and needs to close" (Windows XP style) */
static void crash_close_later(void *a) { window_t *w = a; if (wm_alive(w)) wm_destroy(w); }
static void crash_ok(window_t *w, widget_t *wd) { (void)wd; gui_post(crash_close_later, w); }
typedef struct { char name[32], fault[160]; u32 pid; } crash_t;
static void crash_dialog(void *a) {
    crash_t *c = a;
    char msg[700];
    snprintf(msg, sizeof msg,
             "%s has encountered a problem and needs to close. We are sorry for the inconvenience.\n\n"
             "Halcyon ended the program; your other programs and the system are not affected.\n\n"
             "Details: process %u stopped because of %s.", c->name, c->pid, c->fault);
    kfree(c);
    msgbox_window("Program Error", msg, ICON_ERROR, MB_OK, crash_ok);   /* not modal: none nests */
}
static void crash(proc_t *p) {
    crash_t *c = kzalloc(sizeof *c);
    if (!c) return;
    strlcpy(c->name, p->name, sizeof c->name);
    strlcpy(c->fault, p->fault, sizeof c->fault);
    c->pid = p->pid;
    gui_post(crash_dialog, c);
}

void usergui_init(void) {
    proc_release_hook = release;
    proc_crash_hook = crash;
}
