#!/usr/bin/env python3
"""Boot Halcyon in QEMU for a matrix of USB controllers / firmware and check
the kernel serial log.

usage: tools/boottest.py [CONFIG ...]     CONFIG = hc:fw[:variant[+variant]], e.g. xhci:uefi,
       xhci:bios:nvme+nomsi (default: the full matrix); "matrix" and
       "install" (the installer's variants) name sets of CONFIGs

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
  help    run C:\\PROGRAMS\\HELP.EXE --test notepad after boot (Help and Support):
          the contents and index load, every page in the contents renders and
          its links lead to pages and anchors, Back / Forward, an anchor, the
          index filter and the search ranking work; it must report
          "helptest: all N checks passed"
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
  recycle the Recycle Bin and drag and drop (QEMU fw_cfg recycletest, a kernel
          thread: kernel/apps/recycletest.c): on the FAT32 test disk a file
          goes to the bin and stays, another goes to the bin, its folder is
          deleted for good and it is restored; on the exFAT disk two files
          go to the bin, which is emptied; and C:\\Documents\\Getting
          Started.txt is dragged onto EXPLORER.EXE showing the FAT32 disk's
          RBTEST folder, which must answer "copy" (another drive) and copy
          it. Afterwards, on the host: the bin's INDEX.TXT and D1.txt (mtools),
          the restored and copied files, no bin left on the exFAT disk
          (exfat_tool.py), and fsck.fat / fsck.exfat / exfat_tool.py check
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
  dusk    with a sound card variant: SETTINGS.INI selects the Halcyon Dusk
          sound scheme, and the recording must start with DUSKSTRT.WAV
          instead of STARTUP.WAV.
  tide    the same with the Halcyon Tide scheme (TIDESTRT.WAV).
or install Halcyon on a second disk with the installer and start that disk
alone ("install" as a CONFIG runs the whole set; see INSTALL_DOC below for
each variant): install, installnvme, installnvme4k, installntfs, installgpt
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
          "xhci:bios:cd", "xhci:bios:user", "xhci:bios:help", "uhci:uefi:user", "xhci:bios:sock", "xhci:bios:web",
          "xhci:bios:net", "xhci:bios:net+loss",
          "xhci:bios:res", "xhci:uefi:res+fwvideo", "xhci:uefi:switch",
          "xhci:bios:virtio", "xhci:uefi:e1000e", "xhci:bios:ac97", "xhci:bios:ac97+dusk", "xhci:uefi:hda+tide", "xhci:uefi:virtiosnd", "xhci:bios:audio",
          "xhci:uefi:hda", "xhci:bios:ntfs", "xhci:bios:ntfsrw", "xhci:bios:recycle", "xhci:bios:saver",
          "xhci:uefi:install", "xhci:bios:install"]
VARIANTS = {"pic": 0x200, "nomsi": 0x100, "noirq": 0x80, "nvme": 0, "nvme4k": 0, "cd": 0, "user": 0, "help": 0, "sock": 0, "web": 0,
            "fwvideo": 0x800, "res": 0, "switch": 0, "net": 0, "loss": 0, "beep": 0, "ntfs": 0, "ntfsrw": 0,
            "recycle": 0, "saver": 0}
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
VARIANTS["dusk"] = 0                     # with a sound card: the Halcyon Dusk scheme (DUSKSTRT.WAV)
VARIANTS["tide"] = 0                     # with a sound card: the Halcyon Tide scheme (TIDESTRT.WAV)
STARTUP_REF = ["STARTUP.WAV"]            # the startup sound the recording must start with (run() sets it)
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
PROGRAMS = {"user": ("SELFTEST.EXE", "selftest"), "help": ("HELP.EXE", "helptest"), "sock": ("SOCKTEST.EXE", "socktest"), "web": ("WEBTEST.EXE", "webtest"),
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
    ref_path = os.path.join(BUILD, "media", STARTUP_REF[0])
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


# ---------------- the installer (M8): install, then boot the installed disk ----------------
# Variants (the "install" set; see INSTALL_DOC below):
INSTALL_DOC = """
  install       the stick (BIOS: MBR, UEFI: GPT) and a blank 1 GiB SATA disk
                (model INSTALLDISK): the installer's test hook (fw_cfg
                installtest "INSTALLDISK whole") installs Halcyon on the whole
                disk as the wizard would. On the host: the partition table
                (gpt_check.py, sfdisk -V, sgdisk -v), fsck.fat on Halcyon's
                partition, the kernel, loaders and boot code byte for byte.
                Then QEMU starts from that disk alone, BIOS and UEFI (a fresh
                variable store: the disk's \\EFI\\BOOT\\BOOTX64.EFI; after a
                UEFI install also with the installer's variable store, where
                the firmware must start the "Halcyon" boot entry it added),
                and each boot must reach the desktop with the disk as C:
  installnvme   the same on an NVMe disk
  installnvme4k the same on an NVMe namespace with 4096-byte blocks (UEFI
                only: GPT without BIOS boot code, which PC BIOSes cannot start)
  installntfs   (needs root, mkntfs, ntfs-3g) an MBR disk with Windows' NTFS
                partition (active, \\Windows\\System32, made by mkntfs and
                ntfs-3g) and 512 MiB free: Halcyon goes into the free space
                ("free"). The NTFS partition must be byte for byte unchanged
                (hash), pass ntfsfix -n, and keep its MBR entry; the disk's
                original first sector is saved as \\HALCYON\\BOOT\\MBRSAVE.BIN.
                BIOS: the boot menu offers Windows and starts Halcyon after
                the countdown; a second boot chooses Windows (arrow down,
                Enter), and the NTFS boot sector must run (its message on the
                text screen). UEFI: Halcyon starts through its boot entry.
  installgpt    (needs root, mkntfs, ntfs-3g) a Windows-like GPT disk: an EFI
                system partition with \\EFI\\Microsoft\\Boot\\bootmgfw.efi and
                \\EFI\\BOOT\\BOOTX64.EFI, a Microsoft reserved partition, an
                NTFS basic data partition with \\Windows\\System32, 600 MiB
                free; installed from a UEFI boot into the free space. Windows'
                three partitions and their GPT entries must be unchanged, the
                NTFS one must pass ntfsfix -n; the disk must start Halcyon
                through its firmware boot entry (UEFI) and through its BIOS
                boot partition (BIOS)
