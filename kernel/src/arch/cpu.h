#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void pause(void) { __asm__ volatile("pause" ::: "memory"); }

static inline uint64_t read_rflags(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f) : : "memory");
    return f;
}
static inline uint64_t irq_save(void) {
    uint64_t f = read_rflags();
    cli();
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) sti();
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
/* XSAVE対応/CR4.OSXSAVEの検査後だけ使用。XCR0はfpu.cの起動policy専用。 */
static inline uint64_t xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | lo;
}
static inline void xsetbv0(uint64_t value) {
    __asm__ volatile("xsetbv" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(0u) : "memory");
}

/* interrupt frame pushed by isr.asm */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B
#define USER_CS   0x23
#define TSS_SEL   0x28

void gdt_init(void);
void gdt_init_cpu(unsigned index);
void tss_set_rsp0(uint64_t rsp0);
void idt_init(void);
void idt_load(void);
typedef void (*irq_handler_t)(struct regs *r);
void irq_register(int irq, irq_handler_t h);
void vector_register(int vec, irq_handler_t h);
bool lapic_present(void);
bool lapic_x2apic(void); /* Limineが選んだBSP/AP共通mode。起動後の切替は禁止。 */
uint32_t lapic_current_id(void);
bool lapic_worker_init(void);
bool lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_eoi(void);
bool lapic_timer_start(unsigned hz, int vector);
bool lapic_runner_timer_start(void); /* BSPで較正済みのperiodic tickだけをAPで再使用 */
void isr_dispatch_remote(struct regs *r, uint64_t cr2);
extern uint64_t tsc_hz; /* measured by lapic_timer_start(); 0 if unknown */
#define VEC_TIMER 0x30
void pic_init(void);
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_eoi(int irq);
void fpu_init(void);
void cpu_get_brand(char *out);
