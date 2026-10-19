#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "quickjs.h"
/* What the accounting allocator records about a runtime's heap. */
struct js_alloc_diagnostics { size_t current, peak, requested, used, limit; unsigned failures, reported; bool quota; };
/* A runtime whose every allocation is counted at its exact size, so the
 * cycle collector's threshold and the memory limit see the real heap
 * (QuickJS's default allocator cannot size Nocturne malloc blocks).
 * diagnostics may be NULL. */
JSRuntime *web_js_new_runtime(struct js_alloc_diagnostics *diagnostics);
/* Before a large host-side allocation: collect unreachable JS cycles once if
 * it would exhaust the existing runtime quota. Never called by the allocator
 * itself, and never changes the limit or releases live author-owned buffers. */
void web_js_prepare_bytes(JSContext *ctx, size_t bytes);
