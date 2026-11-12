# Milestone 10 design: NVIDIA graphics

M10 brings a native driver for NVIDIA cards from the GeForce 256 to the
GeForce 10 series (see the roadmap). It is built in steps:

| Step | What | Status |
|------|------|--------|
| 10a | Identification: chip, VRAM, video BIOS and its tables, EDIDs over I2C / DP AUX, System Information, NVREPORT | **this document** |
| 10b | Modesetting on the NV50 "EVO" display engine (Tesla to Pascal) | to do (headings below) |
| 10c | Modesetting on the VGA-style CRTCs (Celsius to Curie) | deferred |
| 10d | Hardware cursor, page flips with vblank interrupts, several monitors | to do |
| 10e | 2D acceleration, if the speed test says it is needed | to decide |

The code lives in `kernel/drivers/nv/`:

| File | What | Host tested |
|------|------|-------------|
| `nvhw.h` | types and functions that touch no hardware | yes |
| `chip.c` | PMC_BOOT_0 / BOOT_42 to chipset, family, code name, register layouts; PCI IDs to card names; VRAM size from register callbacks | `tests/host/nvtest.c` |
| `vbios.c` | the PCI ROM image chain (IFR, PCIR, NPDE), BIT and BMP, DCB 1.5-4.1 with the I2C / CCB, connector and GPIO tables | `nvtest.c`, `fuzz_nv.c` |
| `i2c.c` | the bit-banged I2C master, one AUX transaction in the G94 / GM200 layouts, native AUX and I2C over AUX, EDID over DDC | `nvtest.c` (simulated buses) |
| `nv.h`, `nv.c` | the kernel side: probe, ROM sources, ports and pads, EDIDs, hot-plug sense; the API for 10b | QEMU test hook only |
| `report.c` | BOOT.LOG lines, the report text and register dump, System Information rows, `SYS_NV_QUERY` and the frame buffer speed test | QEMU (`nvfake`) |

![System Information with the QEMU test hook's simulated GTX 1070](m10a-sysinfo.png)

## 10a: identification

### Rules

- **Read only.** The only writes are the ones reading needs, each undone at
  once: the PRAMIN window base (`0x001700`), the PROM shadow bit (PCI config
  `0x50` bit 0), the expansion ROM BAR's enable bit, the CRTC index and
  lock registers (pre-NV50 I2C), an I2C port's bit-bang register, a hybrid
  pad's mode, an AUX channel's request bits and auto-DPCD bit, and the AUX
  status register write-back nouveau also does. Every one is saved first and
  put back after the step; the BOOT.LOG says so if a read-back differs.
- **Nothing on screen changes**: no display engine register is written; an
  I2C pad whose DisplayPort sink answered is never switched to I2C.
- **No interrupts nobody answers.** An AUX transaction or a pad switch
  latches status bits in the I2C / AUX unit's interrupt status. Halcyon has
  no NVIDIA interrupt handler, so on G94 and later the monitors are only
  read if the function cannot interrupt (INTx disabled and no MSI), the
  GPU's interrupts are off (`PMC_INTR_EN`, `0x000140`, = 0, which is what
  the firmware usually leaves) or the AUX unit's enables (`0x00e068`,
  `0x00dc68` from Kepler on) are all off; otherwise the connectors say why
  and BOOT.LOG has `INTR_EN` and "I2C / AUX left alone". 10b, which owns
  the interrupt, uses `nv_i2c_xfer` / `nv_aux_xfer` freely.
- **Bounded.** Every wait has a limit measured with the TSC: 2.2 ms clock
  stretching and 200 ms per I2C transfer, 1 ms for an AUX channel to become
  idle and to be granted, 2 ms per AUX transaction, 32 DEFERs, 3 s per ROM
  source, 6 s and 6 timeouts for all of a card's EDIDs. A timeout or an
  unexpected value (all ones, an NV50+ "PRI error" `0xbadf....`, a big-endian
  card, memory decoding off, a BAR0 under 16 MiB) is logged and that step is
  given up.
- **Switches**: `nv.disable=1` in `SETTINGS.INI` or F8 Safe Mode
  (`BOOTF_NO_DISPLAY`) skip everything; `nv.i2c=0` skips I2C and AUX,
  `nv.prom=0` and `nv.rombar=0` those two ROM sources.
