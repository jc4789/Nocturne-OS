/* What the CPUs are, and the state each one sets itself up in.

   The leaves that name features are read here once, on the first CPU, before anything uses them;
   every other CPU is checked against them as it starts. Nocturne requires the set below of every
   CPU (x86-64-v4, the system features the kernel builds on, and nothing older: the floor is Zen 4)
   and stops at boot when one is missing. The optional features are the ones a hypervisor or a
   model may lack (WHPX shows no PCID, MWAIT or invariant TSC bit): their users choose a path by
   them. Of those, what Nocturne needs of the hardware itself (an invariant TSC) is required
   where no hypervisor shows itself. Leaf 0xD (the XSAVE layout) is arch/fpu.c's, the topology
   leaves (0xB, 0x1F, 4, 0x8000001D) arch/topology.c's, the hypervisor's leaves from 0x40000000
   vm/'s.

   What the CPU states of its TSC's rate is read here too, with the leaves: on the hardware the
   P0 P-state's definition (tsc_p0), which the clock starts from (sys/clock.c).

   cpu_setup is the one way a CPU gets into the state the kernel runs it in: CPU 0 at boot, each
   other CPU as it starts, CPU 0 again after S3. Nothing is copied back from before a sleep. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/fpu.h"
#include "arch/gdt.h"
#include "arch/idle.h"
#include "arch/irq.h"
#include "arch/pat.h"
#include "arch/smp.h"
#include "arch/spin.h"

struct cpu_features cpu_features;

enum { EAX, EBX, ECX, EDX };

/* The leaves whose bits name features; a CPU reads them all at once. */
static const struct { uint32_t leaf, sub; } leaves[] = {
    {1, 0}, {6, 0}, {7, 0}, {0xD, 1}, {0x80000001, 0}, {0x80000007, 0}, {0x80000008, 0}, {0x80000021, 0},
};
enum { L1, L6, L7, LD1, LE1, LE7, LE8, LE21, LEAVES };
_Static_assert(LEAVES == ARRAY_SIZE(leaves), "the leaf names follow the table");
typedef uint32_t leaf_regs[LEAVES][4];

static const struct feature {
    uint8_t leaf, reg, bit;
    const char *name;
} required[] = {
    {L1, EDX, 0, "FPU"}, {L1, EDX, 4, "TSC"}, {L1, EDX, 5, "MSR"}, {L1, EDX, 6, "PAE"},
    {L1, EDX, 7, "MCE"}, {L1, EDX, 8, "CX8"}, {L1, EDX, 9, "APIC"}, {L1, EDX, 12, "MTRR"},
    {L1, EDX, 13, "PGE"}, {L1, EDX, 14, "MCA"}, {L1, EDX, 15, "CMOV"}, {L1, EDX, 16, "PAT"},
    {L1, EDX, 19, "CLFSH"}, {L1, EDX, 24, "FXSR"}, {L1, EDX, 25, "SSE"}, {L1, EDX, 26, "SSE2"},
    {L1, ECX, 0, "SSE3"}, {L1, ECX, 1, "PCLMULQDQ"}, {L1, ECX, 9, "SSSE3"}, {L1, ECX, 12, "FMA"},
    {L1, ECX, 13, "CX16"}, {L1, ECX, 19, "SSE4.1"}, {L1, ECX, 20, "SSE4.2"}, {L1, ECX, 21, "x2APIC"},
    {L1, ECX, 22, "MOVBE"}, {L1, ECX, 23, "POPCNT"}, {L1, ECX, 25, "AES"}, {L1, ECX, 26, "XSAVE"},
    {L1, ECX, 28, "AVX"}, {L1, ECX, 29, "F16C"}, {L1, ECX, 30, "RDRAND"},
    {L7, EBX, 0, "FSGSBASE"}, {L7, EBX, 3, "BMI1"}, {L7, EBX, 5, "AVX2"}, {L7, EBX, 7, "SMEP"},
    {L7, EBX, 8, "BMI2"}, {L7, EBX, 9, "ERMS"}, {L7, EBX, 10, "INVPCID"}, {L7, EBX, 16, "AVX512F"},
    {L7, EBX, 17, "AVX512DQ"}, {L7, EBX, 18, "RDSEED"}, {L7, EBX, 19, "ADX"}, {L7, EBX, 20, "SMAP"},
    {L7, EBX, 23, "CLFLUSHOPT"}, {L7, EBX, 24, "CLWB"}, {L7, EBX, 28, "AVX512CD"},
    {L7, EBX, 30, "AVX512BW"}, {L7, EBX, 31, "AVX512VL"}, {L7, ECX, 22, "RDPID"},
    {LD1, EAX, 1, "XSAVEC"}, {LD1, EAX, 3, "XSAVES"},
    {LE1, ECX, 0, "LAHF"}, {LE1, ECX, 5, "LZCNT"}, {LE1, ECX, 8, "PREFETCHW"},
    {LE1, EDX, 11, "SYSCALL"}, {LE1, EDX, 20, "NX"}, {LE1, EDX, 26, "1 GiB pages"},
    {LE1, EDX, 27, "RDTSCP"}, {LE1, EDX, 29, "long mode"},
};

