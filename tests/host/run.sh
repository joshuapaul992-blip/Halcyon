#!/bin/sh
# Build and run the host tests: the keyboard layouts, the network stack
# (netsim: simulated computers on a simulated Ethernet, and the fuzzer's
# seeds replayed), the USB HID report parser, the EDID / video mode choice,
# then the storage stack. Extra arguments
# are passed to fstest: "-v" for verbose output, scenario names
# (paths fat12 fat16 fat32 gpt 4k misc random testdisks hostile unicode format
# exfat) to run a subset.
set -e
cd "$(dirname "$0")"
make --no-print-directory kbd
out=../../build/hosttest
mkdir -p $out
make --no-print-directory net > $out/netsim.log 2>&1 || { cat $out/netsim.log; exit 1; }
grep -E "MB/s|simulated,|checks, " $out/netsim.log | grep -v "^netsim: [a-z]*: " || true
make --no-print-directory fuzz-net-regress > $out/fuzz_net.log 2>&1 || { cat $out/fuzz_net.log; exit 1; }
tail -1 $out/fuzz_net.log
exec make --no-print-directory test ARGS="$*"
