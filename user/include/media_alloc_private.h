#ifndef NMEDIA_ALLOC_PRIVATE_H
#define NMEDIA_ALLOC_PRIVATE_H
#include <stddef.h>

/* メディア専用 private API。JS setter、kernel ABI、RSS 保証はない。 */
enum nmedia_alloc_failure {
    NMEDIA_ALLOC_OK, NMEDIA_ALLOC_LIMIT, NMEDIA_ALLOC_OVERFLOW,
    NMEDIA_ALLOC_BACKEND_OOM
};
struct nmedia_alloc_stats {
    size_t limit, current, peak, blocks, rejects, backend_ooms;
    enum nmedia_alloc_failure last_failure;
};
void *nmedia_ff_malloc(size_t size);
void *nmedia_ff_mallocz(size_t size);
void *nmedia_ff_realloc(void *pointer, size_t size);
void nmedia_ff_free(void *pointer);
void nmedia_alloc_snapshot(struct nmedia_alloc_stats *out);
/* 正規化 payload + private header + alignment slack を 16 byte 切上げ。
 * overflow は 0。照会は統計を変更しない。基本 libc heap header は読まない。 */
size_t nmedia_alloc_charge(size_t requested);
#endif
