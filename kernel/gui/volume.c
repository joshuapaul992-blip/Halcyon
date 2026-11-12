/* Halcyon OS - the volume UI (see volume.h and docs/DESIGN-M7.md).
 *
 * The speaker in the tray shows the master volume (crossed out when muted);
 * a click opens a small popup with a vertical slider and a Mute box, a
 * double click Sounds and Audio Devices, and resting the pointer on it shows
 * the level. The keyboard's Mute / Volume Up / Volume Down keys change the
 * volume in steps of 5 % and show an indicator over everything, which fades
 * out after 1.5 s. Volume changes apply at once; sound/sndevent.c writes
 * them to SETTINGS.INI a second after the last one, from its own thread. */
#include "volume.h"
#include "core/lib.h"
#include "core/kernel.h"
#include "core/irq.h"
#include "core/kmath.h"
#include "sound/snddev.h"
#include "sound/sound.h"

#define KEY_VOL_MUTE 0x7F            /* HID keyboard page (input.h) */
#define KEY_VOL_UP   0x80
#define KEY_VOL_DOWN 0x81
#define STEP 5
#define OSD_SHOW_MS 1500
#define OSD_FADE_MS 400
#define OSD_W 236
#define OSD_H 76
#define TIP_DELAY_MS 500

static u32 seen_gen;
static int seen_tray = -1;

/* ---------------- the trackbar ---------------- */
#define THUMB_LONG 22                /* across the channel */
#define THUMB_SHORT 11               /* along it */

static int span(rect_t r, int vertical) { return (vertical ? r.h : r.w) - THUMB_SHORT; }

rect_t slider_thumb(rect_t r, int vertical, int value) {
    int s = span(r, vertical), off = s * CLAMP(value, 0, 100) / 100;
    if (vertical) return mkrect(r.x + (r.w - THUMB_LONG) / 2, r.y + s - off, THUMB_LONG, THUMB_SHORT);
    return mkrect(r.x + off, r.y + (r.h - THUMB_LONG) / 2, THUMB_SHORT, THUMB_LONG);
}

int slider_value_at(rect_t r, int vertical, int x, int y) {
    int s = span(r, vertical);
    int p = vertical ? s - (y - r.y - THUMB_SHORT / 2) : x - r.x - THUMB_SHORT / 2;
    return s > 0 ? CLAMP((p * 100 + s / 2) / s, 0, 100) : 0;
}

void slider_paint(gc_t *g, rect_t r, int vertical, int value, int hot, int disabled) {
    const theme_t *th = g_theme;
    rect_t t = slider_thumb(r, vertical, value);
    /* the channel: sunken, filled up to the thumb */
    rect_t ch = vertical ? mkrect(r.x + r.w / 2 - 2, r.y + THUMB_SHORT / 2, 4, r.h - THUMB_SHORT)
                         : mkrect(r.x + THUMB_SHORT / 2, r.y + r.h / 2 - 2, r.w - THUMB_SHORT, 4);
    gfx_fill(g, ch, HEX(0xF4F3EE));
    if (!disabled) {
        rect_t f = vertical ? mkrect(ch.x, t.y + t.h / 2, ch.w, ch.y + ch.h - t.y - t.h / 2)
                            : mkrect(ch.x, ch.y, t.x + t.w / 2 - ch.x, ch.h);
        gfx_fill(g, f, color_lighten(th->select, 40));
    }
    gfx_hline(g, ch.x, ch.y, ch.w, HEX(0x9D9C99));
    gfx_vline(g, ch.x, ch.y, ch.h, HEX(0x9D9C99));
    gfx_hline(g, ch.x, ch.y + ch.h - 1, ch.w, HEX(0xFFFFFF));
    gfx_vline(g, ch.x + ch.w - 1, ch.y, ch.h, HEX(0xFFFFFF));
    /* ticks every 10 % */
    for (int i = 0; i <= 10; i++) {
        rect_t k = slider_thumb(r, vertical, i * 10);
        if (vertical) gfx_hline(g, r.x + r.w - 4, k.y + k.h / 2, 4, HEX(0xA0A0A0));
        else gfx_vline(g, k.x + k.w / 2, r.y + r.h - 4, 4, HEX(0xA0A0A0));
    }
    /* the thumb */
    color_t top = disabled ? HEX(0xF0F0EC) : HEX(0xFFFFFF), bot = disabled ? HEX(0xE0E0DA) : HEX(0xE3E1D8);
    gfx_rounded(g, t, 2, 15, top, bot);
    gfx_rounded_outline(g, t, 2, 15, disabled ? HEX(0xC0C0B8) : th->button_border);
    if (!disabled) {
        color_t edge = hot ? HEX(0xF8B030) : HEX(0x5CB04C);
        if (vertical) { gfx_vline(g, t.x + 1, t.y + 2, t.h - 4, edge); gfx_vline(g, t.x + t.w - 2, t.y + 2, t.h - 4, edge); }
        else { gfx_hline(g, t.x + 2, t.y + 1, t.w - 4, edge); gfx_hline(g, t.x + 2, t.y + t.h - 2, t.w - 4, edge); }
    }
}

