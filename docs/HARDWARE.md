# Testing Halcyon on real hardware

Halcyon has only been tested in QEMU so far. Reports from real PCs are the most
useful contribution right now, and both a success and a failure are worth reporting.

## How to test

1. Write `halcyon.img` to a USB stick (see the README). Use a stick you don't need,
   because writing the image erases it.
2. Boot the PC from the stick. Secure Boot must be off on UEFI machines. If your
   firmware offers both "UEFI: <stick>" and a legacy entry for the stick, please try
   both.
3. Work through the checklist:
   - Does it reach the desktop?
   - Do the keyboard and mouse work? Try the stick in different ports, including
     front ports and any USB 3 (blue) ports.
   - Does Caps Lock switch the keyboard's Caps Lock light? Do the volume, media and
     Calculator keys of a multimedia keyboard show up in `BOOT.LOG` ("usb-hid: debug:
     ... key pressed"), and does the Calculator key open the Calculator?
   - Open My Computer and the stick (`C:`). Open `README.TXT`.
   - Write a file: Notepad → type something → File → Save As → `C:\test.txt`.
   - Plug in a second USB stick. Does a "Found New Hardware" balloon appear?
     Can you open it in My Computer?
   - Are your internal SATA / NVMe disks listed? FAT, exFAT and NTFS volumes get
     a drive letter. A Windows drive shows read-only (its Properties say why);
     open a few folders and files on it. If you allow writing on a drive whose
     data you have backed up, copy a file to it, then start Windows, run
     `chkdsk X:` (without /f) and report what it says.
   - Open the Command Prompt and type `IRQ`. Every USB and SATA controller should
     be listed with a count above zero; one marked "polled" means its interrupt was
     not found. `IRQ > C:\IRQ.TXT` saves the table to the stick.
   - Leave the desktop idle for a minute. The fan should stay quiet and the CPU
     cool: Halcyon halts the CPU when there is nothing to do.
   - If the PC has wired Ethernet, plug a cable in and look at "Network adapters"
     in System Information: the adapter should be listed with its MAC address,
     and "connected" with the right speed once the cable is in (unplugging it
     should show "cable unplugged" within a second). BOOT.LOG has a `net: eth0:`
     line per adapter and the driver's own lines (`e1000e:`, `r8169:`, ...).
   - If the PC has speakers or headphones, does Halcyon's sound play, and from the
     jack you expect (rear line out, front headphones, laptop speakers)? On a
     laptop, plug headphones in and out while sound plays: the speakers should
     go quiet and come back. `BOOT.LOG` has the sound card's `hda` lines (the
     codec, every widget, the paths chosen) and `sound: snd0:` (see "Sound"
     below); please include them, whatever the result.
   - Turn Off and Restart from the Start menu.
4. Use Turn Off or Restart, so the log is written, then collect
   **`\HALCYON\BOOT.LOG`** and **`\HALCYON\BOOTPREV.LOG`** from the stick on another
   computer. These logs contain the hardware summary (PCI devices, USB tree,
   disks, memory map, video mode) and every driver message.

## If something goes wrong

Press **F8** while "Press F8 for Advanced Startup Options" is shown, right after
the BIOS/UEFI hands over. Repeatedly tapping F8 during boot works too. The menu
offers:

| Option | Use it when |
|--------|-------------|
| Verbose boot | Boot hangs: the kernel log scrolls on screen, so the last line shows where it stopped. Take a photo of the screen. |
| Safe Mode | Verbose boot with no USB, SATA or ACPI drivers and no interrupts. If this reaches the desktop, re-enable drivers one at a time. |
| Disable USB 3.x / 2.0 / 1.1 | Find which USB controller driver misbehaves. With the stick's controller disabled, `C:` won't appear. That is expected. |
| Disable SATA | Hangs or errors while detecting disks. |
| Disable NVMe | Hangs or errors while detecting NVMe SSDs. |
| Disable ACPI | Hangs right at the start. Also switches to the legacy PIC (the I/O APIC is described by ACPI). |
| Disable interrupts | Hangs or freezes after the "irq:" lines, or input stops working. Everything is polled, as in the first Halcyon releases, and the CPU never idles. |
| Disable MSI | A USB 3 or SATA controller misbehaves: it then uses its INTx line instead. |
| Use the legacy PIC | Interrupts misbehave with the I/O APIC. Uses the firmware's PCI interrupt lines. |
| Safe video mode | Black or garbled screen: forces 800×600 and the firmware frame buffer, ignoring the saved resolution. |

