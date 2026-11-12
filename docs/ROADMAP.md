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

6. **Networking:** network card drivers (virtio-net; Intel e1000, e1000e
   with the I217/I218/I219 of many boards, igb with the I210/I211/I350;
   Realtek RTL8139 and RTL8111/8168), an IPv4 stack behind BSD sockets (ARP,
   IP with fragments, ICMP and ping sockets, UDP, TCP with window scaling,
   DHCP, DNS), PING / IPCONFIG / NSLOOKUP, Network Connections and a tray
   icon, a time zone setting, and on the web side HTTP and HTTPS (BearSSL,
   TLS 1.2) for programs, WGET and a small web browser. See
   docs/DESIGN-M6.md and docs/DESIGN-M6-WEB.md.

7. **Audio:** sound cards (Intel HD Audio with codec path finding and jack
   sensing, AC'97, virtio-sound, ES1370, Sound Blaster 16, PC-speaker
   beeps), a mixer and audio thread, sound streams for programs, Halcyon's own
   synthesized system sounds, the taskbar volume control and volume keys,
   Sounds and Audio Devices, and Media Player for WAV files. The version
   string gained the build number (1.0.<commits>) and pre-release builds a
   desktop watermark. See docs/DESIGN-M7.md.

7.5. **NTFS:** read and write for Windows drives, USB sticks and external
   disks (fragmented, sparse and LZNT1-compressed files, attribute lists,
   hard links, `$UpCase` names, B+ tree directories with splits and merges;
   create, delete, rename, write, extend, truncate), mounted read-write only
   when Windows shut the volume down cleanly (no dirty flag, clean
   `$LogFile`, no Fast Startup hibernation), and internal Windows disks
   read-only until allowed in the drive's new Properties dialog. An NTFS
   formatter in Format and `FORMAT /FS:NTFS`. Checked against the ntfs-3g
   tools, fuzzed; not yet checked by Windows' `chkdsk` (left for real
   hardware). See docs/DESIGN-M7.5.md and docs/DESIGN-M7.5-FORMAT.md.

8. **Polish:** Halcyon Setup installs to an internal SATA or NVMe disk
   (whole disk or free space; GPT with its own EFI system partition and a
   firmware boot entry on UEFI, MBR with an OS menu that can start Windows
   on BIOS; Windows' partitions are never written). Explorer gained multiple
   selection, drag and drop between windows, the desktop and programs, and
   a per-drive Recycle Bin. Notepad has multi-level undo and word wrap.
   Help and Support has 35 pages with contents, index and search, and F1 in
   every program opens the right one. Five screen savers come with a Screen
   Saver tab in Display Properties. A Mouse dialog was added. Window
   places, desktop icons, Paint, Explorer and Notepad choices are kept in a
   power-cut-safe `SETTINGS.INI`. Three sound schemes are available
   (Halcyon, Halcyon Dusk, Halcyon Tide). See docs/DESIGN-M8-*.md.

## Planned (in recommended order)

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

Done first, as the base the NVIDIA driver plugs into: the **display core**
(docs/DESIGN-M10-DISPLAY.md): a driver interface for adapters with several
heads and connectors (modes with full timings from EDID / DMT / CVT, scanout
buffers, hardware cursors, page flips completing at vertical blank,
hotplug), the firmware frame buffer as the fallback whenever a driver fails,
an extended desktop across monitors with Display Properties' arrangement,
and two drivers that exercise all of it in QEMU: Bochs (moved behind the
interface) and virtio GPU (several outputs, cursor, flips, hotplug). Its
"Writing a native display driver" section is the checklist for 10b and 10d.

**Status:** the display core, 10a and 10b are done. 10b ran on the GTX 1070
(DisplayPort, 2560x1440 at 120 Hz); its one bug, a grayscale picture from a
zero saturation setting, is fixed. The NVIDIA side of 10d (hardware cursor,
page flips at the vertical blank, several monitors) is written and
host-tested against the simulated display engine; it awaits the GTX 1070.
10e is written with the copy engine instead of the 2D engine: on Kepler to
Pascal it needs no firmware, and through one PFIFO channel it uploads the
desktop's changes, keeps the page-flip buffers in step and fills new
buffers, with the CPU as the fallback; host-tested against a simulated
PFIFO, MMU and copy engine (and the display core's side in QEMU through a
software test hook), it awaits the GTX 1070. (CPU drawing alone already
reached 205 frames/s at 2560x1440: 10e frees the CPU rather than speeding
the desktop up.) The 2D engine through NVIDIA's signed firmware remains a
later option. Display Properties now offers every refresh rate the
monitor lists, per monitor (60, 120, 144 and 165 Hz on the LG); not yet
tried on the GTX 1070. 10c stays deferred.

Two display engines cover all eight generations, so the work splits by engine:

- **10a, identification:** PCI ID and the chip ID register, the video BIOS
  (DCB: which connectors exist), EDID over the card's I2C buses. Shown in System
  Information. Low risk, useful for every later step. Written (tested on the
  host and with a simulated card in QEMU; NVREPORT collects the data from
  the GTX 1070): see [DESIGN-M10-NVIDIA.md](DESIGN-M10-NVIDIA.md).
- **10b, Tesla to Pascal modesetting:** the NV50 "EVO" display engine (a command
  channel to the display; small changes per generation). Covers five
  generations and most cards still in use. No firmware needed. Written
  (ported from nouveau; host-tested against a simulated display engine,
  awaiting the GTX 1070): see [DESIGN-M10-NVIDIA.md](DESIGN-M10-NVIDIA.md).
- **10c, Kelvin to Curie modesetting: deferred.** The older VGA-style CRTCs and
  RAMDAC, with output setup through the video BIOS's init scripts (needs a
  script interpreter). Until then these cards run on the firmware framebuffer
  (VBE) with the M5 features, and 10a still identifies them.
