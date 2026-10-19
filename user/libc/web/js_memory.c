/* QuickJS runtime memory: the allocator shared by documents and workers. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "js_memory.h"

/* Alignment and accounting include the allocation header, not an invented
   malloc_usable_size. This also handles realloc failure without losing state. */
union alloc_header { size_t size; long double align; };
static void allocation_failed(JSMallocState *s, size_t requested, bool quota) {
    struct js_alloc_diagnostics *d=s->opaque;if(!d)return;
    d->requested=requested;d->used=s->malloc_size;d->limit=s->malloc_limit;d->quota=quota;d->failures++;
}
static void allocation_peak(JSMallocState *s) {
    struct js_alloc_diagnostics *d=s->opaque;
    if(d){d->current=s->malloc_size;if(s->malloc_size>d->peak)d->peak=s->malloc_size;}
}
static void *qmalloc(JSMallocState *s, size_t n) {
    if (!n || n > SIZE_MAX - sizeof(union alloc_header)) return NULL;
    size_t total = n + sizeof(union alloc_header);
    if (s->malloc_size > s->malloc_limit || total > s->malloc_limit - s->malloc_size) { allocation_failed(s,total,true);return NULL; }
    union alloc_header *h = malloc(total);
    if (!h) { allocation_failed(s,total,false);return NULL; }
    h->size = n; s->malloc_count++; s->malloc_size += total;
    allocation_peak(s);
    return h + 1;
}
static void qfree(JSMallocState *s, void *p) {
    if (!p) return;
    union alloc_header *h = (union alloc_header *)p - 1;
    s->malloc_count--; s->malloc_size -= h->size + sizeof *h; free(h);
    struct js_alloc_diagnostics *d=s->opaque;if(d)d->current=s->malloc_size;
}
static void *qrealloc(JSMallocState *s, void *p, size_t n) {
    if (!p) return qmalloc(s, n);
    if (!n) { qfree(s, p); return NULL; }
    if (n > SIZE_MAX - sizeof(union alloc_header)) return NULL;
    union alloc_header *h = (union alloc_header *)p - 1;
    size_t old = h->size, total = n + sizeof *h;
    if (n > old && (s->malloc_size > s->malloc_limit || n - old > s->malloc_limit - s->malloc_size)) { allocation_failed(s,total,true);return NULL; }
    if (n == old) return p;
    /* Nocturne's realloc intentionally keeps the entire adjacent free block
       when growing in place, and retains capacity when shrinking. QuickJS
       frequently resizes many small tables: using that policy here makes
       their physical footprint diverge from the runtime's exact quota.
       Fit each engine allocation to its requested size without changing the
       OS allocator or ABI. Accounting stays exact without a per-page quota. */
    union alloc_header *next = malloc(total);
    if (!next) { allocation_failed(s,total,false);return NULL; }
    memcpy(next + 1, p, old < n ? old : n); free(h);
    next->size = n; s->malloc_size = s->malloc_size - old + n;
    allocation_peak(s);
    return next + 1;
}
static size_t qusable(const void *p) { return p ? ((const union alloc_header *)p - 1)->size : 0; }
static const JSMallocFunctions allocator = {qmalloc, qfree, qrealloc, qusable};

JSRuntime *web_js_new_runtime(struct js_alloc_diagnostics *diagnostics) {
    return JS_NewRuntime2(&allocator, diagnostics);
}
