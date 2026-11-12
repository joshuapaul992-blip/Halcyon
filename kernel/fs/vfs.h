/* Halcyon OS - virtual file system
 *
 * Paths are DOS style with drive letters: "C:/Documents/readme.txt", in UTF-8.
 * Both '/' and '\\' are accepted as separators, names are case-insensitive,
 * "." and ".." components are resolved. A path without a drive letter is
 * invalid (E_INVAL). "C:" and "C:/" both name the root directory.
 *
 * Drive letters: the boot volume (the partition we were booted from) is C:.
 * Other volumes get the first free letter from D: upwards, in the order they
 * are mounted. Letters are released on unmount.
 *
 * All functions return 0 / a byte count on success and a negative E_* code
 * (core/types.h) on failure.
 */
#ifndef VFS_H
#define VFS_H
#include "core/types.h"
#include "fs/block.h"

#define VFS_MAX_PATH 520          /* bytes of UTF-8 */
#define VFS_MAX_NAME 255          /* bytes of UTF-8 */

/* open flags */
#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_RDWR   (VFS_O_READ | VFS_O_WRITE)
#define VFS_O_CREATE 0x04    /* create if missing */
#define VFS_O_TRUNC  0x08    /* truncate to 0 on open */
#define VFS_O_APPEND 0x10    /* every write goes to the end */
#define VFS_O_EXCL   0x20    /* with CREATE: fail with E_EXIST if it exists */

/* FAT-style attributes */
#define VFS_ATTR_READONLY 0x01
#define VFS_ATTR_HIDDEN   0x02
#define VFS_ATTR_SYSTEM   0x04
#define VFS_ATTR_VOLUME   0x08
#define VFS_ATTR_DIR      0x10
#define VFS_ATTR_ARCHIVE  0x20

typedef struct vfs_time {
    u16 year; u8 month, day, hour, minute, second;
} vfs_time_t;

typedef struct vfs_dirent {
    char name[VFS_MAX_NAME + 1];   /* long name if present, else 8.3 name (UTF-8) */
    char short_name[13];           /* 8.3 name "README.TXT" */
    u64 size;
    u8  attr;                      /* VFS_ATTR_* */
    u8  is_dir;
    vfs_time_t mtime;
    vfs_time_t ctime;
} vfs_dirent_t;

typedef struct volume volume_t;
typedef struct fs_ops fs_ops_t;

struct volume {
    char letter;                   /* 'C' ... 'Z' */
    char label[64];                /* volume label (UTF-8, up to 32 characters on NTFS), trimmed, or "" */
    char fs_name[8];               /* "FAT12" / "FAT16" / "FAT32" / "exFAT" / "ISO9660" / "NTFS" */
    blockdev_t *bd;                /* partition or whole disk */
    const fs_ops_t *ops;
    void *fs_priv;
    u8 alive;                      /* cleared on unmount; stale handles fail with E_NODEV */
    u8 is_boot;
    u8 readonly;                   /* mounted read-only by the driver: every change fails with E_ROFS */
    u8 ro_policy;                  /* read-only only because writing needs the user's permission
                                      (NTFS: an internal disk with Windows on it, no ntfs.write.<serial>
                                      in SETTINGS.INI): Explorer then offers "Allow Halcyon to write" */
    u32 open_files;
    char ro_reason[128];           /* why it is read-only, for Explorer ("" if writable or unknown) */
    u64 serial;                    /* the volume's serial number (0 = none), e.g. for per-volume settings */
};

/* Interface implemented by file system drivers. Paths passed to the driver
 * are relative to the volume root, '/' separated, already normalised, no
 * leading '/' ("" = root, "DIR/SUB/file.txt"). */