- **10d:** hardware cursor, page flipping at the vertical blank, several
  monitors. Written (the base and cursor channels per head, flips and the
  vertical blank polled, no interrupt path; host-tested, awaiting the GTX
  1070): see [DESIGN-M10-NVIDIA.md](DESIGN-M10-NVIDIA.md).
- **10e, acceleration** (copy, fill). Written with the **copy engines**,
  which need no firmware from Kepler on (GK104 to GP108): one PFIFO channel,
  a small GPU address space, uploads from the desktop surface, copies
  between buffers, fills, completion polled (host-tested, awaiting the GTX
  1070): see [DESIGN-M10-NVIDIA.md](DESIGN-M10-NVIDIA.md). Fermi's and
  Tesla's copy engines run falcon microcode and are not covered. The 2D
  engine (PGRAPH) stays a later option:
  - Kelvin to Tesla: no firmware.
  - Fermi, Kepler, first-generation Maxwell: context-switch firmware; nouveau's
    open-source version can be used.
  - Second-generation Maxwell and Pascal: NVIDIA-signed firmware from
    linux-firmware (redistributable, but a binary blob on the image).
- **Stretch:** start a card the firmware did not initialise (a second card, or
  a Tesla or Fermi card on a UEFI machine without CSM: they have no GOP driver)
  by running the video BIOS's init tables ("devinit").

