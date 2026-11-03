/* What differs by who made the CPU. One implementation each (arch/cpu_amd.c, arch/cpu_intel.c); the
   first CPU chooses once, by what CPUID leaf 0 names (cpu_features_init), and cpu_features.maker
   holds it. A hypervisor's CPU is its maker's: what a hypervisor changes is vm/vm.h's.

   Slow operations only: the TSC's read, EOI and IPIs go direct. An operation left NULL means the
   maker's manuals give nothing of it, so that a maker with no implementation runs on the
   architecture alone. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct mca_layout;
struct tsc_rate;

/* What a CPU can be asked for: floor is the bottom of the range it is asked to keep to. */
struct cpu_perf_caps {
    unsigned highest, nominal, floor, lowest;
};

/* A maker's registers for the performance request (arch/idle.h). */
struct cpu_perf_way {
    const char *name;
    uint32_t enable_msr;
    void (*caps)(struct cpu_perf_caps *c);
    void (*request)(const struct cpu_perf_caps *c, uint8_t preference);
};

struct cpu_maker {
    const char *cpuid_names[2]; /* what leaf 0 calls it, in EBX, EDX, ECX */
    /* What the table of CPUID bits does not say: the first CPU, once a fault has a handler. */
    void (*read_features)(void);
    /* Another CPU as it starts: false when it lacks what read_features found. */
    bool (*same_features)(void);
    /* What this CPU's registers state of its TSC's rate (arch/tsc_rate.h). */
    void (*tsc_stated)(struct tsc_rate *r);
    /* The way to ask for performance; NULL where this CPU has none. */
    const struct cpu_perf_way *(*perf_way)(void);
    /* Where the machine-check banks are; NULL: the layout both manuals give (arch/mca.c). */
    const struct mca_layout *(*mca_layout)(void);
    /* A bank about to report, before its CTL is written. */
    void (*mca_bank_on)(unsigned bank);
    /* This CPU's corrected errors told through an interrupt (mca_interrupt_on). */
    void (*mca_interrupt_on)(void);
    /* The APIC ID's shift to a die, where the extended topology leaf names none. */
    bool (*die_shift)(unsigned *shift);
    /* The CPUID leaf that gives the caches' sharing; 0 where there is none. */
    uint32_t (*cache_leaf)(void);
};

extern const struct cpu_maker cpu_amd, cpu_intel;
