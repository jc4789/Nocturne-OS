/* A document's content as pages: the one storage engine behind the namespace (fs/ramfs.c) and the
   persistent volume (fs/nos.c), docs/nosfs.md §3–4.

   A content is one version of a document. Each page is, in this order of precedence:
   - a frame of its own in the page table (dirty when the medium does not have it yet);
   - a block of its own on the medium (its extent map), read in on first use;
   - bytes of the initrd archive, read in place;
   - below base_limit, the page as the version it was made from (its base) shows it;
   - zero.
   A content of at most CONTENT_TINY bytes that owns nothing else keeps them on the heap.
   Bytes past the size inside the last page are zero in every representation, so growing a
   content never shows stale bytes.

   Versions: a working version (content_version) is changed by writes; freezing it (content_freeze)
   makes it the version a save writes, and a new working version goes on top of it; once saved
   (content_saved) it is the document's committed version and merges with the version under it as
   soon as nothing else reads that one. A frame or a block belongs to the one version whose table
   holds it: versions share pages through the base chain, never by counting.

   All calls run on the BSP in task context; a call that reads the medium or waits for room sleeps,
   and other calls on the same content may run meanwhile (each page's I/O is waited for, never
   repeated). */
#pragma once
#include "kernel.h"

#define CONTENT_TINY 512 /* bytes a content keeps on the heap, and a volume keeps in its index */

struct content;

/* The medium behind a content's extent map. A content without one (store NULL) lives in memory
   only and none of these is called. */
struct content_store {
    /* Read count blocks starting at block into the frames (physical addresses), one per block. */
    int (*read)(struct content *c, uint64_t block, uint64_t count, const uint64_t *frames);
    /* pages more dirty pages without a block of their own (fewer when negative): each needs room
       to be written somewhere. A write's change reserves it and may wait for it; refusing it (an
       error) fails the write with no page dirtied. forced: a change that already happened, which
       neither waits nor fails. */
    int (*dirty)(struct content *c, int64_t pages, bool forced);
    /* The content no longer names these blocks (truncated away, replaced, merged or freed). */
    void (*drop)(struct content *c, uint64_t block, uint64_t count);
};

enum content_state {
    CONTENT_WORKING,   /* written by handles */
    CONTENT_FROZEN,    /* a save asked for it; nothing writes it any more */
    CONTENT_SAVING,    /* recorded by a save that is on its way to the medium */
    CONTENT_COMMITTED, /* what the medium has */
};

/* A committed, empty content. */
struct content *content_new(const struct content_store *store);
/* A working version on top of base (NULL: an empty one). */
struct content *content_version(struct content *base, const struct content_store *store);
struct content *content_ref(struct content *c);
void content_unref(struct content *c); /* the last one frees its pages and drops its blocks */
uint64_t content_size(struct content *c);
enum content_state content_state(struct content *c);
struct content *content_base(struct content *c);
uint64_t content_base_limit(struct content *c); /* pages read from the base */

int64_t content_read(struct content *c, void *buf, uint64_t off, uint64_t n);
/* Working versions only (-N_ERR_BUSY otherwise: the caller writes the document's new one). */
int64_t content_write(struct content *c, const void *buf, uint64_t off, uint64_t n);
int content_truncate(struct content *c, uint64_t size);
/* The content (new, empty) reads size bytes of the initrd archive in place (fs/initrd.c). */
void content_borrow(struct content *c, const uint8_t *data, uint64_t size);
/* Loading a content as the medium recorded it: its size, and where its pages are (a later map of
   the same pages replaces the earlier one; nothing is dropped), or its bytes when they are few. */
void content_load(struct content *c, uint64_t size);
int content_map(struct content *c, uint64_t page, uint64_t count, uint64_t block);
int content_load_tiny(struct content *c, const void *data, uint64_t size);

/* Saving. content_freeze waits for the writes and write-backs in progress and stops new ones.
   content_own_all makes the version hold every page below its size itself (no base any more).
   content_compact keeps a content of at most CONTENT_TINY bytes on the heap only (no pages, no
   blocks, no base). content_saving (once the write-backs in progress end) and content_saved follow
   the save. content_thaw takes back a freeze no save took (nothing was made on top of it). */
void content_freeze(struct content *c);
void content_thaw(struct content *c);
int content_own_all(struct content *c);
int content_compact(struct content *c);
bool content_is_tiny(struct content *c);
void content_saving(struct content *c);
void content_saved(struct content *c);

/* Write-back, with no sleep between content_dirty_run and content_snapshot:
   content_dirty_run finds the next run of dirty pages at or after *page (at most max long; its
   first page in *page, its length returned, 0: none). content_block says which block of its own
   holds a page (0: none; *left: pages from there on that continue the same way). content_snapshot
   copies such a run into buf, marks it clean and names the blocks from block on as its pages'
   own (dropping the ones they named before, other than these); a dirty page without a block hands
   its reservation to the caller. Until content_written says buf is on the medium the pages stay
   in RAM; a page changed again meanwhile is dirty again. content_write_failed: buf did not reach
   the medium, the pages are dirty again (keeping the blocks, so no reservation). */
uint64_t content_dirty_run(struct content *c, uint64_t *page, uint64_t max);
uint64_t content_block(struct content *c, uint64_t page, uint64_t *left);
int content_snapshot(struct content *c, uint64_t page, uint64_t count, void *buf, uint64_t block);
void content_written(struct content *c, uint64_t page, uint64_t count);
void content_write_failed(struct content *c, uint64_t page, uint64_t count);
/* The extent map, run by run, in page order. */
int content_extents(struct content *c, int (*run)(void *ctx, uint64_t page, uint64_t count, uint64_t block),
                    void *ctx);
uint64_t content_dirty_pages(struct content *c);
/* The working or frozen content with a medium that has the most dirty pages (NULL: none). */
struct content *content_spill_pick(void);

/* Give back up to want clean pages read from a medium (those not used lately first). Returns how
   many were freed. */
uint64_t content_reclaim(uint64_t want);
uint64_t content_cached_pages(void); /* pages of contents with a medium that are in RAM */
