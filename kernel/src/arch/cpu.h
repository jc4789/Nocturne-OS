#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "arch/percpu.h"

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* Device registers (MMIO): one plain mov of exactly that width, kept in order with every other
   memory access. Through a volatile dereference the compiler may fold the access into another
   instruction (a push of an argument, an arithmetic operand), which a hypervisor emulating the
   register then has to decode; QEMU's WHPX emulator stops at such a push. Memory the CPU shares
   with devices (rings, tables, frame buffers) is RAM and keeps plain volatile accesses. */
static inline uint8_t mmio_read8(const volatile void *p) {
    uint8_t v;
    __asm__ volatile("movb %1, %0" : "=q"(v) : "m"(*(const volatile uint8_t *)p) : "memory");
    return v;
}
static inline uint16_t mmio_read16(const volatile void *p) {
    uint16_t v;
    __asm__ volatile("movw %1, %0" : "=r"(v) : "m"(*(const volatile uint16_t *)p) : "memory");
    return v;
}
static inline uint32_t mmio_read32(const volatile void *p) {
    uint32_t v;
    __asm__ volatile("movl %1, %0" : "=r"(v) : "m"(*(const volatile uint32_t *)p) : "memory");
    return v;
}
static inline uint64_t mmio_read64(const volatile void *p) {
    uint64_t v;
    __asm__ volatile("movq %1, %0" : "=r"(v) : "m"(*(const volatile uint64_t *)p) : "memory");
    return v;
}
static inline void mmio_write8(volatile void *p, uint8_t v) {
    __asm__ volatile("movb %1, %0" : "=m"(*(volatile uint8_t *)p) : "q"(v) : "memory");
}
static inline void mmio_write16(volatile void *p, uint16_t v) {
    __asm__ volatile("movw %1, %0" : "=m"(*(volatile uint16_t *)p) : "r"(v) : "memory");
}
static inline void mmio_write32(volatile void *p, uint32_t v) {
    __asm__ volatile("movl %1, %0" : "=m"(*(volatile uint32_t *)p) : "r"(v) : "memory");
}
static inline void mmio_write64(volatile void *p, uint64_t v) {
    __asm__ volatile("movq %1, %0" : "=m"(*(volatile uint64_t *)p) : "r"(v) : "memory");
}

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
/* sys/sched.c: a switch is due on this CPU; the switch where preemption is allowed (arch/preempt.h). */
extern PERCPU volatile bool sched_resched;
void preempt_schedule(void);
/* Interrupts on again where f had them on: one of the points where this CPU is preempted, when a
   switch is due and nothing else holds preemption off. */
