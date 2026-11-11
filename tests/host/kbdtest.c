/* Host tests for the keyboard layouts (kernel/drivers/keymap.c and
 * keylayouts.c, compiled unmodified).
 *
 * The expected characters below were written from each Windows layout as
 * documented (key caps, Microsoft's layout pages), independently of the
 * tables, so they cross-check the transcription: per layout, keys at every
 * level (base, Shift, AltGr, Shift+AltGr), Caps Lock, dead keys, and the
 * shortcut letter (virtual key) of moved letters. Then structural checks on
 * every table and the engine's rules (dead key + space / uncomposable key /
 * another dead key / a key without a character, Ctrl+Alt as AltGr).
 *
 * usage: kbdtest [-v]
 */
#include <stdio.h>
#include "core/lib.h"
#include "core/types.h"
#include "core/unicode.h"
#include "drivers/input.h"
#include "drivers/keymap.h"

static int g_fail, g_checks, g_verbose;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

/* key positions, named after the US key caps */
enum {
    A = 0x04, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    D1 = 0x1E, D2, D3, D4, D5, D6, D7, D8, D9, D0,
    ENTER = 0x28, ESC, BKSP, TAB, SPACE, MINUS, EQUAL, LBRACK, RBRACK, BSLASH, HASH, SEMI, QUOTE, GRAVE,
    COMMA, DOT, SLASH,
    LEFT = 0x50, KPDOT = 0x63, ISO = 0x64, KPCOMMA = 0x85, RO = 0x87,
};

/* modifiers: shift, AltGr (Right Alt), Caps Lock, Ctrl, Ctrl+Alt */
#define SH  KLM_SHIFT
#define AG  (KLM_ALT | KLM_RALT)
#define CL  KLM_CAPS
#define CT  KLM_CTRL
#define CA  (KLM_CTRL | KLM_ALT)
#define NL  KLM_NUM

typedef struct { u8 usage; u32 mods; } press_t;
typedef struct { const char *layout; press_t keys[6]; const char *out; } seq_t;

static const kl_layout_t *layout(const char *id) {
    int i = kl_find(id);
    if (i < 0) { printf("FAIL: no layout %s\n", id); g_fail++; return NULL; }
    return &kl_layouts[i];
}

/* type a sequence on a fresh state, the result as UTF-8 */
static void type(const kl_layout_t *l, const press_t *keys, int n, char *out, int outsz) {
    kl_state_t st = { l, 0 };
    int o = 0;
    for (int i = 0; i < n && keys[i].usage; i++) {
        u32 c[2];
        int k = kl_translate(&st, keys[i].usage, keys[i].mods, c);
        for (int j = 0; j < k && o < outsz - 5; j++) o += utf8_put(c[j], out + o);
    }
    out[o] = 0;
}

static void check_seq(const seq_t *t, int line) {
    const kl_layout_t *l = layout(t->layout);
    if (!l) return;
    char got[64];
    type(l, t->keys, 6, got, sizeof got);
    CHECK(!strcmp(got, t->out), "line %d: %s: got \"%s\", expected \"%s\"", line, t->layout, got, t->out);
    if (g_verbose) printf("  %-6s %-12s ok\n", t->layout, t->out);
}

#define K1(lay, k, m, out) { lay, { { k, m } }, out }
#define K2(lay, k1, m1, k2, m2, out) { lay, { { k1, m1 }, { k2, m2 } }, out }
#define K3(lay, k1, m1, k2, m2, k3, m3, out) { lay, { { k1, m1 }, { k2, m2 }, { k3, m3 } }, out }

