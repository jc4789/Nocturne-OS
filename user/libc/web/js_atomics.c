/* SharedArrayBuffer storage in native shared pages and Atomics.waitAsync
 * settlement for the QuickJS realms of one process. */
#include "js_atomics.h"
#include "nocturne.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

/* One mapping of a shared-page object. Every SharedArrayBuffer object over it
   holds a reference; the last one unmaps it and closes the descriptor. */
struct shared_region {
    struct shared_region *next;
    uint8_t *data;
    size_t bytes;
    int fd;
    unsigned refs;
};
static struct shared_region *regions;

/* The region containing data. A WebAssembly memory's buffers start after its
   header page; a zero-length buffer may sit at a region's end, so an exact
   start wins over an end that touches the next mapping. */
static struct shared_region *region_find(const void *data) {
    const uint8_t *p = data;
    struct shared_region *end = NULL;
    for (struct shared_region *r = regions; r; r = r->next) {
        if (p >= r->data && p < r->data + r->bytes) return r;
        if (p == r->data + r->bytes) end = r;
    }
    return end;
}
static void region_unlink(struct shared_region *region) {
    for (struct shared_region **link = &regions; *link; link = &(*link)->next)
        if (*link == region) { *link = region->next; break; }
    munmap(region->data, region->bytes);
    close(region->fd);
    free(region);
}
static struct shared_region *region_map(int fd, size_t bytes, int prot) {
    struct shared_region *region = calloc(1, sizeof *region);
    if (!region) { close(fd); return NULL; }
    region->data = n_shared_map(0, fd, bytes, prot);
    if (!region->data) { close(fd); free(region); return NULL; }
    region->bytes = bytes; region->fd = fd;
    region->next = regions; regions = region;
    return region;
}
static size_t page_round(size_t size) {
    if (!size) size = 1;
    return size > SIZE_MAX - (N_PAGE_SIZE - 1) ? 0 : (size + N_PAGE_SIZE - 1) & ~(size_t)(N_PAGE_SIZE - 1);
}

static void *sab_alloc(void *opaque, size_t size) {
    (void)opaque;
    size_t bytes = page_round(size);
    if (!bytes) return NULL;
    int fd = n_shared_reserve(bytes);
    if (fd < 0) return NULL;
    struct shared_region *region = region_map(fd, bytes, N_PROT_WRITE);
    if (!region) return NULL;
    region->refs = 1;
    return region->data;
}
static void sab_dup(void *opaque, void *ptr) {
    (void)opaque;
    struct shared_region *region = region_find(ptr);
    if (region) region->refs++;
}
static void sab_free(void *opaque, void *ptr) {
    (void)opaque;
    struct shared_region *region = region_find(ptr);
    if (region && !--region->refs) region_unlink(region);
}

/* A new SharedArrayBuffer object over a region; the region gains a reference
   through sab_dup only once the object exists. */
static JSValue region_buffer(JSContext *ctx, struct shared_region *region, size_t offset, size_t length) {
    JSValue buffer = JS_NewArrayBuffer(ctx, region->data + offset, length, NULL, NULL, true);
    if (!region->refs) region_unlink(region);
    return buffer;
}
static struct shared_region *buffer_region(JSContext *ctx, JSValueConst buffer, size_t *offset, size_t *length) {
    uint8_t *data = JS_GetArrayBuffer(ctx, length, buffer);
    struct shared_region *region = data ? region_find(data) : NULL;
    if (!region && !JS_HasException(ctx)) JS_ThrowTypeError(ctx, "SharedArrayBuffer expected");
    if (region) *offset = (size_t)(data - region->data);
    return region;
}

/* [pid, ticket, mapped bytes, offset, length]: the receiving agent claims the
   ticket once; the offset locates a buffer inside a WebAssembly memory. */
