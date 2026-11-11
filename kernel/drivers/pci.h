#ifndef PCI_H
#define PCI_H
#include "core/types.h"

typedef struct pci_bar {
    u64 addr;      /* physical address (memory) or port base (I/O) */
    u64 size;
    u8  is_io;
    u8  is_64;
    u8  prefetch;
} pci_bar_t;

typedef struct pci_dev {
    u8  bus, dev, func;
    u16 vendor, device;
    u8  class_code, subclass, prog_if, revision;
    u8  header_type;
    u8  irq_line;
    u16 subsys_vendor, subsys_id;
    pci_bar_t bar[6];
    const char *driver;     /* name of the driver that claimed it, or NULL */
} pci_dev_t;

void pci_init(void);
int  pci_count(void);
pci_dev_t *pci_get(int index);
/* iterate devices by class: pass prog_if = -1 to match any */
pci_dev_t *pci_find_class(int class_code, int subclass, int prog_if, int index);

u32  pci_read32(pci_dev_t *d, u8 off);
u16  pci_read16(pci_dev_t *d, u8 off);
u8   pci_read8(pci_dev_t *d, u8 off);
void pci_write32(pci_dev_t *d, u8 off, u32 v);
void pci_write16(pci_dev_t *d, u8 off, u16 v);
void pci_write8(pci_dev_t *d, u8 off, u8 v);

/* enable memory + I/O decoding and bus mastering; disable INTx */
void pci_enable(pci_dev_t *d);
/* map a memory BAR (handles 64-bit BARs above 4 GiB); returns virtual address */
void *pci_map_bar(pci_dev_t *d, int bar);

const char *pci_class_name(u8 cls, u8 sub, u8 progif);
const char *pci_vendor_name(u16 vendor);

#endif
