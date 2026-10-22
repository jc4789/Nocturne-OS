/* A document's content as pages: the one storage engine behind the RAM root (ramfs) and the
   persistent volume (NOSfs), docs/nosfs.md §3.

   A content is one version of a document. Each page is, in this order of precedence:
   - a frame of its own in the page table (dirty when the medium does not have it yet);
   - bytes of the initrd archive, read in place until the page is first changed;
   - a block on the content's medium (its extent map), read in on first use;
   - zero.
   Bytes past the size inside the last page are zero in every representation, so growing a
   content never shows stale bytes.

   All calls run on the BSP in task context; a call that reads the medium or waits for room sleeps,
   and other calls on the same content may run meanwhile (each page's I/O is waited for, never
   repeated). */
#pragma once
#include "kernel.h"

struct content;

/* The medium behind a content's extent map. A content without one (store NULL) lives in memory
   only and none of these is called. */
struct content_store {
    /* Read count blocks starting at block into the frames (physical addresses), one per block. */
    int (*read)(struct content *c, uint64_t block, uint64_t count, const uint64_t *frames);
    /* pages more pages (fewer when negative) are dirty. A write's change reserves room for their
       write-back and may wait for it; refusing it (an error) fails the write with no page dirtied.
       forced: a change that already happened (fewer pages, or a write-back undone), which neither
       waits nor fails. */
    int (*dirty)(struct content *c, int64_t pages, bool forced);
    /* The content no longer names these blocks (truncated away, or replaced by a write-back). */
    void (*drop)(struct content *c, uint64_t block, uint64_t count);
};

struct content *content_new(const struct content_store *store, void *owner);
struct content *content_ref(struct content *c);
void content_unref(struct content *c); /* the last one frees its pages and drops its blocks */
void *content_owner(struct content *c);
uint64_t content_size(struct content *c);

int64_t content_read(struct content *c, void *buf, uint64_t off, uint64_t n);
int64_t content_write(struct content *c, const void *buf, uint64_t off, uint64_t n);
int content_truncate(struct content *c, uint64_t size);
/* The content (empty) reads size bytes of the initrd archive in place (fs/initrd.c). */
void content_borrow(struct content *c, const uint8_t *data, uint64_t size);
/* Loading a content as the medium recorded it (mounting): its size, and where its pages are. A
   later map of the same pages replaces the earlier one; nothing is dropped. block 0: a hole. */
void content_load(struct content *c, uint64_t size);
int content_map(struct content *c, uint64_t page, uint64_t count, uint64_t block);

/* Write-back (the store), with no sleep between the three steps of one run:
   content_dirty_run finds the next run of dirty pages at or after *page (at most max long; its
   first page in *page, its length returned, 0: none). content_snapshot copies them into buf and
   marks them clean; their reservation is the caller's from then on, and a page changed again is
   dirty again. Once buf is on the medium, content_written maps the pages still there to the blocks
   (dropping the blocks they named before); the blocks of pages truncated away meanwhile are
   dropped instead. */
uint64_t content_dirty_run(struct content *c, uint64_t *page, uint64_t max);
void content_snapshot(struct content *c, uint64_t page, uint64_t count, void *buf);
int content_written(struct content *c, uint64_t page, uint64_t count, uint64_t block);
/* The extent map, run by run, in page order: what a checkpoint records. */
int content_extents(struct content *c, int (*run)(void *ctx, uint64_t page, uint64_t count, uint64_t block),
                    void *ctx);
uint64_t content_dirty_pages(struct content *c);

/* Give back up to want clean pages read from a medium (those not used lately first). Returns how
   many were freed. */
uint64_t content_reclaim(uint64_t want);
uint64_t content_cached_pages(void); /* pages of contents with a medium that are in RAM */
