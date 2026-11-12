# Milestone 8 design: Explorer's selection, drag and drop, the Recycle Bin

This part of milestone 8 makes Explorer a file manager that works on many
items at once, lets files be dragged between windows of any program and the
desktop, and keeps deleted files in a Recycle Bin until they are restored or
the bin is emptied.

Code: `userland/programs/explorer.c` (EXPLORER.EXE), `kernel/gui/dnd.c` /
`dnd.h` (drag and drop in the window manager), `kernel/apps/recycle.c` /
`recycle.h` (the Recycle Bin), `kernel/apps/shellsys.c` (their system calls),
`kernel/apps/fileops.c` (lists of items, emptying the bins, for kernel drop
targets), `kernel/gui/desktop.c` (the Recycle Bin icon, the desktop as a drop
target), `userland/lib/dnd.c` (a drop-target helper for programs that open
files), `kernel/apps/recycletest.c` (the boot test).

## Multiple selection

Every item has a selected flag; besides it Explorer keeps the *focus* (the
item the keyboard is on, drawn with a dotted rectangle) and the *anchor*
(where Shift ranges start), as Windows does.

| Input | Effect |
|-------|--------|
| click | select only that item (on an already selected item of several: at the release, so the group can be dragged) |
| Ctrl+click | toggle it (switching one off waits for the release, so a Ctrl+drag of the group copies it) |
| Shift+click | the range from the anchor; Ctrl+Shift+click adds the range to the selection |
| press on the background, drag | a rubber band (translucent, drawn straight into the window's mapped pixels): it selects what it touches; with Ctrl it toggles against the earlier selection, with Shift it adds; the view scrolls when the band reaches its edge. In every view: icons, details, My Computer's tiles, the Recycle Bin |
| arrows, Home, End, Page Up / Down | move the focus and select only it |
| Shift+arrows | select from the anchor to the focus (Ctrl+Shift: added) |
| Ctrl+arrows | move only the focus; Ctrl+Space toggles the focused item, Space selects it |
| Ctrl+A, Edit > Invert Selection | everything / the opposite |

Commands work on the selection: Open (every file; a folder opens when it is
the only one), Cut / Copy / Paste (the shared Explorer clipboard now holds a
list of paths: `halcyon.explorer.clip2`, up to 60 KB), Delete, Rename (only
with one item: disabled otherwise), Properties (one item as before; several:
how many, where, their total size with everything inside, files and
folders), the context menu (the right button selects the item under it
unless it is already part of the selection, and opens at the release). The
status bar says "N objects selected" with the selected files' total size,
the task pane's tasks switch to plural ("Copy these items") and its Details
box shows "N items selected", the total file size and how many files and
folders. The desktop has no file icons, so it has no multiple selection.

## Drag and drop

### Model

The window manager carries every drag; programs never talk to each other.
It is OLE drag and drop in spirit, without the blocking `DoDragDrop`:

1. **Source.** A program sees a press on an item and a move of more than 4
   pixels with the button held, and calls `dnd_begin(win, paths, len, count,
   effects, icon)` (`SYS_DND_BEGIN`): NUL-separated absolute paths, the effects
   it allows (`DND_COPY | DND_MOVE`; copy only from read-only drives) and the
   icon the pointer carries. The call returns at once with the drag's serial
   number. The kernel copies the paths (at most 256 KiB) and keeps them until
   the next drag starts, takes the mouse away from the window (no more mouse
   events for that press) and closes open menus. It fails with `E_AGAIN` if no
   button is held any more and `E_BUSY` during another drag.
2. **Feedback.** While the button is held, `gui/wm.c` hands the pointer, the
   buttons and the keys to `dnd.c`. It draws the first item's 32x32 icon half
   transparent next to the pointer, a blue badge with the number of items
   when there are several, and the effect: a "+" box for a copy, nothing for
   a move, a red no-entry sign when nothing can be dropped there.
3. **Targets.** The target is the window under the pointer if it has an
   `on_dnd` handler and the pointer is in its client area (title bars,
   frames, the taskbar, windows with a modal dialog and menus take nothing),
   or the desktop. It gets `DND_ENTER` when the pointer arrives, `DND_OVER`
   on every move and whenever Ctrl / Shift / Alt change, and `DND_LEAVE` when
   the pointer goes, each with the position (client coordinates; the desktop:
   screen), the modifier keys, the button that drags, the number of items and
   the effects the source allows. The target answers with the effect a drop
   there would have now and every effect it could have (for the right-drag
   menu). Kernel targets answer by returning `DND_ANSWER(effect, possible)`;
   programs answer with `dnd_accept(win, serial, effect, possible)`
   (`SYS_DND_ACCEPT`) later, and until the first answer for this window
   arrives the drag shows "none". The window manager keeps the last answer for
   the current target only (answers for an old serial or another window are
   ignored), and limits it to the effects the source allows.
4. **Drop.** Releasing the left button over a target that answered an effect
   sends `DND_DROP` with that effect; the target fetches the paths with
   `dnd_data(serial, buf, size)` (`SYS_DND_DATA`) and does the work in its own
   time (questions, a progress window). Releasing the right button first
   shows a menu at the pointer: Copy Here, Move Here (each enabled when the
   target could do it and the source allows it; the target's own choice in
   bold), Cancel; the choice is dropped, dismissing the menu cancels. Esc or
   pressing another button cancels. The source gets `DND_END` with the
   effect of the drop (0 = canceled).
5. **Resting on a task button** for 0.8 s during a drag brings its window up,
   so a file can be dropped into a minimized or covered window.

Because nothing blocks, a window can be its own target (a file dragged onto
a folder in the same Explorer), and a program that is busy only answers late.
A program that ends during a drag is simply no longer a source or target.
The target does the move itself (an "optimized move"), as Windows Explorer
does, so the source only refreshes: Explorer watches the paths it gave away
for two minutes after a `DND_END` with a move and reloads when they are
gone.

### Programs

`EVT_DRAG` (16) carries the drag events to a program's window queue
(`DND_OVER` events are coalesced like mouse moves):

| field | meaning |
|-------|---------|
| `button` | `DND_ENTER`, `DND_OVER`, `DND_LEAVE`, `DND_DROP`, `DND_END` |
| `x`, `y` | the pointer, client coordinates (below a menu bar) |
| `id` | the drag's serial number |
| `dz` | how many paths |
| `vk` | the effects the source allows |
| `mods` | the modifier keys |
| `buttons` | the button that drags (`MOUSE_RIGHT`: the effect comes from the menu) |
| `pressed` | `DND_DROP`: the effect to perform; `DND_END`: the effect of the drop |

System calls (numbers 180-182 in `core/sysabi.h`, handled by `gui/usergui.c`):

| # | call | |
|---|------|-|
| 180 | `SYS_DND_BEGIN(win, paths, len, effects | icon << 8, count)` | the serial number, `E_AGAIN`, `E_BUSY`, `E_INVAL` |
| 181 | `SYS_DND_ACCEPT(win, serial, effect, possible)` | the answer to `DND_ENTER` / `DND_OVER` |
| 182 | `SYS_DND_DATA(serial, buf, size)` | the paths' length in bytes (with the NULs); `E_NOENT` once another drag started |

`dnd_open_target(&ev, exts, path, size)` (`userland/lib/dnd.c`) is the whole
target for programs that open a file dropped on them: it answers "copy" for
one file with one of the extensions and returns 1 with its path on the drop.
Notepad (any file), Paint (`.bmp`) and Media Player (`.wav`) use it in their
event loops; Notepad and Paint ask to save changes first.

### Explorer's rules

The target folder is the folder or drive under the pointer (it is
highlighted), else the folder the window shows (My Computer's background
takes nothing).

- Same drive: move. Another drive: copy. Ctrl forces a copy, Shift a move.
- Nothing can be dropped on a read-only drive, into an item being dragged or
  inside it, or back into the folder the items are in (except as a copy:
  Ctrl, or Copy Here from the right-drag menu, makes "Copy of x").
- On the Recycle Bin (its window, or the desktop icon): a move, which
  deletes the items to the bin (with the usual confirmation and the
  permanent-delete rules below); items on read-only drives cannot go there.
- Copies and moves run in Explorer's worker thread behind its progress
  window. Before they start, every name that is already taken asks "This
  folder already contains a file named..." with Yes, Yes to All, No and
  Cancel; a folder replaces by merging (files of the same name in it are
  replaced), as Windows XP does. A move within a drive is a rename; onto an
  existing folder it becomes a copy and a delete.

The desktop is a kernel target: the Recycle Bin icon takes everything not
on read-only drives (`fileop_recycle`), the My Documents icon takes copies
and moves into `C:\Documents` (`fileop_transfer`: the kernel's background
copy / move with the progress dialog and a replace question per conflict).
The built-in kernel Explorer (the fallback) is a source (one item) and a
target with the same rules.

## The Recycle Bin

### On disk

Every drive that can have a bin keeps it in a folder of its root,
**`X:\RECYCLED`**, on every file system (Windows' own `\$RECYCLE.BIN` on NTFS,
with its per-user `$I` / `$R` files, is left alone, and Windows 98's `Dc1.txt`
naming does not clash). Deleting renames the item into that folder as
`D<id>` plus its original extension (`D17.txt`; folders have none), so it is
instant whatever its size and never crosses drives. The index
`X:\RECYCLED\INDEX.TXT` says where each item came from:

```
HALCYON RECYCLE BIN 1
17<TAB>2026-11-12 10:07:17<TAB>594<TAB>F<TAB>C:/Documents/Unicode – Ünïcödé.txt
END 1
```

one line per entry: the id, the local date and time of the delete, the size
in bytes (a folder's whole contents), `F` or `D`, and the original full path
(UTF-8; bytes below 0x20 and `%` are written as `%XX`). Lines are in delete
order, so the oldest come first. Ids grow from the highest one in use.

The folder is not marked hidden (the VFS has no call to change attributes);
both Explorers and the desktop leave a drive root's `RECYCLED` folder out of
their views. It is removed when the bin becomes empty (unless something
else was put in it).

### Safety

- **Atomic index:** the new index is written as `INDEX.NEW`, then the old one
  is deleted and the new one renamed into place. When reading, an
  `INDEX.NEW` that ends with its `END` line is complete and wins (the
  rename did not happen); a cut-off one is ignored. If writing the index
  fails, the item is renamed back.
- **Recovery** happens on every read: lines that do not parse (bad numbers,
  dates, kinds, another drive's path, a drive root, duplicates) are dropped,
  as are entries whose stored item is gone or of the other kind; items in
  the folder named `D<id>[.ext]` that no line lists are kept, to be restored
  to the root of the drive under their stored name, with their own date and
  size. A recovered index is written back at once.
- **Concurrency:** one kernel mutex covers every read-modify-write of an
  index (two programs deleting at once), taken before the VFS's storage lock.
  The desktop does not wait for it: it looks again at its next tick.
- **Size limit:** a bin may use a share of its drive: `recycle.percent.<the
  volume's serial number, 16 hex digits>` in `SETTINGS.INI`, else
  `recycle.percent`, default 10 %. After each delete the oldest entries
  (by date, then id) are purged until it fits; an item larger than the whole
  limit is refused (`E_NOSPC`) and Explorer offers to delete it permanently.
  Lowering the limit in Properties purges at once.

### Which drives

`rb_supported()`: read-only volumes (the device, the file system driver,
NTFS volumes Halcyon may not write, CDs) have none and **nothing on them can
be deleted**; Explorer says so with the volume's own reason. Removable
drives (USB sticks, floppies) have none and **delete permanently** after a
confirmation, as on Windows - except the boot drive, Halcyon's own system
drive, which keeps a bin even on a USB stick. Fixed disks (SATA, NVMe) have
one. `recycle.nuke=1` ("Do not move files to the Recycle Bin") makes every
delete permanent; `recycle.confirm=0` drops the "send to the Recycle Bin?"
question (permanent deletes always ask).

### System calls

Numbers 183-189 (`apps/shellsys.c`, absolute paths):

| # | call | |
|---|------|-|
| 183 | `recycle_delete(path)` | the entry's id; `E_NOTSUP` (no bin there: delete for good after asking), `E_ROFS`, `E_NOSPC` |
| 184 | `recycle_list(drive or 0, hal_rbentry_t *, max)` | entries (all drives: 0), oldest first per drive; max 0 counts |
| 185 | `recycle_restore(drive, id, RBR_FAIL / RBR_REPLACE / RBR_RENAME, out, size)` | missing folders are made again; `E_EXIST` with `RBR_FAIL` when the place is taken |
| 186 | `recycle_purge(drive, id)` | one entry for good; id 0 empties the drive's bin, drive 0 every bin |
| 187 | `recycle_info(drive, hal_rbinfo_t *)` | supported, entries, bytes, limit, percent, the drive's size, the options, a generation number that changes with every change of any bin |
| 188 | `recycle_set(drive, percent, 0)` / `recycle_set(0, 0, RBF_*)` | saved in `SETTINGS.INI` (on the GUI thread) |
| 189 | `shell_setting_get / _set(key, ...)` | Explorer's own settings, keys `explorer.*` only |

### In the shell

- **Explorer:** Delete (key, menus, task pane) sends the selection to the
  bin, after "Are you sure you want to send ... to the Recycle Bin?";
  Shift+Delete and File > Delete Permanently delete for good after "...
  permanently delete ...?". The Recycle Bin view (Go > Recycle Bin, Other
  Places, the address "Recycle Bin", or `EXPLORER.EXE "Recycle Bin"`) lists
  the entries of every drive with Name, Original Location, Date Deleted and
  Size; Restore (the selection, or all; it asks Replace / Keep Both / Skip /
  Cancel when the original place is taken), Delete (for good), Empty Recycle
  Bin (behind the progress window), Properties (an entry's details; with
  nothing selected the bin's Properties). The view follows changes made
  elsewhere (the bins' generation number, checked twice a second).
- **Recycle Bin Properties** (`EXPLORER.EXE -rbprops`, the desktop icon's
  Properties): every drive with a bin, its items and bytes; the selected
  drive's limit in percent with what that means in bytes and a bar of how
  full it is; "Do not move files to the Recycle Bin" and "Display delete
  confirmation dialog".
- **Desktop:** a Recycle Bin icon (new artwork: an empty glass wastebasket,
  and a full one with paper and a crumpled sheet over the rim) that follows
  the bins; double click opens the view; its menu has Open, Empty Recycle
  Bin (a question, then the kernel's progress dialog: `FOP_EMPTY`) and
  Properties. Files dropped on it are deleted to the bin.
- **The built-in Explorer** deletes to the bin as well (Shift+Delete for
  good) and hides the bins' folders; its single selection is unchanged (the
  fallback is frozen).

## Explorer's settings

Kept in `SETTINGS.INI` through `SYS_SHELL_SETTING`, written when they change:
`explorer.view` (0 icons, 1 details), `explorer.sort` (the column: name, size,
type, date modified; in the Recycle Bin name, original location, date
deleted, size), `explorer.sortdesc` (0/1), `explorer.pane` (the task pane
with its Details box: View > Task Pane). Clicking a column header of the
details view sorts by it (again: the other way), with an arrow in the header;
View > Arrange by does the same. Folders stay before files.

## Tests

- **Host** (`tests/host/recycletest.c`, scenario `recycle` of fstest, part
  of `tests/host/run.sh`): `recycle.c` compiled unmodified against the host
  VFS, on a FAT32 image from mkfs.fat, an exFAT image from mkfs.exfat and an
  NTFS volume made by the kernel's formatter: the index format (read back,
  and on FAT through mtools after a remount), unique stored names (the same
  name twice, the same name from two folders, Unicode), folders and their
  sizes, refusals (roots, the bin itself, missing items), restoring into
  deleted folders, conflicts (fail, keep both, replace, a folder replaced by
  a folder), purging one entry, emptying (the folder goes), the limit
  (oldest first, `rb_enforce` keeping one, too big for the bin, the percent
  settings), cancelling an empty half way, and recovery: garbage lines, bad
  dates, duplicate ids, entries for missing items, another drive's paths, a
  cut-off last line, orphans adopted, foreign files left alone, a complete
  and a half-written `INDEX.NEW`, no index, an empty and a binary index, an
  escaped path round trip; then read-only and removable drives. Every image
  is checked with fsck.fat / fsck.exfat / exfat_tool.py / ntfsfix -n.
  562 checks.
- **Boot** (`tools/boottest.py` variant `recycle`, in the default matrix as
  `xhci:bios:recycle`; QEMU fw_cfg `opt/halcyon/recycletest`): a kernel
  thread deletes to the bin, restores into a deleted folder and empties the
  exFAT disk's bin, then starts EXPLORER.EXE on the FAT32 test disk and
  drops `C:\Documents\Getting Started.txt` on its window through the window
  manager's drag code (the program must answer "copy" and copy it). The
  host then checks the bin's `INDEX.TXT` and `D1.txt` with mtools, the
  restored and the copied files, that no bin is left on the exFAT disk
  (exfat_tool.py), and fsck.fat, fsck.exfat and exfat_tool.py check.
  18 checks in the guest.
- **Screenshots** (`tools/shot.sh`, and `tools/qemu_ctl.py`'s new `press:`,
  `release:`, `keydown:` / `keyup:` and `SCREEN=WxH`): several items
  selected with Ctrl, the rubber band, a drag over a folder, the right-drag
  menu, delete confirmations, the Recycle Bin window and its menu, its
  Properties, the desktop icon's menu, a bitmap dropped on Paint.

## Not done

- No undo of a move, copy or delete (Windows XP's Edit > Undo).
- Drags carry paths only: no text, pictures or other formats, and no "link"
  effect (Ctrl+Shift copies).
- No drag out of the Recycle Bin (restore it instead) or out of My Computer
  (drives), and the desktop has no file icons to select or drag.
- Explorer has no change notifications: a window learns about changes made
  elsewhere only for drops it took part in, the bins, and drives coming and
  going (F5 refreshes).
- The `RECYCLED` folder is hidden by name, not by its attributes (the VFS
  has no call to set them), so the Command Prompt's `DIR` shows it.
- The built-in kernel Explorer has no multiple selection and no Recycle Bin
  view (it tells the user that EXPLORER.EXE shows it).
