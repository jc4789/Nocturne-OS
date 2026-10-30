/* What the CPUs are. The leaves that name features are read here once, on the first CPU, before
   anything uses them; every other CPU is checked against them as it starts. Nocturne requires the
   set below of every CPU (x86-64-v4, the system features the kernel builds on, and nothing older:
   the floor is Zen 4) and stops at boot when one is missing. The optional features are the ones a
   hypervisor or a model may lack (WHPX shows no PCID, MWAIT or invariant TSC bit): their users
   choose a path by them. Leaf 0xD (the XSAVE layout) is arch/fpu.c's, the topology leaves (0xB,
   0x1F, 4, 0x8000001D) arch/topology.c's, the hypervisor's leaves from 0x40000000 vm/'s. */
#include "kernel.h"
#include "arch/cpu.h"

struct cpu_features cpu_features;

enum { EAX, EBX, ECX, EDX };

/* The leaves whose bits name features; a CPU reads them all at once. */
static const struct { uint32_t leaf, sub; } leaves[] = {
    {1, 0}, {6, 0}, {7, 0}, {0xD, 1}, {0x80000001, 0}, {0x80000007, 0}, {0x80000008, 0},
};
enum { L1, L6, L7, LD1, LE1, LE7, LE8, LEAVES };
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

/* The optional bits the kernel reads, each into its flag; another CPU has to show each the same. */
#define OPTIONAL(l, r, b, n, flag) {{l, r, b, n}, __builtin_offsetof(struct cpu_features, flag)}
static const struct {
    struct feature f;
    size_t flag;
} optional[] = {
    OPTIONAL(L1, ECX, 3, "MWAIT", mwait), OPTIONAL(L1, ECX, 17, "PCID", pcid),
    OPTIONAL(L1, ECX, 24, "TSC deadline", tsc_deadline), OPTIONAL(L1, ECX, 31, "hypervisor", hypervisor),
    OPTIONAL(L6, EAX, 7, "HWP", intel_hwp), OPTIONAL(L6, ECX, 0, "APERF/MPERF", aperf_mperf),
    OPTIONAL(L7, ECX, 13, "TME", tme), OPTIONAL(L7, EDX, 29, "IA32_ARCH_CAPABILITIES", arch_capabilities),
    OPTIONAL(LE1, ECX, 22, "topology extensions", topology_extensions),
    OPTIONAL(LE7, EDX, 8, "invariant TSC", invariant_tsc), OPTIONAL(LE8, EBX, 3, "INVLPGB", invlpgb),
    OPTIONAL(LE8, EBX, 27, "CPPC", cppc),
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

static leaf_regs first;

void cpu_features_init(void) {
    struct cpu_features *f = &cpu_features;
    read_leaves(first);
    uint32_t max, a, b, c, d;
    cpuid(0, 0, &max, &b, &c, &d);
    f->vendor = vendor_of(b, c, d);
    read_brand(f->brand);
    for (unsigned i = 0; i < LEAVES; i++)
        kprintf("cpu: cpuid %x.%u eax %08x ebx %08x ecx %08x edx %08x\n", leaves[i].leaf, leaves[i].sub,
                first[i][EAX], first[i][EBX], first[i][ECX], first[i][EDX]);

    char missing[256];
    missing[0] = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(required); i++)
        if (!has(first, &required[i])) {
            if (missing[0]) strlcat(missing, ", ", sizeof missing);
            strlcat(missing, required[i].name, sizeof missing);
        }
    if (missing[0]) panic("cpu: %s lacks what Nocturne requires: %s", f->brand, missing);

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
    for (unsigned i = 0; i < ARRAY_SIZE(optional); i++)
        *(bool *)((char *)f + optional[i].flag) = has(first, &optional[i].f);
    /* CPUID 15h: the TSC's ratio to the core crystal clock and, where it is given, the crystal's
       frequency (Intel SDM vol. 2A, CPUID leaf 15h). */
    if (max >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c) f->tsc_crystal_hz = (uint64_t)c * b / a;
    }
    char present[256];
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
    return true;
}

/* Each CPU's own controls for what every one has: no-execute pages (before a page table uses bit
   63), write protection in the kernel too, so read-only pages really are read-only; the app's
   pages neither run nor are touched by the kernel (SMEP, SMAP: user_copy alone opens them,
   arch/isr.asm); and FS and GS bases the app writes itself (FSGSBASE: a task switch keeps them,
   sys/sched.c, and the kernel's GS base is never told from the app's by its value); and global
   pages, which a CR3 load keeps: the kernel half's leaves, the same in every space (mm/vmm.c). */
void cpu_setup(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);
    write_cr4(read_cr4() | CR4_PGE | CR4_FSGSBASE | CR4_SMEP | CR4_SMAP);
}
