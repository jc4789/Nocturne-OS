#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A task's extended state: every user component Nocturne enables (x87, SSE, AVX and AVX-512's
   three), saved with XSAVES in the compacted form. Its size is fixed at fpu_init, from the CPU:
   an area lives inside storage the owner allocates (fpu_storage_bytes, any alignment) and is
   reached through fpu_state_in. An area is never copied as bytes: XRSTORS checks its header.
   The kernel itself runs with general registers only. */
struct fpu_state;

void fpu_init(void); /* the first CPU, before its cpu_setup: the components and the area's layout */
/* Each CPU, from cpu_setup (arch/cpu.h), CR4.OSXSAVE set: XCR0 and IA32_XSS, and its registers in
   the initial state. False when it cannot hold the components the first CPU fixed. */
bool fpu_cpu_setup(void);
size_t fpu_storage_bytes(void);
struct fpu_state *fpu_state_in(void *storage);
void fpu_state_init(struct fpu_state *state); /* every component in its initial state */
void fpu_save(struct fpu_state *state);
void fpu_restore(const struct fpu_state *state);
void fpu_reset(void); /* this CPU's registers to the initial state */
/* The legacy region's bytes (x87 and SSE, the first 512) and a component's in the area:
   NULL when the component is in its initial state there. */
const uint8_t *fpu_state_component(const struct fpu_state *state, unsigned component);
