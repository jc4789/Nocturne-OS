#pragma once
#include <stdio.h>
#include <stdlib.h>
/* ISO C11 7.2: the static_assert convenience macro (C++ has the keyword). */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) do { if (!(x)) { fprintf(stderr, "assertion failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); abort(); } } while (0)
#endif
