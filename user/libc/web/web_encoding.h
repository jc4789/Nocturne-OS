/* Small, allocation-free Encoding Standard decoder shared by HTML and future
   TextDecoder bindings. This is not an OS or process ABI. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct web_shift_jis_decoder {
    uint8_t leading; /* zero-initialize; retained between non-final chunks */
} web_shift_jis_decoder;

/* false from emit stops decoding. malformed distinguishes a replacement caused
   by a decoding error, so a future fatal TextDecoder need not guess from U+FFFD.
   The callback must not reenter this decoder. */
typedef bool (*web_decode_emit)(void *opaque, uint32_t scalar, bool malformed);
bool web_shift_jis_label(const char *label);
bool web_shift_jis_decode(web_shift_jis_decoder *decoder,
                         const uint8_t *bytes, size_t length, bool final,
                         web_decode_emit emit, void *opaque);
