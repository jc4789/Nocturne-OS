/* Host codec test substitutes only the native scheduler declarations. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MIN(a,b) ((a) < (b) ? (a) : (b))
typedef int64_t n_result;
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms);
n_result wake_address(volatile uint32_t *address, unsigned count);
