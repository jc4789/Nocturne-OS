/* Fuzzes user/libc/http.c (HTTP/1.1 response head, chunked, content-length, gzip/deflate content
   decoding) and user/libc/http2.c (frame parser, HPACK) by serving fuzz bytes as the server.
   Byte 0: bit0 = ALPN h2, bit1 = collect body (vs stream callback), bits2..7 = read chunk size class.
   For h2 a SETTINGS frame is prepended so the fuzzer starts at the interesting frames. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../user/include/http.h"
#include "../../../user/include/http.h"
#include "net_stub.h"

static int on_body(void *ctx, const char *d, size_t n) { *(size_t *)ctx += n; return 0; }
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    unsigned flags = data[0];
    bool h2 = flags & 1;
    size_t chunk = ((flags >> 2) % 6 == 0) ? 1 : ((flags >> 2) % 6 == 1) ? 3 : ((flags >> 2) % 6 == 2) ? 16 : ((flags >> 2) % 6 == 3) ? 100 : 4096;
    static const uint8_t settings[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
    size_t total = (h2 ? 9 : 0) + size - 1;
    uint8_t *in = malloc(total ? total : 1); /* exact size: ASan catches overreads */
    if (h2) memcpy(in, settings, 9);
    memcpy(in + (h2 ? 9 : 0), data + 1, size - 1);
    stub_reset(in, total, chunk, h2);
    size_t seen = 0;
    struct http_req rq; memset(&rq, 0, sizeof rq);
    rq.url = "http://fuzz.test/x?y"; rq.timeout_ms = 1000;
    rq.headers = "Accept-Encoding: gzip\r\n";
    if (!(flags & 2)) { rq.on_body = on_body; rq.ctx = &seen; }
    struct http_resp rs; memset(&rs, 0, sizeof rs);
    http_request_limited(&rq, &rs, 1u << 20);
    http_resp_free(&rs);
    http_pool_clear();
    free(in);
    return 0;
}
