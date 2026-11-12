#!/bin/sh
# Build and run the host tests: the keyboard layouts, the network stack
# (netsim: simulated computers on a simulated Ethernet, and the fuzzer's
# seeds replayed), the USB HID report parser, the EDID / video mode choice,
# the HD Audio codec layer (hdatest.c: synthetic codecs), Notepad's text
# model and undo engine (notepadtest.c), the NTFS
# formatter (ntfsfmt.c: NTFSARGS=quick for fewer images and no FUSE),
# then the storage stack. Extra arguments
# are passed to fstest: "-v" for verbose output, scenario names
# (paths fat12 fat16 fat32 gpt 4k misc random testdisks hostile unicode format
# iso exfat ntfs ntfsw ntfssafe ntfsbig recycle) to run a subset; the NTFS driver
# scenarios (ntfstest.c) need root, FUSE and ntfs-3g.
set -e
cd "$(dirname "$0")"
make --no-print-directory kbd
out=../../build/hosttest
mkdir -p $out
make --no-print-directory net > $out/netsim.log 2>&1 || { cat $out/netsim.log; exit 1; }
grep -E "MB/s|simulated,|checks, " $out/netsim.log | grep -v "^netsim: [a-z]*: " || true
make --no-print-directory fuzz-net-regress > $out/fuzz_net.log 2>&1 || { cat $out/fuzz_net.log; exit 1; }
tail -1 $out/fuzz_net.log
# Notepad's text model (word wrap) and undo engine
make --no-print-directory notepad > $out/notepad.log 2>&1 || { cat $out/notepad.log; exit 1; }
tail -1 $out/notepad.log
# SETTINGS.INI: faults and power cuts at every step (settingstest), and at
# every sector on FAT32 / exFAT images, NTFS sampled (settingsfs)
make --no-print-directory settings > $out/settings.log 2>&1 || { cat $out/settings.log; exit 1; }
grep "^settings" $out/settings.log
# the NTFS formatter: images checked with ntfs-3g (skipped without it)
make --no-print-directory ntfsfmt NTFSARGS="$NTFSARGS" > $out/ntfsfmt.log 2>&1 || { cat $out/ntfsfmt.log; exit 1; }
tail -1 $out/ntfsfmt.log
exec make --no-print-directory test ARGS="$*"
