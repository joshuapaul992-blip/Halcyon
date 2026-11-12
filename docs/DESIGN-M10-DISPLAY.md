# Milestone 10 design: the display core

Milestone 10 brings native graphics drivers (NVIDIA first, see ROADMAP.md).
Before a modesetting driver can be useful the system needs somewhere to plug
it in: an interface for adapters with several heads, connectors with
monitors, modes with full timings, scanout buffers, hardware cursors and
page flips at vertical blank; a desktop that spans several monitors; and
Display Properties to arrange them. This document describes that display
core, the two drivers that use it in QEMU (Bochs and virtio GPU), and what
a native driver has to implement ("Writing a native display driver").

Files:

| File | What |
|------|------|
| `kernel/drivers/display.h`, `display.c` | the core: registry, firmware adapter, configurations, presentation (with optional acceleration, M10e), cursor, fallback |
| `kernel/drivers/modes.h`, `modes.c` | timings: EDID timings, DMT and CEA tables, CVT reduced blanking, mode lists (host-tested) |
| `kernel/drivers/bochs.c` | QEMU std-vga / bochs-display / VirtualBox VGA |
| `kernel/drivers/virtio_gpu.c` | virtio GPU (`virtio-gpu-pci`, `virtio-vga`, `-vga virtio`) |
| `kernel/gui/rect.h`, `layout.h`, `layout.c` | rectangles and the monitor arrangement math (host-tested) |
| `kernel/gui/wm.c`, `desktop.c`, `gfx.c` | the multi-monitor desktop, the hardware cursor, presenting |
| `kernel/apps/display.c` | Display Properties, Settings tab |
| `kernel/apps/sysinfo.c` | System Information: adapters, connectors, monitors |
| `tools/vnc_ctl.py` | QEMU output hotplug for the tests (RFB SetDesktopSize) |
| `tests/host/modetest.c`, `layouttest.c`, `presenttest.c` | host tests |

## Words

- **Adapter**: one display device (a PCI function) and its driver. The
  firmware frame buffer (VBE or GOP) is an adapter too.
- **Head**: a scanout engine (a CRTC): it reads a buffer and sends one
  mode's timing to an output. Each head has its own mode, scanout buffer(s),
  hardware cursor and vertical blank.
- **Connector**: where a monitor plugs in (`DP-1`, `HDMI-1`, `VGA-1`,
  `Virtual-2`): a status, an EDID, a mode list, and a mask of the heads that
  can drive it (`possible_heads`; on NVIDIA the routing goes through the
  output resources the DCB describes). The connector's name is its key in
  `SETTINGS.INI`.
- **Output** / monitor: a connector on the desktop, driven by a head.
- **Buffer**: a scanout buffer (`display_fb_t`): pixels the CPU writes and a
  head reads (VRAM through a write-combining BAR, or RAM the device fetches
  from).

## The interface

`display.h` holds both sides. A driver fills a `display_ops_t` and calls:

```c
display_adapter_t   *display_register(driver, name, ops, priv, pci);   /* from its probe */
display_head_t      *display_add_head(a, caps, cursor_size);            /* HEAD_CAP_CURSOR|FLIP|VBLANK */
display_connector_t *display_add_connector(a, name, type, possible_heads);
void display_connector_update(c, status, edid, edid_len, pref_w, pref_h);   /* from ops->detect */
void display_hotplug(a);          /* status may have changed: the core re-runs detect */
void display_flip_done(h);        /* the queued flip is on screen */
int  display_test_fail(a, step);  /* fw_cfg test hook for the fallback tests */
```

