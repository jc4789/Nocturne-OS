/* CRC-32C, eight bytes per step through eight tables built on first use (store/crc32c.h). Threads
   that come first build them alike, and each sees them whole once it sees ready. */
#include "store/crc32c.h"

static uint32_t table[8][256];
static int ready;

static void build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}

uint32_t crc32c(uint32_t crc, const void *data, size_t n) {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) build();
    const uint8_t *p = data;
    crc = ~crc;
    for (; n && ((uintptr_t)p & 7); n--) crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 0xFF];
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t w = *(const uint64_t *)p ^ crc;
        crc = table[7][w & 0xFF] ^ table[6][(w >> 8) & 0xFF] ^ table[5][(w >> 16) & 0xFF] ^
              table[4][(w >> 24) & 0xFF] ^ table[3][(w >> 32) & 0xFF] ^ table[2][(w >> 40) & 0xFF] ^
              table[1][(w >> 48) & 0xFF] ^ table[0][w >> 56];
    }
    for (; n; n--) crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}