- `nv_init()` runs in `kmain` after `settings_load()` (for the keys) and
  before the first `bootlog_save()`, so the `nvN:` lines are in BOOT.LOG.
  It takes about 0.1-0.5 s per card (PROM reads and DDC dominate).

### Probe

Every PCI function with vendor `10de` and class 03 (VGA, 3D controller):
BAR0 must be memory, at least 16 MiB, and decoding (the firmware set the
card up); its 2 MiB pages are made uncached (`mm_set_uncached`, PAT entry 3)
when below 4 GiB, so the register mapping does not depend on the MTRRs.
`PMC_BOOT_1` (`0x000004`) bits `0x01000001` set means big-endian mode:
skipped.

**Chip.** `PMC_BOOT_0` (`0x000000`): bits 28:20 are the chipset when 28:24
are non-zero (NV10 and later), else `0x20004000` is NV04 and NV05;
`PMC_BOOT_42` (`0x000a00`, read on Fermi and later) wins only when it names
a chip of Turing or later that BOOT_0 cannot. The table in `chip.c` maps
every chipset to a code name and product line; the family follows from the
range:

| Chipsets | Family | Display engine | I2C / AUX | VRAM size from |
|----------|--------|----------------|-----------|----------------|
| 04, 05 | Fahrenheit (Riva TNT, out of range) | NV04 | CRTC | PFB_BOOT_0 |
| 10-1F | Celsius (GeForce 256, 2, 4 MX = NV17/18) | NV04 | CRTC | `0x10020c` (NV1A/1F: system memory) |
| 20-2A | Kelvin | NV04 | CRTC | `0x10020c` |
| 30-36 | Rankine | NV04 | CRTC | `0x10020c` |
| 40-4E, 63-68 | Curie (C51 = NV4E has its own I2C) | NV04 | CRTC / NV4E | `0x10020c` |
| 50-AF | Tesla (G80, G92, GT200: NV50 I2C; G94 on: AUX) | NV50 | NV50 / G94 | `0x10020c` with high bits; MCP7x `0x100e14` |
| C0-CF | Fermi GF100-GF116 | NV50 | G94 | FB partitions `0x11020c` |
| D7, D9 | Fermi GF117 / GF119 | GF119 | GF119 | FB partitions |
| E4-108 | Kepler | GF119 | GF119 | FBPAs (`0x022438`, `0x02243c`) |
| 117-118 | Maxwell GM10x | GF119 | GF119 | FBPAs, mask `0x021c14` |
| 120-126 | Maxwell GM20x | GF119 | GM200 | FBPAs per FBP (`0x022458`) |
| 130 | Pascal GP100 | GF119 | GM200 | `0x90020c` per FBPA |
| 132-138 | Pascal GP102-GP108 (GTX 1070: 134) | GF119 | GM200 | `0x100ce0` (MMU local memory range, ECC 15/16) |
| 140 | Volta | - | - | "not supported until Halcyon 1.1" |
| 160 on | Turing or later | - | - | "Turing or later: not supported until Halcyon 1.1" |

Volta and later are named only (no ROM, I2C or VRAM). The VRAM value must
be 1 MiB to 64 GiB in whole MiB, else "unknown". It is checked against BAR1:
up to Curie BAR1 maps all of it (a missing value is taken from BAR1, a
different one noted); from Tesla on BAR1 is a window and must not be larger.
The card's name comes from a table of about 130 PCI device IDs (checked
against the PCI ID database), else "<product line> (<code name>)".

**Bus.** The PCI Express capability's link status and capabilities (speed,
width; Kepler and later slow the link when idle, said in the report), or the
AGP capability's status and command registers.

### Video BIOS

Sources, in this order (as nouveau), every one read whatever the others gave,
the best kept:

