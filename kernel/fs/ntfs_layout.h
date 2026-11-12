/* Halcyon OS - NTFS on-disk structures (milestone 7.5).
 *
 * Shared by the NTFS driver (fs/ntfs*.c) and the formatter. Everything is
 * little-endian and packed. References: the NTFS documentation of the
 * Linux-NTFS project ("NTFS Documentation", Richard Russon and Yuval Fledel)
 * and Microsoft's published FSCTL/structure documentation; field names
 * follow the common usage there. NTFS versions 3.0 / 3.1 (Windows 2000 and
 * later) are what Halcyon reads and writes.
 *
 * Conventions:
 *  - An MFT reference is 48 bits of record number and 16 bits of sequence
 *    number (NTFS_MREF / NTFS_MSEQ).
 *  - Times are 100 ns intervals since 1601-01-01 00:00 UTC.
 *  - MFT records ("FILE") and index blocks ("INDX") are protected by an
 *    update sequence array: the last u16 of every 512-byte stride is
 *    replaced by the update sequence number when written and must be put
 *    back (ntfs_fixup_read) before use, and swapped out again
 *    (ntfs_fixup_write) before writing. */
#ifndef FS_NTFS_LAYOUT_H
#define FS_NTFS_LAYOUT_H
#include "core/types.h"

#define NTFS_PACKED __attribute__((packed))

/* ---------------- boot sector ($Boot, sector 0 of the volume) ---------------- */
typedef struct NTFS_PACKED ntfs_boot {
    u8  jump[3];
    char oem[8];                    /* "NTFS    " */
    u16 bytes_per_sector;           /* 512 or 4096 (Halcyon: 256..4096, a power of 2) */
    u8  sectors_per_cluster;        /* <= 0x80: the count; 0xF4..0xFF: 2^(256-x) (clusters > 64 KiB) */
    u16 reserved_sectors;           /* 0 */
    u8  zero1[3];
    u16 unused1;
    u8  media;                      /* 0xF8 */
    u16 zero2;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;             /* the partition's start LBA (for booting) */
    u32 unused2;
    u32 unused3;                    /* 0x00800080 */
    u64 total_sectors;              /* 0x28: sectors in the volume, minus one (the backup boot sector) */
    u64 mft_lcn;                    /* 0x30 */
    u64 mftmirr_lcn;                /* 0x38 */
    i8  clusters_per_mft_record;    /* 0x40: > 0: clusters; < 0: 2^-x bytes (usually -10: 1024) */
    u8  pad1[3];
    i8  clusters_per_index_block;   /* 0x44: same encoding (usually 1 cluster or -12: 4096) */
    u8  pad2[3];
    u64 serial;                     /* 0x48: volume serial number */
    u32 checksum;                   /* 0x50: unused (0) */
    u8  code[426];
    u16 signature;                  /* 0x1FE: 0xAA55 */
} ntfs_boot_t;

/* ---------------- MFT records ---------------- */
#define NTFS_MREF(x) ((x) & 0x0000FFFFFFFFFFFFull)
#define NTFS_MSEQ(x) ((u16)((x) >> 48))
#define NTFS_MK_MREF(rec, seq) (((u64)(seq) << 48) | ((u64)(rec) & 0x0000FFFFFFFFFFFFull))

/* system files: their MFT record numbers */
enum {
    NTFS_FILE_MFT = 0, NTFS_FILE_MFTMIRR = 1, NTFS_FILE_LOGFILE = 2, NTFS_FILE_VOLUME = 3,
    NTFS_FILE_ATTRDEF = 4, NTFS_FILE_ROOT = 5, NTFS_FILE_BITMAP = 6, NTFS_FILE_BOOT = 7,
    NTFS_FILE_BADCLUS = 8, NTFS_FILE_SECURE = 9, NTFS_FILE_UPCASE = 10, NTFS_FILE_EXTEND = 11,
    NTFS_FILE_FIRST_USER = 16,      /* 12-15 are reserved; Windows puts $Extend's children at 24+ */
};

