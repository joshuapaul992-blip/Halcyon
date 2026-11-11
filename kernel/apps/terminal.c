/* Halcyon OS - Command Prompt: terminal window + DOS-style shell */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/irq.h"
#include "core/proc.h"
#include "core/thread.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/vfs.h"
#include "fs/block.h"
#include "drivers/pci.h"
#include "usb/usb.h"
#include "drivers/keymap.h"
#include "fileops.h"

#define MAX_LINES 1000
#define LINE_MAX 512
#define SB_W 17
#define HIST 32

typedef struct term {
    char *lines[MAX_LINES];
    int nlines;                  /* number of completed lines */
    char cur[LINE_MAX];          /* line being written by output */
    int curlen;
    char input[LINE_MAX];
    int inlen, incur;
    char hist[HIST][LINE_MAX]; int nhist, hpos;
    char cwd[VFS_MAX_PATH];
    int scroll;                  /* lines scrolled back from the bottom */
    int cols, rows;
    u8 fg, bg;
    int caret_on; u64 caret_t;
    /* output redirection */
    char *rbuf; int rlen, rcap;
    int sb_drag;
    struct tcon *con;            /* console of the program running in this window */
    int errorlevel;              /* exit code of the last program */
    window_t *win;
} term_t;

/* stdin / stdout of a program started from the Command Prompt. Lives until
 * the program has ended, even if the window closes first (t = NULL then). */
typedef struct tcon {
    term_t *t;
    u32 pid;                     /* the program started from the prompt */
    int refs;                    /* programs using this console (it and its children) */
    char in[LINE_MAX + 2];
    int in_len, in_ready;
} tcon_t;

static const u32 palette[16] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xC0C0C0,
    0x808080, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
};

static int cw(void) { return gfx_char_width(&font_mono, 'M'); }
static int chh(void) { return font_mono.height + 1; }

/* ---------------- output ---------------- */
static void push_line(term_t *t) {
    t->cur[t->curlen] = 0;
    if (t->nlines == MAX_LINES) {
        kfree(t->lines[0]);
        memmove(t->lines, t->lines + 1, sizeof(char *) * (MAX_LINES - 1));
        t->nlines--;
    }
    t->lines[t->nlines++] = strdup(t->cur);
    t->curlen = 0;
}

static void tputc(term_t *t, char c) {
    if (t->rbuf || t->rcap) {
        if (t->rlen + 2 > t->rcap) {
            int nc = MAX(4096, t->rcap * 2);
            char *nb = kmalloc(nc);
            if (t->rbuf) { memcpy(nb, t->rbuf, t->rlen); kfree(t->rbuf); }
            t->rbuf = nb; t->rcap = nc;
        }
        if (c == '\n') t->rbuf[t->rlen++] = '\r';
        t->rbuf[t->rlen++] = c;
        return;
    }
    if (c == '\n') { push_line(t); return; }
    if (c == '\r') return;
    if (c == '\t') { do tputc(t, ' '); while (t->curlen % 8); return; }
    /* wrap by characters (UTF-8): only before the first byte of one */
    int lead = ((u8)c & 0xC0) != 0x80;
    if (t->curlen >= LINE_MAX - 1 ||
        (lead && t->curlen >= MAX(10, t->cols) && utf8_count(t->cur, t->curlen) >= MAX(10, t->cols))) push_line(t);
    t->cur[t->curlen++] = c;
}
static void tputs(term_t *t, const char *s) { while (*s) tputc(t, *s++); }
static void tprintf(term_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tprintf(term_t *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tputs(t, buf);
}

static void con_write(void *ctx, const char *s, int n) {
    tcon_t *c = ctx;
    if (!c->t) return;
    for (int i = 0; i < n; i++) tputc(c->t, s[i]);
    c->t->scroll = 0;
    wm_invalidate(c->t->win);
}
static int con_line_ready(void *ctx) { tcon_t *c = ctx; return !c->t || c->in_ready; }
static int con_read(void *ctx, char *buf, int n) {
    tcon_t *c = ctx;
    if (proc_wait(con_line_ready, c, ~0ull) < 0) return E_CANCELED;
    if (!c->t || !c->in_ready) return -1;            /* window closed: end of input */
    int len = MIN(n, c->in_len);
    memcpy(buf, c->in, len);
    c->in_ready = 0;
    return len;
}
static int calc_program(void) {
    vfs_dirent_t st;
    return vfs_stat("C:/PROGRAMS/CALC.EXE", &st) == 0 && idle_enabled();
}
static void con_attach(void *ctx, u32 pid) { ((tcon_t *)ctx)->refs++; (void)pid; }
static void con_exited(void *ctx, u32 pid, int code) {
    tcon_t *c = ctx;
    if (pid == c->pid && c->t) {
        /* the program started from the prompt ended: the prompt is back, and
         * the window no longer belongs to this console (later output of
         * programs it started is dropped) */
        term_t *t = c->t;
        if (t->con == c) t->con = NULL;
        t->errorlevel = code;
        if (t->curlen) tputc(t, '\n');
        tputc(t, '\n');
        wm_invalidate(t->win);
        c->t = NULL;
    }
    if (--c->refs <= 0) kfree(c);
}
static const console_ops_t term_con_ops = { con_write, con_read, con_attach, con_exited };

static void prompt_str(term_t *t, char *out, int n) {
    if (t->con) { if (n) out[0] = 0; return; }      /* a program is running: no prompt */
    char p[VFS_MAX_PATH];
    strlcpy(p, t->cwd, sizeof p);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    snprintf(out, n, "%s>", p);
}

/* ---------------- paths ---------------- */
static int resolve(term_t *t, const char *arg, char *out, int n) {
    char tmp[VFS_MAX_PATH * 2];
    if (isalpha(arg[0]) && arg[1] == ':') {
        if (!arg[2]) snprintf(tmp, sizeof tmp, "%c:/", arg[0]);
        else strlcpy(tmp, arg, sizeof tmp);
    } else if (arg[0] == '/' || arg[0] == '\\') snprintf(tmp, sizeof tmp, "%c:%s", t->cwd[0], arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", t->cwd, arg);
    return vfs_normalize(tmp, out, n);
}

static int wild_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (const char *q = s;; q++) { if (wild_match(p + 1, q)) return 1; if (!*q) return 0; }
    }
    if (!*s) return 0;
    if (*p == '?' || tolower(*p) == tolower(*s)) return wild_match(p + 1, s + 1);
    return 0;
}
static int has_wild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

