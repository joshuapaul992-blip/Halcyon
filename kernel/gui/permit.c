/* Halcyon OS - asking the user before a program may do something to another
 * program (ending it). The question is a window of the kernel, answered with
 * the mouse or keyboard: programs cannot inject input, so only the user can
 * allow it. The asking thread waits (killably); the GUI thread never runs a
 * nested loop for it. */
#include "wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/proc.h"

typedef struct ask { window_t *w; volatile int result; char title[64]; char text[512]; } ask_t;

static void ask_click(window_t *w, widget_t *wd) {
    ask_t *a = w->data;
    if (a && !a->result) a->result = wd->id;
}
static int ask_close(window_t *w) {
    ask_t *a = w->data;
    if (a && !a->result) a->result = ID_NO;
    return 0;
}
static void job_ask(void *arg) {
    ask_t *a = arg;
    window_t *w = msgbox_window(a->title, a->text, ICON_WARNING, MB_YESNO, ask_click);
    if (!w) { a->result = ID_NO; return; }
    w->data = a;
    w->on_close = ask_close;
    widget_t *no = widget_find(w, ID_NO);
    if (no) { w->focus = no; no->style |= WS_DEFAULT; }         /* Enter means no */
    widget_t *yes = widget_find(w, ID_YES);
    if (yes) yes->style &= ~WS_DEFAULT;
    wm_raise(w);
    a->w = w;
}
static void job_unask(void *arg) {
    ask_t *a = arg;
    if (a->w && wm_alive(a->w)) { a->w->data = NULL; a->w->on_close = NULL; wm_destroy(a->w); }
    a->w = NULL;
}
static int answered(void *arg) { return ((ask_t *)arg)->result != 0; }

/* 1 allowed, 0 refused, E_BUSY (already asking), E_CANCELED (the asker is ending) */
int gui_ask_permission(proc_t *asker, const char *title, const char *text) {
    if (asker->confirm_pending) return E_BUSY;
    ask_t *a = kzalloc(sizeof *a);
    if (!a) return E_NOMEM;
    strlcpy(a->title, title, sizeof a->title);
    strlcpy(a->text, text, sizeof a->text);
    asker->confirm_pending = 1;
    gui_call(job_ask, a);
    int r = proc_wait(answered, a, ~0ull);
    int res = a->result;
    gui_call(job_unask, a);
    kfree(a);
    asker->confirm_pending = 0;
    if (r < 0) return r;
    return res == ID_YES;
}
