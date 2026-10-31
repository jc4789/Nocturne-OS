/* The ways into the kernel (arch/irq.h): the IDT, SYSCALL's MSRs, each CPU's entry stacks, and the
   exceptions. The interrupts (vector 32 and up) are arch/irq.c's. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/preempt.h"
#include "arch/smp.h"
#include "sys/process.h"
#include "sys/stream.h"
#include "sys/thread.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "arch/irq.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t ist;
    uint8_t type;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} PACKED;

static struct idt_entry idt[256];
extern uint64_t isr_stub_table[256];
extern char syscall_entry[];

/* Each CPU's stacks for its entries: the interrupt stack, for the handlers of vector 32 and up, so
   that neither a task's stack nor a nested interrupt pays for them (arch/isr.asm switches to it),
   and the IST stacks. Each has a guard page below it (vmalloc's), so that an overflow faults
   instead of writing over what lies below; the CPU allocates its own, from its node. The user's
   rsp in SYSCALL's entry, until the frame holds it. */
#define ENTRY_STACK_PAGES 4
PERCPU uint64_t irq_stack_top;
PERCPU static uint64_t ist_top[IST_STACKS];
PERCPU uint64_t syscall_user_rsp;
static bool ist_gates; /* the gates below take their IST stacks: the first CPU's are there */

/* SYSCALL's flag mask: the entry runs with interrupts off (until syscall_dispatch turns them on),
   no single-stepping, the string direction up, SMAP's check on (AC), no task nesting, I/O
   privilege 0. */
#define RFLAGS_TF   (1u << 8)
#define RFLAGS_IF   (1u << 9)
#define RFLAGS_DF   (1u << 10)
#define RFLAGS_IOPL (3u << 12)
#define RFLAGS_NT   (1u << 14)
#define RFLAGS_AC   (1u << 18)
#define SYSCALL_FLAGS_MASK (RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_IOPL | RFLAGS_NT | RFLAGS_AC)

static void idt_set(int vec, uint64_t handler, uint8_t type, uint8_t ist) {
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].sel = KERNEL_CS;
    idt[vec].ist = ist;
    idt[vec].type = type;
    idt[vec].off_mid = (handler >> 16) & 0xFFFF;
    idt[vec].off_hi = handler >> 32;
    idt[vec].zero = 0;
}

/* Until the first CPU has its IST stacks every gate runs on the stack in use (early boot's). */
void idt_init(void) {
    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_table[i], 0x8E, 0);
}

bool entry_stacks_init(void) {
    if (!this_cpu(irq_stack_top)) { /* a CPU started again after a sleep keeps its own */
        uint64_t top[1 + IST_STACKS];
        for (unsigned i = 0; i < 1 + IST_STACKS; i++) {
            uint8_t *stack = vmalloc(ENTRY_STACK_PAGES);
            if (!stack) {
                while (i--) vfree((void *)(top[i] - ENTRY_STACK_PAGES * PAGE_SIZE), ENTRY_STACK_PAGES);
                return false;
            }
            top[i] = (uint64_t)stack + ENTRY_STACK_PAGES * PAGE_SIZE;
        }
        for (unsigned i = 0; i < IST_STACKS; i++) this_cpu(ist_top)[i] = top[1 + i];
        this_cpu(irq_stack_top) = top[0];
    }
    tss_set_ist(*this_cpu_ptr(ist_top)); /* this CPU's TSS (arch/gdt.c) */
    if (!ist_gates) { /* the first CPU's, before any other starts */
        idt_set(1, isr_stub_table[1], 0x8E, IST_DEBUG);
        idt_set(2, isr_stub_table[2], 0x8E, IST_NMI);
        idt_set(8, isr_stub_table[8], 0x8E, IST_DOUBLE_FAULT);
        idt_set(18, isr_stub_table[18], 0x8E, IST_MACHINE_CHECK);
        ist_gates = true;
    }
    return true;
}

void entry_load(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } PACKED idtr = {sizeof idt - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr));
    /* SYSCALL into the kernel's code (0x08, its stack 0x10); SYSRET to the app's (0x13 + 16 =
       0x23, its stack 0x13 + 8 = 0x1B) */
    wrmsr(MSR_STAR, (uint64_t)(USER_DS - 8) << 48 | (uint64_t)KERNEL_CS << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, SYSCALL_FLAGS_MASK); /* EFER.SCE is cpu_setup's */
}

/* ---- exceptions ---- */
static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault", "Page fault", "Reserved",
    "x87 FP error", "Alignment check", "Machine check", "SIMD FP error", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved"};

extern char user_copy_start[], user_copy_end[], user_copy_fault[];

/* The kernel's copy of the app's memory (user_copy) touched a page that is not there yet, or
   that it may not touch: an access the tables allow now runs on (a translation cached before they
   gave it, mm/vmm.c); otherwise resolved as the app's own fault would be when the copy runs with
   interrupts on (it may sleep then), or else, or when it cannot be, the copy stops short. */
