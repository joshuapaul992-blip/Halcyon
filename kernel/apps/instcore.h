/* Halcyon OS - the installer's engine: which disks can take Halcyon, the
 * plan for one, and the installation itself (partition, format, boot
 * code, copy, verify, firmware boot entry). The wizard (apps/installer.c)
 * and its QEMU test hook drive it. See docs/DESIGN-M8-INSTALL.md. */
#ifndef INSTCORE_H
#define INSTCORE_H
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/parttab.h"

#define INST_MIN_BYTES (128ull << 20)   /* the smallest Halcyon partition */
#define INST_MAX_DISKS 8
#define INST_MAX_VOLS  24

typedef struct inst_vol {             /* a partition, as the wizard shows it */
    u64 start, count;
    int index;
    char letter;                      /* drive letter in Halcyon, 0 if not mounted */
    char fs[8], label[64];
    const char *type;                 /* "NTFS / exFAT", "EFI system partition", ... */
    u8 windows;                       /* \Windows\System32 */
    u8 winboot;                       /* \EFI\Microsoft\Boot\bootmgfw.efi (Windows Boot Manager) */
    u8 active, logical, extended;
} inst_vol_t;

typedef struct inst_disk {
    blockdev_t *bd;
    int usable;                       /* can be chosen */
    char why[160];                    /* if not: why */
    pt_table_t *pt;                   /* (heap) */
    int nvols;
    inst_vol_t vols[INST_MAX_VOLS];
    int windows;                      /* Windows (or its boot manager) is on the disk */
    int busy;                         /* a volume on it has open files */
    u64 free_start, free_count;       /* the largest free region (sectors, 1 MiB aligned) */
    int free_ok;
    char free_why[200];
} inst_disk_t;

enum { INST_WHOLE = 1, INST_FREE = 2 };

typedef struct inst_plan {
    blockdev_t *disk;
    int mode;                         /* INST_* */
    int gpt;                          /* the table: GPT, else MBR */
    u64 start, count;                 /* Halcyon's partition */
    int bios;                         /* Halcyon's MBR boot code and stage 2 are installed */
    u64 stage2_lba;
    u64 bb_start, bb_count;           /* GPT: the BIOS boot partition (0 = none) */
    int uefi_entry;                   /* register "Halcyon" with the firmware */
    char bios_note[200];              /* why BIOS cannot start Halcyon from this disk ("" if it can) */
    u64 copy_bytes;                   /* what will be copied */
    u32 copy_files;
} inst_plan_t;

enum { IS_PREPARE = 0, IS_PARTITION, IS_FORMAT, IS_BOOTCODE, IS_COPY, IS_VERIFY, IS_FIRMWARE, IS_DONE, IS_COUNT };

typedef struct inst_job {
    inst_plan_t plan;
    volatile int step, done, result;
    char status[160];
    char current[VFS_MAX_PATH + 1];   /* the file being copied / verified */
    u64 total_bytes, done_bytes;      /* copy, then verify */
    u32 files, dirs;
    char error[240];
    char letter;                      /* the new volume's drive letter */
    blockdev_t *part;
    int boot_entry;                   /* Boot#### number, or an E_* code; -1000 = not tried */
    char warning[320];
} inst_job_t;

/* The disks the wizard lists: internal SATA and NVMe disks (never the one
 * Halcyon started from), with what is on them. Returns the count. */
int  inst_survey(inst_disk_t *d, int max);
void inst_survey_free(inst_disk_t *d, int n);
/* 0, or an E_* code with why[] telling the user what is wrong */
int  inst_plan(const inst_disk_t *d, int mode, inst_plan_t *p, char *why, int whysz);
/* the installation; blocks (run it on a kernel thread) */
void inst_run(inst_job_t *j);
const char *inst_step_name(int step);
/* "Disk 1: QEMU HARDDISK (sata0), 1.0 GB" style names */
void inst_disk_title(const inst_disk_t *d, char *out, int n);
void inst_vol_line(const inst_disk_t *d, const inst_vol_t *v, char *out, int n);

#endif
