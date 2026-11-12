/* Halcyon OS - Keyboard settings, in the spirit of Windows XP's "Text
 * Services and Input Languages": the installed keyboard layouts (Left
 * Alt+Shift or the taskbar indicator switches between them), the default
 * one, and a box to try typing. Saved as keyboard.layouts (comma-separated
 * layout ids) and keyboard.default in C:\HALCYON\SETTINGS.INI. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "drivers/keymap.h"

#define KB_W 420
#define KB_H 414

enum { ID_LIST = 10, ID_ADD, ID_REMOVE, ID_DEFAULT, ID_DEFNAME, ID_ACTIVE, ID_TEST, ID_APPLY };

typedef struct kb {
    int list[INPUT_MAX_LAYOUTS], n;     /* pending installed layouts */
    int deflt;                          /* pending default */
    u32 gen;                            /* input_layout_generation() shown */
} kb_t;

static window_t *kb_win;

/* ---------------- settings ---------------- */
static void save_settings(void) {
    int idx[INPUT_MAX_LAYOUTS];
    int n = input_layouts(idx, INPUT_MAX_LAYOUTS);
    char v[INPUT_MAX_LAYOUTS * 8] = "";
    for (int i = 0; i < n; i++) {
        if (i) strlcat(v, ",", sizeof v);
        strlcat(v, kl_layouts[idx[i]].id, sizeof v);
    }
    settings_set("keyboard.layouts", v);
    settings_set("keyboard.default", kl_layouts[input_layout_default()].id);
    settings_save();
}

/* at boot, after settings_load() */
void keyboard_settings_load(void) {
    const char *v = settings_get("keyboard.layouts", NULL);
    if (!v) return;
    int idx[INPUT_MAX_LAYOUTS], n = 0;
    char id[16];
    while (*v && n < INPUT_MAX_LAYOUTS) {
        int l = 0;
        while (*v == ' ' || *v == ',') v++;
        while (*v && *v != ',' && *v != ' ') { if (l < (int)sizeof id - 1) id[l++] = *v; v++; }
        id[l] = 0;
        int i = kl_find(id);
        if (i >= 0) idx[n++] = i;
        else if (l) kprintf("keyboard: unknown layout \"%s\" in the settings\n", id);
    }
    int def = kl_find(settings_get("keyboard.default", ""));
    input_layouts_set(idx, n, def);
    input_layout_select(input_layout_default());
    kprintf("keyboard: %d layout(s), %s active\n", n, kl_layouts[input_layout()].name);
}

/* ---------------- the taskbar indicator's menu ---------------- */
static menu_item_t ind_items[INPUT_MAX_LAYOUTS + 2];
static menu_t ind_menu = { "Keyboard", ind_items, 0 };
static window_t *ind_owner;

static void ind_cmd(window_t *w, int id) {
    if (id == 1000) app_keyboard();
    else input_layout_select(id);
}

void keyboard_menu(int x, int y) {
    int idx[INPUT_MAX_LAYOUTS];
    int n = input_layouts(idx, INPUT_MAX_LAYOUTS), m = 0;
    for (int i = 0; i < n; i++) {
        const kl_layout_t *l = &kl_layouts[idx[i]];
        ind_items[m++] = (menu_item_t){ l->name, l->abbr, idx[i], idx[i] == input_layout() ? MI_CHECKED : 0, -1 };
    }
    ind_items[m++] = (menu_item_t){ "", NULL, 0, MI_SEPARATOR, -1 };
    ind_items[m++] = (menu_item_t){ "Keyboard Settings...", NULL, 1000, 0, -1 };
    ind_menu.count = m;
    if (!ind_owner) ind_owner = kzalloc(sizeof(window_t));
    ind_owner->on_menu = ind_cmd;
    menu_popup(ind_owner, &ind_menu, x, y);
}

/* ---------------- the dialog ---------------- */
static int is_pending(kb_t *k) {
    int idx[INPUT_MAX_LAYOUTS];
    int n = input_layouts(idx, INPUT_MAX_LAYOUTS);
    if (n != k->n || k->deflt != input_layout_default()) return 1;
    for (int i = 0; i < n; i++) if (idx[i] != k->list[i]) return 1;
    return 0;
}