/* The optional bits the kernel reads, each into its flag (two bits may name one flag, Intel's and
   AMD's); another CPU has to show each the same. HARDWARE ones are required where no hypervisor
   shows itself: a hypervisor may hide them (WHPX hides the invariant TSC), the hardware may not. */
#define OPTIONAL(l, r, b, n, flag) {{l, r, b, n}, __builtin_offsetof(struct cpu_features, flag), false}
#define HARDWARE(l, r, b, n, flag) {{l, r, b, n}, __builtin_offsetof(struct cpu_features, flag), true}
static const struct {
    struct feature f;
    size_t flag;
    bool hardware;
} optional[] = {
    OPTIONAL(L1, ECX, 3, "MWAIT", mwait), OPTIONAL(L1, ECX, 17, "PCID", pcid),
    OPTIONAL(L1, ECX, 24, "TSC deadline", tsc_deadline), OPTIONAL(L1, ECX, 31, "hypervisor", hypervisor),
    OPTIONAL(L6, EAX, 2, "ARAT", arat), OPTIONAL(L6, ECX, 0, "APERF/MPERF", aperf_mperf),
    OPTIONAL(L7, ECX, 2, "UMIP", umip),
    OPTIONAL(L7, ECX, 13, "TME", tme), OPTIONAL(L7, EDX, 26, "IBRS and IBPB", ibpb),
    OPTIONAL(L7, EDX, 29, "IA32_ARCH_CAPABILITIES", arch_capabilities),
    OPTIONAL(LE1, ECX, 22, "topology extensions", topology_extensions),
    OPTIONAL(LE7, EBX, 3, "scalable MCA", scalable_mca), HARDWARE(LE7, EDX, 8, "invariant TSC", invariant_tsc),
    OPTIONAL(LE8, EBX, 3, "INVLPGB", invlpgb), OPTIONAL(LE8, EBX, 12, "IBPB", ibpb),
    OPTIONAL(LE8, EBX, 14, "IBRS", ibrs), OPTIONAL(LE8, EBX, 15, "STIBP", stibp),
    OPTIONAL(LE8, EBX, 16, "IBRS always on", ibrs_always_on),
    OPTIONAL(LE8, EBX, 17, "STIBP always on", stibp_always_on), OPTIONAL(LE8, EBX, 27, "CPPC", cppc),
    OPTIONAL(LE21, EAX, 2, "LFENCE serializing", lfence_serializing),
    OPTIONAL(LE21, EAX, 8, "automatic IBRS", auto_ibrs),
};

static bool has(leaf_regs r, const struct feature *f) { return r[f->leaf][f->reg] & (1u << f->bit); }

static void read_leaves(leaf_regs r) {
    uint32_t max, ext, b, c, d;
    cpuid(0, 0, &max, &b, &c, &d);
    cpuid(0x80000000, 0, &ext, &b, &c, &d);
    for (unsigned i = 0; i < LEAVES; i++) {
        uint32_t leaf = leaves[i].leaf;
        if (leaf <= (leaf >= 0x80000000 ? ext : max)) cpuid(leaf, leaves[i].sub, &r[i][EAX], &r[i][EBX], &r[i][ECX], &r[i][EDX]);
        else memset(r[i], 0, sizeof r[i]);
    }
}

/* The vendor leaf 0 names in EBX, EDX, ECX ("GenuineIntel"; "AuthenticAMD", and "HygonGenuine",
   which follows AMD's manuals). */
