/* IndexedDB: the native half of js_indexeddb.js for a process's QuickJS realms, the Window
 * realms of js.c and the Worker realm of browserjsworker.c. https://w3c.github.io/IndexedDB/
 * Each realm owns a struct web_idb: its own storage broker (webidb_async.h, the child
 * browserstorageworker) and its requests waiting for it, in the order they were made. All the
 * brokers of all the processes work on the same databases on the disk (webidb.h): a database's
 * lease serializes their transactions, and its connection table in the profile's temporary folder
 * carries versionchange and blocked between the connections of every realm of the origin, Window
 * or Worker. A result comes back as a task of the realm's own event loop: web_idb_next says one
 * can run, web_idb_complete runs it (its request's promise settles). */
#pragma once
#include "quickjs.h"
#include <webidb_async.h>
#include <stdbool.h>
#include <stdint.h>

struct web_idb_pending;
struct web_idb {
    webidb_async *broker; /* started by the first request */
    struct web_idb_pending *pending, *pending_last; /* the requests the broker has not answered */
    struct webidb_async_result taken; /* the result web_idb_next took, while has_taken */
    bool has_taken;
    bool failed; /* the broker died: each waiting request gets WEB_IDB_IO, none is sent again */
    bool used; /* the realm made a request: its task ends deactivate its transactions */
};
/* What only the realm's owner knows, the same for every realm of the process. */
struct web_idb_realm {
    /* The realm's IndexedDB; NULL when ctx is not (or no longer) a realm of the owner. */
    struct web_idb *(*state)(JSContext *ctx);
    /* JS_UNDEFINED while the realm may reach its storage now; otherwise it throws and returns
       JS_EXCEPTION. Asked before and after the arguments are converted (author code runs). */
    JSValue (*authority)(JSContext *ctx);
    /* Storage's "obtain a storage key": the serialized origin of the realm's storage key, which
       the caller frees with JS_FreeCString; NULL for an opaque origin, or with an exception. */
    const char *(*origin)(JSContext *ctx);
};
/* Registers host.indexedDB (the broker's operations, each a promise of its result) and
   host.indexedDBStorageKey (whether open and deleteDatabase may obtain a storage key). */
void web_idb_host_init(JSContext *ctx, JSValueConst host, const struct web_idb_realm *realm);
/* Moves the broker's transport on without waiting: an interrupt handler may call it. */
void web_idb_pump(struct web_idb *idb);
bool web_idb_used(const struct web_idb *idb);
/* Requests the broker has not answered yet. The broker is not in an event queue: while some
   wait, the realm's event loop looks again soon (js.c and browserjsworker.c: 8 ms). */
bool web_idb_waiting(const struct web_idb *idb);
/* The sequence of the request whose completion task can run now, 0 when none can. */
uint32_t web_idb_next(struct web_idb *idb);
/* The completion task's work, inside the caller's task (and before its microtask checkpoint):
   the request's promise settles. An exception doing so stays pending in ctx for the caller. */
void web_idb_complete(JSContext *ctx, struct web_idb *idb);
/* Before the realm goes: its requests are dropped and its broker retires (its connections and
   leases go with it); the realm may begin again with a new broker. */
void web_idb_release(JSContext *ctx, struct web_idb *idb);
