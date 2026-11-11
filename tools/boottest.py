#!/usr/bin/env python3
"""Boot Halcyon in QEMU for a matrix of USB controllers / firmware and check
the kernel serial log.

usage: tools/boottest.py [CONFIG ...]     CONFIG = hc:fw, e.g. xhci:uefi
       (default: the full matrix)

Each boot must reach "gui: desktop ready", mount the boot stick as C:, mount
the SATA test disk, write \\HALCYON\\BOOT.LOG, and log no exception/panic.
Exit status is the number of failed configurations.
"""
import os
import subprocess
import sys
import time

BUILD = os.environ.get("BUILD", "build")
MATRIX = ["uhci:bios", "ohci:bios", "ehci:bios", "ich9:bios", "xhci:bios",
          "uhci:uefi", "ehci:uefi", "ich9:uefi", "xhci:uefi"]
TIMEOUT = {"uhci": 90, "ohci": 60}
CHECKS = [
    ("desktop", "gui: desktop ready"),
    ("boot volume C:", "(boot volume)"),
    ("SATA volume", "on sata0"),
    ("BOOT.LOG", "bootlog: saved"),
]
BAD = ["CPU EXCEPTION", "KERNEL PANIC", "nested fault"]


def run(cfg):
    hc, fw = cfg.split(":")
    log = os.path.join(BUILD, "serial.log")
    if os.path.exists(log):
        os.remove(log)
    sata = os.path.join(BUILD, "ci_sata.img")
    subprocess.run(["cp", os.path.join(BUILD, "fat32gpt.img"), sata], check=True)
    env = dict(os.environ, HEADLESS="1", BUILD=BUILD,
               EXTRA="-device usb-tablet,bus=usb.0 -drive if=none,id=d0,format=raw,file=%s "
                     "-device ide-hd,drive=d0,bus=ide.0" % sata)
    # boot from a scratch copy so tests never modify the release image
    img = os.path.join(BUILD, "ci_boot.img")
    subprocess.run(["cp", os.path.join(BUILD, "halcyon.img"), img], check=True)
    env["IMG_OVERRIDE"] = img
    p = subprocess.Popen(["sh", "tools/run.sh", hc, fw], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    p.terminate()
    try:
        p.wait(5)
    except subprocess.TimeoutExpired:
        p.kill()
    failures = [name for name, pat in CHECKS if pat not in text]
    failures += ["log contains '%s'" % b for b in BAD if b in text]
    status = "PASS" if not failures else "FAIL (%s)" % ", ".join(failures)
    print("%-11s %5.1fs  %s" % (cfg, elapsed, status), flush=True)
    if failures:
        print("    --- last lines of the serial log ---")
        for line in text.strip().splitlines()[-15:]:
            print("    " + line)
    return not failures


def main():
    cfgs = sys.argv[1:] or MATRIX
    if not os.path.exists(os.path.join(BUILD, "fat32gpt.img")):
        subprocess.run(["make", "testdisks"], check=True, stdout=subprocess.DEVNULL)
    failed = sum(0 if run(c) else 1 for c in cfgs)
    print("boot matrix: %d/%d passed" % (len(cfgs) - failed, len(cfgs)))
    sys.exit(failed)


if __name__ == "__main__":
    main()
