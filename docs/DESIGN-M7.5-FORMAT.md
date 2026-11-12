# M7.5: the NTFS formatter and the NTFS user interface

This part of milestone 7.5 adds an NTFS formatter (`kernel/fs/ntfs_mkfs.c`),
NTFS in the Format dialog and the `FORMAT` command, and drive properties that
show why a drive is read-only and let the user allow writing to an NTFS drive
on an internal disk. The NTFS driver itself (reading, writing, deciding when a
volume is read-only) is described in DESIGN-M7.5.md.

## The formatter

`int ntfs_format(blockdev_t *bd, const char *label, u32 cluster_bytes)`
(`kernel/fs/ntfs_mkfs.h`) writes an empty NTFS 3.1 volume, the layout Windows
writes, as a quick format: only metadata is written. `vfs_format(bd, "NTFS",
label, cluster)` calls it and then sets the MBR partition type to 0x07 (on GPT
disks the partition keeps its basic data GUID, the same for FAT, exFAT and
NTFS, so nothing changes there).

It was written from the Linux-NTFS project's NTFS documentation and
Microsoft's published structure definitions; no code of ntfs-3g, mkntfs or
Linux was used. The ntfs-3g tools serve as black boxes in the tests only.

### Geometry and limits

| | |
|---|---|
| Sector sizes | 512 to 4096 bytes (powers of two; 512 and 4096 are tested, and 2048) |
| Cluster sizes | 512 bytes to 64 KiB, at least the sector size |
| Default cluster | 4 KiB up to 16 TiB, then 8, 16, 32, 64 KiB up to 256 TiB (Windows' table); never below the sector size |
| MFT records | 1024 bytes; 4096 bytes on 4096-byte sectors (as Windows on 4Kn disks) |
| Index blocks | 4096 bytes |
| Smallest volume | 2 MiB (`NTFS_MIN_VOLUME`; smaller devices: `E_NOSPC`) |
| Largest volume | 2^32 - 1 clusters (Windows' limit): 16 TiB with 4 KiB clusters, 256 TiB with 64 KiB; beyond: `E_INVAL` |
| Label | up to 32 UTF-16 units, no control characters or `" * / : < > ? \ |` |
| Serial number | 64 random bits from `random_bytes` |

All sizes and cluster numbers are 64-bit. Volumes of 2 TiB were formatted and
used in the tests; the bitmap of a 256 TiB volume (512 MiB) is written in
64 KiB pieces like everything else.

### Layout

The volume is the device minus its last sector, which holds the backup boot
sector (`total_sectors` in the boot sector = device sectors - 1, as Windows).
Clusters are allocated in this order:

| Clusters | Contents |
|---|---|
| 0 | `$Boot`: 8 KiB (one cluster when clusters are larger); sector 0 is the boot sector |
| next | `$MFTMirr`: the first 4 MFT records, or a whole cluster's worth when clusters hold more than 4 records (16 with 64 KiB clusters and 4 KiB records), as Windows and ntfs-3g read it. At LCN 2 with 4 KiB clusters, where Windows puts it |
| next | `$AttrDef` (2560 bytes), the root directory's index block (4 KiB), `$UpCase` (128 KiB), `$Secure:$SDS` (256 KiB + its entries), `$Bitmap`, `$MFT:$BITMAP` (8 bytes in one cluster), `$LogFile` |
| then | `$MFT`: 64 records to start with (64 KiB, or 256 KiB with 4 KiB records). On volumes of 12 GiB and more it starts 3 GiB into the volume (LCN 786432 with 4 KiB clusters), as on volumes Windows formats |

Nothing is allocated after the MFT, so the MFT zone that drivers keep free
for the MFT to grow into (1/8 of the volume after the MFT) is free. The zone
is not an on-disk structure; it is an allocation policy of the driver.

Examples (from the tests): a 2 MiB volume has 511 clusters, the MFT at LCN
168 and 327 clusters free; 1 GiB: MFT at LCN 4207, a 16 MiB log, 98.4% free;
13 GiB: MFT at LCN 786432.

### The boot sector

`EB 52 90 "NTFS    "`, bytes per sector, sectors per cluster, media 0xF8,
63 sectors per track and 255 heads, hidden sectors = the partition's first
LBA (0 for a disk without a partition table), 0x00800080 at 0x24, the volume
size, the MFT and mirror LCNs, clusters per MFT record and per index block in
the signed encoding (a positive count when the record or block is at least a
cluster, else -log2 of its size: 0xF6 for 1 KiB, 0xF4 for 4 KiB), the serial
number and 0xAA55. The code at 0x54 is a small real-mode routine of ours that
prints "This is not a bootable disk (an NTFS data volume formatted by
Halcyon). Remove it and press any key to try the next boot device.", waits for
a key and calls INT 19h. The rest of `$Boot` is zeros. The backup at the
device's last sector is identical. The old boot sector is zeroed first and the
new one written last, so a format that fails halfway leaves no mountable
volume.

### MFT records

| Record | File | Contents |
|---|---|---|
| 0 | `$MFT` | `$DATA` (the MFT), `$BITMAP` (records in use) |
| 1 | `$MFTMirr` | `$DATA` |
| 2 | `$LogFile` | `$DATA` |
| 3 | `$Volume` | `$VOLUME_NAME` (the label; empty if none), `$VOLUME_INFORMATION` (3.1, flags 0: clean), empty `$DATA` |
| 4 | `$AttrDef` | `$DATA` (non-resident on every volume) |
| 5 | `.` (root) | `$INDEX_ROOT $I30` with one entry pointing to VCN 0, `$INDEX_ALLOCATION $I30` (one 4 KiB block with the 12 entries `$AttrDef` ... `.`, sorted with `$UpCase`), `$BITMAP $I30` |
| 6 | `$Bitmap` | `$DATA`: 1 bit per cluster, padded to 8 bytes; the bits past the last cluster are set |
| 7 | `$Boot` | `$DATA` at cluster 0 |
| 8 | `$BadClus` | an empty `$DATA` and `$DATA:$Bad`, one sparse run as long as the volume |
| 9 | `$Secure` | `$DATA:$SDS`, `$INDEX_ROOT $SDH`, `$INDEX_ROOT $SII` (record flag 0x08: view index) |
| 10 | `$UpCase` | `$DATA`: 65536 UTF-16 units |
| 11 | `$Extend` | a small `$I30` index of `$ObjId`, `$Quota`, `$Reparse` |
| 12-15 | | reserved: in use, a `$STANDARD_INFORMATION` and an empty `$DATA`, no name |
| 16-23 | | formatted, not in use, and not handed out: Windows keeps them for `$MFT`'s own extension records (the driver's allocator must start at 24) |
| 24 | `$Extend\$Quota` | `$INDEX_ROOT $O` (owner SID -> owner id: Administrators -> 0x100) and `$Q` (owner id -> quota entry: 1 = the defaults, 0x100 = Administrators; version 2, no limits) |
| 25 | `$Extend\$ObjId` | `$INDEX_ROOT $O`, empty |
| 26 | `$Extend\$Reparse` | `$INDEX_ROOT $R`, empty |
| 27-63 | | formatted, free |

