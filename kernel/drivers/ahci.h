/* Halcyon OS - AHCI (SATA) disk driver
 *
 * ahci_init() probes every PCI AHCI controller (class 01.06.01), brings up
 * all implemented ports and registers one blockdev_t ("sata0", "sata1", ...)
 * per ATA disk found. ATAPI CD/DVD drives get a blockdev "cd0", "cd1", ...
 * (BDEV_DISK_CD, read-only) that is registered while a data disc is in the
 * drive. Port multipliers and enclosure bridges are logged and skipped.
 * Interrupts only wake the main loop; device state is polled.
 */
#ifndef AHCI_H
#define AHCI_H
#include "core/types.h"
#include "fs/block.h"

void ahci_init(void);
/* number of SATA disks registered by ahci_init() */
int  ahci_disk_count(void);

/* CD/DVD drives: media change detection, called from the main loop (cheap
 * when there is no CD drive; asks the idle loop to wake for the next poll) */
void ahci_poll(void);
int  ahci_cd_count(void);
/* the drive's blockdev (name, model filled in; present only with a disc) */
blockdev_t *ahci_cd_get(int i);
/* unmount the disc of the drive holding bd (the CD or a volume on it) and
 * open the tray */
int  ahci_cd_eject(blockdev_t *bd);

#endif
