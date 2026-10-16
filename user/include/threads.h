/* ISO C11 threads on Nocturne native threads and address waits. */
#pragma once
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(__cplusplus) && !defined(thread_local)
#define thread_local _Thread_local
#endif
#define ONCE_FLAG_INIT {0}
#define TSS_DTOR_ITERATIONS 4

enum { thrd_success = 0, thrd_nomem = 1, thrd_timedout = 2, thrd_busy = 3, thrd_error = 4 };
enum { mtx_plain = 1, mtx_recursive = 2, mtx_timed = 4 };

typedef struct __nocturne_thrd *thrd_t;
typedef int (*thrd_start_t)(void *);
/* state: 0 unlocked, 1 locked, 2 locked with waiters. */
typedef struct { volatile uint32_t state; int type; int owner; unsigned depth; } mtx_t;
typedef struct { volatile uint32_t sequence; } cnd_t;
/* 0 not run, 1 running, 2 done. */
typedef struct { volatile uint32_t state; } once_flag;
typedef unsigned tss_t;
typedef void (*tss_dtor_t)(void *);

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
thrd_t thrd_current(void);
int thrd_detach(thrd_t thr);
int thrd_equal(thrd_t a, thrd_t b);
__attribute__((noreturn)) void thrd_exit(int res);
int thrd_join(thrd_t thr, int *res);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);

int mtx_init(mtx_t *mtx, int type);
void mtx_destroy(mtx_t *mtx);
int mtx_lock(mtx_t *mtx);
int mtx_timedlock(mtx_t *mtx, const struct timespec *ts);
int mtx_trylock(mtx_t *mtx);
int mtx_unlock(mtx_t *mtx);

int cnd_init(cnd_t *cond);
void cnd_destroy(cnd_t *cond);
int cnd_signal(cnd_t *cond);
int cnd_broadcast(cnd_t *cond);
int cnd_wait(cnd_t *cond, mtx_t *mtx);
int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts);

void call_once(once_flag *flag, void (*func)(void));

int tss_create(tss_t *key, tss_dtor_t dtor);
void tss_delete(tss_t key);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *value);

#ifdef __cplusplus
}
#endif