Every record: "FILE", the update sequence array at 0x30 (3.1 layout, with the
record number at 0x2C), sequence number = the record number (1 for `$MFT`,
1 for 24 and up), attributes in type then name order, instance numbers from
0, the end marker. `$MFT:$BITMAP` marks records 0-15 and 24-26. Every file has
a 72-byte `$STANDARD_INFORMATION` (attributes hidden + system; the `$Extend`
files also archive and view index) with a security id, and every named file a
`$FILE_NAME` in the Win32 and DOS namespace whose duplicated sizes match its
data. There are no `$SECURITY_DESCRIPTOR` attributes: NTFS 3.x keeps them in
`$Secure`.

### $Secure

Three security descriptors, self-relative (DACL, then owner, then group),
each stored in `$SDS` with its header (hash, id, offset, length) on 16-byte
boundaries and copied 256 KiB further on, as `$SDS` requires; `$SII` (by id,
ULONG collation) and `$SDH` (by hash and id, SECURITY_HASH collation, with
Windows' "II" padding) index them. The hash is the documented one (rotate left
3, add each 32-bit word).

| Id | For | Owner / group | DACL |
|---|---|---|---|
| 0x100 | metadata files | Administrators / Administrators | SYSTEM and Administrators: read (0x120089) |
| 0x101 | `$Secure`, `$Extend` and its files | Administrators / Administrators | SYSTEM and Administrators: read and write (0x12019F) |
| 0x102 | the root directory | Administrators / SYSTEM | Administrators and SYSTEM: full control; Authenticated Users: modify; Users: read and execute (all inherited by files and folders); CREATOR OWNER: full control of what they create (inherit only) |

