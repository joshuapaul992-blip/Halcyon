# Halcyon

![Halcyon desktop](docs/screenshot.png)

Halcyon is a small x86-64 hobby operating system with an early-2000s style
desktop: glossy blue title bars, a Start menu, a taskbar with a clock, notification
balloons, and desktop icons. It boots from a USB stick on BIOS and UEFI PCs and has
its own drivers for USB, SATA and FAT file systems.

## Features

| Area | What's there |
|------|-------------|
| Boot | Custom MBR plus stage 2 for BIOS (unreal mode, reads `\HALCYON\KERNEL.BIN` from FAT32 via INT 13h), and a UEFI loader (`\EFI\BOOT\BOOTX64.EFI`, GOP framebuffer). One image boots both ways. |
| Kernel | 64-bit, identity mapped, TSC timing, PCI enumeration, ACPI S5 power-off, PS/2 keyboard and mouse. |
| Threads and programs | Kernel threads with a scheduler (copies, moves and deletes run in the background with a progress dialog and Cancel). Programs run in user mode in their own address space: static position-independent ELF64 `.EXE` files loaded from disk (`C:\PROGRAMS`), preempted every 10 ms, with system calls for files, memory, processes, the console and windows (an early-2000s style window, drawing and controls API). A program that crashes gets a "has encountered a problem and needs to close" dialog and takes nothing else down. Task Manager (Ctrl+Alt+Del) ends programs that hang. |
| Interrupts | Local APIC (xAPIC or x2APIC) and I/O APICs from the ACPI MADT, or the 8259 PICs. MSI-X and MSI for xHCI and AHCI. PCI INTx for UHCI, OHCI and EHCI: without an AML interpreter the routing is learned from which device raised an interrupt. The CPU halts when there is nothing to do: a one-shot local APIC (or PIT) timer wakes it for the next deadline. A device whose interrupt does not work is polled. |
| USB | Host controllers: UHCI and OHCI (USB 1.1), EHCI (USB 2.0, with companion hand-off), and xHCI (USB 3.x, SuperSpeed). Class drivers: hubs (USB 2 and SuperSpeed), HID keyboards (boot protocol), HID mice and tablets (report descriptor parsing), and mass storage (Bulk-Only Transport plus SCSI). Hot-plug. |
| Storage | AHCI SATA (LBA48, multiple controllers). NVMe SSDs (MSI-X, MSI, INTx or polled; every active namespace with 512- or 4096-byte blocks becomes a disk, `nvme0`, `nvme1`, ...; flushed when the drive has a volatile cache, shutdown notification on Turn Off/Restart). CD/DVD drives on SATA (ATAPI): read-only drive letters, discs detected when inserted, ejected or swapped, Eject in the drive's right-click menu. Block layer with MBR (including extended partitions), GPT and superfloppy detection. |
| File systems | FAT12, FAT16 and FAT32, read and write: long file names, create, delete, rename and move, mkdir and rmdir, truncate, FSInfo. exFAT, read and write (USB sticks and SD cards over 32 GB): fragmented and contiguous files, ValidDataLength, the volume's up-case table for case-insensitive names, VolumeDirty handling. DOS drive letters; the boot drive is `C:`. Quick format (FAT12/16/32 and exFAT) from Explorer (right-click a drive) or the `FORMAT` command, with Windows' default cluster sizes. |
| CD/DVD | ISO 9660 with Joliet (long Unicode names) and Rock Ridge names, multi-sector directories and multi-extent files, hardened against corrupt discs. Hybrid ISO images written to USB sticks or disks mount too. |
| Unicode | File names and text are UTF-8 throughout; FAT long names are stored as UTF-16 (including characters outside the BMP), with Unicode case-insensitive lookup. The fonts cover Latin, Greek, Cyrillic and common symbols; other characters are kept and drawn as a replacement box. |
| GUI | Compositing window manager (move, resize, minimize, maximize), menus, dialogs, widgets, three colour themes, four procedural wallpapers. |
| Apps | Explorer (My Computer, file management), Notepad, Command Prompt (DOS-style shell with redirection), System Information (device tree), Paint (BMP), Calculator, Sonar (a mine-hunting puzzle), Control Panel, About. |

## Building

You need gcc, binutils, nasm, clang and lld (for the UEFI loader), python3 with Pillow
(it renders fonts and icons at build time), sfdisk, mtools and dosfstools.

```bash
make
```

This produces `build/halcyon.img`, a 128 MiB disk image with an MBR partition table and
one bootable FAT32 partition. Files in `rootfs/` are copied onto it.

## Running in QEMU

```bash
tools/run.sh xhci bios
```

- First argument: the USB controller the stick is attached to. One of `uhci`, `ohci`,
  `ehci`, `ich9` (EHCI with UHCI companions) or `xhci`.
- Second argument: `bios` or `uefi` (OVMF).
- Extra QEMU options go in `EXTRA`, and a CPU model in `CPUARG` (e.g. `-cpu max`).
  For example, a SATA disk and a USB tablet:

```bash
EXTRA="-drive if=none,id=d0,format=raw,file=build/fat16.img -device ide-hd,drive=d0,bus=ide.0 -device usb-tablet,bus=usb.0" tools/run.sh xhci uefi
```

`make testdisks` builds sample FAT12, FAT16 and GPT/FAT32 images for testing.

## Writing it to a USB stick

**This erases the stick.** Replace `/dev/sdX` with your stick's device name
(check it with `lsblk`).

