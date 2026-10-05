#include "kernel.h"
#include "arch/cpu.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} PACKED;

static struct tss tss;
static uint64_t gdt[7];
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} PACKED;

void tss_set_rsp0(uint64_t rsp0) { tss.rsp0 = rsp0; }

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* 0x08 kernel code 64 */
    gdt[2] = 0x00CF92000000FFFFULL; /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL; /* 0x18 user data (DPL3) */
    gdt[4] = 0x00AFFA000000FFFFULL; /* 0x20 user code 64 (DPL3) */
    memset(&tss, 0, sizeof tss);
    tss.iomap_base = sizeof tss;
    tss.ist[0] = (uint64_t)double_fault_stack + sizeof double_fault_stack;
    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr g = {sizeof gdt - 1, (uint64_t)gdt};
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
}

/* ---- FPU / SSE ---- */
void fpu_init(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1ULL << 2); /* EM */
    cr0 |= (1ULL << 1);  /* MP */
    cr0 &= ~(1ULL << 3); /* TS */
    write_cr0(cr0);
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR, OSXMMEXCPT */
    write_cr4(cr4);
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

void cpu_get_brand(char *out) {
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) {
        uint32_t v[3];
        cpuid(0, 0, &a, &v[0], &v[2], &v[1]);
        memcpy(out, v, 12);
        out[12] = 0;
        return;
    }
    uint32_t *o = (uint32_t *)out;
    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, 0, &o[i * 4], &o[i * 4 + 1], &o[i * 4 + 2], &o[i * 4 + 3]);
    }
    out[48] = 0;
    /* trim leading spaces */
    char *p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}
