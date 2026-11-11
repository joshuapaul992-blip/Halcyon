/* Halcyon OS - keyboard layout engine (see keymap.h; the tables are in
 * keylayouts.c) */
#include "keymap.h"
#include "input.h"
#include "core/unicode.h"

const kl_key_t *kl_key(const kl_layout_t *l, u8 usage) {
    if (!l) return NULL;
    if (usage == 0x32) usage = 0x31;    /* ISO "#~" key: the same key as "\|" (scancode 0x2B) */
    for (int i = 0; i < l->nkeys; i++) if (l->keys[i].usage == usage) return &l->keys[i];
    return NULL;
}

u8 kl_vk(const kl_layout_t *l, u8 usage) {
    const kl_key_t *k = kl_key(l, usage);
    return k ? k->vk : 0;
}

u32 kl_compose(const kl_layout_t *l, u32 accent, u32 c) {
    for (int i = 0; l && i < l->ndead; i++) {
        if (l->dead[i].accent != accent) continue;
        const char *f = l->dead[i].from, *t = l->dead[i].to;
        u32 a, b;
        while ((a = utf8_next(&f)) != 0 && (b = utf8_next(&t)) != 0)
            if (a == c) return b;
        return 0;
    }
    return 0;
}

int kl_find(const char *id) {
    for (int i = 0; id && i < kl_layout_count; i++) {
        const char *a = kl_layouts[i].id, *b = id;
        while (*a && (*a == *b || (*b >= 'A' && *b <= 'Z' && *a == *b + 32))) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

enum { K_CHAR, K_DEAD, K_CTRL };

/* the character a key gives before dead key processing; *kind says whether
 * it is a dead key's accent or a Ctrl+letter control character */
static u32 key_char(const kl_layout_t *l, u8 usage, u32 mods, int *kind) {
    *kind = K_CHAR;
    int ctrl = (mods & KLM_CTRL) != 0, alt = (mods & KLM_ALT) != 0;
    int altgr = (ctrl && alt) || (l->altgr && (mods & KLM_RALT));
    int shift = (mods & KLM_SHIFT) != 0, caps = (mods & KLM_CAPS) != 0;
    const kl_key_t *k = kl_key(l, usage);

    if (!altgr && (ctrl || alt || (mods & KLM_GUI))) {
        /* shortcuts: Ctrl+letter is a control character, the rest nothing */
        if (ctrl && !alt && k && k->vk >= 'A' && k->vk <= 'Z') { *kind = K_CTRL; return (u32)(k->vk - 'A' + 1); }
        return 0;
    }
    if (k) {
        int lv;
        if (altgr) lv = 2 + (shift ^ (caps && (k->flags & KC_CAPS_AG)));
        else if (caps && (k->flags & KC_SGCAPS)) return k->caps[shift];
        else lv = shift ^ (caps && (k->flags & KC_CAPS));
        if ((k->dead >> lv) & 1) *kind = K_DEAD;
        return k->c[lv];
    }
    if (altgr) return 0;
    switch (usage) {
    case KEY_ENTER: case KEY_KP_ENTER: return '\n';
    case KEY_ESC: return 27;
    case KEY_BACKSPACE: return '\b';
    case KEY_TAB: return '\t';
    case KEY_KP_SLASH: return '/';
    case KEY_KP_STAR: return '*';
    case KEY_KP_MINUS: return '-';
    case KEY_KP_PLUS: return '+';
    }
    if (usage >= KEY_KP_1 && usage <= KEY_KP_DOT) {
        if (!(mods & KLM_NUM)) return 0;
        if (usage == KEY_KP_DOT) return l->kp_decimal;
        return usage == KEY_KP_0 ? '0' : (u32)('1' + usage - KEY_KP_1);
    }
    return 0;
}

int kl_translate(kl_state_t *s, u8 usage, u32 mods, u32 out[2]) {
    const kl_layout_t *l = s->layout;
    if (!l) return 0;
    int kind;
    u32 c = key_char(l, usage, mods, &kind);
    if (!c) return 0;                                /* no character: a pending dead key waits */
    if (kind == K_CTRL) { out[0] = c; return 1; }    /* shortcuts leave a dead key pending */
    if (s->dead) {
        u32 acc = s->dead;
        s->dead = 0;
        u32 r = kl_compose(l, acc, c);
        if (r) { out[0] = r; return 1; }
        out[0] = acc;                                /* no composition: the accent, then the key */
        out[1] = c;
        return 2;
    }
    if (kind == K_DEAD) { s->dead = c; return 0; }
    out[0] = c;
    return 1;
}
