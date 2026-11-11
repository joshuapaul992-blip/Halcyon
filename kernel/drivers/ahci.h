/* Halcyon OS - AHCI (SATA) disk driver
 *
 * ahci_init() probes every PCI AHCI controller (class 01.06.01), brings up
 * all implemented ports and registers one blockdev_t ("sata0", "sata1", ...)
 * per ATA disk found. ATAPI devices, port multipliers and enclosure bridges
 * are logged and skipped. The driver is fully polled (no interrupts).
 */
#ifndef AHCI_H
#define AHCI_H
#include "core/types.h"

void ahci_init(void);
/* number of SATA disks registered by ahci_init() */
int  ahci_disk_count(void);

#endif
