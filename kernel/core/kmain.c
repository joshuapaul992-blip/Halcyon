/* Halcyon OS - kernel main */
#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "io.h"
#include "thread.h"
#include "proc.h"
#include "fs/block.h"
#include "drivers/pci.h"

/* Subsystems are linked in as they are written; weak references let the
 * kernel build while a subsystem is missing. */
#define WEAK __attribute__((weak))
void blockdev_init(void) WEAK;
void vfs_init(void) WEAK;
void ahci_init(void) WEAK;
void nvme_init(void) WEAK;
void usb_init(void) WEAK;
void usb_poll(void) WEAK;
void ps2_init(void) WEAK;
void gui_main(void) WEAK;
void kernel_selftest(void) WEAK;   /* optional test hook (debug builds) */
void splash_init(void) WEAK;
void acpi_init(void) WEAK;
void irq_init(void) WEAK;
void splash_progress(int pct, const char *msg) WEAK;
void splash_finish(void) WEAK;
void hwreport_early(void) WEAK;
void hwreport_late(void) WEAK;
int  bootlog_save(const char *reason) WEAK;
void settings_load(void) WEAK;
void keyboard_settings_load(void) WEAK;
#define PROGRESS(p, m) do { if (splash_progress) splash_progress(p, m); } while (0)



void kmain(struct bootinfo *bi) {
    memcpy(&g_bootinfo, bi, sizeof g_bootinfo);
    serial_init();
    kprintf("\n%s %s kernel starting (boot type %s)\n", OS_NAME, OS_VERSION,
            g_bootinfo.boot_type == BOOT_TYPE_UEFI ? "UEFI" : "BIOS");
    if (g_bootinfo.magic != BOOTINFO_MAGIC) panic("bad bootinfo magic");
    cpu_init();
    kprintf("cpu: %s (%s)\n", cpu_brand(), cpu_vendor());
    mm_init();
    u32 qf = qemu_boot_flags();
    if (qf) {
        g_bootinfo.boot_flags |= qf;
        kprintf("boot flags %#x added by the QEMU test hook\n", qf);
    }
    u32 bf = g_bootinfo.boot_flags;
    /* ACPI first: its PM timer is a fallback clock for TSC calibration */
    if (acpi_init && !(bf & BOOTF_NO_ACPI)) acpi_init();
    time_init();
    if (irq_init) irq_init();
    thread_init();
    proc_init();
    if (hwreport_early) hwreport_early();
    if (splash_init) splash_init();
    PROGRESS(10, "Scanning PCI devices...");
    pci_init();
    if (blockdev_init) blockdev_init();
    if (vfs_init) vfs_init();
    PROGRESS(20, "Starting keyboard and mouse...");
    if (ps2_init) ps2_init();
    PROGRESS(35, "Detecting SATA disks...");
    if (ahci_init && !(bf & BOOTF_NO_AHCI)) ahci_init();
    else kprintf("ahci: disabled by boot option\n");
    PROGRESS(45, "Detecting NVMe disks...");
    if (nvme_init && !(bf & BOOTF_NO_NVME)) nvme_init();
    else kprintf("nvme: disabled by boot option\n");
    PROGRESS(55, "Starting USB controllers...");
    if (usb_init) usb_init();
    storage_service();          /* register the USB disks found by usb_init */
    PROGRESS(100, "Starting the desktop...");
    if (hwreport_late) hwreport_late();
    if (settings_load) settings_load();
    if (keyboard_settings_load) keyboard_settings_load();
    if (bootlog_save) {
        int r = bootlog_save("desktop starting");
        kprintf("bootlog: %s\n", r == 0 ? "saved" : strerror(r));
    }
    if (splash_finish) splash_finish();
    if (kernel_selftest) kernel_selftest();
    if (gui_main) gui_main();

    kprintf("kmain: no GUI linked, polling devices\n");
    for (;;) {
        if (usb_poll) usb_poll();
        cpu_relax();
    }
}
