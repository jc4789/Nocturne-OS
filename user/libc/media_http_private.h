#pragma once
#include <stddef.h>
#include <stdint.h>

/* Single-owner synchronous anonymous HTTP(S) reader. Not a browser fetch API. */
#define NMEDIA_HTTP_CACHE_BYTES (256u * 1024u)
#define NMEDIA_HTTP_URL_BYTES 2048u
typedef struct nmedia_http nmedia_http;
nmedia_http *nmedia_http_open(const char *url, char *error, size_t error_size);
int nmedia_http_read(nmedia_http *reader, void *out, int count);
int64_t nmedia_http_seek(nmedia_http *reader, int64_t offset, int whence);
int64_t nmedia_http_size(const nmedia_http *reader);
const char *nmedia_http_error(const nmedia_http *reader);
void nmedia_http_close(nmedia_http *reader);
