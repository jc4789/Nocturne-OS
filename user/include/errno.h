#pragma once
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
int *__errno_location(void);
NOCTURNE_END_DECLS
/* ISO C: errno holds the reason the last library call failed. */
#define errno (*__errno_location())
#define EDOM 1
#define ERANGE 2
#define EILSEQ 3
