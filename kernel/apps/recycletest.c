/* Halcyon OS - boot test of the Recycle Bin and drag and drop (QEMU fw_cfg
 * opt/halcyon/recycletest, boottest.py variant "recycle"). Once the desktop
 * is up, a kernel thread works on the SATA test disks:
 *   - GPTDISK (FAT32): RBTEST\deleted.txt goes to the bin and stays there;
 *     RBTEST\Sub\restored.txt goes to the bin, its folder is deleted for good,
 *     and it is restored (the folder made again);
 *   - EXFATDISK (exFAT): two files go to the bin, which is then emptied (the
 *     bin's folder must be gone);
 *   - a cross-drive drag: EXPLORER.EXE opens GPTDISK's RBTEST folder, and a
 *     drag of C:\Documents\Getting Started.txt is dropped on its window: the
 *     program answers (a copy: another drive) and copies it.
 * It logs "recycle test: ..." lines and ends with "recycle test: done (N
 * checks, M failed)"; boottest.py then checks the images with mtools,
 * fsck.fat, fsck.exfat and exfat_tool.py. */
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/thread.h"
#include "core/proc.h"
#include "gui/wm.h"
#include "gui/dnd.h"
#include "fs/vfs.h"
#include "recycle.h"

static int checks, failed;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) failed++;
    kprintf("recycle test: %s: %s\n", what, ok ? "ok" : "FAILED");
}

static char drive_labelled(const char *label) {
    for (int i = 0; i < vfs_volume_count(); i++)
        if (!strcmp(vfs_volume(i)->label, label)) return vfs_volume(i)->letter;
    return 0;
}

static int put(const char *path, const char *text) { return vfs_write_file(path, text, (u32)strlen(text)) == (int)strlen(text); }

static int same_text(const char *path, const char *text) {
    char *d;
    u32 n;
    if (vfs_read_file(path, &d, &n) < 0) return 0;
    int ok = n == strlen(text) && !memcmp(d, text, n);
    kfree(d);
    return ok;
}

static u64 file_size(const char *path) {
    vfs_dirent_t st;
    return vfs_stat(path, &st) ? ~0ull : st.size;
}

/* GUI thread parts */
typedef struct { const char *title; rect_t client; int found; } find_t;
static void job_find(void *a) {
    find_t *f = a;
    f->found = 0;
    for (window_t *w = wm_first(); w; w = w->next)
        if (!strcmp(w->title, f->title) && w->on_dnd && w->client) { f->client = wm_client_rect(w); f->found = 1; }
}
typedef struct { char *paths; int len, x, y, step, result; } drag_t;
static void job_drag(void *a) {
    drag_t *d = a;
    if (d->step == 0) d->result = dnd_test_begin(d->paths, d->len, 1, ICON_FILE_TEXT);
    else if (d->step == 1) { dnd_test_move(d->x, d->y); d->result = dnd_effect(); }
    else { d->result = dnd_effect(); dnd_test_drop(); }
}

