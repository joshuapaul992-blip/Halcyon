# Milestone 8 design: screen savers, and settings that survive a reboot

Two parts of M8 (see ROADMAP.md): screen savers with Display Properties'
Screen Saver tab, and every choice the user makes kept in
`C:\HALCYON\SETTINGS.INI`, written so that a power cut cannot lose it.

## SETTINGS.INI

`kernel/core/settings.c` keeps a table in memory (up to 1024 keys of 63
bytes, values of 599 bytes, one line each) and writes all of it on a save.
The file is plain `key=value` lines (UTF-8, `#` / `;` comments, a BOM and
CR LF accepted), sorted by key with the `display.*` keys first: the BIOS and
UEFI loaders read only the first 8 KiB to choose the video mode. The last
line is an end marker, `# end of settings (N)`.

### An atomic save

`settings_save()` builds the whole text first (nothing blocks while the table
is read), compares it with what the last save wrote (an unchanged table is
not written again), then, under a mutex:

1. deletes a leftover `SETTINGS.TMP`, writes the new text to it, flushes the
   volume (`vfs_sync_all`) and reads the file back to compare;
2. deletes `SETTINGS.BAK`, renames `SETTINGS.INI` to `SETTINGS.BAK`;
3. renames `SETTINGS.TMP` to `SETTINGS.INI` and flushes again.

A failing step leaves `SETTINGS.INI` as it was (after a failed final rename
the backup is renamed back). The table in memory always keeps the new
values, and a later save tries again.

### Recovery at boot

`settings_load()` reads `SETTINGS.INI`, `SETTINGS.TMP` and `SETTINGS.BAK` and
takes the first *complete* one (it ends with the marker) in that order; if
none is complete, whatever parses of a cut-short INI (then BAK). A value cut
in the middle can only be the last line read. If the table did not come
from a complete `SETTINGS.INI`, it is saved again at once, which repairs the
INI. A leftover `SETTINGS.TMP` (a save interrupted after step 1) is deleted,
unless it holds exactly the INI's bytes: then it may be the same clusters
under a second name (see below), so it is left alone forever and saves use
`SETTINGS.TM2` instead.

Hand edits work as before, as long as the last line stays: a file without
the marker counts as cut short, and a complete backup would win over it.

### The rename paths of the file systems

The boot volume is FAT32 (the loaders read FAT only), but the code is the
same for any volume, so all three file systems were checked, by cutting the
power after every single sector a save writes (`tests/host/settingsfs.c`):

