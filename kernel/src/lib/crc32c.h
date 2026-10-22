#pragma once
/* CRC-32C (Castagnoli, reflected polynomial 0x82F63B78): the check every NOSfs block carries
   (fs/nos.c). crc32c(0, p, n) is the CRC of p; feeding a running value continues it. */
#include <stddef.h>
#include <stdint.h>

uint32_t crc32c(uint32_t crc, const void *data, size_t n);