/* ---------------- changing the volume ---------------- */
static void osd_show(void);

static void set_volume(int v, int mute) {
    sound_volume_set(CLAMP(v, 0, 100), mute);
}

int volume_key(u8 key, int pressed) {
    if (key != KEY_VOL_MUTE && key != KEY_VOL_UP && key != KEY_VOL_DOWN) return 0;
    if (!snddev_count()) return 0;
    if (!pressed) return 1;
    int v = snd_volume(), m = snd_muted();
    if (key == KEY_VOL_MUTE) m = !m;
    else if (key == KEY_VOL_UP) { v = MIN(100, (v / STEP + 1) * STEP); m = 0; }
    else v = MAX(0, (v + STEP - 1) / STEP * STEP - STEP);
    set_volume(v, m);
    osd_show();
    return 1;
}

/* ---------------- the popup ---------------- */
#define POP_W 76
#define POP_H 172
#define ID_MUTE 1
static window_t *pop;
static u64 pop_closed_ms;
static int pop_drag, pop_hot;

static rect_t pop_slider(void) { return mkrect((POP_W - 30) / 2, 30, 30, 104); }

static void pop_paint(window_t *w, gc_t *g) {
    const theme_t *th = g_theme;
    rect_t r = mkrect(0, 0, w->client->w, w->client->h);
    gfx_fill(g, r, th->face);
    gfx_rect(g, r, color_lighten(th->frame, -20));
    gfx_text_box(g, &font_ui, mkrect(0, 6, r.w, 16), "Volume", HEX(0x000000), 1);
    slider_paint(g, pop_slider(), 1, snd_volume(), pop_hot || pop_drag, 0);
    gfx_hline(g, 8, 140, r.w - 16, color_lighten(th->face, -30));
}

static void pop_set_from(window_t *w, int x, int y) {
    int v = slider_value_at(pop_slider(), 1, x, y);
    if (v != snd_volume()) { set_volume(v, snd_muted()); wm_invalidate(w); }
}

static void pop_event(window_t *w, gui_event_t *e) {
    rect_t s = pop_slider();
    switch (e->type) {
    case GE_MOUSE_DOWN:
        if (e->button == MOUSE_LEFT && rect_contains(mkrect(s.x - 8, s.y - 4, s.w + 16, s.h + 8), e->x, e->y)) {
            pop_drag = 1;
            pop_set_from(w, e->x, e->y);
        }
        break;
    case GE_MOUSE_MOVE: {
        int hot = rect_contains(slider_thumb(s, 1, snd_volume()), e->x, e->y);
        if (hot != pop_hot) { pop_hot = hot; wm_invalidate(w); }
        if (pop_drag) pop_set_from(w, e->x, e->y);
        break;
    }
    case GE_MOUSE_UP:
        if (pop_drag) { pop_drag = 0; wm_invalidate(w); if (!snd_muted()) sound_event(SNDEV_DEFAULT); }
        break;
    case GE_WHEEL:
        set_volume(snd_volume() + (e->dz > 0 ? STEP : -STEP), snd_muted());
        wm_invalidate(w);
        break;
    case GE_KEY:
        if (!e->key.pressed) break;
        switch (e->key.key) {
        case KEY_ESC: case KEY_ENTER: wm_destroy(w); return;
        case KEY_UP: case KEY_RIGHT: set_volume(snd_volume() + STEP, snd_muted()); break;
        case KEY_DOWN: case KEY_LEFT: set_volume(snd_volume() - STEP, snd_muted()); break;
        case KEY_PAGEUP: set_volume(snd_volume() + 20, snd_muted()); break;
        case KEY_PAGEDOWN: set_volume(snd_volume() - 20, snd_muted()); break;
        case KEY_HOME: set_volume(100, snd_muted()); break;
        case KEY_END: set_volume(0, snd_muted()); break;
        }
        wm_invalidate(w);
        break;
    }
}

