/* The local APIC (arch/apic.h). One mode for every CPU, chosen once on CPU 0 (apic_choose_mode) and
   entered by each CPU itself as it starts (apic_init_cpu): the CPUs come in through the wake page
   in the mode the firmware or INIT left them in (arch/wake.asm reads their IDs from CPUID, which
   gives them in either mode). Device interrupts reach APIC IDs up to 255 through the plain MSI
   address and IOAPIC entry, and every APIC ID through interrupt remapping (arch/irq.c,
   dev/iommu.c). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/apic.h"
#include "arch/cpuset.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "mm/heap.h"
#include "mm/vmm.h"

#define X2APIC_MSR_BASE 0x800
#define X2APIC_ICR 0x830   /* 64 bits: the destination in 63:32 (13.12.9) */
#define LAPIC_ID 0x20      /* xAPIC mode: the ID in 31:24 (13.4.6, APM 16.3.3) */
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xB0
#define LAPIC_LDR 0xD0     /* the logical x2APIC ID, read-only (13.12.10.2) */
#define LAPIC_SVR 0xF0
#define LAPIC_TMR 0x180    /* 8 registers of 32 bits, 10h apart (13.8.4, Figure 13-20) */
#define LAPIC_IRR 0x200    /* 8 registers of 32 bits, 10h apart (13.8.4) */
#define LAPIC_ESR 0x280
#define LAPIC_ICR_LOW 0x300  /* xAPIC mode: the write that sends (13.6.1) */
#define LAPIC_ICR_HIGH 0x310 /* xAPIC mode: the destination in 31:24 */
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2 (1ULL << 10)
#define APIC_BASE_ADDRESS 0x000FFFFFFFFFF000ULL /* bits MAXPHYADDR-1:12, the rest reserved (13.4.4) */
/* ICR and LVT fields (13.6.1, 13.5.1): delivery mode 10:8, logical destination 11, delivery status
   12 (xAPIC mode only), level assert 14 (to be 1 but for INIT de-assert), polarity 13 (LINTn: 1
   active low) */
#define DELIVER_NMI (4u << 8)
#define DELIVER_INIT (5u << 8)
#define DELIVER_STARTUP (6u << 8)
#define ICR_LOGICAL (1u << 11)
#define ICR_PENDING (1u << 12)
#define ICR_ASSERT (1u << 14)
#define LVT_ACTIVE_LOW (1u << 13)
#define SVR_ENABLE (1u << 8)
#define XAPIC_BROADCAST 0xFF
/* the DMAR's Flags byte (VT-d 8.1): INTR_REMAP, and X2APIC_OPT_OUT, which counts only with it */
#define DMAR_FLAGS 37
#define DMAR_INTR_REMAP (1u << 0)
#define DMAR_X2APIC_OPT_OUT (1u << 1)

static bool x2;                /* the chosen mode: x2APIC, else xAPIC */
static uint64_t xapic_phys;    /* xAPIC mode: where every CPU's registers are */
static volatile uint8_t *xapic; /* and the kernel's uncached mapping of them */

bool apic_x2(void) { return x2; }

bool apic_reaches(uint32_t apic_id) { return x2 || apic_id < XAPIC_BROADCAST; }

void lapic_write(uint32_t reg, uint32_t v) {
    if (x2) wrmsr(X2APIC_MSR_BASE + (reg >> 4), v);
    else mmio_write32(xapic + reg, v);
}

uint32_t lapic_read(uint32_t reg) {
    if (x2) return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
    return mmio_read32(xapic + reg);
}

/* ---- the mode ---- */

/* The DMAR asks the OS to keep the APICs out of x2APIC mode (VT-d 8.1). */
static bool dmar_x2apic_opt_out(void) {
    const struct acpi_sdt *dmar = acpi_find("DMAR", 0);
    if (!dmar || dmar->length <= DMAR_FLAGS) return false;
    uint8_t flags = ((const uint8_t *)dmar)[DMAR_FLAGS];
    return (flags & (DMAR_INTR_REMAP | DMAR_X2APIC_OPT_OUT)) == (DMAR_INTR_REMAP | DMAR_X2APIC_OPT_OUT);
}

