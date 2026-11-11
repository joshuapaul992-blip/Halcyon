#!/bin/sh
# Halcyon continuous-integration checks. Run locally or from CI.
#   tools/ci.sh            build + host FAT tests + QEMU boot matrix
#   tools/ci.sh quick      build + host FAT tests + xhci:bios and xhci:uefi boots
set -e
cd "$(dirname "$0")/.."

echo "== build"
make clean >/dev/null
out=$(make 2>&1)
echo "$out" | tail -1
if echo "$out" | grep -E "warning:|error:"; then
    echo "build produced warnings or errors"
    exit 1
fi
make testdisks >/dev/null

echo "== host file system tests"
tests/host/run.sh 2>&1 | tail -1

echo "== host web tests"
if ! web=$(sh tests/web/run.sh 2>&1); then
    echo "$web" | grep -v "^RESULT" | tail -30
    exit 1
fi
echo "$web" | grep -E "^(url|trust anchors|inflate|images|html|web):"

echo "== QEMU boot matrix"
if [ "$1" = quick ]; then
    python3 tools/boottest.py xhci:bios xhci:uefi
else
    python3 tools/boottest.py
fi
