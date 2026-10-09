#ifndef NMEDIA_ALLOC_PRIVATE_H
#define NMEDIA_ALLOC_PRIVATE_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* メディア専用 private API。JS setter、kernel ABI、RSS 保証はない。 */
enum nmedia_alloc_failure {
    NMEDIA_ALLOC_OK, NMEDIA_ALLOC_LIMIT, NMEDIA_ALLOC_OVERFLOW,
    NMEDIA_ALLOC_BACKEND_OOM
};
struct nmedia_alloc_stats {
    size_t limit, current, peak, blocks, rejects, backend_ooms;
    enum nmedia_alloc_failure last_failure;
    size_t reserved; /* actual live/retired child count, retained until reap */
};
/* No default artificial heap quota: allocation is governed by real OS malloc
 * failure and representable size_t accounting. Child lifecycle counts are
 * not fictional preallocated RAM or a child heap ceiling. */
#define NMEDIA_ALLOC_DEFAULT_BYTES SIZE_MAX
/* Native single-owner calls. Child count is retained until the child is reaped.
 * Worker entry is checked before FFmpeg; it does not invent a lower heap quota
 * than an explicit caller/build limit. */
bool nmedia_alloc_reserve_worker(void);
void nmedia_alloc_release_worker(void);
bool nmedia_alloc_restrict_worker(void);
void *nmedia_ff_malloc(size_t size);
void *nmedia_ff_mallocz(size_t size);
void *nmedia_ff_realloc(void *pointer, size_t size);
void nmedia_ff_free(void *pointer);
void nmedia_alloc_snapshot(struct nmedia_alloc_stats *out);
/* 正規化 payload + private header + alignment slack を 16 byte 切上げ。
 * overflow は 0。照会は統計を変更しない。基本 libc heap header は読まない。 */
size_t nmedia_alloc_charge(size_t requested);
#endif
