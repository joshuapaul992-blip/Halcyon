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

### M11: Software 3D

- A 3D library for programs: a subset of OpenGL 1.3, the fixed-function
  pipeline of early-2000s games (transforms and lighting, textures with
  mipmaps and bilinear filtering, depth buffer, blending, fog).
- A software renderer behind it, written for speed: perspective-correct
  triangle setup, SSE2 inner loops, tiles. Target: textured scenes at 640x480
  at interactive frame rates on one core.
- The API is the contract for M12: the same programs later run on the GPU, and
  the software renderer stays as the fallback for any GPU without a 3D driver.
- Frames reach the window through M9's shared memory (no copy per frame).
- Demos: a 3D screensaver (ties in with M8) and a small 3D game or viewer.
- Fully testable without hardware: the renderer also builds on the host and
  the CI compares rendered scenes with reference images.
- Depends on M9 (threads, shared memory).

### M12: Hardware 3D (NVIDIA, Pascal first)

- The M11 API on the GPU, first on the GTX 1070. Second-generation Maxwell
  (900 series) shares Pascal's instruction set and firmware model, so it
  comes almost for free; Tesla to Kepler would each need their own shader
  back end and come later, if at all.
- Kernel side: GPU memory management (VRAM and per-process GPU page tables, so
  programs stay isolated on the GPU too), command channels, fences, and
  recovery from GPU faults (end the program, reset the engine).
- The graphics engine needs NVIDIA's signed GP104 firmware (from
  linux-firmware); for 3D it is mandatory, not optional.
- Shaders: the fixed-function pipeline is turned into GPU machine code by a
  small generator for the Maxwell/Pascal instruction set, not a full shader
  compiler. Programmable shaders (and with them a compiler) are a later step.
  The alternative, porting Mesa's nouveau driver, needs a far larger C/C++
  runtime and kernel interface than Halcyon has; not recommended at first.
- The card stays at its boot clocks (no power-management firmware for Pascal),
  so it is much slower than under NVIDIA's own driver, but still far faster than
  software.
- Tested on the GTX 1070 only (VFIO passthrough or bare metal); none of it can
  run in the CI.
- Depends on M10 (10a, 10b) and M11.

### M13: Intel and AMD graphics

Display (modesetting) and the M10 features (native modes, resolution switching
without reboot, several monitors, hardware cursor, page flips), like M10 for
NVIDIA. Proposed range, matching M10's Pascal (2016):

- **Intel:** Gen4 (GMA X3100/X4500, 2006) up to Gen9.5 (Skylake to Comet Lake).
  Intel publishes full programmer's reference manuals for all of these, so it
  is the best-documented GPU family. This is the GPU in most laptops, including
  those whose NVIDIA GPU has no outputs (see M10), so it reaches the most
  machines. Laptop panels (eDP) need DisplayPort link training and panel power
  sequencing. Gen11/Gen12 (Ice Lake and later) come later.
- **AMD:** Radeon HD 2000 (R600) up to Polaris (RX 400/500), the DCE display
  engines, programmed largely through AtomBIOS command tables in the video BIOS:
  this needs an AtomBIOS interpreter (the Linux radeon/amdgpu version is
  MIT-licensed). Vega and RDNA are deferred: their security processor must load
  signed firmware first, and their display code (DCN) is very large.
- Hardware 3D on Intel or AMD is not part of M13: each needs its own shader back
  end and command submission, a separate milestone after M12 if wanted. Until
  then they use M11's software renderer.
- Testing: QEMU emulates neither. AMD cards can be tested through VFIO
  passthrough; Intel integrated GPUs mostly on bare metal.
- Depends on M5. Independent of M10 to M12, so it can move earlier if running
  well on laptops matters more than the NVIDIA card.

**Recommended order:** 5 → 9 → 6 → 7 → 8 → 10 → 11 → 12 → 13.
