#pragma once
#include <stdbool.h>
#include <stddef.h>
void serial_init(void);
void serial_write(const char *s, size_t n);
void serial_seize(void);      /* panic_enter: the line's lock, whoever held it */
void serial_flush(void);       /* panic, power-off, reboot, sleep: send all of it, waiting */
