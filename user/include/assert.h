#pragma once
#include <stdio.h>
#include <stdlib.h>
#define assert(x) do { if (!(x)) { fprintf(stderr, "assertion failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); abort(); } } while (0)
