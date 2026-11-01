/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <nocturne.h>

#include "time.h"
#include "error.h"

/* The system's wall clock, in ns since 1970 (UTC). */
int64_t av_gettime(void)
{
    return (int64_t)(n_clock_wall() / N_NS_PER_US);
}

/* The system's monotonic clock. */
int64_t av_gettime_relative(void)
{
    return (int64_t)(n_clock_monotonic() / N_NS_PER_US);
}

int av_gettime_relative_is_monotonic(void)
{
    return 1;
}

/* The native sleep ends at a monotonic deadline in ns. */
int av_usleep(unsigned usec)
{
    if (usec)
        n_sleep_until(n_clock_after((uint64_t)usec * N_NS_PER_US));
    return 0;
}
