#pragma once
#include <stdint.h>
#define NJW_MAGIC 0x4e4a5731u
#define NJW_MAX_BYTES (8u*1024u*1024u)
#define NJW_MAX_QUEUE (16u*1024u*1024u)
#define NJW_MAX_WORKERS 4u
#define NJW_HEAP_BYTES (64u*1024u*1024u)
enum { NJW_START=1,NJW_MESSAGE,NJW_LOADED,NJW_ERROR,NJW_LOAD,NJW_CLOSE,NJW_CONSOLE };
/* Native Nocturne pipe records, no pointers or engine bytecode. */
struct njw_header { uint32_t magic,op,generation,request,kind,bytes; };
_Static_assert(sizeof(struct njw_header)==24,"worker wire header");