/* The rule of apic_choose_mode (arch/apic.h); why: what decided it. */
static bool x2_chosen(bool handed_x2, bool has_x2, bool opt_out, bool unremapped, const char **why) {
    if (handed_x2) return *why = "the firmware handed it over", true;
    if (!has_x2) return *why = "this CPU has no x2APIC mode", false;
    if (opt_out) return *why = "the DMAR asks the OS to keep out of x2APIC mode (X2APIC_OPT_OUT)", false;
    if (!unremapped) return *why = "the firmware handed over xAPIC mode, and x2APIC mode is to follow interrupt remapping, which comes later", false;
    return *why = "this maker's APIC enters it before interrupt remapping", true;
}

void apic_choose_mode(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    bool handed_x2 = (base & (APIC_BASE_ENABLE | APIC_BASE_X2)) == (APIC_BASE_ENABLE | APIC_BASE_X2);
    const char *why;
    x2 = x2_chosen(handed_x2, cpu_features.x2apic, dmar_x2apic_opt_out(), cpu_features.maker->x2apic_unremapped, &why);
    kprintf("apic: %s mode: %s\n", x2 ? "x2APIC" : "xAPIC", why);
}

/* x2APIC mode is entered from the enabled xAPIC mode, the disabled state first enabled (SDM
   13.12.5.1, APM 16.9.1); INIT keeps the mode (13.12.5.1, APM 16.10), reset and so S3 does not. */
static bool x2apic_enter(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE) && !wrmsr_checked(IA32_APIC_BASE, base |= APIC_BASE_ENABLE)) return false;
    if (!(base & APIC_BASE_X2) && !wrmsr_checked(IA32_APIC_BASE, base |= APIC_BASE_X2)) return false;
    return (rdmsr(IA32_APIC_BASE) & (APIC_BASE_ENABLE | APIC_BASE_X2)) == (APIC_BASE_ENABLE | APIC_BASE_X2);
}

/* xAPIC mode, enabled, its registers where CPU 0's are (a CPU's own base may be anywhere, 13.4.5).
   A CPU found in x2APIC mode is not taken out of it: the firmware hands every CPU over in one mode
   (13.12.8.1), and xAPIC mode is chosen only when CPU 0 came in xAPIC mode. */
static bool xapic_enter(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (base & APIC_BASE_X2) return false;
    uint64_t want = (base & ~APIC_BASE_ADDRESS) | xapic_phys | APIC_BASE_ENABLE;
    if (want != base && !wrmsr_checked(IA32_APIC_BASE, want)) return false;
    return (rdmsr(IA32_APIC_BASE) & (APIC_BASE_ENABLE | APIC_BASE_X2 | APIC_BASE_ADDRESS)) ==
           (APIC_BASE_ENABLE | xapic_phys);
}

/* ---- each CPU's APIC ---- */

/* The LINTn inputs NMI comes in on (MADT, ACPI 6.5 5.2.12.7 and 5.2.12.13), for the processor
   with that UID or every one: read once (apic_init), set on each CPU (apic_init_cpu). */
struct lint_nmi {
    uint32_t uid;
    bool every;
    uint8_t lint;
    uint16_t flags;
};
static struct lint_nmi *nmis;
static unsigned nnmis;
PERCPU static uint32_t logical_id; /* this CPU's LDR in x2APIC mode; 0 in xAPIC mode (physical IPIs) */

static enum irq_result apic_error(void *context) {
    (void)context;
    lapic_write(LAPIC_ESR, 0); /* a write before the read latches the errors (13.5.3); only 0 in x2APIC mode */
    kprintf("apic: cpu%u reports error %02x\n", cpu_current_index(), lapic_read(LAPIC_ESR));
    return IRQ_HANDLED;
}