static enum cpu_vendor vendor_of(uint32_t b, uint32_t c, uint32_t d) {
    if (b == 0x756E6547 && d == 0x49656E69 && c == 0x6C65746E) return CPU_VENDOR_INTEL;
    if ((b == 0x68747541 && d == 0x69746E65 && c == 0x444D4163) || (b == 0x6F677948 && d == 0x6E65476E && c == 0x656E6975))
        return CPU_VENDOR_AMD;
    return CPU_VENDOR_OTHER;
}

static void read_brand(char *out) {
    uint32_t a, b, c, d, *o = (uint32_t *)out;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) {
        cpuid(0, 0, &a, &o[0], &o[2], &o[1]);
        out[12] = 0;
        return;
    }
    for (uint32_t i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &o[i * 4], &o[i * 4 + 1], &o[i * 4 + 2], &o[i * 4 + 3]);
    out[48] = 0;
    char *p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    for (size_t n = strlen(out); n && out[n - 1] == ' '; n--) out[n - 1] = 0;
}

/* ---- the TSC's rate ----
   On the hardware the TSC counts at the P0 P-state's frequency (PPR 56713 for Family 19h Model
   61h, 2.1.9; HWCR[TscFreqSel], MSR C001_0015 bit 24, read-only: "1=The TSC increments at the P0
   frequency"). PStateDef[0] (MSR C001_0064) defines P0: PstateEn (bit 63) says the definition is
   valid; CpuFid (bits 7:0, 10h-FFh) times 25 MHz is the VCO; CpuDfsId (bits 13:8) divides it by
   DID/8, its valid values 08h-1Ah and the even ones 1Ch-2Ch (the rest reserved, 00h off). So the
   TSC counts 200 MHz * FID / DID a second, the glossary's CoreCOF; the register's own line "The core
   COF is defined as CpuFid * 5MHz" reaches 1275 MHz at most and agrees with neither table.
   PStateDef's frequency fields are the same on every core of the coherent fabric (PPR,
   PStateDef), and HWCR[LockTscToCurrentP0] 0 keeps the TSC at P0's frequency, which nothing here
   changes; each CPU is checked as it starts (cpu_features_same). The rate is nominal: spread
   spectrum lowers it (the glossary), and its crystal is off by its tolerance; the clock measures
   what the TSC really counts (sys/clock.c). A hypervisor's guest reads nothing of its own TSC
   here (a TSC ratio may scale it): vm/ registers that rate. They are read before the IDT is up,
   where a fault would end the boot with nothing said; every AMD CPU with what Nocturne requires
   (AVX-512: Zen 4 on) has both registers. */
#define MSR_HWCR 0xC0010015u
#define HWCR_TSC_FREQ_SEL (1ull << 24)
#define MSR_PSTATE_DEF0 0xC0010064u
#define PSTATE_EN (1ull << 63)
#define PSTATE_FID(v) ((unsigned)(v) & 0xFF)
#define PSTATE_DID(v) ((unsigned)((v) >> 8) & 0x3F)

static bool did_valid(unsigned did) { return (did >= 0x08 && did <= 0x1A) || (did >= 0x1C && did <= 0x2C && !(did & 1)); }

/* This CPU's P0 IDs; NULL, or why there is no rate in them. */
static const char *tsc_p0(unsigned *fid, unsigned *did) {
    if (cpu_features.vendor != CPU_VENDOR_AMD) return "only an AMD CPU's P0 P-state states its TSC's rate";
    if (!(rdmsr(MSR_HWCR) & HWCR_TSC_FREQ_SEL)) return "HWCR[TscFreqSel] is 0: the TSC does not count at P0";
    uint64_t def = rdmsr(MSR_PSTATE_DEF0);
    *fid = PSTATE_FID(def);
    *did = PSTATE_DID(def);
    if (!(def & PSTATE_EN)) return "PStateDef[0] is not valid (PstateEn 0)";
    if (*fid < 0x10 || !did_valid(*did)) return "PStateDef[0]'s FID or DID is reserved";
    return NULL;
}

static leaf_regs first;

