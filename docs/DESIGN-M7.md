# Milestone 7 design: sound

Milestone 7 gives Halcyon sound output: drivers for sound cards, a mixer that
plays any number of streams at once, system calls for programs, system sounds
for the desktop's events, the volume in the taskbar and on the keyboard's
volume keys, a Sounds and Audio Devices dialog, and Media Player. Output only:
no recording, no MIDI synthesizer, no compressed formats beyond what WAV files
carry.

Code: `kernel/sound/` (the device layer, mixer and audio thread in
`snddev.c`; the system calls in `sndsys.c`, ABI in `sndabi.h`; system sounds
and the sound scheme in `sndevent.c`; drivers in `drivers/`),
`kernel/gui/volume.c` (the tray icon, its popup, the volume keys and the
on-screen indicator), `kernel/apps/sound.c` (Sounds and Audio Devices), and in
user mode `userland/lib/wav.c` (the WAV library), `MPLAYER.EXE`, `PLAY.EXE`
and `SNDTEST.EXE`. The system sounds are synthesized at build time by
`tools/mksounds.py`.

## The device layer

A sound card driver fills in a `snddev_t` (description, output name, rate,
channels, period length, and `start` / `stop` / `poll` / `set_volume`
callbacks) and registers it; the core names it `snd0`, `snd1`... The first
card registered is the default: every stream plays there. Drivers are probed
by `sound_init()` from `kmain` once `SETTINGS.INI` has been read, so the saved
volume applies from the first sound; the boot option "no sound" (part of Safe
Mode, `BOOTF_NO_SOUND`) skips them.

The device format is signed 16-bit little-endian, interleaved, at the device's
rate (48 kHz wherever the hardware allows), two channels. Drivers keep a ring
of a few periods of about 10 ms ahead of the hardware and fill each free one
with `snd_mix()`. If the hardware catches up (an underrun), the driver counts
it and starts over.

**AC'97** (`drivers/ac97.c`): Intel ICH-style controllers (QEMU's 82801AA, the
ICH0..ICH7, 440MX, and the nForce and AMD controllers that copy the register
set). The PCM-out box walks a 32-entry buffer descriptor list round robin, one
10 ms period per entry, four periods ahead of the hardware; the codec is set to
48 kHz (variable rate is not needed: the mixer resamples) and 0 dB, and the
master volume is applied by the mixer. INTx or polled. Other drivers (HD
Audio, virtio-sound, ES1370, SB16, the PC speaker) plug in the same way.

## The mixer and the audio thread

The `audio` kernel thread owns all sound work. While any stream has something
to play it starts the default device and calls its `poll()` whenever the
device may need data; after 3 s with nothing to play it stops the device
again, so an idle system takes no sound interrupts. Interrupts are only
wake-ups, as everywhere in Halcyon: with a working interrupt the period
interrupt wakes the thread (a timer is the backstop), without one it wakes
twice a period.

`snd_mix()` mixes every stream into 32-bit accumulators: each stream's
samples (8-bit unsigned or 16-bit signed, mono or stereo) are converted and
resampled from the stream's rate to the device's by linear interpolation (a
32.32 fixed-point position), scaled by the stream's volume, summed, scaled by
the master volume and clipped. The master volume is squared so the slider
feels like a fader (70 % is -6 dB, 50 % -12 dB, 10 % -40 dB); mute is a gain of
zero. A driver that can apply the volume in hardware may (`hw_volume`).

Everything runs under the big kernel lock: the mixer, the system calls and
the GUI never run at the same time, and no lock is needed between them.

### Kernel streams and static sounds

A stream (`snd_stream_open`) is a ring of stereo 16-bit frames at the
stream's own rate, sized from a buffer length (20 ms to 2 s). Writes copy whole
frames in and never block; the mixer keeps one frame back to interpolate with
the next until `snd_stream_finish` says no more data comes. Streams can be
paused, given a volume, closed at once or closed "with drain" (the stream
frees itself once it has played out). A static sound (`snd_play_static`) plays
a buffer that stays valid until it ends, without a ring: the system sounds.
A program's streams are released when it ends (`snd_release_pid`).

