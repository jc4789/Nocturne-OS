/* Local APIC. Legacy device IRQs still come from the 8259 PIC, routed through LINT0 (virtual wire
   mode); the scheduler tick comes from the LAPIC timer, because Hyper-V Generation 2 VMs have no
   PIT. Vectors 0x30 and up are LAPIC-delivered (timer, and later Hyper-V's SynIC) and need a LAPIC EOI. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/hyperv.h"
#include "mm/vmm.h"

#define LAPIC_EOI       0xB0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

static volatile uint32_t *lapic;
static uint32_t timer_reload;
static int timer_vector;

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
    lapic_write(LAPIC_LVT_TIMER, 0x10000);                /* timer masked */
    lapic_write(0x350, 0x700);                            /* LINT0: ExtINT (the PIC) */
    lapic_write(0x360, 0x400);                            /* LINT1: NMI */
    lapic_write(0x370, 0x10000 | 0xFE);                   /* error: masked */
    kprintf("apic: LAPIC at %p id %u, PIC in virtual wire mode\n", (void *)phys, lapic_read(0x20) >> 24);
}

bool lapic_present(void) { return lapic != NULL; }

uint32_t lapic_current_id(void) { return lapic ? lapic_read(0x20) >> 24 : 0; }

bool lapic_worker_init(void) {
    if (!lapic) return false;
    uint64_t base = rdmsr(0x1B);
    if (!(base & (1ULL << 11)) || (base & (1ULL << 10))) return false;
    lapic_write(0x80, 0);
    lapic_write(0xF0, (lapic_read(0xF0) & ~0xFFu) | 0x1FF);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    lapic_write(0x350, 0x10000); /* PIC は BSP 専用 */
    lapic_write(0x360, 0x10000);
    lapic_write(0x370, 0x10000 | 0xFE);
    uint32_t max_lvt = (lapic_read(0x30) >> 16) & 0xFF;
    if (max_lvt >= 4) lapic_write(0x340, 0x10000); /* performance counter */
    if (max_lvt >= 5) lapic_write(0x330, 0x10000); /* thermal sensor */
    if (max_lvt >= 6) lapic_write(0x2F0, 0x10000); /* corrected machine check */
    return true;
}

static bool ipi_ready(void) {
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(lapic_read(0x300) & (1u << 12))) return true;
        pause();
    }
    return false;
}

bool lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    if (!lapic || apic_id > 255 || vector < 32) return false;
    uint64_t flags = irq_save();
    bool ok = ipi_ready();
    if (ok) {
        lapic_write(0x310, apic_id << 24);
        lapic_write(0x300, vector); /* fixed, physical, edge-triggered */
        ok = ipi_ready();
    }
    irq_restore(flags);
    return ok;
}

void lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

uint64_t tsc_hz;

/* Count LAPIC timer ticks (divide by 1) over 10 ms of PIT channel 2, and the TSC's into tsc_hz.
   Returns Hz, or 0 if the PIT is missing. Channel 2 is polled through port 0x61, so no PIT
   interrupt is needed. */
static uint64_t calibrate_with_pit(void) {
    uint8_t p61 = inb(0x61);
    if (p61 == 0xFF) return 0;                /* nothing decodes the port */
    outb(0x61, (p61 & ~0x02) | 0x01);         /* speaker off, gate on */
    outb(0x43, 0xB0);                         /* channel 2, lo/hi byte, mode 0 */
    uint16_t count = 11932;                   /* 1193182 Hz / 100 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    outb(0x61, inb(0x61) & ~0x01);            /* restart the count: gate low ... */
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    uint64_t tsc0 = rdtsc();
    outb(0x61, inb(0x61) | 0x01);             /* ... and high */
    uint64_t spins = 0;
    while (!(inb(0x61) & 0x20)) {
        if (++spins > 50000000) return 0;
    }
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    tsc_hz = (rdtsc() - tsc0) * 100;
    lapic_write(LAPIC_TIMER_INIT, 0);
    outb(0x61, p61);
    if (elapsed < 1000) return 0;             /* the output went high at once: no PIT */
    return (uint64_t)elapsed * 100;
}

/* Start the LAPIC timer as a periodic interrupt on `vector`. Returns false if that is impossible. */
bool lapic_timer_start(unsigned hz, int vector) {
    if (!lapic) return false;
    lapic_write(LAPIC_TIMER_DIV, 0xB);        /* divide by 1 */
    lapic_write(LAPIC_LVT_TIMER, 0x10000 | (uint32_t)vector);
    const char *src = "Hyper-V";
    uint64_t freq = hv_apic_hz();
    if (freq) tsc_hz = hv_tsc_hz();
    if (!freq) {
        src = "PIT";
        freq = calibrate_with_pit();
    }
    if (!freq) {
        kprintf("apic: cannot measure the LAPIC timer frequency\n");
        return false;
    }
    uint32_t init = (uint32_t)(freq / hz);
    timer_reload = init;
    timer_vector = vector;
    lapic_write(LAPIC_LVT_TIMER, 0x20000 | (uint32_t)vector); /* periodic, unmasked */
    lapic_write(LAPIC_TIMER_INIT, init);
    kprintf("apic: timer %lu kHz (from %s), %u Hz tick, TSC %lu kHz\n", freq / 1000, src, hz, tsc_hz / 1000);
    return true;
}

bool lapic_runner_timer_start(void) {
    if (!lapic || !timer_reload) return false;
    lapic_write(LAPIC_TIMER_DIV, 0xB);
    lapic_write(LAPIC_LVT_TIMER, 0x20000 | (uint32_t)timer_vector);
    lapic_write(LAPIC_TIMER_INIT, timer_reload);
    return true;
}
