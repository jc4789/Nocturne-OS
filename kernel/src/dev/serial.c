/* The serial port the log leaves by (dev/serial.h): a 16550-compatible UART, its registers as
   every such UART has them (National Semiconductor's PC16550D; the data sheet is not at hand).

   Which port: the one the firmware names in its SPCR, when the SPCR describes a 16550 (interface
   types 0, 1 and 0x12) in system memory or I/O space (the address is a GAS, ACPI 6.5 5.2.3.2 table
   5.1). The SPCR is Microsoft's table, which ACPI 6.5 table 5.6 only links to; its layout here is
   the one uACPI's acpi.h gives (revision 4, 88 bytes), the specification itself not at hand.
   Without one, COM1: I/O port 3F8h on ISA IRQ 4, the PC's assignment, when its scratch register
   keeps what is written to it.

   How it sends: from the log's rings (sys/log.h) through a cursor of its own, a FIFO's worth at a
   time, in the log's thread (serial_drain), at the line's rate: one fill, then none until the FIFO
   has had its time on the line, whatever the rounds between. A UART is touched no oftener than it
   can send, and no interrupt is asked of it. When the cursor has nothing more the port stays quiet,
   and the next records' round fills it again. No writer waits for the line: what a ring overwrites
   before the port sends it, the cursor says it lost.

   The waits on the line's status (LSR) are where nothing else may run: a panic, the power going
   off, a sleep's start. Each is bounded by twice the time the FIFO and the shift register take at
   the line's rate (twice, so that a slow clock is not taken for a dead port), after which the
   port is given up. Before the clock runs (a panic early in the boot) that time cannot be
   measured and the wait has no bound: a 16550 sends at its rate unless its modem lines are told
   to hold it back, which this never does. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/serial.h"
#include "hal/hal.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/log.h"
#include "sys/panic.h"

enum {
    REG_DATA = 0, /* RBR (read) / THR (write); DLL with LCR_DLAB */
    REG_IER = 1,  /* interrupt enable; DLM with LCR_DLAB */
    REG_IIR = 2,  /* interrupt identification (read) / FCR, FIFO control (write) */
    REG_LCR = 3,
    REG_MCR = 4,
    REG_LSR = 5,
    REG_MSR = 6,
    REG_SCR = 7,
};
#define IIR_FIFOS 0xC0   /* both set: the FIFOs are on */
#define FCR_ON 0x07      /* FIFOs on, both emptied */
#define LCR_8N1 0x03     /* 8 data bits, no parity, 1 stop bit */
#define LCR_DLAB 0x80
#define MCR_ON 0x0B      /* DTR, RTS and OUT2, which passes the interrupt to the PC's line */
#define LSR_THRE 0x20    /* the holding register (the FIFO, when on) is empty */
#define LSR_TEMT 0x40    /* the holding register and the shift register are empty */
#define FIFO_16550A 16   /* the 16550A's transmit FIFO */
#define CHAR_BITS 10     /* 8N1: a start bit, 8 data bits, a stop bit */
#define UART_CLOCK 1843200 /* the PC's UART clock, Hz: 115200 baud at divisor 1 */
#define COM1_PORT 0x3F8
#define LOG_BAUD 115200

/* The SPCR (see above). A field past h.length is not there (revision 2 ends before uart_clock,
   revision 3 before precise_baud). */
struct acpi_spcr {
    struct acpi_sdt h;
    uint8_t interface, reserved[3];
    struct acpi_gas base;
    uint8_t interrupt_type; /* bit 0: an 8259's line (irq), bit 1: an IOAPIC's GSI (gsi) */
    uint8_t irq;
    uint32_t gsi;
    uint8_t baud, parity, stop_bits, flow_control, terminal, language;
    uint16_t pci_device, pci_vendor;
    uint8_t pci_bus, pci_dev, pci_func;
    uint32_t pci_flags;
    uint8_t pci_segment;
    uint32_t uart_clock;   /* Hz, 0: not given */
    uint32_t precise_baud; /* 0: not given */
    uint16_t namespace_length, namespace_offset;
} __attribute__((packed));
_Static_assert(sizeof(struct acpi_spcr) == 88, "the SPCR's revision 4 layout");
#define SPCR_HAS(t, field) ((t)->h.length >= offsetof(struct acpi_spcr, field) + sizeof((t)->field))