### The test hook

Under QEMU, the fw_cfg file `opt/halcyon/soundtest` (a length in ms) makes the
audio thread play a known signal once the desktop is drawn: a 440 Hz sine at
44.1 kHz through an ordinary stream, then a 1 kHz triangle at 22.05 kHz as a
static sound for 0.25 s, and log a summary (underruns, interrupt mode). It
first waits until the startup sound has finished and nothing else plays (at
most 20 s), so that QEMU's recording holds the startup sound and then the
test signal, apart. Real hardware has no fw_cfg, so it never runs there.

## System calls

Programs play sound through streams that are handles (`H_SOUND`) in the
process's handle table, like files and sockets: `close()`, `write()` and
`poll()` work on them. ABI: `kernel/sound/sndabi.h` (included by
`core/sysabi.h`); wrappers in `userland/include/hal.h`. Results are
Halcyon's negative `E_*` codes.

| # | Call | Notes |
|---|------|-------|
| 140 | `snd_open(rate, channels, bits, buffer_ms, flags)` → handle | 8000..192000 frames/s, 1 or 2 channels, 8-bit unsigned or 16-bit signed samples, a buffer of 20..2000 ms (0 = 250); `SND_NONBLOCK`. `E_NODEV` without a sound card, `E_BUSY` past the limits |
| 141 | `snd_write(h, buf, len)`, `snd_write_nb` | whole frames only (`E_INVAL` otherwise). Blocking: waits until everything is queued; non-blocking (stream flag or `SNDW_NONBLOCK`): takes what fits, `E_AGAIN` if nothing does |
| 142 | `snd_drain(h, timeout_ms)` | waits until all that was written has been played (`E_TIMEOUT`); the stream is empty and writable afterwards |
| 143 | `snd_ctl(h, op, value)` | `SNDCTL_PAUSE` 0/1, `SNDCTL_VOLUME` 0..100 (the stream's own), `SNDCTL_NONBLOCK` 0/1, `SNDCTL_DISCARD` (drop the queue: a seek) |
| 144 | `snd_status(h, &st)` | frames written, played and queued, underruns, the format and buffer, free space in bytes, volume, pause, latency estimate (queue plus two device periods), the device period |
| 145 | `snd_close(h, how)` | `SNDCLOSE_NOW` (stop at once), `SNDCLOSE_DRAIN` (let the queue play out in the background; what `close()` does), `SNDCLOSE_WAIT` (play out and wait) |
| 146 | `snd_master(volume, mute, flags)` | -1 keeps a value; `SNDVOL_SAVE` writes `sound.volume` / `sound.mute`; returns the volume, plus `SND_MUTED` |
| 147 | `snd_event(event, flags)` | plays a system sound (`SNDEV_*`): 1 played, 0 the sound scheme has it off. `SNDEVF_FORCE` plays it anyway (a test button), `SNDEVF_QUERY` only answers whether it would play |
| 148 | `snd_devinfo(index, &d)` | a sound card: name, description, driver, output, rate, channels, period, running, default, interrupt, frames played, underruns; `E_NOENT` past the last |
| 149 | `snd_info(&i)` | the number of cards, the master volume, whether system sounds are on, stream use and limits (system-wide and the caller's), whether the startup sound is still playing, a generation that changes with volume, mute and devices |

150 and 151 are free (recording later).

- **Handles over kernel streams.** A handle holds a `ustream_t` that owns one
  kernel stream. Because a kernel stream that was told "no more data"
  refuses writes for good, `drain` and `discard` replace it with a fresh one
  (keeping the volume and the pause), so a program's stream stays usable after
  a drain or a seek; played frames and underruns are carried over.
- **Waiting.** A full buffer (blocking write), a drain and `SNDCLOSE_WAIT`
  wait with `proc_wait` on a generation that moves whenever a device has mixed
  a period or a stream changed (pause, discard, close), so every wait ends
  with `E_CANCELED` as soon as the program is being ended. A `ustream_t` is
  reference counted: another thread of the program may close the handle while
  one waits in it (the waiter then gets `E_CANCELED`). A write into a paused
  stream waits until it is resumed; a drain of a paused stream times out.
- **Poll.** `poll()` (net/socket.c) reports `POLLOUT` on a sound handle when a
  quarter of its buffer is free and wakes on the same generation; a closed
  handle is `POLLNVAL`. A program can wait for its windows and its stream at
  once (Media Player does).
- **Limits.** A program has at most 8 streams (`SND_PROC_STREAMS`), all
  programs together 24 (`SND_USER_STREAMS`); the kernel's 32 leave room for
  system sounds. Every pointer and length is checked (`copyin` / `copyout`);
  data is copied 16 KB at a time, between blocking points. A program's
  streams stop when it ends, however it ends; a stream closed with drain plays
  on after its program has gone (at most its buffer, 2 s).
- Any program may change the master volume and play system sounds, as any
  program may change the network configuration (there are no permissions
  for this yet).

## System sounds

`sndevent.c` plays a sound for these events:

| Event | When | File |
|-------|------|------|
| Start Halcyon | the desktop is ready (`gui_desktop_ready`), once | `STARTUP.WAV` |
| Exit Halcyon | Turn Off and Restart: played, and waited for (at most 4 s) before the power goes | `SHUTDOWN.WAV` |
| Error, Warning, Information, Question | a message box with that icon: the kernel's `msgbox`, programs' `msgbox()` (SYS_MSGBOX), crash reports, permission questions | `ERROR.WAV`, `WARNING.WAV`, `INFO.WAV`, `QUESTION.WAV` |
| Notification | a balloon from the tray ("Found New Hardware") | `NOTIFY.WAV` |
| Device Connect / Disconnect | a USB device appears or goes | `DEVADD.WAV`, `DEVREM.WAV` |
| Default Sound | after the volume slider is moved (feedback) | `DEFAULT.WAV` |

- The files live in `C:\HALCYON\MEDIA`. The "sndevents" kernel thread loads
  them all into memory at boot and again when the scheme changes, so playing
  one only queues a static sound: the GUI thread never waits for a disk or
  for sound. A sound replaced while it may still be playing is freed only
  after it can no longer be.
- The thread also plays the startup sound once the desktop is up (while it
  plays, `snd_info` reports `startup_pending`, and the kernel's sound test
  waits) and writes `SETTINGS.INI` for the volume and the scheme.
- Shutdown: the sound starts as the final screen appears, plays while the
  boot log is saved and the disks are synced, and the system waits for the
  rest of it, at most 4 s in all, before restarting or turning off.
- The same sound again within 150 ms is skipped. Device and balloon sounds
  stay quiet for the first 10 s after the desktop comes up (devices found
  while booting are not news), and a balloon right after a device sound
  stays quiet.
- **The sound scheme** (`SETTINGS.INI`): `sound.sounds=0` turns system sounds
  off, `sound.event.<key>=0` one of them (`startup`, `shutdown`, `error`,
  `warning`, `info`, `question`, `notify`, `devadd`, `devremove`, `default`),
  `sound.file.<key>` names another WAV file (8 or 16-bit PCM, mono or stereo,
  any rate the mixer takes, up to 8 MB). `sound.tray=0` hides the volume icon.

### The sounds themselves

All of them are made by `tools/mksounds.py` at build time from nothing but
Python's `math`: FM bells and mallets (a sine carrier phase-modulated by a
sine at a fixed ratio, the modulation index decaying faster than the
amplitude, so each note starts bright and rings out pure), additive pads
(harmonics at 1/n^1.6 in two slightly detuned voices with slow swells),
gliding blips, one-pole low-pass filters, and a Schroeder reverb (four damped
combs and two all-passes per channel, the right channel's delays a little
longer for width). The script is deterministic.

- Startup (4.6 s, stereo): a rising D, A, D, E, A arpeggio of glass bells
  panned left to right over a D-major-9 pad that swells in, settling on an
  F#-A-D bell chord.
- Shutdown (2.7 s, stereo): the same colours falling, F#, D, A, to a low D.
- Error: two low inharmonic knocks, the second lower. Warning: one bell with
  a small downward bend. Information: two light mallet notes rising.
  Question: three soft notes rising like a voice asking. Notification: a
  glassy double ding. Device connect / disconnect: two blips gliding up /
  down. Default: one short soft ding.

They are 16-bit PCM at 22.05 kHz (startup and shutdown stereo, the rest mono),
under 1 MB together. They are Halcyon's own and do not imitate any other
system's sounds. The script also writes a 13-second piece for Media Player,
`C:\Documents\Music\Halcyon Dawn.wav`, encoded with its own IMA ADPCM
encoder (so the image carries a compressed file to play).

## The volume in the taskbar and on the keyboard

- **Tray icon**: a speaker (crossed out when muted), shown when there is a
  sound card and `sound.tray` is on. Resting the pointer on it shows
  "Volume: 80%"; a click opens a small popup (XP style) with a vertical
  slider, its ticks and a Mute box, which follows the wheel and the arrow
  keys and closes on a click elsewhere; a double click opens Sounds and Audio
  Devices. Releasing the slider plays the default sound as feedback.
- **Keys**: Mute, Volume Up and Volume Down (HID keyboard usages 0x7F-0x81,
  also from Consumer Control reports, and PS/2 E0 20 / E0 30 / E0 2E) change
  the master volume in 5 % steps (Volume Up also unmutes) wherever the focus
  is, and show an indicator over everything: a translucent panel with the
  speaker, 20 segments and the level, drawn by the window manager after all
  windows (`gui_overlay_paint`), which fades out after 1.5 s.
- **Saving**: changes apply at once; `SETTINGS.INI` is written by the
  sndevents thread a second after the last change, so a held key or a slider
  drag writes the file once.

## Sounds and Audio Devices

A kernel dialog (Control Panel's "Sounds..." button, or a double click on the
tray icon): the master volume (a horizontal slider with Low / High and the
level), Mute, "Place volume icon in the taskbar"; the sound cards with driver,
output, rate, state (playing / idle, default) and interrupt; and the sound
scheme: "Play system sounds", the list of program events (those turned off are
marked), Enabled, Play (also when the event is off), Browse... (another WAV
file, through the file dialog) and Default, with the file's path and length or
why it cannot be used. Volume and mute apply at once and Cancel puts them
back; the rest applies with Apply or OK. System Information has a "Sound
devices" category with each card's details (driver, output, format and
periods, state, interrupt, frames played, underruns, PCI details) and audio
PCI devices that have no driver.