void cpu_features_init(void) {
    struct cpu_features *f = &cpu_features;
    read_leaves(first);
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    f->vendor = vendor_of(b, c, d);
    read_brand(f->brand);
    for (unsigned i = 0; i < LEAVES; i++)
        kprintf("cpu: cpuid %x.%u eax %08x ebx %08x ecx %08x edx %08x\n", leaves[i].leaf, leaves[i].sub,
                first[i][EAX], first[i][EBX], first[i][ECX], first[i][EDX]);
    for (unsigned i = 0; i < ARRAY_SIZE(optional); i++)
        *(bool *)((char *)f + optional[i].flag) |= has(first, &optional[i].f);

    char missing[256];
    missing[0] = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(required); i++)
        if (!has(first, &required[i])) {
            if (missing[0]) strlcat(missing, ", ", sizeof missing);
            strlcat(missing, required[i].name, sizeof missing);
        }
    for (unsigned i = 0; i < ARRAY_SIZE(optional); i++)
        if (optional[i].hardware && !f->hypervisor && !has(first, &optional[i].f)) {
            if (missing[0]) strlcat(missing, ", ", sizeof missing);
            strlcat(missing, optional[i].f.name, sizeof missing);
        }
    if (missing[0]) panic("cpu: %s lacks what Nocturne requires: %s", f->brand, missing);
    if (!f->hypervisor) {
        const char *why = tsc_p0(&f->p0_fid, &f->p0_did);
        if (why) panic("cpu: %s states no TSC rate Nocturne can use: %s", f->brand, why);
    }

    a = first[L1][EAX];
    f->family = (a >> 8) & 0xF;
    f->model = (a >> 4) & 0xF;
    if (f->family == 0xF) {
        f->family += (a >> 20) & 0xFF;
        f->model |= ((a >> 16) & 0xF) << 4;
    }
    f->stepping = a & 0xF;
    f->clflush_bytes = ((first[L1][EBX] >> 8) & 0xFF) * 8;
    f->phys_bits = first[LE8][EAX] & 0xFF;
    f->invlpgb_pages = first[LE8][EDX] & 0xFFFF;
    char present[384];
    present[0] = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(optional); i++)
        if (has(first, &optional[i].f)) {
            if (present[0]) strlcat(present, ", ", sizeof present);
            strlcat(present, optional[i].f.name, sizeof present);
        }
    kprintf("cpu: %s, family %xh model %xh stepping %u, %u-bit physical addresses; has %s\n", f->brand,
            f->family, f->model, f->stepping, f->phys_bits, present[0] ? present : "none of the optional features");
}

bool cpu_features_same(void) {
    leaf_regs here;
    read_leaves(here);
    for (unsigned i = 0; i < ARRAY_SIZE(required); i++)
        if (!has(here, &required[i])) return false;
    for (unsigned i = 0; i < ARRAY_SIZE(optional); i++)
        if (has(here, &optional[i].f) != has(first, &optional[i].f)) return false;
    unsigned fid, did;
    if (!cpu_features.hypervisor && (tsc_p0(&fid, &did) || fid != cpu_features.p0_fid || did != cpu_features.p0_did))
        return false; /* its TSC would count at another rate than the clock's */
    return true;
}

/* ---- machine checks ----
   The banks (AMD APM vol. 2 9.3): MCG_CAP gives their count (bits 7:0) and whether MCG_CTL is
   there (bit 8). A bank's registers are at 400h + 4i (CTL, STATUS, ADDR, MISC0), which reach banks
   0 to 31 (400h-47Fh; 9.3.2, as Intel SDM vol. 3B's IA32_MCi_CTL); with scalable MCA (CPUID
   8000_0007h EBX[3]) every bank has its own block at C000_2000h + 10h * i, up to 64 banks (9.3.3,
   Figure 9-10), the first four registers the same and MCA_CONFIG fifth. */
#define MSR_MCG_CAP    0x179u
#define MSR_MCG_STATUS 0x17Au
#define MSR_MCG_CTL    0x17Bu
#define MCG_CTL_P      (1ULL << 8)
#define MCG_RIPV       (1ULL << 0) /* the interrupted code may go on at the saved rip */
#define MCI_VAL   (1ULL << 63)
#define MCI_OVER  (1ULL << 62)
#define MCI_UC    (1ULL << 61)
#define MCI_MISCV (1ULL << 59)
#define MCI_ADDRV (1ULL << 58)
#define MCI_PCC   (1ULL << 57)
#define MISC_VAL   (1ULL << 63) /* the thresholding format of MCi_MISC0 (9.3.2.6, Figure 9-9) */
#define MISC_CNTP  (1ULL << 62)
#define MISC_LKD   (1ULL << 61)
#define MISC_INTP  (1ULL << 60)
#define MISC_LVTOFF(m) ((unsigned)((m) >> 52) & 0xF)
#define MISC_CNTE  (1ULL << 51)
#define MISC_INTT  (3ULL << 49)
#define MISC_INTT_APIC (1ULL << 49)
#define MISC_OF    (1ULL << 48)
#define MISC_ERRCT_SHIFT 32
#define MISC_ERRCT (0xFFFFULL << MISC_ERRCT_SHIFT)
#define CONFIG_MCAX   (1ULL << 0)  /* MCA_CONFIG (9.3.3.1) */
#define CONFIG_MCAXEN (1ULL << 32)
#define CONFIG_INTEN  (1ULL << 40)
#define LEGACY_BANKS 32u
#define SCALABLE_BANKS 64u
/* The corrected errors a legacy bank's counter takes before it interrupts: one, so that each is
   told as it happens (APM 9.3.2.6: starting count = threshold level - this). */
