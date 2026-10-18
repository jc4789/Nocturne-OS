#pragma once
#include <stdbool.h>

/* LAPIC-delivered interrupt vector allocator (idt.c). Vectors 0x00-0x4F (exceptions, PIC, and the fixed
   driver vectors: timer 0x30, VMBus 0x31, SMP 0x32-0x34, xHCI 0x40-0x47, NVMe 0x48-0x4B, RTL8125 0x4C),
   0x80 (syscall) and 0xF0-0xFF are never handed out; the pool is 0x50-0xEF minus 0x80.
   Returns the first of `count` consecutive vectors whose base is a multiple of `align` (a power of two,
   0 = 1; multi-message MSI needs align >= count), or -1 when none is free. The caller then installs the
   handler with vector_register(). vector_register() panics if a vector already has a different handler. */
int irq_vector_alloc(unsigned count, unsigned align);
void irq_vector_free(unsigned vector, unsigned count); /* drops the handlers and returns them to the pool */
