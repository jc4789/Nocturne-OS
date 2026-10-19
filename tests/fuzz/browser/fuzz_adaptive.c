/* Fuzzes user/libc/media_adaptive_parser.c: HLS (m3u8) and DASH (MPD/XML) manifests and URI resolution.
   Byte 0 bit0 selects DASH. The text is parsed in place (NUL-terminated copy, as the product does). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "media_adaptive_private.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    bool dash = data[0] & 1;
    size_t n = size - 1;
    char *text = malloc(n + 1); memcpy(text, data + 1, n); text[n] = 0;
    struct nmedia_adaptive_manifest *m = calloc(1, sizeof *m);
    char err[160];
    if (dash) nmedia_adaptive_parse_dash(text, n, "https://example.com/dir/manifest.mpd?x=1", m, err, sizeof err);
    else nmedia_adaptive_parse_hls(text, n, "https://example.com/dir/master.m3u8", m, err, sizeof err);
    nmedia_adaptive_manifest_clear(m);
    free(m);
    char out[512];
    nmedia_adaptive_resolve("https://example.com/a/b/c.m3u8?q", text, out, sizeof out);
    char *owned = NULL;
    if (nmedia_adaptive_resolve_owned("https://example.com:8443/a/b/c", text, &owned) == HTTP_URL_TUPLE) free(owned);
    free(text);
    return 0;
}

/* The media allocator is a thin accounting layer over the C heap; the harness uses the host heap directly. */
void *nmedia_ff_malloc(size_t n) { return malloc(n); }
void *nmedia_ff_realloc(void *p, size_t n) { return realloc(p, n); }
void nmedia_ff_free(void *p) { free(p); }
void *nmedia_ff_mallocz(size_t n) { return calloc(1, n); }
