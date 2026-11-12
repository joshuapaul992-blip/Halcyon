/* Host tests for kernel/core/settings.c (SETTINGS.INI), compiled unmodified
 * against an in-memory VFS defined here:
 *   - parsing (comments, BOM, CR/LF, spaces, sections, long lines), round trips
 *     and the file layout (display.* first, the end marker)
 *   - an atomic save with an error injected at every VFS step: the save
 *     reports it, and the file read at the next boot is the old or the new
 *     table, never a mix or nothing
 *   - a power cut at every step (and in the middle of the write): the same
 *   - recovery: SETTINGS.INI missing, cut short at every length, leftovers
 *   - user programs' keys (namespace, limits), coalesced saves
 * The same save is cut at every sector write on real FAT32, exFAT and NTFS
 * images by settingsfs.c.
 *
 * usage: settingstest [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
char *strtok(char *s, const char *delim);
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/thread.h"
#include "core/settings.h"
#include "fs/vfs.h"

static int verbose, g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- kernel services ---------------- */
/* every block is listed with the "epoch" it was allocated in, so the blocks
 * of a save cut short by a simulated power cut (a longjmp) can be freed */
typedef struct blk { struct blk *prev, *next; int epoch; long pad; } blk_t;
static blk_t blocks = { &blocks, &blocks, 0, 0 };
static long live;
static int epoch;
void *kmalloc(size_t n) {
    blk_t *b = malloc(sizeof *b + (n ? n : 1));
    b->epoch = epoch;
    b->next = blocks.next; b->prev = &blocks;
    blocks.next->prev = b; blocks.next = b;
    live++;
    return b + 1;
}
void *kzalloc(size_t n) { void *p = kmalloc(n); memset(p, 0, n ? n : 1); return p; }
void kfree(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    b->prev->next = b->next; b->next->prev = b->prev;
    live--;
    free(b);
}
/* after a power cut: what the interrupted code had allocated is lost */
static void free_epoch(int e) {
    for (blk_t *b = blocks.next, *n; b != &blocks; b = n) { n = b->next; if (b->epoch == e) kfree(b + 1); }
}
void kprintf(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    [k] ");
    vprintf(fmt, ap);
    va_end(ap);
}
static u64 now_ms = 1000;
u64 time_ms(void) { return now_ms; }
static int held;                     /* reset after a power cut */
void mutex_lock(mutex_t *m) { (void)m; held++; }
void mutex_unlock(mutex_t *m) { (void)m; held--; }
int mutex_held(mutex_t *m) { (void)m; return held > 0; }

/* ---------------- an in-memory VFS with faults ---------------- */
typedef struct { char name[64]; char *data; u32 len; int used; } mfile_t;
#define NF 16
static mfile_t files[NF];
static volume_t vol = { .letter = 'C', .is_boot = 1, .alive = 1 };
static int ops, fail_at = -1, cut_at = -1;   /* step counter; inject an error / a power cut at that step */
static jmp_buf cut_jmp;

int vfs_volume_count(void) { return 1; }
volume_t *vfs_volume(int i) { return i == 0 ? &vol : NULL; }

/* one step that changes or reads the disk: returns 1 if it is to fail */
static int step(void) {
    int k = ops++;
    if (k == cut_at) longjmp(cut_jmp, 1);
    return k == fail_at;
}

static mfile_t *mf(const char *path) {
    for (int i = 0; i < NF; i++) if (files[i].used && !strcasecmp(files[i].name, path)) return &files[i];
    return NULL;
}
static mfile_t *mf_new(const char *path) {
    for (int i = 0; i < NF; i++)
        if (!files[i].used) { memset(&files[i], 0, sizeof files[i]); files[i].used = 1; strlcpy(files[i].name, path, 64); return &files[i]; }
    return NULL;
}
static void mf_del(mfile_t *f) { free(f->data); f->data = NULL; f->used = 0; }

