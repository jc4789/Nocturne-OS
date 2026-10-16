/* Nocturne x86-64 / clang port of WABT's src/config.h.in.
 * WABT is Copyright 2016 WebAssembly Community Group participants,
 * licensed under Apache-2.0; see third_party/wabt/LICENSE. */
#ifndef WABT_CONFIG_H_
#define WABT_CONFIG_H_
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#define alloca(size) __builtin_alloca(size)
#define WABT_VERSION_STRING "1.0.42-nocturne"
#define WABT_NOCTURNE 1
/* WABT_DEBUG stays undefined: interp.h tests it with #ifdef, and a defined 0
 * would give every Value a type tag checked on each access. */
#define HAVE_ALLOCA_H 0
#define HAVE_UNISTD_H 0
#define HAVE_SNPRINTF 1
#define HAVE_SSIZE_T 0
#define HAVE_STRCASECMP 1
#define HAVE_WIN32_VT100 0
#define WABT_BIG_ENDIAN 0
#define HAVE_OPENSSL_SHA_H 0
#define COMPILER_IS_CLANG 1
#define COMPILER_IS_GNU 0
#define COMPILER_IS_MSVC 0
#define WITH_EXCEPTIONS 0
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#define WABT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define WABT_LIKELY(x) __builtin_expect(!!(x), 1)
#define WABT_VECTORCALL
#define WABT_PRINTF_FORMAT(format_arg, first_arg) \
  __attribute__((format(printf, (format_arg), (first_arg))))
#ifdef __cplusplus
#define WABT_STATIC_ASSERT(x) static_assert((x), #x)
#else
#define WABT_STATIC_ASSERT(x) _Static_assert((x), #x)
#endif
#define WABT_UNREACHABLE abort()
#ifdef __cplusplus
#define PRIzd "zd"
#define PRIzx "zx"
#define wabt_snprintf snprintf
#define wabt_vsnprintf vsnprintf
double wabt_convert_uint64_to_double(uint64_t);
float wabt_convert_uint64_to_float(uint64_t);
double wabt_convert_int64_to_double(int64_t);
float wabt_convert_int64_to_float(int64_t);
#endif
#endif
