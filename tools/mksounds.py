#!/usr/bin/env python3
"""Synthesize Halcyon's system sounds and write them as 16-bit PCM WAV files.

usage: mksounds.py OUTDIR

Every sound is made here from scratch: FM bells and mallets (a sine carrier
phase-modulated by a sine at a fixed ratio, the modulation index and the
amplitude decaying), additive pads (harmonics with slightly detuned voices
and slow envelopes), gliding blips, and a small Schroeder reverb (four damped
comb filters and two all-passes per channel, with different delays left and
right for width). Deterministic: the same script makes the same files.

The sounds are Halcyon's own: a rising F-major-9 arpeggio of glassy bells
over a slow pad for startup, a falling D-minor figure (the relative minor)
for shutdown, and
short motifs for the message box icons, balloons and USB devices. The
"Halcyon Dusk" scheme adds a longer startup (A-flat major: IV, V, then a
wide I-add9 with a noise swell and echoing shimmer) and shutdown (F minor:
falling bells, iv, V7, i) as DUSKSTRT.WAV and DUSKSHUT.WAV. They do
not copy or imitate any other system's sounds.

All files are 22050 Hz (startup and shutdown stereo, the rest mono), so the
set, Dusk included, stays near 2 MB on the boot disk. C:\\HALCYON\\MEDIA\\ gets them
(tools/mkimage.sh); kernel/sound/sndevent.c names them. A 13-second demo
piece for Media Player, "Halcyon Dawn", goes to OUTDIR/music as 4-bit IMA
ADPCM (encoded here) and from there to C:\\Documents\\Music.
"""
import math
import os
import struct
import sys

RATE = 22050
TAU = 2 * math.pi


def note(name):
    """'A4' -> Hz (equal temperament, A4 = 440)"""
    names = {"C": -9, "C#": -8, "D": -7, "D#": -6, "E": -5, "F": -4, "F#": -3, "G": -2, "G#": -1, "A": 0, "A#": 1, "B": 2}
    pitch, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((names[pitch] + 12 * (octave - 4)) / 12)


class Track:
    """a mono or stereo buffer of floats that voices are added into"""

    def __init__(self, seconds, stereo=False):
        self.n = int(seconds * RATE)
        self.ch = [[0.0] * self.n for _ in range(2 if stereo else 1)]

    def add(self, start, samples, gain=1.0, pan=0.0):
        """samples at time start (s); pan -1 left .. 1 right (stereo only)"""
        i0 = int(start * RATE)
        if len(self.ch) == 1:
            gains = [gain]
        else:
            a = (pan + 1) * math.pi / 4          # constant power
            gains = [gain * math.cos(a) * 1.4142, gain * math.sin(a) * 1.4142]
        for c, g in zip(self.ch, gains):
            for k, v in enumerate(samples):
                i = i0 + k
                if i >= self.n:
                    break
                c[i] += v * g


def env_ad(t, attack, decay):
    """fast attack, exponential decay (time constant decay s)"""
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def fm_bell(freq, dur, ratio=3.5, index=2.2, decay=0.6, idecay=0.25, attack=0.003, bend=0.0):
    """FM bell / mallet: index decays faster than the amplitude (bright onset, pure tail)"""
    out = []
    n = int(dur * RATE)
    ph_c = ph_m = 0.0
    for k in range(n):
        t = k / RATE
        f = freq * (1 + bend * math.exp(-t / 0.05))
        ph_c += TAU * f / RATE
        ph_m += TAU * f * ratio / RATE
        a = env_ad(t, attack, decay)
        i = index * math.exp(-t / idecay)
        out.append(a * math.sin(ph_c + i * math.sin(ph_m)))
    fade(out, 0.02)
    return out


def pad(freqs, dur, attack=1.0, release=1.2, harmonics=4, detune=0.004, bright=1.6):
    """additive pad: each note's harmonics (1/n^bright) in two voices a little apart in pitch"""
    n = int(dur * RATE)
    parts = []
    for f in freqs:
        for v, d in enumerate((-detune, detune)):
            for h in range(1, harmonics + 1):
                if f * h * (1 + d) > RATE * 0.45:
                    continue
                parts.append((TAU * f * h * (1 + d) / RATE, 1.0 / h ** bright, (v * 1.3 + h * 0.7) % TAU))
    out = [0.0] * n
    for w, amp, ph in parts:
        for k in range(n):
            out[k] += amp * math.sin(ph + w * k)
    norm = 1.0 / max(1, len(freqs) * 2)
    for k in range(n):
        t = k / RATE
        e = min(1.0, t / attack) if attack else 1.0
        e = e * e * (3 - 2 * e)                  # smooth swell
        tail = dur - t
        if tail < release:
            r = tail / release
            e *= r * r
        out[k] *= e * norm
    return out


