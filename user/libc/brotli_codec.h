/* Brotli (RFC 7932) streams through the brotli library in third_party/brotli
   (third_party_brotli.c): HTTP's br content coding (http_coding.h) and the
   Compression Standard's "brotli" (web/js_compression.c). Streams of the RFC only:
   the large-window extension is not enabled. Both directions move bytes from
   *in (*in_n of them) to *out (*room free), advancing the pointers and counts. */
#pragma once
#include <stddef.h>
#include <stdbool.h>

/* NULL when memory runs out */
void *brotli_decoder_begin(void);
/* 1: the stream ended (*in_n bytes follow it). 0: every input byte was taken, or
   the output is full (*room is 0). -1: the data is not a valid brotli stream. */
int brotli_decode(void *state, const unsigned char **in, size_t *in_n, unsigned char **out, size_t *room);
void brotli_decoder_end(void *state);

/* quality 0 (fastest) to 11 (smallest), the default 4 MiB window; NULL when memory runs out */
void *brotli_encoder_begin(int quality);
/* Without finish: 1 when every input byte was taken and no output is held back, 0
   when the output is full. With finish (and no new input while it lasts): 1 when the
   stream is complete, 0 when the output is full. -1: the encoder failed. */
int brotli_encode(void *state, const unsigned char **in, size_t *in_n, unsigned char **out, size_t *room, bool finish);
void brotli_encoder_end(void *state);
