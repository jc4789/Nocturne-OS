#pragma once
#include <stdint.h>
#define NJW_MAGIC 0x4e4a5731u
/* Payloads/identifiers use uint32_t on the unchanged pipe ABI. Queues are
   linked lazy allocations, constrained only by size_t accounting and OOM. */
#define NJW_MAX_BYTES UINT32_MAX
#define NJW_MAX_QUEUE SIZE_MAX
#define NJW_MAX_WORKERS UINT32_MAX
/* Heap ownership is local to the child, not a uint32_t pipe field. Keep the
   runtime default (no configured ceiling); real allocation failure is OOM. */
#define NJW_HEAP_BYTES SIZE_MAX
#define NJW_TASK_MS UINT32_MAX
enum { NJW_START=1,NJW_MESSAGE,NJW_LOADED,NJW_ERROR,NJW_LOAD,NJW_CLOSE,NJW_CONSOLE,NJW_PORT_MESSAGE,NJW_PORT_CLOSE,NJW_PORT_DRAINED,NJW_CANCEL,NJW_CSP_REPORT,NJW_CONNECT,NJW_CREDIT,NJW_ATTACH,
       NJW_FRAME,NJW_RESUME };
/* NJW_FRAME (parent to child) is one frame of a streamed Fetch response, a fetch the child asked
 * to stream (NJW_LOAD_STREAM): its kind is the frame's event and, for the headers, the
 * NJW_LOADED_* flags. The parent's transport sends the next frame only after the child's
 * NJW_RESUME (child to parent, payload null) for the one before; an END frame (payload: [the
 * error, "" for none; the request's timing record or null], NJW_LOADED_TIMING's) or an NJW_LOADED
 * error ends the request. */
/* NJW_ATTACH is a carrier: a bare header (bytes 0, nothing else in the
 * message) whose channel attachment is one shared-page handle of the message
 * that follows. The sender puts every carrier of a message ahead of its
 * payload record, in attachment-index order, with nothing between; the
 * receiver keeps the handles and gives all it holds to the next payload
 * record. Carriers are not counted for credit. */
/* Flow-control quantum, not a message size/site quota. One larger legitimate
 * record is allowed alone; the producer waits for native consumer credit. */
#define NJW_TRANSPORT_WINDOW 65536u
/* Additional private START array fields; the 24-byte record ABI is unchanged.
 * Only the trusted parent chooses policy authority, never author JS. */
enum { NJW_START_CREATOR_POLICY=7, NJW_START_INHERIT_POLICY=8, NJW_START_SHARED=9,
       NJW_START_CREATOR_ORIGIN=10, NJW_START_CREATOR_REFERRER_POLICY=11,
       NJW_START_CROSS_ORIGIN_ISOLATED=12, NJW_START_STREAMING=13 };
/* An NJW_LOADED's (a request's, never START's) array field: Fetch's report timing of a request
   whose client is the Worker (its importScripts and fetch), the record js.c's timing_value makes,
   its times in monotonic milliseconds (n_clock_uptime_ms) for the Worker's own time origin to
   convert (Resource Timing in the Worker's performance timeline). Absent for a request with no
   report (aborted, or not HTTP(S)). */
enum { NJW_LOADED_TIMING=7 };
#define NJW_WITH_PORTS 3u
/* LOAD policy belongs to the private transport, not URL/header author data.
   Import scripts cannot opt out of their existing origin policy. */
#define NJW_LOAD_NO_CORS 0x100u
#define NJW_LOAD_NO_REFERRER 0x200u
/* A fetch's (kind 2) request modes, as Fetch's request records them: mode "same-origin", redirect
 * mode "error", the use-CORS-preflight flag, keepalive, a streamed response (NJW_FRAME), the
 * credentials mode (omit, same-origin, include) and the cache mode (enum webnet_cache). */
