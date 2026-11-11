/* Halcyon OS - system calls: processes, memory, console, files (see sysabi.h).
 * Window and drawing calls are in gui/usergui.c. Every pointer from user
 * mode goes through copyin / copyout, which check the process's page tables. */
#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "irq.h"
#include "thread.h"
#include "proc.h"
#include "sysabi.h"
#include "fs/vfs.h"
#include "fs/block.h"
#include "gui/wm.h"
#include "apps/fileops.h"

struct sysframe {
    u64 r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax, rip, rflags, rsp;
};

/* window / drawing calls (gui/usergui.c); E_NOTSUP without a GUI */
long usergui_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) __attribute__((weak));
long socket_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) __attribute__((weak));   /* net/ */

/* ---- helpers ---- */

/* wait that ends early when the process is being ended */
typedef struct { wait_cond_t cond; void *arg; proc_t *p; } kwait_t;
static int kwait_cond(void *a) { kwait_t *k = a; return k->p->killed || (k->cond && k->cond(k->arg)); }
int proc_wait(wait_cond_t cond, void *arg, u64 deadline_tsc) {
    kwait_t k = { cond, arg, proc_current() };
    if (!k.p) return thread_wait(cond, arg, deadline_tsc);
    thread_wait(kwait_cond, &k, deadline_tsc);
    return k.p->killed ? E_CANCELED : (cond && cond(arg)) ? 1 : 0;
}

/* absolute, normalised path from a user path (relative to the cwd) */
static const char *proc_cwd(proc_t *p) { return p->cwd[0] ? p->cwd : "C:/"; }

static int user_path(proc_t *p, u64 uva, char *out) {
    char tmp[VFS_MAX_PATH];
    int r = copyin_str(tmp, uva, sizeof tmp);
    if (r) return r;
    char joined[VFS_MAX_PATH * 2];
    if (tmp[0] && tmp[1] == ':') strlcpy(joined, tmp, sizeof joined);
    else if (tmp[0] == '/' || tmp[0] == '\\') snprintf(joined, sizeof joined, "%c:%s", proc_cwd(p)[0], tmp);
    else snprintf(joined, sizeof joined, "%s/%s", proc_cwd(p), tmp);
    return vfs_normalize(joined, out, VFS_MAX_PATH);
}

static void to_user_dirent(const vfs_dirent_t *e, hal_dirent_t *o) {
    memset(o, 0, sizeof *o);
    strlcpy(o->name, e->name, sizeof o->name);
    o->size = e->size;
    o->is_dir = e->is_dir;
    o->attr = e->attr;
    o->year = e->mtime.year; o->month = e->mtime.month; o->day = e->mtime.day;
    o->hour = e->mtime.hour; o->minute = e->mtime.minute; o->second = e->mtime.second;
}

/* ---- processes ---- */

static int zombie(void *arg) { proc_t *p = proc_by_pid((u32)(uptr)arg); return !p || p->state == P_ZOMBIE; }

static long sys_wait(proc_t *me, u32 pid) {
    proc_t *c = proc_by_pid(pid);
    if (!c || c->parent != me->pid) return E_NOENT;
    int r = proc_wait(zombie, (void *)(uptr)pid, ~0ull);
    if (r < 0) return r;
    c = proc_by_pid(pid);
    if (!c) return E_NOENT;
    c->parent = 0;                  /* collected: may be freed now */
    return c->exit_code;
}

static long sys_sbrk(proc_t *p, i64 incr) {
    u64 old = p->brk;
    if (incr < 0) return old;       /* memory is given back when the program ends */
    if (incr == 0) return (long)old;
    if ((u64)incr > 512ull * 1024 * 1024 || p->brk + (u64)incr > USER_STACK_TOP - USER_STACK_SIZE - 0x100000) return E_NOMEM;
    if (vm_grow(p, p->brk, (u64)incr)) return E_NOMEM;
    p->brk += (u64)incr;
    return (long)old;
}

