#pragma once
#include "kernel.h"
struct handle;
struct event_source;

int events_create(struct handle **out);
bool events_is(struct handle *h);
/* h: a handle the caller holds a reference to, which keeps it from its last close until the watch
   is on its list. spare is a kmalloc'd source of events_watch_size bytes the call takes when it
   needs one. */
int events_watch(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                 struct event_source **spare);
int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
/* A timer on the queue: due at that moment (clock_now_ns; CLOCK_NEVER: never), then every period
   ns after it on the same grid (0: once). */
int events_timer(struct handle *queue, uint64_t due, uint64_t period, uint64_t token);
int events_unwatch(struct handle *queue, uint64_t token);
int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, uint64_t deadline);
size_t events_watch_size(void);
/* The handle's last close (sys/object.c), no spinlock held: its watches end, each taken off its
   queue and freed. */
void events_handle_closed(struct handle *h);