static void refresh(window_t *w) {
    kb_t *k = w->data;
    widget_t *lb = widget_find(w, ID_LIST);
    int sel = lb->value;
    char *items[INPUT_MAX_LAYOUTS], buf[INPUT_MAX_LAYOUTS][80];
    for (int i = 0; i < k->n; i++) {
        const kl_layout_t *l = &kl_layouts[k->list[i]];
        snprintf(buf[i], sizeof buf[i], "%s (%s)%s", l->name, l->abbr, k->list[i] == k->deflt ? " - default" : "");
        items[i] = buf[i];
    }
    widget_list_set(w, lb, items, k->n);
    lb->value = k->n ? CLAMP(sel, 0, k->n - 1) : -1;
    int rows = (lb->r.h - 2) / 18;
    if (lb->value >= rows) lb->top = lb->value - rows + 1;
    widget_set_text(w, widget_find(w, ID_DEFNAME), kl_layouts[k->deflt].name);
    widget_t *rm = widget_find(w, ID_REMOVE), *df = widget_find(w, ID_DEFAULT), *ad = widget_find(w, ID_ADD);
    if (k->n > 1) rm->style &= ~WS_DISABLED; else rm->style |= WS_DISABLED;
    if (k->n < kl_layout_count) ad->style &= ~WS_DISABLED; else ad->style |= WS_DISABLED;
    if (lb->value >= 0 && k->list[lb->value] != k->deflt) df->style &= ~WS_DISABLED; else df->style |= WS_DISABLED;
    widget_t *ap = widget_find(w, ID_APPLY);
    if (is_pending(k)) ap->style &= ~WS_DISABLED; else ap->style |= WS_DISABLED;
    if (w->focus && (w->focus->style & WS_DISABLED)) w->focus = lb;
    wm_invalidate(w);
}

static void show_active(window_t *w) {
    kb_t *k = w->data;
    char t[120];
    const kl_layout_t *l = &kl_layouts[input_layout()];
    snprintf(t, sizeof t, "Type here to try the active layout, %s (%s):", l->name, l->abbr);
    widget_set_text(w, widget_find(w, ID_ACTIVE), t);
    k->gen = input_layout_generation();
}

/* "Add Input Language": pick one of the layouts that are not installed */
typedef struct addlg { volatile int done; int result; int map[INPUT_MAX_LAYOUTS]; } addlg_t;
static void add_btn(window_t *w, widget_t *b) {
    addlg_t *a = w->data;
    widget_t *lb = widget_find(w, ID_LIST);
    a->result = b->id == ID_OK && lb->value >= 0 ? a->map[lb->value] : -1;
    a->done = 1;
}
static void add_activate(window_t *w, widget_t *lb) { add_btn(w, widget_find(w, ID_OK)); }
static int add_close(window_t *w) { ((addlg_t *)w->data)->done = 1; return 0; }

