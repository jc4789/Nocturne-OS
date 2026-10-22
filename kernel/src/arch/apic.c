/* Local APIC. Legacy device IRQs still come from the 8259 PIC, routed through LINT0 (virtual wire
   mode); the scheduler tick comes from the LAPIC timer, because Hyper-V Generation 2 VMs have no
   PIT. Vectors 0x30 and up are LAPIC-delivered (timer, and later Hyper-V's SynIC) and need a LAPIC EOI. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/hyperv.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "limine.h"
#include "mm/vmm.h"

#define LAPIC_ID        0x20
#define LAPIC_VERSION   0x30
#define LAPIC_EOI       0xB0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0
#define LAPIC_EXT_FEATURE 0x400 /* AMD's extended register space (APM 16.3.5, 16.3.6) */
#define LAPIC_EXT_CONTROL 0x410
#define VERSION_EAS (1u << 31)  /* AMD: the extended register space is there (APM 16.3.4) */
#define EXT_XAIDC (1u << 2)     /* AMD: the 8-bit xAPIC ID is there ... */
#define EXT_XAIDN (1u << 2)     /* ... and in use */
#define LVT_MASKED (1u << 16)
#define LVT_UNUSED (LVT_MASKED | 0xFE) /* masked, with a vector a Fixed entry may hold (lapic_setup) */
#define IA32_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2 (1ULL << 10)
#define X2APIC_MSR_BASE 0x800
#define X2APIC_ICR 0x830
#define IA32_MCG_CAP 0x179
#define MCG_CMCI_P (1ULL << 10)
#define IA32_ARCH_CAPABILITIES 0x10A
#define ARCH_CAP_XAPIC_DISABLE_STATUS (1ULL << 21)
#define IA32_XAPIC_DISABLE_STATUS 0xBD
#define LEGACY_XAPIC_DISABLED (1ULL << 0)
#define IA32_TME_ACTIVATE 0x982

static volatile uint32_t *lapic;
static uint64_t lapic_phys;      /* the xAPIC page's address (IA32_APIC_BASE), 0 in x2APIC mode */
static bool available, x2_mode;
static bool amd, cmci;           /* AMD's APIC architecture; the CMCI LVT entry is there (apic_init) */
static unsigned family;          /* CPUID.1:EAX family, the extended one added at Fh */
static uint32_t xapic_id_max = 0xFE;
static uint32_t timer_reload;
static int timer_vector;

/* x2APICではMMIO pageを読むこと自体が無効。通常32bit registerは
   offset/16 + 0x800 のMSR、64bit ICRだけは送信側で別扱いする。 */
static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2_mode) wrmsr(X2APIC_MSR_BASE + (reg >> 4), v);
    else mmio_write32(lapic + reg / 4, v);
}
static uint32_t lapic_read(uint32_t reg) {
    return x2_mode ? (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4)) : mmio_read32(lapic + reg / 4);
}

/* The firmware locked the APIC in x2APIC mode: IA32_XAPIC_DISABLE_STATUS, there where
   IA32_ARCH_CAPABILITIES says so, itself there where CPUID.(EAX=7,ECX=0):EDX[29] does. */
static bool x2apic_locked(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7) return false;
    cpuid(7, 0, &a, &b, &c, &d);
    return (d & (1u << 29)) && (rdmsr(IA32_ARCH_CAPABILITIES) & ARCH_CAP_XAPIC_DISABLE_STATUS) &&
           (rdmsr(IA32_XAPIC_DISABLE_STATUS) & LEGACY_XAPIC_DISABLED);
}

/* Takes this CPU's APIC to the mode in use. INIT keeps the mode (SDM 11.12.5, APM 16.10): an AP
   the bootloader started has its mode. Reset, and so S3, leaves Intel's enabled in xAPIC mode and
   AMD's disabled (APM 16.10), as the firmware may then change. x2APIC mode is entered from
   enabled xAPIC mode, and left only through the disabled state, EN and EXTD cleared together
   (SDM 11.12.5, APM 16.9), not where the firmware locked it. AMD's PPR for Family 19h Model 61h
   (56713, MSR0000_001B) says clearing x2ApicEn at all faults: the CPU may refuse to leave, and
   is then still in x2APIC mode. The registers but the APIC ID are not preserved across these
   transitions (SDM 11.12.5): lapic_setup writes them all. */
