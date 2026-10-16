/*
 * Copyright 2020 Lag Free Games, LLC
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include "rwlock.h"

#if defined(NOCTURNE)

/* Writer-preferring: new readers wait while a writer holds or awaits the
 * lock, so a stream of readers cannot starve a writer. */
int u_rwlock_init(struct u_rwlock *rwlock)
{
   rwlock->readers = 0;
   rwlock->writers_waiting = 0;
   rwlock->writer = 0;
   if (mtx_init(&rwlock->lock, mtx_plain) != thrd_success)
      return -1;
   if (cnd_init(&rwlock->cond) != thrd_success) {
      mtx_destroy(&rwlock->lock);
      return -1;
   }
   return 0;
}

int u_rwlock_destroy(struct u_rwlock *rwlock)
{
   cnd_destroy(&rwlock->cond);
   mtx_destroy(&rwlock->lock);
   return 0;
}

int u_rwlock_rdlock(struct u_rwlock *rwlock)
{
   mtx_lock(&rwlock->lock);
   while (rwlock->writer || rwlock->writers_waiting)
      cnd_wait(&rwlock->cond, &rwlock->lock);
   rwlock->readers++;
   mtx_unlock(&rwlock->lock);
   return 0;
}

int u_rwlock_rdunlock(struct u_rwlock *rwlock)
{
   mtx_lock(&rwlock->lock);
   assert(rwlock->readers > 0);
   if (--rwlock->readers == 0)
      cnd_broadcast(&rwlock->cond);
   mtx_unlock(&rwlock->lock);
   return 0;
}

int u_rwlock_wrlock(struct u_rwlock *rwlock)
{
   mtx_lock(&rwlock->lock);
   rwlock->writers_waiting++;
   while (rwlock->writer || rwlock->readers)
      cnd_wait(&rwlock->cond, &rwlock->lock);
   rwlock->writers_waiting--;
   rwlock->writer = 1;
   mtx_unlock(&rwlock->lock);
   return 0;
}

int u_rwlock_wrunlock(struct u_rwlock *rwlock)
{
   mtx_lock(&rwlock->lock);
   assert(rwlock->writer);
   rwlock->writer = 0;
   cnd_broadcast(&rwlock->cond);
   mtx_unlock(&rwlock->lock);
   return 0;
}

#else /* !NOCTURNE */

#if defined(_WIN32) && !defined(HAVE_PTHREAD)
#include <windows.h>
static_assert(sizeof(struct u_rwlock) == sizeof(SRWLOCK),
   "struct u_rwlock should have equal size with SRWLOCK");
#endif

int u_rwlock_init(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   InitializeSRWLock((PSRWLOCK)(&rwlock->rwlock));
   return 0;
#else
   return pthread_rwlock_init(&rwlock->rwlock, NULL);
#endif
}

int u_rwlock_destroy(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   return 0;
#else
   return pthread_rwlock_destroy(&rwlock->rwlock);
#endif
}

int u_rwlock_rdlock(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   AcquireSRWLockShared((PSRWLOCK)&rwlock->rwlock);
   return 0;
#else
   return pthread_rwlock_rdlock(&rwlock->rwlock);
#endif
}

int u_rwlock_rdunlock(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   ReleaseSRWLockShared((PSRWLOCK)&rwlock->rwlock);
   return 0;
#else
   return pthread_rwlock_unlock(&rwlock->rwlock);
#endif
}

int u_rwlock_wrlock(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   AcquireSRWLockExclusive((PSRWLOCK)&rwlock->rwlock);
   return 0;
#else
   return pthread_rwlock_wrlock(&rwlock->rwlock);
#endif
}

int u_rwlock_wrunlock(struct u_rwlock *rwlock)
{
#if defined(_WIN32) && !defined(HAVE_PTHREAD)
   ReleaseSRWLockExclusive((PSRWLOCK)&rwlock->rwlock);
   return 0;
#else
   return pthread_rwlock_unlock(&rwlock->rwlock);
#endif
}

#endif /* NOCTURNE */