Test hardware: a GeForce GTX 1070 (GP104, Pascal). It is PCIe and has a GOP
driver, so it already boots Halcyon through UEFI; 10a, 10b and 10d are
developed against it first. Its copy engines need no firmware (10e); its
2D engine would need the signed GP104 firmware.

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
- Halcyon's own programs use it; Linux programs (M13) get their 3D from Mesa
  instead (M15, or Mesa's own software renderer without a GPU driver).
- Frames reach the window through M9's shared memory (no copy per frame).
- Demos: a 3D screensaver (ties in with M8) and a small 3D game or viewer.
- Fully testable without hardware: the renderer also builds on the host and
  the CI compares rendered scenes with reference images.
- Depends on M9 (threads, shared memory).

### M12: Intel and AMD graphics

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
- Hardware 3D on Intel or AMD is not part of M12. With M15's design it would
  mean the Linux DRM interface for Intel (i915) or AMD (radeon/amdgpu) behind
  the kernel driver, with Mesa's matching drivers on top: a separate milestone
  after M15 if wanted. Until then Mesa and M11 render in software.
- Testing: QEMU emulates neither. AMD cards can be tested through VFIO
  passthrough; Intel integrated GPUs mostly on bare metal.
- Depends on M5. Independent of M10, M11 and M15, so it can move earlier if running
  well on laptops matters more than the NVIDIA card.

### M13: Linux compatibility layer

Both M14 and M16 need far more than Halcyon's program interface offers: a full
C/C++ runtime, POSIX threads and futexes, `mmap` with protection changes
(JIT compilers), signals (JITs use fault handlers), sockets and `poll`,
shared libraries (`dlopen`), thread-local storage, and several CPU cores to
be usable. Decided: get all of it at once by running Linux programs.

- Run unmodified Linux x86-64 programs: the Linux system call interface on top
  of Halcyon's kernel (the approach of FreeBSD's Linux emulation and WSL 1), a
  Linux user land (libraries from a distribution) under a Halcyon directory,
  and `/proc` and `/sys` as far as programs need them.
- A Wayland compositor that puts Linux programs' windows on the Halcyon
  desktop (shared-memory buffers first, GPU buffers with M15).
- Multi-core support (SMP), with a real kernel locking scheme in place of the
  big lock: browsers and games are unusable on one core.
- Depends on M6 (sockets) and M9. Comparable in size to everything built so
  far.

### M14: A web browser for modern sites

Only three engines render today's web: Gecko (Firefox), Blink (Chromium) and
WebKit. Ladybird, a new independent engine, is promising but not yet complete.

- **Decided: the official Linux Firefox build, unmodified, on M13.** No
  port to maintain, and the security updates a browser needs every few weeks
  come for free. Firefox's own software renderer (SWGL) draws without a GPU;
  with M15 it can use the GPU. Its sandbox needs Linux features (seccomp,
  namespaces) that M13 can add later or switch off at first.
- Halcyon side: the browser as the default for web links, downloads into the
  file system, and a handler for app links (needed by M16).
- Depends on M13 (and M6, M5; M7 for sound).

### M15: Hardware 3D: Mesa on NVIDIA (Pascal first)

Decided: a Mesa port, so programs get modern OpenGL, OpenGL ES 3 and (later)
Vulkan with programmable shaders, as M16 needs.

- Mesa itself runs unmodified as Linux libraries on M13, the distribution's
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
- The M13 compositor then takes GPU buffers (dma-buf) directly, and Firefox
  can render on the GPU.
- Tested on the GTX 1070 only (VFIO passthrough or bare metal).
- Depends on M10 (10a, 10b) and M13.

### M16: Xenithium, a Roblox client for Halcyon (Sober-style)

Xenithium runs the official Roblox client, unmodified, the way Sober does on
Linux: the Windows client cannot work (its anti-tamper protection rejects
compatibility layers), so Xenithium runs the x86-64 build of the Android app.

- An Android runtime for one app: load the app's native libraries, provide the
  Android C library (bionic) interfaces, and a stand-in for the Java side
  (JNI): the native code calls into hundreds of Android and Roblox Java
  classes, which must be reimplemented. This is most of the work, and it is
  reverse engineering.
- Graphics: OpenGL ES 3 or Vulkan through Mesa (M15). Audio through M7,
  input (mouse lock, keyboard, controllers), the window through M13's
  compositor.
- Browser integration: roblox.com's "Play" buttons and app links open
  Xenithium through M14's link handler; parts of the app that show web pages
  (Android WebView) can be served by M14's engine.