If a resolution chosen in Display Properties leaves the screen black after a
restart, use F8 "Safe video mode", then pick another one; or delete the
`display.width` / `display.height` lines from `\HALCYON\SETTINGS.INI` on the stick.

Programs (the `.EXE` files in `C:\PROGRAMS`) run in user mode: if one crashes, only
that program is ended. Press Ctrl+Alt+Del for Task Manager to end one that hangs.
`CRASHME` in the Command Prompt opens a Crash Test window to try it.

If Halcyon crashes, the crash screen tries to save `BOOT.LOG` to the stick before it
stops. In the Command Prompt, `BOOTLOG` saves the log at any time, and
`CRASHTEST` deliberately crashes the system so you can check that crash logging
works.

## Known risks on real hardware

- **Intel 5/6-series chipsets (about 2010–2012) without xHCI.** Every USB port sits
  behind a built-in USB 2.0 hub, so USB 1.1 keyboards and mice depend on the EHCI
  driver's split transactions. QEMU cannot test those. A PS/2 keyboard works
  regardless.
- **Keyboards behind a USB 2.0 hub** (monitor or dock hubs) on xHCI depend on
  Transaction Translator support. That is also untested.
- **USB keyboards are read in HID report protocol**, like Windows does: the driver
  parses each interface's report descriptor, so gaming keyboards with n-key
  rollover (a key bitmap on a second interface), wireless receivers (keyboard,
  mouse and media keys on one interface, told apart by report ID) and the extra
  Consumer Control / System Control keys work. QEMU only emulates a plain 6-key
  keyboard, so these are tested on the host with typical report descriptors, not
  on real devices yet. A keyboard whose descriptor cannot be read or parsed falls
  back to the boot protocol (6 keys, no media keys). `BOOT.LOG` has one line per
  interface, e.g. `usb-hid: <name> (interface 1): keyboard (report protocol, NKRO
  bitmap, report ID 1) + consumer control (report ID 2)`: please include it when
  a keyboard misbehaves. The keyboard LEDs are set with SET_REPORT; if a keyboard
  refuses that, the driver tries its interrupt OUT endpoint (logged).
- **Reports longer than one USB packet** (some low-speed wireless receivers) need
  multi-packet interrupt transfers, which the UHCI and EHCI drivers don't do; such
  devices may lose keys there. Plugging the receiver into a USB 3 port (xHCI)
  avoids it.
- **Interrupt routing.** Without an ACPI AML interpreter, Halcyon learns which I/O
  APIC input each USB controller uses from the first interrupt it raises. If a
  controller shares its line with a device Halcyon has no driver for, it may stay
  polled: it still works, but the CPU wakes up more often. The `IRQ` command and
  `BOOT.LOG` show this.
- **Old AMD CPUs (about 2003–2011)** can stop their clocks in HLT (C1E), so on
  CPUs that report neither an always-running APIC timer nor an invariant TSC,
  Halcyon leaves CPU idle off and polls as before.
- **Native resolution** depends on the firmware reporting the monitor's EDID
  (VBE/DDC on BIOS, the EDID protocols on UEFI). `BOOT.LOG` shows it (`hw: monitor`)
  and why a mode was chosen (`hw: video modes`). Without EDID the loaders keep
  1024×768 (BIOS) or 1024×768 / the firmware's mode (UEFI). The VESA BIOS of many
  cards offers no wide-screen modes at all; UEFI's GOP usually offers the native
  one. Write-combining for the screen is logged as `display: frame buffer ...
  write-combining`.
- **Laptop touchpads** are usually I2C devices and are not supported. Use a USB
  mouse.