/* expand "dir/pattern" into matching full paths; returns count, *out must be freed */
static int expand(term_t *t, const char *arg, char ***out) {
    char full[VFS_MAX_PATH], dir[VFS_MAX_PATH];
    *out = NULL;
    if (!has_wild(arg)) {
        if (resolve(t, arg, full, sizeof full) < 0) return E_INVAL;
        *out = kzalloc(sizeof(char *));
        (*out)[0] = strdup(full);
        return 1;
    }
    /* split pattern from its directory */
    char a[VFS_MAX_PATH];
    strlcpy(a, arg, sizeof a);
    char *sl = strrchr(a, '/'), *bs = strrchr(a, '\\');
    if (bs > sl) sl = bs;
    const char *pat = a;
    if (sl) { *sl = 0; pat = sl + 1; if (resolve(t, a[0] ? a : "/", dir, sizeof dir) < 0) return E_INVAL; }
    else if (a[1] == ':' && isalpha(a[0])) { snprintf(dir, sizeof dir, "%c:/", a[0]); pat = a + 2; }
    else strlcpy(dir, t->cwd, sizeof dir);
    vfs_dirent_t *ents;
    int n = vfs_list(dir, &ents);
    if (n < 0) return n;
    char **res = kzalloc(sizeof(char *) * (n + 1));
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!wild_match(pat, ents[i].name) && !wild_match(pat, ents[i].short_name)) continue;
        res[k] = kmalloc(VFS_MAX_PATH);
        vfs_join(dir, ents[i].name, res[k], VFS_MAX_PATH);
        k++;
    }
    kfree(ents);
    *out = res;
    return k;
}
static void free_list(char **l, int n) { for (int i = 0; i < n; i++) kfree(l[i]); kfree(l); }

/* ---------------- commands ---------------- */
static void err(term_t *t, const char *what, int e) { tprintf(t, "%s: %s\n", what, strerror(e)); }

static void cmd_dir(term_t *t, int argc, char **argv) {
    int wide = 0;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "/w")) wide = 1;
        else target = argv[i];
    }
    char path[VFS_MAX_PATH], pat[VFS_MAX_NAME + 1] = "*";
    if (!target) strlcpy(path, t->cwd, sizeof path);
    else if (has_wild(target)) {
        char a[VFS_MAX_PATH]; strlcpy(a, target, sizeof a);
        char *sl = strrchr(a, '/'), *bs = strrchr(a, '\\'); if (bs > sl) sl = bs;
        if (sl) { strlcpy(pat, sl + 1, sizeof pat); *sl = 0; resolve(t, a[0] ? a : "/", path, sizeof path); }
        else { strlcpy(pat, target, sizeof pat); strlcpy(path, t->cwd, sizeof path); }
    } else if (resolve(t, target, path, sizeof path) < 0) { tputs(t, "Invalid path\n"); return; }
    vfs_dirent_t st;
    if (strcmp(pat, "*") == 0 && vfs_stat(path, &st) == 0 && !st.is_dir) {
        strlcpy(pat, vfs_basename(path), sizeof pat);
        char d[VFS_MAX_PATH]; vfs_dirname(path, d, sizeof d); strlcpy(path, d, sizeof path);
    }
    vfs_dirent_t *ents;
    int n = vfs_list(path, &ents);
    if (n < 0) { err(t, "dir", n); return; }
    volume_t *v = vfs_volume_by_letter(path[0]);
    tprintf(t, " Volume in drive %c is %s\n", path[0], v && v->label[0] ? v->label : "unlabeled");
    char dp[VFS_MAX_PATH]; strlcpy(dp, path, sizeof dp);
    for (char *c = dp; *c; c++) if (*c == '/') *c = '\\';
    tprintf(t, "\n Directory of %s\n\n", dp);
    int files = 0, dirs = 0, col = 0;
    u64 bytes = 0;
    for (int i = 0; i < n; i++) {
        vfs_dirent_t *e = &ents[i];
        if (!wild_match(pat, e->name) && !wild_match(pat, e->short_name)) continue;
        if (e->is_dir) dirs++; else { files++; bytes += e->size; }
        if (wide) {
            /* 18 columns: truncate and pad by characters, not bytes */
            char nm[VFS_MAX_NAME + 3], cell[96];
            snprintf(nm, sizeof nm, e->is_dir ? "[%s]" : "%s", e->name);
            int nb = utf8_offset(nm, 18), nc = utf8_count(nm, nb);
            memcpy(cell, nm, nb);
            while (nc < 18) { cell[nb++] = ' '; nc++; }
            cell[nb] = 0;
            tputs(t, cell);
            if (++col == MAX(1, (t->cols - 1) / 18)) { tputc(t, '\n'); col = 0; }
            continue;
        }
        char tm[40];
        format_time(&e->mtime, tm, sizeof tm);
        if (e->is_dir) tprintf(t, "%-20s    <DIR>          %s\n", tm, e->name);
        else tprintf(t, "%-20s %16lu %s\n", tm, e->size, e->name);
    }
    if (col) tputc(t, '\n');
    kfree(ents);
    u64 total = 0, fr = 0;
    vfs_statfs(path[0], &total, &fr);
    char a[40], b[40];
    format_size_exact(bytes, a, sizeof a);
    format_size_exact(fr, b, sizeof b);
    tprintf(t, "%16d File(s) %s\n%16d Dir(s)  %s free\n", files, a, dirs, b);
}

static void cmd_type(term_t *t, const char *arg) {
    char p[VFS_MAX_PATH];
    if (resolve(t, arg, p, sizeof p) < 0) { tputs(t, "Invalid path\n"); return; }
    char *d; u32 sz;
    int r = vfs_read_file(p, &d, &sz);
    if (r < 0) { err(t, arg, r); return; }
    for (u32 i = 0; i < sz && i < 256 * 1024; i++) {
        char c = d[i];
        if (c == '\n' || c == '\t' || (u8)c >= 32) tputc(t, c);
        else if (c != '\r') tputc(t, '.');
    }
    if (sz && d[sz - 1] != '\n') tputc(t, '\n');
    kfree(d);
}

static void cmd_hexdump(term_t *t, const char *arg) {
    char p[VFS_MAX_PATH];
    if (resolve(t, arg, p, sizeof p) < 0) { tputs(t, "Invalid path\n"); return; }
    char *d; u32 sz;
    int r = vfs_read_file(p, &d, &sz);
    if (r < 0) { err(t, arg, r); return; }
    for (u32 o = 0; o < sz && o < 4096; o += 16) {
        tprintf(t, "%08x  ", o);
        for (u32 i = 0; i < 16; i++) { if (o + i < sz) tprintf(t, "%02x ", (u8)d[o + i]); else tputs(t, "   "); }
        tputc(t, ' ');
        for (u32 i = 0; i < 16 && o + i < sz; i++) tputc(t, isprint((u8)d[o + i]) ? d[o + i] : '.');
        tputc(t, '\n');
    }
    if (sz > 4096) tprintf(t, "... (%u bytes total, first 4096 shown)\n", sz);
    kfree(d);
}

