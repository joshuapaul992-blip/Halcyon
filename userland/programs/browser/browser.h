/* BROWSER.EXE ("Web Browser") internals: the document model, layout and
 * images. html.c, layout.c and image.c are also compiled for Linux
 * (-DWEB_HOST) by tests/web for tests with ASan. See docs/DESIGN-M6-WEB.md. */
#ifndef BROWSER_H
#define BROWSER_H
#include <stddef.h>
#include <stdint.h>

#ifdef WEB_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#else
#include "hal.h"
static inline void *b_memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
#define memchr b_memchr
#endif

/* ---- limits (a page is untrusted input) ---- */
#define DOC_MAX_NODES   200000
#define DOC_MAX_DEPTH   200          /* deeper elements are flattened into the deepest one */
#define DOC_MAX_ATTRS   32           /* per element */
#define LAYOUT_MAX_DEPTH 60          /* boxes nested deeper are laid out as inline content */
#define DOC_MAX_ITEMS   400000       /* display list entries */
#define DOC_MAX_HEIGHT  4000000      /* pixels */
#define IMG_MAX_DIM     8192
#define IMG_MAX_PIXELS  (16u << 20)
#define DOC_MAX_IMAGES  200

/* ---- an arena: everything of one document, freed at once ---- */
typedef struct arena_blk { struct arena_blk *next; size_t used, size; } arena_blk_t;
typedef struct { arena_blk_t *head; size_t total, limit; int failed; } arena_t;
void *arena_alloc(arena_t *a, size_t n);           /* zeroed; NULL past the limit */
char *arena_strndup(arena_t *a, const char *s, size_t n);
void  arena_free(arena_t *a);

/* ---- elements ---- */
enum {
    T_UNKNOWN = 0, T_HTML, T_HEAD, T_BODY, T_TITLE, T_META, T_LINK, T_BASE, T_STYLE, T_SCRIPT, T_NOSCRIPT,
    T_TEMPLATE, T_P, T_DIV, T_SPAN, T_A, T_B, T_STRONG, T_I, T_EM, T_U, T_S, T_STRIKE, T_DEL, T_INS,
    T_SMALL, T_BIG, T_SUB, T_SUP, T_CODE, T_TT, T_KBD, T_SAMP, T_VAR, T_PRE, T_LISTING, T_XMP, T_PLAINTEXT,
    T_BR, T_WBR, T_HR, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_MENU, T_DIR,
    T_BLOCKQUOTE, T_CENTER, T_FONT, T_IMG, T_TABLE, T_CAPTION, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH,
    T_COL, T_COLGROUP, T_FORM, T_INPUT, T_BUTTON, T_SELECT, T_OPTION, T_OPTGROUP, T_TEXTAREA, T_LABEL,
    T_FIELDSET, T_LEGEND, T_ADDRESS, T_ARTICLE, T_ASIDE, T_FOOTER, T_HEADER, T_MAIN, T_NAV, T_SECTION,
    T_FIGURE, T_FIGCAPTION, T_IFRAME, T_OBJECT, T_EMBED, T_SVG, T_MATH, T_CANVAS, T_VIDEO, T_AUDIO, T_MAP,
    T_AREA, T_ABBR, T_CITE, T_Q, T_MARK, T_NOBR, T_DETAILS, T_SUMMARY, T_PICTURE, T_SOURCE, T_NOFRAMES,
    T_FRAMESET, T_FRAME, T_PARAM, T_TRACK, T_DIALOG, T_HGROUP, T_DFN, T_TIME, T_DATA, T_OUTPUT, T_PROGRESS,
    T_METER, T_SELECTEDCONTENT, T_SEARCH, T_COUNT
};

typedef struct { char *name, *value; } attr_t;      /* names lower case; values decoded UTF-8 */

/* display types */
enum { D_INLINE = 0, D_BLOCK, D_NONE, D_LIST_ITEM, D_TABLE, D_ROW, D_CELL, D_ROWGROUP, D_CAPTION, D_INLINE_BLOCK };

typedef struct node {
    struct node *parent, *first, *last, *next;
    uint16_t tag;
    uint8_t is_text, display;
    char *text;                  /* text nodes: UTF-8 */
    int textlen;
    attr_t *attrs;
    int nattrs;
    /* table measurement cache (layout.c): -1 = not measured */
    int min_w, max_w;
    /* forms: the control's state (struct fctl in layout.c / browser.c) */
    struct fctl *ctl;
    int img;                     /* index into doc->images + 1, 0 = none */
} node_t;

