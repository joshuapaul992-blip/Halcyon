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

## Planned (in recommended order)

### M7.5: NTFS (read and write)

Windows disks become usable from Halcyon: an internal Windows drive, or a USB
stick or external disk formatted NTFS.

- Reading: the boot sector and the MFT (update sequence fixups, attribute
  lists for files with many attributes), resident and non-resident
  attributes and their run lists, directories as `$INDEX_ROOT` /
  `$INDEX_ALLOCATION` B+ trees, file names in the Win32, DOS and POSIX
  namespaces compared through the volume's `$UpCase` table, hard links,
  sparse files, LZNT1-compressed files, symbolic links and junctions shown as
  links. Encrypted (EFS) files are listed but cannot be opened; alternate
  data streams are kept but not shown.
- Writing: create, delete, rename and move files and folders, write, extend
  and truncate files (resident data moved out of the MFT record when it
  grows), allocate MFT records and clusters through `$MFT:$Bitmap` and
  `$Bitmap`, keep the index B+ trees balanced (node splits and merges), and
  update the duplicated name information, timestamps and sizes the way
  Windows expects so that its `chkdsk` finds nothing to fix. Writes to
  compressed or encrypted files are refused.
- The journal: Halcyon does not replay or write `$LogFile`. A volume is
  mounted read-write only if Windows shut it down cleanly; the journal is
  then reset and the volume marked dirty while mounted, as other non-Windows
  NTFS drivers do. A volume that is dirty, or hibernated (Windows Fast
  Startup or hibernation leaves `hiberfil.sys` active), is mounted read-only,
  and Halcyon says why and what to do in Windows.
- Safety: an internal disk with Windows on it is read-only until the user
  allows writing in the drive's properties; removable disks are writable by
  default.
- Explorer and the Format tool: NTFS volumes show with their label and free
  space, and Format offers NTFS (cluster size, quick format).
- Tests: host tests against images made by `mkntfs` and checked by
  `ntfsfix`/`ntfsinfo` (ntfs-3g tools), with random operation sequences
  compared to a model and every image checked afterwards; images with
  compression, sparse files, many hard links and huge directories; a fuzzer
  for the on-disk structures; and `chkdsk` on real Windows disks written by
  Halcyon.
- Depends on M3 (the file system layer) and M4.

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
