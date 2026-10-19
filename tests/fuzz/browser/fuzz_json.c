/* Fuzzes user/libc/json.c: json_parse + json_write round trip. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *copy = malloc(size ? size : 1); /* exact-size heap copy so ASan sees overreads */
    if (!copy) return 0;
    memcpy(copy, data, size);
    const char *err = 0;
    struct json *j = json_parse(copy, size, &err);
    if (j) {
        struct jbuf b; memset(&b, 0, sizeof b);
        json_write(&b, j);
        char *out = jb_take(&b);
        if (out) {
            const char *e2 = 0;
            struct json *k = json_parse(out, strlen(out), &e2);
            if (!k) __builtin_trap(); /* serializer must emit parseable JSON */
            json_free(k);
            free(out);
        }
        json_free(j);
    }
    free(copy);
    return 0;
}
