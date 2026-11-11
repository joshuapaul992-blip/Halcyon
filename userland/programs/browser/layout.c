/* BROWSER.EXE: layout. The element tree becomes a display list of text
 * runs, images, rules, bullets, form controls and table frames in page
 * coordinates.
 *
 * Block boxes stack vertically with collapsed margins; inline content is
 * broken into lines at spaces (words longer than the line are split), each
 * line aligned on a common baseline and to the left, centre or right.
 * <pre> keeps white space. Tables: column widths from each cell's minimum
 * and maximum content width (measured with the same code without emitting
 * anything, cached per element), distributed like the classic automatic
 * table layout; colspan is honoured, rowspan is not. Styles come from the
 * element, its old attributes (align, bgcolor, color, width, border...) and
 * a few properties of the style attribute (display, color, background,
 * font-weight, text-align, text-decoration, white-space). */
#include "browser.h"
#include "web.h"

metrics_t g_metrics;

#define INF_W (1 << 22)
enum { FONT_N_UI = 0, FONT_N_UI_BOLD, FONT_N_TITLE, FONT_N_MONO, FONT_N_LARGE, FONT_N_HUGE, FONT_N_SMALL };

typedef struct {
    int8_t size;                 /* -1 small, 0 normal, 1 big, 2 large, 3 huge */
    uint8_t bold, mono, underline, strike, pre, nowrap, align;
    uint32_t color;
    node_t *link;
} ist_t;

typedef struct {
    doc_t *d;
    int measure, m_min, m_max;
    int lx0, lw;                 /* the current block's content box: left and width */
    int y;                       /* top of the current line */
    int x;                       /* pen, relative to lx0 */
    int line_first;              /* first item of the current line */
    int asc, desc;               /* of the current line */
    int has;                     /* the line has content */
    int space;                   /* a collapsed space is pending */
    int space_w;
    int align;
    int margin;                  /* pending collapsed vertical margin */
    int list_depth;
    /* a list marker waiting for the first line of its item */
    int mk, mk_num, mk_type, mk_font, mk_x;
    uint32_t mk_color;
    int depth;
    /* text merge: the last text item and where its text ended */
    const char *merge_end;
} lc_t;

static int fontof(const ist_t *s) {
    if (s->mono) return FONT_N_MONO;
    if (s->size >= 3) return FONT_N_HUGE;
    if (s->size == 2) return FONT_N_LARGE;
    if (s->size == 1) return FONT_N_TITLE;
    if (s->size < 0) return FONT_N_SMALL;
    return s->bold ? FONT_N_UI_BOLD : FONT_N_UI;
}
static int tw(const char *s, int n, int font) { return n > 0 ? g_metrics.text_width(s, n, font) : 0; }
static int fh(int font) { return g_metrics.font_height(font); }
static int fa(int font) { return g_metrics.font_ascent(font); }