#define THRESHOLD_ERRORS 1u

enum { MCA_CTL, MCA_STATUS, MCA_ADDR, MCA_MISC0, MCA_CONFIG };

static uint32_t mca_msr(unsigned bank, unsigned reg) {
    return cpu_features.scalable_mca ? 0xC0002000u + 0x10u * bank + reg : 0x400u + 4u * bank + reg;
}

static unsigned mca_banks(void) {
    unsigned count = (unsigned)(rdmsr(MSR_MCG_CAP) & 0xFF);
    return MIN(count, cpu_features.scalable_mca ? SCALABLE_BANKS : LEGACY_BANKS);
}

static void mca_report(unsigned bank, uint64_t status, const char *what) {
    uint64_t addr = status & MCI_ADDRV ? rdmsr(mca_msr(bank, MCA_ADDR)) : 0;
    uint64_t misc = status & MCI_MISCV ? rdmsr(mca_msr(bank, MCA_MISC0)) : 0;
    kprintf("cpu%u: machine check bank %u %s: status %016lx%s%s, address %lx, misc %lx\n",
            cpu_current_index(), bank, what, status, status & MCI_UC ? " uncorrected" : " corrected",
            status & MCI_OVER ? ", errors lost" : "", addr, misc);
}

/* APM 9.4: every bank's reporting on, MCG_CTL's too, what the banks hold from before logged and
   cleared, CR4.MCE last. Banks with scalable MCA are told the kernel knows its registers
   (McaxEn): without it an uncorrected error is a system fatal event with no #MC to log it
   (9.3.3.1). A bank the CPU (or a hypervisor) refuses a write to is left as it is. */
static void mca_setup(void) {
    unsigned banks = mca_banks();
    for (unsigned i = 0; i < banks; i++) {
        if (cpu_features.scalable_mca) {
            uint64_t config = rdmsr(mca_msr(i, MCA_CONFIG));
            if (config & CONFIG_MCAX) wrmsr_checked(mca_msr(i, MCA_CONFIG), config | CONFIG_MCAXEN);
        }
        wrmsr_checked(mca_msr(i, MCA_CTL), ~0ULL);
        uint64_t status = rdmsr(mca_msr(i, MCA_STATUS));
        if (!(status & MCI_VAL)) continue;
        mca_report(i, status, "from before");
        wrmsr_checked(mca_msr(i, MCA_STATUS), 0);
    }
    if (rdmsr(MSR_MCG_CAP) & MCG_CTL_P) wrmsr_checked(MSR_MCG_CTL, ~0ULL);
    write_cr4(read_cr4() | CR4_MCE);
}

/* The banks' corrected errors (AMD): each bank whose counter has an interrupt (MISC0: VAL, CNTP,
   IntP, not LKD, 9.3.2.6) signals it through one entry of the APIC's extended LVT (16.4.5), the
   offset the firmware set (LVTOFF, the same for every bank: an alias of MCA_INTR_CFG's, 9.3.1.4),
   on the system vector VEC_MCE_THRESHOLD. With scalable MCA a bank interrupts on each corrected
   error through MCA_CONFIG's IntEn instead (9.3.3.1). */
static struct irq *threshold_irq;
static uint16_t threshold_start[LEGACY_BANKS]; /* the count a legacy bank's counter starts from */

static bool threshold_bank(uint64_t misc) {
    return (misc & (MISC_VAL | MISC_CNTP | MISC_INTP | MISC_LKD)) == (MISC_VAL | MISC_CNTP | MISC_INTP);
}