static void on_add(window_t *w, widget_t *b) {
    kb_t *k = w->data;
    addlg_t a = { 0 };
    a.result = -1;
    char *items[INPUT_MAX_LAYOUTS], buf[INPUT_MAX_LAYOUTS][80];
    int n = 0;
    for (int i = 0; i < kl_layout_count && n < INPUT_MAX_LAYOUTS; i++) {
        int have = 0;
        for (int j = 0; j < k->n; j++) if (k->list[j] == i) have = 1;
        if (have) continue;
        snprintf(buf[n], sizeof buf[n], "%s (%s)", kl_layouts[i].name, kl_layouts[i].abbr);
        items[n] = buf[n];
        a.map[n++] = i;
    }
    if (!n) return;
    window_t *d = wm_create("Add Input Language", -1, 0, 0, 300, 290, WF_DIALOG | WF_CENTER);
    if (!d) return;
    widget_add(d, W_LABEL, mkrect(12, 12, 276, 16), "Keyboard layout:", 0);
    widget_t *lb = widget_add(d, W_LISTBOX, mkrect(12, 32, 276, 11 * 18 + 2), "", ID_LIST);
    widget_list_set(d, lb, items, n);
    lb->on_activate = add_activate;
    widget_t *ok = widget_add(d, W_BUTTON, mkrect(300 - 12 - 2 * 75 - 8, 290 - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = add_btn;
    widget_t *cn = widget_add(d, W_BUTTON, mkrect(300 - 12 - 75, 290 - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = add_btn;
    d->focus = lb;
    d->data = &a;
    d->on_close = add_close;
    d->owner = w;
    w->modal = d;
    while (!a.done && wm_alive(d) && wm_alive(w)) gui_step();
    if (wm_alive(w) && w->modal == d) w->modal = NULL;
    if (wm_alive(d)) { d->on_close = NULL; wm_destroy(d); }
    if (!wm_alive(w) || a.result < 0 || k->n >= INPUT_MAX_LAYOUTS) return;
    wm_raise(w);
    k->list[k->n++] = a.result;
    widget_find(w, ID_LIST)->value = k->n - 1;
    refresh(w);
}

static void on_remove(window_t *w, widget_t *b) {
    kb_t *k = w->data;
    widget_t *lb = widget_find(w, ID_LIST);
    int s = lb->value;
    if (s < 0 || s >= k->n || k->n < 2) return;
    int gone = k->list[s];
    memmove(&k->list[s], &k->list[s + 1], sizeof k->list[0] * (k->n - s - 1));
    k->n--;
    if (gone == k->deflt) k->deflt = k->list[0];
    refresh(w);
}

static void on_default(window_t *w, widget_t *b) {
    kb_t *k = w->data;
    widget_t *lb = widget_find(w, ID_LIST);
    if (lb->value >= 0 && lb->value < k->n) k->deflt = k->list[lb->value];
    refresh(w);
}

static void list_change(window_t *w, widget_t *lb) { refresh(w); }

static void apply(window_t *w) {
    kb_t *k = w->data;
    if (!is_pending(k)) return;
    input_layouts_set(k->list, k->n, k->deflt);
    save_settings();
    refresh(w);
}
static void on_apply(window_t *w, widget_t *b) { apply(w); }
static void on_ok(window_t *w, widget_t *b) { apply(w); wm_close(w); }
static void on_cancel(window_t *w, widget_t *b) { wm_close(w); }

static void kb_tick(window_t *w) {
    kb_t *k = w->data;
    if (k->gen != input_layout_generation()) show_active(w);
}

static void kb_destroy(window_t *w) {
    kfree(w->data);
    w->data = NULL;
    if (kb_win == w) kb_win = NULL;
}

void app_keyboard(void) {
    if (kb_win && wm_alive(kb_win)) { wm_show(kb_win); return; }
    kb_t *k = kzalloc(sizeof *k);
    if (!k) return;
    k->n = input_layouts(k->list, INPUT_MAX_LAYOUTS);
    k->deflt = input_layout_default();
    window_t *w = wm_create("Text Services and Input Languages", ICON_CONTROL, -1, -1, KB_W, KB_H, WF_NO_MAXIMIZE | WF_NO_MINIMIZE);
    if (w) w->help = "keyboard";              /* F1 */
    if (!w) { kfree(k); return; }
    w->data = k;
    w->on_tick = kb_tick;
    w->on_destroy = kb_destroy;

    widget_add(w, W_GROUP, mkrect(12, 8, KB_W - 24, 66), "Default input language", 0);
    widget_add(w, W_LABEL, mkrect(24, 28, KB_W - 48, 16), "The keyboard layout used when the computer starts:", 0);
    widget_t *dn = widget_add(w, W_LABEL, mkrect(24, 47, KB_W - 48, 16), "", ID_DEFNAME);
    dn->style |= WS_BOLD;

    widget_add(w, W_GROUP, mkrect(12, 84, KB_W - 24, 196), "Installed services", 0);
    widget_add(w, W_LABEL, mkrect(24, 104, 260, 16), "Keyboard layouts (Left Alt+Shift switches):", 0);
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(24, 124, 262, 8 * 18 + 2), "", ID_LIST);
    lb->on_change = list_change;
    widget_t *b = widget_add(w, W_BUTTON, mkrect(296, 124, 100, 23), "Add...", ID_ADD);
    b->on_click = on_add;
    b = widget_add(w, W_BUTTON, mkrect(296, 153, 100, 23), "Remove", ID_REMOVE);
    b->on_click = on_remove;
    b = widget_add(w, W_BUTTON, mkrect(296, 182, 100, 23), "Set as Default", ID_DEFAULT);
    b->on_click = on_default;

    widget_add(w, W_GROUP, mkrect(12, 290, KB_W - 24, 74), "Test", 0);
    widget_add(w, W_LABEL, mkrect(24, 310, KB_W - 48, 16), "", ID_ACTIVE);
    widget_add(w, W_TEXTBOX, mkrect(24, 330, KB_W - 48, 22), "", ID_TEST);

    widget_t *ok = widget_add(w, W_BUTTON, mkrect(KB_W - 12 - 3 * 75 - 2 * 8, KB_H - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(KB_W - 12 - 2 * 75 - 8, KB_H - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(KB_W - 12 - 75, KB_H - 35, 75, 23), "Apply", ID_APPLY);
    ap->on_click = on_apply;

    refresh(w);                         /* fills the list */
    for (int i = 0; i < k->n; i++) if (k->list[i] == input_layout()) lb->value = i;
    refresh(w);
    show_active(w);
    w->focus = lb;
    kb_win = w;
}
