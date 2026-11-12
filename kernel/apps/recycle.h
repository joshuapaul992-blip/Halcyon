/* Halcyon OS - the Recycle Bin (see docs/DESIGN-M8-EXPLORER.md).
 *
 * Every drive that can have one keeps its deleted items in a folder of its
 * own root, X:\RECYCLED (the same name on FAT, exFAT and NTFS; Windows' own
 * \$RECYCLE.BIN is left alone). Deleting moves the item there under a
 * unique name, "D<id>" plus the original extension ("D17.txt"), with a rename
 * on the same volume, so it is instant whatever its size. The index,
 * X:\RECYCLED\INDEX.TXT, says where each item came from:
 *
 *   HALCYON RECYCLE BIN 1
 *   <id> TAB <YYYY-MM-DD HH:MM:SS> TAB <bytes> TAB <F|D> TAB <original path>
 *   ...
 *   END <number of entries>
 *
 * (UTF-8, CRLF; bytes below 0x20 and '%' in the path are written as %XX). It
 * is replaced atomically: written as INDEX.NEW, then renamed over the old
 * one; an INDEX.NEW with its END line is complete and wins over INDEX.TXT.
 * A damaged index is recovered when it is read: lines that do not parse and
 * entries whose item is gone are dropped, and items of the folder named
 * D<id>[.ext] that the index does not list are kept, to be restored to the
 * root of the drive under that name. When the bin becomes empty the folder
 * is removed.
 *
 * The folder holds at most a share of the drive (recycle.percent.<serial> in
 * SETTINGS.INI, else recycle.percent, default 10 %): the oldest items are
 * purged first to make room, and an item larger than the limit is refused
 * (E_NOSPC: the caller offers to delete it permanently).
 *
 * The functions take the storage lock through the VFS and may be called
 * from any kernel thread or system call. */
#ifndef RECYCLE_H
#define RECYCLE_H
#include "core/types.h"
#include "fs/vfs.h"

#define RB_DIR    "RECYCLED"
#define RB_INDEX  "INDEX.TXT"
#define RB_NEW    "INDEX.NEW"
#define RB_DEFAULT_PERCENT 10

typedef struct rb_entry {
    u32 id;                         /* stored as D<id>[.ext] */
    u8  is_dir;
    u64 size;                       /* bytes, a folder's whole contents */
    vfs_time_t when;                /* deleted (local time) */
    char orig[VFS_MAX_PATH];        /* original full path ("C:/Documents/a.txt") */
} rb_entry_t;

/* whether drive letter can have a Recycle Bin: 0, E_NODEV (no such drive),
 * E_ROFS (read-only: nothing can be deleted there at all), E_NOTSUP
 * (removable drives, except the boot drive: deletes there are permanent) */
int  rb_supported(char letter);

/* move path (a file or a folder) to its drive's Recycle Bin. limit: bytes
 * the bin may hold (0 = rb_limit(); ~0 = no limit). Returns the entry's id
 * (> 0), or E_NOSPC (larger than the bin may ever hold), E_INVAL (a drive's
 * root, or something in the bin itself), or the VFS's error. */
int  rb_delete(const char *path, u64 limit);
/* the bin's entries, oldest first (*out kfree()d by the caller, NULL if none);
 * returns the count or an error */
int  rb_list(char letter, rb_entry_t **out);
/* restore an entry to its original path, creating missing parent folders.
 * mode: RB_FAIL (E_EXIST when something is there now), RB_REPLACE (delete
 * that first), RB_RENAME ("name (2).txt"). The path used goes to out. */
enum { RB_FAIL = 0, RB_REPLACE, RB_RENAME };
int  rb_restore(char letter, u32 id, int mode, char *out, size_t outsz);
/* delete one entry for good / every entry of the drive. tick (may be NULL)
 * is called with each name before it is deleted; nonzero cancels (E_CANCELED) */
typedef int (*rb_tick_t)(void *ctx, const char *name, u64 bytes);
int  rb_purge(char letter, u32 id);
int  rb_empty(char letter, rb_tick_t tick, void *ctx);
/* entries and bytes in the drive's bin (0, 0 for none) */
int  rb_usage(char letter, int *count, u64 *bytes);
/* the most the bin of drive letter may hold, in bytes, and its share of the drive in percent */
u64  rb_limit(char letter);
int  rb_percent(char letter);
void rb_percent_key(char letter, char *out, size_t n);   /* "recycle.percent.<serial>" */
/* purge the oldest entries until the bin holds at most limit bytes (keeping
 * keep_id); returns how many were purged */
int  rb_enforce(char letter, u64 limit, u32 keep_id);
/* changes on every change of any bin (the desktop's icon follows it) */
u32  rb_generation(void);

/* the stored name of an entry ("D17.txt") */
void rb_stored_name(const rb_entry_t *e, char *out, size_t n);
/* whether path is a drive's bin folder or inside it */
int  rb_is_bin_path(const char *path);
/* bytes in a file or folder tree */
u64  rb_tree_size(const char *path);
/* delete a file or a folder tree */
int  rb_delete_tree(const char *path);

#endif
