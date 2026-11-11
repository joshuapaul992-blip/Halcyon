/* Halcyon OS - modal dialogs: message box, input box, file open/save */
#include "wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "fs/vfs.h"

typedef struct dlg { volatile int done; int result; char *buf; int bufsz; } dlg_t;

static void dlg_button(window_t *w, widget_t *wd) {
    dlg_t *d = w->data;
    d->result = wd->id;
    if (d->buf) {
        widget_t *t = widget_find(w, 100);
        if (t) strlcpy(d->buf, t->text, d->bufsz);
    }
    d->done = 1;
}
static int dlg_close(window_t *w) {
    dlg_t *d = w->data;
    d->result = ID_CANCEL;
    d->done = 1;
    return 0;   /* the runner destroys the window */
}

static int run_modal(window_t *owner, window_t *dw, dlg_t *d) {
    if (owner && wm_alive(owner)) { owner->modal = dw; dw->owner = owner; }
    dw->data = d;
    dw->on_close = dlg_close;
    while (!d->done && wm_alive(dw)) gui_step();
    if (owner && wm_alive(owner) && owner->modal == dw) owner->modal = NULL;
    if (wm_alive(dw)) { dw->on_close = NULL; wm_destroy(dw); }
    if (owner && wm_alive(owner)) wm_raise(owner);
    return d->result;
}

int msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons) {
    int tw = gfx_text_width(&font_ui, text);
    int textw = CLAMP(tw + 10, 200, 420);
    int texth = gfx_text_wrap_height(&font_ui, textw, text);
    int nb = buttons == MB_OK ? 1 : buttons == MB_YESNOCANCEL ? 3 : 2;
    int cw = MAX(textw + 76, nb * 82 + 30);
    int ch = MAX(texth, 36) + 72;
    window_t *w = wm_create(title, -1, 0, 0, cw, ch, WF_DIALOG | WF_CENTER);
    widget_t *ic = widget_add(w, W_ICON, mkrect(16, 16, 32, 32), "", 0);
    ic->value = icon >= 0 ? icon : ICON_INFO;
    widget_t *lb = widget_add(w, W_LABEL, mkrect(62, 16 + (texth < 32 ? (32 - texth) / 2 : 0), textw, texth + 4), text, 0);
    lb->style |= WS_WRAP;
    static const char *names[] = { "OK", "Cancel", "Yes", "No" };
    int ids[3], n = 0;
    const char *lbl[3];
    if (buttons == MB_OK) { ids[n] = ID_OK; lbl[n++] = names[0]; }
    if (buttons == MB_OKCANCEL) { ids[n] = ID_OK; lbl[n++] = names[0]; ids[n] = ID_CANCEL; lbl[n++] = names[1]; }
    if (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) { ids[n] = ID_YES; lbl[n++] = names[2]; ids[n] = ID_NO; lbl[n++] = names[3]; }
    if (buttons == MB_YESNOCANCEL) { ids[n] = ID_CANCEL; lbl[n++] = names[1]; }
    int bx = (cw - (n * 82 - 7)) / 2;
    for (int i = 0; i < n; i++) {
        widget_t *b = widget_add(w, W_BUTTON, mkrect(bx + i * 82, ch - 36, 75, 23), lbl[i], ids[i]);
        b->on_click = dlg_button;
        if (i == 0) { b->style |= WS_DEFAULT; w->focus = b; }
        if (ids[i] == ID_CANCEL || (n == 1)) b->style |= WS_CANCEL;
    }
    dlg_t d = { 0 };
    return run_modal(owner, w, &d);
}

