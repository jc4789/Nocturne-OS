#pragma once
#include "kernel.h"

/* The monitor's mode (docs/nocturne-native-abi.md §9): the display driver offers the modes it can
   show on its monitor (n_pci_screen_modes, dev/pciuser.c, the only caller of the first two, one
   call at a time); the system chooses the one it shows: a mode on trial, else the user's choice
   for that monitor, else the monitor's preferred mode; and tells the driver through the screen's
   state (wm_screen_mode, gui/wm.c). The Settings app lists the modes, tries one, keeps it or goes
   back (SYS_SCREEN_INFO, display_abi.h); a mode tried and not kept goes back by itself. The mode
   kept for a monitor is a user's setting, which the system keeps on C: (sys/usersettings.c).
   screen_modes_offer: the driver's monitor, its modes and what it showed (a kernel copy);
   count 0: it drives none any more, though it still runs (it may have given the screen back).
   screen_modes_gone: the same, for a driver whose claim ended. */
struct n_screen_modes;
void screen_modes_offer(const struct n_screen_modes *m);
void screen_modes_gone(void);
/* The refresh (millihertz) of the mode the driver says its head shows; 0 while it says none. Any
   thread, no lock (the compositor's frame period, gui/wm.c). */
uint32_t screen_shown_refresh_mhz(void);
/* The user's choice of mode for a monitor (its struct n_screen_modes monitor text), put back from
   the user's settings: shown whenever that monitor is driven and offers it. Tells no app. */
struct n_screen_mode;
void screen_modes_choose(const char *monitor, const struct n_screen_mode *mode);
/* The i'th monitor the user chose a mode for and that mode, for keeping them; false past the last. */
bool screen_modes_choice(unsigned i, char monitor[32], struct n_screen_mode *mode);
/* SYS_SCREEN_INFO with op SCREEN_MODES_GET or SCREEN_MODE_SET: user is the app's struct of size
   bytes. Takes no WM lock of its own (it comes before the WM's). */
int64_t screen_modes_syscall(void *user, uint64_t size, uint64_t op);