static void test(void *arg) {
    (void)arg;
    while (!gui_desktop_ready) thread_sleep_ms(100);
    thread_sleep_ms(1500);                  /* (the startup sound, the first paint) */
    char F = drive_labelled("GPTDISK"), X = drive_labelled("EXFATDISK");
    kprintf("recycle test: FAT32 on %c:, exFAT on %c:\n", F ? F : '?', X ? X : '?');
    check(F && X, "test disks mounted");
    if (!F || !X) goto done;
    char p[VFS_MAX_PATH], q[VFS_MAX_PATH], out[VFS_MAX_PATH];

    /* to the bin, and it stays there */
    snprintf(p, sizeof p, "%c:/RBTEST", F); vfs_mkdir(p);
    snprintf(p, sizeof p, "%c:/RBTEST/Sub", F); vfs_mkdir(p);
    snprintf(p, sizeof p, "%c:/RBTEST/deleted.txt", F);
    check(put(p, "This file waits in the Recycle Bin.\r\n"), "create deleted.txt");
    check(rb_supported(F) == 0, "the SATA disk has a bin");
    int id1 = rb_delete(p, 0);
    check(id1 > 0 && file_size(p) == ~0ull, "delete to the bin");

    /* to the bin, the folder deleted for good, restored */
    snprintf(p, sizeof p, "%c:/RBTEST/Sub/restored.txt", F);
    check(put(p, "This file came back from the Recycle Bin.\r\n"), "create restored.txt");
    int id2 = rb_delete(p, 0);
    snprintf(q, sizeof q, "%c:/RBTEST/Sub", F);
    check(id2 > 0 && rb_delete_tree(q) == 0, "delete the folder for good");
    int r = rb_restore(F, (u32)id2, RB_FAIL, out, sizeof out);
    check(r == 0 && !strcmp(out, p) && same_text(p, "This file came back from the Recycle Bin.\r\n"), "restore into a new folder");
    rb_entry_t *e;
    int n = rb_list(F, &e);
    snprintf(q, sizeof q, "%c:/RBTEST/deleted.txt", F);
    check(n == 1 && e[0].id == (u32)id1 && !strcmp(e[0].orig, q), "the bin lists what is left");
    kfree(e);

    /* emptied: the exFAT bin's folder goes */
    snprintf(p, sizeof p, "%c:/RBX", X); vfs_mkdir(p);
    snprintf(p, sizeof p, "%c:/RBX/one.txt", X); put(p, "one");
    snprintf(q, sizeof q, "%c:/RBX/two.txt", X); put(q, "two");
    check(rb_delete(p, 0) > 0 && rb_delete(q, 0) > 0, "delete two files on exFAT");
    int cnt = 0;
    u64 bytes = 0;
    rb_usage(X, &cnt, &bytes);
    check(cnt == 2 && bytes == 6, "exFAT bin usage");
    check(rb_empty(X, NULL, NULL) == 0, "empty the exFAT bin");
    snprintf(p, sizeof p, "%c:/" RB_DIR, X);
    vfs_dirent_t st;
    check(vfs_stat(p, &st) == E_NOENT && file_size(q) == ~0ull, "the emptied bin is gone");

    /* a drag from C: dropped on EXPLORER.EXE showing the FAT32 disk: a copy */
    snprintf(p, sizeof p, "\"%c:/RBTEST\"", F);
    int pid = proc_spawn("C:/PROGRAMS/EXPLORER.EXE", p, NULL, NULL);
    check(pid > 0, "start EXPLORER.EXE");
    find_t f = { "RBTEST", { 0, 0, 0, 0 }, 0 };
    for (int i = 0; i < 100 && !f.found; i++) { thread_sleep_ms(100); gui_call(job_find, &f); }
    check(f.found, "its window");
    if (!f.found) goto done;
    thread_sleep_ms(800);
    static const char src[] = "C:/Documents/Getting Started.txt";
    u64 want = file_size(src);
    drag_t d = { (char *)src, (int)sizeof src, f.client.x + f.client.w * 3 / 4, f.client.y + f.client.h * 3 / 4, 0, 0 };
    gui_call(job_drag, &d);
    check(d.result > 0, "start a drag");
    int eff = -1;
    for (int i = 0; i < 30 && eff != DND_COPY; i++) {          /* the program answers asynchronously */
        d.step = 1;
        d.x += (i & 1) ? 1 : -1;
        gui_call(job_drag, &d);
        eff = d.result;
        thread_sleep_ms(100);
    }
    check(eff == DND_COPY, "the target answers: a copy (another drive)");
    d.step = 2;
    gui_call(job_drag, &d);
    snprintf(p, sizeof p, "%c:/RBTEST/Getting Started.txt", F);
    for (int i = 0; i < 100 && file_size(p) != want; i++) thread_sleep_ms(100);
    thread_sleep_ms(500);
    check(want != ~0ull && file_size(p) == want, "the drop copied the file");
    check(file_size(src) == want, "the source is still there");
done:
    vfs_sync_all();
    kprintf("recycle test: done (%d checks, %d failed)\n", checks, failed);
}

void recycle_test_hook(void) {
    char spec[16];
    if (!qemu_fw_file("opt/halcyon/recycletest", spec, sizeof spec) || !spec[0]) return;
    thread_create("recycle test", test, NULL);
}