static int lcase(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* ---------------- styles ---------------- */
/* value of a CSS property in a style attribute (lower-cased copy) */
static int css_prop(const node_t *n, const char *prop, char *out, size_t outn) {
    const char *s = node_attr(n, "style");
    if (!s) return 0;
    size_t pl = strlen(prop);
    while (*s) {
        while (*s && (ws((unsigned char)*s) || *s == ';')) s++;
        const char *k = s;
        while (*s && *s != ':' && *s != ';') s++;
        size_t kl = (size_t)(s - k);
        while (kl && ws((unsigned char)k[kl - 1])) kl--;
        if (*s != ':') continue;
        s++;
        while (*s && ws((unsigned char)*s)) s++;
        const char *v = s;
        while (*s && *s != ';') s++;
        size_t vl = (size_t)(s - v);
        while (vl && ws((unsigned char)v[vl - 1])) vl--;
        int match = kl == pl;
        for (size_t i = 0; match && i < pl; i++) if (lcase((unsigned char)k[i]) != prop[i]) match = 0;
        if (match) {
            size_t o = 0;
            for (size_t i = 0; i < vl && o + 1 < outn; i++) out[o++] = (char)lcase((unsigned char)v[i]);
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

static int display_of(node_t *n) {
    if (n->is_text) return D_INLINE;
    char v[32];
    if (node_attr(n, "hidden")) return D_NONE;
    if (css_prop(n, "display", v, sizeof v)) {
        if (!strncmp(v, "none", 4)) return D_NONE;
        if (!strncmp(v, "block", 5) || !strncmp(v, "flex", 4) || !strncmp(v, "grid", 4)) return D_BLOCK;
        if (!strncmp(v, "inline", 6)) return D_INLINE;
        if (!strncmp(v, "list-item", 9)) return D_LIST_ITEM;
    }
    if (css_prop(n, "visibility", v, sizeof v) && !strncmp(v, "hidden", 6)) return D_NONE;
    switch (n->tag) {
    case T_HEAD: case T_TITLE: case T_META: case T_LINK: case T_BASE: case T_STYLE: case T_SCRIPT: case T_TEMPLATE:
    case T_PARAM: case T_SOURCE: case T_TRACK: case T_AREA: case T_MAP: case T_COL: case T_COLGROUP: case T_OPTION:
    case T_OPTGROUP: case T_EMBED: case T_SVG: case T_MATH: case T_FRAMESET: case T_DIALOG: case T_SELECTEDCONTENT:
        return D_NONE;
    case T_INPUT: {
        const char *t = node_attr(n, "type");
        return t && !strcasecmp(t, "hidden") ? D_NONE : D_INLINE;
    }
    case T_P: case T_DIV: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_UL: case T_OL:
    case T_DL: case T_DT: case T_DD: case T_MENU: case T_DIR: case T_BLOCKQUOTE: case T_CENTER: case T_PRE: case T_LISTING:
    case T_XMP: case T_PLAINTEXT: case T_FORM: case T_FIELDSET: case T_LEGEND: case T_ADDRESS: case T_ARTICLE:
    case T_ASIDE: case T_FOOTER: case T_HEADER: case T_MAIN: case T_NAV: case T_SECTION: case T_FIGURE: case T_FIGCAPTION:
    case T_DETAILS: case T_SUMMARY: case T_HR: case T_NOSCRIPT: case T_HGROUP: case T_BODY: case T_HTML: case T_NOFRAMES:
    case T_SEARCH:
        return D_BLOCK;
    case T_LI: return D_LIST_ITEM;
    case T_TABLE: return D_TABLE;
    case T_TR: return D_ROW;
    case T_TD: case T_TH: return D_CELL;
    case T_THEAD: case T_TBODY: case T_TFOOT: return D_ROWGROUP;
    case T_CAPTION: return D_CAPTION;
    }
    return D_INLINE;
}

static int align_attr(const node_t *n, int def) {
    const char *a = node_attr(n, "align");
    char v[16];
    if (css_prop(n, "text-align", v, sizeof v)) a = v;
    if (!a) return def;
    if (!strcasecmp(a, "center") || !strcasecmp(a, "middle")) return 1;
    if (!strcasecmp(a, "right")) return 2;
    if (!strcasecmp(a, "left") || !strcasecmp(a, "justify")) return 0;
    return def;
}

/* inline style changes of an element */
static void apply_style(doc_t *d, node_t *n, ist_t *s) {
    uint32_t c;
    switch (n->tag) {
    case T_B: case T_STRONG: case T_TH: case T_DT: case T_LEGEND: case T_SUMMARY: s->bold = 1; break;
    case T_U: case T_INS: s->underline = 1; break;
    case T_S: case T_STRIKE: case T_DEL: s->strike = 1; break;
    case T_CODE: case T_TT: case T_KBD: case T_SAMP: case T_PRE: case T_LISTING: case T_XMP: case T_PLAINTEXT: s->mono = 1; break;
    case T_SMALL: case T_SUB: case T_SUP: s->size = -1; break;
    case T_BIG: s->size = s->size < 1 ? 1 : s->size; break;
    case T_H1: s->size = 3; s->bold = 1; break;
    case T_H2: s->size = 2; s->bold = 1; break;
    case T_H3: s->size = 1; s->bold = 1; break;
    case T_H4: case T_H5: case T_H6: s->size = 0; s->bold = 1; break;
    case T_NOBR: s->nowrap = 1; break;
    case T_CENTER: s->align = 1; break;
    case T_A:
        if (node_attr(n, "href")) { s->link = n; s->color = d->link_color; s->underline = 1; }
        break;
    case T_FONT: {
        const char *col = node_attr(n, "color"), *sz = node_attr(n, "size");
        if (col && parse_color(col, &c)) s->color = c;
        if (sz) {
            int v = atoi(sz[0] == '+' ? sz + 1 : sz);
            if (sz[0] == '+') v += 3;
            else if (sz[0] == '-') v = 3 - atoi(sz + 1);
            s->size = v >= 7 ? 3 : v >= 5 ? 2 : v == 4 ? 1 : v <= 2 ? -1 : 0;
        }
        break;
    }
    }
    if (n->tag == T_PRE || n->tag == T_LISTING || n->tag == T_XMP || n->tag == T_PLAINTEXT || n->tag == T_TEXTAREA) s->pre = 1;
    char v[48];
    if (css_prop(n, "color", v, sizeof v) && parse_color(v, &c)) s->color = c;
    if (css_prop(n, "font-weight", v, sizeof v)) s->bold = !strcmp(v, "bold") || !strcmp(v, "bolder") || atoi(v) >= 600;
    if (css_prop(n, "text-decoration", v, sizeof v) || css_prop(n, "text-decoration-line", v, sizeof v)) {
        if (strstr(v, "underline")) s->underline = 1;
        if (strstr(v, "line-through")) s->strike = 1;
        if (strstr(v, "none")) s->underline = s->strike = 0;
    }
    if (css_prop(n, "white-space", v, sizeof v)) {
        if (!strncmp(v, "pre", 3)) s->pre = 1;
        if (!strcmp(v, "nowrap")) s->nowrap = 1;
        if (!strcmp(v, "normal")) s->pre = s->nowrap = 0;
    }
    if (css_prop(n, "font-family", v, sizeof v) && strstr(v, "monospace")) s->mono = 1;
}

/* ---------------- the display list ---------------- */
static item_t *emit(lc_t *c, int kind) {
    if (c->measure) return 0;
    doc_t *d = c->d;
    if (d->nitems >= DOC_MAX_ITEMS) return 0;
    if (d->nitems == d->cap_items) {
        int nc = d->cap_items ? d->cap_items * 2 : 1024;
        item_t *ni = realloc(d->items, sizeof(item_t) * (size_t)nc);
        if (!ni) return 0;
        d->items = ni;
        d->cap_items = nc;
    }
    item_t *it = &d->items[d->nitems++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    return it;
}

static void add_anchor(lc_t *c, const char *name) {
    doc_t *d = c->d;
    if (c->measure || !name || !name[0]) return;
    if (d->nanchors == d->cap_anchors) {
        int nc = d->cap_anchors ? d->cap_anchors * 2 : 64;
        if (nc > 100000) return;
        struct anchor *na = realloc(d->anchors, sizeof *na * (size_t)nc);
        if (!na) return;
        d->anchors = na;
        d->cap_anchors = nc;
    }
    d->anchors[d->nanchors].name = name;
    d->anchors[d->nanchors].y = c->y + c->margin;
    d->nanchors++;
}

int anchor_y(const doc_t *d, const char *name) {
    for (int i = 0; i < d->nanchors; i++) if (!strcmp(d->anchors[i].name, name)) return d->anchors[i].y;
    return -1;
}

/* ---------------- lines ---------------- */
static void grow_line(lc_t *c, int a, int dsc);
static void line_end(lc_t *c, int forced_font) {
    if (!c->has && forced_font < 0) return;
    if (!c->has) { c->asc = fa(forced_font); c->desc = fh(forced_font) - c->asc; }
    if (c->measure) {
        if (c->x > c->m_max) c->m_max = c->x;
    } else {
        doc_t *d = c->d;
        int shift = c->align == 1 ? (c->lw - c->x) / 2 : c->align == 2 ? c->lw - c->x : 0;
        if (shift < 0) shift = 0;
        for (int i = c->line_first; i < d->nitems; i++) {
            item_t *it = &d->items[i];
            int a = it->kind == IT_TEXT || it->kind == IT_BULLET ? fa(it->font) : it->kind == IT_CONTROL ? it->len : it->h;
            if (it->kind == IT_ALT) a = it->h - 3;
            it->y = c->y + c->asc - a;
            it->x += shift;
        }
        if (c->x + c->lx0 > d->content_w) d->content_w = c->x + c->lx0;
    }
    c->y += c->asc + c->desc;
    if (c->y > DOC_MAX_HEIGHT) c->y = DOC_MAX_HEIGHT;
    c->x = 0;
    c->asc = c->desc = 0;
    c->has = 0;
    c->space = 0;
    c->merge_end = 0;
    c->line_first = c->d->nitems;
}

/* something of height h (ascent a) is about to go on the line */
static void line_open(lc_t *c) {
    if (c->has) return;
    c->y += c->margin;
    c->margin = 0;
    c->line_first = c->d->nitems;
    c->has = 1;
    if (c->mk) {
        /* the list marker, left of the line */
        c->mk = 0;
        grow_line(c, fa(c->mk_font), fh(c->mk_font) - fa(c->mk_font));
        item_t *it = emit(c, IT_BULLET);
        if (it) {
            it->font = (uint8_t)c->mk_font;
            it->color = c->mk_color;
            it->flags = (uint8_t)(c->mk_type == 1 ? IF_CIRCLE : c->mk_type == 2 ? IF_SQUARE : 0);
            it->len = c->mk_type >= 10 ? c->mk_num : -1;
            it->w = c->mk_type;
            it->x = c->lx0 - 18;
            it->h = 0;
        }
    }
}

static void grow_line(lc_t *c, int a, int dsc) {
    if (a > c->asc) c->asc = a;
    if (dsc > c->desc) c->desc = dsc;
}

static void put_text(lc_t *c, const ist_t *s, const char *t, int n, int w) {
    int f = fontof(s);
    line_open(c);
    grow_line(c, fa(f), fh(f) - fa(f));
    if (c->measure) { c->x += w; return; }
    doc_t *d = c->d;
    item_t *prev = d->nitems > c->line_first ? &d->items[d->nitems - 1] : 0;
    /* merge with the word before when the text continues it (one space between, same style) */
    if (prev && prev->kind == IT_TEXT && c->merge_end && prev->text + prev->len == c->merge_end && c->merge_end + 1 == t && *c->merge_end == ' ' &&
        prev->font == f && prev->color == s->color && prev->link == s->link &&
        prev->flags == ((s->underline ? IF_UNDERLINE : 0) | (s->strike ? IF_STRIKE : 0)) && prev->len + n + 1 < 2000) {
        prev->len += n + 1;
        prev->w = c->x + w - (prev->x - c->lx0);
        c->x += w;
        c->merge_end = t + n;
        return;
    }
    item_t *it = emit(c, IT_TEXT);
    if (it) {
        it->font = (uint8_t)f;
        it->color = s->color;
        it->flags = (uint8_t)((s->underline ? IF_UNDERLINE : 0) | (s->strike ? IF_STRIKE : 0));
        it->text = t;
        it->len = n;
        it->x = c->lx0 + c->x;
        it->w = w;
        it->h = fh(f);
        it->link = s->link;
    }
    c->x += w;
    c->merge_end = t + n;
}

/* a box (image, control) of w x h on the line, bottom on the baseline */
static item_t *put_box_a(lc_t *c, const ist_t *s, int kind, int w, int h, int asc);
static item_t *put_box(lc_t *c, const ist_t *s, int kind, int w, int h) { return put_box_a(c, s, kind, w, h, h); }
static item_t *put_box_a(lc_t *c, const ist_t *s, int kind, int w, int h, int asc) {
    if (c->has && !s->nowrap && c->x + (c->space ? c->space_w : 0) + w > c->lw && c->x > 0) line_end(c, -1);
    if (c->space && c->has) c->x += c->space_w;
    c->space = 0;
    line_open(c);
    grow_line(c, asc, h - asc > 2 ? h - asc : 2);
    if (c->measure) {
        if (w > c->m_min) c->m_min = w;
        c->x += w;
        return 0;
    }
    item_t *it = emit(c, kind);
    if (it) { it->x = c->lx0 + c->x; it->w = w; it->h = h; it->link = s->link; it->len = asc; }
    c->x += w;
    c->merge_end = 0;
    return it;
}

static void text_run(lc_t *c, const ist_t *s, const char *t, int n) {
    int f = fontof(s);
    if (s->pre) {
        int i = 0;
        while (i < n) {
            int e = i;
            while (e < n && t[e] != '\n' && e - i < 1500) e++;
            /* tabs: shown as up to 8 spaces (the text is drawn as is: tabs are rare in pages) */
            if (e > i) {
                int w = tw(t + i, e - i, f);
                for (int k = i; k < e; k++) if (t[k] == '\t') w += tw("        ", 7, f);
                c->merge_end = 0;
                put_text(c, s, t + i, e - i, w);
                if (c->measure && w > c->m_min) c->m_min = w;
            }
            if (e < n && t[e] == '\n') { line_end(c, f); e++; }
            i = e;
        }
        return;
    }
    int sw = tw(" ", 1, f);
    int i = 0;
    while (i < n) {
        if (ws((unsigned char)t[i])) {
            while (i < n && ws((unsigned char)t[i])) i++;
            if (c->has) { c->space = 1; c->space_w = sw; }
            continue;
        }
        int e = i;
        while (e < n && !ws((unsigned char)t[e])) e++;
        int w = tw(t + i, e - i, f);
        if (c->measure) {
            if (w > c->m_min) c->m_min = w;
            if (c->space && c->has) c->x += c->space_w;
            c->space = 0;
            put_text(c, s, t + i, e - i, w);
            i = e;
            continue;
        }
        int sp = c->space && c->has ? c->space_w : 0;
        if (c->has && !s->nowrap && c->x + sp + w > c->lw) { line_end(c, -1); sp = 0; }
        if (!c->has && w > c->lw && !s->nowrap && c->lw > 20) {
            /* a word longer than the line: split it */
            int k = i;
            while (k < e) {
                int fit = g_metrics.text_fit(t + k, e - k, f, c->lw);
                if (fit <= 0) fit = 1;
                while (k + fit < e && ((unsigned char)t[k + fit] & 0xC0) == 0x80) fit++;
                c->merge_end = 0;
                put_text(c, s, t + k, fit, tw(t + k, fit, f));
                k += fit;
                if (k < e) line_end(c, -1);
            }
            i = e;
            continue;
        }
        c->x += sp;
        c->space = 0;
        if (!sp) c->merge_end = 0;
        put_text(c, s, t + i, e - i, w);
        i = e;
    }
}

/* ---------------- form controls ---------------- */
static int avg_cw(void) { int w = tw("0", 1, FONT_N_UI); return w > 0 ? w : 7; }

int ctl_width(const fctl_t *k, int *h) {
    int lh = fh(FONT_N_UI);
    switch (k->kind) {
    case FC_TEXT: case FC_PASSWORD:
        *h = lh + 8;
        return (k->size > 0 && k->size < 200 ? k->size : 20) * avg_cw() + 8;
    case FC_TEXTAREA:
        *h = (k->rows > 0 && k->rows < 100 ? k->rows : 2) * lh + 8;
        return (k->cols > 0 && k->cols < 200 ? k->cols : 20) * avg_cw() + 8;
    case FC_CHECKBOX: case FC_RADIO:
        *h = 13;
        return 13;
    case FC_SELECT: {
        int w = 20;
        for (int i = 0; i < k->noptions; i++) { int x = tw(k->opt_label[i], (int)strlen(k->opt_label[i]), FONT_N_UI); if (x > w) w = x; }
        *h = lh + 8;
        return (w > 400 ? 400 : w) + 26;
    }
    default: {
        const char *v = k->value && k->value[0] ? k->value : k->kind == FC_RESET ? "Reset" : "Submit";
        int w = tw(v, (int)strlen(v), FONT_N_UI);
        *h = lh + 10;
        return (w > 500 ? 500 : w) + 20;
    }
    }
}

node_t *find_form(node_t *n) {
    for (node_t *p = n->parent; p; p = p->parent) if (p->tag == T_FORM) return p;
    return 0;
}

/* the text content of an element, flattened (labels of buttons and options) */
static void text_content(const node_t *n, char *out, size_t on, size_t *pos) {
    for (const node_t *k = n->first; k; k = k->next) {
        if (k->is_text) {
            for (int i = 0; i < k->textlen && *pos + 1 < on; i++) {
                char ch = ws((unsigned char)k->text[i]) ? ' ' : k->text[i];
                if (ch == ' ' && (*pos == 0 || out[*pos - 1] == ' ')) continue;
                out[(*pos)++] = ch;
            }
        } else if (k->tag != T_SCRIPT && k->tag != T_STYLE) text_content(k, out, on, pos);
    }
    out[*pos] = 0;
}
static char *flat_text(const node_t *n) {
    char buf[256];
    size_t p = 0;
    buf[0] = 0;
    text_content(n, buf, sizeof buf, &p);
    while (p && buf[p - 1] == ' ') buf[--p] = 0;
    return strdup(buf);
}

static void add_ctl(doc_t *d, fctl_t *k) {
    if (d->nctls == d->cap_ctls) {
        int nc = d->cap_ctls ? d->cap_ctls * 2 : 32;
        fctl_t **n = realloc(d->ctls, sizeof *n * (size_t)nc);
        if (!n) { free(k->value); free(k); return; }
        d->ctls = n;
        d->cap_ctls = nc;
    }
    d->ctls[d->nctls++] = k;
    k->node->ctl = k;
}

static void make_ctls(doc_t *d, node_t *n) {
    for (node_t *k = n->first; k; k = k->next) {
        if (k->is_text) continue;
        if (d->nctls >= 5000) return;
        int t = k->tag;
        if (t == T_INPUT || t == T_SELECT || t == T_TEXTAREA || t == T_BUTTON) {
            fctl_t *c = calloc(1, sizeof *c);
            if (!c) return;
            c->node = k;
            c->form = find_form(k);
            const char *nm = node_attr(k, "name");
            c->name = nm ? (char *)nm : 0;
            const char *v = node_attr(k, "value");
            if (t == T_INPUT) {
                const char *ty = node_attr(k, "type");
                ty = ty ? ty : "text";
                c->kind = !strcasecmp(ty, "password") ? FC_PASSWORD : !strcasecmp(ty, "submit") ? FC_SUBMIT :
                          !strcasecmp(ty, "reset") ? FC_RESET : !strcasecmp(ty, "button") ? FC_BUTTON :
                          !strcasecmp(ty, "checkbox") ? FC_CHECKBOX : !strcasecmp(ty, "radio") ? FC_RADIO :
                          !strcasecmp(ty, "hidden") ? FC_HIDDEN : !strcasecmp(ty, "image") ? FC_IMAGE : FC_TEXT;
                if (c->kind == FC_IMAGE) c->kind = FC_SUBMIT;
                c->value = strdup(v ? v : (c->kind == FC_CHECKBOX || c->kind == FC_RADIO) ? "on" : "");
                c->checked = node_attr(k, "checked") != 0;
                const char *sz = node_attr(k, "size");
                c->size = sz ? atoi(sz) : 20;
                if (c->kind == FC_SUBMIT && !v) { free(c->value); c->value = strdup(node_attr(k, "alt") ? node_attr(k, "alt") : "Submit"); }
            } else if (t == T_BUTTON) {
                const char *ty = node_attr(k, "type");
                c->kind = ty && !strcasecmp(ty, "reset") ? FC_RESET : ty && !strcasecmp(ty, "button") ? FC_BUTTON : FC_SUBMIT;
                char *label = flat_text(k);
                c->value = label ? label : strdup("");
                if (v) { c->opt_value = malloc(sizeof(char *)); if (c->opt_value) { c->opt_value[0] = strdup(v); c->noptions = 1; } }
            } else if (t == T_TEXTAREA) {
                c->kind = FC_TEXTAREA;
                size_t p = 0;
                char *buf = malloc(65536);
                if (buf) {
                    buf[0] = 0;
                    for (node_t *x = k->first; x; x = x->next)
                        if (x->is_text && p + (size_t)x->textlen < 65535) { memcpy(buf + p, x->text, (size_t)x->textlen); p += (size_t)x->textlen; }
                    buf[p] = 0;
                }
                c->value = buf ? buf : strdup("");
                const char *r = node_attr(k, "rows"), *cl = node_attr(k, "cols");
                c->rows = r ? atoi(r) : 2;
                c->cols = cl ? atoi(cl) : 20;
            } else {
                c->kind = FC_SELECT;
                /* the options, in order (also inside optgroups) */
                node_t *opts[256];
                int no = 0;
                for (node_t *o = k->first; o && no < 256; o = o->next) {
                    if (!o->is_text && o->tag == T_OPTION) opts[no++] = o;
                    else if (!o->is_text && o->tag == T_OPTGROUP)
                        for (node_t *q = o->first; q && no < 256; q = q->next) if (!q->is_text && q->tag == T_OPTION) opts[no++] = q;
                }
                c->opt_label = calloc((size_t)(no ? no : 1), sizeof(char *));
                c->opt_value = calloc((size_t)(no ? no : 1), sizeof(char *));
                if (c->opt_label && c->opt_value) {
                    for (int i = 0; i < no; i++) {
                        c->opt_label[i] = flat_text(opts[i]);
                        const char *ov = node_attr(opts[i], "value");
                        c->opt_value[i] = ov ? strdup(ov) : strdup(c->opt_label[i] ? c->opt_label[i] : "");
                        if (node_attr(opts[i], "selected")) c->selected = i;
                        if (!c->opt_label[i] || !c->opt_value[i]) { no = i; break; }
                    }
                    c->noptions = no;
                }
                c->value = strdup("");
            }
            if (!c->value) { free(c); return; }
            c->vlen = (int)strlen(c->value);
            c->vcap = c->vlen + 1;
            c->caret = c->vlen;
            add_ctl(d, c);
            if (t == T_SELECT || t == T_TEXTAREA || t == T_BUTTON) continue;   /* their children are not more controls */
        }
        make_ctls(d, k);
    }
}

void forms_init(doc_t *d) { if (!d->nctls) make_ctls(d, d->root); }

/* ---------------- blocks ---------------- */
static void flow(lc_t *c, node_t *n, ist_t s);
static void block(lc_t *c, node_t *n, ist_t s, int disp);

static void margin(lc_t *c, int m) { if (m > c->margin) c->margin = m; }

static int px_attr(const node_t *n, const char *name, int rel, int def) {
    const char *v = node_attr(n, name);
    if (!v || !*v) return def;
    int x = atoi(v);
    if (strchr(v, '%')) return rel > 0 && x > 0 ? (int)((long)rel * (x > 100 ? 100 : x) / 100) : def;
    return x < 0 ? def : x > 100000 ? 100000 : x;
}

static void image_box(lc_t *c, node_t *n, const ist_t *s) {
    doc_t *d = c->d;
    const char *src = node_attr(n, "src");
    if (!n->img && src && src[0] && d->nimages < DOC_MAX_IMAGES && !c->measure) {
        char abs[URL_MAX];
        if (!url_resolve(d->base, src, abs, sizeof abs)) {
            int k = d->nimages;
            d->images[k].url = arena_strndup(&d->ar, abs, strlen(abs));
            if (d->images[k].url) { d->nimages++; n->img = k + 1; }
        }
    }
    image_t *im = n->img ? &d->images[n->img - 1] : 0;
    int w = px_attr(n, "width", c->lw, -1), h = px_attr(n, "height", -1, -1);
    if (im && im->state == 2) {
        if (w < 0 && h < 0) { w = im->w; h = im->h; }
        else if (w < 0) w = im->h ? (int)((long)h * im->w / im->h) : im->w;
        else if (h < 0) h = im->w ? (int)((long)w * im->h / im->w) : im->h;
    }
    const char *alt = node_attr(n, "alt");
    if (w < 0 || h < 0 || (im && im->state == 3)) {
        /* no size known: the alternative text (or nothing) */
        if (!alt || !alt[0]) {
            if (w > 0 && h > 0 && w < 4096 && h < 4096) { item_t *it = put_box(c, s, IT_FRAME, w, h); if (it) it->node = n; }
            return;
        }
        int f = FONT_N_UI;
        int aw = tw(alt, (int)strlen(alt), f) + 8;
        if (aw > c->lw && c->lw > 40) aw = c->lw;
        item_t *it = put_box(c, s, IT_ALT, w > 0 && w < 4096 ? w : aw, h > 0 && h < 4096 ? h : fh(f) + 6);
        if (it) { it->text = alt; it->len = (int)strlen(alt); it->font = (uint8_t)f; it->node = n; it->color = s->color; }
        return;
    }
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    item_t *it = put_box(c, s, im && im->state == 2 ? IT_IMAGE : IT_FRAME, w, h);
    if (it) { it->node = n; it->text = alt; it->len = alt ? (int)strlen(alt) : 0; }
}

static void control_box(lc_t *c, node_t *n, const ist_t *s) {
    fctl_t *k = n->ctl;
    if (!k || k->kind == FC_HIDDEN) return;
    int h, w = ctl_width(k, &h);
    /* the baseline of the text inside, or the bottom of a check box less a little */
    int asc = k->kind == FC_CHECKBOX || k->kind == FC_RADIO ? h - 1 :
              k->kind == FC_SUBMIT || k->kind == FC_RESET || k->kind == FC_BUTTON ? (h - fh(FONT_N_UI)) / 2 + fa(FONT_N_UI) :
              4 + fa(FONT_N_UI);
    if (asc > h) asc = h;
    item_t *it = put_box_a(c, s, IT_CONTROL, w, h, asc);
    if (it) { it->node = n; it->link = 0; }
}

/* the table's rows (in thead / tbody / tfoot or directly) */
#define MAX_ROWS 4000
#define MAX_COLS 64
typedef struct { node_t *cell[MAX_COLS]; int span[MAX_COLS]; int n; node_t *tr; } row_t;

static void add_cell(row_t *r, node_t *td) {
    if (r->n >= MAX_COLS || display_of(td) == D_NONE) return;
    r->cell[r->n] = td;
    int sp = px_attr(td, "colspan", -1, 1);
    r->span[r->n++] = sp < 1 ? 1 : sp > MAX_COLS ? MAX_COLS : sp;
}

static int collect_rows(node_t *t, row_t *rows, int max) {
    int nr = 0;
    for (node_t *k = t->first; k && nr < max; k = k->next) {
        if (k->is_text || display_of(k) == D_NONE) continue;
        if (k->tag == T_TD || k->tag == T_TH) {
            /* cells without a row: one implied row for consecutive cells */
            row_t *r = &rows[nr++];
            r->n = 0;
            r->tr = 0;
            node_t *last = k;
            for (node_t *q = k; q && (q->is_text || q->tag == T_TD || q->tag == T_TH); q = q->next) {
                last = q;
                if (!q->is_text) add_cell(r, q);
            }
            k = last;
            continue;
        }
        node_t *first, *only = 0;
        if (k->tag == T_THEAD || k->tag == T_TBODY || k->tag == T_TFOOT) first = k->first;
        else if (k->tag == T_TR) { first = k; only = k; }
        else continue;
        for (node_t *tr = first; tr && nr < max; tr = only ? 0 : tr->next) {
            if (tr->is_text || tr->tag != T_TR || display_of(tr) == D_NONE) continue;
            row_t *r = &rows[nr++];
            r->n = 0;
            r->tr = tr;
            for (node_t *td = tr->first; td; td = td->next)
                if (!td->is_text && (td->tag == T_TD || td->tag == T_TH)) add_cell(r, td);
        }
    }
    return nr;
}

/* minimum and maximum content width of an element (cached) */
static void measure(lc_t *outer, node_t *n, ist_t s, int *mn, int *mx) {
    if (n->min_w >= 0) { *mn = n->min_w; *mx = n->max_w; return; }
    lc_t m;
    memset(&m, 0, sizeof m);
    m.d = outer->d;
    m.measure = 1;
    m.lw = INF_W;
    m.line_first = outer->d->nitems;
    m.depth = outer->depth + 1;
    m.list_depth = outer->list_depth;
    if (m.depth > LAYOUT_MAX_DEPTH) { *mn = *mx = 0; return; }
    flow(&m, n, s);
    line_end(&m, -1);
    n->min_w = m.m_min;
    n->max_w = m.m_max > m.m_min ? m.m_max : m.m_min;
    *mn = n->min_w;
    *mx = n->max_w;
}

static void table(lc_t *c, node_t *t, ist_t s) {
    row_t *rows = malloc(sizeof(row_t) * MAX_ROWS);
    if (!rows) return;
    int nr = collect_rows(t, rows, MAX_ROWS);
    const char *bs = node_attr(t, "border");
    int border = bs ? (bs[0] ? atoi(bs) : 1) : 0;
    if (border > 20) border = 20;
    int spacing = px_attr(t, "cellspacing", -1, 2), pad = px_attr(t, "cellpadding", -1, 1);
    if (spacing > 50) spacing = 50;
    if (pad > 50) pad = 50;
    int cb = border ? 1 : 0;                     /* cell frames */
    int ncols = 0;
    for (int r = 0; r < nr; r++) {
        int w = 0;
        for (int k = 0; k < rows[r].n; k++) w += rows[r].span[k];
        if (w > ncols) ncols = w;
    }
    if (ncols > MAX_COLS) ncols = MAX_COLS;
    int cmin[MAX_COLS], cmax[MAX_COLS], fixed[MAX_COLS];
    for (int i = 0; i < MAX_COLS; i++) cmin[i] = cmax[i] = fixed[i] = 0;
    ist_t cs = s;
    cs.align = 0;
    /* column widths from the cells */
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < nr; r++) {
            int col = 0;
            for (int k = 0; k < rows[r].n && col < ncols; k++) {
                node_t *td = rows[r].cell[k];
                int sp = rows[r].span[k];
                if (col + sp > ncols) sp = ncols - col;
                if ((sp == 1) != (pass == 0)) { col += sp; continue; }
                ist_t ts = cs;
                apply_style(c->d, td, &ts);
                int mn, mx;
                measure(c, td, ts, &mn, &mx);
                int wa = px_attr(td, "width", -1, -1);
                if (wa > 0 && !strchr(node_attr(td, "width"), '%')) { if (wa > mn) mn = wa; mx = mn; }
                if (node_attr(td, "nowrap")) mn = mx;
                mn += 2 * (pad + cb);
                mx += 2 * (pad + cb);
                if (sp == 1) {
                    if (mn > cmin[col]) cmin[col] = mn;
                    if (mx > cmax[col]) cmax[col] = mx;
                    if (wa > 0) fixed[col] = 1;
                } else {
                    int smin = 0, smax = 0;
                    for (int i = col; i < col + sp; i++) { smin += cmin[i]; smax += cmax[i]; }
                    smin += (sp - 1) * spacing;
                    smax += (sp - 1) * spacing;
                    for (int i = col; i < col + sp; i++) {
                        if (mn > smin) cmin[i] += (mn - smin) / sp + 1;
                        if (mx > smax) cmax[i] += (mx - smax) / sp + 1;
                    }
                }
                col += sp;
            }
        }
    }
    int extra = (ncols + 1) * spacing + 2 * border;
    int summin = 0, summax = 0;
    for (int i = 0; i < ncols; i++) { if (cmax[i] < cmin[i]) cmax[i] = cmin[i]; summin += cmin[i]; summax += cmax[i]; }
    int avail = c->lw;
    int want = px_attr(t, "width", c->measure ? 0 : avail, -1);
    char wv[16];
    if (want < 0 && css_prop(t, "width", wv, sizeof wv) && strchr(wv, '%') && !c->measure) want = avail * (atoi(wv) > 100 ? 100 : atoi(wv)) / 100;
    if (c->measure) {
        /* the table's own minimum and maximum */
        int mn = summin + extra, mx = summax + extra;
        int wa = px_attr(t, "width", -1, -1);
        if (wa > 0 && !strchr(node_attr(t, "width"), '%')) { if (wa > mn) mn = wa; mx = mn; }
        line_end(c, -1);
        if (mn > c->m_min) c->m_min = mn;
        if (mx > c->m_max) c->m_max = mx;
        free(rows);
        return;
    }
    int tw0 = want > 0 ? want : summax + extra;
    if (tw0 > avail && want <= 0) tw0 = avail;
    if (tw0 < summin + extra) tw0 = summin + extra;
    int inner = tw0 - extra;
    int cw[MAX_COLS];
    if (inner >= summax) {
        /* room to spare: grow the columns in proportion (only if a width was asked for) */
        int spare = inner - summax, nflex = 0;
        for (int i = 0; i < ncols; i++) if (!fixed[i]) nflex++;
        for (int i = 0; i < ncols; i++) {
            cw[i] = cmax[i];
            if (spare > 0) {
                if (nflex && !fixed[i]) cw[i] += summax ? (int)((long)spare * cmax[i] / (summax ? summax : 1)) : spare / nflex;
                else if (!nflex) cw[i] += spare / ncols;
            }
        }
    } else {
        long range = summax - summin;
        for (int i = 0; i < ncols; i++)
            cw[i] = cmin[i] + (range > 0 ? (int)((long)(inner - summin) * (cmax[i] - cmin[i]) / range) : 0);
    }
    /* place the table */
    line_end(c, -1);
    c->y += c->margin;
    c->margin = 0;
    int talign = align_attr(t, s.align == 1 ? 1 : 0);
    int tx = c->lx0 + (talign == 1 ? (c->lw - tw0) / 2 : talign == 2 ? c->lw - tw0 : 0);
    if (tx < c->lx0) tx = c->lx0;
    int ty = c->y;
    uint32_t tbg = 0;
    int has_tbg = parse_color(node_attr(t, "bgcolor") ? node_attr(t, "bgcolor") : "", &tbg);
    item_t *tbox = 0;
    int tbox_i = -1;
    if (has_tbg) { tbox = emit(c, IT_RECT); if (tbox) { tbox_i = (int)(tbox - c->d->items); tbox->x = tx; tbox->y = ty; tbox->w = tw0; tbox->color = tbg; } }
    int y = ty + border + spacing;
    for (int r = 0; r < nr; r++) {
        int col = 0, x = tx + border + spacing, rowh = 0;
        int first[MAX_COLS], hts[MAX_COLS], xs[MAX_COLS], ws_[MAX_COLS], bgi[MAX_COLS], valign[MAX_COLS];
        int ncell = 0;
        uint32_t rbg = 0;
        int has_rbg = rows[r].tr && parse_color(node_attr(rows[r].tr, "bgcolor") ? node_attr(rows[r].tr, "bgcolor") : "", &rbg);
        const char *rva = rows[r].tr ? node_attr(rows[r].tr, "valign") : 0;
        int row_align = rows[r].tr ? align_attr(rows[r].tr, -1) : -1;
        for (int k = 0; k < rows[r].n && col < ncols; k++) {
            node_t *td = rows[r].cell[k];
            int sp = rows[r].span[k];
            if (col + sp > ncols) sp = ncols - col;
            int w = (sp - 1) * spacing;
            for (int i = col; i < col + sp; i++) w += cw[i];
            uint32_t bg = 0;
            int has_bg = parse_color(node_attr(td, "bgcolor") ? node_attr(td, "bgcolor") : "", &bg);
            char bv[32];
            if (!has_bg && css_prop(td, "background-color", bv, sizeof bv)) has_bg = parse_color(bv, &bg);
            if (!has_bg && has_rbg) { bg = rbg; has_bg = 1; }
            bgi[ncell] = -1;
            if (has_bg) { item_t *b = emit(c, IT_RECT); if (b) { b->x = x; b->y = y; b->w = w; b->color = bg; bgi[ncell] = (int)(b - c->d->items); } }
            /* the cell's content: a block of its own */
            lc_t save = *c;
            c->lx0 = x + pad + cb;
            c->lw = w - 2 * (pad + cb);
            if (c->lw < 1) c->lw = 1;
            c->y = y + pad + cb;
            c->x = 0; c->has = 0; c->space = 0; c->margin = 0; c->mk = 0; c->asc = c->desc = 0;
            c->line_first = c->d->nitems;
            c->depth++;
            ist_t ts = cs;
            if (td->tag == T_TH) { ts.bold = 1; ts.align = 1; }
            if (row_align >= 0) ts.align = (uint8_t)row_align;
            apply_style(c->d, td, &ts);
            ts.align = (uint8_t)align_attr(td, ts.align);
            c->align = ts.align;
            first[ncell] = c->d->nitems;
            if (c->depth <= LAYOUT_MAX_DEPTH) flow(c, td, ts);
            line_end(c, -1);
            int h = c->y - y + pad + cb;
            const char *va = node_attr(td, "valign");
            if (!va) va = rva;
            valign[ncell] = va && !strcasecmp(va, "top") ? 0 : va && !strcasecmp(va, "bottom") ? 2 : 1;
            *c = save;
            c->merge_end = 0;
            hts[ncell] = h;
            xs[ncell] = x;
            ws_[ncell] = w;
            if (h > rowh) rowh = h;
            ncell++;
            x += w + spacing;
            col += sp;
        }
        /* vertical alignment, backgrounds and frames */
        for (int k = 0; k < ncell; k++) {
            int end = k + 1 < ncell ? first[k + 1] - (bgi[k + 1] >= 0 ? 1 : 0) : c->d->nitems;
            int dy = valign[k] == 1 ? (rowh - hts[k]) / 2 : valign[k] == 2 ? rowh - hts[k] : 0;
            if (dy > 0) for (int i = first[k]; i < end; i++) c->d->items[i].y += dy;
            if (bgi[k] >= 0) c->d->items[bgi[k]].h = rowh;
            if (cb) { item_t *f = emit(c, IT_FRAME); if (f) { f->x = xs[k]; f->y = y; f->w = ws_[k]; f->h = rowh; f->color = 0x808080; f->flags = 1; } }
        }
        y += rowh + spacing;
        if (y > DOC_MAX_HEIGHT) break;
    }
    c->line_first = c->d->nitems;
    y += border;
    if (tbox_i >= 0) c->d->items[tbox_i].h = y - ty;
    if (border) {
        item_t *f = emit(c, IT_FRAME);
        if (f) { f->x = tx; f->y = ty; f->w = tw0; f->h = y - ty; f->color = 0x404040; f->flags = (uint8_t)(border > 3 ? 3 : border); }
    }
    if (tx + tw0 > c->d->content_w) c->d->content_w = tx + tw0;
    c->y = y;
    free(rows);
}

static void list_marker(lc_t *c, node_t *li, ist_t s, int num) {
    node_t *p = li->parent;
    int ordered = p && p->tag == T_OL;
    const char *ty = node_attr(li, "type");
    if (!ty && p) ty = node_attr(p, "type");
    c->mk = 1;
    c->mk_num = num;
    c->mk_font = fontof(&s);
    c->mk_color = s.color;
    if (ordered) {
        c->mk_type = 10;
        if (ty && ty[0] == 'a') c->mk_type = 11;
        else if (ty && ty[0] == 'A') c->mk_type = 12;
        else if (ty && ty[0] == 'i') c->mk_type = 13;
        else if (ty && ty[0] == 'I') c->mk_type = 14;
    } else {
        int lvl = c->list_depth - 1;
        c->mk_type = ty && !strcasecmp(ty, "circle") ? 1 : ty && !strcasecmp(ty, "square") ? 2 : lvl <= 0 ? 0 : lvl == 1 ? 1 : 2;
        char v[24];
        if (css_prop(li, "list-style-type", v, sizeof v) || (p && css_prop(p, "list-style-type", v, sizeof v)) ||
            css_prop(li, "list-style", v, sizeof v) || (p && css_prop(p, "list-style", v, sizeof v)))
            if (strstr(v, "none")) c->mk = 0;
    }
}

static void block(lc_t *c, node_t *n, ist_t s, int disp) {
    int t = n->tag;
    int mt = 0, mb = 0, ml = 0, mr = 0;
    int fsz = fh(fontof(&s));
    switch (t) {
    case T_P: case T_DL: case T_PRE: case T_LISTING: case T_XMP: case T_ADDRESS: case T_FIGURE: case T_HGROUP:
        mt = mb = fsz; break;
    case T_H1: mt = mb = 16; break;
    case T_H2: mt = mb = 14; break;
    case T_H3: mt = mb = 12; break;
    case T_H4: case T_H5: case T_H6: mt = mb = 11; break;
    case T_BLOCKQUOTE: mt = mb = fsz; ml = mr = 40; break;
    case T_UL: case T_OL: case T_MENU: case T_DIR:
        if (c->list_depth == 0) mt = mb = fsz;
        ml = 40;
        break;
    case T_DD: ml = 40; break;
    case T_FORM: mb = fsz; break;
    case T_FIELDSET: mt = mb = 6; ml = mr = 4; break;
    case T_FIGCAPTION: case T_LEGEND: break;
    case T_HR: {
        line_end(c, -1);
        margin(c, 7);
        c->y += c->margin;
        c->margin = 0;
        int w = px_attr(n, "width", c->lw, c->lw);
        if (w > c->lw) w = c->lw;
        int a = align_attr(n, 1);
        item_t *it = emit(c, IT_HR);
        if (it) {
            it->x = c->lx0 + (a == 1 ? (c->lw - w) / 2 : a == 2 ? c->lw - w : 0);
            it->w = w;
            it->h = 2;
            it->y = c->y;
        }
        c->y += 2;
        margin(c, 7);
        return;
    }
    }
    if (t == T_UL || t == T_OL || t == T_MENU || t == T_DIR) {
        /* the first list keeps its margins; nested ones none */
    }
    line_end(c, -1);
    margin(c, mt);
    const char *id = node_attr(n, "id");
    if (id) add_anchor(c, id);
    int save_x0 = c->lx0, save_w = c->lw, save_align = c->align;
    c->lx0 += ml;
    c->lw -= ml + mr;
    int wa = px_attr(n, "width", save_w, -1);
    if (wa > 0 && wa < c->lw && (t == T_DIV || t == T_CENTER || t == T_FORM)) c->lw = wa;
    if (c->lw < 10) c->lw = 10;
    c->align = align_attr(n, s.align);
    s.align = (uint8_t)c->align;
    /* a background colour: a rectangle behind the block, its height set at the end */
    uint32_t bg;
    char bv[32];
    int bgi = -1;
    if (!c->measure && ((css_prop(n, "background-color", bv, sizeof bv) && parse_color(bv, &bg)) ||
                        (css_prop(n, "background", bv, sizeof bv) && parse_color(bv, &bg)))) {
        c->y += c->margin;
        c->margin = 0;
        item_t *it = emit(c, IT_RECT);
        if (it) { it->x = c->lx0; it->y = c->y; it->w = c->lw; it->color = bg; bgi = (int)(it - c->d->items); }
    }
    int top = c->y;
    if (disp == D_LIST_ITEM) {
        int num = 1;
        if (n->parent && n->parent->tag == T_OL) {
            const char *st = node_attr(n->parent, "start");
            long v0 = st ? atoi(st) : 1;
            num = (int)(v0 < -1000000000L ? -1000000000L : v0 > 1000000000L ? 1000000000L : v0);
            for (node_t *k = n->parent->first; k && k != n; k = k->next) if (!k->is_text && k->tag == T_LI && num < 2000000000) num++;
            const char *v = node_attr(n, "value");
            if (v) { long x = atoi(v); num = (int)(x < -1000000000L ? -1000000000L : x > 1000000000L ? 1000000000L : x); }
        }
        list_marker(c, n, s, num);
    }
    int ld = c->list_depth;
    if (t == T_UL || t == T_OL || t == T_MENU || t == T_DIR) c->list_depth++;
    flow(c, n, s);
    c->list_depth = ld;
    if (c->mk) {
        /* an empty list item still shows its marker */
        line_open(c);
        grow_line(c, fa(fontof(&s)), fh(fontof(&s)) - fa(fontof(&s)));
    }
    line_end(c, -1);
    if (bgi >= 0) {
        c->d->items[bgi].h = c->y - c->d->items[bgi].y;
        if (c->d->items[bgi].h < 0) c->d->items[bgi].h = 0;
    }
    (void)top;
    c->lx0 = save_x0;
    c->lw = save_w;
    c->align = save_align;
    margin(c, mb);
}

/* the content of an element */
static void flow(lc_t *c, node_t *n, ist_t s) {
    c->depth++;
    for (node_t *k = n->first; k; k = k->next) {
        if (k->is_text) { text_run(c, &s, k->text, k->textlen); continue; }
        int disp = display_of(k);
        k->display = (uint8_t)disp;
        if (disp == D_NONE) continue;
        ist_t ks = s;
        apply_style(c->d, k, &ks);
        int t = k->tag;
        if (c->depth > LAYOUT_MAX_DEPTH) {
            /* too deep: the text only */
            if (t != T_IMG && t != T_INPUT) flow(c, k, ks);
            continue;
        }
        const char *nm = t == T_A ? node_attr(k, "name") : 0;
        if (nm) add_anchor(c, nm);
        switch (t) {
        case T_BR: {
            int f = fontof(&ks);
            if (!c->has) { line_open(c); grow_line(c, fa(f), fh(f) - fa(f)); }
            line_end(c, f);
            continue;
        }
        case T_IMG: image_box(c, k, &ks); continue;
        case T_INPUT: case T_SELECT: case T_TEXTAREA: case T_BUTTON: control_box(c, k, &ks); continue;
        case T_IFRAME: case T_FRAME: {
            const char *src = node_attr(k, "src");
            if (src && src[0]) {
                ks.link = k;
                ks.color = c->d->link_color;
                ks.underline = 1;
                if (c->has) { c->space = 1; c->space_w = tw(" ", 1, fontof(&ks)); }
                text_run(c, &ks, "[Frame]", 7);
            }
            continue;
        }
        case T_Q: text_run(c, &ks, "\xe2\x80\x9c", 3); flow(c, k, ks); text_run(c, &ks, "\xe2\x80\x9d", 3); continue;
        }
        if (node_attr(k, "id") && (disp == D_INLINE || disp == D_INLINE_BLOCK)) add_anchor(c, node_attr(k, "id"));
        switch (disp) {
        case D_INLINE: case D_INLINE_BLOCK: flow(c, k, ks); break;
        case D_TABLE:
            if (c->depth + 2 > LAYOUT_MAX_DEPTH) { flow(c, k, ks); break; }
            if (node_attr(k, "id")) add_anchor(c, node_attr(k, "id"));
            {
                /* a caption above the table */
                for (node_t *cap = k->first; cap; cap = cap->next)
                    if (!cap->is_text && cap->tag == T_CAPTION) { ist_t cs2 = ks; cs2.align = 1; block(c, cap, cs2, D_BLOCK); }
                table(c, k, ks);
            }
            break;
        case D_ROW: case D_CELL: case D_ROWGROUP: case D_CAPTION:
            /* table parts outside a table: as blocks */
            block(c, k, ks, D_BLOCK);
            break;
        default: block(c, k, ks, disp); break;
        }
    }
    c->depth--;
}

static void clear_measures(node_t *n) {
    n->min_w = n->max_w = -1;
    for (node_t *k = n->first; k; k = k->next) if (!k->is_text) clear_measures(k);
}

void layout_doc(doc_t *d, int width) {
    if (width < 100) width = 100;
    d->nitems = 0;
    d->nanchors = 0;
    d->content_w = 0;
    d->width = width;
    forms_init(d);
    clear_measures(d->root);
    lc_t c;
    memset(&c, 0, sizeof c);
    c.d = d;
    c.lx0 = 8;
    c.lw = width - 16;
    c.y = 8;
    ist_t s;
    memset(&s, 0, sizeof s);
    s.color = d->fg;
    flow(&c, d->root, s);
    line_end(&c, -1);
    d->height = c.y + c.margin + 8;
}
