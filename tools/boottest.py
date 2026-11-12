#!/usr/bin/env python3
"""Boot Halcyon in QEMU for a matrix of USB controllers / firmware and check
the kernel serial log.

usage: tools/boottest.py [CONFIG ...]     CONFIG = hc:fw[:variant[+variant]], e.g. xhci:uefi,
       xhci:bios:nvme+nomsi (default: the full matrix)

Each boot must reach "gui: desktop ready", mount the boot stick as C:, mount
the SATA test disks (FAT32/GPT and exFAT/MBR), write \\HALCYON\\BOOT.LOG, and
log no exception/panic. A USB keyboard (QEMU usb-kbd) and tablet are attached
on the controller under test: the keyboard must run in HID report protocol,
and Caps Lock pressed through the QEMU monitor must come back as a keyboard
LED output report (key report in, LED report out).
With interrupts on, the idle desktop must also leave QEMU mostly idle (the
guest halts the CPU), and the USB controller must get a working interrupt.
Variants add boot flags through QEMU fw_cfg (see qemu_boot_flags()):
  pic    legacy 8259 PICs instead of the I/O APIC
  nomsi  no MSI: xHCI / AHCI / NVMe fall back to (learned) INTx
  noirq  no interrupts at all: the old fully polled mode
or replace the SATA disk with an NVMe disk (a scratch copy of a test image),
which must mount as a volume "on nvme0" without NVMe errors and, with
interrupts on, get a working interrupt:
  nvme    fat32gpt.img on an NVMe controller (512-byte LBAs)
  nvme4k  fat32_4k.img on an NVMe namespace with 4096-byte LBAs
  user    run C:\\PROGRAMS\\SELFTEST.EXE after boot (QEMU fw_cfg autorun): user-mode
          system calls, crashing / looping child programs, windows; it must
          report "selftest: all N checks passed"
  sock    run C:\\PROGRAMS\\SOCKTEST.EXE after boot: local stream and datagram
          sockets between programs, poll, limits, peers that crash or are
          ended; it must report "socktest: all N checks passed"
  web     run C:\\PROGRAMS\\WEBTEST.EXE after boot: TLS 1.2 handshakes through
          BearSSL as built for Halcyon (several cipher suites, RSA and EC
          certificates, rejected certificates), inflate, URLs, the clock, and a
          fetch on the system's network; it must report "webtest: all N checks passed"
  net     an Intel e1000 on QEMU's user network, with servers on the host
          (tools/netservers.py: DNS, HTTP, a TCP sink, UDP echo; the DNS
          server's port goes into net.dns in SETTINGS.INI): runs
          C:\\PROGRAMS\\NETTEST.EXE (DHCP, ping, DNS, a 32 MiB download and an
          8 MiB upload with CRCs, 40 parallel connections, UDP, the loopback,
          errors, and a server in Halcyon that the host connects to through
          hostfwd); it must report "nettest: all N checks passed"
          (net+virtio etc.: the same on another adapter of the list below)
  loss    (with net) 3% of frames dropped on receive and transmit by the
          stack's test hook (fw_cfg opt/halcyon/netloss): a shorter NETTEST
          run that must still pass, with retransmissions
  res     a saved resolution (display.width/height = 1280 x 1024 in
          \\HALCYON\\SETTINGS.INI): the loader must choose it and the desktop use it
  fwvideo no display adapter driver (boot flag): the firmware frame buffer only
  switch  switch the display mode to 1600 x 900 once the desktop is up (QEMU
          fw_cfg test hook; needs the live QEMU/Bochs display driver)
  ntfsrw  an internal SATA disk (one MBR partition of type 7) with an NTFS
          volume made on the host by mkntfs and ntfs-3g (tests/host/
          ntfs_mkimg.py, needs root): it has \\Windows\\System32, so it
          mounts read-only until allowed; C:\\PROGRAMS\\NTFSTEST.EXE checks
          that, reads HELLO.TXT and Folder\\data.bin (its CRC-32 is
          compared), allows writing (volume_set_write: SETTINGS.INI and a
          remount), creates, moves and deletes files, then takes the
          permission back (the volume is synced, its dirty flag cleared);
          it must report "ntfstest: all N checks passed", and the volume
          must pass ntfsfix -n and ntfs_check.py and show the changes when
          mounted by ntfs-3g
  ntfs    a blank 256 MiB SATA disk (model NTFSTEST, one MBR partition of
          type 0x0C) that the kernel's format test hook (fw_cfg formattest)
          formats as NTFS with 4 KiB clusters through vfs_format once the
          desktop is up; afterwards the partition table must say 0x07 and the
          partition must pass ntfsfix -n, ntfsinfo -m (label, cluster size)
          and tests/host/ntfs_tool.py check on the host (ntfs-3g tools)
  saver   the screen saver test hook (fw_cfg screensaver = "5:STARS.SCR"), with
          GUIDEMO.EXE running (it logs the input it gets): after 5 s without
          input Starfield must start full screen and a screendump be mostly
          black with stars; a click on GUIDEMO's window must end it without
          reaching GUIDEMO (press or release) and the desktop come back, the
          next click must reach it; started again, a key must end it, and
          the third time a mouse movement
  cd      also an AHCI CD-ROM drive with an ISO 9660 + Joliet test disc
          (built with xorriso): the disc must mount, and after the desktop is
          up the disc is ejected and another one inserted through the QEMU
          monitor; the volume must go away and the new one mount
or attach a network adapter on QEMU's user-mode network (-netdev user):
  virtio        virtio-net-pci (transitional: the virtio 1.0 interface is used)
  virtiolegacy  virtio-net-pci with disable-modern=on (the legacy I/O interface)
  virtiomodern  virtio-net-pci-non-transitional (virtio 1.0 only)
  e1000         Intel 82540EM          e1000e   Intel 82574L
  igb           Intel 82576            rtl8139  Realtek RTL8139C+
          The adapter's driver must register it as eth0 with its link up; the
          kernel's net probe (QEMU fw_cfg netprobe) sends 3000 ARP requests for
          10.0.2.2 (at most 32 outstanding) and then a burst of 750 as fast as
          the transmit ring takes them: the gateway's reply must arrive, every
          request must be answered and reported transmitted, the burst must
          be transmitted (replies to it may be dropped when the receive ring
          overruns), and the adapter must have the expected interrupt (MSI-X,
          INTx, or polled with noirq).
or attach a sound card, its output recorded by QEMU's wav backend (48 kHz):
  ac97          Intel 82801AA AC'97
  virtiosnd     virtio-sound-pci (virtio 1.2 sound device; MSI-X)
  es1370        Ensoniq AudioPCI ES1370 (plays at 44.1 kHz)
  sb16          Creative Sound Blaster 16 (ISA, IRQ 5, DMA 5; plays at 44.1 kHz)
  hda           Intel HD Audio, ICH6 (intel-hda) with QEMU's hda-duplex codec
                (line out, line in)
  hda9          Intel HD Audio, ICH9 (ich9-intel-hda) with hda-output (line out)
  hdamicro      Intel HD Audio, ICH6 with hda-micro (speaker, microphone)
  hdaimm        as hda, with the driver's test hook making every verb written
                to the CORB time out: the driver must fall back to the
                immediate command interface (not in the default matrix)
  hdalpib       ICH9 with hda-micro, the DMA position buffer off by the test
                hook: positions from LPIB (not in the default matrix)
          The card's driver must register it as snd0. The recording must
          start with the startup sound (C:\\HALCYON\\MEDIA\\STARTUP.WAV,
          played when the desktop is ready): its loudness envelope must match
          the file's, at the master volume's gain. Then the kernel's sound
          test (QEMU fw_cfg soundtest, which waits for the startup sound to
          end) plays a 440 Hz sine at 44.1 kHz for 2 s through a stream,
          then 100 ms later a 1 kHz triangle at 22.05 kHz for 0.25 s as a
          static sound: the log must report no underruns and the expected
          interrupt (live, or polled with noirq), and the recording must hold
          both, at the right pitch and length, without dropouts. (QEMU
          resamples a 44.1 kHz card to the recording's 48 kHz; the pitch
          still comes out within 1 Hz.)
  beep    instead of the sound test, the beep test (soundtest = "beep"):
          snd_beep(880, 300), 4.3 s later (the device has been stopped and
          must start again) snd_beep(660, 200). With a sound card (e.g.
          ac97+beep) both go through it; without one (just beep) through the
          PC speaker, recorded with -machine pcspk-audiodev. The recording
          must hold both beeps at the right pitch and length.
  audio   an AC'97 card recorded as above, and C:\\PROGRAMS\\SNDTEST.EXE run
          after boot: the sound system calls (formats, limits, bad
          arguments, blocking and non-blocking writes, drain timing, pause,
          volumes, poll, a child that ends with open streams, one ended while
          it waits); it must report "sndtest: all N checks passed", and the
          recording must hold the startup sound and then SNDTEST's signal, a
          660 Hz sine written as 8-bit mono at 32 kHz for 1 s, at the right
          pitch and length without dropouts.
NETDUMP=file.pcap records the net variants' frames (128 bytes each) for debugging.
Exit status is the number of failed configurations.
"""
import os
import re
import subprocess
import sys
import threading
import time

