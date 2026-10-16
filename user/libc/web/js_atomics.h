/* SharedArrayBuffer storage and Atomics.waitAsync for a process's QuickJS
 * realms. Shared buffers live in native shared pages, so a dedicated Worker
 * (a child process) maps the same memory and Atomics.wait/notify meet in the
 * kernel's physical-address waiter list. waitAsync waiters are kernel address
 * watches whose notifications arrive on one pollable descriptor. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

/* Install shared-page SharedArrayBuffer allocation and waitAsync enrollment. */
void web_atomics_runtime_init(JSRuntime *rt);
/* host.sharedBufferExport(sab) -> [pid, ticket, mapped bytes, byteLength] for
 * a message to a direct parent/child; host.sharedBufferImport(record) maps it;
 * host.sharedBufferAlias(sab) makes a new SharedArrayBuffer object over the
 * same memory in this process. */
void web_atomics_host_init(JSContext *ctx, JSValueConst host);
/* Readable when a waitAsync waiter was notified; -1 before the first one. */
int web_atomics_fd(void);
/* Earliest due waitAsync settlement of ctx: now when one is ready, -1 if none. */
int64_t web_atomics_deadline(JSContext *ctx, uint64_t now);
/* Settle ctx's notified and timed-out waiters in enrollment order. Run inside
 * a task: the caller performs the microtask checkpoint. Returns the count. */
int web_atomics_run(JSContext *ctx, uint64_t now);
/* Withdraw every waiter of ctx before the realm is freed. */
void web_atomics_release(JSContext *ctx);
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
