/* I/O APIC. Register layout per the Intel 82093AA datasheet / ICH IOAPIC chapter: IOREGSEL at +0x00,
   IOWIN at +0x10; reg 0 = ID, reg 1 = version and max redirection entry, 0x10+2n / 0x11+2n = RTE n. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "mm/vmm.h"

#define IOAPIC_MAX 8
#define REG_ID     0x00
#define REG_VER    0x01
#define REG_RTE(n) (0x10 + 2 * (n))

#define RTE_MASK      (1u << 16)
#define RTE_LEVEL     (1u << 15)
#define RTE_ACTIVELOW (1u << 13)
#define RTE_DELIV(x)  (((x) >> 8) & 7)
#define DELIV_EXTINT  7

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base, pins;
    uint8_t id, version;
};

static struct ioapic ios[IOAPIC_MAX];
static unsigned nios;
/* The entry each pin was given, in the plain format: written again through dev/iommu.h when
   interrupt remapping comes on or goes off (ioapic_remap). */
static uint64_t given[IOAPIC_MAX][256];

static uint32_t rd(const struct ioapic *io, uint32_t reg) {
    io->regs[0] = reg;
    return io->regs[4];
}

static void wr(const struct ioapic *io, uint32_t reg, uint32_t v) {
    io->regs[0] = reg;
    io->regs[4] = v;
}

void ioapic_init(void) {
    unsigned n;
    const struct acpi_ioapic *a = acpi_ioapics(&n);
    for (unsigned i = 0; i < n && nios < IOAPIC_MAX; i++) {
        if (!a[i].addr) continue;
        struct ioapic *io = &ios[nios];
        io->regs = vmm_map_mmio(a[i].addr, 4096);
        io->gsi_base = a[i].gsi_base;
        uint64_t f = irq_save();
        uint32_t ver = rd(io, REG_VER);
        if (ver == 0xFFFFFFFF) { /* nothing decodes this address */
            irq_restore(f);
            kprintf("ioapic: id %u at %x does not respond, ignored\n", a[i].id, a[i].addr);
            continue;
        }
        io->version = ver & 0xFF;
        io->pins = ((ver >> 16) & 0xFF) + 1;
        io->id = a[i].id; /* the MADT's: the one the DMAR's scopes name (its register may differ) */
        unsigned kept = 0;
        for (uint32_t pin = 0; pin < io->pins; pin++) {
            uint32_t lo = rd(io, REG_RTE(pin));
            if (RTE_DELIV(lo) == DELIV_EXTINT && !(lo & RTE_MASK)) { /* virtual wire B: leave the PIC's path */
                given[nios][pin] = lo | (uint64_t)rd(io, REG_RTE(pin) + 1) << 32;
                kept++;
                continue;
            }
            wr(io, REG_RTE(pin), RTE_MASK | (lo & ~0xFFu)); /* mask first, then clear the destination */
            wr(io, REG_RTE(pin) + 1, 0);
            wr(io, REG_RTE(pin), RTE_MASK);
            given[nios][pin] = RTE_MASK;
        }
        irq_restore(f);
        kprintf("ioapic: id %u at %x, version %x, GSI %u-%u, all masked%s\n", io->id, a[i].addr, io->version,
                io->gsi_base, io->gsi_base + io->pins - 1, kept ? " (ExtINT pin kept)" : "");
        nios++;
    }
}

unsigned ioapic_count(void) { return nios; }

static struct ioapic *owner(uint32_t gsi, uint32_t *pin) {
    for (unsigned i = 0; i < nios; i++) {
        if (gsi >= ios[i].gsi_base && gsi - ios[i].gsi_base < ios[i].pins) {
            *pin = gsi - ios[i].gsi_base;
            return &ios[i];
        }
    }
    return NULL;
}

/* An entry given in the plain format, as interrupt remapping wants it (dev/iommu.h): false when
   it cannot be delivered, and the pin is masked. */
static bool rte_write(struct ioapic *io, uint32_t pin, uint64_t rte) {
    given[io - ios][pin] = rte;
    if (!iommu_ioapic_entry(io->id, io->gsi_base + pin, &rte)) {
        wr(io, REG_RTE(pin), rd(io, REG_RTE(pin)) | RTE_MASK);
        given[io - ios][pin] |= RTE_MASK;
        return false;
    }
    wr(io, REG_RTE(pin), RTE_MASK | (uint32_t)rte); /* mask while the destination changes */
    wr(io, REG_RTE(pin) + 1, (uint32_t)(rte >> 32));
    wr(io, REG_RTE(pin), (uint32_t)rte);
    return true;
}

bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, bool level, bool active_low) {
    uint32_t pin;
    struct ioapic *io = owner(gsi, &pin);
    if (!io || vector < 0x30 || vector == 0xFF || dest_apic_id > 0xFF) return false;
    uint32_t lo = vector | (level ? RTE_LEVEL : 0) | (active_low ? RTE_ACTIVELOW : 0); /* fixed, physical */
    uint64_t f = irq_save();
    bool ok = rte_write(io, pin, lo | (uint64_t)dest_apic_id << 56);
    irq_restore(f);
    return ok;
}

bool ioapic_remap(void) {
    uint64_t f = irq_save();
    bool ok = true;
    for (unsigned i = 0; i < nios; i++)
        for (uint32_t pin = 0; pin < ios[i].pins; pin++) {
            uint64_t requested = given[i][pin];
            if (!(requested & RTE_MASK) && !rte_write(&ios[i], pin, requested)) {
                given[i][pin] = requested; /* transition rollback, not a new failed route */
                ok = false;
            }
        }
    irq_restore(f);
    return ok;
}

bool ioapic_mask(uint32_t gsi) {
    uint32_t pin;
    struct ioapic *io = owner(gsi, &pin);
    if (!io) return false;
    uint64_t f = irq_save();
    wr(io, REG_RTE(pin), rd(io, REG_RTE(pin)) | RTE_MASK);
    given[io - ios][pin] |= RTE_MASK;
    irq_restore(f);
    return true;
}

uint32_t ioapic_isa_to_gsi(unsigned isa_irq, bool *level, bool *active_low) {
    unsigned n;
    const struct acpi_iso *o = acpi_isos(&n);
    bool lv = false, low = false; /* ISA: edge-triggered, active high */
    uint32_t gsi = isa_irq;
    for (unsigned i = 0; i < n; i++) {
        if (o[i].bus != 0 || o[i].source != isa_irq) continue;
        gsi = o[i].gsi;
        unsigned pol = o[i].flags & 3, trg = (o[i].flags >> 2) & 3;
        if (pol == 3) low = true;     /* 00 and 01 keep the ISA default / active high */
        if (trg == 3) lv = true;      /* 00 and 01 keep the ISA default / edge */
        break;
    }
    if (level) *level = lv;
    if (active_low) *active_low = low;
    return gsi;
}

/* Sleep: every redirection entry and the id, written back after S3 (the chip comes back reset). */
static uint64_t saved_rte[IOAPIC_MAX][256];
static uint32_t saved_id[IOAPIC_MAX];

void ioapic_suspend(void) {
    for (unsigned i = 0; i < nios; i++) {
        saved_id[i] = rd(&ios[i], REG_ID);
        for (uint32_t pin = 0; pin < ios[i].pins && pin < 256; pin++)
            saved_rte[i][pin] = rd(&ios[i], REG_RTE(pin)) | (uint64_t)rd(&ios[i], REG_RTE(pin) + 1) << 32;
    }
    ioapic_mask_all(); /* saved state is the requested route; no source emits during unit restore */
}

void ioapic_mask_all(void) {
    for (unsigned i = 0; i < nios; i++)
        for (uint32_t pin = 0; pin < ios[i].pins && pin < 256; pin++)
            wr(&ios[i], REG_RTE(pin), rd(&ios[i], REG_RTE(pin)) | RTE_MASK);
}

void ioapic_resume(void) {
    for (unsigned i = 0; i < nios; i++) {
        wr(&ios[i], REG_ID, saved_id[i]);
        for (uint32_t pin = 0; pin < ios[i].pins && pin < 256; pin++) {
            uint64_t v = saved_rte[i][pin];
            wr(&ios[i], REG_RTE(pin), (uint32_t)v | RTE_MASK); /* masked while the destination goes in */
            wr(&ios[i], REG_RTE(pin) + 1, (uint32_t)(v >> 32));
            wr(&ios[i], REG_RTE(pin), (uint32_t)v);
        }
    }
}
