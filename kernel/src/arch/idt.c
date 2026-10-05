#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "mm/vmm.h"

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
static irq_handler_t irq_handlers[16];

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
    struct {
        uint16_t limit;
        uint64_t base;
    } PACKED idtr = {sizeof idt - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void irq_register(int irq, irq_handler_t h) { irq_handlers[irq] = h; }

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

static void exception(struct regs *r) {
    const char *name = exc_names[r->vector];
    if (r->vector == 14) {
        uint64_t cr2 = read_cr2();
        if (vmm_handle_user_fault(cr2, r->error)) return;
    }
    if ((r->cs & 3) == 3) {
        struct task *t = current_task;
        uint64_t cr2 = read_cr2();
        kprintf("[%s] pid %d (%s): %s at rip=%p", r->vector == 14 ? "segfault" : "crash",
                t->pid, t->name, name, (void *)r->rip);
        if (r->vector == 14) kprintf(" addr=%p err=%lx", (void *)cr2, r->error);
        kprintf("\n");
        task_printf_stderr(t, "\n\x1b[31m*** %s (%s) at %p, addr %p ***\x1b[0m\n", name, t->name,
                           (void *)r->rip, r->vector == 14 ? (void *)cr2 : 0);
        task_exit(128 + (int)r->vector);
    }
    panic_regs(name, r);
}

void isr_dispatch(struct regs *r) {
    uint64_t v = r->vector;
    if (v < 32) {
        exception(r);
    } else if (v < 48) {
        int irq = (int)v - 32;
        if (irq == 7 && !(pic_isr(0x20) & 0x80)) return;
        if (irq == 15 && !(pic_isr(0xA0) & 0x80)) {
            outb(0x20, 0x20);
            return;
        }
        pic_eoi(irq);
        if (irq_handlers[irq]) irq_handlers[irq](r);
    } else if (v == 0x80) {
        syscall_dispatch(r);
    }
    if ((r->cs & 3) == 3) sched_user_return(r);
}
