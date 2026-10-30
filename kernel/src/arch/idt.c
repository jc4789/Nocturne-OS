#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "mm/vmm.h"
#include "dev/entropy.h"
#include "dev/timer.h"
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
#define IRQ_SHARED 4 /* level-triggered PCI lines and the ACPI SCI can share an ISA IRQ */
static irq_handler_t irq_handlers[16][IRQ_SHARED];
static irq_handler_t vec_handlers[256]; /* LAPIC-delivered vectors (0x30 and up) */
/* A driver on any CPU registers and allocates under it; isr_dispatch reads the slots without it,
   each slot stored whole. */
static struct spinlock vectors_lock;

static void idt_set(int vec, uint64_t handler, uint8_t type, uint8_t ist) {
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].sel = KERNEL_CS;
    idt[vec].ist = ist;
    idt[vec].type = type;
    idt[vec].off_mid = (handler >> 16) & 0xFFFF;
    idt[vec].off_hi = handler >> 32;
    idt[vec].zero = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_table[i], 0x8E, 0);
    idt_set(8, isr_stub_table[8], 0x8E, 1);      /* double fault on IST1 */
    idt_set(0x80, isr_stub_table[0x80], 0xEE, 0); /* syscall gate, DPL 3 */
    idt_load();
}

void idt_load(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } PACKED idtr = {sizeof idt - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void irq_register(int irq, irq_handler_t h) {
    uint64_t f = spin_lock(&vectors_lock);
    unsigned i = 0;
    while (i < IRQ_SHARED && irq_handlers[irq][i] && irq_handlers[irq][i] != h) i++;
    if (i == IRQ_SHARED) panic("idt: more than %d handlers on IRQ %d", IRQ_SHARED, irq);
    __atomic_store_n(&irq_handlers[irq][i], h, __ATOMIC_RELEASE);
    spin_unlock(&vectors_lock, f);
}

void irq_unregister(int irq, irq_handler_t h) {
    uint64_t f = spin_lock(&vectors_lock);
    for (unsigned i = 0; i < IRQ_SHARED; i++)
        if (irq_handlers[irq][i] == h) {
            for (; i + 1 < IRQ_SHARED; i++)
                __atomic_store_n(&irq_handlers[irq][i], irq_handlers[irq][i + 1], __ATOMIC_RELEASE);
            __atomic_store_n(&irq_handlers[irq][IRQ_SHARED - 1], NULL, __ATOMIC_RELEASE);
        }
    spin_unlock(&vectors_lock, f);
}

/* ---- vector ownership ----
   A vector is owned once it has been allocated or registered. Fixed driver vectors are reserved by
   range so the allocator never collides with them (they keep their numbers). */
#define VEC_POOL_FIRST 0x50
#define VEC_POOL_LAST  0xEF
static bool vec_owned[256];

static bool vec_in_pool(unsigned v) { return v >= VEC_POOL_FIRST && v <= VEC_POOL_LAST && v != 0x80; }

int irq_vector_alloc(unsigned count, unsigned align) {
    if (!align) align = 1;
    if (!count || (align & (align - 1)) || count > VEC_POOL_LAST) return -1;
    uint64_t f = spin_lock(&vectors_lock);
    int found = -1;
    for (unsigned base = ALIGN_UP(VEC_POOL_FIRST, align); base + count - 1 <= VEC_POOL_LAST; base += align) {
        bool free = true;
        for (unsigned i = 0; i < count && free; i++) free = vec_in_pool(base + i) && !vec_owned[base + i];
        if (!free) continue;
        for (unsigned i = 0; i < count; i++) vec_owned[base + i] = true;
        found = (int)base;
        break;
    }
    spin_unlock(&vectors_lock, f);
    return found;
}

void irq_vector_free(unsigned vector, unsigned count) {
    uint64_t f = spin_lock(&vectors_lock);
    for (unsigned i = 0; i < count && vector + i < 256; i++) {
        if (!vec_in_pool(vector + i)) continue;
        vec_owned[vector + i] = false;
        __atomic_store_n(&vec_handlers[vector + i], NULL, __ATOMIC_RELEASE);
    }
    spin_unlock(&vectors_lock, f);
}

void vector_register(int vec, irq_handler_t h) {
    if (vec < 0 || vec > 255) panic("idt: bad vector %d", vec);
    uint64_t f = spin_lock(&vectors_lock);
    /* the same handler again (a controller re-initialising) is harmless; a different one is a collision */
    if (h && vec_handlers[vec] && vec_handlers[vec] != h) panic("idt: vector %#x registered twice", vec);
    if (h) vec_owned[vec] = true;
    __atomic_store_n(&vec_handlers[vec], h, __ATOMIC_RELEASE);
    spin_unlock(&vectors_lock, f);
}

/* ---- 8259 PIC ---- */
void pic_init(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); /* master vectors 0x20-0x27 */
    outb(0xA1, 0x28); io_wait(); /* slave vectors 0x28-0x2F */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFB); /* everything masked except cascade */
    outb(0xA1, 0xFF);
}

/* The ELCR (ports 0x4D0/0x4D1) says which lines are level-triggered (PCI INTx, the SCI); the
   firmware sets it at power on, and a machine woken from S3 may come back with it cleared. */
uint32_t pic_state(void) {
    return inb(0x21) | (uint32_t)inb(0xA1) << 8 | (uint32_t)inb(0x4D0) << 16 | (uint32_t)inb(0x4D1) << 24;
}

