#pragma once
#include <stdint.h>
#include <stdbool.h>

/* I/O APIC driver. Every pin is left MASKED at init; nothing is routed until a caller asks. The legacy
   8259 path (LAPIC LINT0 in virtual-wire mode, see apic.c) is independent of the IOAPIC and unaffected.
   Entries the firmware left as unmasked ExtINT (virtual-wire mode B: 8259 through IOAPIC pin 0) are kept.
   Destinations are 8-bit physical APIC IDs (the IOAPIC RTE has no wider field without interrupt
   remapping), and the vector must be a LAPIC vector >= 0x30 (0x20-0x2F belong to the PIC).
   Call from the BSP only; register access is serialised by disabling interrupts. */
void ioapic_init(void); /* after acpi_init(); maps and masks every IOAPIC found in the MADT */
unsigned ioapic_count(void);
/* Redirect GSI `gsi` to `vector` on the CPU with physical APIC id `dest_apic_id` (fixed delivery),
   unmasked. level/active_low describe the source. False if no IOAPIC owns the GSI or an argument is bad. */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, bool level, bool active_low);
bool ioapic_mask(uint32_t gsi);
/* ISA IRQ -> GSI honoring MADT interrupt source overrides. level/active_low (may be NULL) receive the
   trigger mode and polarity to use for that source (ISA default: edge, active high). */
uint32_t ioapic_isa_to_gsi(unsigned isa_irq, bool *level, bool *active_low);
/* Sleep (dev/pm.c), interrupts off: save every entry before S3, write them back after it. */
void ioapic_suspend(void);
void ioapic_resume(void);
void ioapic_mask_all(void); /* a hibernation restore: nothing more comes in before the image takes over */
