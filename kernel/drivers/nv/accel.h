/* Halcyon OS - NVIDIA GPUs (M10e): the copy engine behind the display
 * core's accelerated operations (display.h, "acceleration"). The hardware
 * logic is fifo.c (host-tested); this is the kernel side: the deferred,
 * guarded bring-up, the VRAM and system RAM it uses, the operations, the
 * self test, the speed test's numbers and the report.
 *
 * The bring-up waits until the display driver's start guard is done with
 * the adapter (the desktop runs on it, its picture confirmed), saves its
 * own marker (display.nv.accel.guard) and only then writes a register.
 * nv.accel=0 keeps it off; nv.disable=1, nv.modeset=0, F8 Safe Mode and
 * display.disable=nv keep it off with the driver. */
#ifndef NV_ACCEL_H
#define NV_ACCEL_H
#include "drivers/nv/nv.h"
#include "drivers/nv/evo.h"

struct display_adapter;
typedef struct nv_accel nv_accel_t;

/* what disp.c lends the copy engine once the display is taken over */
typedef struct nv_accel_host {
    u8  *bar1;                  /* BAR1, VRAM 1:1 (checked by 10b) */
    u64 window;                 /* the part of VRAM BAR1 shows */
    nv_evo_vram_t *vram;        /* 10b's VRAM allocator (2 MiB pages) */
    void *ctx;
    void (*log)(void *ctx, const char *line);                 /* nvN: lines (BOOT.LOG, NVREPORT) */
    int  (*vram_linear)(void *ctx, u64 off);                  /* BAR1 maps this offset 1:1 (a PRAMIN check) */
} nv_accel_host_t;

/* registration (probe_one): reads nothing; logs (through log) what will happen */
nv_accel_t *nv_accel_probe(nv_device_t *d, struct display_adapter *a, void (*log)(void *ctx, const char *line), void *ctx);
/* the display is ours (enable): the BAR1 window and VRAM for later */
void nv_accel_attach(nv_accel_t *x, const nv_accel_host_t *h);
/* main-loop work (nvd_poll): the bring-up once the start guard is done */
void nv_accel_poll(nv_accel_t *x);
/* the device is given back (restore): the channel stops, registers back */
void nv_accel_release(nv_accel_t *x, const char *why);
/* one line ("copy engine 2, in use" / why it is off) and the report section */
const char *nv_accel_state(nv_accel_t *x, char *buf, int n);
int  nv_accel_report(nv_accel_t *x, char *buf, int size);

#endif
