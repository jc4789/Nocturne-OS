/* Harness-only stand-in for user/include/nocturne.h (the real one pulls in the kernel ABI). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
static inline int wait_on_address(volatile void *a, uint32_t v, uint32_t t) { return 0; }
static inline int wake_address(volatile void *a, uint32_t n) { return 0; }