#define NJW_LOAD_SAME_ORIGIN 0x400u
#define NJW_LOAD_REDIRECT_ERROR 0x800u
#define NJW_LOAD_PREFLIGHT 0x1000u
#define NJW_LOAD_KEEPALIVE 0x2000u
#define NJW_LOAD_STREAM 0x4000u
#define NJW_LOAD_CREDENTIALS_SHIFT 16u
#define NJW_LOAD_CREDENTIALS (3u << NJW_LOAD_CREDENTIALS_SHIFT)
#define NJW_LOAD_CACHE_SHIFT 18u
#define NJW_LOAD_CACHE (7u << NJW_LOAD_CACHE_SHIFT)
#define NJW_LOAD_POLICY_FLAGS (NJW_LOAD_NO_CORS | NJW_LOAD_NO_REFERRER | NJW_LOAD_SAME_ORIGIN | NJW_LOAD_REDIRECT_ERROR | NJW_LOAD_PREFLIGHT | NJW_LOAD_KEEPALIVE | NJW_LOAD_STREAM | NJW_LOAD_CREDENTIALS | NJW_LOAD_CACHE)
static inline int njw_load_kind_valid(uint32_t kind) {
    return kind == 1u || ((kind & ~NJW_LOAD_POLICY_FLAGS) == 2u &&
        ((kind & NJW_LOAD_CREDENTIALS) >> NJW_LOAD_CREDENTIALS_SHIFT) <= 2u &&
        ((kind & NJW_LOAD_CACHE) >> NJW_LOAD_CACHE_SHIFT) <= 5u);
}
#define NJW_LOADED_CORS 1u
#define NJW_LOADED_REDIRECTED 2u
#define NJW_LOADED_OPAQUE 4u
/* An opaque response's: what the transport noted of the response it hides (webfetch.c's resource=,
   shifted by NJW_LOADED_RESOURCE_SHIFT), for Cache Storage's cross-origin resource policy check. */
#define NJW_LOADED_RESOURCE_SHIFT 3u
#define NJW_LOADED_RESOURCE (7u << NJW_LOADED_RESOURCE_SHIFT)
#define NJW_LOADED_FLAGS (NJW_LOADED_CORS | NJW_LOADED_REDIRECTED | NJW_LOADED_OPAQUE | NJW_LOADED_RESOURCE)
static inline int njw_loaded_kind_valid(uint32_t kind) {
    return kind <= (NJW_LOADED_CORS | NJW_LOADED_REDIRECTED) || (kind & ~NJW_LOADED_RESOURCE) == NJW_LOADED_OPAQUE;
}
/* An NJW_FRAME's event (webnet's: headers, a chunk, the end, upload progress), shifted. */
#define NJW_FRAME_SHIFT 8u
#define NJW_FRAME_HEADERS (1u << NJW_FRAME_SHIFT)
#define NJW_FRAME_CHUNK (2u << NJW_FRAME_SHIFT)
#define NJW_FRAME_END (3u << NJW_FRAME_SHIFT)
#define NJW_FRAME_UPLOAD (4u << NJW_FRAME_SHIFT)
#define NJW_FRAME_EVENT (7u << NJW_FRAME_SHIFT)
static inline int njw_frame_kind_valid(uint32_t kind) {
    uint32_t event = kind & NJW_FRAME_EVENT, rest = kind & ~NJW_FRAME_EVENT;
    if (kind & ~(NJW_FRAME_EVENT | NJW_LOADED_FLAGS)) return 0;
    if (event == NJW_FRAME_HEADERS) return njw_loaded_kind_valid(rest);
    return (event == NJW_FRAME_CHUNK || event == NJW_FRAME_END || event == NJW_FRAME_UPLOAD) && !rest;
}
/* Native Nocturne pipe records, no pointers or engine bytecode. */
struct njw_header { uint32_t magic,op,generation,request,kind,bytes; };
_Static_assert(sizeof(struct njw_header)==24,"worker wire header");
