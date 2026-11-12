/* Halcyon OS - persistent settings (C:\HALCYON\SETTINGS.INI)
 *
 * The table lives in memory; settings_save() writes all of it. A save never
 * overwrites the file in place:
 *   1. SETTINGS.TMP is written, the volume flushed and the file read back
 *   2. SETTINGS.BAK is deleted, SETTINGS.INI renamed to SETTINGS.BAK
 *   3. SETTINGS.TMP renamed to SETTINGS.INI, the volume flushed
 * so a power cut leaves at least one complete copy. The last line of every
 * copy is END_MARK; settings_load() takes the first complete one of INI,
 * TMP, BAK (then whatever parses of a cut-short INI or BAK) and, if that was
 * not SETTINGS.INI, saves again to put things right.
 *
 * Callers on any thread may get / set (the big kernel lock keeps the table
 * consistent between blocking points); saves are serialised by save_lock,
 * and the text is built before any file is touched. */
#include "settings.h"
#include "lib.h"
#include "mm.h"
#include "kernel.h"
#include "thread.h"
#include "fs/vfs.h"

#define MAX_SETTINGS 1024
#define USER_MAX_KEYS 64             /* keys one program may hold */
#define USER_TABLE_LIMIT 768         /* user programs cannot fill the last part of the table */
#define FILE_MAX (1u << 20)
#define SAVE_DELAY_MS 1500           /* settings_save_later(): quiet time before writing */
#define SAVE_DELAY_MAX_MS 8000       /* ... but at most this long after the first change */

static const char END_MARK[] = "# end of settings";

typedef struct { char key[SETTINGS_KEY_MAX]; char val[SETTINGS_VAL_MAX]; } setting_t;
static setting_t *tab[MAX_SETTINGS];
static int ntab;

static mutex_t save_lock;
static u32 gen, saved_gen;           /* changes made / written */
static char *last_text;              /* what the last save wrote (or load read): unchanged tables are not rewritten */
static u32 last_len;
static int pending;                  /* settings_save_later() */
static u64 pending_since, pending_due;
static int tmp_stale;                /* a SETTINGS.TMP equal to the INI was left at boot: use SETTINGS.TM2 */

/* ---------------- paths ---------------- */
static int base_dir(char *out, int n) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->is_boot) { snprintf(out, (size_t)n, "%c:/HALCYON", v->letter); return 0; }
    }
    return E_NODEV;
}
static void path_of(const char *dir, const char *name, char *out, int n) { snprintf(out, (size_t)n, "%s/%s", dir, name); }

/* ---------------- the table ---------------- */
static int find_i(const char *key) {
    for (int i = 0; i < ntab; i++) if (!strcasecmp(tab[i]->key, key)) return i;
    return -1;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

const char *settings_get(const char *key, const char *def) {
    int i = find_i(key);
    return i >= 0 ? tab[i]->val : def;
}

int settings_get_int(const char *key, int def) {
    int i = find_i(key);
    if (i < 0 || !tab[i]->val[0]) return def;
    char *end;
    long v = strtol(tab[i]->val, &end, 0);
    return *end ? def : (int)v;
}

static int key_ok(const char *key) {
    size_t n = strlen(key);
    if (!n || n >= SETTINGS_KEY_MAX) return 0;
    if (key[0] == '#' || key[0] == ';' || key[0] == '[') return 0;
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)key[i];
        if (c < 0x21 || c == '=' || c == 0x7F) return 0;
    }
    return 1;
}