static long sys_proclist(u64 ubuf, int max) {
    int n = 0;
    for (proc_t *p = proc_first(); p && n < max; p = p->next) {
        hal_procinfo_t pi;
        memset(&pi, 0, sizeof pi);
        pi.pid = p->pid; pi.parent = p->parent;
        strlcpy(pi.name, p->name, sizeof pi.name);
        pi.mem_kib = p->pages * 4;
        pi.cpu_ms = proc_cpu_ms(p);
        pi.run_ms = time_ms() - p->start_ms;
        pi.threads = p->nthreads;
        pi.state = p->state;
        if (copyout(ubuf + (u64)n * sizeof pi, &pi, sizeof pi)) return E_INVAL;
        n++;
    }
    return n;
}

static long sys_sysinfo(u64 ubuf) {
    hal_sysinfo_t si;
    memset(&si, 0, sizeof si);
    struct mm_stats ms;
    mm_get_stats(&ms);
    si.uptime_ms = time_ms();
    si.ram_kib = ms.total_ram >> 10;
    si.heap_kib = ms.heap_size >> 10;
    si.heap_used_kib = ms.heap_used >> 10;
    si.cpu_load = cpu_load_percent();
    for (proc_t *p = proc_first(); p; p = p->next) if (p->state == P_RUNNING) si.nprocs++;
    si.nthreads = thread_count();
    strlcpy(si.cpu, cpu_brand(), sizeof si.cpu);
    return copyout(ubuf, &si, sizeof si);
}

/* ---- console ---- */

static long con_write(proc_t *p, u64 ubuf, u64 len) {
    char buf[256];
    if (len > 64 * 1024) len = 64 * 1024;     /* bounded work per call (programs loop) */
    u64 done = 0;
    while (done < len) {
        u64 n = MIN(len - done, sizeof buf);
        if (copyin(buf, ubuf + done, n)) return done ? (long)done : E_INVAL;
        if (p->con && p->con->write) p->con->write(p->con_ctx, buf, (int)n);
        else kprintf("[%s] %.*s", p->name, (int)n, buf);
        done += n;
    }
    return (long)done;
}

static long con_read(proc_t *p, u64 ubuf, u64 len) {
    if (!p->con || !p->con->read) return 0;          /* no console: end of input */
    if (!user_writable(ubuf, MIN(len, 512))) return E_INVAL;   /* before waiting for a line */
    char buf[512];
    int n = p->con->read(p->con_ctx, buf, (int)MIN(len, sizeof buf));
    if (n <= 0) return n;
    if (p->killed) return E_CANCELED;
    return copyout(ubuf, buf, (u64)n) ? E_INVAL : n;
}

/* ---- files ---- */

static long sys_open(proc_t *p, u64 upath, int flags) {
    char path[VFS_MAX_PATH];
    int r = user_path(p, upath, path);
    if (r) return r;
    vfs_file_t *f = NULL;
    int vf = 0;
    if (flags & O_READ) vf |= VFS_O_READ;
    if (flags & O_WRITE) vf |= VFS_O_WRITE;
    if (flags & O_CREATE) vf |= VFS_O_CREATE;
    if (flags & O_TRUNC) vf |= VFS_O_TRUNC;
    if (flags & O_APPEND) vf |= VFS_O_APPEND;
    if (flags & O_EXCL) vf |= VFS_O_EXCL;
    if (!(vf & (VFS_O_READ | VFS_O_WRITE))) vf |= VFS_O_READ;
    r = vfs_open(path, vf, &f);
    if (r) return r;
    int h = handle_alloc(p, H_FILE, f);
    if (h < 0) vfs_close(f);
    return h;
}

static long file_io(proc_t *p, int fd, u64 ubuf, u64 len, int write) {
    if (fd == 0 && !write) return con_read(p, ubuf, len);
    if ((fd == 1 || fd == 2) && write) return con_write(p, ubuf, len);
    vfs_file_t *f = handle_get(p, fd, H_FILE);
    if (!f) return E_INVAL;
    u8 *buf = kmalloc(64 * 1024);
    if (!buf) return E_NOMEM;
    u64 done = 0;
    long r = 0;
    while (done < len) {
        u32 n = (u32)MIN(len - done, 64 * 1024);
        if (write) {
            if (copyin(buf, ubuf + done, n)) { r = E_INVAL; break; }
            r = vfs_write(f, buf, n);
        } else {
            r = vfs_read(f, buf, n);
            if (r > 0 && copyout(ubuf + done, buf, (u64)r)) { r = E_INVAL; break; }
        }
        if (r <= 0) break;
        done += (u64)r;
        if ((u64)r < n) break;
    }
    kfree(buf);
    return done ? (long)done : r;
}

