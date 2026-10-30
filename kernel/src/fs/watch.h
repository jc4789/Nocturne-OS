/* Watches on C: (n_entry_watch, docs/nocturne-native-abi.md §3): struct watched (sys/watch.h) on
   a name in a folder or on everything directly in a folder, told of each change a new opening,
   n_entry_info or n_folder_list sees. The handles come from store_entry_watch (fs/store.h). */
#pragma once
#include "store/namespace.h"

/* What one change told (store/namespace.h), from whatever made it: a change to a folder's names
   tells it with that folder still locked (fs/document.h), so that a watch made meanwhile on it
   counts every change from its opening on. Any CPU; never sleeps. */
void entry_watch_publish(const struct ns_told *t);
