#pragma once
#include_next <time.h>
#include "nocturne.h"
/* Used only by upstream's fallback jitter seed; this is the native monotonic
 * elapsed clock, not a claim that Nocturne implements POSIX process CPU time. */
typedef int64_t clock_t;
#define CLOCKS_PER_SEC 1000
static inline clock_t clock(void) { return (clock_t)uptime_ms(); }
