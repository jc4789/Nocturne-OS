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
enum { NJW_START=1,NJW_MESSAGE,NJW_LOADED,NJW_ERROR,NJW_LOAD,NJW_CLOSE,NJW_CONSOLE,NJW_PORT_MESSAGE,NJW_PORT_CLOSE };
#define NJW_WITH_PORTS 3u
/* Native Nocturne pipe records, no pointers or engine bytecode. */
struct njw_header { uint32_t magic,op,generation,request,kind,bytes; };
_Static_assert(sizeof(struct njw_header)==24,"worker wire header");
