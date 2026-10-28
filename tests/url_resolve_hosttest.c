/* Host side of tests/check_url_resolve.py: runs the native callers' URL parser
   (web_url_resolve and web_resolve_url_owned, user/libc/web/web_url.c) over a
   file of requests and writes a file of results.
   Each request: label length (u32), label, base length (u32; 0xffffffff for no
   base), base, input length (u32), input. An empty label is
   web_resolve_url_owned, any other web_url_resolve with that encoding. Each
   result: status (i8: 1, 0 or -1), length (u32), the href.
   Then the same requests on 8 threads at once, each from its own offset, 4
   times over: a result unlike the first pass is counted and printed. */
#include <lexbor/encoding/encoding.h>
#include <process.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* web_url.h's and web_encoding.h's, declared here: the harness is built with the
   host's headers, web_url.c with Nocturne's (user/include). */
int web_url_resolve(const char *base, const char *input, lxb_encoding_t encoding, char **out);
int web_resolve_url_owned(const char *base, const char *rel, char **out);
lxb_encoding_t web_encoding_get(const char *label, size_t length);

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct request { char *label, *base, *input; };
struct result { int status; char *href; };
static struct request *requests;
static struct result *expected;
static size_t count;
static volatile LONG mismatches;

static char *field(const uint8_t **at, const uint8_t *end, bool *none) {
    if (end - *at < 4) return NULL;
    uint32_t length = u32(*at);
    *at += 4;
    if (none) *none = length == 0xffffffffu;
    if (none && *none) return NULL;
    if ((size_t)(end - *at) < length) return NULL;
    char *text = malloc(length + 1);
    if (!text) return NULL;
    memcpy(text, *at, length);
    text[length] = 0;
    *at += length;
    return text;
}

static int resolve(const struct request *r, char **out) {
    return *r->label ? web_url_resolve(r->base, r->input, web_encoding_get(r->label, strlen(r->label)), out)
                     : web_resolve_url_owned(r->base, r->input, out);
}

static unsigned __stdcall worker(void *opaque) {
    size_t offset = (size_t)(uintptr_t)opaque;
    for (int round = 0; round < 4; round++)
        for (size_t k = 0; k < count; k++) {
            size_t i = (k + offset * (count / 8 + 1)) % count;
            char *href = NULL;
            int status = resolve(&requests[i], &href);
            if (status != expected[i].status || (href && strcmp(href, expected[i].href))) {
                InterlockedIncrement(&mismatches);
                printf("thread mismatch: request %zu\n", i);
            }
            free(href);
        }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) return 2;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) return 2;
    const uint8_t *at = data, *end = data + size;
    size_t capacity = 0;
    while (at < end) {
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 1024;
            requests = realloc(requests, capacity * sizeof *requests);
            if (!requests) return 2;
        }
        struct request *r = &requests[count];
        bool none;
        r->label = field(&at, end, NULL);
        r->base = r->label ? field(&at, end, &none) : NULL;
        if (!r->label || (!r->base && !none)) return 2;
        r->input = field(&at, end, NULL);
        if (!r->input) return 2;
        count++;
    }
    expected = calloc(count ? count : 1, sizeof *expected);
    if (!expected) return 2;
    for (size_t i = 0; i < count; i++) {
        char *href = NULL;
        int status = resolve(&requests[i], &href);
        if ((status == 1) != (href != NULL)) return 3; /* *out is set exactly on success */
        size_t length = href ? strlen(href) : 0;
        uint8_t head[5] = {(uint8_t)(int8_t)status, (uint8_t)length, (uint8_t)(length >> 8),
                           (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
        fwrite(head, 1, 5, out);
        fwrite(href ? href : "", 1, length, out);
        expected[i] = (struct result){status, href};
    }
    fclose(out);
    HANDLE threads[8];
    for (unsigned t = 0; t < 8; t++)
        threads[t] = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)(uintptr_t)t, 0, NULL);
    WaitForMultipleObjects(8, threads, TRUE, INFINITE);
    printf("url_resolve_hosttest: %zu requests, 8 threads x 4 rounds, %ld mismatches\n", count, mismatches);
    return mismatches != 0;
}
