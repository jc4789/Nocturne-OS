/* The boot screen (dev/fbcon.c): one reader of the log (sys/log.h), its records and the console's
   text drawn on the firmware's framebuffer until the window manager takes the screen; and the
   panic screen, drawn on whatever the screen shows then. */
#pragma once
#include <stdbool.h>

struct panic_line;
/* CPU 0 at boot, after fb_init and log_init: its canvas, kept for the panic screen, and what the
   log holds so far drawn. */
void fbcon_init(void);
/* sys/log.c before log_start, interrupts off: what was written since, drawn now. */
void fbcon_boot_draw(void);
/* sys/log.c's log_start, CPU 0: from now on the boot screen draws in a thread of its own, woken
   by the log's readers' interrupt. */
void fbcon_start(void);
/* The window manager took the screen (gui/wm.c): nothing is drawn on it any more but the panic
   screen. */
void fbcon_stop(void);
/* The screen lost what the boot screen drew there (a display driver held it, dev/pciuser.c):
   all of it again, while the boot screen is shown. */
void fbcon_repaint(void);
/* sys/panic.c: the report and the log's newest lines, straight to the screen shown. */
void fbcon_panic(const struct panic_line *lines, unsigned count);