static enum irq_result mca_threshold(void *context) {
    (void)context;
    unsigned banks = mca_banks();
    for (unsigned i = 0; i < banks; i++) {
        uint64_t status = rdmsr(mca_msr(i, MCA_STATUS));
        if ((status & MCI_VAL) && !(status & (MCI_UC | MCI_PCC))) {
            mca_report(i, status, "told");
            wrmsr_checked(mca_msr(i, MCA_STATUS), 0);
        }
        if (cpu_features.scalable_mca) continue;
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (threshold_bank(misc) && (misc & MISC_OF))
            wrmsr_checked(mca_msr(i, MCA_MISC0),
                          (misc & ~(MISC_OF | MISC_ERRCT)) | (uint64_t)threshold_start[i] << MISC_ERRCT_SHIFT);
    }
    return IRQ_HANDLED;
}

/* x2APIC registers (APM 16.11.1, Table 16-6): the version (EAS, bit 31: the extended space is
   there), the extended feature register (XLC, bits 23:16: its LVT entries) and those entries. */
#define X2APIC_VERSION     0x803u
#define X2APIC_EXT_FEATURE 0x840u
#define X2APIC_EXT_LVT     0x850u

void cpu_mca_interrupt_on(void) {
    if (cpu_features.vendor != CPU_VENDOR_AMD) return;
    unsigned banks = mca_banks(), offset = 16;
    for (unsigned i = 0; i < banks && offset == 16; i++) {
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (threshold_bank(misc)) offset = MISC_LVTOFF(misc);
    }
    if (offset == 16) return; /* no bank interrupts */
    if (!(rdmsr(X2APIC_VERSION) & (1ULL << 31)) || offset >= ((rdmsr(X2APIC_EXT_FEATURE) >> 16) & 0xFF)) {
        if (!cpu_current_index()) kprintf("cpu: machine-check thresholds name LVT entry %u, which the APIC lacks\n", offset);
        return;
    }
    if (!threshold_irq && /* the first CPU, at boot */
        !(threshold_irq = irq_request_system(
              &(struct irq_spec){.name = "machine-check threshold", .handler = mca_threshold, .node = -1},
              VEC_MCE_THRESHOLD)))
        panic("cpu: VEC_MCE_THRESHOLD is taken");
    /* the entry first: a counter enabled before it may interrupt through anything (9.3.2.6) */
    wrmsr(X2APIC_EXT_LVT + offset, VEC_MCE_THRESHOLD); /* fixed, unmasked */
    unsigned told = 0;
    for (unsigned i = 0; i < banks; i++) {
        uint64_t misc = rdmsr(mca_msr(i, MCA_MISC0));
        if (!threshold_bank(misc)) continue;
        if (cpu_features.scalable_mca) {
            uint64_t config = rdmsr(mca_msr(i, MCA_CONFIG));
            if (wrmsr_checked(mca_msr(i, MCA_CONFIG), config | CONFIG_INTEN)) told++;
            continue;
        }
        /* the counter's width: its implemented bits read back as written (9.3.2.6) */
        misc &= ~(MISC_CNTE | MISC_INTT | MISC_OF | MISC_ERRCT);
        if (!wrmsr_checked(mca_msr(i, MCA_MISC0), misc | MISC_ERRCT)) continue;
        uint16_t level = (uint16_t)(rdmsr(mca_msr(i, MCA_MISC0)) >> MISC_ERRCT_SHIFT);
        if (level < THRESHOLD_ERRORS) continue;
        threshold_start[i] = (uint16_t)(level - THRESHOLD_ERRORS);
        if (wrmsr_checked(mca_msr(i, MCA_MISC0), misc | MISC_CNTE | MISC_INTT_APIC |
                                                     (uint64_t)threshold_start[i] << MISC_ERRCT_SHIFT))
            told++;
    }
    if (!cpu_current_index())
        kprintf("cpu: corrected machine checks of %u banks interrupt on vector %u (LVT entry %u)\n", told,
                VEC_MCE_THRESHOLD, offset);
}

void cpu_machine_check(struct regs *r) {
    uint64_t mcg = rdmsr(MSR_MCG_STATUS);
    bool fatal = !(mcg & MCG_RIPV), logged = spin_none_here();
    unsigned banks = mca_banks(), uncorrected = 0;
    for (unsigned i = 0; i < banks; i++) {
        uint64_t status = rdmsr(mca_msr(i, MCA_STATUS));
        if (!(status & MCI_VAL)) continue;
        bool bad = (status & (MCI_UC | MCI_PCC)) != 0;
        if (bad) uncorrected++;
        if (bad || logged) mca_report(i, status, "in a machine check");
        if (!bad) wrmsr_checked(mca_msr(i, MCA_STATUS), 0);
    }
    if (fatal || uncorrected)
        panic("cpu%u: machine check at %lx: %u uncorrected errors%s", cpu_current_index(), r->rip, uncorrected,
              fatal ? ", and the interrupted code cannot go on" : "");
    wrmsr(MSR_MCG_STATUS, 0); /* MCIP: another machine check may come again */
}

