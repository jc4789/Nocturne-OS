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
#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2 (1ULL << 10)
#define X2APIC_MSR_BASE 0x800
#define X2APIC_ICR 0x830

static volatile uint32_t *lapic;
static bool available, x2_mode;
static uint32_t timer_reload;
static int timer_vector;

/* x2APICではMMIO pageを読むこと自体が無効。通常32bit registerは
   offset/16 + 0x800 のMSR、64bit ICRだけは送信側で別扱いする。 */
static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2_mode) wrmsr(X2APIC_MSR_BASE + (reg >> 4), v);
    else lapic[reg / 4] = v;
}
static uint32_t lapic_read(uint32_t reg) {
    return x2_mode ? (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4)) : lapic[reg / 4];
}

void apic_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1 << 9))) {
        kprintf("apic: no local APIC, using plain PIC\n");
        return;
    }
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        kprintf("apic: local APIC globally disabled, using plain PIC\n");
        return;
    }
    x2_mode = (base & APIC_BASE_X2) != 0;
    if (x2_mode && !(c & (1u << 21))) panic("apic: x2APIC enabled without CPU support");
    uint64_t phys = base & 0xFFFFFF000ULL;
    if (!x2_mode) {
        lapic = vmm_map_mmio(phys, 4096);
        if (!lapic) panic("apic: cannot map local APIC");
    }
    available = true;
    lapic_write(0x80, 0);                                 /* TPR: accept everything */
    lapic_write(0xF0, (lapic_read(0xF0) & ~0xFFu) | 0x1FF); /* enable, spurious vector 0xFF */
    lapic_write(LAPIC_LVT_TIMER, 0x10000);                /* timer masked */
    lapic_write(0x350, 0x700);                            /* LINT0: ExtINT (the PIC) */
    lapic_write(0x360, 0x400);                            /* LINT1: NMI */
    lapic_write(0x370, 0x10000 | 0xFE);                   /* error: masked */
    if (x2_mode) kprintf("apic: x2APIC MSR id %u, PIC in virtual wire mode\n", lapic_current_id());
    else kprintf("apic: xAPIC at %p id %u, PIC in virtual wire mode\n", (void *)phys, lapic_current_id());
}

bool lapic_present(void) { return available; }
bool lapic_x2apic(void) { return available && x2_mode; }

uint32_t lapic_current_id(void) {
    if (!available) return 0;
    uint32_t id = lapic_read(0x20);
    return x2_mode ? id : id >> 24;
}

bool lapic_worker_init(void) {
    if (!available) return false;
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9)) || (x2_mode && !(c & (1u << 21)))) return false;
    uint64_t base = rdmsr(IA32_APIC_BASE);
    /* Limineの全CPU modeを採用する。APだけを切り替えてBSPと混在させない。 */
    if (!(base & APIC_BASE_ENABLE) || ((base & APIC_BASE_X2) != 0) != x2_mode) return false;
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

static bool xapic_ipi_ready(void) {
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(lapic[0x300 / 4] & (1u << 12))) return true;
        pause();
    }
    return false;
}

bool lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    /* physical broadcast宛先とspurious vectorをmailbox wakeに使わない。 */
    if (!available || vector < 32 || vector == 0xFF || apic_id == UINT32_MAX ||
        (!x2_mode && apic_id >= 255)) return false;
    uint64_t flags = irq_save();
    bool ok = true;
    if (x2_mode) {
        /* x2APIC WRMSRはserializingではない。mailboxのrelease storeが
           相手CPUへ見える前にIPIだけが到着することを防ぐ。 */
        __asm__ volatile("mfence; lfence" : : : "memory");
        wrmsr(X2APIC_ICR, ((uint64_t)apic_id << 32) | vector);
        /* x2APIC ICRにはdelivery-status bitが無く、busy pollは禁止。 */
    } else {
        ok = xapic_ipi_ready();
        if (ok) {
            /* ICR highはxAPIC専用。汎用MSR helperへ0x310を渡さない。 */
            lapic[0x310 / 4] = apic_id << 24;
            lapic[0x300 / 4] = vector; /* fixed, physical, edge-triggered */
            ok = xapic_ipi_ready();
        }
    }
    irq_restore(flags);
    return ok;
}

void lapic_eoi(void) {
    if (available) lapic_write(LAPIC_EOI, 0);
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
    while (!(inb(0x61) & 0x20) && ++spins <= 50000000) pause();
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    uint64_t tsc_ticks = rdtsc() - tsc0;
    lapic_write(LAPIC_TIMER_INIT, 0);
    outb(0x61, p61);
    if (spins > 50000000 || elapsed < 1000) return 0; /* PITなしでもtimer/portを復旧 */
    tsc_hz = tsc_ticks * 100;
    return (uint64_t)elapsed * 100;
}

/* Start the LAPIC timer as a periodic interrupt on `vector`. Returns false if that is impossible. */
bool lapic_timer_start(unsigned hz, int vector) {
    if (!available || !hz || vector < 32 || vector >= 0xFF) return false;
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
    uint64_t reload = freq / hz;
    if (!reload || reload > UINT32_MAX) return false;
    uint32_t init = (uint32_t)reload;
    timer_reload = init;
    timer_vector = vector;
    lapic_write(LAPIC_LVT_TIMER, 0x20000 | (uint32_t)vector); /* periodic, unmasked */
    lapic_write(LAPIC_TIMER_INIT, init);
    kprintf("apic: timer %lu kHz (from %s), %u Hz tick, TSC %lu kHz\n", freq / 1000, src, hz, tsc_hz / 1000);
    return true;
}

bool lapic_runner_timer_start(void) {
    if (!available || !timer_reload) return false;
    lapic_write(LAPIC_TIMER_DIV, 0xB);
    lapic_write(LAPIC_LVT_TIMER, 0x20000 | (uint32_t)timer_vector);
    lapic_write(LAPIC_TIMER_INIT, timer_reload);
    return true;
}