static bool user_copy_fixup(struct regs *r, uint64_t cr2) {
    if (r->rip < (uint64_t)user_copy_start || r->rip >= (uint64_t)user_copy_end || cr2 >= USER_TOP) return false;
    if (vmm_fault_spurious(cr2, r->error | 4)) return true;
    if ((r->rflags & RFLAGS_IF) && vmm_handle_user_fault(cr2, r->error | 4)) return true;
    r->rip = (uint64_t)user_copy_fault;
    return true;
}

/* A page fault, CR2 read before anything could fault again: handled with interrupts on where the
   code it interrupted had them on (the app's, always; user_copy's when its caller had), as the
   resolution may sleep and may take long (mm/vmm.h); off again before the way out. */
static bool page_fault(struct regs *r, uint64_t cr2) {
    bool on = r->rflags & RFLAGS_IF;
    if (on) sti();
    bool handled = (r->cs & 3) == 3 ? vmm_handle_user_fault(cr2, r->error) : user_copy_fixup(r, cr2);
    if (on) cli();
    return handled;
}

static inline uint64_t read_dr6(void) { uint64_t v; __asm__ volatile("mov %%dr6, %0" : "=r"(v)); return v; }
static inline void write_dr6(uint64_t v) { __asm__ volatile("mov %0, %%dr6" : : "r"(v)); }
#define DR6_BS (1u << 14) /* the #DB is a single step */
#define DR6_INIT 0xFFFF0FF0u

static void exception(struct regs *r, uint64_t cr2) {
    const char *name = exc_names[r->vector];
    if (r->vector == 14) {
        if (page_fault(r, cr2)) return;
        if (!(r->cs & 3) && cr2 < USER_TOP) name = "Page fault on the app's memory outside user_copy (SMAP)";
    }
    if (r->vector == 1 && !(r->cs & 3)) {
        /* The kernel sets no breakpoint: a step in it is the app's TF carried across SYSCALL's
           first instruction. The app keeps its TF (the return is an IRET then, arch/isr.asm). */
        uint64_t dr6 = read_dr6();
        write_dr6(DR6_INIT);
        if (dr6 & DR6_BS) return;
    }
    /* #DF is the machine's, not the code it was running (its saved CS is not even defined):
       never the app's end */
    if (r->vector == 8) panic_regs(name, r);
    if ((r->cs & 3) == 3) {
        struct thread *t = thread_current();
        struct process *p = t->process; /* ring 3 runs only an app's threads */
        kprintf("[%s] pid %d (%s) cpu%u: %s at rip=%p", r->vector == 14 ? "segfault" : "crash",
                t->tid, t->name, cpu_current_index(), name, (void *)r->rip);
        if (r->vector == 14) kprintf(" addr=%p err=%lx", (void *)cr2, r->error);
        /* The words at the stack top name the caller when the jump target
           itself was bad (rip == addr after a call through a stale pointer). */
        kprintf(" rsp=%p stack", (void *)r->rsp);
        for (unsigned i = 0; i < 4; i++) {
            uint64_t va = r->rsp + i * 8, pa = p->vmm_space ? vmm_translate(p->vmm_space, va) : 0;
            if (!pa || (va & 0xFFF) > 0xFF8) break;
            kprintf(" %p", (void *)*(uint64_t *)phys_to_virt(pa));
        }
        kprintf("\n");
        /* A helper can die while its owner waits for a completion token or
           while stderr's pipe is full. Never block process abort on that pipe;
           the serial diagnostic above still records the exact exception. */
        if (t == p->main)
            stream_error_printf(p, "\n\x1b[31m*** %s (%s) at %p, addr %p ***\x1b[0m\n", name, t->name,
                                (void *)r->rip, r->vector == 14 ? (void *)cr2 : 0);
        process_exit(128 + (int)r->vector);
    }
    panic_regs(name, r);
}

extern char wrmsr_checked_write[], wrmsr_checked_refused[];

/* Every CPU's exceptions (vector below 32), on the stack they came in on: the task's, which an
   exception from the app may sleep on (a page fault), or, from the kernel, the IST stack of an
   NMI, #DB, #DF or #MC (arch/isr.asm moves one from the app to the task's). */
void isr_exception(struct regs *r) {
    uint64_t v = r->vector;
    if (v == 13 && !(r->cs & 3) && r->rip == (uint64_t)wrmsr_checked_write) { /* cpu.h: wrmsr_checked */
        r->rip = (uint64_t)wrmsr_checked_refused;
        return;
    }
    /* NMI and #MC, from the kernel on their IST stacks, counted as such (arch/preempt.h) */
    if (v == 2 || v == 18) {
        this_cpu(preempt_count) += PREEMPT_NMI_ONE;
        if (v == 2) smp_nmi(r); /* another CPU's panic stops this one; any other is reported */
        else cpu_machine_check(r);
        this_cpu(preempt_count) -= PREEMPT_NMI_ONE;
        return;
    }
    exception(r, v == 14 ? read_cr2() : 0);
}