struct fs_ops {
    const char *name;
    int (*unmount)(volume_t *v);
    int (*open)(volume_t *v, const char *path, int flags, void **fh);
    int (*close)(volume_t *v, void *fh);
    int (*read)(volume_t *v, void *fh, u64 pos, void *buf, u32 len);         /* returns bytes */
    int (*write)(volume_t *v, void *fh, u64 pos, const void *buf, u32 len);  /* returns bytes */
    int (*truncate)(volume_t *v, void *fh, u64 size);
    u64 (*size)(volume_t *v, void *fh);
    /* call cb for each entry except "." and ".." and the volume label; stop if cb returns nonzero */
    int (*readdir)(volume_t *v, const char *path, int (*cb)(void *ctx, const vfs_dirent_t *e), void *ctx);
    int (*stat)(volume_t *v, const char *path, vfs_dirent_t *out);
    int (*mkdir)(volume_t *v, const char *path);
    int (*unlink)(volume_t *v, const char *path);         /* file or empty directory */
    int (*rename)(volume_t *v, const char *from, const char *to);  /* may move between dirs */
    int (*statfs)(volume_t *v, u64 *total_bytes, u64 *free_bytes);
    int (*sync)(volume_t *v);
};

/* A file system driver probe: return 0 and fill v->ops/fs_priv/fs_name/label
 * if the blockdev holds a file system it understands. */
typedef int (*fs_probe_fn)(blockdev_t *bd, volume_t *v);

void vfs_init(void);
/* called by the block layer for every partition / superfloppy disk */
int  vfs_try_mount(blockdev_t *bd);
/* called by the block layer before a disk disappears */
void vfs_unmount_blockdev(blockdev_t *bd);

/* volumes */
int        vfs_volume_count(void);
volume_t  *vfs_volume(int index);          /* ordered by drive letter */
volume_t  *vfs_volume_by_letter(char letter);
int        vfs_statfs(char letter, u64 *total, u64 *free_bytes);
u32        vfs_generation(void);           /* increments on every mount/unmount */

/* files */
typedef struct vfs_file vfs_file_t;
int  vfs_open(const char *path, int flags, vfs_file_t **out);
int  vfs_read(vfs_file_t *f, void *buf, u32 len);
int  vfs_write(vfs_file_t *f, const void *buf, u32 len);
int  vfs_seek(vfs_file_t *f, u64 pos);
u64  vfs_tell(vfs_file_t *f);
u64  vfs_size(vfs_file_t *f);
int  vfs_truncate(vfs_file_t *f, u64 size);
int  vfs_close(vfs_file_t *f);

/* directories: list all entries of a directory into a heap array.
 * *out must be kfree()d by the caller. Returns the entry count. */
int  vfs_list(const char *path, vfs_dirent_t **out);
int  vfs_stat(const char *path, vfs_dirent_t *out);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_rename(const char *from, const char *to);   /* same volume only (E_NOTSUP otherwise) */
int  vfs_copy(const char *from, const char *to);     /* file copy, any volumes */
int  vfs_sync_all(void);

/* Unmount a volume and mount it again on the same drive letter (after a
 * setting that changes how it mounts, e.g. allowing writes to an NTFS
 * volume). E_BUSY while it has open files or is the boot volume. */
int  vfs_remount(char letter);

/* Quick-format bd (a partition, or a disk without a partition table) with
 * fs = "FAT" (automatic), "FAT12", "FAT16", "FAT32", "exFAT" or "NTFS", and
 * mount it again (with its old drive letter; if no driver can mount the new
 * file system it still returns 0, and no volume appears). Refuses the boot
 * volume, volumes with open files and partitioned disks (E_BUSY).
 * cluster_bytes 0 = default. */
int  vfs_format(blockdev_t *bd, const char *fs, const char *label, u32 cluster_bytes);

/* convenience: read a whole file into a NUL-terminated heap buffer */
int  vfs_read_file(const char *path, char **data, u32 *size);
int  vfs_write_file(const char *path, const void *data, u32 size);

/* path helpers */
/* normalise "c:\\a\\..\\B/./x" -> "C:/B/x"; returns 0 or E_INVAL */
int  vfs_normalize(const char *in, char *out, size_t outsz);
/* join dir + name into out ("C:/A" + "b.txt" -> "C:/A/b.txt") */
void vfs_join(const char *dir, const char *name, char *out, size_t outsz);
/* pointer to the last component ("C:/A/b.txt" -> "b.txt") */
const char *vfs_basename(const char *path);
/* parent directory ("C:/A/b.txt" -> "C:/A", "C:/A" -> "C:/") */
void vfs_dirname(const char *path, char *out, size_t outsz);

#endif
