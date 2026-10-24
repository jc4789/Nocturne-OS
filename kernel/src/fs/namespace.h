/* The namespace over the catalog (common/store_format.h): folders and the entries in them,
   records, and where each document's content is. The kernel's documents (fs/document.c) and the
   host tool (host/store) build on it. The caller holds v->lock, except where a function says it
   reads blocks without it. */
#pragma once
#include "fs/catalog.h"

/* Makes the root folder and the case-folding table of a volume volume_format just made; loads the
   table of a mounted one. */
int ns_format(struct volume *v);
int ns_mount(struct volume *v);

/* A record, with a small document's content (up to STORE_INLINE_MAX bytes) into content. Returns that
   content's length (0 for a folder or a document kept in blocks), or an error. */
int ns_record(struct volume *v, uint64_t id, struct store_record *out, void *content);
int ns_record_put(struct volume *v, uint64_t id, const struct store_record *r, const void *content, size_t n);

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
/* The entry the path names (the root: id STORE_ID_ROOT, no name), with its record. */
int ns_lookup(struct volume *v, const struct path *p, struct ns_entry *entry, struct store_record *record);
/* The folder entry after `after` (NULL: the first). */
int ns_next(struct volume *v, uint64_t folder, const struct ns_entry *after, struct ns_entry *out);
/* A new entry name in folder, naming a new record (kind, attributes, flags). */
int ns_create(struct volume *v, uint64_t folder, const char *name, size_t n, uint16_t kind, uint16_t attributes,
              uint16_t flags, uint64_t *id);
/* The entry `name` of folder out of it (to_folder's to_name from then on). Never replaces an entry
   (-N_ERR_EXISTS), and a folder never goes into itself (-N_ERR_INVALID); a new name for the same
   entry changes only its case. A move that fails changes nothing. */
int ns_move(struct volume *v, uint64_t folder, const char *name, size_t n, uint64_t to_folder, const char *to_name,
            size_t to_n);
/* ns_move onto the name of a document, which goes to aside's aside_name in the same change, or not
   at all (anything else there, or anything at aside_name: -N_ERR_EXISTS); with nothing there, an
   ns_move. */
int ns_replace(struct volume *v, uint64_t folder, const char *name, size_t n, uint64_t to_folder, const char *to_name,
               size_t to_n, uint64_t aside, const char *aside_name, size_t aside_n);
/* The entry and its record go; a folder must be empty (-N_ERR_NOT_EMPTY). A document's blocks are
   added to dropped, held: they go back with space_release once nothing reads them. */
int ns_remove(struct volume *v, uint64_t folder, const char *name, size_t n, struct ranges *dropped);
/* Whether folder has no entries. */
int ns_empty(struct volume *v, uint64_t folder);
/* Everything in folder goes, its folders with all they hold. Their blocks go back at once, so
   nothing may have any of it open. */
int ns_clear(struct volume *v, uint64_t folder);

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
/* count blocks of e from its block first on, read into the cache (no lock needed). */
int ns_extent_read(struct volume *v, const struct ns_extent *e, uint64_t first, uint32_t count, struct cblock **out);
/* The blocks of the document from index on: data[i] (kmalloc'd STORE_BLOCKs the volume takes in
   every case) goes into new blocks that the next commit writes. reserved: space_reserve made room
   for the n of them, and the reservation is spent whatever the outcome. What they replace is added
   to dropped, held. data NULL: the blocks keep whatever the medium has (an unchecked document
   whose blocks the kernel writes itself). */
int ns_write(struct volume *v, uint64_t doc, bool checked, uint64_t index, size_t n, uint8_t **data, bool reserved,
             struct ranges *dropped);
/* Every block from index on out of the document, into dropped (held). */
int ns_cut(struct volume *v, uint64_t doc, uint64_t index, struct ranges *dropped);
/* Every extent of the document, in order (a kmalloc'd list). */
int ns_extents(struct volume *v, uint64_t doc, struct ns_extent **out, size_t *n);

/* The document's content from now on: size bytes, its blocks from cut on gone, then each page
   (index, and a kmalloc'd STORE_BLOCK the volume takes in every case) over the block it names.
   Bytes after the end in the last block are zeros, whichever way it came. Small content goes into
   the record. reserved: space_reserve made room for the n pages, spent whatever the outcome.
   What is replaced is added to dropped, held. */
struct ns_page {
    uint64_t index;
    uint8_t *data;
};
int ns_update(struct volume *v, uint64_t id, uint64_t size, uint64_t cut, struct ns_page *pages, size_t n, bool reserved,
              struct ranges *dropped);

/* doc's content becomes from's, whose blocks it names from then on, and from is left empty (a
   replacement made apart, swapped in whole). doc's old blocks are added to dropped, held. */
int ns_take(struct volume *v, uint64_t doc, uint64_t from, struct ranges *dropped);

/* n bytes of the document's content from offset: how many there were. */
int64_t ns_read(struct volume *v, uint64_t id, uint64_t offset, void *buf, size_t n);
/* The document's whole content becomes data (its old blocks are released). */
int ns_save(struct volume *v, uint64_t id, const void *data, size_t n);
