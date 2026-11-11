# Halcyon roadmap

## Done

1. **Real-hardware bring-up and test automation:** boot log on the stick,
   text-mode log, safe-mode switches, QEMU boot matrix, host file system tests.
2. **Interrupts and idle:** local APIC / I/O APIC / PIC, MSI(-X), learned INTx
   routes, one-shot timer, the CPU halts when idle.
3. **Storage:** exFAT, NVMe, Unicode long names, CD/DVD (ISO 9660 + Joliet),
   format tool.
4. **Threads and user mode:** kernel threads (background copies), programs in
   their own address space loaded from `C:\PROGRAMS`, crash isolation, Task
   Manager, Calculator and Sonar as programs.

## Planned (in recommended order)

### M5: Input and display
- Non-US keyboard layouts; HID keyboards that need the report protocol.
- The monitor's native resolution (EDID) and a resolution picker, through the
  firmware (VBE / GOP).
- Write-combining for the framebuffer (PAT), so drawing is faster everywhere.
- Optional: I2C touchpads (hard; only if test machines need them).

### M9: Program platform
- Threads, menus and a sockets API for programs.
- Notepad, Paint and Explorer move from the kernel to user mode.
- Permissions, at least: a program may end only programs it started.

### M6: Networking
- Network card drivers: Intel e1000, Realtek, virtio-net.
- TCP/IP with DHCP and DNS; `PING`; a minimal web browser as a user program.

### M7: Audio
- Intel HD Audio, AC'97 (QEMU), a startup sound, a system call API for sound.

### M8: Polish
- Settings that survive a reboot; installer to the internal disk; Explorer
  drag-and-drop, multiple selection, Recycle Bin; Notepad undo and word wrap;
  screensaver; help viewer.

### M10: NVIDIA graphics (Kelvin up to Pascal)

Generations covered: Kelvin (NV2x: GeForce3, GeForce4 Ti), Rankine (NV3x:
GeForce FX), Curie (NV4x/G7x: GeForce 6 and 7), Tesla (G80-GT21x: GeForce
8/9/200/300), Fermi (GF1xx: 400/500), Kepler (GK1xx: 600/700), Maxwell
(GM1xx/GM2xx: 750, 900) and Pascal (GP10x: 10 series). GeForce4 MX (NV17/18)
is Celsius, not Kelvin, but shares Kelvin's display hardware, so modesetting
covers it at no extra cost.

What the driver adds over the firmware framebuffer: any mode the monitor
supports (EDID), switching resolution without a reboot (GOP cannot after boot),
several monitors, a hardware cursor, tear-free page flips, and later 2D
acceleration. 3D acceleration is out of scope.

Two display engines cover all eight generations, so the work splits by engine:

- **10a, identification:** PCI ID and the chip ID register, the video BIOS
  (DCB: which connectors exist), EDID over the card's I2C buses. Shown in System
  Information. Low risk, useful for every later step.
- **10b, Tesla to Pascal modesetting:** the NV50 "EVO" display engine (a command
  channel to the display; small changes per generation). Covers five
  generations and most cards still in use. No firmware needed.
- **10c, Kelvin to Curie modesetting: deferred.** The older VGA-style CRTCs and
  RAMDAC, with output setup through the video BIOS's init scripts (needs a
  script interpreter). Until then these cards run on the firmware framebuffer
  (VBE) with the M5 features, and 10a still identifies them.
- **10d:** hardware cursor, page flipping with vblank interrupts, several monitors.
- **10e, optional 2D acceleration** (fill, copy) through the 2D engine:
  - Kelvin to Tesla: no firmware.
  - Fermi, Kepler, first-generation Maxwell: context-switch firmware; nouveau's
    open-source version can be used.
  - Second-generation Maxwell and Pascal: NVIDIA-signed firmware from
    linux-firmware (redistributable, but a binary blob on the image).
  - Measure first: with write-combining from M5, CPU drawing may be fast enough.
- **Stretch:** start a card the firmware did not initialise (a second card, or
  a Tesla or Fermi card on a UEFI machine without CSM: they have no GOP driver)
  by running the video BIOS's init tables ("devinit").

Test hardware: a GeForce GTX 1070 (GP104, Pascal). It is PCIe and has a GOP
driver, so it already boots Halcyon through UEFI; 10a, 10b and 10d are
developed against it first. Its 2D acceleration (10e) would need the signed
GP104 firmware.

Constraints:

- **No emulator:** QEMU has no NVIDIA GPU, so none of this runs in the CI
  matrix. PCIe cards can be tested in QEMU through VFIO passthrough on a Linux
  host with an IOMMU (the GTX 1070 qualifies), or on the real machine with
  `BOOT.LOG` reports.
- **Clock speeds:** on second-generation Maxwell and Pascal, NVIDIA has not
  released the power-management firmware, so the GPU stays at its boot clocks
  (fine for a 2D desktop).
- **Laptops:** many laptop NVIDIA GPUs (Optimus) have no display outputs of
  their own; the screen is driven by the Intel GPU, so this driver cannot help
  there.
- **Sources:** register documentation comes from envytools (rnndb), NVIDIA's
  open-gpu-doc, and the nouveau driver (MIT-licensed; ported code keeps its
  copyright notices).

Order: after M5, which does the generic display work (EDID, mode list,
resolution picker, write-combining) that M10 plugs into. Otherwise independent
of M6-M9; it can move earlier if test cards are at hand, since real-hardware
testing sets its pace.

**Recommended order:** 5 → 9 → 6 → 7 → 8 → 10.
