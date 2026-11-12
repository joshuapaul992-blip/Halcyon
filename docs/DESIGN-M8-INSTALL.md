# M8: the installer (Halcyon Setup)

Halcyon Setup installs Halcyon from the USB stick it is running from onto an
internal SATA or NVMe disk: the whole disk, or the free space next to
partitions that are already there (Windows), which are never shrunk, moved
or written to. The installed disk starts with a PC BIOS and with UEFI where
the layout allows it, and Windows on the same disk keeps starting.

| Part | File |
|---|---|
| Partition tables (MBR and GPT reader and writer) | `kernel/fs/parttab.c`, `parttab.h` |
| The engine: disks, plan, installation | `kernel/apps/instcore.c`, `instcore.h` |
| The wizard, the welcome prompt, the test hook | `kernel/apps/installer.c` |
| UEFI runtime services (boot entries) | `kernel/core/efirt.c`, `efirt.h` |
| BIOS boot: patchable MBR, GPT-aware stage 2, OS menu | `boot/mbr.asm`, `boot/stage2.c`, `boot/stage2_entry.S` |
| UEFI loader: runtime services and GUIDs for the kernel | `efi/loader.c`, `boot/bootinfo.h` |
| Host tests | `tests/host/parttest.c`, `tests/host/gpt_check.py` |
| Boot tests, screenshots | `tools/boottest.py` (the `install` set), `tools/installshots.sh` |

## What the user sees

- **Start → Install Halcyon** (right column of the Start menu) opens the
  wizard. When Halcyon started from a removable disk (USB stick) and the
  computer has an internal disk, a **Welcome to Halcyon** prompt offers it
  once the desktop is up ("Install Halcyon...", "Not now", and "Don't show
  this again", which sets `install.welcome=0` in `SETTINGS.INI`).
- The wizard, in the style of Windows XP's wizards (a banner page to start
  and to finish, interior pages with a white header):
  1. **Welcome.**
  2. **Choose a disk:** every internal SATA and NVMe disk with its model,
     name and size; below the list, what is on the selected disk: the
     partition table, each partition with its file system, label, drive
     letter, size and type, "Windows" (an NTFS volume with
     `\Windows\System32`) or "Windows Boot Manager" (a FAT volume with
     `\EFI\Microsoft\Boot\bootmgfw.efi`), and the unpartitioned space.
     Disks that cannot be used are listed with the reason (the disk Halcyon
     runs from, read-only, removable, too small, a sector size other than
     512 or 4096 bytes).
  3. **Where to install:** *Use the whole disk* (everything on it is erased)
     or *Install in the free space* (only offered when there is a free
     region of at least 128 MB and the table has room for a partition; the
     reason is shown otherwise).
  4. **Ready to install:** the disk, what happens to it, how Halcyon will
     start (UEFI boot entry, BIOS boot code, or why not), and how many files
     are copied. *Install* starts; for the whole disk it first asks "All data
     on Disk 1, <model> (<name>), <size>, will be erased: <the partitions> —
     Are you sure...?" with **No** as the default button and Esc meaning No.
  5. **Installing Halcyon:** a progress bar, the file being copied or
     verified, and the steps ticked off. Back, Next, Cancel and the close
     button do nothing until it is finished.
  6. **Finish:** where Halcyon was installed, warnings (e.g. no firmware boot
     entry), how to start Windows, and *Restart* (or the error, and whether
     the disk was changed).

`tools/installshots.sh OUTDIR [DISK]` takes a screenshot of every page,
the welcome prompt and the Start menu (it was used to check their layout).

## Built into the kernel, not a program

The installer is a kernel application like Format, not a user program with
new system calls. It needs raw access to whole disks (partition tables, the
first sector, the gap after it), to re-read a disk's partitions and register
new ones in the block layer, to unmount volumes, to format through
`vfs_format`, and to call the UEFI runtime services. Giving programs raw disk
writes would need a permission model that Halcyon does not have yet (M17:
"permissions between programs"; today any program may do anything); a
system call only the installer may use would be the same code with an ABI in
front of it. No system calls were added.

