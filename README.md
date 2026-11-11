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
| Kernel | 64-bit, identity mapped, polled (no IRQs), TSC timing, PCI enumeration, ACPI S5 power-off, PS/2 keyboard and mouse. |
| USB | Host controllers: UHCI and OHCI (USB 1.1), EHCI (USB 2.0, with companion hand-off), and xHCI (USB 3.x, SuperSpeed). Class drivers: hubs (USB 2 and SuperSpeed), HID keyboards (boot protocol), HID mice and tablets (report descriptor parsing), and mass storage (Bulk-Only Transport plus SCSI). Hot-plug. |
| Storage | AHCI SATA (LBA48, multiple controllers). Block layer with MBR (including extended partitions), GPT and superfloppy detection. |
| File systems | FAT12, FAT16 and FAT32, read and write: long file names, create, delete, rename and move, mkdir and rmdir, truncate, FSInfo. DOS drive letters; the boot drive is `C:`. |
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
- Extra QEMU options go in `EXTRA`. For example, a SATA disk and a USB tablet:

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

## Layout

```
boot/        MBR, BIOS stage 2, bootinfo contract shared with the kernel
efi/         UEFI loader
kernel/core  entry, CPU and timing, memory, library, panic screen
kernel/drivers  PCI, PS/2, input, AHCI, ACPI
kernel/usb   USB core, hub/HID/MSC class drivers, UHCI/OHCI/EHCI/xHCI
kernel/fs    block layer and partitions, VFS, FAT
kernel/gui   graphics, window manager, widgets, menus, dialogs, desktop, splash
kernel/apps  applications
tools/       image builder, font and icon generators, QEMU test harness
tests/host   host-side FAT driver tests
```

## Limitations

- No multitasking or user mode. Everything runs in one polled kernel loop.
- No networking, audio or isochronous USB.
- Keyboard layout is US only.
- Some code paths are written to spec but can't be exercised in QEMU:
  - EHCI split transactions
  - xHCI SuperSpeed hubs and Transaction Translators
  - LBA28-only disks
