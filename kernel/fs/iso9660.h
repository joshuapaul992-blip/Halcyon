/* Halcyon OS - ISO 9660 driver (private interface for vfs.c / block.c) */
#ifndef ISO9660_H
#define ISO9660_H
#include "fs/vfs.h"

/* VFS probe: mount bd (read-only) if it holds an ISO 9660 file system
 * (volume descriptors at byte 32768, any device sector size) */
int iso9660_probe(blockdev_t *bd, volume_t *v);

/* nonzero if bd has an ISO 9660 volume descriptor at byte 32768. Used by
 * the block layer to recognise hybrid ISO images (isohybrid, xorriso
 * --protective-msdos-label) whose partition table describes the image itself. */
int iso9660_detect(blockdev_t *bd);

#endif