#define NTFS_MAGIC_FILE 0x454C4946u /* "FILE" */
#define NTFS_MAGIC_INDX 0x58444E49u /* "INDX" */
#define NTFS_MAGIC_BAAD 0x44414142u /* "BAAD": a record that failed its fixup check */
#define NTFS_MAGIC_RSTR 0x52545352u /* "RSTR": $LogFile restart page */
#define NTFS_MAGIC_RCRD 0x44524352u /* "RCRD": $LogFile record page */
#define NTFS_MAGIC_CHKD 0x444B4843u /* "CHKD": chkdsk touched it */

/* the start of an MFT record or index block: magic and the update sequence array */
typedef struct NTFS_PACKED ntfs_record_hdr {
    u32 magic;
    u16 usa_offset;                 /* the update sequence array: the number, then one u16 per stride */
    u16 usa_count;                  /* 1 + strides (a 1024-byte record: 3) */
} ntfs_record_hdr_t;

#define NTFS_MFT_IN_USE     0x0001
#define NTFS_MFT_DIRECTORY  0x0002  /* has a $I30 file name index */
#define NTFS_MFT_IN_EXTEND  0x0004  /* (a record in $Extend: $Quota, $ObjId, $Reparse) */
#define NTFS_MFT_VIEW_INDEX 0x0008  /* has a view index other than $I30 */

typedef struct NTFS_PACKED ntfs_mft_record {
    u32 magic;                      /* "FILE" */
    u16 usa_offset;                 /* 0x30 on NTFS 3.1 (0x2A on 3.0) */
    u16 usa_count;
    u64 lsn;                        /* $LogFile sequence number of the last change */
    u16 sequence;                   /* incremented each time the record is reused */
    u16 link_count;                 /* hard links: the number of $FILE_NAME attributes in indexes */
    u16 attrs_offset;               /* the first attribute */
    u16 flags;                      /* NTFS_MFT_* */
    u32 bytes_in_use;               /* up to and including the end marker, 8-byte aligned */
    u32 bytes_allocated;            /* the record size */
    u64 base_record;                /* 0, or the base record this extension record belongs to (MREF) */
    u16 next_attr_instance;         /* the instance number the next new attribute gets */
    u16 reserved;                   /* (3.1) */
    u32 record_number;              /* (3.1) this record's number, low 32 bits */
} ntfs_mft_record_t;

/* ---------------- attributes ---------------- */
enum {
    NTFS_AT_STANDARD_INFORMATION = 0x10, NTFS_AT_ATTRIBUTE_LIST = 0x20, NTFS_AT_FILE_NAME = 0x30,
    NTFS_AT_OBJECT_ID = 0x40, NTFS_AT_SECURITY_DESCRIPTOR = 0x50, NTFS_AT_VOLUME_NAME = 0x60,
    NTFS_AT_VOLUME_INFORMATION = 0x70, NTFS_AT_DATA = 0x80, NTFS_AT_INDEX_ROOT = 0x90,
    NTFS_AT_INDEX_ALLOCATION = 0xA0, NTFS_AT_BITMAP = 0xB0, NTFS_AT_REPARSE_POINT = 0xC0,
    NTFS_AT_EA_INFORMATION = 0xD0, NTFS_AT_EA = 0xE0, NTFS_AT_LOGGED_UTILITY_STREAM = 0x100,
    NTFS_AT_END = 0xFFFFFFFFu,
};

#define NTFS_ATTR_COMPRESSED 0x0001 /* (& 0x00FF: the compression method; 1 = LZNT1) */
#define NTFS_ATTR_ENCRYPTED  0x4000
#define NTFS_ATTR_SPARSE     0x8000

#define NTFS_RESIDENT_INDEXED 0x01  /* (resident_flags) the value is in an index ($FILE_NAME) */