static const seq_t g_keys[] = {
    /* US */
    K1("us", A, 0, "a"), K1("us", A, SH, "A"), K1("us", A, CL, "A"), K1("us", A, CL | SH, "a"),
    K1("us", D2, SH, "@"), K1("us", D6, SH, "^"), K1("us", QUOTE, 0, "'"), K1("us", QUOTE, SH, "\""),
    K1("us", GRAVE, SH, "~"), K1("us", BSLASH, SH, "|"), K1("us", HASH, 0, "\\"), K1("us", ISO, 0, "\\"),
    K1("us", D1, CL, "1"), K1("us", Q, AG, ""), K1("us", Q, CA, ""), K1("us", C, CT, "\x03"),
    K1("us", SPACE, 0, " "), K1("us", SPACE, SH, " "), K1("us", ENTER, 0, "\n"), K1("us", BKSP, 0, "\b"),
    K1("us", 0x59, NL, "1"), K1("us", 0x59, 0, ""), K1("us", 0x62, NL, "0"), K1("us", KPDOT, NL, "."),
    K1("us", 0x54, 0, "/"), K1("us", 0x57, 0, "+"),

    /* US-International */
    K2("us-intl", QUOTE, 0, E, 0, "é"), K2("us-intl", QUOTE, 0, C, 0, "ç"), K2("us-intl", QUOTE, 0, C, SH, "Ç"),
    K2("us-intl", QUOTE, 0, SPACE, 0, "'"), K2("us-intl", QUOTE, 0, D, 0, "'d"), K2("us-intl", QUOTE, SH, U, 0, "ü"),
    K2("us-intl", QUOTE, SH, SPACE, 0, "\""), K2("us-intl", GRAVE, 0, A, 0, "à"), K2("us-intl", GRAVE, SH, N, 0, "ñ"),
    K2("us-intl", D6, SH, O, 0, "ô"), K2("us-intl", GRAVE, SH, N, SH, "Ñ"),
    K1("us-intl", Q, AG, "ä"), K1("us-intl", Q, AG | SH, "Ä"), K1("us-intl", Q, AG | CL, "Ä"),
    K1("us-intl", D5, AG, "€"), K1("us-intl", COMMA, AG, "ç"), K1("us-intl", D1, AG, "¡"),
    K1("us-intl", D1, AG | SH, "¹"), K1("us-intl", S, AG, "ß"), K1("us-intl", SEMI, AG, "¶"),
    K1("us-intl", N, AG, "ñ"), K1("us-intl", SLASH, AG, "¿"), K1("us-intl", Q, CA, "ä"),

    /* United Kingdom */
    K1("uk", D2, SH, "\""), K1("uk", D3, SH, "£"), K1("uk", QUOTE, SH, "@"), K1("uk", HASH, 0, "#"),
    K1("uk", HASH, SH, "~"), K1("uk", BSLASH, 0, "#"), K1("uk", GRAVE, 0, "`"), K1("uk", GRAVE, SH, "¬"),
    K1("uk", GRAVE, AG, "¦"), K1("uk", D4, AG, "€"), K1("uk", A, AG, "á"), K1("uk", E, AG | SH, "É"),
    K1("uk", E, AG | CL, "É"), K1("uk", ISO, 0, "\\"), K1("uk", ISO, SH, "|"), K1("uk", U, AG, "ú"),

    /* German */
    K1("de", Y, 0, "z"), K1("de", Z, 0, "y"), K1("de", SEMI, 0, "ö"), K1("de", QUOTE, 0, "ä"),
    K1("de", LBRACK, 0, "ü"), K1("de", LBRACK, SH, "Ü"), K1("de", QUOTE, CL, "Ä"), K1("de", QUOTE, CL | SH, "ä"),
    K1("de", RBRACK, 0, "+"), K1("de", RBRACK, SH, "*"), K1("de", RBRACK, AG, "~"),
    K1("de", MINUS, 0, "ß"), K1("de", MINUS, SH, "?"), K1("de", MINUS, AG, "\\"),
    K1("de", GRAVE, SH, "°"), K1("de", Q, AG, "@"), K1("de", E, AG, "€"), K1("de", M, AG, "µ"),
    K1("de", D7, AG, "{"), K1("de", D8, AG, "["), K1("de", D9, AG, "]"), K1("de", D0, AG, "}"),
    K1("de", D2, AG, "²"), K1("de", D3, AG, "³"), K1("de", D2, SH, "\""), K1("de", D3, SH, "§"),
    K1("de", D7, SH, "/"), K1("de", D0, SH, "="), K1("de", ISO, 0, "<"), K1("de", ISO, SH, ">"),
    K1("de", ISO, AG, "|"), K1("de", HASH, 0, "#"), K1("de", HASH, SH, "'"), K1("de", COMMA, SH, ";"),
    K1("de", DOT, SH, ":"), K1("de", SLASH, 0, "-"), K1("de", SLASH, SH, "_"),
    K1("de", Q, CA, "@"), K1("de", E, CA, "€"), K1("de", Q, KLM_ALT, ""), K1("de", A, AG, ""),
    K1("de", D1, CL, "!"),                        /* kbdgr: Caps Lock also shifts the digit row */
    K1("de", KPDOT, NL, ","),
    K2("de", EQUAL, 0, E, 0, "é"), K2("de", EQUAL, 0, E, SH, "É"), K2("de", EQUAL, 0, SPACE, 0, "´"),
    K2("de", EQUAL, 0, X, 0, "´x"), K2("de", EQUAL, 0, Z, 0, "ý"), K2("de", EQUAL, SH, A, 0, "à"),
    K2("de", GRAVE, 0, A, 0, "â"), K2("de", GRAVE, 0, SPACE, 0, "^"), K2("de", GRAVE, 0, GRAVE, 0, "^^"),
    K2("de", GRAVE, 0, ENTER, 0, "^\n"), K2("de", GRAVE, 0, BKSP, 0, "^\b"), K2("de", GRAVE, 0, U, CL, "Û"),
    K2("de", GRAVE, 0, EQUAL, 0, "^´"), K3("de", GRAVE, 0, LEFT, 0, O, 0, "ô"),
    K3("de", EQUAL, 0, 0xE1, SH, E, SH, "É"),                          /* Shift pressed after the dead key */
    K3("de", GRAVE, 0, C, CT, I, 0, "\x03î"),                          /* a shortcut leaves it pending */

    /* Swiss German */
    K1("de-ch", Y, 0, "z"), K1("de-ch", LBRACK, 0, "ü"), K1("de-ch", LBRACK, SH, "è"), K1("de-ch", LBRACK, CL, "Ü"),
    K1("de-ch", LBRACK, CL | SH, "È"), K1("de-ch", SEMI, 0, "ö"), K1("de-ch", SEMI, SH, "é"),
    K1("de-ch", QUOTE, 0, "ä"), K1("de-ch", QUOTE, SH, "à"), K1("de-ch", QUOTE, CL, "Ä"),
    K1("de-ch", D2, AG, "@"), K1("de-ch", D3, AG, "#"), K1("de-ch", D7, AG, "|"), K1("de-ch", D4, SH, "ç"),
    K1("de-ch", D1, SH, "+"), K1("de-ch", D3, SH, "*"), K1("de-ch", MINUS, 0, "'"), K1("de-ch", RBRACK, SH, "!"),
    K1("de-ch", LBRACK, AG, "["), K1("de-ch", RBRACK, AG, "]"), K1("de-ch", QUOTE, AG, "{"),
    K1("de-ch", BSLASH, AG, "}"), K1("de-ch", BSLASH, 0, "$"), K1("de-ch", BSLASH, SH, "£"),
    K1("de-ch", GRAVE, 0, "§"), K1("de-ch", ISO, AG, "\\"), K1("de-ch", E, AG, "€"),
    K2("de-ch", EQUAL, 0, E, 0, "ê"), K2("de-ch", EQUAL, SH, A, 0, "à"), K2("de-ch", EQUAL, AG, N, 0, "ñ"),
    K2("de-ch", RBRACK, 0, U, 0, "ü"), K2("de-ch", RBRACK, 0, Z, 0, "ÿ"), K2("de-ch", MINUS, AG, E, 0, "é"),

    /* French (AZERTY) */
    K1("fr", A, 0, "q"), K1("fr", Q, 0, "a"), K1("fr", Z, 0, "w"), K1("fr", W, 0, "z"), K1("fr", SEMI, 0, "m"),
    K1("fr", M, 0, ","), K1("fr", M, SH, "?"), K1("fr", D1, 0, "&"), K1("fr", D1, SH, "1"), K1("fr", D2, 0, "é"),
    K1("fr", D3, 0, "\""), K1("fr", D4, 0, "'"), K1("fr", D5, 0, "("), K1("fr", D6, 0, "-"), K1("fr", D7, 0, "è"),
    K1("fr", D8, 0, "_"), K1("fr", D9, 0, "ç"), K1("fr", D0, 0, "à"), K1("fr", MINUS, 0, ")"),
    K1("fr", MINUS, SH, "°"), K1("fr", EQUAL, 0, "="), K1("fr", EQUAL, SH, "+"), K1("fr", RBRACK, 0, "$"),
    K1("fr", RBRACK, SH, "£"), K1("fr", QUOTE, 0, "ù"), K1("fr", QUOTE, SH, "%"), K1("fr", BSLASH, 0, "*"),
    K1("fr", BSLASH, SH, "µ"), K1("fr", GRAVE, 0, "²"), K1("fr", COMMA, 0, ";"), K1("fr", DOT, 0, ":"),
    K1("fr", DOT, SH, "/"), K1("fr", SLASH, 0, "!"), K1("fr", SLASH, SH, "§"), K1("fr", D0, AG, "@"),
    K1("fr", D3, AG, "#"), K1("fr", D4, AG, "{"), K1("fr", D5, AG, "["), K1("fr", D6, AG, "|"),
    K1("fr", D8, AG, "\\"), K1("fr", D9, AG, "^"), K1("fr", MINUS, AG, "]"), K1("fr", EQUAL, AG, "}"),
    K1("fr", E, AG, "€"), K1("fr", RBRACK, AG, "¤"), K1("fr", ISO, 0, "<"), K1("fr", ISO, SH, ">"),
    K1("fr", D2, CL, "2"), K1("fr", Q, CL, "A"),   /* kbdfr: Caps Lock gives the digits, as Shift */
    K1("fr", Q, CT, "\x01"), K1("fr", W, CT, "\x1A"), K1("fr", SEMI, CT, "\x0D"),
    K2("fr", LBRACK, 0, E, 0, "ê"), K2("fr", LBRACK, SH, I, 0, "ï"), K2("fr", LBRACK, 0, SPACE, 0, "^"),
    K2("fr", D2, AG, N, 0, "ñ"), K2("fr", D7, AG, E, 0, "è"), K2("fr", LBRACK, 0, W, 0, "^z"),
    K2("fr", LBRACK, SH, Y, 0, "ÿ"), K2("fr", LBRACK, 0, Q, SH, "Â"),

    /* Belgian (AZERTY) */
    K1("be", Q, 0, "a"), K1("be", D1, 0, "&"), K1("be", D2, 0, "é"), K1("be", D6, 0, "§"), K1("be", D8, 0, "!"),
    K1("be", D0, 0, "à"), K1("be", MINUS, 0, ")"), K1("be", MINUS, SH, "°"), K1("be", EQUAL, 0, "-"),
    K1("be", EQUAL, SH, "_"), K1("be", RBRACK, 0, "$"), K1("be", RBRACK, SH, "*"), K1("be", QUOTE, 0, "ù"),
    K1("be", BSLASH, 0, "µ"), K1("be", BSLASH, SH, "£"), K1("be", SLASH, 0, "="), K1("be", SLASH, SH, "+"),
    K1("be", GRAVE, SH, "³"), K1("be", D1, AG, "|"), K1("be", D2, AG, "@"), K1("be", D3, AG, "#"),
    K1("be", D9, AG, "{"), K1("be", D0, AG, "}"), K1("be", LBRACK, AG, "["), K1("be", RBRACK, AG, "]"),
    K1("be", ISO, AG, "\\"), K1("be", E, AG, "€"), K1("be", D6, AG, "^"),
    K2("be", QUOTE, AG, E, 0, "é"), K2("be", SLASH, AG, N, 0, "ñ"), K2("be", BSLASH, AG, Q, 0, "à"),
    K2("be", LBRACK, 0, O, 0, "ô"), K2("be", LBRACK, SH, E, 0, "ë"),

    /* Spanish */
    K1("es", SEMI, 0, "ñ"), K1("es", SEMI, SH, "Ñ"), K1("es", BSLASH, 0, "ç"), K1("es", GRAVE, 0, "º"),
    K1("es", GRAVE, SH, "ª"), K1("es", GRAVE, AG, "\\"), K1("es", MINUS, 0, "'"), K1("es", MINUS, SH, "?"),
    K1("es", EQUAL, 0, "¡"), K1("es", EQUAL, SH, "¿"), K1("es", D3, SH, "·"), K1("es", D1, AG, "|"),
    K1("es", D2, AG, "@"), K1("es", D3, AG, "#"), K1("es", D6, AG, "¬"), K1("es", LBRACK, AG, "["),
    K1("es", RBRACK, AG, "]"), K1("es", QUOTE, AG, "{"), K1("es", BSLASH, AG, "}"), K1("es", E, AG, "€"),
    K1("es", D5, AG, "€"), K1("es", RBRACK, 0, "+"), K1("es", SLASH, 0, "-"), K1("es", SEMI, CL, "Ñ"),
    K2("es", QUOTE, 0, A, 0, "á"), K2("es", QUOTE, SH, U, 0, "ü"), K2("es", LBRACK, 0, E, 0, "è"),
    K2("es", LBRACK, SH, O, 0, "ô"), K2("es", QUOTE, 0, SPACE, 0, "´"), K2("es", D4, AG, N, 0, "ñ"),
    K2("es", QUOTE, 0, I, SH, "Í"),

    /* Italian (no dead keys) */
    K1("it", LBRACK, 0, "è"), K1("it", LBRACK, SH, "é"), K1("it", LBRACK, AG, "["), K1("it", LBRACK, AG | SH, "{"),
    K1("it", RBRACK, 0, "+"), K1("it", RBRACK, SH, "*"), K1("it", RBRACK, AG, "]"), K1("it", RBRACK, AG | SH, "}"),
    K1("it", SEMI, 0, "ò"), K1("it", SEMI, SH, "ç"), K1("it", SEMI, AG, "@"), K1("it", QUOTE, 0, "à"),
    K1("it", QUOTE, SH, "°"), K1("it", QUOTE, AG, "#"), K1("it", BSLASH, 0, "ù"), K1("it", BSLASH, SH, "§"),
    K1("it", GRAVE, 0, "\\"), K1("it", GRAVE, SH, "|"), K1("it", MINUS, 0, "'"), K1("it", EQUAL, 0, "ì"),
    K1("it", EQUAL, SH, "^"), K1("it", D3, SH, "£"), K1("it", E, AG, "€"), K1("it", SEMI, CL, "ò"),
    K2("it", EQUAL, SH, E, 0, "^e"),

    /* Portuguese */
    K1("pt", LBRACK, 0, "+"), K1("pt", LBRACK, SH, "*"), K1("pt", SEMI, 0, "ç"), K1("pt", SEMI, CL, "Ç"),
    K1("pt", QUOTE, 0, "º"), K1("pt", QUOTE, SH, "ª"), K1("pt", GRAVE, 0, "\\"), K1("pt", GRAVE, SH, "|"),
    K1("pt", MINUS, 0, "'"), K1("pt", EQUAL, 0, "«"), K1("pt", EQUAL, SH, "»"), K1("pt", D2, AG, "@"),
    K1("pt", D3, AG, "£"), K1("pt", D4, AG, "§"), K1("pt", D7, AG, "{"), K1("pt", D0, AG, "}"),
    K1("pt", RBRACK, AG, "]"), K1("pt", E, AG, "€"),
    K2("pt", BSLASH, 0, A, 0, "ã"), K2("pt", BSLASH, 0, O, 0, "õ"), K2("pt", RBRACK, 0, E, 0, "é"),
    K2("pt", BSLASH, SH, E, 0, "ê"), K2("pt", RBRACK, SH, A, 0, "à"), K2("pt", LBRACK, AG, U, 0, "ü"),
    K2("pt", BSLASH, 0, N, 0, "ñ"),

    /* Brazilian ABNT2 */
    K1("pt-br", RBRACK, 0, "["), K1("pt-br", RBRACK, SH, "{"), K1("pt-br", BSLASH, 0, "]"),
    K1("pt-br", BSLASH, SH, "}"), K1("pt-br", HASH, 0, "]"), K1("pt-br", SEMI, 0, "ç"), K1("pt-br", GRAVE, 0, "'"),
    K1("pt-br", GRAVE, SH, "\""), K1("pt-br", SLASH, 0, ";"), K1("pt-br", SLASH, SH, ":"), K1("pt-br", RO, 0, "/"),
    K1("pt-br", RO, SH, "?"), K1("pt-br", RO, AG, "°"), K1("pt-br", KPCOMMA, 0, "."), K1("pt-br", KPDOT, NL, ","),
    K1("pt-br", Q, AG, "/"), K1("pt-br", W, AG, "?"), K1("pt-br", E, AG, "°"), K1("pt-br", EQUAL, AG, "§"),
    K1("pt-br", D1, AG, "¹"), K1("pt-br", D2, SH, "@"), K1("pt-br", RBRACK, AG, "ª"), K1("pt-br", BSLASH, AG, "º"),
    K1("pt-br", ISO, 0, "\\"), K1("pt-br", ISO, SH, "|"),
    K2("pt-br", QUOTE, 0, A, 0, "ã"), K2("pt-br", QUOTE, SH, E, 0, "ê"), K2("pt-br", LBRACK, 0, E, 0, "é"),
    K2("pt-br", LBRACK, SH, A, 0, "à"), K2("pt-br", D6, SH, U, 0, "ü"), K2("pt-br", QUOTE, 0, O, SH, "Õ"),

    /* Swedish/Finnish */
    K1("sv", LBRACK, 0, "å"), K1("sv", LBRACK, CL, "Å"), K1("sv", SEMI, 0, "ö"), K1("sv", QUOTE, 0, "ä"),
    K1("sv", MINUS, 0, "+"), K1("sv", MINUS, SH, "?"), K1("sv", MINUS, AG, "\\"), K1("sv", BSLASH, 0, "'"),
    K1("sv", BSLASH, SH, "*"), K1("sv", GRAVE, 0, "§"), K1("sv", GRAVE, SH, "½"), K1("sv", D2, SH, "\""),
    K1("sv", D4, SH, "¤"), K1("sv", D2, AG, "@"), K1("sv", D3, AG, "£"), K1("sv", D4, AG, "$"),
    K1("sv", D5, AG, "€"), K1("sv", D7, AG, "{"), K1("sv", D0, AG, "}"), K1("sv", ISO, AG, "|"),
    K1("sv", SLASH, 0, "-"), K1("sv", KPDOT, NL, ","),
    K2("sv", RBRACK, 0, O, 0, "ö"), K2("sv", EQUAL, 0, E, 0, "é"), K2("sv", RBRACK, SH, A, 0, "â"),
    K2("sv", RBRACK, AG, N, 0, "ñ"), K2("sv", EQUAL, SH, E, 0, "è"),

    /* Norwegian */
    K1("no", SEMI, 0, "ø"), K1("no", QUOTE, 0, "æ"), K1("no", LBRACK, 0, "å"), K1("no", SEMI, CL, "Ø"),
    K1("no", EQUAL, 0, "\\"), K1("no", GRAVE, 0, "|"), K1("no", GRAVE, SH, "§"), K1("no", MINUS, 0, "+"),
    K1("no", D2, AG, "@"), K1("no", D4, AG, "$"),
    K2("no", EQUAL, AG, E, 0, "é"), K2("no", EQUAL, SH, O, 0, "ò"), K2("no", RBRACK, 0, U, 0, "ü"),

    /* Danish */
    K1("da", SEMI, 0, "æ"), K1("da", QUOTE, 0, "ø"), K1("da", LBRACK, 0, "å"), K1("da", QUOTE, CL, "Ø"),
    K1("da", EQUAL, AG, "|"), K1("da", GRAVE, 0, "½"), K1("da", GRAVE, SH, "§"), K1("da", ISO, AG, "\\"),
    K1("da", D2, AG, "@"),
    K2("da", EQUAL, 0, E, 0, "é"), K2("da", EQUAL, SH, A, 0, "à"), K2("da", RBRACK, SH, O, 0, "ô"),

    /* Dutch */
    K1("nl", MINUS, 0, "/"), K1("nl", MINUS, SH, "?"), K1("nl", MINUS, AG, "\\"), K1("nl", EQUAL, 0, "°"),
    K1("nl", RBRACK, 0, "*"), K1("nl", RBRACK, SH, "|"), K1("nl", BSLASH, 0, "<"), K1("nl", BSLASH, SH, ">"),
    K1("nl", SEMI, 0, "+"), K1("nl", SEMI, SH, "±"), K1("nl", GRAVE, 0, "@"), K1("nl", GRAVE, SH, "§"),
    K1("nl", GRAVE, AG, "¬"), K1("nl", ISO, 0, "]"), K1("nl", ISO, SH, "["), K1("nl", ISO, AG, "¦"),
    K1("nl", D7, SH, "_"), K1("nl", D0, SH, "'"), K1("nl", SLASH, 0, "-"), K1("nl", SLASH, SH, "="),
    K1("nl", D1, AG, "¹"), K1("nl", S, AG, "ß"), K1("nl", E, AG, "€"),
    K2("nl", QUOTE, 0, E, 0, "é"), K2("nl", LBRACK, 0, E, 0, "ë"), K2("nl", EQUAL, AG, C, 0, "ç"),
    K2("nl", LBRACK, SH, E, 0, "ê"), K2("nl", EQUAL, SH, N, 0, "ñ"), K2("nl", QUOTE, SH, A, 0, "à"),

    /* Polish (Programmers) */
    K1("pl", A, AG, "ą"), K1("pl", A, AG | SH, "Ą"), K1("pl", A, AG | CL, "Ą"), K1("pl", A, AG | CL | SH, "ą"),
    K1("pl", C, AG, "ć"), K1("pl", E, AG, "ę"), K1("pl", L, AG, "ł"), K1("pl", L, AG | SH, "Ł"),
    K1("pl", N, AG, "ń"), K1("pl", O, AG, "ó"), K1("pl", S, AG, "ś"), K1("pl", X, AG, "ź"), K1("pl", Z, AG, "ż"),
    K1("pl", Z, AG | SH, "Ż"), K1("pl", U, AG, "€"), K1("pl", Z, CA, "ż"), K1("pl", GRAVE, 0, "`"),
    K1("pl", D2, SH, "@"),
    K2("pl", GRAVE, SH, A, 0, "ą"), K2("pl", GRAVE, SH, Z, 0, "ż"), K2("pl", GRAVE, SH, X, 0, "ź"),
    K2("pl", GRAVE, SH, SPACE, 0, "~"), K2("pl", GRAVE, SH, L, SH, "Ł"),

    /* Czech (QWERTZ) */
    K1("cs", Y, 0, "z"), K1("cs", Z, 0, "y"), K1("cs", D2, 0, "ě"), K1("cs", D3, 0, "š"), K1("cs", D4, 0, "č"),
    K1("cs", D5, 0, "ř"), K1("cs", D6, 0, "ž"), K1("cs", D7, 0, "ý"), K1("cs", D8, 0, "á"), K1("cs", D9, 0, "í"),
    K1("cs", D0, 0, "é"), K1("cs", D2, SH, "2"), K1("cs", D2, CL, "Ě"), K1("cs", D2, CL | SH, "2"),
    K1("cs", D1, 0, "+"), K1("cs", D1, SH, "1"), K1("cs", MINUS, 0, "="), K1("cs", MINUS, SH, "%"),
    K1("cs", LBRACK, 0, "ú"), K1("cs", LBRACK, SH, "/"), K1("cs", LBRACK, CL, "Ú"), K1("cs", RBRACK, 0, ")"),
    K1("cs", RBRACK, SH, "("), K1("cs", SEMI, 0, "ů"), K1("cs", SEMI, SH, "\""), K1("cs", QUOTE, 0, "§"),
    K1("cs", QUOTE, SH, "!"), K1("cs", BSLASH, SH, "'"), K1("cs", GRAVE, 0, ";"), K1("cs", COMMA, SH, "?"),
    K1("cs", DOT, SH, ":"), K1("cs", SLASH, 0, "-"), K1("cs", Q, AG, "\\"), K1("cs", W, AG, "|"),
    K1("cs", V, AG, "@"), K1("cs", B, AG, "{"), K1("cs", N, AG, "}"), K1("cs", F, AG, "["), K1("cs", G, AG, "]"),
    K1("cs", X, AG, "#"), K1("cs", C, AG, "&"), K1("cs", E, AG, "€"),
    K2("cs", EQUAL, 0, A, 0, "á"), K2("cs", EQUAL, SH, C, 0, "č"), K2("cs", EQUAL, SH, R, 0, "ř"),
    K2("cs", EQUAL, SH, D, 0, "ď"), K2("cs", EQUAL, SH, T, SH, "Ť"), K2("cs", GRAVE, SH, U, 0, "ů"),
    K2("cs", BSLASH, 0, U, 0, "ü"), K2("cs", D0, AG, O, 0, "ő"), K2("cs", D2, AG, S, 0, "š"),
    K2("cs", D3, AG, O, 0, "ô"), K2("cs", EQUAL, 0, SPACE, 0, "´"),

    /* Turkish Q */
    K1("tr", I, 0, "ı"), K1("tr", I, SH, "I"), K1("tr", I, CL, "I"), K1("tr", QUOTE, 0, "i"), K1("tr", QUOTE, SH, "İ"),
    K1("tr", QUOTE, CL, "İ"), K1("tr", LBRACK, 0, "ğ"), K1("tr", RBRACK, 0, "ü"), K1("tr", SEMI, 0, "ş"),
    K1("tr", COMMA, 0, "ö"), K1("tr", DOT, 0, "ç"), K1("tr", SLASH, 0, "."), K1("tr", SLASH, SH, ":"),
    K1("tr", MINUS, 0, "*"), K1("tr", MINUS, SH, "?"), K1("tr", EQUAL, 0, "-"), K1("tr", EQUAL, SH, "_"),
    K1("tr", BSLASH, 0, ","), K1("tr", BSLASH, SH, ";"), K1("tr", GRAVE, 0, "\""), K1("tr", GRAVE, SH, "é"),
    K1("tr", D2, SH, "'"), K1("tr", D4, SH, "+"), K1("tr", Q, AG, "@"), K1("tr", E, AG, "€"),
    K1("tr", I, AG, "i"), K1("tr", I, AG | SH, "İ"), K1("tr", D2, AG, "£"), K1("tr", D3, AG, "#"),
    K1("tr", D4, AG, "$"), K1("tr", D5, AG, "½"), K1("tr", D7, AG, "{"), K1("tr", D0, AG, "}"),
    K1("tr", MINUS, AG, "\\"), K1("tr", EQUAL, AG, "|"), K1("tr", ISO, 0, "<"), K1("tr", ISO, AG, "|"),
    K2("tr", D3, SH, A, 0, "â"), K2("tr", D3, SH, I, 0, "î"), K2("tr", D3, SH, QUOTE, 0, "î"),
    K2("tr", SEMI, AG, E, 0, "é"), K2("tr", LBRACK, AG, O, 0, "ö"), K2("tr", RBRACK, AG, N, 0, "ñ"),
    K2("tr", BSLASH, AG, A, 0, "à"),

    /* Russian */
    K1("ru", Q, 0, "й"), K1("ru", W, 0, "ц"), K1("ru", E, 0, "у"), K1("ru", R, 0, "к"), K1("ru", T, 0, "е"),
    K1("ru", Y, 0, "н"), K1("ru", U, 0, "г"), K1("ru", I, 0, "ш"), K1("ru", O, 0, "щ"), K1("ru", P, 0, "з"),
    K1("ru", LBRACK, 0, "х"), K1("ru", RBRACK, 0, "ъ"), K1("ru", A, 0, "ф"), K1("ru", S, 0, "ы"),
    K1("ru", D, 0, "в"), K1("ru", F, 0, "а"), K1("ru", G, 0, "п"), K1("ru", H, 0, "р"), K1("ru", J, 0, "о"),
    K1("ru", K, 0, "л"), K1("ru", L, 0, "д"), K1("ru", SEMI, 0, "ж"), K1("ru", QUOTE, 0, "э"),
    K1("ru", Z, 0, "я"), K1("ru", X, 0, "ч"), K1("ru", C, 0, "с"), K1("ru", V, 0, "м"), K1("ru", B, 0, "и"),
    K1("ru", N, 0, "т"), K1("ru", M, 0, "ь"), K1("ru", COMMA, 0, "б"), K1("ru", DOT, 0, "ю"),
    K1("ru", SLASH, 0, "."), K1("ru", SLASH, SH, ","), K1("ru", GRAVE, 0, "ё"), K1("ru", GRAVE, SH, "Ё"),
    K1("ru", D2, SH, "\""), K1("ru", D3, SH, "№"), K1("ru", D4, SH, ";"), K1("ru", D6, SH, ":"),
    K1("ru", D7, SH, "?"), K1("ru", BSLASH, SH, "/"), K1("ru", F, SH, "А"), K1("ru", F, CL, "А"),
    K1("ru", SEMI, CL, "Ж"), K1("ru", D8, AG, "₽"), K1("ru", C, CT, "\x03"),

    /* Greek */
    K1("el", A, 0, "α"), K1("el", B, 0, "β"), K1("el", C, 0, "ψ"), K1("el", D, 0, "δ"), K1("el", E, 0, "ε"),
    K1("el", F, 0, "φ"), K1("el", G, 0, "γ"), K1("el", H, 0, "η"), K1("el", I, 0, "ι"), K1("el", J, 0, "ξ"),
    K1("el", K, 0, "κ"), K1("el", L, 0, "λ"), K1("el", M, 0, "μ"), K1("el", N, 0, "ν"), K1("el", O, 0, "ο"),
    K1("el", P, 0, "π"), K1("el", Q, 0, ";"), K1("el", R, 0, "ρ"), K1("el", S, 0, "σ"), K1("el", T, 0, "τ"),
    K1("el", U, 0, "θ"), K1("el", V, 0, "ω"), K1("el", W, 0, "ς"), K1("el", X, 0, "χ"), K1("el", Y, 0, "υ"),
    K1("el", Z, 0, "ζ"), K1("el", A, SH, "Α"), K1("el", S, CL, "Σ"), K1("el", E, AG, "€"), K1("el", D2, AG, "²"),
    K1("el", C, CT, "\x03"),
    K2("el", SEMI, 0, A, 0, "ά"), K2("el", SEMI, 0, V, 0, "ώ"), K2("el", SEMI, 0, A, SH, "Ά"),
    K2("el", SEMI, SH, I, 0, "ϊ"), K2("el", SEMI, AG, I, 0, "ΐ"), K2("el", W, SH, Y, 0, "ΰ"),
    K2("el", SEMI, 0, SPACE, 0, "΄"), K2("el", SEMI, 0, T, 0, "΄τ"),
};

