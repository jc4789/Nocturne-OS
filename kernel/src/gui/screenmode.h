#pragma once
#include "kernel.h"

/* The monitor's mode (docs/nocturne-native-abi.md §9): the display driver offers the modes it can
   show on its monitor (n_pci_screen_modes, dev/pciuser.c, the only caller of the first two, one
   call at a time); the system chooses the one it shows: a mode on trial, else the user's choice
   for that monitor, else the monitor's preferred mode; and tells the driver through the screen's
   state (wm_screen_mode, gui/wm.c). The Settings app lists the modes, tries one, keeps it or goes
   back (SYS_SCREEN_INFO, display_abi.h); a mode tried and not kept goes back by itself. The mode
   kept for each monitor is the user's: it is kept in the user's AppData\Nocturne\Displays.conf
   (sys/keep.c), which screen_modes_init registers to be read back by keep_start.
   screen_modes_offer: the driver's monitor, its modes and what it showed (a kernel copy);
   count 0: it drives none any more, though it still runs (it may have given the screen back).
   screen_modes_gone: the same, for a driver whose claim ended. */
struct n_screen_modes;
void screen_modes_init(void); /* before keep_start (kmain_late) */
void screen_modes_offer(const struct n_screen_modes *m);
void screen_modes_gone(void);
/* The refresh (millihertz) of the mode the driver says its head shows; 0 while it says none. Any
   thread, no lock (the compositor's frame period, gui/wm.c). */
uint32_t screen_shown_refresh_mhz(void);
/* SYS_SCREEN_INFO with op SCREEN_MODES_GET or SCREEN_MODE_SET: user is the app's struct of size
   bytes. Takes no WM lock of its own (it comes before the WM's). */
int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op);