static void cmd_copy_move(term_t *t, int argc, char **argv, int move) {
    if (argc < 3) { tprintf(t, "usage: %s source destination\n", move ? "move" : "copy"); return; }
    char **src; int n = expand(t, argv[1], &src);
    if (n <= 0) { tputs(t, n == 0 ? "File not found\n" : "Invalid path\n"); free_list(src, MAX(n, 0)); return; }
    char dst[VFS_MAX_PATH];
    if (resolve(t, argv[2], dst, sizeof dst) < 0) { tputs(t, "Invalid destination\n"); free_list(src, n); return; }
    vfs_dirent_t st;
    int dst_is_dir = vfs_stat(dst, &st) == 0 && st.is_dir;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        char target[VFS_MAX_PATH];
        if (dst_is_dir) vfs_join(dst, vfs_basename(src[i]), target, sizeof target);
        else strlcpy(target, dst, sizeof target);
        int r;
        if (move && toupper(src[i][0]) == toupper(target[0])) r = vfs_rename(src[i], target);
        else { r = fileops_copy_tree(src[i], target); if (r >= 0 && move) r = fileops_delete_tree(src[i]); }
        if (r < 0) err(t, vfs_basename(src[i]), r);
        else { ok++; tprintf(t, "%s\n", vfs_basename(src[i])); }
    }
    tprintf(t, "%9d file(s) %s.\n", ok, move ? "moved" : "copied");
    free_list(src, n);
}

static void cmd_del(term_t *t, const char *arg) {
    char **l; int n = expand(t, arg, &l);
    if (n <= 0) { tputs(t, n == 0 ? "Could not find the file\n" : "Invalid path\n"); free_list(l, MAX(n, 0)); return; }
    for (int i = 0; i < n; i++) {
        vfs_dirent_t st;
        if (vfs_stat(l[i], &st) < 0) continue;
        if (st.is_dir) { tprintf(t, "%s is a directory (use RD)\n", vfs_basename(l[i])); continue; }
        int r = vfs_unlink(l[i]);
        if (r < 0) err(t, vfs_basename(l[i]), r);
    }
    free_list(l, n);
}

static void tree_rec(term_t *t, const char *path, char *prefix, int depth) {
    if (depth > 12) return;
    vfs_dirent_t *ents;
    int n = vfs_list(path, &ents);
    if (n < 0) return;
    int ndirs = 0;
    for (int i = 0; i < n; i++) if (ents[i].is_dir) ndirs++;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!ents[i].is_dir) continue;
        k++;
        tprintf(t, "%s%s%s\n", prefix, k == ndirs ? "\\---" : "+---", ents[i].name);
        char child[VFS_MAX_PATH];
        vfs_join(path, ents[i].name, child, sizeof child);
        size_t pl = strlen(prefix);
        strlcat(prefix, k == ndirs ? "    " : "|   ", 256);
        tree_rec(t, child, prefix, depth + 1);
        prefix[pl] = 0;
    }
    kfree(ents);
}

static void cmd_lsusb(term_t *t) {
    for (usb_hc_t *hc = usb_first_hc(); hc; hc = hc->next) {
        tprintf(t, "Controller %u: %s, %u root ports\n", hc->id, hc->desc, hc->nports);
        for (usb_device_t *d = usb_first_device(); d; d = d->next) {
            if (d->hc != hc) continue;
            char ind[32] = "";
            for (int i = 0; i <= d->hub_depth && i < 8; i++) strlcat(ind, "  ", sizeof ind);
            tprintf(t, "%sPort %d: %04x:%04x %s - %s\n", ind, d->port, d->desc.idVendor, d->desc.idProduct,
                    d->product, d->description[0] ? d->description : usb_class_name(d->desc.bDeviceClass));
            tprintf(t, "%s        %s, address %u\n", ind, usb_speed_name(d->speed), d->address);
        }
    }
    if (!usb_first_hc()) tputs(t, "No USB controllers found.\n");
}

static void cmd_lspci(term_t *t) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        tprintf(t, "%02x:%02x.%x %04x:%04x %s, %s", d->bus, d->dev, d->func, d->vendor, d->device,
                pci_vendor_name(d->vendor), pci_class_name(d->class_code, d->subclass, d->prog_if));
        if (d->driver) tprintf(t, " [%s]", d->driver);
        tputc(t, '\n');
    }
}

static void cmd_disks(term_t *t) {
    for (blockdev_t *b = blockdev_first(); b; b = b->next) {
        char sz[32];
        format_size(b->sector_count * b->sector_size, sz, sizeof sz);
        if (b->type == BDEV_PART) tprintf(t, "  %-8s partition %d, type %02x, start %lu, %s\n", b->name, b->part_index, b->part_type, b->part_start, sz);
        else tprintf(t, "%-10s %-28s %s%s\n", b->name, b->model, sz, b->readonly ? " (read-only)" : "");
    }
    tputs(t, "\nVolumes:\n");
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        u64 tot, fr; vfs_statfs(v->letter, &tot, &fr);
        char a[32], b[32];
        format_size(tot, a, sizeof a); format_size(fr, b, sizeof b);
        tprintf(t, "  %c:  %-11s %-6s %10s total %10s free  on %s%s\n", v->letter, v->label[0] ? v->label : "(no label)",
                v->fs_name, a, b, v->bd ? v->bd->name : "?", v->is_boot ? "  [boot]" : "");
    }
}

static void cmd_help(term_t *t) {
    tputs(t,
        "Halcyon command shell. Commands (not case sensitive):\n\n"
        "  DIR [path] [/W]       list a directory        CD [path]   change directory\n"
        "  X:                    change drive            TREE [path] show the folder tree\n"
        "  TYPE file             show a text file        HEXDUMP file\n"
        "  COPY src dst          copy files/folders      MOVE src dst\n"
        "  DEL file              delete files (*,?)      REN old new rename\n"
        "  MD dir                make a directory        RD [/S] dir remove a directory\n"
        "  ECHO text [> file]    print or write text     CLS         clear the screen\n"
        "  VOL / DRIVES / DISKS  storage information     MEM         memory usage\n"
        "  LSUSB / LSPCI         hardware lists          DMESG [n]   kernel log\n"
        "  DATE / TIME / VER     system information      COLOR xy    set colours (e.g. 0A)\n"
        "  START file            open with its program   NOTEPAD [f] EXPLORER [d] PAINT CALC\n"
        "  SYNC                  flush disks             REBOOT / SHUTDOWN / EXIT\n"
        "  BOOTLOG               save \\HALCYON\\BOOT.LOG   CRASHTEST   test the crash screen\n"
        "  IRQ                   interrupts, timer and CPU usage\n"
        "  FORMAT [x: | device]  format a disk (FAT, FAT32, exFAT)\n"
        "  DISPLAY               screen resolution (Display Properties)\n"
        "  TASKLIST / TASKKILL   running programs / end one   NAME [args]  run C:\\PROGRAMS\\NAME.EXE\n"
        "  KEYBOARD [id | /L]    keyboard settings, switch layout, list layouts\n"
        "\nAny command's output can be redirected with > file or >> file.\n");
}

