/* Fuzzes the URL parsers in user/libc/http.c. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../user/include/http.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = malloc(size + 1); memcpy(s, data, size); s[size] = 0;
    struct url u; url_parse(s, &u);
    struct url_owned o;
    if (url_parse_owned_n(s, size, &o)) url_owned_free(&o);
    char *canon = NULL, *origin = NULL;
    if (http_canonical_owned_n(s, size, &canon, &origin) == HTTP_URL_TUPLE) {
        /* canonicalization must be idempotent */
        char *c2 = NULL, *o2 = NULL;
        if (http_canonical_owned_n(canon, strlen(canon), &c2, &o2) != HTTP_URL_TUPLE || strcmp(c2, canon) || strcmp(o2, origin)) __builtin_trap();
        free(c2); free(o2);
    }
    free(canon); free(origin); free(s);
    return 0;
}
