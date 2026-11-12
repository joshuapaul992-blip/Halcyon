#!/bin/sh
# Host tests of the WAV library (userland/lib/wav.c): every format against
# known values and independent encoders, rejections, and a fuzzer (ASan and
# UBSan); the system sounds made by tools/mksounds.py must parse as 16-bit
# PCM and its demo piece as IMA ADPCM, neither silent, clipped nor noisy.
#   tests/sound/run.sh [-n fuzz-iterations]     (needs gcc, python3)
set -e
cd "$(dirname "$0")/../.."
OUT=build/hostsound
make -s -C tests/sound OUT=../../$OUT >/dev/null
mkdir -p $OUT/media
python3 tools/mksounds.py $OUT/media
exec $OUT/wavtest "$@" $OUT/media/*.WAV $OUT/media/music/*.wav