- Automatic client updates (a setting, "Keep the Roblox client up to date",
  on by default):
  - Xenithium checks an APK download site for a newer Roblox release when it
    starts and every few hours while it runs, and downloads it in the
    background. The site is a setting: a download site that verifies
    developers' signatures is the default, and others (or "choose an APK
    file...") can be used instead. Which site, and that its terms allow
    automated downloads, is decided in the design; scraping a page breaks when
    the site changes, so an API or a stable feed is preferred.
  - The site is not what makes an update trusted. Before anything is
    installed, Xenithium checks that the package is `com.roblox.client`, that
    its APK signature (scheme v2/v3) is valid and signed with Roblox's
    certificate, whose fingerprint Xenithium ships with, that its version is
    newer than the installed one (no downgrades from the site), and that it
    has x86-64 native libraries. Split packages (a base APK plus
    configuration APKs, as many sites serve them) are checked file by file.
    Anything that fails is deleted and reported, never run.
  - The new version is installed next to the old one and used from the next
    start. If it fails to start or crashes at once (often the case when Roblox
    calls Java classes the runtime does not have yet), Xenithium goes back to
    the previous version, marks the new one as not working, and says so;
    the last two working versions are kept.
  - Xenithium's own updates can add the classes a new client needs. A list
    of client versions known to work, published with Xenithium, lets the
    updater wait for a version that runs rather than install one that will
    not.
  - With the setting off, Xenithium only reports that an update is available.
- Risks: Roblox ships a new client every week, so the runtime needs constant
  upkeep (Sober is maintained by a dedicated team). Roblox does not support
  unofficial runtimes and could block them, and running one may conflict with
  its terms of use, with a risk to the account. APK sites redistribute the
  app without Roblox's involvement; the signature check guards against a
  tampered package, but not against the site disappearing or being blocked.
- Depends on M13, M14, M7 and M15.

### M17: Release: Halcyon 1.0 RTM

Everything after M16 that turns the milestones into a release people can
install and keep using. No new big features: finishing, fixing and testing.

- Stabilization: a bug bash over every app and driver; no known crash, hang,
  data loss or file system corruption left open; every "written to spec,
  untested" driver either tested on real hardware or listed in the release
  notes as untested. Long-running tests (days of uptime, copying, playing
  sound, browsing) under QEMU and on real PCs.
- Hardware compatibility: a matrix of real machines (desktops and laptops,
  Intel and AMD, BIOS and UEFI, NVIDIA/Intel/AMD graphics, common network
  and sound chips) with results published in docs/HARDWARE.md; boot-log
  reports from testers turned into fixes.
- Performance: boot time, desktop responsiveness under load (audio must
  not stutter while the GUI works), memory use, file copy and network
  throughput measured and tuned; no regressions between release candidates.
- Security: permissions between programs and for files (today any program
  may do anything), user accounts or at least a protected system folder,
  the network configuration and system settings behind the user's OK,
  hardening of system calls (fuzzing every one), and a review of the
  vendored libraries' versions (BearSSL, CA list, Mesa, Firefox) for known
  vulnerabilities.
- Install and update: the installer to an internal disk (from M8) finished
  and tested next to Windows and Linux (dual boot, without touching their
  partitions), an update mechanism for Halcyon itself (signed update
  packages, rollback), and a recovery option in the F8 menu.
- UEFI Secure Boot: Halcyon is not signed by Microsoft, so the release
  documents turning Secure Boot off or enrolling Halcyon's own key, and the
  loader says so clearly when Secure Boot blocks it.
- Finish and consistency: every app's menus, shortcuts, dialogs, icons and
  wording checked against one style guide; keyboard access everywhere;
  high-contrast theme and larger text for accessibility; the help viewer
  (M8) with a page for every app; error messages that say what to do.
- Release engineering:
  - Feature freeze, then release candidates (RC1, RC2, ...) built from a
    release branch, each one through the full CI and the hardware matrix.
  - The version stays 1.0.<build>: RTM is the build the last release
    candidate is approved as, tagged `v1.0` in git, and later fixes are
    1.0.<higher build> updates. Pre-release builds say "Pre-release" in
    About and on the boot screen; the RTM build does not.
  - Release images (USB stick image, ISO), checksums, release notes, known
    issues, a changelog generated from the milestones, and a final license
    audit (THIRD_PARTY_NOTICES.md for everything shipped, including
    Firefox, Mesa and the Android runtime pieces).