uint16_t pic_mask_all(void) {
    uint16_t masks = inb(0x21) | (uint16_t)inb(0xA1) << 8;
    pic_restore_mask(UINT16_MAX);
    return masks;
}

void pic_restore_mask(uint16_t masks) {
    outb(0x21, (uint8_t)masks);
    outb(0xA1, (uint8_t)(masks >> 8));
}

void pic_restore(uint32_t state) {
    pic_init();
    if ((state >> 16) != 0xFFFF) { /* 0xFFFF: nothing answers there */
        outb(0x4D0, (uint8_t)(state >> 16));
        outb(0x4D1, (uint8_t)(state >> 24));
    }
    pic_restore_mask((uint16_t)state);
}

void pic_unmask(int irq) {
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_mask(int irq) {
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static uint8_t pic_isr(uint16_t cmd) {
    outb(cmd, 0x0B);
    return inb(cmd);
}

/* ---- exceptions ---- */
static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault", "Page fault", "Reserved",
    "x87 FP error", "Alignment check", "Machine check", "SIMD FP error", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved"};

void panic_regs(const char *what, struct regs *r);

extern char user_copy_start[], user_copy_end[], user_copy_fault[];

/* The kernel's copy of the app's memory (user_copy) touched a page that is not there yet, or
   that it may not touch: resolved as the app's own fault would be when the copy runs with
   interrupts on (it may sleep then), otherwise, or when it cannot be, the copy stops short. */
static bool user_copy_fixup(struct regs *r, uint64_t cr2) {
    if (r->rip < (uint64_t)user_copy_start || r->rip >= (uint64_t)user_copy_end || cr2 >= USER_TOP) return false;
    if ((r->rflags & 0x200) && vmm_handle_user_fault(cr2, r->error | 4)) return true;
    r->rip = (uint64_t)user_copy_fault;
    return true;
}

static void exception(struct regs *r, uint64_t cr2) {
    const char *name = exc_names[r->vector];
    if (r->vector == 14) {
        if ((r->cs & 3) == 3 ? vmm_handle_user_fault(cr2, r->error) : user_copy_fixup(r, cr2)) return;
    }
    if ((r->cs & 3) == 3) {
        struct task *t = current_task;
        kprintf("[%s] pid %d (%s) cpu%u: %s at rip=%p", r->vector == 14 ? "segfault" : "crash",
                t->pid, t->name, cpu_current_index(), name, (void *)r->rip);
        if (r->vector == 14) kprintf(" addr=%p err=%lx", (void *)cr2, r->error);
        /* The words at the stack top name the caller when the jump target
           itself was bad (rip == addr after a call through a stale pointer). */
        kprintf(" rsp=%p stack", (void *)r->rsp);
        for (unsigned i = 0; i < 4; i++) {
            uint64_t va = r->rsp + i * 8, pa = t->pml4 ? vmm_translate(t->pml4, va) : 0;
            if (!pa || (va & 0xFFF) > 0xFF8) break;
            kprintf(" %p", (void *)*(uint64_t *)phys_to_virt(pa));
        }
        kprintf("\n");
        /* A helper can die while its owner waits for a completion token or
           while stderr's pipe is full. Never block process abort on that pipe;
           the serial diagnostic above still records the exact exception. */
        if (!t->is_thread)
            task_printf_stderr(t, "\n\x1b[31m*** %s (%s) at %p, addr %p ***\x1b[0m\n", name, t->name,
                               (void *)r->rip, r->vector == 14 ? (void *)cr2 : 0);
        task_exit_app(128 + (int)r->vector);
    }
    panic_regs(name, r);
}

extern char wrmsr_checked_write[], wrmsr_checked_refused[];

/* Every CPU's traps, faults, system calls and interrupts, on the stack of the task it runs. */
void isr_dispatch(struct regs *r) {
    uint64_t v = r->vector;
    if (v == 13 && !(r->cs & 3) && r->rip == (uint64_t)wrmsr_checked_write) { /* cpu.h: wrmsr_checked */
        r->rip = (uint64_t)wrmsr_checked_refused;
        return;
    }
    if (v == VEC_TLB_SHOOTDOWN) {
        vmm_tlb_ipi();
        lapic_eoi();
        return;
    }
    if (v == 2) smp_panic_nmi(); /* another CPU's panic stops this one; any other NMI is reported */
    timer_wake(); /* a CPU idle without its tick: uptime_ms catches up first */
    if (v < 32) {
        exception(r, v == 14 ? read_cr2() : 0);
    } else if (v < 48) { /* the 8259's lines, which reach CPU 0 only */
        int irq = (int)v - 32;
        if (irq == 7 && !(pic_isr(0x20) & 0x80)) return;
        if (irq == 15 && !(pic_isr(0xA0) & 0x80)) {
            outb(0x20, 0x20);
            return;
        }
        entropy_add(rdtsc() ^ (uint64_t)irq << 56);
        pic_eoi(irq);
        for (unsigned i = 0; i < IRQ_SHARED; i++) {
            irq_handler_t h = __atomic_load_n(&irq_handlers[irq][i], __ATOMIC_ACQUIRE);
            if (!h) break;
            h(r);
        }
    } else if (v == 0x80) {
        syscall_dispatch(r);
    } else if (v < 0xFF) {
        entropy_add(rdtsc() ^ v << 56);
        lapic_eoi();
        irq_handler_t h = __atomic_load_n(&vec_handlers[v], __ATOMIC_ACQUIRE);
        if (h) h(r);
    }
    if ((r->cs & 3) == 3) sched_user_return(r);
}
