#pragma once
#include "kernel.h"
#include "sys/syscall.h"
struct handle;
struct event_watch;

int events_create(struct handle **out);
bool events_is(struct handle *h);
/* Caller holds the BSP IRQ guard over looking h up and this call; spare is a
   kmalloc'd watch the call takes when it needs one. */
int events_watch_locked(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                        struct event_watch **spare);
int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
int events_timer(struct handle *queue, uint64_t ms, bool repeat, uint64_t token);
int events_unwatch(struct handle *queue, uint64_t token);
/* A wait done again after it parked on an AP keeps its first deadline (the task's wait_until),
   on either path. The BSP's entry consumes it first, whatever ends the call (events_wait_resume:
   the timeout left), and events_wait takes that timeout. */
int64_t events_wait_resume(int64_t timeout_ms);
int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms);
/* The same in an AP read section (syscall_ap), answered from the watched handles alone: done
   when one is ready or the wait may not block; parked on the queue's generation word when none
   is, until a change or the earliest of its deadline and the next timer's. A due timer, a
   memory-word event or a buffer not mapped is the BSP's. It only reads the queue: a change racing
   with it is reported by the next wait, as level-triggered reports allow. */
enum ap_syscall events_wait_ap(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms,
                               int64_t *result);
size_t events_watch_size(void);
/* The handle's last close, BSP IRQ guard held: its watches end. The caller frees them with
   events_watches_free after ap_read_synchronize, when no AP wait can still be reading them. */
struct event_watch *events_handle_closed(struct handle *h);
void events_watches_free(struct event_watch *ended);
