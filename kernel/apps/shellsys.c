/* Halcyon OS - system calls of the shell for Explorer (M8): the Recycle Bin
 * (SYS_RECYCLE_*, on apps/recycle.c) and Explorer's own settings
 * (SYS_SHELL_SETTING). Paths are absolute ("C:/..."). SETTINGS.INI is
 * written on the GUI thread, as the rest of the shell writes it. */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/proc.h"
#include "core/thread.h"
#include "core/sysabi.h"
#include "core/settings.h"
#include "gui/wm.h"
#include "recycle.h"

static int user_abs_path(u64 uva, char *out) {
    char tmp[VFS_MAX_PATH];
    int r = copyin_str(tmp, uva, sizeof tmp);
    if (r) return r;
    if (!isalpha(tmp[0]) || tmp[1] != ':') return E_INVAL;
    return vfs_normalize(tmp, out, VFS_MAX_PATH);
}

static unsigned rb_flags(void) {
    return (settings_get_int("recycle.confirm", 1) ? RBF_CONFIRM : 0) | (settings_get_int("recycle.nuke", 0) ? RBF_NUKE : 0);
}

/* changes of SETTINGS.INI: made and saved on the GUI thread */
typedef struct { char key[48]; char val[208]; int remove; volatile int done; int result; } set_job_t;
static void job_setting(void *a) {
    set_job_t *j = a;
    settings_set(j->key, j->remove ? NULL : j->val);
    j->result = settings_save();
    j->done = 1;
}
static int save_setting(const char *key, const char *val) {
    set_job_t *j = kzalloc(sizeof *j);
    if (!j) return E_NOMEM;
    strlcpy(j->key, key, sizeof j->key);
    if (val) strlcpy(j->val, val, sizeof j->val);
    j->remove = val == NULL;
    gui_post(job_setting, j);
    while (!j->done) thread_sleep_ms(5);
    int r = j->result;
    kfree(j);
    return r;
}

static void to_user(const rb_entry_t *e, char drive, hal_rbentry_t *o) {
    memset(o, 0, sizeof *o);
    o->id = e->id;
    o->drive = drive;
    o->is_dir = e->is_dir;
    o->size = e->size;
    o->year = e->when.year; o->month = e->when.month; o->day = e->when.day;
    o->hour = e->when.hour; o->minute = e->when.minute; o->second = e->when.second;
    strlcpy(o->orig, e->orig, sizeof o->orig);
}

static long sys_rb_list(char drive, u64 ubuf, int max) {
    int total = 0;
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (drive && v->letter != drive) continue;
        if (rb_supported(v->letter) == E_ROFS && drive != v->letter) continue;
        rb_entry_t *e;
        int n = rb_list(v->letter, &e);
        if (n < 0) { if (drive) return n; continue; }
        for (int k = 0; k < n; k++, total++) {
            if (total >= max) continue;
            hal_rbentry_t o;
            to_user(&e[k], v->letter, &o);
            if (copyout(ubuf + (u64)total * sizeof o, &o, sizeof o)) { kfree(e); return E_INVAL; }
        }
        kfree(e);
    }
    return max ? MIN(total, max) : total;
}

static long sys_rb_info(char drive, u64 uinfo) {
    hal_rbinfo_t in;
    memset(&in, 0, sizeof in);
    in.drive = drive;
    in.flags = rb_flags();
    in.generation = rb_generation();
    if (drive) {
        in.supported = rb_supported(drive);
        if (in.supported != E_NODEV) {
            u64 total = 0, freeb = 0;
            vfs_statfs(drive, &total, &freeb);
            in.total = total;
            in.percent = rb_percent(drive);
            in.limit = rb_limit(drive);
            u64 bytes = 0;
            if (!in.supported) rb_usage(drive, &in.count, &bytes);
            in.bytes = bytes;
        }
    }
    return copyout(uinfo, &in, sizeof in) ? E_INVAL : 0;
}

long shell_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    (void)a6;
    char path[VFS_MAX_PATH];
    char drive = (char)toupper((int)(a1 & 0x7F));
    if (drive && !isalpha(drive)) drive = 0;
    int r;
    switch (nr) {
    case SYS_RECYCLE_DELETE:
        if ((r = user_abs_path(a1, path))) return r;
        if ((r = rb_supported(path[0]))) return r;
        if (rb_flags() & RBF_NUKE) return E_NOTSUP;
        r = rb_delete(path, 0);
        if (r > 0) kprintf("recycle: %s to the bin of %c: (%d)\n", path, path[0], r);
        return r;
    case SYS_RECYCLE_LIST:
        return sys_rb_list(drive, a2, (int)MIN(a3, 100000ull));
    case SYS_RECYCLE_RESTORE: {
        if (!drive) return E_INVAL;
        char out[VFS_MAX_PATH] = "";
        r = rb_restore(drive, (u32)a2, (int)a3, out, sizeof out);
        if (!r && a4 && a5) {
            u64 n = MIN((u64)strlen(out) + 1, a5);
            char tmp[VFS_MAX_PATH];
            memcpy(tmp, out, n);
            tmp[n - 1] = 0;
            if (copyout(a4, tmp, n)) return E_INVAL;
        }
        if (!r) kprintf("recycle: restored %s\n", out);
        return r;
    }
    case SYS_RECYCLE_PURGE:
        if (a2) return drive ? rb_purge(drive, (u32)a2) : E_INVAL;
        r = 0;
        for (int i = 0; i < vfs_volume_count(); i++) {
            char l = vfs_volume(i)->letter;
            if ((drive && l != drive) || rb_supported(l)) continue;
            int e = rb_empty(l, NULL, NULL);
            if (e && !r) r = e;
        }
        if (drive) kprintf("recycle: emptied the bin of %c:\n", drive);
        else kprintf("recycle: emptied every bin\n");
        return r;
    case SYS_RECYCLE_INFO:
        return sys_rb_info(drive, a2);
    case SYS_RECYCLE_SET: {
        if (!drive) {
            int e = save_setting("recycle.confirm", (a3 & RBF_CONFIRM) ? "1" : "0");
            return e ? e : save_setting("recycle.nuke", (a3 & RBF_NUKE) ? "1" : "0");
        }
        if (a2 > 100 || rb_supported(drive) == E_NODEV) return E_INVAL;
        char key[48], val[8];
        rb_percent_key(drive, key, sizeof key);
        snprintf(val, sizeof val, "%d", (int)a2);
        r = save_setting(key, val);
        if (!r) rb_enforce(drive, rb_limit(drive), 0);
        return r;
    }
    case SYS_SHELL_SETTING: {
        char key[48], val[208];
        if (copyin_str(key, a1, sizeof key) == E_INVAL) return E_INVAL;
        if (strncmp(key, "explorer.", 9) || !key[9]) return E_PERM;
        for (const char *k = key; *k; k++) if (!isalnum(*k) && *k != '.' && *k != '_') return E_INVAL;
        if (a2) {
            if (copyin_str(val, a2, sizeof val) == E_INVAL || strchr(val, '\n') || strchr(val, '\r')) return E_INVAL;
            if (!strcmp(settings_get(key, ""), val) && val[0]) return 0;      /* unchanged: no write */
            return save_setting(key, val[0] ? val : NULL);
        }
        const char *v = settings_get(key, NULL);
        if (!v) return E_NOENT;
        u64 n = MIN((u64)strlen(v) + 1, a4);
        if (a3 && n) {
            memcpy(val, v, n);
            val[n - 1] = 0;
            if (copyout(a3, val, n)) return E_INVAL;
        }
        return (int)strlen(v);
    }
    }
    return E_NOTSUP;
}