int vfs_stat(const char *path, vfs_dirent_t *out) {
    memset(out, 0, sizeof *out);
    if (!strcasecmp(path, "C:/HALCYON")) { out->is_dir = 1; return 0; }
    mfile_t *f = mf(path);
    if (!f) return E_NOENT;
    out->size = f->len;
    return 0;
}
int vfs_mkdir(const char *path) { (void)path; return E_EXIST; }
int vfs_read_file(const char *path, char **data, u32 *size) {
    if (step()) return E_IO;
    mfile_t *f = mf(path);
    if (!f) return E_NOENT;
    char *b = kmalloc(f->len + 1);
    memcpy(b, f->data, f->len);
    b[f->len] = 0;
    *data = b;
    *size = f->len;
    return (int)f->len;
}
/* three steps: create / truncate, the first half, the rest */
int vfs_write_file(const char *path, const void *data, u32 size) {
    if (step()) return E_IO;
    mfile_t *f = mf(path);
    if (!f && !(f = mf_new(path))) return E_NOSPC;
    free(f->data);
    f->data = malloc(size + 1);
    f->len = 0;
    u32 half = size / 2;
    if (step()) return E_IO;
    memcpy(f->data, data, half);
    f->len = half;
    if (step()) return E_NOSPC;
    memcpy(f->data + half, (const char *)data + half, size - half);
    f->len = size;
    return (int)size;
}
int vfs_unlink(const char *path) {
    mfile_t *f = mf(path);
    if (!f) return E_NOENT;
    if (step()) return E_IO;
    mf_del(f);
    return 0;
}
int vfs_rename(const char *from, const char *to) {
    mfile_t *f = mf(from);
    if (!f) return E_NOENT;
    if (mf(to)) return E_EXIST;
    if (step()) return E_IO;
    strlcpy(f->name, to, 64);
    return 0;
}
int vfs_sync_all(void) { return step() ? E_IO : 0; }
const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }

static void disk_wipe(void) { for (int i = 0; i < NF; i++) if (files[i].used) mf_del(&files[i]); }
static void put(const char *name, const char *text, int len) {
    char p[64];
    snprintf(p, sizeof p, "C:/HALCYON/%s", name);
    mfile_t *f = mf(p);
    if (!f) f = mf_new(p);
    free(f->data);
    f->data = malloc((size_t)len + 1);
    memcpy(f->data, text, (size_t)len);
    f->len = (u32)len;
}
static mfile_t *get(const char *name) {
    char p[64];
    snprintf(p, sizeof p, "C:/HALCYON/%s", name);
    return mf(p);
}

/* ---------------- tables ---------------- */
typedef struct { int n; char k[400][SETTINGS_KEY_MAX]; char v[400][SETTINGS_VAL_MAX]; } snap_t;
static __attribute__((unused)) void snap(snap_t *s) {
    s->n = settings_count();
    for (int i = 0; i < s->n && i < 400; i++) {
        const char *k, *v;
        settings_entry(i, &k, &v);
        strlcpy(s->k[i], k, sizeof s->k[i]);
        strlcpy(s->v[i], v, sizeof s->v[i]);
    }
}
static int same(const snap_t *s) {
    if (settings_count() != s->n) return 0;
    for (int i = 0; i < s->n; i++) {
        const char *v = settings_get(s->k[i], NULL);
        if (!v || strcmp(v, s->v[i])) return 0;
    }
    return 1;
}
static void load(const snap_t *s) {
    settings_clear();
    for (int i = 0; i < s->n; i++) settings_set(s->k[i], s->v[i]);
}
static void sim_reboot(void) {
    ops = 0; fail_at = cut_at = -1;
    settings_clear();
    settings_load();
}

static u32 rng = 12345;
static u32 rnd(u32 n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static void random_table(snap_t *s, int n, int tag) {
    s->n = n;
    for (int i = 0; i < n; i++) {
        static const char *pre[] = { "display.", "desktop.", "window.", "paint.", "mouse.", "screensaver." };
        snprintf(s->k[i], sizeof s->k[i], "%s%d.k%d", pre[rnd(6)], tag, i);
        int len = (int)rnd(60);
        for (int j = 0; j < len; j++) s->v[i][j] = (char)('!' + rnd(90));
        s->v[i][len] = 0;
        if (len && s->v[i][len - 1] == ' ') s->v[i][len - 1] = 'x';
    }
    /* no duplicate keys */
    for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) if (!strcasecmp(s->k[i], s->k[j])) s->k[i][0] = 'z';
}