def blip(f0, f1, dur, harm=0.25):
    """a sine gliding from f0 to f1 with a little second harmonic, soft attack and decay"""
    out = []
    ph = 0.0
    n = int(dur * RATE)
    for k in range(n):
        t = k / RATE
        x = t / dur
        f = f0 * (f1 / f0) ** min(1.0, x * 1.6)
        ph += TAU * f / RATE
        a = min(1.0, t / 0.006) * math.exp(-t / (dur * 0.35))
        out.append(a * (math.sin(ph) + harm * math.sin(2 * ph)))
    fade(out, 0.02)
    return out


def fade(buf, seconds):
    n = min(len(buf), int(seconds * RATE))
    for k in range(n):
        buf[len(buf) - n + k] *= 1 - (k + 1) / n


def lowpass(buf, cutoff):
    """one-pole low-pass (softens FM brightness)"""
    a = math.exp(-TAU * cutoff / RATE)
    y = 0.0
    for k, x in enumerate(buf):
        y = (1 - a) * x + a * y
        buf[k] = y


def reverb(track, wet=0.25, size=1.0, damp=0.35, feedback=0.78):
    """Schroeder reverb: damped combs in parallel, then all-passes; the right
    channel's delays are a few samples longer (stereo width)"""
    combs = [1116, 1188, 1277, 1356]              # samples at 44.1 kHz, scaled below
    allp = [556, 341]
    for ci, c in enumerate(track.ch):
        spread = 23 * ci
        dry = c[:]
        acc = [0.0] * track.n
        for d0 in combs:
            d = max(1, int((d0 + spread) * size * RATE / 44100))
            line = [0.0] * d
            pos = 0
            store = 0.0
            for k in range(track.n):
                out = line[pos]
                store = out * (1 - damp) + store * damp
                line[pos] = dry[k] + store * feedback
                pos = pos + 1 if pos + 1 < d else 0
                acc[k] += out
        for d0 in allp:
            d = max(1, int((d0 + spread) * size * RATE / 44100))
            line = [0.0] * d
            pos = 0
            for k in range(track.n):
                b = line[pos]
                x = acc[k]
                line[pos] = x + b * 0.5
                acc[k] = b - x * 0.5
                pos = pos + 1 if pos + 1 < d else 0
        for k in range(track.n):
            c[k] = dry[k] + acc[k] * wet * 0.25


def noise_sweep(dur, f0, f1, rise=True, seed=1):
    """white noise through a low-pass whose cutoff glides f0 -> f1; swells in
    (rise) or dies away; deterministic"""
    out = []
    n = int(dur * RATE)
    x = seed
    y1 = y2 = 0.0
    for k in range(n):
        p = k / n
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        v = x / 0x3FFFFFFF - 1.0
        fc = f0 * (f1 / f0) ** p
        a = math.exp(-TAU * fc / RATE)
        y1 = (1 - a) * v + a * y1             # two poles: a softer hiss
        y2 = (1 - a) * y1 + a * y2
        e = p * p * p if rise else (1 - p) ** 2
        out.append(y2 * e * 2.5)
    fade(out, 0.05)
    return out


def sub(freq, dur, attack=0.02, decay=1.2, drop=0.0):
    """a sine sub bass with a little drop in pitch at the start"""
    out = []
    ph = 0.0
    n = int(dur * RATE)
    for k in range(n):
        t = k / RATE
        f = freq * (1 + drop * math.exp(-t / 0.08))
        ph += TAU * f / RATE
        a = min(1.0, t / attack) * math.exp(-t / decay)
        out.append(a * (math.sin(ph) + 0.15 * math.sin(2 * ph)))
    fade(out, 0.05)
    return out


def pingpong(track, delay, feedback=0.35, wet=0.3):
    """an echo that bounces between left and right (stereo tracks)"""
    if len(track.ch) != 2:
        return
    d = int(delay * RATE)
    L, R = track.ch
    el, er = [0.0] * track.n, [0.0] * track.n
    for k in range(d, track.n):
        el[k] = R[k - d] * 0.5 + L[k - d] * 0.2 + er[k - d] * feedback
        er[k] = L[k - d] * 0.5 + R[k - d] * 0.2 + el[k - d] * feedback
    for k in range(track.n):
        L[k] += el[k] * wet
        R[k] += er[k] * wet


