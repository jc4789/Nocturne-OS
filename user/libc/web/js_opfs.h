/* Origin private file system storage for a process's QuickJS realms.
 * Each origin owns /data/browser/opfs/<sha256(origin)>; entry names are
 * encoded into case-insensitive FAT-safe spellings so every Web name keeps
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
