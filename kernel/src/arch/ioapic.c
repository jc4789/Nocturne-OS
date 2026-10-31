/* The I/O APICs (arch/ioapic.h). Register layout of the Intel 82093AA: IOREGSEL at +00h, IOWIN at
   +10h; register 0 the ID, register 1 the version and the highest redirection entry in bits 23:16,
   10h + 2n and 11h + 2n redirection entry n. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "mm/heap.h"
#include "mm/vmm.h"

#define REG_ID     0x00
#define REG_VER    0x01
#define REG_RTE(n) (0x10 + 2 * (n))

/* A redirection entry (82093AA 3.2.4): vector 7:0, delivery mode 10:8, polarity 13 (1: active
   low), trigger 15 (1: level), mask 16, destination 63:56 */
#define RTE_ACTIVELOW (1u << 13)
#define RTE_LEVEL     (1u << 15)
#define RTE_MASK      (1u << 16)
#define DELIV_NMI     (4u << 8)

/* What a pin was given: its signal, and its route (the IOMMU makes the entry from it). */
struct pin {
    uint32_t dest;   /* APIC ID */
    uint8_t vector;
    bool routed;     /* a vector's (ioapic_route) */
    bool held;       /* masked by its line (ioapic_hold) */
    bool nmi;        /* an NMI source: delivered as an NMI to CPU 0, no device's */
    bool level, active_low;
};

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base, pins;
    uint32_t saved_id; /* the ID register across S3 (the chip comes back reset) */
    uint8_t id;        /* the MADT's: the one the DMAR's scopes name (its register may differ) */
    /* IOREGSEL/IOWIN (a register access is two) and the pins; before the IOMMU back end's lock */
    struct spinlock lock;
    struct pin pin[];
};

static struct ioapic **ios; /* by GSI base, their ranges apart */
static unsigned nios;

static uint32_t rd(const struct ioapic *io, uint32_t reg) {
    mmio_write32(io->regs, reg);
    return mmio_read32(io->regs + 4);
}

static void wr(const struct ioapic *io, uint32_t reg, uint32_t v) {
    mmio_write32(io->regs, reg);
    mmio_write32(io->regs + 4, v);
}

/* The IOAPIC with gsi, *n its pin there; NULL when none has it. */
static struct ioapic *owner(uint32_t gsi, uint32_t *n) {
    unsigned lo = 0, hi = nios;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (gsi < ios[mid]->gsi_base) hi = mid;
        else if (gsi - ios[mid]->gsi_base >= ios[mid]->pins) lo = mid + 1;
        else {
            *n = gsi - ios[mid]->gsi_base;
            return ios[mid];
        }
    }
    return NULL;
}

/* The plain entry of a pin as given: masked unless routed or an NMI source, and while held. */
static uint64_t plain_entry(const struct pin *p) {
    if (!p->routed && !p->nmi) return RTE_MASK;
    uint64_t rte = p->nmi ? DELIV_NMI : p->vector; /* fixed delivery: 000b */
    if (p->active_low) rte |= RTE_ACTIVELOW;
    if (p->level && !p->nmi) rte |= RTE_LEVEL; /* an NMI is edge-triggered */
    if (p->held) rte |= RTE_MASK;
    return rte | (uint64_t)(p->dest <= 0xFF ? p->dest : 0) << 56;
}

/* Pin n written from what it was given, through interrupt remapping (dev/iommu.h): false when
   that cannot deliver it, and the pin is masked. io->lock held. */
static bool pin_write(struct ioapic *io, uint32_t n) {
    struct pin *p = &io->pin[n];
    uint64_t rte = plain_entry(p);
    bool ok = !(p->routed || p->nmi) || iommu_ioapic_entry(io->id, io->gsi_base + n, p->dest, &rte);
    if (!ok) rte = RTE_MASK;
    wr(io, REG_RTE(n), RTE_MASK | (uint32_t)rte); /* masked while the destination changes */
    wr(io, REG_RTE(n) + 1, (uint32_t)(rte >> 32));
    wr(io, REG_RTE(n), (uint32_t)rte);
    return ok;
}

/* The signal of an ISA source the MPS INTI flags describe: the bus's own (00) is edge and active
   high, except for the SCI, level and active low (ACPI 6.5 5.2.9, SCI_INT). */
static void inti_signal(struct pin *p, uint16_t flags, bool sci) {
    unsigned polarity = ACPI_INTI_POLARITY(flags), trigger = ACPI_INTI_TRIGGER(flags);
    p->active_low = polarity == ACPI_INTI_BUS ? sci : polarity == ACPI_INTI_LOW;
    p->level = trigger == ACPI_INTI_BUS ? sci : trigger == ACPI_INTI_LOW;
}