- Depends on M16 and every milestone before it.

**Recommended order:** 5 → 9 → 6 → 7 → 7.5 → 8 → 10 → 11 → 12 → 13 → 14 → 15 → 16 → 17
(Mesa, M15, comes after the Linux layer, M13, which it runs on; Firefox, M14,
does not need it.)

## Halcyon 1.1

After 1.0 RTM the version becomes 1.1.<build> (`major_minor` in
tools/mkversion.sh), and pre-release builds show the watermark again. 1.1
starts with newer graphics: the generations that M10 and M12 stop short of, up
to 2020, the year NVIDIA's Ampere came out.

### M18: NVIDIA Turing and Ampere

Generations: Turing (TU102/104/106: RTX 20 series; TU116/117: GTX 16 series;
2018-2019) and Ampere (GA102/103/104/106/107: RTX 30 series, 2020-2021).
GA100 is a compute-only chip with no display and is not covered.

- **Display:** Turing and Ampere use the newer display engine (NVDisplay, from
  Volta on) instead of M10's EVO: the same idea of a command channel to the
  display, with different channel classes, memory-based notifiers and a new
  output and clock setup per generation. The M10 features follow: native
  modes, resolution switching without a reboot, several monitors, hardware
  cursor, page flips; DisplayPort 1.4 and HDMI 2.0/2.1 links (link training,
  DSC on high-resolution monitors where needed).
- **Firmware:** both generations have a GPU System Processor (GSP, a RISC-V
  core). NVIDIA publishes its firmware (GSP-RM) in linux-firmware under a
  license that allows redistribution; with it, the GPU's initialisation,
  power management and engine setup are done by the GSP, and the driver
  talks to it through a message queue. This is how Turing and Ampere get full
  clock speeds, unlike Pascal in M10. Modesetting is attempted without the
  firmware first (as nouveau did before GSP); acceleration and clocks need
  it. The firmware is large (tens of MB per generation), so it lives in a
  separate firmware pack on the image or the disk, not in the kernel.
- **Sources:** NVIDIA's open GPU kernel modules (released under MIT and GPLv2:
  the MIT terms allow porting, with copyright notices kept), open-gpu-doc,
  envytools and nouveau (MIT).
- **3D:** M15's approach extended: the nouveau DRM interface on top of the
  GSP-based driver, and Mesa on top. Vulkan through NVK is at its best on
  Turing and later, so these cards become the preferred ones for Xenithium
  (M16) and Firefox's GPU rendering.
- **Testing:** no test card at hand yet (the GTX 1070 is Pascal); VFIO
  passthrough or bare metal, with `BOOT.LOG` reports from testers.
- Depends on M10 (identification, the display framework) and, for 3D, M15.

### M19: Intel and AMD graphics up to 2020

M12's range carried forward to the same point in time.

- **Intel:**
  - Gen11 (Ice Lake, 2019; Jasper Lake and Elkhart Lake) and Gen12 / Xe-LP
    (Tiger Lake 2020, Rocket Lake 2021, Iris Xe MAX "DG1" as the first Intel
    discrete GPU).
  - The display engine changed a lot: Type-C ports with DisplayPort Alt Mode
    and Thunderbolt (a handshake with the Type-C subsystem before a port can
    be used), new combo PHYs, the display state buffer, and power wells that
    the display microcontroller (DMC) firmware manages in low-power states
    (redistributable, in linux-firmware).
  - Intel still publishes programmer's reference manuals for these
    generations.
