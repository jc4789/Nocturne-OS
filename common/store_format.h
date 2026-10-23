/* The store's format on the medium: Nocturne's one volume, drive C:. The kernel (kernel/src/fs) and
   the host tool that makes and updates volumes (host/store) read and write it with the same code.

   The volume is a run of 4 KiB blocks (inside a GPT partition of type STORE_PARTITION_TYPE, or a
   whole image). Nothing on it is ever written in place except the two superblocks and the two
   copies of the space map; everything else is copied on write, so a volume is always the state of
   its last finished commit:

     block 0             superblock of the even generations
     1 ...               space map copy 0 (index blocks, then bitmap blocks)
     ...                 space map copy 1
     ...                 catalog nodes and document contents, wherever the space map has room
     last block          superblock of the odd generations

   A commit writes the new catalog nodes and contents into free blocks, the space map into the copy
   of its generation's parity, waits until they are on the medium, then writes its superblock. The
   superblock of the other parity still describes the previous commit, whose blocks the new one
   never reused: a commit cut short by a power loss leaves the previous one whole. Blocks a commit
   lets go are reused only after the next commit too, so a last commit found damaged leaves the
   one before it.

   A bitmap block goes into the copy of each parity in the commit that changes it and in the next
   one, so the generation that last wrote it into either copy follows from the generation its
   index entry gives; what a commit cut short left in a copy never passes for it.

   The catalog is one B+tree over every entry, record and extent of the volume. A key is
   [id: 8 bytes big-endian][type: 1 byte][rest], so memcmp orders keys by document or folder first:
     RECORD   [id][1]                    struct store_record, then a small document's content
     ENTRY    [folder][2][folded name]   struct store_entry: a name in a folder
     EXTENT   [document][3][index: 8 BE] struct store_extent: blocks of content from that block on
   Names compare by their simple case folding, through the table the volume carries (document
   STORE_ID_FOLD), so every kernel that mounts the volume names its entries alike.

   Every block but the superblocks carries a CRC-32C seeded from the volume's identity (a block of
   another volume never passes for one of this), its own address and the generation that wrote it;
   a branch names a child by address and generation. Contents carry a CRC-32C per block in their
   extents. */
#pragma once
#include <stdint.h>

#define STORE_BLOCK 4096u
#define STORE_VERSION 1u
#define STORE_SIGNATURE "Nocturne"

/* GPT partition type of a Nocturne volume, df8027d2-685d-4511-a682-fed55b9b8c7f, in GPT's byte
   order */
#define STORE_PARTITION_TYPE {0xd2, 0x27, 0x80, 0xdf, 0x5d, 0x68, 0x11, 0x45, \
                              0xa6, 0x82, 0xfe, 0xd5, 0x5b, 0x9b, 0x8c, 0x7f}

/* the start of every block the store describes itself with */
struct store_head {
    uint32_t magic;
    uint32_t check;      /* CRC-32C of the block with this field 0 */
    uint64_t address;    /* the block it belongs at */
    uint64_t generation; /* the commit that wrote it */
};

#define STORE_MAGIC_SUPER 0x5055534eu /* "NSUP" */
#define STORE_MAGIC_NODE 0x5441434eu  /* "NCAT" */
#define STORE_MAGIC_INDEX 0x5844494eu /* "NIDX" */
#define STORE_MAGIC_MAP 0x50414d4eu   /* "NMAP" */

/* The superblock's check is seeded with 0; every other block's with store_seed(volume_id). */
struct store_super {
    struct store_head head;
    char signature[8];      /* STORE_SIGNATURE */
    uint32_t version;       /* STORE_VERSION */
    uint32_t block_size;    /* STORE_BLOCK */
    uint8_t volume_id[16];  /* random, when the volume was made */
    uint64_t blocks;        /* the volume's size */
    uint64_t catalog;       /* the catalog's root node */
    uint64_t catalog_generation;
    uint64_t next_id;       /* the next record id to give out */
    uint64_t used;          /* blocks in use, the superblocks and space maps included */
    int64_t created;        /* nanoseconds since 1970-01-01 UTC */
    char label[32];
};

/* ---- the space map ---- */

/* An index block lists, for each bitmap block, the generation whose commit last changed it and
   how many blocks it leaves free. */
