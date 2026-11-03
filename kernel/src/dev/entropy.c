/* Kernel random numbers for SYS_RANDOM (and through it, TLS keys in userland).

   ---- the feeds ----
   Every interrupt mixes its timestamp-counter value into its CPU's 256-bit feed (entropy_add).

   ---- the seed ----
   One 256-bit seed for the machine, folded from every CPU's feed, RDRAND (when the CPU has it) and
   the seed before it, by the "entropy" thread with interrupts on and no lock held: the work grows
   with the CPUs, so it is never on a reader's way. The thread folds a new seed when a reader finds
   the seed older than SEED_AGE_NS (it wakes the thread and goes on with the seed it has). The seed
   is published under a sequence count, its writer's interrupts off for the 32-byte store, so that
   a reader on any CPU, in any context, copies a whole one.

   ---- each CPU's generator ----
   Each CPU has a ChaCha20 key of its own. A read takes the CPU's key forward by one block (its
   first half the CPU's next key, its second the read's key: forward secrecy, an earlier output
   cannot be recomputed from the state that follows), then the read's bytes are the keystream of
   the read's key. The CPU's key is made again from the seed, its own feed, RDRAND and the time
   once the seed changed or after RESEED_BYTES of output. A read touches its own CPU's lines only,
   its interrupts off for one or two blocks. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/topology.h"
#include "dev/entropy.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define SEED_AGE_NS (1000ull * NS_PER_MS) /* a reader asks for a new seed past this age */
#define RESEED_BYTES (1u << 20)             /* a CPU's key is made again after this much output */

/* Each CPU's interrupts mix into its own feed, with that CPU's interrupts off: no other CPU writes
   it, and the folds read it with plain word loads (a word half a mix behind is jitter all the
   same). */
PERCPU static struct feed {
    uint64_t word[4];
    unsigned pos;
} __attribute__((aligned(64))) feed;

/* Each CPU's generator, touched by that CPU with its interrupts off. */
PERCPU static struct generator {
    uint32_t key[8];
    uint64_t counter;
    uint64_t seed_gen; /* the seed it was made from */
    uint64_t made;     /* output since it was made */
} __attribute__((aligned(64))) generator;

/* The seed: gen odd while the thread writes it. ns: when it was folded. */
static struct {
    uint64_t gen;
    uint64_t key[4];
    uint64_t ns;
} seed __attribute__((aligned(64)));
static uint64_t seed_wanted; /* a reader asked for a new seed; the thread clears it */
static struct wait_queue seed_wq;

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

static inline uint64_t mix(uint64_t w, uint64_t next, uint64_t v) {
    return rotl64(w ^ (v * 0x9E3779B97F4A7C15ULL), 23) + next;
}

/* cheap enough for every interrupt: one multiply-rotate mix into the next word of this CPU's feed */
void entropy_add(uint64_t v) {
    uint64_t f = irq_save();
    struct feed *s = this_cpu_ptr(feed);
    unsigned i = s->pos++ & 3;
    __atomic_store_n(&s->word[i], mix(s->word[i], s->word[(i + 1) & 3], v), __ATOMIC_RELAXED);
    irq_restore(f);
}

static bool rdrand64(uint64_t *out) {
    if (!cpu_features.rdrand) return false;
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

/* 256 bits mixed with v, four words at a time. */
static void fold(uint64_t w[4], unsigned *pos, uint64_t v) {
    unsigned i = (*pos)++ & 3;
    w[i] = mix(w[i], w[(i + 1) & 3], v);
}

/* A copy of the seed, whole, and its gen. */
static uint64_t seed_read(uint64_t out[4], uint64_t *ns) {
    for (;;) {
        uint64_t gen = __atomic_load_n(&seed.gen, __ATOMIC_ACQUIRE);
        if (gen & 1) {
            pause();
            continue;
        }
        for (unsigned i = 0; i < 4; i++) out[i] = __atomic_load_n(&seed.key[i], __ATOMIC_RELAXED);
        *ns = __atomic_load_n(&seed.ns, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&seed.gen, __ATOMIC_RELAXED) == gen) return gen;
    }
}

/* The thread's: a new seed from the old one, every CPU's feed, RDRAND and the time, its first
   block ChaCha20 under the fold (the fold itself is not the seed). */