- **CD/DVD drives** are supported on SATA (AHCI) only, tested with QEMU's
  `ide-cd`: data discs (ISO 9660 / Joliet / Rock Ridge) are read-only, disc
  changes are detected by polling every 2 s. UDF-only and audio discs, and USB or
  IDE (non-AHCI) optical drives, are not supported.
- **NVMe SSDs** are tested in QEMU only (512- and 4096-byte blocks; MSI-X, INTx
  and polled). If an NVMe disk is missing, check the `nvme:` lines in BOOT.LOG and
  try F8 "Disable MSI"; "Disable NVMe controllers" turns the driver off.
- **Network adapters** other than QEMU's are untested (see below). The drivers
  only start the adapter, receive and send frames and report the link; a driver
  that hangs the boot can be avoided with F8 "Safe Mode", which leaves network
  adapters off.
- **exFAT** volumes (most USB sticks and SD cards larger than 32 GB) are read and
  written, but have only been tested in QEMU and with images made by Linux tools.
  Please report sticks formatted by Windows, a Mac or a camera that don't mount,
  with `BOOT.LOG`.
- **NTFS / Windows disks** are read and written, but have been tested with
  volumes made by Linux tools (mkntfs, ntfs-3g) and Halcyon's formatter only,
  not yet with Windows' chkdsk. Halcyon writes to an NTFS volume only if
  Windows shut it down completely. Windows 8 and later shut down with **Fast
  Startup** by default, which hibernates the system and leaves its drives in
  use: Halcyon then shows the drive read-only ("Windows did not shut down
  completely"). To write to it, turn Fast Startup off (Control Panel, Power
  Options, "Choose what the power buttons do", "Turn on fast startup") or hold
  Shift while choosing Shut down, or restart Windows instead of shutting down.
  A drive that needs a check (dirty) stays read-only until `chkdsk` has run in
  Windows. Internal disks with Windows on them are read-only until you allow
  writing in the drive's Properties. After writing from Halcyon, use "Safely
  Remove Hardware" (or shut Halcyon down) before unplugging; otherwise Windows
  checks the drive at its next start. Reports of Windows drives that don't
  mount, or that chkdsk complains about after Halcyon wrote to them, are very
  welcome (with `BOOT.LOG` and chkdsk's output).

## Network adapters

Each driver gives the adapter's MAC address, link state and speed, receive and
transmit rings, statistics, and its interrupt (MSI-X, MSI or INTx, polled when
none works). "Tested" means in QEMU with `tools/boottest.py`: the adapter on
QEMU's user-mode network under BIOS and UEFI, with MSI-X, INTx (I/O APIC and
legacy PIC) and polled, an ARP exchange with the gateway and 3,000 more
requests (every ring wraps many times), then a burst that fills the transmit
ring and overruns the receive ring.

| Driver | Adapters | PCI IDs | Status |
|--------|----------|---------|--------|
| `virtio-net` | virtio network device (QEMU/KVM, most cloud and VM hosts) | 1AF4:1000 (transitional), 1AF4:1041 (virtio 1.0) | Tested: virtio 1.0 interface, legacy I/O interface (with and without ANY_LAYOUT), non-transitional device; MSI-X, INTx, polled |
| `e1000` | Intel 82540EM/EP, 82541, 82545EM/GM, 82546, 82547 (8254x PCI/PCI-X) | 8086:100E and relatives | Tested with QEMU `e1000` (82540EM, INTx) |
| `e1000e` | Intel 82574L/LA | 8086:10D3, 10F6 | Tested with QEMU `e1000e`: MSI-X, INTx, polled |
| `e1000e` | Intel 82571EB/PT, 82572EI, 82573E/L/V, 82583V (copper) | 8086:105E, 10A4, 10BC, 10D5, 107D, 10B9, 108B, 108C, 109A, 150C | Written to the datasheets, untested |
| `e1000e` | Intel I217-LM/V, I218-LM/V, I219-LM/V (on-board, Haswell to Raptor Lake chipsets) | 8086:153A, 153B, 155A, 1559, 15A0-15A3, I219: 156F, 1570, 15B7-15B9, 15D6-15D8, 15E3, 0D53, 0D55, 15BB-15BE, 15DF-15E2, 0D4C-0D4F, 15F4, 15F5, 15F9-15FC, 1A1C-1A1F, 550C, 550D, 0DC5-0DC8 | Written to spec, untested: see below |
| `igb` | Intel 82576 (copper) | 8086:10C9, 10E8, 1526, 150A | Tested with QEMU `igb`: MSI-X, INTx, polled |
| `igb` | Intel I350, I210, I211 (copper) | 8086:1521, 1533, 1534, 1535, 157B, 1539 | Written to the datasheets, untested |
| `rtl8139` | Realtek RTL8139/8139A-D/8139C+, RTL8100/8130 and cards built on them | 10EC:8139 and 17 OEM IDs | Tested with QEMU `rtl8139` (INTx, polled) |
| `r8169` | Realtek RTL8169/8110 (PCI), RTL8111/8168 B to H (PCIe), RTL8411 | 10EC:8161, 8167, 8168, 8169 and 5 OEM IDs | **Untested**: QEMU has no model of these chips |

