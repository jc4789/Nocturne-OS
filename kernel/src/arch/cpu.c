/* What the CPUs are, and the state each one sets itself up in.

   The leaves that name features are read here once, on the first CPU, before anything uses them;
   every other CPU is checked against them as it starts. Nocturne requires the set below of every
   CPU, each row for code that runs without asking, and stops at boot when one is missing, whoever
   made it. The optional features are the ones a machine may lack: their users choose a path by
   them. Of those, what Nocturne needs of the hardware itself (an invariant TSC) is required where
   no hypervisor runs the CPU (vm_cpu_guest), which may hide it. Where makers differ, each maker has
   its implementation (arch/cpu_maker.h). Leaf 0xD (the XSAVE layout) is arch/fpu.c's, the topology
   leaves (0xB, 0x1F) arch/topology.c's, the hypervisor's leaves from 0x40000000 vm/'s.

   cpu_setup is the one way a CPU gets into the state the kernel runs it in: CPU 0 at boot, each
   other CPU as it starts, CPU 0 again after S3. Nothing is copied back from before a sleep. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/cpu_maker.h"
#include "arch/fpu.h"
#include "arch/gdt.h"
#include "arch/idle.h"
#include "arch/irq.h"
#include "arch/mca.h"
#include "arch/pat.h"
#include "arch/smp.h"
#include "arch/tsc_rate.h"
#include "vm/vm.h"

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
    /* what x86-64 code as compiled here runs on (Makefile: -msse2) */
    {L1, EDX, 0, "FPU"}, {L1, EDX, 15, "CMOV"}, {L1, EDX, 24, "FXSR"}, {L1, EDX, 25, "SSE"}, {L1, EDX, 26, "SSE2"},
    {LE1, EDX, 11, "SYSCALL"}, {LE1, EDX, 29, "long mode"},
    /* the page tables and memory types (mm/vmm.c, arch/pat.c) */
    {L1, EDX, 6, "PAE"}, {L1, EDX, 12, "MTRR"}, {L1, EDX, 13, "PGE"}, {L1, EDX, 16, "PAT"}, {L1, EDX, 19, "CLFSH"},
    {L7, EBX, 7, "SMEP"}, {L7, EBX, 10, "INVPCID"}, {L7, EBX, 20, "SMAP"},
    {LE1, EDX, 20, "NX"}, {LE1, EDX, 26, "1 GiB pages"},
    /* the entries, the per-CPU word and the clock (arch/isr.asm, kernel/entry/clock.c, user/libc/sys.c) */
    {L1, EDX, 4, "TSC"}, {L1, EDX, 5, "MSR"}, {L7, EBX, 0, "FSGSBASE"}, {L7, ECX, 22, "RDPID"}, {LE1, EDX, 27, "RDTSCP"},
    /* the machine checks (arch/mca.c), the interrupts (arch/apic.c), the extended state (arch/fpu.c),
       the IOMMU's entries (dev/iommu_amd.c) */
    {L1, EDX, 7, "MCE"}, {L1, EDX, 14, "MCA"}, {L1, EDX, 9, "APIC"}, {L1, ECX, 21, "x2APIC"},
    {L1, ECX, 26, "XSAVE"}, {LD1, EAX, 3, "XSAVES"}, {L1, ECX, 13, "CX16"},
};

/* The optional bits the kernel reads, each into its flag (two bits may name one flag, Intel's and
   AMD's); another CPU has to show each the same. HARDWARE ones are required where no hypervisor
   runs the CPU. */