void settings_set(const char *key, const char *value) {
    if (!key || !key_ok(key)) return;
    int i = find_i(key);
    if (!value) {
        if (i >= 0) {
            kfree(tab[i]);
            for (int j = i; j < ntab - 1; j++) tab[j] = tab[j + 1];
            ntab--;
            gen++;
        }
        return;
    }
    char v[SETTINGS_VAL_MAX];
    strlcpy(v, value, sizeof v);
    for (char *c = v; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';   /* one line per setting */
    char *t = trim(v);
    if (i < 0) {
        if (ntab >= MAX_SETTINGS) return;
        setting_t *s = kmalloc(sizeof *s);
        if (!s) return;
        strlcpy(s->key, key, sizeof s->key);
        s->val[0] = 0;
        tab[ntab] = s;
        i = ntab++;
    } else if (!strcmp(tab[i]->val, t)) {
        return;
    }
    strlcpy(tab[i]->val, t, sizeof tab[i]->val);
    gen++;
}

void settings_set_int(const char *key, int value) {
    char b[16];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}

int settings_count(void) { return ntab; }
int settings_entry(int i, const char **key, const char **value) {
    if (i < 0 || i >= ntab) return 0;
    *key = tab[i]->key;
    *value = tab[i]->val;
    return 1;
}
void settings_clear(void) {
    for (int i = 0; i < ntab; i++) kfree(tab[i]);
    ntab = 0;
    gen = saved_gen = 0;
    kfree(last_text);
    last_text = NULL;
    last_len = 0;
    pending = 0;
    tmp_stale = 0;
}

/* ---------------- user programs ---------------- */
void settings_prog_prefix(const char *prog, char *out, int n) {
    int o = 0;
    for (const char *c = prog; *c && *c != '.' && o < n - 1; c++) out[o++] = (char)tolower((u8)*c);
    out[o] = 0;
}

int settings_user_set(const char *prog, const char *key, const char *value) {
    char pre[40];
    settings_prog_prefix(prog, pre, sizeof pre);
    size_t pl = strlen(pre);
    if (!pl || strncasecmp(key, pre, pl) || key[pl] != '.' || !key[pl + 1]) return E_PERM;
    for (const char *c = key; *c; c++)
        if (!isalnum((u8)*c) && *c != '.' && *c != '-' && *c != '_') return E_INVAL;
    if (!key_ok(key)) return E_INVAL;
    if (value) {
        if (strlen(value) >= SETTINGS_VAL_MAX) return E_INVAL;
        for (const char *c = value; *c; c++) if ((u8)*c < 0x20 && *c != '\t') return E_INVAL;
        if (find_i(key) < 0) {
            int mine = 0;
            for (int i = 0; i < ntab; i++) if (!strncasecmp(tab[i]->key, pre, pl) && tab[i]->key[pl] == '.') mine++;
            if (mine >= USER_MAX_KEYS || ntab >= USER_TABLE_LIMIT) return E_NOSPC;
        }
    }
    settings_set(key, value);
    settings_save_later();
    return 0;
}

/* ---------------- the file's text ---------------- */
static int is_display(const char *k) { return !strncasecmp(k, "display.", 8); }
static int before(const setting_t *a, const setting_t *b) {
    int da = is_display(a->key), db = is_display(b->key);
    if (da != db) return da;               /* display.* first: the loaders read only the first 8 KiB */
    return strcasecmp(a->key, b->key) < 0;
}

/* the whole file; *len its length (kfree it) */
static char *serialize(u32 *len) {
    size_t cap = 160 + (size_t)ntab * (SETTINGS_KEY_MAX + SETTINGS_VAL_MAX + 3);
    char *out = kmalloc(cap);
    setting_t **order = kmalloc(sizeof(setting_t *) * (size_t)(ntab + 1));
    if (!out || !order) { kfree(out); kfree(order); return NULL; }
    for (int i = 0; i < ntab; i++) {       /* insertion sort: a few hundred entries at most */
        int j = i;
        while (j > 0 && before(tab[i], order[j - 1])) { order[j] = order[j - 1]; j--; }
        order[j] = tab[i];
    }
    size_t o = (size_t)snprintf(out, cap, "# Halcyon settings (written by the system; edit with care,\r\n"
                                          "# and keep the last line: it shows the file is complete)\r\n");
    for (int i = 0; i < ntab; i++) o += (size_t)snprintf(out + o, cap - o, "%s=%s\r\n", order[i]->key, order[i]->val);
    o += (size_t)snprintf(out + o, cap - o, "%s (%d)\r\n", END_MARK, ntab);
    kfree(order);
    *len = (u32)o;
    return out;
}

/* does the text end with the marker line? */
static int complete(const char *d, u32 n) {
    while (n && (d[n - 1] == '\n' || d[n - 1] == '\r' || d[n - 1] == ' ')) n--;
    u32 ls = n;
    while (ls && d[ls - 1] != '\n') ls--;
    u32 ml = sizeof END_MARK - 1;
    return n - ls >= ml && !memcmp(d + ls, END_MARK, ml);
}

/* parse into the table (which is cleared first); returns the entries read */
static int parse(const char *data, u32 size) {
    for (int i = 0; i < ntab; i++) kfree(tab[i]);
    ntab = 0;
    const char *s = data, *end = data + size;
    if (size >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s += 3;   /* UTF-8 BOM */
    static char line_buf[SETTINGS_KEY_MAX + SETTINGS_VAL_MAX + 8];
    while (s < end) {
        const char *nl = s;
        while (nl < end && *nl != '\n') nl++;
        size_t ll = (size_t)(nl - s);
        if (ll < sizeof line_buf) {          /* longer lines cannot be ours: skipped */
            memcpy(line_buf, s, ll);
            line_buf[ll] = 0;
            char *line = trim(line_buf);
            char *eq = strchr(line, '=');
            if (*line && *line != '#' && *line != ';' && *line != '[' && eq) {
                *eq = 0;
                settings_set(trim(line), trim(eq + 1));
            }
        }
        s = nl + 1;
    }
    return ntab;
}

/* ---------------- files ---------------- */
static int read_copy(const char *path, char **data, u32 *size) {
    vfs_dirent_t st;
    int r = vfs_stat(path, &st);
    if (r) return r;
    if (st.is_dir || st.size > FILE_MAX) return E_INVAL;
    return vfs_read_file(path, data, size);
}

static int same_text(const char *path, const char *text, u32 len) {
    char *d;
    u32 n;
    if (read_copy(path, &d, &n) < 0) return 0;
    int same = n == len && !memcmp(d, text, len);
    kfree(d);
    return same;
}

static void remember(char *text, u32 len) {
    kfree(last_text);
    last_text = text;
    last_len = len;
}

/* the steps of an atomic save; returns 0 or E_* (SETTINGS.INI is then as
 * before, or, if only the final rename failed, put back from the BAK) */
static int write_atomic(const char *dir, const char *text, u32 len) {
    char ini[64], tmp[64], bak[64];
    path_of(dir, "SETTINGS.INI", ini, sizeof ini);
    path_of(dir, tmp_stale ? "SETTINGS.TM2" : "SETTINGS.TMP", tmp, sizeof tmp);
    path_of(dir, "SETTINGS.BAK", bak, sizeof bak);
    vfs_dirent_t st;
    int r;
    if (vfs_stat(dir, &st) != 0) vfs_mkdir(dir);
    r = vfs_unlink(tmp);                                  /* a leftover of an interrupted save */
    if (r && r != E_NOENT) return r;
    r = vfs_write_file(tmp, text, len);
    if (r < 0) { vfs_unlink(tmp); return r; }
    r = vfs_sync_all();
    if (r < 0) { vfs_unlink(tmp); return r; }
    if (!same_text(tmp, text, len)) { vfs_unlink(tmp); return E_IO; }   /* read back */
    int have_ini = vfs_stat(ini, &st) == 0;
    if (have_ini) {
        r = vfs_unlink(bak);
        if (r && r != E_NOENT) { vfs_unlink(tmp); return r; }
        r = vfs_rename(ini, bak);
        if (r) { vfs_unlink(tmp); return r; }
    }
    r = vfs_rename(tmp, ini);
    if (r) {
        if (have_ini && vfs_rename(bak, ini) == 0) vfs_unlink(tmp);
        vfs_sync_all();
        return r;
    }
    r = vfs_sync_all();
    return r < 0 ? r : 0;
}

static int save_locked(void) {
    char dir[32];
    int r = base_dir(dir, sizeof dir);
    if (r) return r;
    u32 len, g = gen;
    char *text = serialize(&len);
    if (!text) return E_NOMEM;
    if (last_text && len == last_len && !memcmp(text, last_text, len)) {   /* nothing new */
        kfree(text);
        saved_gen = g;
        return 0;
    }
    r = write_atomic(dir, text, len);
    if (r) { kfree(text); return r; }
    remember(text, len);
    saved_gen = g;
    return 0;
}

int settings_save(void) {
    mutex_lock(&save_lock);
    int r = save_locked();
    if (!r && saved_gen == gen) pending = 0;
    mutex_unlock(&save_lock);
    if (r) kprintf("settings: save failed: %s\n", strerror(r));
    return r;
}

void settings_save_later(void) {
    u64 now = time_ms();
    if (!pending) { pending = 1; pending_since = now; }
    pending_due = MIN(now + SAVE_DELAY_MS, pending_since + SAVE_DELAY_MAX_MS);
}

int settings_pending(void) { return pending || gen != saved_gen; }

void settings_tick(void) {
    if (!pending || time_ms() < pending_due) return;
    if (mutex_held(&save_lock)) return;               /* a save is under way on this thread */
    if (settings_save()) {                            /* try again in a while */
        pending = 1;
        pending_since = time_ms();
        pending_due = pending_since + 30000;
    }
}

int settings_flush(void) {
    if (!pending && gen == saved_gen) return 0;
    return settings_save();
}

/* ---------------- loading and recovery ---------------- */
void settings_load(void) {
    char dir[32];
    if (base_dir(dir, sizeof dir)) return;
    static const char *const names[3] = { "SETTINGS.INI", "SETTINGS.TMP", "SETTINGS.BAK" };
    char *data[3] = { 0 };
    u32 size[3] = { 0 };
    int ok[3] = { 0 }, have[3] = { 0 };
    for (int i = 0; i < 3; i++) {
        char p[64];
        path_of(dir, names[i], p, sizeof p);
        if (read_copy(p, &data[i], &size[i]) >= 0) { have[i] = 1; ok[i] = complete(data[i], size[i]); }
    }
    mutex_lock(&save_lock);
    settings_clear();
    /* the first complete copy; else what parses of a cut-short INI or BAK */
    int pick = ok[0] ? 0 : ok[1] ? 1 : ok[2] ? 2 : have[0] && size[0] ? 0 : have[2] && size[2] ? 2 : -1;
    int repair = 0;
    if (pick >= 0) {
        parse(data[pick], size[pick]);
        if (pick != 0 || !ok[0]) {
            kprintf("settings: %s %s; recovered from %s\n", names[0], !have[0] ? "missing" : "incomplete", names[pick]);
            repair = 1;
        } else {
            remember(data[0], size[0]);       /* an unchanged table is not written again */
            data[0] = NULL;
        }
        kprintf("settings: %d loaded from %s/%s\n", ntab, dir, names[pick]);
    } else {
        kprintf("settings: none saved yet\n");
    }
    /* a SETTINGS.TMP next to a complete INI: an interrupted save. If it holds
     * the same bytes as the INI it may be the INI's own clusters under a
     * second name (a rename cut short on FAT): leave it alone so they are
     * never freed, and write new copies under another name. */
    if (have[1] && ok[0]) {
        if (last_text && size[1] == last_len && !memcmp(data[1], last_text, last_len)) {
            tmp_stale = 1;
            kprintf("settings: SETTINGS.TMP left as it is (the same data as SETTINGS.INI)\n");
        } else {
            char p[64];
            path_of(dir, names[1], p, sizeof p);
            vfs_unlink(p);
        }
    }
    gen = saved_gen = 0;
    if (repair) {
        gen = 1;
        int r = save_locked();
        if (r) { pending = 1; pending_since = pending_due = time_ms(); }
    }
    mutex_unlock(&save_lock);
    for (int i = 0; i < 3; i++) kfree(data[i]);
}
