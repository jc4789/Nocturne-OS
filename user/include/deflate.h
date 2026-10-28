/* DEFLATE (RFC 1951), raw or in a zlib (RFC 1950) or gzip (RFC 1952) wrapper, both
   ways (libc/deflate.c): the one DEFLATE codec of the system, which HTTP's content
   codings, PNG and the Compression Standard's streams all use. Input is fed in
   pieces of any size; each direction keeps a 32 KiB window and its own tables, so
   its memory does not grow with the input. */
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

/* Decompression. Each wrapper is checked as its RFC says: gzip's magic, CM 8, no
   reserved flag, FHCRC, CRC32 and ISIZE; zlib's CM 8, a window of at most 32 KiB,
   FCHECK, no preset dictionary (FDICT) and ADLER32; DEFLATE's block types, code
   lengths and distances. One stream ends the input, unless an option says more. */
enum {
    /* DEFLATE_ZLIB: data whose first two bytes are not a zlib header is a bare
       DEFLATE stream, as some HTTP servers send their "deflate" coding */
    INFLATE_ZLIB_OR_RAW = 1,
    /* DEFLATE_GZIP: members one after another (RFC 1952 2.2), the end only with last */
    INFLATE_MEMBERS = 2,
};
/* Takes decompressed bytes in pieces of at most 16 KiB. false: stop after these,
   and inflate_feed returns INFLATE_STOPPED. */
typedef bool (*inflate_put)(void *context, const uint8_t *bytes, size_t n);
enum inflate_result {
    INFLATE_MORE,    /* every input byte taken; the stream goes on */
    INFLATE_END,     /* the stream ended: *used bytes taken, the rest follow its end */
    INFLATE_INVALID, /* not this format, or with last, cut short; bytes still held are dropped */
    INFLATE_STOPPED, /* put said stop: feed the bytes from in + *used later to go on */
};
struct inflate_stream;

/* NULL when memory runs out */
struct inflate_stream *inflate_begin(enum deflate_wrap wrap, unsigned options, inflate_put put, void *context);
/* Decompresses in[0..n), every decoded byte reaching put before it returns (or
   stops). With last, the input ends here: a stream that has not ended is invalid.
   After the end it takes nothing more and returns INFLATE_END; after
   INFLATE_INVALID it returns that again. */
enum inflate_result inflate_feed(struct inflate_stream *s, const void *in, size_t n, bool last, size_t *used);
void inflate_end(struct inflate_stream *s);