typedef struct NTFS_PACKED ntfs_attr {
    u32 type;                       /* NTFS_AT_* */
    u32 length;                     /* the whole attribute record, 8-byte aligned */
    u8  non_resident;
    u8  name_length;                /* UTF-16 units; 0 = unnamed */
    u16 name_offset;
    u16 flags;                      /* NTFS_ATTR_* */
    u16 instance;                   /* unique within the base record and its extensions */
    union {
        struct NTFS_PACKED {        /* resident */
            u32 value_length;
            u16 value_offset;
            u8  resident_flags;
            u8  reserved;
        } r;
        struct NTFS_PACKED {        /* non-resident */
            i64 lowest_vcn;         /* the first cluster of the attribute this record maps */
            i64 highest_vcn;        /* the last (-1 for an empty attribute) */
            u16 mapping_pairs_offset;   /* the run list */
            u8  compression_unit;   /* log2 of clusters per compression unit (4 = 16), 0 = none */
            u8  reserved[5];
            i64 allocated_size;     /* bytes allocated (clusters), only in the first extent */
            i64 data_size;          /* the attribute's size */
            i64 initialized_size;   /* bytes ever written; beyond it reads as zeros */
            i64 compressed_size;    /* (only when compressed or sparse) clusters actually allocated */
        } nr;
    };
} ntfs_attr_t;

#define NTFS_ATTR_RESIDENT_HDR   0x18
#define NTFS_ATTR_NONRES_HDR     0x40   /* 0x48 with compressed_size */

/* $STANDARD_INFORMATION (resident, every base record) */
#define NTFS_FA_READONLY           0x00000001
#define NTFS_FA_HIDDEN             0x00000002
#define NTFS_FA_SYSTEM             0x00000004
#define NTFS_FA_DIRECTORY          0x00000010   /* (in $FILE_NAME: NTFS_FA_DUP_FILE_NAME_INDEX instead) */
#define NTFS_FA_ARCHIVE            0x00000020
#define NTFS_FA_DEVICE             0x00000040
#define NTFS_FA_NORMAL             0x00000080
#define NTFS_FA_TEMPORARY          0x00000100
#define NTFS_FA_SPARSE             0x00000200
#define NTFS_FA_REPARSE_POINT      0x00000400
#define NTFS_FA_COMPRESSED         0x00000800
#define NTFS_FA_OFFLINE            0x00001000
#define NTFS_FA_NOT_INDEXED        0x00002000
#define NTFS_FA_ENCRYPTED          0x00004000
#define NTFS_FA_DUP_FILE_NAME_INDEX 0x10000000  /* a directory (in $FILE_NAME and the index) */
#define NTFS_FA_DUP_VIEW_INDEX     0x20000000

typedef struct NTFS_PACKED ntfs_std_info {
    i64 creation_time;
    i64 data_change_time;
    i64 mft_change_time;
    i64 access_time;
    u32 file_attributes;            /* NTFS_FA_* */
    u32 max_versions;
    u32 version;
    u32 class_id;
    /* NTFS 3.x only (the value is 72 bytes; 48 on 1.2) */
    u32 owner_id;
    u32 security_id;                /* the descriptor in $Secure */
    u64 quota_charged;
    u64 usn;
} ntfs_std_info_t;

/* $FILE_NAME (resident; also the key of $I30 index entries) */
enum { NTFS_NS_POSIX = 0, NTFS_NS_WIN32 = 1, NTFS_NS_DOS = 2, NTFS_NS_WIN32_AND_DOS = 3 };

typedef struct NTFS_PACKED ntfs_file_name {
    u64 parent;                     /* MFT reference of the directory */
    i64 creation_time;              /* the "duplicated information": a copy of the file's */
    i64 data_change_time;           /* times, sizes and attributes, kept current in the */
    i64 mft_change_time;            /* directory index entries (Windows updates them when */
    i64 access_time;                /* the file changes; chkdsk checks them) */
    i64 allocated_size;
    i64 data_size;
    u32 file_attributes;
    union { struct NTFS_PACKED { u16 packed_ea_size; u16 reserved; } ea; u32 reparse_tag; };
    u8  name_length;                /* UTF-16 units, 1..255 */
    u8  name_type;                  /* NTFS_NS_* */
    u16 name[];                     /* UTF-16LE, not NUL-terminated */
} ntfs_file_name_t;

/* $VOLUME_INFORMATION (in $Volume) */
#define NTFS_VOL_DIRTY              0x0001
#define NTFS_VOL_RESIZE_LOG_FILE    0x0002
#define NTFS_VOL_UPGRADE_ON_MOUNT   0x0004
#define NTFS_VOL_MOUNTED_ON_NT4     0x0008
#define NTFS_VOL_DELETE_USN_UNDERWAY 0x0010
#define NTFS_VOL_REPAIR_OBJECT_ID   0x0020
#define NTFS_VOL_CHKDSK_UNDERWAY    0x4000
#define NTFS_VOL_MODIFIED_BY_CHKDSK 0x8000

