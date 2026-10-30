/* The catalog: the volume's one B+tree of entries, records and extents (common/store_format.h).
   Keys compare as bytes. A change copies the nodes from the root to its leaf into the open
   generation the first time it touches them; the commit places them (catalog_freeze).

   Any number of threads read and change it at once. Each call keeps the tree whole by itself, with
   the volume's tree latch (shared by reads, and by a change that only touches a leaf of the open
   generation; alone for one that copies or splits or joins nodes) and each leaf's own latch; none
   is held when the call returns, nor while it waits for the medium: a node that is not in memory
   is read with every latch let go, and the call starts again from the root. Calls never wait for
   each other but for the moment a latch is held. */
#pragma once
#include "store/volume.h"

struct item {
    uint8_t key[STORE_KEY_MAX];
    size_t klen;
    uint8_t value[STORE_VALUE_MAX];
    size_t vlen;
};

/* The empty tree of a new volume; the root of a mounted one read (the one thread that has the
   volume yet). */
int catalog_format(struct volume *v);
int catalog_check(struct volume *v);
/* The nodes of the open generation go (volume_free). */
void catalog_destroy(struct volume *v);

/* key's value into value (cap bytes): its whole length, or -N_ERR_NOT_FOUND. */
int catalog_get(struct volume *v, const uint8_t *key, size_t klen, void *value, size_t cap);
/* key gets value, replacing what it had; catalog_insert gives -N_ERR_EXISTS instead. A change that
   fails changes nothing the tree holds. */
int catalog_put(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen);
int catalog_insert(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen);
int catalog_delete(struct volume *v, const uint8_t *key, size_t klen);
/* key's value as change makes it from the one it has, with nothing else changing it between:
   change gets a copy (n bytes in room for STORE_VALUE_MAX) and gives its new length, or an error
   that leaves it as it was. -N_ERR_NOT_FOUND when key has none. change may be called more than
   once, each time with the value as it is then: only what its last call made is kept. It neither
   waits nor takes a lock. */
typedef int catalog_change(void *ctx, uint8_t *value, size_t n);
int catalog_amend(struct volume *v, const uint8_t *key, size_t klen, catalog_change *change, void *ctx);
/* key's way from the root into the open generation, nothing it holds changed. Until the next commit,
   whatever else changes meanwhile, a get, a delete or an amend at key needs no memory and no read,
   nor does a put or an amend of a value no longer than the one key has: none of them can fail. */
int catalog_ready(struct volume *v, const uint8_t *key, size_t klen);
/* The first item from key on (after it, unless equal) whose key is below limit, or the last one
   at key or before it and from floor on. -N_ERR_NOT_FOUND when there is none. */
int catalog_next(struct volume *v, const uint8_t *key, size_t klen, bool equal, const uint8_t *limit, size_t llen,
                 struct item *out);
int catalog_last(struct volume *v, const uint8_t *key, size_t klen, const uint8_t *floor, size_t flen, struct item *out);

/* Every node (its address and level) and every item, in key order, each node checked against the
   keys its parent gives it: a callback's negative result ends the walk with it. Nothing may change
   the catalog meanwhile; the walk waits for the medium. */
struct catalog_visit {
    int (*node)(void *ctx, uint64_t address, unsigned level);
    int (*item)(void *ctx, const struct item *it);
};
int catalog_walk(struct volume *v, const struct catalog_visit *visit, void *ctx);

/* The commit (volume_freeze, with the tree latch held alone): every node of the open generation
   gets the next block of blocks (held: v->temp_nodes of them), its block is sealed, and it goes to
   the volume's dirty list. */
int catalog_freeze(struct volume *v, uint64_t generation, const struct ranges *blocks);
