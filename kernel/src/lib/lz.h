#pragma once
/* A small LZ77 block compressor for the hibernation image (dev/hibernate.c): blocks of at most
   64 KiB, matches of 4 bytes and more found through a hash of the next 4 bytes.
   A block is a run of sequences: a token (literal count in the high nibble, match length - 4 in
   the low one, 15 meaning "more in the following bytes, each 255 adding on"), the literals, a
   16-bit little-endian distance back and the length bytes. The last sequence has literals only. */
#include <stddef.h>
#include <stdint.h>

#define LZ_BLOCK_MAX 65536
#define LZ_TABLE_BYTES (4096 * sizeof(uint32_t)) /* the compressor's scratch */

/* The compressed size, or 0 when it would not fit in cap (store the block as it is). */
size_t lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, uint32_t *table);
/* The size written to dst, or -1 for a damaged block (nothing is read or written out of bounds). */
long lz_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
