#pragma once
#include <stddef.h>
#include <stdint.h>
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
void *krealloc(void *p, size_t size);
/* An array of `count` elements of `size` bytes with room for one more: `array` itself while
   *room exceeds count, else a copy twice as large (*room updated, the old one freed). NULL
   without memory, `array` then unchanged. */
void *kgrow(void *array, size_t count, size_t *room, size_t size);
uint64_t heap_used_bytes(void);
uint64_t heap_size_bytes(void);
void heap_trim_start(void); /* the task that returns large free blocks' pages */
