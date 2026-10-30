/* The store: Nocturne's volume format (common/store_format.h) and the code that reads and changes a
   volume of it, for any program that keeps one: the kernel's C: (kernel/src/fs) and the build host's
   tool (host/store). This code is a library: it has no locks of its own program's, no waits, no log
   and no clock. What it is given, it is given by the program that opens a volume:

   - the medium (struct store_device): reads and writes of blocks, which may wait;
   - the blocks in memory (struct store_supply): the program's cache of the medium, which finds a
     block without waiting and reads one that is not there, waiting;
   - memory (struct store_env), and whom to tell of trouble on the volume.

   Changes are made in memory, in the open generation, and reach the medium together in a commit.
   Many threads may read and change a volume at once: the store keeps its own data whole with
   latches (store/latch.h), never held across a wait, so it reads what is not in memory with every
   latch let go and starts again. What it cannot see from one change at a time is the program's:
   that no change is under way while a commit takes the open generation (volume_freeze), and that
   the changes of a folder's names and of a document's content come one at a time (store/
   namespace.h says which). */
#pragma once
#include "abi.h"
#include "store_format.h"
#include "store/latch.h"
#include "store/names.h"

#define STORE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define STORE_MAX(a, b) ((a) > (b) ? (a) : (b))

struct volume;

/* What the store found wrong on its volume, for whoever looks after the machine. The volume tells
   the program each one with the block it is about, or the error it came to. */
enum store_trouble {
    STORE_TROUBLE_CONTENT,     /* a block does not hold what the catalog says (block) */
    STORE_TROUBLE_NODE,        /* a catalog node cannot be read (block, error) */
    STORE_TROUBLE_ORDER,       /* a catalog node's keys are out of order (block) */
    STORE_TROUBLE_MAP,         /* a space map block is damaged (block) */
    STORE_TROUBLE_INDEX,       /* a space index block is damaged (block) */
    STORE_TROUBLE_SUM,         /* the space map does not add up */
    STORE_TROUBLE_FOLD,        /* the case-folding table cannot be read (error) */
    STORE_TROUBLE_OLDER,       /* the last commit is damaged: the volume is as it was one before */
    STORE_TROUBLE_PREPARE,     /* a commit could not be prepared (error): read only from now on */
    STORE_TROUBLE_WRITE,       /* a commit could not be written (error): read only from now on */
    STORE_TROUBLE_UNFINISHED,  /* a change could not be finished (error): read only from now on */
    STORE_TROUBLE_RULE,        /* the store broke a rule of its own (block): read only from now on */
};
/* A line telling what t is, without its block or error. */
const char *store_trouble_text(enum store_trouble t);

/* The program's. The memory functions are its own allocator's: NULL when there is none; what alloc
   gives holds anything until it is written. trouble is called without waiting. */
struct store_env {
    void *(*alloc)(size_t n);
    void *(*realloc)(void *p, size_t n);
    void (*free)(void *p);
    void (*trouble)(enum store_trouble t, uint64_t block, int error);
};

/* One more item at the end of a growing array (*array_at, *count items, room for *room): its
   place, or NULL without memory (the array as it was). */
void *store_array_add(const struct store_env *env, void *array_at, size_t *count, size_t *room, size_t size);

/* What the volume is on: a partition, a disk image, memory. Requests are in STORE_BLOCKs; they may
   wait, so the store makes them with no latch held. */
struct store_device {
    uint64_t blocks;
    int (*read)(struct store_device *d, uint64_t block, uint32_t count, void *buf);
    int (*write)(struct store_device *d, uint64_t block, uint32_t count, const void *buf);
    int (*persist)(struct store_device *d); /* everything written is on the medium (NULL: at once, as in memory) */
};

/* A block in memory, the supply's: its address and STORE_BLOCK bytes, and a latch the store keeps
   for what the block holds (a catalog leaf's items; free when the supply gives a block it did not
   have). Content blocks never change once written (copy on write): an address names the same bytes
   until the block is freed. A block is held from the call that gives it to its put; the supply
   keeps every held block, and gives it back to memory only after its last put. */
struct store_block {
    uint64_t address; /* TEMP_ADDRESS | n for a catalog node not yet placed */
    uint8_t *data;
    struct latch latch;
};
#define TEMP_ADDRESS (1ULL << 63)

/* What was read is what the volume holds there: a node's generation, a content block's check. */
typedef bool store_block_check(struct volume *v, const uint8_t *data, uint64_t address, uint64_t expect);

/* The program's blocks in memory, over the volume's device. */
struct store_supply {
    /* The block at address, held, when memory has it whole; NULL when it does not or it is being
       read. Never waits. */
    struct store_block *(*find)(struct store_supply *s, uint64_t address);
    /* The block at address, read from the device when memory does not have it, and checked (valid
       NULL: anything is); may wait. NULL with *error on failure. */
    struct store_block *(*read)(struct store_supply *s, struct volume *v, uint64_t address, store_block_check *valid,
                                uint64_t expect, int *error);
    /* count blocks from start (expect[i] for the i'th, NULL: no checks), the missing ones read in as
       few requests as they allow; may wait. On failure out holds nothing. */
    int (*read_many)(struct store_supply *s, struct volume *v, uint64_t start, uint32_t count,
                     store_block_check *valid, const uint32_t *expect, struct store_block **out);
    /* A block with new content at address (data: STORE_BLOCK bytes from the env's alloc that the
       block takes, the caller's still when it fails; NULL: zeros), held; whatever memory had at
       address is forgotten. Never waits. */
    struct store_block *(*make)(struct store_supply *s, uint64_t address, uint8_t *data, int *error);
    void (*put)(struct store_supply *s, struct store_block *b);
    /* A held block's new address; whatever memory had there is forgotten. */
    void (*rename)(struct store_supply *s, struct store_block *b, uint64_t address);
    /* A held block that nothing will name: gone with its last put. */
    void (*discard)(struct store_supply *s, struct store_block *b);
    /* Blocks given back to the volume's free space: nothing kept of them. */
    void (*forget)(struct store_supply *s, uint64_t start, uint64_t count);
};