/* ---- the state of each CPU ---- */
/* Speculation controls, as APM vol. 2 3.2.9 and 3.1.7 give them. Automatic IBRS keeps what the
   app trained out of the kernel's indirect branches without a write on each entry; without it,
   IBRS on a CPU that prefers it set once at boot ("IBRS always on"). STIBP where the CPU prefers it
   set once, so that apps on sibling threads do not train each other's. The kernel's half and the
   app's are apart by more than a page and SMEP is on, so return predictions need no clearing
   (3.2.9). IBPB between apps is cpu_ibpb's. */
static void speculation_setup(void) {
    if (cpu_features.auto_ibrs) wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_AIBRSE);
    uint64_t spec = 0;
    if (!cpu_features.auto_ibrs && cpu_features.ibrs && cpu_features.ibrs_always_on) spec |= SPEC_CTRL_IBRS;
    if (cpu_features.stibp && cpu_features.stibp_always_on) spec |= SPEC_CTRL_STIBP;
    if (spec) wrmsr(MSR_SPEC_CTRL, rdmsr(MSR_SPEC_CTRL) | spec);
}

static bool said; /* the first CPU's lines, once */

/* CR0: protected mode and paging as the bootloader set them, write protection in the kernel too
   (read-only pages really are), x87 errors as exceptions (NE) and WAIT checking TS (MP), no x87
   emulation and no task-switched trap, caching on (CD and NW clear). CR4: global pages, which a
   CR3 load keeps (the kernel half's leaves, mm/vmm.c); FS and GS bases the app writes itself
   (FSGSBASE: a task switch keeps them, sys/sched.c); the app's pages neither run nor are touched
   by the kernel (SMEP, SMAP: user_copy alone opens them, arch/isr.asm); SSE with its exceptions and
   XSAVE (arch/fpu.c); no descriptor-table reads in the app (UMIP); machine checks, last of their
   own setup. PCIDE is mm/vmm.c's, which sets it once CR3 holds PCID 0 (vmm_init_cpu). EFER:
   SYSCALL (entry_load) and no-execute pages, before a page table uses bit 63. */
bool cpu_setup(unsigned index) {
    gdt_init_cpu(index); /* and the GS base: a null selector load clears it on these CPUs */
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS | CR0_CD | CR0_NW)) | CR0_WP | CR0_NE | CR0_MP);
    write_cr4(read_cr4() | CR4_PGE | CR4_FSGSBASE | CR4_SMEP | CR4_SMAP | CR4_OSFXSR | CR4_OSXMMEXCPT |
              CR4_OSXSAVE | (cpu_features.umip ? CR4_UMIP : 0));
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE | EFER_SCE);
    entry_load(); /* from here a fault has a handler, wrmsr_checked among them */
    speculation_setup();
    if (!pat_cpu_setup()) return false;
    if (!fpu_cpu_setup()) return false;
    mca_setup();
    cpu_perf_setup();
    if (!index && !said) {
        said = true;
        /* LFENCE orders RDTSC after what comes before it (sys/clock.c) where it is dispatch
           serializing: always on Intel (SDM vol. 2A, LFENCE), on AMD where CPUID says so (APM
           vol. 2 7.6.4); the MSR that makes it so on older AMD CPUs is in none of the manuals here */
        if (cpu_features.vendor != CPU_VENDOR_INTEL && !cpu_features.lfence_serializing)
            kprintf("cpu: LFENCE is not known to be dispatch serializing here; a TSC read may pass what comes before it\n");
        kprintf("cpu: %s%s%s%s\n", cpu_features.auto_ibrs ? "automatic IBRS" : "no automatic IBRS",
                cpu_features.ibpb ? ", IBPB between apps" : ", no IBPB",
                cpu_features.stibp && cpu_features.stibp_always_on ? ", STIBP" : "",
                cpu_features.umip ? ", UMIP" : "");
    }
    return true;
}
