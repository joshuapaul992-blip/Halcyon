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

struct kernel_header {
    uint32_t magic;       /* KERNEL_MAGIC */
    uint32_t version;     /* 1 */
    uint64_t load_addr;   /* physical load address */
    uint64_t file_size;   /* bytes of the image file */
    uint64_t bss_end;     /* physical end of memory used by the kernel */
    uint64_t entry;       /* physical entry point */
} __attribute__((packed));

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
    uint32_t pad2;
    struct bootinfo_mmap mmap[BOOTINFO_MAX_MMAP];
} __attribute__((packed));

#endif
