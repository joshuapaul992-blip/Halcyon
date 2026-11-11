/* Halcyon OS - persistent settings (C:\HALCYON\SETTINGS.INI) */
#include "settings.h"
#include "lib.h"
#include "mm.h"
#include "kernel.h"
#include "fs/vfs.h"

#define MAX_SETTINGS 128
#define KEY_MAX 48
#define VAL_MAX 208

typedef struct { char key[KEY_MAX]; char val[VAL_MAX]; } setting_t;
static setting_t tab[MAX_SETTINGS];
static int ntab;

static int boot_path(char *out, int n) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->is_boot) { snprintf(out, (size_t)n, "%c:/HALCYON/SETTINGS.INI", v->letter); return 0; }
    }
    return E_NODEV;
}

static setting_t *find(const char *key) {
    for (int i = 0; i < ntab; i++) if (!strcasecmp(tab[i].key, key)) return &tab[i];
    return NULL;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

void settings_load(void) {
    char path[32];
    if (boot_path(path, sizeof path)) return;
    char *data;
    u32 size;
    if (vfs_read_file(path, &data, &size) < 0) { kprintf("settings: none saved yet\n"); return; }
    ntab = 0;
    char *s = data;
    if ((u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s += 3;   /* UTF-8 BOM */
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        char *line = trim(s);
        char *eq = strchr(line, '=');
        if (*line && *line != '#' && *line != ';' && *line != '[' && eq) {
            *eq = 0;
            settings_set(trim(line), trim(eq + 1));
        }
        if (!nl) break;
        s = nl + 1;
    }
    kfree(data);
    kprintf("settings: %d loaded from %s\n", ntab, path);
}

const char *settings_get(const char *key, const char *def) {
    setting_t *s = find(key);
    return s ? s->val : def;
}

int settings_get_int(const char *key, int def) {
    setting_t *s = find(key);
    if (!s || !s->val[0]) return def;
    char *end;
    long v = strtol(s->val, &end, 0);
    return *end ? def : (int)v;
}

void settings_set(const char *key, const char *value) {
    setting_t *s = find(key);
    if (!value) {
        if (s) { *s = tab[--ntab]; }
        return;
    }
    if (!s) {
        if (ntab >= MAX_SETTINGS || strlen(key) >= KEY_MAX) return;
        s = &tab[ntab++];
        strlcpy(s->key, key, sizeof s->key);
    }
    strlcpy(s->val, value, sizeof s->val);
}

void settings_set_int(const char *key, int value) {
    char b[16];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}

int settings_save(void) {
    char path[32];
    int r = boot_path(path, sizeof path);
    if (r) return r;
    char dir[16];
    snprintf(dir, sizeof dir, "%.2s/HALCYON", path);
    vfs_mkdir(dir);
    size_t cap = 128 + (size_t)ntab * (KEY_MAX + VAL_MAX + 4);
    char *out = kmalloc(cap);
    if (!out) return E_NOMEM;
    size_t o = (size_t)snprintf(out, cap, "# Halcyon settings (written by the system; edit with care)\r\n");
    for (int i = 0; i < ntab; i++) o += (size_t)snprintf(out + o, cap - o, "%s=%s\r\n", tab[i].key, tab[i].val);
    r = vfs_write_file(path, out, (u32)o);
    kfree(out);
    if (r >= 0) { vfs_sync_all(); r = 0; }
    return r;
}
