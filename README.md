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
| Threads and programs | Kernel threads with a scheduler (copies, moves and deletes run in the background with a progress dialog and Cancel). Programs run in user mode in their own address space: static position-independent ELF64 `.EXE` files loaded from disk (`C:\PROGRAMS`), preempted every 10 ms, with system calls for files, memory, processes, the console and windows (an early-2000s style window, drawing and controls API). Pages are no-execute unless they hold code; SMEP, SMAP and UMIP are on where the CPU has them. A program that crashes gets a "has encountered a problem and needs to close" dialog and takes nothing else down. Task Manager (Ctrl+Alt+Del) ends programs that hang. Calculator, Sonar and Notepad are user programs. |
| Interrupts | Local APIC (xAPIC or x2APIC) and I/O APICs from the ACPI MADT, or the 8259 PICs. MSI-X and MSI for xHCI and AHCI. PCI INTx for UHCI, OHCI and EHCI: without an AML interpreter the routing is learned from which device raised an interrupt. The CPU halts when there is nothing to do: a one-shot local APIC (or PIT) timer wakes it for the next deadline. A device whose interrupt does not work is polled. |
| USB | Host controllers: UHCI and OHCI (USB 1.1), EHCI (USB 2.0, with companion hand-off), and xHCI (USB 3.x, SuperSpeed). Class drivers: hubs (USB 2 and SuperSpeed), HID (report descriptor parsing, report protocol as on Windows, boot protocol as the fallback): keyboards including n-key rollover and wireless receivers with several devices on one interface, multimedia keys (volume, media, browser, mail, calculator, power and sleep), mice and tablets; and mass storage (Bulk-Only Transport plus SCSI). Hot-plug. |
| Storage | AHCI SATA (LBA48, multiple controllers). NVMe SSDs (MSI-X, MSI, INTx or polled; every active namespace with 512- or 4096-byte blocks becomes a disk, `nvme0`, `nvme1`, ...; flushed when the drive has a volatile cache, shutdown notification on Turn Off/Restart). CD/DVD drives on SATA (ATAPI): read-only drive letters, discs detected when inserted, ejected or swapped, Eject in the drive's right-click menu. Block layer with MBR (including extended partitions), GPT and superfloppy detection. |
| File systems | FAT12, FAT16 and FAT32, read and write: long file names, create, delete, rename and move, mkdir and rmdir, truncate, FSInfo. exFAT, read and write (USB sticks and SD cards over 32 GB): fragmented and contiguous files, ValidDataLength, the volume's up-case table for case-insensitive names, VolumeDirty handling. DOS drive letters; the boot drive is `C:`. Quick format (FAT12/16/32 and exFAT) from Explorer (right-click a drive) or the `FORMAT` command, with Windows' default cluster sizes. |
| CD/DVD | ISO 9660 with Joliet (long Unicode names) and Rock Ridge names, multi-sector directories and multi-extent files, hardened against corrupt discs. Hybrid ISO images written to USB sticks or disks mount too. |
| Keyboard | 20 layouts that follow Windows': US, US-International, UK, German, Swiss German, French, Belgian (AZERTY), Spanish, Italian, Portuguese, Brazilian (ABNT2), Swedish/Finnish, Norwegian, Danish, Dutch, Polish (Programmers), Czech, Turkish Q, Russian and Greek. AltGr (Right Alt, or Ctrl+Alt), dead keys (a dead key followed by a key it does not compose with gives the accent, then the key), Caps Lock per key, the ISO key next to left Shift and the ABNT2 keys. Ctrl shortcuts go by the letter on the key (Ctrl+A is the AZERTY "A"; US letters on Russian and Greek). Several layouts can be installed: Left Alt+Shift or the indicator on the taskbar switches; Keyboard settings in the Control Panel or the `KEYBOARD` command. Characters reach programs as Unicode code points. |
| Unicode | File names and text are UTF-8 throughout; FAT long names are stored as UTF-16 (including characters outside the BMP), with Unicode case-insensitive lookup. The fonts cover Latin, Greek, Cyrillic and common symbols; other characters are kept and drawn as a replacement box. |
| GUI | Compositing window manager (move, resize, minimize, maximize), menus, dialogs, widgets, three colour themes, four procedural wallpapers. Theme and wallpaper are kept across restarts (`C:\HALCYON\SETTINGS.INI`). |
| Display | The loaders read the monitor's EDID and start in its native resolution (a 4K panel gets an exact fraction such as 1920×1080, since the UI has no scaling), or in the resolution chosen in Display Properties. With the firmware frame buffer a new resolution applies at the next start; the QEMU/Bochs/VirtualBox adapter switches at once, with Windows' "keep these settings?" 15-second revert. The frame buffer is write-combining (PAT). |
| Apps | Explorer (My Computer, file management), Notepad (UTF-8 text in any keyboard layout, files up to 16 MB), Command Prompt (DOS-style shell with redirection), System Information (device tree), Paint (BMP), Calculator, Sonar (a mine-hunting puzzle), Control Panel, Display Properties, Keyboard settings, About. |

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
- the host keyboard layout test (`tests/host/kbdtest.c`): keys at every level,
  Caps Lock, AltGr, dead keys and shortcut letters of each layout, written from
  the Windows layouts independently of the tables, plus checks on every table