The work runs on a kernel thread (`inst_run`); the wizard watches it from
its 10 Hz tick, so the desktop stays responsive (DESIGN-M4: kernel threads
are cooperative, the thread gives up the CPU at every disk wait and after
every file). Raw writes to the disk happen with `storage_lock` held; the
copy goes through the VFS, which takes the lock per call.

## The layouts

Partitions start and end on 1 MiB boundaries. Halcyon's partition is FAT32
(formatted by `vfs_format(part, "FAT32", "HALCYON", 0)`), at least 128 MB,
and at most 2^32 - 1 sectors (the FAT32 limit).

| Case | Table | Partitions | BIOS | UEFI |
|---|---|---|---|---|
| Whole disk, started with UEFI (or a disk over 2 TiB, or 4 KB sectors) | GPT, 128 entries | 1: Halcyon, EFI system partition type, from 2 MiB to the end; 2: BIOS boot partition, 1 MiB at 1 MiB | MBR boot code in the protective MBR, stage 2 in the BIOS boot partition | `\EFI\BOOT\BOOTX64.EFI` on Halcyon's partition, and a "Halcyon" boot entry |
| Whole disk, started with a BIOS | MBR, random disk signature | 1: Halcyon, type 0x0C, active, from 1 MiB | MBR boot code, stage 2 at LBA 1-64 (as on the stick) | `\EFI\BOOT\BOOTX64.EFI` (UEFI firmware starts FAT partitions on MBR disks too) |
| Free space on a GPT disk | the existing GPT | + BIOS boot partition (1 MiB) and Halcyon (EFI system partition type) | if the protective MBR's code is empty or Halcyon's, the sector size is 512 and two entries are free | a "Halcyon" boot entry |
| Free space on an MBR disk | the existing MBR | + Halcyon, type 0x0C, in a free primary entry; active only if no partition is | if the 64 sectors after the MBR are empty (or hold Halcyon's stage 2) and the MBR's code is a standard one (see below) | a "Halcyon" boot entry (MBR signature in the device path) |
| 4 KB sectors | GPT | Halcyon from 1 MiB | none: PC BIOSes cannot start such disks | as above |

If the computer started with a BIOS and the disk cannot get BIOS boot code,
the free-space installation is refused with the reason.

**Why a BIOS boot partition on GPT.** Stage 2 lives right after the MBR on
the stick (LBA 1-64). On a GPT disk LBA 1 is the GPT header and LBA 2-33 the
entries, so stage 2 needs a place of its own: a 1 MiB partition of the "BIOS
boot partition" type (21686148-6449-6E6F-744E-656564454649, the type GRUB
uses for the same purpose), which no file system claims. `mbr.asm` now reads
stage 2 from the LBA in its `stage2_lba` field (offset 432, 1 on the stick),
which the installer patches. The protective MBR's partition entry is not
marked active (as the UEFI specification requires); a few old BIOSes refuse
to start a disk without an active partition, they would need a hybrid MBR,
which Halcyon does not write.

**How stage 2 finds Halcyon.** It no longer takes the first FAT partition:
on an MBR disk it probes the active FAT partition first, then the others; on
a GPT disk every entry, for a FAT32 volume with `\HALCYON\KERNEL.BIN`. So a
Windows EFI system partition (FAT32, no `\HALCYON`) before Halcyon's is
skipped. On GPT it hands the kernel the partition's GUID instead of the
protective MBR's (meaningless) disk signature.

**How the kernel finds its boot volume.** As before by MBR signature and
start LBA; for a GPT partition (UEFI and BIOS) by its unique partition GUID,
which the block layer now records for GPT partitions. The installer gives
every new MBR disk a random signature that no other disk in the computer
has, so the stick and the installed disk are never confused (both are
labelled HALCYON, and the old label fallback would pick the first one).

