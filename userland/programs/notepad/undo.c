/* NOTEPAD.EXE: multi-level undo and redo. See undo.h. */
#include "undo.h"
#ifdef NP_HOST
#include <stdlib.h>
#include <string.h>
#define E_NOMEM -2
#else
#include "hal.h"
#endif

#define EDIT_OVERHEAD 48
#define STEP_OVERHEAD 96

void undo_init(undo_t *u, size_t limit) {
    memset(u, 0, sizeof *u);
    u->limit = limit;
}

static void step_free(ustep_t *s) {
    if (!s) return;
    for (int i = 0; i < s->nedits; i++) { free(s->e[i].del); free(s->e[i].ins); }
    free(s->e);
    free(s);
}

void undo_clear(undo_t *u) {
    for (int i = 0; i < u->n; i++) step_free(u->s[i]);
    free(u->s);
    u->s = NULL;
    u->n = u->pos = u->cap = 0;
    u->bytes = 0;
    u->open = 0;
    u->saved = 0;
}

void undo_break(undo_t *u) { u->open = 0; }
int  undo_can_undo(const undo_t *u) { return u->pos > 0; }
int  undo_can_redo(const undo_t *u) { return u->pos < u->n; }
void undo_mark_saved(undo_t *u) { u->saved = u->pos; u->open = 0; }
int  undo_is_saved(const undo_t *u) { return u->saved == u->pos; }

