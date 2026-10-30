#pragma once
#include "kernel.h"
struct handle;
struct event_watch;

int events_create(struct handle **out);
bool events_is(struct handle *h);
/* h: a handle the caller holds a reference to. spare is a kmalloc'd watch the call takes when it
   needs one. */
int events_watch(struct handle *queue, struct handle *h, uint32_t interest, uint64_t token,
                 struct event_watch **spare);
int events_watch_address(struct handle *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
int events_timer(struct handle *queue, uint64_t ms, bool repeat, uint64_t token);
int events_unwatch(struct handle *queue, uint64_t token);
int64_t events_wait(struct handle *queue, struct n_event *uout, size_t max, int64_t timeout_ms);
size_t events_watch_size(void);
/* The handle's last close: its watches end (each off its queue under the queue's lock). The
   caller frees them with events_watches_free. */
struct event_watch *events_handle_closed(struct handle *h);
void events_watches_free(struct event_watch *ended);
