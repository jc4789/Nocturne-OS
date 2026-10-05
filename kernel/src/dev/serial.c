#include "kernel.h"
#include "arch/cpu.h"
#include "dev/serial.h"

#define COM1 0x3F8
static bool serial_ok;

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* DLAB */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* FIFO */
    outb(COM1 + 4, 0x1E); /* loopback for self-test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        serial_ok = false;
        return;
    }
    outb(COM1 + 4, 0x0F);
    serial_ok = true;
}

static void serial_putc(char c) {
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20) break;
        pause();
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n) {
    if (!serial_ok) return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') serial_putc('\r');
        serial_putc(s[i]);
    }
}