struct uart {
    bool present;
    bool mmio;
    uint16_t port;           /* I/O space */
    uint64_t phys;           /* system memory: the registers' address */
    volatile uint8_t *base;  /* system memory: mapped */
    unsigned stride;         /* the bytes from one register to the next, and the width of an access */
    unsigned depth;          /* the bytes the transmitter takes at once */
    uint32_t clock, divisor;
    uint64_t char_ns;        /* a character's time on the line */
};

static struct uart uart;
/* The port's transmitter and the cursor: a leaf. The log's thread takes it with interrupts on (no
   interrupt handler takes it), the polled paths with them off. */
static struct spinlock serial_lock;
static struct log_cursor *cursor;   /* from serial_start */
static bool started;
static uint64_t next_fill;          /* the log's thread: no fill before this */
static unsigned room;               /* polled sending: the bytes the FIFO still takes */

static uint8_t reg_in(unsigned reg) {
    if (!uart.mmio) return inb((uint16_t)(uart.port + reg));
    volatile uint8_t *p = uart.base + reg * uart.stride;
    if (uart.stride == 4) return (uint8_t)*(volatile uint32_t *)p;
    if (uart.stride == 2) return (uint8_t)*(volatile uint16_t *)p;
    return *p;
}

static void reg_out(unsigned reg, uint8_t v) {
    if (!uart.mmio) {
        outb((uint16_t)(uart.port + reg), v);
        return;
    }
    volatile uint8_t *p = uart.base + reg * uart.stride;
    if (uart.stride == 4) *(volatile uint32_t *)p = v;
    else if (uart.stride == 2) *(volatile uint16_t *)p = v;
    else *p = v;
}

/* Something keeps what is written to the scratch register: a UART decodes the port. */
static bool answers(void) {
    static const uint8_t probe[] = {0x5A, 0xA5};
    for (unsigned i = 0; i < ARRAY_SIZE(probe); i++) {
        reg_out(REG_SCR, probe[i]);
        if (reg_in(REG_SCR) != probe[i]) return false;
    }
    return true;
}

/* The divisor the firmware left (its console's rate), 0 for none. */
static uint32_t firmware_divisor(void) {
    uint8_t lcr = reg_in(REG_LCR);
    reg_out(REG_LCR, lcr | LCR_DLAB);
    uint32_t divisor = reg_in(REG_DATA) | (uint32_t)reg_in(REG_IER) << 8;
    reg_out(REG_LCR, lcr);
    return divisor;
}

static uint32_t divisor_for(uint32_t clock, uint32_t baud) {
    uint32_t d = clock / (16 * baud);
    return d ? (d > 0xFFFF ? 0xFFFF : d) : 1;
}

/* The registers as the log wants them: no interrupt yet, 8N1 at the divisor chosen, the FIFOs on
   and emptied, OUT2; then the FIFO's depth and a character's time. */
static void setup(void) {
    reg_out(REG_IER, 0);
    reg_out(REG_LCR, LCR_DLAB);
    reg_out(REG_DATA, (uint8_t)uart.divisor);
    reg_out(REG_IER, (uint8_t)(uart.divisor >> 8));
    reg_out(REG_LCR, LCR_8N1);
    reg_out(REG_IIR, FCR_ON);
    reg_out(REG_MCR, MCR_ON);
    uart.depth = (reg_in(REG_IIR) & IIR_FIFOS) == IIR_FIFOS ? FIFO_16550A : 1;
    uart.char_ns = (uint64_t)CHAR_BITS * 16 * uart.divisor * NS_PER_S / uart.clock;
    room = 0;
}

