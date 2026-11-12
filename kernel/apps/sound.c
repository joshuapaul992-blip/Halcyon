/* Halcyon OS - Sounds and Audio Devices: the master volume and mute, the
 * volume icon in the taskbar, the sound cards (driver, output, rate, state)
 * and the sound scheme: system sounds on or off, each event on or off, a
 * test button and another WAV file per event. Volume and mute apply at once
 * (Cancel puts them back); the rest applies with Apply / OK and is saved in
 * SETTINGS.INI in the background (sound/sndevent.c). Opened from Control
 * Panel and by a double click on the tray's speaker. */
#include "gui/wm.h"
#include "gui/volume.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "fs/vfs.h"
#include "sound/snddev.h"
#include "sound/sound.h"

#define SW_W 440
#define SW_H 566

enum { ID_MUTE = 10, ID_TRAY, ID_DEVS, ID_SOUNDS_ON, ID_EVENTS, ID_EV_ON, ID_PLAY, ID_BROWSE, ID_DEFAULT, ID_APPLY };

typedef struct sndwin {
    int vol0, mute0;                   /* at open: Cancel restores them */
    int scheme_on, tray;               /* pending */
    int enabled[SNDEV_COUNT];
    char *file0[SNDEV_COUNT];          /* the file before a Browse (Cancel restores it), or NULL */
    int dirty;
    int ev;                            /* the selected event */
    int drag, hot;
    u64 last_refresh;
    u32 gen;
} sndwin_t;

static window_t *sw_win;

static rect_t vol_slider(void) { return mkrect(100, 52, SW_W - 100 - 70, 30); }

/* ---------------- painting ---------------- */
static void sw_paint(window_t *w, gc_t *g) {
    sndwin_t *c = w->data;
    int have = snddev_count() > 0;
    snddev_t *d = snd_default();
    draw_group(g, mkrect(12, 8, SW_W - 24, 122), "Volume");
    gfx_icon(g, 26, 30, &icons32[!have || snd_muted() ? ICON_SPEAKER_MUTED : ICON_SPEAKER]);
    gfx_text_box(g, &font_ui_bold, mkrect(70, 28, SW_W - 100, font_ui.height), d ? d->desc : "No sound card", HEX(0x1C2230), 0);
    rect_t s = vol_slider();
    gfx_text(g, &font_ui, s.x - 30, s.y + 8, "Low", HEX(0x4A5060));
    gfx_text(g, &font_ui, s.x + s.w + 8, s.y + 8, "High", HEX(0x4A5060));
    slider_paint(g, s, 0, snd_volume(), c->hot || c->drag, !have);
    char t[16];
    snprintf(t, sizeof t, "%d%%", snd_volume());
    gfx_text_box(g, &font_small, mkrect(s.x, s.y + s.h - 2, s.w, 12), have ? t : "", HEX(0x6A7080), 1);

    rect_t dg = mkrect(12, 140, SW_W - 24, 128);
    draw_group(g, dg, "Sound devices");
    if (!have) {
        gfx_text_box(g, &font_ui, mkrect(26, 166, SW_W - 52, 40),
                     "No sound card was found. docs/HARDWARE.md lists the sound cards Halcyon supports.", HEX(0x1C2230), 0);
    } else {
        widget_t *lb = widget_find(w, ID_DEVS);
        snddev_t *sel = snddev_get(lb && lb->value >= 0 ? lb->value : 0);
        if (sel) {
            char a[96], b[128];
            snprintf(a, sizeof a, "Driver: %s    Output: %s", sel->driver ? sel->driver : "-", sel->output[0] ? sel->output : "-");
            snprintf(b, sizeof b, "%d Hz, %s, 16-bit; %s%s; %s", sel->rate, sel->channels == 2 ? "stereo" : "mono",
                     sel->running ? "playing" : "idle", sel == snd_default() ? " (default)" : "",
                     sel->irq_live ? "interrupt" : "polled");
            gfx_text_box(g, &font_ui, mkrect(26, 218, SW_W - 52, font_ui.height), a, HEX(0x1C2230), 0);
            gfx_text_box(g, &font_ui, mkrect(26, 236, SW_W - 52, font_ui.height), b, HEX(0x4A5060), 0);
        }
    }

    draw_group(g, mkrect(12, 278, SW_W - 24, 238), "Sounds");
    gfx_text(g, &font_ui, 26, 324, "Program events:", HEX(0x1C2230));
    char path[VFS_MAX_PATH], line[VFS_MAX_PATH + 64];
    sound_event_file(c->ev, path, sizeof path);
    int st = sound_event_loaded(c->ev);
    if (st == 1) snprintf(line, sizeof line, "%s (%d.%d s)", path, sound_event_ms(c->ev) / 1000, sound_event_ms(c->ev) / 100 % 10);
    else if (st == 0) snprintf(line, sizeof line, "%s (loading)", path);
    else snprintf(line, sizeof line, "%s: cannot be used (%s)", path, st == E_NOTSUP ? "not 8 or 16-bit PCM WAV" : strerror(st));
    gfx_text(g, &font_ui, 26, 488, "Sound:", HEX(0x4A5060));
    gfx_text_box(g, &font_ui, mkrect(72, 488, SW_W - 72 - 26, font_ui.height), line, st < 0 ? HEX(0xB02010) : HEX(0x1C2230), 0);
}

