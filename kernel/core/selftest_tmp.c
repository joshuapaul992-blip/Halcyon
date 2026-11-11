/* TEMPORARY: print input events */
#include "core/kernel.h"
#include "core/lib.h"
#include "drivers/input.h"
void kernel_selftest(void) {
    u64 end = time_ms() + 12000;
    while (time_ms() < end) {
        input_poll_devices();
        input_event_t e;
        while (input_get(&e)) {
            if (e.type == EV_KEY) kprintf("EV key %02x %s ch=%c mods=%x\n", e.key, e.pressed ? "down" : "up", e.ch >= 32 ? e.ch : '.', e.mods);
            else kprintf("EV mouse %s d=%d,%d,%d abs=%d,%d btn=%x\n", e.absolute ? "abs" : "rel", e.dx, e.dy, e.dz, e.x, e.y, e.buttons);
        }
    }
    kprintf("selftest done\n");
}