static long sys_readdir(proc_t *p, u64 upath, u64 ubuf, int max) {
    char path[VFS_MAX_PATH];
    int r = user_path(p, upath, path);
    if (r) return r;
    vfs_dirent_t *ents;
    int n = vfs_list(path, &ents);
    if (n < 0) return n;
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        hal_dirent_t d;
        to_user_dirent(&ents[i], &d);
        if (copyout(ubuf + (u64)out * sizeof d, &d, sizeof d)) { out = E_INVAL; break; }
        out++;
    }
    kfree(ents);
    return out < 0 ? out : (max ? out : n);
}

/* ending another program: its ancestors may; anyone else only if the user
 * says so in a kernel dialog (Task Manager's End Process asks that way) */
static long sys_kill(proc_t *p, u32 pid) {
    proc_t *t = proc_by_pid(pid);
    if (!t || t->state != P_RUNNING) return E_NOENT;
    if (pid == p->pid || proc_is_ancestor(p->pid, pid)) return proc_kill(pid, "ended by another program");
    char msg[400];
    snprintf(msg, sizeof msg,
             "%s (process %u) wants to end %s (process %u).\n\n"
             "If you allow it, any unsaved information in %s will be lost. Do you want to end %s?",
             p->name, p->pid, t->name, pid, t->name, t->name);
    int a = gui_ask_permission(p, "End Program", msg);
    if (a < 0) return a;
    if (!a) return E_PERM;
    char why[80];
    snprintf(why, sizeof why, "ended by %s, with your permission", p->name);
    return proc_kill(pid, why);
}

typedef struct { char path[VFS_MAX_PATH]; } open_job_t;
static void job_open(void *a) { shell_open(((open_job_t *)a)->path); kfree(a); }
static long sys_shell_open(proc_t *p, u64 upath) {
    open_job_t *j = kmalloc(sizeof *j);
    if (!j) return E_NOMEM;
    int r = user_path(p, upath, j->path);
    if (r) { kfree(j); return r; }
    vfs_dirent_t st;
    if ((r = vfs_stat(j->path, &st)) != 0) { kfree(j); return r; }
    gui_post(job_open, j);              /* does not wait: the program keeps running */
    return 0;
}

static volatile int format_open;
static void job_format(void *a) {
    format_dialog(NULL, (char)(uptr)a);
    format_open = 0;
}
static long sys_shell_format(u64 letter) {
    char c = (char)toupper((int)(letter & 0x7F));
    volume_t *v = vfs_volume_by_letter(c);
    if (!v || !v->bd) return E_NOENT;
    if (format_open) return E_BUSY;     /* one at a time; the user answers it */
    format_open = 1;
    gui_post(job_format, (void *)(uptr)c);
    return 0;
}

int ahci_cd_eject(blockdev_t *bd) __attribute__((weak));   /* drivers/ahci.c */
static long sys_eject(u64 letter) {
    volume_t *v = vfs_volume_by_letter((char)toupper((int)(letter & 0x7F)));
    if (!v || !v->bd) return E_NOENT;
    if (!ahci_cd_eject) return E_NOTSUP;
    return ahci_cd_eject(v->bd);
}

static long sys_volume_info(u64 letter, u64 uinfo) {
    volume_t *v = vfs_volume_by_letter((char)toupper((int)(letter & 0x7F)));
    if (!v) return E_NOENT;
    hal_volinfo_t vi;
    memset(&vi, 0, sizeof vi);
    vi.letter = v->letter;
    int ic = drive_icon(v);
    vi.kind = ic == ICON_USBDRIVE ? VOL_REMOVABLE : ic == ICON_CDROM ? VOL_CDROM : ic == ICON_FLOPPY ? VOL_FLOPPY : VOL_HDD;
    vi.is_boot = (char)v->is_boot;
    vi.readonly = (char)(v->bd && v->bd->readonly);
    strlcpy(vi.label, v->label, sizeof vi.label);
    strlcpy(vi.fs, v->fs_name, sizeof vi.fs);
    if (v->bd) strlcpy(vi.device, v->bd->name, sizeof vi.device);
    u64 total = 0, freeb = 0;
    if (!vfs_statfs(v->letter, &total, &freeb)) { vi.total = total; vi.free_bytes = freeb; }
    return copyout(uinfo, &vi, sizeof vi) ? E_INVAL : 0;
}