def write_wav(path, track, peak=0.72):
    """normalise to peak (of full scale) and write 16-bit PCM"""
    m = max(max(abs(v) for v in c) for c in track.ch) or 1.0
    g = peak * 32767 / m
    nch = len(track.ch)
    for c in track.ch:
        fade(c, 0.03)
    frames = bytearray()
    for k in range(track.n):
        for c in track.ch:
            v = int(round(c[k] * g))
            frames += struct.pack("<h", max(-32768, min(32767, v)))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, nch, RATE, RATE * 2 * nch, 2 * nch, 16))
        f.write(b"data" + struct.pack("<I", len(frames)) + frames)


# ---------------------------------------------------------------- the sounds
def s_startup():
    """a rising F-major-9 arpeggio of glass bells over a slow pad, ending in a bell chord"""
    t = Track(4.6, stereo=True)
    t.add(0.0, pad([note("F3"), note("C4"), note("A4"), note("G5")], 4.0, attack=1.3, release=1.6, harmonics=5), 0.55)
    arp = [("F4", 0.00, -0.5), ("C5", 0.17, -0.25), ("F5", 0.34, 0.0), ("G5", 0.51, 0.25), ("C6", 0.72, 0.5)]
    for name, at, pan in arp:
        b = fm_bell(note(name), 1.8, ratio=3.5, index=1.8, decay=0.55, idecay=0.18)
        lowpass(b, 5000)
        t.add(at, b, 0.32, pan)
    for name, pan in (("A5", -0.35), ("C6", 0.35), ("F6", 0.0)):
        b = fm_bell(note(name), 2.6, ratio=2.0, index=1.2, decay=0.9, idecay=0.4)
        lowpass(b, 6000)
        t.add(1.12, b, 0.22, pan)
    reverb(t, wet=0.35, size=1.1)
    return t


def s_shutdown():
    """the startup colours falling in the relative minor: D-minor bells down to a low D-minor chord"""
    t = Track(2.7, stereo=True)
    t.add(0.0, pad([note("D3"), note("A3"), note("D4"), note("F4")], 2.3, attack=0.25, release=1.6, harmonics=4), 0.5)
    for name, at, pan in (("F5", 0.0, 0.4), ("D5", 0.19, 0.0), ("A4", 0.38, -0.4)):
        b = fm_bell(note(name), 1.6, ratio=3.5, index=1.6, decay=0.5, idecay=0.2)
        lowpass(b, 5000)
        t.add(at, b, 0.32, pan)
    b = fm_bell(note("D4"), 1.9, ratio=2.0, index=1.0, decay=0.7, idecay=0.3)
    t.add(0.6, b, 0.3, 0.0)
    reverb(t, wet=0.35, size=1.1)
    return t


def dusk_bell(name, dur=1.8, bright=1.8):
    b = fm_bell(note(name), dur, ratio=3.5, index=bright, decay=0.6, idecay=0.2)
    lowpass(b, 5500)
    return b