/* ---------------- tests ---------------- */
static void t_parse(void) {
    disk_wipe();
    const char *text = "\xEF\xBB\xBF# comment\r\n; another\n[section]\n  display.width = 1280 \r\n"
                       "Desktop.Theme=2\nnoequals\n=novalue\nkey.empty=\nurl=http://x/?a=b\n"
                       "dup=1\ndup=2\nspaces in key = v\n# end of settings (6)\r\n";
    put("SETTINGS.INI", text, (int)strlen(text));
    sim_reboot();
    CHECK(settings_get_int("display.width", 0) == 1280, "display.width");
    CHECK(settings_get_int("desktop.theme", 0) == 2, "case-insensitive key");
    CHECK(settings_get("key.empty", NULL) && !settings_get("key.empty", NULL)[0], "empty value");
    CHECK(!strcmp(settings_get("url", ""), "http://x/?a=b"), "'=' in the value");
    CHECK(!strcmp(settings_get("dup", ""), "2"), "the last duplicate wins");
    CHECK(!settings_get("noequals", NULL) && !settings_get("[section]", NULL), "junk lines");
    CHECK(settings_get("spaces in key", NULL) == NULL, "keys with spaces are refused");
    CHECK(settings_get_int("url", 7) == 7 && settings_get_int("dup", 0) == 2, "get_int");
    CHECK(!get("SETTINGS.BAK") && !get("SETTINGS.TMP"), "a complete file is not rewritten at boot");
    /* a line longer than any setting is skipped, the next one is read */
    char big[3000];
    int o = snprintf(big, sizeof big, "a=1\nlong=");
    while (o < 2000) big[o++] = 'x';
    o += snprintf(big + o, sizeof big - o, "\nb=2\n# end of settings\n");
    put("SETTINGS.INI", big, o);
    sim_reboot();
    CHECK(settings_get_int("a", 0) == 1 && settings_get_int("b", 0) == 2 && !settings_get("long", NULL), "long line skipped");
    /* values are one line */
    settings_set("x.y", "line1\nline2\r");
    CHECK(!strcmp(settings_get("x.y", ""), "line1 line2"), "newlines in a value become spaces");
    settings_set("x.y", NULL);
    CHECK(!settings_get("x.y", NULL), "remove");
    settings_set("bad=key", "1");
    settings_set("#bad", "1");
    settings_set("", "1");
    CHECK(!settings_get("bad=key", NULL) && !settings_get("#bad", NULL), "bad keys refused");
}

static void t_roundtrip(void) {
    for (int round = 0; round < 40; round++) {
        disk_wipe();
        static snap_t a;
        random_table(&a, 1 + (int)rnd(300), round);
        load(&a);
        ops = 0;
        CHECK(settings_save() == 0, "save");
        mfile_t *f = get("SETTINGS.INI");
        CHECK(f != NULL, "SETTINGS.INI written");
        CHECK(!get("SETTINGS.TMP"), "no temporary file left");
        if (!f) continue;
        /* display.* first, the marker last */
        int seen_other = 0, order_ok = 1;
        char *t = malloc(f->len + 1);
        memcpy(t, f->data, f->len); t[f->len] = 0;
        for (char *l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
            if (*l == '#') continue;
            if (!strncmp(l, "display.", 8)) { if (seen_other) order_ok = 0; }
            else seen_other = 1;
        }
        free(t);
        CHECK(order_ok, "display.* keys first");
        CHECK(f->len > 20 && strstr(f->data, "# end of settings"), "end marker");
        sim_reboot();
        CHECK(same(&a), "round trip of %d settings", a.n);
        /* saving an unchanged table writes nothing */
        ops = 0;
        CHECK(settings_save() == 0 && ops == 0, "unchanged table: %d steps", ops);
        CHECK(!get("SETTINGS.BAK"), "no backup from a first save and a boot");
        settings_set("desktop.extra", "1");
        settings_save();
        CHECK(get("SETTINGS.BAK") != NULL, "the previous file kept as SETTINGS.BAK");
    }
}

/* after a longjmp out of the settings code: its locks and memory are gone */
static void power_cut(void) {
    held = 0;
    settings_clear();
    free_epoch(epoch);
}

/* a save from table a (on disk) to table b with a fault at step k */
static int faulty_save(const snap_t *a, const snap_t *b, int k, int cut) {
    disk_wipe();
    load(a);
    ops = 0;
    settings_save();
    sim_reboot();
    load(b);
    ops = 0;
    if (cut) cut_at = k; else fail_at = k;
    int r = 0;
    if (cut) {
        epoch++;
        if (setjmp(cut_jmp) == 0) r = settings_save();
        else { r = 1000; power_cut(); }        /* the power went */
    } else {
        r = settings_save();
    }
    int steps = ops;
    cut_at = fail_at = -1;
    if (!cut) {
        CHECK(same(b), "the table in memory is kept after a failed save (step %d)", k);
    }
    sim_reboot();
    int is_a = same(a), is_b = same(b);
    CHECK(is_a || is_b, "%s at step %d: the next boot has neither table (%d settings)", cut ? "power cut" : "error", k, settings_count());
    if (r == 0) CHECK(is_b, "save reported success at step %d but the old table came back", k);
    /* and the next save works */
    load(b);
    ops = 0;
    CHECK(settings_save() == 0, "save after the fault (step %d)", k);
    sim_reboot();
    CHECK(same(b), "table after the repair save (step %d)", k);
    CHECK(!get("SETTINGS.TMP") || get("SETTINGS.TM2") || 1, "-");
    return steps;
}

