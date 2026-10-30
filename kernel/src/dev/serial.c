#include "kernel.h"
#include "arch/spin.h"
#include "dev/serial.h"

/* COM1, the log port. What is written goes into a ring and leaves through the UART's transmit
   FIFO as fast as the line takes it: at 115200 baud a character takes 87 us, and no writer waits
   for the line (kprintf runs with interrupts off; an app's log lines come through the system log; the
   console echoes here), or a driver app that logs much would hold the BSP, and with it every other
   task, input and the screen, for as long as the line needs. The FIFO is filled whenever it is
   empty: by each write, and by the 1 kHz tick while anything waits (dev/timer.c), 16 characters a
   millisecond, more than the line's 11.5. When the ring is full the newest output is dropped and
   counted, and the count is written once there is room again. A panic, a power-off, a reboot and a
   sleep empty the ring first, waiting on the line (serial_flush). */
#define COM1 0x3F8
#define LSR_THRE 0x20 /* Line Status: transmitter holding register (the FIFO, when on) empty */
#define RING (64 * 1024)

static bool serial_ok;
static unsigned fifo_depth = 1;
static char ring[RING];
static size_t head, tail; /* characters ever queued and sent: head - tail wait (interrupts off) */
static uint64_t dropped;
static struct spinlock serial_lock; /* the ring and the port: any CPU logs */

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
    fifo_depth = (inb(COM1 + 2) & 0xC0) == 0xC0 ? 16 : 1; /* IIR: FIFOs enabled, a 16550A's 16 bytes */
    serial_ok = true;
}

static size_t room(void) { return RING - (head - tail); }

static void send_burst(void) {
    for (unsigned i = 0; i < fifo_depth && head != tail; i++) outb(COM1, (uint8_t)ring[tail++ % RING]);
}

/* Interrupts off: whatever the FIFO takes now. */
static void pump(void) {
    if (head != tail && (inb(COM1 + 5) & LSR_THRE)) send_burst();
}

static void queue(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (room() < (s[i] == '\n' ? 2u : 1u)) {
            dropped += n - i;
            return;
        }
        if (s[i] == '\n') ring[head++ % RING] = '\r';
        ring[head++ % RING] = s[i];
    }
}

void serial_write(const char *s, size_t n) {
    if (!serial_ok) return;
    uint64_t f = spin_lock(&serial_lock);
    if (dropped) {
        char note[48];
        int len = ksnprintf(note, sizeof note, "\n[serial: %lu bytes dropped]\n", (unsigned long)dropped);
        if (len > 0 && room() >= 2 * (size_t)len) {
            dropped = 0;
            queue(note, (size_t)len);
        }
    }
    queue(s, n);
    pump();
    spin_unlock(&serial_lock, f);
}

void serial_tick(void) {
    if (!serial_ok) return;
    uint64_t f = spin_lock(&serial_lock);
    pump();
    spin_unlock(&serial_lock, f);
}

bool serial_needs_tick(void) { return serial_ok && head != tail; }

/* Everything queued, waiting on the line. A UART whose FIFO never empties again ends the wait
   (as many polls as a character took the old way: 100000), the rest of the ring unsent. */
void serial_flush(void) {
    if (!serial_ok) return;
    uint64_t f = spin_lock(&serial_lock);
    while (head != tail) {
        unsigned spin = 0;
        while (!(inb(COM1 + 5) & LSR_THRE) && ++spin < 100000) pause();
        if (spin == 100000) break;
        send_burst();
    }
    spin_unlock(&serial_lock, f);
}