## Media Player and the WAV library

- **`userland/lib/wav.c`** parses a WAV file held in memory and decodes any
  stretch of it to 16-bit samples: PCM 8-bit (unsigned), 16, 24 and 32-bit
  (and smaller sample sizes in those containers, such as 20 bits in 3 bytes),
  IEEE float 32 and 64-bit (clipped; NaN is silence), IMA ADPCM (random access
  by block), G.711 mu-law and A-law, also as `WAVE_FORMAT_EXTENSIBLE`; 1 to 32
  channels, more than two mixed down to stereo (front, centre at -3 dB, LFE
  dropped, back and side at -3 dB); any rate from 100 Hz to 1 MHz
  (`wav_stream_rate` picks a whole multiple or fraction the mixer takes).
  Chunks may come in any order, odd chunks are padded, the RIFF size is not
  trusted, a file cut short plays what it has. Everything else is refused
  with a reason (`wav_tag_name`: MP3, Microsoft ADPCM, GSM...). No allocation
  and no C library, so the same file builds for Halcyon and for the host
  tests. It is linked only into the programs that use it.
- **Media Player** (`MPLAYER.EXE`, Start menu; Explorer and the desktop open
  `.wav` files with it): open (File > Open, Ctrl+O, or a file on the command
  line), play / pause (Space), stop, a seek bar (click or drag), skip 5 s
  (arrow keys), position and length, a two-channel level meter, its own
  stream volume (slider, wheel, Up / Down), and the file's format. One
  thread: `poll()` waits for window events and for room in the non-blocking
  stream at once; the file is decoded a few thousand frames at a time, and the
  stream is filled while paused before it starts or after a seek, so it never
  starts dry. The position is the stream's played frames since the last seek.