static JSValue region_export(JSContext *ctx, struct shared_region *region, size_t offset, size_t length) {
    long ticket = n_shared_send(region->fd);
    if (ticket < 0) return JS_ThrowRangeError(ctx, "Shared memory could not be offered to the receiving agent");
    JSValue record = JS_NewArray(ctx);
    if (JS_IsException(record)) return record;
    JS_SetPropertyUint32(ctx, record, 0, JS_NewInt32(ctx, getpid()));
    JS_SetPropertyUint32(ctx, record, 1, JS_NewFloat64(ctx, (double)ticket));
    JS_SetPropertyUint32(ctx, record, 2, JS_NewFloat64(ctx, (double)region->bytes));
    JS_SetPropertyUint32(ctx, record, 3, JS_NewFloat64(ctx, (double)offset));
    JS_SetPropertyUint32(ctx, record, 4, JS_NewFloat64(ctx, (double)length));
    return record;
}
/* Map an exported region without a reference yet (refs 0). */
static struct shared_region *region_import(JSContext *ctx, JSValueConst record, size_t *offset, size_t *length,
                                            int prot) {
    double field[5];
    for (unsigned i = 0; i < 5; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, record, i);
        int bad = JS_ToFloat64(ctx, &field[i], v);
        JS_FreeValue(ctx, v);
        if (bad) return NULL;
        if (!(field[i] >= 0) || field[i] != floor(field[i]) || field[i] > 9007199254740991.0) {
            JS_ThrowTypeError(ctx, "Invalid shared memory record");
            return NULL;
        }
    }
    size_t bytes = (size_t)field[2];
    *offset = (size_t)field[3]; *length = (size_t)field[4];
    if (*offset > bytes || *length > bytes - *offset || page_round(bytes) != bytes) {
        JS_ThrowTypeError(ctx, "Invalid shared memory record");
        return NULL;
    }
    int fd = n_shared_receive((int)field[0], (long)field[1]);
    if (fd < 0) { JS_ThrowTypeError(ctx, "Shared memory sender is not reachable from this agent"); return NULL; }
    struct shared_region *region = region_map(fd, bytes, prot);
    if (!region) JS_ThrowRangeError(ctx, "Shared memory could not be mapped");
    return region;
}

static JSValue native_export(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    size_t offset = 0, length = 0;
    struct shared_region *region = buffer_region(ctx, argv[0], &offset, &length);
    return region ? region_export(ctx, region, offset, length) : JS_EXCEPTION;
}
static JSValue native_import(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    size_t offset, length;
    struct shared_region *region = region_import(ctx, argv[0], &offset, &length, N_PROT_WRITE);
    return region ? region_buffer(ctx, region, offset, length) : JS_EXCEPTION;
}
/* host.sharedBufferReserve(bytes): a SharedArrayBuffer whose pages are
   zero-filled at first touch, unlike the constructor's eager clear, for
   large mostly-unused regions such as an OffscreenCanvas frame link. */
static JSValue native_reserve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    double requested;
    if (JS_ToFloat64(ctx, &requested, argv[0])) return JS_EXCEPTION;
    if (!(requested >= 1) || requested > (double)(SIZE_MAX / 2)) return JS_ThrowRangeError(ctx, "Invalid shared buffer size");
    size_t length = (size_t)requested, bytes = page_round(length);
    int fd = bytes ? n_shared_reserve(bytes) : -1;
    struct shared_region *region = fd >= 0 ? region_map(fd, bytes, N_PROT_WRITE) : NULL;
    if (!region) return JS_ThrowRangeError(ctx, "Shared memory could not be reserved");
    return region_buffer(ctx, region, 0, length);
}
static JSValue native_alias(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    size_t offset = 0, length = 0;
    struct shared_region *region = buffer_region(ctx, argv[0], &offset, &length);
    return region ? region_buffer(ctx, region, offset, length) : JS_EXCEPTION;
}

/* WebAssembly shared memories: a header page whose first word is the
   agent-wide byte length, then the reserved data. */