/* the shortcut letter of moved keys: Ctrl+A is the key labelled A */
static const struct { const char *layout; u8 usage; u8 vk; } g_vk[] = {
    { "us", A, 'A' }, { "us", D1, '1' }, { "us", SEMI, 0 },
    { "de", Y, 'Z' }, { "de", Z, 'Y' }, { "de-ch", Y, 'Z' }, { "cs", Y, 'Z' },
    { "fr", Q, 'A' }, { "fr", A, 'Q' }, { "fr", W, 'Z' }, { "fr", Z, 'W' }, { "fr", SEMI, 'M' }, { "fr", M, 0 },
    { "fr", D2, '2' }, { "be", Q, 'A' }, { "be", SEMI, 'M' },
    { "ru", F, 'F' }, { "ru", C, 'C' }, { "el", V, 'V' }, { "tr", I, 'I' }, { "tr", QUOTE, 0 },
};

static int utf8_len(const char *s) { int n = 0; while (utf8_next(&s)) n++; return n; }

/* every table: each letter and digit once, dead tables complete */
static void check_structure(void) {
    for (int li = 0; li < kl_layout_count; li++) {
        const kl_layout_t *l = &kl_layouts[li];
        int seen[128] = { 0 };
        for (int i = 0; i < l->nkeys; i++) {
            const kl_key_t *k = &l->keys[i];
            if (k->vk) seen[k->vk]++;
            CHECK(i == 0 || k->usage > l->keys[i - 1].usage, "%s: keys not sorted at 0x%02X", l->id, k->usage);
            CHECK(!(k->flags & KC_SGCAPS) || (k->caps[0] && k->caps[1]), "%s: 0x%02X SGCAPS without caps", l->id, k->usage);
            CHECK(l->altgr || (!k->c[2] && !k->c[3]), "%s: AltGr levels without AltGr", l->id);
            for (int lv = 0; lv < 4; lv++) {
                if (!((k->dead >> lv) & 1)) continue;
                CHECK(k->c[lv] != 0, "%s: 0x%02X dead level %d empty", l->id, k->usage, lv);
                int found = 0;
                for (int d = 0; d < l->ndead; d++) if (l->dead[d].accent == k->c[lv]) found = 1;
                CHECK(found, "%s: dead key U+%04X has no table", l->id, k->c[lv]);
            }
        }
        for (int c = 'A'; c <= 'Z'; c++) CHECK(seen[c] == 1, "%s: letter %c on %d keys", l->id, c, seen[c]);
        for (int c = '0'; c <= '9'; c++) CHECK(seen[c] == 1, "%s: digit %c on %d keys", l->id, c, seen[c]);
        for (int d = 0; d < l->ndead; d++) {
            const kl_dead_t *t = &l->dead[d];
            CHECK(utf8_len(t->from) == utf8_len(t->to), "%s: dead U+%04X table lengths differ", l->id, t->accent);
            CHECK(kl_compose(l, t->accent, ' ') != 0, "%s: dead U+%04X has no space entry", l->id, t->accent);
        }
        CHECK(kl_find(l->id) == li, "%s: kl_find", l->id);
        CHECK(l->abbr && strlen(l->abbr) >= 2 && strlen(l->abbr) <= 3, "%s: indicator", l->id);
        if (g_verbose) printf("  %-6s %-30s %2d keys, %d dead tables\n", l->id, l->name, l->nkeys, l->ndead);
    }
    CHECK(kl_find("DE") == kl_find("de") && kl_find("xx") < 0, "kl_find case / unknown");
    CHECK(kl_layout_count >= 20, "only %d layouts", kl_layout_count);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    printf("== keyboard layouts\n");
    check_structure();
    for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) check_seq(&g_keys[i], (int)i);
    for (size_t i = 0; i < sizeof g_vk / sizeof g_vk[0]; i++) {
        const kl_layout_t *l = layout(g_vk[i].layout);
        if (l) CHECK(kl_vk(l, g_vk[i].usage) == g_vk[i].vk, "%s 0x%02X: vk %d, expected %d", g_vk[i].layout,
                     g_vk[i].usage, kl_vk(l, g_vk[i].usage), g_vk[i].vk);
    }
    /* the pending dead key survives modifiers and keys without characters */
    {
        kl_state_t st = { layout("de"), 0 };
        u32 c[2];
        CHECK(kl_translate(&st, EQUAL, 0, c) == 0 && st.dead == 0xB4, "dead key pending");
        CHECK(kl_translate(&st, 0xE1, SH, c) == 0 && st.dead == 0xB4, "Shift keeps it");
        CHECK(kl_translate(&st, 0x3A, SH, c) == 0 && st.dead == 0xB4, "F1 keeps it");
        CHECK(kl_translate(&st, E, SH, c) == 1 && c[0] == 0xC9 && st.dead == 0, "then É");
    }
    printf("   keyboard layouts: %d checks, %d failed\n", g_checks, g_fail);
    printf("TOTAL: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