/* ---------------- state ---------------- */
static void enable(window_t *w, int id, int on) {
    widget_t *wd = widget_find(w, id);
    if (!wd) return;
    if (on) wd->style &= ~WS_DISABLED; else wd->style |= WS_DISABLED;
}

static void fill_events(window_t *w) {
    sndwin_t *c = w->data;
    char *items[SNDEV_COUNT];
    char buf[SNDEV_COUNT][48];
    for (int i = 0; i < SNDEV_COUNT; i++) {
        snprintf(buf[i], sizeof buf[i], "%s%s", sound_event_label(i), c->enabled[i] ? "" : "  (off)");
        items[i] = buf[i];
    }
    widget_t *lb = widget_find(w, ID_EVENTS);
    int top = lb->top;
    widget_list_set(w, lb, items, SNDEV_COUNT);
    lb->value = c->ev;
    lb->top = top;
}

static void update(window_t *w) {
    sndwin_t *c = w->data;
    int have = snddev_count() > 0;
    widget_find(w, ID_MUTE)->value = snd_muted();
    widget_find(w, ID_EV_ON)->value = c->enabled[c->ev];
    enable(w, ID_MUTE, have);
    enable(w, ID_TRAY, have);
    enable(w, ID_DEVS, have);
    enable(w, ID_SOUNDS_ON, have);
    int ev = have && c->scheme_on;
    enable(w, ID_EVENTS, ev);
    enable(w, ID_EV_ON, ev);
    enable(w, ID_PLAY, have && sound_event_loaded(c->ev) == 1);
    enable(w, ID_BROWSE, have);
    char key[48];
    snprintf(key, sizeof key, "sound.file.%s", sound_event_key(c->ev));
    const char *custom = settings_get(key, NULL);
    enable(w, ID_DEFAULT, have && custom && *custom);
    enable(w, ID_APPLY, c->dirty);
    wm_invalidate(w);
}

static void refresh_devices(window_t *w) {
    sndwin_t *c = w->data;
    widget_t *lb = widget_find(w, ID_DEVS);
    int n = MIN(snddev_count(), SNDDEV_MAX);
    char *items[SNDDEV_MAX];
    char buf[SNDDEV_MAX][96];
    for (int i = 0; i < n; i++) {
        snddev_t *d = snddev_get(i);
        snprintf(buf[i], sizeof buf[i], "%s: %s", d->name, d->desc);
        items[i] = buf[i];
    }
    if (lb->nitems != n) {
        widget_list_set(w, lb, items, n);
        lb->value = n ? 0 : -1;
    }
    c->gen = snd_generation();
    c->last_refresh = time_ms();
}

static void set_dirty(window_t *w) { ((sndwin_t *)w->data)->dirty = 1; update(w); }

static void apply(window_t *w) {
    sndwin_t *c = w->data;
    sound_scheme_set_on(c->scheme_on);
    sound_tray_set(c->tray);
    for (int i = 0; i < SNDEV_COUNT; i++) {
        sound_event_set_enabled(i, c->enabled[i]);
        kfree(c->file0[i]);                /* the Browse choices stay */
        c->file0[i] = NULL;
    }
    sound_scheme_save();
    c->vol0 = snd_volume();                /* (Cancel after Apply keeps them) */
    c->mute0 = snd_muted();
    c->dirty = 0;
    taskbar_invalidate();
    update(w);
}