static inline void irq_restore(uint64_t f) {
    if (!(f & 0x200)) return;
    sti();
    if (this_cpu(sched_resched)) preempt_schedule();
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
/* A write the manuals allow but a model's own document says faults (arch/apic.c): false where
   it raised #GP, which isr_exception resumes past (arch/isr.asm). */
bool wrmsr_checked(uint32_t msr, uint64_t v);
enum cpu_vendor { CPU_VENDOR_OTHER, CPU_VENDOR_INTEL, CPU_VENDOR_AMD };
/* What the CPUs have (arch/cpu.c): set by cpu_features_init on the first CPU, before anything
   reads it, and the same on every CPU after (cpu_features_same). The features Nocturne requires
   have no field: every CPU has them. The flags are the optional ones. */
struct cpu_features {
    enum cpu_vendor vendor;
    unsigned family, model, stepping;
    unsigned phys_bits;      /* physical address width */
    unsigned clflush_bytes;  /* the line CLFLUSH and CLFLUSHOPT act on */
    unsigned invlpgb_pages;  /* most pages one INVLPGB names, beyond the first */
    /* The TSC's rate on the hardware: it counts at the P0 P-state's frequency, which PStateDef[0]
       gives as these two IDs: exactly CPU_COF_UNIT_HZ * p0_fid / p0_did a second (arch/cpu.c).
       Both 0 under a hypervisor, which states its guest's TSC rate itself (vm/). */
    unsigned p0_fid, p0_did;
    bool hypervisor, mwait, pcid, tsc_deadline, invariant_tsc, aperf_mperf, cppc, invlpgb, tme,
        arch_capabilities, topology_extensions, umip, scalable_mca, lfence_serializing;
    bool arat; /* the LAPIC timer goes on counting in every C-state */
    /* speculation controls (AMD APM vol. 2 3.2.9, 3.1.7; Intel SDM vol. 2A CPUID leaf 7) */
    bool ibpb, ibrs, stibp, ibrs_always_on, stibp_always_on, auto_ibrs;
    char brand[49];
};
extern struct cpu_features cpu_features;
/* A P-state's frequency from its IDs: CpuFid / CpuDfsId * 200 MHz (PPR 56713, the glossary's
   CoreCOF). */
#define CPU_COF_UNIT_HZ 200000000ull
void cpu_features_init(void); /* the first CPU, first: stops when a required feature is missing */
bool cpu_features_same(void); /* another CPU as it starts: true when it has what the first one has */
/* The state every CPU runs in, set by each one the same way: CPU 0 at boot (after idt_init and
   fpu_init), every other CPU as it starts, and CPU 0 again after S3 (arch/wake.c). index is the
   CPU's (arch/topology.h). Its GDT and TSS, then its GS base and TSC_AUX (arch/percpu.h); CR0,
   CR4 and EFER; the entries (arch/irq.h: the IDT and SYSCALL's MSRs); the speculation
   controls; the memory types (arch/pat.h); the extended state (arch/fpu.h); machine-check
   reporting; the performance request (arch/idle.h). Interrupts off, the CPU's page tables still
   the bootloader's or the wake page's. False when this CPU cannot hold what the first one holds. */
bool cpu_setup(unsigned index);
/* Machine checks (AMD APM vol. 2 ch. 9). cpu_mca_interrupt_on: this CPU, once its LAPIC is
   enabled (an LVT entry written before that stays masked, APM 16.4.7): its corrected errors are
   told through an interrupt as they happen. cpu_machine_check: the #MC handler, on its IST stack:
   it logs every bank's error, returns where all of them were corrected, panics otherwise. */
void cpu_mca_interrupt_on(void);
struct regs;
void cpu_machine_check(struct regs *r);

#define MSR_EFER  0xC0000080u
#define EFER_SCE  (1u << 0)
#define EFER_LME  (1u << 8)
#define EFER_LMA  (1u << 10)
#define EFER_NXE  (1u << 11)
#define EFER_AIBRSE (1u << 21) /* automatic IBRS (APM vol. 2 3.1.7) */
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_CSTAR   0xC0000083u
#define MSR_SFMASK  0xC0000084u
#define MSR_FS_BASE 0xC0000100u
#define MSR_TSC_AUX 0xC0000103u
#define MSR_PAT   0x277u
#define MSR_SPEC_CTRL 0x48u /* APM vol. 2 3.2.9 */
#define SPEC_CTRL_IBRS  (1u << 0)
#define SPEC_CTRL_STIBP (1u << 1)
#define MSR_PRED_CMD  0x49u
#define PRED_CMD_IBPB (1u << 0)
#define CR0_MP    (1ULL << 1)
#define CR0_EM    (1ULL << 2)
#define CR0_TS    (1ULL << 3)
#define CR0_NE    (1ULL << 5)
#define CR0_WP    (1ULL << 16)
#define CR0_NW    (1ULL << 29)
#define CR0_CD    (1ULL << 30)
#define CR4_MCE      (1ULL << 6)
#define CR4_PGE      (1ULL << 7)
#define CR4_OSFXSR   (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_UMIP     (1ULL << 11)
#define CR4_FSGSBASE (1ULL << 16)
#define CR4_PCIDE    (1ULL << 17)
#define CR4_OSXSAVE  (1ULL << 18)
#define CR4_SMEP     (1ULL << 20)
#define CR4_SMAP     (1ULL << 21)

/* Before this CPU runs another app's code: indirect branches trained before no longer steer it
   (APM vol. 2 3.2.9, PRED_CMD). mm/vmm.c, where an app's space replaces another's. */
static inline void cpu_ibpb(void) {
    if (cpu_features.ibpb) wrmsr(MSR_PRED_CMD, PRED_CMD_IBPB);
}
static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }
#define INVPCID_ONE 1 /* every translation of one PCID, global ones excepted */
#define INVPCID_ALL 2 /* every translation of every PCID, global ones included */
static inline void invpcid(uint64_t type, uint64_t pcid, uint64_t va) {
    struct { uint64_t pcid, va; } d = {pcid, va};
    __asm__ volatile("invpcid %0, %1" : : "m"(d), "r"(type) : "memory");
}
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

/* The frame every way into the kernel leaves on the stack (arch/isr.asm): an interrupt's or an
   exception's vector, or REGS_SYSCALL for a system call (SYSCALL: rcx and r11 then hold rip and
   rflags, which the instruction put there). */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};
#define REGS_SYSCALL 256

#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

/* The app's FS and GS bases, which it writes itself (WRFSBASE, WRGSBASE): a task switch reads the
   one leaving's and writes the one coming's. While the kernel runs, the app's GS base is the one
   swapgs put aside; interrupts off around these, since an interrupt from the kernel takes the GS
   base in use for the kernel's. */
static inline uint64_t fs_base_read(void) { uint64_t v; __asm__ volatile("rdfsbase %0" : "=r"(v)); return v; }
static inline void fs_base_write(uint64_t v) { __asm__ volatile("wrfsbase %0" : : "r"(v) : "memory"); }
static inline uint64_t user_gs_base_read(void) {
    uint64_t v;
    __asm__ volatile("swapgs; rdgsbase %0; swapgs" : "=r"(v) : : "memory");
    return v;
}
static inline void user_gs_base_write(uint64_t v) { __asm__ volatile("swapgs; wrgsbase %0; swapgs" : : "r"(v) : "memory"); }

/* Onto the stack below top (aligned down to 16), calling entry(argument), which does not return
   (arch/smp.asm): a CPU leaving the stack the bootloader started it on for its idle task's. */
__attribute__((noreturn)) void cpu_switch_stack(uint64_t top, void (*entry)(void *), void *argument);
