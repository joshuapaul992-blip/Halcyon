/* Halcyon OS - UEFI runtime services: firmware variables (see
 * docs/DESIGN-M8-INSTALL.md). Used by the installer to add a "Halcyon"
 * entry to the firmware's boot menu.
 *
 * The UEFI loader hands the kernel the runtime services table and the
 * memory ranges they need (bootinfo.efi_*). The kernel never calls
 * SetVirtualAddressMap, so the services run in physical mode, on the
 * kernel's identity map: each call runs on a private 64 KiB stack with
 * interrupts off, and a CPU exception inside the firmware ends the call
 * with an error (and disables the services) instead of the system. */
#ifndef EFIRT_H
#define EFIRT_H
#include "types.h"

#define EFI_VAR_NV  0x1
#define EFI_VAR_BS  0x2
#define EFI_VAR_RT  0x4

extern const u8 EFI_GLOBAL_GUID[16];

int  efi_rt_available(void);     /* 1: started by UEFI, runtime services usable */
/* variables; names are ASCII. 0, E_NOENT, E_NOSPC (buffer too small), E_IO */
int  efi_get_variable(const char *name, const u8 guid[16], u32 *attrs, void *buf, u32 *size);
int  efi_set_variable(const char *name, const u8 guid[16], u32 attrs, const void *buf, u32 size);

/* a partition, as a UEFI hard drive device path node describes it */
typedef struct efi_hd {
    u32 part_num;                /* 1-based */
    u64 start, count;            /* sectors */
    u8  sig[16];                 /* GPT: the partition's unique GUID; MBR: the disk signature (first 4 bytes) */
    int gpt;
} efi_hd_t;

/* Add (or update) the boot entry desc -> HD(...)/path ("\EFI\BOOT\BOOTX64.EFI")
 * and put it first in BootOrder. An entry with the same description for the
 * same partition is reused. Returns the Boot#### number or an E_* code;
 * every variable written is read back and compared. */
int  efi_add_boot_entry(const char *desc, const efi_hd_t *hd, const char *path);

/* the boot entry the firmware started Halcyon from (BootCurrent), for the log */
void efi_log_boot_entry(void);

#endif
