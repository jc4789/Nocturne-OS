/* ISO C11 <threads.h> on Nocturne native threads (thread_create/join) and
 * address waits (wait_on_address/wake_address). Not a POSIX layer. */
#include <threads.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

void **__libc_tss_values(size_t need, size_t *capacity);

struct __nocturne_thrd {
    thrd_start_t func;
    void *arg;
    int tid, result;
    /* 2 references while running and joinable; join or detach drops one. */
    volatile uint32_t refs;
    bool started_here;
};

static void thrd_release(struct __nocturne_thrd *t) {
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0) free(t);
}

/* Thread identity: the record a thrd_create'd thread runs with, or one made
   on first use for the main thread and for native threads. */
static struct __nocturne_thrd *thrd_self_record(void) {
    struct __nocturne_thrd *t = thread_local_get(7);
    if (t) return t;
    t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->tid = thread_id();
    t->refs = 1;
    thread_local_set(7, t);
    return t;
}

/* ---- tss ---- */
struct tss_key { bool used; tss_dtor_t dtor; };
static struct tss_key *tss_keys;
static size_t tss_key_count;
static mtx_t tss_lock = { 0, mtx_plain, 0, 0 };

static void tss_run_dtors(void) {
    for (int round = 0; round < TSS_DTOR_ITERATIONS; round++) {
        size_t cap = 0;
        void **values = __libc_tss_values(0, &cap);
        bool ran = false;
        for (size_t i = 0; values && i < cap; i++) {
            void *value = values[i];
            if (!value) continue;
            mtx_lock(&tss_lock);
            tss_dtor_t dtor = i < tss_key_count && tss_keys[i].used ? tss_keys[i].dtor : NULL;
            mtx_unlock(&tss_lock);
            values[i] = NULL;
            if (dtor) { dtor(value); ran = true; }
        }
        if (!ran) break;
    }
}

int tss_create(tss_t *key, tss_dtor_t dtor) {
    mtx_lock(&tss_lock);
    size_t i = 0;
    while (i < tss_key_count && tss_keys[i].used) i++;
    if (i == tss_key_count) {
        struct tss_key *grown = realloc(tss_keys, (tss_key_count + 1) * sizeof *grown);
        if (!grown) { mtx_unlock(&tss_lock); return thrd_nomem; }
        tss_keys = grown;
        tss_key_count++;
    }
    tss_keys[i].used = true;
    tss_keys[i].dtor = dtor;
    mtx_unlock(&tss_lock);
    *key = (tss_t)i;
    return thrd_success;
}
void tss_delete(tss_t key) {
    mtx_lock(&tss_lock);
    if (key < tss_key_count) tss_keys[key].used = false;
    mtx_unlock(&tss_lock);
    /* Values of other threads are the caller's to release (C11 7.26.6.2). */
    size_t cap = 0;
    void **values = __libc_tss_values(0, &cap);
    if (values && key < cap) values[key] = NULL;
}
void *tss_get(tss_t key) {
    size_t cap = 0;
    void **values = __libc_tss_values(0, &cap);
    return values && key < cap ? values[key] : NULL;
}
int tss_set(tss_t key, void *value) {
    void **values = __libc_tss_values((size_t)key + 1, NULL);
    if (!values) return thrd_error;
    values[key] = value;
    return thrd_success;
}

/* ---- threads ---- */
static void thrd_entry(void *opaque) {
    struct __nocturne_thrd *t = opaque;
    t->tid = thread_id();
    thread_local_set(7, t);
    t->result = t->func(t->arg);
    tss_run_dtors();
    thrd_release(t);
}
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    struct __nocturne_thrd *t = calloc(1, sizeof *t);
    if (!t) return thrd_nomem;
    t->func = func;
    t->arg = arg;
    t->refs = 2;
    t->started_here = true;
    int tid = thread_create(thrd_entry, t);
    if (tid < 0) { free(t); return errno == ENOMEM ? thrd_nomem : thrd_error; }
    /* The child also records its id; both writes store the same value. */
    t->tid = tid;
    *thr = t;
    return thrd_success;
}
thrd_t thrd_current(void) { return thrd_self_record(); }
int thrd_detach(thrd_t thr) {
    if (!thr || !thr->started_here) return thrd_error;
    thrd_release(thr);
    return thrd_success;
}
int thrd_equal(thrd_t a, thrd_t b) { return a == b; }
__attribute__((noreturn)) void thrd_exit(int res) {
    struct __nocturne_thrd *t = thread_local_get(7);
    tss_run_dtors();
    if (t && t->started_here) {
        t->result = res;
        thrd_release(t);
        thread_exit();
    }
    /* The main thread ends the program, as returning from main would. */
    exit(res);
}
int thrd_join(thrd_t thr, int *res) {
    if (!thr || !thr->started_here) return thrd_error;
    if (thread_join(thr->tid) < 0) return thrd_error;
    if (res) *res = thr->result;
    thrd_release(thr);
    return thrd_success;
}
int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    if (!duration || duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000L) return -2;
    uint64_t ms = (uint64_t)duration->tv_sec * 1000u + (uint64_t)(duration->tv_nsec + 999999L) / 1000000u;
    while (ms) {
        unsigned step = ms > 0x7fffffffu ? 0x7fffffffu : (unsigned)ms;
        msleep(step);
        ms -= step;
    }
    if (remaining) { remaining->tv_sec = 0; remaining->tv_nsec = 0; }
    return 0;
}
void thrd_yield(void) { yield(); }

