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
   - Are your internal SATA disks listed? Only FAT-formatted volumes get a drive
     letter.
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
| Disable interrupts | Hangs or freezes after the "irq:" lines, or input stops working. Everything is polled, as in Halcyon 1.0, and the CPU never idles. |
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

## Compatibility list

| Machine | Year | Firmware | USB controllers | Result | Notes |
|---------|------|----------|-----------------|--------|-------|
| QEMU q35 | – | SeaBIOS / OVMF | UHCI, OHCI, EHCI, EHCI+UHCI, xHCI | Works | All 13 boot configurations pass in CI, including the legacy PIC, no-MSI and no-interrupt modes; network adapters virtio-net, e1000, e1000e (82574L), igb (82576) and rtl8139 |
| *your PC here* | | | | | |
