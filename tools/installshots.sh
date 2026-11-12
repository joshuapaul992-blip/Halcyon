#!/bin/sh
# Screenshots of Halcyon Setup (the installer wizard), every page, in QEMU.
#   tools/installshots.sh OUTDIR [DISK.img] [bios|uefi]
# DISK.img is the target disk (copied first; default: a blank 2 GiB disk).
# For a disk with Windows on it (needs root, mkntfs, ntfs-3g):
#   python3 -c "import sys; sys.path.insert(0, 'tools'); import boottest; boottest.make_install_disk('installntfs')"
#   tools/installshots.sh shots build/ci_install.img
# A 100 MB file is added to the stick's copy so that the progress page can
# be caught. Uses the installer's test hook (fw_cfg installtest "... wizard").
set -e
BUILD=${BUILD:-build}
O=$1; D=$2; FW=${3:-uefi}
[ -n "$O" ] || { echo "usage: $0 OUTDIR [DISK.img] [bios|uefi]"; exit 2; }
cd "$(dirname "$0")/.."
mkdir -p "$O"
T=$BUILD/shot_target.img
if [ -n "$D" ]; then cp "$D" $T; else rm -f $T; truncate -s 2G $T; fi
cp $BUILD/halcyon.img $BUILD/ci_boot.img
head -c 100000000 /dev/urandom > $BUILD/shot_big.bin
MTOOLS_SKIP_CHECK=1 mcopy -i $BUILD/ci_boot.img@@1M $BUILD/shot_big.bin "::/Documents/Big file.bin"
printf 'INSTALLDISK wizard' > $BUILD/shot_spec.txt
DEV="-device usb-tablet,bus=usb.0 -device usb-kbd,bus=usb.0 -drive if=none,id=d0,format=raw,file=$T -device ide-hd,drive=d0,bus=ide.0,model=INSTALLDISK"
M=$BUILD/mon.sock
ctl() { python3 tools/qemu_ctl.py $M "$@"; }
wait_log() { for i in $(seq 1 90); do sleep 1; grep -q "$1" $BUILD/serial.log 2>/dev/null && return 0; done; return 1; }

rm -f $BUILD/serial.log
HEADLESS=1 IMG_OVERRIDE=$BUILD/ci_boot.img EXTRA="$DEV -fw_cfg name=opt/halcyon/installtest,file=$BUILD/shot_spec.txt" \
    timeout 300 sh tools/run.sh xhci $FW >/dev/null 2>&1 &
wait_log "desktop ready"
sleep 3
ctl move:1010,720 sleep:0.5 png:$O/1-welcome.png
ctl "sendkey ret" sleep:2 png:$O/2-disk.png
ctl "sendkey ret" sleep:1 png:$O/3-mode.png
ctl "sendkey up" sleep:1 png:$O/4-mode-other.png
ctl "sendkey ret" sleep:2 png:$O/5-summary.png
ctl "sendkey ret" sleep:1 png:$O/6-confirm-erase.png          # (only for the whole disk)
ctl "sendkey ret" sleep:1                                      # No (the default)
ctl click:537,541 sleep:1 "sendkey down" sleep:1 "sendkey ret" sleep:2 move:1010,720 png:$O/7-summary-2.png
ctl "sendkey ret" sleep:1 "sendkey tab" "sendkey ret" sleep:1.5 png:$O/8-progress.png   # (Tab, Enter: Yes if it asks)
wait_log "install: Done\|install: failed" || true
sleep 2
ctl png:$O/9-finish.png
pkill -f "file=$T" || true
sleep 1

# the welcome prompt (a boot from the stick) and the Start menu
rm -f $BUILD/serial.log
WELCOME=1 HEADLESS=1 IMG_OVERRIDE=$BUILD/ci_boot.img EXTRA="$DEV" timeout 120 sh tools/run.sh xhci $FW >/dev/null 2>&1 &
wait_log "desktop ready"
sleep 3
ctl move:1010,720 sleep:0.5 png:$O/0-welcome-prompt.png
ctl "sendkey esc" sleep:1 click:40,753 sleep:1 png:$O/0-start-menu.png
pkill -f "file=$T" || true
echo "screenshots in $O"
