#pragma once
/* The kernel heap: objects of any size, uninitialized (kzalloc: zeroed), at a multiple of 16 bytes
   (an object of a power-of-two size below a page: of its size). NULL without memory, and for a
   size larger than the RAM. Any CPU, any context but NMI and machine-check handlers: it never
   sleeps, and waits for another CPU only where an object of pages comes from vmalloc because no
   run of free frames holds it (mm/vmalloc.h). An object may be freed on any CPU; kfree takes
   what the heap gave and nothing else, and stops the system on anything else. */
#include <stddef.h>
#include <stdint.h>
void heap_init(void); /* the first CPU, after pmm_init and before anything allocates */
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
uint64_t heap_used_bytes(void); /* the bytes of the objects given out and not freed */
/* CPU cpu will not run again (it failed to start, or to come back from a sleep): the objects
   waiting on it go back to their slabs. */
void heap_cpu_gone(unsigned cpu);
