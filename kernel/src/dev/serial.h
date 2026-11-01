/* The serial port the log leaves by (dev/serial.c): one reader of the log (sys/log.h), the
   console's text and every record, sent by the log's thread as the port's FIFO empties. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct panic_line;
/* The first thing at boot: COM1 when it answers, set up for a panic until serial_start. */
void serial_init(void);
/* sys/log.c's log_start, CPU 0 with interrupts off: the port the ACPI SPCR names (else COM1), and
   the log from its oldest record on. */
void serial_start(void);
/* The log's thread (sys/log.c), a round: an empty transmitter whose time has come is filled again.
   The time to be called again while there is text to send, else CLOCK_NEVER. */
uint64_t serial_drain(void);
/* Interrupts off, the power about to go (dev/pm.c): what the log holds goes out, the caller
   waiting on the line. */
void serial_flush(void);
/* A sleep (sys/log.c's log_suspend and log_resume, interrupts off): the port quiet, what its
   FIFO holds sent; then its registers again, and on with what it had not sent. */
void serial_suspend(void);
void serial_resume(void);
/* sys/panic.c: the report; after the panic screen, what the line had not sent of the log, waited
   for. */
void serial_panic(const struct panic_line *lines, unsigned count);
void serial_panic_log(void);