/* ---- time limits ---- */
/* Milliseconds from now until the TIME_UTC deadline, for wait_on_address:
   0 when it has passed; UINT32_MAX - 1 caps a single wait (callers loop). */
static unsigned deadline_ms(const struct timespec *ts) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    int64_t ms = (int64_t)(ts->tv_sec - now.tv_sec) * 1000 + (ts->tv_nsec - now.tv_nsec) / 1000000;
    if (ms <= 0) return 0;
    return ms >= (int64_t)UINT32_MAX - 1 ? UINT32_MAX - 1 : (unsigned)ms;
}

/* ---- mutexes ---- */
int mtx_init(mtx_t *mtx, int type) {
    if (!mtx) return thrd_error;
    mtx->state = 0;
    mtx->type = type;
    mtx->owner = 0;
    mtx->depth = 0;
    return thrd_success;
}
void mtx_destroy(mtx_t *mtx) { (void)mtx; }
static bool mtx_reenter(mtx_t *mtx) {
    if (!(mtx->type & mtx_recursive) || __atomic_load_n(&mtx->owner, __ATOMIC_RELAXED) != thread_id()) return false;
    mtx->depth++;
    return true;
}
static void mtx_acquired(mtx_t *mtx) {
    if (mtx->type & mtx_recursive) { mtx->owner = thread_id(); mtx->depth = 1; }
}
/* timeout: NULL waits forever. */
static int mtx_acquire(mtx_t *mtx, const struct timespec *timeout) {
    if (mtx_reenter(mtx)) return thrd_success;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&mtx->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        mtx_acquired(mtx);
        return thrd_success;
    }
    for (;;) {
        if (__atomic_exchange_n(&mtx->state, 2, __ATOMIC_ACQUIRE) == 0) {
            mtx_acquired(mtx);
            return thrd_success;
        }
        unsigned wait = timeout ? deadline_ms(timeout) : UINT32_MAX;
        if (timeout && !wait) return thrd_timedout;
        if (wait_on_address(&mtx->state, 2, wait) < 0 && errno != EAGAIN && errno != ETIMEDOUT && errno != EINTR)
            return thrd_error;
    }
}
int mtx_lock(mtx_t *mtx) { return mtx_acquire(mtx, NULL); }
int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
    if (!ts) return thrd_error;
    return mtx_acquire(mtx, ts);
}
int mtx_trylock(mtx_t *mtx) {
    if (mtx_reenter(mtx)) return thrd_success;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&mtx->state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return thrd_busy;
    mtx_acquired(mtx);
    return thrd_success;
}
int mtx_unlock(mtx_t *mtx) {
    if (mtx->type & mtx_recursive) {
        if (mtx->owner != thread_id() || !mtx->depth) return thrd_error;
        if (--mtx->depth) return thrd_success;
        mtx->owner = 0;
    }
    if (__atomic_exchange_n(&mtx->state, 0, __ATOMIC_RELEASE) == 2) wake_address(&mtx->state, 1);
    return thrd_success;
}

/* ---- condition variables ---- */
int cnd_init(cnd_t *cond) { cond->sequence = 0; return thrd_success; }
void cnd_destroy(cnd_t *cond) { (void)cond; }
int cnd_signal(cnd_t *cond) {
    __atomic_add_fetch(&cond->sequence, 1, __ATOMIC_RELEASE);
    wake_address(&cond->sequence, 1);
    return thrd_success;
}
int cnd_broadcast(cnd_t *cond) {
    __atomic_add_fetch(&cond->sequence, 1, __ATOMIC_RELEASE);
    wake_address(&cond->sequence, 0);
    return thrd_success;
}
static int cnd_wait_until(cnd_t *cond, mtx_t *mtx, const struct timespec *ts) {
    uint32_t sequence = __atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE);
    /* A recursive mutex is released and restored at its full depth. */
    unsigned depth = mtx->type & mtx_recursive ? mtx->depth : 0;
    if (depth) { mtx->depth = 1; }
    if (mtx_unlock(mtx) != thrd_success) return thrd_error;
    int result = thrd_success;
    unsigned wait = ts ? deadline_ms(ts) : UINT32_MAX;
    if (ts && !wait) result = thrd_timedout;
    else if (wait_on_address(&cond->sequence, sequence, wait) < 0 && errno == ETIMEDOUT && ts && !deadline_ms(ts))
        result = thrd_timedout;
    if (mtx_lock(mtx) != thrd_success) return thrd_error;
    if (depth) mtx->depth = depth;
    return result;
}
int cnd_wait(cnd_t *cond, mtx_t *mtx) { return cnd_wait_until(cond, mtx, NULL); }
int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts) {
    if (!ts) return thrd_error;
    return cnd_wait_until(cond, mtx, ts);
}

/* ---- once ---- */
void call_once(once_flag *flag, void (*func)(void)) {
    if (__atomic_load_n(&flag->state, __ATOMIC_ACQUIRE) == 2) return;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&flag->state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        func();
        __atomic_store_n(&flag->state, 2, __ATOMIC_RELEASE);
        wake_address(&flag->state, 0);
        return;
    }
    while (__atomic_load_n(&flag->state, __ATOMIC_ACQUIRE) != 2)
        wait_on_address(&flag->state, 1, UINT32_MAX);
}