static long sys_volumes(u64 ubuf, int max) {
    /* "C:FAT32:HALCYON\n..." */
    char buf[1024];
    int o = 0;
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        o += snprintf(buf + o, sizeof buf - o, "%c:%s:%s\n", v->letter, v->fs_name, v->label);
        if (o >= (int)sizeof buf - 1) { o = (int)sizeof buf - 1; break; }
    }
    if (max <= 0) return o + 1;
    int n = MIN(o + 1, max);
    if (copyout(ubuf, buf, (u64)n)) return E_INVAL;
    return n;
}

/* ---- Explorer: the shell's built-in tools, file details ---- */

/* open a built-in tool on the GUI thread (like shell_open, without
 * waiting); a few may be queued at a time, so a looping program cannot
 * pile up windows faster than the GUI makes them */
typedef struct { int app; char path[VFS_MAX_PATH]; } app_job_t;
static volatile int app_jobs;
static void job_app(void *a) {
    app_job_t *j = a;
    const char *path = j->path[0] ? j->path : NULL;
    switch (j->app) {
    case SHELL_APP_TERMINAL: app_terminal_in(path); break;
    case SHELL_APP_SYSINFO: app_sysinfo(); break;
    case SHELL_APP_CONTROL: app_control(); break;
    case SHELL_APP_ABOUT: app_about(); break;
    case SHELL_APP_NOTEPAD: shell_notepad(path); break;
    }
    kfree(j);
    app_jobs--;
}
static long sys_shell_app(proc_t *p, u64 app, u64 upath) {
    if (app < SHELL_APP_TERMINAL || app > SHELL_APP_NOTEPAD) return E_INVAL;
    if (app_jobs >= 4) return E_BUSY;
    app_job_t *j = kzalloc(sizeof *j);
    if (!j) return E_NOMEM;
    j->app = (int)app;
    if (upath && (app == SHELL_APP_TERMINAL || app == SHELL_APP_NOTEPAD)) {
        vfs_dirent_t st;
        int r = user_path(p, upath, j->path);
        if (!r) r = vfs_stat(j->path, &st);
        if (!r && st.is_dir != (app == SHELL_APP_TERMINAL)) r = app == SHELL_APP_TERMINAL ? E_NOTDIR : E_ISDIR;
        if (r) { kfree(j); return r; }
    }
    app_jobs++;
    gui_post(job_app, j);
    return 0;
}

static long sys_stat_ex(proc_t *p, u64 upath, u64 ubuf) {
    char path[VFS_MAX_PATH];
    vfs_dirent_t st;
    int r = user_path(p, upath, path);
    if (!r) r = vfs_stat(path, &st);
    if (r) return r;
    hal_statx_t x;
    memset(&x, 0, sizeof x);
    to_user_dirent(&st, &x.d);
    x.cyear = st.ctime.year; x.cmonth = st.ctime.month; x.cday = st.ctime.day;
    x.chour = st.ctime.hour; x.cminute = st.ctime.minute; x.csecond = st.ctime.second;
    strlcpy(x.short_name, st.short_name, sizeof x.short_name);
    volume_t *v = vfs_volume_by_letter(path[0]);
    if (v && v->bd) strlcpy(x.model, v->bd->model, sizeof x.model);
    return copyout(ubuf, &x, sizeof x) ? E_INVAL : 0;
}

/* ---- dispatcher ---- */

