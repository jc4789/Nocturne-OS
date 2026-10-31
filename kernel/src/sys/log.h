/* The kernel's log (sys/log.c): its rings, and how readers read them.

   Writers (klog and kprintf in kernel.h; an app's lines through the system log stream; the
   console's text, sys/stream.c) write a record into their CPU's own ring: the clock's ns
   (clock_now_ns, 0 for a record written before the clock runs), the CPU, the task (0 for none),
   a level and the text. A ring holds the newest records: the oldest are overwritten, and a reader
   that had not got to them is told how many bytes it missed. No writer waits for a reader.

   Readers each hold a cursor: a place in every ring. log_read gives the records after it in the
   order of their time, as text: each line of a record after a prefix "[seconds.micro cpu:task]"
   (the level after the task when it is not LOG_INFO), the console's text as it was written. The
   readers are the serial port (dev/serial.c), the boot screen (dev/fbcon.c), the system log
   stream's receivers (sys/stream.c: an app, init's keeping the log on C:), n_system_log, and
   the panic screen. A reader waits for records with log_told and log_wait. */
#pragma once
#include "kernel.h"

/* A record's text: a line longer than this is cut (klog), or split over records that a reader
   joins again (an app's lines, the console). */
#define LOG_TEXT_MAX 4096

/* CPU 0 at boot, after vmm_init and before another CPU starts: every CPU's ring, sized from the
   memory and the CPUs. Until then the records go to a ring of the boot's own. */
void log_init(void);
/* CPU 0 at boot, after the APIC, the IOAPIC and the scheduler and before another CPU starts: from
   now on a writer tells the readers by an interrupt to itself, the boot screen draws in its own
   thread and the serial port sends from its interrupt. Until then the boot screen draws as each
   record is written (one CPU, interrupts off). */
void log_start(void);
/* A sleep (dev/pm.c), CPU 0 with interrupts off and the other CPUs parked. log_suspend, before
   the interrupt controllers go down: the writers stop telling the readers (no interrupt to itself
   reaches a CPU whose APIC is not back), the serial port goes quiet. log_resume, once the APICs
   and the IOAPICs are back: the interrupts that were on their way are gone with the CPUs' state,
   the serial port's registers are set again, and the readers learn of what was written meanwhile.
   The rings are in memory as they were. */
void log_suspend(void);
void log_resume(void);

/* Text that is not lines of the log: an app's lines to the system log (level: LOG_INFO), and the
   console's output (log_console), which the serial port and the boot screen show and the
   system log's readers do not. Any CPU, not with interrupts off for long: n bytes are copied. */
void log_text(enum log_level level, const char *s, size_t n);
void log_console(const char *s, size_t n);

/* A reader's place. LOG_CURSOR_CONSOLE: the console's text too; LOG_CURSOR_CRLF: "\r\n" for each
   line's end (a terminal). A new cursor is before the oldest record held. One reader at a time:
   its caller keeps others out. */
#define LOG_CURSOR_CONSOLE 1u
#define LOG_CURSOR_CRLF 2u
struct log_cursor;
struct log_cursor *log_cursor_new(unsigned flags); /* NULL: no memory */
void log_cursor_free(struct log_cursor *c);
void log_cursor_rewind(struct log_cursor *c); /* before the oldest record held again */
/* Up to cap bytes of text from the cursor on, the cursor moved past them: 0 when no record past it
   is whole yet. Any context; it only reads the rings. */
size_t log_read(struct log_cursor *c, char *out, size_t cap);
/* A hint, reading only: whether log_read may give text now. */
bool log_pending(const struct log_cursor *c);
/* The newest text from the cursor on, at most max bytes of it (the cursor read to the end). */
size_t log_newest(struct log_cursor *c, char *buf, size_t max);
/* The bytes all the rings hold: as much as n_system_log can give. */
size_t log_capacity(void);
/* The cursor a panic reads with, made when the rings are (log_init): the boot ring's own until
   then. Only the panic uses it. */
struct log_cursor *log_panic_cursor(void);

/* Waiting for records: read log_told before looking, then log_wait(seen): asleep until records
   are written after that look, or the deadline (sys/wait.h); false when the deadline came. */
uint64_t log_told(void);
bool log_wait(uint64_t seen, uint64_t deadline);
