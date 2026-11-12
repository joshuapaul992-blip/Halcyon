/* Halcyon OS - partition tables for the installer (see docs/DESIGN-M8-INSTALL.md)
 *
 * Reads an MBR (primary and logical partitions) or GPT disk into a list of
 * partitions and the free space between them, writes new tables (an MBR, or
 * a GPT: protective MBR, both headers and both entry arrays with their
 * CRC32s), and adds partitions to an existing table while every other byte
 * of it stays as it was (the entries of other systems are never rewritten).
 *
 * Pure code: all I/O goes through pt_io_t, so the host tests
 * (tests/host/parttest.c) run it on image files and check the results with
 * sfdisk, sgdisk and an independent checker (tests/host/gpt_check.py).
 * All functions return 0 or a negative E_* code.
 */
#ifndef PARTTAB_H
#define PARTTAB_H
#include "core/types.h"

#define PT_MAX_PARTS 128          /* GPT entries looked at (and MBR logical partitions) */
#define PT_GPT_ENTRIES 128        /* entries in a new GPT (16 KiB array) */

enum { PT_NONE = 0,               /* blank: no boot signature, nothing recognizable */
       PT_MBR = 1, PT_GPT = 2,
       PT_OTHER = 3 };            /* something else: a file system on the whole disk, a damaged table */

typedef struct pt_io {
    void *ctx;
    u32 sector_size;              /* 512 ... 4096 */
    u64 sectors;
    int (*read)(void *ctx, u64 lba, u32 count, void *buf);
    int (*write)(void *ctx, u64 lba, u32 count, const void *buf);
} pt_io_t;

typedef struct pt_part {
    u64 start, count;             /* sectors */
    int index;                    /* MBR: 1-4 primary, 5... logical; GPT: entry number (1-based) */
    u8 mbr_type, active;          /* MBR */
    u8 logical, extended;         /* MBR: inside an extended partition / the extended container */
    u8 type_guid[16], guid[16];   /* GPT, as stored on disk */
    u64 attrs;                    /* GPT attribute bits */
    char name[76];                /* GPT partition name in UTF-8 */
} pt_part_t;

typedef struct pt_table {
    int kind;                     /* PT_* */
    char problem[120];            /* PT_OTHER: what is wrong / what is there */
    u32 ss;
    u64 sectors;
    u8 mbr[512];                  /* sector 0 as read (first 512 bytes) */
    u32 disk_sig;                 /* MBR disk signature (bytes 440..443) */
    int free_slots;               /* MBR: empty primary entries */
    int has_extended;
    /* GPT */
    u8 disk_guid[16];
    u64 first_usable, last_usable, alt_lba, ent_lba;
    u32 nent, esz;
    int free_entries;             /* empty GPT entries */
    int nparts;
    pt_part_t parts[PT_MAX_PARTS + 4];
} pt_table_t;

typedef struct pt_region { u64 start, count; } pt_region_t;

/* a partition to create */
typedef struct pt_new {
    u64 start, count;
    u8 mbr_type, active;          /* MBR */
    u8 type_guid[16], guid[16];   /* GPT */
    const char *name;             /* GPT (ASCII / UTF-8 BMP), may be NULL */
} pt_new_t;

int pt_read(const pt_io_t *io, pt_table_t *t);

/* Unpartitioned space usable for new partitions, aligned to align sectors
 * at both ends, largest first; at most max regions; returns the count.
 * MBR: below 2^32 sectors, outside every primary and extended partition,
 * and only if a primary entry is free. GPT: between the usable LBAs, if an
 * entry is free. */
int pt_free_regions(const pt_table_t *t, u64 align, pt_region_t *out, int max);

/* A new table on the whole disk (what was there is gone). boot440: MBR
 * boot code (bytes 0..439) or NULL for none. The GPT gets PT_GPT_ENTRIES
 * entries right after its header and a backup at the end of the disk, and
 * a protective MBR whose disk signature is 0 (the UEFI specification). */
int pt_write_mbr(const pt_io_t *io, const pt_new_t *p, int n, u32 disk_sig, const u8 *boot440);
int pt_write_gpt(const pt_io_t *io, const pt_new_t *p, int n, const u8 disk_guid[16], const u8 *boot440);

/* Add partitions to the table t was read from (it is read again and must
 * not have changed). MBR: one primary partition in a free entry (*index_out
 * = its number 1-4). GPT: n partitions in the first free entries
 * (index_out[i] = their numbers); both headers are rewritten with the
 * new entry array CRC, nothing else changes. */
int pt_add_mbr(const pt_io_t *io, const pt_table_t *t, const pt_new_t *p, int *index_out);
int pt_add_gpt(const pt_io_t *io, const pt_table_t *t, const pt_new_t *p, int n, int *index_out);

/* MBR boot code: bytes 0..439 of sector 0 replaced (the disk signature
 * and partition table stay) */
int pt_write_bootcode(const pt_io_t *io, const u8 *boot440);

/* type GUIDs (on-disk byte order) */
extern const u8 PT_GUID_ESP[16];          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const u8 PT_GUID_BIOS_BOOT[16];    /* 21686148-6449-6E6F-744E-656564454649 */
extern const u8 PT_GUID_BASIC_DATA[16];   /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
extern const u8 PT_GUID_MS_RESERVED[16];  /* E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
extern const u8 PT_GUID_WIN_RECOVERY[16]; /* DE94BBA4-06D1-4D40-A16A-BFD50179D6AC */

u32  pt_crc32(const void *p, size_t n);
void pt_guid_v4(u8 g[16], const u8 random16[16]);      /* a random (version 4) GUID */
void pt_guid_str(const u8 g[16], char out[37]);        /* "C12A7328-F81F-11D2-..." */
int  pt_guid_zero(const u8 g[16]);
/* a short description of a partition's type ("NTFS / exFAT", "EFI system", ...) */
const char *pt_type_name(int kind, const pt_part_t *p);

#endif
