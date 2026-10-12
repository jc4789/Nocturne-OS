/* Test-only host CRT isolation; no guest ABI or product headers are changed. */
#include <stdio.h>
#undef SYS_OPEN
