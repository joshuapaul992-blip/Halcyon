#!/bin/sh
# Build and run the host tests: the USB HID report parser (hidtest, always,
# a second or two) and then the storage stack. Extra arguments are passed to
# fstest: "-v" for verbose output, scenario names (paths fat12 fat16 fat32 gpt
# 4k misc random testdisks hostile unicode format exfat) to run a subset.
set -e
cd "$(dirname "$0")"
exec make --no-print-directory test ARGS="$*"
