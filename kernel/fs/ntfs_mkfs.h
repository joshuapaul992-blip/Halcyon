/* Halcyon OS - the NTFS formatter (milestone 7.5; see docs/DESIGN-M7.5-FORMAT.md) */
#ifndef FS_NTFS_MKFS_H
#define FS_NTFS_MKFS_H
#include "core/types.h"
#include "fs/block.h"

#define NTFS_MIN_VOLUME   (2u * 1024 * 1024)    /* smaller devices are refused (E_NOSPC) */
#define NTFS_LABEL_MAX    32                    /* UTF-16 units in a volume label */

/* Quick-format bd as an NTFS 3.1 volume. label: UTF-8, up to 32 UTF-16
 * units, may be NULL or "". cluster_bytes: 512 .. 65536, a power of two and
 * at least the sector size, or 0 for the default (4 KiB up to 16 TiB, then
 * as Windows chooses). Returns 0 or E_INVAL (bad label / cluster size, or a
 * volume needing more than 2^32 - 1 clusters), E_NOSPC (too small),
 * E_NOTSUP (sector size), E_ROFS, E_NOMEM, E_IO. */
int  ntfs_format(blockdev_t *bd, const char *label, u32 cluster_bytes);

/* the cluster size ntfs_format chooses for a volume of `bytes` bytes on
 * sectors of `sector_size` bytes (0: too large for NTFS) */
u32  ntfs_default_cluster(u64 bytes, u32 sector_size);

/* fill t[65536] with the $UpCase table the formatter writes (ntfs_upcase.c) */
void ntfs_upcase_table(u16 *t);

#endif