uint8_t *web_atomics_memory_reserve(uint64_t max_bytes, uint64_t **length) {
    size_t data = page_round(max_bytes ? (size_t)max_bytes : 1);
    if (!data || max_bytes > SIZE_MAX || data > SIZE_MAX - N_PAGE_SIZE) return NULL;
    size_t bytes = data + N_PAGE_SIZE;
    int fd = n_shared_reserve(bytes);
    if (fd < 0) return NULL;
    struct shared_region *region = region_map(fd, bytes, N_PROT_WRITE);
    if (!region) return NULL;
    region->refs = 1;
    *length = (uint64_t *)region->data;
    return region->data + N_PAGE_SIZE;
}
void web_atomics_memory_retain(uint8_t *data) { sab_dup(NULL, data); }
void web_atomics_memory_release(uint8_t *data) { sab_free(NULL, data); }
JSValue web_atomics_memory_export(JSContext *ctx, uint8_t *data) {
    struct shared_region *region = region_find(data);
    if (!region) return JS_ThrowTypeError(ctx, "WebAssembly memory is not shared");
    return region_export(ctx, region, (size_t)(data - region->data), 0);
}
uint8_t *web_atomics_memory_import(JSContext *ctx, JSValueConst record, uint64_t **length) {
    size_t offset, unused;
    struct shared_region *region = region_import(ctx, record, &offset, &unused, N_PROT_WRITE);
    if (!region) return NULL;
    if (offset != N_PAGE_SIZE) {
        region_unlink(region);
        JS_ThrowTypeError(ctx, "Invalid WebAssembly memory record");
        return NULL;
    }
    region->refs = 1;
    *length = (uint64_t *)region->data;
    return region->data + N_PAGE_SIZE;
}
/* An image is written in full right away, so its pages are committed when the
   region is created: a shortage fails here instead of faulting mid-copy. */
uint8_t *web_atomics_image_reserve(size_t size) {
    size_t bytes = page_round(size);
    if (!bytes) return NULL;
    int fd = n_shared_create(bytes);
    if (fd < 0) return NULL;
    struct shared_region *region = region_map(fd, bytes, N_PROT_WRITE);
    if (!region) return NULL;
    region->refs = 1;
    return region->data;
}
JSValue web_atomics_image_export(JSContext *ctx, uint8_t *data, size_t length) {
    struct shared_region *region = region_find(data);
    if (!region || data != region->data || length > region->bytes)
        return JS_ThrowTypeError(ctx, "Shared image is not a region");
    return region_export(ctx, region, 0, length);
}
const uint8_t *web_atomics_image_import(JSContext *ctx, JSValueConst record, size_t *length) {
    size_t offset;
    struct shared_region *region = region_import(ctx, record, &offset, length, 0);
    if (!region) return NULL;
    if (offset) {
        region_unlink(region);
        JS_ThrowTypeError(ctx, "Invalid shared image record");
        return NULL;
    }
    region->refs = 1;
    return region->data;
}
int web_atomics_memory_wait(void *address, uint64_t expected, bool wide, int64_t timeout_ns) {
    /* A negative timeout waits forever. Longer finite waits than one kernel
       request (UINT32_MAX - 1 ms) continue until their own deadline. */
    uint64_t deadline = 0;
    if (timeout_ns >= 0) {
        uint64_t ms = (uint64_t)timeout_ns / 1000000u + ((uint64_t)timeout_ns % 1000000u != 0);
        uint64_t now = uptime_ms();
        deadline = ms > UINT64_MAX - now ? UINT64_MAX : now + ms;
    }
    for (;;) {
        unsigned slice = UINT32_MAX;
        if (timeout_ns >= 0) {
            uint64_t now = uptime_ms();
            uint64_t left = deadline > now ? deadline - now : 0;
            slice = left >= UINT32_MAX ? UINT32_MAX - 1 : (unsigned)left;
        }
        int r = wide ? wait_on_address64(address, expected, slice)
                     : wait_on_address(address, (uint32_t)expected, slice);
        if (r == 0) return 0;
        if (errno == EAGAIN) return 1;
        if (errno != ETIMEDOUT) return 2; /* killed: the agent is terminating */
        if (timeout_ns >= 0 && uptime_ms() >= deadline) return 2;
    }
}
uint32_t web_atomics_memory_notify(void *address, uint32_t count) {
    /* The kernel reads a zero count as "every waiter". */
    if (!count) return 0;
    int woken = wake_address(address, count == UINT32_MAX ? 0 : count);
    return woken > 0 ? (uint32_t)woken : 0;
}