static void seed_fold(void) {
    uint64_t w[4], ns;
    unsigned pos = 0;
    seed_read(w, &ns);
    fold(w, &pos, rdtsc());
    for (unsigned c = 0; c < topology.cpus; c++)
        for (unsigned i = 0; i < 4; i++) fold(w, &pos, __atomic_load_n(&per_cpu(feed, c).word[i], __ATOMIC_RELAXED));
    for (unsigned i = 0; i < 4; i++) {
        uint64_t hw;
        if (rdrand64(&hw)) fold(w, &pos, hw);
    }
    uint32_t blk[16];
    chacha_block((const uint32_t *)w, 0, blk);
    uint64_t f = irq_save(); /* a reader on this CPU never waits for a store it interrupted */
    __atomic_store_n(&seed.gen, seed.gen + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; i++)
        __atomic_store_n(&seed.key[i], (uint64_t)blk[2 * i] | (uint64_t)blk[2 * i + 1] << 32, __ATOMIC_RELAXED);
    __atomic_store_n(&seed.ns, clock_now_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&seed.gen, seed.gen + 1, __ATOMIC_RELEASE);
    irq_restore(f);
    memset(w, 0, sizeof w);
    memset(blk, 0, sizeof blk);
}

static void seed_thread(void *arg) {
    (void)arg;
    for (;;) {
        wq_wait_unless(&seed_wq, &seed_wanted, 0, CLOCK_NEVER);
        __atomic_store_n(&seed_wanted, 0, __ATOMIC_SEQ_CST);
        seed_fold();
    }
}

/* Interrupts off: this CPU's key made again from the seed (gen, key), its own feed, RDRAND and
   the time. */
static void generator_make(struct generator *g, uint64_t gen, const uint64_t key[4]) {
    uint64_t w[4];
    unsigned pos = 0;
    memcpy(w, g->key, sizeof w);
    for (unsigned i = 0; i < 4; i++) fold(w, &pos, key[i]);
    struct feed *own = this_cpu_ptr(feed);
    for (unsigned i = 0; i < 4; i++) fold(w, &pos, own->word[i]);
    uint64_t hw;
    if (rdrand64(&hw)) fold(w, &pos, hw);
    fold(w, &pos, rdtsc());
    fold(w, &pos, cpu_current_index());
    uint32_t blk[16];
    chacha_block((const uint32_t *)w, g->counter++, blk);
    memcpy(g->key, blk, sizeof g->key);
    g->seed_gen = gen;
    g->made = 0;
    memset(w, 0, sizeof w);
    memset(blk, 0, sizeof blk);
}

void entropy_get(void *buf, size_t n) {
    uint64_t key[4], ns;
    uint64_t gen = seed_read(key, &ns);
    if (clock_now_ns() - ns > SEED_AGE_NS && !__atomic_exchange_n(&seed_wanted, 1, __ATOMIC_SEQ_CST))
        wq_wake_all(&seed_wq);
    uint32_t read_key[8], blk[16];
    uint64_t f = irq_save();
    struct generator *g = this_cpu_ptr(generator);
    if (g->seed_gen != gen || g->made >= RESEED_BYTES) generator_make(g, gen, key);
    chacha_block(g->key, g->counter++, blk);
    memcpy(g->key, blk, sizeof g->key);
    memcpy(read_key, blk + 8, sizeof read_key);
    g->made += n;
    irq_restore(f);
    memset(key, 0, sizeof key);

    uint8_t *b = buf;
    uint64_t ctr = 0;
    while (n) {
        chacha_block(read_key, ctr++, blk);
        size_t k = MIN(n, sizeof blk);
        memcpy(b, blk, k);
        b += k;
        n -= k;
    }
    memset(read_key, 0, sizeof read_key);
    memset(blk, 0, sizeof blk);
}

void entropy_init(void) {
    for (int i = 0; i < 64; i++) {
        entropy_add(rdtsc()); /* into this CPU's feed, which the first seed folds in */
        for (volatile int j = 0; j < (i & 7) * 50; j++) {
        }
    }
    seed_fold();
    if (!kthread_create("entropy", seed_thread, NULL)) panic("random: no memory for the seed's thread");
    kprintf("random: entropy pool ready\n");
}