typedef struct NTFS_PACKED ntfs_volume_info {
    u64 reserved;
    u8  major;                      /* 3 */
    u8  minor;                      /* 0 or 1 */
    u16 flags;                      /* NTFS_VOL_* */
} ntfs_volume_info_t;

/* ---------------- indexes ($INDEX_ROOT / $INDEX_ALLOCATION / $BITMAP) ---------------- */
enum { NTFS_COLLATION_BINARY = 0, NTFS_COLLATION_FILE_NAME = 1, NTFS_COLLATION_UNICODE_STRING = 2,
       NTFS_COLLATION_ULONG = 16, NTFS_COLLATION_SID = 17, NTFS_COLLATION_SECURITY_HASH = 18,
       NTFS_COLLATION_ULONGS = 19 };

#define NTFS_INDEX_LARGE 0x01       /* (index header flags) entries have sub-nodes in $INDEX_ALLOCATION */

typedef struct NTFS_PACKED ntfs_index_header {
    u32 entries_offset;             /* from this header */
    u32 index_length;               /* bytes used, from this header */
    u32 allocated_size;             /* bytes available, from this header */
    u8  flags;                      /* NTFS_INDEX_LARGE */
    u8  reserved[3];
} ntfs_index_header_t;

typedef struct NTFS_PACKED ntfs_index_root {
    u32 type;                       /* the indexed attribute (NTFS_AT_FILE_NAME for $I30), 0 for view indexes */
    u32 collation_rule;             /* NTFS_COLLATION_* */
    u32 index_block_size;           /* bytes per INDX block */
    u8  clusters_per_index_block;   /* (or log2 of blocks below a cluster: see the boot sector) */
    u8  reserved[3];
    ntfs_index_header_t ih;
} ntfs_index_root_t;

typedef struct NTFS_PACKED ntfs_index_block {
    u32 magic;                      /* "INDX" */
    u16 usa_offset;                 /* 0x28 */
    u16 usa_count;
    u64 lsn;
    i64 vcn;                        /* this block's position in $INDEX_ALLOCATION (in index block units
                                       if the block is smaller than a cluster, else clusters) */
    ntfs_index_header_t ih;
} ntfs_index_block_t;

#define NTFS_IE_NODE 0x0001         /* a sub-node VCN (i64) is the entry's last 8 bytes */
#define NTFS_IE_END  0x0002         /* the last entry of the node (no key) */

typedef struct NTFS_PACKED ntfs_index_entry {
    union {
        u64 file;                   /* $I30: the MFT reference the key's $FILE_NAME belongs to */
        struct NTFS_PACKED { u16 data_offset; u16 data_length; u32 reserved; } view;   /* view indexes */
    };
    u16 length;                     /* the whole entry, 8-byte aligned */
    u16 key_length;
    u16 flags;                      /* NTFS_IE_* */
    u16 reserved;
    u8  key[];                      /* $I30: an ntfs_file_name_t */
} ntfs_index_entry_t;

/* ---------------- $AttrDef entries ---------------- */
typedef struct NTFS_PACKED ntfs_attr_def {
    u16 name[64];                   /* "$STANDARD_INFORMATION", zero padded */
    u32 type;
    u32 display_rule;
    u32 collation_rule;
    u32 flags;                      /* 0x02 indexable, 0x40 must be resident, 0x80 may be non-resident */
    i64 min_size;
    i64 max_size;
} ntfs_attr_def_t;

/* ---------------- $ATTRIBUTE_LIST entries ---------------- */
typedef struct NTFS_PACKED ntfs_attr_list_entry {
    u32 type;
    u16 length;                     /* 8-byte aligned */
    u8  name_length;
    u8  name_offset;
    i64 lowest_vcn;
    u64 mft_reference;              /* the record that holds this attribute (extent) */
    u16 instance;
    u16 name[];
} ntfs_attr_list_entry_t;