static void add(const struct acpi_madt_ioapic *a) {
    volatile uint32_t *regs = vmm_map_mmio(a->address, PAGE_SIZE);
    if (!regs) {
        kprintf("ioapic: id %u at %x: no memory to map it, ignored\n", a->id, a->address);
        return;
    }
    struct ioapic probe = {.regs = regs};
    uint32_t ver = rd(&probe, REG_VER);
    if (ver == 0xFFFFFFFF) { /* nothing decodes this address */
        kprintf("ioapic: id %u at %x does not respond, ignored\n", a->id, a->address);
        return;
    }
    uint32_t pins = ((ver >> 16) & 0xFF) + 1;
    for (unsigned i = 0; i < nios; i++)
        if (a->gsi_base < ios[i]->gsi_base + ios[i]->pins && ios[i]->gsi_base < a->gsi_base + pins) {
            kprintf("ioapic: id %u's GSIs %u-%u overlap id %u's, ignored\n", a->id, a->gsi_base,
                    a->gsi_base + pins - 1, ios[i]->id);
            return;
        }
    struct ioapic *io = kzalloc(sizeof *io + pins * sizeof io->pin[0]);
    struct ioapic **grown = io ? kmalloc((nios + 1) * sizeof *grown) : NULL;
    if (!grown) {
        kfree(io);
        kprintf("ioapic: id %u at %x: no memory for it, ignored\n", a->id, a->address);
        return;
    }
    *io = (struct ioapic){.regs = regs, .gsi_base = a->gsi_base, .pins = pins, .id = a->id};
    for (uint32_t n = 0; n < pins; n++) { /* ISA's identity below 16, PCI's INTx above */
        io->pin[n].level = io->pin[n].active_low = a->gsi_base + n >= 16;
    }
    unsigned at = 0;
    while (at < nios && ios[at]->gsi_base < io->gsi_base) at++;
    for (unsigned i = 0; i < nios; i++) grown[i < at ? i : i + 1] = ios[i];
    grown[at] = io;
    kfree(ios);
    ios = grown;
    nios++;
    kprintf("ioapic: id %u at %x, version %x, GSI %u-%u\n", io->id, a->address, ver & 0xFF, io->gsi_base,
            io->gsi_base + pins - 1);
}

void ioapic_init(void) {
    const struct acpi_sdt *madt = acpi_find("APIC", 0);
    const struct acpi_fadt *fadt = acpi_find("FACP", 0);
    const struct acpi_entry *e;
    for (e = madt ? acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL) : NULL; e; e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e))
        if (e->type == ACPI_MADT_IOAPIC && e->length >= sizeof(struct acpi_madt_ioapic) &&
            ((const struct acpi_madt_ioapic *)e)->address)
            add((const void *)e);
    /* The SCI: the GSI of its ISA IRQ (an override for it, else the same number), level and active
       low where the override leaves the signal to the bus */
    uint32_t n, sci = fadt && fadt->h.length >= __builtin_offsetof(struct acpi_fadt, sci_int) + 2 ? fadt->sci_int : UINT32_MAX;
    bool sci_overridden = false;
    for (e = madt ? acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL) : NULL; e; e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e)) {
        const struct acpi_madt_override *o = (const void *)e;
        const struct acpi_madt_nmi_source *s = (const void *)e;
        struct ioapic *io;
        if (e->type == ACPI_MADT_OVERRIDE && e->length >= sizeof *o && !o->bus && (io = owner(o->gsi, &n))) {
            inti_signal(&io->pin[n], o->flags, o->source == sci);
            sci_overridden |= o->source == sci;
        } else if (e->type == ACPI_MADT_NMI_SOURCE && e->length >= sizeof *s && (io = owner(s->gsi, &n))) {
            struct pin *p = &io->pin[n];
            p->nmi = true;
            p->active_low = ACPI_INTI_POLARITY(s->flags) == ACPI_INTI_LOW;
            p->dest = topology.cpu[0].apic_id;
        }
    }
    struct ioapic *io;
    if (!sci_overridden && sci != UINT32_MAX && (io = owner(sci, &n))) io->pin[n].level = io->pin[n].active_low = true;
    for (unsigned i = 0; i < nios; i++) {
        io = ios[i];
        uint64_t f = spin_lock(&io->lock);
        for (n = 0; n < io->pins; n++)
            if (!pin_write(io, n)) kprintf("ioapic: the NMI source at GSI %u cannot be delivered\n", io->gsi_base + n);
        spin_unlock(&io->lock, f);
    }
}