def s_dusk_startup():
    """Halcyon Dusk, A-flat major (8 s): IV (D-flat maj9) and V (E-flat
    sus4 -> E-flat) under rising bells and a noise swell, landing on a wide
    A-flat add9 with sub bass, a bell chord and an echoing shimmer"""
    t = Track(8.0, stereo=True)
    # IV: D-flat major 9
    t.add(0.0, pad([note(n) for n in ("C#3", "G#3", "C4", "F4", "D#5")], 1.9, attack=0.9, release=0.6,
                   harmonics=5, detune=0.006), 0.5)
    t.add(0.0, sub(note("C#2"), 1.6, attack=0.3, decay=1.0), 0.35)
    for name, at, pan in (("C#5", 0.15, -0.6), ("F5", 0.40, -0.3), ("G#5", 0.65, 0.0), ("C6", 0.90, 0.3)):
        t.add(at, dusk_bell(name), 0.26, pan)
    # V: E-flat sus4 resolving to E-flat
    t.add(1.5, pad([note(n) for n in ("D#3", "A#3", "G#4", "D#5")], 0.85, attack=0.15, release=0.3,
                   harmonics=5, detune=0.006), 0.5)
    t.add(2.25, pad([note(n) for n in ("D#3", "A#3", "G4", "D#5")], 0.85, attack=0.1, release=0.25,
                    harmonics=5, detune=0.006), 0.5)
    t.add(1.5, sub(note("D#2"), 1.5, attack=0.05, decay=1.0), 0.35)
    for name, at, pan in (("D#5", 1.60, 0.6), ("G5", 1.85, 0.3), ("A#5", 2.10, 0.0), ("D#6", 2.35, -0.3),
                          ("G6", 2.60, -0.6)):
        t.add(at, dusk_bell(name, 1.4), 0.22, pan)
    sw = noise_sweep(2.2, 200, 7000, rise=True, seed=7)
    t.add(0.8, sw, 0.05, -0.2)
    t.add(0.8, noise_sweep(2.2, 220, 7600, rise=True, seed=11), 0.05, 0.2)
    # I: A-flat add9, wide
    t.add(3.0, sub(note("G#1"), 4.2, attack=0.01, decay=1.6, drop=0.25), 0.6)
    t.add(3.0, pad([note(n) for n in ("G#2", "D#3", "G#3", "C4", "A#4", "C5")], 5.0, attack=0.04, release=3.2,
                   harmonics=6, detune=0.007, bright=1.3), 0.55)
    for name, pan in (("C5", -0.7), ("D#5", 0.7), ("G#5", -0.35), ("C6", 0.35), ("G#6", 0.0)):
        b = fm_bell(note(name), 3.4, ratio=2.0, index=1.3, decay=1.2, idecay=0.5)
        lowpass(b, 6500)
        t.add(3.0, b, 0.2, pan)
    for k, (name, at) in enumerate((("G#6", 3.45), ("C7", 3.75), ("A#6", 4.05), ("G6", 4.35), ("D#6", 4.8),
                                    ("G#6", 5.3))):
        b = fm_bell(note(name), 1.2, ratio=3.5, index=1.0, decay=0.35, idecay=0.1)
        lowpass(b, 7000)
        t.add(at, b, 0.09 - k * 0.008, -0.5 if k % 2 else 0.5)
    pingpong(t, 0.27, feedback=0.4, wet=0.35)
    reverb(t, wet=0.45, size=1.35, feedback=0.82)
    return t


def s_dusk_shutdown():
    """Halcyon Dusk, F minor (6 s): F-minor-7 bells falling over a closing
    noise sweep, then iv (D-flat maj7), V7 (C7) and a low F-minor chord"""
    t = Track(6.0, stereo=True)
    t.add(0.0, noise_sweep(1.4, 6000, 250, rise=False, seed=3), 0.06, 0.3)
    t.add(0.0, noise_sweep(1.4, 6400, 260, rise=False, seed=5), 0.06, -0.3)
    t.add(0.0, pad([note(n) for n in ("F3", "C4", "G#4", "D#5")], 1.6, attack=0.15, release=0.5,
                   harmonics=5, detune=0.006), 0.5)
    for name, at, pan in (("C6", 0.0, 0.6), ("G#5", 0.17, 0.3), ("F5", 0.34, 0.0), ("D#5", 0.51, -0.3),
                          ("C5", 0.68, -0.6)):
        t.add(at, dusk_bell(name, 1.5), 0.25, pan)
    # iv: D-flat major 7
    t.add(1.4, pad([note(n) for n in ("C#3", "G#3", "C4", "F4")], 1.25, attack=0.2, release=0.4,
                   harmonics=5, detune=0.006), 0.5)
    t.add(1.4, sub(note("C#2"), 1.2, attack=0.05, decay=0.9), 0.35)
    t.add(1.45, dusk_bell("C5", 1.5), 0.24, 0.3)
    t.add(1.75, dusk_bell("G#4", 1.5), 0.24, -0.3)
    # V7: C7 (the E natural pulls back to F)
    t.add(2.5, pad([note(n) for n in ("C3", "G3", "E4", "A#4")], 0.9, attack=0.1, release=0.35,
                   harmonics=5, detune=0.006), 0.5)
    t.add(2.5, sub(note("C2"), 0.9, attack=0.03, decay=0.8), 0.35)
    t.add(2.55, dusk_bell("E5", 1.3), 0.24, 0.4)
    t.add(2.85, dusk_bell("G4", 1.3), 0.22, -0.4)
    # i: F minor, low and long
    t.add(3.3, sub(note("F1"), 2.7, attack=0.01, decay=1.3, drop=0.2), 0.6)
    t.add(3.3, pad([note(n) for n in ("F2", "C3", "F3", "G#3", "C4")], 2.7, attack=0.04, release=2.3,
                   harmonics=6, detune=0.007, bright=1.4), 0.55)
    for name, pan, g in (("F4", -0.3, 0.24), ("C5", 0.3, 0.18), ("G#5", 0.0, 0.12)):
        b = fm_bell(note(name), 2.6, ratio=2.0, index=1.1, decay=1.0, idecay=0.45)
        lowpass(b, 5000)
        t.add(3.3, b, g, pan)
    pingpong(t, 0.31, feedback=0.35, wet=0.3)
    reverb(t, wet=0.42, size=1.3, feedback=0.8)
    return t


