/* DEFLATE compression (RFC 1951), raw or in a zlib (RFC 1950) or gzip (RFC 1952)
   wrapper (libc/deflate.c). Input is fed in pieces of any size; the compressor keeps
   a 32 KiB window and its own tables, so its memory does not grow with the input. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum deflate_wrap { DEFLATE_RAW, DEFLATE_ZLIB, DEFLATE_GZIP };

/* Where compressed bytes go. A fixed buffer (grow false) that fills up fails the
   stream; a growing one is realloc'd, and p belongs to the caller afterwards. */
struct deflate_out {
    uint8_t *p;
    size_t len, cap;
    bool grow;
};

struct deflate_stream;

/* level 1 (fastest) to 9 (smallest); NULL when memory runs out */
struct deflate_stream *deflate_begin(int level, enum deflate_wrap wrap);
/* Compresses in[0..n) into out. With last, the stream ends: the final block and
   the wrapper's check value follow, and the stream takes no more input.
   0, or -1 when out is full and cannot grow or memory runs out. */
int deflate_feed(struct deflate_stream *s, const void *in, size_t n, bool last, struct deflate_out *out);
void deflate_end(struct deflate_stream *s);
/* The most bytes a stream of n input bytes can take in any wrapper, fed in pieces
   of any size: incompressible data goes out as stored blocks. 0 on overflow. */
size_t deflate_bound(size_t n);