What is written to spec only, and what may go wrong on a real PC:

- **I217/I218/I219.** The PHY sits in the chipset and is shared with Intel's
  Management Engine. The driver takes the software semaphore (EXTCNF_CTRL.SWFLAG)
  for every PHY access, wakes the PHY from Ultra Low Power mode (through the ME
  when ME firmware is present, else by toggling LANPHYPC and clearing the ULP and
  SMBus settings in the PHY), and does not reset the PHY when the ME blocks it.
  Not done: the I219 descriptor ring flush that a reset after another driver
  (e.g. the UEFI network stack) used the rings can need (it is logged), EEE and
  the K1 power state, Wake-on-LAN. The MAC address is taken from the receive
  address register the hardware loads from the NVM; the NVM (in the platform
  flash) is not read. Meteor Lake and newer I219s, the 82577/82578/82579
  (2010-2012 chipsets) and ICH8-10 Ethernet are not supported.
- **82571/82572/82573/82583, I350/I210/I211.** The reset and semaphore
  sequences and the register bits the specification updates require are
  implemented; fiber, SerDes and SGMII ports are not claimed. An I210/I211
  with a blank iNVM has no MAC address and is not used.
- **RTL8169/8111/8168.** The chip version (TCR XID) selects the
  version-specific steps public documentation describes: MAC reset handling for
  the first 8169s, PHY wake-up, C+ mode, early-receive settings, the 8168G+
  receive gate. Realtek's per-version PHY parameter tables (undocumented
  register writes Realtek's own drivers make) are not applied, so some boards may
  link at a lower speed or not at all; the 8168G+ out-of-band (DASH) hand-over
  is not done. The RTL8101/8102/8105/8106 Fast Ethernet chips (10EC:8136) and
  the RTL8125 (2.5 Gbit/s) are not supported.
- Jumbo frames, checksum and segmentation offloads, VLANs and Wake-on-LAN are
  not used by any driver.

## Sound

