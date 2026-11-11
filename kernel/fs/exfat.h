/* Halcyon OS - exFAT driver (interface for vfs.c / block.c / format tools) */
#ifndef EXFAT_H
#define EXFAT_H
#include "fs/vfs.h"

/* VFS probe: mount bd if it holds an exFAT file system */
int exfat_probe(blockdev_t *bd, volume_t *v);

/* sanity check of a boot sector (first 512 bytes of sector 0): nonzero if
 * it looks like an exFAT boot sector usable on a device with dev_sector_size
 * sectors. Used by the block layer to tell an MBR from a superfloppy. */
int exfat_boot_sane(const u8 *sec0, u32 dev_sector_size);

/* Write a new, empty exFAT file system on bd (the whole block device: a
 * partition or a superfloppy disk) through blk_write: main and backup boot
 * regions with checksums, FAT, allocation bitmap, up-case table and a root
 * directory holding the label. The file system sector size is the device's
 * (512..4096 bytes).
 *   label         volume label (UTF-8, at most 11 UTF-16 units; "" or NULL
 *                 for none)
 *   cluster_bytes cluster size, a power of two from the sector size to
 *                 32 MiB; 0 picks the default by volume size, as Windows
 *                 does: 4 KiB up to 256 MiB, 32 KiB up to 32 GiB, 128 KiB
 *                 above
 * The device must not be mounted (unmount the volume first; afterwards
 * vfs_try_mount(bd) mounts the new file system). The volume must be at least
 * 1 MiB. Returns 0 or a negative E_* code (E_INVAL: bad label / cluster size,
 * E_NOSPC: device too small for the requested cluster size, E_ROFS, ...). */
int exfat_format(blockdev_t *bd, const char *label, u32 cluster_bytes);

#endif