and sets the adapter's limits: `live` (modes switch at once), `max_w`,
`max_h`, `max_clock_khz`, `vram` (bytes a head's buffers may use),
`virtual_sizes` (virtual hardware: offer the common sizes too), `rates`
(`set_mode` programs each mode's own timing: every refresh rate the
monitor lists is offered, see "Refresh rates"), `owns_fw_fb` (the device
shows the firmware frame buffer; the core also works it out from the
BARs).

The operations (all called on the GUI thread, never from an interrupt):

| op | required | does |
|----|----------|------|
| `enable(a)` | no | take the device over from the firmware (before the first mode set) |
| `restore(a)` | no | give it back: show `g_bootinfo`'s frame buffer again (after a failure) |
| `detect(a)` | yes | read every connector's status and EDID (`display_connector_update`) |
| `mode_valid(a, c, m)` | no | can the hardware drive this timing on this connector? |
| `fb_alloc(a, w, h, fb)` / `fb_free` | live adapters | a 32 bpp XRGB scanout buffer |
| `set_mode(h, c, m, fb)` | yes | drive connector `c` from head `h` with mode `m` (full timing) scanning out `fb`; `m == NULL`: head off |
| `flush(h, fb, x, y, w, h)` | no | the CPU wrote that rectangle (devices that copy from RAM: virtio) |
| `flip(h, fb)` | with `HEAD_CAP_FLIP` | show `fb` from the next vertical blank; `display_flip_done(h)` when it is |
| `wait_vblank(h)` | with `HEAD_CAP_VBLANK` | wait (bounded) for the next vertical blank |
| `cursor_image(h, argb, size, hx, hy)` / `cursor_move(h, x, y, visible)` | with `HEAD_CAP_CURSOR` | hardware cursor; the position is the hot spot's, in head coordinates |
| `poll(a)` | no | main-loop work: completed flips, hotplug status, deferred commands |
| `panic_flush(h, fb)` | no | after a fatal error, polled: make the panic screen drawn into `fb` visible |

A **mode** (`display_mode_t`, `modes.h`) is the size, the rounded refresh
rate, flags (sync polarities, interlace, where it came from: EDID,
preferred, DMT, CVT, driver) and the full timing (`display_timing_t`:
pixel clock in kHz, active / sync start / sync end / total in both
directions), as a CRTC is programmed. Modes from the firmware's list have no
timing (`clock_khz == 0`).

### Mode lists

`modes_build()` makes a connector's list from its EDID and the adapter's
limits:

- every timing the monitor lists: the detailed timing descriptors of the
  base block (the first is the preferred one), of CTA-861 extension blocks
  and of DisplayID extension blocks (type I and type VII detailed timings;
  their own "preferred" flag counts only when nothing before was
  preferred), CTA short video descriptors (a table of the common
  progressive VICs), standard timings (also from 0xFA descriptors) and
  established timings, these two through a VESA DMT table or, for sizes
  it lacks, the CVT reduced blanking formula;
- with `virtual_sizes`, the common sizes from 640 x 480 to 3840 x 2160
  (DMT or CVT-RB timings, 60 Hz where DMT has it), and the hardware's size
  hint (virtio's display info) as the preferred mode when the EDID has
  none;
- filtered by the limits: at least 640 x 480, at most `max_w` x `max_h`,
  pixel clock up to `max_clock_khz`, `w * h * 4 * buffers <= vram` (two
  buffers with page flips), no interlaced modes (none are listed), no
  timing below half a frame per second, and the driver's `mode_valid`
  (Bochs: widths divisible by 8; NVIDIA: the head's pixel clock, TMDS
  clocks, the DisplayPort link) — called for each timing, so for each
  refresh rate separately;
- without `rates`: one entry per size (the preferred timing, else the
  monitor's timing nearest to 60 Hz, else DMT, else CVT);
- with `rates`: one entry per size and refresh rate, the rate rounded to
  whole Hertz; timings that round to the same rate are one entry: the
  preferred timing, else the monitor's detailed timing (its exact clock:
  a CEA DTD at 59.94 Hz beats VIC 4's 60 Hz, the EDID's 2560 x 1440 at 60
  Hz beats DMT's identical one), else another timing it lists, else DMT,
  else CVT. A preferred timing the limits leave out (a DisplayPort link
  too slow for it) leaves its size recommended at the highest rate left
  there;
- sorted by width, height, then rate.

`modes_find(w, h)` is the mode to use at a size: the one at the preferred
mode's refresh rate if the size has it, else the highest rate there.
`modes_find_rate(w, h, hz)` the exact one, `modes_rates(w, h)` the rates
at a size, `modes_saved(w, h, hz)` a saved one or `modes_find`'s with a
flag that it fell back. `modes_default()` picks the mode to start with
when nothing was chosen: the preferred one up to 2560 x 1600 (the UI has no
scaling), else an exact fraction of it (a 4K panel gets 1920 x 1080), else
the largest of its shape, else the largest up to 1024 x 768 — the loaders'
policy (`boot/modesel.h`) — at `modes_find`'s rate.

### Refresh rates

Today only native drivers that program the mode's timing themselves set
`a->rates`: the NVIDIA driver (each head's own video PLL at the timing's
exact clock: 679.30 MHz for 2560 x 1440 at 165 Hz on the user's LG
UltraGear). The firmware frame buffer cannot change the rate (the loader
chose the mode), and Bochs and the virtio GPU have no real refresh (QEMU
shows the picture whenever it is flushed; virtio's emulated vertical
blank only paces page flips): their lists keep one mode per size, and
Display Properties shows their rate as "Default". The virtio GPU offers
rates only with the QEMU test hook `opt/halcyon/vgpuedid` (an EDID with
several rates for its first output; the rate then sets the emulated
vertical blank's period), for the boot tests.

On the NVIDIA driver `mode_valid` checks every rate on its own: the head's
pixel clock limit (1080 MHz on Pascal), TMDS limits, and for DisplayPort
the most the link can carry — the lanes and rate both ends support (the
sink's DPCD, capped by the DCB entry and the generation, read at detect),
24 bits per pixel through 8b/10b. A mode set retrains the link for the
mode's own clock, the widest and fastest configuration first, so a rate
change on a lit head (165 Hz after 120 Hz) trains again; 165 Hz (2037.9
MB/s) fits 4 lanes at 5.4 or 8.1 GHz, not 2 lanes.

### Configurations

A `display_conf_t` says, for every connector of the started adapters,
whether it is on the desktop (`attach`), its mode (`w` x `h` and the
refresh rate `hz`, 0 for `modes_find`'s), its place (`x`, `y`) and which
one is the primary monitor. `display_conf_mode()` finds the mode an entry
means. `display_get_conf()` returns the one in use; `display_set_conf()`
applies another one live:

1. heads are assigned (the primary's first; a connector keeps its head),
2. the places are normalized (`layout_normalize`, below),
3. heads that go off or change connector are switched off first,
4. each changed head (another size, rate or timing) gets new buffers
   (black), `set_mode`, and the old buffers are freed,
5. `display_changed_hook` lets the GUI follow (a new desktop surface,
   wallpaper, windows moved, the pointer kept on a monitor).

If any step fails the old configuration is applied again; if even that
fails, the adapter is given up and the firmware frame buffer comes back
(below). `display_save_conf()` writes it to `SETTINGS.INI`:

```
display.primary=Virtual-1
display.virtual-1.attach=1
display.virtual-1.width=1280
display.virtual-1.height=800
display.virtual-1.x=0
display.virtual-1.y=0
display.virtual-2.attach=1
display.virtual-2.width=1024
...
display.dp-1.hz=165         (the refresh rate: only where the adapter offers a choice)
display.width=1280          (the primary's mode: the loaders read these)
display.height=800
```

At the start (and when a monitor comes back by hotplug) a saved size and
rate is used if the list has it. A saved rate it does not have any more
(another monitor on the connector, a slower cable or link) gives
`modes_find`'s rate at that size — the monitor's preferred one if it is
offered there, else the highest — and the log says so ("display: DP-1:
165 Hz at 2560x1440 is not offered any more: 120 Hz instead (the
monitor's preferred rate)"); BOOT.LOG is saved again once the display has
started, since it was last saved before.

Other keys: `display.disable=virtio-gpu,bochs` (drivers not to start: safe
mode for one driver), `display.flip=0` (no page flips),
`display.hwcursor=0` (software cursor).

### Start and the fallback rule

- `display_init()` (after PCI, from `kmain`) registers the firmware frame
  buffer as an adapter with one head and one connector (`Default`), whose
  modes are the loader's list (they change at the next start), and runs the
  native drivers' probes, unless the F8 menu's "no display driver" or safe
  video flag is set. A probe only identifies the device and registers it:
  nothing on the screen changes, so the boot splash and the verbose console
  keep using the firmware frame buffer.
- `display_start()` (from `gui_main`, once `SETTINGS.INI` is loaded) starts
  every probed adapter that `display.disable` does not name: `enable`,
  `detect`, mode lists, the configuration from the settings (else: each
  connected connector on, the firmware's mode for the adapter that owns the
  firmware frame buffer, else the default mode, monitors left to right in
  connector order), then one `display_set_conf`. An adapter that owns the
  firmware frame buffer and comes up replaces the firmware adapter; another
  one (a second card) adds its monitors next to it.
- **Fallback**: a driver failing at any step — `enable`, `fb_alloc`,
  `set_mode`, no monitor connected, a later configuration that cannot even
  be undone — is given up: `restore` gives the device back (Bochs: the
  firmware's registers; virtio: a device reset, after which virtio-vga shows
  its VGA part again), the firmware frame buffer becomes the primary output
  again, and the reason goes to the kernel log (`BOOT.LOG`): "display:
  Virtio GPU (1 output) (virtio-gpu) failed: ...; the firmware frame buffer
  is used instead". Never a black screen, as long as the firmware's frame
  buffer was visible to begin with (see "Not done" for virtio-gpu-pci under
  UEFI).
- **The start guard** (`display_guard_begin`, for drivers that set
  `a->guarded`: the NVIDIA one): right before the driver's first register
  write the core saves `display.<driver>.guard=start` in SETTINGS.INI and
  BOOT.LOG. Once the desktop runs on the driver, a configuration (connectors,
  modes and refresh rates, and whether each head flips pages and draws the
  hardware cursor: `DP-1 2560x1440@165 fc;`) confirmed before clears the
  marker after 10 seconds; a new one (a rate never confirmed too; a
  signature saved before rates were part of it does not match, so each
  monitor is confirmed once more after the update)
  makes the GUI ask "Halcyon now uses its own driver for the ... Can you
  see this message?" (15 seconds, No is the default): Yes stores it in
  `display.<driver>.confirmed` and clears the marker, anything else gives
  the device back (the fallback above). A driver that fails after touching
  its device keeps the marker. A marker found at the next start (a hang, a
  crash, a dark screen switched off) becomes `blocked`: the adapter is not
  started ("turned off: its last start did not finish"), which BOOT.LOG,
  System Information and a balloon say, until Display Properties' **Turn on
  driver...** button or removing the key. `display_guard_checkpoint` saves
  BOOT.LOG before other risky steps (the first mode set). The fw_cfg test
  hook `opt/halcyon/displayguard` = `<driver>` guards a driver that does not
  use the guard itself (the boot tests `guard` and `guardblock` use the
  virtio GPU).
- The fw_cfg test hook `opt/halcyon/displayfail` = `<driver>:<step>`
  (`enable`, `fb`, `setmode`) makes that step fail, after the driver did
  it (`setmode`: the device is programmed, then the core says it failed),
  so the fallback is tested for real.

### Acceleration (M10e)

A driver whose device has a copy engine or blitter may offer three
asynchronous operations on its own buffers (`display_accel_ops_t`, set in
`a->accel_ops` with `a->accel_priv`); the core uses them to present and
keeps its CPU copies as the fallback for everything:

| op | does |
|----|------|
| `source(a, px, bytes)` | make the desktop surface readable by the device (mapped read-only); called lazily before the first upload from a surface, with nothing in flight |
| `upload(a, dst, x, y, w, h, src_off, src_pitch)` | queue: a rectangle from the desktop surface into a buffer |
| `copy(a, dst, dx, dy, src, sx, sy, w, h)` | queue: a rectangle between two buffers of the adapter (not overlapping) |
| `fill(a, dst, x, y, w, h, xrgb)` | queue: a solid rectangle |
| `submit(a)` | send what is queued; the fence (> 0; the last one again if nothing new), 0 on failure |
| `done(a, fence)` | polled: 1 done, 0 not yet, `E_*` the device failed (with `a->accel_why`) |
| `stop(a, why)` | acceleration ends for the session (bounded) |

The driver says which operations work with `display_accel_ready(a,
ACCEL_UPLOAD | ACCEL_COPY | ACCEL_FILL, name)` (uploads are the one that
matters), and may give up itself with `display_accel_lost(a, why)`.
Operations of one adapter must complete in the order they were queued.

How the core uses them:

- **Single-buffered heads**: a damaged rectangle is queued as an upload
  into the buffer on screen instead of being copied by the CPU; the
  commit submits; a device that needs flushes (virtio) gets its `flush`
  only once the fence is done.
- **Double-buffered heads** (page flips): the back buffer is brought up to
  date by copying from the front buffer what the front already has (every
  pixel outside the front's stale rectangles is current there), then
  uploading what neither has (the front's stale rectangles), so the
  desktop surface is read once per change, not twice; when the front is
  wholly stale (after a mode set) everything is uploaded. **The flip (and
  the driver's flush of those rectangles) is queued only when that fence
  is done**: `head_commit` checks it, `display_service` calls it again,
  the idle loop is asked to come back in a millisecond meanwhile.
- **New buffers** are filled black by the device (bounded wait before the
  mode set shows them).
- Before buffers are freed (a mode set, a head off, a failure) and before
  the desktop surface is replaced (`display_set_source`), outstanding
  work is waited for (bounded); `display_sync` (shutdown) waits too.
- **Failure**: a fence not done within `DISPLAY_ACCEL_TIMEOUT_MS` (250 ms)
  of its submission, `done` or `submit` failing, an operation refused
  (other than "no room": that rectangle is simply drawn by the CPU) ends
  acceleration for the session: `stop` is called once, "display: ...:
  acceleration off for this session: why; the CPU draws" is logged, and
  every buffer of the adapter is redrawn by the CPU. The desktop never
  goes down with the accelerator.
- A rectangle in flight may be read from the desktop surface after the
  compositor has drawn the next frame into it; the next frame's damage
  covers it again, so this can only show a rectangle one frame early.

**The acceleration guard**: a driver brings its engine up only once the
start guard is done with the adapter (`display_guard_settled`), and
saves `display.<driver>.accel.guard=start` first
(`display_accel_guard_begin`: `E_PERM` when the last boot's marker is
still there, which becomes `blocked`). The marker goes 10 seconds after
`display_accel_ready` or at a clean shutdown; a failure keeps it, so the
next start does not try again. A hang while the engine starts therefore
costs acceleration at the next start, never the driver.

**State**: `display_accel_state(a)` ("in use: copy engine 0 (class c1b5);
N uploads, N copies, N fills, N rectangles by the CPU" / "off: why") is in
System Information for every adapter; `display_accel_bench(test)` runs
NVREPORT's copy engine speed tests (fill, upload, buffer to buffer on the
primary's buffer on screen, about 200 ms, CPU time counted apart).

**Test hook**: `-fw_cfg name=opt/halcyon/displayaccel,string=<driver>`
(or `<driver>:hang`) gives an adapter without acceleration of its own a
software implementation that does the operations with the CPU, but only
when `done` is asked at least 2 ms after `submit`, so the queueing, the
fences, the flip and flush ordering and the fallback run on the real
presentation path in QEMU (`vgpu+accel`, `dual+accel`; `:hang` stops
completing after 50 fences: `vgpu+accelhang`).

## The desktop on several monitors

**One large surface.** The compositor draws into one surface in system RAM,
`g_screen`, the bounding box of the arrangement (`screen_w()` x
`screen_h()`); each monitor shows its rectangle of it. Desktop coordinates
are those of this surface: the bounding box's top left corner is (0, 0)
(a monitor left of the primary simply moves the primary right). Areas of
the bounding box no monitor shows (L-shaped arrangements) are drawn but
never seen. Window placement, the pointer and maximizing only use the
monitors' rectangles.

**Presenting** (`display_present`, `display_commit`). The window manager
composes its dirty rectangles into `g_screen` as before, then calls
`screen_present(r)` for each and `screen_commit()` at the end of the frame:

- a single-buffered head copies the part of `r` it shows into its buffer at
  once (with format conversion for 24 bpp or unusual firmware frame
  buffers) and remembers the rectangle; at the commit the driver's `flush`
  gets the rectangles (virtio: TRANSFER_TO_HOST_2D, RESOURCE_FLUSH). Only
  damage is copied, never the whole desktop.
- a double-buffered head (`HEAD_CAP_FLIP`) keeps, for each of its two
  buffers, the list of rectangles that buffer lacks (up to 8, merged). At
  the commit, when no flip is pending, the back buffer gets its stale
  rectangles copied from `g_screen`, the driver flushes them and flips; the
  flip completes at the next vertical blank (`display_flip_done`), the
  buffers swap, and the next commit brings the new back buffer up to date.
  While a flip is pending the compositor carries on drawing into
  `g_screen`; the damage accumulates and goes out with the next flip, so a
  head shows complete frames only and at most one per vertical blank.
  `display_service()` (each GUI step) polls the drivers, completes flips,
  commits again and runs hotplug; a flip that never completes (250 ms) or
  fails turns the head into a single-buffered one, on the buffer on screen
  (it first gets the rectangles it lacks; a driver that gives flips up
  itself clears `HEAD_CAP_FLIP` and the core does the same).
- `display_sync()` (shutdown, restart) commits and waits for the flips,
  since the GUI loop stops.

**Monitors in the window manager** (`wm.h`): `mon_count()`, `mon_rect(i)`,
`mon_work(i)` (the primary without the taskbar), `mon_at(x, y)`,
`mon_of(rect)`, `mon_of_window(w)`, refreshed from `display_outputs()` (the
primary first) whenever the arrangement changes.

- The **primary monitor** carries the taskbar (along its bottom), the Start
  menu, the tray's popups and balloons, the desktop icons (their grid and
  saved cells are relative to it) and the pre-release watermark.
- **New windows** open on the primary monitor (cascading in its work area);
  dialogs (`WF_CENTER`) are centred on the monitor of the active window
  (their program's). **Saved window places** (`window.<key>` in
  `SETTINGS.INI`) are put back with `layout_fit`: onto the monitor that
  shows most of the saved frame, moved inside its work area; when that
  monitor is not there any more, onto the primary one.
- **Maximize** fills the work area of the monitor the window is on; the
  window remembers that monitor (`max_mon`), and drawing and hit testing of
  a maximized window are clipped to it (its borders hang over the edges, as
  in Windows, but are not seen or clicked on the neighbouring monitor).
- **Moving windows**: a title bar dragged across monitors follows the
  pointer; it is kept between the top of the pointer's monitor and the
  bottom of its work area.
- **The pointer** crosses where monitors touch: after each movement it is
  clamped to the nearest point of the union of the monitors
  (`layout_clamp_point`), so it cannot get lost in the bounding box's
  invisible corners. An absolute pointer (USB tablet) maps over the whole
  bounding box.
- **Full screen**: the screen saver (its cover and its program's window,
  `WF_SAVER`) covers the whole bounding box, so every monitor; other
  full-screen windows cover the primary monitor.
- **Wallpaper**: every monitor gets the whole picture at its size; a
  monitor whose size did not change keeps its rendering when the
  arrangement changes (rendering is slow under emulation).
- **When the arrangement changes** (Display Properties, hotplug): windows on
  a monitor that moved move with it; windows on a monitor that went away
  move onto the primary one ("gui: "GUI Demo": its monitor went away, moved
  onto monitor 1" in the log); maximized windows fill their monitor again
  (or the primary). Dirty rectangles of the old size are dropped (they would
  reach past a smaller surface).

**The arrangement math** (`gui/layout.c`, pure functions on `rect_t`
arrays, tested on the host): monitors never overlap and each touches
another along an edge. `layout_snap` puts a dragged monitor at its own
place if that is valid, else at the nearest free place along the others'
edges; `layout_normalize` keeps the primary where it is, leaves monitors
that already touch the placed ones alone (so a valid arrangement does not
change), snaps the others in order of their distance, and moves the
bounding box's corner to (0, 0).

### The hardware cursor

When every output's head has `HEAD_CAP_CURSOR`, the window manager sends
the cursor's shape as a 64 x 64 ARGB image (the existing cursor art,
`cursor_argb`, with its shadow as 70/255 black) and its position, and draws
no software cursor: moving the mouse redraws nothing. The core shows the
cursor on the head under the hot spot and hides it on the others. When an
output without one joins (a second adapter), the software cursor takes
over; "display: hardware cursor in use on Virtual-1, Virtual-2 (64x64)" is
logged once. Screendumps from QEMU do not contain a hardware cursor.

## The drivers

### Bochs (`bochs.c`)

QEMU's std-vga (1234:1111, also `bochs-display`) and VirtualBox's VGA
(80EE:BEEF): one head, one connector `VGA-1`, modes switch at once through
the DISPI registers (I/O ports, or BAR 2 on QEMU), the EDID from BAR 2,
`virtual_sizes` with widths divisible by 8 up to the device's maximum and
VRAM. No hardware cursor, no vertical blank. The scanout buffer is the
start of VRAM, the same memory as the firmware's frame buffer. `enable`
saves the firmware's registers, `restore` writes them back.

### virtio GPU (`virtio_gpu.c`)

virtio 1.x, 2D only, one head and one connector `Virtual-<n>` per scanout
(QEMU's `max_outputs`), features VERSION_1 and EDID.

- **Buffers** are `RESOURCE_CREATE_2D` (B8G8R8X8) resources whose backing
  (`ATTACH_BACKING`, one contiguous range) is guest RAM. `set_mode` is
  `SET_SCANOUT` plus a full transfer and flush; `flush` is
  `TRANSFER_TO_HOST_2D` of the rectangle (offset `y * pitch + x * 4`) and,
  when the resource is on screen, `RESOURCE_FLUSH`.
- **Page flips**: two resources per head. The device has no vertical blank,
  so a timer at the mode's refresh rate (phase from the mode set) stands in:
  `flip` records the resource and the next "vblank" time and asks the idle
  loop to wake then; `poll` sends `SET_SCANOUT` + `RESOURCE_FLUSH` once that
  time has come and reports the flip done. `wait_vblank` waits for the next
  such time.
- **Cursor**: a 64 x 64 B8G8R8A8 resource, uploaded when the image changes;
  `UPDATE_CURSOR` and `MOVE_CURSOR` go through the cursor queue, fire and
  forget (16 commands in flight; a move is dropped if the device is that
  far behind). A move with resource 0 hides the cursor on a head.
- **Connectors**: `GET_DISPLAY_INFO` (a scanout's `enabled` bit is "a
  monitor is connected", its size the window the host shows: the preferred
  size hint) and `GET_EDID` per scanout.
- **Hotplug**: the device sets `VIRTIO_GPU_EVENT_DISPLAY` in `events_read`
  and raises the configuration-change interrupt when the host's display
  changes (a window resized, an output enabled or disabled). `poll` reads
  `events_read` on every wake-up and every 500 ms (without interrupts),
  clears it and calls `display_hotplug`. The core re-runs `detect`; a newly
  connected monitor joins on the right (or at its saved place), a
  disconnected one leaves the desktop, and a monitor whose preferred size
  changed (the QEMU window resized) follows it unless a mode was saved for
  it.
- **Commands** on the control queue are synchronous: queued (up to 16 in one
  batch, e.g. transfer + flush + set_scanout), the used ring polled; QEMU
  answers at once; after half a second without an answer the device is not
  used again. Interrupts (MSI-X with the configuration vector, or INTx
  with the ISR read to clear) only wake the GUI thread.
- **Owning the firmware frame buffer**: virtio-vga's VGA BAR holds it;
  with virtio-gpu-pci and OVMF the GOP draws into RAM, which no BAR holds,
  so the driver claims it then.
- `restore` resets the device; `panic_flush` points the scanout at the
  buffer the panic screen was drawn into and transfers it.

## Display Properties

The Settings tab (`apps/display.c`) has an XP-style arrangement picture: the
monitors scaled to fit, each showing the wallpaper, its number, the
taskbar on the primary one; monitors that are connected but not part of
the desktop are drawn hatched to the right. Click a monitor (or pick it in
the Display list, "2. QEMU Monitor on Virtio GPU (2 outputs)") to choose its
resolution from its own list (each size once, the preferred one
"(recommended)"), its refresh rate, "Use this device as the primary
monitor" or "Extend my desktop onto this monitor"; drag it to rearrange (it
snaps to an edge; dropping a detached monitor next to the others extends
onto it). Identify shows each monitor's big number on it for three seconds.
Apply on adapters that switch live applies the whole configuration and
asks "Do you want to keep these settings?", going back after 15 seconds;
the result is saved (`display.<connector>.*`). With only the firmware frame
buffer the choice is saved for the loader and a restart is offered.

**Refresh rate.** Below the resolution list, in the Screen resolution
group, a compact "Refresh rate" drop-down lists the rates of the selected
monitor's list at the chosen resolution ("60 Hertz", "120 Hertz", the
current one ticked); "Colors: Highest (32 bit)" stands beside it.
Choosing another resolution takes the monitor's preferred rate if it is
offered there, else the highest (`modes_find`); choosing the same one
again keeps the rate. Each monitor has its own (the one selected in the
arrangement). Apply goes through the same 15-second "keep these
settings?". On adapters without `rates`, or that do not switch live, the
drop-down is disabled and says "Default".

Why not XP's way: XP put the rate under Settings, Advanced, Monitor tab
("Screen refresh rate", with "Hide modes that this monitor cannot
display"). Halcyon's Display Properties has two tabs and no Advanced
button, adapter or monitor pages; an Advanced dialog with a Monitor tab
would be a new property sheet for one control, two clicks away from the
resolution it depends on. And the checkbox would have nothing to show:
the lists hold only timings the monitor lists and the driver accepts, so
it would always be checked. One drop-down where the "Color quality" line
was keeps the rate next to its resolution.

System Information lists each adapter (driver, state and why it failed or
is off, heads and their capabilities) with its connectors below it (status,
the monitor's EDID name, manufacturer, native mode, size, how many modes
and resolutions, the mode in use with its full timing, its refresh rate and
the rates offered at that resolution, frames and page flips, the hardware
cursor), and PCI display devices that have no driver. BOOT.LOG lists, per
connector, the sizes with several rates ("display:   2560x1440 at 60, 120,
144, 165 Hz"), each mode set with its timing, and each monitor's rate
("display: monitor 1: DP-1 (LG ULTRAGEAR) 2560x1440@165 at 0,0 on ...",
for adapters with `rates`).

## Writing a native display driver

This is the checklist the NVIDIA modesetting driver (Tesla to Pascal, the
EVO display engine) follows; the virtio driver is the worked example.

1. **Probe** (called from `display_init`; add a weak declaration and a call
   next to `virtio_gpu_probe` in `display.c`, as `nv_display_probe` is):
   find the PCI device, map its BARs, read what identifies it, but **do not
   touch the display**: the firmware's frame buffer is on screen and the
   boot splash uses it. `display_register("nv", "NVIDIA GeForce GTX 1070",
   &ops, priv, pci)`, set `live = 1`, `max_w`/`max_h` (the head's maximum
   surface), `max_clock_khz` (the lowest of the output's limits, or 0 and
   check per connector in `mode_valid`), `vram` (what scanout buffers may
   use). One `display_add_head(a, HEAD_CAP_CURSOR | HEAD_CAP_FLIP |
   HEAD_CAP_VBLANK, 64)` per head; one `display_add_connector` per DCB
   connector, named by type and number (`DP-1`, `HDMI-1`, `DVI-I-1`,
   `VGA-1`), with `possible_heads` from the DCB's head masks. Set
   `pci->driver`. Set `rates = 1` when `set_mode` programs `m->t` as it
   is (every refresh rate the monitor lists is then offered and checked
   with `mode_valid` one by one).
2. **enable**: take the display engine over (on EVO: the core channel's
   push buffer, the notifier, the semaphores), without changing what is on
   screen if possible. Remember what is needed to give it back.
3. **detect**: for each connector, its status (hotplug detect pin, DP sink
   count, load detection on analog outputs; `CONN_MAYBE` when it cannot
   tell) and its EDID (I2C/DDC or DP AUX, base block plus extensions; up to
   512 bytes are kept), then `display_connector_update(c, status, edid,
   len, 0, 0)`.
4. **mode_valid**: refuse what the output cannot carry: TMDS clock limits
   (165 MHz single link, 330 MHz dual link DVI, 340/600 MHz HDMI), DP link
   bandwidth (lanes x rate, at 24 bpp), the head's line buffer width,
   interlace. The core already applied `max_*` and `vram`.
5. **fb_alloc / fb_free**: VRAM for a `w x h` XRGB8888 surface (pitch
   aligned as the head needs, e.g. 256 bytes), mapped write-combining
   through the frame buffer BAR (`px`, `phys` = the BAR address; the core
   turns write-combining on for buffers inside a display BAR), `handle` =
   the VRAM offset or the surface's DMA object.
6. **set_mode(h, c, m, fb)**: route connector `c` to head `h` (an SOR / DAC
   / PIOR the connector can use), program the head with `m->t` (pixel
   clock, active / sync start / sync end / total, polarities from
   `m->flags`), point the base channel at `fb` (offset, pitch, format) and
   update. `m == NULL`: switch the head and its output off. Return an
   `E_*` error instead of leaving a half-programmed head: the core goes
   back to the previous configuration or to the firmware frame buffer.
7. **flush**: not needed (the head reads VRAM); leave it NULL.
8. **flip(h, fb)** and the vertical blank: queue the new surface on the
   head's base channel with "flip at vblank" and a notifier / semaphore
   release; return at once. When the notifier says it happened — from
   `poll`, which also runs when the head's vblank interrupt wakes the GUI
   thread — call `display_flip_done(h)`. Interrupts only wake: `poll` must
   find completed flips without them. Ask the idle loop to wake for the
   expected completion with `idle_wake_at()` if the interrupt might not
   come. **wait_vblank**: wait for the head's next vblank counter change,
   bounded.
9. **cursor_image / cursor_move**: the head's cursor channel: a 64 x 64
   ARGB surface in VRAM (two, to update without tearing), and the
   position (the hot spot's, head coordinates, may be negative near the left
   or top edge: subtract the hot spot and clip as the hardware needs).
10. **poll**: completions, hotplug (HPD interrupts or polled status, every
    second or so): call `display_hotplug(a)` when a connector may have
    changed; the core calls `detect` and reconfigures.
11. **Acceleration** (optional, "Acceleration" above): offer
    `display_accel_ops_t` once the engine runs (after
    `display_guard_settled`, with `display_accel_guard_begin` before the
    first register write of its bring-up), `display_accel_ready` with what
    works. Keep every wait bounded; on any doubt return an error and let
    the core fall back.
12. **restore**: put the firmware's mode back (on NVIDIA: tear down the
    channels and restore the VGA/EFI framebuffer head state saved in
    `enable`) so that the firmware frame buffer is visible again.
13. **panic_flush**: nothing for VRAM scanout; the core points
    `g_bootinfo.fb_*` at the primary head's visible buffer before drawing
    the panic screen.
14. **Test the fallback**: `-fw_cfg name=opt/halcyon/displayfail,string=nv:setmode`
    (or `enable`, `fb`) must leave the desktop on the firmware frame buffer
    with the reason in `BOOT.LOG`; `display.disable=nv` in `SETTINGS.INI`
    and F8's "no display driver" must keep the driver off.

The core does the rest: mode lists from the EDIDs, the arrangement and its
settings, buffer copies, page flip scheduling, the cursor across heads,
hotplug reconfiguration, Display Properties and System Information.

## Tests

- `tests/host/modetest.c`: CVT reduced blanking against published timings
  (1920 x 1200, 2560 x 1600, 1440 x 900, 1280 x 720), DMT entries, an EDID
  with every kind of timing (detailed with sync polarities, established,
  standard from DMT and from CVT, a CTA extension with VICs, a detailed
  timing and an interlaced one), mode lists under pixel clock, memory
  (two buffers) and size limits and a driver's own check, virtual sizes
  with a size hint, the default mode for a 4K panel, broken EDIDs; refresh
  rates from an EDID like the LG UltraGear's (120 Hz preferred in the base
  block, 60 and 144 Hz CEA DTDs, 165 Hz in DisplayID type I and type VII,
  exact clocks and totals), one entry per rate, duplicates (59.94 / 60 Hz,
  the EDID's 60 Hz against DMT's), the preferred mark, `modes_find`'s rule
  (120 Hz at 1920 x 1080, the highest at 1024 x 768), the GTX 1070's
  DisplayPort check for five link widths (a preferred timing too fast for
  the link leaves 2560 x 1440 at 60 Hz recommended), a 600 MHz clock
  limit, saved rates restored or replaced (`modes_saved`), the QEMU test
  EDID (`--write-edid qemu`); and 200,000 random EDIDs with CEA and
  DisplayID blocks and rates on and off (no crash, every listed mode
  within the limits, sorted, no duplicates, a saved mode comes back).
- `tests/host/evotest.c` (NVIDIA, see DESIGN-M10-NVIDIA.md): the GP104's
  PLL table reconstructed from the coefficients NVREPORT logged for 497.75
  MHz (N 54 fN 19c7 M 1 P 3), 241.50 / 604.25 / 679.30 MHz exact inside
  the VCO range; 2560 x 1440 at 165, 144, 60 and 120 Hz set one after the
  other on a lit head of the simulated GP104 (the link trained again at 4
  x 8.1 GHz, the PLL's coefficients), a DCB capped at 5.4 GHz (165 Hz at
  4 x 5.4), a 2-lane sink (120 Hz at most).
- `tests/host/layouttest.c`: adjacency, the pointer between monitors of
  different heights, window places (on monitor 2, monitor 2 gone, larger
  than a monitor), snapping, normalizing saved arrangements (all at the
  origin, a monitor left of the primary, a gap), and 20,000 random
  arrangements: no overlaps, all connected, origin at (0, 0), the primary in
  place, idempotent, the pointer always on a monitor, a dragged monitor
  snapped to an edge.
- `tests/host/presenttest.c` (M10e): `display.c` itself, compiled with
  `modes.c`, `edid.c`, `layout.c` and stubs, against a fake adapter whose
  copy engine completes on simulated time; each scenario in a fork of its
  own. The **ordering rule** (a flip queued, a rectangle flushed, a buffer
  freed or the desktop surface replaced while an operation on it is in
  flight is a violation the fake device counts), and **pixels**: after
  hundreds of frames of random rectangles on two double-buffered heads and
  on a single-buffered head of a device that needs flushes, the buffers on
  screen equal the desktop; the same with an engine that stops completing,
  reports an error, refuses operations or has no room (the CPU takes over,
  `stop` is called once, the screen still equals the desktop); a mode
  change and a new desktop surface with work in flight; the speed test;
  the acceleration guard (saved, cleared after 10 s, `blocked`, kept after
  a failure, cleared at shutdown); the software test hook, also hanging.
  A core that flipped without waiting is caught (tried: hundreds of
  violations).
- Boot matrix (`tools/boottest.py`):
  - `vgpu` (`-vga virtio`, BIOS): the driver replaces the firmware frame
    buffer, hardware cursor, page flips, a screendump with the desktop and
    taskbar;
  - `dual` (`virtio-vga,max_outputs=2`, UEFI): output 2 enabled through a
    VNC server bound to head 1 (`tools/vnc_ctl.py`, RFB SetDesktopSize: QEMU
    8.2 has no monitor command for it), monitor 2 switched to 800 x 600 by
    the setmode hook, per-head screendumps (QMP `screendump` with `device`
    and `head`) of the right sizes with the taskbar on the primary only, the
    hardware cursor on both heads, flips on both, GUIDEMO's window dragged
    onto monitor 2 and maximized there (its size and place from its own
    log), hotplug (resize to 1280 x 1024 with the interrupt, disconnect: the
    window moves to monitor 1, reconnect), the screen saver covering both;
  - `arrange` (BIOS): a saved arrangement (monitor 2 primary at 800 x 600,
    monitor 1 right of it, 100 pixels lower) restored, the taskbar on
    monitor 2, a window saved on a monitor that is not there opened inside
    the primary;
  - `multi` (BIOS): std-vga (Bochs) and virtio-gpu-pci together: two
    adapters, the taskbar on the Bochs screen, the software cursor;
  - `gpufail` (BIOS): the virtio mode set fails after programming the
    device: reset, the firmware frame buffer in use, a screendump shows the
    desktop;
  - `gpuoff` (UEFI): `display.disable=virtio-gpu`;
  - `rates` (BIOS): the virtio GPU with the test EDID (fw_cfg
    `opt/halcyon/vgpuedid`, `modetest --write-edid qemu`: 1280 x 800 at 60
    Hz preferred, 75 and 120 Hz CEA DTDs, 144 Hz DisplayID; 1024 x 768 at
    60, 70 and 75; 1152 x 864 at 75 only): the rates logged, the loader's
    1024 x 768 at the preferred 60 Hz; the Display Properties test hook
    (fw_cfg `opt/halcyon/displaytest` = `1152x864,1280x800@75`) chooses
    1152 x 864 (75 Hz, the only one), 1280 x 800 (60 Hz, the preferred)
    and 75 Hz through the drop-down's menu handler, a screendump of the
    dialog, Apply, "keep these settings?" answered Yes by the hook,
    `display.virtual-1.hz=75` in SETTINGS.INI;
  - `ratesave` (UEFI): `display.virtual-1.hz=120` saved: restored with its
    timing; with the start guard (`displayguard` hook) and
    `display.virtio-gpu.confirmed=Virtual-1 1280x800@60 fc;`, the new rate
    is a new configuration that asks again;
  - `ratefall` (BIOS): 85 Hz saved, not offered: 60 Hz (the monitor's
    preferred rate) and the reason in BOOT.LOG (read from the image);
  - `vgpu+accel`, `dual+accel`, `vgpu+accelhang` (M10e): the software
    acceleration test hook on the virtio GPU; with Starfield providing
    frames, 100 updates through acceleration (uploads and buffer-to-buffer
    copies) with the page flips and screendumps of `vgpu` / `dual` still
    right; `accelhang`: acceleration given up after 250 ms and a
    screendump of the desktop drawn by the CPU afterwards;
  - the existing `res`, `res+fwvideo`, `switch` and every other variant
    keep using the Bochs adapter or the firmware frame buffer (BIOS VBE and
    UEFI GOP).

## Status

Done: everything above. Last run: the default boot matrix 47/47 (with the
six display variants `vgpu`, `dual`, `arrange`, `multi`, `gpufail`,
`gpuoff`), `virtio+noirq` (flips and the cursor polled), `tools/ci.sh quick`,
and the host tests (modetest 42 checks + 200,000 random EDIDs, layouttest
24 checks + 20,000 random arrangements, displaytest 19). With M10e's
acceleration: presenttest 58 checks (each a whole screen compared pixel by
pixel), and the display variants above plus `vgpu+accel`, `dual+accel`,
`vgpu+accelhang`, `guard`, `guardblock`, `nvfake` (BIOS and UEFI),
`nvfake+nvoff`, `nvfake+nvset`, `res`, `switch`, `res+fwvideo`, `help`.

Refresh rates: last run 22/22 boot variants (`vgpu`, `dual`, `arrange`, `multi`,
`gpufail`, `gpuoff`, `nvfake` BIOS and UEFI, `nvfake+nvoff`, `nvfake+nvset`,
`guard`, `guardblock`, `res`, `switch`, `res+fwvideo`, `help`, and the new
`rates`, `ratesave`, `ratefall` on BIOS and UEFI), `tools/ci.sh quick`,
modetest 97 checks + 200,000 random EDIDs, layouttest 24, displaytest 19,
evotest 45,580. The drop-down was looked at in screendumps (open with the
rates at 1024 x 768, and disabled "Default" on a plain virtio GPU). Not run
on the GTX 1070 yet.

The NVIDIA driver (10b, 10d, 10e) implements the whole interface above:
cursor, page flips, vertical blank, several heads and acceleration through
a copy engine (see DESIGN-M10-NVIDIA.md; 10d and 10e have not run on a
card yet). The acceleration interface is generic: an Intel or AMD driver
would offer the same operations through its blitter or DMA engine. Remaining, besides
the list below: `dual+noirq` (hotplug found by
polling: the test knows how to check it, it was not run); screenshots for
the documentation were looked at but are not in the repository.

## Not done

- **Moves and scrolls as device copies**: the window manager composes in
  RAM and reports only damage, so a window move or a scroll is uploaded as
  damage, not copied within the buffer; `copy` is in the acceleration
  interface for when the compositor can express them.
- **virtio-gpu-pci under UEFI**: OVMF's GOP on a virtio GPU without VGA
  offers only Blt modes (no linear frame buffer), so the UEFI loader stops
  ("graphics adapter only offers PixelBltOnly modes"). The loader could
  start without a frame buffer and leave the screen to the native driver;
  not done (it would leave a black screen whenever that driver is off).
  `virtio-vga` (and `-vga virtio`) work under UEFI and BIOS.
- **Absolute pointers on several monitors**: a USB tablet's coordinates map
  over the whole bounding box. QEMU sends absolute coordinates relative to
  the window of the head the pointer is in, so with several QEMU windows
  the pointer only lines up on the first; a relative mouse (PS/2) works
  everywhere. (QEMU can bind one tablet per head; the input layer does not
  tell devices apart yet.)
- **Refresh rates** only where the driver sets the timing (NVIDIA); the
  firmware frame buffer keeps the loader's mode, and Bochs and the virtio
  GPU have no real rate (one mode per size, "Default"). Not read from the
  EDID: DisplayID 2.0's formula-based timings (types VIII to X), CTA VICs
  outside the small progressive table (e.g. 1080p at 100 / 120 Hz, VICs 63
  and 64), CTA-861's 4:2:0 and HDMI forum blocks; no variable refresh rate
  (G-Sync, Adaptive-Sync), no deep color (a rate that needs fewer bits per
  pixel is not offered), no DSC. No "Hide modes that this monitor cannot
  display" (nothing is listed that the monitor does not list). Interlaced
  timings stay out of the lists.
- No rotation, no mirroring ("clone" mode), no color depths but 32 bit, no
  gamma tables, no DPI scaling.
- **Bochs page flips**: the DISPI interface could flip with its Y offset,
  but it has no vertical blank to pace them; it stays single buffered.
- **Hardware cursor across a seam**: the cursor is shown on the head under
  its hot spot only; near the edge between two monitors the part of the
  image on the other monitor is not shown (real drivers show it on both).
- The arrangement picture's scale changes with the arrangement (XP keeps
  it fixed while the dialog is open).
- VBE/GOP firmware frame buffers of a second card are used only when the
  firmware set them up (one GOP / VBE device).