The root's descriptor is the shape Windows gives a new data volume, so
Windows shows familiar permissions and files created there inherit them.

### $LogFile

Size: 1/64 of the volume, at least 256 KiB, at most 64 MiB (64 MiB from
4 GiB up, as Windows; Windows' own sizes for small volumes are not documented),
a multiple of 64 KiB and of the cluster size.

The first two 4 KiB pages are identical restart pages, version 1.1: "RSTR",
the update sequence array at 0x1E (9 entries: the page is protected in
512-byte strides), system and log page size 4096, the restart area at 0x30:
current LSN 0, one log client slot that is free (free list = client 0, in-use
list empty), flags = clean (0x0002), sequence number bits = 67 - the bit
length of the file size (the LSN keeps the file offset / 8 in the low bits),
restart area length 0xD0, client array at 0x30, the file size, record header
length 0x30, page data offset 0x40, a random open count, and the client record
"NTFS" (previous / next 0xFFFF, no restart LSN). The remaining pages are 0xFF
("never used"). This is a log with nothing to replay: ntfs-3g mounts it as
clean, and Windows starts a new log on it. Writing a fully filled-in log like
Windows' own after its first mount would need record pages with checkpoint
records, which only matter to Windows' log replay; mkntfs instead fills the
whole file with 0xFF, which Windows also accepts.

### $UpCase

The table must equal the one Windows writes closely enough that names compare
the same way under Windows and Halcyon (and chkdsk does not "fix" it). It is
generated, not copied: `tools/mkntfsupcase.py` derives it from the Unicode
character database of the Python running it (Unicode 15.1 here) with the
rules Windows' table follows:

- simple one-to-one uppercase mappings (a character whose uppercase is several
  characters keeps its one-character titlecase mapping, e.g. U+1F80 -> U+1F88,
  else maps to itself: U+00DF, the ligatures);
- reversible pairs only: c -> U only if U lowercases back to c. This drops
  the one-way mappings Windows leaves out: U+00B5 micro sign, U+0131 dotless i,
  U+017F long s, U+03C2 final sigma, the Greek symbol variants U+03D0..,
  titlecase digraphs U+01C5.., U+0345, U+1E9B, U+1FBE;
- the Unicode 5.1 repertoire (Windows Vista SP1 / 7, and Windows 8 and later
  format with the same mappings): pairs with a character added in Unicode 5.2
  or later are left out (a list of ranges from the Unicode version history in
  the script).

The script writes `kernel/fs/ntfs_upcase.c`: 134 runs (first, count, step,
delta) that `ntfs_upcase_table()` expands to 973 changed characters. Checked
against the table mkntfs writes (which follows Windows Vista / 7): all 65536
entries are equal (the host test compares them on every run). Windows 8 and
later also add a `$UpCase:$Info` stream (a CRC64 and the Windows version of
the table); the formatter does not, like Windows 7 (the stream is optional).

### $AttrDef

The standard table of 15 attributes (`$STANDARD_INFORMATION` ... `$LOGGED_UTILITY_STREAM`)
with their minimum and maximum sizes and flags (resident only 0x40, indexed
0x02, may be non-resident 0x80), padded with zeros to 2560 bytes like
Windows'. The collation rule field is 0 in every entry, `$FILE_NAME`'s too
(the index roots carry the real collation). Byte for byte equal to mkntfs's
table, which is Windows' (checked on every test run).

## Verification

No Windows (and so no `chkdsk`) is available here. What was checked instead,
on every run of `tests/host/run.sh` (`tests/host/ntfsfmt.c`, about 25 s):

- 27 volumes formatted by the kernel code compiled for the host: 2, 3, 7, 16,
  100 MiB (+ 3 sectors), 257 MiB, 1, 4 (+ 7 sectors), 13, 100 GiB and 2 TiB with
  default clusters; 256 MiB with 512 B to 64 KiB clusters; 3 MiB and 40 GiB with
  64 KiB clusters; 16 MiB and 1 GiB on 4096-byte sectors (4 KiB, default and
  64 KiB clusters); 64 MiB on 2048-byte sectors; a 32-character label and one
  with Greek, an emoji (a surrogate pair) and CJK; an MBR partition formatted
  through `vfs_format` (type set to 0x07, hidden sectors 2048).
- Each passes `ntfsfix -n` (nothing to fix, backup boot sector OK, $MFTMirr
  OK), `ntfsinfo -m` (sector and cluster size, label, version 3.1, flags 0),
  `ntfsls` / `ntfscat` (every system file listed, `$UpCase` read back), and
  `tests/host/ntfs_tool.py check`, an independent NTFS reader in Python (written
  from the documentation too, sharing no code with the formatter): boot sector
  and backup, every record's update sequence array, attribute order, instances
  and run lists, `$Bitmap` exactly equal to the clusters the run lists use
  (no cross links, nothing leaked, the bits past the end set), `$MFT:$BITMAP`
  equal to the records in use, `$MFTMirr` equal to the MFT, every index walked
  (sorted with the volume's `$UpCase`, entries matching the files'
  `$FILE_NAME`s, link counts), `$Secure` (hashes, mirror copies, `$SDH` and
  `$SII` equal to `$SDS`, every security id in use present, descriptors well
  formed), the `$LogFile` restart pages, `$AttrDef`, `$Volume`, `$BadClus`,
  `$Extend`. Four damaged copies (backup boot sector, mirror, a torn record,
  the dirty flag) must fail it, so it is not vacuous; mkntfs-made volumes pass
  it.
- 8 of them (3 MiB, 257 MiB, 13 GiB, 2 TiB, 256 MiB with 512 B and 64 KiB
  clusters, 1 GiB on 4K sectors, the partition) are mounted with the ntfs-3g
  FUSE driver: up to 5000 files (0 bytes to 4 MiB, mixed-case and non-ASCII
  names, a directory with a multi-level index) and up to 126 directories
  written, a third deleted, a tenth renamed, a tenth rewritten (about 13,000
  files in all); unmounted; `ntfsfix -n` clean; mounted again and every file
  compared by SHA-256; most deleted; `ntfsfix -n` clean again; and
  `ntfs_tool.py check` on the result.
- The system files are compared with mkntfs volumes of the same geometry
  (64 MiB / 4 KiB, 300 MiB with 512 B and 64 KiB clusters, 300 MiB on 4K
  sectors, 2 GiB) with `ntfs_tool.py dump` (records 0-26: flags, bitmap state,
  every attribute's type, name, residency, flags, names, collations, index
  block sizes). The only differences, all expected:
  - mkntfs gives `$Volume`, `$AttrDef`, `$Boot`, the root and records 12-15 a
    `$SECURITY_DESCRIPTOR` attribute and a 48-byte (NTFS 1.2 style)
    `$STANDARD_INFORMATION`; Halcyon uses 72-byte ones with security ids for
    every file, as Windows' format does;
  - the root's `$STANDARD_INFORMATION`: mkntfs sets the archive bit, Halcyon
    hidden + system only;
  - mkntfs adds `$UpCase:$Info` (see above);
  - on 4 KiB-record volumes mkntfs keeps `$AttrDef`'s 2560 bytes resident;
    Halcyon keeps it non-resident everywhere;
  - placement and sizes (not compared): mkntfs puts the MFT at LCN 4 (after
    `$Boot`), the mirror in the middle of the volume, makes the MFT 27 records
    and the log 2 MiB on 64 MiB; Halcyon follows Windows (mirror at LCN 2, MFT
    after the metadata or at 3 GiB, 64 records).
- In QEMU: the `xhci:bios:ntfs` boot test has the kernel format a SATA scratch
  disk's partition as NTFS through `vfs_format` (test hook below) and checks
  the image on the host (partition type 0x07, `ntfsfix -n`, `ntfsinfo -m`,
  `ntfs_tool.py check`). By hand: SATA disks formatted from the Format dialog
  (NTFS, 2 KiB clusters, a 27-character label) and with `FORMAT F: /FS:NTFS
  /V:CmdNTFS /A:8K /Y` passed the same checks, and ntfs-3g wrote 1500 files to
  the first.

## User interface

### Format

The Format dialog (`kernel/apps/format.c`; Explorer: right-click a drive,
Format...) lists FAT32, FAT, exFAT and NTFS, an allocation unit size list
(Default, then 512 bytes up to 64 KB for FAT and NTFS, up to 32 MB for exFAT,
never below the sector size) that follows the chosen file system, and the
label (11 characters for FAT and exFAT, 32 for NTFS: checked before
formatting). The file system it suggests is the current one, else what
Windows would choose: NTFS on fixed disks of 64 MiB and more, FAT32 on
removable ones up to 32 GiB, exFAT on larger removable ones, FAT on small
ones. The user-mode Explorer reaches the same dialog through
`SYS_SHELL_FORMAT`.

`FORMAT target /FS:NTFS [/V:label] [/A:size] [/Y]` in the Command Prompt, with
the same default choice when `/FS` is missing.

Until the NTFS driver is merged, nothing mounts the new volume:
`vfs_format` then still returns 0 (and logs "formatted as NTFS; no driver
mounts it"), the dialog says the drive is ready for Windows but not shown,
and `FORMAT` says the same.

### Drive properties and read-only drives

Explorer's drive Properties (both the user-mode Explorer and the kernel's
fallback, `kernel/apps/driveprops.c`) is a dialog: the label (up to 32
characters; `volume_t.label` is now 64 bytes of UTF-8), type, file system,
device, serial number (low 32 bits as XXXX-XXXX, as Windows), used and free
space with a bar, and for a read-only drive a warning icon, "This drive is
read-only." and the reason (`volume_t.ro_reason`, or "The disk is
write-protected"). My Computer draws a small padlock on the icon of a
read-only drive (not on CD drives) and the Details pane says "Read-only";
Format is disabled for it.

For an NTFS volume on an internal disk that the driver mounted read-only only
because of the internal-Windows-disk rule, the dialog has a check box "Allow
Halcyon to write to this drive". Ticking it first shows a warning (writing can
damage files Windows needs, keep backups, shut Windows down completely:
Fast Startup and hibernation keep the drive in use); OK then sets
`ntfs.write.<serial as 16 uppercase hex digits> = 1` in `SETTINGS.INI` (or
removes the key when the box is cleared), saves it, and mounts the volume
again with `vfs_remount`. If files are open on it (`E_BUSY`) nothing changes
and the user is asked to close them; the setting is put back.

