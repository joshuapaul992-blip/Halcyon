# M7.5: the NTFS driver (read and write)

Halcyon reads and writes NTFS volumes: USB sticks and external disks
formatted by Windows or Linux, internal Windows drives (read-only unless the
user allows writing), and volumes made by Halcyon's own formatter
(DESIGN-M7.5-FORMAT.md). This document covers the driver, the rules that
decide when a volume may be written, and the tests.

The driver was written from the NTFS documentation of the Linux-NTFS project
and Microsoft's published structure definitions ([MS-XCA] for LZNT1, the
reparse point and FSCTL documentation). No code of ntfs-3g, mkntfs or Linux
was used or read for copying; the ntfs-3g *tools* are black boxes in the
tests.

## Files

| File | |
|---|---|
| `kernel/fs/ntfs_layout.h` | on-disk structures (shared with the formatter) |
| `kernel/fs/ntfs.h` | `ntfs_probe`, `ntfs_boot_sane`, `ntfs_settings_changed`, `ntfs_setting_key` |
| `kernel/fs/ntfs_priv.h` | the driver's internal interfaces |
| `kernel/fs/ntfs.c` | mount, the safety rules, names and lookups, the VFS operations |
| `kernel/fs/ntfs_mft.c` | device I/O, the two caches, run lists, MFT records, attributes, attribute lists |
| `kernel/fs/ntfs_index.c` | B+ tree indexes (`$I30` directories, the `$O` / `$R` view indexes) |
| `kernel/fs/ntfs_alloc.c` | clusters (`$Bitmap`) and MFT records (`$MFT:$BITMAP`, growing `$MFT`) |
| `kernel/fs/ntfs_lznt1.c` | LZNT1 decompression |

`ntfs_probe` is the last entry of the VFS probe list; the block layer also
uses `ntfs_boot_sane` to tell an NTFS boot sector at LBA 0 (a superfloppy)
from an MBR.

## Structure

**I/O.** All addresses inside the driver are volume byte offsets; `nt_dev_rw`
turns them into device sectors (read-modify-write for partial sectors), so a
4096-byte-sector volume works on a 512-byte device and the other way round.
File data goes straight between the caller's buffer and the disk
(`nt_runs_rw`). Metadata goes through two caches:

- the *fixup block cache* (40 entries) holds MFT records and index blocks with
  their update sequence fixups applied. A block is read from up to 16 pieces
  (a 1 KiB record or 4 KiB index block may span clusters in different runs);
  a torn block (a stride whose last two bytes do not match the update
  sequence number) is refused with `E_IO`. Write-back reapplies the fixups in
  a copy (the sequence number increments) and, for the first records, writes
  `$MFTMirr` too.
- the *page cache* (48 pages of 4 KiB) holds bitmap pages: `$Bitmap`,
  `$MFT:$BITMAP`, non-resident index `$BITMAP`s. Small metadata attributes are
  also read through it (`nt_attr_read`), so a bit just set is never read back
  stale.

Every operation that changes the volume flushes both caches before it
returns: pages (allocation) first, then records and index blocks. Freed
clusters are dropped from both caches.

**Records and attributes.** `nt_rec_get` reads a record by number through
`$MFT`'s run list and checks its header (magic, sizes, the record number
field). Attributes are walked with `nt_rec_walk`, which checks every header
(lengths, name and value inside the attribute, mapping pairs offset, VCN
range) before anything uses it. An *inode* (`nino_t`) is the pinned base
record plus a copy of the `$ATTRIBUTE_LIST` (resident or non-resident); an
attribute search (`actx_t`) walks the base record or, with a list, the list
entries, loading each extension record and checking that it belongs to the
file (in use, base reference, sequence number) and holds the listed attribute
(type, instance, name, lowest VCN).

**Run lists** are decoded into arrays of (VCN, LCN, length) runs; every run
must lie inside the volume, lengths must be positive, extents must follow each
other without gaps and cover `highest_vcn`, and the whole list must cover the
initialized size. Sparse runs have LCN -1. The encoder writes minimal signed
fields as Windows does.

