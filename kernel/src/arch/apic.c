/* Local APIC: we keep using the 8259 PIC, routed through LINT0 (virtual wire mode). */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/vmm.h"

static volatile uint32_t *lapic;

static void lapic_write(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }
static uint32_t lapic_read(uint32_t reg) { return lapic[reg / 4]; }

void apic_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1 << 9))) {
        kprintf("apic: no local APIC, using plain PIC\n");
        return;
    }
    uint64_t base = rdmsr(0x1B);
    if (!(base & (1 << 11))) {
        kprintf("apic: local APIC globally disabled, using plain PIC\n");
        return;
    }
    uint64_t phys = base & 0xFFFFFF000ULL;
    lapic = vmm_map_mmio(phys, 4096);
    lapic_write(0x80, 0);                                 /* TPR: accept everything */
    lapic_write(0xF0, (lapic_read(0xF0) & ~0xFFu) | 0x1FF); /* enable, spurious vector 0xFF */
    lapic_write(0x320, 0x10000);                          /* timer masked */
    lapic_write(0x350, 0x700);                            /* LINT0: ExtINT (the PIC) */
    lapic_write(0x360, 0x400);                            /* LINT1: NMI */
    lapic_write(0x370, 0x10000 | 0xFE);                   /* error: masked */
    kprintf("apic: LAPIC at %p id %u, PIC in virtual wire mode\n", (void *)phys, lapic_read(0x20) >> 24);
}
