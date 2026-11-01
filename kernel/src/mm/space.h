/* An app's address-space layout: its reservation regions, the holes in them, and the ranges its
   threads are changing (mm/space.c). Any CPU, thread context, no spinlock held. */
#pragma once
#include "kernel.h"

struct process;

/* [va, va+len) of the mmap region (with its guard page) is free again; after the process's end
   nothing is taken back. */
void mmap_hole_add(struct process *owner, uint64_t va, uint64_t len);
/* len bytes of the mmap region followed by an unmapped guard page; 0 if full or after the end. */
uint64_t mmap_reserve(struct process *owner, uint64_t len);

/* The calling app's n_memory_reserve, n_memory_release and n_memory_protect (access: N_MEMORY_*). */
int64_t memory_reserve(size_t len, int access);
int64_t memory_release(uint64_t va, size_t len);
int64_t memory_protect(uint64_t va, size_t len, int access);
