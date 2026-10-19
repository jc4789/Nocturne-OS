/* Nocturne's wasm2c embedding ABI. The interpreter Store owns every resource;
 * generated code owns neither JS values nor a second copy of Wasm state. */
#ifndef NOCTURNE_WASM_RT_HOST_H
#define NOCTURNE_WASM_RT_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "wasm-rt.h"

typedef uint64_t wasm_host_ref;
typedef union wasm_host_value {
  uint32_t i32;
  uint64_t i64;
  float f32;
  double f64;
  unsigned char v128[16];
  wasm_host_ref ref;
} wasm_host_value;

/* Addresses refer to live C stack slots, initialized before registration.
 * A trap boundary restores its saved head before the C stack is reused. */
typedef struct wasm_host_roots {
  struct wasm_host_roots *previous;
  wasm_host_ref **slots;
  size_t count;
} wasm_host_roots;

enum wasm_host_table_op {
  WASM_HOST_TABLE_GET, WASM_HOST_TABLE_SET, WASM_HOST_TABLE_GROW,
  WASM_HOST_TABLE_SIZE, WASM_HOST_TABLE_FILL, WASM_HOST_TABLE_COPY,
  WASM_HOST_TABLE_INIT, WASM_HOST_ELEM_DROP
};

typedef struct wasm_host_instance {
  void *opaque;
  wasm_host_roots **root_head;
  wasm_host_ref owner_ref;
  /* One view per memory, read in place by every access. The host brings them
   * up to date before generated code runs again after anything that may have
   * grown a memory: entry, and the return of memory_grow, call and indirect. */
  wasm_rt_memory_t *memories;
  uint64_t (*memory_grow)(void *, uint32_t, uint64_t);
  int (*memory_init)(void *, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t);
  void (*data_drop)(void *, uint32_t);
  int (*global)(void *, uint32_t, int, wasm_host_value *);
  int (*table)(void *, enum wasm_host_table_op, uint32_t, uint32_t,
               uint64_t, uint64_t, uint64_t, wasm_host_value *);
  int (*call)(void *, uint32_t, const wasm_host_value *, wasm_host_value *);
  int (*indirect)(void *, uint32_t, uint32_t, uint64_t,
                  const wasm_host_value *, wasm_host_value *);
  wasm_host_ref (*func_ref)(void *, uint32_t);
} wasm_host_instance;

/* The generated function is typed C internally; only this entry has a uniform
 * calling convention. Its caller must use wasm_rt_run, never longjmp over C++. */
typedef void (*wasm_host_invoke)(wasm_host_instance *, uint32_t,
                                 const wasm_host_value *, wasm_host_value *);
#endif
