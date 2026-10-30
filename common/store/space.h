/* The space map: which blocks of the volume are in use (common/store_format.h).

   A block is used when the open generation's catalog names it, held when it must not be given out
   though the catalog no longer names it (the last commit still does, or something in memory still
   reads it) or does not yet (content written before its catalog changes), and free otherwise. The
   map on the medium records only use, so a volume mounted after a power loss frees what was held.

   Any thread may call these at once: the space's latch (struct space) keeps it whole. The bitmap
   blocks come from the medium when first needed. What needs one that memory does not have waits
   for it with no latch held (the caller holds none either), or, where it says so, gives
   -N_ERR_NOT_READY with nothing changed: space_load, with no latch held, then makes it there. */
#pragma once
#include "store/volume.h"

int space_format(struct volume *v);
int space_mount(struct volume *v);
void space_destroy(struct volume *v);
/* The volume of blocks (more than it has, the device as large) in the open generation: the map for
   the new size in the room grown by, the old copies and odd superblock let go (volume_grow, with
   nothing else under way). Fails whole, with nothing changed; -N_ERR_INVALID when that room is
   too small for the map. */
int space_grow(struct volume *v, uint64_t blocks);
/* Bitmap blocks both copies hold as memory has them: out of memory, read again when needed. */
void space_trim(struct volume *v);

/* The bitmap blocks of blocks start .. start + count - 1 into memory. Waits. */
int space_load(struct volume *v, uint64_t start, uint64_t count);

/* Room for blocks the caller will ask for later (content written into memory now): -N_ERR_NO_SPACE
   when it would leave less than catalog commits need. */
int space_reserve(struct volume *v, uint64_t blocks);
void space_unreserve(struct volume *v, uint64_t blocks);
/* Up to want free neighbouring blocks, held. reserved: the caller reserved them. Waits. */
int space_alloc(struct volume *v, uint64_t want, bool reserved, struct range *out);
/* held → used: the catalog names them now. The blocks came from space_alloc: never waits. */
int space_use(struct volume *v, uint64_t start, uint64_t count);
/* used → held: the catalog no longer names them. Fails whole; -N_ERR_NOT_READY. */
int space_unuse(struct volume *v, uint64_t start, uint64_t count);
/* held → free once the open generation and the next one are on the medium: the last commit and
   the one before it stay whole while another is written */
int space_release(struct volume *v, uint64_t start, uint64_t count);
/* space_unuse and space_release in one, which fails whole; -N_ERR_NOT_READY. */
int space_let_go(struct volume *v, uint64_t start, uint64_t count);
/* space_release for each range of a list, which is emptied */
void space_release_list(struct volume *v, struct ranges *list);
/* generation is on the medium */
void space_committed(struct volume *v, uint64_t generation);
/* blocks that can still be reserved */
uint64_t space_available(struct volume *v);
/* 1 when the open generation uses block, else 0. Waits. */
int space_used(struct volume *v, uint64_t block);

/* The commit of generation (volume_freeze): the copy of its parity, each block that changes as a
   new buffer. */
int space_freeze(struct volume *v, uint64_t generation, struct space_write **out, size_t *n);
