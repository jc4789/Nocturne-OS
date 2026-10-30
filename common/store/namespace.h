/* The namespace over the catalog (common/store_format.h): folders and the entries in them,
   records, and where each document's content is. The kernel's C: (kernel/src/fs) and the host
   tool (host/store) build on it.

   Reads need nothing of the caller's: each catalog call is whole by itself, and a change writes a
   new entry's record before its name and takes a name away before its record, so a reader never
   finds a name without its record. What the store cannot see from one call at a time is the
   program's to make sure of:
   - a folder's names change one at a time: ns_create, ns_remove, ns_move and ns_replace hold off
     every other of them in the same folder (in both folders for a move; in the folder removed as
     well; for ns_replace, the folder its document goes aside to as well), and a move of a folder
     to another one holds off every other such move (one folder cannot then go into another that
     goes into it);
   - a document's content changes one at a time: ns_write, ns_cut, ns_update, ns_take (both
     documents) and the removal of a document;
   - every change is in one generation: no commit freezes the open one while a change is under
     way (volume_freeze).
   A record's own fields change in place (catalog_amend): a move's new parent, a folder's time and
   a document's new content never undo each other. A change takes its time `now` in nanoseconds
   since 1970 UTC, and tells what it changed in struct ns_told for whoever watches (NULL: no one). */
#pragma once
#include "store/catalog.h"

/* Makes the root folder and the case-folding table of a volume volume_format just made; loads the
   table of a mounted one. */
int ns_format(struct volume *v, int64_t now);
int ns_mount(struct volume *v);

/* A record, with a small document's content (up to STORE_INLINE_MAX bytes) into content. Returns that
   content's length (0 for a folder or a document kept in blocks), or an error. */
int ns_record(struct volume *v, uint64_t id, struct store_record *out, void *content);
int ns_record_put(struct volume *v, uint64_t id, const struct store_record *r, const void *content, size_t n);
/* What may not change a folder may not change what it holds: an entry has these attributes of its
   folder's for as long as it is in that folder (ns_create, and a move gives them up for the new
   folder's). */
#define NS_GIVEN (STORE_SYSTEM | STORE_PRIVATE)

/* What a record becomes: each field named in `fields` from the amend, the rest as the record has
   it at the moment of the change. */
#define NS_PARENT 1u
#define NS_SIZE 2u
#define NS_MODIFIED 4u
#define NS_ATTRIBUTES 8u /* these attributes added, after those taken went */
#define NS_FLAGS 16u
#define NS_CONTENT 32u   /* the small content after the record: n bytes */
struct ns_amend {
    unsigned fields;
    uint64_t parent, size;
    int64_t modified;
    uint16_t attributes, taken, flags;
    const void *content;
    size_t n;
};
int ns_record_amend(struct volume *v, uint64_t id, const struct ns_amend *a);

/* An entry of a folder: what it names and its name as given. */
struct ns_entry {
    uint64_t id;
    size_t n;
    char name[256];
    size_t klen; /* its place in the folder */
    uint8_t key[STORE_KEY_MAX];
};
/* name in folder. */
int ns_find(struct volume *v, uint64_t folder, const char *name, size_t n, struct ns_entry *out);
/* The place name has in folder, whether an entry has it or not: its key (STORE_KEY_MAX bytes of
   room), as struct ns_entry and struct ns_change give it. Its length, or an error for a bad name. */
int ns_key(struct volume *v, uint64_t folder, const char *name, size_t n, uint8_t *key);

/* A change to the namespace: a name of folder that names another entry from now on, or none
   (named; key: its place; id: what it names now, 0: nothing), or document id of folder whose
   content changed. Each change that a new opening, entry info or folder listing sees is told
   once, with what else changed in the same call (a move: the name it leaves, the name it takes,
   the document that goes aside). */
struct ns_change {
    uint64_t folder, id;
    bool named;
    bool hidden;   /* the entry is PRIVATE: no app sees the change */
    uint64_t gone; /* a folder that is no more, removed with its name */
    size_t klen;
    uint8_t key[STORE_KEY_MAX];
};
struct ns_told {
    size_t n;
    struct ns_change c[3];
};

/* The entry the path names (the root: id STORE_ID_ROOT, no name), with its record. */
int ns_lookup(struct volume *v, const struct path *p, struct ns_entry *entry, struct store_record *record);
/* The folder entry after `after` (NULL: the first). */
int ns_next(struct volume *v, uint64_t folder, const struct ns_entry *after, struct ns_entry *out);
/* A new entry name in folder, naming a new record (kind, attributes, flags). */
int ns_create(struct volume *v, uint64_t folder, const char *name, size_t n, uint16_t kind, uint16_t attributes,
              uint16_t flags, int64_t now, uint64_t *id, struct ns_told *told);
