#include "kernel.h"
#include "arch/cpu.h"
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

static struct tss cpu_tss[CPU_MAX_COUNT];
static uint64_t cpu_gdt[CPU_MAX_COUNT][7];
static uint8_t double_fault_stack[CPU_MAX_COUNT][16384] __attribute__((aligned(16)));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} PACKED;

void tss_set_rsp0(uint64_t rsp0) { cpu_tss[cpu_current_index()].rsp0 = rsp0; }

void gdt_init(void) { gdt_init_cpu(0); }

void gdt_init_cpu(unsigned index) {
    ASSERT(index < CPU_MAX_COUNT);
    struct tss *tss = &cpu_tss[index];
    uint64_t *gdt = cpu_gdt[index];
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* 0x08 kernel code 64 */
    gdt[2] = 0x00CF92000000FFFFULL; /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL; /* 0x18 user data (DPL3) */
    gdt[4] = 0x00AFFA000000FFFFULL; /* 0x20 user code 64 (DPL3) */
    memset(tss, 0, sizeof *tss);
    tss->iomap_base = sizeof *tss;
    tss->ist[0] = (uint64_t)double_fault_stack[index] + sizeof double_fault_stack[index];
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof *tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr g = {sizeof cpu_gdt[index] - 1, (uint64_t)gdt};
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
    cpu_local_init(index);
}