uint32_t ioapic_isa_gsi(unsigned isa_irq) {
    const struct acpi_sdt *madt = acpi_find("APIC", 0);
    for (const struct acpi_entry *e = madt ? acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL) : NULL; e;
         e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e)) {
        const struct acpi_madt_override *o = (const void *)e;
        if (e->type == ACPI_MADT_OVERRIDE && e->length >= sizeof *o && !o->bus && o->source == isa_irq) return o->gsi;
    }
    return isa_irq;
}

bool ioapic_gsi_signal(uint32_t gsi, bool level, bool active_low) {
    uint32_t n;
    struct ioapic *io = owner(gsi, &n);
    if (!io) return false;
    uint64_t f = spin_lock(&io->lock);
    struct pin *p = &io->pin[n];
    bool ok = !p->nmi && (!p->routed || (p->level == level && p->active_low == active_low));
    if (ok) {
        p->level = level;
        p->active_low = active_low;
    }
    spin_unlock(&io->lock, f);
    return ok;
}

bool ioapic_gsi_owned(uint32_t gsi, bool *level) {
    uint32_t n;
    struct ioapic *io = owner(gsi, &n);
    if (!io) return false;
    uint64_t f = spin_lock(&io->lock);
    bool ok = !io->pin[n].nmi;
    *level = io->pin[n].level;
    spin_unlock(&io->lock, f);
    return ok;
}

bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest) {
    uint32_t n;
    struct ioapic *io = owner(gsi, &n);
    if (!io || io->pin[n].nmi) return false;
    uint64_t f = spin_lock(&io->lock);
    struct pin *p = &io->pin[n];
    p->vector = vector;
    p->dest = dest;
    p->routed = true;
    bool ok = pin_write(io, n);
    if (!ok) p->routed = false;
    spin_unlock(&io->lock, f);
    return ok;
}

void ioapic_unroute(uint32_t gsi) {
    uint32_t n;
    struct ioapic *io = owner(gsi, &n);
    if (!io || io->pin[n].nmi) return;
    uint64_t f = spin_lock(&io->lock);
    io->pin[n].routed = false;
    pin_write(io, n);
    spin_unlock(&io->lock, f);
}

/* The entry's mask bit alone: it is the same bit in the remappable format (VT-d 5.1.5.1), and the
   rest stays as pin_write made it. */
void ioapic_hold(uint32_t gsi, bool held) {
    uint32_t n;
    struct ioapic *io = owner(gsi, &n);
    if (!io) return;
    spin_take(&io->lock);
    struct pin *p = &io->pin[n];
    p->held = held;
    uint32_t lo = rd(io, REG_RTE(n));
    wr(io, REG_RTE(n), held || !p->routed ? lo | RTE_MASK : lo & ~RTE_MASK);
    spin_give(&io->lock);
}

bool ioapic_remap(void) {
    bool ok = true;
    for (unsigned i = 0; i < nios; i++) {
        struct ioapic *io = ios[i];
        uint64_t f = spin_lock(&io->lock);
        for (uint32_t n = 0; n < io->pins; n++) ok &= pin_write(io, n);
        spin_unlock(&io->lock, f);
    }
    return ok;
}

void ioapic_mask_all(void) {
    for (unsigned i = 0; i < nios; i++) {
        struct ioapic *io = ios[i];
        uint64_t f = spin_lock(&io->lock);
        for (uint32_t n = 0; n < io->pins; n++) wr(io, REG_RTE(n), rd(io, REG_RTE(n)) | RTE_MASK);
        spin_unlock(&io->lock, f);
    }
}

void ioapic_suspend(void) {
    for (unsigned i = 0; i < nios; i++) {
        struct ioapic *io = ios[i];
        uint64_t f = spin_lock(&io->lock);
        io->saved_id = rd(io, REG_ID);
        spin_unlock(&io->lock, f);
    }
    ioapic_mask_all();
}

void ioapic_resume(void) {
    for (unsigned i = 0; i < nios; i++) {
        struct ioapic *io = ios[i];
        uint64_t f = spin_lock(&io->lock);
        wr(io, REG_ID, io->saved_id);
        spin_unlock(&io->lock, f);
    }
    if (!ioapic_remap()) kprintf("ioapic: an entry stays masked after sleep\n");
}