- the host USB HID tests (`tests/host/hidtest.c`, run first by `tests/host/run.sh`):
  the report descriptor parser and report decoder against QEMU's keyboard, mouse
  and tablet, an n-key-rollover gaming keyboard, a wireless receiver, media key
  interfaces and broken, truncated and randomly mutated descriptors
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
  pointers, crashing and looping child programs, preemption and windows, and
  the display: a saved resolution chosen by both loaders, the firmware frame
  buffer alone, and a live mode switch
- the EDID parser and the loaders' resolution choice (`tests/host/displaytest.c`)

The partition, FAT, exFAT and ISO 9660 code can also be fuzzed with libFuzzer, ASan and UBSan
(needs clang and `libclang-rt-dev`):

```bash
make -C tests/host fuzz-run FUZZ_TIME=900 FUZZ_JOBS=2
```

Each boot must reach the desktop, mount the boot stick and a SATA disk, and write
`BOOT.LOG`. A USB keyboard and tablet sit on the controller under test: the
keyboard must run in report protocol, and Caps Lock sent through the QEMU
monitor must switch its LED on and off. With interrupts on, the stick's USB controller must get a working
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
different ways on purpose; `TASKMGR.EXE` is Task Manager; `CALC.EXE` and
`SONAR.EXE` are full GUI programs (they draw into their own pixel buffer and
blit it, then add text). `NOTEPAD.EXE` is Notepad: menus, a scroll bar and a
status bar from the API below, the text drawn line by line and measured with
the local font metrics, a gap buffer so editing a file of several megabytes
stays fast; the shell starts it for text files. `SELFTEST.EXE` checks the
system calls (`selftest` in the Command Prompt; the QEMU `user` test variant
runs it). `GUIDEMO.EXE` (GUI Demo) uses every part of the window API below
and logs each event it gets.

The window API in `hal.h` (comments there give the details):

- **Windows and events:** `win_create` gives a window whose client area is a
  canvas kept in the kernel; `win_get_event` returns paint, close, key, mouse,
  wheel, resize, timer, focus, command (controls), `EVT_MENU` and `EVT_SCROLL`
  events. A press in the client area captures the mouse until the buttons are
  released (moves outside the window still arrive), and mouse events carry the
  modifier keys. `win_set_cursor` picks the pointer shape (arrow, I-beam,
  wait, crosshair, hand, resize arrows), `win_get_rect` gives the client area's
  place on screen, `win_set_min_size` the smallest size the user can drag it
  to, and `win_set_owner` makes a window a (modal) dialog of another one.
