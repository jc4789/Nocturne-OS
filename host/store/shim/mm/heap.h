/* kernel/src/mm/heap.h on the build host: the tool's own heap (host/store/store.c), which guards
   every block so that a write past one is found, and fills new ones with garbage as the kernel's
   heap may. */
#pragma once
#include <stddef.h>

void *kmalloc(size_t n);
void *kzalloc(size_t n);
void kfree(void *p);
void *krealloc(void *p, size_t n);
void *kappend(void *array_at, size_t *count, size_t *room, size_t size);
