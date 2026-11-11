/* Halcyon OS - progress dialog for long file operations ("Copying...").
 * The work runs in a kernel thread; this dialog only watches it, so the
 * desktop and every other window stay responsive. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/thread.h"
#include "fileops.h"

enum { ID_WHAT = 400, ID_WHERE, ID_BAR, ID_TIME, ID_CANCEL_OP };

static void worker(void *arg) { fileop_execute(arg); }

static void on_cancel(window_t *w, widget_t *wd) {
    fileop_t *op = w->data;
    op->cancel = 1;
    widget_set_text(w, wd, "Canceling...");
    wd->style |= WS_DISABLED;
}
static int on_close(window_t *w) {
    ((fileop_t *)w->data)->cancel = 1;
    return 0;       /* keep the window until the worker has stopped */
}

static void short_dir(const char *path, char *out, int n) {
    char d[VFS_MAX_PATH];
    vfs_dirname(path, d, sizeof d);
    const char *b = vfs_basename(d);
    snprintf(out, n, "%s", *b ? b : d);
}

static void on_tick(window_t *w) {
    fileop_t *op = w->data;
    char t[VFS_MAX_NAME + 64];
    const char *verb = op->kind == FOP_DELETE ? "Deleting" : op->kind == FOP_MOVE ? "Moving" : "Copying";
    if (op->counting) snprintf(t, sizeof t, "Preparing to %s...", op->kind == FOP_DELETE ? "delete" : op->kind == FOP_MOVE ? "move" : "copy");
    else snprintf(t, sizeof t, "%s '%s'", verb, op->current);
    widget_t *wd = widget_find(w, ID_WHAT);
    if (wd && strcmp(wd->text, t)) widget_set_text(w, wd, t);
    /* progress by bytes; by items for deletes and empty files */
    int val = 0;
    if (op->total_bytes && op->kind != FOP_DELETE) val = (int)(op->done_bytes * 1000 / op->total_bytes);
    else if (op->total_items) val = (int)((u64)op->done_items * 1000 / op->total_items);
    widget_t *bar = widget_find(w, ID_BAR);
    if (bar && bar->value != val) { bar->value = MIN(val, 1000); wm_invalidate(w); }
    u64 el = time_ms() - op->start_ms;
    if (op->counting || !val || el < 1500) snprintf(t, sizeof t, " ");
    else {
        u64 left = el * (1000 - (u64)val) / (u64)val / 1000;
        if (left >= 120) snprintf(t, sizeof t, "%lu minutes remaining", (left + 30) / 60);
        else snprintf(t, sizeof t, "%lu second%s remaining", left + 1, left ? "s" : "");
    }
    wd = widget_find(w, ID_TIME);
    if (wd && strcmp(wd->text, t)) widget_set_text(w, wd, t);
}

int fileop_run(window_t *owner, fileop_t *op) {
    op->done = 0;
    op->cancel = 0;
    thread_t *t = thread_create("file operation", worker, op);
    if (!t) { fileop_execute(op); return op->result; }   /* no memory for a thread: do it here */
    /* quick operations finish without a dialog */
    u64 show_at = time_ms() + 300;
    while (!op->done && time_ms() < show_at) gui_step();
    if (op->done) return op->result;

    const char *title = op->kind == FOP_DELETE ? "Deleting..." : op->kind == FOP_MOVE ? "Moving..." : "Copying...";
    window_t *w = wm_create(title, op->kind == FOP_DELETE ? ICON_RECYCLE : ICON_FOLDER, 0, 0, 380, 150, WF_DIALOG | WF_CENTER);
    w->data = op;
    w->on_close = on_close;
    w->on_tick = on_tick;
    widget_add(w, W_LABEL, mkrect(14, 12, 352, 16), "", ID_WHAT);
    char a[VFS_MAX_NAME + 1], b[VFS_MAX_NAME + 1], line[2 * VFS_MAX_NAME + 32];
    short_dir(op->src, a, sizeof a);
    if (op->kind == FOP_DELETE) snprintf(line, sizeof line, "From '%s'", a);
    else { short_dir(op->dst, b, sizeof b); snprintf(line, sizeof line, "From '%s' to '%s'", a, b); }
    widget_add(w, W_LABEL, mkrect(14, 32, 352, 16), line, ID_WHERE);
    widget_add(w, W_PROGRESS, mkrect(14, 56, 352, 16), "", ID_BAR);
    widget_add(w, W_LABEL, mkrect(14, 80, 250, 16), " ", ID_TIME);
    widget_t *c = widget_add(w, W_BUTTON, mkrect(291, 112, 75, 23), "Cancel", ID_CANCEL_OP);
    c->style |= WS_CANCEL;
    c->on_click = on_cancel;
    on_tick(w);
    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    while (!op->done) gui_step();
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    if (wm_alive(w)) { w->on_close = NULL; wm_destroy(w); }
    if (owner && wm_alive(owner)) wm_raise(owner);
    return op->result;
}