- **`PLAY.EXE`** (Command Prompt): `PLAY file.wav [/V percent]` plays a file
  and waits for it; `PLAY /E event` plays a system sound; `PLAY /VOLUME
  [percent|MUTE|UNMUTE]` shows or sets (and saves) the master volume; `PLAY`
  alone lists the sound cards and the volume.

## Tests

- **Host** (`tests/sound/run.sh`, in `tools/ci.sh`): the WAV library, built
  with ASan and UBSan, against known values and independent encoders written
  in the test from the standards (G.711 mu-law and A-law over every 8th
  16-bit value; IMA ADPCM, whose decoded output must equal the encoder's own
  reconstruction, mono and stereo, sequentially in odd pieces and at random
  positions, with a `fact` chunk and a cut-off last block), PCM of every size,
  float with clipping and NaN, 5.1 extensible mix-down, odd and truncated
  layouts, every rejection, and 200,000 mutated files decoded at random
  positions. The system sounds made by `mksounds.py` must parse as 16-bit PCM
  and the demo piece as IMA ADPCM, neither silent, clipped nor noisy.
- **`SNDTEST.EXE`** (boot test variant `audio`): device and system
  information; bad arguments (rates, channels, sample sizes, buffer, flags,
  partial frames, bad pointers, unknown controls, other handle types);
  `write()` and `close()` on a stream; non-blocking writes that fill the
  buffer and then get `E_AGAIN`; `poll()` without and with room and on a
  closed handle; discard; a blocking write of 500 ms into a 100 ms buffer
  (must take at least 250 ms), drain timing and played = written, writing
  after a drain, a drain that times out on a paused stream; pause holding
  the position; close-and-wait; the per-program limit (8, then `E_BUSY`) and
  the system-wide one (three copies holding 8 each); a copy that ends with
  open streams and one ended while it waits in a write (its streams must be
  gone at once); the master volume and mute (restored); events (bad ids,
  query). Then, after the startup sound, it plays a 660 Hz sine as 8-bit mono
  at 32 kHz for 1 s and drains it: no underruns, about 1 s.
