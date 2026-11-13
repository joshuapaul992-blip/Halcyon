#!/bin/sh
# Host tests of Halcyon's float math (userland/lib/math.c) and 3D library
# (userland/lib/gl), with ASan and UBSan: the math against the host's
# double-precision library, the 3D library's scenes against the reference
# pictures in tests/gl/ref, exact and API checks (needs gcc).
#   tests/gl/run.sh [-v]     math and scenes (the last line sums them up)
#   tests/gl/run.sh bench    the benchmark (frames per second, not a test)
set -e
cd "$(dirname "$0")/../.."
OUT=build/hostgl
make -s -C tests/gl OUT=../../$OUT >/dev/null
if [ "$1" = bench ]; then
    shift
    exec $OUT/bench "$@"
fi
math=$($OUT/mathtest "$@") || { echo "$math"; exit 1; }
echo "$math"
gl=$($OUT/scenetest "$@") || { echo "$gl"; exit 1; }
echo "$gl"
echo "$(echo "$math" | tail -1); $(echo "$gl" | tail -1)"
