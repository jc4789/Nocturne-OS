#pragma once
#include <stddef.h>
#ifdef __TINYC__
void *alloca(size_t n);
#else
#define alloca __builtin_alloca
#endif