void web_atomics_host_init(JSContext *ctx, JSValueConst host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("sharedBufferExport", 1, native_export),
        JS_CFUNC_DEF("sharedBufferImport", 1, native_import),
        JS_CFUNC_DEF("sharedBufferAlias", 1, native_alias),
        JS_CFUNC_DEF("sharedBufferReserve", 1, native_reserve),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof *functions);
    /* The brand for structured clone, without allocating a shared region:
       memory outside the registry is neither referenced nor freed. */
    static uint8_t brand_sample;
    JSValue sample = JS_NewArrayBuffer(ctx, &brand_sample, 0, NULL, NULL, true);
    if (JS_IsException(sample)) sample = JS_GetException(ctx);
    else JS_SetPropertyStr(ctx, host, "sharedBufferClassID", JS_NewUint32(ctx, JS_GetClassID(sample)));
    JS_FreeValue(ctx, sample);
}

/* waitAsync waiters, in enrollment order. */
struct async_wait {
    struct async_wait *next;
    JSContext *ctx;
    JSValue resolve;
    uint64_t token, deadline; /* deadline UINT64_MAX: no timeout */
    uint64_t enrolled;
    volatile uint32_t *address;
    uint32_t expected;
    bool notified;
};
static struct async_wait *waits, **waits_tail = &waits;
static n_handle queue = -1;
static uint64_t next_token;

n_handle web_atomics_queue(void) {
    if (queue < 0) { n_handle q = n_events_create(); if (q >= 0) queue = q; }
    return queue;
}

static int wait_async(JSContext *ctx, void *opaque, void *address, uint32_t expected,
                      double timeout_ms, JSValueConst resolve) {
    (void)opaque;
    if (web_atomics_queue() < 0) {
        JS_ThrowRangeError(ctx, "Atomics.waitAsync queue could not be created");
        return -1;
    }
    struct async_wait *w = calloc(1, sizeof *w);
    if (!w) { JS_ThrowOutOfMemory(ctx); return -1; }
    w->token = WEB_ATOMICS_TOKEN | ++next_token;
    n_result watched = n_events_watch_address(queue, address, expected, w->token);
    if (watched < 0) {
        free(w);
        if (watched == -N_ERR_NOT_READY) return 1;
        JS_ThrowRangeError(ctx, "Atomics.waitAsync waiter could not be enrolled");
        return -1;
    }
    uint64_t now = uptime_ms();
    w->deadline = isfinite(timeout_ms) && timeout_ms < (double)(UINT64_MAX - now) ?
        now + (uint64_t)ceil(timeout_ms) : UINT64_MAX;
    w->ctx = ctx;
    w->enrolled = now;
    w->address = (volatile uint32_t *)address;
    w->expected = expected;
    w->resolve = JS_DupValue(ctx, resolve);
    *waits_tail = w; waits_tail = &w->next;
    return 0;
}

void web_atomics_runtime_init(JSRuntime *rt) {
    static const JSSharedArrayBufferFunctions functions = {
        .sab_alloc = sab_alloc, .sab_free = sab_free, .sab_dup = sab_dup,
    };
    JS_SetSharedArrayBufferFunctions(rt, &functions);
    JS_SetAtomicsWaitAsyncFunc(rt, wait_async, NULL);
}

void web_atomics_events(const struct n_event *events, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!(events[i].events & N_EVENT_ADDRESS)) continue;
        for (struct async_wait *w = waits; w; w = w->next)
            if (w->token == events[i].token) { w->notified = true; break; }
    }
}

/* Mark notified waiters without waiting. Memory-word events come first in a
   report and only once; other watches of a shared queue stay ready, so they
   are left to their owner's own wait. */
static void drain(void) {
    if (queue < 0) return;
    struct n_event events[64];
    for (;;) {
        n_result n = n_events_wait(queue, events, 64, 0);
        if (n <= 0) return;
        web_atomics_events(events, (size_t)n);
        if (n < 64 || !(events[n - 1].events & N_EVENT_ADDRESS)) return;
    }
}

