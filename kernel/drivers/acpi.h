#ifndef ACPI_H
#define ACPI_H
#include "core/types.h"

/* Interrupt controller description from the MADT ("APIC" table) */
#define MADT_MAX_IOAPIC 8
#define MADT_MAX_ISO    16

typedef struct madt_ioapic { u8 id; u32 addr; u32 gsi_base; } madt_ioapic_t;
/* ISA IRQ -> GSI override; flags: bits 0-1 polarity, bits 2-3 trigger (MPS INTI flags) */
typedef struct madt_iso { u8 bus, irq; u32 gsi; u16 flags; } madt_iso_t;

typedef struct acpi_madt {
    int present;
    u32 lapic_addr;
    u32 flags;                 /* bit 0: PCAT_COMPAT (dual 8259 present) */
    int nioapic, niso;
    madt_ioapic_t ioapic[MADT_MAX_IOAPIC];
    madt_iso_t iso[MADT_MAX_ISO];
    int nmi_lint;              /* LAPIC LINT pin wired to NMI (0/1), -1 unknown */
    u16 nmi_flags;
    int ncpus;
} acpi_madt_t;

const acpi_madt_t *acpi_madt(void);   /* NULL without ACPI or MADT */
int acpi_pm_timer(u16 *port, int *bits32);
int acpi_poweroff(void);
void acpi_init(void);

#endif