static bool lapic_mode_restore(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (!x2_mode && (base & APIC_BASE_X2)) {
        base &= ~(APIC_BASE_ENABLE | APIC_BASE_X2);
        if (x2apic_locked() || !wrmsr_checked(IA32_APIC_BASE, base)) return false;
    }
    if (!(base & APIC_BASE_ENABLE)) wrmsr(IA32_APIC_BASE, base |= APIC_BASE_ENABLE);
    if (x2_mode && !(base & APIC_BASE_X2)) wrmsr(IA32_APIC_BASE, base |= APIC_BASE_X2);
    return ((rdmsr(IA32_APIC_BASE) & APIC_BASE_X2) != 0) == x2_mode;
}

/* Families 0Fh, 10h and 15h: whether every node's northbridge has 8-bit APIC IDs on, ApicExtId
   (bit 18) and ApicExtBrdCst (bit 17) of F0x68, at bus 0, device 18h + node, function 0 (BKDG
   32559 4.1, 4.3.9 and 7.8.1; 31116 F0x68 and APIC20; 42300 D18F0x68). The firmware sets them;
   configuration mechanism #1 reaches them before pci_init. */
static bool northbridge_8bit_ids(void) {
    unsigned nodes = 0;
    for (uint8_t dev = 0x18; dev < 0x20; dev++) {
        if ((pci_read32(0, dev, 0, 0) & 0xFFFF) != 0x1022) continue;
        nodes++;
        if ((pci_read32(0, dev, 0, 0x68) & (3u << 17)) != (3u << 17)) return false;
    }
    return nodes != 0;
}

/* The highest APIC ID xAPIC mode addresses, FFh being its broadcast (SDM 11.6.2.1). An AMD APIC
   with 4-bit IDs uses the low four bits only (APM 16.3.6), broadcasting at xxxx1111b (31116 and
   42300 APIC410). The APM's 8-bit ID is the extended register space's (16.3.4, 16.3.5: XAIDC),
   which lapic_setup turns on (16.3.6: XAIDN). The guides for three families add the northbridge's,
   ApicExtId and ApicExtBrdCst (northbridge_8bit_ids): Family 0Fh's APIC has no ID bit of its own
   (32559 7.8.26), and the northbridge's alone makes 8-bit IDs; so it does in Family 10h, whose
   ExtApicIdEn may be set only with it (31116 APIC410); in Family 15h the IDs are 8-bit with
   ExtApicIdEn, set only with the northbridge's (42300 APIC410: "If ExtApicIdEn=1 then program
   D18F0x68[ApicExtId]=1 and D18F0x68[ApicExtBrdCst]=1"). The others follow the APM. Either
   mode. */
static uint32_t xapic_id_limit(void) {
    if (!amd) return 0xFE;
    if (family == 0x0F || family == 0x10) return northbridge_8bit_ids() ? 0xFE : 0x0E;
    bool xaidc = (lapic_read(LAPIC_VERSION) & VERSION_EAS) && (lapic_read(LAPIC_EXT_FEATURE) & EXT_XAIDC);
    if (family == 0x15) xaidc = xaidc && northbridge_8bit_ids();
    return xaidc ? 0xFE : 0x0E;
}

/* The bits of the xAPIC ID register's ID that hold it: with 4-bit IDs bits 7:4 are reserved
   (32559 7.8.1, 31116 APIC20). */
static uint32_t xapic_id_bits(void) { return xapic_id_max < 0xFE ? 0x0F : 0xFF; }

/* The 8-bit ID of AMD's extended register space is to be turned on in xAPIC mode, which INIT
   turns off (APM 16.3.2, Table 16-2: 410h after INIT; 42300 2.4.8.1.15). Not in Families 0Fh and
   10h, whose 8-bit IDs are the northbridge's. */
static bool xaidn(void) {
    return !x2_mode && amd && family != 0x0F && family != 0x10 && xapic_id_max == 0xFE;
}

