/* What the two sides of nvrm's UVM OS layer share: uvm_os.h (what UVM is compiled against) and
 * uvm_rm.h (what RM's nv_uvm_interface.c is compiled against) cannot be included together, so
 * the words both need are defined once here: UVM's errno numbers, atomics and counting
 * semaphores.
 *
 * The errno numbers are private to UVM's code and its OS layer. errno_to_nv_status and
 * nv_status_to_errno (uvm_os.c) translate them to RM's NV_STATUS, which is what crosses to a
 * client; they are never Nocturne's n_result codes and no Nocturne interface returns them.
 */
#ifndef NVRM_UVM_SHARED_H
#define NVRM_UVM_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#include "nocturne.h"

/* ---------------------------------------------------------------- errno */
enum {
    EPERM = 1, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF, ECHILD, EAGAIN, ENOMEM,
    EACCES, EFAULT, ENOTBLK, EBUSY, EEXIST, EXDEV, ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE,
    ENOTTY, ETXTBSY, EFBIG, ENOSPC, ESPIPE, EROFS, EMLINK, EPIPE, EDOM, ERANGE,
    EDEADLK, ENAMETOOLONG, ENOLCK, ENOSYS, ENOTEMPTY, ELOOP, ENOMSG, EIDRM, ENODATA, ETIME,
    EOVERFLOW, EBADFD, EOPNOTSUPP, ETIMEDOUT, ECANCELED, ENOTSUPP, EHWPOISON,
};
#define EWOULDBLOCK EAGAIN
#define MAX_ERRNO 4095

/* ---------------------------------------------------------------- one access of a variable */
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))

/* ---------------------------------------------------------------- atomics (x86-64)
   No atomic_fetch_*: those names are <stdatomic.h>'s, which RM's side includes, and UVM does
   not use them. */
typedef struct { int counter; } atomic_t;
typedef struct { int64_t counter; } atomic64_t;
typedef struct { long counter; } atomic_long_t;
#define ATOMIC_INIT(v) { (v) }
#define ATOMIC64_INIT(v) { (v) }
#define ATOMIC_LONG_INIT(v) { (v) }

#define _UVM_ATOMIC_OPS(prefix, type, ctype)                                                         \
static inline ctype prefix##_read(const type *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); } \
static inline ctype prefix##_read_acquire(const type *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); } \
static inline void prefix##_set(type *v, ctype i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); } \
static inline void prefix##_set_release(type *v, ctype i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); } \
static inline void prefix##_add(ctype i, type *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED); } \
static inline void prefix##_sub(ctype i, type *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED); } \
static inline void prefix##_inc(type *v) { __atomic_fetch_add(&v->counter, 1, __ATOMIC_RELAXED); } \
static inline void prefix##_dec(type *v) { __atomic_fetch_sub(&v->counter, 1, __ATOMIC_RELAXED); } \
static inline ctype prefix##_add_return(ctype i, type *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static inline ctype prefix##_sub_return(ctype i, type *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); } \
static inline ctype prefix##_inc_return(type *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); } \
static inline ctype prefix##_dec_return(type *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); } \
static inline bool prefix##_dec_and_test(type *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0; } \
static inline bool prefix##_inc_and_test(type *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0; } \
static inline bool prefix##_sub_and_test(ctype i, type *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST) == 0; } \
static inline ctype prefix##_xchg(type *v, ctype i) { return __atomic_exchange_n(&v->counter, i, __ATOMIC_SEQ_CST); } \
static inline ctype prefix##_cmpxchg(type *v, ctype old, ctype new_value)                            \
{                                                                                                    \
    __atomic_compare_exchange_n(&v->counter, &old, new_value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    return old;                                                                                      \
}                                                                                                    \
static inline bool prefix##_add_unless(type *v, ctype a, ctype u)                                    \
{                                                                                                    \
    ctype c = prefix##_read(v);                                                                      \
    do {                                                                                             \
        if (c == u) return false;                                                                    \
    } while (!__atomic_compare_exchange_n(&v->counter, &c, c + a, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)); \
    return true;                                                                                     \
}                                                                                                    \
static inline ctype prefix##_dec_if_positive(type *v)                                                \
{                                                                                                    \
    ctype c = prefix##_read(v);                                                                      \
    do {                                                                                             \
        if (c - 1 < 0) return c - 1;                                                                 \
    } while (!__atomic_compare_exchange_n(&v->counter, &c, c - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)); \
    return c - 1;                                                                                    \
}
_UVM_ATOMIC_OPS(atomic, atomic_t, int)
_UVM_ATOMIC_OPS(atomic64, atomic64_t, int64_t)
_UVM_ATOMIC_OPS(atomic_long, atomic_long_t, long)
#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)
#define atomic64_inc_not_zero(v) atomic64_add_unless((v), 1, 0)

/* ---------------------------------------------------------------- counting semaphores
   The count itself is the word waiters sleep on (wait_on_address): a down that finds it zero
   sleeps until an up changes it. A zeroed semaphore is a valid one with nothing to take. */
struct semaphore {
    volatile uint32_t count;
};
static inline void sema_init(struct semaphore *sem, int count)
{
    __atomic_store_n(&sem->count, (uint32_t)count, __ATOMIC_RELEASE);
}
/* 0 when one was taken (as Linux has it), 1 when none was there. */
static inline int down_trylock(struct semaphore *sem)
{
    uint32_t c = __atomic_load_n(&sem->count, __ATOMIC_RELAXED);
    while (c) {
        if (__atomic_compare_exchange_n(&sem->count, &c, c - 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
    return 1;
}
static inline void down(struct semaphore *sem)
{
    while (down_trylock(sem))
        wait_on_address(&sem->count, 0, UINT32_MAX);
}
/* Nothing interrupts a wait in nvrm. */
static inline int down_interruptible(struct semaphore *sem)
{
    down(sem);
    return 0;
}
static inline void up(struct semaphore *sem)
{
    __atomic_add_fetch(&sem->count, 1, __ATOMIC_RELEASE);
    wake_address(&sem->count, 1);
}

#endif
