/* Halcyon OS - keyboard layouts: HID usages to Unicode characters.
 *
 * Each layout maps a key to up to four levels (base, Shift, AltGr,
 * Shift+AltGr), with a per-key Caps Lock rule, and has a table of dead keys
 * that compose with the next character. Layouts follow Windows: AltGr is
 * Right Alt (on layouts that have AltGr) or Ctrl+Alt, a dead key followed by
 * a character it does not compose with gives the accent and then that
 * character, and Ctrl shortcuts use each key's Windows virtual-key letter
 * (the AZERTY "A" key is Ctrl+A; Russian and Greek keys use the US letters).
 *
 * This file and keymap.c / keylayouts.c do not depend on the rest of the
 * kernel (tests/host/kbdtest.c builds them on the host). */
#ifndef KEYMAP_H
#define KEYMAP_H
#include "core/types.h"

#define KL_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* kl_key_t.flags: what Caps Lock does to the key */
#define KC_CAPS    0x01   /* acts as Shift on the base and Shift levels */
#define KC_CAPS_AG 0x02   /* acts as Shift on the AltGr levels */
#define KC_SGCAPS  0x04   /* selects caps[] instead of the base and Shift levels */

typedef struct kl_key {
    u8 usage;             /* HID keyboard usage */
    u8 vk;                /* 'A'..'Z' or '0'..'9': the key's virtual-key letter or digit; 0 = other */
    u8 flags;             /* KC_* */
    u8 dead;              /* bit n: level n is a dead key (c[n] is its accent) */
    u16 c[4];             /* base, Shift, AltGr, Shift+AltGr; 0 = nothing */
    u16 caps[2];          /* KC_SGCAPS: Caps Lock, Caps Lock+Shift */
} kl_key_t;

typedef struct kl_dead {
    u16 accent;           /* the dead key's own character */
    const char *from;     /* UTF-8: the n-th character of from ... */
    const char *to;       /* ... composes to the n-th character of to */
} kl_dead_t;

typedef struct kl_layout {
    const char *id;       /* settings name, e.g. "de" */
    const char *name;     /* "German" */
    const char *abbr;     /* taskbar indicator, e.g. "DE" */
    u8 altgr;             /* Right Alt is AltGr */
    u16 kp_decimal;       /* keypad decimal key with Num Lock */
    const kl_key_t *keys; int nkeys;
    const kl_dead_t *dead; int ndead;
} kl_layout_t;

extern const kl_layout_t kl_layouts[];
extern const int kl_layout_count;

/* translation state: the layout and a pending dead key */
typedef struct kl_state {
    const kl_layout_t *layout;
    u32 dead;             /* accent of the pending dead key, 0 = none */
} kl_state_t;

/* kl_translate() modifier and lock bits */
#define KLM_SHIFT 0x01
#define KLM_CTRL  0x02
#define KLM_ALT   0x04    /* either Alt */
#define KLM_RALT  0x08    /* Right Alt (AltGr on layouts that have it) */
#define KLM_GUI   0x10
#define KLM_CAPS  0x20    /* Caps Lock on */
#define KLM_NUM   0x40    /* Num Lock on */

/* A key press: writes up to two characters to out (the accent of a dead key
 * that did not compose, then the key's own character) and returns how many.
 * A dead key returns 0 and stays pending. Ctrl+letter gives the control
 * character (1..26). */
int kl_translate(kl_state_t *s, u8 usage, u32 mods, u32 out[2]);
/* the key's virtual-key letter / digit on layout l, or 0 */
u8 kl_vk(const kl_layout_t *l, u8 usage);
const kl_key_t *kl_key(const kl_layout_t *l, u8 usage);
/* dead key composition: the composed character or 0 */
u32 kl_compose(const kl_layout_t *l, u32 accent, u32 c);
/* layout index by id (case-insensitive), or -1 */
int kl_find(const char *id);

#endif
