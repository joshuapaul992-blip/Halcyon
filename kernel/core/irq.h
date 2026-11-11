#ifndef IRQ_H
#define IRQ_H
/* Halcyon OS - interrupts, timer and CPU idle.
 *
 * Model: device interrupts are wake-up signals. An interrupt handler only
 * counts the event (and masks a level-triggered line); the driver work runs in
 * the main loop as before. Device state is always read from the device, so a
 * missed or never-routed interrupt costs latency, never correctness: a device
 * whose interrupts are not (yet) known to work is simply polled.
 *
 * Interrupt controllers: local APIC (xAPIC or x2APIC) + I/O APICs from the
 * ACPI MADT, or the 8259 PICs when there is no MADT / I/O APIC. PCI devices use
 * MSI-X or MSI when they have it; otherwise INTx. Without an AML interpreter the
 * PCI INTx -> I/O APIC routing (_PRT) is unknown, so it is learned: unclaimed
 * I/O APIC inputs are unmasked, and when one fires the PCI devices waiting for
 * a route are checked for their Interrupt Status bit (PCI 2.3). In PIC mode the
 * firmware's Interrupt Line register is used. */
#include "types.h"

struct pci_dev;

enum { IRQ_MODE_POLL = 0, IRQ_MODE_PIC, IRQ_MODE_APIC };
enum { IRQ_NONE = 0, IRQ_MSI, IRQ_MSIX, IRQ_INTX, IRQ_ISA };

typedef struct irq_handle irq_handle_t;
struct irq_handle {
    /* filled in by the driver */
    const char *name;
    struct pci_dev *pci;
    /* INTx only: acknowledge (clear) the device's interrupt status. Called
     * from the main loop, never from an interrupt handler, possibly while the
     * driver is in the middle of something else: touch status registers
     * only. Return 1 if the device was asserting its interrupt. */
    int (*ack)(void *ctx);
    void *ctx;
    u8 intx_only;          /* driver: never MSI (status is only acked for INTx) */
    /* managed by irq.c */
    u8 kind;               /* IRQ_NONE: polled */
    u8 broken;             /* interrupts stopped arriving: polled again */
    u8 shown;
    int vector;
    int line;              /* INTx: GSI (APIC) / IRQ (PIC); -1 while unknown. ISA: IRQ */
    volatile u32 events;   /* interrupts seen from this device */
    u32 seen;
    u64 wd_start;
    u32 wd_work, wd_events, noack;
    irq_handle_t *next_on_line, *next_all;
};

void irq_init(void);
int  irq_mode(void);
const char *irq_mode_name(void);
const char *irq_timer_name(void);
int  irq_msi_allowed(void);

int  irq_attach(irq_handle_t *h);                 /* PCI device; returns kind */
int  irq_attach_isa(irq_handle_t *h, int isa_irq);
void irq_detach(irq_handle_t *h);
int  irq_live(const irq_handle_t *h);             /* its interrupts are known to work */
int  irq_fired(irq_handle_t *h);                  /* interrupts since the last call */
void irq_note_work(irq_handle_t *h);              /* driver found work by polling (lost-interrupt watchdog) */
const char *irq_kind_name(const irq_handle_t *h);
void irq_describe(const irq_handle_t *h, char *buf, int n);

u32  irq_generation(void);                        /* device interrupts so far */
void irq_service(void);                           /* main loop: ack + unmask level lines, learn routes */

/* CPU idle. idle_wait() halts until an interrupt or the deadline (absolute
 * time_ms), unless a device interrupt arrived since `gen` was read. Without
 * a working timer interrupt (or with interrupts disabled by the caller) it
 * returns at once, and callers keep polling. */
int  idle_enabled(void);
void idle_wait(u32 gen, u64 deadline_ms);
void idle_wait_tsc(u32 gen, u64 deadline_tsc);
/* polled code asks for the next idle period to end by `ms` (absolute) */
void idle_wake_at(u64 ms);
u64  idle_take_deadline(void);                    /* and reset; ~0 if none */
/* wait helper for driver loops: halt (bounded) if h's interrupts work, else spin */
void irq_wait(irq_handle_t *h, u32 gen, u64 deadline_ms);

/* statistics */
u64  idle_tsc_total(void);
u64  idle_wakeups(void);
int  cpu_load_percent(void);       /* busy % since the previous call (>= 250 ms windows) */
u64  irq_total(void);
int  irq_list(char *buf, int n);   /* table for the IRQ command / boot log */

static inline u64 irq_save(void) {
    u64 f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(u64 f) { if (f & 0x200) __asm__ volatile("sti" ::: "memory"); }
static inline int irq_enabled_now(void) {
    u64 f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return (f & 0x200) != 0;
}

#endif
