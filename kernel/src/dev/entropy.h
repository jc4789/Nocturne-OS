/* Kernel entropy pool (dev/entropy.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void entropy_init(void);
void entropy_add(uint64_t v); /* safe in interrupt context */
void entropy_get(void *buf, size_t n);
