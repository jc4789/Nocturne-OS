/* Watches (N_KIND_WATCH, docs/nocturne-native-abi.md §1): a handle that turns N_EVENT_RECEIVABLE
   once what it watches has changed, until SYS_WATCH_TAKE takes the count. Whatever is watched (a
   part of the system's state, a name or a folder on C:) keeps a struct watched and says when it
   changed; every handle opened on one counts for itself from its opening, so one struct serves
   any number of watchers.

   Any CPU, any context: watched_changed and watched_gone do not sleep and take no lock but the
   object's (object_notify), so an owner may call them with its own lock held. The counts are
   atomic words: what the owner said is never lost to a take on another CPU. */
#pragma once
#include "sys/object.h"

struct watched {
    struct object object;
    uint64_t changes; /* the changes its owner said, ever (atomic) */
    bool gone;        /* what it watched is no more: its handles turn N_EVENT_CLOSED (atomic) */
    /* The last handle on it closed (NULL: its owner keeps it for good). */
    void (*release)(struct watched *w);
};

void watched_init(struct watched *w, void (*release)(struct watched *w));
/* A new handle on w in the calling app's table: its number, or a negative N_ERR_*. */
int64_t watched_open(struct watched *w);
/* What w watches changed once more: every handle on it becomes receivable. */
void watched_changed(struct watched *w);
/* What w watches is no more: its handles turn N_EVENT_CLOSED; a take gives what was left, then
   N_ERR_CLOSED. */
void watched_gone(struct watched *w);
/* SYS_WATCH_TAKE: the changes since this handle's last take (0: none), at most
   N_WATCH_CHANGES_MAX. */
int64_t watch_take(int index);
