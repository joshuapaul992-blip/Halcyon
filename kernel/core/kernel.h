#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#include "../../boot/bootinfo.h"

#define OS_NAME    "Halcyon"
#define OS_VERSION "1.0"

extern struct bootinfo g_bootinfo;

void cpu_init(void);
const char *cpu_brand(void);
const char *cpu_vendor(void);

/* timing (TSC based, calibrated against the PIT at boot) */
void time_init(void);
u64  time_ms(void);              /* milliseconds since boot */
u64  time_us(void);
void udelay(u64 us);
void mdelay(u64 ms);
u64  tsc_khz(void);              /* TSC ticks per ms */
u64  time_ms_to_tsc(u64 ms);    /* absolute time_ms() -> TSC value */

struct rtc_time { int year, month, day, hour, minute, second, weekday; };
void rtc_read(struct rtc_time *t);

/* registered as the system idle hook; called from busy-wait loops so that
 * input keeps flowing (e.g. during long disk transfers). May be NULL. */
extern void (*g_idle_hook)(void);

u32  qemu_boot_flags(void);       /* test hook: extra BOOTF_* from QEMU fw_cfg */

void reboot(void);
void shutdown(void);   /* ACPI-less: tries QEMU/Bochs/VirtualBox ports, then halts */

#endif
