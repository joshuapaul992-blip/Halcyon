/* Halcyon OS - kernel main */
#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "io.h"
#include "drivers/pci.h"

/* Subsystems are linked in as they are written; weak references let the
 * kernel build while a subsystem is missing. */
#define WEAK __attribute__((weak))
void blockdev_init(void) WEAK;
void vfs_init(void) WEAK;
void ahci_init(void) WEAK;
void usb_init(void) WEAK;
void usb_poll(void) WEAK;
void ps2_init(void) WEAK;
void gui_main(void) WEAK;
void kernel_selftest(void) WEAK;   /* optional test hook (debug builds) */



void kmain(struct bootinfo *bi) {
    memcpy(&g_bootinfo, bi, sizeof g_bootinfo);
    serial_init();
    kprintf("\n%s %s kernel starting (boot type %s)\n", OS_NAME, OS_VERSION,
            g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "BIOS");
    if (g_bootinfo.magic != BOOTINFO_MAGIC) panic("bad bootinfo magic");
    cpu_init();
    kprintf("cpu: %s (%s)\n", cpu_brand(), cpu_vendor());
    mm_init();
    time_init();
    kprintf("fb: %ux%u bpp %u pitch %u at %lx\n", g_bootinfo.fb_width, g_bootinfo.fb_height,
            g_bootinfo.fb_bpp, g_bootinfo.fb_pitch, g_bootinfo.fb_addr);
    pci_init();
    if (blockdev_init) blockdev_init();
    if (vfs_init) vfs_init();
    if (ps2_init) ps2_init();
    if (ahci_init) ahci_init();
    if (usb_init) usb_init();
    if (kernel_selftest) kernel_selftest();
    if (gui_main) gui_main();

    kprintf("kmain: no GUI linked, polling devices\n");
    for (;;) {
        if (usb_poll) usb_poll();
        cpu_relax();
    }
}
