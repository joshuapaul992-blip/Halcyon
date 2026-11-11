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
5. **Input and display:** 20 keyboard layouts (dead keys, AltGr, switching),
   USB keyboards in report protocol (n-key rollover, receivers, media keys),
   the monitor's native resolution from EDID, Display Properties (live switching
   on QEMU/Bochs/VirtualBox, otherwise at the next start), write-combining frame
   buffer, settings kept in `C:\HALCYON\SETTINGS.INI`. Not done: I2C touchpads
   (optional; no test machine needs them yet).
9. **Program platform:** threads (futexes, mutexes, condition variables, TLS),
   named shared memory and the window's pixels mapped into programs, menus,
   scroll bars, toolbars and status bars for programs, BSD sockets (local) and
   `poll`, permission to end other programs only with the user's OK. Notepad,
   Paint and Explorer are user programs. See docs/DESIGN-M9.md.

## Planned (in recommended order)

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
- Halcyon's own programs use it; Linux programs (M14) get their 3D from Mesa
  instead (M12, or Mesa's own software renderer without a GPU driver).
- Frames reach the window through M9's shared memory (no copy per frame).
- Demos: a 3D screensaver (ties in with M8) and a small 3D game or viewer.
- Fully testable without hardware: the renderer also builds on the host and
  the CI compares rendered scenes with reference images.
- Depends on M9 (threads, shared memory).

### M12: Hardware 3D: Mesa on NVIDIA (Pascal first)

Decided: a Mesa port, so programs get modern OpenGL, OpenGL ES 3 and (later)
Vulkan with programmable shaders, as M16 needs.

- Mesa itself runs unmodified as Linux libraries on M14, the distribution's
  build. Halcyon's kernel provides what Mesa's nouveau driver talks to: the
  Linux DRM device files (`/dev/dri/card0`, `renderD128`) with the nouveau
  interface (GPU memory objects, per-process GPU address spaces, command
  submission, fences, mode setting for the compositor).
- Behind that interface, Halcyon's own NVIDIA kernel driver, extending M10 with
  GPU memory management, command channels, the graphics engine and recovery
  from GPU faults (end the program, reset the engine). The Linux nouveau kernel
  driver (MIT-licensed) is the reference and source for porting.
- First on the GTX 1070; second-generation Maxwell (900 series) uses the same
  firmware model and comes almost for free. Other generations later.
- Needs NVIDIA's signed GP104 firmware (from linux-firmware); mandatory for 3D.
- OpenGL / OpenGL ES through Mesa's nouveau driver is the mature path on
  Pascal; Vulkan (NVK) on Pascal is less mature.
- The card stays at its boot clocks (no power-management firmware for Pascal),
  so it is much slower than under NVIDIA's own driver, but far faster than
  software.
- The M14 compositor then takes GPU buffers (dma-buf) directly, and Firefox
  can render on the GPU.
- Tested on the GTX 1070 only (VFIO passthrough or bare metal).
- Depends on M10 (10a, 10b) and M14.

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
- Hardware 3D on Intel or AMD is not part of M13. With M12's design it would
  mean the Linux DRM interface for Intel (i915) or AMD (radeon/amdgpu) behind
  the kernel driver, with Mesa's matching drivers on top: a separate milestone
  after M12 if wanted. Until then Mesa and M11 render in software.
- Testing: QEMU emulates neither. AMD cards can be tested through VFIO
  passthrough; Intel integrated GPUs mostly on bare metal.
- Depends on M5. Independent of M10 to M12, so it can move earlier if running
  well on laptops matters more than the NVIDIA card.

### M14: Linux compatibility layer

Both M15 and M16 need far more than Halcyon's program interface offers: a full
C/C++ runtime, POSIX threads and futexes, `mmap` with protection changes
(JIT compilers), signals (JITs use fault handlers), sockets and `poll`,
shared libraries (`dlopen`), thread-local storage, and several CPU cores to
be usable. Decided: get all of it at once by running Linux programs.

- Run unmodified Linux x86-64 programs: the Linux system call interface on top
  of Halcyon's kernel (the approach of FreeBSD's Linux emulation and WSL 1), a
  Linux user land (libraries from a distribution) under a Halcyon directory,
  and `/proc` and `/sys` as far as programs need them.
- A Wayland compositor that puts Linux programs' windows on the Halcyon
  desktop (shared-memory buffers first, GPU buffers with M12).
- Multi-core support (SMP), with a real kernel locking scheme in place of the
  big lock: browsers and games are unusable on one core.
- Depends on M6 (sockets) and M9. Comparable in size to everything built so
  far.

### M15: A web browser for modern sites

Only three engines render today's web: Gecko (Firefox), Blink (Chromium) and
WebKit. Ladybird, a new independent engine, is promising but not yet complete.

- **Decided: the official Linux Firefox build, unmodified, on M14.** No
  port to maintain, and the security updates a browser needs every few weeks
  come for free. Firefox's own software renderer (SWGL) draws without a GPU;
  with M12 it can use the GPU. Its sandbox needs Linux features (seccomp,
  namespaces) that M14 can add later or switch off at first.
- Halcyon side: the browser as the default for web links, downloads into the
  file system, and a handler for app links (needed by M16).
- Depends on M14 (and M6, M5; M7 for sound).

### M16: A Roblox launcher (Sober-style)

Runs the official Roblox client, unmodified, the way Sober does on Linux: the
Windows client cannot work (its anti-tamper protection rejects compatibility
layers), so the launcher runs the x86-64 build of the Android app.

- An Android runtime for one app: load the app's native libraries, provide the
  Android C library (bionic) interfaces, and a stand-in for the Java side
  (JNI): the native code calls into hundreds of Android and Roblox Java
  classes, which must be reimplemented. This is most of the work, and it is
  reverse engineering.
- Graphics: OpenGL ES 3 or Vulkan through Mesa (M12). Audio through M7,
  input (mouse lock, keyboard, controllers), the window through M14's
  compositor.
- Browser integration: roblox.com's "Play" buttons and app links open the
  launcher through M15's link handler; parts of the app that show web pages
  (Android WebView) can be served by M15's engine.
- Getting the client: the launcher downloads the official app package; how to
  obtain it in a legitimate way is part of the design.
- Risks: Roblox ships a new client every week, so the runtime needs constant
  upkeep (Sober is maintained by a dedicated team). Roblox does not support
  unofficial runtimes and could block them, and running one may conflict with
  its terms of use, with a risk to the account.
- Depends on M14, M15, M7 and M12.

**Recommended order:** 5 → 9 → 6 → 7 → 8 → 10 → 11 → 13 → 14 → 15 → 12 → 16
(M12 moved after M14, which Mesa runs on; Firefox does not need it).
