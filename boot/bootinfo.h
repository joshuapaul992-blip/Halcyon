/* Halcyon OS - boot information handed from the loaders (BIOS stage2 / UEFI)
 * to the kernel. Shared by boot/stage2.c, efi/ and kernel/.
 *
 * Contract:
 *   - The kernel flat binary (KERNEL.BIN) starts with struct kernel_header
 *     and must be loaded at physical address header.load_addr (0x100000).
 *   - Memory [load_addr, bss_end) must be free RAM; the loader zeroes
 *     [load_addr + file_size, bss_end).
 *   - The loader jumps to header.entry in 64-bit long mode, interrupts off,
 *     with at least the first 4 GiB identity-mapped, RDI = pointer to a
 *     struct bootinfo (anywhere below 4 GiB; the kernel copies it at once).
 */
#ifndef HALCYON_BOOTINFO_H
#define HALCYON_BOOTINFO_H

#include <stdint.h>

#define KERNEL_MAGIC     0x4E524B48u              /* "HKRN" */
#define BOOTINFO_MAGIC   0x214E4F59434C4148ull    /* "HALCYON!" */

#define BOOT_TYPE_BIOS 1
#define BOOT_TYPE_UEFI 2

#define MMAP_USABLE   1
#define MMAP_RESERVED 2
#define MMAP_ACPI     3
#define MMAP_NVS      4
#define MMAP_BAD      5

#define BOOTINFO_MAX_MMAP 256

/* boot_flags: set by the loaders' F8 "Advanced Startup Options" menu */
#define BOOTF_VERBOSE   0x0001   /* show the kernel log on screen while booting */
#define BOOTF_NO_XHCI   0x0002   /* don't start USB 3.x controllers */
#define BOOTF_NO_EHCI   0x0004   /* don't start USB 2.0 controllers */
#define BOOTF_NO_USB11  0x0008   /* don't start UHCI/OHCI controllers */
#define BOOTF_NO_AHCI   0x0010   /* don't start SATA controllers */
#define BOOTF_SAFE_VIDEO 0x0020  /* loader picked a conservative 800x600 mode */
#define BOOTF_NO_ACPI   0x0040   /* don't parse ACPI tables */
#define BOOTF_NO_IRQ    0x0080   /* no interrupts: everything polled, no CPU idle */
#define BOOTF_NO_MSI    0x0100   /* PCI devices use INTx (or polling), never MSI */
#define BOOTF_NO_IOAPIC 0x0200   /* legacy 8259 PICs instead of the I/O APIC */
#define BOOTF_NO_NVME   0x0400   /* don't start NVMe controllers */
#define BOOTF_SAFE_MODE (BOOTF_VERBOSE | BOOTF_NO_XHCI | BOOTF_NO_EHCI | BOOTF_NO_USB11 | BOOTF_NO_AHCI | \
                         BOOTF_NO_NVME | BOOTF_NO_ACPI | BOOTF_NO_IRQ)

struct kernel_header {
    uint32_t magic;       /* KERNEL_MAGIC */
    uint32_t version;     /* 1 */
    uint64_t load_addr;   /* physical load address */
    uint64_t file_size;   /* bytes of the image file */
    uint64_t bss_end;     /* physical end of memory used by the kernel */
    uint64_t entry;       /* physical entry point */
} __attribute__((packed));

#define BOOTINFO_MAX_MODES 64
struct bootinfo_mode {
    uint16_t width, height;
    uint16_t bpp;              /* 32 or 24 */
    uint16_t id;               /* VBE mode number or GOP mode index */
} __attribute__((packed));

#define MODE_BY_DEFAULT  0     /* no setting, no EDID: the loader's default */
#define MODE_BY_SETTING  1     /* display.width / display.height in SETTINGS.INI */
#define MODE_BY_EDID     2     /* the monitor's native (preferred) mode */
#define MODE_BY_SAFE     3     /* F8 safe video mode */
#define MODE_BY_FIRMWARE 4     /* kept the mode the firmware had set */

struct bootinfo_mmap {
    uint64_t base;
    uint64_t length;
    uint32_t type;        /* MMAP_* */
    uint32_t reserved;
} __attribute__((packed));

struct bootinfo {
    uint64_t magic;            /* BOOTINFO_MAGIC */
    uint32_t boot_type;        /* BOOT_TYPE_* */
    uint32_t bios_drive;       /* BIOS boot drive (DL) or 0 */

    uint64_t fb_addr;          /* linear framebuffer physical address */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;         /* bytes per scanline */
    uint32_t fb_bpp;           /* 32 or 24 */
    uint8_t  fb_red_pos,   fb_red_size;
    uint8_t  fb_green_pos, fb_green_size;
    uint8_t  fb_blue_pos,  fb_blue_size;
    uint8_t  fb_pad[2];

    uint64_t rsdp;             /* ACPI RSDP physical address or 0 */
    uint32_t boot_disk_sig;    /* MBR disk signature of the boot disk (0 if unknown) */
    uint32_t boot_part_lba;    /* first LBA of the boot partition (0 if unknown) */

    uint32_t mmap_count;
    uint32_t boot_flags;       /* BOOTF_* chosen in the F8 Advanced Startup Options menu */
    struct bootinfo_mmap mmap[BOOTINFO_MAX_MMAP];

    /* display: every usable mode the firmware offers (VBE / GOP), the monitor's
     * EDID if the firmware could read it, and why the mode was chosen.
     * fb_* above always describe the mode in use. */
    uint32_t mode_count;
    uint32_t mode_current;     /* index into modes[], or BOOTINFO_MAX_MODES if not listed */
    struct bootinfo_mode modes[BOOTINFO_MAX_MODES];
    uint32_t edid_size;        /* 0 = no EDID; else 128 (base block only) */
    uint8_t  edid[128];
    uint32_t mode_reason;      /* MODE_BY_* */
} __attribute__((packed));

#endif