void serial_init(void) {
    uart = (struct uart){.port = COM1_PORT, .stride = 1, .clock = UART_CLOCK};
    uart.divisor = divisor_for(UART_CLOCK, LOG_BAUD);
    if (!answers()) return;
    setup();
    uart.present = true;
}

/* The SPCR's port into u, when it is a 16550 this can reach; not set up yet. */
static bool spcr_port(struct uart *u) {
    const struct acpi_spcr *t = acpi_find("SPCR", 0);
    if (!t || !SPCR_HAS(t, pci_segment)) return false;
    if (t->interface != 0x00 && t->interface != 0x01 && t->interface != 0x12) return false;
    const struct acpi_gas *g = &t->base;
    unsigned stride = 1;
    if (t->interface == 0x12) { /* the registers as wide as the GAS's accesses */
        static const unsigned widths[] = {[1] = 1, [2] = 2, [3] = 4};
        unsigned w = g->access < ARRAY_SIZE(widths) ? widths[g->access] : 0;
        if (!g->access && (g->bit_width == 8 || g->bit_width == 16 || g->bit_width == 32)) w = g->bit_width / 8;
        if (!w) return false;
        stride = w;
    }
    *u = (struct uart){.stride = stride, .clock = UART_CLOCK};
    if (g->space == 1) {
        if (stride != 1 || !g->address || g->address > 0xFFFF - REG_SCR) return false;
        u->port = (uint16_t)g->address;
    } else if (g->space == 0) {
        if (!g->address || !(u->base = vmm_map_mmio(g->address, (REG_SCR + 1) * stride))) return false;
        u->phys = g->address;
        u->mmio = true;
    } else {
        return false;
    }
    if (SPCR_HAS(t, uart_clock) && t->uart_clock) u->clock = t->uart_clock;
    if (SPCR_HAS(t, precise_baud) && t->precise_baud) u->divisor = divisor_for(u->clock, t->precise_baud);
    return true;
}

/* serial_lock held: what the FIFO takes from the log, the holding register empty. False when the
   cursor had nothing. */
static bool fill(void) {
    char piece[FIFO_16550A];
    size_t n = log_read(cursor, piece, uart.depth);
    for (size_t i = 0; i < n; i++) reg_out(REG_DATA, (uint8_t)piece[i]);
    return n != 0;
}

void serial_start(void) {
    struct uart named;
    const char *from = "the SPCR's port";
    if (spcr_port(&named)) {
        uart = named;
        if (!uart.divisor) uart.divisor = firmware_divisor(); /* the rate the firmware's console uses */
        if (!uart.divisor) uart.divisor = divisor_for(uart.clock, LOG_BAUD);
        setup();
        uart.present = true;
    } else if (uart.present) {
        from = "COM1";
    } else {
        kprintf("serial: no port (no SPCR, and nothing answers at COM1)\n");
        return;
    }
    if (!(cursor = log_cursor_new(LOG_CURSOR_CONSOLE | LOG_CURSOR_CRLF))) panic("serial: no memory for its cursor");
    char where[40];
    if (uart.mmio) ksnprintf(where, sizeof where, "memory %p", (void *)uart.base);
    else ksnprintf(where, sizeof where, "I/O %x", uart.port);
    kprintf("serial: %s at %s, %u baud, a %u-byte FIFO\n", from, where, uart.clock / (16 * uart.divisor), uart.depth);
    started = true;
}

uint64_t serial_drain(void) {
    if (!started) return CLOCK_NEVER;
    uint64_t now = clock_now_ns();
    spin_take(&serial_lock);
    bool sending = uart.present;
    if (sending && now >= next_fill && (reg_in(REG_LSR) & LSR_THRE)) {
        sending = fill() && log_pending(cursor);
        next_fill = now + uart.depth * uart.char_ns; /* a FIFO's time on the line */
    } else if (sending && now >= next_fill) {
        next_fill = now + uart.char_ns; /* it is still sending its last: a character's time more */
    }
    uint64_t again = sending ? next_fill : CLOCK_NEVER;
    spin_give(&serial_lock);
    return again;
}

/* Interrupts off: until the line status shows bit (see the top). False, the port given up, when it
   does not come. */
