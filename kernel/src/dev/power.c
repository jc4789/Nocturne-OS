/* ACPI-based power off and reboot (with emulator fallbacks): the last step of a power change
   (dev/pm.c), after the apps have ended and the devices have stopped. uACPI goes first; the FADT
   registers with the _S5_ bytes found in the DSDT remain for when the AML could not be loaded. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/acpi.h"
#include "mm/vmm.h"
#include "vm/vm.h"

static const struct acpi_fadt *fadt;
static uint16_t slp_typa, slp_typb;
static bool s5_ok;

/* Write a Generic Address Structure register (system memory or I/O space). */
static void gas_write(const struct acpi_gas *g, uint64_t v) {
    if (g->space == 0) {
        uint8_t *p = vmm_map_mmio(g->address & ~0xFFFULL, 4096);
        if (!p) return;
        p += g->address & 0xFFF;
        switch (g->bit_width) {
        case 16: mmio_write16(p, (uint16_t)v); break;
        case 32: mmio_write32(p, (uint32_t)v); break;
        case 64: mmio_write64(p, v); break;
        default: mmio_write8(p, (uint8_t)v); break;
        }
    } else if (g->space == 1) {
        switch (g->bit_width) {
        case 16: outw((uint16_t)g->address, (uint16_t)v); break;
        case 32: outl((uint16_t)g->address, (uint32_t)v); break;
        default: outb((uint16_t)g->address, (uint8_t)v); break;
        }
    }
}

static bool fadt_has(size_t end) { return fadt && fadt->h.length >= end; }

void acpi_init(void) {
    fadt = acpi_find("FACP", 0);
    if (!fadt) {
        kprintf("acpi: no FADT found\n");
        return;
    }
    uint64_t dsdt_phys = (fadt->h.length >= 148 && fadt->x_dsdt) ? fadt->x_dsdt : fadt->dsdt;
    const struct acpi_sdt *dsdt = acpi_table_at(dsdt_phys);
    if (!dsdt) kprintf("acpi: the DSDT lies outside the firmware's ranges\n"); /* no S5 from it; the AML interpreter still runs */
    const uint8_t *p = dsdt ? (const uint8_t *)(dsdt + 1) : NULL;
    const uint8_t *end = dsdt ? (const uint8_t *)dsdt + dsdt->length : NULL;
    for (; p && p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4)) continue;
        if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
        const uint8_t *q = p + 4;
        if (*q != 0x12) continue;
        q++;
        q += ((*q & 0xC0) >> 6) + 2; /* skip PkgLength and NumElements */
        if (*q == 0x0A) q++;
        slp_typa = *q++;
        if (*q == 0x0A) q++;
        slp_typb = *q;
        s5_ok = true;
        break;
    }
    kprintf("acpi: FADT rev %u flags %x%s, PM1a_CNT=%x, S5 %s (SLP_TYP %u/%u)\n", fadt->h.revision,
            fadt->flags, (fadt->flags & FADT_HW_REDUCED) ? " (hardware-reduced)" : "", fadt->pm1a_cnt,
            s5_ok ? "found" : "missing", slp_typa, slp_typb);
    if (fadt_has(268))
        kprintf("acpi: sleep control %u:%lx, reset %u:%lx=%x\n", fadt->sleep_control.space,
                fadt->sleep_control.address, fadt->reset_reg.space, fadt->reset_reg.address, fadt->reset_value);
    acpi_aml_init();
}

void power_off(void) {
    cli();
    kprintf("power: shutting down\n");
    acpi_sleep_enter(5); /* uACPI: SLP_TYP from \_S5 with the firmware's own layout; returns only on failure */
    if (fadt && s5_ok && fadt_has(268) && fadt->sleep_control.address) {
        /* hardware-reduced ACPI: SLP_TYPx in bits 2-4, SLP_EN is bit 5 */
        gas_write(&fadt->sleep_control, ((slp_typa & 7) << 2) | (1 << 5));
        for (int i = 0; i < 1000000; i++) io_wait();
    }
    if (fadt && s5_ok && fadt->pm1a_cnt) {
        if (fadt->smi_cmd && fadt->acpi_enable && !(inw(fadt->pm1a_cnt) & 1)) {
            outb(fadt->smi_cmd, fadt->acpi_enable);
            for (int i = 0; i < 100000 && !(inw(fadt->pm1a_cnt) & 1); i++) io_wait();
        }
        outw(fadt->pm1a_cnt, (slp_typa << 10) | (1 << 13));
        if (fadt->pm1b_cnt) outw(fadt->pm1b_cnt, (slp_typb << 10) | (1 << 13));
        for (int i = 0; i < 1000000; i++) io_wait();
    }
    vm_power_off(); /* on real hardware no port of an emulator is written */
    for (;;) hlt();
}

void power_reboot(void) {
    cli();
    kprintf("power: rebooting\n");
    if (acpi_reset()) for (int i = 0; i < 100000; i++) io_wait(); /* the reset register, any address space */
    if (fadt_has(129) && (fadt->flags & FADT_RESET_REG_SUP) && fadt->reset_reg.address) {
        gas_write(&fadt->reset_reg, fadt->reset_value);
        for (int i = 0; i < 100000; i++) io_wait();
    }
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) io_wait();
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000000; i++) io_wait();
    /* triple fault */
    struct {
        uint16_t l;
        uint64_t b;
    } PACKED z = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(z));
    for (;;) hlt();
}