static void pop_mute(window_t *w, widget_t *wd) {
    set_volume(snd_volume(), wd->value);
    wm_invalidate(w);
}

static void pop_destroy(window_t *w) {
    if (pop == w) pop = NULL;
    pop_closed_ms = time_ms();
    pop_drag = pop_hot = 0;
}

static void pop_open(rect_t icon) {
    rect_t tb = taskbar_rect();
    int x = CLAMP(icon.x + icon.w / 2 - POP_W / 2, tb.x, tb.x + tb.w - POP_W);
    int y = tb.y - POP_H - 2;
    pop = wm_create("Volume", -1, x, y, POP_W, POP_H, WF_POPUP | WF_TOPMOST);
    if (!pop) return;
    pop->on_paint = pop_paint;
    pop->on_event = pop_event;
    pop->on_destroy = pop_destroy;
    widget_t *m = widget_add(pop, W_CHECKBOX, mkrect(12, 147, POP_W - 16, 18), "Mute", ID_MUTE);
    m->value = snd_muted();
    m->on_change = pop_mute;
}

/* ---------------- the tray icon and its tooltip ---------------- */
static int tip_on, tip_shown;
static u64 tip_since;
static rect_t tip_icon, tip_rect;

int volume_tray_visible(void) { return snddev_count() > 0 && sound_tray_enabled(); }

void volume_tray_paint(gc_t *g, int x, int y, int hover) {
    if (hover) gfx_fill_alpha(g, mkrect(x - 3, y - 2, 22, 20), HEX(0xFFFFFF), 70);
    gfx_icon(g, x, y, &icons16[snd_muted() ? ICON_SPEAKER_MUTED : ICON_SPEAKER]);
}

static void tip_hide(void) {
    if (tip_shown) wm_invalidate_rect(tip_rect);
    tip_shown = 0;
}

void volume_tray_click(rect_t icon, int dbl) {
    tip_on = 0;
    tip_hide();
    if (dbl) {
        if (pop) wm_destroy(pop);
        app_sound();
        return;
    }
    if (pop) { wm_destroy(pop); return; }
    if (time_ms() - pop_closed_ms < 150) return;    /* this click closed it (popups close on any click outside) */
    pop_open(icon);
}

void volume_tray_hover(rect_t icon, int on) {
    if (on == tip_on) return;
    tip_on = on;
    tip_icon = icon;
    tip_since = time_ms();
    if (!on) tip_hide();
}

static void tip_text(char *out, int n) {
    if (snd_muted()) snprintf(out, n, "Volume: muted (%d%%)", snd_volume());
    else snprintf(out, n, "Volume: %d%%", snd_volume());
}

/* ---------------- the on-screen indicator ---------------- */
static u64 osd_t0;
static int osd_on;

static rect_t osd_rect(void) { rect_t tb = taskbar_rect(); return mkrect(tb.x + (tb.w - OSD_W) / 2, tb.y - OSD_H - 70, OSD_W, OSD_H); }

static void osd_show(void) {
    osd_on = 1;
    osd_t0 = time_ms();
    wm_invalidate_rect(osd_rect());
    idle_wake_at(osd_t0 + 50);
}

static int osd_alpha(u64 now) {
    u64 t = now - osd_t0;
    if (t < OSD_SHOW_MS) return 255;
    if (t >= OSD_SHOW_MS + OSD_FADE_MS) return 0;
    return (int)(255 - (t - OSD_SHOW_MS) * 255 / OSD_FADE_MS);
}

