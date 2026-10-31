#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The I/O APICs (arch/ioapic.c): every one the MADT describes, as many as it describes, and the
   only owner of the global system interrupts (GSIs) their pins carry. The interrupt layer
   (arch/irq.c) routes a GSI to a CPU's vector; ACPI (dev/acpi.c) says what a PCI function's INTx
   signals. Register layout of the Intel 82093AA: IOREGSEL at +00h, IOWIN at +10h; register 1 holds
   the version and the highest redirection entry, 10h + 2n and 11h + 2n entry n.

   A pin's signal (trigger and polarity) comes from the MADT's interrupt source overrides (ACPI 6.5
   5.2.12.5: the MPS INTI flags, 00 meaning the bus's own: ISA edge and active high), the FADT's
   SCI (shareable, level, active low), and otherwise the ISA identity below GSI 16, PCI's level and
   active low above. An NMI source (5.2.12.6) is delivered as an NMI to CPU 0 and given to no
   device. A redirection entry names an 8-bit destination: an APIC ID above 255 only through
   interrupt remapping (dev/iommu.h, iommu_ioapic_entry), which every entry passes. Any CPU; each
   IOAPIC has its lock, taken before the IOMMU back end's (iommu_ioapic_entry is called under it). */
void ioapic_init(void); /* CPU 0 once (irq_init), after the MADT and the heap: every pin masked */
/* An ISA IRQ's GSI: the MADT's override for it, else the same number. */
uint32_t ioapic_isa_gsi(unsigned isa_irq);
/* What ACPI says the source on gsi signals (a PCI INTx through _PRT): false when the pin is routed
   with another signal already, belongs to no IOAPIC, or is an NMI source. */
bool ioapic_gsi_signal(uint32_t gsi, bool level, bool active_low);
/* Whether an IOAPIC has gsi for a device, and if so whether it is level-triggered. */
bool ioapic_gsi_owned(uint32_t gsi, bool *level);
/* Fixed delivery of gsi as vector to the CPU with that APIC ID, unmasked unless held: false when
   interrupt remapping cannot deliver it (the pin then masked). unroute: masked, routed nowhere. */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest);
void ioapic_unroute(uint32_t gsi);
/* Masked while held, whatever its route (a level line whose threads have not run, arch/irq.c);
   any context, interrupts off. */
void ioapic_hold(uint32_t gsi, bool held);
/* Interrupt remapping came on, went off, or is back from a sleep (dev/iommu.c): every pin written
   again from its route, remappable while remapping is on. False when a route could not be
   delivered (that pin masked, its route kept for the next try). */
bool ioapic_remap(void);
/* Every pin masked in the chips, the routes kept (ioapic_remap and ioapic_resume write them):
   before interrupt remapping changes format, and before a hibernation image takes over. */
void ioapic_mask_all(void);
/* Sleep (dev/pm.c): every pin masked before S3; after it (the chips reset), after the IOMMU
   (iommu_resume), each chip's ID and every pin written again. */
void ioapic_suspend(void);
void ioapic_resume(void);
