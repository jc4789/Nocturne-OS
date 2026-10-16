#pragma once
#include "kernel.h"
struct file;
struct vnode;
struct event_watch;

struct vnode *events_new(void);
bool events_is(struct file *f);
/* Caller holds the BSP IRQ guard over looking f up and this call; spare is a
   kmalloc'd watch the call takes when it needs one. */
int events_watch_locked(struct file *queue, struct file *f, uint32_t interest, uint64_t token,
                        struct event_watch **spare);
int events_watch_address(struct file *queue, volatile uint32_t *address, uint32_t expected, uint64_t token);
int events_timer(struct file *queue, uint64_t ms, bool repeat, uint64_t token);
int events_unwatch(struct file *queue, uint64_t token);
int64_t events_wait(struct file *queue, struct n_event *uout, size_t max, int64_t timeout_ms);
size_t events_watch_size(void);