/* a rounded translucent panel */
static void panel(gc_t *g, rect_t r, color_t c, int alpha, int radius) {
    for (int y = 0; y < r.h; y++) {
        int d = y < radius ? radius - y : y >= r.h - radius ? y - (r.h - radius - 1) : 0;
        int in = 0;
        if (d) in = radius - (int)k_sqrt((double)(radius * radius - d * d));
        gfx_fill_alpha(g, mkrect(r.x + in, r.y + y, r.w - 2 * in, 1), c, alpha);
    }
}

void gui_overlay_paint(gc_t *g, rect_t clip) {
    u64 now = time_ms();
    if (osd_on) {
        rect_t r = osd_rect();
        int a = osd_alpha(now);
        if (a > 0 && !rect_empty(rect_intersect(r, clip))) {
            panel(g, r, HEX(0x101828), a * 190 / 255, 12);
            int m = snd_muted(), v = snd_volume();
            gfx_icon_alpha(g, r.x + 14, r.y + (r.h - 32) / 2, &icons32[m ? ICON_SPEAKER_MUTED : ICON_SPEAKER], a);
            /* 20 segments */
            int bx = r.x + 58, by = r.y + 38, n = 20, sw = 7, gap = 1;
            for (int i = 0; i < n; i++) {
                int lit = !m && v * n >= (i + 1) * 100 - 50;
                gfx_fill_alpha(g, mkrect(bx + i * (sw + gap), by, sw, 14), lit ? HEX(0xFFFFFF) : HEX(0x5A6478), a * (lit ? 230 : 160) / 255);
            }
            char t[32];
            if (m) snprintf(t, sizeof t, "Muted");
            else snprintf(t, sizeof t, "Volume %d%%", v);
            gfx_text(g, &font_ui_bold, bx, r.y + 14, t, color_mix(HEX(0x30384A), HEX(0xFFFFFF), a));
        }
    }
    if (tip_shown) {
        if (!rect_empty(rect_intersect(tip_rect, clip))) {
            char t[40];
            tip_text(t, sizeof t);
            gfx_fill(g, tip_rect, HEX(0xFFFFE1));
            gfx_rect(g, tip_rect, HEX(0x767676));
            gfx_text_box(g, &font_ui, mkrect(tip_rect.x + 5, tip_rect.y, tip_rect.w - 8, tip_rect.h), t, HEX(0x000000), 0);
        }
    }
}

/* ---------------- shell tick ---------------- */
void volume_tick(void) {
    u64 now = time_ms();
    int tray = volume_tray_visible();
    if (snd_generation() != seen_gen || tray != seen_tray) {
        seen_gen = snd_generation();
        seen_tray = tray;
        taskbar_invalidate();
        if (pop) {
            widget_t *m = widget_find(pop, ID_MUTE);
            if (m && m->value != snd_muted()) m->value = snd_muted();
            wm_invalidate(pop);
        }
        if (osd_on) wm_invalidate_rect(osd_rect());
        if (tip_shown) wm_invalidate_rect(tip_rect);
    }
    if (pop && !tray) wm_destroy(pop);
    if (osd_on) {
        if (osd_alpha(now) == 0) osd_on = 0;
        else if (now - osd_t0 >= OSD_SHOW_MS - 100) idle_wake_at(now + 40);
        else idle_wake_at(osd_t0 + OSD_SHOW_MS);
        if (now - osd_t0 >= OSD_SHOW_MS) wm_invalidate_rect(osd_rect());
        if (!osd_on) wm_invalidate_rect(osd_rect());
    }
    if (tip_on && !tip_shown && !pop && tray) {
        if (now - tip_since >= TIP_DELAY_MS) {
            char t[40];
            tip_text(t, sizeof t);
            int w = gfx_text_width(&font_ui, t) + 12, h = font_ui.height + 6;
            rect_t tb = taskbar_rect();
            tip_rect = mkrect(CLAMP(tip_icon.x + tip_icon.w / 2 - w / 2, tb.x, tb.x + tb.w - w), tip_icon.y - h - 2, w, h);
            tip_shown = 1;
            wm_invalidate_rect(tip_rect);
        } else idle_wake_at(tip_since + TIP_DELAY_MS);
    }
    if (tip_shown && (!tip_on || pop)) tip_hide();
}
