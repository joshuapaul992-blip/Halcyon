/* Halcyon OS - NTFS file system driver (read and write, milestone 7.5).
 * See docs/DESIGN-M7.5.md. */
#ifndef FS_NTFS_H
#define FS_NTFS_H
#include "core/types.h"
#include "fs/block.h"
#include "fs/vfs.h"

int ntfs_probe(blockdev_t *bd, volume_t *v);

/* nonzero if sec0 (the first 512 bytes of a volume) is a plausible NTFS
 * boot sector (used by the block layer to tell it from an MBR) */
int ntfs_boot_sane(const u8 *sec0, u32 dev_sector_size);

/* Re-apply the "internal Windows disk" rule of every mounted NTFS volume
 * after SETTINGS.INI was loaded or changed (ntfs.write.<serial> = 1 allows
 * writing). Called by kmain after settings_load(). */
void ntfs_settings_changed(void);

/* the settings key that allows writing to the internal NTFS volume with
 * this serial number: "ntfs.write.<16 uppercase hex digits>" */
void ntfs_setting_key(u64 serial, char *out, int outsz);

#endif