/* This CPU's APIC registers, whatever they hold: the reset values after INIT, undefined ones
   after leaving x2APIC mode, else the firmware's. The task priority accepts everything. Every
   entry of the architectural LVT the APIC has is masked first (SDM 11.4.8: Max LVT Entry; CMCI
   where IA32_MCG_CAP lists it too): the error and timer ones (the timer starts by its own calls),
   LINT0 and LINT1, and the performance counter, thermal and CMCI ones the kernel does not use.
   A masked entry holds a vector all the same, one 16 or above: 0 to 15 in a Fixed entry may raise
   an illegal vector error even masked (SDM 11.5.2). AMD's extended LVT (APM 16.4.5) is left as it
   is. Then the spurious-interrupt register, written whole (SDM 11.9): vector FFh, the APIC
   enabled, and the default of the other bits, EOI broadcasts to the IOAPICs (which the
   level-triggered pins need) and focus checking. Only an enabled APIC lets a mask bit be cleared
   (SDM 11.4.7.2, APM 16.4.7): after it, LINT0 takes the PIC (ExtINT) and LINT1 the NMI on the
   BSP. On AMD the 8-bit xAPIC ID is turned on where the APIC has it (xapic_id_limit). */
static void lapic_setup(bool bsp) {
    uint32_t version = lapic_read(LAPIC_VERSION), max_lvt = (version >> 16) & 0xFF;
    lapic_write(0x80, 0);                                 /* TPR */
    lapic_write(0x370, LVT_UNUSED);                       /* error */
    lapic_write(LAPIC_LVT_TIMER, LVT_UNUSED);
    lapic_write(0x350, LVT_UNUSED);                       /* LINT0 */
    lapic_write(0x360, LVT_UNUSED);                       /* LINT1 */
    if (max_lvt >= 4) lapic_write(0x340, LVT_UNUSED);     /* performance counter */
    if (max_lvt >= 5) lapic_write(0x330, LVT_UNUSED);     /* thermal sensor */
    if (max_lvt >= 6 && cmci) lapic_write(0x2F0, LVT_UNUSED); /* corrected machine check */
    lapic_write(0xF0, 0x1FF);                             /* SVR */
    if (bsp) {
        lapic_write(0x350, 0x700);                        /* LINT0: ExtINT */
        lapic_write(0x360, 0x400);                        /* LINT1: NMI */
    }
    if (xaidn()) lapic_write(LAPIC_EXT_CONTROL, lapic_read(LAPIC_EXT_CONTROL) | EXT_XAIDN);
}

/* The bootloader enables x2APIC mode where the CPUs have it, as the kernel asks (boot.c): not
   asked, it leaves the x2APIC mode the firmware enabled, and fails the boot where it cannot
   (Limine protocol, MP feature), as where the firmware locked it or on an AMD CPU that faults on
   leaving it (lapic_mode_restore); and the firmware hands over in x2APIC mode where an APIC ID
   needs it (SDM 11.12.7). That is x2APIC mode before interrupt remapping with Extended Interrupt
   Mode, the order 11.12.7 asks of software, which the SDM makes no exception to: a known
   deviation, which the bootloader's choice leaves the kernel. No device interrupt reaches a CPU
   before iommu_init has interrupt remapping on: the PIC and the IOAPICs are masked (boot.c), and
   MSIs wait for the drivers. After sleep the kernel keeps the order (dev/pm.c). The
   IOMMU may keep the CPUs out of x2APIC mode (iommu_x2apic_conflict: the firmware asks for that,
   or a unit remaps interrupts in xAPIC format only). It is left where every CPU has an APIC ID
   xAPIC mode can address (xapic_id_limit) and the firmware has not locked it. In x2APIC mode
   now: whether to leave it. */
static bool x2apic_leave(const struct limine_mp_response *mp) {
    const char *conflict = iommu_x2apic_conflict();
    if (!conflict) return false;
    uint32_t max = xapic_id_limit();
    const char *why = x2apic_locked() ? "the firmware locked x2APIC mode" : NULL;
    if (!why && lapic_read(LAPIC_ID) > max) why = "this CPU's APIC ID needs it";
    for (uint64_t i = 0; !why && mp && i < mp->cpu_count; i++)
        if (mp->cpus[i] && mp->cpus[i]->lapic_id > max) why = "a CPU's APIC ID needs it";
    if (why) kprintf("apic: x2APIC mode kept although %s, as %s\n", conflict, why);
    else kprintf("apic: leaving x2APIC mode for xAPIC mode, as %s\n", conflict);
    return !why;
}

