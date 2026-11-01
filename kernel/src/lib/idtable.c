/* Tables by id (lib/idtable.h). */
#include "lib/idtable.h"
#include "kernel.h"
#include "mm/heap.h"

#define FIRST_SIZE 16u

static size_t slot(uint64_t id, size_t size) { return (size_t)((id * 0x9e3779b97f4a7c15ull) >> 32) & (size - 1); }

int id_table_room(struct id_table *t, struct spinlock *lock, uint64_t *flags) {
    *flags = spin_lock(lock);
    while (t->count >= t->size) {
        size_t size = t->size ? t->size * 2 : FIRST_SIZE;
        spin_unlock(lock, *flags);
        struct id_link **bucket = kzalloc(size * sizeof *bucket), **old = NULL;
        *flags = spin_lock(lock);
        if (!bucket) return t->size ? 0 : -N_ERR_NO_MEMORY;
        if (t->size < size) { /* nobody grew it meanwhile */
            for (size_t i = 0; i < t->size; i++)
                for (struct id_link *l = t->bucket[i], *next; l; l = next) {
                    next = l->next;
                    struct id_link **at = &bucket[slot(l->id, size)];
                    l->next = *at;
                    *at = l;
                }
            old = t->bucket;
            t->bucket = bucket;
            t->size = size;
            bucket = NULL;
        }
        if (bucket || old) {
            spin_unlock(lock, *flags);
            kfree(bucket);
            kfree(old);
            *flags = spin_lock(lock);
        }
    }
    return 0;
}

void id_table_add(struct id_table *t, struct id_link *l) {
    struct id_link **at = &t->bucket[slot(l->id, t->size)];
    l->next = *at;
    *at = l;
    t->count++;
}

void id_table_drop(struct id_table *t, struct id_link *l) {
    struct id_link **at = &t->bucket[slot(l->id, t->size)];
    while (*at != l) at = &(*at)->next;
    *at = l->next;
    l->next = NULL;
    t->count--;
}

struct id_link *id_table_find(const struct id_table *t, uint64_t id) {
    if (!t->size) return NULL;
    for (struct id_link *l = t->bucket[slot(id, t->size)]; l; l = l->next)
        if (l->id == id) return l;
    return NULL;
}

uint64_t id_table_scan(const struct id_table *t, uint64_t cursor, void (*fn)(struct id_link *l, void *arg), void *arg) {
    if (!t->size) return 0;
    uint64_t mask = t->size - 1;
    for (struct id_link *l = t->bucket[cursor & mask]; l; l = l->next) fn(l, arg);
    /* the next index in reverse binary order: the cursor's high bits count up from the top bit down */
    cursor |= ~mask;
    return __builtin_bitreverse64(__builtin_bitreverse64(cursor) + 1);
}

struct id_link *id_table_next(const struct id_link *l) {
    for (struct id_link *n = l->next; n; n = n->next)
        if (n->id == l->id) return n;
    return NULL;
}