BUILD = os.environ.get("BUILD", "build")
MATRIX = ["uhci:bios", "ohci:bios", "ehci:bios", "ich9:bios", "xhci:bios",
          "uhci:uefi", "ehci:uefi", "ich9:uefi", "xhci:uefi",
          "xhci:bios:nomsi", "uhci:bios:pic", "xhci:uefi:pic", "ehci:bios:noirq",
          "xhci:bios:nvme", "xhci:uefi:nvme4k", "xhci:bios:nvme+nomsi", "xhci:uefi:nvme+noirq",
          "xhci:bios:cd", "xhci:bios:user", "uhci:uefi:user", "xhci:bios:sock", "xhci:bios:web",
          "xhci:bios:net", "xhci:bios:net+loss",
          "xhci:bios:res", "xhci:uefi:res+fwvideo", "xhci:uefi:switch",
          "xhci:bios:virtio", "xhci:uefi:e1000e", "xhci:bios:ac97", "xhci:uefi:virtiosnd", "xhci:bios:audio",
          "xhci:uefi:hda", "xhci:bios:ntfs", "xhci:bios:ntfsrw", "xhci:bios:saver"]
VARIANTS = {"pic": 0x200, "nomsi": 0x100, "noirq": 0x80, "nvme": 0, "nvme4k": 0, "cd": 0, "user": 0, "sock": 0, "web": 0,
            "fwvideo": 0x800, "res": 0, "switch": 0, "net": 0, "loss": 0, "beep": 0, "ntfs": 0, "ntfsrw": 0,
            "saver": 0}
# network variants: QEMU device, the driver's name, whether it has MSI-X
NICS = {"virtio": ("virtio-net-pci", "virtio-net", True),
        "virtiolegacy": ("virtio-net-pci,disable-modern=on", "virtio-net", True),
        "virtiomodern": ("virtio-net-pci-non-transitional", "virtio-net", True),
        "e1000": ("e1000", "e1000", False), "e1000e": ("e1000e", "e1000e", True),
        "igb": ("igb", "igb", True), "rtl8139": ("rtl8139", "rtl8139", False)}
for _n in NICS:
    VARIANTS[_n] = 0
# sound card variants: QEMU device arguments (audiodev snd0), the driver's name
SOUNDS = {"ac97": ("-device AC97,audiodev=snd0", "ac97"),
          "virtiosnd": ("-device virtio-sound-pci,audiodev=snd0", "virtio-snd"),
          "es1370": ("-device ES1370,audiodev=snd0", "es1370"),
          "sb16": ("-device sb16,audiodev=snd0", "sb16"),
          "hda": ("-device intel-hda -device hda-duplex,audiodev=snd0", "hda"),
          "hda9": ("-device ich9-intel-hda -device hda-output,audiodev=snd0", "hda"),
          "hdamicro": ("-device intel-hda -device hda-micro,audiodev=snd0", "hda"),
          # the driver's fallbacks, through its test hook (not in the default matrix)
          "hdaimm": ("-device intel-hda -device hda-duplex,audiodev=snd0 -fw_cfg name=opt/halcyon/hda,string=corbstall", "hda"),
          "hdalpib": ("-device ich9-intel-hda -device hda-micro,audiodev=snd0 -fw_cfg name=opt/halcyon/hda,string=lpib", "hda")}
for _n in SOUNDS:
    VARIANTS[_n] = 0
VARIANTS["audio"] = 0
SOUND_TEST_MS = 2000
BEEP_AMP = 12000                         # snd_beep's sine (sound/beep.c)
SNDTEST_HZ, SNDTEST_MS = 660, 1000       # SNDTEST.EXE's signal
SNDTEST_AMP = 100 * 256                  # its amplitude: 8-bit samples 128 +- 100
MASTER_GAIN = 0.64                       # the default master volume, 80 %, squared
NET_PROBE = 3000
NVME_ERRORS = ["failed", "timed out", "fatal", "offline", "not responding", "no interrupt seen"]
IDLE_MAX_CPU = 40      # % of one host CPU used by QEMU at the idle desktop
TIMEOUT = {"uhci": 90, "ohci": 60}
CHECKS = [
    ("desktop", "gui: desktop ready"),
    ("boot volume C:", "(boot volume)"),
    ("SATA volume", "on sata0"),
    ("exFAT volume", "exFAT 'EXFATDISK' on sata"),
    ("BOOT.LOG", "bootlog: saved"),
    ("USB keyboard in report protocol", "keyboard (report protocol"),
]
BAD = ["CPU EXCEPTION", "KERNEL PANIC", "nested fault"]
# variants that run a test program after boot: (program, its report prefix)
PROGRAMS = {"user": ("SELFTEST.EXE", "selftest"), "sock": ("SOCKTEST.EXE", "socktest"), "web": ("WEBTEST.EXE", "webtest"),
            "net": ("NETTEST.EXE", "nettest"), "audio": ("SNDTEST.EXE", "sndtest"), "ntfsrw": ("NTFSTEST.EXE", "ntfstest")}
