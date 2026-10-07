/* Nocturne-native Lexbor allocator port. No filesystem, POSIX or thread port.
 * Lexbor's process-global allocator hooks must only change while no Lexbor
 * objects exist; parser/document lifetimes use Nocturne's ordinary allocator.
 */
#include "lexbor/core/base.h"

static lexbor_memory_malloc_f native_malloc = malloc;
static lexbor_memory_realloc_f native_realloc = realloc;
static lexbor_memory_calloc_f native_calloc = calloc;
static lexbor_memory_free_f native_free = free;

void *lexbor_malloc(size_t size) { return native_malloc(size); }
void *lexbor_realloc(void *ptr, size_t size) { return native_realloc(ptr, size); }
void *lexbor_calloc(size_t count, size_t size) { return native_calloc(count, size); }
void *lexbor_free(void *ptr) { native_free(ptr); return NULL; }

lxb_status_t lexbor_memory_setup(lexbor_memory_malloc_f new_malloc,
                               lexbor_memory_realloc_f new_realloc,
                               lexbor_memory_calloc_f new_calloc,
                               lexbor_memory_free_f new_free) {
    if (!new_malloc || !new_realloc || !new_calloc || !new_free)
        return LXB_STATUS_ERROR_OBJECT_IS_NULL;
    native_malloc = new_malloc;
    native_realloc = new_realloc;
    native_calloc = new_calloc;
    native_free = new_free;
    return LXB_STATUS_OK;
}
