/* Cache Storage: the native half of js_cache_storage.js for a process's QuickJS realms, the Window
 * realms of js.c and the Worker realm of browserjsworker.c (Service Workers, "Caches",
 * https://w3c.github.io/ServiceWorker/#cache-objects). Each command goes to the realm's storage
 * broker, the one its IndexedDB uses (js_indexeddb.h), in the order the realm made its requests;
 * the broker runs webcache.h's engine on the origin's caches. The origin and the right to reach
 * storage come from the realm's owner (struct web_idb_realm), never from the script. */
#pragma once
#include "quickjs.h"
#include "js_indexeddb.h"

/* Registers host.cacheStorage(operation, ...): a promise of the broker's result, [status, ...]
   (js_cache_storage.js reads it), or a TypeError thrown when the realm has gone or may not reach
   its storage now; and host.cacheEmbedderPolicy(), the realm's embedder policy now. */
void web_cache_host_init(JSContext *ctx, JSValueConst host, const struct web_idb_realm *realm);
