/* Kernel entropy pool (dev/entropy.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void entropy_init(void);
void entropy_add(uint64_t v); /* any CPU, any context: into that CPU's feed */
void entropy_get(void *buf, size_t n); /* any CPU, any context: its lock takes no other */