- **FAT**: a rename used to add the new directory entry and then delete the
  old one; when the two were in different sectors, a cut in between left two
  names on the same clusters (deleting either would free the other's data).
  A rename within one directory from a short name to a short name now
  rewrites the entry in place: one 32-byte change in one sector, so the
  result is the old name or the new one. `SETTINGS.TMP` -> `.INI` / `.BAK`
  are such renames. Result: 26 sectors per save, every cut boots with the
  old or the new table, `fsck.fat -n` finds nothing.
- **exFAT**: renames were already in place when the new entry set is not
  larger, but a set could straddle a sector boundary, and a cut then left a
  set with a wrong checksum (`fsck.exfat` reported it; the file was gone).
  `dir_add` now keeps a set that fits in one sector within one sector
  (entries skipped past the end-of-directory marker are marked unused). The
  same test: 26 sectors per save, old or new table every time, no
  `fsck.exfat` complaint.
- **NTFS**: Halcyon writes NTFS without its journal, and a save rewrites
  about 4200 sectors (MFT records, indexes, the bitmap, `$LogFile`'s restart
  area). Sampled every 23 sectors: the old table 181 times, the new one once,
  neither 3 times; afterwards the volume is not clean and Halcyon mounts it
  read-only (the rules of DESIGN-M7.5), `ntfsfix -n` finds no errors.
  `SETTINGS.INI` never lives on NTFS, so this is reported, not required.

### Coalesced saves

Changes that come in streams (a window being dragged, a slider, a program
setting several keys) call `settings_save_later()`: the file is written
1.5 s after the last change, at most 8 s after the first (`settings_tick()`
from the GUI loop). `settings_flush()` writes what is pending before a
restart or shutdown. A save that fails is retried 30 s later.

### Programs

`SYS_SETTING_GET` (170, `setting_get(key, buf, n)`) reads any key: the
value's length, the value NUL-terminated in buf (cut to n - 1 bytes), or
`E_NOENT`. `SYS_SETTING_SET` (171, `setting_set(key, value)`, `NULL`
removes) writes only keys named after the program: `paint.*` for
`PAINT.EXE`, `marquee.*` for `MARQUEE.SCR` (the file name without its
extension), so no program can change another's settings or a system one
(`ntfs.write.*` is a permission, `display.*` what the loaders use).
Otherwise `E_PERM`; keys of letters, digits, `.`, `-`, `_`; values of one
line up to 599 bytes (`E_INVAL`); 64 keys per program and never the last
quarter of the table (`E_NOSPC`). The write is coalesced as above, so a
program can set many keys cheaply and end at once. `setting_get_int` /
`setting_set_int` are in `userland/lib/shell.c`. Explorer keeps its own call
for `explorer.*` (189).

### What is kept

| Keys | What | Where |
|------|------|-------|
| `window.<program>` = `x,y,w,h,maximized` | the place, size and maximized state of a program's main window (h without the menu bar) and of the Command Prompt (`cmd`), System Information (`sysinfo`), Control Panel (`control`), Display Properties (`display`), Mouse Properties (`mouse`) | `wm_place_restore()` in gui/wm.c: restored clamped to the screen (resizable windows only get their size back; a second window of the same program cascades), recorded on every move, resize, maximize and minimize |
| `desktop.icon.<name>` = `column,row` | desktop icons dragged to another cell of the 84-pixel grid (dropped on the nearest free one); "Arrange Icons" in the desktop's menu removes them | gui/desktop.c |
| `mouse.speed` (1..20), `mouse.doubleclick` (ms), `mouse.swap` | Control Panel, Mouse... | apps/mouse.c, gui/wm.c |
| `screensaver.file`, `screensaver.wait` (minutes) | the screen saver (`""` = none; default `LOGO.SCR`, 10) | apps/display.c, gui/saver.c |
| `dialog.<program>.folder` | where a program's last Open / Save dialog ended; used when it asks for no folder or for My Documents | gui/usergui.c |
| `paint.fg`, `paint.bg`, `paint.palette`, `paint.tool`, `paint.options` | Paint's colours, its edited palette (only if changed), the tool and each tool's option | userland/programs/paint.c |
| `marquee.text`, `.font`, `.color`, `.size`, `.speed` | Scrolling Text | userland/savers/marquee.c |
| `sonar.level`, `sonar.best0..2` | Sonar's level and best times | userland/programs/sonar.c |

Earlier milestones already kept the theme and wallpaper, the display mode,
keyboard layouts, the network configuration, the time zone, sound volumes
and schemes and NTFS write permissions. Notepad (`notepad.*`), Help
(`help.*`) and Explorer (`explorer.*`) keep theirs (the other M8 parts). The
taskbar has no options to keep.

## Screen savers

### Running one

`kernel/gui/saver.c`. The window manager hands every input event to
`saver_input()` before anything else. A key, a button, the wheel, or the
pointer moving more than 6 pixels from where it last counted is input;
smaller movements are jitter. After `screensaver.wait` minutes without
input `saver_start()`:

- closes menus, puts up a black full-screen cover window (it also is the
  blank screen if the program cannot run) and hides the pointer;
- starts `C:\PROGRAMS\<screensaver.file>` with `/s`; the program creates a
  `WIN_FULLSCREEN` window, which the window manager marks `WF_SAVER` because
  it belongs to the running saver: above every other window, topmost ones
  included, never active, never hit by the mouse.

While it runs, `saver_input()` swallows every event. The first key press,
button press, wheel turn or real movement (motion is ignored for the first
0.8 s, so the Preview button's own click and a nudge do not end it at once)
calls `saver_stop()`: the saver's windows are destroyed at once (the program
is told by `proc_kill` and ends at its next system call), the pointer
comes back and the desktop is redrawn. That event, the release of the key
or buttons involved, and nothing else, is swallowed, so nothing reaches the
window underneath. A saver whose program ends by itself leaves the black
cover until input.

It does not start while a program shows a full-screen window of its own
(`WIN_FULLSCREEN` but not the saver: a video), or while a program has
called `keep_awake(1)` (`SYS_KEEP_AWAKE`, 176; per process, ends with it;
Media Player calls it while it plays). The idle time restarts when that
ends. Double-clicking a `.SCR` file in Explorer runs it like Preview.

### The programs

A screen saver is an ordinary static program named `NAME.SCR` in
`C:\PROGRAMS` (`userland/savers/*.c`, built by the Makefile's `%.scr` rule
and copied by `tools/mkimage.sh`), started as on Windows:

| Arguments | |
|-----------|---|
| `/s` | full screen until input |
| `/p <handle>` (or `/p:<handle>`) | draw in Display Properties' preview box |
| `/c`, or none | its settings dialog ("no options" if it has none) |

Its display name is an ELF note (owner `Halcyon`, type 1, the UTF-8 name)
that `SAVER_NAME("Starfield")` puts in a `.note.halcyon` section; the
linker gives it a `PT_NOTE` segment, which `saver_list()` reads. A file
without one is listed by its file name.

`userland/savers/saver.h` is the common part: argument parsing, the window,
the canvas mapped into the program (`win_canvas`), a window timer for the
frames (`SAVER_MS`, 33 ms by default: no CPU between frames), time-based
motion (dt in ms), small drawing helpers, and the end conditions when the
saver was started by hand (any key, click or 6-pixel move ends it). A saver
defines `saver_start`, `saver_frame` and `saver_config`, and draws straight
into the pixels.

| | |
|---|---|
| **Blank** (`BLANK.SCR`) | black; draws once |
| **Starfield** (`STARS.SCR`) | stars in 3D flying towards the viewer, near ones bigger with a short streak; 25 frames a second; only the pixels of the last frame are cleared |
| **Drifting Lines** (`LINES.SCR`) | two four-cornered ribbons bouncing off the edges with fading 14-step trails, colours sliding round the colour wheel half a wheel apart; erases the old trail by drawing it in black |
| **Halcyon Logo** (`LOGO.SCR`) | the Start button's badge, drawn from its outline (the shapes of `tools/mkicons.py`) at a quarter of the screen with 4 x 4 samples per pixel, bouncing, the badge taking the next of six colours at each edge, "Halcyon" under it |
| **Scrolling Text** (`MARQUEE.SCR`) | text gliding right to left at a new height each time; its dialog sets the text, font (5), size (1x-4x, scaled bilinearly from the system font's coverage), colour (white, four gradients, a moving rainbow) and speed, with a strip showing the result |

Frames are cheap for the window manager: `SYS_WIN_INVALIDATE_RECT` (177,
`win_invalidate_rect`) says which part of a canvas changed (`sv_dirty()`:
the logo's old and new place, the text's, the lines' bounding box), a
full-screen window is drawn straight from the program's canvas (no copy into
a client surface, no background fill), and nothing under a full-screen
window that covers a dirty rectangle is composed (desktop, taskbar, other
windows; their clients are still painted if they asked, so the GUI loop
can rest). Host CPU used by QEMU (TCG, no acceleration, 1280 x 800):
Scrolling Text 10%, the logo 22%, Starfield (the whole screen every frame,
25 per second) 54%, the lines (their bounding box, often most of the
screen) 58%; Blank and the idle desktop 0%. On real hardware a full frame
is a few 4 MB copies.

### Previews

Display Properties asks `saver_preview_open(window, box)` for a handle,
starts the saver with `/p <handle>` and registers the pid. The program calls
`win_create_preview(title, handle)` (`WIN_PREVIEW`, the handle as
`win_create`'s fifth argument): the kernel accepts it only from that pid,
gives the canvas the box's size and makes a `WF_EMBED` window owned by
Display Properties, which the window manager draws inside its owner's
client area (clipped to it, moving with it, never active, clicks going to
the owner). Closing the box (another saver, the other tab, the window
closing) destroys the embedded window and ends the program. After a `/c`
dialog ends, the preview restarts with the new settings.

### Display Properties

`kernel/apps/display.c` has two tabs, **Screen Saver** and **Settings** (the
resolution page as before), drawn by the dialog (tabs in the XP manner,
Ctrl+Tab switches). The Screen Saver tab: the monitor with the running
preview (or the desktop for "(None)"), a drop-down list of the installed
savers and "(None)" (a popup menu under a field), **Settings** (`/c`),
**Preview** (full screen until input), **Wait: N minutes** (a text box with
spin arrows, Up/Down keys), and an explanation. OK and Apply save the
saver and the resolution; Apply is enabled while either differs from what
is saved.

### Test hook and tests

QEMU fw_cfg `opt/halcyon/screensaver` = `SECONDS[:FILE.SCR]` sets the wait
in seconds (and the saver) for this boot without saving it; `display` or
`mouse` open Display Properties on the Screen Saver tab or Mouse Properties
(for screenshots). The kernel logs `screensaver: started`, `... is showing
(WxH)` when the program's window is up, and `screensaver: stopped (key |
click | wheel | mouse moved)`.

`tools/boottest.py` variant `saver` (in the full matrix; xhci under BIOS and
UEFI pass): `GUIDEMO.EXE` runs and logs the input it gets; Starfield starts
after 5 s; a screendump must be at least 90% dark with stars; a click on
GUIDEMO's window ends the saver and GUIDEMO must log neither the press nor
the release, a screendump must show the desktop again, the next click must
reach GUIDEMO; the saver starts again and a key ends it, then a mouse
movement. `SELFTEST.EXE` (variant `user`) checks the settings calls (own
keys, another program's and a permission key refused, bad keys and values,
truncation, removal, bad pointers) and `keep_awake`.

Host tests (`make -C tests/host settings`, run by `tests/host/run.sh`):
`settingstest.c` (2573 checks: parsing, round trips of random tables, the
file's layout, an error injected at every VFS step and a power cut at every
step and in the middle of the write, a cut during the repair at boot, INI
cut at every length with and without a backup, leftovers, the user
namespace and limits, coalescing) and `settingsfs.c` (the cuts on real
images through the kernel's drivers, above).

## Not done

- Monitor power management (no DPMS), password-protected savers, savers
  with sound, the 3D saver of M11.
- The screen saver starts only from the GUI loop's 1-second shell tick, so
  it may start up to a second late.
- Window places are kept per program, not per document or per window of a
  program with several (they share one key and cascade).
- Desktop icons snap to the grid; there is no free placement and no
  "Auto Arrange".
- Kernel applications other than those listed (Calculator's built-in
  fallback, Network Connections, Sounds, Keyboard) open where the window
  manager puts new windows.
- A stale `SETTINGS.TMP` left after a FAT rename that a power cut split
  (before this milestone's in-place rename) is never deleted, by design.
