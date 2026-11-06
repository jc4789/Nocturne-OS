#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The I/O APICs (arch/ioapic.c): every one the MADT describes, as many as it describes, and the
   only owner of the global system interrupts (GSIs) their pins carry. The interrupt layer
   (arch/irq.c) routes a GSI to a CPU's vector; ACPI (dev/acpi.c) says what a PCI function's INTx
   signals. Register layout of the Intel 82093AA: IOREGSEL at +00h, IOWIN at +10h; register 1 holds
   the version and the highest redirection entry, 10h + 2n and 11h + 2n entry n.

   A pin's signal (trigger, polarity, sharing) comes from the MADT's interrupt source overrides
   (ACPI 6.5 5.2.12.5: the MPS INTI flags, 00 meaning the bus's own: ISA edge and active high; a
   level one shared), the FADT's SCI (shareable, level, active low), and otherwise the ISA identity
   below GSI 16 (edge, active high, exclusive), PCI's level, active low and shared above. An NMI source (5.2.12.6) is delivered as an NMI to CPU 0 and given to no
   device. A redirection entry names an 8-bit destination: an APIC ID above 255 only through
   interrupt remapping (dev/iommu.h, iommu_ioapic_entry), which every entry passes. unroute returns
   once the chip has the masked entry. Any CPU; each
   IOAPIC has its lock, taken before the IOMMU back end's (iommu_ioapic_entry is called under it). */
void ioapic_init(void); /* CPU 0 once (irq_init), after the MADT and the heap: every pin masked */
/* An ISA IRQ's GSI: the MADT's override for it, else the same number. */
uint32_t ioapic_isa_gsi(unsigned isa_irq);
/* An interrupt descriptor's signal (ACPI 6.5 6.4.2.1 and 6.4.3.6: _HE, _LL, _SHR). The interrupt
   layer shares a line only when it is level and shared: two sources' edges would merge. */
struct gsi_signal {
    bool level, active_low, shared;
};
/* What ACPI says the source on gsi signals (a PCI INTx through _PRT, a namespace device's _CRS),
   told by the interrupt layer as it routes it (irq_request_gsi): false when the pin is routed
   already with another signal or exclusively (either side), belongs to no IOAPIC, or is an NMI
   source. A pin the MADT describes (an override, the SCI) keeps the MADT's trigger and polarity
   (ACPI 6.5 5.2.12.5). takes: whether signal would be taken now, changing nothing. */
bool ioapic_gsi_signal(uint32_t gsi, struct gsi_signal signal);
bool ioapic_gsi_takes(uint32_t gsi, struct gsi_signal signal);
/* Whether an IOAPIC has gsi for a device, and if so its signal. */
bool ioapic_gsi_owned(uint32_t gsi, struct gsi_signal *signal);
/* Fixed delivery of gsi as vector to the CPU with that APIC ID, unmasked unless held: false when
   interrupt remapping cannot deliver it (the pin then masked). unroute: masked, routed nowhere. */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest);
void ioapic_unroute(uint32_t gsi);
/* The highest APIC ID gsi's entry can name (iommu_ioapic_reach of its IOAPIC); 0 for no IOAPIC's. */
uint32_t ioapic_gsi_reach(uint32_t gsi);
/* The end of a level interrupt of gsi on vector that the CPU took as edge-triggered (apic_tmr), so
   that its APIC's EOI never reached the IOAPICs: interrupt remapping may deliver it without its
   trigger mode. Remote IRR cleared in the pin, through the IOAPIC's EOI register where it has one
   (version 20h and up), else by turning the masked entry edge-triggered and back, which clears it.
   On the CPU that took it, after its EOI, interrupts off. */
void ioapic_eoi(uint32_t gsi, uint8_t vector);
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
/* Sleep (dev/pm.c): every pin masked before it; after it, after the IOMMU (iommu_resume), each
   chip's ID back and every pin still masked, Remote IRR cleared where the CPU that took its
   interrupt was reset; the entries are written again by ioapic_remap. */
void ioapic_suspend(void);
void ioapic_resume(void);