int64_t web_atomics_deadline(JSContext *ctx, uint64_t now) {
    if (!waits) return -1;
    drain();
    uint64_t due = UINT64_MAX;
    for (struct async_wait *w = waits; w; w = w->next) {
        if (w->ctx != ctx) continue;
        if (w->notified) return (int64_t)now;
        if (w->deadline < due) due = w->deadline;
    }
    return due == UINT64_MAX ? -1 : (int64_t)(due > (uint64_t)INT64_MAX ? INT64_MAX : due);
}

/* Diagnostics: pending waiters and the oldest one's word. A current value
   that differs from the expected one with no notification means a store
   whose notify never reached this process. */
int web_atomics_report(char *out, size_t size, uint64_t now) {
    if (!waits || !size) return 0;
    drain();
    unsigned pending = 0, notified = 0;
    for (struct async_wait *w = waits; w; w = w->next) { pending++; if (w->notified) notified++; }
    const struct async_wait *oldest = waits;
    return snprintf(out, size, "Atomics.waitAsync: %u pending, %u notified; oldest %p value %u expected %u, age %lu ms",
                    pending, notified, (void *)oldest->address,
                    __atomic_load_n(oldest->address, __ATOMIC_ACQUIRE), oldest->expected,
                    (unsigned long)(now - oldest->enrolled));
}

static void unlink_wait(struct async_wait **link) {
    struct async_wait *w = *link;
    *link = w->next;
    if (waits_tail == &w->next) waits_tail = link;
}

int web_atomics_run(JSContext *ctx, uint64_t now) {
    if (!waits) return 0;
    drain();
    int settled = 0;
    for (struct async_wait **link = &waits; *link;) {
        struct async_wait *w = *link;
        if (w->ctx != ctx || (!w->notified && w->deadline > now)) { link = &w->next; continue; }
        bool ok = w->notified;
        /* A notify racing with the timeout already counted this waiter. */
        if (!ok) ok = n_events_unwatch(queue, w->token) == 2;
        unlink_wait(link);
        JSValue value = JS_NewString(ctx, ok ? "ok" : "timed-out");
        JSValue result = JS_Call(ctx, w->resolve, JS_UNDEFINED, 1, (JSValueConst *)&value);
        JS_FreeValue(ctx, result); JS_FreeValue(ctx, value); JS_FreeValue(ctx, w->resolve);
        free(w);
        settled++;
    }
    return settled;
}

void web_atomics_release(JSContext *ctx) {
    for (struct async_wait **link = &waits; *link;) {
        struct async_wait *w = *link;
        if (w->ctx != ctx) { link = &w->next; continue; }
        if (!w->notified) n_events_unwatch(queue, w->token);
        unlink_wait(link);
        JS_FreeValue(ctx, w->resolve);
        free(w);
    }
}

static JSValue isolated_getter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                               int magic, JSValue *data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_DupValue(ctx, data[0]);
}
int web_atomics_isolation(JSContext *ctx, bool isolated) {
    JSValue global = JS_GetGlobalObject(ctx);
    int result = 0;
    if (!isolated) {
        JSAtom name = JS_NewAtom(ctx, "SharedArrayBuffer");
        result = JS_DeleteProperty(ctx, global, name, 0) < 0 ? -1 : 0;
        JS_FreeAtom(ctx, name);
    }
    JSValue value = JS_NewBool(ctx, isolated);
    JSValue getter = result < 0 ? JS_EXCEPTION : JS_NewCFunctionData(ctx, isolated_getter, 0, 0, 1, &value);
    if (JS_IsException(getter)) result = -1;
    else {
        JSAtom name = JS_NewAtom(ctx, "crossOriginIsolated");
        result = JS_DefinePropertyGetSet(ctx, global, name, getter, JS_UNDEFINED,
                                         JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
        JS_FreeAtom(ctx, name);
    }
    JS_FreeValue(ctx, global);
    return result;
}
