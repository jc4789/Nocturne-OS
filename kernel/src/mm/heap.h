#pragma once
#include <stddef.h>
#include <stdint.h>
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
void *krealloc(void *p, size_t size);
/* One more element of `size` bytes at the end of an array: array_at is the address of the
   caller's pointer to it, *count its elements and *room the ones it has room for. Returns the
   new element (its bytes as they were), *count counted up; when there was no room the array
   moved first to a copy twice as large (*room updated, the old one freed). NULL without
   memory, nothing changed. */
void *kappend(void *array_at, size_t *count, size_t *room, size_t size);
uint64_t heap_used_bytes(void);
uint64_t heap_size_bytes(void);
void heap_trim_start(void); /* the task that returns large free blocks' pages */
