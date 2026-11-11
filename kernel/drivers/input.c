/* Halcyon OS - input event queue and US keyboard layout */
#include "input.h"
#include "core/lib.h"

#define QSIZE 256
static input_event_t queue[QSIZE];
static int qhead, qtail;
static u8 keys_down[256];
static u8 leds = LED_NUM;
static u32 led_gen;

void usb_poll(void) __attribute__((weak));

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

/* US layout: HID usage -> {normal, shifted} */
static const char keymap[][2] = {
    [0x04] = {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'}, {'f', 'F'}, {'g', 'G'},
    {'h', 'H'}, {'i', 'I'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'},
    {'p', 'P'}, {'q', 'Q'}, {'r', 'R'}, {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'},
    {'x', 'X'}, {'y', 'Y'}, {'z', 'Z'},
    [0x1E] = {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'},
    {'8', '*'}, {'9', '('}, {'0', ')'},
    [0x28] = {'\n', '\n'}, {27, 27}, {'\b', '\b'}, {'\t', '\t'}, {' ', ' '},
    [0x2D] = {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'}, {'#', '~'}, {';', ':'},
    {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'}, {'/', '?'},
    [0x54] = {'/', '/'}, {'*', '*'}, {'-', '-'}, {'+', '+'}, {'\n', '\n'},
    [0x64] = {'\\', '|'},
};
static const char keypad_num[] = "1234567890.";

static char translate(u8 key, u8 mods) {
    if (mods & (MOD_CTRL | MOD_ALT | MOD_GUI)) {
        if ((mods & MOD_CTRL) && key >= 0x04 && key <= 0x1D) return (char)(key - 0x04 + 1);  /* ^A.. */
        return 0;
    }
    if (key >= KEY_KP_1 && key <= KEY_KP_DOT) {
        if (leds & LED_NUM) return keypad_num[key - KEY_KP_1];
        return 0;
    }
    if (key >= ARRAY_SIZE(keymap)) return 0;
    int shift = (mods & MOD_SHIFT) != 0;
    if (key >= 0x04 && key <= 0x1D && (leds & LED_CAPS)) shift = !shift;
    return keymap[key][shift];
}

void input_key(u8 key, int pressed) {
    if (pressed && !keys_down[key]) {
        if (key == KEY_CAPSLOCK) { leds ^= LED_CAPS; led_gen++; }
        if (key == KEY_NUMLOCK) { leds ^= LED_NUM; led_gen++; }
        if (key == KEY_SCROLLLOCK) { leds ^= LED_SCROLL; led_gen++; }
    }
    keys_down[key] = pressed ? 1 : 0;
    input_event_t e = { 0 };
    e.type = EV_KEY;
    e.key = key;
    e.pressed = pressed ? 1 : 0;
    e.mods = input_mods();
    e.ch = pressed ? translate(key, e.mods) : 0;
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