Every driver plays 16-bit stereo through the kernel mixer (which resamples
each stream to the card's rate and applies the master volume), keeps a few
periods of 10 ms queued ahead of the hardware, counts underruns and recovers
from them, and works with its interrupt or polled. "Tested" means in QEMU
with `tools/boottest.py`: a 2 s tone and a short beep recorded from QEMU's
audio output and checked for pitch, length and dropouts, under BIOS and UEFI,
with the interrupt (I/O APIC and legacy PIC) and polled.

Sound cards other than HD Audio:

| Driver | Devices | IDs | Status |
|--------|---------|-----|--------|
| `virtio-snd` | virtio sound device (QEMU `virtio-sound-pci`) | 1AF4:1059 (virtio 1.0+ only) | Tested with QEMU 8.2 at 48 kHz: MSI-X, INTx, polled. The first output stream that takes S16 stereo at 48 or 44.1 kHz is used; capture streams, jack remapping and controls are not |
| `es1370` | Ensoniq AudioPCI ES1370 (Creative Sound Blaster PCI64/PCI128 of 1998) | 1274:5000 | Tested with QEMU `ES1370` (INTx, polled). Plays on DAC2 at 44.1 kHz (48 kHz is not a divider of its clock); the AK4531 mixer is set to 0 dB but was not checked (QEMU does not model it) |
| `sb16` | Creative Sound Blaster 16 and compatibles with DSP 4.xx (ISA) | ports 0x220 or 0x240 | Tested with QEMU `sb16` on the q35 machine (ISA IRQ 5 edge and level-triggered, PIC, polled; 16-bit DMA channel 5). Plays at 44.1 kHz; IRQ and DMA channel come from the card's mixer registers 0x80/0x81. Probed only when no PCI sound card was found |
| `ac97` | Intel ICH AC'97 and compatibles | 8086:2415 and others | Tested with QEMU `AC97` |
| PC speaker | PIT channel 2 and port 0x61 | – | Tested with QEMU's `pcspk-audiodev`. Used for beeps (message boxes, the Command Prompt's BEL) only when there is no sound card; otherwise beeps go through the card |

Not supported: the Ensoniq ES1371/ES1373 and Creative CT5880 (AudioPCI 97,
Sound Blaster PCI 128 rev. 2 / Live! "Value"; 1274:1371, 1274:5880): they
have an AC'97 codec and a sample rate converter whose setup is a different
driver. On real hardware the Sound Blaster 16 needs its DMA buffer below
16 MiB: it lives in the kernel image (loaded at 1 MiB), so this holds on any
PC; Plug and Play ISA cards that the firmware did not configure (no
resources assigned) are not found.

HD Audio (the `hda` driver: Intel High Definition Audio, the on-board sound
of nearly every PC since 2005, and the HDMI audio of graphics cards; plays at
48 kHz):

| Driver | Controllers | PCI IDs | Status |
|--------|-------------|---------|--------|
| `hda` | Intel High Definition Audio, ICH6 and ICH9 | 8086:2668, 8086:293E | Tested with QEMU `intel-hda` / `ich9-intel-hda` and the `hda-duplex`, `hda-output` and `hda-micro` codecs; also the immediate command interface and LPIB positions (test hooks) |
| `hda` | Intel ICH7/ICH8/ICH10, 5- to 9-series PCH, 100- to 800-series PCH (Skylake to Arrow Lake, desktop and mobile), Bay Trail, Braswell, Apollo/Gemini Lake, Jasper/Elkhart Lake, SCH | 8086:27D8, 284B, 3A3E, 3A6E, 3B56, 1C20, 1D20, 1E20, 8C20, 8CA0, 8D20, 9C20, 9CA0, A170, 9D70, 9D71, A2F0, A348, 9DC8, A3F0, 06C8, 02C8, 34C8, A0C8, 43C8, 4DC8, 7AD0, 51C8-51CF, 54C8, 7A50, 7E28, 7F50, 0F04, 2284, 5A98, 3198, 811B and others | Written to the specification, untested |
| `hda` | AMD/ATI SB450-SB950, FCH, Zen chipsets and APUs | 1002:437B, 1002:4383, 1022:780D, 1022:1457, 1022:1487, 1022:15E3 | Written to spec, untested |
| `hda` | NVIDIA nForce MCP51-MCP89; VIA VT8251/VT8237A; SiS 966; ULi M5461 | 10DE:026C-0D97 (list in the driver), 1106:3288, 1039:7502, 10B9:5461 | Written to spec, untested |
| `hda` | HDMI/DisplayPort audio of graphics cards (NVIDIA, AMD, Intel) and any other PCI class 04/03 device | by class | Written to spec, untested; used only when there is no analog output |

**Codecs.** The HD Audio driver has no per-codec tables: it reads the codec's
widget graph and the pin configuration the BIOS left (what each jack is and
where), and drives it to the specification. Analog codecs (Realtek ALC,
Conexant, IDT/SigmaTel, Analog Devices, VIA, Cirrus Logic) should all play:

