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
    uint64_t tsc_crystal_hz; /* the TSC's rate as CPUID 15h states it; 0 where it does not */
    bool hypervisor, mwait, pcid, tsc_deadline, invariant_tsc, aperf_mperf, intel_hwp, cppc,
        invlpgb, tme, arch_capabilities, topology_extensions;
    char brand[49];
};
extern struct cpu_features cpu_features;
void cpu_features_init(void); /* the first CPU, first: stops when a required feature is missing */
bool cpu_features_same(void); /* another CPU as it starts: true when it has what the first one has */
void cpu_setup(void);         /* each CPU, before its page tables: what every one turns on */

#define MSR_EFER  0xC0000080u
#define EFER_SCE  (1u << 0)
#define EFER_LME  (1u << 8)
#define EFER_LMA  (1u << 10)
#define EFER_NXE  (1u << 11)
#define MSR_STAR    0xC0000081u
#define MSR_LSTAR   0xC0000082u
#define MSR_CSTAR   0xC0000083u
#define MSR_SFMASK  0xC0000084u
#define MSR_FS_BASE 0xC0000100u
#define MSR_TSC_AUX 0xC0000103u
#define MSR_PAT   0x277u
#define CR0_WP    (1ULL << 16)
#define CR4_PGE      (1ULL << 7)
#define CR4_FSGSBASE (1ULL << 16)
#define CR4_PCIDE    (1ULL << 17)
#define CR4_SMEP     (1ULL << 20)
#define CR4_SMAP     (1ULL << 21)
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

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B
#define USER_CS   0x23
#define TSS_SEL   0x28

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

/* Each CPU's stacks for the exceptions that may come where the stack in use cannot be trusted
   (the TSS's IST, arch/gdt.c): NMI, #DB and #MC anywhere, #DF after a stack went bad. */
enum { IST_DOUBLE_FAULT = 1, IST_NMI, IST_DEBUG, IST_MACHINE_CHECK, IST_STACKS = IST_MACHINE_CHECK };

void gdt_init(void);
void gdt_init_cpu(unsigned index);
void tss_set_rsp0(uint64_t rsp0);
void tss_set_ist(const uint64_t top[IST_STACKS]);
/* The ways into the kernel (arch/idt.c, arch/isr.asm): idt_init builds the IDT on the first CPU
   and loads its entries there, entry_load loads them on each other CPU as it starts (the IDT and
   SYSCALL's MSRs). entry_stacks_init gives this CPU its interrupt and IST stacks, once the page
   tables can map them (vmm_init), before its interrupts are on: false without the memory. */
void idt_init(void);
void entry_load(void);
bool entry_stacks_init(void);
/* This CPU's handlers running on its interrupt stack (arch/isr.asm): 0 outside them. */
extern PERCPU uint32_t irq_nesting;
typedef void (*irq_handler_t)(struct regs *r);
void irq_register(int irq, irq_handler_t h); /* a line may have a few handlers; each checks its device */
void irq_unregister(int irq, irq_handler_t h);
void vector_register(int vec, irq_handler_t h);
bool lapic_present(void);
bool lapic_x2apic(void); /* BSP/AP共通mode：Limineの選択（IOMMUの要件、iommu_x2apic_conflictではapic_initがxAPICへ戻す）。以後の切替は禁止。 */
uint32_t lapic_current_id(void);
/* A physical destination the APIC mode in use addresses, not its broadcast: below FFh in xAPIC
   mode (below Fh on an AMD APIC without the 8-bit ID), below FFFFFFFFh in x2APIC mode. */
bool lapic_addressable(uint32_t apic_id);
bool lapic_ap_init(void);
bool lapic_send_ipi(uint32_t apic_id, uint8_t vector);
bool lapic_send_nmi(uint32_t apic_id);
void lapic_eoi(void);
/* The wake device of sys/alarm.c: this CPU's LAPIC timer, interrupting it once on vector at the
   moment lapic_wake_at names (clock_now_ns; CLOCK_NEVER: not at all) or soon after, never long
   before (the alarm code sets it again when it comes early). lapic_wake_init on each CPU as it
   starts, CPU 0 first; both with this CPU's interrupts off or from its own code only. */
bool lapic_wake_init(int vector);
void lapic_wake_at(uint64_t ns);
void lapic_resume(void); /* BSP after S3: its registers again (the wake device: alarm_resume) */
/* INIT and STARTUP, sent and not waited for (SDM 9.4.4.1, Table 9-1): the caller waits
   LAPIC_INIT_WAIT_NS after its INITs before any STARTUP, and sends STARTUP twice, waiting
   LAPIC_STARTUP_WAIT_NS after each round, the second ignored by a CPU already running. One wait
   serves every CPU sent to. */
#define LAPIC_INIT_WAIT_NS 10000000ULL
#define LAPIC_STARTUP_WAIT_NS 200000ULL
bool lapic_send_init(uint32_t apic_id); /* INIT: the AP waits for a STARTUP, running nothing */
bool lapic_send_startup(uint32_t apic_id, uint64_t page); /* after an INIT: into real mode at page */
/* What a CPU a STARTUP reached needs to learn whether the STARTUP was its own (arch/wake.h): the
   kernel's mode, and in xAPIC mode the ID register's mapping, the bits of it that hold the ID,
   the IA32_APIC_BASE value (under base_mask) of an APIC at that mapping, and whether AMD's 8-bit
   ID is to be turned on before the register is read. */
struct lapic_ident {
    bool x2, xaidn;
    const volatile uint32_t *id_reg;
    uint32_t id_mask;
    uint64_t base, base_mask;
};
void lapic_ident(struct lapic_ident *out);
#define VEC_TIMER 0x30
void pic_init(void);
void pic_unmask(int irq);
void pic_mask(int irq);
void pic_eoi(int irq);
uint32_t pic_state(void);           /* masks (bit n: IRQ n masked) and, above them, the ELCR */
uint16_t pic_mask_all(void);        /* mask every source; return the previous masks, without reinitializing */
void pic_restore_mask(uint16_t masks); /* restore masks only, preserving pending PIC requests */
void pic_restore(uint32_t state);   /* after S3: pic_init, then that state again */
void fpu_init(void);