```bash
sudo dd if=build/halcyon.img of=/dev/sdX bs=4M conv=fsync status=progress
```

On Windows, use a raw image writer such as Rufus (in "DD image" mode) or balenaEtcher.
Then boot from the stick. On UEFI machines, turn off Secure Boot: the loader is not signed.

## Troubleshooting and testing on real PCs

- Press **F8** at the "Press F8 for Advanced Startup Options" prompt to open the
  advanced startup menu. It has verbose boot (the kernel log on screen), Safe Mode,
  switches that disable individual drivers (xHCI, EHCI, UHCI/OHCI, AHCI, ACPI),
  interrupt fallbacks (no interrupts at all, no MSI, legacy PIC instead of the I/O
  APIC), and an 800×600 safe video mode.
- The Command Prompt's `IRQ` command shows the interrupt mode, the timer, CPU usage
  and every interrupt source with its count. System Information shows the same under
  Computer → Interrupts.
- The kernel log is saved to `\HALCYON\BOOT.LOG` on the stick when the desktop
  starts, on shutdown or restart, and after a crash. The previous boot's log is kept
  as `BOOTPREV.LOG`.
- See [docs/HARDWARE.md](docs/HARDWARE.md) for the test checklist and the
  compatibility list.

## Tests

```bash
tools/ci.sh
```

This runs the following checks; `.github/workflows/ci.yml` runs the same script:

- a warning-free build
- the host storage test suite (`tests/host/run.sh`, about 110,000 checks): the
  FAT, exFAT and ISO 9660 drivers, Unicode names and formatting, compiled for the
  host with ASan/UBSan and run against images made by mkfs.fat, mkfs.exfat and
  xorriso. Results are cross-checked with mtools, `fsck.fat`, `fsck.exfat` and an
  independent exFAT implementation in Python (`tests/host/exfat_tool.py`), and
  a few hundred deliberately corrupted images plus the fuzzer's regression
  inputs must fail cleanly
- a QEMU boot matrix (`tools/boottest.py`) covering UHCI, OHCI, EHCI, EHCI with
  companions, and xHCI, under BIOS and UEFI, plus the interrupt fallbacks (legacy
  PIC, no MSI, no interrupts) set through a QEMU-only `fw_cfg` hook, NVMe disks
  (512- and 4096-byte blocks, INTx, polled), a CD drive with a disc swap, and
  user mode: `SELFTEST.EXE` runs after boot and checks the system calls, bad
  pointers, crashing and looping child programs, preemption and windows

The partition, FAT, exFAT and ISO 9660 code can also be fuzzed with libFuzzer, ASan and UBSan
(needs clang and `libclang-rt-dev`):

```bash
make -C tests/host fuzz-run FUZZ_TIME=900 FUZZ_JOBS=2
```

Each boot must reach the desktop, mount the boot stick and a SATA disk, and write
`BOOT.LOG`. With interrupts on, the stick's USB controller must get a working
interrupt and QEMU must use under 40% of a host CPU at the idle desktop (it uses
0–6%; 100% with interrupts off).

## Writing programs

Programs live in `userland/programs/*.c`, one file each, and use the API in
`userland/include/hal.h`: a small C library (strings, `printf`, `malloc`, files) and
windows with drawing calls, standard controls (buttons, text boxes, list boxes),
message boxes and file dialogs. `make` builds each into `build/user/NAME.exe` and
puts it on the image as `C:\PROGRAMS\NAME.EXE`. Type its name in the Command Prompt
(console output and input go to that window) or double-click it in Explorer.
`HELLO.EXE` is a console example; `CRASHME.EXE` (Crash Test) fails in nine
different ways on purpose; `TASKMGR.EXE` is Task Manager.

## Layout

```
boot/        MBR, BIOS stage 2, bootinfo contract shared with the kernel
efi/         UEFI loader
kernel/core  entry, CPU and timing, interrupts and idle, memory, library, panic screen
kernel/drivers  PCI, PS/2, input, AHCI (disks and CD/DVD), NVMe, ACPI
kernel/usb   USB core, hub/HID/MSC class drivers, UHCI/OHCI/EHCI/xHCI
kernel/fs    block layer and partitions, VFS, FAT, exFAT, ISO 9660
kernel/gui   graphics, window manager, widgets, menus, dialogs, desktop, splash
kernel/apps  applications built into the kernel
userland/    user-mode runtime (crt0, C library subset, GUI calls) and programs
tools/       image builder, font and icon generators, QEMU test harness
tests/host   host-side storage tests and fuzzer
```

## Limitations

- One CPU core. Kernel threads are cooperative (one big kernel lock); only
  user programs are preempted. Without a working timer interrupt (F8 "Disable
  interrupts") programs are not preempted, so one that loops forever hangs the
  desktop.
- Programs can't create threads or share memory yet, and the built-in
  applications (Explorer, Notepad, Paint...) still run in the kernel.
- PCI INTx routes are learned, not read from ACPI (no AML interpreter). A device
  that shares its line with a device that has no driver may stay polled.
- x2APIC mode is written to spec but QEMU 8.2's emulator cannot run it.
- No networking, audio or isochronous USB.
- Keyboard layout is US only, so non-ASCII characters can be displayed and kept in
  names but not typed.
- No CJK glyphs: Chinese, Japanese and Korean text shows as replacement boxes.
- Some code paths are written to spec but can't be exercised in QEMU:
  - EHCI split transactions
  - xHCI SuperSpeed hubs and Transaction Translators
  - LBA28-only disks
