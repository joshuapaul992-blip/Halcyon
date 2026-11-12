/* NOTEPAD.EXE: the text and its display rows (doc.c). Also compiled for the
 * host by tests/host (notepadtest.c, with -DNP_HOST).
 *
 * The text is a gap buffer: [0, gs) gap [ge, tcap). The display rows are a
 * second gap array of row starts: entries before its gap hold their offset,
 * entries after it their distance from the end of the text, so an edit
 * renumbers nothing after it. A row that starts a line of the file (offset
 * 0, or just after a newline) is "hard"; one that continues the row before
 * is SOFT. Rows break
 *   - without word wrap: every LINE_BYTES bytes (as Windows Notepad breaks
 *     lines past 1024 characters), which bounds the work per row;
 *   - with word wrap: at the wrap width in pixels, after the last space or
 *     tab that fits (spaces at the end of a row may hang past the edge);
 *     a word longer than the row is broken between characters; and never
 *     more than LINE_BYTES bytes.
 * An edit rebuilds only the rows of the lines it touches. Changing the wrap
 * width marks the whole text stale; doc_rewrap() rebuilds it a slice at a
 * time (the visible part first), so a large file stays responsive while
 * stale rows below or above the view still show the old breaks. */
#ifndef NP_DOC_H
#define NP_DOC_H

#ifdef NP_HOST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define E_NOMEM -2
unsigned utf8_get(const char *s, int len, int *pos);
int utf8_prev(const char *s, int pos);
#else
#include "hal.h"
#endif

#define MAX_TEXT (16 * 1024 * 1024)     /* bytes (the kernel's Notepad: 4 MiB) */
#ifndef LINE_BYTES
#define LINE_BYTES 2048                 /* bytes per row at most */
#endif
#define SOFT 0x40000000                 /* row entry flag: continues the row before (no newline) */
#define OFFM 0x3FFFFFFF
#define E_TOOBIG (-1000)
#define NP_STALE 8                      /* stale ranges at most */

typedef struct {
    char *tb;                           /* text: [0, gs) gap [ge, tcap) */
    int tcap, gs, ge;
    int *la;                            /* row starts: [0, lg) gap [lge, lcap) */
    int lcap, lg, lge, nsoft;
    int wrap;                           /* wrap width in pixels; 0 = no word wrap */
    int tabw;                           /* tab stop distance in pixels (> 0) */
    int (*cwidth)(unsigned cp);         /* advance width of a character (not tab) */
    int pa[NP_STALE], pb[NP_STALE], npend;  /* stale rows: [pa, pb) (pa a row start, pb a line start or the end), first first */
    int lc_off, lc_ln;                  /* cache: the line starting at lc_off is line lc_ln (0-based) */
    unsigned gen;                       /* changes with every edit */
} npdoc_t;
extern npdoc_t D;

static inline int tlen(void) { return D.tcap - (D.ge - D.gs); }
static inline unsigned char at(int p) { return (unsigned char)D.tb[p < D.gs ? p : p + (D.ge - D.gs)]; }
static inline int nlines(void) { return D.lcap - (D.lge - D.lg); }
static inline int lraw(int i) { return i < D.lg ? D.la[i] : D.la[i + (D.lge - D.lg)]; }
static inline int ls(int i) { return i < D.lg ? (D.la[i] & OFFM) : tlen() - (D.la[i + (D.lge - D.lg)] & OFFM); }
static inline int lsoft(int i) { return lraw(i) & SOFT; }

int  doc_init(void);                    /* 0 or E_NOMEM; set cwidth and tabw first */
void doc_reset(void);                   /* empty text (keeps wrap) */
/* Replace [a, b) with n bytes of s (which must not point into the text):
 * 0, E_TOOBIG (past MAX_TEXT) or E_NOMEM (nothing changed then) */
int  doc_replace(int a, int b, const char *s, int n);
void text_get(int a, int n, char *out);  /* n bytes from a into out */
const char *text_span(int a, int b);     /* [a, b) as one piece of memory (moves the gap out of it) */
void move_gap(int p);
int  next_ch(int p);
int  prev_ch(int p);
int  line_of(int off);                   /* the row holding offset off (a row start belongs to its row) */
int  line_end(int l);                    /* end of row l's text (before its newline) */
int  hard_newline_after(int l);          /* row l ends with a newline */
int  logical_line(int row);              /* the line of the file (0-based) row belongs to */
int  logical_start(int row);             /* the first row of that line */

/* the width of the row starting at s (n bytes, no newline), tab stops from x 0 */
int  row_width(const char *s, int n);
/* the length in bytes of the row starting at s, n bytes to the end of its line
 * (no newline in them), wrapping at width (0: none); at least one character */
int  row_break(const char *s, int n, int width);

/* word wrap at width pixels (0 = off); stale rows are rebuilt from the line
 * holding offset from first, then the rest (doc_rewrap) */
void doc_set_wrap(int width, int from);
int  doc_rewrap(int budget);             /* rebuild up to about budget bytes of stale rows; 1 if more remain */
int  doc_stale_at(void);                 /* where the next doc_rewrap starts, -1 if nothing is stale */
void doc_rewrap_prefer(int from);        /* rebuild from the line holding offset from next (the view moved) */

#endif
