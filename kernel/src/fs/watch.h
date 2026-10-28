/* Watches on C: (n_entry_watch, docs/nocturne-native-abi.md §3): struct watched (sys/watch.h) on
   a name in a folder or on everything directly in a folder, told by the namespace (struct ns_change)
   of each change a new opening, n_entry_info or n_folder_list sees. The handles come from
   store_entry_watch (fs/store.h). Everything here runs with store.v->lock held, on the BSP. */
#pragma once
#include "fs/namespace.h"

/* The kernel's C: tells its watches of every change from now on. */
void entry_watch_start(struct volume *v);
/* The changes of the namespace from entry_watch_together to entry_watch_apart are one change to
   whatever watches them: a save that makes its name, then takes its content in under it. */
void entry_watch_together(void);
void entry_watch_apart(void);