**What is written where.** Whole disk: every volume on it is unmounted
first (refused if one has open files), the first and last MiB of the disk
(old tables, boot code, a backup GPT) and the start of the new partitions
are zeroed, the table is written, and the block layer scans it again. Free
space: the partition is added with `pt_add_mbr` / `pt_add_gpt`, which
re-read the table and refuse if it changed since the wizard looked
(`E_BUSY`) or if the new partition would overlap anything; only then is the
start of the new partitions zeroed, and the new partition is registered
with `blockdev_add_part` without touching the mounted ones. In every case
the table is read back and must contain Halcyon's partition as planned.

## Partition tables (parttab.c)

Pure code over a read/write callback pair, so the host tests run it on
image files.

- `pt_read`: MBR (primary entries, the extended partition's chain of
  logical partitions, overlap checks; a FAT/NTFS boot sector at LBA 0 is a
  "whole-disk file system", not a table) or GPT (primary header and entry
  array with their CRC32s; a damaged one is reported and only "whole disk"
  is offered). A missing or damaged backup GPT is not a reason to refuse:
  the next addition writes it again.
- `pt_free_regions`: the gaps between partitions, aligned, largest first;
  MBR: below 2^32 sectors and outside the extended partition, and only with a
  free primary entry; GPT: between the first and last usable LBAs, with a
  free entry.
- `pt_write_mbr`, `pt_write_gpt`: new tables (GPT: protective MBR with one
  0xEE entry over the disk, or 0xFFFFFFFF sectors beyond 2 TiB; primary
  header at LBA 1, entries from LBA 2, backup entries before the backup
  header in the last sector; both CRC32s; random version-4 GUIDs).
- `pt_add_mbr`, `pt_add_gpt`: one MBR entry in a free slot; GPT entries in
  the first free slots, both headers rewritten with the new array CRC. Every
  other byte of the table stays as it was: the other systems' entries, their
  attribute bits, names, the disk GUID, the usable range, the entry size and
  count.

## Boot code on a disk that has another system

On an **MBR** disk with Windows, Windows' partition is the active one and
its MBR boot code (the standard code in every Windows and DOS MBR) starts
it. Halcyon replaces that code with its own, after saving the disk's first
sector as `\HALCYON\BOOT\MBRSAVE.BIN` on its partition, and leaves the
active flag and Windows' entry alone. Stage 2 notices that the active
partition is not Halcyon's and shows Windows XP's boot menu:

```
Please select the operating system to start:

    Halcyon
    Windows
...
Seconds until highlighted choice will be started automatically: 10
For troubleshooting and advanced startup options for Halcyon, press F8.
```

"Windows" when the partition's boot record says NTFS, "Other operating
system" otherwise; `boot.timeout` in `SETTINGS.INI` sets the countdown
(default 10 s, 0 waits). Choosing Windows loads its partition's boot record
to 0000:7C00 and jumps there in real mode (64 KiB segment limits again, DL =
the drive, DS:SI = its partition entry), exactly as the standard MBR code
would. The installer refuses to replace MBR code that belongs to another boot
loader (it looks for GRUB, LILO, SYSLINUX and similar signatures) and boot
code in the gap after the MBR that is not Halcyon's: Linux installations
with GRUB keep working, Halcyon then starts only through UEFI or not on
that disk (the wizard says why). To go back to Windows' own MBR code: write
`MBRSAVE.BIN`'s first 440 bytes back, or run `bootrec /fixmbr` in Windows'
recovery console.

