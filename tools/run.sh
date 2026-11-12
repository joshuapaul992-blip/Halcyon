#!/bin/sh
# Run Halcyon in QEMU.
#   tools/run.sh [hc] [fw]       hc = uhci | ohci | ehci | xhci (default xhci), fw = bios | uefi
# Environment:
#   HEADLESS=1   no window; serial log to $BUILD/serial.log, monitor at $BUILD/mon.sock
#                (and no "Install Halcyon" welcome prompt, unless WELCOME=1)
#   EXTRA="..."  extra QEMU arguments
#   CPUARG="..." CPU model, e.g. "-cpu max"
#   NOSTICK=1    no USB stick (start from a disk in EXTRA, e.g. one Halcyon was installed on)
#   VARS=file    UEFI variable store to use as it is (default: a fresh copy in $BUILD/ovmf_vars.fd)
BUILD=${BUILD:-build}
HC=${1:-xhci}
FW=${2:-bios}
IMG=${IMG_OVERRIDE:-$BUILD/halcyon.img}

case $HC in
  uhci) CTRL="-device piix3-usb-uhci,id=usb" ; BUS=usb.0 ;;
  ohci) CTRL="-device pci-ohci,id=usb" ; BUS=usb.0 ;;
  ehci) CTRL="-device usb-ehci,id=usb" ; BUS=usb.0 ;;
  ich9) CTRL="-device ich9-usb-ehci1,id=usb -device ich9-usb-uhci1,masterbus=usb.0,firstport=0,multifunction=on -device ich9-usb-uhci2,masterbus=usb.0,firstport=2 -device ich9-usb-uhci3,masterbus=usb.0,firstport=4" ; BUS=usb.0 ;;
  xhci) CTRL="-device qemu-xhci,id=usb" ; BUS=usb.0 ;;
  *) echo "unknown controller $HC"; exit 1 ;;
esac

FWARGS=""
if [ "$FW" = uefi ]; then
  if [ -z "$VARS" ]; then
    VARS=$BUILD/ovmf_vars.fd
    cp /usr/share/OVMF/OVMF_VARS_4M.fd $VARS 2>/dev/null
  fi
  FWARGS="-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=$VARS"
fi

if [ -n "$HEADLESS" ]; then
  DISP="-display none -serial file:$BUILD/serial.log -monitor unix:$BUILD/mon.sock,server,nowait -qmp unix:$BUILD/qmp.sock,server,nowait"
  [ -z "$WELCOME" ] && DISP="$DISP -fw_cfg name=opt/halcyon/nowelcome,string=1"
else
  DISP="-serial stdio"
fi

STICK="-drive if=none,id=stick,format=raw,file=$IMG -device usb-storage,bus=$BUS,drive=stick,bootindex=0"
[ -n "$NOSTICK" ] && STICK=""

exec qemu-system-x86_64 -machine q35 $CPUARG -m 512 -no-reboot $FWARGS $DISP $CTRL \
  $STICK \
  $EXTRA
