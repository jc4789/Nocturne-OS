/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 */

#include "u_call_once.h"
#include "detect_os.h"

struct util_call_once_context_t
{
   const void *data;
   util_call_once_data_func func;
};

#if DETECT_OS_NOCTURNE
#include "nocturne.h"

/* The argument travels in a native thread-local slot (no ELF TLS, so tcc -run
   programs can link this); the previous value is kept for nested calls. */
static void
util_call_once_data_slow_once(void)
{
   struct util_call_once_context_t *once_context = thread_local_get(N_TLS_CALL_ONCE);
   once_context->func(once_context->data);
}

void
util_call_once_data_slow(once_flag *once, util_call_once_data_func func, const void *data)
{
   struct util_call_once_context_t once_context = { data, func };
   void *previous = thread_local_get(N_TLS_CALL_ONCE);
   thread_local_set(N_TLS_CALL_ONCE, &once_context);
   call_once(once, util_call_once_data_slow_once);
   thread_local_set(N_TLS_CALL_ONCE, previous);
}
#else
static thread_local struct util_call_once_context_t call_once_context;

static void
util_call_once_data_slow_once(void)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->func(once_context->data);
}

void
util_call_once_data_slow(once_flag *once, util_call_once_data_func func, const void *data)
{
   struct util_call_once_context_t *once_context = &call_once_context;
   once_context->data = data;
   once_context->func = func;
   call_once(once, util_call_once_data_slow_once);
}
#endif