/* The xAPIC page's address in IA32_APIC_BASE: bits MAXAPICADDR-1:12, MAXAPICADDR being the
   physical-address width CPUID.80000008H:EAX[7:0], 36 without that leaf (SDM 13.4.4 of the 093
   edition). AMD's field, bits 51:12 where the CPU implements them all (APM 16.3.1), ends at the
   width it does implement, PhysAddrSize in that leaf (as APM 7.6.6 sizes such MSR fields; PPR
   56713 MSR0000_001B: 47:12 at its 48 bits, 63:48 reserved).
   On Intel the width is less IA32_TME_ACTIVATE[35:32], the KeyID bits, where [39:36] > 0, TDX
   KeyIDs configured (SDM Vol. 4 Table 2-2: the MSR is there where CPUID.(EAX=7,ECX=0):ECX[13]
   says, a bit AMD reserves). A width below 32, which cannot hold the reset address FEE00000h, is
   no CPU's: the 36 bits of a CPU without the leaf then. */
static uint64_t xapic_base(uint64_t base, uint32_t max_leaf) {
    uint32_t a, b, c, d;
    unsigned bits = 36;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        if ((a & 0xFF) >= 32) bits = MIN(a & 0xFF, 52u);
    }
    if (!amd && max_leaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        uint64_t tme = c & (1u << 13) ? rdmsr(IA32_TME_ACTIVATE) : 0;
        if ((tme >> 36) & 0xF) bits -= (tme >> 32) & 0xF;
    }
    return base & ((1ULL << bits) - 1) & ~0xFFFULL;
}

/* mp: the bootloader's description of the CPUs, or NULL. */
void apic_init(const struct limine_mp_response *mp) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    amd = cpu_vendor_of(b, c, d) == CPU_VENDOR_AMD;
    uint32_t max_leaf = a;
    cpuid(1, 0, &a, &b, &c, &d);
    family = (a >> 8) & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (!(d & (1 << 9))) {
        kprintf("apic: no local APIC, using plain PIC\n");
        return;
    }
    /* IA32_MCG_CAP is read where the CPU has the machine-check exception and architecture
       (CPUID.1:EDX[7], EDX[14]; SDM Vol. 3B 18.8); MCG_CMCI_P is Intel's, the bit reserved on AMD
       (APM 9.3.1.1), whose APIC has no such entry. */
    cmci = !amd && (d & (1u << 7)) && (d & (1u << 14)) && (rdmsr(IA32_MCG_CAP) & MCG_CMCI_P);
    uint64_t base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        kprintf("apic: local APIC globally disabled, using plain PIC\n");
        return;
    }
    x2_mode = (base & APIC_BASE_X2) != 0;
    if (x2_mode && !(c & (1u << 21))) panic("apic: x2APIC enabled without CPU support");
    if (x2_mode && x2apic_leave(mp)) {
        x2_mode = false;
        if (!lapic_mode_restore()) {
            if ((rdmsr(IA32_APIC_BASE) & (APIC_BASE_ENABLE | APIC_BASE_X2)) != (APIC_BASE_ENABLE | APIC_BASE_X2))
                panic("apic: cannot leave x2APIC mode");
            x2_mode = true;
            kprintf("apic: x2APIC mode kept, as this CPU refused to leave it\n");
        }
    }
    uint64_t phys = 0;
    if (!x2_mode) {
        phys = xapic_base(base, max_leaf);
        lapic = vmm_map_mmio(phys, 4096);
        if (!lapic) panic("apic: cannot map local APIC");
        lapic_phys = phys;
    }
    available = true;
    xapic_id_max = xapic_id_limit();
    lapic_setup(true);
    if (x2_mode) kprintf("apic: x2APIC MSR id %u, PIC in virtual wire mode\n", lapic_current_id());
    else kprintf("apic: xAPIC at %p id %u%s, PIC in virtual wire mode\n", (void *)phys, lapic_current_id(),
                 xapic_id_max < 0xFE ? " (4-bit IDs)" : "");
}

bool lapic_present(void) { return available; }
bool lapic_x2apic(void) { return available && x2_mode; }

bool lapic_addressable(uint32_t apic_id) {
    return available && (x2_mode ? apic_id != UINT32_MAX : apic_id <= xapic_id_max);
}

uint32_t lapic_current_id(void) {
    if (!available) return 0;
    uint32_t id = lapic_read(LAPIC_ID);
    return x2_mode ? id : (id >> 24) & xapic_id_bits();
}

