/* Halcyon OS - FAT12/16/32 driver (private interface for vfs.c / block.c) */
#ifndef FAT_H
#define FAT_H
#include "fs/vfs.h"

/* VFS probe: mount bd if it holds a FAT file system */
int fat_probe(blockdev_t *bd, volume_t *v);

/* sanity check of a boot sector (first 512 bytes of sector 0): nonzero if
 * it looks like a FAT BPB usable on a device with dev_sector_size sectors.
 * Used by the block layer to tell an MBR from a superfloppy boot sector. */
int fat_bpb_sane(const u8 *sec0, u32 dev_sector_size);

/* Write a new, empty FAT file system on bd (a partition or a whole disk
 * without a partition table). type 12/16/32 or 0 for automatic; cluster_bytes
 * 0 for the Windows default. The chosen type is returned in *type_out. */
int fat_format(blockdev_t *bd, int type, const char *label, u32 cluster_bytes, int *type_out);

#endif
