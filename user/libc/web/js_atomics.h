/* SharedArrayBuffer storage and Atomics.waitAsync for a process's QuickJS
 * realms. Shared buffers live in native shared pages, so a dedicated Worker
 * (a child process) maps the same memory and Atomics.wait/notify meet in the
 * kernel's physical-address waiter list. waitAsync waiters are kernel address
 * watches whose notifications arrive in one native event queue. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"
#include "nocturne.h"

/* Install shared-page SharedArrayBuffer allocation and waitAsync enrollment. */
void web_atomics_runtime_init(JSRuntime *rt);
/* host.sharedBufferExport(sab) -> [attachment index, mapped bytes, offset,
 * length] inside an open export scope; host.sharedBufferImport(record) maps
 * the attachment of that index; host.sharedBufferAlias(sab) makes a new
 * SharedArrayBuffer object over the same memory in this process;
 * host.sharedBufferReserve(bytes) makes one whose pages are zero-filled at
 * first touch.
 *
 * A shared region reaches another agent only as a channel attachment. The
 * clone code of an outgoing worker message brackets its walk with
 * host.sharedBufferBegin() ... host.sharedBufferSeal() (-> scope id, 0 when
 * nothing was exported; sharedBufferAbandon() on failure). The id rides in
 * slot 2 of the wire array [data, metadata, id]; web_attachments_extract takes
 * the regions out of it for the sender, and host.sharedBufferDiscard(id)
 * drops a sealed scope that is never sent. The receiver collects the handles
 * that arrive ahead of a message in a web_inbox and opens it around the
 * delivery call, so the records of that message import by index. */
void web_atomics_host_init(JSContext *ctx, JSValueConst host);

struct shared_region;
/* Sending side: the regions of one outgoing message, retained until released.
 * Their handles go out one per carrier message ahead of the payload. */
struct web_attachments { struct shared_region **region; size_t count; };
/* Take the attachments out of wire[2] (and cut that slot off, so the array is
 * ready to serialize). False with an exception pending when the slot is not a
 * sealed scope. */
bool web_attachments_extract(JSContext *ctx, JSValueConst wire, struct web_attachments *out);
n_handle web_attachments_handle(const struct web_attachments *attachments, size_t index);
void web_attachments_release(struct web_attachments *attachments);

/* Receiving side: handles received from carriers and not yet claimed by the
 * message that follows them. A claimed entry's handle belongs to its region. */
struct web_inbox_entry { n_handle handle; struct shared_region *region; };
struct web_inbox { struct web_inbox_entry *entry; size_t count, capacity; };
bool web_inbox_add(struct web_inbox *inbox, n_handle handle);
/* Close every unclaimed handle and empty the inbox. */
void web_inbox_close(struct web_inbox *inbox);
/* Imports performed between begin and end read their regions from inbox. */
void web_atomics_import_begin(struct web_inbox *inbox);
void web_atomics_import_end(void);
/* waitAsync tokens in the queue below carry this mark; other owners of the
 * queue (the GUI loop, webnet) use their own. */
#define WEB_ATOMICS_TOKEN (UINT64_C(0x41) << 56)
/* The process's event queue for waitAsync waiters (made at the first call),
 * shared with the thread's main wait: an event loop that waits on it hands
 * every report it gets to web_atomics_events. */
n_handle web_atomics_queue(void);
void web_atomics_events(const struct n_event *events, size_t count);
/* Earliest due waitAsync settlement of ctx: now when one is ready, -1 if none. */
int64_t web_atomics_deadline(JSContext *ctx, uint64_t now);
/* Settle ctx's notified and timed-out waiters in enrollment order. Run inside
 * a task: the caller performs the microtask checkpoint. Returns the count. */
int web_atomics_run(JSContext *ctx, uint64_t now);
/* Withdraw every waiter of ctx before the realm is freed. */
void web_atomics_release(JSContext *ctx);
/* Native work that another thread finishes (Web Crypto's RSA key generation): ctx waits on
 * *word as a waitAsync waiter without a timeout, so the Window and Worker event loops wake
 * for it, and callback("ok") runs in their Atomics.waitAsync task once the thread stored a
 * value other than expected and woke the word (wake_address). The word must stay mapped
 * while it is watched: web_atomics_release withdraws the watch before the realm goes.
 * 0 watching, 1 the word already differs (nothing enrolled), -1 an exception. */
int web_atomics_watch_word(JSContext *ctx, volatile uint32_t *word, uint32_t expected, JSValueConst callback);
/* Debug summary of pending waitAsync waiters; 0 when none. */
int web_atomics_report(char *out, size_t size, uint64_t now);
/* WebAssembly shared memory storage: a region of one header page, whose first
 * word is the agent-wide byte length, followed by max_bytes of zero-filled
 * pages. Returns the data start (one reference) or NULL. */
uint8_t *web_atomics_memory_reserve(uint64_t max_bytes, uint64_t **length);
void web_atomics_memory_retain(uint8_t *data);
void web_atomics_memory_release(uint8_t *data);
/* Clone record for a dedicated Worker (same form as sharedBufferExport). */
JSValue web_atomics_memory_export(JSContext *ctx, uint8_t *data);
/* Map a received record; returns data with one reference, or NULL with an
 * exception. */
uint8_t *web_atomics_memory_import(JSContext *ctx, JSValueConst record, uint64_t **length);
/* Immutable byte images shared with dedicated Workers, such as a compiled
 * WebAssembly module: reserve a zero-filled writable region of at least bytes
 * (one reference; web_atomics_memory_retain/release count it), export a
 * record of its first length bytes, and map a received record read-only
 * (one reference). */
uint8_t *web_atomics_image_reserve(size_t bytes);
JSValue web_atomics_image_export(JSContext *ctx, uint8_t *data, size_t length);
const uint8_t *web_atomics_image_import(JSContext *ctx, JSValueConst record, size_t *length);
/* memory.atomic.wait: 0 ok, 1 not-equal, 2 timed-out; a negative timeout is
 * unbounded. Blocks the calling agent. */
int web_atomics_memory_wait(void *address, uint64_t expected, bool wide, int64_t timeout_ns);
uint32_t web_atomics_memory_notify(void *address, uint32_t count);
/* Expose crossOriginIsolated. Outside an isolated agent cluster the
 * SharedArrayBuffer constructor is not a global; Atomics always is. Call after
 * the platform bootstrap captured its built-ins. -1 leaves an exception. */
int web_atomics_isolation(JSContext *ctx, bool isolated);