/* The BSP after S3: the same registers as apic_init, and the tick with its calibration. */
void lapic_resume(void) {
    if (!available) return;
    if (!lapic_mode_restore()) panic("apic: cannot return to %s mode after sleep", x2_mode ? "x2APIC" : "xAPIC");
    lapic_setup(true);
    if (timer_reload) {
        lapic_write(LAPIC_TIMER_DIV, 0xB);
        lapic_tick_resume();
    }
}

static bool xapic_ipi_ready(void);

static void spin_us(unsigned us) {
    uint64_t t0 = rdtsc(), n = tsc_hz / 1000000 * us;
    while (rdtsc() - t0 < n) pause();
}

/* One INIT or STARTUP, then the wait that follows it: 10 ms after an INIT, 200 us after a
   STARTUP (SDM 9.4.4.1, Table 9-1). */
static bool lapic_send_start(uint32_t apic_id, uint32_t cmd, unsigned wait_us) {
    if (!lapic_addressable(apic_id) || tsc_hz < 1000000) return false;
    if (x2_mode) {
        wrmsr(X2APIC_ICR, ((uint64_t)apic_id << 32) | cmd);
    } else {
        if (!xapic_ipi_ready()) return false;
        mmio_write32(lapic + 0x310 / 4, apic_id << 24);
        mmio_write32(lapic + 0x300 / 4, cmd);
        if (!xapic_ipi_ready()) return false;
    }
    spin_us(wait_us);
    return true;
}

bool lapic_send_init(uint32_t apic_id) { return lapic_send_start(apic_id, 0x4500, 10000); }

/* STARTUP twice, the second ignored by a CPU that already runs. */
bool lapic_send_startup(uint32_t apic_id, uint64_t page) {
    if (page & 0xFFF || page >= 0x100000) return false;
    uint32_t cmd = 0x4600 | (uint32_t)(page >> 12);
    return lapic_send_start(apic_id, cmd, 200) && lapic_send_start(apic_id, cmd, 200);
}

/* In xAPIC mode IA32_APIC_BASE's address bits (xapic_base) and EN. */
void lapic_ident(struct lapic_ident *out) {
    *out = (struct lapic_ident){.x2 = x2_mode};
    if (!available || x2_mode) return;
    uint32_t max_leaf, b, c, d;
    cpuid(0, 0, &max_leaf, &b, &c, &d);
    out->id_reg = lapic + LAPIC_ID / 4;
    out->id_mask = xapic_id_bits();
    out->base = lapic_phys | APIC_BASE_ENABLE;
    out->base_mask = xapic_base(~0ULL, max_leaf) | APIC_BASE_ENABLE;
    out->xaidn = xaidn();
}

bool lapic_worker_init(void) {
    if (!available) return false;
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (x2_mode && !(c & (1u << 21))) return false;
    /* BSPのmode（Limineの選択、またはIOMMUの要件でapic_initが戻したxAPIC）に揃える。
       無効状態のAPICではCPUID.1:EDX[9]も0になる（SDM 11.4.3）ため、有無ではなく遷移の結果で判断する。 */
    if (!lapic_mode_restore()) return false;
    lapic_setup(false); /* PIC と NMI は BSP 専用 */
    return true;
}

static bool xapic_ipi_ready(void) {
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(mmio_read32(lapic + 0x300 / 4) & (1u << 12))) return true;
        pause();
    }
    return false;
}

bool lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    /* physical broadcast宛先とspurious vectorをmailbox wakeに使わない。 */
    if (!lapic_addressable(apic_id) || vector < 32 || vector == 0xFF) return false;
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
            mmio_write32(lapic + 0x310 / 4, apic_id << 24);
            mmio_write32(lapic + 0x300 / 4, vector); /* fixed, physical, edge-triggered */
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

/* Tickless idle. An initial count of 0 stops the timer in either mode; one-shot mode counts
   down once and stops. The CPU's own LAPIC only, with interrupts off. */
void lapic_tick_stop(void) { lapic_write(LAPIC_TIMER_INIT, 0); }

void lapic_tick_once(unsigned ms) {
    if (ms > UINT32_MAX / timer_reload) ms = UINT32_MAX / timer_reload;
    lapic_write(LAPIC_LVT_TIMER, (uint32_t)timer_vector);
    lapic_write(LAPIC_TIMER_INIT, ms * timer_reload);
}

void lapic_tick_resume(void) {
    lapic_write(LAPIC_LVT_TIMER, 0x20000 | (uint32_t)timer_vector);
    lapic_write(LAPIC_TIMER_INIT, timer_reload);
}