"""
INSTALL_SET = ["xhci:uefi:install", "xhci:bios:install", "xhci:uefi:installnvme", "xhci:uefi:installnvme4k", "xhci:bios:installntfs",
               "xhci:uefi:installgpt"]
for _n in ("install", "installnvme", "installnvme4k", "installntfs", "installgpt"):
    VARIANTS[_n] = 0


def sh_ok(cmd):
    return subprocess.run(["sh", "-c", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def file_hash(path, start=0, length=None):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(start)
        left = length
        while left is None or left > 0:
            b = f.read(1 << 20 if left is None else min(1 << 20, left))
            if not b:
                break
            h.update(b)
            if left is not None:
                left -= len(b)
    return h.hexdigest()


def qemu_boot(hc, fw, extra, stick=None, vars_file=None, done=None, timeout=60, during=None):
    """boot headless through run.sh; wait until every pattern of done is in
    the serial log (or a BAD one, or timeout); during(log) runs meanwhile
    (keys through the monitor). Returns the log."""
    log = os.path.join(BUILD, "serial.log")
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ, HEADLESS="1", BUILD=BUILD, EXTRA=extra)
    if stick:
        env["IMG_OVERRIDE"] = stick
    else:
        env["NOSTICK"] = "1"
    env.pop("VARS", None)
    if vars_file:
        env["VARS"] = vars_file
    p = subprocess.Popen(["sh", "tools/run.sh", hc, fw], env=env, stdout=subprocess.DEVNULL,
                         stderr=open(os.path.join(BUILD, "qemu.err"), "w"))
    t0, text = time.time(), ""
    done = done or ["gui: desktop ready", "bootlog: saved"]
    result = None
    try:
        while time.time() - t0 < timeout and p.poll() is None:
            time.sleep(0.5)
            try:
                text = open(log, errors="replace").read()
            except OSError:
                continue
            if during:
                result = during(text)
                if result is not None:
                    break
            if all(d in text for d in done) or any(b in text for b in BAD):
                break
        time.sleep(1.5)
        try:
            text = open(log, errors="replace").read()
        except OSError:
            pass
    finally:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    return text, result


def vga_text():
    """the VGA text screen (80 x 25) through the QEMU monitor"""
    dump = os.path.abspath(os.path.join(BUILD, "ci_vga.bin"))
    subprocess.run([sys.executable, "tools/qemu_ctl.py", os.path.join(BUILD, "mon.sock"),
                    "pmemsave 0xb8000 4000 \"%s\"" % dump], stdout=subprocess.DEVNULL)
    time.sleep(0.3)
    try:
        b = open(dump, "rb").read()
    except OSError:
        return ""
    return "\n".join(bytes(b[r * 160 + i] for i in range(0, 160, 2)).decode("latin-1") for r in range(25))


def make_install_disk(kind):
    """the target disk of an install variant; (path, Windows partitions as
    [(start, sectors)], which disk model) or None if the tools are missing"""
    disk = os.path.abspath(os.path.join(BUILD, "ci_install.img"))
    if os.path.exists(disk):
        os.remove(disk)
    if kind in ("install", "installnvme", "installnvme4k"):
        with open(disk, "wb") as f:
            f.truncate(1 << 30)
        return disk, []
    if os.geteuid() != 0 or not sh_ok("command -v mkntfs && command -v ntfs-3g && test -w /dev/fuse"):
        return None
    vol = os.path.join(BUILD, "ci_install_ntfs.img")
    if kind == "installntfs":
        start, count = 2048, 512 * 2048
        subprocess.run([sys.executable, "tests/host/ntfs_mkimg.py", vol, "512", "boot", "-p", str(start), "-H", "255",
                        "-S", "63"], check=True, stdout=subprocess.DEVNULL)
        with open(disk, "wb") as f:
            f.truncate(1 << 30)
        subprocess.run(["sfdisk", "-q", disk], input="label: dos\nlabel-id: 0x77696e00\nstart=%d, size=%d, type=7, bootable\n"
                       % (start, count), text=True, check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["dd", "if=" + vol, "of=" + disk, "bs=512", "seek=%d" % start, "conv=notrunc,sparse", "status=none"],
                       check=True)
        return disk, [(start, count)]
    # installgpt: ESP 100 MiB, MSR 16 MiB, NTFS 700 MiB, 600 MiB free (+ the GPT's own room)
    with open(disk, "wb") as f:
        f.truncate((2048 + (100 + 16 + 700 + 600) * 2048 + 2048) * 512)
    esp, msr, nt = (2048, 100 * 2048), (2048 + 100 * 2048, 16 * 2048), (2048 + 116 * 2048, 700 * 2048)
    subprocess.run(["sfdisk", "-q", disk], text=True, check=True, stdout=subprocess.DEVNULL, input=(
        "label: gpt\nstart=%d, size=%d, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI system partition\"\n"
        "start=%d, size=%d, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE, name=\"Microsoft reserved partition\"\n"
        "start=%d, size=%d, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"Basic data partition\"\n") % (esp + msr + nt))
    espimg = os.path.join(BUILD, "ci_install_esp.img")
    if os.path.exists(espimg):
        os.remove(espimg)
    subprocess.run(["mkfs.fat", "-F", "32", "-C", espimg, str(esp[1] // 2)], check=True, stdout=subprocess.DEVNULL)
    stub = os.path.join(BUILD, "ci_install_stub.efi")
    with open(stub, "wb") as f:
        f.write(b"not a real boot manager\n" * 100)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    for d in ("::/EFI", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot", "::/EFI/BOOT"):
        subprocess.run(["mmd", "-i", espimg, d], check=True, env=env)
    subprocess.run(["mcopy", "-i", espimg, stub, "::/EFI/Microsoft/Boot/bootmgfw.efi"], check=True, env=env)
    subprocess.run(["mcopy", "-i", espimg, stub, "::/EFI/BOOT/BOOTX64.EFI"], check=True, env=env)
    subprocess.run(["dd", "if=" + espimg, "of=" + disk, "bs=512", "seek=%d" % esp[0], "conv=notrunc", "status=none"],
                   check=True)
    with open(disk, "r+b") as f:                   # something in the MSR partition too
        f.seek(msr[0] * 512)
        f.write(bytes(range(256)) * 64)
    with open(disk, "r+b") as f:                   # boot code in the protective MBR, as Windows writes it
        code = (b"\xfa\x33\xc0\x8e\xd0\xbc\x00\x7c" + b"\x90" * 300 + b"Invalid partition table\0"
                b"Error loading operating system\0Missing operating system\0")
        f.write(code[:440].ljust(440, b"\0"))
    subprocess.run([sys.executable, "tests/host/ntfs_mkimg.py", vol, "700", "boot", "-p", str(nt[0])], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["dd", "if=" + vol, "of=" + disk, "bs=512", "seek=%d" % nt[0], "conv=notrunc,sparse", "status=none"],
                   check=True)
    return disk, [esp, msr, nt]


def nvme_dev(disk, ss):
    """an NVMe controller with the disk as namespace 1 (512- or 4096-byte blocks)"""
    if ss == 512:
        return "-drive if=none,id=d0,format=raw,file=%s -device nvme,serial=HALCINST,drive=d0" % disk
    return ("-device nvme,id=nvme0,serial=HALCINST -drive if=none,id=d0,format=raw,file=%s "
            "-device nvme-ns,drive=d0,bus=nvme0,nsid=1,logical_block_size=%d,physical_block_size=%d" % (disk, ss, ss))


def gpt_entries(disk):
    with open(disk, "rb") as f:
        f.seek(1024)
        return f.read(128 * 128)


def installed_disk_failures(disk, gpt, windows, ss=512):
    """the installed disk, on the host"""
    fails = []
    r = subprocess.run([sys.executable, "tests/host/gpt_check.py", disk, str(ss)], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        fails.append("gpt_check: " + r.stdout.strip().replace("\n", "; "))
    lines = [l.split() for l in r.stdout.splitlines() if l.startswith(("gpt ", "mbr "))]
    if ss == 512 and not sh_ok("sfdisk -V '%s'" % disk):
        fails.append("sfdisk -V")
    if gpt and ss == 512 and sh_ok("command -v sgdisk") and not sh_ok("sgdisk -v '%s' | grep -q 'No problems found'" % disk):
        fails.append("sgdisk -v")
    # Halcyon's partition: the ESP-type / type 0c one that is not Windows'
    win_starts = {s for s, _ in windows}
    part = None
    for l in lines:
        if l[0] == "gpt" and l[4] == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" and int(l[2]) not in win_starts:
            part = (int(l[2]), int(l[3]) - int(l[2]) + 1)
        if l[0] == "mbr" and l[4] == "0c" and int(l[2]) not in win_starts:
            part = (int(l[2]), int(l[3]))
    if not part:
        return fails + ["no Halcyon partition in the table"], None, None
    img = os.path.join(BUILD, "ci_install_part.img")
    subprocess.run(["dd", "if=" + disk, "of=" + img, "bs=%d" % ss, "skip=%d" % part[0], "count=%d" % part[1],
                    "conv=sparse", "status=none"], check=True)
    r = subprocess.run(["fsck.fat", "-n", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        fails.append("fsck.fat: " + r.stdout.strip().splitlines()[-1])
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    for src, dst in (("KERNEL.BIN", "::/HALCYON/KERNEL.BIN"), ("efi/BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI"),
                     ("boot/stage2.bin", "::/HALCYON/BOOT/STAGE2.BIN"), ("user/notepad.exe", "::/PROGRAMS/NOTEPAD.EXE"),
                     ("media/STARTUP.WAV", "::/HALCYON/MEDIA/STARTUP.WAV"), ("help/install.htm", "::/HALCYON/HELP/install.htm")):
        out = os.path.join(BUILD, "ci_install_file.bin")
        if os.path.exists(out):
            os.remove(out)
        subprocess.run(["mcopy", "-n", "-i", img, dst, out], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not os.path.exists(out) or open(out, "rb").read() != open(os.path.join(BUILD, src), "rb").read():
            fails.append("%s differs from the build" % dst)
    r = subprocess.run(["mdir", "-b", "-/", "-i", img, "::/Documents"], env=env, stdout=subprocess.PIPE, text=True)
    if "Getting Started.txt" not in r.stdout:
        fails.append("Documents not copied")
    return fails, part, img


def run_install(cfg, hc, fw, opts):
    kind = next(v for v in opts if v.startswith("install"))
    t_start = time.time()
    made = make_install_disk(kind)
    if not made:
        print("%-22s SKIP (needs root, mkntfs and ntfs-3g)" % cfg, flush=True)
        return True
    disk, windows = made
    win_hash = [file_hash(disk, s * 512, n * 512) for s, n in windows]
    sector0 = open(disk, "rb").read(512)
    entries0 = gpt_entries(disk) if kind == "installgpt" else None
    stick = os.path.join(BUILD, "ci_boot.img")
    subprocess.run(["cp", os.path.join(BUILD, "halcyon.img"), stick], check=True)
    if kind == "installntfs":            # a short boot menu countdown (copied to the installed disk)
        ini = os.path.join(BUILD, "ci_settings.ini")
        with open(ini, "w") as f:
            f.write("boot.timeout=3\r\n")
        subprocess.run(["mcopy", "-o", "-i", stick + "@@1M", ini, "::/HALCYON/SETTINGS.INI"], check=True)
    vars_file = os.path.abspath(os.path.join(BUILD, "ci_install_vars.fd"))
    if fw == "uefi":
        subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", vars_file], check=True)
    spec = os.path.abspath(os.path.join(BUILD, "ci_installtest.txt"))
    nvme = kind.startswith("installnvme")
    ss = 4096 if kind == "installnvme4k" else 512
    with open(spec, "w") as f:
        f.write("%s %s" % ("nvme0" if nvme else "INSTALLDISK", "free" if kind in ("installntfs", "installgpt") else "whole"))
    if nvme:
        target = nvme_dev(disk, ss)
    else:
        target = "-drive if=none,id=d0,format=raw,file=%s -device ide-hd,drive=d0,bus=ide.0,model=INSTALLDISK" % disk
    extra = "-device usb-tablet,bus=usb.0 -device usb-kbd,bus=usb.0 %s -fw_cfg name=opt/halcyon/installtest,file=%s" % (
        target, spec)
    text, _ = qemu_boot(hc, fw, extra, stick=stick, vars_file=vars_file if fw == "uefi" else None,
                        done=["install test: "], timeout=150)
    fails = []
    line = next((l for l in text.splitlines() if l.startswith("install test: done") or l.startswith("install test: failed")), None)
    plan = next((l for l in text.splitlines() if l.startswith("install test: plan:")), "")
    if not line or "failed" in line:
        fails.append(line.strip() if line else "the installation did not finish")
    fails += ["log contains '%s'" % b for b in BAD if b in text]
    gpt = "GPT" in plan
    bios_code = "BIOS boot code yes" in plan
    if fw == "uefi" and "boot entry -" in (line or ""):
        fails.append("no firmware boot entry")
    boots = []
    if not fails:
        f2, part, partimg = installed_disk_failures(disk, gpt, windows, ss)
        fails += f2
        for i, (s, n) in enumerate(windows):
            if file_hash(disk, s * 512, n * 512) != win_hash[i]:
                fails.append("Windows' partition %d changed" % (i + 1))
        if kind in ("installntfs", "installgpt"):
            s0 = open(disk, "rb").read(512)
            keep = (446, 462) if kind == "installntfs" else (440, 512)
            if s0[keep[0]:keep[1]] != sector0[keep[0]:keep[1]] or s0[440:444] != sector0[440:444]:
                fails.append("Windows' (protective) MBR entry or the disk signature changed")
            if not bios_code:
                fails.append("no BIOS boot code")
            out = os.path.join(BUILD, "ci_install_file.bin")
            if os.path.exists(out):
                os.remove(out)
            subprocess.run(["mcopy", "-n", "-i", partimg, "::/HALCYON/BOOT/MBRSAVE.BIN", out],
                           env=dict(os.environ, MTOOLS_SKIP_CHECK="1"), stdout=subprocess.DEVNULL)
            if not os.path.exists(out) or open(out, "rb").read() != sector0:
                fails.append("MBRSAVE.BIN is not the original first sector")
        if kind == "installgpt":
            e1 = gpt_entries(disk)
            if e1[:3 * 128] != entries0[:3 * 128]:
                fails.append("Windows' GPT entries changed")
        for i, (s, n) in enumerate(windows):
            if n > 200 * 2048:          # the NTFS partitions
                vol = os.path.join(BUILD, "ci_install_ntfs.img")
                subprocess.run(["dd", "if=" + disk, "of=" + vol, "bs=512", "skip=%d" % s, "count=%d" % n,
                                "conv=sparse", "status=none"], check=True)
                r = subprocess.run(["ntfsfix", "-n", vol], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                if r.returncode or "processed successfully" not in r.stdout:
                    fails.append("ntfsfix -n on Windows' partition")
        survey = [l for l in text.splitlines() if l.startswith("install test: disk ")]
        if windows and not any(l.endswith(", Windows") for l in survey):
            fails.append("Windows not detected on the disk")
        # boot the installed disk alone
        dev = (nvme_dev(disk, ss) if nvme else
               "-drive if=none,id=d0,format=raw,file=%s -device ide-hd,drive=d0,bus=ide.0" % disk)
        dev += " -device usb-tablet,bus=usb.0 -device usb-kbd,bus=usb.0"
        runs = []
        if bios_code:
            runs.append(("bios", None))
        runs.append(("uefi", "fresh"))
        if fw == "uefi":
            runs.append(("uefi", "installer's"))
        for bfw, vars_kind in runs:
            vf = None
            if bfw == "uefi":
                vf = vars_file
                if vars_kind == "fresh":
                    vf = os.path.abspath(os.path.join(BUILD, "ci_install_vars2.fd"))
                    subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", vf], check=True)
            if kind == "installgpt" and vars_kind == "fresh":
                continue            # (Windows' ESP comes first: without the boot entry the firmware starts that)
            t0 = time.time()
            text2, _ = qemu_boot(hc, bfw, dev, vars_file=vf, timeout=60)
            name = "%s%s" % (bfw, " (%s variables)" % vars_kind if vars_kind else "")
            bf = []
            if "gui: desktop ready" not in text2:
                bf.append("no desktop")
            pdev = "nvme0" if nvme else "sata0"
            if not re.search(r"vfs: C: FAT32 'HALCYON' on %sp\d+ \(boot volume\)" % pdev, text2):
                bf.append("the installed disk is not C:")
            bf += ["log contains '%s'" % b for b in BAD if b in text2]
            if vars_kind == "installer's" and not re.search(r"efi: started from firmware boot entry Boot[0-9A-F]{4} 'Halcyon'", text2):
                bf.append("not started through the 'Halcyon' boot entry")
            boots.append("%s %.0fs" % (name, time.time() - t0))
            fails += ["%s boot: %s" % (name, f) for f in bf]
        if kind == "installntfs" and not fails:
            # the boot menu: choose Windows, its boot sector must run
            def keys(text3):
                if "Please select the operating system to start" not in vga_text():
                    return None
                subprocess.run([sys.executable, "tools/qemu_ctl.py", os.path.join(BUILD, "mon.sock"), "sendkey down",
                                "sleep:0.3", "sendkey ret", "sleep:2"], stdout=subprocess.DEVNULL)
                return vga_text()
            _, screen = qemu_boot(hc, "bios", dev, done=["never"], timeout=10, during=keys)
            if not screen or "This is not a bootable disk" not in screen:
                fails.append("choosing Windows in the boot menu did not start its boot sector")
            else:
                boots.append("Windows chosen")
    status = "PASS" if not fails else "FAIL (%s)" % ", ".join(fails)
    print("%-22s %5.1fs  %s" % (cfg, time.time() - t_start, status), flush=True)
    print("    %s; booted: %s" % (plan.replace("install test: plan: ", "").strip() or "no plan", ", ".join(boots) or "-"))
    if fails:
        print("    --- last lines of the serial log ---")
        for l in text.strip().splitlines()[-15:]:
            print("    " + l)
    return not fails


RB_DELETED = b"This file waits in the Recycle Bin.\r\n"
RB_RESTORED = b"This file came back from the Recycle Bin.\r\n"


def recycle_failures(text, sata, sata_x):
    """the Recycle Bin test's disks, checked on the host"""
    m = re.search(r"recycle test: done \((\d+) checks, (\d+) failed\)", text)
    if not m:
        return ["recycle test did not finish"]
    fails = [l[l.index("recycle test:"):].strip() for l in text.splitlines() if "recycle test:" in l and "FAILED" in l][:4]
    fat = sata + "@@1M"                 # (fat32gpt.img: the partition at LBA 2048)

    def mtype(path):
        r = subprocess.run(["mtype", "-i", fat, "::" + path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
        return r.stdout if r.returncode == 0 else None
    index = mtype("/RECYCLED/INDEX.TXT") or b""
    lines = index.split(b"\r\n")
    if lines[0] != b"HALCYON RECYCLE BIN 1" or b"END 1" not in lines or \
            not any(re.match(rb"1\t\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\t%d\tF\t[D-Z]:/RBTEST/deleted\.txt$" % len(RB_DELETED), l)
                    for l in lines):
        fails.append("the bin's index on the FAT32 disk: %r" % index[:160])
    if mtype("/RECYCLED/D1.txt") != RB_DELETED:
        fails.append("RECYCLED\\D1.txt")
    if mtype("/RBTEST/deleted.txt") is not None:
        fails.append("deleted.txt still in its folder")
    if mtype("/RBTEST/Sub/restored.txt") != RB_RESTORED:
        fails.append("RBTEST\\Sub\\restored.txt not restored")
    want = open("rootfs/Documents/Getting Started.txt", "rb").read()
    if mtype("/RBTEST/Getting Started.txt") != want:
        fails.append("the dropped copy of Getting Started.txt")
    part = os.path.join(BUILD, "ci_rb_part.img")
    subprocess.run(["dd", "if=" + sata, "of=" + part, "bs=512", "skip=2048", "status=none"], check=True)
    r = subprocess.run(["fsck.fat", "-n", part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        fails.append("fsck.fat: " + r.stdout.strip().splitlines()[-1])
    xt = [sys.executable, "tests/host/exfat_tool.py", sata_x + "@@1048576"]
    r = subprocess.run(xt + ["ls"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode or "RECYCLED" in r.stdout.upper() or "D RBX" not in r.stdout or "RBX/" in r.stdout:
        fails.append("the exFAT disk after emptying its bin: " + " ".join(r.stdout.split()[:12]))
    r = subprocess.run(xt + ["check"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        fails.append("exfat_tool.py check: " + r.stdout.strip().splitlines()[0])
    subprocess.run(["dd", "if=" + sata_x, "of=" + part, "bs=512", "skip=2048", "status=none"], check=True)
    if subprocess.run(["fsck.exfat", "-n", part], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        fails.append("fsck.exfat")
    os.remove(part)
    return fails


def run(cfg):
    parts = cfg.split(":")
    hc, fw = parts[0], parts[1]
    opts = [v for v in (parts[2] if len(parts) > 2 else "").split("+") if v]
    if any(v.startswith("install") for v in opts):
        return run_install(cfg, hc, fw, opts)
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
    if "recycle" in opts:
        extra += " -fw_cfg name=opt/halcyon/recycletest,string=1"
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
    prog_args = " --test notepad" if "help" in opts else ""
    ini_lines = []
    STARTUP_REF[0] = "DUSKSTRT.WAV" if "dusk" in opts else "TIDESTRT.WAV" if "tide" in opts else "STARTUP.WAV"
    for sch in ("dusk", "tide"):
        if sch in opts:
            ini_lines.append("sound.scheme=" + sch)
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
    if "recycle" in opts:
        t1 = time.time()
        while time.time() - t1 < 60 and p.poll() is None and "recycle test: done" not in text:
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
    if "recycle" in opts and not nvme:
        failures += recycle_failures(text, sata, sata_x)
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
    cfgs = []
    for a in sys.argv[1:] or MATRIX:
        for c in INSTALL_SET if a == "install" else MATRIX if a == "matrix" else [a]:     # (named sets)
            if c not in cfgs:
                cfgs.append(c)
    if not all(os.path.exists(os.path.join(BUILD, f)) for f in ("fat32gpt.img", "exfat.img")):
        subprocess.run(["make", "testdisks"], check=True, stdout=subprocess.DEVNULL)
    failed = sum(0 if run(c) else 1 for c in cfgs)
    print("boot matrix: %d/%d passed" % (len(cfgs) - failed, len(cfgs)))
    sys.exit(failed)


if __name__ == "__main__":
    main()
