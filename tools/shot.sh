#!/bin/sh
# Boot headless, wait, take a screenshot, print serial log.
#   tools/shot.sh HC FW SECONDS OUT.png [extra monitor cmds...]
BUILD=${BUILD:-build}
HC=$1; FW=$2; T=$3; OUT=$4; shift 4
rm -f $BUILD/serial.log $BUILD/mon.sock
HEADLESS=1 sh tools/run.sh $HC $FW > $BUILD/qemu.out 2>&1 &
PID=$!
sleep $T
python3 tools/qemu_ctl.py $BUILD/mon.sock "$@" png:$OUT
kill $PID 2>/dev/null; wait $PID 2>/dev/null
cat $BUILD/qemu.out
cat $BUILD/serial.log 2>/dev/null
