#pragma once
#include "kernel.h"

/* The monitors' modes (docs/nocturne-native-abi.md §9). For each screen (dev/fb.h) a display
   driver offers the modes it can show on the output's monitor (n_pci_screen_modes, dev/pciuser.c,
   the only caller of the offers, one call at a time); the system chooses the one that screen
   shows: a mode on trial, else the user's choice for that monitor, else the monitor's preferred
   mode; and tells the driver through the screen's state (wm_screen_mode, gui/wm.c). The Settings
   app lists the modes of each screen, tries one, keeps it or goes back (SYS_SCREEN_INFO,
   display_abi.h); a mode tried and not kept goes back by itself. The mode kept for each monitor
   is the user's: it is kept in the user's AppData\Nocturne\Displays.conf (sys/keep.c), which
   screen_modes_init registers to be read back by keep_start.
   screen_modes_offer: the monitor on screen's output, its modes and what it showed (a kernel
   copy); count 0: it drives none any more, though it still runs (it may have given the screen
   back). screen_modes_gone: the same, for a driver whose claim ended. screen_modes_shown: what the
   driver said of the mode a screen is given at (its refresh, its variable refresh range, the
   monitor), into desc, whose width and height are the buffers'. */
struct n_screen_modes;
struct screen_mode_desc;
void screen_modes_init(void); /* before keep_start (kmain_late) */
void screen_modes_offer(unsigned screen, const struct n_screen_modes *m);
void screen_modes_gone(unsigned screen);
void screen_modes_shown(unsigned screen, struct screen_mode_desc *desc);
/* SYS_SCREEN_INFO with op SCREEN_MODES_GET or SCREEN_MODE_SET (and the screen in its bits from 8
   on): user is the app's struct of size bytes. Takes no WM lock of its own (it comes before the
   WM's). */
int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op);