**Data.** Resident data is read from the record. Non-resident data is read
run by run; holes and everything past the initialized size read as zeros.
LZNT1-compressed files are read by compression unit (16 clusters, clusters up
to 4 KiB): a unit with all clusters allocated is stored raw, one with no
clusters is zeros, otherwise the allocated clusters are decompressed. The last
unit is cached per open file.

**Indexes.** Lookups descend the B+ tree comparing keys with the index's
collation: `$I30` compares names through the volume's `$UpCase` table, then
case-sensitively (names that differ only in case are different entries; a
lookup takes the exact name first, then any name equal ignoring case).
Listings walk the tree in order (depth limited to 24, at most one visit per
index block, so a loop in a damaged tree ends with `E_IO`). Index blocks
smaller than a cluster use the VCN unit the index root gives (512 bytes on
volumes made by mkntfs, Windows and Halcyon).

**Open files** are nodes shared by every handle of the same record (so
several handles see one size); the node keeps the `$DATA` sizes and run list.
A file that is open cannot be deleted (`E_BUSY`).

## Names

On disk names are UTF-16 in four namespaces: POSIX, Win32, DOS and Win32+DOS.
Listings show every name except DOS-only names; a Win32 name's DOS partner is
reported as `vfs_dirent_t.short_name` (and a Win32+DOS name is its own short
name). A DOS name still opens the file, as in Windows. Names the VFS cannot
show (control characters, `/`, `\`, `:`, unpaired surrogates, trailing dots or
spaces, more than 255 bytes of UTF-8) get a sanitized display name, as in the
exFAT driver, and lookups of such a name fall back to a scan of the directory.

New names are checked as Windows does (`"*/:<>?\|` and control characters
are refused, trailing dots and spaces are stripped). A new name that is a
valid upper-case 8.3 name gets the Win32+DOS namespace, any other name the
Win32 namespace alone: Halcyon does not generate short names (as Windows with
8.3 name creation off, which chkdsk accepts). Renaming a file with a
Win32/DOS pair replaces both names with the new one.

**Hidden and shown.** The system files (MFT records 0-15: `$MFT`, `$Bitmap`,
`$Extend`, ...) and the root's `.` entry are not listed and cannot be opened by
name. `System Volume Information` and `$Recycle.Bin` are listed with their
attributes (hidden, system), as Windows stores them. Alternate data streams
are not shown and never touched except that deleting a file frees them.

**Attributes and times.** NTFS read-only, hidden, system and archive map to
the `VFS_ATTR_*` bits; directories have `VFS_ATTR_DIR`. Times are UTC on
disk; Halcyon's clock is local time, so the driver converts with the Control
Panel's `clock.utc_offset` setting (minutes east of UTC, 0 if unset), both
when showing times and when writing them.

## Links, reparse points, special files