void apic_init(void) {
    if (!x2) {
        xapic_phys = rdmsr(IA32_APIC_BASE) & APIC_BASE_ADDRESS;
        if (!(xapic = vmm_map_mmio(xapic_phys, PAGE_SIZE))) panic("apic: no page table for the APIC's registers");
    }
    const struct acpi_sdt *madt = acpi_find("APIC", 0);
    const struct acpi_entry *e;
    for (int pass = 0; pass < 2; pass++) {
        unsigned found = 0;
        for (e = madt ? acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL) : NULL; e; e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e)) {
            const struct acpi_madt_lapic_nmi *l = (const void *)e;
            const struct acpi_madt_x2apic_nmi *x = (const void *)e;
            struct lint_nmi n;
            if (e->type == ACPI_MADT_LAPIC_NMI && e->length >= sizeof *l)
                n = (struct lint_nmi){l->uid, l->uid == 0xFF, l->lint, l->flags};
            else if (e->type == ACPI_MADT_X2APIC_NMI && e->length >= sizeof *x)
                n = (struct lint_nmi){x->uid, x->uid == UINT32_MAX, x->lint, x->flags};
            else continue;
            if (n.lint > 1) continue; /* LINT0 and LINT1 are all an APIC has */
            if (pass) nmis[found] = n;
            found++;
        }
        if (pass || !found) {
            nnmis = found;
            break;
        }
        if (!(nmis = kmalloc(found * sizeof *nmis))) panic("apic: no memory for the MADT's NMI inputs");
    }
    if (!irq_request_system(&(struct irq_spec){.name = "apic error", .handler = apic_error, .node = -1}, VEC_APIC_ERROR))
        panic("apic: the error vector is taken");
}

/* The registers but the APIC ID are not kept across a mode transition (13.12.5.1), nor INIT's
   (13.4.7.3): all written. Every architectural LVT entry the APIC has is masked first (13.4.8:
   Max LVT Entry; CMCI where the CPU has it, cpu_features.cmci), then the maker's own (AMD's
   extended LVT, APM 16.4.5), with a vector of 16 or above, as 0 to 15 in a Fixed entry may raise
   an illegal vector error even masked (13.5.2). Then the spurious-interrupt register, written whole
   (13.9): VEC_SPURIOUS, the APIC enabled, EOI broadcasts to the IOAPICs kept (the level pins need
   them), focus checking. Only an enabled APIC lets a mask bit be cleared (13.4.7.2, APM 16.4.7):
   the error entry and the NMI inputs after it. NMI delivery is edge-triggered whatever the entry's
   trigger bit says (13.5.1). */
bool apic_init_cpu(void) {
    if (!(x2 ? x2apic_enter() : xapic_enter())) return false;
    uint32_t max_lvt = (lapic_read(LAPIC_VERSION) >> 16) & 0xFF;
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_ERROR, LVT_UNUSED);
    lapic_write(LAPIC_LVT_TIMER, LVT_UNUSED);
    lapic_write(LAPIC_LVT_LINT0, LVT_UNUSED);
    lapic_write(LAPIC_LVT_LINT1, LVT_UNUSED);
    if (max_lvt >= 4) lapic_write(LAPIC_LVT_PERF, LVT_UNUSED);
    if (max_lvt >= 5) lapic_write(LAPIC_LVT_THERMAL, LVT_UNUSED);
    if (max_lvt >= 6 && cpu_features.cmci) lapic_write(LAPIC_LVT_CMCI, LVT_UNUSED);
    if (cpu_features.maker->mask_own_lvt) cpu_features.maker->mask_own_lvt();
    lapic_write(LAPIC_SVR, SVR_ENABLE | VEC_SPURIOUS);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_LVT_ERROR, VEC_APIC_ERROR);
    uint32_t uid = topology.cpu[cpu_current_index()].uid;
    for (unsigned i = 0; i < nnmis; i++)
        if (nmis[i].every || nmis[i].uid == uid)
            lapic_write(nmis[i].lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0,
                        DELIVER_NMI | (ACPI_INTI_POLARITY(nmis[i].flags) == ACPI_INTI_LOW ? LVT_ACTIVE_LOW : 0));
    this_cpu(logical_id) = x2 ? lapic_read(LAPIC_LDR) : 0;
    return true;
}

