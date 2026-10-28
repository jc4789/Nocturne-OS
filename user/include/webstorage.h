/* Browser-owned storage, not a page-accessible filesystem API. All text is
 * length-delimited CESU-8: one UTF-16 code unit per sequence, including lone
 * surrogates and embedded NUL. origin must be a canonical HTTP(S) origin. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <nocturne.h>

enum { WEB_STORAGE_LOCAL, WEB_STORAGE_SESSION };
enum { WEB_STORAGE_LENGTH, WEB_STORAGE_KEY, WEB_STORAGE_GET, WEB_STORAGE_SET,
       WEB_STORAGE_REMOVE, WEB_STORAGE_CLEAR, WEB_STORAGE_CHECK,
       WEB_STORAGE_SUBSCRIBE, WEB_STORAGE_POLL, WEB_STORAGE_ACK, WEB_STORAGE_RELEASE };
enum { WEB_STORAGE_OK, WEB_STORAGE_SECURITY, WEB_STORAGE_QUOTA, WEB_STORAGE_IO };
/* The quota of one storage area (UTF-16 key + value bytes): an origin's localStorage, or its
 * sessionStorage in one tab. It is the only bound on what a page stores: no count of origins,
 * keys or tabs, and no total of a window, refuses a page before it; only memory and the disk do. */
#define WEB_STORAGE_QUOTA_BYTES (5u * 1024u * 1024u)
/* The token of webstorage_watch's watch in the window's queue. */
#define WEBSTORAGE_TOKEN (UINT64_C(0x53) << 56)

struct web_storage_request {
    int kind, operation;
    const char *key, *value;
    size_t key_len, value_len;
    uint32_t index;
    /* Native document identity, never a page-provided ID or filesystem path. */
    uint64_t source, cursor;
    const char *url;
};
struct web_storage_result {
    char *text; /* malloc'd; caller frees; NULL for an absent key */
    size_t text_len;
    uint32_t length;
    /* Host-only aggregate diagnostics. No key/value/origin is retained here.
     * Bytes are serialized snapshot bytes, not a claim of completed disk IO. */
    uint32_t save_attempts;
    uint64_t save_ms, snapshot_bytes;
    bool event;
    char *event_key, *old_value, *new_value, *url;
    size_t event_key_len, old_len, new_len;
    uint64_t cursor;
};
typedef struct webstorage webstorage;
/* One instance per browser window, surviving document navigation. Local writes
 * go to the fixed managed folder AppData\Browser\storage of the user's folder. No path argument. */
webstorage *webstorage_create(void);
void webstorage_free(webstorage *store);
/* A tab's sessionStorage: the session storage shed of one top-level traversable (HTML, Storage
 * Standard). The window makes one for each tab it opens and names it in every request of that
 * tab's documents; page code never chooses it. 0 when it cannot be made (no memory). Numbers are
 * never given twice by one store. */
uint64_t webstorage_session_open(webstorage *store);
/* The tab closed: its sessionStorage goes, with the storage events still queued for its
 * documents ("session storage buckets must be cleared as traversable navigables are closed").
 * sessionStorage requests that still name it fail with WEB_STORAGE_SECURITY. */
void webstorage_session_close(webstorage *store, uint64_t session);
/* Mutations publish to RAM immediately. Idle commits coalesce all writes to an
 * origin; force is used for navigation/window shutdown. A failure keeps the
 * dirty RAM map for retry and leaves the last completed snapshot untouched. */
int webstorage_flush(webstorage *store, bool force, struct web_storage_result *out);
bool webstorage_pending(webstorage *store);
/* session is the requesting document's tab (webstorage_session_open). sessionStorage requests
 * use that tab's area; localStorage is the origin's, one for every tab and window, and ignores
 * it, as WEB_STORAGE_RELEASE (origin NULL) does. */
int webstorage_access(webstorage *store, uint64_t session, const char *origin,
                      const struct web_storage_request *request,
                      struct web_storage_result *out);
void webstorage_result_free(struct web_storage_result *result);

/* Another program (the Settings window) can clear site data while this window holds
 * sessionStorage in its memory. webstorage_watch watches for that in queue under
 * WEBSTORAGE_TOKEN; call it once after webstorage_create, before documents run: clears made
 * before it are older than every sessionStorage of this store. webstorage_events applies those
 * clears between tasks, from the events of a wait (others are left alone). Without the watch,
 * only this store's own clears reach its sessionStorage. */
n_result webstorage_watch(webstorage *store, n_handle queue);
void webstorage_events(webstorage *store, const struct n_event *events, size_t count);

/* Web Storage of one origin, for the browser's site data settings. local and session are what
 * the quota counts (UTF-16 bytes of keys and values; the Storage Standard's storage usage):
 * local is the origin's localStorage as every window sees it, session its sessionStorage in this
 * store's tabs (another program's tabs keep theirs in their memory). disk is the bytes of the
 * origin's saved localStorage in the profile folder. */
struct web_storage_usage {
    char *origin;
    uint64_t local, session, disk;
};
/* Every origin with Web Storage, in a new array sorted by origin; the count, or -1 (no memory,
 * a folder that cannot be read). */
long webstorage_usage(webstorage *store, struct web_storage_usage **out);
void webstorage_usage_free(struct web_storage_usage *usage, size_t count);
/* The user agent clears storage (not a page's Storage.clear): every map of the origins whose
 * host is domain or inside it (any scheme and port) becomes empty in its entirety, localStorage
 * on the disk and in every window, sessionStorage in every tab of every window (another
 * program's through its webstorage_watch). Open documents read the empty maps; no storage event
 * fires, as HTML fires one only for a Storage object's own setItem, removeItem and clear. domain
 * is a site's registrable domain (or host) as the public suffix list gives it.
 * webstorage_clear_all does the same for every origin. */
int webstorage_clear_site(webstorage *store, const char *domain);
int webstorage_clear_all(webstorage *store);
/* The browser's profile folder, where everything it keeps lives (localStorage, IndexedDB, OPFS,
 * search history): Browser in the user's AppData. With temporary, its folder for what lasts only
 * until the next start: Browser in the temporary folder. Made when absent and found once; NULL
 * when it cannot be. */
const char *web_profile_folder(bool temporary);
