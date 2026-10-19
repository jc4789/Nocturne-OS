/* Fuzzes user/libc/webcookie.c: Set-Cookie parsing, jar lookup, snapshot import/export. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "webcookie.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    unsigned mode = data[0] % 3;
    const uint8_t *p = data + 1; size_t n = size - 1;
    webcookie_jar *j = webcookie_create();
    if (!j) return 0;
    static const char psl[] = "com\nco.uk\n*.ck\n!www.ck\n";
    webcookie_psl_load(j, psl, sizeof psl - 1);
    if (mode == 2) {
        webcookie_import(j, p, n, 1000000000);
    } else {
        char *v = malloc(n ? n : 1); memcpy(v, p, n);
        struct webcookie_context ctx; memset(&ctx, 0, sizeof ctx);
        ctx.url = "https://www.example.co.uk/a/b?q"; ctx.site_url = ctx.url; ctx.method = "GET";
        ctx.http = mode == 1; ctx.top_level = true;
        webcookie_set(j, &ctx, v, n, 1000000000);
        webcookie_set(j, &ctx, v, n, 1000000000 + 100000000);
        char out[4096];
        webcookie_get(j, &ctx, out, sizeof out, 1000000000);
        free(v);
    }
    long need = webcookie_export(j, NULL, 0, 1000000000);
    if (need > 0) {
        void *buf = malloc((size_t)need);
        long got = webcookie_export(j, buf, (size_t)need, 1000000000);
        if (got > 0) { webcookie_jar *k = webcookie_create(); webcookie_import(k, buf, (size_t)got, 1000000000); webcookie_free(k); }
        free(buf);
    }
    webcookie_free(j);
    return 0;
}