- **AMD:**
  - Vega (Vega 56/64 and Radeon VII, and the Raven Ridge, Picasso and Renoir
    APUs, 2017-2020), RDNA1 (Navi 10/14: RX 5000, 2019) and RDNA2 (Navi
    21-23: RX 6000, November 2020: two months after Ampere, the same
    generation).
  - These are the cards M12 deferred: their platform security processor
    (PSP) must first load AMD's signed firmware (system management unit,
    display microcontroller DMCUB, and others; redistributable, in
    linux-firmware), and their display code (DCN 1.0 to 3.0) is very large.
    The amdgpu driver's display core is MIT-licensed; porting the needed
    parts, rather than writing DCN from scratch, is the realistic path.
- **3D on both:** as for NVIDIA: the Linux DRM interfaces (i915 or amdgpu)
  behind Halcyon's drivers and Mesa's drivers on top (iris and ANV for
  Intel, radeonsi and RADV for AMD), on M13.
- **Testing:** AMD through VFIO passthrough; Intel on laptops and NUCs (bare
  metal).
- Depends on M12 and, for 3D, M13 and M15.

### M20: Several processor cores (SMP)

Halcyon runs on one core today, under one big kernel lock. Firefox (M14) and
Roblox (M16) are heavily multi-threaded, so this is the biggest performance
step of 1.1.

- Start the other cores (ACPI MADT, INIT/SIPI), per-core GDT/TSS, local APIC
  timers and idle; a scheduler with per-core run queues, load balancing and
  core affinity for user threads.
- Replace the big kernel lock step by step with finer locks (memory, VFS and
  block layer, network stack, GUI, sound), so that system calls on different
  cores run in parallel; TLB shootdowns for shared address spaces.
- Interrupts spread over cores (MSI-X vectors per core for NICs and NVMe).
- Tests: the whole CI matrix with 2, 4 and 8 virtual CPUs, stress tests for
  races (many threads in many programs), and lock-order checking in debug
  builds.
- Could move before 1.0 if M14 or M16 turn out too slow on one core.

### M21: ACPI and power management

- An ACPI AML interpreter: the namespace and methods of the firmware's DSDT and
  SSDTs. With it: PCI interrupt routing from `_PRT` (replacing M2's learned
  routes), proper power-off and restart on every machine, and the devices
  ACPI describes.
- Laptops: battery level and charging (a battery icon in the tray, warnings,
  the "critical battery" sound M7 left out), the lid switch, the power and
  sleep buttons, screen brightness keys.
- Sleep (S3, suspend to RAM) and resume: every driver saves and restores its
  device; a "Stand By" choice next to Turn Off. Hibernation (S4) later.
- CPU frequency and power states (P-states, C-states; Intel HWP and AMD CPPC),
  thermal zones and fan control through ACPI.
- Depends on M20 for per-core power states.

### M22: Wi-Fi and Bluetooth