void syscall_dispatch(struct sysframe *f) {
    proc_t *p = proc_current();
    u64 nr = f->rax, a1 = f->rdi, a2 = f->rsi, a3 = f->rdx, a4 = f->r10, a5 = f->r8, a6 = f->r9;
    long r = E_NOTSUP;
    char path[VFS_MAX_PATH], path2[VFS_MAX_PATH];
    if (!p) { f->rax = (u64)E_INVAL; return; }
    switch (nr) {
    case SYS_EXIT: proc_exit((int)a1);
    case SYS_SPAWN: {
        char args[256] = "";
        r = user_path(p, a1, path);
        if (!r && a2) r = copyin_str(args, a2, sizeof args) == E_INVAL ? E_INVAL : 0;
        if (!r) {
            r = proc_spawn(path, args, p->con, p->con_ctx);
            proc_t *c = r > 0 ? proc_by_pid((u32)r) : NULL;
            if (c) strlcpy(c->cwd, proc_cwd(p), sizeof c->cwd);
        }
        break;
    }
    case SYS_WAIT: r = sys_wait(p, (u32)a1); break;
    case SYS_GETPID: r = p->pid; break;
    case SYS_YIELD: thread_yield(); r = 0; break;
    case SYS_SLEEP: proc_wait(NULL, NULL, time_ms_to_tsc(time_ms() + MIN(a1, 86400000ull))); r = 0; break;
    case SYS_TIME: r = (long)time_ms(); break;
    case SYS_SBRK: r = sys_sbrk(p, (i64)a1); break;
    case SYS_GETCWD: {
        const char *c = proc_cwd(p);
        u64 n = strlen(c) + 1;
        r = n > a2 ? E_NAMETOOLONG : copyout(a1, c, n) ? E_INVAL : (long)n - 1;
        break;
    }
    case SYS_CHDIR: {
        vfs_dirent_t st;
        r = user_path(p, a1, path);
        if (!r) r = vfs_stat(path, &st);
        if (!r && !st.is_dir) r = E_NOTDIR;
        if (!r) strlcpy(p->cwd, path, sizeof p->cwd);
        break;
    }
    case SYS_KILL: r = sys_kill(p, (u32)a1); break;
    case SYS_THREAD_CREATE: r = uthread_create(p, a1, a2); break;
    case SYS_THREAD_EXIT: uthread_exit((int)a1);
    case SYS_THREAD_JOIN: r = uthread_join(p, (u32)a1); break;
    case SYS_THREAD_DETACH: r = uthread_detach(p, (u32)a1); break;
    case SYS_FUTEX_WAIT:
        r = futex_wait(p, a1, (u32)a2, (i64)a3 < 0 ? ~0ull : time_ms_to_tsc(time_ms() + MIN(a3, 86400000ull)));
        break;
    case SYS_FUTEX_WAKE: r = futex_wake(p, a1, (int)MIN(a2, 0x7FFFFFFFull)); break;
    case SYS_SET_TLS: uthread_set_tls(a1); r = 0; break;
    case SYS_GETTID: r = thread_current()->utid; break;
    case SYS_SHM_OPEN: r = shm_open(p, a1, a2, a3); break;
    case SYS_SHM_MAP: r = shm_map(p, (int)a1, a2); break;
    case SYS_SHM_UNMAP: r = shm_unmap(p, a1); break;
    case SYS_SHELL_OPEN: r = sys_shell_open(p, a1); break;
    case SYS_SHELL_FORMAT: r = sys_shell_format(a1); break;
    case SYS_EJECT: r = sys_eject(a1); break;
    case SYS_DATETIME: {
        struct rtc_time tm;
        rtc_read(&tm);
        hal_datetime_t d = { tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second, tm.weekday };
        r = copyout(a1, &d, sizeof d) ? E_INVAL : 0;
        break;
    }
    case SYS_VOLUME_INFO: r = sys_volume_info(a1, a2); break;
    case SYS_VFS_GENERATION: r = vfs_generation(); break;
    case SYS_GETRANDOM: {
        u8 tmp[256];
        u64 n = MIN(a2, 65536ull), done = 0;
        if (!user_writable(a1, n)) { r = E_INVAL; break; }
        while (done < n) {
            u64 c = MIN(n - done, (u64)sizeof tmp);
            random_bytes(tmp, c);
            if (copyout(a1 + done, tmp, c)) break;
            done += c;
        }
        memset(tmp, 0, sizeof tmp);
        r = (long)done;
        break;
    }
    case SYS_SHELL_APP: r = sys_shell_app(p, a1, a2); break;
    case SYS_STAT_EX: r = sys_stat_ex(p, a1, a2); break;
    case SYS_PROCLIST: r = sys_proclist(a1, (int)MIN(a2, 256)); break;
    case SYS_SYSINFO: r = sys_sysinfo(a1); break;

    case SYS_OPEN: r = sys_open(p, a1, (int)a2); break;
    case SYS_CLOSE: {
        if (handle_get(p, (int)a1, H_SHM)) { r = shm_close(p, (int)a1); break; }
        if (handle_get(p, (int)a1, H_SOCKET)) { r = socket_handle_close ? socket_handle_close(p, (int)a1) : E_INVAL; break; }
        vfs_file_t *fl = handle_get(p, (int)a1, H_FILE);
        if (!fl) { r = E_INVAL; break; }
        handle_free(p, (int)a1);
        r = vfs_close(fl);
        break;
    }
    case SYS_READ:
    case SYS_WRITE:
        if (handle_get(p, (int)a1, H_SOCKET) && socket_handle_io) r = socket_handle_io(p, (int)a1, a2, a3, nr == SYS_WRITE);
        else r = file_io(p, (int)a1, a2, a3, nr == SYS_WRITE);
        break;
    case SYS_SEEK: {
        vfs_file_t *fl = handle_get(p, (int)a1, H_FILE);
        if (!fl) { r = E_INVAL; break; }
        u64 pos = a3 == 1 ? vfs_tell(fl) + a2 : a3 == 2 ? vfs_size(fl) + a2 : a2;
        /* a write far past the end zero-fills the gap under the storage
         * lock: keep that bounded */
        if (pos > vfs_size(fl) + (64ull << 20)) { r = E_INVAL; break; }
        r = vfs_seek(fl, pos);
        if (!r) r = (long)vfs_tell(fl);
        break;
    }
    case SYS_FSIZE: {
        vfs_file_t *fl = handle_get(p, (int)a1, H_FILE);
        r = fl ? (long)vfs_size(fl) : E_INVAL;
        break;
    }
    case SYS_TRUNCATE: {
        vfs_file_t *fl = handle_get(p, (int)a1, H_FILE);
        r = !fl ? E_INVAL : a2 > vfs_size(fl) + (64ull << 20) ? E_INVAL : vfs_truncate(fl, a2);
        break;
    }
    case SYS_STAT: {
        vfs_dirent_t st;
        hal_dirent_t d;
        r = user_path(p, a1, path);
        if (!r) r = vfs_stat(path, &st);
        if (!r) { to_user_dirent(&st, &d); r = copyout(a2, &d, sizeof d); }
        break;
    }
    case SYS_READDIR: r = sys_readdir(p, a1, a2, (int)MIN(a3, 100000)); break;
    case SYS_MKDIR: r = user_path(p, a1, path); if (!r) r = vfs_mkdir(path); break;
    case SYS_UNLINK: r = user_path(p, a1, path); if (!r) r = vfs_unlink(path); break;
    case SYS_RENAME:
        r = user_path(p, a1, path);
        if (!r) r = user_path(p, a2, path2);
        if (!r) r = vfs_rename(path, path2);
        break;
    case SYS_VOLUMES: r = sys_volumes(a1, (int)a2); break;
    default:
        if (nr >= 100 && nr < 140) {
            if (socket_syscall) r = socket_syscall((int)nr, a1, a2, a3, a4, a5, a6);
        } else if (nr >= SYS_WIN_CREATE && nr < SYS_COUNT && usergui_syscall) {
            r = usergui_syscall((int)nr, a1, a2, a3, a4, a5, a6);
        }
        break;
    }
    f->rax = (u64)r;
    /* SYSRET needs a canonical return address (USER_END keeps it so) */
    if (f->rip >= USER_END) proc_exit(-1);
    /* the time slice and kill checks follow in syscall_entry, with
     * interrupts disabled (proc_slice_check) */
}
