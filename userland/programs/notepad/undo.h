/* NOTEPAD.EXE: multi-level undo and redo (undo.c). Also compiled for the
 * host by tests/host (notepadtest.c, with -DNP_HOST).
 *
 * The history is a list of steps; each step is one or more edits
 * ("replace [pos, pos + deleted) with inserted") and the selection before
 * and after it. Typing is grouped into steps: a run of typed characters is
 * one step until the caret is moved (undo_break), the user pauses
 * (UNDO_PAUSE_MS), a new word starts (a character other than a space after
 * a space), a newline is typed, or a different kind of edit comes. Runs of
 * Backspace and of Delete are grouped the same way. Anything else (cut,
 * paste, deleting a selection, Time/Date, a drop, replacing) is one step of
 * its own; UNDO_JOIN adds an edit to the step before (Replace All).
 *
 * Memory: the text kept by all steps is limited (limit bytes, 4 MiB in
 * Notepad): the oldest steps are dropped first, but the newest step is
 * always kept, so even an edit of a whole large file can be undone once.
 * The step that matches the saved file is remembered, so undoing back to it
 * makes the document clean again. */
#ifndef NP_UNDO_H
#define NP_UNDO_H
#include <stddef.h>
#include <stdint.h>

enum { UNDO_TYPE = 1, UNDO_BACKSPACE, UNDO_DELETE, UNDO_OTHER };
#define UNDO_JOIN     0x100              /* flag: part of the step before (same kind of edit) */
#define UNDO_PAUSE_MS 1500

typedef struct { int pos, dlen, ilen, dcap, icap; char *del, *ins; } uedit_t;   /* deleted, inserted text */
typedef struct {
    int kind;
    int cur0, anchor0, cur1, anchor1;    /* selection before and after */
    uint64_t t;                          /* time of the last edit */
    int nedits, cap;
    uedit_t *e;
    size_t bytes;
} ustep_t;
typedef struct {
    ustep_t **s;
    int n, pos, cap;                     /* steps [0, pos) can be undone, [pos, n) redone */
    long saved;                          /* pos of the saved state, -1 = not in the history */
    int open;                            /* the last step may take more typing */
    size_t bytes, limit;
} undo_t;

/* the text the history is applied to: replace [a, b) with s (n bytes); 0 or < 0 */
typedef int (*undo_replace_fn)(void *ctx, int a, int b, const char *s, int n);

void undo_init(undo_t *u, size_t limit);
void undo_clear(undo_t *u);              /* forget everything; the current text is the saved one */
/* an edit about to be made: [a, a + oldn) (old) becomes ins (insn bytes);
 * selection before and after; now in ms. 0, or E_NOMEM (then the history
 * is cleared, and the saved state is lost) */
int  undo_record(undo_t *u, int kind, int a, const char *old, int oldn, const char *ins, int insn,
                 int cur0, int anchor0, int cur1, int anchor1, uint64_t now);
void undo_break(undo_t *u);              /* the next edit starts a new step */
int  undo_can_undo(const undo_t *u);
int  undo_can_redo(const undo_t *u);
/* undo / redo one step through fn; the selection to restore in *cur, *anchor.
 * 1 done, 0 nothing to do, < 0 fn's error (the step stays where it was) */
int  undo_undo(undo_t *u, undo_replace_fn fn, void *ctx, int *cur, int *anchor);
int  undo_redo(undo_t *u, undo_replace_fn fn, void *ctx, int *cur, int *anchor);
void undo_mark_saved(undo_t *u);
int  undo_is_saved(const undo_t *u);     /* the text is the saved one */

#endif
