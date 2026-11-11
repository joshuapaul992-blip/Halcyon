/* Halcyon OS - input event queue, keyboard layout state */
#include "input.h"
#include "keymap.h"
#include "core/lib.h"

#define QSIZE 256
static input_event_t queue[QSIZE];
static int qhead, qtail;
static u8 keys_down[256];
static u8 leds = LED_NUM;
static u32 led_gen;

void usb_poll(void) __attribute__((weak));
void ahci_poll(void) __attribute__((weak));   /* CD/DVD media change */

static void push(const input_event_t *e) {
    int n = (qhead + 1) % QSIZE;
    if (n == qtail) {
        /* full: coalesce pointer motion into the last event if possible */
        return;
    }
    queue[qhead] = *e;
    qhead = n;
}

int input_pending(void) { return qtail != qhead; }

int input_get(input_event_t *ev) {
    if (qtail == qhead) return 0;
    *ev = queue[qtail];
    qtail = (qtail + 1) % QSIZE;
    return 1;
}

void input_poll_devices(void) {
    ps2_poll();
    if (usb_poll) usb_poll();
    if (ahci_poll) ahci_poll();
}

u8 input_mods(void) {
    u8 m = 0;
    if (keys_down[KEY_LCTRL] || keys_down[KEY_RCTRL]) m |= MOD_CTRL;
    if (keys_down[KEY_LSHIFT] || keys_down[KEY_RSHIFT]) m |= MOD_SHIFT;
    if (keys_down[KEY_LALT] || keys_down[KEY_RALT]) m |= MOD_ALT;
    if (keys_down[KEY_LGUI] || keys_down[KEY_RGUI]) m |= MOD_GUI;
    return m;
}
u8 input_leds(void) { return leds; }
u32 input_led_generation(void) { return led_gen; }
int input_key_down(u8 key) { return keys_down[key]; }

/* ---------------- keyboard layouts ---------------- */
static int installed[INPUT_MAX_LAYOUTS] = { 0 }, ninstalled = 1;   /* kl_layouts[0] is US */
static int active, deflt;                   /* kl_layouts indexes */
static kl_state_t kst = { &kl_layouts[0], 0 };
static u32 layout_gen;
static int hotkey_armed;                    /* Left Alt+Shift held with nothing else */

int input_layout(void) { return active; }
int input_layout_default(void) { return deflt; }
u32 input_layout_generation(void) { return layout_gen; }

int input_layouts(int *idx, int max) {
    int n = MIN(ninstalled, max);
    for (int i = 0; i < n; i++) idx[i] = installed[i];
    return n;
}

void input_layout_select(int idx) {
    if (idx < 0 || idx >= kl_layout_count) return;
    active = idx;
    kst.layout = &kl_layouts[idx];
    kst.dead = 0;
    layout_gen++;
}

void input_layout_next(void) {
    if (ninstalled < 2) return;
    int pos = 0;
    for (int i = 0; i < ninstalled; i++) if (installed[i] == active) pos = i;
    input_layout_select(installed[(pos + 1) % ninstalled]);
}

void input_layouts_set(const int *idx, int n, int def) {
    int m = 0;
    for (int i = 0; i < n && m < INPUT_MAX_LAYOUTS; i++) {
        if (idx[i] < 0 || idx[i] >= kl_layout_count) continue;
        int dup = 0;
        for (int j = 0; j < m; j++) if (installed[j] == idx[i]) dup = 1;
        if (!dup) installed[m++] = idx[i];
    }
    if (!m) installed[m++] = 0;
    ninstalled = m;
    int ok = 0, keep = 0;
    for (int i = 0; i < m; i++) {
        if (installed[i] == def) ok = 1;
        if (installed[i] == active) keep = 1;
    }
    deflt = ok ? def : installed[0];
    if (keep) layout_gen++;
    else input_layout_select(deflt);
}

static int is_modifier(u8 key) { return key >= KEY_LCTRL && key <= KEY_RGUI; }

/* Left Alt+Shift switches the layout when released, if no other key was
 * pressed meanwhile (as Windows: Alt+Shift+Tab does not switch) */
static void layout_hotkey(u8 key, int pressed) {
    if (pressed) {
        int lalt_shift = keys_down[KEY_LALT] && (keys_down[KEY_LSHIFT] || keys_down[KEY_RSHIFT]);
        int others = keys_down[KEY_LCTRL] || keys_down[KEY_RCTRL] || keys_down[KEY_RALT] ||
                     keys_down[KEY_LGUI] || keys_down[KEY_RGUI];
        if (!is_modifier(key) || others) hotkey_armed = 0;
        else if (lalt_shift) hotkey_armed = 1;
    } else if (hotkey_armed && (key == KEY_LALT || key == KEY_LSHIFT || key == KEY_RSHIFT)) {
        hotkey_armed = 0;
        input_layout_next();
    }
}

void input_key(u8 key, int pressed) {
    if (pressed && !keys_down[key]) {
        if (key == KEY_CAPSLOCK) { leds ^= LED_CAPS; led_gen++; }
        if (key == KEY_NUMLOCK) { leds ^= LED_NUM; led_gen++; }
        if (key == KEY_SCROLLLOCK) { leds ^= LED_SCROLL; led_gen++; }
    }
    keys_down[key] = pressed ? 1 : 0;
    layout_hotkey(key, pressed);
    input_event_t e = { 0 };
    e.type = EV_KEY;
    e.key = key;
    e.pressed = pressed ? 1 : 0;
    e.mods = input_mods();
    e.vk = kl_vk(kst.layout, key);
    if (pressed) {
        u32 m = 0, c[2];
        if (e.mods & MOD_SHIFT) m |= KLM_SHIFT;
        if (e.mods & MOD_CTRL) m |= KLM_CTRL;
        if (e.mods & MOD_ALT) m |= KLM_ALT;
        if (e.mods & MOD_GUI) m |= KLM_GUI;
        if (keys_down[KEY_RALT]) m |= KLM_RALT;
        if (leds & LED_CAPS) m |= KLM_CAPS;
        if (leds & LED_NUM) m |= KLM_NUM;
        int n = kl_translate(&kst, key, m, c);
        if (n == 2) {
            /* a dead key that did not compose: its accent first, as a key-less event */
            input_event_t a = e;
            a.key = 0;
            a.vk = 0;
            a.ch = c[0];
            push(&a);
        }
        if (n) e.ch = c[n - 1];
    }
    push(&e);
}

void input_mouse_rel(int dx, int dy, int dz, u8 buttons) {
    /* merge with the previous queued motion event if buttons are unchanged */
    if (qhead != qtail) {
        input_event_t *last = &queue[(qhead + QSIZE - 1) % QSIZE];
        if (last->type == EV_MOUSE && !last->absolute && last->buttons == buttons && dz == 0 && last->dz == 0) {
            last->dx += dx; last->dy += dy;
            return;
        }
    }
    input_event_t e = { 0 };
    e.type = EV_MOUSE;
    e.dx = dx; e.dy = dy; e.dz = dz;
    e.buttons = buttons;
    push(&e);
}

void input_mouse_abs(int x, int y, int dz, u8 buttons) {
    if (qhead != qtail) {
        input_event_t *last = &queue[(qhead + QSIZE - 1) % QSIZE];
        if (last->type == EV_MOUSE && last->absolute && last->buttons == buttons && dz == 0 && last->dz == 0) {
            last->x = x; last->y = y;
            return;
        }
    }
    input_event_t e = { 0 };
    e.type = EV_MOUSE;
    e.absolute = 1;
    e.x = x; e.y = y; e.dz = dz;
    e.buttons = buttons;
    push(&e);
}
