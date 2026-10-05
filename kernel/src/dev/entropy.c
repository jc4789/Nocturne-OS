/* Kernel random numbers for /dev/random (and through it, TLS keys in userland).
   Every interrupt mixes its timestamp-counter value into a 256-bit pool; RDRAND is added when the
   CPU has it. Output is a ChaCha20 keystream whose key is re-derived from the pool on every read,
   so the generator never repeats and later interrupts keep feeding fresh jitter in. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/entropy.h"

static uint64_t pool[4] = {0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL,
                           0xA54FF53A5F1D36F1ULL};
static unsigned pool_pos;
static uint64_t counter;
static bool have_rdrand;

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

/* cheap enough for every interrupt: one multiply-rotate mix into the next pool word */
void entropy_add(uint64_t v) {
    unsigned i = pool_pos++ & 3;
    pool[i] = rotl64(pool[i] ^ (v * 0x9E3779B97F4A7C15ULL), 23) + pool[(i + 1) & 3];
}

static bool rdrand64(uint64_t *out) {
    for (int i = 0; i < 10; i++) {
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
        if (ok) return true;
    }
    return false;
}

#define QR(a, b, c, d)                                                                                       \
    a += b, d ^= a, d = (d << 16) | (d >> 16), c += d, b ^= c, b = (b << 12) | (b >> 20), a += b, d ^= a,  \
    d = (d << 8) | (d >> 24), c += d, b ^= c, b = (b << 7) | (b >> 25)

static void chacha_block(const uint32_t key[8], uint64_t ctr, uint32_t out[16]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, key[0], key[1], key[2], key[3],
                      key[4],     key[5],     key[6],     key[7],     (uint32_t)ctr, (uint32_t)(ctr >> 32), 0, 0};
    uint32_t x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
}

void entropy_get(void *buf, size_t n) {
    uint64_t f = irq_save();
    entropy_add(rdtsc());
    if (have_rdrand) {
        uint64_t r;
        for (int i = 0; i < 4; i++)
            if (rdrand64(&r)) entropy_add(r);
    }
    /* key = first block of ChaCha20 under the pool; the pool is then replaced by the second block
       (forward secrecy: earlier outputs can't be recomputed from the new pool) */
    uint32_t key[8], blk[16];
    memcpy(key, pool, sizeof key);
    chacha_block(key, counter++, blk);
    memcpy(key, blk, 32);
    memcpy(pool, blk + 8, 32);
    irq_restore(f);

    uint8_t *b = buf;
    uint64_t ctr = 0;
    while (n) {
        chacha_block(key, ctr++, blk);
        size_t k = MIN(n, sizeof blk);
        memcpy(b, blk, k);
        b += k;
        n -= k;
    }
    memset(key, 0, sizeof key);
    memset(blk, 0, sizeof blk);
}

void entropy_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    have_rdrand = c & (1u << 30);
    for (int i = 0; i < 64; i++) {
        entropy_add(rdtsc());
        for (volatile int j = 0; j < (i & 7) * 50; j++) {
        }
    }
    kprintf("random: entropy pool ready%s\n", have_rdrand ? " (rdrand)" : "");
}