def mono(dur, voices, wet=0.18):
    t = Track(dur)
    for at, samples, gain in voices:
        t.add(at, samples, gain)
    reverb(t, wet=wet, size=0.7)
    return t


def s_error():
    """two low inharmonic knocks, the second lower"""
    a = fm_bell(233.0, 0.45, ratio=1.41, index=3.0, decay=0.12, idecay=0.08)
    b = fm_bell(174.6, 0.5, ratio=1.41, index=3.0, decay=0.15, idecay=0.08)
    lowpass(a, 2500)
    lowpass(b, 2500)
    return mono(0.7, [(0.0, a, 0.8), (0.13, b, 0.9)])


def s_warning():
    """one mid bell with a small downward bend"""
    a = fm_bell(note("C#5"), 0.65, ratio=2.76, index=2.0, decay=0.22, idecay=0.1, bend=0.03)
    lowpass(a, 4500)
    return mono(0.75, [(0.0, a, 0.9)])


def s_info():
    """two light mallet notes rising"""
    a = fm_bell(note("B5"), 0.4, ratio=4.0, index=1.4, decay=0.12, idecay=0.04)
    b = fm_bell(note("E6"), 0.5, ratio=4.0, index=1.4, decay=0.16, idecay=0.04)
    return mono(0.6, [(0.0, a, 0.7), (0.09, b, 0.8)])


def s_question():
    """three soft notes rising, like a voice asking"""
    v = []
    for i, (name, at) in enumerate((("G5", 0.0), ("A5", 0.09), ("D6", 0.18))):
        b = fm_bell(note(name), 0.45, ratio=1.0, index=1.0, decay=0.13 + i * 0.04, idecay=0.05)
        v.append((at, b, 0.6 + i * 0.1))
    return mono(0.7, v)


def s_notify():
    """a glassy double ding"""
    a = fm_bell(note("E6"), 0.5, ratio=3.5, index=1.2, decay=0.14, idecay=0.05)
    b = fm_bell(note("G#6"), 0.6, ratio=3.5, index=1.2, decay=0.2, idecay=0.05)
    return mono(0.7, [(0.0, a, 0.6), (0.12, b, 0.6)], wet=0.25)


def s_devadd():
    """two blips gliding up"""
    return mono(0.45, [(0.0, blip(600, 720, 0.14), 0.7), (0.11, blip(900, 1080, 0.2), 0.7)])


def s_devremove():
    """two blips gliding down"""
    return mono(0.45, [(0.0, blip(1080, 900, 0.14), 0.7), (0.11, blip(720, 600, 0.2), 0.7)])


def s_default():
    """a single short soft ding"""
    a = fm_bell(note("A5"), 0.3, ratio=2.0, index=0.8, decay=0.08, idecay=0.03)
    return mono(0.32, [(0.0, a, 0.8)], wet=0.12)


