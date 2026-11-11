/* Halcyon OS - input events (keyboard + pointer). Key codes are USB HID
 * keyboard-page usages, PS/2 scancodes are translated into them. */
#ifndef INPUT_H
#define INPUT_H
#include "core/types.h"

enum {
    KEY_A = 0x04, KEY_Z = 0x1D, KEY_1 = 0x1E, KEY_0 = 0x27,
    KEY_ENTER = 0x28, KEY_ESC = 0x29, KEY_BACKSPACE = 0x2A, KEY_TAB = 0x2B, KEY_SPACE = 0x2C,
    KEY_CAPSLOCK = 0x39, KEY_F1 = 0x3A, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_PRINTSCREEN = 0x46, KEY_SCROLLLOCK = 0x47, KEY_PAUSE = 0x48, KEY_INSERT = 0x49,
    KEY_HOME = 0x4A, KEY_PAGEUP = 0x4B, KEY_DELETE = 0x4C, KEY_END = 0x4D, KEY_PAGEDOWN = 0x4E,
    KEY_RIGHT = 0x4F, KEY_LEFT = 0x50, KEY_DOWN = 0x51, KEY_UP = 0x52, KEY_NUMLOCK = 0x53,
    KEY_KP_SLASH = 0x54, KEY_KP_STAR = 0x55, KEY_KP_MINUS = 0x56, KEY_KP_PLUS = 0x57, KEY_KP_ENTER = 0x58,
    KEY_KP_1 = 0x59, KEY_KP_0 = 0x62, KEY_KP_DOT = 0x63, KEY_NONUS_BSLASH = 0x64, KEY_MENU = 0x65,
    KEY_NONUS_HASH = 0x32,          /* ISO key left of Enter (PS/2: the same scancode as \|) */
    KEY_KP_EQUAL = 0x67,
    KEY_KP_COMMA = 0x85,            /* Brazilian ABNT2 keypad "." (PS/2 set 1: 0x7E) */
    KEY_INTL1 = 0x87,               /* ABNT2 "/?" left of right Shift, JIS "\_" (PS/2: 0x73) */
    KEY_INTL2 = 0x88, KEY_INTL3 = 0x89, KEY_INTL4 = 0x8A, KEY_INTL5 = 0x8B,   /* JIS kana, yen, henkan, muhenkan */
    KEY_LCTRL = 0xE0, KEY_LSHIFT = 0xE1, KEY_LALT = 0xE2, KEY_LGUI = 0xE3,
    KEY_RCTRL = 0xE4, KEY_RSHIFT = 0xE5, KEY_RALT = 0xE6, KEY_RGUI = 0xE7,

    /* Consumer Control (usage page 0x0C) and System Control (Generic Desktop
     * 0x81-0x83) keys of multimedia keyboards, mapped by kernel/usb/hidparse.c.
     * The HID keyboard page leaves 0xE8-0xFF unused, so these live there.
     * Mute / Volume Up / Volume Down are the keyboard page's 0x7F-0x81. */
    KEY_MEDIA_PLAYPAUSE = 0xE8, KEY_MEDIA_NEXT = 0xE9, KEY_MEDIA_PREV = 0xEA, KEY_MEDIA_STOP = 0xEB,
    KEY_BROWSER_HOME = 0xEC, KEY_MAIL = 0xED, KEY_CALCULATOR = 0xEE,
    KEY_POWER = 0xEF, KEY_SLEEP = 0xF0, KEY_WAKE = 0xF1,
    KEY_BROWSER_BACK = 0xF2, KEY_BROWSER_FORWARD = 0xF3, KEY_BROWSER_REFRESH = 0xF4,
    KEY_BROWSER_SEARCH = 0xF5, KEY_BROWSER_FAVORITES = 0xF6, KEY_MY_COMPUTER = 0xF7,
};

#define MOD_CTRL  0x01
#define MOD_SHIFT 0x02
#define MOD_ALT   0x04
#define MOD_GUI   0x08

#define LED_NUM    0x01
#define LED_CAPS   0x02
#define LED_SCROLL 0x04

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

enum { EV_KEY = 1, EV_MOUSE = 2 };

typedef struct input_event {
    u8 type;
    /* EV_KEY */
    u8 key;          /* KEY_* / HID usage */
    u8 pressed;      /* 1 = down (or auto-repeat), 0 = up */
    u8 mods;         /* MOD_* at the time of the event */
    u8 vk;           /* the key's letter 'A'..'Z' or digit '0'..'9' on the active layout, for
                        shortcuts (US letters on Russian and Greek, as Windows), or 0 */
    u32 ch;          /* the character typed (Unicode code point) or 0; key 0 = the accent of a
                        dead key that did not compose, sent just before the key's own event */
    /* EV_MOUSE */
    u8 buttons;      /* MOUSE_* currently held */
    u8 absolute;     /* x/y are absolute (0..32767 over the screen) */
    i32 dx, dy, dz;  /* relative motion / wheel (dz > 0 = wheel up) */
    i32 x, y;        /* absolute position when absolute */
} input_event_t;

/* Ctrl+letter shortcut by the letter on the active layout (Ctrl+A is the key
 * labelled A on AZERTY too); not with Alt, which makes Ctrl+Alt AltGr */
static inline int key_shortcut(const input_event_t *k, char letter) {
    return (k->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL && k->vk == (u8)letter;
}
/* the event types a printable character (AltGr and dead keys included) */
static inline int key_is_text(const input_event_t *k) { return k->ch >= 32 && k->ch != 127; }

/* producers (drivers) */
void input_key(u8 key, int pressed);
void input_mouse_rel(int dx, int dy, int dz, u8 buttons);
void input_mouse_abs(int x, int y, int dz, u8 buttons);   /* x, y in 0..32767 */

/* consumers */
void input_poll_devices(void);          /* service PS/2 + USB */
int  input_pending(void);
int  input_get(input_event_t *ev);      /* 1 if an event was dequeued */
u8   input_mods(void);
u8   input_leds(void);                  /* LED_* lock state */
u32  input_led_generation(void);        /* changes when lock state changes */
int  input_key_down(u8 key);

/* keyboard layouts (drivers/keymap.h): indexes into kl_layouts[]. Several
 * can be installed; Left Alt+Shift cycles through them. */
#define INPUT_MAX_LAYOUTS 32
int  input_layout(void);                         /* the active one */
void input_layout_select(int idx);               /* activate an installed layout */
void input_layout_next(void);
int  input_layouts(int *idx, int max);           /* installed, in order; returns the count */
int  input_layout_default(void);
/* install these (US if none is valid); the active layout stays if it is
 * still installed, else the default becomes active */
void input_layouts_set(const int *idx, int n, int deflt);
u32  input_layout_generation(void);              /* changes with the active layout or the list */

/* PS/2 */
void ps2_init(void);
void ps2_poll(void);
int  ps2_have_keyboard(void);
int  ps2_have_mouse(void);

#endif
