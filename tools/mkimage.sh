#!/bin/sh
# Build the Halcyon bootable disk image (BIOS + UEFI, MBR partitioned, FAT32).
# Write it to a USB stick with:  dd if=build/halcyon.img of=/dev/sdX bs=4M conv=fsync
set -e
BUILD=${BUILD:-build}
IMG=$BUILD/halcyon.img
PART=$BUILD/part.img
SIZE_MB=${SIZE_MB:-128}
PART_START=2048
TOTAL=$((SIZE_MB * 2048))
PART_SECTORS=$((TOTAL - PART_START))

rm -f "$IMG.tmp" "$PART"
dd if=/dev/zero of="$IMG.tmp" bs=1M count=0 seek=$SIZE_MB 2>/dev/null
printf 'label: dos\nlabel-id: 0x48414c43\nstart=%s, size=%s, type=c, bootable\n' $PART_START $PART_SECTORS \
    | sfdisk -q "$IMG.tmp"

# MBR boot code (first 440 bytes only: keep partition table) + stage2 in the gap
dd if=$BUILD/boot/mbr.bin of="$IMG.tmp" bs=440 count=1 conv=notrunc 2>/dev/null
dd if=$BUILD/boot/stage2.bin of="$IMG.tmp" bs=512 seek=1 conv=notrunc 2>/dev/null

# FAT32 partition
dd if=/dev/zero of="$PART" bs=512 count=0 seek=$PART_SECTORS 2>/dev/null
mkfs.fat -F 32 -s 1 -h $PART_START -n HALCYON "$PART" >/dev/null
export MTOOLS_SKIP_CHECK=1
mmd -i "$PART" ::/HALCYON ::/EFI ::/EFI/BOOT
mcopy -i "$PART" $BUILD/KERNEL.BIN ::/HALCYON/KERNEL.BIN
if [ -f $BUILD/efi/BOOTX64.EFI ]; then
    mcopy -i "$PART" $BUILD/efi/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
fi
# user files
if [ -d rootfs ]; then
    for f in rootfs/*; do
        [ -e "$f" ] && mcopy -s -i "$PART" "$f" ::/
    done
fi
dd if="$PART" of="$IMG.tmp" bs=512 seek=$PART_START conv=notrunc 2>/dev/null
rm -f "$PART"
mv "$IMG.tmp" "$IMG"
echo "Built $IMG (${SIZE_MB} MiB)"