| Source | How | Notes |
|--------|-----|-------|
| PRAMIN | NV50+: unless the display is fused off (`0x022500` Fermi, `0x021c04` Maxwell+ bit 0), `0x619f04` must say "enabled, in VRAM"; its address (bits 31:8 << 8, or `0x001700` << 16 + `0xf0000`) goes into the PRAMIN window `0x001700` (restored), the image is read at `0x700000`. Before NV50 the window shows it as it is. | The copy the VBIOS made at POST: what the GPU uses |
| PROM | PCI config `0x50` bit 0 cleared (restored) so the window at `0x300000` shows the flash, not the shadow. Skipped on integrated GPUs and if `0x50` lies inside a standard capability. | May start with IFR data (Maxwell, Pascal) |
| PCI ROM BAR | Only if the firmware assigned an address; sized, checked against the other BARs, enabled for the read only (BAR0 untouched meanwhile: the decoder may be shared) | |
| ACPI `_ROM` | Not available: it is an AML method and Halcyon has no AML interpreter | |
| Legacy shadow | `C0000`, first card only, only if the image's PCI device ID is this card's | BIOS boot |

Reads go 32 KiB at a time until the image chain is complete (at most 1 MiB,
3 s); a source with no ROM signature in its first 256 KiB stops early. Each
copy is kept (trimmed to its chain) for NVREPORT.

**Validation** (`nv_rom_scan`). The PCI ROM starts at 0 if `55AA` + PCIR are
there; else after an IFR header (`"NVGI"`: version 1 / 2, the offset at
`FIXED_DATA_SIZE + 4`; version 3, the ROM directory `"RFRD"` at the flash
status offset + 4096, the offset at +8); else at the first 512-byte boundary
with `55AA` and a PCIR structure (as the BIT specification says). Images
follow each other: signature (`AA55`, later ones also `BB77` / `"NV"`), PCIR
(also `NPDS`, `RGIS`), NPDE right after it (16-byte aligned) overriding size
and "last" (the legacy image says "last" for PC BIOSes). The legacy image's
bytes must sum to 0. Score: 1, +3 per image (+1 for a legacy image with a bad
checksum), +1 for a complete chain; 0 if PCIR's vendor is not NVIDIA. The
highest score wins, the earlier source on a tie.

**Pointers.** BIT / DCB pointers past the legacy image point into the
NVIDIA data image (code type `E0`; or the image after an EFI image): `p -
legacy_size + base_of_E0` (nouveau's rule; the BIT specification says the
same as "+ the EFI image's length"). All reads are bounds checked
(`nv_rom_rd8/16/32`) and count out-of-range reads.

**BIT** (`FF B8 "BIT"` in the legacy image): header size, token size and
count, checksum (a bad one is noted, parsing goes on); tokens as id,
version, size, pointer. The version comes from token `'i'` (or `'B'`):
`86.04.50.00.70` style. **BMP** (`FF 7F "NV" 00`, before NV40): version
5.x keeps a DCB pointer at `0x36`, older ones two CRTC I2C pairs at `0x48`
(BMP < 4) or in the BMP structure.

**DCB** (pointer at `0x36`): 4.x and 3.0 with the `4EDCBDCB` signature at
+6, header size / count / entry size, CCB at +4, GPIO at +10, connector table
at +20; 2.x with the signature at +4 and the I2C table at +2 (entries up to
it, GPIO through `dcb - 15` when `dcb[-1] >= 0x13`); 1.5 with `"DEV_REC"`
before it (10-byte entries, a different bit layout); older: nothing usable.
Entries stop at a type `E` or an all-zero / all-one word, type `F` is
skipped. Each output: type, CCB index, heads, connector, bus, location,
OR / pad macro, sub-link, external encoder, HDMI, DP lanes and rate, EDID
source. CCB 4.1 (GM20x+): I2C port and AUX channel per pad (`1F` = none, the
AUX channel is also the hybrid pad); CCB 3.0 / 4.0: access method 5 (I2C,
optional hybrid pad) or 6 (AUX); before 3.0: 16 fixed entries, CRTC
register pairs. Connector table 3.0 / 4.0: type, location, hot-plug lines A
to G, DP-to-DVI, AUX/I2C select. GPIO 3.x, 4.0, 4.1 (line, function,
logical states). DCB 2.x / 1.5 have no connector table: connectors are made
up from the outputs' connector fields (CRT + TMDS = DVI-I). Malformed parts
set bits in `problems` and are skipped; counts are capped (16 outputs,
CCB entries and connectors, 48 GPIOs, 40 BIT tokens).

