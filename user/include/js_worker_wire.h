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
enum { NJW_START=1,NJW_MESSAGE,NJW_LOADED,NJW_ERROR,NJW_LOAD,NJW_CLOSE,NJW_CONSOLE,NJW_PORT_MESSAGE,NJW_PORT_CLOSE,NJW_PORT_DRAINED,NJW_CANCEL };
#define NJW_WITH_PORTS 3u
/* LOAD policy belongs to the private transport, not URL/header author data.
   Import scripts cannot opt out of their existing origin policy. */
#define NJW_LOAD_NO_CORS 0x100u
#define NJW_LOAD_NO_REFERRER 0x200u
#define NJW_LOAD_POLICY_FLAGS (NJW_LOAD_NO_CORS | NJW_LOAD_NO_REFERRER)
static inline int njw_load_kind_valid(uint32_t kind) {
    return kind == 1u || (kind & ~NJW_LOAD_POLICY_FLAGS) == 2u;
}
#define NJW_LOADED_CORS 1u
#define NJW_LOADED_REDIRECTED 2u
#define NJW_LOADED_OPAQUE 4u
#define NJW_LOADED_FLAGS (NJW_LOADED_CORS | NJW_LOADED_REDIRECTED | NJW_LOADED_OPAQUE)
static inline int njw_loaded_kind_valid(uint32_t kind) {
    return kind <= (NJW_LOADED_CORS | NJW_LOADED_REDIRECTED) || kind == NJW_LOADED_OPAQUE;
}
/* Native Nocturne pipe records, no pointers or engine bytecode. */
struct njw_header { uint32_t magic,op,generation,request,kind,bytes; };
_Static_assert(sizeof(struct njw_header)==24,"worker wire header");
