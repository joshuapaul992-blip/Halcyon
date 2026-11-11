/* Storage stack fuzz harness (see fuzz_fs.c). */
#ifndef FUZZ_FS_H
#define FUZZ_FS_H
#include <stddef.h>
#include <stdint.h>

/* config byte (first byte of every fuzz input) */
#define FZ_CFG_4K       0x01    /* device sector size 4096 (else 512) */
#define FZ_CFG_RO       0x02    /* read-only device */
#define FZ_CFG_BOOT     0x40    /* boot disk signature 0x12345678, boot partition at LBA 8 */
#define FZ_CFG_FAULTS   0x80    /* inject device I/O errors */
#define FZ_CFG_VARIANT(c) ((c) >> 2)  /* bits 2..7 also select operation variants */

/* Run one input: data[0] = config byte, data[1..] = disk image. Aborts
 * (with a message on stderr) on any bug: sanitizer report, kernel heap leak,
 * device left registered, write to a read-only device, out-of-range device
 * I/O, unbounded data-driven work (time budget), ... Returns 0. */
int fuzz_fs_run(const uint8_t *data, size_t size);

/* Same, with a separate config byte and image (used by the regression tests).
 * Returns the number of volumes that got mounted. */
int fuzz_fs_image(uint8_t cfg, const uint8_t *img, size_t size);

/* seconds of CPU a single input may take before it counts as a hang
 * (default 5, env FUZZ_HANG_SECS) */
extern double fuzz_hang_secs;
extern int fuzz_verbose;

#endif
