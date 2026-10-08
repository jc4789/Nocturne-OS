/* プロセス単位の生存メディア割当を課金する。
 * 単一 BSP / single-owner 契約。全プロセス RAM や RSS 上限ではない。
 * 元 FFmpeg 原文を変更せず、既存 MALLOC_PREFIX に接続する。
 * free はこの allocator の有効 pointer または NULL のみを受ける。
 */
#include "media_alloc_private.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef NMEDIA_ALLOC_LIMIT_BYTES
#define NMEDIA_ALLOC_LIMIT_BYTES NMEDIA_ALLOC_DEFAULT_BYTES
#endif
#if NMEDIA_ALLOC_LIMIT_BYTES <= 0
#error "メディア予算は正のbyte数を指定する"
#endif
#ifndef NMEDIA_BASE_MALLOC
#define NMEDIA_BASE_MALLOC malloc
#endif
#ifndef NMEDIA_BASE_FREE
#define NMEDIA_BASE_FREE free
#endif

struct allocation {
    void *base;
    size_t requested, charge;
};
static struct nmedia_alloc_stats totals = { .limit = NMEDIA_ALLOC_LIMIT_BYTES };
static bool worker_restricted;
static void increment(size_t *counter) {
    if (*counter != SIZE_MAX) ++*counter;
}
static void rejected(enum nmedia_alloc_failure why) {
    increment(&totals.rejects);
    totals.last_failure = why;
}
size_t nmedia_alloc_charge(size_t requested) {
    size_t bytes = requested ? requested : 1;
    size_t overhead = sizeof(struct allocation) + 15;
    if (bytes > SIZE_MAX - overhead - 15) return 0;
    return (bytes + overhead + 15) & ~(size_t)15;
}
void nmedia_alloc_snapshot(struct nmedia_alloc_stats *out) {
    if (out) *out = totals;
}
bool nmedia_alloc_reserve_worker(void) {
    if(totals.reserved || NMEDIA_WORKER_BYTES > totals.limit-totals.current) {
        rejected(NMEDIA_ALLOC_LIMIT);return false;
    }
    totals.reserved=NMEDIA_WORKER_BYTES;
    if(totals.peak<totals.current+totals.reserved)totals.peak=totals.current+totals.reserved;
    return true;
}
void nmedia_alloc_release_worker(void) { totals.reserved=0; }
bool nmedia_alloc_restrict_worker(void) {
    if(worker_restricted||totals.current||totals.blocks||totals.reserved||totals.peak)return false;
    if(totals.limit>NMEDIA_WORKER_BYTES)totals.limit=NMEDIA_WORKER_BYTES;
    worker_restricted=true;
    return true;
}
void *nmedia_ff_malloc(size_t size) {
    size_t charge = nmedia_alloc_charge(size);
    if (!charge) { rejected(NMEDIA_ALLOC_OVERFLOW); return NULL; }
    if (charge > totals.limit - totals.current - totals.reserved) {
        rejected(NMEDIA_ALLOC_LIMIT);
        return NULL;
    }
    void *base = NMEDIA_BASE_MALLOC(charge);
    if (!base) {
        increment(&totals.backend_ooms);
        totals.last_failure = NMEDIA_ALLOC_BACKEND_OOM;
        return NULL;
    }
    uintptr_t aligned = ((uintptr_t)base + sizeof(struct allocation) + 15)
                        & ~(uintptr_t)15;
    struct allocation *a = (struct allocation *)(aligned - sizeof *a);
    a->base = base;
    a->requested = size;
    a->charge = charge;
    totals.current += charge;
    ++totals.blocks;
    if (totals.peak < totals.current+totals.reserved) totals.peak = totals.current+totals.reserved;
    return (void *)aligned;
}
void nmedia_ff_free(void *pointer) {
    if (!pointer) return;
    struct allocation *a = (struct allocation *)
        ((unsigned char *)pointer - sizeof(struct allocation));
    void *base = a->base;
    totals.current -= a->charge;
    --totals.blocks;
    NMEDIA_BASE_FREE(base);
}
void *nmedia_ff_realloc(void *pointer, size_t size) {
    if (!pointer) return nmedia_ff_malloc(size);
    if (!size) { nmedia_ff_free(pointer); return NULL; }
    struct allocation *a = (struct allocation *)
        ((unsigned char *)pointer - sizeof(struct allocation));
    if (size == a->requested) return pointer;
    /* old と new が同時に生存する実ピークを全量課金する。
     * 失敗時は old header/content/charge を一切変更しない。
     * shrink も新割当方式なので満額使用中は失敗しうる。 */
    void *replacement = nmedia_ff_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, size < a->requested ? size : a->requested);
    nmedia_ff_free(pointer);
    return replacement;
}

/* 固定サイズのnative/state構造体だけ。積を計算するcalloc APIではない。 */
void *nmedia_ff_mallocz(size_t size) {
    void *p = nmedia_ff_malloc(size);
    if (p) memset(p, 0, size);
    return p;
}
