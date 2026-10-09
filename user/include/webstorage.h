/* Browser-owned storage, not a page-accessible filesystem API. All text is
 * length-delimited CESU-8: one UTF-16 code unit per sequence, including lone
 * surrogates and embedded NUL. origin must be a canonical HTTP(S) origin. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { WEB_STORAGE_LOCAL, WEB_STORAGE_SESSION };
enum { WEB_STORAGE_LENGTH, WEB_STORAGE_KEY, WEB_STORAGE_GET, WEB_STORAGE_SET,
       WEB_STORAGE_REMOVE, WEB_STORAGE_CLEAR, WEB_STORAGE_CHECK };
enum { WEB_STORAGE_OK, WEB_STORAGE_SECURITY, WEB_STORAGE_QUOTA, WEB_STORAGE_IO };
#define WEB_STORAGE_QUOTA_BYTES (5u * 1024u * 1024u) /* UTF-16 key + value bytes */

struct web_storage_request {
    int kind, operation;
    const char *key, *value;
    size_t key_len, value_len;
    uint32_t index;
};
struct web_storage_result {
    char *text; /* malloc'd; caller frees; NULL for an absent key */
    size_t text_len;
    uint32_t length;
    /* Host-only aggregate diagnostics. No key/value/origin is retained here.
     * Bytes are serialized snapshot bytes, not a claim of completed disk IO. */
    uint32_t save_attempts;
    uint64_t save_ms, snapshot_bytes;
};
typedef struct webstorage webstorage;
/* One instance per browser window, surviving document navigation. Local writes
 * go to the fixed /data/browser/storage managed directory. No path argument. */
webstorage *webstorage_create(void);
void webstorage_free(webstorage *store);
int webstorage_access(webstorage *store, const char *origin,
                      const struct web_storage_request *request,
                      struct web_storage_result *out);
