/* Waits on a word of an app's memory (SYS_WAIT_ADDRESS, SYS_WAKE_ADDRESS) and the event queues'
   watches of such words (sys/address_wait.c). */
#pragma once
#include "kernel.h"

struct wait_item;

/* At boot, once the CPUs are counted (arch/topology.h): the buckets the waits hash to. */
void address_wait_init(void);
/* The physical address of the word at address in the calling app's space, its page made present:
   what the waits on it are keyed by. 0 when it is no memory of the app's, or not aligned (wide: 8
   bytes, else 4). It may fault the page in: no spinlock held. */
uint64_t address_word(volatile void *address, bool wide);
/* The calling thread waits while the word at address (an app's) reads expected, until a wake, the
   deadline (the monotonic clock's ns, CLOCK_NEVER for none) or a kill. wide: compare the 8-byte
   word at address (wasm memory.atomic.wait64). */
int address_wait(volatile void *address, uint64_t expected, bool wide, uint64_t deadline);
/* Up to count waiters of the word (0: all) are woken, the oldest first, whether threads or
   watches: how many. */
int address_wake(volatile uint32_t *address, unsigned count);
/* An event queue's watch of a word (sys/events.c): item, a callback item, waits among the word's
   waiters as a blocked thread does, keyed by phys (address_word), and a wake that takes it calls
   its callback under the word's bucket's lock (sys/wait.h's contract). Armed unless the 4-byte
   word at phys no longer reads expected (false). A bucket's lock comes after the caller's
   spinlocks, an event queue's lock among them. */
bool address_watch_arm(struct wait_item *item, uint64_t phys, uint32_t expected);
/* item, armed, taken back: true while it still waited; false when a wake took it, whose callback
   has run by the time this returns. */
bool address_watch_disarm(struct wait_item *item);
