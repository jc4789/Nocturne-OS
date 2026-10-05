#pragma once
#include <stddef.h>
#include <stdint.h>
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
void *krealloc(void *p, size_t size);
uint64_t heap_used_bytes(void);
uint64_t heap_size_bytes(void);