- **Menus:** `menu_set` gives a window a menu bar from a flat array of
  `hal_menu_item_t` (a `MENU_TITLE` entry starts each menu; items have an id,
  shortcut text, an icon, and separator, disabled, checked and bold flags;
  `&` marks mnemonics). The bar sits above the client area, so adding,
  replacing or removing it leaves the canvas and coordinates as they were.
  `menu_popup` shows a context menu at a client position, `menu_enable` /
  `menu_check` change items by id. F10 or a tap of Alt opens the menus from
  the keyboard; Alt+letter opens one, arrows, Enter and Esc work inside.
- **Drawing:** fills, lines, rectangles, circles, bevels, icons, pixel blits,
  text (`gfx_text`, `gfx_text_n` for text without a NUL, `gfx_text_box`
  with alignment and "..." when it does not fit), gradients and the rounded,
  sunken and shadowed shapes of the theme (`gfx_shade`), whose colours
  `gfx_theme` returns. `gfx_clip` / `gfx_clip_origin` limit drawing to a
  rectangle (and move the origin to it), e.g. to draw a text area next to the
  program's own scroll bars.
- **Text measurement without a system call per string:** `text_width`,
  `text_width_n`, `text_fit` and `text_char_width` measure locally from
  advance widths the library copies from the kernel once per font and block
  of 256 characters (`gfx_font_metrics`); `utf8_get` / `utf8_put` /
  `utf8_prev` walk UTF-8 text the way the kernel draws it.
- **Controls:** buttons, labels, text boxes, check boxes, list boxes, groups,
  progress bars, vertical and horizontal scroll bars (`ctl_scroll_set`: range,
  page, position and line step; arrows repeat while held, the track pages, the
  thumb drags and the wheel scrolls), flat tool bar buttons with an icon and a
  tooltip (`ctl_set_tool`), and a status bar docked at the bottom with
  tab-separated parts. `ctl_move` lays them out again after a resize,
  `ctl_focus` moves the keyboard between controls and the program's own
  drawing, and `CTL_NOTIFY` makes Enter in a text box (or a double click in a
  list box) send a command.
- **Dialogs and clipboard:** `msgbox`, `file_dialog`, `inputbox` (a one-line
  question), `clipboard_set_n` for text up to 1 MiB, `clipboard_get`.

Every call checks the program's pointers and bounds its work, and a window's
menus, controls and canvas go away with the window or the program.

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
tests/host   host-side storage, keyboard layout, USB HID and display tests, fuzzer
```

## Limitations

- One CPU core. Kernel threads are cooperative (one big kernel lock); only
  user programs are preempted. Without a working timer interrupt (F8 "Disable
  interrupts") programs are not preempted, so one that loops forever hangs the
  desktop.
- Programs can't create threads or share memory yet, and the other built-in
  applications (Explorer, Paint...) still run in the kernel.
- No permissions between programs: any program can read or write any file and
  end any other program. Isolation protects against crashes, not against a
  hostile program.
- PCI INTx routes are learned, not read from ACPI (no AML interpreter). A device
  that shares its line with a device that has no driver may stay polled.
- x2APIC mode is written to spec but QEMU 8.2's emulator cannot run it.
- Real graphics cards have no driver yet (NVIDIA, Intel and AMD come in later
  milestones): resolution changes need a restart there, refresh rates are
  whatever the firmware chose, and there is one monitor. OVMF (QEMU's UEFI)
  reports no EDID, so the UEFI loader's choice was only tested with a saved
  setting; real UEFI firmware usually provides it.
- No networking, audio or isochronous USB.
- The keyboard layout is one for the whole system (Windows keeps one per
  program). No input methods (IME), so no Chinese, Japanese or Korean input, and
  no Japanese or Korean layouts.
- No CJK glyphs: Chinese, Japanese and Korean text shows as replacement boxes.
- Some code paths are written to spec but can't be exercised in QEMU:
  - EHCI split transactions
  - USB keyboards beyond QEMU's: n-key rollover bitmaps, wireless receivers,
    Consumer and System Control keys, LEDs on an interrupt OUT endpoint (these
    are covered by host tests only)
  - xHCI SuperSpeed hubs and Transaction Translators
  - LBA28-only disks
