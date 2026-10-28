/* The user's site data across the browser's stores, as the Settings window shows and clears it:
   cookies (webnet.h), localStorage and sessionStorage (webstorage.h), IndexedDB (webidb.h), OPFS
   (js_opfs.h) and Cache Storage (webcache.h). A site is a registrable domain by the Public Suffix List (webnet_site); a
   site's data is that of every origin whose host is the site or inside it, and a clear removes it
   whole (Storage Standard: a bucket is cleared in its entirety, and the user agent offers to clear
   a site's). The user's choices that decide what is kept are webprefs.h's. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "nocturne.h"
#include "webnet.h"
#include "webstorage.h"

struct websitedata_site {
    char site[254];        /* "": OPFS directories made before their origin was noted */
    size_t cookies;        /* how many */
    uint64_t cookie_bytes; /* their names, values, domains and paths */
    uint64_t storage_bytes;   /* localStorage and sessionStorage: UTF-16 keys and values */
    uint64_t indexeddb_bytes; /* the files of its databases */
    uint64_t opfs_bytes;      /* the files of its origin private file systems */
    uint64_t cache_bytes;     /* the files of its Cache Storage: lists and bodies */
};
/* Every site with data, sorted by site: a malloc'd array (free it) in *out and its length, or a
   negative n_result (no memory, a store that cannot be read). */
long websitedata_sites(webnet *net, webstorage *store, struct websitedata_site **out);
/* What one site's entry adds up to. */
uint64_t websitedata_bytes(const struct websitedata_site *site);

/* Removes the data of site (NULL or "": of every site) from every store: cookies in this and,
   once they read the cookie store, every window; Web Storage in every window; IndexedDB, OPFS
   and Cache Storage on the disk. 0; -N_ERR_BUSY when something in use was left whole (an
   IndexedDB transaction, an OPFS file open for sync access, an origin's caches under a command:
   clearing again later removes it); or the first failure, after the other stores were cleared. */
n_result websitedata_clear(webnet *net, webstorage *store, const char *site);

/* The browser session, which the user's "delete cookies and site data when the browser closes"
   and per-site "clear on close" exceptions end: from the first window to open to the last to
   close, counted in the document "windows" of the browser's temporary folder (rows of process id
   and start time, those of ended processes dropped). begin: when the window opens, after
   webnet_create and webstorage_create; the first window of a session removes what the last
   session left when it was ended before it could, and the caches an earlier session deleted
   while Cache objects still used them (webcache_purge). end: when the window closes, before webnet_free;
   the last window removes the storage of every origin whose host webprefs_site_kept refuses.
   Cookies need no clearing here: the cookie store never holds those (webnet.h). */
void websitedata_session_begin(webstorage *store);
void websitedata_session_end(webstorage *store);
