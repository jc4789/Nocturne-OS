#pragma once
#include <stddef.h>
#include <stdbool.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);         /* user-visible console output */
void fbcon_write_kernel(const char *s, size_t n);  /* kernel log (suppressed once GUI runs) */
void fbcon_set_enabled(bool on);
bool fbcon_enabled(void);
void fbcon_clear(void);
