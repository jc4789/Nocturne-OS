/* Browser-owned, memory-only cookies. No kernel, filesystem, or JS dependency. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define WEBCOOKIE_FIELD_MAX 8192u
#define WEBCOOKIE_OUTPUT_MAX 32768u
#define WEBCOOKIE_SNAPSHOT_MAX (256u * 1024u)
#define WEBCOOKIE_PSL_MAX (2u * 1024u * 1024u)

typedef struct webcookie_jar webcookie_jar;
struct webcookie_context {
    const char *url;
    /* Trusted top-level site-for-cookies, never a page-supplied Origin header.
       NULL is no client (address-bar navigation); "" is an opaque client. */
    const char *site_url;
    const char *method;
    bool top_level;
    bool http; /* false for document.cookie: HttpOnly is never exposed */
    bool redirect_cross_site;
};

webcookie_jar *webcookie_create(void);
void webcookie_free(webcookie_jar *);
/* Load trusted complete ASCII/A-label PSL, including PRIVATE rules. Unicode
   rules must be converted on the host. Failed loads keep the prior policy.
   Without a PSL, Domain cookies cannot widen scope and sites are exact-host. */
bool webcookie_psl_load(webcookie_jar *, const char *text, size_t len);
bool webcookie_same_site(const webcookie_jar *, const char *a, const char *b);
/* One Set-Cookie field VALUE (not a combined header). 1 accepted (including
   deletion), 0 rejected by policy/syntax, -1 allocation/size failure. now: UTC s. */
int webcookie_set(webcookie_jar *, const struct webcookie_context *, const char *value, size_t len, int64_t now);
/* Cookie value only (no "Cookie:" or CRLF). Empty is valid. out=NULL queries
   length (excluding NUL). -1 on invalid URL or insufficient capacity; output
   is cleared, never partially serialized. */
long webcookie_get(webcookie_jar *, const struct webcookie_context *, char *out, size_t cap, int64_t now);
/* Privileged browser->worker transport ONLY: includes HttpOnly secrets.
   No pointers/padding/native-endian fields. out=NULL queries required bytes.
   Import is atomic and only permitted into an empty jar. Never merge a worker
   snapshot into the browser jar; merge new response events instead. */
long webcookie_export(webcookie_jar *, void *out, size_t cap, int64_t now);
bool webcookie_import(webcookie_jar *, const void *data, size_t len, int64_t now);
