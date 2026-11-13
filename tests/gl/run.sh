#!/bin/sh
# Host tests of Halcyon's float math (userland/lib/math.c): every function
# against the host's double-precision library, in ulps (ASan and UBSan).
#   tests/gl/run.sh [-v]     (needs gcc)
set -e
cd "$(dirname "$0")/../.."
OUT=build/hostgl
make -s -C tests/gl OUT=../../$OUT >/dev/null
exec $OUT/mathtest "$@"
