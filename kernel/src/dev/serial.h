/* The serial port the log leaves by (dev/serial.c): one reader of the log (sys/log.h), the
   console's text and every record, sent from the port's transmit interrupt. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct panic_line;
/* The first thing at boot: COM1 when it answers, set up for a panic until serial_start. */
void serial_init(void);
/* sys/log.c's log_start, CPU 0 with interrupts off: the port the ACPI SPCR names (else COM1), its
   interrupt, and the log from its oldest record on. */
void serial_start(void);
/* sys/log.c, interrupts off: records were written; an empty transmitter is filled again. */
void serial_log_ready(void);
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
