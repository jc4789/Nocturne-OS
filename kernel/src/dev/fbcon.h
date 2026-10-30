#pragma once
#include <stddef.h>
#include <stdbool.h>
void fbcon_init(void);
void fbcon_write(const char *s, size_t n);         /* user-visible console output */
void fbcon_write_kernel(const char *s, size_t n);  /* kernel log (suppressed once GUI runs) */
void fbcon_set_enabled(bool on);
void fbcon_repaint(void); /* the screen lost the console (a display driver held it): draw all of it again */
bool fbcon_enabled(void);
void fbcon_clear(void);
void fbcon_seize(void); /* panic_enter: the console's lock, whoever held it */
