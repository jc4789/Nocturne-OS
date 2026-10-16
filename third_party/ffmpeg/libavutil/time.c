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

/* Wall time is the system clock's whole seconds; Nocturne has no finer wall
 * clock, and gluing a millisecond counter onto it would jump when the two
 * disagree. Callers here only stamp logs and live streams, so a second is
 * enough. */
int64_t av_gettime(void)
{
    return n_clock_wall() * 1000000;
}

/* Milliseconds since boot, monotonic; the resolution of the native uptime clock. */
int64_t av_gettime_relative(void)
{
    return (int64_t)n_clock_uptime_ms() * 1000;
}

int av_gettime_relative_is_monotonic(void)
{
    return 1;
}

/* The native sleep counts milliseconds; round up so a sleep is never short. */
int av_usleep(unsigned usec)
{
    if (usec)
        n_sleep_ms(((uint64_t)usec + 999) / 1000);
    return 0;
}
