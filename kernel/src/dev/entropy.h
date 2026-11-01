/* Kernel entropy pool (dev/entropy.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void entropy_init(void); /* kmain_late's thread: the first seed and the thread that folds the next */
void entropy_add(uint64_t v); /* any CPU, any context: into that CPU's feed */
/* Any CPU, any context but an NMI and the scheduler's own locks (it may wake the seed's thread): no lock
   of its own, its CPU's state with interrupts off for a block or two. */
void entropy_get(void *buf, size_t n);
