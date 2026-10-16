#pragma once
#include <stdio.h>
#include <stdlib.h>
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) do { if (!(x)) { fprintf(stderr, "assertion failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); abort(); } } while (0)
#endif