int inputbox(window_t *owner, const char *title, const char *prompt, char *buf, int bufsz) {
    window_t *w = wm_create(title, -1, 0, 0, 340, 120, WF_DIALOG | WF_CENTER);
    widget_add(w, W_LABEL, mkrect(14, 12, 312, 18), prompt, 0);
    widget_t *t = widget_add(w, W_TEXTBOX, mkrect(14, 36, 312, 22), buf, 100);
    t->sel_anchor = 0;
    w->focus = t;
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(170, 82, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = dlg_button;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(251, 82, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = dlg_button;
    dlg_t d = { 0 };
    d.buf = buf; d.bufsz = bufsz;
    return run_modal(owner, w, &d) == ID_OK;
}

/* ---------------- file dialog ---------------- */
typedef struct fdlg {
    dlg_t d;
    char dir[VFS_MAX_PATH];
    char ext[16];
    int save;
    vfs_dirent_t *ents; int nents;
    char **isdir;   /* unused */
} fdlg_t;

static void fd_fill(window_t *w) {
    fdlg_t *f = w->data;
    widget_t *lb = widget_find(w, 101);
    widget_t *path = widget_find(w, 102);
    kfree(f->ents); f->ents = NULL; f->nents = 0;
    char **items;
    int n = 0;
    if (!f->dir[0]) {
        int nv = vfs_volume_count();
        items = kzalloc(sizeof(char *) * (nv + 1));
        for (int i = 0; i < nv; i++) {
            volume_t *v = vfs_volume(i);
            items[n] = kmalloc(64);
            snprintf(items[n], 64, "[%c:] %s", v->letter, v->label[0] ? v->label : "Local Disk");
            n++;
        }
        widget_set_text(w, path, "My Computer");
    } else {
        int cnt = vfs_list(f->dir, &f->ents);
        if (cnt < 0) cnt = 0;
        f->nents = cnt;
        items = kzalloc(sizeof(char *) * (cnt + 2));
        items[n++] = strdup("[..] Up one level");
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < cnt; i++) {
                vfs_dirent_t *e = &f->ents[i];
                if ((pass == 0) != (e->is_dir != 0)) continue;
                if (!e->is_dir && f->ext[0]) {
                    const char *dot = strrchr(e->name, '.');
                    if (!dot || strcasecmp(dot + 1, f->ext)) continue;
                }
                items[n] = kmalloc(VFS_MAX_NAME + 8);
                snprintf(items[n], VFS_MAX_NAME + 8, e->is_dir ? "[%s]" : "%s", e->name);
                n++;
            }
        widget_set_text(w, path, f->dir);
    }
    widget_list_set(w, lb, items, n);
    for (int i = 0; i < n; i++) kfree(items[i]);
    kfree(items);
}

static void fd_enter(window_t *w, const char *sel) {
    fdlg_t *f = w->data;
    if (!f->dir[0]) {
        /* "[C:] label" */
        f->dir[0] = sel[1]; f->dir[1] = ':'; f->dir[2] = '/'; f->dir[3] = 0;
        fd_fill(w);
        return;
    }
    if (!strncmp(sel, "[..]", 4)) {
        if (strlen(f->dir) <= 3) f->dir[0] = 0;
        else { char p[VFS_MAX_PATH]; vfs_dirname(f->dir, p, sizeof p); strlcpy(f->dir, p, sizeof f->dir); }
        fd_fill(w);
        return;
    }
    if (sel[0] == '[') {
        char name[VFS_MAX_NAME + 1];
        strlcpy(name, sel + 1, sizeof name);
        name[strlen(name) - 1] = 0;
        char p[VFS_MAX_PATH];
        vfs_join(f->dir, name, p, sizeof p);
        strlcpy(f->dir, p, sizeof f->dir);
        fd_fill(w);
        return;
    }
    widget_set_text(w, widget_find(w, 100), sel);
    widget_t *ok = widget_find(w, ID_OK);
    if (ok) ok->on_click(w, ok);
}

static void fd_activate(window_t *w, widget_t *lb) {
    if (lb->value >= 0 && lb->value < lb->nitems) fd_enter(w, lb->items[lb->value]);
}
static void fd_change(window_t *w, widget_t *lb) {
    if (lb->value < 0 || lb->value >= lb->nitems) return;
    const char *s = lb->items[lb->value];
    fdlg_t *f = w->data;
    if (f->dir[0] && s[0] != '[') widget_set_text(w, widget_find(w, 100), s);
}
static void fd_up(window_t *w, widget_t *b) { fd_enter(w, "[..]"); }
static void fd_ok(window_t *w, widget_t *b) {
    fdlg_t *f = w->data;
    widget_t *name = widget_find(w, 100);
    if (!name->text[0]) {
        widget_t *lb = widget_find(w, 101);
        if (lb->value >= 0) fd_enter(w, lb->items[lb->value]);
        return;
    }
    if (!f->dir[0] && !strchr(name->text, ':')) { msgbox(w, "Save", "Please choose a drive first.", ICON_WARNING, MB_OK); return; }
    f->d.result = ID_OK;
    f->d.done = 1;
}
static void fd_cancel(window_t *w, widget_t *b) { fdlg_t *f = w->data; f->d.result = ID_CANCEL; f->d.done = 1; }

int file_dialog(window_t *owner, int save, const char *title, const char *dir, const char *ext, char *out, int outsz) {
    window_t *w = wm_create(title, save ? ICON_FOLDER : ICON_FOLDER_OPEN, 0, 0, 440, 330, WF_DIALOG | WF_CENTER);
    fdlg_t *f = kzalloc(sizeof *f);
    f->save = save;
    if (dir) strlcpy(f->dir, dir, sizeof f->dir);
    if (ext) strlcpy(f->ext, ext, sizeof f->ext);
    w->data = f;
    widget_add(w, W_LABEL, mkrect(12, 12, 60, 22), "Look in:", 0);
    widget_t *path = widget_add(w, W_TEXTBOX, mkrect(76, 12, 270, 22), "", 102);
    path->style |= WS_READONLY;
    widget_t *up = widget_add(w, W_BUTTON, mkrect(352, 11, 76, 24), "Up", 103);
    up->on_click = fd_up;
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(12, 42, 416, 206), "", 101);
    lb->on_activate = fd_activate;
    lb->on_change = fd_change;
    widget_add(w, W_LABEL, mkrect(12, 260, 70, 22), "File name:", 0);
    widget_t *name = widget_add(w, W_TEXTBOX, mkrect(86, 260, 260, 22), "", 100);
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(352, 259, 76, 24), save ? "Save" : "Open", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = fd_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(352, 291, 76, 24), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = fd_cancel;
    w->focus = save ? name : lb;
    fd_fill(w);

    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    w->on_close = dlg_close;
    while (!f->d.done && wm_alive(w)) gui_step();
    int ok_res = f->d.result == ID_OK;
    if (ok_res) {
        widget_t *nm = widget_find(w, 100);
        if (strchr(nm->text, ':')) strlcpy(out, nm->text, outsz);
        else vfs_join(f->dir, nm->text, out, outsz);
    }
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    w->data = NULL;
    if (wm_alive(w)) { w->on_close = NULL; wm_destroy(w); }
    kfree(f->ents);
    kfree(f);
    if (owner && wm_alive(owner)) wm_raise(owner);
    return ok_res;
}
