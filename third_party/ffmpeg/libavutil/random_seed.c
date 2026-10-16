/*
 * Copyright (c) 2009 Baptiste Coudurier <baptiste.coudurier@gmail.com>
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

#include <nocturne.h>
#include <math.h>
#include <string.h>
#include "avassert.h"
#include "internal.h"
#include "intreadwrite.h"
#include "timer.h"
#include "random_seed.h"
#include "sha.h"

#ifndef TEST
#define TEST 0
#endif

/* Fallback jitter timer: the native uptime clock, in milliseconds. */
typedef uint64_t jitter_tick_t;
#define jitter_ticks() n_clock_uptime_ms()
#define JITTER_TICKS_PER_SEC 1000

static uint32_t get_generic_seed(void)
{
    uint64_t tmp[120/8];
    struct AVSHA *sha = (void*)tmp;
    jitter_tick_t last_t  = 0;
    jitter_tick_t last_td = 0;
    jitter_tick_t init_t = 0;
    static uint64_t i = 0;
    static uint32_t buffer[512] = { 0 };
    unsigned char digest[20];
    uint64_t last_i = i;
    int repeats[3] = { 0 };

    av_assert0(sizeof(tmp) >= av_sha_size);

    if(TEST){
        memset(buffer, 0, sizeof(buffer));
        last_i = i = 0;
    }else{
#ifdef AV_READ_TIME
        buffer[13] ^= AV_READ_TIME();
        buffer[41] ^= AV_READ_TIME()>>32;
#endif
    }

    for (;;) {
        jitter_tick_t t = jitter_ticks();
        int incremented_i = 0;
        int cur_td = t - last_t;
        if (last_t + 2*last_td + (JITTER_TICKS_PER_SEC > 1000) < t) {
            // If the timer incremented by more than 2*last_td at once,
            // we may e.g. have had a context switch. If the timer resolution
            // is high (JITTER_TICKS_PER_SEC > 1000), require that the timer
            // incremented by more than 1. If the timer resolution is low,
            // it is enough that the timer incremented at all.
            buffer[++i & 511] += cur_td % 3294638521U;
            incremented_i = 1;
        } else if (t != last_t && repeats[0] > 0 && repeats[1] > 0 &&
                   repeats[2] > 0 && repeats[0] != repeats[1] &&
                   repeats[0] != repeats[2]) {
            // If the timer resolution is high, and we get the same timer
            // value multiple times, use variances in the number of repeats
            // of each timer value as entropy. If we get a different number of
            // repeats than the last two unique cases, count that as entropy
            // and proceed to the next index.
            buffer[++i & 511] += (repeats[0] + repeats[1] + repeats[2]) % 3294638521U;
            incremented_i = 1;
        } else {
            buffer[i & 511] = 1664525*buffer[i & 511] + 1013904223 + (cur_td % 3294638521U);
        }
        if (incremented_i && (t - init_t) >= JITTER_TICKS_PER_SEC>>5) {
            if (last_i && i - last_i > 4 || i - last_i > 64 || TEST && i - last_i > 8)
                break;
        }
        if (t == last_t) {
            repeats[0]++;
        } else {
            // If we got a new unique number of repeats, update the history.
            if (repeats[0] != repeats[1]) {
                repeats[2] = repeats[1];
                repeats[1] = repeats[0];
            }
            repeats[0] = 0;
        }
        last_t = t;
        last_td = cur_td;
        if (!init_t)
            init_t = t;
    }

    if(TEST) {
        buffer[0] = buffer[1] = 0;
    } else {
#ifdef AV_READ_TIME
        buffer[111] += AV_READ_TIME();
#endif
    }

    av_sha_init(sha, 160);
    av_sha_update(sha, (const uint8_t *)buffer, sizeof(buffer));
    av_sha_final(sha, digest);
    return AV_RB32(digest) + AV_RB32(digest + 16);
}

int av_random_bytes(uint8_t* buf, size_t len)
{
    /* The system entropy pool fills the whole request or fails. */
    return n_random(buf, len) < 0 ? AVERROR_UNKNOWN : 0;
}

uint32_t av_get_random_seed(void)
{
    uint32_t seed;

    if (av_random_bytes((uint8_t *)&seed, sizeof(seed)) < 0)
        return get_generic_seed();

    return seed;
}