/* ---------------- controls ---------------- */
static void on_mute(window_t *w, widget_t *wd) { sound_volume_set(snd_volume(), wd->value); update(w); }
static void on_tray(window_t *w, widget_t *wd) { ((sndwin_t *)w->data)->tray = wd->value; set_dirty(w); }
static void on_sounds_on(window_t *w, widget_t *wd) { ((sndwin_t *)w->data)->scheme_on = wd->value; set_dirty(w); }
static void on_dev(window_t *w, widget_t *wd) { (void)wd; wm_invalidate(w); }
static void on_event_sel(window_t *w, widget_t *lb) {
    sndwin_t *c = w->data;
    if (lb->value < 0 || lb->value >= SNDEV_COUNT) return;
    c->ev = lb->value;
    update(w);
}
static void on_ev_on(window_t *w, widget_t *wd) {
    sndwin_t *c = w->data;
    c->enabled[c->ev] = wd->value;
    fill_events(w);
    set_dirty(w);
}
static void on_play(window_t *w, widget_t *b) { (void)b; sound_event_force(((sndwin_t *)w->data)->ev); }
static void on_event_activate(window_t *w, widget_t *lb) { on_event_sel(w, lb); on_play(w, NULL); }

static void choose_file(window_t *w, const char *path) {
    sndwin_t *c = w->data;
    if (!c->file0[c->ev]) {                /* remember the scheme's choice for Cancel */
        char key[48];
        snprintf(key, sizeof key, "sound.file.%s", sound_event_key(c->ev));
        const char *f = settings_get(key, "");
        c->file0[c->ev] = strdup(f ? f : "");
    }
    sound_event_set_file(c->ev, path);
    set_dirty(w);
}
static void on_browse(window_t *w, widget_t *b) {
    (void)b;
    char out[VFS_MAX_PATH], dir[VFS_MAX_PATH];
    sound_event_file(((sndwin_t *)w->data)->ev, dir, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (!file_dialog(w, 0, "Browse for a sound", dir, "wav", out, sizeof out) || !wm_alive(w)) return;
    choose_file(w, out);
}
static void on_default(window_t *w, widget_t *b) { (void)b; choose_file(w, NULL); }

static void on_ok(window_t *w, widget_t *b) { (void)b; apply(w); wm_close(w); }
static void on_apply(window_t *w, widget_t *b) { (void)b; apply(w); }
static void on_cancel(window_t *w, widget_t *b) { (void)b; wm_close(w); }

/* the volume slider */
static void sw_event(window_t *w, gui_event_t *e) {
    sndwin_t *c = w->data;
    if (!snddev_count()) return;
    rect_t s = vol_slider();
    switch (e->type) {
    case GE_MOUSE_DOWN:
        if (e->button == MOUSE_LEFT && rect_contains(s, e->x, e->y)) {
            c->drag = 1;
            sound_volume_set(slider_value_at(s, 0, e->x, e->y), snd_muted());
            update(w);
        }
        break;
    case GE_MOUSE_MOVE: {
        int hot = rect_contains(slider_thumb(s, 0, snd_volume()), e->x, e->y);
        if (hot != c->hot) { c->hot = hot; wm_invalidate(w); }
        if (c->drag) {
            int v = slider_value_at(s, 0, e->x, e->y);
            if (v != snd_volume()) { sound_volume_set(v, snd_muted()); update(w); }
        }
        break;
    }
    case GE_MOUSE_UP:
        if (c->drag) { c->drag = 0; update(w); if (!snd_muted()) sound_event(SNDEV_DEFAULT); }
        break;
    case GE_WHEEL:
        if (rect_contains(mkrect(0, 0, SW_W, 130), e->x, e->y)) {
            sound_volume_set(CLAMP(snd_volume() + (e->dz > 0 ? 5 : -5), 0, 100), snd_muted());
            update(w);
        }
        break;
    }
}

static void sw_tick(window_t *w) {
    sndwin_t *c = w->data;
    if (snd_generation() != c->gen || time_ms() - c->last_refresh >= 1000) {
        refresh_devices(w);
        update(w);
    }
    widgets_tick(w);
}

static int sw_close(window_t *w) {
    sndwin_t *c = w->data;
    if (!c) return 1;
    /* Cancel / close: the volume and the files chosen with Browse go back */
    if (snd_volume() != c->vol0 || snd_muted() != c->mute0) sound_volume_set(c->vol0, c->mute0);
    for (int i = 0; i < SNDEV_COUNT; i++)
        if (c->file0[i]) { sound_event_set_file(i, c->file0[i][0] ? c->file0[i] : NULL); kfree(c->file0[i]); c->file0[i] = NULL; }
    return 1;
}

static void sw_destroy(window_t *w) {
    sndwin_t *c = w->data;
    if (c) for (int i = 0; i < SNDEV_COUNT; i++) kfree(c->file0[i]);
    kfree(c);
    w->data = NULL;
    if (sw_win == w) sw_win = NULL;
}

void app_sound(void) {
    if (sw_win && wm_alive(sw_win)) { wm_show(sw_win); return; }
    sndwin_t *c = kzalloc(sizeof *c);
    if (!c) return;
    c->vol0 = snd_volume();
    c->mute0 = snd_muted();
    c->scheme_on = sound_scheme_on();
    c->tray = sound_tray_enabled();
    for (int i = 0; i < SNDEV_COUNT; i++) c->enabled[i] = sound_event_enabled(i);
    window_t *w = wm_create("Sounds and Audio Devices", ICON_SPEAKER, -1, -1, SW_W, SW_H, WF_NO_MAXIMIZE);
    if (w) w->help = "sounds";              /* F1 */
    if (!w) { kfree(c); return; }
    sw_win = w;
    w->data = c;
    w->on_paint = sw_paint;
    w->on_event = sw_event;
    w->on_tick = sw_tick;
    w->on_close = sw_close;
    w->on_destroy = sw_destroy;

    widget_t *m = widget_add(w, W_CHECKBOX, mkrect(26, 100, 120, 18), "Mute", ID_MUTE);
    m->on_change = on_mute;
    widget_t *t = widget_add(w, W_CHECKBOX, mkrect(150, 100, 270, 18), "Place volume icon in the taskbar", ID_TRAY);
    t->value = c->tray;
    t->on_change = on_tray;
    widget_t *dl = widget_add(w, W_LISTBOX, mkrect(26, 162, SW_W - 52, 2 * 18 + 6), "", ID_DEVS);
    dl->on_change = on_dev;
    widget_t *so = widget_add(w, W_CHECKBOX, mkrect(26, 298, 300, 18), "Play system sounds", ID_SOUNDS_ON);
    so->value = c->scheme_on;
    so->on_change = on_sounds_on;
    widget_t *el = widget_add(w, W_LISTBOX, mkrect(26, 342, SW_W - 52, 5 * 18 + 2), "", ID_EVENTS);
    el->on_change = on_event_sel;
    el->on_activate = on_event_activate;
    widget_t *eo = widget_add(w, W_CHECKBOX, mkrect(26, 448, 110, 22), "Enabled", ID_EV_ON);
    eo->on_change = on_ev_on;
    widget_t *pl = widget_add(w, W_BUTTON, mkrect(SW_W - 26 - 3 * 75 - 2 * 6, 448, 75, 23), "Play", ID_PLAY);
    pl->on_click = on_play;
    strlcpy(pl->tip, "Play this sound (also when it is off)", sizeof pl->tip);
    widget_t *br = widget_add(w, W_BUTTON, mkrect(SW_W - 26 - 2 * 75 - 6, 448, 75, 23), "Browse...", ID_BROWSE);
    br->on_click = on_browse;
    strlcpy(br->tip, "Choose another WAV file for this event", sizeof br->tip);
    widget_t *df = widget_add(w, W_BUTTON, mkrect(SW_W - 26 - 75, 448, 75, 23), "Default", ID_DEFAULT);
    df->on_click = on_default;
    strlcpy(df->tip, "Use Halcyon's own sound for this event", sizeof df->tip);

    widget_t *ok = widget_add(w, W_BUTTON, mkrect(SW_W - 12 - 3 * 75 - 2 * 8, SW_H - 35, 75, 23), "OK", ID_OK);
    ok->style |= WS_DEFAULT; ok->on_click = on_ok;
    widget_t *cn = widget_add(w, W_BUTTON, mkrect(SW_W - 12 - 2 * 75 - 8, SW_H - 35, 75, 23), "Cancel", ID_CANCEL);
    cn->style |= WS_CANCEL; cn->on_click = on_cancel;
    widget_t *ap = widget_add(w, W_BUTTON, mkrect(SW_W - 12 - 75, SW_H - 35, 75, 23), "Apply", ID_APPLY);
    ap->on_click = on_apply;
    fill_events(w);
    refresh_devices(w);
    w->focus = ok;
    update(w);
}
