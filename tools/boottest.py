#!/usr/bin/env python3
"""Boot Halcyon in QEMU for a matrix of USB controllers / firmware and check
the kernel serial log.

usage: tools/boottest.py [CONFIG ...]     CONFIG = hc:fw[:variant[+variant]], e.g. xhci:uefi,
       xhci:bios:nvme+nomsi (default: the full matrix)

Each boot must reach "gui: desktop ready", mount the boot stick as C:, mount
the SATA test disks (FAT32/GPT and exFAT/MBR), write \\HALCYON\\BOOT.LOG, and
log no exception/panic.
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
  res     a saved resolution (display.width/height = 1280 x 1024 in
          \\HALCYON\\SETTINGS.INI): the loader must choose it and the desktop use it
  fwvideo no display adapter driver (boot flag): the firmware frame buffer only
  switch  switch the display mode to 1600 x 900 once the desktop is up (QEMU
          fw_cfg test hook; needs the live QEMU/Bochs display driver)
  cd      also an AHCI CD-ROM drive with an ISO 9660 + Joliet test disc
          (built with xorriso): the disc must mount, and after the desktop is
          up the disc is ejected and another one inserted through the QEMU
          monitor; the volume must go away and the new one mount
Exit status is the number of failed configurations.
"""
import os
import subprocess
import sys
import time

BUILD = os.environ.get("BUILD", "build")
MATRIX = ["uhci:bios", "ohci:bios", "ehci:bios", "ich9:bios", "xhci:bios",
          "uhci:uefi", "ehci:uefi", "ich9:uefi", "xhci:uefi",
          "xhci:bios:nomsi", "uhci:bios:pic", "xhci:uefi:pic", "ehci:bios:noirq",
          "xhci:bios:nvme", "xhci:uefi:nvme4k", "xhci:bios:nvme+nomsi", "xhci:uefi:nvme+noirq",
          "xhci:bios:cd", "xhci:bios:user", "uhci:uefi:user",
          "xhci:bios:res", "xhci:uefi:res+fwvideo", "xhci:uefi:switch"]
VARIANTS = {"pic": 0x200, "nomsi": 0x100, "noirq": 0x80, "nvme": 0, "nvme4k": 0, "cd": 0, "user": 0,
            "fwvideo": 0x800, "res": 0, "switch": 0}
NVME_ERRORS = ["failed", "timed out", "fatal", "offline", "not responding", "no interrupt seen"]
IDLE_MAX_CPU = 40      # % of one host CPU used by QEMU at the idle desktop
TIMEOUT = {"uhci": 90, "ohci": 60}
CHECKS = [
    ("desktop", "gui: desktop ready"),
    ("boot volume C:", "(boot volume)"),
    ("SATA volume", "on sata0"),
    ("exFAT volume", "exFAT 'EXFATDISK' on sata"),
    ("BOOT.LOG", "bootlog: saved"),
]
BAD = ["CPU EXCEPTION", "KERNEL PANIC", "nested fault"]


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
    extra = "-device usb-tablet,bus=usb.0 "
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
    if "user" in opts:
        extra += " -fw_cfg name=opt/halcyon/autorun,string=C:/PROGRAMS/SELFTEST.EXE"
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
        ini = os.path.join(BUILD, "ci_settings.ini")
        with open(ini, "w") as f:
            f.write("display.width=1280\r\ndisplay.height=1024\r\n")
        subprocess.run(["mcopy", "-o", "-i", img + "@@1M", ini, "::/HALCYON/SETTINGS.INI"], check=True)
    env["IMG_OVERRIDE"] = img
    errlog = os.path.join(BUILD, "qemu.err")
    p = subprocess.Popen(["sh", "tools/run.sh", hc, fw], env=env,
                         stdout=subprocess.DEVNULL, stderr=open(errlog, "w"))
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
    if "user" in opts:
        t1 = time.time()
        while time.time() - t1 < 90 and p.poll() is None:
            text = open(log, errors="replace").read()
            if "selftest: all" in text or "checks FAILED" in text or any(b in text for b in BAD):
                break
            time.sleep(0.5)
        time.sleep(1)
        text = open(log, errors="replace").read()
    idle = None
    if p.poll() is None and "gui: desktop ready" in text and "user" not in opts:
        time.sleep(2)
        c0 = cpu_seconds(p.pid)
        time.sleep(4)
        c1 = cpu_seconds(p.pid)
        if c0 is not None and c1 is not None:
            idle = (c1 - c0) * 100 / 4
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
    if "user" in opts:
        if "selftest: all" not in text:
            bad = [l for l in text.splitlines() if "selftest:" in l and ("FAIL" in l or "skipped" in l)]
            failures.append("selftest (%s)" % (bad[0].strip() if bad else "did not finish"))
    if isos:
        if "ISO9660 'HALCYONCD' on cd0" not in text:
            failures.append("CD volume")
        failures += cd_fails
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
    print("%-16s %5.1fs%s  %s" % (cfg, elapsed, cpu, status), flush=True)
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