/* The entry `name` of folder out of it (to_folder's to_name from then on). Never replaces an entry
   (-N_ERR_EXISTS), and a folder never goes into itself (-N_ERR_INVALID); a new name for the same
   entry changes only its case. A move that fails changes nothing. */
int ns_move(struct volume *v, uint64_t folder, const char *name, size_t n, uint64_t to_folder, const char *to_name,
            size_t to_n, int64_t now, struct ns_told *told);
/* ns_move onto the name of a document, which goes to aside's aside_name in the same change, or not
   at all (anything else there, or anything at aside_name: -N_ERR_EXISTS); with nothing there, an
   ns_move. */
int ns_replace(struct volume *v, uint64_t folder, const char *name, size_t n, uint64_t to_folder, const char *to_name,
               size_t to_n, uint64_t aside, const char *aside_name, size_t aside_n, int64_t now, struct ns_told *told);
/* The entry and its record go; a folder must be empty (-N_ERR_NOT_EMPTY). A document's blocks are
   added to dropped, held: they go back with space_release once nothing reads them. */
int ns_remove(struct volume *v, uint64_t folder, const char *name, size_t n, int64_t now, struct ranges *dropped,
              struct ns_told *told);
/* Whether folder has no entries. */
int ns_empty(struct volume *v, uint64_t folder);
/* Everything in folder goes, its folders with all they hold. Their blocks go back at once, so
   nothing may have any of it open, nor change it meanwhile. */
int ns_clear(struct volume *v, uint64_t folder, int64_t now);

/* Where content is: count blocks from index on, at block on (0: a hole, read as zeros), with each
   one's check unless the document is unchecked. */
struct ns_extent {
    uint64_t index, count, block;
    bool checked;
    uint32_t check[STORE_EXTENT_CHECKS];
};
/* The extent holding block index of the document, or the hole up to the next one (to the end of
   everything when there is none). */
int ns_extent(struct volume *v, uint64_t doc, uint64_t index, struct ns_extent *out);
/* count blocks of e from its block first on, held, read when memory does not have them. */
int ns_extent_read(struct volume *v, const struct ns_extent *e, uint64_t first, uint32_t count, struct store_block **out);
/* The blocks of the document from index on: data[i] (STORE_BLOCKs from the env's alloc that the
   volume takes in every case) goes into new blocks that the next commit writes. reserved:
   space_reserve made room for the n of them, and the reservation is spent whatever the outcome.
   What they replace is added to dropped, held. data NULL: the blocks keep whatever the medium has
   (an unchecked document whose blocks the kernel writes itself). */
int ns_write(struct volume *v, uint64_t doc, bool checked, uint64_t index, size_t n, uint8_t **data, bool reserved,
             struct ranges *dropped);
/* Every block from index on out of the document, into dropped (held). */
int ns_cut(struct volume *v, uint64_t doc, uint64_t index, struct ranges *dropped);
/* Every extent of the document, in order (a list from the env's alloc). */
int ns_extents(struct volume *v, uint64_t doc, struct ns_extent **out, size_t *n);

/* The document's content from now on: size bytes, its blocks from cut on gone, then each page
   (index, and a STORE_BLOCK from the env's alloc that the volume takes in every case) over the
   block it names. Bytes after the end in the last block are zeros, whichever way it came. Small
   content goes into the record. reserved: space_reserve made room for the n pages, spent whatever
   the outcome. What is replaced is added to dropped, held. */
struct ns_page {
    uint64_t index;
    uint8_t *data;
};
int ns_update(struct volume *v, uint64_t id, uint64_t size, uint64_t cut, struct ns_page *pages, size_t n, bool reserved,
              int64_t now, struct ranges *dropped, struct ns_told *told);

/* doc's content becomes from's, whose blocks it names from then on, and from is left empty (a
   replacement made apart, swapped in whole). doc's old blocks are added to dropped, held. */
int ns_take(struct volume *v, uint64_t doc, uint64_t from, int64_t now, struct ranges *dropped, struct ns_told *told);

/* n bytes of the document's content from offset: how many there were. */
int64_t ns_read(struct volume *v, uint64_t id, uint64_t offset, void *buf, size_t n);
/* The document's whole content becomes data (its old blocks are released). */
int ns_save(struct volume *v, uint64_t id, const void *data, size_t n, int64_t now);