On a **GPT** disk Windows starts through UEFI only; the protective MBR's
code is not used by it, so Halcyon writes its code there only if it is empty
(or Halcyon's), plus the BIOS boot partition. Windows' partitions, its EFI
system partition and every file on it are left alone.

## Windows and UEFI: where Halcyon's loader goes

The firmware starts an operating system from a boot entry (`Boot####`
variables and `BootOrder`) or, without one, from the fallback file
`\EFI\BOOT\BOOTX64.EFI` of a disk's (first) EFI system partition. On a disk
with Windows that fallback is Windows' own (a copy of its boot manager), so
Halcyon needs a boot entry. The options were:

1. **Halcyon's loader in Windows' EFI system partition** (`\EFI\Halcyon\`)
   plus a boot entry. This is what Linux distributions do. It writes into a
   partition that belongs to Windows (often only 100 MB, sometimes nearly
   full, rewritten by Windows updates and `bcdboot`), and the loader would
   then have to find Halcyon's partition elsewhere. It breaks the rule that
   Halcyon never writes to another system's partitions.
2. **A separate EFI system partition for Halcyon** plus a boot entry
   (chosen). Halcyon's FAT32 partition gets the EFI system partition type
   GUID and holds `\EFI\BOOT\BOOTX64.EFI` itself; the boot entry points at
   it. The UEFI specification allows several EFI system partitions on a
   disk; Windows uses the one its boot manager is on and hides both in
   Explorer (so Halcyon's files are not shown as a drive in Windows). Nothing
   of Windows' is written; its `\EFI\Microsoft` and `\EFI\BOOT` stay as they
   are (the `installgpt` boot test checks the bytes).
3. A separate partition without a boot entry: firmware would keep starting
   Windows (its ESP comes first); the user would have to pick the disk's
   second ESP by hand, which many firmware menus cannot do.
4. Letting a loader register the entry at the next start: the installed
   loader never runs without an entry, and the stick's loader could only
   do it if the stick is started again.

**The boot entry is written by the kernel through the UEFI runtime
services**, right after the copy is verified:

- The UEFI loader passes `SystemTable->RuntimeServices` and the memory map
  ranges marked `EFI_MEMORY_RUNTIME` in the boot information. The kernel
  never calls `SetVirtualAddressMap`, so the services stay at their physical
  addresses, which the kernel's identity map covers: the first 4 GiB are
  mapped anyway, ranges above are mapped (uncached) before the first call.
  Runtime code and data are not usable memory in the converted memory map,
  so the kernel never allocates them.
- A call (`efi_thunk`) switches to a private 64 KiB stack (the UEFI
  specification asks callers for a generous stack; kernel threads have
  32 KiB), turns interrupts off, moves the arguments to the Microsoft x64
  convention with the 32-byte shadow space, and restores everything after.
- If the firmware faults (a CPU exception in kernel mode while
  `efi_in_call` is set), the exception handler resumes at `efi_recover`:
  the call returns an error with the caller's stack and registers, the
  runtime services are not used again, and the installer reports the
  missing boot entry instead of the system stopping.
- `efi_add_boot_entry("Halcyon", HD(partition)/File(\EFI\BOOT\BOOTX64.EFI))`
  builds an `EFI_LOAD_OPTION` with a short-form hard drive device path (GPT:
  the partition GUID; MBR: the disk signature, as Windows' own entries do),
  reuses a "Halcyon" entry of the same partition (installing again) or takes
  the first free `Boot####` number, writes it non-volatile, puts it first in
  `BootOrder`, and reads both variables back to compare them.
- If the services are missing or fail, the installation still succeeds and
  the finish page says how to start Halcyon (the firmware's boot menu, or
  adding the file in the firmware setup).

The loader also tells the kernel which boot entry started it (`BootCurrent`
and its description, read with boot services); the kernel logs it ("efi:
started from firmware boot entry Boot0003 'Halcyon'"), which the boot tests
check.

Halcyon first in `BootOrder` makes it the default; Windows stays in the list
and in the firmware's boot menu (usually F12, F11, F8 or Esc). Some
firmware reorders or drops entries it did not create; then the boot menu
still offers "Halcyon" or the disk. Secure Boot must be off (the loader is
not signed).

## Copying and verifying

Everything on the boot volume is copied, recursively, except `BOOT.LOG`,
`BOOTPREV.LOG`, hidden system folders at the root (Recycle Bins, System
Volume Information), volume labels and links: the kernel, both loaders,
`\HALCYON\BOOT\MBR.BIN` and `STAGE2.BIN` (the BIOS boot code, which
`mkimage.sh` now also puts there as files, so an installed Halcyon can
install again), `\PROGRAMS`, `\HALCYON\MEDIA`, `\Documents` and
`SETTINGS.INI`, saved just before (so the installed system keeps the
display, keyboard, network and other settings from the stick). The plan
checks that it fits with room to spare.

Then the volume is synced, unmounted and mounted again, so that nothing comes
from a cache, and every file is read back and compared with the original
byte for byte. The boot code and stage 2 are read back after writing, and
the partition table is read back and parsed. Any difference fails the
installation with the file's name.

## Tests

- `tests/host/parttest.c` (run by `tests/host/run.sh`, 874 checks): new MBRs
  and GPTs on 512- and 4096-byte-sector images (4K through a loop device
  for sfdisk and sgdisk), partitions added next to Windows-like layouts made
  by sgdisk / sfdisk (ESP, MSR, basic data, recovery) and to MBRs with
  extended partitions, with the other entries compared byte for byte and the
  data in the other partitions hashed; refusals (full tables, overlaps,
  stale tables, two active partitions, MBR past 2 TiB); damaged headers and
  entry arrays; a missing backup GPT repaired; a 3 TiB disk; 60 random
  layouts written, read back and filled. Every table is checked by
  `gpt_check.py` (an independent reader written from the UEFI specification,
  CRC32s from Python's zlib), `sfdisk -V`, `sfdisk -d` and `sgdisk -v`.
- `tools/boottest.py install` (QEMU; the installer's test hook
  `opt/halcyon/installtest` = "DISK whole|free" runs the engine as the
  wizard would): `install` (BIOS and UEFI, blank SATA disk), `installnvme`,
  `installnvme4k`, `installntfs` (MBR disk with an active Windows NTFS
  partition, free space, BIOS; the boot menu's countdown and choosing
  Windows, whose boot sector must then run), `installgpt` (Windows-like GPT
  with ESP, MSR and NTFS, free space, UEFI). After each installation, on the
  host: the table (gpt_check, sfdisk, sgdisk), fsck.fat, the kernel, loaders,
  stage 2, a program and a sound compared with the build, Windows'
  partitions hashed before and after, ntfsfix -n, Windows' GPT entries and
  MBR entry unchanged, `MBRSAVE.BIN`; then QEMU starts from the installed
  disk alone with BIOS, with UEFI and a fresh variable store (fallback file),
  and with the installer's variable store (must start through the "Halcyon"
  entry), each to the desktop with that disk as C:. `xhci:uefi:install` and
  `xhci:bios:install` are in the default matrix (about 30 and 20 s);
  `tools/ci.sh full` runs the whole set.

## Limits and what is not done

- Existing partitions are never shrunk or moved: without free space next to
  Windows the only choice is the whole disk (resizing comes with Disk
  Management, M26).
- MBR disks: a free primary entry is needed (Halcyon does not create logical
  partitions), and only space below 2 TiB is used. The boot menu offers one
  other system: the active partition.
- Windows' MBR code is replaced on MBR disks (saved in `MBRSAVE.BIN`);
  another boot loader's code is never replaced, so on such disks Halcyon
  starts with UEFI only (or the installation is refused on a BIOS computer).
- GPT and BIOS: the protective MBR is not marked active; BIOSes that insist
  on an active partition will not start the disk.
- The boot entry needs working UEFI runtime variable services; the call is
  protected against faults but not against firmware that hangs. It was only
  tested with OVMF (QEMU), not on real firmware.
- No cancel once the disk is being written; an interrupted installation
  leaves a partial Halcyon partition. On a whole disk, Setup simply runs
  again; next to Windows the partition has taken the free space, so it must
  be deleted with another tool first (Halcyon has no Disk Management yet).
- File times are not copied (the copies get the time of the installation).
- No uninstall, no update of an existing installation in place (M17).
- Real hardware is untested; QEMU covers AHCI and NVMe (512 and 4096-byte
  blocks), SeaBIOS and OVMF.
