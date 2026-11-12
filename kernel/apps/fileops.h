/* Halcyon OS - shared file helpers for the shell applications */
#ifndef FILEOPS_H
#define FILEOPS_H
#include "core/types.h"
#include "fs/vfs.h"

int  fileops_delete_tree(const char *path);                 /* file or directory (recursive) */
int  fileops_copy_tree(const char *src, const char *dst);   /* file or directory (recursive) */
int  fileops_unique_name(const char *dir, const char *base, char *out, int outsz);  /* "Copy of x", "x (2)" */
/* Copy / move / delete with progress, run by a worker thread (see
 * progress.c). The fields are read by the GUI thread while it runs. */
enum { FOP_COPY = 1, FOP_MOVE, FOP_DELETE };
typedef struct fileop {
    int kind;
    char src[VFS_MAX_PATH], dst[VFS_MAX_PATH];
    volatile int cancel;            /* set by the dialog */
    volatile int done;              /* set by the worker */
    int result;                     /* 0 or E_* (E_CANCELED) */
    int counting;                   /* still adding up the work */
    u64 total_bytes, done_bytes;
    u32 total_items, done_items;
    char current[VFS_MAX_NAME + 1]; /* name being worked on */
    u64 start_ms;
} fileop_t;
void fileop_execute(fileop_t *op);
/* run op in a worker thread behind a progress dialog (modal for owner, the
 * rest of the desktop keeps working); returns op->result */
struct window;
int  fileop_run(struct window *owner, fileop_t *op);

const char *file_type_name(const char *name, int is_dir);
int  file_icon(const char *name, int is_dir);
void format_size(u64 bytes, char *out, int outsz);           /* "1.5 MB" */
void format_size_exact(u64 bytes, char *out, int outsz);     /* "1,572,864 bytes" */
void format_time(const vfs_time_t *t, char *out, int outsz); /* "11/11/2026 7:23 AM" */
int  drive_icon(volume_t *v);
const char *drive_kind(volume_t *v);                         /* "Local Disk", "Removable Disk" */

/* driveprops.c: "Allow Halcyon to write to this drive" (NTFS on internal
 * disks). The key in SETTINGS.INI is ntfs.write.<serial, 16 uppercase hex
 * digits> = 1. volume_write_option: the volume offers the choice (the
 * driver set ro_policy, or writing is allowed already). volume_set_write
 * saves the choice and mounts the volume again: 0, E_BUSY (open files; then
 * nothing changes), E_NOTSUP (no such choice for this volume), or a mount
 * error. */
void ntfs_write_key(u64 serial, char *out, size_t n);
int  volume_write_allowed(const volume_t *v);
int  volume_write_option(const volume_t *v);
int  volume_set_write(char letter, int allow);
const char *volume_write_warning(void);
int  is_text_file(const char *name);

#endif
