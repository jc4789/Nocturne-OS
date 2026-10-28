/* The user's settings (docs/nocturne-native-abi.md §12).

   Each setting has its owner, which applies it: the desktop the pointer's speed and smooth
   gradients (gui/wm.c), the screen's mode the mode chosen for each monitor (gui/screenmode.c),
   sound the volume and muting (dev/audio.c). The languages the user reads have no other owner and
   are kept here. This part keeps them all on C: in the user's AppData\Nocturne\Settings.conf and
   tells the apps: at the start it puts the saved values back into their owners; afterwards an owner
   says when the user changed one (user_settings_changed), every watch whose interest has it becomes
   receivable, and the change goes to C: once the user has stopped changing it for a moment, and at
   once before the power goes (user_settings_flush). No app writes the file. */
#pragma once
#include "kernel.h"

/* C: and the user's folder are ready (store_start) and the desktop is set up (wm_init, when it
   runs): the saved settings go back to their owners, and changes are kept from now on. */
void user_settings_start(void);
/* The owner of the settings bits names (N_USER_SETTING_*) changed one: the watches are told and
   it is saved. On the BSP, with no lock of this part held; any lock of the owner may be. */
void user_settings_changed(uint32_t bits);
/* What changed and is not on C: yet goes there now (before store_sync, at a power change). */
void user_settings_flush(void);

/* The system calls (sys/syscall.c). */
int64_t user_settings_watch(uint32_t interest);
int64_t user_settings_changes(int watch);
int64_t user_languages(char *user_out, uint64_t cap);
/* The same on an AP; false: the BSP serves it (the app's memory needs it). */
bool user_languages_ap(char *user_out, uint64_t cap, int64_t *result);
int64_t user_languages_set(const char *user_list, uint64_t length);