The contract with the driver (`kernel/fs/vfs.h`):

- `volume_t.readonly`, `ro_reason`, `serial` as in the M7.5 base commit;
- `volume_t.ro_policy`, new: set by the driver when the internal-disk rule is
  the only reason the volume is read-only (not when it is dirty or
  hibernated). The check box is offered when the volume is NTFS, has a serial,
  its device is writable, and either `ro_policy` is set or the key is already
  set (so writing can be taken back);
- the driver reads `ntfs.write.<serial>` at mount time.

The user-mode Explorer gets this through `SYS_VOLUME_INFO`, whose
`hal_volinfo_t` (`kernel/core/sysabi.h`) now has a 64-byte label,
`readonly` = the device or the file system is read-only, the serial, `flags`
(`VOLF_DEVICE_RO`, `VOLF_FS_RO`, `VOLF_WRITE_OPT`: the check box applies,
`VOLF_WRITE_OK`: writing is allowed) and `ro_reason[128]`. The new system call
`SYS_VOLUME_SET_WRITE` (164; `volume_set_write(drive, allow)` in `hal.h`) runs
`volume_set_write()` on the GUI thread (which owns `SETTINGS.INI`) and returns
0, `E_BUSY`, `E_NOTSUP` (no such choice for this volume) or a mount error.