#define OPTIONAL(l, r, b, n, flag) {{l, r, b, n}, __builtin_offsetof(struct cpu_features, flag), false}
#define HARDWARE(l, r, b, n, flag) {{l, r, b, n}, __builtin_offsetof(struct cpu_features, flag), true}
static const struct {
    struct feature f;
    size_t flag;
    bool hardware;
} optional[] = {
    OPTIONAL(L1, ECX, 3, "MWAIT", mwait), OPTIONAL(L1, ECX, 17, "PCID", pcid), OPTIONAL(L1, ECX, 28, "AVX", avx),
    OPTIONAL(L1, ECX, 24, "TSC deadline", tsc_deadline), OPTIONAL(L1, ECX, 30, "RDRAND", rdrand),
    OPTIONAL(L6, EAX, 2, "ARAT", arat), OPTIONAL(L6, EAX, 7, "HWP", hwp), OPTIONAL(L6, EAX, 10, "HWP EPP", hwp_epp),
    OPTIONAL(L6, ECX, 0, "APERF/MPERF", aperf_mperf),
    OPTIONAL(L7, ECX, 2, "UMIP", umip),
    OPTIONAL(L7, ECX, 13, "TME", tme), OPTIONAL(L7, EDX, 26, "IBPB", ibpb), OPTIONAL(L7, EDX, 26, "IBRS", ibrs),
    OPTIONAL(L7, EDX, 27, "STIBP", stibp),
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

/* ---- the maker ---- */
static const struct cpu_maker cpu_unknown;
static const struct cpu_maker *const makers[] = {&cpu_amd, &cpu_intel};

/* The maker leaf 0 names in EBX, EDX, ECX. */
static const struct cpu_maker *maker_named(void) {
    uint32_t a, name[4] = {0};
    cpuid(0, 0, &a, &name[0], &name[2], &name[1]);
    for (unsigned i = 0; i < ARRAY_SIZE(makers); i++)
        for (unsigned j = 0; j < ARRAY_SIZE(makers[i]->cpuid_names); j++)
            if (makers[i]->cpuid_names[j] && !strcmp((const char *)name, makers[i]->cpuid_names[j])) return makers[i];
    return &cpu_unknown;
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

/* What the CPU states of its TSC's rate (arch/tsc_rate.h says what the registers mean); the clock
   starts from it, and measures the rate where it states none (sys/clock.c). A hypervisor's guest
   reads nothing of its own TSC in them (a TSC ratio may scale it): vm/ registers that rate where
   the hypervisor states one. */
static void tsc_stated(struct tsc_rate *r) {
    const struct cpu_maker *maker = cpu_features.maker;
    if (vm_cpu_guest()) return tsc_rate_none(r, "a hypervisor runs this CPU");
    if (!maker->tsc_stated) return tsc_rate_none(r, "no manual here gives this maker's registers");
    maker->tsc_stated(r);
}

static leaf_regs first;

void cpu_features_init(void) {
    struct cpu_features *f = &cpu_features;
    read_leaves(first);
    f->maker = maker_named();
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
        if (optional[i].hardware && !has(first, &optional[i].f) && !vm_cpu_guest()) {
            if (missing[0]) strlcat(missing, ", ", sizeof missing);
            strlcat(missing, optional[i].f.name, sizeof missing);
        }
    if (missing[0]) panic("cpu: %s lacks what Nocturne requires: %s", f->brand, missing);

    uint32_t a = first[L1][EAX];
    f->family = (a >> 8) & 0xF;
    f->model = (a >> 4) & 0xF;
    if (f->family == 0xF) {
        f->family += (a >> 20) & 0xFF;
        f->model |= ((a >> 16) & 0xF) << 4;
    }
    f->stepping = a & 0xF;
    struct tsc_rate rate;
    tsc_stated(&rate);
    f->tsc_num = rate.num;
    f->tsc_den = rate.den;
    f->tsc_by = rate.by;
    f->clflush_bytes = ((first[L1][EBX] >> 8) & 0xFF) * 8;
    f->phys_bits = first[LE8][EAX] & 0xFF;
    f->invlpgb_pages = first[LE8][EDX] & 0xFFFF;
    char present[512];
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
    struct tsc_rate rate;
    tsc_stated(&rate);
    if (rate.num != cpu_features.tsc_num || (rate.num && rate.den != cpu_features.tsc_den))
        return false; /* its TSC would count at another rate than the clock's */
    const struct cpu_maker *maker = cpu_features.maker;
    return !maker->same_features || maker->same_features();
}

/* ---- the state of each CPU ---- */
/* Speculation controls, as APM vol. 2 3.2.9 and 3.1.7 and SDM vol. 4 (IA32_SPEC_CTRL) give them.
   Automatic IBRS keeps what the app trained out of the kernel's indirect branches without a write
   on each entry; without it, IBRS on a CPU that prefers it set once at boot ("IBRS always on",
   Intel's enhanced IBRS). STIBP where the CPU prefers it set once, so that apps on sibling threads
   do not train each other's. The kernel's half and the app's are apart by more than a page and SMEP
   is on, so return predictions need no clearing (3.2.9). IBPB between apps is cpu_ibpb's. */
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
    if (!index && cpu_features.maker->read_features) cpu_features.maker->read_features();
    speculation_setup();
    if (!pat_cpu_setup()) return false;
    if (!fpu_cpu_setup()) return false;
    mca_setup();
    cpu_perf_setup();
    if (!index && !said) {
        said = true;
        /* LFENCE orders RDTSC after what comes before it (sys/clock.c) where it is dispatch
           serializing (each maker's manual; AMD's APM vol. 2 7.6.4 where CPUID says so); the MSR
           that makes it so on older AMD CPUs is in none of the manuals here */
        if (!cpu_features.lfence_serializing)
            kprintf("cpu: LFENCE is not known to be dispatch serializing here; a TSC read may pass what comes before it\n");
        kprintf("cpu: %s%s%s%s\n",
                cpu_features.auto_ibrs ? "automatic IBRS"
                : cpu_features.ibrs && cpu_features.ibrs_always_on ? "IBRS set once"
                                                                   : "no automatic IBRS",
                cpu_features.ibpb ? ", IBPB between apps" : ", no IBPB",
                cpu_features.stibp && cpu_features.stibp_always_on ? ", STIBP" : "",
                cpu_features.umip ? ", UMIP" : "");
    }
    return true;
}