PROGRAM_TIMEOUT = {"net": 300}
NET_LOSS_PCT = 3
_netservers = None


def net_servers():
    """the host's servers for the net variant (started once)"""
    global _netservers
    if _netservers is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import netservers
        _netservers = (netservers, netservers.start())
    return _netservers


def cpu_seconds(pid):
    try:
        f = open("/proc/%d/stat" % pid).read().rsplit(")", 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return None


def make_cd_isos():
    """two small test discs for the cd variant"""
    isos = []
    for label, files in (("HALCYONCD", {"README.TXT": "Halcyon CD test disc\r\n",
                                         "Folder/Long name \u00e9.txt": "Joliet\n"}),
                         ("SECONDDISC", {"SECOND.TXT": "second disc\n"})):
        src = os.path.join(BUILD, "ci_cdsrc_" + label)
        subprocess.run(["rm", "-rf", src], check=True)
        for name, data in files.items():
            path = os.path.join(src, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8").write(data)
        iso = os.path.abspath(os.path.join(BUILD, "ci_%s.iso" % label.lower()))
        subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-input-charset", "UTF-8", "-J", "-V", label,
                        "-o", iso, src], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        isos.append(iso)
    return isos


def wait_log(log, pattern, start, timeout):
    """wait until pattern appears in the log after offset start"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = open(log, errors="replace").read()
        if pattern in text[start:]:
            return True
        time.sleep(0.25)
    return False


def cd_media_change(log, isos):
    """eject the disc and insert another one through the QEMU monitor"""
    mon = os.path.join(BUILD, "mon.sock")
    fails = []
    start = len(open(log, errors="replace").read())
    subprocess.run([sys.executable, "tools/qemu_ctl.py", mon, "eject -f cd0"], stdout=subprocess.DEVNULL)
    if not wait_log(log, "block: cd0: removed", start, 8):
        fails.append("CD eject not seen")
    start = len(open(log, errors="replace").read())
    subprocess.run([sys.executable, "tools/qemu_ctl.py", mon, "change cd0 %s" % isos[1]], stdout=subprocess.DEVNULL)
    if not wait_log(log, "ISO9660 'SECONDDISC' on cd0", start, 8):
        fails.append("second CD not mounted")
    return fails


def screen_stats(png):
    """(share of dark pixels, share of bright pixels) of a screendump"""
    from PIL import Image
    im = Image.open(png).convert("L")
    h = im.histogram()
    n = sum(h) or 1
    return sum(h[:24]) / n, sum(h[121:]) / n


def saver_test(log):
    """the screen saver (GUIDEMO.EXE runs and logs the input it gets): it
    starts after the wait and shows stars without the pointer; a click on
    GUIDEMO's window ends it and reaches GUIDEMO neither as the press nor as
    the release, the next click does; it starts again and a key ends it,
    and again, a mouse movement"""
    mon = os.path.join(BUILD, "mon.sock")
    png = os.path.abspath(os.path.join(BUILD, "ci_saver.png"))
    fails = []

    def ctl(*cmds):
        subprocess.run([sys.executable, "tools/qemu_ctl.py", mon] + list(cmds), stdout=subprocess.DEVNULL)

    def offset():
        return len(open(log, errors="replace").read())

    if not wait_log(log, "guidemo: window client at", 0, 30):
        return ["GUIDEMO.EXE did not start"]
    text = open(log, errors="replace").read()
    m = re.search(r"guidemo: window client at (-?\d+),(-?\d+) size (\d+)x(\d+)", text)
    cx, cy = (int(m.group(1)) + int(m.group(3)) // 2, int(m.group(2)) + int(m.group(4)) // 2) if m else (512, 384)
    s = re.search(r"desktop ready \((\d+)x(\d+)\)", text)
    if s:                                # qemu_ctl's pointer positions are for 1024 x 768
        cx, cy = cx * 1024 // int(s.group(1)), cy * 768 // int(s.group(2))
    ctl("move:%d,%d" % (cx, cy))         # the pointer waits on the window: the click is a click only
    start = offset()
    if not wait_log(log, "screensaver: STARS.SCR is showing", start, 30):
        return ["screen saver did not start"]
    time.sleep(1.5)
    ctl("png:" + png)
    dark, bright = screen_stats(png)
    if dark < 0.9 or bright < 0.0003:
        fails.append("screen saver frame: %.0f%% dark, %.2f%% bright" % (dark * 100, bright * 100))
    start = offset()
    ctl("click:%d,%d" % (cx, cy))
    if not wait_log(log, "screensaver: stopped (click)", start, 8):
        fails.append("a click did not end the screen saver")
    time.sleep(1)
    after = open(log, errors="replace").read()[start:]
    if "guidemo: press at" in after or "guidemo: drag" in after:
        fails.append("the click that ended the screen saver reached the window")
    ctl("png:" + png)
    dark, bright = screen_stats(png)
    if dark > 0.5:
        fails.append("desktop not back after the screen saver (%.0f%% dark)" % (dark * 100))
    start = offset()
    ctl("click:%d,%d" % (cx, cy))
    if not wait_log(log, "guidemo: press at", start, 5):
        fails.append("a click after the screen saver did not reach the window")
    start = offset()
    if not wait_log(log, "screensaver: STARS.SCR is showing", start, 15):
        return fails + ["screen saver did not start again"]
    start = offset()
    ctl("sendkey a")
    if not wait_log(log, "screensaver: stopped (key)", start, 8):
        fails.append("a key did not end the screen saver")
    start = offset()
    if not wait_log(log, "screensaver: STARS.SCR is showing", start, 15):
        return fails + ["screen saver did not start a third time"]
    time.sleep(1.2)                  # (motion right after a start is ignored)
    start = offset()
    ctl("move:200,150", "move:600,500")
    if not wait_log(log, "screensaver: stopped (mouse moved)", start, 8):
        fails.append("mouse movement did not end the screen saver")
    return fails


def keyboard_leds(log):
    """Caps Lock on the USB keyboard must switch its Caps Lock LED on and off"""
    mon = os.path.join(BUILD, "mon.sock")
    fails = []
    for want in ("LEDs Num Caps\n", "LEDs Num\n"):
        start = len(open(log, errors="replace").read())
        subprocess.run([sys.executable, "tools/qemu_ctl.py", mon, "sendkey caps_lock"], stdout=subprocess.DEVNULL)
        if not wait_log(log, want, start, 8):
            fails.append("USB keyboard LEDs ('%s' not seen after Caps Lock)" % want.strip())
            break
    return fails


def net_failures(text, nic, opts):
    """the network adapter variant's checks (see the docstring)"""
    dev, driver, msix = NICS[nic]
    lines = text.splitlines()
    fails = []
    if not any(l.startswith("net: eth0: ") and "(%s), MAC" % driver in l for l in lines):
        return ["no %s adapter registered" % driver]
    if not any(l.startswith("net: eth0: ") and "link up" in l for l in lines):
        fails.append("link not up")
    if "net: eth0: probe reply from 52:55:0a:00:02:02" not in text:
        fails.append("no reply from 10.0.2.2")
    m = re.search(r"net: eth0: probe: (\d+) sent, (\d+) replies, (\d+) transmitted in \d+ ms; "
                  r"errors: (\d+) send, (\d+) tx, (\d+) rx; interrupt (.*)", text)
    if not m:
        fails.append("probe did not finish")
    else:
        sent, replies, done, e1, e2, e3 = (int(x) for x in m.groups()[:6])
        irq = m.group(7).strip()
        # (transmitted: the driver's count, which includes the stack's own frames: DHCP, ARP)
        if sent != NET_PROBE or replies != sent or done < sent or done > sent + 40 or e1 or e2 or e3:
            fails.append("probe: %s" % m.group(0).split("probe: ")[1])
        want = "polled" if "noirq" in opts else "MSI-X" if msix and "nomsi" not in opts else "INTx"
        if not irq.startswith(want) or "lost" in irq or "none seen" in irq or (want != "polled" and "polled" in irq):
            fails.append("interrupt %s (expected %s)" % (irq, want))
    m = re.search(r"net: eth0: probe burst: (\d+) sent \(transmit ring full \d+ times\), (\d+) transmitted, "
                  r"\d+ replies in \d+ ms; errors: (\d+) tx, (\d+) rx", text)
    if not m:
        fails.append("probe burst did not finish")
    elif int(m.group(1)) != NET_PROBE // 4 or not 0 <= int(m.group(2)) - int(m.group(1)) <= 40 or int(m.group(3)) or int(m.group(4)):
        fails.append("burst: %s" % m.group(0).split("burst: ")[1])
    bad = [l for l in lines if l.startswith("irq: ") and driver in l and ("stopped arriving" in l or "polling it" in l)]
    if bad:
        fails.append(bad[0].strip())
    return fails


def wav_segments(path):
    """the loud stretches of a QEMU wav capture (16-bit, 48 kHz expected):
    [(start_s, length_ms, pitch_hz, dropouts)] (pitch from zero crossings)"""
    import array
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    # the header's sizes may be stale if QEMU did not exit cleanly: find "data"
    i = data.find(b"data")
    if data[:4] != b"RIFF" or i < 0:
        return None
    ch = data[22] | data[23] << 8
    rate = int.from_bytes(data[24:28], "little")
    pcm = array.array("h")
    pcm.frombytes(data[i + 8: i + 8 + (len(data) - i - 8) // (2 * ch) * 2 * ch])
    left = pcm[0::ch]
    blk = rate // 100
    loud = []
    for b in range(len(left) // blk):
        seg = left[b * blk:(b + 1) * blk]
        loud.append(sum(v * v for v in seg) / blk > 1000 * 1000)
    segs, b = [], 0
    while b < len(loud):
        if not loud[b]:
            b += 1
            continue
        e = b
        while e < len(loud) and loud[e]:
            e += 1
        x = left[b * blk:e * blk]
        # pitch: rising zero crossings, interpolated, over the whole stretch
        cross = []
        for k in range(1, len(x)):
            if x[k - 1] < 0 <= x[k]:
                cross.append(k - 1 + (-x[k - 1]) / (x[k] - x[k - 1]))
        pitch = (len(cross) - 1) * rate / (cross[-1] - cross[0]) if len(cross) > 2 else 0
        run, drops = 0, 0
        for v in x:
            run = run + 1 if -50 < v < 50 else 0
            if run == rate // 500:
                drops += 1
        segs.append((b / 100, (e - b) * 10, pitch, drops))
        b = e
    return segs


def pitch_runs(path):
    """the tones of a QEMU wav capture told apart by pitch, also where no
    silence separates them: [(start_s, length_ms, pitch_hz)] from the
    spacing of rising zero crossings (in loud stretches)"""
    import array
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    i = data.find(b"data")
    if data[:4] != b"RIFF" or i < 0:
        return None
    ch = data[22] | data[23] << 8
    rate = int.from_bytes(data[24:28], "little")
    pcm = array.array("h")
    pcm.frombytes(data[i + 8: i + 8 + (len(data) - i - 8) // (2 * ch) * 2 * ch])
    x = pcm[0::ch]
    cross, armed = [], False
    for k in range(1, len(x)):           # (armed below -2000: noise near zero is no crossing)
        if x[k] < -2000:
            armed = True
        elif armed and x[k - 1] < 0 <= x[k]:
            cross.append(k - 1 + (-x[k - 1]) / (x[k] - x[k - 1]))
            armed = False
    runs, start = [], 0
    for k in range(1, len(cross) + 1):
        if k < len(cross):
            d, d0 = cross[k] - cross[k - 1], cross[min(start + 1, k)] - cross[start]
            if k == start + 1 or abs(d - d0) < d0 * 0.1:
                continue
        n = k - 1 - start
        if n >= 4:
            span = cross[k - 1] - cross[start]
            runs.append((cross[start] / rate, (span + span / n) * 1000 / rate, n * rate / span))
        start = k
    return runs


def wav_envelope(path, rate_out=100):
    """RMS of the first channel in 1/rate_out s blocks (16-bit WAV)"""
    import array
    data = open(path, "rb").read()
    i = data.find(b"data")
    ch = data[22] | data[23] << 8
    rate = int.from_bytes(data[24:28], "little")
    pcm = array.array("h")
    pcm.frombytes(data[i + 8: i + 8 + (len(data) - i - 8) // (2 * ch) * 2 * ch])
    x = pcm[0::ch]
    blk = rate // rate_out
    return [(sum(v * v for v in x[b * blk:(b + 1) * blk]) / blk) ** 0.5 for b in range(len(x) // blk)]


def startup_failures(wav, segs, signal_amp):
    """the recording must start with the startup sound: its 10 ms loudness
    envelope must follow STARTUP.WAV's (correlation >= 0.95), at the same
    gain as the test signal that follows it (a sine of amplitude signal_amp
    in a stream at 100 %: the master volume, and QEMU's AC'97 codec, which
    takes off another 2.5 dB, apply to both). Returns (failures, the
    segments after it)."""
    ref_path = os.path.join(BUILD, "media", "STARTUP.WAV")
    if not segs:
        return ["no startup sound (silence)"], segs
    ref = wav_envelope(ref_path)
    rec = wav_envelope(wav)
    t0 = int(round(segs[0][0] * 100))
    best = (-1, 0, 0)
    for lag in range(max(0, t0 - 30), t0 + 10):
        x = rec[lag:lag + len(ref)]
        if len(x) < len(ref):
            break
        sxy = sum(a * b for a, b in zip(x, ref))
        sxx = sum(a * a for a in x)
        syy = sum(b * b for b in ref)
        corr = sxy / ((sxx * syy) ** 0.5 or 1)
        if corr > best[0]:
            best = (corr, lag, sxy / (syy or 1))
    corr, lag, gain = best
    end = (lag + len(ref)) / 100
    rest = [sg for sg in segs if sg[0] >= end - 0.05]
    fails = []
    want = MASTER_GAIN
    if rest and rest[0][1] >= 300:            # the signal's gain, from its middle
        b0 = int(round(rest[0][0] * 100))
        mid = rec[b0 + 10: b0 + rest[0][1] // 10 - 10]
        want = (sum(mid) / len(mid)) / (signal_amp / 2 ** 0.5)
    if corr < 0.95 or abs(gain / want - 1) > 0.12:
        fails.append("startup sound: envelope correlation %.3f, gain %.2f (want >= 0.95, %.2f as the signal)" % (corr, gain, want))
    return fails, rest


def sound_failures(text, snd, opts, wav):
    """the sound card variant's checks (see the docstring)"""
    driver = SOUNDS[snd][1]
    fails = []
    if not any(l.startswith("sound: snd0: ") and "(%s)" % driver in l for l in text.splitlines()):
        return ["no %s sound card registered" % driver]
    if "audio" in opts:
        segs = wav_segments(wav)
        if segs is None:
            return fails + ["no recording"]
        sf, rest = startup_failures(wav, segs, SNDTEST_AMP)
        fails += sf
        desc = ", ".join("%.2fs %dms %.1fHz%s" % (a, b, c, " %d dropouts" % d if d else "") for a, b, c, d in rest)
        if len(rest) != 1 or not (SNDTEST_MS - 30 <= rest[0][1] <= SNDTEST_MS + 30 and abs(rest[0][2] - SNDTEST_HZ) < 1.5
                                  and not rest[0][3]):
            fails.append("recording after the startup sound: expected SNDTEST's %d Hz signal, got: %s" % (SNDTEST_HZ, desc or "silence"))
        return fails
    m = re.search(r"sound: test: done: tone played in \d+ ms, stream underruns (\d+), device snd0 underruns (\d+), irq (\w+)", text)
    if not m:
        return fails + ["sound test did not finish"]
    if m.group(1) != "0" or m.group(2) != "0":
        fails.append("underruns (stream %s, device %s)" % (m.group(1), m.group(2)))
    want = "polled" if "noirq" in opts else "live"
    if m.group(3) != want:
        fails.append("sound interrupt %s (expected %s)" % (m.group(3), want))
    segs = wav_segments(wav)
    if segs is None:
        return fails + ["no recording"]
    sf, segs = startup_failures(wav, segs, 16000)
    fails += sf
    desc = ", ".join("%.2fs %dms %.1fHz%s" % (a, b, c, " %d dropouts" % d if d else "") for a, b, c, d in segs)
    if len(segs) != 2:
        return fails + ["recording: expected the tone and the beep, got: %s" % (desc or "silence")]
    (t0, l0, f0, d0), (t1, l1, f1, d1) = segs
    if not (SOUND_TEST_MS - 30 <= l0 <= SOUND_TEST_MS + 30 and abs(f0 - 440) < 1 and not d0 and
            230 <= l1 <= 270 and abs(f1 - 1000) < 3 and not d1 and t1 > t0):
        fails.append("recording: %s" % desc)
    return fails


def beep_failures(text, snd, wav):
    """the beep variant's checks (see the docstring)"""
    fails = []
    if snd and not any(l.startswith("sound: snd0: ") and "(%s)" % SOUNDS[snd][1] in l for l in text.splitlines()):
        return ["no %s sound card registered" % SOUNDS[snd][1]]
    m = re.search(r"sound: beep test: done through (.+?) \((-?\d+), (-?\d+)\), device underruns (\d+)", text)
    if not m:
        return ["beep test did not finish"]
    if m.group(1) != ("snd0" if snd else "PC speaker"):
        fails.append("beep through %s" % m.group(1))
    if m.group(2) != "0" or m.group(3) != "0" or m.group(4) != "0":
        fails.append("beep test: %s" % m.group(0))
    # (QEMU's wav file only grows while a voice plays: the 4 s between the
    # beeps are not all there, and with the PC speaker not at all)
    runs = pitch_runs(wav)
    if runs is None:
        return fails + ["no recording"]
    if snd:     # with a card the startup sound comes first: check it, then look after it
        sf, rest = startup_failures(wav, wav_segments(wav) or [], BEEP_AMP)
        fails += sf
        if rest:
            runs = [r for r in runs if r[0] >= rest[0][0] - 0.02]
    desc = ", ".join("%.2fs %dms %.1fHz" % r for r in runs)
    if len(runs) != 2:
        return fails + ["recording: expected two beeps, got: %s" % (desc or "silence")]
    (t0, l0, f0), (t1, l1, f1) = runs
    # (the second beep may lose its last ~20 ms: QEMU stops writing the file
    # when the PC speaker goes off, with some of it still buffered)
    if not (270 <= l0 <= 330 and abs(f0 - 880) < 2 and 150 <= l1 <= 230 and abs(f1 - 660) < 2 and t1 > t0):
        fails.append("recording: %s" % desc)
    return fails


NTFS_LABEL = "Halcyon_NTFS"
NTFS_PART = (2048, 522240)          # start, sectors of the scratch disk's partition


def make_ntfs_disk():
    """a blank 256 MiB disk with one (unformatted) FAT32-type partition"""
    disk = os.path.join(BUILD, "ci_ntfs.img")
    if os.path.exists(disk):
        os.remove(disk)
    with open(disk, "wb") as f:
        f.truncate(256 << 20)
    subprocess.run(["sfdisk", "-q", disk], input="start=%d, size=%d, type=c\n" % NTFS_PART, text=True,
                   check=True, stdout=subprocess.DEVNULL)
    return disk


def ntfs_failures(text, disk):
    """the format test hook's volume, checked on the host"""
    if "format test: " not in text:
        return ["format test did not run"]
    line = [l for l in text.splitlines() if "format test: " in l][0]
    if "as NTFS '%s': done" % NTFS_LABEL not in line:
        return ["format test: %s" % line.strip()]
    fails = []
    with open(disk, "rb") as f:
        mbr = f.read(512)
    if mbr[0x1BE + 4] != 0x07:
        fails.append("partition type 0x%02x, not 0x07" % mbr[0x1BE + 4])
    part = os.path.join(BUILD, "ci_ntfs_part.img")
    subprocess.run(["dd", "if=" + disk, "of=" + part, "bs=512", "skip=%d" % NTFS_PART[0],
                    "count=%d" % NTFS_PART[1], "conv=sparse", "status=none"], check=True)
    if subprocess.run(["sh", "-c", "command -v ntfsfix && command -v ntfsinfo"], stdout=subprocess.DEVNULL).returncode:
        print("    (ntfs-3g tools missing: the NTFS volume was not checked)")
        return fails
    r = subprocess.run(["ntfsfix", "-n", part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode or "processed successfully" not in r.stdout:
        fails.append("ntfsfix -n: " + r.stdout.strip().splitlines()[-1])
    r = subprocess.run(["ntfsinfo", "-m", part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode or "Volume Name: %s\n" % NTFS_LABEL not in r.stdout or "Cluster Size: 4096" not in r.stdout:
        fails.append("ntfsinfo -m: label or cluster size")
    r = subprocess.run([sys.executable, "tests/host/ntfs_tool.py", part, "check"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        fails.append("ntfs_tool.py check: " + r.stdout.strip().splitlines()[0])
    return fails


NTFSRW_PART = (2048, 129024)        # start, sectors of the NTFS partition of the ntfsrw disk


def make_ntfsrw_disk():
    """a 64 MiB disk with an MBR partition (type 7) holding an NTFS volume
    made by mkntfs and populated through ntfs-3g (tests/host/ntfs_mkimg.py,
    profile boot: HELLO.TXT, Folder\\data.bin, Windows\\System32)"""
    if os.geteuid() != 0 or subprocess.run(["sh", "-c", "command -v mkntfs && command -v ntfs-3g && test -w /dev/fuse"],
                                           stdout=subprocess.DEVNULL).returncode:
        return None
    disk, vol = os.path.join(BUILD, "ci_ntfsrw.img"), os.path.join(BUILD, "ci_ntfsrw_vol.img")
    subprocess.run([sys.executable, "tests/host/ntfs_mkimg.py", vol, str(NTFSRW_PART[1] * 512 >> 20), "boot"], check=True)
    with open(disk, "wb") as f:
        f.truncate((NTFSRW_PART[0] + NTFSRW_PART[1]) * 512)
    subprocess.run(["sfdisk", "-q", disk], input="start=%d, size=%d, type=7\n" % NTFSRW_PART, text=True,
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["dd", "if=" + vol, "of=" + disk, "bs=512", "seek=%d" % NTFSRW_PART[0], "conv=notrunc,sparse",
                    "status=none"], check=True)
    return disk


def ntfsrw_failures(text, disk):
    """the volume NTFSTEST.EXE wrote to, checked on the host"""
    fails = []
    sys.path.insert(0, "tests/host")
    import ntfs_mkimg
    import zlib
    want = "data.bin 100000 bytes crc %08x" % zlib.crc32(ntfs_mkimg.data("data.bin", 100000))
    if want not in text:
        fails.append("data.bin read wrong (%s)" % want)
    if "on sata" not in text or "NTFS 'Halcyon Test' on sata" not in text:
        fails.append("NTFS volume not mounted")
    part = os.path.join(BUILD, "ci_ntfsrw_part.img")
    subprocess.run(["dd", "if=" + disk, "of=" + part, "bs=512", "skip=%d" % NTFSRW_PART[0],
                    "count=%d" % NTFSRW_PART[1], "conv=sparse", "status=none"], check=True)
    r = subprocess.run(["ntfsfix", "-n", part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode or "processed successfully" not in r.stdout:
        fails.append("ntfsfix -n: " + r.stdout.strip().splitlines()[-1])
    r = subprocess.run([sys.executable, "tests/host/ntfs_check.py", part], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode or "volume clean" not in r.stdout:
        fails.append("ntfs_check.py: " + r.stdout.strip().splitlines()[0])
    mnt = os.path.join(BUILD, "ci_ntfsrw_mnt")
    os.makedirs(mnt, exist_ok=True)
    if subprocess.run(["ntfs-3g", "-o", "ro", part, mnt]).returncode:
        return fails + ["ntfs-3g cannot mount the volume"]
    try:
        n = 300000
        want = bytes((i * 7 + i // 251) & 0xFF for i in range(n))
        def rd(p):
            try:
                return open(os.path.join(mnt, p), "rb").read()
            except OSError:
                return None
        if rd("Written by Halcyon.txt") != want:
            fails.append("ntfs-3g: Written by Halcyon.txt")
        if rd("New Folder/inner.txt") != b"inner file\r\n" or rd("New Folder/HELLO moved.txt") != b"Hello from NTFS!\r\n":
            fails.append("ntfs-3g: New Folder")
        if rd("HELLO.TXT") is not None or rd("temporary.tmp") is not None or rd("refused.txt") is not None:
            fails.append("ntfs-3g: files that should be gone")
        nf = os.path.join(mnt, "New Folder")
        if not os.path.isdir(nf) or len(os.listdir(nf)) != 42:
            fails.append("ntfs-3g: New Folder missing or not 42 entries")
    finally:
        subprocess.run(["fusermount", "-u", mnt])
    return fails


def run(cfg):
    parts = cfg.split(":")
    hc, fw = parts[0], parts[1]
    opts = [v for v in (parts[2] if len(parts) > 2 else "").split("+") if v]
    flags = 0
    for v in opts:
        flags |= VARIANTS[v]
    irqmode = next((v for v in opts if v in ("pic", "nomsi", "noirq")), "")
    nvme = next((v for v in opts if v.startswith("nvme")), None)
    log = os.path.join(BUILD, "serial.log")
    if os.path.exists(log):
        os.remove(log)
    extra = "-device usb-tablet,bus=usb.0 -device usb-kbd,bus=usb.0 "
    checks = list(CHECKS)
    if nvme:
        disk = os.path.join(BUILD, "ci_nvme.img")
        src = "fat32_4k.img" if nvme == "nvme4k" else "fat32gpt.img"
        subprocess.run(["cp", "--sparse=always", os.path.join(BUILD, src), disk], check=True)
        if nvme == "nvme4k":
            extra += ("-device nvme,id=nvme0,serial=HALC0002 -drive if=none,id=nv0,format=raw,file=%s "
                      "-device nvme-ns,drive=nv0,bus=nvme0,nsid=1,logical_block_size=4096,"
                      "physical_block_size=4096" % disk)
            checks.append(("4K namespace", "blocks x 4096 bytes"))
        else:
            extra += ("-drive if=none,id=nv0,format=raw,file=%s "
                      "-device nvme,serial=HALC0001,drive=nv0" % disk)
        checks = [c if c[0] != "SATA volume" else ("NVMe volume", "on nvme0") for c in checks
                  if c[0] != "exFAT volume"]
    else:
        sata = os.path.join(BUILD, "ci_sata.img")
        subprocess.run(["cp", os.path.join(BUILD, "fat32gpt.img"), sata], check=True)
        extra += "-drive if=none,id=d0,format=raw,file=%s -device ide-hd,drive=d0,bus=ide.0" % sata
        # exFAT test disk on a third SATA port (ide.1 is the CD variant's drive)
        sata_x = os.path.join(BUILD, "ci_sata_exfat.img")
        subprocess.run(["cp", os.path.join(BUILD, "exfat.img"), sata_x], check=True)
        extra += " -drive if=none,id=d1,format=raw,file=%s -device ide-hd,drive=d1,bus=ide.2" % sata_x
    ntfs_disk = None
    if "ntfs" in opts:
        ntfs_disk = make_ntfs_disk()
        spec = os.path.abspath(os.path.join(BUILD, "ci_formattest.txt"))     # (run.sh splits EXTRA at spaces)
        with open(spec, "w") as f:
            f.write("NTFSTEST NTFS %s 4096" % NTFS_LABEL)
        extra += (" -drive if=none,id=d3,format=raw,file=%s -device ide-hd,drive=d3,bus=ide.3,model=NTFSTEST"
                  " -fw_cfg name=opt/halcyon/formattest,file=%s" % (ntfs_disk, spec))
    ntfsrw_disk = None
    if "ntfsrw" in opts:
        ntfsrw_disk = make_ntfsrw_disk()
        if not ntfsrw_disk:
            print("%-22s SKIP (needs root, mkntfs and ntfs-3g)" % cfg, flush=True)
            return True
        extra += (" -drive if=none,id=d4,format=raw,file=%s -device ide-hd,drive=d4,bus=ide.3,model=NTFSRW"
                  % ntfsrw_disk)
    if flags:
        extra += " -fw_cfg name=opt/halcyon/bootflags,string=%#x" % flags
    nic = next((v for v in opts if v in NICS), None)
    if nic and "net" in opts:           # net+NIC: NETTEST on that adapter (no probe)
        nic_dev, nic = NICS[nic][0], None
    else:
        nic_dev = "e1000"
    if nic:
        extra += " -netdev user,id=n0 -device %s,netdev=n0 -fw_cfg name=opt/halcyon/netprobe,string=%d" % (
            NICS[nic][0], NET_PROBE)
    snd = next((v for v in opts if v in SOUNDS), "ac97" if "audio" in opts else None)
    wav = os.path.abspath(os.path.join(BUILD, "ci_sound.wav"))
    beep = "beep" in opts
    if snd or beep:
        if os.path.exists(wav):
            os.remove(wav)
        if snd:
            extra += " -audiodev wav,id=snd0,path=%s,out.frequency=48000 %s" % (wav, SOUNDS[snd][0])
        else:   # beep without a sound card: the PC speaker is recorded
            extra += " -audiodev wav,id=snd0,path=%s,out.frequency=48000 -machine pcspk-audiodev=snd0" % wav
        if beep:
            extra += " -fw_cfg name=opt/halcyon/soundtest,string=beep"
        elif "audio" not in opts:       # (SNDTEST plays its own signal instead of the kernel's test)
            extra += " -fw_cfg name=opt/halcyon/soundtest,string=%d" % SOUND_TEST_MS
    prog = next((PROGRAMS[v] for v in opts if v in PROGRAMS), None)
    prog_args = ""
    ini_lines = []
    net = "net" in opts
    if net:
        ns_mod, ports = net_servers()
        fwd = ns_mod.free_port()
        extra += " -netdev user,id=n0,hostfwd=tcp:127.0.0.1:%d-:8080 -device %s,netdev=n0" % (fwd, nic_dev)
        if os.environ.get("NETDUMP"):       # debugging: the guest's frames in a pcap file
            extra += " -object filter-dump,id=fd0,netdev=n0,maxlen=128,file=%s" % os.path.abspath(os.environ["NETDUMP"])
        ini_lines.append("net.dns=10.0.2.2:%d" % ports["dns"])
        prog_args = " http=%d sink=%d echo=%d" % (ports["http"], ports["sink"], ports["echo"])
        checks.append(("DHCP configuration", "net: eth0: 10.0.2.15/24 via 10.0.2.2"))
        if "loss" in opts:
            prog_args += " loss big=2"
            extra += " -fw_cfg name=opt/halcyon/netloss,string=%d" % NET_LOSS_PCT
            checks.append(("loss test hook", "of frames lost (receive and transmit)"))
    if prog:
        # (a file: run.sh splits EXTRA at spaces)
        argf = os.path.abspath(os.path.join(BUILD, "ci_autorun.txt"))
        with open(argf, "w") as f:
            f.write("C:/PROGRAMS/" + prog[0] + prog_args)
        extra += " -fw_cfg name=opt/halcyon/autorun,file=" + argf
    if "saver" in opts:
        extra += " -fw_cfg name=opt/halcyon/screensaver,string=5:STARS.SCR"
        if not prog:                     # GUIDEMO.EXE logs the input that reaches it
            argf = os.path.abspath(os.path.join(BUILD, "ci_autorun.txt"))
            with open(argf, "w") as f:
                f.write("C:/PROGRAMS/GUIDEMO.EXE")
            extra += " -fw_cfg name=opt/halcyon/autorun,file=" + argf
    if "switch" in opts:
        extra += " -fw_cfg name=opt/halcyon/setmode,string=1600x900"
        checks.append(("live mode switch", "test mode switch to 1600x900: ok (desktop 1600x900)"))
    if "res" in opts:
        checks.append(("saved resolution", "chosen by saved setting"))
        checks.append(("desktop at 1280x1024", "desktop ready (1280x1024)"))
    if "fwvideo" in opts:
        checks.append(("firmware frame buffer only", "changes apply at the next start"))
    isos = None
    if "cd" in opts:
        isos = make_cd_isos()
        extra += " -drive if=none,id=cd0,media=cdrom,file=%s -device ide-cd,drive=cd0,bus=ide.1" % isos[0]
    env = dict(os.environ, HEADLESS="1", BUILD=BUILD, EXTRA=extra)
    # boot from a scratch copy so tests never modify the release image
    img = os.path.join(BUILD, "ci_boot.img")
    subprocess.run(["cp", os.path.join(BUILD, "halcyon.img"), img], check=True)
    if "res" in opts:
        ini_lines += ["display.width=1280", "display.height=1024"]
    if ini_lines:
        ini = os.path.join(BUILD, "ci_settings.ini")
        with open(ini, "w") as f:
            f.write("".join(l + "\r\n" for l in ini_lines))
        subprocess.run(["mcopy", "-o", "-i", img + "@@1M", ini, "::/HALCYON/SETTINGS.INI"], check=True)
    env["IMG_OVERRIDE"] = img
    errlog = os.path.join(BUILD, "qemu.err")
    p = subprocess.Popen(["sh", "tools/run.sh", hc, fw], env=env,
                         stdout=subprocess.DEVNULL, stderr=open(errlog, "w"))
    client = None
    if net and "loss" not in opts:
        client = {"err": "not run"}

        def run_client():
            client["err"] = ns_mod.client_test(fwd, log, 0)
        client["thread"] = threading.Thread(target=run_client, daemon=True)
        client["thread"].start()
    t0 = time.time()
    text = ""
    limit = TIMEOUT.get(hc, 45)
    while time.time() - t0 < limit:
        time.sleep(0.5)
        try:
            text = open(log, errors="replace").read()
        except OSError:
            continue
        if "gui: desktop ready" in text and "bootlog: saved" in text:
            break
        if any(b in text for b in BAD):
            break
        if p.poll() is not None:
            break
    elapsed = time.time() - t0
    if prog:
        t1 = time.time()
        plimit = max([PROGRAM_TIMEOUT.get(v, 90) for v in opts] + [90])
        while time.time() - t1 < plimit and p.poll() is None:
            text = open(log, errors="replace").read()
            if prog[1] + ": all" in text or "checks FAILED" in text or any(b in text for b in BAD):
                break
            time.sleep(0.5)
        time.sleep(1)
        text = open(log, errors="replace").read()
    if nic:
        t1 = time.time()
        while time.time() - t1 < 20 and p.poll() is None and "probe burst:" not in text:
            time.sleep(0.5)
            text = open(log, errors="replace").read()
    if (snd or beep) and "audio" not in opts:
        done = "sound: beep test: done" if beep else "sound: test: done"
        t1 = time.time()
        while time.time() - t1 < 30 and p.poll() is None and done not in text:
            time.sleep(0.5)
            text = open(log, errors="replace").read()
    if ntfs_disk:
        t1 = time.time()
        while time.time() - t1 < 30 and p.poll() is None and "format test: " not in text:
            time.sleep(0.5)
            text = open(log, errors="replace").read()
    idle = None
    saver_fails = []
    if "saver" in opts and p.poll() is None and "gui: desktop ready" in text:
        saver_fails = saver_test(log)
        text = open(log, errors="replace").read()
    if p.poll() is None and "gui: desktop ready" in text and not prog and "saver" not in opts:
        time.sleep(2)
        c0 = cpu_seconds(p.pid)
        time.sleep(4)
        c1 = cpu_seconds(p.pid)
        if c0 is not None and c1 is not None:
            idle = (c1 - c0) * 100 / 4
        text = open(log, errors="replace").read()
    kbd_fails = []
    if p.poll() is None and "gui: desktop ready" in text:
        kbd_fails = keyboard_leds(log)
        text = open(log, errors="replace").read()
    cd_fails = []
    if isos and p.poll() is None and "gui: desktop ready" in text:
        cd_fails = cd_media_change(log, isos)
        text = open(log, errors="replace").read()
    p.terminate()
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        p.kill()
    failures = [name for name, pat in checks if pat not in text]
    failures += ["log contains '%s'" % b for b in BAD if b in text]
    if prog:
        if prog[1] + ": all" not in text:
            bad = [l for l in text.splitlines() if prog[1] + ":" in l and ("FAIL" in l or "skipped" in l)]
            failures.append("%s (%s)" % (prog[1], bad[0].strip() if bad else "did not finish"))
    if client:
        client["thread"].join(5)
        if client["err"]:
            failures.append(client["err"])
    if isos:
        if "ISO9660 'HALCYONCD' on cd0" not in text:
            failures.append("CD volume")
        failures += cd_fails
    failures += kbd_fails
    failures += saver_fails
    if ntfs_disk:
        failures += ntfs_failures(text, ntfs_disk)
    if ntfsrw_disk:
        failures += ntfsrw_failures(text, ntfsrw_disk)
    if nic:
        failures += net_failures(text, nic, opts)
    if beep:
        failures += beep_failures(text, snd, wav)
    elif snd:
        failures += sound_failures(text, snd, opts, wav)
    want_mode = {"": "irq: mode I/O APIC", "pic": "irq: mode 8259 PIC", "nomsi": "irq: mode I/O APIC",
                 "noirq": "interrupts disabled by boot option"}[irqmode]
    if want_mode not in text:
        failures.append("interrupt mode ('%s' missing)" % want_mode)
    if nvme:
        bad = [l for l in text.splitlines() if l.startswith("nvme: ") and any(e in l for e in NVME_ERRORS)]
        if bad:
            failures.append("NVMe error ('%s')" % bad[0])
        if irqmode != "noirq" and not any("NVMe" in l and ("INTx on" in l or "MSI" in l)
                                          for l in text.splitlines() if l.startswith("irq: ")):
            failures.append("no NVMe interrupt")
    if irqmode != "noirq":
        if "CPU idle off" in text:
            failures.append("no idle timer")
        usb = {"uhci": "UHCI", "ohci": "OHCI", "ehci": "EHCI", "ich9": "EHCI", "xhci": "xHCI"}[hc]
        if not any(usb in l and ("INTx on" in l or "MSI" in l) for l in text.splitlines() if l.startswith("irq: ")):
            failures.append("no %s interrupt" % usb)
        if idle is not None and idle > IDLE_MAX_CPU:
            failures.append("idle desktop uses %.0f%% CPU" % idle)
    status = "PASS" if not failures else "FAIL (%s)" % ", ".join(failures)
    cpu = "" if idle is None else "  idle CPU %3.0f%%" % idle
    print("%-22s %5.1fs%s  %s" % (cfg, elapsed, cpu, status), flush=True)
    if net:
        for line in text.splitlines():
            if "nettest: " in line and any(k in line for k in ("MB/s", "KB/s", "parallel", "ping 10", "loopback 4",
                                                                       "retransmitted", "DNS:")):
                print("    " + line[line.index("nettest: "):].strip())
    if failures:
        print("    --- last lines of the serial log ---")
        for line in text.strip().splitlines()[-15:]:
            print("    " + line)
        err = open(errlog, errors="replace").read().strip()
        if err:
            print("    --- QEMU error output ---")
            for line in err.splitlines()[-10:]:
                print("    " + line)
    return not failures


def main():
    cfgs = sys.argv[1:] or MATRIX
    if not all(os.path.exists(os.path.join(BUILD, f)) for f in ("fat32gpt.img", "exfat.img")):
        subprocess.run(["make", "testdisks"], check=True, stdout=subprocess.DEVNULL)
    failed = sum(0 if run(c) else 1 for c in cfgs)
    print("boot matrix: %d/%d passed" % (len(cfgs) - failed, len(cfgs)))
    sys.exit(failed)


if __name__ == "__main__":
    main()