- **QEMU boot tests** (`tools/boottest.py`) record the sound card's output to
  a WAV file. In every sound variant the recording must start with the
  startup sound: its loudness envelope in 10 ms blocks must follow
  `STARTUP.WAV`'s (correlation at least 0.95) at the same gain as the test
  signal that follows (the master volume, and QEMU's AC'97 codec, which takes
  off another 2.5 dB, apply to both). `ac97` then checks the kernel's test
  signal (2 s of 440 Hz and 0.25 s of 1 kHz at the right pitch and length,
  no dropouts, no underruns, the expected interrupt); `audio` checks
  SNDTEST's report and its 660 Hz signal (1 s ± 30 ms, ±1.5 Hz, no
  dropouts). Both are in the default matrix.

## Limitations

- Output only: no recording (150-151 are kept for it), no MIDI synthesizer,
  no MP3 / AAC / Vorbis / FLAC or other compressed formats (WAV's IMA ADPCM,
  mu-law and A-law only), no USB audio.
- One output: every stream plays on the default (first) card; there is no
  per-program device choice and no per-program volume mixer (each stream has
  its own volume).
- Resampling is linear interpolation (no anti-aliasing filter), good enough
  for system sounds and speech, audible on high-pitched music at low rates.
  Media Player plays files above 192 kHz by averaging whole frames and below
  8 kHz by repeating them.
- Latency is the stream's buffer plus a few device periods (about 40 ms with
  AC'97); there is no low-latency path.
- `drain()` returns once the last frame has been mixed into the device's
  ring, a few periods before it is heard.