### Monitors

A connector's outputs are tried DisplayPort first, then the I2C ones (DVI,
HDMI, VGA, a DP++ adapter), once per port:

| Port kind | Generations | Registers |
|-----------|-------------|-----------|
| CRTC-indexed | NV04-Curie | CR3F/CR3E (or the BMP / I2C table's pair) through `0x6013d4/5`, CR1F unlocked (0x57) and locked again |
| NV4E | C51 | `0x600800 + n` |
| NV50 | G80-GF116 | `0xe138, 0xe150, 0xe168, 0xe180, 0xe254, 0xe274, 0xe764, 0xe780, 0xe79c, 0xe7b8` (bits 0 SCL, 1 SDA) |
| GF119 | GF119-Pascal | `0xd014 + 0x20 n` (drive bits 0 / 1, sense bits 4 / 5) |
| hybrid pads | G94-GM10x / GM20x+ | `0xe500 / 0xe50c + 0x50 n`, `0xd970 / 0xd97c + 0x50 n`: I2C `0xc001`, AUX `0x0002` in bits `0xc003`, power bit 0 cleared |
| AUX | G94-GM10x / GM20x+ | `0xe4c0 / 0xd930 + 0x50 n`: data out +0, in +0x10, address +0x20, control +0x24, status +0x28; GM200: auto-DPCD +0x38 bit 16 |

I2C is bit-banged at 100 kHz with clock stretching, bus recovery (nine
clocks for a slave stuck mid-byte) and NAK detection. An AUX transaction
waits for the channel to be idle, requests it (control bits 22:20, granted
in 26:24), checks hot-plug sense (status bit 28), turns auto-DPCD off,
resets, starts (bit 16) and reads the reply (status 19:16, count 4:0, errors
bits 8-11), then puts the request and auto-DPCD bits back. On top: native
reads (DPCD) and I2C over AUX with MOT between messages, DEFER / I2C DEFER
retried 32 times 0.4 ms apart, NACK reported, and an address-only stop
after an error. The DPCD's revision and lanes are noted. EDID: block 0 at
`0x50` (header and checksum, three tries), then every extension (E-DDC
segment pointer `0x30` from block 2 on), up to 8 blocks. `edid.c` gained
`edid_timings()` (the detailed timings of the base block, CEA-861 blocks and
DisplayID type I / VII timings), `edid_block_count()` and `edid_ext_name()`.

Hot-plug sense: the connector's hot-plug lines map to GPIO functions
`07, 08, 51, 52, 5E, 5F, 60`, their line is read (`0x600818..` NV10,
`0xe104..` NV50, `0xd610 + 4 n` bit 14 GF119) and compared with the
"on" state.

### The interface for 10b (`nv.h`)

```c
typedef struct nv_device {
    pci_dev_t *pci; int index;
    volatile u8 *mmio; u64 mmio_phys, mmio_size; nv_regs_t regs;
    nv_chip_t chip;                 /* chipset, family, codename, disp / i2c / gpio / vram kinds */
    char name[96];                  /* "NVIDIA GeForce GTX 1070" */
    u64 vram, bar1_size; struct { int pcie, speed, width, max_speed, max_width; ... } link;
    nv_rom_copy_t rom[NV_SRC_COUNT]; int rom_best;   /* every copy read, and which is used */
    nv_bios_t *bios;                /* outputs (DCB), ccb (I2C), conn, gpio, tokens, version */
    int nconn; nv_connector_t conn[NV_CONN_MAX];     /* outputs on it, hot plug, EDID, edid_info_t */
    ...
} nv_device_t;

int  nv_count(void); nv_device_t *nv_get(int i); nv_device_t *nv_for_pci(const pci_dev_t *p);
u32  nv_rd32(nv_device_t *, u32 reg); void nv_wr32(...); u32 nv_mask(...); u8 nv_rd08(...); void nv_wr08(...);
int  nv_i2c_xfer(nv_device_t *, int ccb, nv_i2c_msg_t *msgs, int n);           /* pad + port taken and given back */
int  nv_aux_xfer(nv_device_t *, int ccb, u8 req, u32 addr, u8 *buf, u8 *len);  /* one AUX transaction */
int  nv_aux_read(nv_device_t *, int ccb, u32 addr, u8 *buf, int len);          /* DPCD */
int  nv_aux_i2c(nv_device_t *, int ccb, nv_i2c_msg_t *msgs, int n);            /* I2C over AUX */
int  nv_ccb_has_i2c(nv_device_t *, int ccb); int nv_ccb_has_aux(nv_device_t *, int ccb);
int  nv_read_edid(nv_device_t *, const nv_dcb_output_t *o, u8 *buf, int max, char *via, int vian);
const nv_dcb_output_t *nv_output(nv_device_t *, int index);
const nv_dcb_output_t *nv_output_on(nv_device_t *, int conn, int type, int nth);
nv_connector_t *nv_connector(nv_device_t *, int conn);
int  nv_hotplug(nv_device_t *, int conn);                                    /* 1, 0, -1 unknown */
```

The host-testable layer (`nvhw.h`) has the same pieces without a device:
`nv_chip_identify`, `nv_vram_probe`, `nv_rom_scan`, `nv_bios_parse` (+
`nv_rom_rd*` with the pointer rule, `nv_bios_token`), `nv_i2c_bb_xfer`,
`nv_aux_hw_xfer`, `nv_aux_native_read`, `nv_aux_i2c_xfer`,
`nv_ddc_read_edid`. 10b would add: the display engine's state read back
(the register dump below decodes the armed core channel state already),
the EVO core channel, and an adapter driver for the new display layer. The
DCB's output resource masks (`or_mask`), heads and links, and the CCB / AUX
mapping are what it needs to route a connector to a SOR and head.

### What the user sees

- **BOOT.LOG**: `nvN:` lines with the chip, BOOT_0 / BOOT_42 / PMC_ENABLE,
  BAR0 mapping, VRAM and how, the link; every ROM source's result and time;
  the VBIOS version and table versions; every DCB output (with its raw
  words); every connector with hot-plug state and monitor; the time taken
  and timeouts. Without a card: `nv: no NVIDIA graphics card`.
- **System Information**: an "NVIDIA graphics" book (only when there is a
  card): the card (name, chip, family, product line, VRAM, VBIOS version
  and source, bus, "Display driver: firmware frame buffer (native
  modesetting arrives with M10b)") and below it each connector: type,
  outputs, hot-plug sense, the monitor's name, maker, native mode, size and
  how its EDID was read.
- **NVREPORT.EXE** (`nvreport [/nospeed] [/quiet]`) writes
  `C:\HALCYON\NVREPORT\`: `REPORT.TXT` (the machine's display adapters and
  frame buffer; per card everything above, every image of every ROM copy,
  the BIT tokens, the DCB / CCB / connector / GPIO tables decoded with raw
  words, the monitors with all their timings, a read-only register dump: PMC,
  PFB and FB partitions, the display fuse, the shadow and PRAMIN windows, the
  display engine's armed state per head (timings, pixel clock, control),
  SOR / DAC / PIOR routing and power, raw core channel and PDISP words, the
  I2C / AUX / pad / GPIO registers; and the raw DCB; then the speed test),
  `VBIOS_<source>.ROM` per copy, `EDID<connector>.BIN`, and `README.TXT`
  (what the files are, that they describe the card and monitors only, that
  EDIDs carry the monitor's serial number). It asks before the speed test
  and says where the files are at the end (`/quiet`: no questions).
- **`SYS_NV_QUERY`** (196; the range 196-199 is reserved for graphics
  cards): `NVQ_COUNT`, `NVQ_REPORT` (a card, or -1 for the machine part),
  `NVQ_ROM` / `NVQ_ROMNAME` (copies by source), `NVQ_EDID` (by connector),
  `NVQ_FBTEST` (one test).

### Frame buffer speed test (for 10e)

Six tests of about 0.2 s each, one system call each so the desktop runs in
between and repaints itself after: fill with `rep stosq`, fill with 32-bit
stores, RAM to screen copy (what the compositor does), screen to screen copy
(half the screen up and down, reads the frame buffer), screen read, and RAM
to RAM copy for comparison. Reported as MB/s and whole frames per second at
the current mode, with the frame buffer's page attribute (write-combining
from M5). In QEMU (TCG) the numbers mean little; on the GTX 1070 they decide
whether 10e is needed: if RAM to screen stays far above 60 frames/s, CPU
drawing is enough.

### Tests

- `tests/host/nvtest.c` (`make -C tests/host nv`, part of `tests/host/run.sh`,
  ASan/UBSan): 96,000 checks. The chip table for every family and bad
  BOOT_0 values, BOOT_42; VRAM sizing for NV04, NV40, G92, GF110, GK104,
  GM204 and GP104 (with ECC) against simulated registers. A generator builds
  video BIOS images in 11 layouts (BMP 2.1 without DCB, BMP 5 with DCB 1.5
  and 2.2, BIT with DCB 2.2, 3.0, 4.0 and 4.1, a Pascal-style chain of PC-AT
  + EFI + NVIDIA data images with NPDE and tables behind translated
  pointers, the same behind IFR headers of versions 2 and 3, a ROM found at
  a 512-byte boundary), each checked field by field, cut at every length
  and mutated 20,000 times; then targeted damage: bad checksum (lower
  score), wrong vendor, image size 0, PCIR pointer outside, DCB signature
  and version, 255 x 255-byte entries, table pointers past the end, CCB 4.2,
  GPIO 5.0, connector 2.0, BIT checksum and absurd token counts, broken IFR
  offsets and directory. EDID: the timings of a base + CEA-861 + DisplayID
  EDID and 20,000 damaged extension blocks. I2C: the bit-banged master
  against a simulated open-drain bus with an E-DDC EEPROM (reads of 1-4
  blocks with segments, a buffer too small, no device, 1 ms clock stretching
  accepted, 5 ms refused, a slave stuck mid-byte freed, SDA stuck low for
  good, the total time limit, a register offset write). AUX: both register
  layouts and two channels against a simulated DisplayPort sink (DPCD, EDID
  over I2C over AUX in 1-4 blocks, DEFER and I2C DEFER, DEFER forever, no
  EDID device, no sink, never granted, never finishing, busy before, receive
  errors, sink timeouts retried, firmware request bits put back); every case
  checks that the request and auto-DPCD bits are as they were.
- `tests/host/fuzz_nv.c`: libFuzzer on `nv_bios_parse`, `edid_timings` and
  `edid_parse` (`make -C tests/host fuzz-nv-run`); the seeds (every generated
  layout) are replayed with gcc + ASan/UBSan in `run.sh`
  (`fuzz-nv-regress`). A 4-minute run: 5.6 million inputs, no crash.
- QEMU has no NVIDIA GPU and no way to give a PCI device NVIDIA's IDs (only
  `vfio-pci` takes `x-pci-vendor-id`), so the kernel side is tested through
  a test hook: `tools/boottest.py xhci:uefi:nvfake` (in the full matrix)
  passes `opt/halcyon/nvfake` (`0x134000a1,0x1b81`), `nvrom` (the generator's
  Pascal-style image) and `nvedid` through fw_cfg; the kernel makes a card
  without registers from them, logs it, and `NVREPORT.EXE /quiet` must write
  `REPORT.TXT` (with the speed test), `README.TXT`, `VBIOS_PROM.ROM` and
  `EDID0.BIN` byte for byte as passed in (mtools on the host). Every other
  boot of the matrix logs `nv: no NVIDIA graphics card` and changes nothing.

### Not verified without hardware

Everything that touches a real card: the BAR0 mapping, every register read
and the few writes above, the PRAMIN / PROM / ROM BAR reads, the I2C ports,
pads and AUX channels, hot-plug sense, VRAM sizing, and the timing of it all.
The register addresses come from nouveau, which runs on these cards; the
order of operations follows it, with save / restore added. The first test on
the GTX 1070 (NVREPORT and BOOT.LOG) is what 10b starts from.

## 10b: Tesla to Pascal modesetting

(to be written)

## 10d: cursor, page flips, several monitors

(to be written)

## 10e: 2D acceleration

(to be written: decided from NVREPORT's frame buffer speed test on the GTX 1070)
