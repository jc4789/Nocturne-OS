#pragma once
#include "quickjs.h"
/* Before a large host-side allocation: collect unreachable JS cycles once if
 * it would exhaust the existing runtime quota. Never called by the allocator
 * itself, and never changes the limit or releases live author-owned buffers. */
void web_js_prepare_bytes(JSContext *ctx, size_t bytes);
