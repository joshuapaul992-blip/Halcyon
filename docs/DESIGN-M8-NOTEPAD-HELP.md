# Milestone 8 design: Notepad's undo and word wrap, Help and Support

Two parts of M8 ("Polish"): Notepad gains multi-level undo and redo, word
wrap, a Font dialog, Replace and Go To; and Halcyon gets a help viewer,
Help and Support (`HELP.EXE`), with its own help pages.

## Notepad

Notepad (`userland/programs/notepad.c`) is now three files:

| File | What |
|------|------|
| `notepad.c` | the window: drawing, input, menus, dialogs, files |
| `notepad/doc.c`, `doc.h` | the text (gap buffer) and its display rows, with or without word wrap |
| `notepad/undo.c`, `undo.h` | the undo and redo history |

`doc.c` and `undo.c` include nothing of the window API, so
`tests/host/notepadtest.c` compiles them for Linux (`-DNP_HOST`, with
`userland/lib/utf8.c`, which now holds the user library's UTF-8 functions so
that it can be shared) under ASan and UBSan. The Makefile builds
`notepad.exe` from `notepad.c` plus `notepad/*.c` (an explicit rule, like the
browser's).

### Rows

The text is a gap buffer; the display rows are a second gap array of row
start offsets (entries after the gap are stored as distances from the end of
the text, so an edit renumbers nothing after it), as before M8. A row that
starts a line of the file (offset 0 or after a newline) is *hard*; one that
continues the row before is *soft* (flag `SOFT`). Rows break:

- without word wrap, every `LINE_BYTES` (2048) bytes, as before (Windows
  Notepad breaks lines past 1024 characters), which bounds the work per row;
- with word wrap, at the wrap width in pixels: after the last space or tab
  that fits (spaces at the end of a row hang past the edge and never cause
  a break, as in Windows' edit control), between characters for a word
  longer than the row (at least one character per row), and never past
  `LINE_BYTES` bytes (preferring a space there too). `row_break()` is the
  one function that decides; tabs advance to multiples of the tab width
  from the start of the row.

An edit (`doc_replace`) drops the rows of the lines it touches and rebuilds
just those (`rebuild()`), so typing costs the same anywhere in a 16 MB file,
wrapped or not.

**Changing the width** (resizing the window, turning wrap on or off,
another font) makes every row stale. Rebuilding all rows of a 10 MB file takes
about 0.3 s natively and several seconds under QEMU's emulation, so it is
incremental:

- `doc_set_wrap(width, from)` records *stale ranges* `[pa, pb)`: from the
  line of the view's top to the end of the text first, then from the start to
  there. `pa` is always the start of a (correct) row; `pb` is a line start or
  the end. Stale rows keep their old breaks: they are a valid row structure,
  just broken at the old width, so every function keeps working on them.
- `doc_rewrap(budget)` rebuilds about `budget` bytes from the first range: new
  rows are made from `pa` while the old ones they cover are dropped, and it
  stops at the first row start past the budget: at a line start the old row is
  kept; at a soft break the new break is entered and becomes the range's new
  `pa`. A line of several megabytes is therefore done in slices too.
- Notepad rebuilds the visible rows at once (`sync_view`: the ranges are
  reordered by `doc_rewrap_prefer()` so the view's line comes first, then
  16 KiB slices until the view is fresh), and the rest in 64 KiB slices
  between events (the event loop polls with a zero timeout while anything is
  stale; the scroll bar follows). The view is anchored by the offset of its
  first row, so rows rebuilt above it do not move it.
- Edits adjust the ranges: positions after the edited lines shift, a range
  start inside them moves to their end (those rows were just rebuilt), a range
  end inside them moves to their start.

**Line and column.** The status bar shows the line of the file and the
character column, not the row, so wrapped rows count as one line (Windows XP
hides the status bar while wrapping; Halcyon keeps it). `logical_line(row)`
counts hard rows from a cached (line start, line number) pair; edits update
the cache (newlines counted in the deleted and inserted text), so caret moves
and typing walk only a few rows.

**The caret at a soft break.** An offset at a wrap break is both the end of
one row and the start of the next. Notepad keeps a flag (`eol`): End, and Up
or Down or a click past the end of a wrapped row, put the caret at the end of
that row; anything else at the start of the next.

**The horizontal scroll bar** (new) is shown without word wrap only. Its
range is the widest row drawn so far (and at least the current position plus
a page): measuring every row of a large file would cost what wrapping costs.

### Undo and redo

The history is a list of *steps*; each step holds one or more edits
("replace `[pos, pos + deleted)` with inserted", both texts kept) and the
selection before and after it. Undo applies the step's edits in reverse
(inserted text replaced by the deleted text) and restores the selection before
it; redo applies them forwards and restores the selection after. If an edit
fails half way (no memory), the edits already undone are applied again, so the
text and the history stay in step.

Grouping (`undo_record`): a typed character joins the open step if the step
is typing, the character goes right after the step's text, less than
`UNDO_PAUSE_MS` (1.5 s) passed, the step is not the saved one, and no new word
starts: a non-space after a space starts a new step, and so does a newline
(Enter, with its indentation). Backspace and Delete runs group the same way
(deleted text grows at the front, or at the back). Typing over a selection
starts a step that the following characters join. Caret moves, clicks, Find,
Select All and the like call `undo_break()`. Cut, paste, deleting a
selection, Delete from the menu, Time/Date and each Replace are steps of their
own; Replace All is one edit of the whole changed span (built in one buffer),
so it is one step and one row rebuild.

The *saved* state is the history position matching the file (`saved`;
loading, New and Save set it). Modified is "position != saved", so undoing back
to it clears the asterisk in the title. A new edit after undoing drops the
redo steps, and with them the saved state if it was among them; dropping old
steps can drop it too (then the document stays modified until saved).

Memory: the bytes of text kept by all steps (plus a small overhead per step
and edit) are limited to 4 MiB; the oldest steps are dropped first, but the
newest step is always kept, so even replacing a whole 10 MB file can be undone
once. If the history cannot get memory it is cleared and Notepad says so;
if the text cannot, nothing changes.

### Settings, font, dialogs

Word wrap, the font and the status bar are kept in `SETTINGS.INI`
(`notepad.wrap`, `notepad.font`, `notepad.statusbar`) through two new system
calls, `SYS_SETTING_GET` (170) and `SYS_SETTING_SET` (171) (`setting_get`,
`setting_set`, `setting_get_int`, `setting_set_int` in `hal.h`). Keys are
`[A-Za-z][A-Za-z0-9._-]*`, values one line of up to 127 bytes; programs
cannot set the `ntfs.*` keys (those are permissions). A set runs on the GUI
thread (SETTINGS.INI belongs to the shell, as for `SYS_VOLUME_SET_WRITE`)
and is saved at once, unless the value did not change. *At merge:* the M8
"settings everywhere" work may bring its own calls for this; Notepad uses them
in two small functions (`load_settings`, `save_font`) plus two
`setting_set_int` calls.

Format > Font... offers the system fonts that suit text (monospace 12,
sans serif 9 / 11 / 11 bold / 12 bold, and the 20-pixel bold font, which
only has Latin-1), with a sample. Edit > Replace... (Ctrl+H) and Find... are
one dialog (Find what, Replace with, Match case, Find Next, Replace, Replace
All); Go To... (Ctrl+G) works with word wrap on, unlike XP's. View > Status
Bar, Help > Help Topics (F1), and Undo in the context menu complete the
menus; Cut, Copy and Delete are disabled without a selection, Undo and Redo
when there is nothing to undo or redo.

### Tests

`tests/host/notepadtest.c` (`make -C tests/host notepad`, run by
`tests/host/run.sh`), about 67,000 checks:

- `row_break`: wrapping at spaces, hanging spaces, long words broken,
  tabs, wide characters, UTF-8 never split, invalid bytes, the byte limit;
- 120 rounds of random text edited 300 times each, the wrap width changed and
  stale rows rebuilt in random slices (and reordered) in between: after every
  step the text must equal a plain copy and the rows must be valid (sorted, a
  hard row at every line start and nowhere else, `nsoft` right, line numbers
  right); whenever nothing is stale they must equal rows made from scratch;
- undo grouping (words, pauses, newlines, caret moves, Backspace and Delete
  runs, typing over a selection, `UNDO_JOIN`), restored selections, the saved
  state through undo, redo and new edits, the memory limit (oldest dropped,
  newest kept even when larger than the limit), and 40 rounds of random
  edits, undos and redos checked against a copy of the text at every step;
- a 10 MB file: loaded, wrapped in 256 KiB slices (the longest slice must stay
  under 250 ms with ASan; it is about 10 ms), 2000 edits in the middle (about
  0.05 ms each), and the line number of the last row.

### The kernel's Notepad

The built-in Notepad (`kernel/apps/notepad.c`), the fallback when programs
cannot run, is frozen (docs/DESIGN-M9.md) and did not get undo or word wrap.
F1 there opens the Notepad help page like in every built-in window.

## Help and Support

### Pages

The help pages are HTML files in `help/` (34 pages, original text), with
`help/contents.txt` for the Contents tree. `tools/mkhelp.py` (run by `make`)
checks them and writes `build/help/` (the pages, `CONTENTS.TXT` and
`INDEX.TXT`), which `tools/mkimage.sh` copies to `C:\HALCYON\HELP`:

- every page has a `<title>` and `<meta name="keywords" content="a; b#anchor">`;
  the keywords make the index (`keyword<TAB>page#anchor<TAB>title`, sorted);
- every link and keyword points to an existing page and anchor, every page is
  in the contents, and pages use only markup HELP.EXE draws (no `<h1>`, whose
  34-pixel font is too large, no images, no scripts); the build fails
  otherwise;
- `<!-- CHECK: ... -->` comments mark text written before the feature it
  describes was merged (the Recycle Bin, selection and drag and drop, screen
  savers, the installer, sound schemes); `MKHELP_CHECKS=1 make` lists them.

Page names are lower case 8.3-friendly (`notepad.htm`); a program's page is
named after it (`notepad`, `paint`, `calc`, `mplayer`, `browser`, `cmd`,
`taskmgr`, `sysinfo`, `sonar`, `files`, `control`, `display`, `keyboard`,
`sounds`, `network`, `timezone`, `format`, `drives`, `welcome`).

### HELP.EXE

`userland/programs/help.c`, linked with the Web Browser's HTML parser and
layout (`browser/html.c`, `browser/layout.c`, and libweb for URLs): the page is
parsed, laid out to the pane's width, and its display list drawn by the
program (text, rectangles, rules, bullets, table frames) into the mapped
window pixels, as the browser does; no images or form controls are needed.

- **Contents**: the tree from `CONTENTS.TXT` (two spaces of indentation per
  level; an entry followed by deeper ones is a book), drawn by the program with
  16-pixel book and page icons, its own scroll bar, the mouse wheel and the
  keyboard (arrows, Left and Right close and open books, Enter). The page shown
  is selected in the tree and its books opened.
- **Index**: a text box and a list box; the list shows the keywords starting
  with the typed text (or, if none, containing it), updated on every key: text
  boxes can now report each change (`CTL_CHANGES` style, `CMD_CHANGE`
  notification; the control creation call takes style bits up to 0x3FF). A
  keyword on several pages offers the pages in a popup menu.
- **Search**: the pages are read once (lower-case text without tags, the
  title and the keywords); every word must occur (at the start of a word), and
  the score adds the occurrences in the text (at most 20 per word), 15 for a
  word in the title, 8 for one in the keywords, and for several words 10 per
  occurrence of the whole phrase and 30 for the phrase in the title. Results
  are listed by score, then title.
- **The page pane**: links (hand pointer, highlighted under the pointer),
  `#anchors`, Back / Forward / Home (Alt+Left / Alt+Right / Alt+Home),
  PageUp/PageDown/Space, the wheel; a draggable splitter (width kept in
  `help.pane`). Links `run:NAME` start `C:\PROGRAMS\NAME.EXE` (`run:control`
  opens the Control Panel) and `http(s):` links open the Web Browser.
- **One window**: HELP.EXE listens on the local socket `halcyon.help`; a
  second HELP.EXE sends its page there, and the first shows it and comes to
  the front (`SYS_WIN_SHOW`).

### Opening help

- `help_open("page")` in the user runtime (`userland/lib/shell.c`) starts
  `C:\PROGRAMS\HELP.EXE page`. F1 in Notepad, Paint, Calculator, Media Player,
  the Web Browser, Task Manager, Sonar and Explorer opens their page; Notepad,
  Paint, Media Player and the Web Browser also have Help > Help Topics.
  Calculator's Help button and Sonar's "?" keep their own boxes.
- `shell_help(page)` in the kernel (`gui/desktop.c`). A built-in window names
  its page in the new `window_t.help`; the window manager opens it on F1
  (for a dialog, the page of the window it belongs to). All built-in windows
  have one (Control Panel, Display, Keyboard, Sounds, Network, Time Zone,
  Command Prompt, System Information, Format, drive properties, About and the
  fallback Notepad, Paint, Explorer, Calculator and Sonar). F1 on the desktop
  opens the home page.
- Start > Help and Support opens the home page (`welcome.htm`).

### Test

The boot test variant `help` (`tools/boottest.py xhci:bios:help`, in the full
matrix) runs `HELP.EXE --test notepad` through the `opt/halcyon/autorun`
hook: the contents and index load, the Notepad page renders and is selected in
the contents, every page in the contents renders (34) and every one of its
links leads to an existing page and anchor (162 links), Back and Forward, an
anchor scrolls, a link from the home page, the index filter, and the search
ranking ("word wrap" finds Notepad first, "recycle bin" the Recycle Bin page,
nonsense nothing); it must report `helptest: all N checks passed`.

## Not done

- The kernel's fallback Notepad has no undo or word wrap (frozen).
- Help pages have no pictures, and there is no Print (as asked) and no
  "Favorites" or "History" tab.
- The search has no stemming ("wrapping" does not find "wrap") and no
  highlighting of the words in the page.
- Notepad's horizontal scroll range grows with the rows drawn rather than
  being the widest row of the file.
- The Ln/Col computation walks from the start of the line: in a single line
  of several megabytes, moving near its end costs a scan of that line per
  status bar update.