| File | Listed as | Open |
|---|---|---|
| symbolic link, junction (reparse tags `0xA000000C`, `0xA0000003`) | `VFS_ATTR_LINK` (a junction or directory link also has `VFS_ATTR_DIR`) | `E_NOTSUP`; listing a directory link: `E_NOTSUP` |
| Interix symbolic link (ntfs-3g's default) | a system file | read as a file (its content, `IntxLNK...`) |
| WOF / Compact OS (`0x80000017`), deduplicated (`0x80000013`) | normally | `E_NOTSUP`, with a log line |
| other reparse points (e.g. OneDrive) | normally | read-only; placeholders not on the disk (offline / recall attributes): `E_NOTSUP` |
| EFS encrypted | normally | `E_PERM`, with a log line |
| compressed (LZNT1), sparse | normally | read; writes `E_NOTSUP` |
| hard links | under every name | one node per record |

Links are never followed: a junction's target names a drive letter of the
Windows system that made it, which means nothing to Halcyon, and following
relative links would let a link lead out of the volume. Deleting a link, a
WOF file or an encrypted file works (and removes the file's entries from
`$Extend\$Reparse` and `$Extend\$ObjId`).

## Writing

Supported: create, write, extend, truncate, delete files; mkdir and rmdir;
rename and move within the volume (also directories, also between
directories, also a change of case only).

- **MFT records** come from `$MFT:$BITMAP`, never below record 24 (0-15 are
  the system files, 16-23 reserved as Windows and Halcyon's formatter keep
  them). A record found in use although the bitmap says free makes the
  volume read-only. A reused record keeps the sequence number incremented when
  it was freed. When the MFT is full it grows by an eighth (at least 64
  records), from clusters right after it (the MFT zone), with its bitmap
  extended and the new records formatted; only an `$MFT` whose `$DATA` is a
  single extent in record 0 can grow (otherwise `E_NOSPC`).
- **Clusters** come from `$Bitmap`: one free extent large enough, searched
  from after the file's last cluster, else the free extents in order. File
  data stays out of the MFT zone (an eighth of the volume after the MFT)
  until the rest is full. The free count is computed once and kept up to
  date.
- **Data.** Small files stay resident while they fit in the record; a file
  that grows past that becomes non-resident (its clusters are allocated and
  the old value written to them). Writing past the initialized size first
  writes zeros up to the write; growing with truncate only allocates (the new
  part reads as zeros); shrinking frees clusters. Run lists are written as a
  single extent in their record; a file whose `$DATA` already spans several
  extents (an attribute list from heavy fragmentation) can be overwritten in
  place but not grown or shrunk (`E_NOTSUP`), and a run list that no longer
  fits in its record fails the write with `E_NOSPC` (nothing changes).
- **Indexes.** Insertion goes into a leaf; a node that no longer fits is split
  at its middle entry, which moves up (the left half goes to a new block, the
  right half stays); a root that no longer fits in its MFT record is pushed
  down into a new block (the root keeps 256 bytes of its record free so the
  push-down always has room for `$INDEX_ALLOCATION` and `$BITMAP`). Deletion
  of an inner entry replaces it with its predecessor; a non-root node left
  without keys takes one from a sibling through the parent (a rotation) or is
  merged with it, up the tree as needed; a keyless root loses a level or takes
  in its only child when that fits, and a directory whose entries fit in the
  root again drops its `$INDEX_ALLOCATION` and `$BITMAP`. So every non-root
  node keeps a key and all leaves stay at one depth. The blocks a split may
  need are reserved before anything changes (deleting from a leaf that keeps
  other keys needs none), so a full volume fails an operation without
  touching the tree. Index allocations grow by at least four blocks (or an
  eighth) at a time. `$BITMAP`s and allocations whose initialized size lies
  below their size (ntfs-3g writes those) are initialized before use.
- **Names and duplicated information.** Every change of size, attributes or
  times updates the file's `$STANDARD_INFORMATION` and then the duplicated
  information (times, allocated and data size, attributes) in every
  `$FILE_NAME` of the file and in the index entry of each of its names, as
  Windows does when a file is closed. Allocated sizes follow Windows: the
  value length rounded up to 8 for resident data, the compressed size for
  compressed and sparse files, 0 for directories (whose duplicated attributes
  carry `0x10000000`). Directory times change when entries come and go.
- **Link counts** count `$FILE_NAME` attributes (a Win32/DOS pair counts two);
  a deletion removes one name (and its partner) and frees the file when no
  name is left: all clusters of every attribute (all streams, all extents),
  extension records, the record itself.
- **Attribute lists.** A file with an attribute list (many hard links, heavy
  fragmentation) can be renamed, deleted and written: the list (resident or
  non-resident) is rewritten as entries come and go, a new `$FILE_NAME` that
  does not fit in the base record goes to an extension record, and extension
  records left empty are freed.
- **Security.** A new file gets its directory's security id (an existing
  descriptor in `$Secure`); a directory without one (ntfs-3g files) passes on
  a copy of its own resident `$SECURITY_DESCRIPTOR`; else the root's id is
  used; on a volume without any (made by mkntfs) a small default descriptor
  is written into the record (owner Administrators, Everyone full control,
  inherited). `$Secure` itself is never written.
