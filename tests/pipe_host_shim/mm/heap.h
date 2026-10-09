#pragma once
#include <stdlib.h>
static inline void *kzalloc(size_t n) { return calloc(1,n); }
static inline void kfree(void *p) { free(p); }
