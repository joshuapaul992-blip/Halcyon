#!/bin/sh
# Generate the seed corpus for the storage stack fuzzer (fuzz_fs.c).
#
#   tests/host/fuzz_seeds.sh OUTDIR
#
# Every seed is one config byte followed by a disk image (see fuzz_fs.c):
#   bit 0  device sector size 4096 (else 512)
#   bit 1  read-only device
#   bit 6  boot disk signature 0x12345678 / boot partition LBA 8
#   bit 7  inject device I/O errors
#   bit 2+ also select variants of the mutating operations
# Images are small: FAT16/FAT32 volumes need many clusters, so they are made
# at their minimum size, populated (mtools allocates from the start of the
# data area) and then truncated: the device is smaller than the file system,
# which the driver has to cope with anyway.
#
# Needs: mkfs.fat, mtools, sfdisk, truncate, dd; xorriso for the ISO 9660 seeds.
set -e
OUT=${1:?usage: $0 OUTDIR}
mkdir -p "$OUT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MTOOLS_SKIP_CHECK=1
export MTOOLS_NO_VFAT=
unset MTOOLS_NO_VFAT

# populate IMG[@@OFFSET] with files, directories, long names, a deleted entry
populate() {
    i="$1"
    printf 'hello from a seed\r\n' > "$T/small.txt"
    seq -w 1 999999 | head -c $(( ${2:-5} * 1024 )) > "$T/multi.bin"
    : > "$T/empty"
    mmd -i "$i" ::/SUBDIR ::/"Long Directory Name" ::/"Long Directory Name/Nested"
    mcopy -i "$i" "$T/small.txt" ::/README.TXT
    mcopy -i "$i" "$T/small.txt" ::/"A long file name.text"
    mcopy -i "$i" "$T/multi.bin" ::/SUBDIR/MULTI.BIN
    mcopy -i "$i" "$T/empty" ::/SUBDIR/empty
    mcopy -i "$i" "$T/small.txt" ::/"Long Directory Name/Nested/deep file.txt"
    mcopy -i "$i" "$T/small.txt" ::/"Long Directory Name/Mixed.Case"
    mcopy -i "$i" "$T/small.txt" ::/DELETED.TXT
    mdel -i "$i" ::/DELETED.TXT
}

# seed NAME CONFIGBYTE IMAGE [TRUNCATE_BYTES]
seed() {
    [ -n "$4" ] && truncate -s "$4" "$3"
    printf "\\$(printf '%03o' "$2")" > "$OUT/$1"
    cat "$3" >> "$OUT/$1"
}

MKFS="mkfs.fat --invariant"

# --- superfloppies -------------------------------------------------------
$MKFS -C -F 12 -n FLOPPY12 "$T/f12.img" 160 >/dev/null
populate "$T/f12.img"
seed super_fat12 0 "$T/f12.img"
seed super_fat12_ro 2 "$T/f12.img"
seed super_fat12_faults $((0x80 | 5 << 2)) "$T/f12.img"
# boot code that happens to contain a plausible partition entry
printf '\200\0\0\0\6\0\0\0\1\0\0\0\144\0\0\0' | dd of="$T/f12.img" bs=1 seek=446 conv=notrunc 2>/dev/null
seed super_fat12_fakembr 0 "$T/f12.img"

# FAT12 with 2 sectors/cluster and a tiny root directory
$MKFS -C -F 12 -s 2 -r 16 -n TINYROOT "$T/f12b.img" 64 >/dev/null
populate "$T/f12b.img" 3
seed super_fat12_tinyroot 4 "$T/f12b.img"

$MKFS -C -F 16 -s 1 -n FAT16 "$T/f16.img" 2200 >/dev/null
populate "$T/f16.img"
seed super_fat16 8 "$T/f16.img" 160K

$MKFS -C -F 32 -s 1 -f 1 -n FAT32 "$T/f32.img" 34000 >/dev/null
populate "$T/f32.img"
seed super_fat32 12 "$T/f32.img" 320K

# 4096-byte sectors on a 4096-byte device, and the same FS on a 512 device
$MKFS -C -F 12 -S 4096 -n SECT4K "$T/f12_4k.img" 400 >/dev/null
populate "$T/f12_4k.img" 20
seed super_fat12_4k 1 "$T/f12_4k.img"
seed super_fat12_4k_on512 16 "$T/f12_4k.img"

$MKFS -C -F 32 -S 4096 -s 1 -f 1 -n SECT4K32 "$T/f32_4k.img" 270000 >/dev/null
populate "$T/f32_4k.img"
seed super_fat32_4k 21 "$T/f32_4k.img" 600K

# --- MBR with an extended partition (two logical partitions) ------------
truncate -s 600K "$T/mbr.img"
printf 'label: dos\nlabel-id: 0x12345678\nstart=8, size=300, type=1\nstart=320, size=880, type=5\nstart=328, size=300, type=1\nstart=640, size=500, type=6\n' \
    | sfdisk -q --no-reread --no-tell-kernel "$T/mbr.img" >/dev/null 2>&1
$MKFS --offset=8 -F 12 -n PRIMARY "$T/mbr.img" 150 >/dev/null
$MKFS --offset=328 -F 12 -n LOGICAL5 "$T/mbr.img" 150 >/dev/null
$MKFS --offset=640 -F 12 -s 1 -n LOGICAL6 "$T/mbr.img" 250 >/dev/null
populate "$T/mbr.img@@4096" 2
populate "$T/mbr.img@@167936" 2
populate "$T/mbr.img@@327680" 2
seed mbr_extended 24 "$T/mbr.img"
seed mbr_extended_boot $((0x40 | 6 << 2)) "$T/mbr.img"

