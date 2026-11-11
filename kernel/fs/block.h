/* Halcyon OS - block device layer
 *
 * Disk drivers (AHCI, USB mass storage, ...) fill in a blockdev_t and call
 * blockdev_register(). The block layer then:
 *   - scans the partition table (MBR incl. extended partitions, or GPT);
 *     each partition becomes a child blockdev_t of type BDEV_PART
 *   - if no partition table is found, treats the whole disk as a
 *     "superfloppy" volume
 *   - offers every partition/volume to the file system drivers
 *     (vfs_try_mount), which assign a drive letter.
 * On hot-unplug the driver calls blockdev_unregister(), which unmounts all
 * volumes on the disk and drops the partitions. After unregister the driver
 * may free the blockdev_t.
 *
 * Driver contract for read/write:
 *   - must accept any count >= 1 (split internally if the hardware has limits)
 *   - buf is any kernel heap or stack pointer (identity mapped, below 4 GiB
 *     for heap memory; drivers needing alignment must bounce internally)
 *   - return 0 on success or a negative E_* code
 *   - may be called with count*sector_size up to at least 1 MiB
 */
#ifndef BLOCK_H
#define BLOCK_H
#include "core/types.h"

enum { BDEV_DISK_USB = 1, BDEV_DISK_SATA, BDEV_DISK_RAM, BDEV_PART };

typedef struct blockdev blockdev_t;
struct blockdev {
    char name[16];          /* "usb0", "sata1", "usb0p1" (set by driver; partitions by block layer) */
    char model[48];         /* human readable ("QEMU HARDDISK", "SanDisk Cruzer") */
    int  type;              /* BDEV_* */
    u32  sector_size;       /* bytes, normally 512 */
    u64  sector_count;
    u8   removable;
    u8   readonly;
    u8   present;           /* cleared by blockdev_unregister */
    u8   reserved;

    int (*read)(blockdev_t *bd, u64 lba, u32 count, void *buf);
    int (*write)(blockdev_t *bd, u64 lba, u32 count, const void *buf);
    void *priv;             /* driver private */

    /* filled in by the block layer */
    blockdev_t *parent;     /* for BDEV_PART: the whole disk */
    u64  part_start;        /* for BDEV_PART: first LBA on the parent */
    int  part_index;        /* 1-based partition number */
    u8   part_type;         /* MBR type byte (0xEE-ish types for GPT: 0) */
    u32  disk_sig;          /* MBR disk signature of the disk (whole-disk devices) */
    blockdev_t *next;       /* registry list */
};

void blockdev_init(void);
int  blockdev_register(blockdev_t *bd);
void blockdev_unregister(blockdev_t *bd);

/* bounds-checked I/O (works for disks and partitions) */
int blk_read(blockdev_t *bd, u64 lba, u32 count, void *buf);
int blk_write(blockdev_t *bd, u64 lba, u32 count, const void *buf);

/* iteration over registered devices (disks and partitions) */
blockdev_t *blockdev_first(void);
int blockdev_count(void);

#endif
