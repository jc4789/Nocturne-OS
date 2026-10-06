/* ACPI-based power off and reboot (with emulator fallbacks). */
#include "kernel.h"
#include "arch/cpu.h"
#include "limine.h"
#include "mm/vmm.h"

extern volatile struct limine_rsdp_request rsdp_req;

struct sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} PACKED;

struct gas {
    uint8_t space, bit_width, bit_offset, access;
    uint64_t address;
} PACKED;

struct fadt {
    struct sdt_header h;
    uint32_t firmware_ctrl, dsdt;
    uint8_t reserved, pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
    uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, pm2_cnt, pm_tmr, gpe0, gpe1;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len, gpe0_len, gpe1_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alarm, mon_alarm, century;
    uint16_t boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_ctrl, x_dsdt;
    struct gas x_pm1a_evt, x_pm1b_evt, x_pm1a_cnt, x_pm1b_cnt, x_pm2_cnt, x_pm_tmr, x_gpe0, x_gpe1;
    struct gas sleep_control, sleep_status; /* ACPI 5: hardware-reduced platforms */
} PACKED;

#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED    (1u << 20)

static struct fadt *fadt;
static uint16_t slp_typa, slp_typb;
static bool s5_ok;

static void *acpi_ptr(uint64_t phys) {
    vmm_map_mmio(phys, 4096); /* make sure it is mapped (uncached is fine for tables) */
    struct sdt_header *h = phys_to_virt(phys);
    vmm_map_mmio(phys, h->length > 4096 ? h->length : 4096);
    return h;
}

static void *find_table(const char *sig) {
    if (!rsdp_req.response) return NULL;
    uint8_t *rsdp = (uint8_t *)rsdp_req.response->address;
    if ((uint64_t)rsdp < hhdm_offset) rsdp = phys_to_virt((uint64_t)rsdp);
    uint8_t rev = rsdp[15];
    uint64_t xsdt = rev >= 2 ? *(uint64_t *)(rsdp + 24) : 0;
    uint32_t rsdt = *(uint32_t *)(rsdp + 16);
    if (xsdt) {
        struct sdt_header *x = acpi_ptr(xsdt);
        int n = (x->length - sizeof *x) / 8;
        uint64_t *e = (uint64_t *)(x + 1);
        for (int i = 0; i < n; i++) {
            struct sdt_header *t = acpi_ptr(e[i]);
            if (!memcmp(t->sig, sig, 4)) return t;
        }
    } else if (rsdt) {
        struct sdt_header *x = acpi_ptr(rsdt);
        int n = (x->length - sizeof *x) / 4;
        uint32_t *e = (uint32_t *)(x + 1);
        for (int i = 0; i < n; i++) {
            struct sdt_header *t = acpi_ptr(e[i]);
            if (!memcmp(t->sig, sig, 4)) return t;
        }
    }
    return NULL;
}

/* Write a Generic Address Structure register (system memory or I/O space). */
static void gas_write(const struct gas *g, uint64_t v) {
    if (g->space == 0) {
        void *p = vmm_map_mmio(g->address & ~0xFFFULL, 4096);
        p = (uint8_t *)p + (g->address & 0xFFF);
        switch (g->bit_width) {
        case 16: *(volatile uint16_t *)p = (uint16_t)v; break;
        case 32: *(volatile uint32_t *)p = (uint32_t)v; break;
        case 64: *(volatile uint64_t *)p = v; break;
        default: *(volatile uint8_t *)p = (uint8_t)v; break;
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

static void list_tables(void) {
    if (!rsdp_req.response) return;
    uint8_t *rsdp = (uint8_t *)rsdp_req.response->address;
    if ((uint64_t)rsdp < hhdm_offset) rsdp = phys_to_virt((uint64_t)rsdp);
    uint64_t xsdt = rsdp[15] >= 2 ? *(uint64_t *)(rsdp + 24) : 0;
    char line[160];
    size_t n = 0;
    if (xsdt) {
        struct sdt_header *x = acpi_ptr(xsdt);
        int cnt = (x->length - sizeof *x) / 8;
        uint64_t *e = (uint64_t *)(x + 1);
        for (int i = 0; i < cnt && n + 6 < sizeof line; i++) {
            struct sdt_header *t = acpi_ptr(e[i]);
            memcpy(line + n, t->sig, 4);
            line[n + 4] = ' ';
            n += 5;
        }
    }
    line[n] = 0;
    kprintf("acpi: tables %s\n", line);
}

void acpi_init(void) {
    list_tables();
    fadt = find_table("FACP");
    if (!fadt) {
        kprintf("acpi: no FADT found\n");
        return;
    }
    uint64_t dsdt_phys = (fadt->h.length >= 148 && fadt->x_dsdt) ? fadt->x_dsdt : fadt->dsdt;
    struct sdt_header *dsdt = acpi_ptr(dsdt_phys);
    uint8_t *p = (uint8_t *)(dsdt + 1);
    uint8_t *end = (uint8_t *)dsdt + dsdt->length;
    for (; p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4)) continue;
        if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
        uint8_t *q = p + 4;
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
}

void power_off(void) {
    cli();
    kprintf("power: shutting down\n");
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
    outw(0x604, 0x2000);  /* QEMU */
    outw(0xB004, 0x2000); /* Bochs / old QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */
    for (;;) hlt();
}

void power_reboot(void) {
    cli();
    kprintf("power: rebooting\n");
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