static int parse_args(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

static void run_command(term_t *t, char *line);

/* FORMAT target [/FS:FAT|FAT12|FAT16|FAT32|EXFAT] [/V:label] [/A:size] [/Q] [/Y]
 * target: a drive letter, or a device name from the list FORMAT prints */
static int has_children(blockdev_t *bd) {
    for (blockdev_t *b = blockdev_first(); b; b = b->next) if (b->parent == bd) return 1;
    return 0;
}
static volume_t *volume_of(blockdev_t *bd) {
    for (int i = 0; i < vfs_volume_count(); i++) if (vfs_volume(i)->bd == bd) return vfs_volume(i);
    return NULL;
}
static void cmd_format(term_t *t, int argc, char **argv) {
    if (argc < 2) {
        tputs(t, "Formats a disk for use with Halcyon (quick format).\n\n"
                 "FORMAT target [/FS:file-system] [/V:label] [/A:size] [/Y]\n\n"
                 "  target   a drive letter (E:) or a device name below\n"
                 "  /FS:     FAT32, FAT (FAT12/FAT16 chosen by size), FAT12, FAT16 or EXFAT\n"
                 "  /V:      volume label\n"
                 "  /A:      cluster size, e.g. 4096, 32K\n"
                 "  /Y       do not ask for confirmation\n\nDevices that can be formatted:\n");
        for (blockdev_t *b = blockdev_first(); b; b = b->next) {
            if (has_children(b) || !b->present) continue;
            char sz[32]; format_size(b->sector_count * b->sector_size, sz, sizeof sz);
            volume_t *v = volume_of(b);
            char what[48];
            if (v) snprintf(what, sizeof what, "%c: %s%s", v->letter, v->fs_name, v->is_boot ? " (system)" : "");
            else snprintf(what, sizeof what, "no file system Halcyon knows");
            tprintf(t, "  %-10s %9s  %-26.26s %s%s\n", b->name, sz, b->model, what, b->readonly ? ", read-only" : "");
        }
        return;
    }
    const char *target = argv[1], *fs = NULL, *label = NULL;
    u32 cluster = 0;
    int yes = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strncasecmp(a, "/FS:", 4)) fs = a + 4;
        else if (!strncasecmp(a, "/V:", 3)) label = a + 3;
        else if (!strncasecmp(a, "/A:", 3)) {
            char *end;
            cluster = (u32)strtol(a + 3, &end, 10);
            if (*end == 'K' || *end == 'k') cluster *= 1024;
        } else if (!strcasecmp(a, "/Y")) yes = 1;
        else if (!strcasecmp(a, "/Q")) ;
        else { tprintf(t, "Invalid parameter - %s\n", a); return; }
    }
    blockdev_t *bd = NULL;
    volume_t *v = NULL;
    if (strlen(target) == 2 && target[1] == ':') {
        v = vfs_volume_by_letter(target[0]);
        if (!v) { tputs(t, "The system cannot find the drive specified.\n"); return; }
        bd = v->bd;
    } else {
        for (blockdev_t *b = blockdev_first(); b; b = b->next) if (!strcasecmp(b->name, target)) bd = b;
        if (!bd) { tprintf(t, "There is no drive or device called %s. Type FORMAT for a list.\n", target); return; }
        v = volume_of(bd);
    }
    if (v && v->is_boot) { tputs(t, "Halcyon cannot format the drive it was started from.\n"); return; }
    if (has_children(bd)) { tprintf(t, "%s has partitions: format one of them (type FORMAT for a list).\n", bd->name); return; }
    if (!fs) {
        u64 bytes = bd->sector_count * bd->sector_size;
        fs = v ? (!strcmp(v->fs_name, "exFAT") ? "exFAT" : v->fs_name) : bytes > (32ull << 30) ? "exFAT" : "FAT";
        if (!strcmp(fs, "FAT12") || !strcmp(fs, "FAT16")) fs = "FAT";
    }
    char tgt[40];
    if (v) snprintf(tgt, sizeof tgt, "drive %c:", v->letter); else snprintf(tgt, sizeof tgt, "%s", bd->name);
    if (!yes) {
        char msg[160];
        snprintf(msg, sizeof msg, "WARNING, ALL DATA ON %s WILL BE LOST!\nProceed with Format?", tgt);
        if (msgbox(t->win, "Format", msg, ICON_WARNING, MB_YESNO) != ID_YES) { tputs(t, "Format cancelled.\n"); return; }
    }
    tprintf(t, "Formatting %s as %s...\n", tgt, fs);
    int r = vfs_format(bd, fs, label ? label : (v ? v->label : ""), cluster);
    if (r) {
        tprintf(t, "Format failed: %s\n", r == E_INVAL ? "the size does not suit this file system (or bad parameters)" :
                                             r == E_BUSY ? "the drive is in use (open files)" :
                                             r == E_NOTSUP ? "this file system is not available" : strerror(r));
        return;
    }
    v = volume_of(bd);
    tputs(t, "Format complete.\n");
    if (v) {
        u64 total = 0, freeb = 0;
        vfs_statfs(v->letter, &total, &freeb);
        char a[32], b2[32];
        format_size(total, a, sizeof a); format_size(freeb, b2, sizeof b2);
        tprintf(t, "Drive %c: %s, %s total, %s available.\n", v->letter, v->fs_name, a, b2);
    }
}

/* KEYBOARD: the Keyboard settings dialog; KEYBOARD id switches to an
 * installed layout; KEYBOARD /L lists the layouts */
static void cmd_keyboard(term_t *t, int argc, char **argv) {
    int idx[INPUT_MAX_LAYOUTS];
    int n = input_layouts(idx, INPUT_MAX_LAYOUTS);
    if (argc < 2) {
        tprintf(t, "Active keyboard layout: %s (%s)\n", kl_layouts[input_layout()].name, kl_layouts[input_layout()].id);
        app_keyboard();
        return;
    }
    if (!strcasecmp(argv[1], "/L")) {
        for (int i = 0; i < kl_layout_count; i++) {
            int inst = 0;
            for (int j = 0; j < n; j++) if (idx[j] == i) inst = 1;
            tprintf(t, "%c %-8s %-4s %s%s\n", i == input_layout() ? '*' : ' ', kl_layouts[i].id, kl_layouts[i].abbr,
                    kl_layouts[i].name, i == input_layout_default() ? " (default)" : inst ? " (installed)" : "");
        }
        return;
    }
    int k = kl_find(argv[1]);
    for (int j = 0; k >= 0 && j < n; j++)
        if (idx[j] == k) { input_layout_select(k); tprintf(t, "Keyboard layout: %s\n", kl_layouts[k].name); return; }
    if (k < 0) tprintf(t, "There is no keyboard layout \"%s\". KEYBOARD /L lists them.\n", argv[1]);
    else tprintf(t, "%s is not installed: add it in Keyboard settings (KEYBOARD).\n", kl_layouts[k].name);
}

