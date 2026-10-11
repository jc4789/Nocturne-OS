#pragma once
/* Preserve the native C ABI when these headers are consumed by C++. */
#ifdef __cplusplus
#define NOCTURNE_BEGIN_DECLS extern "C" {
#define NOCTURNE_END_DECLS }
#define NOCTURNE_NORETURN [[noreturn]]
#else
#define NOCTURNE_BEGIN_DECLS
#define NOCTURNE_END_DECLS
#define NOCTURNE_NORETURN _Noreturn
#endif
