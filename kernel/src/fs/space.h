/* The space map: which blocks of the volume are in use (common/store_format.h).

   A block is used when the open generation's catalog names it, held when it must not be given out
   though the catalog no longer names it (the last commit still does, or something in memory still
   reads it) or does not yet (content written before its catalog changes), and free otherwise. The
   map on the medium records only use, so a volume mounted after a power loss frees what was held.
   The caller holds v->lock. */
#pragma once
#include "fs/volume.h"

int space_format(struct volume *v);
int space_mount(struct volume *v);
void space_destroy(struct volume *v);

/* Room for blocks the caller will ask for later (content written into memory now): -N_ERR_NO_SPACE
   when it would leave less than catalog commits need. */
int space_reserve(struct volume *v, uint64_t blocks);
void space_unreserve(struct volume *v, uint64_t blocks);
/* Up to want free neighbouring blocks, held. reserved: the caller reserved them. */
int space_alloc(struct volume *v, uint64_t want, bool reserved, struct range *out);
/* held → used: the catalog names them now */
int space_use(struct volume *v, uint64_t start, uint64_t count);
/* used → held: the catalog no longer names them. Fails whole. */
int space_unuse(struct volume *v, uint64_t start, uint64_t count);
/* held → free once the open generation and the next one are on the medium: the last commit and
   the one before it stay whole while another is written */
int space_release(struct volume *v, uint64_t start, uint64_t count);
/* space_unuse and space_release in one, which fails whole */
int space_let_go(struct volume *v, uint64_t start, uint64_t count);
/* space_release for each range of a list, which is emptied */
void space_release_list(struct volume *v, struct ranges *list);
/* generation is on the medium */
void space_committed(struct volume *v, uint64_t generation);
/* blocks that can still be reserved */
uint64_t space_available(struct volume *v);
/* 1 when the open generation uses block, else 0 */
int space_used(struct volume *v, uint64_t block);

/* The commit of generation: the copy of its parity, each block that changes as a new buffer. */
struct space_write {
    uint64_t address;
    uint8_t *data;
};
int space_freeze(struct volume *v, uint64_t generation, struct space_write **out, size_t *n);
