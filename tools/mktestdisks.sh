#!/bin/sh
# Create FAT test disk images for AHCI / USB mass storage integration tests:
#   $BUILD/fat12.img     1.44 MB superfloppy (no partition table), FAT12, label FLOPPY
#   $BUILD/fat16.img     32 MiB, MBR, one FAT16 partition at LBA 2048, label DATA16
#   $BUILD/fat32gpt.img  64 MiB, GPT, one FAT32 partition at LBA 2048, label GPTDISK
#   $BUILD/fat32_4k.img  320 MiB (sparse), 4096-byte sectors, GPT, FAT32 partition at
#                        LBA 256 (1 MiB), label NVME4K (for a 4Kn NVMe namespace)
# Every image holds the same sample tree:
#   README.TXT                       8.3 name, known text
#   Long File Name Example.txt       long name with spaces
#   Mixed Case.Data                  long name
#   Folder With Spaces/Nested Dir/deep file.txt
#   SUBDIR/HELLO.TXT
#   SUBDIR/pattern.bin               (32 KiB: bytes 0..255 repeated)
# Needs mkfs.fat, mtools, sfdisk, fdisk.
set -e
BUILD=${BUILD:-build}
mkdir -p "$BUILD"
export MTOOLS_SKIP_CHECK=1
TMP=$(mktemp -d "${TMPDIR:-/tmp}/halcyon-testdisks.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

# ---- sample files ----
printf 'Halcyon test disk.\r\nThis file has an 8.3 name.\r\n' > "$TMP/readme.txt"
printf 'This file has a long file name with spaces.\r\n' > "$TMP/long.txt"
printf 'Mixed case long name.\n' > "$TMP/mixed.dat"
printf 'Deep inside nested directories.\n' > "$TMP/deep.txt"
printf 'Hello from SUBDIR!\n' > "$TMP/hello.txt"
python3 -c 'import sys; sys.stdout.buffer.write(bytes(range(256)) * 128)' > "$TMP/pattern.bin" 2>/dev/null ||
    awk 'BEGIN { for (i = 0; i < 32768; i++) printf "%c", i % 256 }' > "$TMP/pattern.bin"

populate() {   # $1 = mtools image spec (file or file@@offset)
    mmd -i "$1" "::/Folder With Spaces" "::/Folder With Spaces/Nested Dir" ::/SUBDIR
    mcopy -i "$1" "$TMP/readme.txt" ::/README.TXT
    mcopy -i "$1" "$TMP/long.txt" "::/Long File Name Example.txt"
    mcopy -i "$1" "$TMP/mixed.dat" "::/Mixed Case.Data"
    mcopy -i "$1" "$TMP/deep.txt" "::/Folder With Spaces/Nested Dir/deep file.txt"
    mcopy -i "$1" "$TMP/hello.txt" ::/SUBDIR/HELLO.TXT
    mcopy -i "$1" "$TMP/pattern.bin" ::/SUBDIR/pattern.bin
}

# ---- FAT12 superfloppy ----
IMG=$BUILD/fat12.img
rm -f "$IMG"
mkfs.fat -C -F 12 -n FLOPPY "$IMG" 1440 >/dev/null 2>&1
populate "$IMG"
echo "Built $IMG (1.44 MB FAT12 superfloppy, label FLOPPY)"

# ---- FAT16, MBR ----
IMG=$BUILD/fat16.img
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=0 seek=32 2>/dev/null
SECT=$((32 * 2048 - 2048))
printf 'label: dos\nlabel-id: 0x48414c16\nstart=2048, size=%s, type=6\n' $SECT | sfdisk -q "$IMG"
mkfs.fat --offset=2048 -F 16 -n DATA16 "$IMG" $((SECT / 2)) >/dev/null 2>&1
populate "$IMG@@1M"
echo "Built $IMG (32 MiB MBR, FAT16 partition at LBA 2048, label DATA16)"

# ---- FAT32, GPT ----
IMG=$BUILD/fat32gpt.img
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=0 seek=64 2>/dev/null
SECT=$((64 * 2048 - 2048 - 2048))
printf 'label: gpt\nstart=2048, size=%s, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="GPTDISK"\n' $SECT \
    | sfdisk -q "$IMG"
mkfs.fat --offset=2048 -F 32 -s 1 -n GPTDISK "$IMG" $((SECT / 2)) >/dev/null 2>&1
populate "$IMG@@1M"
echo "Built $IMG (64 MiB GPT, FAT32 partition at LBA 2048, label GPTDISK)"

# ---- FAT32 on a 4096-byte-sector disk (4Kn NVMe namespace), GPT ----
# sfdisk cannot write a GPT for another sector size; fdisk -b can.
IMG=$BUILD/fat32_4k.img
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=0 seek=320 2>/dev/null
printf 'g\nn\n1\n256\n\nt\nEBD0A0A2-B9E5-4433-87C0-68B6B72699C7\nw\n' | fdisk -b 4096 "$IMG" >/dev/null 2>&1
SECT=$(fdisk -b 4096 -l "$IMG" | awk '$1 == "'"$IMG"'1" { print $4 }')
mkfs.fat --offset=256 -S 4096 -F 32 -s 1 -n NVME4K "$IMG" $((SECT * 4)) >/dev/null 2>&1
populate "$IMG@@1M"
echo "Built $IMG (320 MiB, 4096-byte sectors, GPT, FAT32 partition at LBA 256, label NVME4K)"
