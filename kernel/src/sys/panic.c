/* The kernel stops (kernel.h). The first CPU to panic stops every other one (smp_panic_enter) and
   reports alone: the report is made in this file's own memory, without a lock or an allocation,
   and goes to the serial port (dev/serial.c: the report), the screen (dev/fbcon.c: the panic
   screen, with the log's newest lines, which is all a machine without a serial port shows) and
   the serial port again (what it had not sent of the log yet), in that order: the report reaches
   the line before the screen's longer drawing, and the log's rest, which may take seconds, comes
   after the screen. The report is not written into the log's rings: nothing but these two reads
   them after this. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "dev/fbcon.h"
#include "dev/serial.h"
#include "sys/panic.h"
#include "sys/thread.h"

/* The frames of a backtrace a report shows: with the registers and the log's newest lines they
   fit a 768-pixel screen. */
#define BACKTRACE_FRAMES 10
#define LINE 160

static char text[3 + 7 + 1 + BACKTRACE_FRAMES][LINE];
static struct panic_line lines[ARRAY_SIZE(text)];

/* The reporting CPU from here on, interrupts off, every other CPU stopped; any other halts. */
static void enter(void) {
    cli();
    if (!smp_panic_enter()) for (;;) hlt();
}

/* Each place is tried once: a fault inside one makes a report on top of this one
   (smp_panic_enter), which goes on with the places not tried yet. */
static NORETURN void report(unsigned count) {
    static bool line_tried, screen_tried, log_tried;
    if (!line_tried) {
        line_tried = true;
        serial_panic(lines, count);
    }
    if (!screen_tried) {
        screen_tried = true;
        fbcon_panic(lines, count);
    }
    if (!log_tried) {
        log_tried = true;
        serial_panic_log();
    }
    for (;;) hlt();
}

static unsigned add(unsigned at, enum panic_tone tone, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static unsigned add(unsigned at, enum panic_tone tone, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(text[at], LINE, fmt, ap);
    va_end(ap);
    lines[at] = (struct panic_line){text[at], tone};
    return at + 1;
}

/* The CPU and the thread it ran (none before the scheduler starts), and what follows. */
static unsigned where(unsigned at, const char *more) {
    struct thread *t = thread_current();
    if (!t) return add(at, PANIC_DETAIL, "on CPU %u%s", cpu_current_index(), more);
    return add(at, PANIC_DETAIL, "on CPU %u, thread %d \"%s\"%s", cpu_current_index(), t->tid, t->name, more);
}

/* The return addresses by the frame pointers from fp, while they point into the kernel's half (a
   bad one faults: the CPU that faults while reporting reports that instead, smp_panic_enter). */
static unsigned backtrace(unsigned at, const uint64_t *fp) {
    at = add(at, PANIC_HEADING, "Backtrace:");
    for (unsigned i = 0; i < BACKTRACE_FRAMES && (uint64_t)fp >= 0xFFFF800000000000ULL && !((uint64_t)fp & 7); i++) {
        at = add(at, PANIC_DETAIL, "  #%u  %016lx", i, fp[1]);
        fp = (const uint64_t *)fp[0];
    }
    return at;
}

NORETURN void panic(const char *fmt, ...) {
    enter();
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(text[0], LINE, fmt, ap);
    va_end(ap);
    lines[0] = (struct panic_line){text[0], PANIC_MESSAGE};
    report(backtrace(where(1, ""), __builtin_frame_address(0)));
}

NORETURN void panic_regs(const char *what, struct regs *r) {
    enter();
    char more[48];
    unsigned n = add(0, PANIC_MESSAGE, "%s (error %lx) at %p", what, r->error, (void *)r->rip);
    ksnprintf(more, sizeof more, ", vector %lu, from ring %lu", r->vector, r->cs & 3);
    n = where(n, more);
    n = add(n, PANIC_HEADING, "RIP %016lx  RSP %016lx  RFLAGS %08lx  CS %lx SS %lx", r->rip, r->rsp, r->rflags, r->cs, r->ss);
    n = add(n, PANIC_DETAIL, "RAX %016lx  RBX %016lx  RCX %016lx  RDX %016lx", r->rax, r->rbx, r->rcx, r->rdx);
    n = add(n, PANIC_DETAIL, "RSI %016lx  RDI %016lx  RBP %016lx  R8  %016lx", r->rsi, r->rdi, r->rbp, r->r8);
    n = add(n, PANIC_DETAIL, "R9  %016lx  R10 %016lx  R11 %016lx  R12 %016lx", r->r9, r->r10, r->r11, r->r12);
    n = add(n, PANIC_DETAIL, "R13 %016lx  R14 %016lx  R15 %016lx", r->r13, r->r14, r->r15);
    n = add(n, PANIC_DETAIL, "CR2 %016lx  CR3 %016lx", read_cr2(), read_cr3());
    report(backtrace(n, (const uint64_t *)r->rbp));
}