# 26 logical partitions, each holding the start of a tiny FAT12 volume:
# more volumes than drive letters
$MKFS -C -F 12 -r 16 -f 1 -n TINY "$T/tiny.img" 64 >/dev/null
truncate -s 160K "$T/many.img"
{
    printf 'label: dos\nlabel-id: 0x0000abcd\nstart=4, size=300, type=5\n'
    k=0
    while [ $k -lt 26 ]; do printf 'start=%d, size=8, type=1\n' $((6 + k * 10)); k=$((k + 1)); done
} | sfdisk -q --no-reread --no-tell-kernel "$T/many.img" >/dev/null 2>&1
k=0
while [ $k -lt 26 ]; do
    dd if="$T/tiny.img" of="$T/many.img" bs=512 count=8 seek=$((6 + k * 10)) conv=notrunc 2>/dev/null
    k=$((k + 1))
done
seed mbr_many 0 "$T/many.img"

# MBR with one FAT32 partition (truncated)
truncate -s 34M "$T/mbr32.img"
printf 'label: dos\nstart=64, type=c\n' | sfdisk -q --no-reread --no-tell-kernel "$T/mbr32.img" >/dev/null 2>&1
$MKFS --offset=64 -F 32 -s 1 -f 2 -n MBRFAT32 "$T/mbr32.img" $(( (34 * 2048 - 64) / 2 )) >/dev/null
populate "$T/mbr32.img@@32768"
seed mbr_fat32 28 "$T/mbr32.img" 600K

# --- GPT -----------------------------------------------------------------
truncate -s 600K "$T/gpt.img"
printf 'label: gpt\nfirst-lba: 34\nstart=40, size=400, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=one\nstart=480, size=600, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=two\n' \
    | sfdisk -q --no-reread --no-tell-kernel "$T/gpt.img" >/dev/null 2>&1
$MKFS --offset=40 -F 12 -n GPTONE "$T/gpt.img" 200 >/dev/null
$MKFS --offset=480 -F 12 -n GPTTWO "$T/gpt.img" 300 >/dev/null
populate "$T/gpt.img@@20480" 2
populate "$T/gpt.img@@245760" 2
seed gpt_two 32 "$T/gpt.img"

# GPT on a 4096-byte-sector disk (sfdisk cannot do that on an image file)
if command -v python3 >/dev/null; then
    truncate -s 1200K "$T/gpt4k.img"
    python3 - "$T/gpt4k.img" <<'PY'
import struct, sys, zlib, uuid
ss, path = 4096, sys.argv[1]
f = open(path, "r+b")
nsec = 1200 * 1024 // ss
mbr = bytearray(512)
mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, nsec - 1)
mbr[510:512] = b"\x55\xaa"
f.write(mbr)
ent = bytearray(128 * 128)
ent[0:128] = (uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le + uuid.UUID(int=1).bytes_le +
              struct.pack("<QQQ", 8, 8 + 200 - 1, 0) + "data".encode("utf-16-le").ljust(72, b"\0"))
def hdr(me, alt, entlba):
    h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, me, alt, 6, nsec - 6,
                              uuid.UUID(int=2).bytes_le, entlba, 128, 128, zlib.crc32(ent)))
    h[16:20] = struct.pack("<I", zlib.crc32(h))
    return h
for lba, data in ((1, hdr(1, nsec - 1, 2)), (2, ent), (nsec - 5, ent), (nsec - 1, hdr(nsec - 1, 1, nsec - 5))):
    f.seek(lba * ss); f.write(data)
PY
    $MKFS --offset=8 -S 4096 -F 12 -n GPT4K "$T/gpt4k.img" 800 >/dev/null
    populate "$T/gpt4k.img@@32768" 10
    seed gpt_4k 37 "$T/gpt4k.img"
fi

# --- ISO 9660 (whole-device images: descriptors at byte 32768) -------------
if command -v xorriso >/dev/null; then
    mkdir -p "$T/iso/SUB/INNER" "$T/iso/Long Directory Name"
    printf 'hello from a seed\r\n' > "$T/iso/README.TXT"
    printf 'inner\n' > "$T/iso/SUB/INNER/FILE.TXT"
    seq -w 1 999999 | head -c 7000 > "$T/iso/SUB/MULTI.BIN"
    printf 'long\n' > "$T/iso/Long Directory Name/A long file name.text"
    k=0
    while [ $k -lt 40 ]; do printf '%d\n' $k > "$T/iso/SUB/F$k.TXT"; k=$((k + 1)); done
    xorriso -as mkisofs -quiet -J -V JOLIETSEED -o "$T/j.iso" "$T/iso" >/dev/null 2>&1
    seed iso_joliet_rr 0 "$T/j.iso"
    xorriso -as mkisofs -quiet -V RRSEED -o "$T/rr.iso" "$T/iso" >/dev/null 2>&1
    seed iso_rr_4k 1 "$T/rr.iso"
    xorriso -as mkisofs -quiet --norock -V PLAINSEED -o "$T/p.iso" "$T/iso" >/dev/null 2>&1
    seed iso_plain $((5 << 2)) "$T/p.iso"
fi

ls -l "$OUT"