struct range {
    uint64_t start, count;
};
/* A growing list of block ranges. */
struct ranges {
    struct range *r;
    size_t n, room;
};
int ranges_add(const struct store_env *env, struct ranges *list, uint64_t start, uint64_t count);
void ranges_free(const struct store_env *env, struct ranges *list);

struct space {
    struct latch latch;               /* everything below */
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
    const struct store_env *env;
    struct store_device *dev;
    struct store_supply *blocks;
    struct store_super super;       /* the last commit's (volume_written) */
    uint32_t seed;
    uint64_t durable;               /* the last generation on the medium */
    /* The tree's latch: the catalog's root and its branches (store/catalog.h), and the open
       generation, which a commit closes holding it alone. */
    struct latch tree;
    uint64_t generation;            /* the open one: the next commit (atomic) */
    uint64_t root, root_generation; /* the catalog's root node */
    uint64_t next_id, next_temp;    /* taken atomically */
    size_t temp_nodes;              /* the catalog's nodes of the open generation (atomic) */
    bool changed;                   /* anything to commit (atomic) */
    int error;                      /* read only from the first failed write or broken rule (atomic) */
    struct fold fold;
    struct space space;
    /* Blocks the next commit writes first, held, and room kept for more (by their latch:
       dirty_latch). */
    struct latch dirty_latch;
    struct store_block **dirty;
    size_t ndirty, dirty_room, dirty_reserved;
};

uint32_t store_seed(const uint8_t volume_id[16]);

/* Mounts the volume on dev, or makes a new empty one on it (label, a random volume_id, its creation
   time `now` in nanoseconds since 1970 UTC). The volume keeps env, dev and blocks. */
int volume_mount(const struct store_env *env, struct store_device *dev, struct store_supply *blocks, struct volume **out);
int volume_format(const struct store_env *env, struct store_device *dev, struct store_supply *blocks, const char *label,
                  const uint8_t volume_id[16], int64_t now, struct volume **out);
/* Everything the volume holds in memory goes (its held blocks put); no one may use it any more. */
void volume_free(struct volume *v);

/* -N_ERR_READ_ONLY once a write failed or a rule broke, else 0: every change asks first. */
int volume_writable(struct volume *v);
/* Something went part of the way, or broke a rule of the store's (STORE_TROUBLE_UNFINISHED,
   STORE_TROUBLE_RULE): what was done stays in memory, so nothing more may reach the medium. Returns
   r. */
int volume_broken(struct volume *v, enum store_trouble t, uint64_t block, int r);
/* Something changed in the open generation: there is something to commit. */
void volume_changed(struct volume *v);

/* A commit, in three steps:
   - volume_freeze closes the open generation and gives what it writes, with no change under way
     (the program's to make sure), and lets changes go on in the next one when it returns: 0, or 1
     when there was nothing to commit (out is empty then), or an error (the volume is read only);
   - volume_write writes it, waiting for the medium, with changes going on meanwhile;
   - volume_written tells the volume how that went (r) and gives back what out held.
   Commits come one at a time, the program's to make sure. */
struct space_write {
    uint64_t address;
    uint8_t *data;
};
struct store_commit {
    uint64_t generation;
    struct store_super super;
    struct store_block **blocks;
    size_t n;
    struct space_write *maps;
    size_t nmaps;
};
int volume_freeze(struct volume *v, struct store_commit *out);
int volume_write(struct volume *v, struct store_commit *c);
void volume_written(struct volume *v, struct store_commit *c, int r);
/* The three at once, by a program that has nothing else under way on the volume meanwhile: 0
   (nothing to commit too), or an error (-N_ERR_READ_ONLY once the write failed). */
int volume_commit(struct volume *v);
/* Growing the volume to blocks, once its device is that large (common/store_format.h): after
   volume_even, which leaves the last commit an even generation's (a commit with nothing new when it
   is not), while the device has the size before; then volume_grow, in an odd generation's commit
   and the one after it. Each commits by itself, with no change and no other commit under way. */
int volume_even(struct volume *v);
int volume_grow(struct volume *v, uint64_t blocks);

/* A block with its bytes: the next commit writes it before its superblock. volume_dirty_room
   first makes room for n of them, so that volume_dirty cannot fail: each volume_dirty takes one
   (and b's hold), volume_dirty_unreserve gives back what was not taken. */
int volume_dirty_room(struct volume *v, size_t n);
void volume_dirty(struct volume *v, struct store_block *b);
void volume_dirty_unreserve(struct volume *v, size_t n);
/* How many blocks wait for the next commit now: memory that only a commit gives back. */
size_t volume_unwritten(struct volume *v);

/* Block heads (catalog nodes, space blocks). */
void head_seal(struct volume *v, void *block, uint32_t magic, uint64_t address, uint64_t generation);
bool head_valid(struct volume *v, const void *block, uint32_t magic, uint64_t address, uint64_t generation);
store_block_check content_valid;