def s_demo():
    """"Halcyon Dawn", a short piece for Media Player: four chords (D, Bm, G,
    A) of pad, bass and bell arpeggios, ending on D"""
    t = Track(13.0, stereo=True)
    chords = [("D3", ["D4", "F#4", "A4", "E5"], ["D5", "F#5", "A5", "E6", "A5", "F#5"]),
              ("B2", ["B3", "D4", "F#4", "A4"], ["B4", "D5", "F#5", "A5", "F#5", "D5"]),
              ("G2", ["G3", "B3", "D4", "F#4"], ["G4", "B4", "D5", "F#5", "D5", "B4"]),
              ("A2", ["A3", "C#4", "E4", "B4"], ["A4", "C#5", "E5", "B5", "E5", "C#5"])]
    step = 0.375
    for i, (bass, padn, arp) in enumerate(chords):
        at = i * 3.0
        t.add(at, pad([note(n) for n in padn], 3.6, attack=0.6, release=0.9, harmonics=4), 0.45)
        t.add(at, blip(note(bass), note(bass), 3.0, harm=0.35), 0.35)
        for k in range(8):
            bell = fm_bell(note(arp[k % len(arp)]), 1.2, ratio=3.5, index=1.4, decay=0.4, idecay=0.12)
            lowpass(bell, 5000)
            t.add(at + k * step, bell, 0.16 if k % 2 else 0.22, -0.5 + (k % 4) * 0.33)
    for name, pan in (("D5", -0.3), ("F#5", 0.3), ("A5", 0.0)):
        t.add(12.0, fm_bell(note(name), 1.0, ratio=2.0, index=1.0, decay=0.5, idecay=0.3), 0.2, pan)
    t.add(12.0, pad([note("D3"), note("A3"), note("F#4")], 1.0, attack=0.05, release=0.95, harmonics=3), 0.4)
    reverb(t, wet=0.3, size=1.0)
    return t


IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
             107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
             876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
             4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
             22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def write_ima_wav(path, track, peak=0.7, block=1024):
    """normalise and write as 4-bit IMA ADPCM (the WAV layout: each block has
    a header per channel, then 4 bytes = 8 samples of each channel in turn)"""
    m = max(max(abs(v) for v in c) for c in track.ch) or 1.0
    g = peak * 32767 / m
    nch = len(track.ch)
    for c in track.ch:
        fade(c, 0.05)
    pcm = [[max(-32768, min(32767, int(round(v * g)))) for v in c] for c in track.ch]
    fpb = (block - 4 * nch) * 2 // nch + 1
    pred = [0] * nch
    index = [0] * nch
    data = bytearray()
    n = track.n
    for f0 in range(0, n, fpb):
        for c in range(nch):
            pred[c] = pcm[c][f0]
            data += struct.pack("<hBB", pred[c], index[c], 0)
        for g8 in range((fpb - 1) // 8):
            for c in range(nch):
                codes = []
                for k in range(8):
                    f = f0 + 1 + g8 * 8 + k
                    x = pcm[c][f] if f < n else 0
                    step = IMA_STEPS[index[c]]
                    diff = x - pred[c]
                    code = 0
                    if diff < 0:
                        code, diff = 8, -diff
                    vp = step >> 3
                    if diff >= step:
                        code |= 4
                        diff -= step
                        vp += step
                    step >>= 1
                    if diff >= step:
                        code |= 2
                        diff -= step
                        vp += step
                    step >>= 1
                    if diff >= step:
                        code |= 1
                        vp += step
                    pred[c] = max(-32768, min(32767, pred[c] - vp if code & 8 else pred[c] + vp))
                    index[c] = max(0, min(88, index[c] + IMA_INDEX[code & 7]))
                    codes.append(code)
                data += bytes(codes[2 * j] | codes[2 * j + 1] << 4 for j in range(4))
    fmt = struct.pack("<HHIIHHHH", 0x11, nch, RATE, RATE * block // fpb, block, 4, 2, fpb)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 12 + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"fact" + struct.pack("<II", 4, n))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


SOUNDS = [("STARTUP.WAV", s_startup), ("SHUTDOWN.WAV", s_shutdown), ("ERROR.WAV", s_error),
          ("WARNING.WAV", s_warning), ("INFO.WAV", s_info), ("QUESTION.WAV", s_question),
          ("NOTIFY.WAV", s_notify), ("DEVADD.WAV", s_devadd), ("DEVREM.WAV", s_devremove),
          ("DEFAULT.WAV", s_default), ("DUSKSTRT.WAV", s_dusk_startup), ("DUSKSHUT.WAV", s_dusk_shutdown)]


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in SOUNDS:
        write_wav(os.path.join(out, name), fn(), peak=0.72 if name in ("STARTUP.WAV", "SHUTDOWN.WAV", "DUSKSTRT.WAV", "DUSKSHUT.WAV") else 0.6)
    # a demo for Media Player (C:\Documents\Music), as IMA ADPCM
    os.makedirs(os.path.join(out, "music"), exist_ok=True)
    write_ima_wav(os.path.join(out, "music", "Halcyon Dawn.wav"), s_demo())


if __name__ == "__main__":
    main()