- **Outputs.** Laptop speakers, line outs (the front pair: surround jacks stay
  off) and headphone jacks all play the same stereo stream, each through its own
  DAC where the codec has enough. Digital outputs (HDMI, DisplayPort, S/PDIF) are
  used only on a codec with no analog output at all, and a sound card with only
  those (a graphics card's HDMI audio, such as a GeForce's) never becomes the
  default device while another card exists. The device reports the output in
  use as "Speakers", "Line out", "Headphones" or "HDMI" (also in `BOOT.LOG`).
- **Headphone jack sensing.** While headphones are plugged in, laptop speakers
  are muted, and so are the other outputs of the headphones' group when the BIOS
  marks the jack as a headphone redirect (sequence 15, usual for front-panel
  jacks: on such desktops plugging headphones in front silences the rear line
  out, as with Windows' own HD Audio driver). Logged as `hda ...: headphones
  plugged in: playing on Headphones`. QEMU's codecs have no jack sensing, so
  this is tested only on the host with typical Realtek ALC269, ALC892 and
  ALC1220 layouts (`tests/host/hdatest.c`).
- **Volume** is set in the codec's DAC amplifier when it has at least 46 dB of
  range (full resolution at low volume), otherwise in the mixer. Amplifiers are
  never set above 0 dB.

What may go wrong on a real PC:

- **Laptops whose speaker amplifier is switched by a codec GPIO pin or a
  vendor-specific register** (some HP, Dell and Lenovo models) instead of the
  standard EAPD bit: the speakers stay silent while headphones work. Vendor
  fix-ups (Realtek coefficient registers, headset mic modes) are not applied.
- **Codecs that need firmware or a DSP**: Creative CA0132 (Sound Blaster Z, some
  Alienware), and Intel laptops whose BIOS puts the audio behind the DSP (Smart
  Sound / SOF, mostly with digital microphones); such controllers may not be
  usable, or be missing.
- **HDMI/DisplayPort** audio is written to the specification only (the audio
  InfoFrame, converter channel count); Intel's display codecs from Haswell on
  may hide some of their pins without a vendor-specific enable, and a graphics
  card's HDA function may be switched off by the BIOS when no HDMI display is
  connected.
- **Controller quirks** applied as other drivers document them: snooping (PCIe
  No Snoop off on every controller; the AMD/ATI and NVIDIA MCP coherence bits),
  Intel TCSEL = TC0 and, from Skylake on, clock gating off during reset; MSI is
  not used on ATI SB450/SB600, NVIDIA MCP, VIA and SiS; a CORB/RIRB that does not
  answer falls back to the immediate command interface (logged); a DMA position
  buffer that stays empty falls back to LPIB (logged).
- **Crackles or gaps.** The driver keeps about 40 ms queued ahead of the
  hardware and restarts the stream when the hardware runs dry; if sound
  stutters, please report the machine with `BOOT.LOG` (a stream that does not
  move or a descriptor error is logged).

## NVIDIA graphics cards

Halcyon draws on every graphics card through the frame buffer the firmware
set up (VESA on a BIOS, GOP on UEFI). For NVIDIA cards a native driver is
being written (milestone M10): its first step (10a) **identifies** the card
and reads, never changes, what is on screen; the second (10b) **sets modes**
on cards from the GeForce 8 to the GeForce 10 series, and 10d adds the
**hardware cursor, page flips** and **several monitors** there. 10b has run
on a GTX 1070; 10d has not run on a real card yet. "Tested" below means
with generated video BIOS images and simulated I2C / DisplayPort buses and a
simulated display engine on the host (`tests/host/nvtest.c`, `evotest.c`)
and a simulated card in QEMU.

| Generation | Cards | Identified | Video BIOS read from | Monitors (EDID) | Native modesetting |
|------------|-------|------------|----------------------|-----------------|--------------------|
| Celsius, Kelvin, Rankine, Curie (NV1x-NV4x, G7x) | GeForce 256 to GeForce 7 | Yes (untested) | PRAMIN, PROM, ROM BAR, C0000 | I2C through the CRTC registers (C51: its own I2C) | Deferred (M10c) |
| Tesla (G80-GT21x, MCP7x) | GeForce 8, 9, 200, 300 | Yes (untested) | PRAMIN, PROM, ROM BAR, C0000 | I2C ports; DisplayPort AUX from G94 on | Written (M10b, M10d), untested: VGA, DVI (dual link), DisplayPort from G94; cursor, page flips, two monitors |
| Fermi, Kepler, Maxwell, Pascal | GeForce 400 to GeForce 10 | Yes (untested; the GTX 1070 is the test card) | PRAMIN, PROM (with IFR data), ROM BAR, C0000 | I2C ports, hybrid pads, DisplayPort AUX (GM200 layout from GM20x) | M10b runs on the GTX 1070 (DisplayPort); otherwise written, untested: DVI / HDMI (DVI-mode signal), VGA up to Maxwell. M10d written, untested: cursor, page flips, up to four monitors (two before GF119) |
| Volta, Turing and later | Titan V, GeForce 16, RTX | Named only: "Turing or later: not supported until Halcyon 1.1" | - | - | - |

What it finds is in System Information (an "NVIDIA graphics" book with the
card and every connector's monitor) and in `BOOT.LOG` (lines starting with
`nv0:`).

**Please run NVREPORT** on a PC with an NVIDIA card (the GTX 1070 first):

1. Start Halcyon from the stick (UEFI is fine; the card's GOP driver sets the
   screen up).
2. Open the Command Prompt and type `NVREPORT` (or double-click
   `C:\PROGRAMS\NVREPORT.EXE`). It asks before measuring how fast the screen
   can be drawn: the screen flickers grey for about a second. `NVREPORT
   /nospeed` skips that.
3. Turn Off from the Start menu, then copy the folder `\HALCYON\NVREPORT`
   from the stick, and `\HALCYON\BOOT.LOG`, to another computer and send
   them back. `README.TXT` in the folder says what each file is:
   `REPORT.TXT` (the card, its video BIOS tables, the monitors, a dump of
   the display registers, the speed test), `VBIOS_*.ROM` (the video BIOS
   as read from each place) and `EDIDn.BIN` (each monitor's EDID). They
   describe the card and the monitors only; an EDID holds the monitor's
   model and serial number (leave the EDID files out if you prefer).

If Halcyon misbehaves while starting on such a PC (the last line of a
verbose boot starts with `nv`), add `nv.disable=1` to
`\HALCYON\SETTINGS.INI` on the stick, or use F8 Safe Mode, and send
`BOOT.LOG`. `nv.i2c=0` skips only the monitor part (I2C and DisplayPort
AUX), `nv.prom=0` and `nv.rombar=0` two of the places the video BIOS is
read from.

**Then try the native driver (M10b)** on the same PC; the card is the
first to run it, so expect surprises and please report them:

1. Start Halcyon from the stick (UEFI). When the desktop appears on
   Halcyon's own driver it asks **"Can you see this message?"**: click
   **Yes**. If the screen is dark, wait 15 seconds: the firmware's picture
   comes back by itself.
2. In Display Properties (Settings) the monitor's own resolutions and
   refresh rates should be offered; try one, and a second connector
   (DisplayPort, HDMI, DVI) if you can.
3. If the screen stays dark or the PC stops, switch it off and on: Halcyon
   notices that the driver's start did not finish, leaves it off and says
   so in a balloon. Run `NVREPORT` again (its REPORT.TXT then holds the
   driver's log and the registers after the fallback).
4. Send back `\HALCYON\BOOT.LOG`, `\HALCYON\BOOTPREV.LOG`, the
   `\HALCYON\NVREPORT` folder, which connector and monitor (and its
   resolution) you used and what the screen did.

Switches in `\HALCYON\SETTINGS.INI`: `nv.modeset=0` keeps the firmware's
picture (the card is still identified), `nv.scripts=0` runs no video BIOS
display scripts (one try if a connector stays dark), `display.disable=nv`
leaves the driver off, `display.nv.guard=` (empty) or **Turn on driver...**
in Display Properties turns it on again after a start that did not finish.

**Then the cursor, page flips and a second monitor (M10d)**, on the native
driver: the mouse pointer is drawn by the card (it should slide off the
left and top edges of the screen, and change shape at once), windows
dragged quickly should not tear, and a second monitor plugged into another
connector while Halcyon runs should extend the desktop within a second or
two (then Display Properties arranges them; the next start asks "Can you
see this message?" once for the new configuration). To narrow a problem
down, one switch at a time: `nv.flip=0` (no page flips: one buffer per
monitor, as M10b), `nv.hwcursor=0` (the software cursor), `nv.heads=1`
(one monitor, as M10b). Send back the same files as above; run `NVREPORT`
while the desktop runs on the driver, since its REPORT.TXT then has a
"Heads" section and each head's channel registers. The details are in
[DESIGN-M10-NVIDIA.md](DESIGN-M10-NVIDIA.md) ("What to check on the GTX
1070 (10d)").

**Then the refresh rates (144 and 165 Hz)**, on the native driver with the
LG UltraGear on DisplayPort. Its EDID lists 2560 x 1440 at 120 Hz (the
preferred timing), 60 and 144 Hz (CEA block) and 165 Hz (DisplayID block);
the driver now offers all four.

1. Display Properties, Settings: with 2560 x 1440 chosen, the **Refresh
   rate** list should say 60, 120, 144 and 165 Hertz (System Information
   and `BOOT.LOG`'s `display:   2560x1440 at 60, 120, 144, 165 Hz` say the
   same). If 144 or 165 Hz is missing, the DisplayPort link was read as too
   slow: the driver's line `DP-1: DisplayPort sink, DPCD 1.4, 4 lanes, rate
   1e` in `BOOT.LOG` says what it found (165 Hz needs 4 lanes at 5.4 or
   8.1 GHz).
2. Choose **144 Hertz**, **Apply**, and answer **Yes** if the picture is
   fine; then the same with **165 Hertz**. If the monitor goes dark or says
   "out of range", wait 15 seconds: the old rate comes back by itself. The
   monitor's own menu (the OSD) usually shows the rate it receives.
3. Restart: the rate chosen comes back (`display.dp-1.hz=165` in
   `\HALCYON\SETTINGS.INI`), and since the start guard has not seen that
   rate yet it asks **"Can you see this message?"** once more.
4. Send back `BOOT.LOG` (each mode set logs its timing, the video PLL's
   coefficients, e.g. `VPLL0 ... 679300 kHz (N 49 ... P 2 ...)`, and the
   link it trained, `DP: SOR2 trained, 4 lanes x 1e`), and what the
   monitor's OSD said at each rate.

The pixel clocks come from the EDID exactly (679.30 MHz for 165 Hz) and the
host tests check that the PLL hits them with the table limits the report
implies; what they cannot check is the card's own limit at that clock and
the cable. To go back by hand, delete the `display.dp-1.hz` line (the
monitor's 120 Hz is used) or choose a lower rate.

What may go wrong: the register addresses come from nouveau (Linux's
open-source driver) and are used the way it uses them, but nothing here has
run on a real card yet. Each step that times out or reads a strange value
is logged and given up, and the few registers it must write to read (the
PRAMIN window, the PROM enable bit, an I2C port or pad, an AUX channel's
request bits) are put back right away. Laptops whose NVIDIA GPU has no
outputs of its own (Optimus) show the card with no connectors.

## Compatibility list

| Machine | Year | Firmware | USB controllers | Result | Notes |
|---------|------|----------|-----------------|--------|-------|
| QEMU q35 | – | SeaBIOS / OVMF | UHCI, OHCI, EHCI, EHCI+UHCI, xHCI | Works | All 13 boot configurations pass in CI, including the legacy PIC, no-MSI and no-interrupt modes; network adapters virtio-net, e1000, e1000e (82574L), igb (82576) and rtl8139; sound cards AC'97, Intel HD Audio (ICH6, ICH9), virtio-sound, ES1370 and Sound Blaster 16 |
| *your PC here* | | | | | |
