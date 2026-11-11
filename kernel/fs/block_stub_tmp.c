/* TEMPORARY stub until the real block layer is merged: exercises every
 * registered disk (read LBA0, VBR, throughput, write/readback at the end). */
#include "fs/block.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
int blockdev_register(blockdev_t *bd) {
    kprintf("blockstub: register %s (%s) %lu sectors of %u\n", bd->name, bd->model, bd->sector_count, bd->sector_size);
    u8 *buf = kmalloc(1024 * 1024);
    int r = bd->read(bd, 0, 1, buf);
    kprintf("blockstub: LBA0 r=%d sig=%02x%02x disksig=%02x%02x%02x%02x\n", r, buf[510], buf[511], buf[443], buf[442], buf[441], buf[440]);
    if (bd->sector_count > 4096) {
        r = bd->read(bd, 2048, 1, buf);
        kprintf("blockstub: LBA2048 r=%d sig=%02x%02x oem=%.8s\n", r, buf[510], buf[511], buf + 3);
        u64 t0 = time_ms();
        int ok = 0;
        for (int i = 0; i < 8; i++) if (bd->read(bd, 4096 + (u64)i * 2048, 2048, buf) == 0) ok++;
        u64 dt = time_ms() - t0;
        kprintf("blockstub: read 8 MiB in %lu ms (%lu KiB/s), %d/8 ok\n", dt, dt ? 8192 * 1000 / dt : 0, ok);
        /* write/readback test on the last 64 sectors (unused by the image) */
        u64 lba = bd->sector_count - 64;
        for (int i = 0; i < 64 * 512; i++) buf[i] = (u8)(i * 7 + 13);
        r = bd->write(bd, lba, 64, buf);
        memset(buf, 0, 64 * 512);
        int r2 = bd->read(bd, lba, 64, buf);
        int bad = 0;
        for (int i = 0; i < 64 * 512; i++) if (buf[i] != (u8)(i * 7 + 13)) bad++;
        kprintf("blockstub: write r=%d readback r=%d mismatches=%d -> %s\n", r, r2, bad, (!r && !r2 && !bad) ? "PASS" : "FAIL");
    }
    kfree(buf);
    return 0;
}
void blockdev_unregister(blockdev_t *bd) { kprintf("blockstub: unregister %s\n", bd->name); }