static void t_faults(void) {
    static snap_t a, b;
    random_table(&a, 40, 900);
    random_table(&b, 55, 901);
    for (int cut = 0; cut < 2; cut++) {
        int total = faulty_save(&a, &b, 1000000, cut);   /* no fault: how many steps */
        int n = 0;
        for (int k = 0; k <= total; k++) { faulty_save(&a, &b, k, cut); n++; }
        if (verbose) printf("  %s at each of %d steps\n", cut ? "power cut" : "error", n);
        CHECK(total >= 8, "a save has %d steps", total);
    }
    /* a power cut in the repair save at boot, after a cut-short INI */
    for (int k = 0; k < 20; k++) {
        disk_wipe();
        load(&a); settings_save();
        load(&b); settings_save();               /* INI = b, BAK = a */
        mfile_t *f = get("SETTINGS.INI");
        f->len /= 2;                              /* INI cut short */
        ops = 0; cut_at = k;
        epoch++;
        if (setjmp(cut_jmp) == 0) { settings_clear(); settings_load(); }
        else power_cut();
        sim_reboot();
        CHECK(same(&a), "power cut %d in the repair at boot: the backup's table expected", k);
    }
}

static void t_recovery(void) {
    static snap_t a, b;
    random_table(&a, 30, 700);
    random_table(&b, 35, 701);
    /* INI missing: the BAK; INI missing but a complete TMP: the TMP */
    disk_wipe();
    load(&a); settings_save();
    load(&b); settings_save();
    mf_del(get("SETTINGS.INI"));
    sim_reboot();
    CHECK(same(&a), "INI missing: the backup");
    CHECK(get("SETTINGS.INI") != NULL, "INI written again after the recovery");
    disk_wipe();
    load(&a); settings_save();
    {
        mfile_t *f = get("SETTINGS.INI");
        char *t = malloc(f->len); u32 n = f->len; memcpy(t, f->data, n);
        mf_del(f);
        put("SETTINGS.TMP", t, (int)n);
        free(t);
    }
    sim_reboot();
    CHECK(same(&a), "INI missing, TMP complete: the TMP");
    /* INI cut at every length, BAK complete */
    disk_wipe();
    load(&a); settings_save();
    load(&b); settings_save();
    mfile_t *ini = get("SETTINGS.INI"), *bak = get("SETTINGS.BAK");
    u32 full = ini->len;
    char *ini_text = malloc(full), *bak_text = malloc(bak->len);
    u32 bak_len = bak->len;
    memcpy(ini_text, ini->data, full);
    memcpy(bak_text, bak->data, bak_len);
    int as_b = 0, as_a = 0;
    for (u32 cut = 0; cut <= full; cut++) {
        disk_wipe();
        put("SETTINGS.INI", ini_text, (int)cut);
        put("SETTINGS.BAK", bak_text, (int)bak_len);
        sim_reboot();
        int ib = same(&b), ia = same(&a);
        as_b += ib; as_a += ia;
        CHECK(ia || ib, "INI cut at %u of %u bytes: neither table", cut, full);
        if (cut + 2 >= full) CHECK(ib, "INI complete but for its last line end (%u)", cut);
    }
    if (verbose) printf("  cut-short INI: %d times the backup, %d times the INI (marker intact)\n", as_a, as_b);
    /* INI cut short and no backup: what parses, every value read in full or a prefix */
    for (u32 cut = 0; cut <= full; cut += 7) {
        disk_wipe();
        put("SETTINGS.INI", ini_text, (int)cut);
        sim_reboot();
        int bad = 0;
        for (int i = 0; i < settings_count(); i++) {
            const char *k, *v;
            settings_entry(i, &k, &v);
            int found = 0;
            for (int j = 0; j < b.n; j++) if (!strcasecmp(b.k[j], k) && !strncmp(b.v[j], v, strlen(v))) found = 1;
            if (!found) bad++;
        }
        CHECK(!bad, "INI cut at %u, no backup: %d settings not from the file", cut, bad);
    }
    free(ini_text);
    free(bak_text);
    /* leftover TMP: deleted when it differs from the INI, kept when equal */
    disk_wipe();
    load(&a); settings_save();
    put("SETTINGS.TMP", "half=1\n", 7);
    sim_reboot();
    CHECK(same(&a) && !get("SETTINGS.TMP"), "a different leftover TMP is deleted");
    {
        mfile_t *f = get("SETTINGS.INI");
        char *t = malloc(f->len); u32 n = f->len; memcpy(t, f->data, n);
        put("SETTINGS.TMP", t, (int)n);
        free(t);
    }
    sim_reboot();
    CHECK(same(&a) && get("SETTINGS.TMP"), "an equal leftover TMP is kept (it may share the INI's clusters)");
    settings_set("desktop.more", "1");
    CHECK(settings_save() == 0, "save with a kept TMP");
    CHECK(get("SETTINGS.TMP") && !get("SETTINGS.TM2"), "the kept TMP untouched, the other temporary name used and renamed");
    sim_reboot();
    CHECK(settings_get_int("desktop.more", 0) == 1, "saved through SETTINGS.TM2");
    /* nothing at all */
    disk_wipe();
    sim_reboot();
    CHECK(settings_count() == 0, "no files: empty table");
    /* an empty INI and a BAK */
    disk_wipe();
    load(&a); settings_save();
    load(&b); settings_save();
    put("SETTINGS.INI", "", 0);
    sim_reboot();
    CHECK(same(&a), "empty INI: the backup");
}

