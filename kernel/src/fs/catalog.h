/* The catalog: the volume's one B+tree of entries, records and extents (common/store_format.h).
   Keys compare as bytes. A change copies the nodes from the root to its leaf into the open
   generation the first time it touches them; the commit places them (catalog_freeze). The caller
   holds v->lock. */
#pragma once
#include "fs/volume.h"

struct item {
    uint8_t key[STORE_KEY_MAX];
    size_t klen;
    uint8_t value[STORE_VALUE_MAX];
    size_t vlen;
};

int catalog_format(struct volume *v);
int catalog_check(struct volume *v);

/* key's value into value (cap bytes): its whole length, or -N_ERR_NOT_FOUND. */
int catalog_get(struct volume *v, const uint8_t *key, size_t klen, void *value, size_t cap);
/* key gets value, replacing what it had; catalog_insert gives -N_ERR_EXISTS instead. A change that
   fails changes nothing the tree holds. */
int catalog_put(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen);
int catalog_insert(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen);
int catalog_delete(struct volume *v, const uint8_t *key, size_t klen);
/* key's way from the root into the open generation, nothing it holds changed. Until the next commit,
   whatever else changes meanwhile, a get or a delete at key needs no memory and no read, nor does a
   put of a value no longer than the one key has: none of them can fail. */
int catalog_ready(struct volume *v, const uint8_t *key, size_t klen);
/* The first item from key on (after it, unless equal) whose key is below limit, or the last one
   at key or before it and from floor on. -N_ERR_NOT_FOUND when there is none. */
int catalog_next(struct volume *v, const uint8_t *key, size_t klen, bool equal, const uint8_t *limit, size_t llen,
                 struct item *out);
int catalog_last(struct volume *v, const uint8_t *key, size_t klen, const uint8_t *floor, size_t flen, struct item *out);

/* Every node (its address and level) and every item, in key order, each node checked against the
   keys its parent gives it: a callback's negative result ends the walk with it. */
struct catalog_visit {
    int (*node)(void *ctx, uint64_t address, unsigned level);
    int (*item)(void *ctx, const struct item *it);
};
int catalog_walk(struct volume *v, const struct catalog_visit *visit, void *ctx);

/* The commit: every node changed in generation gets its block and goes to the volume's unwritten
   list. */
int catalog_freeze(struct volume *v, uint64_t generation);
