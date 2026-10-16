/* Host-only header surface for -DMALLOCTRIM_MODEL. Do not use for OS builds. */
#pragma once
#ifndef MALLOCTRIM_MODEL
#error "The allocator region-model header is only for the host model"
#endif
#include <stddef.h>
#define N_MEMORY_READ_WRITE 2
void *n_memory_reserve(size_t bytes, int access);
int n_memory_release(void *addr, size_t bytes);