struct store_space_count {
    uint64_t changed;
    uint32_t free;
    uint32_t reserved;
};
#define STORE_COUNTS_PER_INDEX ((STORE_BLOCK - sizeof(struct store_head)) / sizeof(struct store_space_count))
/* A bitmap block: one bit per block of the volume, 1 in use, after its head. */
#define STORE_BITS_PER_MAP ((STORE_BLOCK - sizeof(struct store_head)) * 8)

struct store_layout {
    uint64_t maps, indexes; /* bitmap and index blocks of one copy */
    uint64_t copy[2];       /* where each copy starts: its index blocks, then its bitmap blocks */
    uint64_t first;         /* the first block for catalog nodes and contents */
};

static inline struct store_layout store_layout(uint64_t blocks) {
    struct store_layout l;
    l.maps = (blocks + STORE_BITS_PER_MAP - 1) / STORE_BITS_PER_MAP;
    l.indexes = (l.maps + STORE_COUNTS_PER_INDEX - 1) / STORE_COUNTS_PER_INDEX;
    l.copy[0] = 1;
    l.copy[1] = 1 + l.indexes + l.maps;
    l.first = 1 + 2 * (l.indexes + l.maps);
    return l;
}

/* ---- the catalog ---- */

/* A node: its head, then count slots (the offsets of its items, in key order) growing up, and the
   items growing down from the end of the block. An item is its key length and value length
   (uint16_t each), the key, then the value. A branch's values are struct store_child: the child
   holds the keys from its item's key up to the next item's (the first item's child, every key
   below the second's). */
struct store_node {
    struct store_head head;
    uint16_t level; /* 0: a leaf */
    uint16_t count;
    uint16_t low;   /* the lowest byte the items use */
    uint16_t reserved;
    uint16_t slot[];
};

struct store_child {
    uint64_t address;
    uint64_t generation;
};

#define STORE_KEY_RECORD 1
#define STORE_KEY_ENTRY 2
#define STORE_KEY_EXTENT 3
#define STORE_KEY_MAX 400   /* an entry's: 9 bytes and a folded name (at most 255 * 3 / 2 bytes) */
#define STORE_VALUE_MAX 800 /* a record with the largest inline content */

#define STORE_ID_ROOT 1  /* C:\ (its own parent) */
#define STORE_ID_FOLD 2  /* the case-folding table: a document no folder names */
#define STORE_ID_FIRST 16

#define STORE_DOCUMENT 1
#define STORE_FOLDER 2

/* attributes */
#define STORE_SYSTEM 1u    /* apps read it but cannot change it or anything in it (C:\Nocturne) */
#define STORE_TEMPORARY 2u /* a temporary folder of the system (C:\Nocturne\Temp, the user's AppData\Temp):
                              the kernel empties it whenever it mounts the volume */
#define STORE_PRIVATE 4u   /* only the kernel reaches it (the hibernation image) */

/* flags */
#define STORE_INLINE 1u    /* the content follows the record; no extents */
#define STORE_UNCHECKED 2u /* its extents carry no checks: the kernel writes its blocks itself */

struct store_record {
    uint64_t parent;
    uint64_t size;     /* a document's bytes */
    int64_t created;   /* nanoseconds since 1970-01-01 UTC */
    int64_t modified;
    uint16_t kind;
    uint16_t attributes;
    uint16_t flags;
    uint16_t reserved;
};
#define STORE_INLINE_MAX (STORE_VALUE_MAX - sizeof(struct store_record))

/* An entry: the record it names, then its name as it was given (UTF-8, the rest of the value). */
struct store_entry {
    uint64_t id;
};

/* Blocks block .. block + count - 1 hold the content from the key's index on; a checked document's
   extents hold at most STORE_EXTENT_CHECKS blocks, each with the CRC-32C of its 4 KiB. */
struct store_extent {
    uint64_t block;
    uint32_t count;
    uint32_t reserved;
    uint32_t check[];
};
#define STORE_EXTENT_CHECKS 128u

/* The case-folding table (document STORE_ID_FOLD): pairs of code points, sorted by the first, for
   every code point whose simple case folding is another. */
struct store_fold_header {
    char magic[8];      /* "NOCFOLD1" */
    uint32_t unicode;   /* the Unicode version it was made from, 0xMMmmuu */
    uint32_t count;
    /* uint32_t pairs[count][2] */
};
