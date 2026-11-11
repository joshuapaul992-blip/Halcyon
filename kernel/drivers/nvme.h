/* Halcyon OS - NVMe (NVM Express) disk driver
 *
 * nvme_init() probes every PCI NVMe controller (class 01.08.02), resets and
 * enables it, and registers one blockdev_t per active namespace with an LBA
 * size of 512 to 4096 bytes. Disks are named "nvme0", "nvme1", ... across all
 * controllers and namespaces (partitions become "nvme0p1", ...).
 * nvme_shutdown() sends the controllers the shutdown notification (CC.SHN);
 * reboot() and shutdown() call it.
 */
#ifndef NVME_H
#define NVME_H
#include "core/types.h"

void nvme_init(void);
/* number of NVMe disks (namespaces) registered by nvme_init() */
int  nvme_disk_count(void);
/* flush, delete the I/O queues and send the normal shutdown notification */
void nvme_shutdown(void);

#endif
