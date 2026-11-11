/* Halcyon OS - windows and drawing for user programs (SYS_WIN_*, SYS_GFX_*,
 * SYS_CTL_*, message boxes, clipboard).
 *
 * Each program window has a canvas in the kernel: drawing calls paint into it
 * and the window manager copies it into the window whenever the window is
 * drawn (retained, like a GDI window with a backing bitmap). Input arrives in
 * a per-process event queue. Window-manager changes run on the GUI thread
 * (gui_call), the drawing and the queue are private to the program. */
#include "wm.h"
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

#define MAX_WINDOWS 16
#define QLEN 128

typedef struct uwin {
    window_t *w;            /* NULL once the window is gone */
    proc_t *p;
    int handle;
    surface_t *canvas;
    int timer_ms;
    u64 next_timer;
    u64 close_asked;        /* EVT_CLOSE sent at (ms), to spot a program that ignores it */
} uwin_t;

typedef struct ustate {
    hal_event_t q[QLEN];
    int qh, qt;
    uwin_t *wins[MAX_WINDOWS];
} ustate_t;

static ustate_t *state_of(proc_t *p) {
    if (!p->gui) p->gui = kzalloc(sizeof(ustate_t));
    return p->gui;
}

static void push(proc_t *p, const hal_event_t *ev) {
    ustate_t *s = p ? p->gui : NULL;
    if (!s) return;
    /* coalesce pointer motion */
    if (ev->type == EVT_MOUSE_MOVE && s->qh != s->qt) {
        hal_event_t *last = &s->q[(s->qh + QLEN - 1) % QLEN];
        if (last->type == EVT_MOUSE_MOVE && last->win == ev->win) { *last = *ev; return; }
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
    if (u && u->canvas) gfx_blit(g, 0, 0, u->canvas, mkrect(0, 0, u->canvas->w, u->canvas->h));
}

static void uw_event(window_t *w, gui_event_t *e) {
    uwin_t *u = w->data;
    if (!u || !u->p) return;
    hal_event_t ev = { 0 };
    ev.win = u->handle;
    ev.x = e->x; ev.y = e->y;
    ev.button = e->button; ev.buttons = e->buttons; ev.dz = e->dz;
    switch (e->type) {
    case GE_MOUSE_DOWN: ev.type = EVT_MOUSE_DOWN; break;
    case GE_MOUSE_UP: ev.type = EVT_MOUSE_UP; break;
    case GE_MOUSE_MOVE: ev.type = EVT_MOUSE_MOVE; break;
    case GE_DBLCLICK: ev.type = EVT_DBLCLICK; break;
    case GE_WHEEL: ev.type = EVT_WHEEL; break;
    case GE_FOCUS: ev.type = EVT_FOCUS; ev.pressed = 1; break;
    case GE_BLUR: ev.type = EVT_FOCUS; ev.pressed = 0; break;
    case GE_KEY:
        ev.type = EVT_KEY;
        ev.id = e->key.key;
        ev.pressed = e->key.pressed;
        ev.mods = e->key.mods;
        ev.ch = (u8)e->key.ch;
        break;
    default: return;
    }
    push(u->p, &ev);
}

static void uw_click(window_t *w, widget_t *wd) {
    uwin_t *u = w->data;
    if (!u || !u->p) return;
    hal_event_t ev = { 0 };
    ev.type = EVT_COMMAND;
    ev.win = u->handle;
    ev.id = wd->id;
    push(u->p, &ev);
}

static int uw_close(window_t *w) {
    uwin_t *u = w->data;
    if (!u || !u->p) return 1;
    u64 now = time_ms();
    if (u->close_asked && now - u->close_asked > 5000) {
        /* asked before and still open: the program is not responding */
        char msg[200];
        snprintf(msg, sizeof msg, "%s is not responding.\n\nIf you end it now, unsaved information will be lost. End it now?", u->p->name);
        if (msgbox(w, w->title, msg, ICON_WARNING, MB_YESNO) == ID_YES) proc_kill(u->p->pid, NULL);
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
    if (!u || !w->client) return;
    surface_t *n = surface_new(w->client->w, w->client->h);
    if (!n) return;
    gc_t g = gc_for(n);
    gfx_fill(&g, mkrect(0, 0, n->w, n->h), g_theme->face);
    if (u->canvas) { gfx_blit(&g, 0, 0, u->canvas, mkrect(0, 0, u->canvas->w, u->canvas->h)); surface_free(u->canvas); }
    u->canvas = n;
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

typedef struct { uwin_t *u; char title[96]; int cw, ch, flags; int ok; } create_job_t;
static void job_create(void *a) {
    create_job_t *j = a;
    u32 f = 0;
    if (j->flags & WIN_RESIZABLE) f |= WF_RESIZABLE;
    if (j->flags & WIN_DIALOG) f |= WF_DIALOG | WF_CENTER;
    if (j->flags & WIN_NO_MAX) f |= WF_NO_MAXIMIZE;
    window_t *w = wm_create(j->title, ICON_FILE_APP, -1, -1, j->cw, j->ch, f);
    if (!w) return;
    j->u->w = w;
    w->data = j->u;
    w->on_paint = uw_paint;
    w->on_event = uw_event;
    w->on_close = uw_close;
    w->on_resize = uw_resize;
    w->on_destroy = uw_destroyed;
    j->ok = 1;
}

static void job_destroy(void *a) {
    uwin_t *u = a;
    if (u->w) { u->w->on_close = NULL; u->w->data = NULL; wm_destroy(u->w); }
    u->w = NULL;
}

typedef struct { uwin_t *u; int type; rect_t r; char text[256]; int id, style; } ctl_job_t;
static void job_ctl(void *a) {
    ctl_job_t *j = a;
    if (!j->u->w) return;
    static const int types[] = { 0, W_BUTTON, W_LABEL, W_TEXTBOX, W_CHECKBOX, W_LISTBOX, W_GROUP, W_PROGRESS };
    widget_t *wd = widget_add(j->u->w, types[j->type], j->r, j->text, j->id);
    if (!wd) return;
    wd->style |= (u32)j->style;
    if (j->type == CTL_BUTTON) wd->on_click = uw_click;
    if (j->type == CTL_CHECKBOX || j->type == CTL_LISTBOX) wd->on_change = uw_click;
    if (j->type == CTL_TEXTBOX && !j->u->w->focus) j->u->w->focus = wd;
}

typedef struct { window_t *owner; char title[96]; char text[1024]; int icon, buttons, result; } mb_job_t;
static void job_msgbox(void *a) {
    mb_job_t *j = a;
    j->result = msgbox(j->owner, j->title, j->text, j->icon, j->buttons);
}

typedef struct { window_t *owner; int save; char title[96], dir[VFS_MAX_PATH], ext[16], out[VFS_MAX_PATH]; int ok; } fd_job_t;
static void job_filedlg(void *a) {
    fd_job_t *j = a;
    j->ok = file_dialog(j->owner, j->save, j->title, j->dir[0] ? j->dir : NULL, j->ext[0] ? j->ext : NULL, j->out, sizeof j->out);
}

/* ---- helpers for the system calls ---- */

static uwin_t *uwin(proc_t *p, int h) {
    uwin_t *u = handle_get(p, h, H_WINDOW);
    return u && u->w ? u : NULL;
}

static void uwin_free(proc_t *p, uwin_t *u) {
    gui_call(job_destroy, u);
    ustate_t *s = p->gui;
    if (s) for (int i = 0; i < MAX_WINDOWS; i++) if (s->wins[i] == u) s->wins[i] = NULL;
    handle_free(p, u->handle);
    if (u->canvas) surface_free(u->canvas);
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
                if (u) u->close_asked = time_ms();
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

/* the user's canvas as a graphics context */
static int canvas_gc(proc_t *p, int h, gc_t *g, uwin_t **out) {
    uwin_t *u = uwin(p, h);
    if (!u || !u->canvas) return E_INVAL;
    *g = gc_for(u->canvas);
    if (out) *out = u;
    return 0;
}

static color_t col(u64 c) { return HEX((u32)c & 0xFFFFFF); }

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
        u->p = p;
        u->canvas = surface_new(cw, ch);
        if (!u->canvas) { kfree(j); kfree(u); return E_NOMEM; }
        gc_t cg = gc_for(u->canvas);
        gfx_fill(&cg, mkrect(0, 0, cw, ch), g_theme->face);
        u->handle = handle_alloc(p, H_WINDOW, u);
        if (u->handle < 0) { surface_free(u->canvas); kfree(j); kfree(u); return E_NOMEM; }
        j->u = u; j->cw = cw; j->ch = ch; j->flags = (int)a4;
        gui_call(job_create, j);
        int ok = j->ok;
        kfree(j);
        if (!ok) { handle_free(p, u->handle); surface_free(u->canvas); kfree(u); return E_NOMEM; }
        s->wins[slot] = u;
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

    case SYS_GFX_FILL: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_fill(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), col(a6)); return 0;
    case SYS_GFX_RECT: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_rect(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), col(a6)); return 0;
    case SYS_GFX_LINE: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_line(&g, (int)a2, (int)a3, (int)a4, (int)a5, col(a6)); return 0;
    case SYS_GFX_BEVEL: if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL; gfx_bevel(&g, mkrect((int)a2, (int)a3, (int)a4, (int)a5), (int)a6); return 0;
    case SYS_GFX_CIRCLE:
        if (canvas_gc(p, (int)a1, &g, NULL)) return E_INVAL;
        if (a6) for (int r = (int)a4; r > 0; r--) gfx_circle(&g, (int)a2, (int)a3, r, col(a5));
        else gfx_circle(&g, (int)a2, (int)a3, (int)a4, col(a5));
        return 0;
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
        if (!(u = uwin(p, (int)a1)) || a2 < CTL_BUTTON || a2 > CTL_PROGRESS) return E_INVAL;
        ctl_job_t *j = kzalloc(sizeof *j);
        if (!j) return E_NOMEM;
        if (a4 && copyin_str(j->text, a4, sizeof j->text) == E_INVAL) { kfree(j); return E_INVAL; }
        j->u = u; j->type = (int)a2;
        j->r = mkrect((i16)(a3 >> 48), (i16)(a3 >> 32), (i16)(a3 >> 16), (i16)a3);
        j->id = (int)a5; j->style = (int)(a6 & 0xFF);
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
        char tmp[256];
        memcpy(tmp, wd->text, n);
        tmp[n - 1] = 0;
        return copyout(a3, tmp, n) ? E_INVAL : (long)n - 1;
    }
    case SYS_CTL_SET_VALUE: {
        widget_t *wd;
        if (!(u = uwin(p, (int)a1)) || !(wd = widget_find(u->w, (int)a2))) return E_INVAL;
        wd->value = (int)a3;
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
        wd->style = (wd->style & ~0xFFu) | (u32)(a3 & 0xFF);
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
        mb_job_t *j = kzalloc(sizeof *j);
        if (!j) return E_NOMEM;
        u = uwin(p, (int)a1);
        j->owner = u ? u->w : NULL;
        if (copyin_str(j->title, a2, sizeof j->title) == E_INVAL || copyin_str(j->text, a3, sizeof j->text) == E_INVAL) { kfree(j); return E_INVAL; }
        static const int icons[] = { ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_QUESTION };
        j->icon = icons[a4 < 4 ? a4 : 0];
        j->buttons = (int)CLAMP((i64)a5, MB_OK, MB_YESNOCANCEL);
        gui_call(job_msgbox, j);
        int r = j->result;
        kfree(j);
        return r;
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
        gui_call(job_filedlg, j);
        int r = j->ok ? (copyout(a6, j->out, strlen(j->out) + 1) ? E_INVAL : 1) : 0;
        kfree(j);
        return r;
    }
    case SYS_CLIPBOARD_SET:
        if (copyin_str(text, a1, sizeof text) == E_INVAL) return E_INVAL;
        clipboard_set(text, (int)strlen(text));
        return 0;
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
    return E_NOTSUP;
}

/* a process ended: close its windows */
static void release(proc_t *p) {
    ustate_t *s = p->gui;
    if (!s) return;
    for (int i = 0; i < MAX_WINDOWS; i++) if (s->wins[i]) uwin_free(p, s->wins[i]);
    kfree(s);
    p->gui = NULL;
}

/* "X has encountered a problem and needs to close" (Windows XP style) */
typedef struct { char name[32], fault[160]; u32 pid; } crash_t;
static void crash_dialog(void *a) {
    crash_t *c = a;
    char msg[700];
    snprintf(msg, sizeof msg,
             "%s has encountered a problem and needs to close. We are sorry for the inconvenience.\n\n"
             "Halcyon ended the program; your other programs and the system are not affected.\n\n"
             "Details: process %u stopped because of %s.", c->name, c->pid, c->fault);
    kfree(c);
    msgbox(NULL, "Program Error", msg, ICON_ERROR, MB_OK);
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
