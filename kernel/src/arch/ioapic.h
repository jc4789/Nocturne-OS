#pragma once
#include <stdint.h>
#include <stdbool.h>

/* I/O APIC driver. Non-ExtINT pins are left MASKED at init. The legacy
   8259 path (LAPIC LINT0 in virtual-wire mode, see apic.c) is independent of the IOAPIC and unaffected.
   Entries the firmware left as unmasked ExtINT (virtual-wire mode B: 8259 through IOAPIC pin 0) are kept.
   Destinations are 8-bit physical APIC IDs (the plain RTE format and the remapping entries
   dev/iommu.c writes carry no more), and the vector must be a LAPIC vector >= 0x30 (0x20-0x2F
   belong to the PIC). Entries go through iommu_ioapic_entry, which makes them remappable where
   interrupt remapping is on. Call from the BSP only; register access is serialised by disabling
   interrupts. */
void ioapic_init(void); /* after acpi_init(); masks non-ExtINT pins of each IOAPIC found in the MADT */
unsigned ioapic_count(void);
/* Redirect GSI `gsi` to `vector` on the CPU with physical APIC id `dest_apic_id` (fixed delivery),
   unmasked. level/active_low describe the source. False if no IOAPIC owns the GSI, an argument is
   bad, or interrupt remapping cannot deliver it (the pin then stays masked). The IOAPIC ids passed
   to dev/iommu.h are the MADT's, which the DMAR's device scopes name. */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, bool level, bool active_low);
bool ioapic_mask(uint32_t gsi);
/* Interrupt remapping came on (dev/iommu_intel.c) or went off again (dev/iommu.c): the unmasked
   entries (the firmware's ExtINT pin) are written again from the plain entries they were given,
   remappable while it is on; one whose remapping entry the units did not confirm stays masked.
   False if any requested route failed; retain its plain request for a safe rollback/retry. */
bool ioapic_remap(void);
/* ISA IRQ -> GSI honoring MADT interrupt source overrides. level/active_low (may be NULL) receive the
   trigger mode and polarity to use for that source (ISA default: edge, active high). */
uint32_t ioapic_isa_to_gsi(unsigned isa_irq, bool *level, bool *active_low);
/* Sleep (dev/pm.c), interrupts off: save every entry and mask it before S3; restore after IOMMU. */
void ioapic_suspend(void);
void ioapic_resume(void);
void ioapic_mask_all(void); /* a hibernation restore: nothing more comes in before the image takes over */
