#pragma once
#include "quickjs.h"

/* The native half of js_streams.js's CompressionStream and DecompressionStream
   (the Compression Standard's compression contexts), on host:
     compressionNew(format, decompress): a context for format, the index of the
       CompressionFormat enum value in its IDL order ("brotli", "deflate",
       "deflate-raw", "gzip")
     compressionFeed(context, buffer, offset, length, finish): [status, used,
       ...ArrayBuffers of output in order]. status -1: the compressed data is not
       valid; 0: all of the input was taken; 1: the compressed data ended (only
       `used` bytes were taken: the rest follow its end), or with finish the
       compressed output is complete; 2: a step's bound was reached, so call again
       with the input after `used` (and the same finish).
     compressionEnd(context): releases the context's memory before collection.
   The codecs are libc's (deflate.h, brotli_codec.h); a context belongs to its realm. */
void web_js_compression_init(JSContext *ctx, JSValue host);
