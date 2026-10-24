/* A mounted store volume (common/store_format.h): the device under it, the blocks it keeps in
   memory, and its commits. The kernel and the host tool (host/store) share this layer, the space
   map (fs/space.c), the catalog (fs/catalog.c) and the namespace (fs/namespace.c).

   Changes are made in memory, in the open generation, and reach the medium together in a commit.
   The volume's caller serializes everything but content reads and writes (v->lock, held around
   every call into these layers); block reads and writes of content need no lock. */
#pragma once
#include "kernel.h"
#include "store_format.h"
#include "fs/names.h"
#include "sys/sched.h"

/* What the volume is on: a partition, a disk image, memory. Requests are in STORE_BLOCKs. */
struct store_device {
    uint64_t blocks;
    int (*read)(struct store_device *d, uint64_t block, uint32_t count, void *buf);
    int (*write)(struct store_device *d, uint64_t block, uint32_t count, const void *buf);
    int (*persist)(struct store_device *d); /* everything written is on the medium */
    void *priv;
};

/* A block in memory. Content blocks never change once written (copy on write): a block address
   names the same bytes until it is freed. */
enum { BLOCK_LOADING, BLOCK_CLEAN, BLOCK_DIRTY, BLOCK_UNWRITTEN, BLOCK_FAILED };
struct cblock {
    uint64_t address; /* TEMP_ADDRESS | n for a catalog node not yet placed */
    uint8_t *data;
    uint32_t refs;
    uint8_t state;
    struct cblock *hash_next, *older, *newer; /* the hash chain; the LRU list of unused ones */
    struct cblock *next_unwritten;
};
#define TEMP_ADDRESS (1ULL << 63)

struct range {
    uint64_t start, count;
};
/* A growing list of block ranges. */
struct ranges {
    struct range *r;
    size_t n, room;
};
int ranges_add(struct ranges *list, uint64_t start, uint64_t count);
void ranges_free(struct ranges *list);

struct space {
    struct store_layout layout;
    struct store_space_count *counts; /* one per bitmap block */
    uint8_t **maps;                   /* the bitmap blocks read so far (NULL: as on the medium) */
    uint64_t *index_changed;          /* the generation that last changed each index block */
    uint64_t used, held, reserved;
    struct ranges holds;              /* free in the bitmap, not to be given out (sorted, apart) */
    struct pending { uint64_t start, count, generation; } *pending; /* holds that end with a commit */
    size_t npending, pending_room;
    uint64_t cursor;
};

struct volume {
    struct store_device *dev;
    struct mutex lock;
    struct store_super super;       /* the last commit's */
    uint32_t seed;
    uint64_t generation;            /* the open one: the next commit */
    uint64_t durable;               /* the last generation on the medium */
    uint64_t root, root_generation; /* the catalog's root node */
    uint64_t next_id, next_temp;
    bool changed;                   /* anything to commit */
    int error;                      /* a failed write: the volume is read only from then on */
    struct fold fold;
    struct space space;
    /* the cache */
    struct cblock **hash;
    size_t hash_size, blocks, unused, limit;
    struct cblock *oldest, *newest;  /* unused clean blocks, least recently used first */
    struct cblock *unwritten;        /* blocks the next commit writes first */
    size_t unwritten_count;          /* of them: memory that only a commit gives back */
    struct wait_queue loaded;        /* waits for blocks being read */
    struct wait_queue committed;     /* waits for commits */
    bool committing;
};

uint32_t store_seed(const uint8_t volume_id[16]);
/* The wall clock in nanoseconds since 1970 UTC: the kernel's or the host's (fs/store.c,
   host/store). */
int64_t store_clock(void);

/* Mounts the volume on dev, or makes a new empty one on it (label, a random volume_id). */
int volume_mount(struct store_device *dev, struct volume **out);
int volume_format(struct store_device *dev, const char *label, const uint8_t volume_id[16], struct volume **out);
void volume_free(struct volume *v);
/* Writes the open generation and everything before it to the medium. Takes v->lock itself, and
   lets other changes go on in the next generation while the blocks are written. */
int volume_commit(struct volume *v);
/* Growing the volume to blocks, once its device is that large (common/store_format.h): after
   volume_even, which leaves the last commit an even generation's (a commit with nothing new when
   it is not), while the device has the size before; then volume_grow, in an odd generation's
   commit and the one after it. Each takes v->lock itself. */
int volume_even(struct volume *v);
int volume_grow(struct volume *v, uint64_t blocks);
/* -N_ERR_READ_ONLY once a write failed, else 0: every change asks first. */
int volume_writable(struct volume *v);

/* The cache. A block is held (refs) between get and put. */
struct cblock *cache_find(struct volume *v, uint64_t address);
/* The block at address, read when it is not in memory; valid (NULL: anything is) judges what was
   read, with expect (a node's generation, a content block's CRC). NULL with *error on failure. */
typedef bool block_check(struct volume *v, const uint8_t *data, uint64_t address, uint64_t expect);
struct cblock *cache_read(struct volume *v, uint64_t address, block_check *valid, uint64_t expect, int *error);
bool content_valid(struct volume *v, const uint8_t *data, uint64_t address, uint64_t check);
/* count blocks from start (expect[i] for the i'th; expect NULL: no checks), the missing ones read
   in as few requests as they allow. On failure out holds nothing. */
int cache_read_many(struct volume *v, uint64_t start, uint32_t count, block_check *valid, const uint32_t *expect,
                    struct cblock **out);
/* A block that is about to get new content at address (zeros, or data, a kmalloc'd STORE_BLOCK
   the block takes): whatever was there is forgotten. */
struct cblock *cache_new(struct volume *v, uint64_t address, int *error);
struct cblock *cache_take(struct volume *v, uint64_t address, uint8_t *data, int *error);
void cache_put(struct volume *v, struct cblock *b);
void cache_rename(struct volume *v, struct cblock *b, uint64_t address);
/* A block changed in memory that nothing will name: gone with b's reference. */
void cache_discard(struct volume *v, struct cblock *b);
/* Freed blocks: nothing kept of them. */
void cache_forget(struct volume *v, uint64_t start, uint64_t count);
/* A content block with its bytes: the next commit writes it before its catalog. Takes b's
   reference. */
void cache_unwritten(struct volume *v, struct cblock *b);

/* Block heads (catalog nodes, space blocks). */
void head_seal(struct volume *v, void *block, uint32_t magic, uint64_t address, uint64_t generation);
bool head_valid(struct volume *v, const void *block, uint32_t magic, uint64_t address, uint64_t generation);