static void execute(term_t *t, int argc, char **argv, const char *rest) {
    const char *c = argv[0];
    if (strlen(c) == 2 && c[1] == ':' && isalpha(c[0])) {
        char p[4] = { (char)toupper(c[0]), ':', '/', 0 };
        if (!vfs_volume_by_letter(p[0])) { tputs(t, "The system cannot find the drive specified.\n"); return; }
        strlcpy(t->cwd, p, sizeof t->cwd);
        return;
    }
    if (!strcasecmp(c, "help") || !strcmp(c, "?")) cmd_help(t);
    else if (!strcasecmp(c, "cls")) {
        for (int i = 0; i < t->nlines; i++) kfree(t->lines[i]);
        t->nlines = 0; t->curlen = 0; t->scroll = 0;
    }
    else if (!strcasecmp(c, "ver")) tprintf(t, "\nHalcyon [Version %s]\n", OS_VERSION);
    else if (!strcasecmp(c, "echo")) tprintf(t, "%s\n", rest ? rest : "");
    else if (!strcasecmp(c, "dir") || !strcasecmp(c, "ls")) cmd_dir(t, argc, argv);
    else if (!strcasecmp(c, "cd") || !strcasecmp(c, "chdir")) {
        if (argc < 2) { char p[VFS_MAX_PATH + 4]; prompt_str(t, p, sizeof p); p[strlen(p) - 1] = 0; tprintf(t, "%s\n", p); return; }
        char p[VFS_MAX_PATH];
        vfs_dirent_t st;
        if (resolve(t, argv[1], p, sizeof p) < 0 || vfs_stat(p, &st) < 0 || !st.is_dir) { tputs(t, "The system cannot find the path specified.\n"); return; }
        strlcpy(t->cwd, p, sizeof t->cwd);
    }
    else if (!strcasecmp(c, "type") || !strcasecmp(c, "cat")) { if (argc < 2) tputs(t, "usage: type file\n"); else cmd_type(t, argv[1]); }
    else if (!strcasecmp(c, "hexdump")) { if (argc < 2) tputs(t, "usage: hexdump file\n"); else cmd_hexdump(t, argv[1]); }
    else if (!strcasecmp(c, "copy") || !strcasecmp(c, "cp")) cmd_copy_move(t, argc, argv, 0);
    else if (!strcasecmp(c, "move") || !strcasecmp(c, "mv")) cmd_copy_move(t, argc, argv, 1);
    else if (!strcasecmp(c, "del") || !strcasecmp(c, "erase") || !strcasecmp(c, "rm")) { if (argc < 2) tputs(t, "usage: del file\n"); else for (int i = 1; i < argc; i++) cmd_del(t, argv[i]); }
    else if (!strcasecmp(c, "ren") || !strcasecmp(c, "rename")) {
        if (argc < 3) { tputs(t, "usage: ren old new\n"); return; }
        char a[VFS_MAX_PATH], b[VFS_MAX_PATH], d[VFS_MAX_PATH];
        if (resolve(t, argv[1], a, sizeof a) < 0) { tputs(t, "Invalid path\n"); return; }
        vfs_dirname(a, d, sizeof d);
        vfs_join(d, argv[2], b, sizeof b);
        int r = vfs_rename(a, b);
        if (r < 0) err(t, "ren", r);
    }
    else if (!strcasecmp(c, "md") || !strcasecmp(c, "mkdir")) {
        if (argc < 2) { tputs(t, "usage: md dir\n"); return; }
        char p[VFS_MAX_PATH];
        if (resolve(t, argv[1], p, sizeof p) < 0) { tputs(t, "Invalid path\n"); return; }
        int r = vfs_mkdir(p);
        if (r < 0) err(t, "md", r);
    }
    else if (!strcasecmp(c, "rd") || !strcasecmp(c, "rmdir")) {
        int recursive = argc > 2 && !strcasecmp(argv[1], "/s");
        const char *a = argv[argc - 1];
        if (argc < 2 || !strcasecmp(a, "/s")) { tputs(t, "usage: rd [/s] dir\n"); return; }
        char p[VFS_MAX_PATH];
        if (resolve(t, a, p, sizeof p) < 0) { tputs(t, "Invalid path\n"); return; }
        int r = recursive ? fileops_delete_tree(p) : vfs_unlink(p);
        if (r < 0) err(t, "rd", r);
    }
    else if (!strcasecmp(c, "tree")) {
        char p[VFS_MAX_PATH];
        if (resolve(t, argc > 1 ? argv[1] : ".", p, sizeof p) < 0) { tputs(t, "Invalid path\n"); return; }
        tprintf(t, "%s\n", p);
        char prefix[256] = "";
        tree_rec(t, p, prefix, 0);
    }
    else if (!strcasecmp(c, "vol")) {
        volume_t *v = vfs_volume_by_letter(t->cwd[0]);
        tprintf(t, " Volume in drive %c is %s (%s)\n", t->cwd[0], v && v->label[0] ? v->label : "unlabeled", v ? v->fs_name : "?");
    }
    else if (!strcasecmp(c, "drives") || !strcasecmp(c, "disks") || !strcasecmp(c, "lsblk")) cmd_disks(t);
    else if (!strcasecmp(c, "mem")) {
        struct mm_stats s; mm_get_stats(&s);
        tprintf(t, "Physical memory: %lu MB\nKernel heap:     %lu MB total, %lu KB used, %lu blocks\n",
                s.total_ram >> 20, s.heap_size >> 20, s.heap_used >> 10, s.heap_blocks);
    }
    else if (!strcasecmp(c, "date") || !strcasecmp(c, "time")) {
        struct rtc_time tm; rtc_read(&tm);
        static const char *wd[] = { "", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        if (!strcasecmp(c, "date")) tprintf(t, "The current date is: %s %02d/%02d/%04d\n", wd[tm.weekday % 8], tm.month, tm.day, tm.year);
        else tprintf(t, "The current time is: %02d:%02d:%02d\nUptime: %lu s\n", tm.hour, tm.minute, tm.second, time_ms() / 1000);
    }
    else if (!strcasecmp(c, "lsusb")) cmd_lsusb(t);
    else if (!strcasecmp(c, "lspci")) cmd_lspci(t);
    else if (!strcasecmp(c, "dmesg")) {
        int want = argc > 1 ? atoi(argv[1]) : 40;
        char *buf = kmalloc(128 * 1024);
        size_t n = klog_read(buf, 128 * 1024);
        int lines = 0; size_t start = n;
        while (start > 0) { if (buf[start - 1] == '\n' && ++lines > want) break; start--; }
        for (size_t i = start; i < n; i++) tputc(t, buf[i]);
        kfree(buf);
    }
    else if (!strcasecmp(c, "color")) {
        if (argc < 2 || strlen(argv[1]) != 2) { tputs(t, "usage: color BF  (hex digits: background, foreground; e.g. 0A, 1F, 07)\n"); return; }
        int b = (int)strtol((char[]){ argv[1][0], 0 }, NULL, 16), f = (int)strtol((char[]){ argv[1][1], 0 }, NULL, 16);
        if (b == f) { tputs(t, "Foreground and background must differ.\n"); return; }
        t->bg = (u8)b; t->fg = (u8)f;
    }
    else if (!strcasecmp(c, "crashtest")) {
        /* diagnostic: verify the crash screen and BOOT.LOG crash logging */
        kprintf("terminal: CRASHTEST requested\n");
        __asm__ volatile("ud2");
    }
    else if (!strcasecmp(c, "bootlog")) {
        int bootlog_save(const char *reason);
        int r = bootlog_save("user request");
        tprintf(t, r == 0 ? "Kernel log saved to \\HALCYON\\BOOT.LOG on the boot drive.\n" : "Could not save the log: %s\n", strerror(r));
    }
    else if (!strcasecmp(c, "format")) cmd_format(t, argc, argv);
    else if (!strcasecmp(c, "tasklist")) {
        tputs(t, "\nImage Name          PID  Parent  Threads    Memory    CPU time\n"
                 "================  =====  ======  =======  ========  ==========\n");
        tprintf(t, "%-16s  %5d  %6s  %7d  %6lu KB  (kernel)\n", "System", 0, "-", thread_count(), (u64)0);
        for (proc_t *pr = proc_first(); pr; pr = pr->next) {
            if (pr->state != P_RUNNING) continue;
            u64 cpu = proc_cpu_ms(pr);
            tprintf(t, "%-16s  %5u  %6u  %7d  %6lu KB  %lu.%03lu s\n", pr->name, pr->pid, pr->parent, pr->nthreads,
                    pr->pages * 4, cpu / 1000, cpu % 1000);
        }
    }
    else if (!strcasecmp(c, "taskkill")) {
        if (argc < 2) { tputs(t, "usage: TASKKILL pid | name.exe\n"); return; }
        int n = 0;
        char *end;
        long pid = strtol(argv[1], &end, 10);
        for (proc_t *pr = proc_first(); pr; pr = pr->next) {
            if (pr->state != P_RUNNING) continue;
            if ((*end == 0 && pr->pid == (u32)pid) || (*end && !strcasecmp(pr->name, argv[1]))) {
                proc_kill(pr->pid, NULL);
                tprintf(t, "SUCCESS: process %s (PID %u) was ended.\n", pr->name, pr->pid);
                n++;
            }
        }
        if (!n) tprintf(t, "ERROR: no running process \"%s\".\n", argv[1]);
    }
    else if (!strcasecmp(c, "irq")) {
        char *buf = kmalloc(4096);
        if (buf) {
            irq_list(buf, 4096);
            tputs(t, buf);
            tprintf(t, "CPU usage: %d%%\n", cpu_load_percent());
            kfree(buf);
        }
    }
    else if (!strcasecmp(c, "sync")) { vfs_sync_all(); tputs(t, "All volumes flushed.\n"); }
    else if (!strcasecmp(c, "notepad") || !strcasecmp(c, "edit")) {
        char p[VFS_MAX_PATH];
        if (argc > 1 && resolve(t, argv[1], p, sizeof p) == 0) shell_notepad(p); else shell_notepad(NULL);
    }
    else if (!strcasecmp(c, "explorer")) {
        char p[VFS_MAX_PATH];
        if (resolve(t, argc > 1 ? argv[1] : ".", p, sizeof p) == 0) shell_explorer(p); else shell_explorer("");
    }
    else if (!strcasecmp(c, "start")) {
        char p[VFS_MAX_PATH];
        if (argc > 1 && resolve(t, argv[1], p, sizeof p) == 0) shell_open(p); else tputs(t, "usage: start file\n");
    }
    else if (!strcasecmp(c, "paint")) shell_paint(NULL);
    else if (!strcasecmp(c, "calc") && !calc_program()) app_calc();
    else if (!strcasecmp(c, "keyboard")) cmd_keyboard(t, argc, argv);     /* else CALC.EXE below */
    else if (!strcasecmp(c, "sysinfo") || !strcasecmp(c, "msinfo")) app_sysinfo();
    else if (!strcasecmp(c, "control")) app_control();
    else if (!strcasecmp(c, "display") || !strcasecmp(c, "desk.cpl")) app_display();
    else if (!strcasecmp(c, "exit")) wm_close(t->win);
    else if (!strcasecmp(c, "reboot")) { vfs_sync_all(); reboot(); }
    else if (!strcasecmp(c, "shutdown")) shutdown_dialog();
    else {
        /* a program: NAME, NAME.EXE, in the current directory or C:\PROGRAMS */
        char p[VFS_MAX_PATH], cand[VFS_MAX_PATH];
        vfs_dirent_t st;
        const char *dot = strrchr(c, '.');
        int has_ext = dot && !strchr(dot, '/') && !strchr(dot, '\\');
        const char *dirs[2] = { NULL, "C:/PROGRAMS" };
        for (int d = 0; d < 2; d++) {
            for (int e = 0; e < 2; e++) {
                if (e == 0 && !has_ext) continue;
                snprintf(cand, sizeof cand, "%s%s", c, e ? ".EXE" : "");
                if (e && has_ext) continue;
                int r = d == 0 ? resolve(t, cand, p, sizeof p) : (vfs_join(dirs[d], cand, p, sizeof p), 0);
                if (r || vfs_stat(p, &st) || st.is_dir) continue;
                const char *x = strrchr(p, '.');
                if (!x || strcasecmp(x, ".EXE")) continue;
                tcon_t *con = kzalloc(sizeof *con);
                if (!con) { tputs(t, "Not enough memory.\n"); return; }
                con->t = t;
                int pid = proc_spawn(p, rest, &term_con_ops, con);
                if (pid < 0) {
                    kfree(con);
                    tprintf(t, "%s cannot be started: %s.\n", vfs_basename(p),
                            pid == E_INVAL ? "it is not a valid Halcyon program" : strerror(pid));
                    return;
                }
                con->pid = (u32)pid;
                proc_t *pr = proc_by_pid((u32)pid);
                if (pr) strlcpy(pr->cwd, t->cwd, sizeof pr->cwd);
                t->con = con;
                return;
            }
        }
        if (resolve(t, c, p, sizeof p) == 0 && vfs_stat(p, &st) == 0) { shell_open(p); return; }
        tprintf(t, "'%s' is not recognized as a command.\nType HELP for a list of commands.\n", c);
    }
}

static void run_command(term_t *t, char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    /* redirection */
    char *redir = NULL; int append = 0;
    for (char *p = line; *p; p++) {
        if (*p == '"') { p = strchr(p + 1, '"'); if (!p) break; continue; }
        if (*p == '>') {
            append = p[1] == '>';
            *p = 0;
            redir = p + 1 + append;
            while (*redir == ' ') redir++;
            char *e = redir + strlen(redir);
            while (e > redir && e[-1] == ' ') *--e = 0;
            if (*redir == '"') { redir++; char *q = strchr(redir, '"'); if (q) *q = 0; }
            /* trim the command part */
            e = p;
            while (e > line && e[-1] == ' ') *--e = 0;
            break;
        }
    }
    char *rest = NULL;
    char copy[LINE_MAX];
    strlcpy(copy, line, sizeof copy);
    char *sp = strchr(copy, ' ');
    if (sp) { rest = sp + 1; while (*rest == ' ') rest++; char *e = rest + strlen(rest); while (e > rest && e[-1] == ' ') *--e = 0; }
    char *argv[32];
    int argc = parse_args(line, argv, 32);
    if (!argc) return;
    if (redir) {
        if (!*redir) { tputs(t, "The syntax of the command is incorrect.\n"); return; }
        t->rcap = 1;   /* enable capture */
        t->rbuf = NULL; t->rlen = 0;
    }
    execute(t, argc, argv, rest);
    if (redir) {
        char *buf = t->rbuf; int len = t->rlen;
        t->rbuf = NULL; t->rcap = 0; t->rlen = 0;
        char p[VFS_MAX_PATH];
        if (resolve(t, redir, p, sizeof p) < 0) { tputs(t, "Invalid redirection target\n"); kfree(buf); return; }
        vfs_file_t *f;
        int r = vfs_open(p, VFS_O_WRITE | VFS_O_CREATE | (append ? VFS_O_APPEND : VFS_O_TRUNC), &f);
        if (r < 0) { err(t, redir, r); kfree(buf); return; }
        if (len) r = vfs_write(f, buf, (u32)len);
        vfs_close(f);
        if (r < 0) err(t, redir, r);
        kfree(buf);
    }
}

/* ---------------- window ---------------- */
static void term_layout(term_t *t) {
    window_t *w = t->win;
    t->cols = MAX(10, (w->client->w - SB_W - 8) / cw());
    t->rows = MAX(3, (w->client->h - 6) / chh());
}

static int total_rows(term_t *t) {
    char p[VFS_MAX_PATH + 4];
    prompt_str(t, p, sizeof p);
    int ilen = utf8_count(p, 1 << 30) + utf8_count(t->input, t->inlen) + 1;
    return t->nlines + MAX(1, (utf8_count(t->cur, t->curlen) + ilen + t->cols - 1) / t->cols);
}

static void term_paint(window_t *w, gc_t *g) {
    term_t *t = w->data;
    term_layout(t);
    color_t bg = HEX(palette[t->bg]), fg = HEX(palette[t->fg]);
    rect_t area = mkrect(0, 0, w->client->w - SB_W, w->client->h);
    gfx_fill(g, area, bg);
    /* build the visual line list: completed lines + (cur + prompt + input) wrapped */
    char p[VFS_MAX_PATH + 4];
    prompt_str(t, p, sizeof p);
    char tail[LINE_MAX * 3];
    snprintf(tail, sizeof tail, "%.*s%s%.*s", t->curlen, t->cur, p, t->inlen, t->input);
    int tail_len = (int)strlen(tail);
    int tail_chars = utf8_count(tail, tail_len);
    int tail_rows = MAX(1, (tail_chars + 1 + t->cols - 1) / t->cols);
    int total = t->nlines + tail_rows;
    int first = MAX(0, total - t->rows - t->scroll);
    gc_t sub = gc_sub(g, mkrect(4, 3, area.w - 4, area.h - 3));
    for (int r = 0; r < t->rows + 1; r++) {
        int idx = first + r;
        int y = r * chh();
        if (idx < t->nlines) gfx_text(&sub, &font_mono, 0, y, t->lines[idx], fg);
        else if (idx < total) {
            int tr = idx - t->nlines;
            int off = utf8_offset(tail, tr * t->cols);
            if (off < tail_len) gfx_text_n(&sub, &font_mono, 0, y, tail + off, utf8_offset(tail + off, t->cols), fg);
            /* caret (in characters) */
            int cpos = utf8_count(t->cur, t->curlen) + utf8_count(p, 1 << 30) + utf8_count(t->input, t->incur);
            if (cpos / t->cols == tr && t->caret_on && wm_active() == w && !t->scroll)
                gfx_fill(&sub, mkrect((cpos % t->cols) * cw(), y + chh() - 3, cw(), 2), fg);
        }
    }
    draw_scrollbar(g, mkrect(area.w, 0, SB_W, area.h), total, t->rows, first, 1);
}

static void term_submit(term_t *t) {
    if (t->con) {
        /* a program is running: the line is its input */
        t->input[t->inlen] = 0;
        tputs(t, t->input);
        tputc(t, '\n');
        if (!t->con->in_ready) {
            int n = MIN(t->inlen, LINE_MAX);
            memcpy(t->con->in, t->input, n);
            t->con->in[n] = '\n';
            t->con->in_len = n + 1;
            t->con->in_ready = 1;
        }
        t->inlen = t->incur = 0;
        t->input[0] = 0;
        t->scroll = 0;
        return;
    }
    char p[VFS_MAX_PATH + 4];
    prompt_str(t, p, sizeof p);
    t->input[t->inlen] = 0;
    tputs(t, p);
    tputs(t, t->input);
    tputc(t, '\n');
    char line[LINE_MAX];
    strlcpy(line, t->input, sizeof line);
    if (t->inlen && (t->nhist == 0 || strcmp(t->hist[(t->nhist - 1) % HIST], line))) {
        strlcpy(t->hist[t->nhist % HIST], line, LINE_MAX);
        t->nhist++;
    }
    t->hpos = t->nhist;
    t->inlen = t->incur = 0;
    t->input[0] = 0;
    t->scroll = 0;
    wm_set_busy(1);
    run_command(t, line);
    wm_set_busy(0);
    if (!wm_alive(t->win)) return;
    if (t->curlen) tputc(t, '\n');
    if (t->nlines && t->lines[t->nlines - 1][0]) tputc(t, '\n');
}

static void complete(term_t *t) {
    /* complete the last word against the directory listing */
    int ws = t->incur;
    while (ws > 0 && t->input[ws - 1] != ' ') ws--;
    char word[VFS_MAX_PATH];
    int wl = t->incur - ws;
    memcpy(word, t->input + ws, wl); word[wl] = 0;
    char dir[VFS_MAX_PATH], pre[VFS_MAX_PATH] = "";
    const char *base = word;
    char *sl = strrchr(word, '/'), *bs = strrchr(word, '\\'); if (bs > sl) sl = bs;
    if (sl) { memcpy(pre, word, sl - word + 1); pre[sl - word + 1] = 0; base = sl + 1; if (resolve(t, pre, dir, sizeof dir) < 0) return; }
    else strlcpy(dir, t->cwd, sizeof dir);
    vfs_dirent_t *ents;
    int n = vfs_list(dir, &ents);
    if (n <= 0) return;
    const char *match = NULL; int count = 0;
    for (int i = 0; i < n; i++) if (!strncasecmp(ents[i].name, base, strlen(base))) { match = ents[i].name; count++; }
    if (count == 1) {
        char rep[VFS_MAX_PATH];
        snprintf(rep, sizeof rep, "%s%s%s", pre, match, strchr(match, ' ') ? "" : "");
        int rl = (int)strlen(rep);
        if (t->inlen - wl + rl < LINE_MAX - 1) {
            memmove(t->input + ws + rl, t->input + t->incur, t->inlen - t->incur);
            memcpy(t->input + ws, rep, rl);
            t->inlen += rl - wl;
            t->incur = ws + rl;
        }
    } else if (count > 1) {
        char p[VFS_MAX_PATH + 4]; prompt_str(t, p, sizeof p);
        tputs(t, p); tputs(t, t->input); tputc(t, '\n');
        for (int i = 0; i < n; i++) if (!strncasecmp(ents[i].name, base, strlen(base))) { tputs(t, ents[i].name); tputs(t, "  "); }
        tputc(t, '\n');
    }
    kfree(ents);
}

static void term_event(window_t *w, gui_event_t *e) {
    term_t *t = w->data;
    rect_t sbr = mkrect(w->client->w - SB_W, 0, SB_W, w->client->h);
    if (e->type == GE_WHEEL) {
        t->scroll = CLAMP(t->scroll + e->dz * 3, 0, MAX(0, total_rows(t) - t->rows));
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_DOWN && rect_contains(sbr, e->x, e->y)) {
        int total = total_rows(t), first = MAX(0, total - t->rows - t->scroll), part;
        first = scrollbar_hit(sbr, total, t->rows, first, e->x, e->y, 1, &part);
        t->scroll = CLAMP(total - t->rows - first, 0, MAX(0, total - t->rows));
        t->sb_drag = part == 5;
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_MOVE && t->sb_drag && (e->buttons & MOUSE_LEFT)) {
        int total = total_rows(t), maxf = MAX(0, total - t->rows);
        int first = CLAMP((e->y - SB_W) * maxf / MAX(1, w->client->h - 2 * SB_W), 0, maxf);
        t->scroll = maxf - first;
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_UP) { t->sb_drag = 0; return; }
    if (e->type == GE_MOUSE_DOWN && e->button == MOUSE_RIGHT) {
        /* right click pastes, like the classic console */
        int l; const char *c = clipboard_get(&l);
        for (int i = 0; c && i < l && t->inlen < LINE_MAX - 1; i++) {
            if (c[i] == '\n' || c[i] == '\r') continue;
            memmove(t->input + t->incur + 1, t->input + t->incur, t->inlen - t->incur);
            t->input[t->incur++] = c[i]; t->inlen++;
        }
        wm_invalidate(w);
        return;
    }
    if (e->type != GE_KEY || !e->key.pressed) return;
    input_event_t *k = &e->key;
    t->scroll = 0;
    t->caret_on = 1; t->caret_t = time_ms();
    if ((k->mods & MOD_CTRL) && !key_is_text(k)) {
        if (key_shortcut(k, 'C') && t->con) { tputs(t, "^C\n"); proc_kill(t->con->pid, NULL); t->inlen = t->incur = 0; }
        else if (key_shortcut(k, 'C')) { char p[VFS_MAX_PATH + 4]; prompt_str(t, p, sizeof p); tputs(t, p); tputs(t, t->input); tputs(t, "^C\n"); t->inlen = t->incur = 0; }
        else if (key_shortcut(k, 'L')) execute(t, 1, (char *[]){ "cls" }, NULL);
        else if (key_shortcut(k, 'V')) { gui_event_t pe = { .type = GE_MOUSE_DOWN, .button = MOUSE_RIGHT }; term_event(w, &pe); return; }
        wm_invalidate(w);
        return;
    }
    switch (k->key) {
    case KEY_ENTER: case KEY_KP_ENTER: term_submit(t); break;
    case KEY_BACKSPACE:
        if (t->incur > 0) {
            int pv = utf8_prev(t->input, t->incur);
            memmove(t->input + pv, t->input + t->incur, t->inlen - t->incur);
            t->inlen -= t->incur - pv; t->incur = pv;
        }
        break;
    case KEY_DELETE:
        if (t->incur < t->inlen) {
            int nx = utf8_after(t->input, t->inlen, t->incur);
            memmove(t->input + t->incur, t->input + nx, t->inlen - nx);
            t->inlen -= nx - t->incur;
        }
        break;
    case KEY_LEFT: if (t->incur > 0) t->incur = utf8_prev(t->input, t->incur); break;
    case KEY_RIGHT: if (t->incur < t->inlen) t->incur = utf8_after(t->input, t->inlen, t->incur); break;
    case KEY_HOME: t->incur = 0; break;
    case KEY_END: t->incur = t->inlen; break;
    case KEY_ESC: t->inlen = t->incur = 0; break;
    case KEY_UP: case KEY_DOWN:
        if (!t->nhist) break;
        if (k->key == KEY_UP) t->hpos = MAX(MAX(0, t->nhist - HIST), t->hpos - 1);
        else t->hpos = MIN(t->nhist, t->hpos + 1);
        if (t->hpos == t->nhist) t->input[0] = 0;
        else strlcpy(t->input, t->hist[t->hpos % HIST], LINE_MAX);
        t->inlen = t->incur = (int)strlen(t->input);
        break;
    case KEY_PAGEUP: t->scroll = MIN(MAX(0, total_rows(t) - t->rows), t->rows); break;
    case KEY_TAB: complete(t); break;
    default:
        if (key_is_text(k)) {
            char u[4];
            int n = utf8_put(k->ch, u);
            if (t->inlen + n > LINE_MAX - 1) break;
            memmove(t->input + t->incur + n, t->input + t->incur, t->inlen - t->incur);
            memcpy(t->input + t->incur, u, n);
            t->incur += n;
            t->inlen += n;
        }
    }
    if (wm_alive(w)) wm_invalidate(w);
}

static void term_tick(window_t *w) {
    term_t *t = w->data;
    if (wm_active() != w) return;
    if (time_ms() - t->caret_t >= 500) { t->caret_on = !t->caret_on; t->caret_t = time_ms(); wm_invalidate(w); }
}

static void term_destroy(window_t *w) {
    term_t *t = w->data;
    if (t->con) { t->con->t = NULL; proc_kill(t->con->pid, NULL); }
    for (int i = 0; i < t->nlines; i++) kfree(t->lines[i]);
    kfree(t->rbuf);
    kfree(t);
}

void app_terminal(void) { app_terminal_in(NULL); }

/* a Command Prompt in folder dir (NULL: the root of C:) */
void app_terminal_in(const char *dir) {
    int c = cw(), h = chh();
    window_t *w = wm_create("Command Prompt", ICON_TERMINAL, -1, -1, 80 * c + SB_W + 8, 25 * h + 6, WF_RESIZABLE);
    term_t *t = kzalloc(sizeof *t);
    t->win = w;
    t->fg = 7; t->bg = 0;
    volume_t *v = vfs_volume_by_letter('C');
    if (!v && vfs_volume_count()) v = vfs_volume(0);
    if (v) snprintf(t->cwd, sizeof t->cwd, "%c:/", v->letter);
    else strlcpy(t->cwd, "C:/", sizeof t->cwd);
    if (dir && dir[0]) strlcpy(t->cwd, dir, sizeof t->cwd);
    w->data = t;
    w->cursor = CURSOR_IBEAM;
    w->on_paint = term_paint;
    w->on_event = term_event;
    w->on_tick = term_tick;
    w->on_destroy = term_destroy;
    term_layout(t);
    tprintf(t, "Halcyon [Version %s]\n(C) Halcyon contributors. Type HELP for a list of commands.\n\n", OS_VERSION);
}
