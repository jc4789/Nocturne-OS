#include "kernel.h"
#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/smp.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} PACKED;
_Static_assert(__builtin_offsetof(struct tss, rsp0) == 4, "arch/isr.asm reads rsp0 at 4");

/* rsp0 is the stack of the task this CPU runs (tss_set_rsp0), where SYSCALL's entry goes too
   (arch/isr.asm); ist the entry's own stacks (entry_stacks_init). */
PERCPU struct tss cpu_tss;
PERCPU static uint64_t cpu_gdt[7];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} PACKED;

void tss_set_rsp0(uint64_t rsp0) { this_cpu(cpu_tss).rsp0 = rsp0; }
void tss_set_ist(const uint64_t top[IST_STACKS]) {
    for (unsigned i = 0; i < IST_STACKS; i++) this_cpu(cpu_tss).ist[i] = top[i];
}

/* CPU index's GDT and TSS, in its per-CPU copy (before percpu_init the first CPU's is the
   template), loaded on this CPU; then its GS base. The TSS keeps what it holds: rsp0 and the IST
   stacks stay across a sleep (entry_stacks_init, sys/sched.c). Its descriptor is written afresh,
   available: one ltr marked busy before a sleep is not loaded again. */
void gdt_init_cpu(unsigned index) {
    ASSERT(!index || index < topology.cpus);
    struct tss *tss = &per_cpu(cpu_tss, index);
    uint64_t *gdt = per_cpu(cpu_gdt, index);
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* 0x08 kernel code 64 */
    gdt[2] = 0x00CF92000000FFFFULL; /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL; /* 0x18 user data (DPL3) */
    gdt[4] = 0x00AFFA000000FFFFULL; /* 0x20 user code 64 (DPL3) */
    tss->iomap_base = sizeof *tss;
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof *tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr g = {sizeof cpu_gdt - 1, (uint64_t)gdt};
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw $0x28, %%ax\n"
        "ltr %%ax\n"
        : : "m"(g) : "rax", "memory");
    percpu_load(index);
}
