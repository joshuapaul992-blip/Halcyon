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
  loss    (with net) 3% of frames dropped on receive and transmit by the
          stack's test hook (fw_cfg opt/halcyon/netloss): a shorter NETTEST
          run that must still pass, with retransmissions
  res     a saved resolution (display.width/height = 1280 x 1024 in
          \\HALCYON\\SETTINGS.INI): the loader must choose it and the desktop use it
  fwvideo no display adapter driver (boot flag): the firmware frame buffer only
  switch  switch the display mode to 1600 x 900 once the desktop is up (QEMU
          fw_cfg test hook; needs the live QEMU/Bochs display driver)
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
          "xhci:bios:virtio", "xhci:uefi:e1000e"]
VARIANTS = {"pic": 0x200, "nomsi": 0x100, "noirq": 0x80, "nvme": 0, "nvme4k": 0, "cd": 0, "user": 0, "sock": 0, "web": 0,
            "fwvideo": 0x800, "res": 0, "switch": 0, "net": 0, "loss": 0}
# network variants: QEMU device, the driver's name, whether it has MSI-X
NICS = {"virtio": ("virtio-net-pci", "virtio-net", True),
        "virtiolegacy": ("virtio-net-pci,disable-modern=on", "virtio-net", True),
        "virtiomodern": ("virtio-net-pci-non-transitional", "virtio-net", True),
        "e1000": ("e1000", "e1000", False), "e1000e": ("e1000e", "e1000e", True),
        "igb": ("igb", "igb", True), "rtl8139": ("rtl8139", "rtl8139", False)}
for _n in NICS:
    VARIANTS[_n] = 0
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
            "net": ("NETTEST.EXE", "nettest")}
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
        if sent != NET_PROBE or replies != sent or done != sent or e1 or e2 or e3:
            fails.append("probe: %s" % m.group(0).split("probe: ")[1])
        want = "polled" if "noirq" in opts else "MSI-X" if msix and "nomsi" not in opts else "INTx"
        if not irq.startswith(want) or "lost" in irq or "none seen" in irq or (want != "polled" and "polled" in irq):
            fails.append("interrupt %s (expected %s)" % (irq, want))
    m = re.search(r"net: eth0: probe burst: (\d+) sent \(transmit ring full \d+ times\), (\d+) transmitted, "
                  r"\d+ replies in \d+ ms; errors: (\d+) tx, (\d+) rx", text)
    if not m:
        fails.append("probe burst did not finish")
    elif int(m.group(1)) != NET_PROBE // 4 or int(m.group(2)) != int(m.group(1)) or int(m.group(3)) or int(m.group(4)):
        fails.append("burst: %s" % m.group(0).split("burst: ")[1])
    bad = [l for l in lines if l.startswith("irq: ") and driver in l and ("stopped arriving" in l or "polling it" in l)]
    if bad:
        fails.append(bad[0].strip())
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
    if flags:
        extra += " -fw_cfg name=opt/halcyon/bootflags,string=%#x" % flags
    nic = next((v for v in opts if v in NICS), None)
    if nic:
        extra += " -netdev user,id=n0 -device %s,netdev=n0 -fw_cfg name=opt/halcyon/netprobe,string=%d" % (
            NICS[nic][0], NET_PROBE)
    prog = next((PROGRAMS[v] for v in opts if v in PROGRAMS), None)
    prog_args = ""
    ini_lines = []
    net = "net" in opts
    if net:
        ns_mod, ports = net_servers()
        fwd = ns_mod.free_port()
        extra += " -netdev user,id=n0,hostfwd=tcp:127.0.0.1:%d-:8080 -device e1000,netdev=n0" % fwd
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
    idle = None
    if p.poll() is None and "gui: desktop ready" in text and not prog:
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
    if nic:
        failures += net_failures(text, nic, opts)
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