/* ---- images ---- */
typedef struct {
    char *url;                   /* absolute */
    int state;                   /* 0 waiting, 1 loading, 2 ready, 3 failed */
    int w, h;                    /* decoded size */
    uint32_t *px;                /* 0xAARRGGBB, straight alpha */
} image_t;

/* ---- form controls (drawn by the browser) ---- */
enum { FC_TEXT = 1, FC_PASSWORD, FC_SUBMIT, FC_RESET, FC_BUTTON, FC_CHECKBOX, FC_RADIO, FC_SELECT, FC_TEXTAREA,
       FC_HIDDEN, FC_IMAGE };
typedef struct fctl {
    int kind;
    node_t *node, *form;
    char *name;
    char *value;                 /* malloc()ed (text fields grow) */
    int vlen, vcap, caret, scroll;
    int checked;
    int selected;                /* select: index of the chosen option */
    int noptions;
    char **opt_label, **opt_value;
    int size;                    /* text field width in characters */
    int rows, cols;              /* textarea */
} fctl_t;

/* ---- the display list (layout output) ---- */
enum { IT_TEXT = 1, IT_RECT, IT_FRAME, IT_IMAGE, IT_ALT, IT_HR, IT_BULLET, IT_CONTROL, IT_LINE };
typedef struct {
    uint8_t kind, font, flags, pad;
    int x, y, w, h;              /* page coordinates */
    uint32_t color;
    const char *text;            /* IT_TEXT / IT_ALT (arena or node text) */
    int len;
    node_t *link;                /* the <a href> it belongs to */
    node_t *node;                /* IT_IMAGE / IT_CONTROL: the element */
} item_t;
#define IF_UNDERLINE 0x01
#define IF_STRIKE    0x02
#define IF_CIRCLE    0x04        /* bullets */
#define IF_SQUARE    0x08

typedef struct {
    arena_t ar;
    node_t *root;
    int nnodes;
    char *title;
    char base[2048];             /* the URL relative links resolve against */
    uint32_t bg, fg, link_color, vlink_color;
    int has_bg;
    char *refresh_url;           /* <meta http-equiv=refresh> target (shown, not followed automatically) */
    /* layout */
    item_t *items;
    int nitems, cap_items;
    int width, height;           /* laid out for this width; total height */
    int content_w;               /* widest line */
    /* anchors: element ids / names and their y */
    struct anchor { const char *name; int y; } *anchors;
    int nanchors, cap_anchors;
    /* images */
    image_t images[DOC_MAX_IMAGES];
    int nimages;
    /* form controls in document order */
    fctl_t **ctls;
    int nctls, cap_ctls;
} doc_t;

/* html.c */
/* decode the bytes to UTF-8 (charset from HTTP, a BOM or <meta>) and parse */
doc_t *html_parse(const char *data, size_t len, const char *http_charset, const char *url);
/* plain text shown as a document */
doc_t *text_doc(const char *data, size_t len, const char *charset, const char *url);
void doc_free(doc_t *d);
const char *node_attr(const node_t *n, const char *name);
int  tag_of(const char *name, size_t len);
int  utf8_valid(const char *s, size_t n);
/* bytes in charset -> malloc()ed UTF-8 (NUL-terminated); *outlen */
char *to_utf8(const char *s, size_t n, const char *charset, size_t *outlen);
/* the charset a document declares in its first bytes (BOM, <meta>), lower case, or "" */
void sniff_charset(const char *s, size_t n, char *out, size_t outn);

/* layout.c */
typedef struct {
    int (*text_width)(const char *s, int n, int font);
    int (*font_height)(int font);
    int (*font_ascent)(int font);
    int (*text_fit)(const char *s, int n, int font, int width);   /* bytes that fit */
} metrics_t;
extern metrics_t g_metrics;
void layout_doc(doc_t *d, int width);
node_t *find_form(node_t *n);
void forms_init(doc_t *d);          /* make the controls (once per document) */
int  anchor_y(const doc_t *d, const char *name);
int  ctl_width(const fctl_t *c, int *h);

/* image.c: decode PNG, GIF or BMP (by content); 0 or < 0. Limits above. */
int  image_decode(const uint8_t *data, size_t len, int *w, int *h, uint32_t **px);

/* colors from attributes / CSS ("#rgb", "#rrggbb", names); 1 if parsed */
int  parse_color(const char *s, uint32_t *out);

#endif