/* ---------------- reparse points ---------------- */
#define NTFS_REPARSE_TAG_MOUNT_POINT 0xA0000003u    /* junctions */
#define NTFS_REPARSE_TAG_SYMLINK     0xA000000Cu
#define NTFS_REPARSE_TAG_DEDUP       0x80000013u
#define NTFS_REPARSE_TAG_WOF         0x80000017u    /* Windows 10 "compact OS" compressed files */

typedef struct NTFS_PACKED ntfs_reparse_point {
    u32 tag;
    u16 data_length;
    u16 reserved;
    u8  data[];
} ntfs_reparse_point_t;

/* ---------------- helpers ---------------- */
/* bytes of an MFT record / index block from the boot sector's signed encoding */
static inline u32 ntfs_record_bytes(i8 enc, u32 cluster_bytes) {
    return enc > 0 ? (u32)enc * cluster_bytes : enc >= -31 ? 1u << (u32)(-enc) : 0;
}

/* Apply the update sequence array of a record read from disk (size bytes;
 * stride NTFS_FIXUP_STRIDE: NTFS uses 512-byte strides whatever the sector
 * size). Returns 0, or -1 if the record is corrupt (a torn write: a
 * stride's last u16 does not match). */
static inline int ntfs_fixup_read(void *rec, u32 size, u32 stride) {
    ntfs_record_hdr_t *h = rec;
    u8 *b = rec;
    if (!stride || size % stride || h->usa_count != size / stride + 1) return -1;
    if ((u32)h->usa_offset + 2u * h->usa_count > stride - 2 || (h->usa_offset & 1)) return -1;
    u16 *usa = (u16 *)(b + h->usa_offset);
    for (u32 i = 1; i < h->usa_count; i++) {
        u16 *end = (u16 *)(b + i * stride - 2);
        if (*end != usa[0]) return -1;
        *end = usa[i];
    }
    return 0;
}

/* Prepare a record for writing: increment the update sequence number
 * (skipping 0 and 0xFFFF) and swap it into every stride's last u16.
 * Afterwards the in-memory copy is in its on-disk form: apply
 * ntfs_fixup_read again before using it. */
static inline void ntfs_fixup_write(void *rec, u32 size, u32 stride) {
    ntfs_record_hdr_t *h = rec;
    u8 *b = rec;
    u16 *usa = (u16 *)(b + h->usa_offset);
    u16 usn = (u16)(usa[0] + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    usa[0] = usn;
    for (u32 i = 1; i < h->usa_count && i * stride <= size; i++) {
        u16 *end = (u16 *)(b + i * stride - 2);
        usa[i] = *end;
        *end = usn;
    }
}

#define NTFS_FIXUP_STRIDE 512u

_Static_assert(sizeof(ntfs_boot_t) == 512, "ntfs_boot_t");
_Static_assert(__builtin_offsetof(ntfs_boot_t, total_sectors) == 0x28, "boot total_sectors");
_Static_assert(__builtin_offsetof(ntfs_boot_t, serial) == 0x48, "boot serial");
_Static_assert(sizeof(ntfs_mft_record_t) == 0x30, "ntfs_mft_record_t");
_Static_assert(__builtin_offsetof(ntfs_attr_t, r.value_length) == 0x10, "attr resident");
_Static_assert(__builtin_offsetof(ntfs_attr_t, nr.mapping_pairs_offset) == 0x20, "attr mapping pairs");
_Static_assert(__builtin_offsetof(ntfs_attr_t, nr.data_size) == 0x30, "attr data_size");
_Static_assert(sizeof(ntfs_attr_t) == 0x48, "ntfs_attr_t");
_Static_assert(sizeof(ntfs_std_info_t) == 72, "ntfs_std_info_t");
_Static_assert(sizeof(ntfs_file_name_t) == 66, "ntfs_file_name_t");
_Static_assert(sizeof(ntfs_index_root_t) == 32, "ntfs_index_root_t");
_Static_assert(sizeof(ntfs_index_block_t) == 40, "ntfs_index_block_t");
_Static_assert(sizeof(ntfs_index_entry_t) == 16, "ntfs_index_entry_t");
_Static_assert(sizeof(ntfs_attr_def_t) == 160, "ntfs_attr_def_t");
_Static_assert(sizeof(ntfs_attr_list_entry_t) == 26, "ntfs_attr_list_entry_t");

#endif