The dialogs were checked on screenshots in QEMU with a temporary hook (since
removed) that made FAT test volumes look like read-only NTFS volumes: the
padlock, the Details line, the dialog with and without the check box, the
warning, allowing writing (the padlock goes, the key appears in
`SETTINGS.INI`) and taking it back (the key is removed).

### Test hook

QEMU fw_cfg `opt/halcyon/formattest` = "MODEL FS LABEL [CLUSTER]": once the
desktop is up, the kernel formats the disk whose model is MODEL (its first
partition, if it has partitions) with `vfs_format`, unmounts it again if a
driver mounted it (the host checks the image as the formatter left it), and
logs "format test: DEVICE as FS 'LABEL': done". `tools/boottest.py`'s `ntfs`
variant (in the default matrix as `xhci:bios:ntfs`) uses it.

## Not done

- No `$UpCase:$Info`, no `System Volume Information`, no `$Extend\$RmMetadata`
  (all optional; Windows creates them when it first mounts the volume).
- No surface scan (full format) and no bad cluster list.
- Clusters above 64 KiB (Windows 10 1709 and later allow up to 2 MiB) and
  compression-capable defaults are not offered.
- The `$LogFile` choice (clean restart pages, empty log) and the small log
  sizes on small volumes are checked by ntfs-3g only, not by Windows.
