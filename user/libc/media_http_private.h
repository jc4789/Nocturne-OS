#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Single-owner synchronous anonymous HTTP(S) reader. Not a browser fetch API. */
#define NMEDIA_HTTP_CACHE_BYTES (256u * 1024u)
#define NMEDIA_HTTP_URL_BYTES 2048u
#define NMEDIA_HTTP_COMPLETE_BYTES (16u * 1024u * 1024u)
typedef struct nmedia_http nmedia_http;
nmedia_http *nmedia_http_open(const char *url, char *error, size_t error_size);
/* Native document URL only: anonymous CORS on each range, no redirects. */
bool nmedia_http_browser_url(const char *url, const char *document_url);
nmedia_http *nmedia_http_open_cors(const char *url, const char *document_url,
                                  char *error, size_t error_size);
/* One bounded 200 response, no Range splicing or global cookies. NULL native
 * document is only for a native caller; otherwise anonymous CORS is enforced.
 * Returned bytes belong to nmedia_ff_free, including the zero-length case. */
bool nmedia_http_get_bounded_cors(const char *url, const char *document_url,
    size_t maximum, uint8_t **bytes, size_t *length, char *error, size_t error_size);
int nmedia_http_read(nmedia_http *reader, void *out, int count);
int64_t nmedia_http_seek(nmedia_http *reader, int64_t offset, int whence);
int64_t nmedia_http_size(const nmedia_http *reader);
const char *nmedia_http_error(const nmedia_http *reader);
void nmedia_http_close(nmedia_http *reader);