uint32_t apic_current_id(void) { return x2 ? lapic_read(LAPIC_ID) : lapic_read(LAPIC_ID) >> 24; }

void apic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

bool apic_irr(uint8_t vector) { return lapic_read(LAPIC_IRR + 0x10 * (vector >> 5)) & (1u << (vector & 31)); }

bool apic_tmr(uint8_t vector) { return lapic_read(LAPIC_TMR + 0x10 * (vector >> 5)) & (1u << (vector & 31)); }

/* ---- IPIs ---- */

/* One IPI. x2APIC mode: one WRMSR, the stores before it reaching every CPU first (13.12.3: WRMSR
   to an APIC register is not serializing), so that an IPI never arrives ahead of what it
   announces. xAPIC mode: the uncached stores keep their order after the earlier ones; the high
   half, then the low half that sends, with this CPU's interrupts off so that no handler's IPI
   comes between them, once the previous IPI has left (delivery status idle, 13.6.1). An NMI or a
   machine check between them that sends is a panic's, which sends no more after. */
static void icr(uint32_t dest, uint32_t low) {
    if (x2) {
        __asm__ volatile("mfence; lfence" : : : "memory");
        wrmsr(X2APIC_ICR, (uint64_t)dest << 32 | low);
        return;
    }
    ASSERT(dest < XAPIC_BROADCAST);
    uint64_t f = irq_save();
    while (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING) pause();
    lapic_write(LAPIC_ICR_HIGH, dest << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    irq_restore(f);
}

void apic_send_ipi(unsigned cpu, uint8_t vector) { icr(topology.cpu[cpu].apic_id, ICR_ASSERT | vector); }

/* The CPUs of set (but this one with skip_self), a write per run of CPUs in one logical cluster:
   the logical x2APIC IDs of a cluster OR'ed together (13.12.10.1). A CPU with no logical ID (its
   LDR read 0, or xAPIC mode) is sent to by its APIC ID. */
static void send_set(const uint64_t *set, bool skip_self, uint32_t low) {
    unsigned self = cpu_current_index();
    uint32_t cluster = 0, ids = 0;
    cpuset_each(cpu, set, topology.cpus) {
        if (skip_self && cpu == self) continue;
        uint32_t id = per_cpu(logical_id, cpu);
        if (!id) {
            icr(topology.cpu[cpu].apic_id, low);
            continue;
        }
        if (ids && id >> 16 != cluster) {
            icr(cluster << 16 | ids, low | ICR_LOGICAL);
            ids = 0;
        }
        cluster = id >> 16;
        ids |= id & 0xFFFF;
    }
    if (ids) icr(cluster << 16 | ids, low | ICR_LOGICAL);
}

void apic_send_ipi_set(const uint64_t *cpus, uint8_t vector) { send_set(cpus, false, ICR_ASSERT | vector); }

void apic_send_ipi_others(uint8_t vector) { send_set(cpu_online_set(), true, ICR_ASSERT | vector); }

void apic_send_nmi(unsigned cpu) { icr(topology.cpu[cpu].apic_id, ICR_ASSERT | DELIVER_NMI); }

void apic_send_init(unsigned cpu) { icr(topology.cpu[cpu].apic_id, ICR_ASSERT | DELIVER_INIT); }

void apic_send_startup(unsigned cpu, uint64_t page) {
    ASSERT(!(page & 0xFFF) && page < 0x100000);
    icr(topology.cpu[cpu].apic_id, ICR_ASSERT | DELIVER_STARTUP | (uint32_t)(page >> 12));
}