static bool wait_line(uint8_t bit) {
    uint64_t now = clock_now_ns(), until = now ? now + 2 * (uart.depth + 1) * uart.char_ns : 0;
    while (!(reg_in(REG_LSR) & bit)) {
        if (until && clock_now_ns() >= until) {
            uart.present = false;
            return false;
        }
        pause();
    }
    return true;
}

/* Polled, interrupts off: one byte, waiting for room in the FIFO. */
static bool put(char ch) {
    if (!room) {
        if (!wait_line(LSR_THRE)) return false;
        room = uart.depth;
    }
    reg_out(REG_DATA, (uint8_t)ch);
    room--;
    return true;
}

/* Polled: s's n bytes, each '\n' as "\r\n" when crlf. */
static bool put_text(const char *s, size_t n, bool crlf) {
    for (size_t i = 0; i < n; i++)
        if ((crlf && s[i] == '\n' && !put('\r')) || !put(s[i])) return false;
    return true;
}

static bool put_cursor(struct log_cursor *c, bool crlf) {
    char piece[256];
    for (size_t n; (n = log_read(c, piece, sizeof piece));)
        if (!put_text(piece, n, crlf)) return false;
    return true;
}

void serial_flush(void) {
    if (!started) return;
    spin_take(&serial_lock);
    room = 0;
    if (uart.present && put_cursor(cursor, false)) wait_line(LSR_TEMT);
    spin_give(&serial_lock);
}

void serial_suspend(void) {
    if (!started) return;
    spin_take(&serial_lock);
    if (uart.present) {
        reg_out(REG_IER, 0);
        wait_line(LSR_TEMT); /* what the FIFO holds is gone with the UART's state otherwise */
    }
    spin_give(&serial_lock);
}

void serial_resume(void) {
    if (!started) return;
    spin_take(&serial_lock);
    if (uart.present) setup();
    spin_give(&serial_lock); /* log_resume owes the log's thread a round, which fills it */
}

/* The report, ahead of what the port had not sent of the log: a line far behind the log
   (megabytes at 115200 baud take minutes) would otherwise hold it back. */
void serial_panic(const struct panic_line *lines, unsigned count) {
    spin_seize(&serial_lock);
    if (!uart.present) return;
    reg_out(REG_IER, 0);
    room = 0;
    for (unsigned i = 0; i < count; i++) {
        const char *before = i ? "" : "\n*** KERNEL PANIC: ";
        if (!put_text(before, strlen(before), true) || !put_text(lines[i].text, strlen(lines[i].text), true) ||
            !put_text("\n", 1, true))
            return;
    }
}

/* What the port had not sent of the log: the rest of its cursor's, or all of it before
   serial_start (the panic's cursor, which the panic screen read before). */
void serial_panic_log(void) {
    if (!uart.present) return;
    struct log_cursor *c = cursor;
    if (!c) {
        c = log_panic_cursor();
        log_cursor_rewind(c);
    }
    static const char rest[] = "*** the log's text the serial port had not sent yet:\n";
    if (log_pending(c) && (!put_text(rest, sizeof rest - 1, true) || !put_cursor(c, !cursor))) return;
    wait_line(LSR_TEMT);
}

/* ---- the machine's device that is this file's (hal/hal.h) ----
   The port the log leaves by: the 16550-compatible device (PNP0500, PNP0501) whose registers it is. */
static bool console_mine(struct hal_device *d) {
    if (!started) return false; /* serial_start came before the drivers bind */
    return uart.mmio ? hal_resource_holds(d, N_HAL_MEMORY, uart.phys) : hal_resource_holds(d, N_HAL_PORT, uart.port);
}

static const struct n_hal_rule console_rules[] = {N_ACPI_ID("PNP0500"), N_ACPI_ID("PNP0501")};
static const struct hal_machine serial_console = {
    .owner = "serial", .rules = console_rules, .rule_count = ARRAY_SIZE(console_rules), .mine = console_mine};
HAL_MACHINE(serial_console);
