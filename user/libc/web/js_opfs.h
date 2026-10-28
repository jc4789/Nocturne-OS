/* Origin private file system storage for a process's QuickJS realms.
 * Each origin owns AppData\Browser\opfs\<sha256(origin)> in the user's folder; entry names are
 * encoded into case-insensitive, file-name-safe spellings so every Web name keeps
 * its identity. Sync access handles are exclusive kernel vnode leases, so
 * Window, Worker and other browser processes see one lock per file. */
#pragma once
#include "quickjs.h"

/* Register host.opfs* on a realm's private host object. url returns the
 * realm's storage URL (Worker: creator snapshot; Window: document URL), or
 * NULL while unknown; opaque and non-HTTP(S) origins have no storage. */
void web_opfs_host_init(JSContext *ctx, JSValueConst host, const char *(*url)(JSContext *ctx));
/* Close every access handle of ctx before the realm is freed. */
void web_opfs_release(JSContext *ctx);
/* The Settings window's site data, from any browser process. usage: each origin's directory to
 * each, with the origin ("" for one made before origins were noted beside them) and the bytes of
 * its files. clear: removes the directories of the origins match accepts (NULL: all) whole; one
 * with a document open for sync access in any process is left whole. 0, -N_ERR_BUSY when one was
 * left, or the failure. */
#include <stdbool.h>
#include <stdint.h>
#include "nocturne.h"
n_result web_opfs_usage(void (*each)(void *opaque, const char *origin, uint64_t bytes), void *opaque);
n_result web_opfs_clear(bool (*match)(void *opaque, const char *origin), void *opaque);