- Wi-Fi: Intel Wi-Fi 5/6 adapters (the most common in laptops and desktops
  with Wi-Fi; they need Intel's redistributable firmware) and a common USB
  Wi-Fi stick family (Realtek or MediaTek); scanning, WPA2-Personal and
  WPA3-Personal (SAE), a "Wireless Network Connection" list in the tray and in
  Network Connections, saved networks.
- Bluetooth: USB and on-board controllers (HCI), pairing (Secure Simple
  Pairing), keyboards and mice (HID), game controllers, and headphones (A2DP
  audio through M7's mixer).
- Tests: QEMU has no Wi-Fi adapter; the WPA handshake and the 802.11 frame
  handling get host tests against recorded and generated traffic, the drivers
  real hardware.

### M23: Networking 2: IPv6 and Windows file sharing

- IPv6 (M6 left it out; DESIGN-M6 lists what it needs): addresses through
  SLAAC and DHCPv6, neighbour discovery, dual-stack sockets and name lookup
  (AAAA), privacy addresses, IPv6 in Firefox and the browser.
- Windows file sharing: an SMB 2/3 client, so that "My Network Places" in
  Explorer shows shared folders on Windows PCs and NAS boxes, with copying
  and opening files in place; NTLMv2 sign-in, signing and encryption.
- Local name discovery (mDNS / LLMNR), so other computers show up by name.
- A firewall for incoming connections (on by default), now that Halcyon has
  servers and shares.

### M24: USB and devices 2

- Isochronous USB transfers (left out since M1): USB audio devices (headsets,
  DACs, microphones) and webcams (USB Video Class).
- Game controllers: Xbox (XInput, wired and the wireless adapter),
  PlayStation (DualShock 4, DualSense) and generic HID gamepads, with a
  controller API for programs and Xenithium (M16).
- USB Attached SCSI (UASP) for fast external disks, USB 3.2 and USB-C details
  (the xHCI driver's newer features), Thunderbolt 3/4 and USB4 docks at least
  as far as their USB and display parts go.
- Printers: IPP Everywhere over the network and over USB, with a print dialog
  for Notepad, Paint and the browser.

### M25: Media 2: recording, codecs and video

- Recording: microphone input on HD Audio, AC'97 and USB audio, a Sound
  Recorder, and capture streams in the sound system calls.
- Codecs written for Halcyon or vendored under permissive licences: MP3, AAC,
  FLAC, Ogg Vorbis and Opus in Media Player and for programs.
- Video playback: H.264 and VP9 (and AV1 where fast enough) decoded in
  software, using the cores from M20; hardware decoding on the M18/M19 GPUs
  later. Media Player plays video files; the browser's own decoder stays for
  Firefox to provide.
- A webcam viewer for M24's cameras.

### M26: Disks 2: Disk Management and Linux disks

- Disk Management: list disks and partitions (MBR and GPT), create, delete,
  format, resize (FAT, exFAT, NTFS) and label partitions, with clear warnings.
- Check Disk for FAT, exFAT and NTFS: find and repair the inconsistencies a
  crash or a pulled USB stick leaves.
- Linux disks: ext4 read and write (journal replayed or refused like NTFS's),
  so dual-boot machines can share files both ways.
- The installer (M8, M17) uses Disk Management to make room next to Windows
  or Linux.

### M27: Release: Halcyon 1.1 RTM

As M17 for 1.0: no new big features, finishing, fixing and testing.

- Feature freeze, release candidates from a release branch, the full CI and
  a hardware matrix that now includes Turing/Ampere, Intel Gen11/12 and AMD
  Vega/RDNA graphics, Wi-Fi and Bluetooth adapters, laptops (sleep, battery),
  and multi-core machines.
- Upgrade from 1.0 in place with the M17 update mechanism, settings and files
  kept; a rollback path.
- Performance pass on SMP (no regressions against 1.0 on one core), power
  use on laptops, and the firmware pack (M18, M19, M22) checked for licences
  and versions.
- Release notes, known issues, the license audit and THIRD_PARTY_NOTICES
  for everything new; the RTM build is tagged `v1.1` and drops the
  pre-release watermark.
- Depends on M18 to M26.

**1.1 order:** 18 → 19 → 20 → 21 → 22 → 23 → 24 → 25 → 26 → 27 (M20, SMP,
may move to the front if Firefox and Roblox need it sooner).

## Release schedule

Releases are gated by milestones and quality, not by dates. Target windows
are named so there is something to aim at; when a gate is not met, the window
slips and the bar stays where it is.

### Versions and build numbers

- Every build is `major.minor.build`: 1.0.<build>, 1.1.<build>, and so on, one
  build number per release line, counted from commits (tools/mkversion.sh).
- Every build of a later line is numbered above every build of an earlier
  line, service packs included: 1.0.1415 is older than 1.1.2362, always.
  1.0 counts commits from the start of the history. When 1.1 begins (at 1.0
  RTM), its builds start at a fixed number well above the RTM build (the next
  thousand above RTM + 1000: RTM 1.0.1415 means 1.1 starts at 1.1.3000) and
  count the commits since the branch point; the same again for 1.2 and later.
- Service packs are built on the release's maintenance branch (`release/1.0`
  from the RTM commit), so their build numbers keep rising within the line
  (1.0 RTM 1.0.1415, SP1 1.0.1472, ...) but stay below the next line's start:
  the version generator refuses to build a number that reaches it. About and
  the boot screen name the service pack ("Halcyon 1.0, Service Pack 1, build
  1472").
- Pre-release builds (everything except an RTM or a service pack's final
  build) show the desktop watermark; finals do not.

### Channels

| Channel | When | What |
|---|---|---|
| Dev builds | every commit | what CI builds; watermark |
| Milestone previews | each finished milestone | a GitHub pre-release: USB image, ISO, checksums, short notes; tag `m7.5`, `m8`, ... |
| Beta | all features of the release are in, polish not done | Beta 1, Beta 2, ... for testers on real hardware |
| Release candidates | during the release milestone (M17, M27) | RC1, RC2, ...: fixes only; any of them may become the final |
| RTM | the approved RC | tag `v1.0`, `v1.1`; no watermark; release notes, known issues |
| Service packs | every few months after an RTM | fixes plus selected features backported from the next release; their own RCs |

### 1.0

- **Beta 1** once M7.5 to M15 are done (Halcyon runs Firefox, with hardware 3D
  on the GTX 1070); M17's work runs alongside the betas.
- **M16 (Xenithium) is not on 1.0's critical path:** it depends on Roblox's
  weekly clients and its willingness to tolerate unofficial runtimes. It
  ships in 1.0 if it is ready by the release candidates, otherwise in a 1.0
  service pack or 1.1.
- **RTM** when the release candidates show no known crash, hang or data-loss
  bugs and the hardware matrix is published (M17).

Target windows (estimates; the graphics drivers move at the pace of
real-hardware testing, and M13-M15 are each bigger than everything before
them):

| Step | Target |
|---|---|
| M7.5 and M8 previews | November-December 2026 |
| Graphics, M10-M12 | early 2027 |
| Linux layer, Firefox, Mesa, M13-M15 | through 2027 |
| 1.0 Beta 1 | mid-2027 |
| 1.0 RTM | late 2027 |

### 1.0 service packs and backports from 1.1

A 1.1 feature may come to 1.0 in a service pack when it has shipped in a 1.1
preview or beta and survived testing there, keeps 1.0's programs, settings
and files working, and is self-contained (drivers, apps, protocols; nothing
that changes the core of the kernel). Every service pack gets its own release
candidates, the full CI and the hardware matrix.

| Backported in service packs | 1.1 only |
|---|---|
| codecs and Sound Recorder (M25); the firewall, IPv6, SMB file sharing (M23); game controllers, UASP, printers (M24); Wi-Fi and Bluetooth (M22); display on Turing/Ampere and the 2020 Intel/AMD GPUs (M18/M19 modesetting); Check Disk, ext4 (M26) | several cores (M20); the ACPI interpreter, sleep and resume (M21); 3D on the new GPUs (M18/M19 acceleration) |

- **SP1**, about 3-4 months after 1.0 RTM: fixes and the low-risk backports
  (codecs, firewall, controllers).
- **SP2**, once 1.1's betas have proven them: Wi-Fi, Bluetooth, IPv6, SMB,
  display on the newer GPUs.
- **SP3**, after 1.1 RTM: the last bundle of fixes. 1.0 then gets security
  fixes only, for 6 months.

### 1.1

Milestone previews for M18-M26, betas once they are in, then M27's release
candidates and RTM; target about 12 months after 1.0 RTM.

### Beyond 1.1

- A feature release about once a year (1.2, 1.3, ...), each with a theme;
  1.2 could bring newer GPUs (NVIDIA Ada, AMD RDNA3, Intel Arc), Wi-Fi 6E/7
  and the security work M17 begins.
- 2.0 is kept for one change in how Halcyon works, such as a GPU-composited
  desktop or an ARM64 port, not for a longer feature list.
- Support: the latest release gets fixes; the one before gets security
  fixes for 6 months after the next one ships.