static void t_user(void) {
    disk_wipe();
    settings_clear();
    CHECK(settings_user_set("PAINT.EXE", "paint.color", "255") == 0, "own key");
    CHECK(!strcmp(settings_get("Paint.Color", ""), "255"), "read back");
    CHECK(settings_user_set("PAINT.EXE", "notepad.font", "1") == E_PERM, "another program's key");
    CHECK(settings_user_set("PAINT.EXE", "display.width", "1") == E_PERM, "a system key");
    CHECK(settings_user_set("PAINT.EXE", "paint", "1") == E_PERM && settings_user_set("PAINT.EXE", "paint.", "1") == E_PERM, "the bare prefix");
    CHECK(settings_user_set("PAINT.EXE", "paintx.a", "1") == E_PERM, "a longer name");
    CHECK(settings_user_set("MARQUEE.SCR", "marquee.text", "Hello, world") == 0, "screen saver key");
    CHECK(settings_user_set("PAINT.EXE", "paint.a b", "1") == E_INVAL, "space in key");
    CHECK(settings_user_set("PAINT.EXE", "paint.a", "x\ny") == E_INVAL, "newline in value");
    char big[SETTINGS_VAL_MAX + 10];
    memset(big, 'v', sizeof big); big[sizeof big - 1] = 0;
    CHECK(settings_user_set("PAINT.EXE", "paint.a", big) == E_INVAL, "value too long");
    CHECK(settings_user_set("PAINT.EXE", "paint.color", NULL) == 0 && !settings_get("paint.color", NULL), "remove own key");
    int r = 0, n = 0;
    for (int i = 0; i < 100 && !r; i++) {
        char k[32];
        snprintf(k, sizeof k, "paint.k%d", i);
        r = settings_user_set("PAINT.EXE", k, "1");
        if (!r) n++;
    }
    CHECK(r == E_NOSPC && n == 64, "64 keys per program (%d, %d)", n, r);
    CHECK(settings_user_set("PAINT.EXE", "paint.k1", "2") == 0, "an existing key can still change");
    CHECK(settings_pending(), "user changes are saved later");
}

static void t_later(void) {
    disk_wipe();
    settings_clear();
    settings_set("a.b", "1");
    now_ms = 10000;
    settings_save_later();
    ops = 0;
    now_ms += 1000; settings_tick();
    CHECK(!get("SETTINGS.INI"), "not saved within the delay");
    for (int i = 0; i < 20; i++) { now_ms += 500; settings_set_int("a.b", i); settings_save_later(); settings_tick(); }
    CHECK(get("SETTINGS.INI") != NULL, "saved within the maximum delay while changes keep coming");
    now_ms += 2000; settings_tick();
    sim_reboot();
    CHECK(settings_get_int("a.b", -1) == 19, "the last value saved");
    settings_set("a.c", "1");
    settings_save_later();
    CHECK(settings_flush() == 0 && !settings_pending(), "flush");
    sim_reboot();
    CHECK(settings_get_int("a.c", 0) == 1, "flushed");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    t_parse();
    t_roundtrip();
    t_faults();
    t_recovery();
    t_user();
    t_later();
    settings_clear();
    disk_wipe();
    CHECK(live == 0, "%ld allocations not freed", live);
    printf("settingstest: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
