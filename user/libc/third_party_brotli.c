/* Brotli (RFC 7932) both ways (brotli_codec.h), over the brotli library libc
   already links for WOFF2 fonts. Only this file sees brotli's headers, so http.c,
   the Compression Standard's streams and the host tests that compile them stay
   free of them. */
#include <stddef.h>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include "brotli_codec.h"

void *brotli_decoder_begin(void) {
    return BrotliDecoderCreateInstance(NULL, NULL, NULL);
}

int brotli_decode(void *state, const unsigned char **in, size_t *in_n, unsigned char **out, size_t *room) {
    switch (BrotliDecoderDecompressStream(state, in_n, in, room, out, NULL)) {
    case BROTLI_DECODER_RESULT_SUCCESS: return 1;
    case BROTLI_DECODER_RESULT_ERROR: return -1;
    default: return 0;
    }
}

void brotli_decoder_end(void *state) {
    BrotliDecoderDestroyInstance(state);
}

void *brotli_encoder_begin(int quality) {
    BrotliEncoderState *state = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (state && !BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, (uint32_t)quality)) {
        BrotliEncoderDestroyInstance(state);
        return NULL;
    }
    return state;
}

int brotli_encode(void *state, const unsigned char **in, size_t *in_n, unsigned char **out, size_t *room, bool finish) {
    if (!BrotliEncoderCompressStream(state, finish ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS,
                                     in_n, in, room, out, NULL))
        return -1;
    if (finish) return BrotliEncoderIsFinished(state) ? 1 : 0;
    return !*in_n && !BrotliEncoderHasMoreOutput(state) ? 1 : 0;
}

void brotli_encoder_end(void *state) {
    BrotliEncoderDestroyInstance(state);
}
