/* HTTP's br content coding (http_coding.h) uses the brotli decoder libc already
   links for WOFF2 fonts. Only this file sees brotli's headers, so http.c and the
   host tests that compile it stay free of them. */
#include <stddef.h>
#include <brotli/decode.h>

void *http_br_create(void) {
    return BrotliDecoderCreateInstance(NULL, NULL, NULL);
}

/* 1: the stream ended. 0: every input byte was taken, or the output is full
   (*room is 0). -1: the data is not a valid brotli stream. */
int http_br_decode(void *state, const unsigned char **in, size_t *in_n, unsigned char **out, size_t *room) {
    switch (BrotliDecoderDecompressStream(state, in_n, in, room, out, NULL)) {
    case BROTLI_DECODER_RESULT_SUCCESS: return 1;
    case BROTLI_DECODER_RESULT_ERROR: return -1;
    default: return 0;
    }
}

void http_br_free(void *state) {
    BrotliDecoderDestroyInstance(state);
}
