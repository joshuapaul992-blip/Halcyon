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
| Safe video mode | Black or garbled screen: forces 800×600. |

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
- **Interrupt routing.** Without an ACPI AML interpreter, Halcyon learns which I/O
  APIC input each USB controller uses from the first interrupt it raises. If a
  controller shares its line with a device Halcyon has no driver for, it may stay
  polled: it still works, but the CPU wakes up more often. The `IRQ` command and
  `BOOT.LOG` show this.
- **Old AMD CPUs (about 2003–2011)** can stop their clocks in HLT (C1E), so on
  CPUs that report neither an always-running APIC timer nor an invariant TSC,
  Halcyon leaves CPU idle off and polls as before.
- **Laptop touchpads** are usually I2C devices and are not supported. Use a USB
  mouse.
- **NVMe SSDs** are tested in QEMU only (512- and 4096-byte blocks; MSI-X, INTx
  and polled). If an NVMe disk is missing, check the `nvme:` lines in BOOT.LOG and
  try F8 "Disable MSI"; "Disable NVMe controllers" turns the driver off.
- **exFAT USB sticks** (most sticks larger than 32 GB) don't get a drive letter yet.

## Compatibility list

| Machine | Year | Firmware | USB controllers | Result | Notes |
|---------|------|----------|-----------------|--------|-------|
| QEMU q35 | – | SeaBIOS / OVMF | UHCI, OHCI, EHCI, EHCI+UHCI, xHCI | Works | All 13 boot configurations pass in CI, including the legacy PIC, no-MSI and no-interrupt modes |
| *your PC here* | | | | | |
