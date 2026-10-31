/* The local APIC, in x2APIC mode only (arch/apic.h). Every CPU's goes to that mode as the CPU
   starts (apic_init_cpu), whatever mode the firmware or the bootloader left. SDM 13.12.7 asks for
   interrupt remapping with Extended Interrupt Mode before x2APIC mode; here x2APIC mode comes first
   (the bootloader already enters it, boot.c) and the IOMMU later (dev/iommu.c), a known deviation:
   until then only APIC IDs up to 255 are given devices' interrupts (arch/irq.c), which the plain
   MSI address and IOAPIC entry name. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/apic.h"
#include "arch/cpuset.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "mm/heap.h"

#define X2APIC_MSR_BASE 0x800
#define X2APIC_ICR 0x830   /* 64 bits: the destination in 63:32 (13.12.9) */
#define LAPIC_ID 0x20
#define LAPIC_VERSION 0x30
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xB0
#define LAPIC_LDR 0xD0     /* the logical x2APIC ID, read-only (13.12.10.2) */
#define LAPIC_SVR 0xF0
#define LAPIC_ESR 0x280
#define LAPIC_LVT_CMCI 0x2F0
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2 (1ULL << 10)
#define IA32_MCG_CAP 0x179
#define MCG_CMCI_P (1ULL << 10)
/* ICR and LVT fields (13.6.1, 13.5.1): delivery mode 10:8, logical destination 11, level assert
   14 (to be 1 but for INIT de-assert), polarity 13 (LINTn: 1 active low) */
#define DELIVER_NMI (4u << 8)
#define DELIVER_INIT (5u << 8)
#define DELIVER_STARTUP (6u << 8)
#define ICR_LOGICAL (1u << 11)
#define ICR_ASSERT (1u << 14)
#define LVT_ACTIVE_LOW (1u << 13)
#define SVR_ENABLE (1u << 8)

void lapic_write(uint32_t reg, uint32_t v) { wrmsr(X2APIC_MSR_BASE + (reg >> 4), v); }
uint32_t lapic_read(uint32_t reg) { return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4)); }

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
static bool cmci; /* Intel's CMCI LVT entry: IA32_MCG_CAP's MCG_CMCI_P (SDM Vol. 3B 18.2.1); AMD reserves the bit */
PERCPU static uint32_t logical_id; /* this CPU's LDR */

static enum irq_result apic_error(void *context) {
    (void)context;
    lapic_write(LAPIC_ESR, 0); /* a write before the read latches the errors (13.5.3); only 0 in x2APIC mode */
    kprintf("apic: cpu%u reports error %02x\n", cpu_current_index(), lapic_read(LAPIC_ESR));
    return IRQ_HANDLED;
}

void apic_init(void) {
    cmci = cpu_features.vendor == CPU_VENDOR_INTEL && (rdmsr(IA32_MCG_CAP) & MCG_CMCI_P);
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

/* x2APIC mode is entered from the enabled xAPIC mode, the disabled state first enabled (SDM
   13.12.5, APM 16.9.1); INIT keeps the mode (APM 16.10), reset and so S3 does not. */
static bool x2apic_enter(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE) && !wrmsr_checked(IA32_APIC_BASE, base |= APIC_BASE_ENABLE)) return false;
    if (!(base & APIC_BASE_X2) && !wrmsr_checked(IA32_APIC_BASE, base |= APIC_BASE_X2)) return false;
    return (rdmsr(IA32_APIC_BASE) & (APIC_BASE_ENABLE | APIC_BASE_X2)) == (APIC_BASE_ENABLE | APIC_BASE_X2);
}

/* The registers but the APIC ID are not kept across a mode transition (13.12.5): all written.
   Every architectural LVT entry the APIC has is masked first (13.4.8: Max LVT Entry; CMCI where
   IA32_MCG_CAP lists it), with a vector of 16 or above, as 0 to 15 in a Fixed entry may raise an
   illegal vector error even masked (13.5.2). AMD's extended LVT (APM 16.4.5) is not touched here.
   Then the spurious-interrupt register, written whole (13.9): VEC_SPURIOUS, the APIC enabled, EOI
   broadcasts to the IOAPICs kept (the level pins need them), focus checking. Only an enabled
   APIC lets a mask bit be cleared (13.4.7.2, APM 16.4.7): the error entry and the NMI inputs
   after it. NMI delivery is edge-triggered whatever the entry's trigger bit says (13.5.1). */
bool apic_init_cpu(void) {
    if (!x2apic_enter()) return false;
    uint32_t max_lvt = (lapic_read(LAPIC_VERSION) >> 16) & 0xFF;
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_ERROR, LVT_UNUSED);
    lapic_write(LAPIC_LVT_TIMER, LVT_UNUSED);
    lapic_write(LAPIC_LVT_LINT0, LVT_UNUSED);
    lapic_write(LAPIC_LVT_LINT1, LVT_UNUSED);
    if (max_lvt >= 4) lapic_write(LAPIC_LVT_PERF, LVT_UNUSED);
    if (max_lvt >= 5) lapic_write(LAPIC_LVT_THERMAL, LVT_UNUSED);
    if (max_lvt >= 6 && cmci) lapic_write(LAPIC_LVT_CMCI, LVT_UNUSED);
    lapic_write(LAPIC_SVR, SVR_ENABLE | VEC_SPURIOUS);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_LVT_ERROR, VEC_APIC_ERROR);
    uint32_t uid = topology.cpu[cpu_current_index()].uid;
    for (unsigned i = 0; i < nnmis; i++)
        if (nmis[i].every || nmis[i].uid == uid)
            lapic_write(nmis[i].lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0,
                        DELIVER_NMI | (ACPI_INTI_POLARITY(nmis[i].flags) == ACPI_INTI_LOW ? LVT_ACTIVE_LOW : 0));
    this_cpu(logical_id) = lapic_read(LAPIC_LDR);
    return true;
}

uint32_t apic_current_id(void) { return lapic_read(LAPIC_ID); }

void apic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

/* One ICR write. The stores before it reach every CPU first (13.12.3: WRMSR to an APIC register is
   not serializing): an IPI never arrives ahead of what it announces. */
static void icr(uint32_t dest, uint32_t low) {
    __asm__ volatile("mfence; lfence" : : : "memory");
    wrmsr(X2APIC_ICR, (uint64_t)dest << 32 | low);
}

void apic_send_ipi(unsigned cpu, uint8_t vector) { icr(topology.cpu[cpu].apic_id, ICR_ASSERT | vector); }

/* The CPUs of set (but this one with skip_self), a write per run of CPUs in one logical cluster:
   the logical x2APIC IDs of a cluster OR'ed together (13.12.10.1). A CPU whose LDR reads 0 (no
   logical ID) is sent to by its APIC ID. */
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