/* grow *buf (capacity *cap) to hold need bytes */
static int grow(char **buf, int *cap, int need) {
    if (need <= *cap) return 0;
    int nc = need + need / 2 + 16;
    char *nb = realloc(*buf, (size_t)nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

static int set_edit(uedit_t *e, int a, const char *old, int oldn, const char *ins, int insn) {
    memset(e, 0, sizeof *e);
    e->pos = a;
    if (grow(&e->del, &e->dcap, oldn) || grow(&e->ins, &e->icap, insn)) { free(e->del); free(e->ins); return -1; }
    if (oldn) memcpy(e->del, old, (size_t)oldn);
    if (insn) memcpy(e->ins, ins, (size_t)insn);
    e->dlen = oldn;
    e->ilen = insn;
    return 0;
}

static int add_edit(ustep_t *s, int a, const char *old, int oldn, const char *ins, int insn) {
    if (s->nedits == s->cap) {
        int nc = s->cap ? s->cap * 2 : 2;
        uedit_t *ne = realloc(s->e, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        s->e = ne;
        s->cap = nc;
    }
    if (set_edit(&s->e[s->nedits], a, old, oldn, ins, insn)) return -1;
    s->nedits++;
    s->bytes += (size_t)oldn + (size_t)insn + EDIT_OVERHEAD;
    return 0;
}

static void drop_redo(undo_t *u) {
    if (u->saved > u->pos) u->saved = -1;
    for (int i = u->pos; i < u->n; i++) { u->bytes -= u->s[i]->bytes; step_free(u->s[i]); }
    u->n = u->pos;
}

/* oldest first, keeping the newest step */
static void trim(undo_t *u) {
    int k = 0;
    while (u->bytes > u->limit && u->n - k > 1) { u->bytes -= u->s[k]->bytes; step_free(u->s[k]); k++; }
    if (!k) return;
    memmove(u->s, u->s + k, (size_t)(u->n - k) * sizeof *u->s);
    u->n -= k;
    u->pos -= k;
    u->saved = u->saved >= k ? u->saved - k : -1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n'; }

/* can the new edit be merged into the open step's last edit? */
static int mergeable(const undo_t *u, int kind, int a, int oldn, const char *ins, int insn, uint64_t now) {
    if (!u->open || u->pos == 0 || u->pos != u->n || u->saved == u->pos) return 0;
    const ustep_t *s = u->s[u->pos - 1];
    if (s->kind != kind || s->nedits != 1 || now - s->t >= UNDO_PAUSE_MS) return 0;
    const uedit_t *e = &s->e[0];
    switch (kind) {
    case UNDO_TYPE:
        if (oldn || insn <= 0 || e->pos + e->ilen != a || ins[0] == '\n') return 0;
        /* a new word starts after a space */
        if (e->ilen && is_space(e->ins[e->ilen - 1]) && !is_space(ins[0])) return 0;
        return 1;
    case UNDO_BACKSPACE: return insn == 0 && e->ilen == 0 && a + oldn == e->pos;
    case UNDO_DELETE: return insn == 0 && e->ilen == 0 && a == e->pos;
    }
    return 0;
}

int undo_record(undo_t *u, int kind, int a, const char *old, int oldn, const char *ins, int insn,
                int cur0, int anchor0, int cur1, int anchor1, uint64_t now) {
    int join = kind & UNDO_JOIN;
    kind &= 0xFF;
    ustep_t *s = NULL;
    if (join && u->pos > 0 && u->pos == u->n) {
        s = u->s[u->pos - 1];
        if (add_edit(s, a, old, oldn, ins, insn)) goto fail;
        u->bytes += (size_t)oldn + (size_t)insn + EDIT_OVERHEAD;
    } else if (mergeable(u, kind, a, oldn, ins, insn, now)) {
        s = u->s[u->pos - 1];
        uedit_t *e = &s->e[0];
        if (kind == UNDO_TYPE) {
            if (grow(&e->ins, &e->icap, e->ilen + insn)) goto fail;
            memcpy(e->ins + e->ilen, ins, (size_t)insn);
            e->ilen += insn;
        } else {
            if (grow(&e->del, &e->dcap, e->dlen + oldn)) goto fail;
            if (kind == UNDO_BACKSPACE) {
                memmove(e->del + oldn, e->del, (size_t)e->dlen);
                memcpy(e->del, old, (size_t)oldn);
                e->pos = a;
            } else {
                memcpy(e->del + e->dlen, old, (size_t)oldn);
            }
            e->dlen += oldn;
        }
        s->bytes += (size_t)oldn + (size_t)insn;
        u->bytes += (size_t)oldn + (size_t)insn;
    } else {
        drop_redo(u);
        if (u->n == u->cap) {
            int nc = u->cap ? u->cap * 2 : 64;
            ustep_t **ns = realloc(u->s, (size_t)nc * sizeof *ns);
            if (!ns) goto fail;
            u->s = ns;
            u->cap = nc;
        }
        s = calloc(1, sizeof *s);
        if (!s) goto fail;
        s->kind = kind;
        s->cur0 = cur0;
        s->anchor0 = anchor0;
        s->bytes = STEP_OVERHEAD;
        if (add_edit(s, a, old, oldn, ins, insn)) { step_free(s); goto fail; }
        u->s[u->n++] = s;
        u->pos = u->n;
        u->bytes += s->bytes;
    }
    s->cur1 = cur1;
    s->anchor1 = anchor1;
    s->t = now;
    u->open = kind != UNDO_OTHER;
    trim(u);
    return 0;
fail:
    undo_clear(u);
    u->saved = -1;
    return E_NOMEM;
}

static int apply(const uedit_t *e, int backwards, undo_replace_fn fn, void *ctx) {
    return backwards ? fn(ctx, e->pos, e->pos + e->ilen, e->del, e->dlen)
                     : fn(ctx, e->pos, e->pos + e->dlen, e->ins, e->ilen);
}

int undo_undo(undo_t *u, undo_replace_fn fn, void *ctx, int *cur, int *anchor) {
    if (u->pos == 0) return 0;
    ustep_t *s = u->s[u->pos - 1];
    for (int i = s->nedits - 1; i >= 0; i--) {
        int r = apply(&s->e[i], 1, fn, ctx);
        if (r < 0) {                     /* put back what was undone */
            for (int j = i + 1; j < s->nedits; j++) apply(&s->e[j], 0, fn, ctx);
            return r;
        }
    }
    u->pos--;
    u->open = 0;
    *cur = s->cur0;
    *anchor = s->anchor0;
    return 1;
}

int undo_redo(undo_t *u, undo_replace_fn fn, void *ctx, int *cur, int *anchor) {
    if (u->pos >= u->n) return 0;
    ustep_t *s = u->s[u->pos];
    for (int i = 0; i < s->nedits; i++) {
        int r = apply(&s->e[i], 0, fn, ctx);
        if (r < 0) {
            for (int j = i - 1; j >= 0; j--) apply(&s->e[j], 1, fn, ctx);
            return r;
        }
    }
    u->pos++;
    u->open = 0;
    *cur = s->cur1;
    *anchor = s->anchor1;
    return 1;
}
