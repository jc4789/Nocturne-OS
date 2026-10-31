/* A table of things found by a 64-bit id (a document's, a folder's): each keeps a struct id_link,
   chained in buckets that double once there are as many things as buckets. Many may share an id.

   The table is its owner's to guard with a spinlock. It never allocates under it: id_table_room
   grows the buckets with the lock let go, then gives it back taken. */
#pragma once
#include "arch/spin.h"
#include <stddef.h>
#include <stdint.h>

struct id_link {
    struct id_link *next;
    uint64_t id;
};

struct id_table {
    struct id_link **bucket; /* a power of two of them */
    size_t size, count;
};

/* Takes lock (*flags as spin_lock gives them) with room in t for one more link: 0, or
   -N_ERR_NO_MEMORY when t has no buckets at all and none could be made. Without memory to grow,
   the chains grow longer. Whatever the caller saw under lock before may have changed. */
int id_table_room(struct id_table *t, struct spinlock *lock, uint64_t *flags);
void id_table_add(struct id_table *t, struct id_link *l);
void id_table_drop(struct id_table *t, struct id_link *l);
/* The first link with id (NULL: none), and the next one with the same id after l. */
struct id_link *id_table_find(const struct id_table *t, uint64_t id);
struct id_link *id_table_next(const struct id_link *l);