- **Refused:** writes to compressed, encrypted, sparse and reparse-point files
  and to system files (`E_NOTSUP` / `E_PERM`); changes to multi-extent run
  lists (`E_NOTSUP`); deleting or renaming files in `$Extend` (`E_PERM`).
- Every failure path gives back what it allocated; a rename or deletion that
  fails half-way puts the directory entries back. Anything that would leave
  the volume inconsistent marks it corrupt instead (see below).

## The journal and the safety rules

Halcyon neither replays nor writes `$LogFile` records. A volume is mounted
read-write only when all of these hold, checked in this order (the first
failing rule gives the reason shown in Explorer, `volume_t.ro_reason`):

1. The device is writable. Else: *"The disk is write-protected."*
2. The volume is consistent: no dirty, chkdsk-underway, repair-object-id or
   delete-USN-underway flag in `$VOLUME_INFORMATION`; the primary boot sector
   is good (else the backup at the volume's last sector is used); `$MFT` record
   0 reads (else `$MFTMirr`'s copy is used); `$MFTMirr` matches `$MFT` for the
   mirrored records; `$Bitmap` covers the volume; `$UpCase` is sane (a-z map to
   A-Z); the volume is not larger than the device; no damage was found during
   mount. Else: *"The volume needs a check in Windows (chkdsk)."*
3. Windows shut down completely. `$LogFile` must be empty (all 0xFF, as left
   by mkntfs, ntfs-3g and Halcyon) or its newer valid restart page (`RSTR`,
   fixups checked, log version 1.x or 2.x) must say clean: no client in use
   (`client_in_use_list` = 0xFFFF) or the clean flag (0x0002) set; garbage
   counts as not clean. And `\hiberfil.sys` must not hold a hibernated system:
   its first four bytes, compared ignoring case, must not be `hibr` or `rstr`
   (`wake`, zeros, a missing or empty file are fine). Else: *"Windows did not
   shut down completely (Fast Startup or hibernation). Shut Windows down fully
   to write here."*
4. The layout is one the writer handles: NTFS 3.0 or 3.1, no upgrade or
   log-resize pending, a non-resident `$MFT:$BITMAP`, MFT records of at least
   1 KiB, index blocks of at least 2 KiB. Else: *"Halcyon cannot write to this
   NTFS version or layout."*
5. Not an internal Windows disk: if the device is not removable
   (`bd->removable == 0`: SATA and NVMe disks; USB disks and the CD are
   removable) and the volume has `\Windows\System32`, it stays read-only unless
   SETTINGS.INI has `ntfs.write.<serial> = 1` (the volume serial number from
   the boot sector as 16 upper-case hex digits). Else: *"This is an internal
   Windows drive: writing is off until you allow it in the drive's
   properties."* In this case alone `volume_t.ro_policy` is set, and Drive
   Properties offers "Allow Halcyon to write to this drive"; it sets the key and
   remounts the volume (`vfs_remount`). Because internal disks mount before
   SETTINGS.INI is loaded, kmain calls `ntfs_settings_changed()` after
   `settings_load()`, which re-applies the rule to mounted volumes.

Removable disks are writable by default (subject to rules 1-4). Reading works
on every volume that mounts.

**The first change of a session** sets the dirty flag in `$VOLUME_INFORMATION`
(record 3 and its `$MFTMirr` copy, flushed at once) and then empties
`$LogFile`: every byte of its data is overwritten with 0xFF. That is the state
mkntfs and ntfs-3g leave and Windows accepts: on its next mount Windows finds
no restart area, initializes a new log and carries on (a dirty flag would make
it run chkdsk first). Writing the whole file (2 to 64 MiB) costs a few seconds
on a slow stick once per mount, but leaves no stale log pages behind. The
order matters: if Halcyon stops between the two steps, the dirty flag makes
Windows check the volume.

**Sync and unmount** flush everything and clear the dirty flag again (the
next change sets it again; the log stays empty). "Safely Remove Hardware",
shutdown and reboot sync all volumes; a stick pulled without that keeps the
dirty flag, and Windows checks it, which is the safe outcome.

**Damage found while mounted** (a torn record, a run list outside the volume,
an index loop, an attribute list pointing to another file's record, a free
record in use...) fails the operation with `E_IO`, logs once, and makes the
volume read-only (*"... needs a check in Windows (chkdsk)."*); the dirty flag
is then never cleared, so Windows checks the volume.

## Limits and robustness

Everything read from the disk is untrusted: lengths and offsets are checked
against their containers before use, run lists must lie in the volume, loops
are bounded by sizes known in advance (attribute lists by their length,
extents by 512 per attribute, runs by 2^20, index trees by depth 24 and one
visit per block, paths by 256 components), sizes above 2^52 bytes are refused,
and buffers are sized from the volume's record and index block sizes (at most
64 KiB). The driver uses about 400 KiB of heap per volume (caches, `$UpCase`)
plus, per open compressed file, two compression units.

Large directories: a listing walks each index block once (a 100,000-entry
directory lists in 0.15 s on the host); lookups descend the tree (about 40 µs
on the host). Short names in listings come from a map of the directory's DOS
names (up to 8192 per listing; beyond that short names stay empty).

## Tests

All in `tests/host` (scenarios of `fstest`; `tests/host/run.sh` runs them,
`make -C tests/host test ARGS="ntfs ntfsw"` runs some). They need root (FUSE),
mkntfs, ntfs-3g, ntfsfix and ntfsresize, and are skipped with a message
otherwise. The kernel sources are compiled unmodified with ASan and UBSan.

- **Images** (`ntfs_mkimg.py`): mkntfs formats, the ntfs-3g FUSE driver
  populates: files from 0 bytes to 3 MiB (resident and not), a 14-level tree,
  Unicode names (also names differing only in case and letters whose case
  pairs come from `$UpCase`), hard links, a file with 160 hard links (a
  non-resident attribute list, names in extension records), sparse files,
  LZNT1-compressed files (`-o compression` and the compressed attribute set
  through `system.ntfs_attrib_be`), Interix and reparse-point symbolic links
  and a junction (`system.ntfs_reparse_data`), alternate data streams (`-o
  streams_interface=windows`), DOS names (`system.ntfs_dos_name`), two
  fragmented files with hundreds of runs each (multi-extent `$DATA` in
  attribute lists), a directory of 3000 files, hidden / read-only / system
  attributes.
- **`ntfs_check.py`**, an independent checker written in Python from the
  documentation: fixups, record and attribute headers, run lists, attribute
  lists, `$MFT:$BITMAP`, `$MFTMirr`, cluster accounting against `$Bitmap`,
  every `$I30` B+ tree (order by `$UpCase` collation, one leaf depth, no empty
  non-root node, blocks used once and marked), keys against the targets'
  `$FILE_NAME`s, duplicated sizes and attributes, every name indexed, link
  counts, the dirty flag and `$LogFile`.
- **`ntfs`** (reads): the image above with 512 B, 1, 4, 16 and 64 KiB clusters
  and 512 / 4096-byte sectors (7 images); every file, directory and link is
  compared with the image mounted read-only by ntfs-3g (names, types, sizes,
  modification times, contents: about 32,000 checks per image), plus free
  space, label, serial, case-insensitive lookups, short names, hidden system
  files, attributes, streams not shown, writes refused to read-only,
  compressed and sparse files.
- **`ntfsw`** (writes): random operations (create, write anywhere, truncate,
  delete, mkdir, rmdir, rename and move of files and directories, case-only
  renames) mirrored in a model, 1500 per configuration (4 KiB, 512 B, 64 KiB
  clusters, 4096-byte sectors, and a 12 MiB volume that is full most of the
  time); every few dozen operations the disk is unplugged and checked, and at
  the end the image must pass `ntfsfix -n`, `ntfsresize -i -n` (cluster
  accounting), `ntfs_check.py` (clean, no errors) and match the model when
  mounted by ntfs-3g. `NTFS_PARANOID=1` runs `ntfs_check.py` after every
  operation; `NTFS_SEED` / `NTFS_OPS` change the run. Writes into the
  ntfs-3g-populated image (data of existing files, the 160-link file renamed
  and half its links deleted, a third of the 3000-file directory deleted and
  600 files added, fragmented / compressed / sparse / stream / link files
  written or deleted, directories moved) are applied to a host copy of the
  original tree too, and the result mounted by ntfs-3g must equal it (`diff
  -r`). 5000 / 3000 files created and deleted in random order in one
  directory (index splits and merges, MFT growth to 5000 records). Volumes made
  by Halcyon's formatter (`ntfs_format`, four geometries) mount read-write,
  take writes and random operations and stay clean.
- **`ntfssafe`**: the dirty and chkdsk flags, `$LogFile` with clean, clean-flag,
  not clean and garbage restart areas, `hiberfil.sys` with `hibr`, `HIBR`,
  `Hibr`, `RSTR`, `rstr` (read-only) and `wake` or zeros (writable), an
  internal disk with and without `Windows\System32` (`ro_policy`, the setting,
  `ntfs_settings_changed`, `vfs_remount`), internal and dirty, a
  write-protected disk (no device write at all), a destroyed boot sector (the
  backup), `$MFTMirr` differing, the dirty flag set during a session and cleared
  by sync and unmount, `$LogFile` emptied by the first write; WOF and EFS
  files (listed, refused, deletable); hostile structures with valid fixups (an
  index whose blocks point to themselves, a run outside the volume, sizes of
  2^62): the operation fails, nothing crashes, the volume turns read-only.
- **`ntfsbig`**: a directory of 100,000 files made by ntfs-3g (a multi-level
  tree with a multi-extent allocation): listing (complete and in order),
  2000 lookups in another case, 5000 creates and 5000 deletes in it,
  checked afterwards (`NTFS_BIG=N` for another size).
- **Fuzzing:** `fuzz_seeds.sh` adds NTFS seeds (512 B to 4 KiB clusters, 4 KiB
  sectors on 512-byte devices, a volume larger than its device, an MBR
  partition, read-only and fault-injection configurations); the storage
  fuzzer (`make -C tests/host fuzz-run`) mounts, walks, reads and writes them.
- **QEMU** (`tools/boottest.py xhci:bios:ntfsrw`, in the default matrix): an
  internal SATA disk with an NTFS partition made on the host; it must mount
  read-only as an internal Windows drive; `NTFSTEST.EXE` reads files, allows
  writing through `volume_set_write`, writes, moves and deletes, and takes the
  permission back; on the host the volume must pass `ntfsfix -n` and
  `ntfs_check.py` and hold the changes when mounted by ntfs-3g.

Not covered by the tests: volumes written by Windows itself and Windows'
chkdsk on volumes written by Halcyon (no Windows in the CI; see HARDWARE.md).

## Not done

- Writing to compressed, sparse and encrypted files; growing or shrinking a
  run list that spans several extents (the reading side handles all of
  them); turning a file without an attribute list into one when its run list
  no longer fits in its record (the write fails with `E_NOSPC`).
- Following symbolic links and junctions; reading WOF / Compact OS files
  (XPRESS / LZX) and deduplicated files.
- Generating DOS (8.3) names; writing `$Secure`, `$UsnJrnl` records, quotas
  (Windows rebuilds what it needs).
- Creating hard links (the VFS has no operation for it).
